#pragma once

#include <cstdint>

namespace dxmt9 {

// Who owns attachment writes is independent of whether their pixels can be
// read back. CPU replay qualification records backend commands without a GPU.
enum class AttachmentContentOwner : std::uint8_t { CpuShadow, BackendCommands };

constexpr bool backendAttachmentContents(AttachmentContentOwner owner,
                                         bool attachment) noexcept {
  return attachment && owner == AttachmentContentOwner::BackendCommands;
}

enum class AttachmentReadRoute : std::uint8_t { CpuShadow, GpuReadback, Unavailable };

constexpr AttachmentReadRoute attachmentReadRoute(
    AttachmentContentOwner owner, bool attachment, bool gpuReadback) noexcept {
  if (!backendAttachmentContents(owner, attachment))
    return AttachmentReadRoute::CpuShadow;
  return gpuReadback ? AttachmentReadRoute::GpuReadback : AttachmentReadRoute::Unavailable;
}

}  // namespace dxmt9
