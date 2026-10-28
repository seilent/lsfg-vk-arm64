/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "lsfg-vk-common/helpers/dll_file.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/helpers/shader_ids.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {
    constexpr uint32_t SPIRV_MAGIC{0x07230203};
    constexpr uint32_t RT_RCDATA{10};
    constexpr uint32_t DIRECTORY_BIT{0x80000000};

    std::string reason() {
        return std::generic_category().message(errno);
    }
}

ls::DllFile::DllFile(const std::filesystem::path& path)
        : fd(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK)) {
    if (this->fd < 0)
        throw ls::error("failed to open dll file: " + reason());

    struct stat st{};
    if (fstat(this->fd, &st) != 0) {
        const auto why = reason();
        close(this->fd);
        throw ls::error("failed to stat dll file: " + why);
    }
    if (!S_ISREG(st.st_mode)) {
        close(this->fd);
        throw ls::error("dll is not a regular file");
    }
    this->size = static_cast<uint64_t>(st.st_size);

    try {
        this->table = this->parse();
    } catch (...) {
        close(this->fd);
        throw;
    }
}

ls::DllFile::~DllFile() {
    close(this->fd);
}

void ls::DllFile::require(uint64_t offset, uint64_t length) const {
    if (offset > this->size || length > this->size - offset)
        throw ls::error("dll is truncated");
}

void ls::DllFile::read(uint64_t offset, void* data, size_t length) const {
    this->require(offset, length);

    auto* out = static_cast<uint8_t*>(data);
    while (length > 0) {
        const ssize_t got = pread(this->fd, out, length, static_cast<off_t>(offset));
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            throw ls::error("failed to read dll file: " + (got < 0 ? reason() : std::string("unexpected end of file")));

        out += got;
        offset += static_cast<uint64_t>(got);
        length -= static_cast<size_t>(got);
    }
}

template<typename T>
T ls::DllFile::value(uint64_t offset) const {
    T result{};
    this->read(offset, &result, sizeof(T));
    return result;
}

std::unordered_map<uint32_t, ls::DllResource> ls::DllFile::parse() const {
    if (this->value<uint16_t>(0) != 0x5A4D)
        throw ls::error("dos header magic number is incorrect");

    const auto pe = static_cast<uint64_t>(this->value<int32_t>(60));
    if (this->value<uint32_t>(pe) != 0x00004550)
        throw ls::error("pe header signature is incorrect");
    const auto sectionCount = this->value<uint16_t>(pe + 6);
    const auto optionalSize = this->value<uint16_t>(pe + 20);

    const uint64_t optional = pe + 24;
    if (this->value<uint16_t>(optional) != 0x20B)
        throw ls::error("pe format is not PE32+");
    const auto rsrcRva = this->value<uint32_t>(optional + 128);
    const auto rsrcSize = this->value<uint32_t>(optional + 132);

    const uint64_t sections = optional + optionalSize;
    this->require(sections, uint64_t{sectionCount} * 40);
    std::optional<uint64_t> rsrc;
    for (uint64_t i = 0; i < sectionCount; i++) {
        const uint64_t section = sections + (i * 40);
        const auto vsize = this->value<uint32_t>(section + 8);
        const auto vaddress = this->value<uint32_t>(section + 12);
        const auto foffset = this->value<uint32_t>(section + 20);
        if (rsrcRva < vaddress || rsrcRva > vaddress + vsize)
            continue;

        rsrc.emplace((rsrcRva - vaddress) + foffset);
        break;
    }
    if (!rsrc)
        throw ls::error("unable to locate resource section");

    const auto rootNames = this->value<uint16_t>(*rsrc + 12);
    const auto rootIds = this->value<uint16_t>(*rsrc + 14);
    if (rootIds < 3)
        throw ls::error("resource directory does not have enough entries");

    const uint64_t rootCount = uint64_t{rootNames} + rootIds;
    const uint64_t rootEntries = *rsrc + 16;
    this->require(rootEntries, rootCount * 8);
    std::optional<uint64_t> directoryOffset;
    for (uint64_t i = 0; i < rootCount; i++) {
        if (this->value<uint32_t>(rootEntries + (i * 8)) != RT_RCDATA)
            continue;
        const auto offset = this->value<uint32_t>(rootEntries + (i * 8) + 4);
        if ((offset & DIRECTORY_BIT) == 0)
            throw ls::error("expected resource directory, found data entry");

        directoryOffset.emplace(offset & ~DIRECTORY_BIT);
    }
    if (!directoryOffset)
        throw ls::error("unable to locate RT_RCDATA directory");

    const uint64_t directory = *rsrc + *directoryOffset;
    const auto names = this->value<uint16_t>(directory + 12);
    const auto ids = this->value<uint16_t>(directory + 14);
    if (ids < 1)
        throw ls::error("RT_RCDATA directory does not have enough entries");

    const uint64_t count = uint64_t{names} + ids;
    const uint64_t entries = directory + 16;
    this->require(entries, count * 8);
    std::unordered_map<uint32_t, DllResource> resources;
    for (uint64_t i = 0; i < count; i++) {
        const auto id = this->value<uint32_t>(entries + (i * 8));
        const auto offset = this->value<uint32_t>(entries + (i * 8) + 4);
        if ((offset & DIRECTORY_BIT) == 0)
            throw ls::error("expected resource directory, found data entry");

        const uint64_t language = *rsrc + (offset & ~DIRECTORY_BIT);
        if (this->value<uint16_t>(language + 14) < 1)
            throw ls::error("Incorrect language directory");
        const auto languageOffset = this->value<uint32_t>(language + 20);
        if ((languageOffset & DIRECTORY_BIT) != 0)
            throw ls::error("expected resource data entry, but found directory");

        const uint64_t data = *rsrc + (languageOffset & ~DIRECTORY_BIT);
        this->require(data, 16);
        const auto blobRva = this->value<uint32_t>(data);
        const auto blobSize = this->value<uint32_t>(data + 4);
        if (blobRva < rsrcRva || blobRva > rsrcRva + rsrcSize)
            throw ls::error("resource data entry points outside resource section");

        const uint64_t blob = (blobRva - rsrcRva) + *rsrc;
        if (blob >= this->size || blob + blobSize > this->size)
            throw ls::error("resource data entry points outside file");
        resources.emplace(id, DllResource{ .offset = blob, .size = blobSize });
    }

    return resources;
}

void ls::validateShaderDll(const std::filesystem::path& dll, bool allowFp16) {
    const DllFile file(dll);
    const auto& resources = file.resources();

    for (const bool fp16 : { false, true }) {
        if (fp16 && !allowFp16)
            continue;

        for (const auto& request : shader_ids::REQUESTS) {
            const uint32_t id = shader_ids::resourceId(request.id, fp16, request.perf);
            const auto it = resources.find(id);
            if (it == resources.end())
                throw ls::error("unable to find shader resource: " + std::to_string(id));

            const auto& blob = it->second;
            uint32_t magic{};
            if (blob.size >= 20 && blob.size % 4 == 0)
                file.read(blob.offset, &magic, sizeof(magic));
            if (magic != SPIRV_MAGIC)
                throw ls::error("shader resource is not SPIR-V: " + std::to_string(id));
        }
    }
}
