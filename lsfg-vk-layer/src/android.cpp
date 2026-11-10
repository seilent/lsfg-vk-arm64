/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "android_pacing.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <streambuf>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <android/log.h>
#include <sys/system_properties.h>
#include <time.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan_core.h>

namespace {
    constexpr const char* LAYER_NAME = "VK_LAYER_LSFGVK_frame_generation";
    constexpr const char* LOG_TAG = "lsfgvk";

    thread_local std::string log_line;

    class LogBuffer : public std::streambuf {
    protected:
        int overflow(int c) override {
            if (c == traits_type::eof())
                return traits_type::not_eof(c);

            if (c == '\n') {
                __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "%s", log_line.c_str());
                log_line.clear();
            } else {
                log_line.push_back(static_cast<char>(c));
            }
            return c;
        }
    };

    std::string property(const char* name) {
        std::string value(PROP_VALUE_MAX, '\0');
        const int len = __system_property_get(name, value.data());
        value.resize(len > 0 ? static_cast<size_t>(len) : 0);
        return value;
    }

    std::string package() {
        std::ifstream cmdline("/proc/self/cmdline");
        std::string name;
        std::getline(cmdline, name, '\0');
        return name;
    }

    void configure() {
        std::cerr.rdbuf(new LogBuffer);

        setenv("LSFGVK_ENV", "1", 1);

        const std::pair<const char*, const char*> props[] = {
            { "debug.lsfgvk.multiplier", "LSFGVK_MULTIPLIER" },
            { "debug.lsfgvk.flow_scale", "LSFGVK_FLOW_SCALE" },
            { "debug.lsfgvk.performance_mode", "LSFGVK_PERFORMANCE_MODE" },
            { "debug.lsfgvk.dll_path", "LSFGVK_DLL_PATH" }
        };
        for (const auto& [prop, env] : props) {
            const auto value = property(prop);
            if (!value.empty())
                setenv(env, value.c_str(), 1);
        }
    }

    struct Instance {
        PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
        bool active;
    };

    struct Device {
        PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
        bool active;
        PFN_vkQueuePresentKHR QueuePresentKHR{};
    };

    struct Pacing {
        const prop_info* info{};
        bool searched{};
        uint32_t areaSerial{};
        bool read{};
        uint32_t serial{};
        int64_t interval{};
        int64_t deadline{};
    };

    struct State {
        std::mutex mutex;
        std::unordered_map<VkInstance, Instance> instances;
        std::unordered_map<VkDevice, Device> devices;
        std::unordered_map<VkQueue, VkDevice> queues;
        Pacing pacing;
        bool unknownQueue{};

        std::once_flag negotiated;
        PFN_vkGetInstanceProcAddr GetInstanceProcAddr{};
        PFN_vkGetDeviceProcAddr GetDeviceProcAddr{};
    };
    State& state = *new State;

    bool negotiate() {
        std::call_once(state.negotiated, [] {
            VkNegotiateLayerInterface info{
                .sType = LAYER_NEGOTIATE_INTERFACE_STRUCT,
                .loaderLayerInterfaceVersion = 2
            };
            configure();
            const VkResult res = vkNegotiateLoaderLayerInterfaceVersion(&info);
            if (res != VK_SUCCESS || !info.pfnGetInstanceProcAddr || !info.pfnGetDeviceProcAddr) {
                __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                    "layer initialization failed (%d), passing through", res);
                return;
            }

            state.GetInstanceProcAddr = info.pfnGetInstanceProcAddr;
            state.GetDeviceProcAddr = info.pfnGetDeviceProcAddr;
        });
        return state.GetInstanceProcAddr != nullptr;
    }

    bool wantsActive(const VkInstanceCreateInfo* info) {
        if (std::getenv("DISABLE_LSFGVK"))
            return false;

        const auto* app = info->pApplicationInfo;
        if (app && app->pEngineName && std::strcmp(app->pEngineName, "mesa zink") == 0)
            return false;

        const auto multiplier = property("debug.lsfgvk.multiplier");
        return !multiplier.empty() && multiplier != "1";
    }

    template<typename T>
    T* findLinkInfo(const void* pNext, VkStructureType type) {
        auto* info = reinterpret_cast<T*>(const_cast<void*>(pNext));
        while (info && (info->sType != type || info->function != VK_LAYER_LINK_INFO))
            info = reinterpret_cast<T*>(const_cast<void*>(info->pNext));
        return info;
    }

    std::optional<Instance> findInstance(VkInstance instance) {
        const std::scoped_lock lock(state.mutex);
        const auto& it = state.instances.find(instance);
        if (it == state.instances.end())
            return std::nullopt;
        return it->second;
    }

    std::optional<Device> findDevice(VkDevice device) {
        const std::scoped_lock lock(state.mutex);
        const auto& it = state.devices.find(device);
        if (it == state.devices.end())
            return std::nullopt;
        return it->second;
    }

    std::optional<std::pair<VkInstance, Instance>> findOwner(VkPhysicalDevice physdev) {
        const std::scoped_lock lock(state.mutex);
        for (const auto& [handle, instance] : state.instances) {
            auto* enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
                instance.GetInstanceProcAddr(handle, "vkEnumeratePhysicalDevices"));
            if (!enumerate)
                continue;

            uint32_t count{};
            if (enumerate(handle, &count, nullptr) != VK_SUCCESS)
                continue;
            std::vector<VkPhysicalDevice> physdevs(count);
            if (enumerate(handle, &count, physdevs.data()) < 0)
                continue;

            for (uint32_t i = 0; i < count; i++)
                if (physdevs.at(i) == physdev)
                    return std::make_pair(handle, instance);
        }
        return std::nullopt;
    }

    VkResult shimCreateInstance(
            const VkInstanceCreateInfo* info,
            const VkAllocationCallbacks* alloc,
            VkInstance* instance) {
        auto* layerInfo = findLinkInfo<VkLayerInstanceCreateInfo>(info->pNext,
            VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO);
        if (!layerInfo || !layerInfo->u.pLayerInfo
                || !layerInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr)
            return VK_ERROR_INITIALIZATION_FAILED;

        auto* link = layerInfo->u.pLayerInfo;
        Instance entry{
            .GetInstanceProcAddr = link->pfnNextGetInstanceProcAddr,
            .active = wantsActive(info) && negotiate()
        };

        VkResult res = VK_ERROR_INITIALIZATION_FAILED;
        if (entry.active) {
            auto* vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
                state.GetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
            if (vkCreateInstance)
                res = vkCreateInstance(info, alloc, instance);
            if (res == VK_SUCCESS) {
                entry.GetInstanceProcAddr = state.GetInstanceProcAddr;
            } else {
                __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                    "frame generation vkCreateInstance failed (%d), passing through", res);
                entry.active = false;
            }
        }
        if (!entry.active) {
            layerInfo->u.pLayerInfo = link->pNext;
            auto* vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
                entry.GetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
            if (!vkCreateInstance)
                return VK_ERROR_INITIALIZATION_FAILED;
            res = vkCreateInstance(info, alloc, instance);
        }
        if (res != VK_SUCCESS)
            return res;

        const std::scoped_lock lock(state.mutex);
        state.instances[*instance] = entry;
        return res;
    }

    void shimDestroyInstance(VkInstance instance, const VkAllocationCallbacks* alloc) {
        const auto entry = findInstance(instance);
        if (!entry)
            return;

        {
            const std::scoped_lock lock(state.mutex);
            state.instances.erase(instance);
        }

        auto* vkDestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(
            entry->GetInstanceProcAddr(instance, "vkDestroyInstance"));
        if (vkDestroyInstance)
            vkDestroyInstance(instance, alloc);
    }

    VkResult shimCreateDevice(
            VkPhysicalDevice physdev,
            const VkDeviceCreateInfo* info,
            const VkAllocationCallbacks* alloc,
            VkDevice* device) {
        auto* layerInfo = findLinkInfo<VkLayerDeviceCreateInfo>(info->pNext,
            VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO);
        if (!layerInfo || !layerInfo->u.pLayerInfo
                || !layerInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr
                || !layerInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr)
            return VK_ERROR_INITIALIZATION_FAILED;

        const auto owner = findOwner(physdev);
        if (!owner) {
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                "vkCreateDevice on an unknown physical device");
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto& [handle, instance] = *owner;

        auto* link = layerInfo->u.pLayerInfo;
        Device entry{
            .GetDeviceProcAddr = link->pfnNextGetDeviceProcAddr,
            .active = instance.active && !std::getenv("DISABLE_LSFGVK")
        };

        VkResult res = VK_ERROR_INITIALIZATION_FAILED;
        if (entry.active) {
            auto* vkCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(
                instance.GetInstanceProcAddr(handle, "vkCreateDevice"));
            if (vkCreateDevice)
                res = vkCreateDevice(physdev, info, alloc, device);
            if (res == VK_SUCCESS) {
                entry.GetDeviceProcAddr = state.GetDeviceProcAddr;
            } else {
                __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                    "frame generation vkCreateDevice failed (%d), passing through", res);
                entry.active = false;
            }
        }
        if (!entry.active) {
            layerInfo->u.pLayerInfo = link->pNext;
            auto* vkCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(
                link->pfnNextGetInstanceProcAddr(handle, "vkCreateDevice"));
            if (!vkCreateDevice)
                return VK_ERROR_INITIALIZATION_FAILED;
            res = vkCreateDevice(physdev, info, alloc, device);
        }
        if (res != VK_SUCCESS)
            return res;

        entry.QueuePresentKHR = reinterpret_cast<PFN_vkQueuePresentKHR>(
            entry.GetDeviceProcAddr(*device, "vkQueuePresentKHR"));

        const std::scoped_lock lock(state.mutex);
        state.devices[*device] = entry;
        return res;
    }

    void shimDestroyDevice(VkDevice device, const VkAllocationCallbacks* alloc) {
        const auto entry = findDevice(device);
        if (!entry)
            return;

        {
            const std::scoped_lock lock(state.mutex);
            state.devices.erase(device);
            std::erase_if(state.queues, [device](const auto& queue) {
                return queue.second == device;
            });
        }

        auto* vkDestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(
            entry->GetDeviceProcAddr(device, "vkDestroyDevice"));
        if (vkDestroyDevice)
            vkDestroyDevice(device, alloc);
    }

    VkResult shimCreateSwapchainKHR(
            VkDevice device,
            const VkSwapchainCreateInfoKHR* info,
            const VkAllocationCallbacks* alloc,
            VkSwapchainKHR* swapchain) {
        const auto entry = findDevice(device);
        if (!entry)
            return VK_ERROR_INITIALIZATION_FAILED;

        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "vkCreateSwapchainKHR in %s (%s)",
            package().c_str(), entry->active ? "active" : "pass through");

        auto* vkCreateSwapchainKHR = reinterpret_cast<PFN_vkCreateSwapchainKHR>(
            entry->GetDeviceProcAddr(device, "vkCreateSwapchainKHR"));
        if (!vkCreateSwapchainKHR)
            return VK_ERROR_INITIALIZATION_FAILED;

        return vkCreateSwapchainKHR(device, info, alloc, swapchain);
    }

    void recordQueue(VkDevice device, VkQueue queue) {
        if (!queue)
            return;

        const std::scoped_lock lock(state.mutex);
        state.queues[queue] = device;
    }

    void shimGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* queue) {
        *queue = VK_NULL_HANDLE;
        const auto entry = findDevice(device);
        if (!entry)
            return;

        auto* vkGetDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(
            entry->GetDeviceProcAddr(device, "vkGetDeviceQueue"));
        if (!vkGetDeviceQueue)
            return;

        vkGetDeviceQueue(device, family, index, queue);
        recordQueue(device, *queue);
    }

    void shimGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue) {
        *queue = VK_NULL_HANDLE;
        const auto entry = findDevice(device);
        if (!entry)
            return;

        auto* vkGetDeviceQueue2 = reinterpret_cast<PFN_vkGetDeviceQueue2>(
            entry->GetDeviceProcAddr(device, "vkGetDeviceQueue2"));
        if (!vkGetDeviceQueue2)
            return;

        vkGetDeviceQueue2(device, info, queue);
        recordQueue(device, *queue);
    }

    int64_t paceInterval(Pacing& pacing) {
        if (!pacing.info) {
            const uint32_t areaSerial = __system_property_area_serial();
            if (pacing.searched && areaSerial == pacing.areaSerial)
                return 0;

            pacing.searched = true;
            pacing.areaSerial = areaSerial;
            pacing.info = __system_property_find("debug.lsfgvk.fps");
            if (!pacing.info)
                return 0;
        }

        const uint32_t serial = __system_property_serial(pacing.info);
        if (!pacing.read || serial != pacing.serial) {
            pacing.read = true;
            pacing.serial = serial;
            __system_property_read_callback(pacing.info,
                [](void* cookie, const char*, const char* value, uint32_t) {
                    *static_cast<int64_t*>(cookie) = lsfgvk::layer::fpsInterval(value);
                }, &pacing.interval);
        }
        return pacing.interval;
    }

    int64_t monotonicNow() {
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
    }

    VkResult shimQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info) {
        PFN_vkQueuePresentKHR vkQueuePresentKHR{};
        int64_t deadline{};
        {
            const std::scoped_lock lock(state.mutex);
            const auto& queueIt = state.queues.find(queue);
            if (queueIt != state.queues.end()) {
                const auto& deviceIt = state.devices.find(queueIt->second);
                if (deviceIt != state.devices.end())
                    vkQueuePresentKHR = deviceIt->second.QueuePresentKHR;
            }
            if (!vkQueuePresentKHR) {
                if (!state.unknownQueue)
                    __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                        "vkQueuePresentKHR on an unknown queue");
                state.unknownQueue = true;
                return VK_ERROR_DEVICE_LOST;
            }

            const int64_t interval = paceInterval(state.pacing);
            if (interval > 0) {
                state.pacing.deadline = lsfgvk::layer::nextPresentDeadline(
                    state.pacing.deadline, monotonicNow(), interval);
                deadline = state.pacing.deadline;
            }
        }

        if (deadline > 0) {
            const timespec ts{
                .tv_sec = static_cast<time_t>(deadline / 1000000000),
                .tv_nsec = static_cast<long>(deadline % 1000000000)
            };
            while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {}
        }

        return vkQueuePresentKHR(queue, info);
    }

    VkResult layerProperties(uint32_t* count, VkLayerProperties* props) {
        if (!props) {
            *count = 1;
            return VK_SUCCESS;
        }
        if (*count < 1) {
            *count = 0;
            return VK_INCOMPLETE;
        }

        *props = VkLayerProperties{
            .specVersion = VK_MAKE_API_VERSION(0, 1, 4, 328),
            .implementationVersion = 2
        };
        std::strncpy(props->layerName, LAYER_NAME, VK_MAX_EXTENSION_NAME_SIZE - 1);
        std::strncpy(props->description, "Lossless Scaling frame generation layer",
            VK_MAX_DESCRIPTION_SIZE - 1);
        *count = 1;
        return VK_SUCCESS;
    }

    VkResult extensionProperties(const char* layer, uint32_t* count) {
        if (!layer || std::strcmp(layer, LAYER_NAME) != 0)
            return VK_ERROR_LAYER_NOT_PRESENT;

        *count = 0;
        return VK_SUCCESS;
    }

    PFN_vkVoidFunction intercept(const char* name);
}

