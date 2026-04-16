#include "util_buffer.hpp"

#include <algorithm>
#include <cstring>

namespace dxmt9::util {

u64 copyStringToBuffer(std::string_view source, char* buffer, u64 bufferCapacity) {
  if (!buffer || bufferCapacity == 0) {
    return 0;
  }

  const size_t bytesToCopy =
      std::min<size_t>(source.size(), static_cast<size_t>(bufferCapacity - 1u));
  if (bytesToCopy != 0) {
    std::memcpy(buffer, source.data(), bytesToCopy);
  }
  buffer[bytesToCopy] = '\0';
  return static_cast<u64>(bytesToCopy);
}

}  // namespace dxmt9::util
