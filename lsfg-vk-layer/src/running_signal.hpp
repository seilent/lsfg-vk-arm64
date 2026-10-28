/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <filesystem>
#include <string>

#include <sys/types.h>

namespace lsfgvk::layer {

    std::filesystem::path runningSignalDirectory();
    std::string runningSignalName(const char* gameId, pid_t pid);
    int createRunningSignal(const std::filesystem::path& dir, const std::string& name);

}
