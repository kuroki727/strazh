#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#if __has_include(<cstdio>)
#include <cstdio>
#else
#include <stdio.h>
#endif
#if __has_include(<cstdlib>)
#include <cstdlib>
#else
#include <stdlib.h>
#endif
#if __has_include(<cerrno>)
#include <cerrno>
#else
#include <errno.h>
#endif
#if __has_include(<csignal>)
#include <csignal>
#else
#include <signal.h>
#endif
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <time.h>
#include <algorithm>
#include <climits>
#if __has_include(<sys/signalfd.h>)
#include <sys/signalfd.h>
#elif __has_include(<linux/signalfd.h>)
#include <linux/signalfd.h>
#else
#error "This program requires Linux signalfd support"
#endif

namespace {

constexpr long long kMinRuntimeMs = 1000;    // меньше — считаем "упал быстро"
constexpr long long kInitialBackoffMs = 500;
constexpr long long kMaxBackoffMs = 30000;

long long now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

int setup_signalfd() {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);

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

pid_t spawn_child(const char *path, const char *arg1, const char *label) {
    pid_t pid = fork();
    if (pid == -1) {
        std::perror("fork");
        return -1;
    }
    if (pid == 0) {
        if (arg1) {
            execl(path, path, arg1, static_cast<char *>(nullptr));
        } else {
            execl(path, path, static_cast<char *>(nullptr));
        }
        std::perror("execl");
        _exit(127);
    }
    std::fprintf(stderr, "[myinit] spawned %s (%s) as pid %d\n", label, path, pid);
    return pid;
}

} // namespace

int main(int argc, char *argv[]) {
    std::fprintf(stderr, "[myinit] starting as pid %d\n", getpid());

    const char *stage1_path     = (argc > 1) ? argv[1] : "/etc/myinit/stage1";
    const char *supervisor_path = (argc > 2) ? argv[2] : "/etc/myinit/supervisor";
    const char *services_dir    = (argc > 3) ? argv[3] : "/etc/sv";

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

    pid_t stage1_pid = spawn_child(stage1_path, nullptr, "stage1");
    if (stage1_pid == -1) {
        close(epfd);
        close(sfd);
        return 1;
    }

    bool stage1_completed = false;
    pid_t supervisor_pid = -1;          // -1 = сейчас не запущен
    long long supervisor_start_ms = 0;
    long long supervisor_next_start_ms = 0; // когда пробовать (пере)запустить
    long long supervisor_backoff_ms = 0;
    bool shutting_down = false;

    bool running = true;
    while (running) {
        // --- сколько ждать в epoll_wait ---
        int epoll_timeout = -1;
        if (!shutting_down && stage1_completed && supervisor_pid <= 0) {
            long long wait_left = std::max<long long>(0, supervisor_next_start_ms - now_ms());
            epoll_timeout = static_cast<int>(std::min<long long>(wait_left, INT_MAX));
        }

        epoll_event events[8];
        int n = epoll_wait(epfd, events, 8, epoll_timeout);
        if (n == -1) {
            if (errno == EINTR) continue;
            std::perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; ++i) {
            signalfd_siginfo si{};
            ssize_t r = read(sfd, &si, sizeof(si));
            if (r != static_cast<ssize_t>(sizeof(si))) continue;

            if (si.ssi_signo == SIGTERM || si.ssi_signo == SIGINT) {
                if (!shutting_down) {
                    shutting_down = true;
                    std::fprintf(stderr, "[myinit] got signal %d, shutting down\n", si.ssi_signo);
                    if (supervisor_pid > 0) {
                        std::fprintf(stderr, "[myinit] forwarding shutdown to supervisor\n");
                        kill(supervisor_pid, SIGTERM);
                    } else if (!stage1_completed) {
                        std::fprintf(stderr, "[myinit] shutdown requested before supervisor started, stopping stage1\n");
                        kill(stage1_pid, SIGTERM);
                    }
                }
                continue;
            }

            if (si.ssi_signo != SIGCHLD) continue;

            for (;;) {
                int status = 0;
                pid_t pid = waitpid(-1, &status, WNOHANG);
                if (pid <= 0) break;

                if (pid == stage1_pid) {
                    stage1_completed = true;
                    if (WIFEXITED(status)) {
                        std::fprintf(stderr, "[myinit] stage1 exited with code %d\n", WEXITSTATUS(status));
                    } else if (WIFSIGNALED(status)) {
                        std::fprintf(stderr, "[myinit] stage1 killed by signal %d\n", WTERMSIG(status));
                    }
                    supervisor_next_start_ms = now_ms(); // запустить супервизор при первой возможности
                } else if (pid == supervisor_pid) {
                    long long ran_ms = now_ms() - supervisor_start_ms;
                    if (WIFEXITED(status)) {
                        std::fprintf(stderr, "[myinit] supervisor exited with code %d after %lld ms\n",
                            WEXITSTATUS(status), ran_ms);
                    } else if (WIFSIGNALED(status)) {
                        std::fprintf(stderr, "[myinit] supervisor killed by signal %d after %lld ms\n",
                            WTERMSIG(status), ran_ms);
                    }
                    supervisor_pid = -1;
                    if (!shutting_down) {
                        if (ran_ms < kMinRuntimeMs) {
                            supervisor_backoff_ms = (supervisor_backoff_ms == 0)
                                ? kInitialBackoffMs
                                : std::min(supervisor_backoff_ms * 2, kMaxBackoffMs);
                            std::fprintf(stderr, "[myinit] supervisor died fast, backing off %lld ms before restart\n",
                                supervisor_backoff_ms);
                        } else {
                            supervisor_backoff_ms = 0;
                        }
                        supervisor_next_start_ms = now_ms() + supervisor_backoff_ms;
                    }
                } else {
                    std::fprintf(stderr, "[myinit] reaped pid %d (not tracked, likely an orphan)\n", pid);
                }
            }
        }

        if (shutting_down && stage1_completed && supervisor_pid <= 0) {
            running = false;
            continue;
        }

        if (!shutting_down && stage1_completed && supervisor_pid <= 0 &&
            now_ms() >= supervisor_next_start_ms) {
            supervisor_pid = spawn_child(supervisor_path, services_dir, "supervisor");
            supervisor_start_ms = now_ms();
        }
    }

    std::fprintf(stderr, "[myinit] all children stopped, exiting main loop\n");
    close(epfd);
    close(sfd);
    return 0;
}
