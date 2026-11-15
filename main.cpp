#include "linux_support.h"
#include <cstring>
#include <exception>

namespace {
using namespace strazh;
constexpr long long kBootTimeoutMs = 30000;
constexpr long long kShutdownGraceMs = 8000;
constexpr long long kKillGraceMs = 1000;

int finish(int result) {
    if (getpid() != 1)
        return result;
    // Exiting PID 1 panics the kernel. Until filesystem teardown and poweroff
    // are implemented, remain quiescent and keep reaping, including on failure.
    std::fprintf(stderr, "[myinit] %s; PID 1 remains in reap-only mode\n",
                 result ? "boot/runtime failure" : "shutdown complete");
    for (;;) {
        while (waitpid(-1, nullptr, WNOHANG) > 0) {
        }
        timespec delay{1, 0};
        nanosleep(&delay, nullptr);
    }
}

int run(int argc, char **argv) {
    const bool test = argc > 1 && std::strcmp(argv[1], "--test") == 0;
    if ((getpid() != 1 && !test) || (test && (getpid() == 1 || argc != 5)) || (!test && argc > 4)) {
        std::fprintf(stderr, "usage: myinit [STAGE1 [SUPERVISOR [SERVICES]]] (PID 1 only)\n"
                             "       myinit --test FAKE_STAGE1 SUPERVISOR SERVICES\n");
        return 2;
    }
    const int offset = test ? 1 : 0;
    const char *stage1 = argc > 1 + offset ? argv[1 + offset] : "/etc/myinit/stage1";
    const char *supervisor = argc > 2 + offset ? argv[2 + offset] : "/etc/myinit/supervisor";
    const char *services = argc > 3 + offset ? argv[3 + offset] : "/etc/sv";
    if (prctl(PR_SET_CHILD_SUBREAPER, 1) < 0) {
        std::perror("subreaper");
        return 1;
    }
    ChildReaper cleanup;
    Fd sfd(setup_signalfd());
    Fd epfd(epoll_create1(EPOLL_CLOEXEC));
    if (sfd.get() < 0 || epfd.get() < 0 || !epoll_add(epfd.get(), sfd.get())) {
        std::perror("init event setup");
        return 1;
    }
    Child early = spawn(stage1, test ? nullptr : "--boot");
    if (early.pid < 0) {
        std::perror("spawn stage1");
        return 1;
    }
    Child manager;
    bool booted = false, stopping = false, killed = false;
    int result = 0;
    const long long boot_deadline = now_ms() + kBootTimeoutMs;
    long long shutdown_deadline = 0;
    std::set<pid_t> term_sent;
    auto shutdown = [&] {
        if (stopping)
            return;
        stopping = true;
        shutdown_deadline = now_ms() + kShutdownGraceMs;
        signal_child_group(early.pid, SIGTERM);
        signal_pid(manager.pid, SIGTERM);
        if (early.pid > 0)
            term_sent.insert(early.pid);
        if (manager.pid > 0)
            term_sent.insert(manager.pid);
        std::fprintf(stderr, "[myinit] bounded shutdown started\n");
    };

    for (;;) {
        epoll_event events[4];
        const int n = epoll_wait(epfd.get(), events, 4, 50);
        if (n < 0 && errno != EINTR) {
            std::perror("epoll_wait");
            result = 1;
            shutdown();
        }
        signalfd_siginfo info{};
        for (int count = 0; count < 64; ++count) {
            const ssize_t r = read(sfd.get(), &info, sizeof(info));
            if (r < 0 && errno == EINTR) {
                --count;
                continue;
            }
            if (r < 0 && errno == EAGAIN)
                break;
            if (r != static_cast<ssize_t>(sizeof(info))) {
                result = 1;
                shutdown();
                break;
            }
            if (info.ssi_signo == SIGTERM || info.ssi_signo == SIGINT)
                shutdown();
        }
        if (early.exec_error.get() >= 0)
            exec_result(early.exec_error);
        if (manager.exec_error.get() >= 0)
            exec_result(manager.exec_error);
        bool no_children = false;
        for (;;) {
            int status = 0;
            const pid_t pid = waitpid(-1, &status, WNOHANG);
            if (pid < 0 && errno == EINTR)
                continue;
            if (pid < 0 && errno == ECHILD)
                no_children = true;
            else if (pid < 0) {
                std::perror("waitpid");
                result = 1;
                shutdown();
            }
            if (pid <= 0)
                break;
            if (pid == early.pid) {
                early.pid = -1;
                if (!stopping) {
                    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                        std::fprintf(stderr, "[myinit] stage1 failed, status=%d\n", status);
                        result = 1;
                        shutdown();
                    } else {
                        booted = true;
                    }
                }
            } else if (pid == manager.pid) {
                manager.pid = -1;
                if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
                    result = 1;
                if (!stopping) {
                    // Fail closed: restarting could duplicate escaped old services.
                    std::fprintf(stderr, "[myinit] unexpected supervisor exit, status=%d\n",
                                 status);
                    result = 1;
                    shutdown();
                }
            }
        }
        if (!stopping && !booted && now_ms() >= boot_deadline) {
            std::fprintf(stderr, "[myinit] stage1 boot timeout\n");
            result = 1;
            shutdown();
        }
        if (!stopping && booted && manager.pid < 0) {
            manager = spawn(supervisor, services);
            if (manager.pid < 0) {
                std::perror("spawn supervisor");
                result = 1;
                shutdown();
            }
        }
        if (stopping) {
            // Once supervision has stopped, take responsibility for adopted
            // descendants. Never broadcast to unrelated host processes.
            if (manager.pid < 0) {
                try {
                    if (killed)
                        signal_children(SIGKILL);
                    else
                        signal_new_children(SIGTERM, term_sent);
                } catch (const std::exception &e) {
                    std::fprintf(stderr, "[myinit] %s\n", e.what());
                }
            }
            if (no_children)
                return result;
            if (now_ms() >= shutdown_deadline) {
                if (killed) {
                    std::fprintf(stderr, "[myinit] children remain after SIGKILL deadline\n");
                    return 1;
                }
                killed = true;
                signal_pid(early.pid, SIGKILL);
                signal_group(early.pid, SIGKILL);
                signal_pid(manager.pid, SIGKILL);
                shutdown_deadline = now_ms() + kKillGraceMs;
            }
        }
    }
}
} // namespace

int main(int argc, char **argv) {
    int result = 1;
    try {
        result = run(argc, argv);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[myinit] fatal: %s\n", e.what());
    }
    return finish(result);
}
