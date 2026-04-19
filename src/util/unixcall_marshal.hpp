#pragma once

#include "util_buffer.hpp"

namespace dxmt9::util::marshal {

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

}  // namespace dxmt9::util::marshal
