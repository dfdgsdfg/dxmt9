#pragma once

#include "dxmt9/device_c.h"
#include "util_buffer.hpp"

#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace dxmt9::util::marshal {

template <typename T>
inline T* decodeOpaque(void* opaque) {
  return static_cast<T*>(opaque);
}

template <typename T>
inline T* decodePtr(u64 value) {
  return u64ToPtr<T>(value);
}

template <typename T>
inline u64 encodePtr(T* value) {
  return ptrToU64(value);
}

template <typename T>
inline T* decodeNullablePtr(u64 value) {
  return value == 0 ? nullptr : decodePtr<T>(value);
}

bool hasEncodedBuffer(u64 bufferPtr, u64 bufferCapacity);
char* decodeCharBuffer(u64 bufferPtr, u64 bufferCapacity);
const char* decodeConstCharBuffer(u64 bufferPtr, u64 bufferCapacity);

template <typename CharT>
inline u64 copyStringToEncodedBuffer(std::basic_string_view<CharT> source,
                                     u64 bufferPtr,
                                     u64 bufferCapacity) {
  return copyStringToBuffer(source, decodePtr<char>(bufferPtr), bufferCapacity);
}

namespace wow64 {

struct HandleRegistry {
  std::mutex mutex;
  uint32_t next = 1;
  std::unordered_map<uint32_t, uintptr_t> values;
};

inline HandleRegistry& registry() {
  static HandleRegistry value;
  return value;
}

template <typename T>
inline T decodeHandle(uint32_t token) {
  if (!token) {
    return nullptr;
  }
  auto& reg = registry();
  std::lock_guard<std::mutex> lock(reg.mutex);
  const auto it = reg.values.find(token);
  if (it == reg.values.end()) {
    return nullptr;
  }
  return reinterpret_cast<T>(it->second);
}

template <typename T>
inline uint32_t encodeHandle(T value) {
  if (!value) {
    return 0;
  }
  auto& reg = registry();
  std::lock_guard<std::mutex> lock(reg.mutex);
  uint32_t token = reg.next++;
  if (!token) {
    token = reg.next++;
  }
  reg.values[token] = reinterpret_cast<uintptr_t>(value);
  return token;
}

inline void eraseHandle(uint32_t token) {
  if (!token) {
    return;
  }
  auto& reg = registry();
  std::lock_guard<std::mutex> lock(reg.mutex);
  reg.values.erase(token);
}

template <typename T>
inline T decodePtr(uint32_t value) {
  return reinterpret_cast<T>(static_cast<uintptr_t>(value));
}

template <typename T>
inline uint32_t encodePtr(T value) {
  return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(value));
}

struct LockedRect32 {
  int32_t pitch;
  uint32_t bits;
};

inline void storeLockedRect(LockedRect32* out, const D9CLockedRect& native) {
  if (!out) {
    return;
  }
  out->pitch = native.pitch;
  out->bits = encodePtr(native.bits);
}

inline void storeEncodedPointer(uint32_t out, const void* native) {
  if (!out) {
    return;
  }
  *decodePtr<uint32_t*>(out) = encodePtr(native);
}

}  // namespace wow64

}  // namespace dxmt9::util::marshal
