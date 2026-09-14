#if __has_include(<cstdio>)
#include <cstdio>
#else
#include <stdio.h>
#endif
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <time.h>
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <deque>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <climits>

namespace {

constexpr long long kMinRuntimeMs = 1000;
constexpr long long kInitialBackoffMs = 500;
constexpr long long kMaxBackoffMs = 30000;
constexpr long long kShutdownGraceMs = 5000;
constexpr long long kDependencyPollMs = 200;

struct ServiceState {
    std::string name;
    std::string run_path;
    std::vector<std::string> after;
    pid_t pid = 0;
    long long start_time_ms = 0;
    long long next_start_ms = 0;
    long long backoff_ms = 0;
    bool disabled = false; // выключен вручную через control socket ('stop')
    bool manual_action = false; // следующий SIGCHLD — от нашего же kill(), не краш
};

long long now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

std::vector<std::string> read_after_file(const std::string &path) {
    std::vector<std::string> deps;
    std::ifstream f(path);
    if (!f) return deps;

    std::string line;
    while (std::getline(f, line)) {
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        size_t end = line.find_last_not_of(" \t\r\n");
        line = line.substr(start, end - start + 1);
        if (line.empty() || line[0] == '#') continue;
        deps.push_back(line);
    }
    return deps;
}

std::vector<ServiceState> discover_services(const std::string &services_dir) {
    std::vector<ServiceState> services;

    DIR *dir = opendir(services_dir.c_str());
    if (dir == nullptr) {
        std::fprintf(stderr, "[supervisor] opendir %s failed: %s\n",
            services_dir.c_str(), std::strerror(errno));
        return services;
    }

    dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;

        std::string run_path = services_dir + "/" + name + "/run";
        struct stat st{};
        if (stat(run_path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        if (access(run_path.c_str(), X_OK) != 0) {
            continue;
        }

        ServiceState svc;
        svc.name = name;
        svc.run_path = run_path;
        svc.after = read_after_file(services_dir + "/" + name + "/after");
        services.push_back(std::move(svc));
    }
    closedir(dir);
    return services;
}

std::vector<ServiceState> resolve_order(std::vector<ServiceState> services) {
    std::map<std::string, size_t> name_to_index;
    for (size_t i = 0; i < services.size(); ++i) {
        name_to_index[services[i].name] = i;
    }

    for (auto &svc : services) {
        std::vector<std::string> valid;
        for (const auto &dep : svc.after) {
            if (dep == svc.name) {
                std::fprintf(stderr, "[supervisor] service %s: 'after' lists itself, ignoring\n",
                    svc.name.c_str());
                continue;
            }
            if (name_to_index.find(dep) == name_to_index.end()) {
                std::fprintf(stderr, "[supervisor] service %s: unknown dependency '%s', ignoring\n",
                    svc.name.c_str(), dep.c_str());
                continue;
            }
            valid.push_back(dep);
        }
        svc.after = std::move(valid);
    }

    std::vector<std::vector<size_t>> dependents(services.size());
    std::vector<int> indegree(services.size(), 0);
    for (size_t i = 0; i < services.size(); ++i) {
        indegree[i] = static_cast<int>(services[i].after.size());
        for (const auto &dep : services[i].after) {
            dependents[name_to_index[dep]].push_back(i);
        }
    }

    std::deque<size_t> queue;
    for (size_t i = 0; i < services.size(); ++i) {
        if (indegree[i] == 0) queue.push_back(i);
    }

    std::vector<size_t> order;
    order.reserve(services.size());
    std::vector<bool> placed(services.size(), false);
    while (!queue.empty()) {
        size_t i = queue.front();
        queue.pop_front();
        order.push_back(i);
        placed[i] = true;
        for (size_t d : dependents[i]) {
            if (--indegree[d] == 0) queue.push_back(d);
        }
    }

    if (order.size() < services.size()) {
        std::string cycle_names;
        for (size_t i = 0; i < services.size(); ++i) {
            if (!placed[i]) {
                if (!cycle_names.empty()) cycle_names += ", ";
                cycle_names += services[i].name;
                services[i].after.clear();
            }
        }
        std::fprintf(stderr,
            "[supervisor] dependency cycle detected involving: %s — starting them without ordering\n",
            cycle_names.c_str());
        for (size_t i = 0; i < services.size(); ++i) {
            if (!placed[i]) order.push_back(i);
        }
    }

    std::vector<ServiceState> sorted;
    sorted.reserve(services.size());
    for (size_t i : order) sorted.push_back(std::move(services[i]));
    return sorted;
}

bool dependencies_ready(const ServiceState &svc, const std::vector<ServiceState> &services,
                         const std::map<std::string, size_t> &name_to_index) {
    for (const auto &dep : svc.after) {
        auto it = name_to_index.find(dep);
        if (it == name_to_index.end()) continue;
        if (services[it->second].pid == 0) return false;
    }
    return true;
}

bool spawn_service(ServiceState &svc) {
    pid_t pid = fork();
    if (pid == -1) {
        std::fprintf(stderr, "[supervisor] fork for %s failed: %s\n",
            svc.name.c_str(), std::strerror(errno));
        return false;
    }
    if (pid == 0) {
        execl(svc.run_path.c_str(), svc.run_path.c_str(), static_cast<char *>(nullptr));
        std::fprintf(stderr, "[supervisor] execl %s failed: %s\n",
            svc.run_path.c_str(), std::strerror(errno));
        _exit(127);
    }
    svc.pid = pid;
    svc.start_time_ms = now_ms();
    std::fprintf(stderr, "[supervisor] started %s as pid %d\n", svc.name.c_str(), pid);
    return true;
}

int setup_signalfd() {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGTERM);

    if (sigprocmask(SIG_BLOCK, &mask, nullptr) == -1) {
        std::perror("sigprocmask");
        return -1;
    }
    int sfd = signalfd(-1, &mask, SFD_CLOEXEC);
    if (sfd == -1) {
        std::perror("signalfd");
    }
    return sfd;
}

// --- control socket ---

int create_control_socket(const std::string &path) {
    if (path.size() >= sizeof(sockaddr_un{}.sun_path)) {
        std::fprintf(stderr, "[supervisor] control socket path too long: %s\n", path.c_str());
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd == -1) {
        std::perror("socket");
        return -1;
    }

    unlink(path.c_str()); // подчищаем сокет от предыдущего (упавшего) запуска

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == -1) {
        std::fprintf(stderr, "[supervisor] bind %s failed: %s\n", path.c_str(), std::strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 16) == -1) {
        std::perror("listen");
        close(fd);
        return -1;
    }
    std::fprintf(stderr, "[supervisor] control socket listening at %s\n", path.c_str());
    return fd;
}

