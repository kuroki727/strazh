#define main strazh_svctl_entry
#include "../svctl.cpp"
#undef main
#include <thread>
#include <functional>

namespace {
void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
void pair(Fd &a, Fd &b) {
    int sockets[2];
    check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets) == 0,
          "socketpair");
    a.reset(sockets[0]);
    b.reset(sockets[1]);
}
void framing() {
    Fd client_fd, server_fd;
    pair(client_fd, server_fd);
    Client server{std::move(server_fd), {}, {}, 0, now_ms() + 2000};
    int commands = 0;
    auto command = [&](const std::string &line) {
        ++commands;
        check(line == "status", "fragmented command corrupted");
        return std::string("OK\n");
    };
    check(pump_client(server, command), "idle client should not block");
    check(send_bytes(client_fd.get(), "sta", 3) == 3, "send fragment");
    check(pump_client(server, command) && commands == 0, "premature command");
    check(send_bytes(client_fd.get(), "tus\n", 4) == 4, "send fragment");
    check(!pump_client(server, command) && commands == 1, "command not completed");
    char buf[128];
    check(recv(client_fd.get(), buf, sizeof(buf), 0) == 3, "reply truncated");
    for (const std::string &request :
         {std::string(513, 'x'), std::string("status\nextra\n"), std::string("status")}) {
        pair(client_fd, server_fd);
        Client bad{std::move(server_fd), {}, {}, 0, now_ms() + 2000};
        send_bytes(client_fd.get(), request.data(), request.size());
        if (request == "status") {
            check(pump_client(bad, command), "incomplete request processed early");
            client_fd.reset();
            pump_client(bad, command);
            check(bad.output.rfind("ERR ", 0) == 0, "incomplete EOF accepted");
            continue;
        }
        for (int i = 0; i < 3 && pump_client(bad, command); ++i) {
        }
        const ssize_t size = recv(client_fd.get(), buf, sizeof(buf), 0);
        if (size <= 4 || std::string(buf, static_cast<size_t>(size)).rfind("ERR ", 0) != 0) {
            std::fprintf(stderr, "request length=%zu reply bytes=%zd errno=%d\n", request.size(),
                         size, errno);
            throw std::runtime_error("malformed framing accepted");
        }
    }
    pair(client_fd, server_fd);
    Client disconnected{std::move(server_fd), {}, "OK\n", 0, now_ms() + 2000};
    client_fd.reset();
    check(!pump_client(disconnected, command), "closed peer not detected");
}
void short_writes() {
    Fd a, b;
    pair(a, b);
    const std::string large = "OK\n" + std::string(900000, 'x') + "\n";
    Client server{std::move(a), {}, large, 0, now_ms() + 2000};
    check(pump_client(server, [](const std::string &) { return "ERR\n"; }),
          "did not produce partial write");
    check(server.sent > 0 && server.sent < large.size(), "short write was not exercised");
    std::string received;
    const auto deadline = now_ms() + 3000;
    while (received.size() < large.size() && now_ms() < deadline) {
        char buf[4096];
        const ssize_t n = recv(b.get(), buf, sizeof(buf), 0);
        if (n > 0)
            received.append(buf, static_cast<size_t>(n));
        if (server.sent < large.size())
            pump_client(server, [](const std::string &) { return "ERR\n"; });
    }
    check(received == large, "partial writes corrupted response");
}
void cli_exchange() {
    for (const std::string &response :
         {std::string("OK\nworker: running\n"), std::string("ERR unknown service\n"),
          std::string(""), std::string("OK"), std::string("OK\n\0\n", 5),
          std::string(1024 * 1024 + 1, 'x')}) {
        Fd client, server;
        pair(client, server);
        std::thread peer([&] {
            char buf[512];
            while (recv(server.get(), buf, sizeof(buf), 0) < 0 && errno == EAGAIN)
                std::this_thread::yield();
            size_t sent = 0;
            while (sent < response.size()) {
                const ssize_t w = send_bytes(server.get(), response.data() + sent,
                                             std::min(size_t(17), response.size() - sent));
                if (w > 0)
                    sent += static_cast<size_t>(w);
                else if (w < 0 && errno == EAGAIN)
                    std::this_thread::yield();
                else
                    break;
            }
            server.reset();
        });
        bool failed = false;
        try {
            check(exchange(client.get(), "status\n", now_ms() + 3000) == response,
                  "CLI corrupted response");
        } catch (const std::exception &) {
            failed = true;
        }
        client.reset();
        peer.join();
        const bool valid = response.rfind("OK\nworker", 0) == 0 || response.rfind("ERR ", 0) == 0;
        check(failed != valid, "CLI accepted malformed or rejected valid response");
    }
    Fd client, server;
    pair(client, server);
    bool failed = false;
    const auto start = now_ms();
    try {
        exchange(client.get(), "status\n", start + 100);
    } catch (const std::exception &) {
        failed = true;
    }
    check(failed && now_ms() - start >= 90 && now_ms() - start < 1000, "CLI timeout not bounded");
}
} // namespace
int main() {
    try {
        framing();
        short_writes();
        cli_exchange();
        std::fprintf(stderr, "PASS: socketpair framing, malformed requests, disconnects, partial "
                             "writes, CLI validation and timeout\n");
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
