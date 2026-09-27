#include "vantix/ton.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>

#define __device__
#define __constant__
#define __forceinline__ inline
#define __global__
#define VANTIX_VENDOR_MOCK 1
struct ThreadCoordinate { unsigned x; };
ThreadCoordinate blockIdx{0}, blockDim{64}, threadIdx{0};

int mockSetDevice(int index) { return index == 0 ? 0 : 1; }
int mockGetDeviceCount(int* count) { *count = 1; return 0; }
int mockDeviceGetName(char* name, int size, int index) {
    if (index != 0 || size < 9) return 1;
    std::memcpy(name, "Mock GPU", 9);
    return 0;
}
int mockMalloc(void** pointer, std::size_t size) {
    *pointer = std::malloc(size);
    return *pointer ? 0 : 1;
}
int mockFree(void* pointer) { std::free(pointer); return 0; }
int mockMemcpyAsync(void* destination, const void* source, std::size_t size,
                    int, void*) {
    std::memcpy(destination, source, size);
    return 0;
}
int mockStreamCreateWithFlags(void** stream, unsigned) {
    *stream = std::malloc(1);
    return *stream ? 0 : 1;
}
int mockStreamSynchronize(void*) { return 0; }
int mockStreamDestroy(void* stream) { std::free(stream); return 0; }
int mockGetLastError() { return 0; }
const char* mockGetErrorString(int) { return "mock GPU failure"; }
int mockMemset(void* pointer, int value, std::size_t size) {
    std::memset(pointer, value, size);
    return 0;
}

#include "../gpu/vendor/backend_impl.hpp"

int main() {
    constexpr char phrase[] =
        "abandon abandon abandon abandon abandon abandon abandon abandon "
        "abandon abandon abandon about";
    if (vantix_vendor_abi_version() != 1 || vantix_vendor_device_count() != 1)
        return 1;
    char name[64]{}, id[64]{};
    if (!vantix_vendor_device_name(0, name, sizeof(name)) ||
        !vantix_vendor_device_id(0, id, sizeof(id)) ||
        std::strcmp(name, "Mock GPU") != 0 || std::strcmp(id, "mock:0") != 0)
        return 2;
    auto* context = vantix_vendor_create(0);
    if (!context) return 3;
    std::array<unsigned char, 128> input{};
    std::array<unsigned char, 32> output{};
    std::memcpy(input.data(), phrase, sizeof(phrase) - 1);
    const auto valid = vantix_vendor_derive(context, input.data(), 1, output.data());
    vantix::Bytes32 seed{};
    std::memcpy(seed.data(), output.data(), seed.size());
    const auto matches = vantix::public_key_from_ed25519_seed(seed) ==
                         vantix::multichain_public_key(phrase);
    const auto invalid = vantix_vendor_derive(context, input.data(), 0, output.data());
    const auto error_set = std::strlen(vantix_vendor_error(context)) > 0;
    vantix_vendor_destroy(context);
    if (!valid || !matches || invalid || !error_set) {
        std::cerr << "Vendor module ABI or derivation failed\n";
        return 4;
    }
    return 0;
}
