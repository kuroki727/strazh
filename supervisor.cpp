#include "control_socket.h"
#include <dirent.h>
#include <deque>
#include <map>
#include <memory>
#include <cstdlib>
#include <sstream>

namespace {
using namespace strazh;

constexpr long long kShutdownGraceMs = 5000;
constexpr long long kKillGraceMs = 1000;

struct ServiceState {
    std::string name;
    std::string run_path;
    std::vector<std::string> after;
    pid_t pid = 0; // process group ID; retained until leader reaped AND group gone
    Fd exec_error;
    long long start_time_ms = 0;
    long long next_start_ms = 0;
    long long backoff_ms = 0;
    long long stop_deadline_ms = 0;
    bool disabled = false;
    bool manual_action = false;
    bool stopping = false;
    bool reaped = false;
    bool exec_ok = false;
};

std::vector<std::string> read_after_file(const std::string &path) {
    std::vector<std::string> deps;
    struct stat st{};
    if (stat(path.c_str(), &st) < 0) {
        if (errno == ENOENT)
            return deps;
        throw std::runtime_error("cannot inspect " + path);
    }
    if (!S_ISREG(st.st_mode) || st.st_size > 65536)
        throw std::runtime_error("after must be a regular file of at most 64 KiB");
    std::ifstream f(path);
    if (!f) {
        if (std::filesystem::exists(path))
            throw std::runtime_error("cannot read " + path);
        return deps;
    }
    if (std::filesystem::file_size(path) > 65536)
        throw std::runtime_error("after file too large");

    std::string line;
    while (std::getline(f, line)) {
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos)
            continue;
        size_t end = line.find_last_not_of(" \t\r\n");
        line = line.substr(start, end - start + 1);
        if (line.empty() || line[0] == '#')
            continue;
        if (!valid_name(line))
            throw std::runtime_error("invalid dependency in " + path);
        if (std::find(deps.begin(), deps.end(), line) == deps.end())
            deps.push_back(line);
    }
    if (f.bad())
        throw std::runtime_error("error reading " + path);
    return deps;
}

std::vector<ServiceState> discover_services(const std::string &services_dir) {
    std::vector<ServiceState> services;

    DIR *dir = opendir(services_dir.c_str());
    if (dir == nullptr) {
        std::fprintf(stderr, "[supervisor] opendir %s failed: %s\n", services_dir.c_str(),
                     std::strerror(errno));
        throw std::runtime_error("service discovery failed");
    }
    const auto close_directory = [](DIR *d) { closedir(d); };
    std::unique_ptr<DIR, decltype(close_directory)> directory(dir, close_directory);

    dirent *entry;
    for (;;) {
        errno = 0;
        entry = readdir(dir);
        if (!entry) {
            if (errno)
                throw std::runtime_error("readdir failed");
            break;
        }
        std::string name = entry->d_name;
        if (!valid_name(name))
            continue;

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
        if (services.size() > 4096)
            throw std::runtime_error("too many services (maximum 4096)");
    }
    std::sort(services.begin(), services.end(),
              [](const auto &a, const auto &b) { return a.name < b.name; });
    return services;
}

std::vector<ServiceState> resolve_order(std::vector<ServiceState> services) {
    std::map<std::string, size_t> name_to_index;
    for (size_t i = 0; i < services.size(); ++i) {
        name_to_index[services[i].name] = i;
    }

    for (const auto &svc : services) {
        for (const auto &dep : svc.after) {
            if (name_to_index.find(dep) == name_to_index.end())
                throw std::runtime_error(svc.name + ": missing dependency " + dep);
        }
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
        if (indegree[i] == 0)
            queue.push_back(i);
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
            if (--indegree[d] == 0)
                queue.push_back(d);
        }
    }

    if (order.size() < services.size()) {
        std::string names;
        for (size_t i = 0; i < services.size(); ++i)
            if (!placed[i])
                names += " " + services[i].name;
        throw std::runtime_error("dependency cycle (or dependent on cycle):" + names);
    }

    std::vector<ServiceState> sorted;
    sorted.reserve(services.size());
    for (size_t i : order)
        sorted.push_back(std::move(services[i]));
    return sorted;
}

bool dependencies_ready(const ServiceState &svc, const std::vector<ServiceState> &services,
                        const std::map<std::string, size_t> &name_to_index) {
    for (const auto &dep : svc.after) {
        auto it = name_to_index.find(dep);
        if (it == name_to_index.end())
            return false;
        const auto &dependency = services[it->second];
        if (!dependency.pid || !dependency.exec_ok || dependency.reaped || dependency.stopping ||
            dependency.disabled)
            return false;
    }
    return true;
}

