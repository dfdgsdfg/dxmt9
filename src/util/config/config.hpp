#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace dxmt9::util {

std::string getenvString(const char* name);
bool getenvFlag(const char* name);
std::optional<std::uint32_t> getenvU32(const char* name);
std::optional<std::uint32_t> getenvU32Auto(const char* name);
std::optional<std::uint64_t> getenvU64(const char* name);
std::optional<std::uint64_t> getenvU64Auto(const char* name);

}  // namespace dxmt9::util
