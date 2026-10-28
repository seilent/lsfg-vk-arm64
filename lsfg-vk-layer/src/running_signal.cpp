/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "running_signal.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

using namespace lsfgvk;
using namespace lsfgvk::layer;

namespace {
    bool isDigits(const std::string& str) {
        return !str.empty() && std::ranges::all_of(str, [](char c) { return c >= '0' && c <= '9'; });
    }

    bool isSignalName(const std::string& name) {
        const auto dash = name.find('-');
        return dash != std::string::npos
            && isDigits(name.substr(0, dash)) && isDigits(name.substr(dash + 1));
    }

    bool sameFile(int dir, const std::string& name, int fd) {
        struct stat opened{};
        struct stat linked{};
        return fstat(fd, &opened) == 0 && fstatat(dir, name.c_str(), &linked, AT_SYMLINK_NOFOLLOW) == 0
            && opened.st_dev == linked.st_dev && opened.st_ino == linked.st_ino;
    }

    void removeStaleSignals(int dir) {
        const int copy = fcntl(dir, F_DUPFD_CLOEXEC, 0);
        if (copy < 0)
            return;
        DIR* entries = fdopendir(copy);
        if (!entries) {
            close(copy);
            return;
        }

        rewinddir(entries);
        while (const dirent* entry = readdir(entries)) {
            const std::string name{entry->d_name};
            if (!isSignalName(name))
                continue;

            const int fd = openat(dir, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
            if (fd < 0)
                continue;

            struct stat st{};
            if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)
                    && flock(fd, LOCK_EX | LOCK_NB) == 0 && sameFile(dir, name, fd))
                unlinkat(dir, name.c_str(), 0);
            close(fd);
        }
        closedir(entries);
    }

    int publishLocked(int dir, const std::string& name) {
        const int fd = openat(dir, ".", O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
        if (fd < 0)
            return -errno;

        const std::string self = "/proc/self/fd/" + std::to_string(fd);
        if (flock(fd, LOCK_EX | LOCK_NB) != 0
                || linkat(AT_FDCWD, self.c_str(), dir, name.c_str(), AT_SYMLINK_FOLLOW) != 0) {
            const int err = errno;
            close(fd);
            return -err;
        }
        return fd;
    }

    int createThenLock(int dir, const std::string& name) {
        const int fd = openat(dir, name.c_str(), O_RDONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0)
            return -errno;

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
        while (flock(fd, LOCK_EX | LOCK_NB) != 0) {
            const int err = errno;
            if (err != EWOULDBLOCK || std::chrono::steady_clock::now() >= deadline) {
                close(fd);
                return -err;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        if (!sameFile(dir, name, fd)) {
            close(fd);
            return -ESTALE;
        }
        return fd;
    }
}

std::filesystem::path layer::runningSignalDirectory() {
    const char* home = std::getenv("HOME");
    if (!home || *home == '\0')
        throw ls::error("HOME is not set");

    return std::filesystem::path(home) / ".cache" / "lsfg-vk" / "active";
}

std::string layer::runningSignalName(const char* gameId, pid_t pid) {
    const std::string id{gameId && isDigits(gameId) ? gameId : "0"};
    return id + "-" + std::to_string(pid);
}

int layer::createRunningSignal(const std::filesystem::path& dir, const std::string& name) {
    if (!isSignalName(name))
        throw ls::error("not a running signal name: " + name);

    std::filesystem::create_directories(dir);
    const int handle = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (handle < 0)
        throw ls::error("unable to open " + dir.string() + ": " + std::generic_category().message(errno));

    int result = -EEXIST;
    for (int attempt = 0; attempt < 3 && result < 0; attempt++) {
        removeStaleSignals(handle);
        result = publishLocked(handle, name);
        if (result < 0 && result != -EEXIST)
            result = createThenLock(handle, name);
    }
    close(handle);

    if (result < 0)
        throw ls::error("unable to create " + (dir / name).string() + ": "
            + std::generic_category().message(-result));
    return result;
}
