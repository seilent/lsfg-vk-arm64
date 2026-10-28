/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <unordered_map>

namespace ls {

    struct DllResource {
        uint64_t offset;
        uint32_t size;
    };

    class DllFile {
    public:
        explicit DllFile(const std::filesystem::path& path);

        [[nodiscard]] const std::unordered_map<uint32_t, DllResource>& resources() const { return this->table; }
        void read(uint64_t offset, void* data, size_t length) const;

        DllFile(const DllFile&) = delete;
        DllFile& operator=(const DllFile&) = delete;
        DllFile(DllFile&&) = delete;
        DllFile& operator=(DllFile&&) = delete;
        ~DllFile();
    private:
        void require(uint64_t offset, uint64_t length) const;
        template<typename T>
        [[nodiscard]] T value(uint64_t offset) const;
        [[nodiscard]] std::unordered_map<uint32_t, DllResource> parse() const;

        int fd;
        uint64_t size{};
        std::unordered_map<uint32_t, DllResource> table;
    };

    void validateShaderDll(const std::filesystem::path& dll, bool allowFp16);

}
