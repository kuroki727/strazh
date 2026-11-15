#include "control_socket.h"
#include <poll.h>
#include <cstdlib>

namespace {
using namespace strazh;

bool ready(int fd, short events, long long deadline) {
    for (;;) {
        const auto remaining = deadline - now_ms();
        if (remaining <= 0)
            return false;
        pollfd p{fd, events, 0};
        const int n = poll(&p, 1, static_cast<int>(remaining));
        if (n < 0 && errno == EINTR)
            continue;
        return n > 0 && (p.revents & (events | POLLHUP)) != 0 && (p.revents & POLLNVAL) == 0;
    }
}

std::string exchange(int fd, const std::string &command, long long deadline) {
    size_t sent = 0;
    while (sent < command.size()) {
        if (!ready(fd, POLLOUT, deadline))
            throw std::runtime_error("send timeout");
        const ssize_t w = send_bytes(fd, command.data() + sent, command.size() - sent);
        if (w < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (w <= 0)
            throw std::runtime_error("send failed");
        sent += static_cast<size_t>(w);
    }
    std::string response;
    for (;;) {
        if (!ready(fd, POLLIN, deadline))
            throw std::runtime_error("response timeout");
        char buf[4096];
        const ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (n < 0)
            throw std::runtime_error("response read failed");
        if (n == 0)
            break;
        response.append(buf, static_cast<size_t>(n));
        if (response.size() > kResponseLimit)
            throw std::runtime_error("response too large");
    }
    const bool ok = response.rfind("OK\n", 0) == 0;
    const bool error = response.rfind("ERR ", 0) == 0;
    if ((!ok && !error) || response.back() != '\n' || response.find('\0') != std::string::npos)
        throw std::runtime_error("empty or malformed response");
    return response;
}

int run(int argc, char **argv) {
    if (argc < 2)
        return 2;
    const std::string verb = argv[1];
    if (!((verb == "status" && argc == 2) ||
          ((verb == "start" || verb == "stop" || verb == "restart") && argc == 3 &&
           valid_name(argv[2]))))
        return 2;
    const std::string command = verb + (argc == 3 ? " " + std::string(argv[2]) : "") + "\n";
    const char *env = std::getenv("STRAZH_SOCKET");
    const auto addr = socket_address(env ? env : "/run/strazh/supervisor.sock");
    Fd fd(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (fd.get() < 0)
        throw std::runtime_error("socket creation failed");
    const long long deadline = now_ms() + 3000;
    if (connect(fd.get(), reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) < 0) {
        if (errno != EINPROGRESS || !ready(fd.get(), POLLOUT, deadline))
            throw std::runtime_error("connection failed or timed out");
        int error = 0;
        socklen_t length = sizeof(error);
        if (getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &error, &length) < 0 || error)
            throw std::runtime_error("connection failed");
    }
    const auto response = exchange(fd.get(), command, deadline);
    const bool ok = response.rfind("OK\n", 0) == 0;
    if (std::fwrite(response.data(), 1, response.size(), stdout) != response.size() ||
        std::fflush(stdout) != 0)
        throw std::runtime_error("output failed");
    return ok ? 0 : 1;
}
} // namespace

int main(int argc, char **argv) {
    try {
        const int result = run(argc, argv);
        if (result == 2)
            std::fprintf(stderr, "usage: svctl status | start NAME | stop NAME | restart NAME\n");
        return result;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "svctl: %s\n", e.what());
        return 1;
    }
}
