#pragma once

#include <cstdint>

namespace dxmt9::d3d9::pe {

inline bool bufferLockRequiresHazardFlush(std::uint32_t flags) noexcept {
  constexpr std::uint32_t kReadOnly = 0x00000010u;
  constexpr std::uint32_t kNoOverwrite = 0x00001000u;
  constexpr std::uint32_t kDiscard = 0x00002000u;
  if ((flags & kReadOnly) != 0u) {
    return false;
  }
  // DISCARD can bypass the unix replay wait only after this PE hazard flush
  // seals the raw chunk that names the pre-rename backing generation.
  if ((flags & kDiscard) != 0u) {
    return true;
  }
  if ((flags & kNoOverwrite) != 0u) {
    return false;
  }
  // Plain writable and MANAGED writable locks seal before unlock can rotate
  // and publish a new backing.
  return true;
}

template <typename Result, typename Seal>
Result sealBufferGenerationBeforeLock(bool recorderAvailable,
                                      std::uint32_t flags,
                                      Result noSealResult,
                                      Seal seal) {
  if (!recorderAvailable || !bufferLockRequiresHazardFlush(flags)) {
    return noSealResult;
  }
  return seal();
}

}  // namespace dxmt9::d3d9::pe
