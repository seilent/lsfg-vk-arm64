/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "extraction/dll_reader.hpp"
#include "lsfg-vk-common/configuration/config.hpp"
#include "lsfg-vk-common/helpers/dll_file.hpp"
#include "lsfg-vk-common/helpers/shader_ids.hpp"

#include <clocale>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sys/stat.h>

namespace {
    std::filesystem::path fixtures;
    std::filesystem::path scratch;

    [[noreturn]] void fail(const std::string& name, const std::string& detail) {
        std::cerr << name << ": " << detail << '\n';
        std::exit(1);
    }

    void expect(bool ok, const std::string& name) {
        if (!ok)
            fail(name, "check failed");
    }

    void expectOk(const std::function<void()>& fn, const std::string& name) {
        try {
            fn();
        } catch (const std::exception& e) {
            fail(name, std::string("threw: ") + e.what());
        }
    }

    void expectThrow(const std::function<void()>& fn, const std::string& what, const std::string& name) {
        try {
            fn();
        } catch (const std::exception& e) {
            if (std::string(e.what()).find(what) == std::string::npos)
                fail(name, "threw without '" + what + "': " + e.what());
            return;
        }
        fail(name, "did not throw");
    }

    std::filesystem::path dll(const std::string& name) {
        return fixtures / (name + ".dll");
    }

    void validate(const std::string& name, bool fp16) {
        ls::validateShaderDll(dll(name), fp16);
    }

    void validatorCases() {
        for (const bool fp16 : { true, false }) {
            const std::string mode = fp16 ? " (fp16 allowed)" : " (fp16 off)";
            expectOk([&] { validate("good", fp16); }, "good dll passes" + mode);
            expectThrow([&] { validate("truncated", fp16); }, "dll is truncated", "truncated dll" + mode);
            expectThrow([&] { validate("no-resources", fp16); }, "unable to locate resource section",
                "dll without a resource directory" + mode);
            expectThrow([&] { validate("no-rcdata", fp16); }, "unable to locate RT_RCDATA directory",
                "dll without RT_RCDATA" + mode);
            expectThrow([&] { validate("missing-id", fp16); }, "unable to find shader resource: "
                + std::to_string(ls::shader_ids::resourceId(ls::shader_ids::MIPMAPS, false, false)),
                "dll without the fp32 mipmaps shader" + mode);
            expectThrow([&] { validate("not-spirv", fp16); }, "is not SPIR-V",
                "dll with a blob without the SPIR-V magic" + mode);
            expectThrow([&] { validate("short-blob", fp16); }, "is not SPIR-V",
                "dll with a 16 byte blob starting with the SPIR-V magic" + mode);
            expectThrow([&] { validate("odd-blob", fp16); }, "is not SPIR-V",
                "dll with a 21 byte blob starting with the SPIR-V magic" + mode);
            expectThrow([&] { validate("no-fp32", fp16); }, "unable to find shader resource",
                "dll without the fp32 set" + mode);
            expectThrow([&] { validate("no-perf", fp16); }, "unable to find shader resource",
                "dll without the performance set" + mode);
            expectThrow([&] { validate("bad-entry", fp16); }, "points outside resource section",
                "dll with a broken unrequested entry" + mode);
        }
        expectThrow([] { validate("no-fp16", true); }, "unable to find shader resource",
            "dll without the fp16 set while fp16 is allowed");
        expectOk([] { validate("no-fp16", false); }, "dll without the fp16 set while fp16 is off");
        expectThrow([] { validate("bad-fp16", true); }, "is not SPIR-V",
            "dll with a bad fp16 blob while fp16 is allowed");
        expectOk([] { validate("bad-fp16", false); }, "dll with a bad fp16 blob while fp16 is off");

        expectThrow([] { validate("does-not-exist", true); }, "failed to open dll file", "missing dll");
        expectThrow([] { ls::validateShaderDll(fixtures, true); }, "dll is not a regular file", "directory as dll");
        const auto fifo = scratch / "fifo.dll";
        expect(mkfifo(fifo.c_str(), 0600) == 0, "fifo created");
        expectThrow([&] { ls::validateShaderDll(fifo, true); }, "dll is not a regular file", "fifo as dll");
    }

