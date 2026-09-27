#pragma once

#include "derive_kernel.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

#ifdef VANTIX_VENDOR_MOCK
#define gpuError_t int
#define gpuSuccess 0
#define gpuStream_t void*
#define gpuSetDevice mockSetDevice
#define gpuGetDeviceCount mockGetDeviceCount
#define gpuDeviceGetName mockDeviceGetName
#define gpuMalloc mockMalloc
#define gpuFree mockFree
#define gpuMallocHost mockMalloc
#define gpuFreeHost mockFree
#define gpuMemcpyAsync mockMemcpyAsync
#define gpuMemcpyHostToDevice 1
#define gpuMemcpyDeviceToHost 2
#define gpuStreamCreateWithFlags mockStreamCreateWithFlags
#define gpuStreamNonBlocking 1
#define gpuStreamSynchronize mockStreamSynchronize
#define gpuStreamDestroy mockStreamDestroy
#define gpuGetLastError mockGetLastError
#define gpuGetErrorString mockGetErrorString
#define gpuMemset mockMemset
#elif defined(VANTIX_HIP)
#define gpuError_t hipError_t
#define gpuSuccess hipSuccess
#define gpuStream_t hipStream_t
#define gpuSetDevice hipSetDevice
#define gpuGetDeviceCount hipGetDeviceCount
#define gpuDeviceGetName hipDeviceGetName
#define gpuMalloc hipMalloc
#define gpuFree hipFree
#define gpuMallocHost(pointer, bytes) hipHostMalloc(pointer, bytes, 0)
#define gpuFreeHost hipHostFree
#define gpuMemcpyAsync hipMemcpyAsync
#define gpuMemcpyHostToDevice hipMemcpyHostToDevice
#define gpuMemcpyDeviceToHost hipMemcpyDeviceToHost
#define gpuStreamCreateWithFlags hipStreamCreateWithFlags
#define gpuStreamNonBlocking hipStreamNonBlocking
#define gpuStreamSynchronize hipStreamSynchronize
#define gpuStreamDestroy hipStreamDestroy
#define gpuGetLastError hipGetLastError
#define gpuGetErrorString hipGetErrorString
#define gpuMemset hipMemset
#else
#define gpuError_t cudaError_t
#define gpuSuccess cudaSuccess
#define gpuStream_t cudaStream_t
#define gpuSetDevice cudaSetDevice
#define gpuGetDeviceCount cudaGetDeviceCount
#define gpuDeviceGetName cudaDeviceGetName
#define gpuMalloc cudaMalloc
#define gpuFree cudaFree
#define gpuMallocHost cudaMallocHost
#define gpuFreeHost cudaFreeHost
#define gpuMemcpyAsync cudaMemcpyAsync
#define gpuMemcpyHostToDevice cudaMemcpyHostToDevice
#define gpuMemcpyDeviceToHost cudaMemcpyDeviceToHost
#define gpuStreamCreateWithFlags cudaStreamCreateWithFlags
#define gpuStreamNonBlocking cudaStreamNonBlocking
#define gpuStreamSynchronize cudaStreamSynchronize
#define gpuStreamDestroy cudaStreamDestroy
#define gpuGetLastError cudaGetLastError
#define gpuGetErrorString cudaGetErrorString
#define gpuMemset cudaMemset
#endif

namespace vantix_vendor {
constexpr unsigned slot_size = 64;
constexpr std::size_t input_bytes = slot_size * 128;
constexpr std::size_t output_bytes = slot_size * 32;

void check(gpuError_t status, const char* operation) {
    if (status != gpuSuccess)
        throw std::runtime_error(std::string(operation) + ": " + gpuGetErrorString(status));
}

void wipe(void* pointer, std::size_t size) {
    volatile unsigned char* out = static_cast<volatile unsigned char*>(pointer);
    while (size--) *out++ = 0;
}

std::string hex_id(const unsigned char* bytes, std::size_t size, const char* prefix) {
    if (std::all_of(bytes, bytes + size, [](auto byte) { return byte == 0; })) return {};
    std::ostringstream out;
    out << prefix << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < size; ++i)
        out << std::setw(2) << static_cast<unsigned>(bytes[i]);
    return out.str();
}

