#pragma once

#include <cstdint>
#include <string_view>

namespace dxmt9::util {

using u64 = std::uint64_t;

template <typename T>
inline u64 ptrToU64(T* ptr) {
  return static_cast<u64>(reinterpret_cast<std::uintptr_t>(ptr));
}

template <typename T>
inline T* u64ToPtr(u64 value) {
  return reinterpret_cast<T*>(static_cast<std::uintptr_t>(value));
}

u64 copyBytesToBuffer(const void* source, u64 sourceSize, void* buffer, u64 bufferCapacity);
u64 copyStringToBuffer(std::string_view source, char* buffer, u64 bufferCapacity);

}  // namespace dxmt9::util