    std::optional<std::string> failure(const std::function<void()>& fn) {
        try {
            fn();
        } catch (const std::exception& e) {
            return e.what();
        }
        return std::nullopt;
    }

    void readerCases() {
        const std::set<std::string> refused{ "truncated", "no-resources", "no-rcdata", "bad-entry" };
        for (const char* name : { "good", "truncated", "no-resources", "no-rcdata", "missing-id", "not-spirv",
                "short-blob", "odd-blob", "bad-fp16", "no-fp32", "no-fp16", "no-perf", "bad-entry" }) {
            const std::string what = std::string(name) + ".dll";
            std::unordered_map<uint32_t, std::vector<uint8_t>> backend;
            std::unordered_map<uint32_t, std::vector<uint8_t>> shared;
            const auto backendFailure = failure([&] {
                backend = lsfgvk::backend::extractResourcesFromDLL(dll(name));
            });
            const auto sharedFailure = failure([&] {
                const ls::DllFile file(dll(name));
                for (const auto& [id, resource] : file.resources()) {
                    std::vector<uint8_t> data(resource.size);
                    file.read(resource.offset, data.data(), data.size());
                    shared.emplace(id, std::move(data));
                }
            });

            expect(backendFailure.has_value() == (refused.count(name) == 1),
                "the backend reader " + std::string(backendFailure ? "refuses " : "accepts ") + what);
            expect(backendFailure == sharedFailure, "the backend reader and the dll check agree on " + what);
            expect(backend == shared, "the backend reader and the dll check read the same resources of " + what);
            if (!backendFailure)
                expect(backend.size() > 1, "resources found in " + what);
        }
    }

    void clearEnvironment() {
        for (const char* name : { "LSFGVK_DLL_PATH", "LSFGVK_NO_FP16", "LSFGVK_MULTIPLIER", "LSFGVK_FLOW_SCALE",
                "LSFGVK_PERFORMANCE_MODE", "LSFGVK_PACING", "LSFGVK_PRESENT_WAIT_OFFSET", "LSFGVK_GPU",
                "XDG_DATA_HOME" })
            unsetenv(name);
        setenv("LSFGVK_ENV", "1", 1);
    }

    std::string loadedDll() {
        const ls::WatchedConfig config;
        return config.get().global().dll.value_or("");
    }

    void envModeCases() {
        clearEnvironment();
        setenv("LSFGVK_DLL_PATH", dll("good").c_str(), 1);
        expectOk([] { expect(loadedDll() == dll("good").string(), "LSFGVK_DLL_PATH is the dll used"); },
            "env mode accepts the good dll");
        setenv("LSFGVK_DLL_PATH", dll("not-spirv").c_str(), 1);
        expectThrow([] { loadedDll(); }, "is not SPIR-V", "env mode refuses a dll without SPIR-V");
        setenv("LSFGVK_DLL_PATH", dll("no-fp16").c_str(), 1);
        expectThrow([] { loadedDll(); }, "unable to find shader resource", "env mode needs fp16 when allowed");
        setenv("LSFGVK_NO_FP16", "1", 1);
        expectOk([] { loadedDll(); }, "env mode with LSFGVK_NO_FP16 skips the fp16 set");
        unsetenv("LSFGVK_NO_FP16");
        setenv("LSFGVK_DLL_PATH", dll("no-perf").c_str(), 1);
        for (const char* perf : { "1", "0" }) {
            setenv("LSFGVK_PERFORMANCE_MODE", perf, 1);
            expectThrow([] { loadedDll(); }, "unable to find shader resource",
                std::string("env mode needs the performance set with LSFGVK_PERFORMANCE_MODE=") + perf);
        }
        unsetenv("LSFGVK_PERFORMANCE_MODE");
        setenv("LSFGVK_DLL_PATH", (scratch / "nothing.dll").c_str(), 1);
        expectThrow([] { loadedDll(); }, "path to dll is invalid", "env mode refuses a missing LSFGVK_DLL_PATH");

        unsetenv("LSFGVK_DLL_PATH");
        const auto home = scratch / "home";
        const auto folder = home / ".local/share/Steam/steamapps/common/Lossless Scaling";
        const auto found = folder / "Lossless.dll";
        std::filesystem::create_directories(folder);
        std::filesystem::create_directories(scratch / "cwd");
        std::filesystem::current_path(scratch / "cwd");
        setenv("HOME", home.c_str(), 1);

        expectThrow([] { loadedDll(); }, "unable to locate Lossless.dll", "env mode without any dll");
        std::filesystem::copy_file(dll("missing-id"), found);
        expectThrow([] { loadedDll(); }, "unable to find shader resource", "env mode checks the searched dll");
        std::filesystem::copy_file(dll("good"), found, std::filesystem::copy_options::overwrite_existing);
        expectOk([&] { expect(loadedDll() == found.string(), "the searched dll is the dll used"); },
            "env mode accepts the searched good dll");
    }

