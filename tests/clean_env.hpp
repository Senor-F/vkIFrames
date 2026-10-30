#pragma once
// The layer reads VKIFRAME_* (and the old FRAMEGEN_*) from the environment; a variable exported in the developer's shell (VKIFRAME_TARGET_FPS=120, say)
// must not change what the mock tests measure. Call once at the start of main, before the layer is loaded.
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
extern "C" char** environ;
inline void ClearFramegenEnv() {
    std::vector<std::string> names;
    for (char** e = environ; e && *e; ++e)
        if (std::strncmp(*e, "VKIFRAME_", 9) == 0 || std::strncmp(*e, "FRAMEGEN_", 9) == 0) names.emplace_back(*e, std::strchr(*e, '=') ? size_t(std::strchr(*e, '=') - *e) : std::strlen(*e));
    for (const auto& n : names) unsetenv(n.c_str());
}