__attribute__((visibility("default")))
VkResult vkEnumerateInstanceLayerProperties(uint32_t* pPropertyCount,
        VkLayerProperties* pProperties) {
    return layerProperties(pPropertyCount, pProperties);
}

__attribute__((visibility("default")))
VkResult vkEnumerateInstanceExtensionProperties(const char* pLayerName,
        uint32_t* pPropertyCount, VkExtensionProperties*) {
    return extensionProperties(pLayerName, pPropertyCount);
}

__attribute__((visibility("default")))
VkResult vkEnumerateDeviceLayerProperties(VkPhysicalDevice,
        uint32_t* pPropertyCount, VkLayerProperties* pProperties) {
    return layerProperties(pPropertyCount, pProperties);
}

__attribute__((visibility("default")))
VkResult vkEnumerateDeviceExtensionProperties(VkPhysicalDevice, const char* pLayerName,
        uint32_t* pPropertyCount, VkExtensionProperties*) {
    return extensionProperties(pLayerName, pPropertyCount);
}

__attribute__((visibility("default")))
PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    if (!pName) return nullptr;

    auto func = intercept(pName);
    if (func) return func;

    const auto entry = findInstance(instance);
    if (!entry) return nullptr;
    return entry->GetInstanceProcAddr(instance, pName);
}

