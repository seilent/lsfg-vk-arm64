/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "shader_registry.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/helpers/shader_ids.hpp"
#include "lsfg-vk-common/vulkan/shader.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

using namespace lsfgvk;
using namespace lsfgvk::backend;
using namespace ls::shader_ids;

namespace {
    template<uint32_t Id, bool Perf>
    const std::vector<uint8_t>& getShaderSource(bool fp16,
            const std::unordered_map<uint32_t, std::vector<uint8_t>>& resources) {
        static_assert(std::ranges::any_of(REQUESTS,
            [](const Request& request) { return request.id == Id && request.perf == Perf; }),
            "every shader the registry loads must be listed in shader_ids.hpp");

        auto it = resources.find(resourceId(Id, fp16, Perf));
        if (it == resources.end())
            throw ls::error("unable to find shader with id: " + std::to_string(Id));

        return it->second;
    }
    /// patch the generate shader
    void patchGenerateShader(std::vector<uint8_t>& data, bool hdr) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage-in-container"
        auto* _ptr = data.data();
        const std::span<uint32_t> words(
            reinterpret_cast<uint32_t*>(_ptr),
            data.size() / sizeof(uint32_t)
        );
#pragma clang diagnostic pop

        const uint16_t SpvOpCapability = 17;
        const uint16_t SpvOpTypeImage = 25;
        const uint32_t SpvCapabilityStorageImageWriteWithoutFormat = 56;
        const uint32_t SpvCapabilityShader = 1;
        const uint32_t SpvImageFormatRgba16f = 2;
        const uint32_t SpvImageFormatRgba8 = 4;

        for (size_t i = 5; i < words.size();) {
            const uint32_t& word = words[i]; // NOLINT ([]-usage)
            const uint16_t wc = (word >> 16);
            const uint16_t op = word & 0xFFFF;

            // remove write without format capability
            if (op == SpvOpCapability && wc >= 2) {
                uint32_t& cap = words[i + 1]; // NOLINT ([]-usage)
                if (cap == SpvCapabilityStorageImageWriteWithoutFormat)
                    cap = SpvCapabilityShader;
            }

            // patch format in image instructions
            if (op == SpvOpTypeImage && wc >= 9) {
                const uint32_t sampled = words[i + 7]; // NOLINT ([]-usage)
                if (sampled == 2)
                    words[i + 8] = // NOLINT ([]-usage)
                        hdr ? SpvImageFormatRgba16f : SpvImageFormatRgba8;
            }

            i += wc ? wc : 1;
        }
    }
}

ShaderRegistry backend::buildShaderRegistry(const vk::Vulkan& vk, bool fp16,
        const std::unordered_map<uint32_t, std::vector<uint8_t>>& resources) {
    std::vector<uint8_t> generate_data = getShaderSource<GENERATE, false>(fp16, resources);
    std::vector<uint8_t> generate_data_hdr = generate_data;
    patchGenerateShader(generate_data, false);
    patchGenerateShader(generate_data_hdr, true);

#define SHADER(id, p1, p2, p3, p4) \
    vk::Shader(vk, getShaderSource<id, PERF>(fp16, resources), \
        p1, p2, p3, p4)

    return {
#define PERF false
        .mipmaps = SHADER(MIPMAPS, 1, 7, 1, 1),
        .generate = vk::Shader(vk, generate_data, 5, 1, 1, 2),
        .generate_hdr = vk::Shader(vk, generate_data_hdr, 5, 1, 1, 2),
        .quality = {
            .alpha = {
                SHADER(ALPHA.at(0), 1, 2, 0, 1),
                SHADER(ALPHA.at(1), 2, 2, 0, 1),
                SHADER(ALPHA.at(2), 2, 4, 0, 1),
                SHADER(ALPHA.at(3), 4, 4, 0, 1)
            },
            .beta = {
                SHADER(BETA.at(0), 12, 2, 0, 1),
                SHADER(BETA.at(1), 2, 2, 0, 1),
                SHADER(BETA.at(2), 2, 2, 0, 1),
                SHADER(BETA.at(3), 2, 2, 0, 1),
                SHADER(BETA.at(4), 2, 6, 1, 1)
            },
            .gamma = {
                SHADER(GAMMA.at(0), 9, 3, 1, 2),
                SHADER(GAMMA.at(1), 3, 4, 0, 1),
                SHADER(GAMMA.at(2), 4, 4, 0, 1),
                SHADER(GAMMA.at(3), 4, 4, 0, 1),
                SHADER(GAMMA.at(4), 6, 1, 1, 2)
            },
            .delta = {
                SHADER(DELTA.at(0), 9, 3, 1, 2),
                SHADER(DELTA.at(1), 3, 4, 0, 1),
                SHADER(DELTA.at(2), 4, 4, 0, 1),
                SHADER(DELTA.at(3), 4, 4, 0, 1),
                SHADER(DELTA.at(4), 6, 1, 1, 2),
                SHADER(DELTA.at(5), 10, 2, 1, 2),
                SHADER(DELTA.at(6), 2, 2, 0, 1),
                SHADER(DELTA.at(7), 2, 2, 0, 1),
                SHADER(DELTA.at(8), 2, 2, 0, 1),
                SHADER(DELTA.at(9), 3, 1, 1, 2)
            }
        },
#undef PERF
#define PERF true
        .performance = {
            .alpha = {
                SHADER(ALPHA.at(0), 1, 1, 0, 1),
                SHADER(ALPHA.at(1), 1, 1, 0, 1),
                SHADER(ALPHA.at(2), 1, 2, 0, 1),
                SHADER(ALPHA.at(3), 2, 2, 0, 1)
            },
            .beta = {
                SHADER(BETA.at(0), 6, 2, 0, 1),
                SHADER(BETA.at(1), 2, 2, 0, 1),
                SHADER(BETA.at(2), 2, 2, 0, 1),
                SHADER(BETA.at(3), 2, 2, 0, 1),
                SHADER(BETA.at(4), 2, 6, 1, 1)
            },
            .gamma = {
                SHADER(GAMMA.at(0), 5, 3, 1, 2),
                SHADER(GAMMA.at(1), 3, 2, 0, 1),
                SHADER(GAMMA.at(2), 2, 2, 0, 1),
                SHADER(GAMMA.at(3), 2, 2, 0, 1),
                SHADER(GAMMA.at(4), 4, 1, 1, 2)
            },
            .delta = {
                SHADER(DELTA.at(0), 5, 3, 1, 2),
                SHADER(DELTA.at(1), 3, 2, 0, 1),
                SHADER(DELTA.at(2), 2, 2, 0, 1),
                SHADER(DELTA.at(3), 2, 2, 0, 1),
                SHADER(DELTA.at(4), 4, 1, 1, 2),
                SHADER(DELTA.at(5), 6, 1, 1, 2),
                SHADER(DELTA.at(6), 1, 1, 0, 1),
                SHADER(DELTA.at(7), 1, 1, 0, 1),
                SHADER(DELTA.at(8), 1, 1, 0, 1),
                SHADER(DELTA.at(9), 2, 1, 1, 2)
            }
        },
#undef PERF
        .is_fp16 = fp16
    };

#undef SHADER
}
