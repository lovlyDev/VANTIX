#include "vantix/resources.hpp"

#include <stdexcept>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace vantix {
std::filesystem::path resource_path(const std::filesystem::path& relative) {
    std::filesystem::path executable;
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const auto count = GetModuleFileNameW(nullptr, buffer.data(),
                                          static_cast<DWORD>(buffer.size()));
    if (count == 0 || count >= buffer.size())
        throw std::runtime_error("Cannot locate VANTIX executable");
    buffer.resize(count);
    executable = std::filesystem::path(buffer);
#else
    executable = std::filesystem::read_symlink("/proc/self/exe");
#endif
    const auto installed = executable.parent_path() / relative;
    if (std::filesystem::is_regular_file(installed)) return installed;
    if (const auto* bundle = std::getenv("VANTIX_RESOURCE_DIR")) {
        const auto bundled = std::filesystem::path(bundle) / relative;
        if (std::filesystem::is_regular_file(bundled)) return bundled;
    }
#ifdef VANTIX_SOURCE_RESOURCE_ROOT
    const auto development = std::filesystem::path(VANTIX_SOURCE_RESOURCE_ROOT) / relative;
    if (std::filesystem::is_regular_file(development)) return development;
#endif
    throw std::runtime_error("VANTIX resource is missing: " + relative.string());
}
}
