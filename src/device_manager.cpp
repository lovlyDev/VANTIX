#include "vantix/compute.hpp"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace vantix {
namespace {

class DynamicLibrary {
public:
    explicit DynamicLibrary(const char* name) {
#ifdef _WIN32
        handle_ = LoadLibraryExA(name, nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
#else
        handle_ = dlopen(name, RTLD_NOW | RTLD_LOCAL);
#endif
    }
    ~DynamicLibrary() {
#ifdef _WIN32
        if (handle_) FreeLibrary(handle_);
#else
        if (handle_) dlclose(handle_);
#endif
    }
    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;
    explicit operator bool() const { return handle_ != nullptr; }
    void* symbol(const char* name) const {
#ifdef _WIN32
        return reinterpret_cast<void*>(GetProcAddress(handle_, name));
#else
        return dlsym(handle_, name);
#endif
    }
private:
#ifdef _WIN32
    HMODULE handle_ = nullptr;
#else
    void* handle_ = nullptr;
#endif
};

std::string hex_uuid(const std::uint8_t* bytes, std::size_t length) {
    const bool all_zero = std::all_of(bytes, bytes + length, [](auto byte) { return byte == 0; });
    if (all_zero) return {};
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < length; ++i) stream << std::setw(2) << static_cast<unsigned>(bytes[i]);
    return stream.str();
}

std::string vendor_name(std::uint32_t vendor_id) {
    switch (vendor_id) {
        case 0x1002: return "AMD";
        case 0x10de: return "NVIDIA";
        case 0x8086: return "Intel";
        default: {
            std::ostringstream stream;
            stream << "Vendor 0x" << std::hex << vendor_id;
            return stream.str();
        }
    }
}

void merge_device(std::vector<DeviceInfo>& devices, DeviceInfo incoming) {
    auto match = std::find_if(devices.begin(), devices.end(), [&](const auto& existing) {
        return existing.hardware_id == incoming.hardware_id;
    });
    // Older HIP drivers can report a different UUID and no LUID. Merge only
    // when exactly one Vulkan device has the same vendor, model and memory.
    if (match == devices.end() && incoming.available_apis.size() == 1 &&
        incoming.available_apis.front() != ComputeApi::vulkan) {
        std::vector<DeviceInfo*> possible;
        for (auto& existing : devices) {
            if (existing.vendor == incoming.vendor && existing.name == incoming.name &&
                existing.memory_bytes == incoming.memory_bytes &&
                std::find(existing.available_apis.begin(), existing.available_apis.end(),
                          ComputeApi::vulkan) != existing.available_apis.end() &&
                std::find(existing.available_apis.begin(), existing.available_apis.end(),
                          incoming.available_apis.front()) == existing.available_apis.end())
                possible.push_back(&existing);
        }
        if (possible.size() == 1)
            match = devices.begin() + (possible.front() - devices.data());
    }
    if (match != devices.end()) {
        auto& existing = *match;
        for (auto api : incoming.available_apis) {
            if (std::find(existing.available_apis.begin(), existing.available_apis.end(), api) ==
                existing.available_apis.end()) {
                existing.available_apis.push_back(api);
            }
        }
        existing.memory_bytes = std::max(existing.memory_bytes, incoming.memory_bytes);
        if (!incoming.driver.empty()) {
            if (!existing.driver.empty()) existing.driver += "; ";
            existing.driver += incoming.driver;
        }
        return;
    }
    devices.push_back(std::move(incoming));
}

void discover_vulkan(std::vector<DeviceInfo>& devices) {
#ifdef _WIN32
    DynamicLibrary library("vulkan-1.dll");
#else
    DynamicLibrary library("libvulkan.so.1");
#endif
    if (!library) return;
    auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(library.symbol("vkGetInstanceProcAddr"));
    if (!get) return;
    auto create = reinterpret_cast<PFN_vkCreateInstance>(get(nullptr, "vkCreateInstance"));
    if (!create) return;
    auto version_fn = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        get(nullptr, "vkEnumerateInstanceVersion"));
    std::uint32_t version = VK_API_VERSION_1_0;
    if (version_fn) version_fn(&version);

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "VANTIX";
    app.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    app.pEngineName = "VANTIX";
    app.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    app.apiVersion = version >= VK_API_VERSION_1_1 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0;

    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (create(&create_info, nullptr, &instance) != VK_SUCCESS) return;

