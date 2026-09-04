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
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>
#include <map>

namespace {

struct Service {
    std::string name;
    std::string run_path;
};

// Ищет <services_dir>/<name>/run для каждой поддиректории.
// Каталог без исполняемого run пропускается (не всё в services_dir
// обязано быть сервисом — например, там может лежать README).
std::vector<Service> discover_services(const std::string &services_dir) {
    std::vector<Service> services;

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

        services.push_back({name, run_path});
    }
    closedir(dir);
    return services;
}

pid_t spawn_service(const Service &svc) {
    pid_t pid = fork();
    if (pid == -1) {
        std::fprintf(stderr, "[supervisor] fork for %s failed: %s\n",
            svc.name.c_str(), std::strerror(errno));
        return -1;
    }
    if (pid == 0) {
        execl(svc.run_path.c_str(), svc.run_path.c_str(), static_cast<char *>(nullptr));
        std::fprintf(stderr, "[supervisor] execl %s failed: %s\n",
            svc.run_path.c_str(), std::strerror(errno));
        _exit(127);
    }
    std::fprintf(stderr, "[supervisor] started %s as pid %d\n", svc.name.c_str(), pid);
    return pid;
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

    std::vector<Service> services = discover_services(services_dir);
    if (services.empty()) {
        std::fprintf(stderr, "[supervisor] no services found in %s\n", services_dir.c_str());
    }

    std::map<pid_t, std::string> running; // pid -> имя сервиса
    for (const Service &svc : services) {
        pid_t pid = spawn_service(svc);
        if (pid > 0) {
            running[pid] = svc.name;
        }
    }

    int sfd = setup_signalfd();
    if (sfd == -1) {
        return 1;
    }
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

    bool running_loop = true;
    while (running_loop && !running.empty()) {
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

            if (si.ssi_signo == SIGTERM) {
                std::fprintf(stderr, "[supervisor] got SIGTERM (graceful stop not implemented yet)\n");
                running_loop = false;
                break;
            }

            // SIGCHLD: подбираем всех завершившихся детей.
            for (;;) {
                int status = 0;
                pid_t pid = waitpid(-1, &status, WNOHANG);
                if (pid <= 0) break;

                auto it = running.find(pid);
                std::string name = (it != running.end()) ? it->second : "(unknown)";
                if (it != running.end()) running.erase(it);

                if (WIFEXITED(status)) {
                    std::fprintf(stderr, "[supervisor] %s (pid %d) exited with code %d — TODO: respawn\n",
                        name.c_str(), pid, WEXITSTATUS(status));
                } else if (WIFSIGNALED(status)) {
                    std::fprintf(stderr, "[supervisor] %s (pid %d) killed by signal %d — TODO: respawn\n",
                        name.c_str(), pid, WTERMSIG(status));
                }
            }
        }
    }

    close(epfd);
    close(sfd);
    return 0;
}
