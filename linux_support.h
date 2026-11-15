#pragma once

#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <stdexcept>
#include <utility>
#include <vector>
#include <set>

namespace strazh {

class Fd {
    int fd_ = -1;

  public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() { reset(); }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    Fd(Fd &&other) noexcept : fd_(other.release()) {}
    Fd &operator=(Fd &&other) noexcept {
        if (this != &other)
            reset(other.release());
        return *this;
    }
    int get() const { return fd_; }
    int release() { return std::exchange(fd_, -1); }
    void reset(int fd = -1) {
        // Linux releases the descriptor even if close reports EINTR; do not retry.
        if (fd_ >= 0)
            close(fd_);
        fd_ = fd;
    }
};

inline long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline long long next_backoff(long long previous, long long runtime) {
    if (runtime >= 30000)
        previous = 0;
    return previous == 0 ? 500 : std::min(previous * 2, 30000LL);
}

inline int setup_signalfd() {
    sigset_t mask;
    sigemptyset(&mask);
    for (int sig : {SIGCHLD, SIGTERM, SIGINT})
        sigaddset(&mask, sig);
    if (sigprocmask(SIG_BLOCK, &mask, nullptr) < 0)
        return -1;
    struct sigaction action {};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    for (int sig : {SIGCHLD, SIGTERM, SIGINT}) {
        if (sigaction(sig, &action, nullptr) < 0)
            return -1;
    }
    return signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
}

inline bool epoll_add(int epfd, int fd, unsigned events = EPOLLIN) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    return epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) == 0;
}

inline bool reset_child_signals() {
    struct sigaction action {};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    for (int sig = 1; sig < NSIG; ++sig) {
        if (sig == SIGKILL || sig == SIGSTOP)
            continue;
        // glibc reserves some real-time signal numbers and reports EINVAL.
        if (sigaction(sig, &action, nullptr) < 0 && errno != EINVAL)
            return false;
    }
    sigset_t empty;
    sigemptyset(&empty);
    return sigprocmask(SIG_SETMASK, &empty, nullptr) == 0;
}

struct Child {
    pid_t pid = -1;
    Fd exec_error;
};

inline Child spawn(const char *path, const char *arg = nullptr, const char *cwd = nullptr) {
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC | O_NONBLOCK) < 0)
        return {};
    Fd read_end(pipefd[0]), write_end(pipefd[1]);
    const pid_t parent = getpid();
    const pid_t pid = fork();
    if (pid < 0)
        return {};
    if (pid == 0) {
        read_end.reset();
        // Each child owns a session/process group; never signal the host's group.
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0 || getppid() != parent || setsid() < 0 ||
            !reset_child_signals() || (cwd && chdir(cwd) < 0)) {
            const int error = errno ? errno : ESRCH;
            while (write(write_end.get(), &error, sizeof(error)) < 0 && errno == EINTR) {
            }
            _exit(127);
        }
        if (arg)
            execl(path, path, arg, static_cast<char *>(nullptr));
        else
            execl(path, path, static_cast<char *>(nullptr));
        const int error = errno;
        while (write(write_end.get(), &error, sizeof(error)) < 0 && errno == EINTR) {
        }
        _exit(127);
    }
    return {pid, std::move(read_end)};
}

// 0: still executing setup, 1: exec succeeded, -1: setup/exec failed.
inline int exec_result(Fd &fd) {
    if (fd.get() < 0)
        return 1;
    int error = 0;
    ssize_t n;
    do {
        n = read(fd.get(), &error, sizeof(error));
    } while (n < 0 && errno == EINTR);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return 0;
    fd.reset();
    if (n == 0)
        return 1;
    if (n > 0)
        errno = error;
    std::perror("child setup/exec");
    return -1;
}

inline bool signal_pid(pid_t pid, int sig) {
    if (pid <= 0)
        return true;
    if (kill(pid, sig) == 0 || errno == ESRCH)
        return true;
    std::perror("kill");
    return false;
}

inline bool signal_group(pid_t pid, int sig) {
    if (pid <= 0)
        return true;
    if (kill(-pid, sig) == 0 || errno == ESRCH)
        return true;
    std::perror("kill process group");
    return false;
}

inline void signal_child_group(pid_t pid, int sig) {
    if (pid <= 0)
        return;
    if (kill(-pid, sig) == 0)
        return;
    if (errno == ESRCH)
        signal_pid(pid, sig); // child may not have called setsid() yet
    else
        std::perror("kill child group");
}

inline bool group_exists(pid_t pid) { return pid > 0 && (kill(-pid, 0) == 0 || errno != ESRCH); }

// Only our own children, including orphans adopted as a subreaper. Unlike
// kill(-1), this remains safe when running the integration harness on a host.
inline std::vector<pid_t> children() {
    std::ifstream file("/proc/self/task/" + std::to_string(getpid()) + "/children");
    if (!file)
        throw std::runtime_error("cannot inspect own children via procfs");
    std::vector<pid_t> result;
    pid_t pid;
    while (file >> pid)
        if (pid > 0)
            result.push_back(pid);
    return result;
}

inline void signal_children(int sig) {
    for (pid_t pid : children())
        signal_pid(pid, sig);
}

inline void signal_new_children(int sig, std::set<pid_t> &signalled) {
    for (pid_t pid : children())
        if (signalled.insert(pid).second)
            signal_pid(pid, sig);
}

// Last-resort cleanup on exceptions as well as normal returns. It operates
// only on this process's children, and never waits indefinitely after KILL.
class ChildReaper {
  public:
    ~ChildReaper() noexcept {
        const long long deadline = now_ms() + 1000;
        do {
            try {
                for (pid_t pid : children()) {
                    signal_group(pid, SIGKILL);
                    signal_pid(pid, SIGKILL);
                }
            } catch (...) {
            }
            pid_t pid;
            do {
                pid = waitpid(-1, nullptr, WNOHANG);
            } while (pid > 0 || (pid < 0 && errno == EINTR));
            if (pid < 0 && errno == ECHILD)
                return;
            timespec delay{0, 10000000};
            nanosleep(&delay, nullptr);
        } while (now_ms() < deadline);
    }
};

inline bool valid_name(const std::string &name) {
    if (name.empty() || name.size() > 128 || name[0] == '.')
        return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-' || c == '.';
    });
}

} // namespace strazh
