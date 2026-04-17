#include "util_hash.hpp"

namespace dxmt9::util {

namespace {

constexpr u64 kFnvOffsetBasis64 = 14695981039346656037ull;
constexpr u64 kFnvPrime64 = 1099511628211ull;

}  // namespace

u64 fnv1a64(const void* data, std::size_t size) {
  if (!data || size == 0) {
    return kFnvOffsetBasis64;
  }

  const auto* bytes = static_cast<const unsigned char*>(data);
  u64 hash = kFnvOffsetBasis64;
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= static_cast<u64>(bytes[i]);
    hash *= kFnvPrime64;
  }
  return hash;
}

u64 fnv1a64(std::string_view text) {
  return fnv1a64(text.data(), text.size());
}

}  // namespace dxmt9::util
