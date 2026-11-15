#include "linux_support.h"
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <linux/magic.h>
#include <cstring>
#include <sstream>

namespace {
struct Mount {
    const char *target;
    const char *type;
    long magic;
    unsigned long flags;
};
constexpr Mount mounts[] = {{"/proc", "proc", PROC_SUPER_MAGIC, MS_NOSUID | MS_NOEXEC | MS_NODEV},
                            {"/sys", "sysfs", SYSFS_MAGIC, MS_NOSUID | MS_NOEXEC | MS_NODEV},
                            {"/dev", "devtmpfs", TMPFS_MAGIC, MS_NOSUID},
                            {"/run", "tmpfs", TMPFS_MAGIC, MS_NOSUID | MS_NODEV}};

bool directory(const std::string &path) {
    struct stat st {};
    return lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string mounted_type(const char *target) {
    std::ifstream f("/proc/self/mountinfo");
    std::string line, result;
    while (std::getline(f, line)) {
        std::istringstream fields(line);
        std::string id, parent, device, root, point;
        fields >> id >> parent >> device >> root >> point;
        const auto sep = line.find(" - ");
        if (point == target && sep != std::string::npos) {
            std::istringstream tail(line.substr(sep + 3));
            tail >> result;
        }
    }
    return result;
}

bool ensure_mount(const Mount &m) {
    struct statfs fs {};
    struct statx point {};
    struct statx parent {};
    // Mount IDs detect bind mounts too, including ones on the same filesystem.
    // Fail closed when the kernel cannot establish whether a mount exists.
    if (statfs(m.target, &fs) < 0 ||
        statx(AT_FDCWD, m.target, AT_SYMLINK_NOFOLLOW, STATX_MNT_ID, &point) < 0 ||
        statx(AT_FDCWD, "/", AT_SYMLINK_NOFOLLOW, STATX_MNT_ID, &parent) < 0) {
        std::perror("[stage1] inspect mount");
        return false;
    }
    if (!(point.stx_mask & STATX_MNT_ID) || !(parent.stx_mask & STATX_MNT_ID)) {
        std::fprintf(stderr, "[stage1] kernel lacks mount-ID support\n");
        return false;
    }
    if (point.stx_mnt_id != parent.stx_mnt_id) {
        // /proc may not yet be readable; procfs magic is sufficient for /proc.
        if (fs.f_type == m.magic &&
            (std::strcmp(m.target, "/proc") == 0 || mounted_type(m.target) == m.type)) {
            std::fprintf(stderr, "[stage1] keeping existing %s at %s\n", m.type, m.target);
            return true;
        }
        std::fprintf(stderr, "[stage1] refusing to cover existing mount at %s\n", m.target);
        return false;
    }
    const char *data = std::strcmp(m.type, "tmpfs") == 0 ? "mode=0755" : nullptr;
    if (mount(m.type, m.target, m.type, m.flags, data) < 0) {
        std::fprintf(stderr, "[stage1] mount %s: %s\n", m.target, std::strerror(errno));
        return false;
    }
    return true;
}
} // namespace

int main(int argc, char **argv) {
    const bool dry = argc >= 2 && std::strcmp(argv[1], "--dry-run") == 0;
    const bool boot = argc == 2 && std::strcmp(argv[1], "--boot") == 0;
    if ((!dry && !boot) || (dry && argc > 3) || (boot && (getppid() != 1 || geteuid() != 0))) {
        std::fprintf(stderr, "usage: stage1 --boot (root, child of PID 1 only)\n"
                             "       stage1 --dry-run [ROOT] (validation only)\n");
        return 2;
    }
    const std::string root = dry && argc == 3 ? argv[2] : "";
    for (const auto &m : mounts) {
        if (!directory(root + m.target)) {
            std::fprintf(stderr, "[stage1] missing, symlinked or invalid directory: %s%s\n",
                         root.c_str(), m.target);
            return 1;
        }
    }
    for (const auto &m : mounts) {
        if (dry)
            std::printf("would ensure %s at %s%s\n", m.type, root.c_str(), m.target);
        else if (!ensure_mount(m))
            return 1;
    }
    if (dry)
        return 0;
    if (mkdir("/run/strazh", 0700) < 0 && errno != EEXIST) {
        std::perror("[stage1] mkdir /run/strazh");
        return 1;
    }
    struct stat st {};
    if (lstat("/run/strazh", &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != 0 ||
        (st.st_mode & 0077) != 0) {
        std::fprintf(stderr, "[stage1] /run/strazh must be a root-owned private directory\n");
        return 1;
    }
    // Hostname is deliberately left to an explicitly configured service.
    return 0;
}