struct Slot {
    unsigned char* device_input = nullptr;
    unsigned char* device_output = nullptr;
    unsigned char* host_input = nullptr;
    unsigned char* host_output = nullptr;
    gpuStream_t stream = nullptr;
    unsigned count = 0;
    void allocate() {
        check(gpuStreamCreateWithFlags(&stream, gpuStreamNonBlocking), "stream creation");
        check(gpuMalloc(reinterpret_cast<void**>(&device_input), input_bytes), "GPU input allocation");
        check(gpuMalloc(reinterpret_cast<void**>(&device_output), output_bytes), "GPU output allocation");
        check(gpuMallocHost(reinterpret_cast<void**>(&host_input), input_bytes), "pinned input allocation");
        check(gpuMallocHost(reinterpret_cast<void**>(&host_output), output_bytes), "pinned output allocation");
    }
    void clear() noexcept {
        if (stream) gpuStreamSynchronize(stream);
        if (host_input) { wipe(host_input, input_bytes); gpuFreeHost(host_input); }
        if (host_output) { wipe(host_output, output_bytes); gpuFreeHost(host_output); }
        if (device_input) { gpuMemset(device_input, 0, input_bytes); gpuFree(device_input); }
        if (device_output) { gpuMemset(device_output, 0, output_bytes); gpuFree(device_output); }
        if (stream) gpuStreamDestroy(stream);
    }
};

struct Context {
    int index;
    std::array<Slot, 2> slots{};
    std::string error;
    explicit Context(int device) : index(device) {
        check(gpuSetDevice(index), "GPU selection");
        try { for (auto& slot : slots) slot.allocate(); }
        catch (...) {
            for (auto& slot : slots) slot.clear();
            throw;
        }
    }
    ~Context() {
        gpuSetDevice(index);
        for (auto& slot : slots) slot.clear();
    }
    void derive(const unsigned char* input, unsigned count, unsigned char* output) {
        if (!input || !output || count == 0 || count > 128)
            throw std::invalid_argument("GPU batch must contain 1..128 phrases");
        check(gpuSetDevice(index), "GPU selection");
        unsigned offset = 0;
        for (auto& slot : slots) {
            slot.count = std::min(slot_size, count - offset);
            if (!slot.count) continue;
            const auto bytes = static_cast<std::size_t>(slot.count) * 128;
            std::memset(slot.host_input, 0, input_bytes);
            std::memcpy(slot.host_input, input + offset * 128, bytes);
            check(gpuMemcpyAsync(slot.device_input, slot.host_input, bytes,
                                 gpuMemcpyHostToDevice, slot.stream), "GPU input transfer");
#ifdef VANTIX_VENDOR_MOCK
            for (unsigned lane = 0; lane < slot.count; ++lane) {
                threadIdx.x = lane;
                vantix_gpu::derive_kernel(slot.device_input, slot.device_output,
                                          slot.count);
            }
#else
            vantix_gpu::derive_kernel<<<1, 64, 0, slot.stream>>>(
                slot.device_input, slot.device_output, slot.count);
#endif
            check(gpuGetLastError(), "GPU kernel launch");
            check(gpuMemcpyAsync(slot.host_output, slot.device_output,
                                 static_cast<std::size_t>(slot.count) * 32,
                                 gpuMemcpyDeviceToHost, slot.stream), "GPU output transfer");
            offset += slot.count;
        }
        offset = 0;
        for (auto& slot : slots) {
            if (!slot.count) continue;
            check(gpuStreamSynchronize(slot.stream), "GPU derivation");
            std::memcpy(output + offset * 32, slot.host_output,
                        static_cast<std::size_t>(slot.count) * 32);
            offset += slot.count;
            wipe(slot.host_input, input_bytes);
            wipe(slot.host_output, output_bytes);
            check(gpuMemset(slot.device_input, 0, input_bytes), "GPU input wipe");
            check(gpuMemset(slot.device_output, 0, output_bytes), "GPU output wipe");
            slot.count = 0;
        }
    }
};

