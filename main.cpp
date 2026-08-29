#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <csignal>

#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/epoll.h>
#include <fcntl.h>

#if __has_include(<sys/signalfd.h>)
#include <sys/signalfd.h>
#elif __has_include(<linux/signalfd.h>)
#include <linux/signalfd.h>
#else
#error "This program requires Linux signalfd support"
#endif

namespace {

int setup_signalfd() {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);

    // Сигналы должны быть заблокированы для обычной доставки —
    // мы читаем их синхронно через signalfd в основном цикле.
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

// Возвращает true, если среди пожатых детей оказался stage1_pid.
bool reap_zombies(pid_t stage1_pid, int &stage1_status) {
    bool stage1_finished = false;
    for (;;) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0) {
            break; // больше нет завершившихся детей (или ECHILD — тоже ок)
        }

        if (pid == stage1_pid) {
            stage1_finished = true;
            stage1_status = status;
        } else {
            std::fprintf(stderr, "[myinit] reaped pid %d (not tracked)\n", pid);
        }
    }
    return stage1_finished;
}

pid_t spawn_stage1(const char *path) {
    pid_t pid = fork();
    if (pid == -1) {
        std::perror("fork");
        return -1;
    }
    if (pid == 0) {
        execl(path, path, static_cast<char *>(nullptr));
        std::perror("execl stage1");
        _exit(127); // конвенция shell: команда не найдена/не исполняема
    }
    std::fprintf(stderr, "[myinit] spawned stage1 (%s) as pid %d\n", path, pid);
    return pid;
}

} // namespace

int main(int argc, char *argv[]) {
    std::fprintf(stderr, "[myinit] starting as pid %d\n", getpid());

    const char *stage1_path = (argc > 1) ? argv[1] : "/etc/myinit/stage1";

    int sfd = setup_signalfd();
    if (sfd == -1) {
        return 1;
    }

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd == -1) {
        std::perror("epoll_create1");
        close(sfd);  // FIX: close sfd on error
        return 1;
    }

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = sfd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, sfd, &ev) == -1) {
        std::perror("epoll_ctl");
        close(epfd);  // FIX: close both fds on error
        close(sfd);
        return 1;
    }

    pid_t stage1_pid = spawn_stage1(stage1_path);
    if (stage1_pid == -1) {
        close(epfd);  // FIX: close both fds on error
        close(sfd);
        return 1;
    }

    bool running = true;
    while (running) {
        epoll_event events[8];
        int n = epoll_wait(epfd, events, 8, -1);
        if (n == -1) {
            if (errno == EINTR) continue;
            std::perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; ++i) {
            signalfd_siginfo si{};
            ssize_t r = read(sfd, &si, sizeof(si));
            if (r != static_cast<ssize_t>(sizeof(si))) continue;

            switch (si.ssi_signo) {
                case SIGCHLD: {
                    int stage1_status = 0;
                    if (reap_zombies(stage1_pid, stage1_status)) {
                        if (WIFEXITED(stage1_status)) {
                            std::fprintf(stderr, "[myinit] stage1 exited with code %d\n",
                                WEXITSTATUS(stage1_status));
                        } else if (WIFSIGNALED(stage1_status)) {
                            std::fprintf(stderr, "[myinit] stage1 killed by signal %d\n",
                                WTERMSIG(stage1_status));
                        }
                        std::fprintf(stderr, "[myinit] TODO: spawn service supervisor here\n");
                    }
                    break;
                }
                case SIGTERM:
                case SIGINT:
                    std::fprintf(stderr,
                        "[myinit] got signal %d, stopping loop (shutdown logic comes later)\n",
                        si.ssi_signo);
                    running = false;
                    break;
            }
        }
    }

    close(epfd);
    close(sfd);
    return 0;
}
