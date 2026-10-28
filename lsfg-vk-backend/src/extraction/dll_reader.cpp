/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "dll_reader.hpp"
#include "lsfg-vk-common/helpers/dll_file.hpp"

#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace lsfgvk;
using namespace lsfgvk::backend;

std::unordered_map<uint32_t, std::vector<uint8_t>> backend::extractResourcesFromDLL(
        const std::filesystem::path& dll) {
    const ls::DllFile file(dll);

    std::unordered_map<uint32_t, std::vector<uint8_t>> resources;
    for (const auto& [id, resource] : file.resources()) {
        std::vector<uint8_t> data(resource.size);
        file.read(resource.offset, data.data(), data.size());
        resources.emplace(id, std::move(data));
    }

    return resources;
}
