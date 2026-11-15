#include "../linux_support.h"
#include <sys/stat.h>

namespace {
volatile sig_atomic_t term_count = 0;
void count_term(int) { ++term_count; }
} // namespace

int main() {
    std::ifstream config("mode");
    std::string mode;
    config >> mode;
    sigset_t mask;
    if (sigprocmask(SIG_SETMASK, nullptr, &mask) < 0)
        return 90;
    struct sigaction action{};
    if (sigaction(SIGPIPE, nullptr, &action) < 0)
        return 91;
    strazh::Fd fd(open("events", O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0600));
    if (fd.get() < 0)
        return 92;
    const std::string event = std::to_string(getpid()) + " " + std::to_string(getppid()) + " " +
                              std::to_string(getpgrp()) + " " + std::to_string(strazh::now_ms()) +
                              " " + std::to_string(sigismember(&mask, SIGTERM)) + " " +
                              std::to_string(sigismember(&mask, SIGINT)) + " " +
                              std::to_string(action.sa_handler == SIG_DFL) + "\n";
    if (write(fd.get(), event.data(), event.size()) != static_cast<ssize_t>(event.size()))
        return 93;
    if (mode == "exit")
        return 7;
    if (mode == "grace") {
        sigset_t blocked, original;
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGTERM);
        if (sigprocmask(SIG_BLOCK, &blocked, &original) < 0)
            return 95;
        signal(SIGTERM, count_term);
        while (!term_count)
            sigsuspend(&original);
        if (sigprocmask(SIG_SETMASK, &original, nullptr) < 0)
            return 96;
        timespec delay{0, 200000000};
        while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {
        }
        std::ofstream("terms") << term_count;
        return 0;
    }
    if (mode == "ignore")
        signal(SIGTERM, SIG_IGN);
    if (mode == "tree" || mode == "orphan" || mode == "churn") {
        const int count = mode == "churn" ? 48 : 1;
        for (int i = 0; i < count; ++i) {
            const pid_t child = fork();
            if (child < 0)
                return 94;
            if (child == 0) {
                if (mode == "churn")
                    _exit(0);
                signal(SIGTERM, SIG_IGN);
                for (;;)
                    pause();
            }
            std::ofstream("descendants", std::ios::app) << child << '\n';
        }
        if (mode == "orphan")
            return 8;
    }
    for (;;)
        pause();
}
