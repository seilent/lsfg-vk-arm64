/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "running_signal.hpp"

#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace lsfgvk::layer;

namespace {
    struct Faults {
        bool armed{false};
        bool noTmpfile{false};
        bool sweepOnLock{false};
        int busyLocks{0};
        int tmpfileCalls{0};
        int exclusiveCalls{0};
        int lockCalls{0};
    };
    Faults faults;
}

extern "C" {
    int __real_openat(int dir, const char* path, int flags, ...);
    int __real_flock(int fd, int operation);

    int __wrap_openat(int dir, const char* path, int flags, ...) {
        mode_t mode{};
        if ((flags & O_CREAT) != 0 || (flags & O_TMPFILE) == O_TMPFILE) {
            va_list args;
            va_start(args, flags);
            mode = va_arg(args, mode_t);
            va_end(args);
        }
        if ((flags & O_TMPFILE) == O_TMPFILE) {
            faults.tmpfileCalls++;
            if (faults.noTmpfile) {
                errno = EOPNOTSUPP;
                return -1;
            }
        }
        if ((flags & O_EXCL) != 0)
            faults.exclusiveCalls++;
        return __real_openat(dir, path, flags, mode);
    }

    int __wrap_flock(int fd, int operation) {
        if (faults.armed && operation == (LOCK_EX | LOCK_NB)) {
            faults.lockCalls++;
            if (faults.sweepOnLock) {
                faults.sweepOnLock = false;
                std::filesystem::remove(std::filesystem::read_symlink("/proc/self/fd/" + std::to_string(fd)));
            }
            if (faults.busyLocks > 0) {
                faults.busyLocks--;
                errno = EWOULDBLOCK;
                return -1;
            }
        }
        return __real_flock(fd, operation);
    }
}

namespace {
    std::filesystem::path scratch;

    [[noreturn]] void fail(const std::string& name) {
        std::cerr << name << '\n';
        std::exit(1);
    }

    void expect(bool ok, const std::string& name) {
        if (!ok)
            fail(name);
    }

    bool held(const std::filesystem::path& path) {
        const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            fail("unable to open " + path.string());
        const bool locked = flock(fd, LOCK_SH | LOCK_NB) != 0 && errno == EWOULDBLOCK;
        close(fd);
        return locked;
    }

