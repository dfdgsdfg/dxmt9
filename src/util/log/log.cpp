#include "log.hpp"

#include "../config/config.hpp"
#include "../util_string.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace dxmt9::util {

namespace {

std::string executableBaseName() {
#if defined(_WIN32)
  std::array<char, MAX_PATH> path{};
  const DWORD len = GetModuleFileNameA(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (len != 0) {
    return std::filesystem::path(std::string(path.data(), len)).stem().string();
  }
#elif defined(__APPLE__)
  uint32_t size = 0;
  (void)_NSGetExecutablePath(nullptr, &size);
  if (size != 0) {
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) == 0) {
      return std::filesystem::path(path.c_str()).stem().string();
    }
  }
#endif
  return "dxmt9";
}

std::string logFilePath() {
  static const std::string value = [] {
    const auto path = getenvString("DXMT_LOG_PATH");
    if (path.empty() || path == "none") {
      return std::string();
    }
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
      return std::string();
    }
    return (std::filesystem::path(path) / (executableBaseName() + "_dxmt9.log")).string();
  }();
  return value;
}

std::ofstream& logFile() {
  static std::ofstream file = [] {
    const auto path = logFilePath();
    if (path.empty()) {
      return std::ofstream{};
    }
    return std::ofstream(path, std::ios::app);
  }();
  return file;
}

void initializeLogSink() {
  static const bool initialized = [] {
    auto& file = logFile();
    (void)file;
    return true;
  }();
  (void)initialized;
}

std::mutex& logMutex() {
  static std::mutex mutex;
  return mutex;
}

const char* levelPrefix(LogLevel level) {
  switch (level) {
    case LogLevel::Trace:
      return "trace";
    case LogLevel::Debug:
      return "debug";
    case LogLevel::Info:
      return "info";
    case LogLevel::Warn:
      return "warn";
    case LogLevel::Error:
      return "error";
    case LogLevel::None:
      return "none";
  }
  return "info";
}

}  // namespace

LogLevel configuredLogLevel() {
  static const LogLevel level = [] {
    const auto value = str::toLowerAscii(getenvString("DXMT_LOG_LEVEL"));
    if (value == "trace") return LogLevel::Trace;
    if (value == "debug") return LogLevel::Debug;
    if (value == "info" || value.empty()) return LogLevel::Info;
    if (value == "warn") return LogLevel::Warn;
    if (value == "error") return LogLevel::Error;
    if (value == "none") return LogLevel::None;
    return LogLevel::Info;
  }();
  initializeLogSink();
  return level;
}

bool shouldLog(LogLevel level) {
  return level >= configuredLogLevel() && configuredLogLevel() != LogLevel::None;
}

void logLine(LogLevel level, const char* tag, const std::string& line) {
  if (!shouldLog(level)) {
    return;
  }
  std::lock_guard lock(logMutex());
  std::ostringstream out;
  out << '[' << (tag ? tag : "dxmt9") << "] " << levelPrefix(level) << ": " << line << '\n';
  const auto rendered = out.str();
  std::fputs(rendered.c_str(), stderr);
  std::fflush(stderr);
  auto& file = logFile();
  if (file) {
    file << rendered;
    file.flush();
  }
}

void vlogf(LogLevel level, const char* tag, const char* fmt, std::va_list args) {
  if (!shouldLog(level)) {
    return;
  }
  std::array<char, 4096> buffer{};
  const int written = std::vsnprintf(buffer.data(), buffer.size(), fmt, args);
  if (written <= 0) {
    return;
  }
  logLine(level, tag,
          std::string(buffer.data(),
                      static_cast<std::size_t>(std::min<int>(written, static_cast<int>(buffer.size() - 1)))));
}

void logf(LogLevel level, const char* tag, const char* fmt, ...) {
  if (!shouldLog(level)) {
    return;
  }
  std::va_list args;
  va_start(args, fmt);
  vlogf(level, tag, fmt, args);
  va_end(args);
}

}  // namespace dxmt9::util
