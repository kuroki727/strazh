#pragma once

#include "linux_support.h"
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace strazh {
constexpr size_t kRequestLimit = 512;
constexpr size_t kResponseLimit = 1024 * 1024;
constexpr long long kClientTimeoutMs = 2000;

inline ssize_t send_bytes(int fd, const void *data, size_t length) {
    iovec buffer{const_cast<void *>(data), length};
    msghdr message{};
    message.msg_iov = &buffer;
    message.msg_iovlen = 1;
    return sendmsg(fd, &message, MSG_NOSIGNAL);
}

inline sockaddr_un socket_address(const std::string &path) {
    sockaddr_un addr{};
    if (path.empty() || path.size() >= sizeof(addr.sun_path))
        throw std::runtime_error("invalid or overlong socket path");
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return addr;
}

struct Client {
    Fd fd;
    std::string input, output;
    size_t sent = 0;
    long long deadline = now_ms() + kClientTimeoutMs;
};

template <class Command> bool pump_client(Client &client, Command command) {
    const int fd = client.fd.get();
    bool close_client = false;
    if (client.output.empty() && !close_client) {
        char buf[1024];
        ssize_t r;
        do {
            r = recv(fd, buf, sizeof(buf), 0);
        } while (r < 0 && errno == EINTR);
        if (r > 0)
            client.input.append(buf, static_cast<size_t>(r));
        const auto end = client.input.find('\n');
        if (client.input.size() > kRequestLimit)
            client.output = "ERR request too large\n";
        else if (end != std::string::npos) {
            if (end + 1 != client.input.size())
                client.output = "ERR one request per connection\n";
            else
                client.output = command(client.input.substr(0, end));
        } else if (r == 0)
            client.output = "ERR incomplete request; newline required\n";
        else if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            close_client = true;
    }
    if (!client.output.empty() && !close_client) {
        ssize_t w;
        do {
            w = send_bytes(fd, client.output.data() + client.sent,
                           client.output.size() - client.sent);
        } while (w < 0 && errno == EINTR);
        if (w > 0)
            client.sent += static_cast<size_t>(w);
        else if (w == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
            close_client = true;
        if (client.sent == client.output.size())
            close_client = true;
    }
    return !close_client;
}

class ControlSocket {
    Fd parent_, lock_, socket_;
    std::string name_;
    struct stat identity_{};

  public:
    explicit ControlSocket(const std::string &path) {
        const auto addr = socket_address(path);
        const std::filesystem::path p(path);
        if (!p.is_absolute() || p.lexically_normal() != p ||
            std::filesystem::canonical(p.parent_path()) != p.parent_path())
            throw std::runtime_error("socket needs an absolute path without symlink parents");
        parent_.reset(
            open(p.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        struct stat st{};
        if (parent_.get() < 0 || fstat(parent_.get(), &st) < 0 || st.st_uid != geteuid() ||
            (st.st_mode & 0022) != 0)
            throw std::runtime_error(
                "socket parent must be owned by this user and not writable by others");
        name_ = p.filename().string();
        lock_.reset(openat(parent_.get(), (name_ + ".lock").c_str(),
                           O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600));
        if (lock_.get() < 0 || fstat(lock_.get(), &st) < 0 || !S_ISREG(st.st_mode) ||
            st.st_uid != geteuid() || st.st_nlink != 1 || (st.st_mode & 0077) != 0 ||
            flock(lock_.get(), LOCK_EX | LOCK_NB) < 0)
            throw std::runtime_error("unsafe lock file or supervisor already running");
        socket_.reset(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
        if (socket_.get() < 0)
            throw std::runtime_error("socket creation failed");
        if (fstatat(parent_.get(), name_.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0) {
            if (!S_ISSOCK(st.st_mode) || st.st_uid != geteuid())
                throw std::runtime_error("refusing to remove non-socket or foreign socket");
            Fd probe(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
            if (probe.get() < 0 ||
                connect(probe.get(), reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) ==
                    0 ||
                errno != ECONNREFUSED)
                throw std::runtime_error(
                    "socket is active or cannot safely be identified as stale");
            struct stat current{};
            if (fstatat(parent_.get(), name_.c_str(), &current, AT_SYMLINK_NOFOLLOW) < 0 ||
                current.st_dev != st.st_dev || current.st_ino != st.st_ino ||
                unlinkat(parent_.get(), name_.c_str(), 0) < 0)
                throw std::runtime_error("stale socket changed during cleanup");
        } else if (errno != ENOENT)
            throw std::runtime_error("cannot inspect socket path");
        const mode_t old_mask = umask(0077);
        const int bound =
            bind(socket_.get(), reinterpret_cast<const sockaddr *>(&addr), sizeof(addr));
        umask(old_mask);
        if (bound < 0)
            throw std::runtime_error("socket bind failed");
        if (fstatat(parent_.get(), name_.c_str(), &identity_, AT_SYMLINK_NOFOLLOW) < 0)
            throw std::runtime_error("cannot identify bound socket");
        if (fchmodat(parent_.get(), name_.c_str(), 0600, 0) < 0 || listen(socket_.get(), 16) < 0) {
            cleanup();
            throw std::runtime_error("socket listen failed");
        }
    }
    ~ControlSocket() { cleanup(); }
    ControlSocket(const ControlSocket &) = delete;
    ControlSocket &operator=(const ControlSocket &) = delete;
    int get() const { return socket_.get(); }

  private:
    void cleanup() {
        struct stat st{};
        if (identity_.st_ino &&
            fstatat(parent_.get(), name_.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0 &&
            st.st_dev == identity_.st_dev && st.st_ino == identity_.st_ino && S_ISSOCK(st.st_mode))
            unlinkat(parent_.get(), name_.c_str(), 0);
        identity_.st_ino = 0;
        // Retain the lock pathname: unlinking it would allow competing locks.
    }
};
} // namespace strazh