__attribute__((visibility("default")))
PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    if (!pName) return nullptr;

    auto func = intercept(pName);
    if (func) return func;

    const auto entry = findDevice(device);
    if (!entry) return nullptr;
    return entry->GetDeviceProcAddr(device, pName);
}

namespace {
    PFN_vkVoidFunction intercept(const char* name) {
#define VKPTR(name) reinterpret_cast<PFN_vkVoidFunction>(name)
        static const std::unordered_map<std::string, PFN_vkVoidFunction> map{
            { "vkGetInstanceProcAddr", VKPTR(vkGetInstanceProcAddr) },
            { "vkGetDeviceProcAddr", VKPTR(vkGetDeviceProcAddr) },
            { "vkCreateInstance", VKPTR(shimCreateInstance) },
            { "vkDestroyInstance", VKPTR(shimDestroyInstance) },
            { "vkCreateDevice", VKPTR(shimCreateDevice) },
            { "vkDestroyDevice", VKPTR(shimDestroyDevice) },
            { "vkCreateSwapchainKHR", VKPTR(shimCreateSwapchainKHR) },
            { "vkGetDeviceQueue", VKPTR(shimGetDeviceQueue) },
            { "vkGetDeviceQueue2", VKPTR(shimGetDeviceQueue2) },
            { "vkQueuePresentKHR", VKPTR(shimQueuePresentKHR) }
        };
#undef VKPTR
        const auto& it = map.find(name);
        if (it == map.end())
            return nullptr;
        return it->second;
    }
}
