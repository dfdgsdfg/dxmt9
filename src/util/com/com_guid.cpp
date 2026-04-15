#include "com_guid.hpp"

#include <array>
#include <cstdio>

namespace dxmt9::util {

std::size_t GuidHash::operator()(REFGUID guid) const noexcept {
  const auto* words = reinterpret_cast<const std::uint64_t*>(&guid);
  return static_cast<std::size_t>(words[0] ^ words[1]);
}

bool guidEqual(REFGUID a, REFGUID b) noexcept {
  return InlineIsEqualGUID(a, b) != FALSE;
}

std::string formatGuid(REFGUID guid) {
  std::array<char, 40> buffer{};
  std::snprintf(buffer.data(), buffer.size(),
                "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
                static_cast<unsigned long>(guid.Data1),
                guid.Data2,
                guid.Data3,
                guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
                guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
  return std::string(buffer.data());
}

}  // namespace dxmt9::util
