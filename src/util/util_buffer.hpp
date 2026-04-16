#pragma once

#include <cstdint>
#include <string_view>

namespace dxmt9::util {

using u64 = std::uint64_t;

u64 copyStringToBuffer(std::string_view source, char* buffer, u64 bufferCapacity);

}  // namespace dxmt9::util