    int lockFile(const std::filesystem::path& path) {
        const int fd = open(path.c_str(), O_RDONLY | O_CREAT | O_CLOEXEC, 0600);
        if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) != 0)
            fail("unable to lock " + path.string());
        return fd;
    }

    void touch(const std::filesystem::path& path) {
        std::ofstream(path).put('x');
    }

    std::string failureOf(const std::function<void()>& fn) {
        try {
            fn();
        } catch (const std::exception& e) {
            return e.what();
        }
        return {};
    }

    bool mode0600(const std::filesystem::path& path) {
        struct stat st{};
        return stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600;
    }

    void nameCases() {
        expect(runningSignalName("570", 1234) == "570-1234", "name from SteamGameId and pid");
        expect(runningSignalName("18446744073709551615", 9) == "18446744073709551615-9", "name with a shortcut id");
        expect(runningSignalName(nullptr, 7) == "0-7", "name without SteamGameId");
        expect(runningSignalName("", 7) == "0-7", "name with an empty SteamGameId");
        expect(runningSignalName("../x", 7) == "0-7", "name with a path in SteamGameId");
        expect(runningSignalName("12a", 7) == "0-7", "name with a non numeric SteamGameId");
    }

    void directoryCases() {
        setenv("HOME", "/home/test", 1);
        expect(runningSignalDirectory() == "/home/test/.cache/lsfg-vk/active", "directory under HOME");

        for (const bool empty : { true, false }) {
            if (empty)
                setenv("HOME", "", 1);
            else
                unsetenv("HOME");
            bool threw = false;
            try {
                runningSignalDirectory();
            } catch (const std::exception&) {
                threw = true;
            }
            expect(threw, empty ? "empty HOME is refused" : "missing HOME is refused");
        }
    }

    void createCases() {
        const auto dir = scratch / "active";
        umask(022);

        faults.tmpfileCalls = 0;
        faults.exclusiveCalls = 0;
        const int fd = createRunningSignal(dir, "570-100");
        const auto path = dir / "570-100";
        expect(fd >= 0 && std::filesystem::is_regular_file(path), "signal file created with its directory");
        expect(faults.tmpfileCalls > 0, "the signal is first tried as an O_TMPFILE");
        std::cout << "running signal: published through "
            << (faults.exclusiveCalls == 0 ? "O_TMPFILE and linkat" : "the O_EXCL fallback") << '\n';
        expect(mode0600(path), "signal file mode 0600");
        expect((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0, "signal fd has close on exec");
        expect(held(path), "reader sees the signal held");
        close(fd);
        expect(!held(path), "reader sees the signal free after close");

        touch(dir / "570-101");
        const int live = lockFile(dir / "570-102");
        touch(scratch / "target");
        std::filesystem::create_symlink(scratch / "target", dir / "570-103");
        std::filesystem::create_directory(dir / "570-104");
        expect(mkfifo((dir / "570-105").c_str(), 0600) == 0, "fifo created");
        touch(dir / "notes.txt");

        const int next = createRunningSignal(dir, "570-106");
        expect(!std::filesystem::exists(path), "closed signal removed as stale");
        expect(!std::filesystem::exists(dir / "570-101"), "unlocked file removed as stale");
        expect(std::filesystem::exists(dir / "570-102") && held(dir / "570-102"), "locked file kept");
        expect(std::filesystem::is_symlink(dir / "570-103"), "symlink kept");
        expect(std::filesystem::exists(scratch / "target"), "symlink target kept");
        expect(std::filesystem::is_directory(dir / "570-104"), "directory kept");
        expect(std::filesystem::is_fifo(dir / "570-105"), "fifo kept");
        expect(std::filesystem::exists(dir / "notes.txt"), "a file with another name kept");
        expect(held(dir / "570-106"), "new signal held");

        const auto start = std::chrono::steady_clock::now();
        const auto taken = failureOf([&] { close(createRunningSignal(dir, "570-102")); });
        expect(taken.find("File exists") != std::string::npos, "a live signal with the same name is refused");
        expect(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500), "the refusal does not block");
        expect(held(dir / "570-102"), "the live signal stays held");

        touch(dir / "570-107");
        const int same = createRunningSignal(dir, "570-107");
        expect(held(dir / "570-107"), "a stale file with the same name is replaced");

        expect(!failureOf([&] { createRunningSignal(dir, "notes"); }).empty(), "a name without a pid is refused");
        expect(!failureOf([&] { createRunningSignal(dir, "../570-1"); }).empty(), "a name with a path is refused");

        std::filesystem::create_directory(scratch / "real");
        touch(scratch / "real" / "570-400");
        std::filesystem::create_directory_symlink(scratch / "real", scratch / "linked");
        expect(!failureOf([&] { createRunningSignal(scratch / "linked", "570-401"); }).empty(),
            "a symlinked signal directory is refused");
        expect(std::filesystem::exists(scratch / "real" / "570-400")
            && !std::filesystem::exists(scratch / "real" / "570-401"),
            "nothing is swept or written through the symlink");

        close(same);
        close(next);
        close(live);
    }

    void fallbackCases() {
        faults.noTmpfile = true;

        faults.tmpfileCalls = 0;
        faults.exclusiveCalls = 0;
        const auto dir = scratch / "fallback";
        const int fd = createRunningSignal(dir, "570-300");
        expect(faults.tmpfileCalls > 0 && faults.exclusiveCalls == 1, "without O_TMPFILE the signal is created O_EXCL");
        expect(mode0600(dir / "570-300"), "fallback signal mode 0600");
        expect((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0, "fallback signal fd has close on exec");
        expect(held(dir / "570-300"), "fallback signal held");
        close(fd);

        faults.armed = true;
        faults.lockCalls = 0;
        faults.busyLocks = 5;
        const int polled = createRunningSignal(scratch / "busy", "570-301");
        expect(faults.lockCalls == 6 && held(scratch / "busy" / "570-301"), "a busy lock is polled until it is free");
        close(polled);

        faults.lockCalls = 0;
        faults.busyLocks = 1'000'000;
        const auto start = std::chrono::steady_clock::now();
        const auto stuck = failureOf([] { createRunningSignal(scratch / "stuck", "570-302"); });
        const auto elapsed = std::chrono::steady_clock::now() - start;
        expect(!stuck.empty() && elapsed < std::chrono::milliseconds(500), "a lock that stays busy gives up in time");
        expect(elapsed >= std::chrono::milliseconds(50), "a busy lock is polled for 50 ms");
        faults.busyLocks = 0;

        faults.lockCalls = 0;
        faults.sweepOnLock = true;
        const int swept = createRunningSignal(scratch / "swept", "570-303");
        expect(faults.lockCalls == 2 && held(scratch / "swept" / "570-303"),
            "a signal removed before its lock is taken is created again");
        close(swept);
        faults.armed = false;

        const int live = lockFile(dir / "570-304");
        faults.exclusiveCalls = 0;
        const auto taken = failureOf([&] { createRunningSignal(dir, "570-304"); });
        expect(taken.find("File exists") != std::string::npos && faults.exclusiveCalls == 3,
            "without O_TMPFILE a live signal with the same name is refused after 3 tries");
        close(live);

        faults.noTmpfile = false;
    }

    void exitCase() {
        const auto dir = scratch / "exit";
        int ready[2];
        int go[2];
        expect(pipe(ready) == 0 && pipe(go) == 0, "pipes created");

        const pid_t child = fork();
        expect(child >= 0, "fork");
        if (child == 0) {
            createRunningSignal(dir, runningSignalName("570", getpid()));
            char c = 1;
            if (write(ready[1], &c, 1) != 1 || read(go[0], &c, 1) != 1)
                _exit(3);
            _exit(0);
        }

        char c{};
        expect(read(ready[0], &c, 1) == 1, "child created its signal");
        const auto path = dir / runningSignalName("570", child);
        expect(held(path), "signal held while the process lives");
        expect(write(go[1], &c, 1) == 1, "child told to exit");
        int status{};
        expect(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "child exited without closing the signal");
        expect(!held(path), "signal free once the process is gone");

        close(createRunningSignal(dir, "570-1"));
        expect(!std::filesystem::exists(path), "next start removes the signal of the ended process");
    }
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: lsfg-vk-running-signal-test <scratch>\n";
        return 2;
    }
    scratch = std::filesystem::absolute(argv[1]);
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch);

    nameCases();
    directoryCases();
    createCases();
    fallbackCases();
    exitCase();
    std::cout << "running signal: all checks passed\n";
    return 0;
}
