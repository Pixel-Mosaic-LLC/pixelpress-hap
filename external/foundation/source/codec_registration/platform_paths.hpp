#pragma once

#include <string>

// macOS 11+ (Apple Silicon baseline) has std::filesystem, so Boost is no longer needed
#include <filesystem>
namespace fs = ::std::filesystem;

namespace fdn
{

typedef fs::path PathType;
PathType getConfigurationPath();

}
