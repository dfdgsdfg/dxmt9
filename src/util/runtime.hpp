#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace dxmt9::runtime {

enum class LogLevel : std::uint32_t {
  Trace = 0,
  Debug = 1,
  Info = 2,
  Warn = 3,
  Error = 4,
  None = 5,
};

std::string getenvString(const char* name);
bool getenvFlag(const char* name);
std::optional<std::uint32_t> getenvU32(const char* name);
std::optional<std::uint32_t> getenvU32Auto(const char* name);
std::optional<std::uint64_t> getenvU64(const char* name);
std::optional<std::uint64_t> getenvU64Auto(const char* name);

LogLevel configuredLogLevel();
bool shouldLog(LogLevel level);
void logLine(LogLevel level, const char* tag, const std::string& line);
void logf(LogLevel level, const char* tag, const char* fmt, ...);

}  // namespace dxmt9::runtime
