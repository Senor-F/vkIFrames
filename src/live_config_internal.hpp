// Shared by live_config.cpp and live_config_io.cpp: config folder paths, safe file reading, the live log line
#pragma once
#include <cstddef>
#include <string>

namespace liveconf {

constexpr size_t kMaxSettingsBytes = 65536;
std::string ConfDir();                              // empty when neither XDG_CONFIG_HOME nor HOME is set
std::string ConfPath();
bool ReadOwnFile(const std::string& path, std::string& text, bool* truncated = nullptr);
void LogLine(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace liveconf
