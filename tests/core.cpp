#define main strazh_supervisor_entry
#include "../supervisor.cpp"
#undef main
#include <functional>
#include <sys/resource.h>

namespace {
void check(bool ok, const std::string &message) {
    if (!ok)
        throw std::runtime_error(message);
}
void expect_error(const std::function<void()> &fn) {
    bool failed = false;
    try {
        fn();
    } catch (const std::exception &) {
        failed = true;
    }
    check(failed, "expected error was not reported");
}
void sleep_ms(long ms) {
    timespec ts{ms / 1000, (ms % 1000) * 1000000};
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {
    }
}
struct Fixture {
    std::string root;
    std::string helper;
    std::vector<ServiceState> services;
    std::map<std::string, size_t> index;
    std::map<pid_t, size_t> pids;
    explicit Fixture(std::string executable) : helper(std::move(executable)) {
        char path[] = "/tmp/strazh-core-XXXXXX";
        const char *dir = mkdtemp(path);
        if (!dir)
            throw std::runtime_error("mkdtemp");
        root = dir;
    }
    ~Fixture() {
        for (auto &svc : services)
            signal_group(svc.pid, SIGKILL);
        {
            ChildReaper cleanup;
        }
        std::filesystem::remove_all(root);
    }
    std::string add(const std::string &name, const std::string &mode = "hold",
                    const std::string &after = "") {
        const auto path = root + "/" + name;
        std::filesystem::create_directory(path);
        std::filesystem::create_symlink(helper, path + "/run");
        std::ofstream(path + "/mode") << mode;
        if (!after.empty())
            std::ofstream(path + "/after") << after;
        return path;
    }
    void load() {
        services = resolve_order(discover_services(root));
        for (size_t i = 0; i < services.size(); ++i)
            index[services[i].name] = i;
    }
    void tick() {
        check(reap_services(services, pids), "reap failure");
        update_services(services);
        start_services(services, index, pids);
    }
    void until(const std::function<bool()> &done, long long timeout = 3000) {
        const auto end = now_ms() + timeout;
        do {
            tick();
            if (done())
                return;
            sleep_ms(10);
        } while (now_ms() < end);
        throw std::runtime_error("lifecycle condition timed out");
    }
    ServiceState &svc(const std::string &name) { return services.at(index.at(name)); }
    std::vector<std::vector<long long>> events(const std::string &name) {
        std::ifstream f(root + "/" + name + "/events");
        std::vector<std::vector<long long>> rows;
        std::string line;
        while (std::getline(f, line)) {
            std::istringstream in(line);
            std::vector<long long> row;
            long long value;
            while (in >> value)
                row.push_back(value);
            if (row.size() == 7)
                rows.push_back(std::move(row));
        }
        return rows;
    }
    void command(const std::string &text) {
        check(handle_command(text, services, index, false) == "OK\n", "command rejected: " + text);
    }
};

void discovery(const std::string &helper) {
    Fixture f(helper);
    f.add("b");
    f.add("a", "hold", "b\nb\n# comment\n");
    f.add(".hidden");
    std::filesystem::create_directory(f.root + "/ignored");
    std::ofstream(f.root + "/ignored/run") << "non-executable";
    f.load();
    check(f.services.size() == 2 && f.services[0].name == "b" && f.services[1].name == "a",
          "ordering/discovery");
    check(f.services[1].after.size() == 1, "duplicate dependency");
    for (const auto &dep : {"missing", "a", "../escape"}) {
        std::ofstream(f.root + "/a/after") << dep;
        expect_error([&] { resolve_order(discover_services(f.root)); });
    }
    std::ofstream(f.root + "/a/after") << "b";
    std::ofstream(f.root + "/b/after") << "a";
    expect_error([&] { resolve_order(discover_services(f.root)); });
    std::filesystem::remove(f.root + "/a/after");
    check(mkfifo((f.root + "/a/after").c_str(), 0600) == 0, "mkfifo");
    expect_error([&] { discover_services(f.root); });
    expect_error([&] { discover_services(f.root + "/missing"); });
}

void lifecycle(const std::string &helper) {
    Fixture f(helper);
    f.add("a", "hold", "b");
    f.add("b");
    f.load();
    f.tick();
    check(f.svc("b").pid > 0 && f.svc("a").pid == 0, "dependent started before exec");
    f.until([&] { return !f.events("a").empty(); });
    const auto row = f.events("a")[0];
    check(row[0] == row[2] && row[4] == 0 && row[5] == 0 && row[6] == 1,
          "inherited signals/session");
    const pid_t old = f.svc("a").pid;
    for (int i = 0; i < 20; ++i) {
        f.command("start a");
        f.tick();
    }
    check(f.events("a").size() == 1, "duplicate start");
    f.command("restart a");
    f.until([&] { return f.events("a").size() == 2; });
    check(kill(old, 0) < 0 && errno == ESRCH, "old leader not reaped before restart");
    f.command("stop a");
    f.until([&] { return f.svc("a").pid == 0; });
    f.command("stop b");
    f.until([&] { return f.svc("b").pid == 0; });
    f.command("start a");
    f.tick();
    check(f.svc("a").pid == 0, "stopped dependency not respected");
    check(build_status_report(f.services, f.index).find("a: blocked") != std::string::npos,
          "blocked status");
    f.command("start b");
    f.until([&] { return f.events("a").size() == 3; });
    for (const std::string &line :
         std::vector<std::string>{"status extra", "start", "stop a extra", "stop ../x",
                                  "start missing", "", std::string("status\0", 7)})
        check(handle_command(line, f.services, f.index, false).rfind("ERR ", 0) == 0,
              "malformed command accepted");
    check(handle_command("start a", f.services, f.index, true).rfind("ERR ", 0) == 0,
          "command during shutdown");
}

void backoff(const std::string &helper) {
    check(next_backoff(0, 0) == 500 && next_backoff(500, 0) == 1000 &&
              next_backoff(16000, 0) == 30000 && next_backoff(30000, 0) == 30000 &&
              next_backoff(30000, 30000) == 500,
          "backoff bounds/reset");
    Fixture f(helper);
    f.add("crash", "exit");
    f.load();
    f.until([&] { return f.events("crash").size() >= 4 && !f.svc("crash").pid; }, 6000);
    const auto rows = f.events("crash");
    check(rows[1][3] - rows[0][3] >= 500 && rows[2][3] - rows[1][3] >= 1000 &&
              rows[3][3] - rows[2][3] >= 2000 && f.svc("crash").backoff_ms == 4000,
          "exponential restart timings");
    f.command("start crash");
    check(f.svc("crash").backoff_ms == 4000, "start bypassed backoff");
}

void failed_exec_and_fork(const std::string &helper) {
    Fixture f(helper);
    const auto bad = f.add("bad");
    std::filesystem::remove(bad + "/run");
    std::ofstream(bad + "/run") << "#!/does/not/exist\n";
    chmod((bad + "/run").c_str(), 0700);
    f.add("dep", "hold", "bad");
    f.load();
    f.until([&] { return f.svc("bad").backoff_ms >= 1000; });
    check(f.events("dep").empty() && f.svc("dep").pid == 0, "failed exec released dependency");
    f.command("stop bad");
    f.until([&] { return f.svc("bad").pid == 0; });
    struct rlimit original{};
    check(getrlimit(RLIMIT_NOFILE, &original) == 0, "getrlimit");
    auto limited = original;
    limited.rlim_cur = 3;
    check(setrlimit(RLIMIT_NOFILE, &limited) == 0, "setrlimit");
    const bool spawned = spawn_service(f.svc("bad"));
    const int restored = setrlimit(RLIMIT_NOFILE, &original);
    check(restored == 0 && !spawned && f.svc("bad").next_start_ms > now_ms(),
          "resource failure spins");
}

void stubborn_and_tree(const std::string &helper) {
    Fixture f(helper);
    const auto tree = f.add("tree", "tree");
    f.add("stubborn", "ignore");
    f.load();
    f.until([&] {
        return !f.events("stubborn").empty() && std::filesystem::exists(tree + "/descendants");
    });
    sleep_ms(50);
    pid_t child = 0;
    std::ifstream(tree + "/descendants") >> child;
    f.command("stop tree");
    f.until([&] { return f.svc("tree").pid == 0; });
    check(kill(child, 0) < 0 && errno == ESRCH, "descendant survived stop");
    const pid_t old = f.svc("stubborn").pid;
    const auto start = now_ms();
    f.command("restart stubborn");
    f.command("start stubborn");
    f.until([&] { return f.events("stubborn").size() == 2; }, 7000);
    check(now_ms() - start >= 5000 && kill(old, 0) < 0 && errno == ESRCH, "stop timeout/overlap");
}

void graceful_term(const std::string &helper) {
    Fixture f(helper);
    const auto path = f.add("grace", "grace");
    f.load();
    f.until([&] { return !f.events("grace").empty(); });
    sleep_ms(50);
    std::set<pid_t> signalled;
    signalled.insert(f.svc("grace").pid);
    f.command("stop grace");
    f.until([&] {
        signal_new_children(SIGTERM, signalled);
        return f.svc("grace").pid == 0;
    });
    int count = 0;
    std::ifstream(path + "/terms") >> count;
    check(count == 1, "graceful shutdown sent repeated TERM");
}

void orphan_churn(const std::string &helper) {
    Fixture f(helper);
    const auto path = f.add("churn", "churn");
    f.add("orphan", "orphan");
    f.load();
    f.until([&] { return f.events("orphan").size() >= 2 && !f.events("churn").empty(); });
    f.until([&] {
        std::ifstream descendants(path + "/descendants");
        pid_t recorded;
        int recorded_count = 0;
        while (descendants >> recorded)
            ++recorded_count;
        return recorded_count == 48;
    });
    f.command("restart churn");
    f.until([&] { return f.events("churn").size() == 2; });
    std::ifstream file(path + "/descendants");
    pid_t pid;
    int count = 0;
    while (count < 48 && file >> pid) {
        check(kill(pid, 0) < 0 && errno == ESRCH, "zombie not reaped");
        ++count;
    }
    check(count == 48, "churn fixture incomplete");
}
} // namespace

int main(int argc, char **argv) {
    try {
        check(argc == 2, "expected helper path");
        check(prctl(PR_SET_CHILD_SUBREAPER, 1) == 0, "subreaper");
        signal(SIGCHLD, SIG_IGN);
        signal(SIGPIPE, SIG_IGN);
        Fd sfd(setup_signalfd());
        check(sfd.get() >= 0, "signalfd");
        for (const auto &test : {discovery, lifecycle, backoff, failed_exec_and_fork,
                                 stubborn_and_tree, graceful_term, orphan_churn})
            test(argv[1]);
        signal_pid(getpid(), SIGTERM);
        signal_pid(getpid(), SIGINT);
        bool term = false, interrupt = false;
        signalfd_siginfo info{};
        while (read(sfd.get(), &info, sizeof(info)) == static_cast<ssize_t>(sizeof(info))) {
            term = term || info.ssi_signo == SIGTERM;
            interrupt = interrupt || info.ssi_signo == SIGINT;
        }
        check(term && interrupt, "TERM/INT not delivered through signalfd");
        std::fprintf(stderr, "PASS: discovery, graphs, signals, lifecycle, backoff, exec/resource "
                             "failures, timeouts, groups, churn\n");
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
