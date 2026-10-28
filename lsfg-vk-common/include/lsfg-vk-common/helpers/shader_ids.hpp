/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ls::shader_ids {

    inline constexpr uint32_t MIPMAPS{255};
    inline constexpr uint32_t GENERATE{256};
    inline constexpr std::array<uint32_t, 4> ALPHA{267, 268, 269, 270};
    inline constexpr std::array<uint32_t, 5> BETA{275, 276, 277, 278, 279};
    inline constexpr std::array<uint32_t, 5> GAMMA{257, 259, 260, 261, 262};
    inline constexpr std::array<uint32_t, 10> DELTA{257, 263, 264, 265, 266, 258, 271, 272, 273, 274};

    struct Request {
        uint32_t id;
        bool perf;
    };

    inline constexpr auto REQUESTS = [] {
        std::array<Request, 2 + (2 * (ALPHA.size() + BETA.size() + GAMMA.size() + DELTA.size()))> requests{};
        size_t i = 0;
        const auto add = [&](const auto& group, bool perf) {
            for (const uint32_t id : group)
                requests.at(i++) = { id, perf };
        };

        requests.at(i++) = { MIPMAPS, false };
        requests.at(i++) = { GENERATE, false };
        for (const bool perf : { false, true }) {
            add(ALPHA, perf);
            add(BETA, perf);
            add(GAMMA, perf);
            add(DELTA, perf);
        }
        return requests;
    }();

    constexpr uint32_t resourceId(uint32_t id, bool fp16, bool perf) {
        return 49 + id + (perf ? 23 : 0) + (fp16 ? 0 : 49);
    }

}
