#pragma once

#include <filesystem>

namespace vantix {
std::filesystem::path resource_path(const std::filesystem::path& relative);
}