std::string device_id(int index) {
#ifdef VANTIX_VENDOR_MOCK
    auto id = std::string("mock:") + std::to_string(index);
#elif defined(VANTIX_HIP)
    hipUUID uuid{};
    std::string id;
    if (hipDeviceGetUuid(&uuid, index) == hipSuccess)
        id = hex_id(reinterpret_cast<const unsigned char*>(&uuid), 16, "gpu:");
#ifdef _WIN32
    char luid[8]{};
    unsigned node_mask = 0;
    if (hipDeviceGetLuid(luid, &node_mask, index) == hipSuccess) {
        auto value = hex_id(reinterpret_cast<const unsigned char*>(luid), 8, "luid:");
        if (!value.empty()) id = std::move(value);
    }
#endif
#else
    std::string id;
    CUdevice device{};
    if (cuInit(0) == CUDA_SUCCESS && cuDeviceGet(&device, index) == CUDA_SUCCESS) {
        CUuuid uuid{};
        if (cuDeviceGetUuid(&uuid, device) == CUDA_SUCCESS)
            id = hex_id(reinterpret_cast<const unsigned char*>(&uuid), 16, "gpu:");
#ifdef _WIN32
        char luid[8]{};
        unsigned node_mask = 0;
        if (cuDeviceGetLuid(luid, &node_mask, device) == CUDA_SUCCESS) {
            auto value = hex_id(reinterpret_cast<const unsigned char*>(luid), 8, "luid:");
            if (!value.empty()) id = std::move(value);
        }
#endif
    }
#endif
    if (id.empty()) id = std::string("vendor:") + std::to_string(index);
    return id;
}
}  // namespace vantix_vendor

#ifdef _WIN32
#define VANTIX_EXPORT extern "C" __declspec(dllexport)
#else
#define VANTIX_EXPORT extern "C" __attribute__((visibility("default")))
#endif

VANTIX_EXPORT int vantix_vendor_abi_version() { return 1; }
VANTIX_EXPORT const char* vantix_vendor_build_id() { return __DATE__ " " __TIME__; }
VANTIX_EXPORT int vantix_vendor_device_count() {
    int count = 0;
    return gpuGetDeviceCount(&count) == gpuSuccess ? count : 0;
}
VANTIX_EXPORT int vantix_vendor_device_name(int index, char* out, std::size_t size) {
    if (!out || size == 0 || size > 4096) return 0;
    return gpuDeviceGetName(out, static_cast<int>(size), index) == gpuSuccess;
}
VANTIX_EXPORT int vantix_vendor_device_id(int index, char* out, std::size_t size) {
    if (!out || size == 0) return 0;
    const auto id = vantix_vendor::device_id(index);
    if (id.size() + 1 > size) return 0;
    std::memcpy(out, id.c_str(), id.size() + 1);
    return 1;
}
VANTIX_EXPORT void* vantix_vendor_create(int index) {
    try { return new vantix_vendor::Context(index); }
    catch (...) { return nullptr; }
}
VANTIX_EXPORT int vantix_vendor_derive(void* pointer, const unsigned char* input,
                                       unsigned count, unsigned char* output) {
    if (!pointer) return 0;
    auto& context = *static_cast<vantix_vendor::Context*>(pointer);
    try { context.derive(input, count, output); context.error.clear(); return 1; }
    catch (const std::exception& failure) { context.error = failure.what(); return 0; }
}
VANTIX_EXPORT const char* vantix_vendor_error(void* pointer) {
    return pointer ? static_cast<vantix_vendor::Context*>(pointer)->error.c_str()
                   : "No vendor GPU context";
}
VANTIX_EXPORT void vantix_vendor_destroy(void* pointer) {
    delete static_cast<vantix_vendor::Context*>(pointer);
}