    void expectFlow(const char* value, float want) {
        setenv("LSFGVK_FLOW_SCALE", value, 1);
        expectOk([&] {
            const ls::WatchedConfig config;
            expect(config.get().profiles().front().flow_scale == want,
                std::string("LSFGVK_FLOW_SCALE=") + value + " parses");
        }, std::string("LSFGVK_FLOW_SCALE=") + value);
        unsetenv("LSFGVK_FLOW_SCALE");
    }

    void expectBadFlow(const char* value) {
        setenv("LSFGVK_FLOW_SCALE", value, 1);
        expectThrow([] { const ls::WatchedConfig config; }, "flow_scale must be between 0.25 and 1.0",
            std::string("LSFGVK_FLOW_SCALE='") + value + "' is refused");
        unsetenv("LSFGVK_FLOW_SCALE");
    }

    void expectMultiplier(const char* value, size_t want) {
        setenv("LSFGVK_MULTIPLIER", value, 1);
        expectOk([&] {
            const ls::WatchedConfig config;
            expect(config.get().profiles().front().multiplier == want,
                std::string("LSFGVK_MULTIPLIER=") + value + " parses");
        }, std::string("LSFGVK_MULTIPLIER=") + value);
        unsetenv("LSFGVK_MULTIPLIER");
    }

    void expectBadMultiplier(const char* value, const std::string& what) {
        setenv("LSFGVK_MULTIPLIER", value, 1);
        expectThrow([] { const ls::WatchedConfig config; }, what,
            std::string("LSFGVK_MULTIPLIER='") + value + "' is refused");
        unsetenv("LSFGVK_MULTIPLIER");
    }

    void parserCases() {
        clearEnvironment();
        setenv("LSFGVK_DLL_PATH", dll("good").c_str(), 1);

        expectFlow("0.25", 0.25F);
        expectFlow("0.5", 0.5F);
        expectFlow("0.75", 0.75F);
        expectFlow("1.0", 1.0F);
        expectFlow("1", 1.0F);
        for (const char* value : { "0,5", "0.5x", "", " 0.5", "abc", "nan", "inf", "0.1", "1.5", "-0.5", "+0.5" })
            expectBadFlow(value);

        expectMultiplier("2", 2);
        expectMultiplier("3", 3);
        expectMultiplier("4", 4);
        for (const char* value : { "2x", "", "-2", "+2", " 2", "2.0", "abc", "99999999999999999999999" })
            expectBadMultiplier(value, "multiplier must be an integer greater than 1");
        for (const char* value : { "0", "1" })
            expectBadMultiplier(value, "multiplier must be greater than 1");
    }
}

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        std::cerr << "usage: lsfg-vk-config-test <fixtures> <scratch> [locale]\n";
        return 2;
    }
    fixtures = std::filesystem::absolute(argv[1]);
    scratch = std::filesystem::absolute(argv[2]);
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch);

    if (argc == 4) {
        expect(std::setlocale(LC_ALL, argv[3]) != nullptr, std::string("locale ") + argv[3] + " is available");
        parserCases();
        std::cout << "config: parser checks passed in " << argv[3] << '\n';
        return 0;
    }

    validatorCases();
    readerCases();
    envModeCases();
    parserCases();
    std::cout << "config: all checks passed\n";
    return 0;
}