    const auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(get(instance, "vkDestroyInstance"));
    const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        get(instance, "vkEnumeratePhysicalDevices"));
    const auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        get(instance, "vkGetPhysicalDeviceProperties"));
    const auto properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        get(instance, "vkGetPhysicalDeviceProperties2"));
    const auto memory = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        get(instance, "vkGetPhysicalDeviceMemoryProperties"));
    const auto queues = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        get(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));

    if (enumerate && properties && memory && queues) {
        std::uint32_t count = 0;
        if (enumerate(instance, &count, nullptr) == VK_SUCCESS && count > 0 && count <= 64) {
            std::vector<VkPhysicalDevice> physical(count);
            if (enumerate(instance, &count, physical.data()) == VK_SUCCESS) {
                std::unordered_set<std::string> seen;
                for (std::uint32_t index = 0; index < count; ++index) {
                    VkPhysicalDeviceProperties prop{};
                    properties(physical[index], &prop);
                    VkPhysicalDeviceMemoryProperties mem{};
                    memory(physical[index], &mem);
                    std::uint64_t local_bytes = 0;
                    for (std::uint32_t heap = 0; heap < mem.memoryHeapCount; ++heap) {
                        if (mem.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                            local_bytes += mem.memoryHeaps[heap].size;
                        }
                    }
                    std::uint32_t queue_count = 0;
                    queues(physical[index], &queue_count, nullptr);
                    std::vector<VkQueueFamilyProperties> family(queue_count);
                    if (queue_count) queues(physical[index], &queue_count, family.data());
                    const bool compute = std::any_of(family.begin(), family.end(), [](const auto& f) {
                        return (f.queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
                    });
                    if (!compute) continue;

                    std::string uuid;
                    if (properties2) {
                        VkPhysicalDeviceIDProperties ids{};
                        ids.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
                        VkPhysicalDeviceProperties2 extended{};
                        extended.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                        extended.pNext = &ids;
                        properties2(physical[index], &extended);
#ifdef _WIN32
                        if (ids.deviceLUIDValid) {
                            const auto value = hex_uuid(ids.deviceLUID, VK_LUID_SIZE);
                            if (!value.empty()) uuid = "luid:" + value;
                        }
#endif
                        if (uuid.empty()) uuid = hex_uuid(ids.deviceUUID, VK_UUID_SIZE);
                    }
                    std::ostringstream fallback;
                    fallback << "vulkan:" << std::hex << prop.vendorID << ':' << prop.deviceID
                             << ':' << std::dec << index;
                    auto hardware_id = uuid.empty() ? fallback.str()
                        : (uuid.starts_with("luid:") ? uuid : "gpu:" + uuid);
                    if (!seen.insert(hardware_id).second) continue;

                    std::ostringstream driver;
                    driver << "Vulkan raw driver version " << prop.driverVersion;
                    merge_device(devices, {
                        hardware_id,
                        prop.deviceName,
                        vendor_name(prop.vendorID),
                        driver.str(),
                        local_bytes,
                        {ComputeApi::vulkan},
                        false  // Search eligibility is decided by backend self-test.
                    });
                }
            }
        }
    }
    if (destroy) destroy(instance, nullptr);
}

void discover_cuda(std::vector<DeviceInfo>& devices) {
#ifdef _WIN32
    DynamicLibrary library("nvcuda.dll");
#else
    DynamicLibrary library("libcuda.so.1");
#endif
    if (!library) return;
    const auto init = reinterpret_cast<int (*)(unsigned)>(library.symbol("cuInit"));
    const auto count_fn = reinterpret_cast<int (*)(int*)>(library.symbol("cuDeviceGetCount"));
    const auto get_fn = reinterpret_cast<int (*)(int*, int)>(library.symbol("cuDeviceGet"));
    const auto name_fn = reinterpret_cast<int (*)(char*, int, int)>(
        library.symbol("cuDeviceGetName"));
    const auto uuid_fn = reinterpret_cast<int (*)(void*, int)>(
        library.symbol("cuDeviceGetUuid_v2") ?
        library.symbol("cuDeviceGetUuid_v2") : library.symbol("cuDeviceGetUuid"));
#ifdef _WIN32
    const auto luid_fn = reinterpret_cast<int (*)(char*, unsigned*, int)>(
        library.symbol("cuDeviceGetLuid"));
#endif
    const auto memory_fn = reinterpret_cast<int (*)(std::size_t*, int)>(
        library.symbol("cuDeviceTotalMem_v2") ?
        library.symbol("cuDeviceTotalMem_v2") : library.symbol("cuDeviceTotalMem"));
    const auto driver_fn = reinterpret_cast<int (*)(int*)>(
        library.symbol("cuDriverGetVersion"));
    if (!init || !count_fn || !get_fn || !name_fn || init(0) != 0) return;
    int count = 0;
    if (count_fn(&count) != 0 || count < 0 || count > 64) return;
    int driver_version = 0;
    if (driver_fn) driver_fn(&driver_version);
    for (int index = 0; index < count; ++index) {
        int handle = 0;
        if (get_fn(&handle, index) != 0) continue;
        std::array<char, 256> name{};
        if (name_fn(name.data(), static_cast<int>(name.size()), handle) != 0) continue;
        std::array<std::uint8_t, 16> uuid{};
        if (uuid_fn) uuid_fn(uuid.data(), handle);
        std::size_t bytes = 0;
        if (memory_fn) memory_fn(&bytes, handle);
        const auto id = hex_uuid(uuid.data(), uuid.size());
        std::string hardware_id = id.empty() ? "cuda:" + std::to_string(index) : "gpu:" + id;
#ifdef _WIN32
        std::array<char, 8> luid{};
        unsigned node_mask = 0;
        if (luid_fn && luid_fn(luid.data(), &node_mask, handle) == 0) {
            const auto value = hex_uuid(reinterpret_cast<const std::uint8_t*>(luid.data()),
                                        luid.size());
            if (!value.empty()) hardware_id = "luid:" + value;
        }
#endif
        merge_device(devices, {
            hardware_id,
            name.data(),
            "NVIDIA",
            "CUDA driver " + std::to_string(driver_version),
            static_cast<std::uint64_t>(bytes),
            {ComputeApi::cuda},
            false
        });
    }
}

