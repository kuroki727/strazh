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
#include <signal.h>
#include <time.h>
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <climits>

namespace {

constexpr long long kMinRuntimeMs = 1000;   // меньше — считаем "упал быстро"
constexpr long long kInitialBackoffMs = 500;
constexpr long long kMaxBackoffMs = 30000;
constexpr long long kShutdownGraceMs = 5000; // сколько ждать после SIGTERM перед SIGKILL

struct ServiceState {
    std::string name;
    std::string run_path;
    pid_t pid = 0;             // 0 = сейчас не запущен
    long long start_time_ms = 0;
    long long next_start_ms = 0; // когда можно (пере)запускать; учитывается только если pid == 0
    long long backoff_ms = 0;    // текущий backoff после быстрого падения
};

long long now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
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
        if (access(run_path.c_str(), X_OK) != 0) {
            continue; // нет исполняемого run — не сервис, пропускаем молча
        }

        ServiceState svc;
        svc.name = name;
        svc.run_path = run_path;
        services.push_back(std::move(svc));
    }
    closedir(dir);
    return services;
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

} // namespace

int main(int argc, char *argv[]) {
    std::string services_dir = (argc > 1) ? argv[1] : "/etc/sv";
    std::fprintf(stderr, "[supervisor] starting as pid %d, services dir: %s\n",
        getpid(), services_dir.c_str());

    std::vector<ServiceState> services = discover_services(services_dir);
    if (services.empty()) {
        std::fprintf(stderr, "[supervisor] no services found in %s\n", services_dir.c_str());
    }

    std::map<pid_t, size_t> pid_to_index;
    for (size_t i = 0; i < services.size(); ++i) {
        if (spawn_service(services[i])) {
            pid_to_index[services[i].pid] = i;
        }
    }

    int sfd = setup_signalfd();
    if (sfd == -1) return 1;
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd == -1) {
        std::perror("epoll_create1");
        close(sfd);
        return 1;
    }
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = sfd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, sfd, &ev) == -1) {
        std::perror("epoll_ctl");
        close(epfd);
        close(sfd);
        return 1;
    }

    bool shutting_down = false;
    long long shutdown_deadline_ms = 0;

    for (;;) {
        // --- считаем, сколько ждать в epoll_wait ---
        long long timeout_ms = -1; // -1 = ждать бесконечно
        if (shutting_down) {
            timeout_ms = std::max<long long>(0, shutdown_deadline_ms - now_ms());
        } else {
            for (const auto &svc : services) {
                if (svc.pid != 0) continue; // уже запущен
                long long wait_left = svc.next_start_ms - now_ms();
                if (wait_left < 0) wait_left = 0;
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
            // Grace period истёк — добиваем оставшихся SIGKILL и ждём синхронно.
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

                if (shutting_down) continue; // не перезапускаем во время остановки

                if (ran_ms < kMinRuntimeMs) {
                    svc.backoff_ms = (svc.backoff_ms == 0)
                        ? kInitialBackoffMs
                        : std::min(svc.backoff_ms * 2, kMaxBackoffMs);
                    std::fprintf(stderr, "[supervisor] %s crashed fast, backing off %lld ms\n",
                        svc.name.c_str(), svc.backoff_ms);
                } else {
                    svc.backoff_ms = 0; // отработал достаточно долго — сбрасываем backoff
                }
                svc.next_start_ms = now_ms() + svc.backoff_ms;
            }
        }

        if (shutting_down) {
            bool all_stopped = true;
            for (const auto &svc : services) {
                if (svc.pid != 0) { all_stopped = false; break; }
            }
            if (all_stopped) break;
            continue; // во время остановки новых сервисов не стартуем
        }

        // --- стартуем всех, у кого подошло время (ре)запуска ---
        long long t = now_ms();
        for (auto &svc : services) {
            if (svc.pid == 0 && svc.next_start_ms <= t) {
                if (spawn_service(svc)) {
                    pid_to_index[svc.pid] = &svc - &services[0];
                }
            }
        }
    }

    close(epfd);
    close(sfd);
    std::fprintf(stderr, "[supervisor] all services stopped, exiting\n");
    return 0;
}
