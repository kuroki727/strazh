#include <sys/mount.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <string>

namespace {

bool do_mount(const std::string &source, const std::string &target,
              const char *fstype, unsigned long flags) {
    if (mount(source.c_str(), target.c_str(), fstype, flags, nullptr) == -1) {
        std::fprintf(stderr, "[stage1] mount %s -> %s (%s) failed: %s\n",
            source.c_str(), target.c_str(), fstype, std::strerror(errno));
        return false;
    }
    std::fprintf(stderr, "[stage1] mounted %s at %s (%s)\n",
        source.c_str(), target.c_str(), fstype);
    return true;
}

} // namespace

// Опциональный argv[1] — корень, под который монтируем proc/sys/dev.
// На реальной машине вызывается без аргументов (root == ""),
// тогда пути — самые обычные /proc, /sys, /dev.
int main(int argc, char *argv[]) {
    std::fprintf(stderr, "[stage1] starting as pid %d\n", getpid());

    std::string root = (argc > 1) ? argv[1] : "";

    bool ok = true;
    ok &= do_mount("proc", root + "/proc", "proc",
        MS_NOSUID | MS_NOEXEC | MS_NODEV);
    ok &= do_mount("sysfs", root + "/sys", "sysfs",
        MS_NOSUID | MS_NOEXEC | MS_NODEV);
    // devtmpfs на реальном железе создаёт узлы устройств сама;
    // здесь пока tmpfs — devtmpfs заведём отдельным шагом вместе с udev.
    ok &= do_mount("tmpfs", root + "/dev", "tmpfs", MS_NOSUID);

    const char *hostname = "myinit-test";
    if (sethostname(hostname, static_cast<int>(std::strlen(hostname))) == -1) {
        std::fprintf(stderr, "[stage1] sethostname failed: %s\n", std::strerror(errno));
        ok = false;
    } else {
        std::fprintf(stderr, "[stage1] hostname set to %s\n", hostname);
    }

    return ok ? 0 : 1;
}