bool spawn_service(ServiceState &svc) {
    const std::string cwd = std::filesystem::path(svc.run_path).parent_path().string();
    Child child = spawn(svc.run_path.c_str(), nullptr, cwd.c_str());
    if (child.pid < 0) {
        std::perror("spawn service");
        svc.backoff_ms = next_backoff(svc.backoff_ms, 0);
        svc.next_start_ms = now_ms() + svc.backoff_ms;
        return false;
    }
    svc.pid = child.pid;
    svc.exec_error = std::move(child.exec_error);
    svc.start_time_ms = now_ms();
    svc.reaped = svc.stopping = svc.exec_ok = false;
    std::fprintf(stderr, "[supervisor] started %s pid=%d\n", svc.name.c_str(), svc.pid);
    return true;
}

void request_stop(ServiceState &svc) {
    if (!svc.pid || svc.stopping || svc.reaped)
        return;
    svc.stopping = true;
    svc.stop_deadline_ms = now_ms() + kShutdownGraceMs;
    signal_child_group(svc.pid, SIGTERM);
}

std::string build_status_report(const std::vector<ServiceState> &services,
                                const std::map<std::string, size_t> &index) {
    std::string out = "OK\n";
    for (const auto &svc : services) {
        out += svc.name + ": ";
        if (svc.pid) {
            out += svc.stopping || svc.reaped ? "stopping" : (svc.exec_ok ? "running" : "starting");
            out += " pid=" + std::to_string(svc.pid);
        } else if (svc.disabled)
            out += "disabled";
        else if (!dependencies_ready(svc, services, index))
            out += "blocked";
        else
            out += "stopped";
        out += " backoff_ms=" + std::to_string(svc.backoff_ms) +
               " next_start_in_ms=" + std::to_string(std::max(0LL, svc.next_start_ms - now_ms())) +
               "\n";
    }
    if (services.empty())
        out += "(no services)\n";
    return out;
}

std::string handle_command(const std::string &line, std::vector<ServiceState> &services,
                           const std::map<std::string, size_t> &index, bool shutting_down) {
    for (char c : line)
        if (static_cast<unsigned char>(c) < 32 && c != '\t')
            return "ERR invalid request bytes\n";
    std::istringstream in(line);
    std::string cmd, name, extra;
    in >> cmd >> name >> extra;
    if (cmd == "status" && name.empty())
        return build_status_report(services, index);
    if ((cmd != "start" && cmd != "stop" && cmd != "restart") || !valid_name(name) ||
        !extra.empty())
        return "ERR expected status or start|stop|restart NAME\n";
    const auto it = index.find(name);
    if (it == index.end())
        return "ERR unknown service\n";
    if (shutting_down)
        return "ERR supervisor is shutting down\n";
    auto &svc = services[it->second];
    if (cmd == "stop") {
        svc.disabled = true;
        svc.manual_action = true;
        request_stop(svc);
    } else if (cmd == "restart") {
        svc.disabled = false;
        svc.manual_action = true;
        if (svc.pid)
            request_stop(svc);
        else {
            svc.backoff_ms = 0;
            svc.next_start_ms = now_ms();
        }
    } else if (svc.disabled) {
        svc.disabled = false;
        svc.manual_action = true;
        svc.backoff_ms = 0;
        svc.next_start_ms = now_ms();
    }
    // Commands acknowledge desired state. Starting while stopping waits for reap.
    return "OK\n";
}

bool reap_services(std::vector<ServiceState> &services, std::map<pid_t, size_t> &pids) {
    for (;;) {
        int status;
        const pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid < 0 && errno == EINTR)
            continue;
        if (pid < 0 && errno != ECHILD) {
            std::perror("waitpid");
            return false;
        }
        if (pid <= 0)
            break;
        const auto it = pids.find(pid);
        if (it == pids.end())
            continue;
        auto &svc = services[it->second];
        pids.erase(it);
        svc.reaped = true;
        svc.exec_ok = false;
        if (svc.exec_error.get() >= 0)
            exec_result(svc.exec_error);
        // The foreground leader is gone. Remove remaining members before
        // allowing another instance to start, including on natural exits.
        signal_group(svc.pid, SIGKILL);
        svc.stop_deadline_ms = now_ms() + kKillGraceMs;
        if (svc.manual_action) {
            svc.manual_action = false;
            svc.backoff_ms = 0;
            svc.next_start_ms = now_ms();
        } else {
            svc.backoff_ms = next_backoff(svc.backoff_ms, now_ms() - svc.start_time_ms);
            svc.next_start_ms = now_ms() + svc.backoff_ms;
        }
        std::fprintf(stderr, "[supervisor] exited %s exit_code=%d signal=%d backoff_ms=%lld\n",
                     svc.name.c_str(), WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                     WIFSIGNALED(status) ? WTERMSIG(status) : 0, svc.backoff_ms);
    }
    return true;
}

