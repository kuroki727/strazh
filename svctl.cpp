#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <string>

namespace {

const char *socket_path() {
    const char *env = getenv("STRAZH_SOCKET");
    return env ? env : "/run/strazh/supervisor.sock";
}

} // namespace

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <status|start NAME|stop NAME|restart NAME>\n", argv[0]);
        return 2;
    }

    std::string cmd;
    for (int i = 1; i < argc; ++i) {
        if (i > 1) cmd += " ";
        cmd += argv[i];
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd == -1) {
        std::perror("socket");
        return 1;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path(), sizeof(addr.sun_path) - 1);

    if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == -1) {
        std::fprintf(stderr, "svctl: connect %s failed: %s\n", socket_path(), std::strerror(errno));
        close(fd);
        return 1;
    }

    if (send(fd, cmd.data(), cmd.size(), 0) == -1) {
        std::perror("send");
        close(fd);
        return 1;
    }

    std::string response;
    char buf[4096];
    ssize_t r;
    while ((r = recv(fd, buf, sizeof(buf), 0)) > 0) {
        response.append(buf, static_cast<size_t>(r));
    }
    close(fd);

    std::fwrite(response.data(), 1, response.size(), stdout);

    return (response.rfind("ERR", 0) == 0) ? 1 : 0;
}
