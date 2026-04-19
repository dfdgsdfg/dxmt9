#include "unixcall_marshal.hpp"

namespace dxmt9::util::marshal {

bool hasEncodedBuffer(u64 bufferPtr, u64 bufferCapacity) {
  return bufferPtr != 0 && bufferCapacity != 0;
}

char* decodeCharBuffer(u64 bufferPtr, u64 bufferCapacity) {
  return hasEncodedBuffer(bufferPtr, bufferCapacity) ? decodePtr<char>(bufferPtr) : nullptr;
}

const char* decodeConstCharBuffer(u64 bufferPtr, u64 bufferCapacity) {
  return hasEncodedBuffer(bufferPtr, bufferCapacity) ? decodePtr<const char>(bufferPtr) : nullptr;
}

}  // namespace dxmt9::util::marshal