void update_services(std::vector<ServiceState> &services) {
    for (auto &svc : services) {
        if (!svc.pid)
            continue;
        if (!svc.reaped && svc.exec_error.get() >= 0) {
            const int state = exec_result(svc.exec_error);
            svc.exec_ok = state == 1;
            if (state < 0)
                signal_pid(svc.pid, SIGKILL);
        }
        if (svc.reaped && !group_exists(svc.pid)) {
            svc.pid = 0;
            svc.stopping = false;
        } else if ((svc.stopping || svc.reaped) && now_ms() >= svc.stop_deadline_ms) {
            signal_group(svc.pid, SIGKILL);
            if (!svc.reaped)
                signal_pid(svc.pid, SIGKILL);
            // No blocking waitpid, even for uninterruptible kernel sleep.
        }
    }
}

void start_services(std::vector<ServiceState> &services, const std::map<std::string, size_t> &index,
                    std::map<pid_t, size_t> &pids) {
    for (size_t i = 0; i < services.size(); ++i) {
        auto &svc = services[i];
        if (!svc.pid && !svc.disabled && now_ms() >= svc.next_start_ms &&
            dependencies_ready(svc, services, index)) {
            svc.manual_action = false;
            if (spawn_service(svc))
                pids[svc.pid] = i;
        }
    }
}

