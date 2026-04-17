#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dxmt9::util {

using u64 = std::uint64_t;

u64 fnv1a64(const void* data, std::size_t size);
u64 fnv1a64(std::string_view text);

}  // namespace dxmt9::util
