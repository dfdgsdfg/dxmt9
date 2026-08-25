#include "util_hash.hpp"

#include "dxmt9/core.hpp"

#include <span>

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

// Public dxmt9::core hashBytes/hashString — declared in include/dxmt9/core.hpp
// but lifted from src/d3d9/core.cpp into the util library so that any layer
// (incl. the ELF winemetal_dxmt9.so unix module's shader-service core) can call
// them without dragging the d3d9 frontend in. The constant differs from
// dxmt9::util::fnv1a64's offset basis (1469... vs 1469... + truncated digit)
// — preserved verbatim from d3d9/core.cpp to keep cached-hash compatibility.
namespace dxmt9::core {

namespace {
constexpr std::uint64_t kFnvOffsetCore = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrimeCore = 1099511628211ull;
}  // namespace

std::uint64_t hashBytes(std::span<const std::byte> bytes) {
  std::uint64_t hash = kFnvOffsetCore;
  for (const auto byte : bytes) {
    hash ^= static_cast<std::uint64_t>(std::to_integer<unsigned char>(byte));
    hash *= kFnvPrimeCore;
  }
  return hash;
}

std::uint64_t hashString(std::string_view text) {
  return hashBytes(std::as_bytes(std::span<const char>(text.data(), text.size())));
}

}  // namespace dxmt9::core