int run(int argc, char **argv) {
    if (argc > 3)
        throw std::runtime_error("usage: supervisor [SERVICES [SOCKET]]");
    const std::string services_dir =
        std::filesystem::canonical(argc > 1 ? argv[1] : "/etc/sv").string();
    const char *env = std::getenv("STRAZH_SOCKET");
    const std::string socket_path =
        argc > 2 ? argv[2] : (env ? env : "/run/strazh/supervisor.sock");
    Fd directory_lock(open(services_dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (directory_lock.get() < 0 || flock(directory_lock.get(), LOCK_EX | LOCK_NB) < 0)
        throw std::runtime_error("services directory is already supervised or inaccessible");
    auto services = resolve_order(discover_services(services_dir));
    ChildReaper cleanup;
    struct GroupCleanup {
        std::vector<ServiceState> &services;
        ~GroupCleanup() {
            for (auto &svc : services)
                signal_group(svc.pid, SIGKILL);
        }
    } group_cleanup{services};
    std::map<std::string, size_t> index;
    for (size_t i = 0; i < services.size(); ++i)
        index[services[i].name] = i;
    std::map<pid_t, size_t> pids;
    if (prctl(PR_SET_CHILD_SUBREAPER, 1) < 0)
        throw std::runtime_error("subreaper setup failed");
    Fd sfd(setup_signalfd());
    Fd epfd(epoll_create1(EPOLL_CLOEXEC));
    ControlSocket listener(socket_path);
    if (sfd.get() < 0 || epfd.get() < 0 || !epoll_add(epfd.get(), sfd.get()) ||
        !epoll_add(epfd.get(), listener.get()))
        throw std::runtime_error("event setup failed");
    // Confirm procfs is available before starting anything.
    children();
    std::map<int, Client> clients;
    bool shutting_down = false, killed = false;
    int result = 0;
    long long shutdown_deadline = 0;
    long long accept_resume_ms = 0;
    std::set<pid_t> term_sent;
    auto shutdown = [&] {
        if (shutting_down)
            return;
        shutting_down = true;
        shutdown_deadline = now_ms() + kShutdownGraceMs;
        for (auto it = services.rbegin(); it != services.rend(); ++it) {
            request_stop(*it);
            if (it->pid > 0)
                term_sent.insert(it->pid);
        }
    };

    for (;;) {
        if (accept_resume_ms && now_ms() >= accept_resume_ms) {
            accept_resume_ms = 0;
            if (!epoll_add(epfd.get(), listener.get())) {
                result = 1;
                shutdown();
            }
        }
        epoll_event events[32];
        const int n = epoll_wait(epfd.get(), events, 32, 50);
        if (n < 0 && errno != EINTR) {
            std::perror("epoll_wait");
            result = 1;
            shutdown();
        }
        // Drain signals before commands or timers. Reap regardless of coalesced
        // SIGCHLD notifications; a signal does not correspond to exactly one child.
        signalfd_siginfo si{};
        for (int budget = 0; budget < 64; ++budget) {
            const ssize_t r = read(sfd.get(), &si, sizeof(si));
            if (r < 0 && errno == EINTR) {
                --budget;
                continue;
            }
            if (r < 0 && errno == EAGAIN)
                break;
            if (r != static_cast<ssize_t>(sizeof(si))) {
                result = 1;
                shutdown();
                break;
            }
            if (si.ssi_signo == SIGTERM || si.ssi_signo == SIGINT)
                shutdown();
        }
        if (!reap_services(services, pids)) {
            result = 1;
            shutdown();
        }
        update_services(services);

        for (int i = 0; i < n; ++i) {
            const int fd = events[i].data.fd;
            if (fd == listener.get()) {
                for (int budget = 0; budget < 16; ++budget) {
                    Fd incoming(accept4(fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK));
                    if (incoming.get() < 0) {
                        if (errno == EINTR) {
                            --budget;
                            continue;
                        }
                        if (errno != EAGAIN && errno != EWOULDBLOCK) {
                            std::perror("accept4");
                            // A readable listener plus EMFILE would otherwise
                            // make epoll return immediately in a tight loop.
                            epoll_ctl(epfd.get(), EPOLL_CTL_DEL, fd, nullptr);
                            accept_resume_ms = now_ms() + 250;
                        }
                        break;
                    }
                    ucred peer{};
                    socklen_t length = sizeof(peer);
                    if (clients.size() >= 64 ||
                        getsockopt(incoming.get(), SOL_SOCKET, SO_PEERCRED, &peer, &length) < 0 ||
                        (peer.uid != geteuid() && peer.uid != 0))
                        continue;
                    const int key = incoming.get();
                    if (!epoll_add(epfd.get(), key, EPOLLIN | EPOLLRDHUP))
                        continue;
                    clients.emplace(
                        key, Client{std::move(incoming), {}, {}, 0, now_ms() + kClientTimeoutMs});
                }
                continue;
            }
            const auto it = clients.find(fd);
            if (it == clients.end())
                continue;
            auto &client = it->second;
            bool close_client = (events[i].events & EPOLLERR) != 0;
            if (!close_client) {
                close_client = !pump_client(client, [&](const std::string &line) {
                    return handle_command(line, services, index, shutting_down);
                });
                if (!close_client && !client.output.empty()) {
                    epoll_event ev{};
                    ev.events = EPOLLOUT;
                    ev.data.fd = fd;
                    if (epoll_ctl(epfd.get(), EPOLL_CTL_MOD, fd, &ev) < 0)
                        close_client = true;
                }
            }
            if (close_client) {
                epoll_ctl(epfd.get(), EPOLL_CTL_DEL, fd, nullptr);
                clients.erase(it);
            }
        }
        for (auto it = clients.begin(); it != clients.end();) {
            if (now_ms() >= it->second.deadline) {
                epoll_ctl(epfd.get(), EPOLL_CTL_DEL, it->first, nullptr);
                it = clients.erase(it);
            } else
                ++it;
        }
        // Deadlines are checked on every turn, even with constant client traffic.
        if (shutting_down) {
            if (killed)
                signal_children(SIGKILL);
            else
                signal_new_children(SIGTERM, term_sent);
            if (pids.empty() && children().empty())
                return result;
            if (now_ms() >= shutdown_deadline) {
                if (killed) {
                    std::fprintf(stderr, "[supervisor] SIGKILL deadline exceeded\n");
                    return 1;
                }
                killed = true;
                for (auto &svc : services)
                    signal_group(svc.pid, SIGKILL);
                signal_children(SIGKILL);
                shutdown_deadline = now_ms() + kKillGraceMs;
            }
            continue;
        }
        start_services(services, index, pids);
    }
}
} // namespace

int main(int argc, char **argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[supervisor] fatal: %s\n", e.what());
        return 1;
    }
}
