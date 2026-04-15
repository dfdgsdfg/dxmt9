#include "util_env.hpp"

#include <cstdlib>
#include <cstring>

namespace dxmt9::util {

std::string getenvString(const char* name) {
  const char* value = std::getenv(name);
  return value ? std::string(value) : std::string();
}

bool getenvFlag(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

std::optional<std::uint32_t> getenvU32(const char* name) {
  const char* value = std::getenv(name);
  if (!value || value[0] == '\0') {
    return std::nullopt;
  }
  char* end = nullptr;
  const auto parsed = std::strtoul(value, &end, 10);
  if (!end || *end != '\0') {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(parsed);
}

std::optional<std::uint32_t> getenvU32Auto(const char* name) {
  const char* value = std::getenv(name);
  if (!value || value[0] == '\0') {
    return std::nullopt;
  }
  char* end = nullptr;
  const auto parsed = std::strtoull(value, &end, 0);
  if (!end || *end != '\0') {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(parsed);
}

std::optional<std::uint64_t> getenvU64(const char* name) {
  const char* value = std::getenv(name);
  if (!value || value[0] == '\0') {
    return std::nullopt;
  }
  char* end = nullptr;
  const auto parsed = std::strtoull(value, &end, 10);
  if (!end || *end != '\0') {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(parsed);
}

std::optional<std::uint64_t> getenvU64Auto(const char* name) {
  const char* value = std::getenv(name);
  if (!value || value[0] == '\0') {
    return std::nullopt;
  }
  char* end = nullptr;
  const auto parsed = std::strtoull(value, &end, 0);
  if (!end || *end != '\0') {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(parsed);
}

}  // namespace dxmt9::util
