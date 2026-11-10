/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <charconv>
#include <cstdint>
#include <cstring>
#include <system_error>

namespace lsfgvk::layer {

    inline int64_t nextPresentDeadline(int64_t previous, int64_t now, int64_t interval) {
        const int64_t next = previous + interval;
        if (next < now - interval)
            return now;
        return next;
    }

    inline int64_t fpsInterval(const char* value) {
        uint32_t fps{};
        const auto* end = value + std::strlen(value);
        const auto [ptr, ec] = std::from_chars(value, end, fps);
        if (ec != std::errc() || ptr != end || fps == 0)
            return 0;
        return 1000000000 / static_cast<int64_t>(fps);
    }

}