std::string build_status_report(const std::vector<ServiceState> &services) {
    std::string out;
    long long t = now_ms();
    for (const auto &svc : services) {
        out += svc.name;
        if (svc.pid != 0) {
            out += ": running pid=" + std::to_string(svc.pid) +
                   " uptime_ms=" + std::to_string(t - svc.start_time_ms);
        } else if (svc.disabled) {
            out += ": disabled";
        } else {
            long long wait_left = std::max<long long>(0, svc.next_start_ms - t);
            out += ": stopped next_start_in_ms=" + std::to_string(wait_left) +
                   " backoff_ms=" + std::to_string(svc.backoff_ms);
        }
        out += "\n";
    }
    if (out.empty()) out = "(no services)\n";
    return out;
}

// pid_to_index нужен, чтобы 'stop'/'restart' сразу помечали слот в карте
// как свободный, когда убивают ещё не пожатый процесс.
void handle_client(int client_fd, std::vector<ServiceState> &services,
                    const std::map<std::string, size_t> &name_to_index) {
    char buf[256];
    ssize_t r = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (r <= 0) {
        close(client_fd);
        return;
    }
    buf[r] = '\0';

    std::string line(buf);
    size_t end = line.find_first_of("\r\n");
    if (end != std::string::npos) line = line.substr(0, end);

    std::istringstream iss(line);
    std::string cmd, name;
    iss >> cmd >> name;

    std::string response;
    if (cmd == "status") {
        response = build_status_report(services);
    } else if (cmd == "stop" || cmd == "start" || cmd == "restart") {
        auto it = name_to_index.find(name);
        if (name.empty()) {
            response = "ERR missing service name\n";
        } else if (it == name_to_index.end()) {
            response = "ERR unknown service '" + name + "'\n";
        } else {
            ServiceState &svc = services[it->second];
            if (cmd == "stop") {
                svc.disabled = true;
                if (svc.pid != 0) {
                    svc.manual_action = true;
                    kill(svc.pid, SIGTERM); // сам реап обработает выход
                }
            } else if (cmd == "start") {
                svc.disabled = false;
                svc.backoff_ms = 0;
                svc.next_start_ms = now_ms(); // старт при ближайшей возможности
            } else { // restart
                svc.disabled = false;
                if (svc.pid != 0) {
                    svc.manual_action = true;
                    kill(svc.pid, SIGTERM); // респавн подхватит сам после реапа
                } else {
                    svc.backoff_ms = 0;
                    svc.next_start_ms = now_ms();
                }
            }
            response = "OK\n";
        }
    } else {
        response = "ERR unknown command '" + cmd + "'\n";
    }

    ssize_t written = 0;
    while (written < static_cast<ssize_t>(response.size())) {
        ssize_t w = write(client_fd, response.data() + written, response.size() - written);
        if (w <= 0) break;
        written += w;
    }
    close(client_fd);
}

} // namespace

