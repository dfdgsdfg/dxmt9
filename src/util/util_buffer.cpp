#include "util_buffer.hpp"

#include <algorithm>
#include <cstring>

namespace dxmt9::util {

u64 copyBytesToBuffer(const void* source, u64 sourceSize, void* buffer, u64 bufferCapacity) {
  if (!buffer || bufferCapacity == 0) {
    return 0;
  }

  const size_t bytesToCopy = std::min<size_t>(static_cast<size_t>(sourceSize),
                                              static_cast<size_t>(bufferCapacity));
  if (bytesToCopy != 0) {
    std::memcpy(buffer, source, bytesToCopy);
  }
  return static_cast<u64>(bytesToCopy);
}

u64 copyStringToBuffer(std::string_view source, char* buffer, u64 bufferCapacity) {
  if (!buffer || bufferCapacity == 0) {
    return 0;
  }
  const u64 copied = copyBytesToBuffer(source.data(), static_cast<u64>(source.size()), buffer,
                                       bufferCapacity - 1u);
  buffer[copied] = '\0';
  return copied;
}

}  // namespace dxmt9::util
