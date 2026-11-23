// Linux (test builds of pphap only; the plugin itself is Windows / macOS): ~/.config/<codec name>
#include <cstdlib>

#include "platform_paths.hpp"

namespace fdn {

PathType getConfigurationPath()
{
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    PathType base = xdg && *xdg ? PathType(xdg) : PathType(home && *home ? home : "/tmp") / ".config";
    return base / FOUNDATION_CODEC_NAME;
}

} // namespace fdn
