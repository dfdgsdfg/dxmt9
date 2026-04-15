#pragma once

#include <cstdarg>
#include <cstdint>
#include <string>

namespace dxmt9::util {

enum class LogLevel : std::uint32_t {
  Trace = 0,
  Debug = 1,
  Info = 2,
  Warn = 3,
  Error = 4,
  None = 5,
};

LogLevel configuredLogLevel();
bool shouldLog(LogLevel level);
void logLine(LogLevel level, const char* tag, const std::string& line);
void vlogf(LogLevel level, const char* tag, const char* fmt, std::va_list args);
void logf(LogLevel level, const char* tag, const char* fmt, ...);

}  // namespace dxmt9::util