void discover_hip(std::vector<DeviceInfo>& devices) {
#ifdef _WIN32
    std::unique_ptr<DynamicLibrary> library;
    for (const auto* name : {"amdhip64_7.dll", "amdhip64_6.dll", "amdhip64.dll"}) {
        auto candidate = std::make_unique<DynamicLibrary>(name);
        if (*candidate) { library = std::move(candidate); break; }
    }
#else
    auto library = std::make_unique<DynamicLibrary>("libamdhip64.so");
#endif
    if (!library || !*library) return;
    const auto init = reinterpret_cast<int (*)(unsigned)>(library->symbol("hipInit"));
    const auto count_fn = reinterpret_cast<int (*)(int*)>(library->symbol("hipGetDeviceCount"));
    const auto name_fn = reinterpret_cast<int (*)(char*, int, int)>(
        library->symbol("hipDeviceGetName"));
    const auto uuid_fn = reinterpret_cast<int (*)(void*, int)>(
        library->symbol("hipDeviceGetUuid"));
#ifdef _WIN32
    const auto luid_fn = reinterpret_cast<int (*)(char*, unsigned*, int)>(
        library->symbol("hipDeviceGetLuid"));
#endif
    const auto memory_fn = reinterpret_cast<int (*)(std::size_t*, int)>(
        library->symbol("hipDeviceTotalMem"));
    const auto driver_fn = reinterpret_cast<int (*)(int*)>(
        library->symbol("hipDriverGetVersion"));
    if (!init || !count_fn || !name_fn || init(0) != 0) return;
    int count = 0;
    if (count_fn(&count) != 0 || count < 0 || count > 64) return;
    int driver_version = 0;
    if (driver_fn) driver_fn(&driver_version);
    for (int index = 0; index < count; ++index) {
        std::array<char, 256> name{};
        if (name_fn(name.data(), static_cast<int>(name.size()), index) != 0) continue;
        std::array<std::uint8_t, 16> uuid{};
        if (uuid_fn) uuid_fn(uuid.data(), index);
        std::size_t bytes = 0;
        if (memory_fn) memory_fn(&bytes, index);
        const auto id = hex_uuid(uuid.data(), uuid.size());
        std::string hardware_id = id.empty() ? "hip:" + std::to_string(index) : "gpu:" + id;
#ifdef _WIN32
        std::array<char, 8> luid{};
        unsigned node_mask = 0;
        if (luid_fn && luid_fn(luid.data(), &node_mask, index) == 0) {
            const auto value = hex_uuid(reinterpret_cast<const std::uint8_t*>(luid.data()),
                                        luid.size());
            if (!value.empty()) hardware_id = "luid:" + value;
        }
#endif
        merge_device(devices, {
            hardware_id,
            name.data(),
            "AMD",
            "HIP driver " + std::to_string(driver_version),
            static_cast<std::uint64_t>(bytes),
            {ComputeApi::hip},
            false
        });
    }
}

}  // namespace

std::vector<DeviceInfo> DeviceManager::discover() {
    std::vector<DeviceInfo> devices;
    devices.push_back({
        "cpu:host",
        "CPU (" + std::to_string(std::max(1u, std::thread::hardware_concurrency())) + " threads)",
        "Host",
        "",
        0,
        {ComputeApi::cpu},
        true
    });
    discover_vulkan(devices);
    discover_cuda(devices);
    discover_hip(devices);
    return devices;
}

}  // namespace vantix