int main(int argc, char *argv[]) {
    std::string services_dir = (argc > 1) ? argv[1] : "/etc/sv";
    std::string socket_path  = (argc > 2) ? argv[2] : "/run/strazh/supervisor.sock";

    std::fprintf(stderr, "[supervisor] starting as pid %d, services dir: %s\n",
        getpid(), services_dir.c_str());

    std::vector<ServiceState> services = resolve_order(discover_services(services_dir));
    if (services.empty()) {
        std::fprintf(stderr, "[supervisor] no services found in %s\n", services_dir.c_str());
    }
    for (const auto &svc : services) {
        if (!svc.after.empty()) {
            std::string deps;
            for (const auto &d : svc.after) { if (!deps.empty()) deps += ", "; deps += d; }
            std::fprintf(stderr, "[supervisor] %s waits for: %s\n", svc.name.c_str(), deps.c_str());
        }
    }

    std::map<std::string, size_t> name_to_index;
    for (size_t i = 0; i < services.size(); ++i) name_to_index[services[i].name] = i;

    std::map<pid_t, size_t> pid_to_index;

    int sfd = setup_signalfd();
    if (sfd == -1) return 1;

    int listen_fd = create_control_socket(socket_path);
    // Отсутствие control socket — не фатально для первого шага: просто нет
    // возможности спросить status, супервизор всё равно управляет сервисами.
    if (listen_fd == -1) {
        std::fprintf(stderr, "[supervisor] continuing without control socket\n");
    }

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd == -1) {
        std::perror("epoll_create1");
        close(sfd);
        if (listen_fd != -1) close(listen_fd);
        return 1;
    }
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = sfd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, sfd, &ev) == -1) {
        std::perror("epoll_ctl");
        close(epfd);
        close(sfd);
        if (listen_fd != -1) close(listen_fd);
        return 1;
    }
    if (listen_fd != -1) {
        epoll_event lev{};
        lev.events = EPOLLIN;
        lev.data.fd = listen_fd;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &lev) == -1) {
            std::perror("epoll_ctl (listen_fd)");
            close(listen_fd);
            listen_fd = -1;
        }
    }

    bool shutting_down = false;
    long long shutdown_deadline_ms = 0;

    for (;;) {
        long long timeout_ms = -1;
        if (shutting_down) {
            timeout_ms = std::max<long long>(0, shutdown_deadline_ms - now_ms());
        } else {
            for (const auto &svc : services) {
                if (svc.pid != 0 || svc.disabled) continue;
                long long wait_left;
                if (dependencies_ready(svc, services, name_to_index)) {
                    wait_left = svc.next_start_ms - now_ms();
                    if (wait_left < 0) wait_left = 0;
                } else {
                    wait_left = kDependencyPollMs;
                }
                if (timeout_ms == -1 || wait_left < timeout_ms) timeout_ms = wait_left;
            }
        }
        int epoll_timeout = (timeout_ms < 0) ? -1
            : static_cast<int>(std::min<long long>(timeout_ms, INT_MAX));

        epoll_event events[8];
        int n = epoll_wait(epfd, events, 8, epoll_timeout);
        if (n == -1) {
            if (errno == EINTR) continue;
            std::perror("epoll_wait");
            break;
        }

        if (n == 0 && shutting_down && now_ms() >= shutdown_deadline_ms) {
            for (auto &svc : services) {
                if (svc.pid != 0) {
                    std::fprintf(stderr, "[supervisor] %s (pid %d) didn't stop in time, SIGKILL\n",
                        svc.name.c_str(), svc.pid);
                    kill(svc.pid, SIGKILL);
                }
            }
            for (auto &svc : services) {
                if (svc.pid != 0) {
                    waitpid(svc.pid, nullptr, 0);
                    svc.pid = 0;
                }
            }
            break;
        }

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;

            if (fd == listen_fd) {
                int client_fd = accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
                if (client_fd != -1) {
                    handle_client(client_fd, services, name_to_index);
                }
                continue;
            }

            if (fd != sfd) continue; // неизвестный fd, не должно происходить

            signalfd_siginfo si{};
            ssize_t r = read(sfd, &si, sizeof(si));
            if (r != static_cast<ssize_t>(sizeof(si))) continue;

            if (si.ssi_signo == SIGTERM && !shutting_down) {
                std::fprintf(stderr, "[supervisor] got SIGTERM, stopping all services (grace %lld ms)\n",
                    kShutdownGraceMs);
                shutting_down = true;
                shutdown_deadline_ms = now_ms() + kShutdownGraceMs;
                for (auto &svc : services) {
                    if (svc.pid != 0) kill(svc.pid, SIGTERM);
                }
                continue;
            }

            if (si.ssi_signo != SIGCHLD) continue;

            for (;;) {
                int status = 0;
                pid_t pid = waitpid(-1, &status, WNOHANG);
                if (pid <= 0) break;

                auto it = pid_to_index.find(pid);
                if (it == pid_to_index.end()) {
                    std::fprintf(stderr, "[supervisor] reaped untracked pid %d\n", pid);
                    continue;
                }
                ServiceState &svc = services[it->second];
                pid_to_index.erase(it);

                long long ran_ms = now_ms() - svc.start_time_ms;
                if (WIFEXITED(status)) {
                    std::fprintf(stderr, "[supervisor] %s (pid %d) exited with code %d after %lld ms\n",
                        svc.name.c_str(), pid, WEXITSTATUS(status), ran_ms);
                } else if (WIFSIGNALED(status)) {
                    std::fprintf(stderr, "[supervisor] %s (pid %d) killed by signal %d after %lld ms\n",
                        svc.name.c_str(), pid, WTERMSIG(status), ran_ms);
                }
                svc.pid = 0;

                if (shutting_down) continue;

                if (svc.manual_action) {
                    svc.manual_action = false;
                    svc.backoff_ms = 0;
                    svc.next_start_ms = now_ms(); // команда, не краш — без штрафа
                } else if (ran_ms < kMinRuntimeMs) {
                    svc.backoff_ms = (svc.backoff_ms == 0)
                        ? kInitialBackoffMs
                        : std::min(svc.backoff_ms * 2, kMaxBackoffMs);
                    std::fprintf(stderr, "[supervisor] %s crashed fast, backing off %lld ms\n",
                        svc.name.c_str(), svc.backoff_ms);
                    svc.next_start_ms = now_ms() + svc.backoff_ms;
                } else {
                    svc.backoff_ms = 0;
                    svc.next_start_ms = now_ms();
                }
            }
        }

        if (shutting_down) {
            bool all_stopped = true;
            for (const auto &svc : services) {
                if (svc.pid != 0) { all_stopped = false; break; }
            }
            if (all_stopped) break;
            continue;
        }

        long long t = now_ms();
        for (auto &svc : services) {
            if (svc.pid == 0 && !svc.disabled && svc.next_start_ms <= t &&
                dependencies_ready(svc, services, name_to_index)) {
                if (spawn_service(svc)) {
                    pid_to_index[svc.pid] = &svc - &services[0];
                }
            }
        }
    }

    close(epfd);
    close(sfd);
    if (listen_fd != -1) {
        close(listen_fd);
        unlink(socket_path.c_str());
    }
    std::fprintf(stderr, "[supervisor] all services stopped, exiting\n");
    return 0;
}
