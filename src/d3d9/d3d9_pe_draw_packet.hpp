#pragma once

#include "dxmt9/device_c.h"

#include <array>
#include <cstdint>

namespace dxmt9::d3d9::pe {

using PeRtWireHandles =
    std::array<D9CWireHandle, D9C_DRAW_PACKET_MAX_RENDER_TARGETS>;
using PeRtExplicitMask =
    std::array<bool, D9C_DRAW_PACKET_MAX_RENDER_TARGETS>;
using PeStreamSources =
    std::array<D9CDrawPacketStreamSource, D9C_DRAW_PACKET_MAX_STREAMS>;

inline bool wireHandleIsNull(const D9CWireHandle& handle) noexcept {
  return handle.lo == 0 && handle.hi == 0;
}

inline std::uint32_t renderTargetPacketMask(std::uint32_t mask) noexcept {
  return mask & ((1u << D9C_DRAW_PACKET_MAX_RENDER_TARGETS) - 1u);
}

inline void populateDrawPacketStreamDependencies(
    D9CDrawPrimitivePacket& packet,
    const PeStreamSources& boundStreams,
    std::uint32_t retainedStreamMask) noexcept {
  for (std::uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_STREAMS; ++slot) {
    const std::uint32_t bit = 1u << slot;
    if ((packet.streamSourceMask & bit) != 0u ||
        (retainedStreamMask & bit) != 0u ||
        wireHandleIsNull(boundStreams[slot].buffer)) {
      continue;
    }
    packet.streamSourceMask |= bit;
    packet.streamSources[slot] = boundStreams[slot];
  }
}

inline void populateDrawPacketIndexDependency(
    D9CDrawIndexedPrimitivePacket& packet,
    bool retained) noexcept {
  if (!retained && !wireHandleIsNull(packet.ibHandle)) {
    packet.ibValid = 1u;
  }
}

inline void populateDrawPacketAttachmentDelta(
    D9CDrawPrimitivePacket& packet,
    std::uint32_t pendingRtMask,
    const PeRtWireHandles& rtHandles,
    bool pendingDepthStencil,
    D9CWireHandle depthStencilHandle) noexcept {
  packet.rtMask = renderTargetPacketMask(pendingRtMask);
  for (std::uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_RENDER_TARGETS;
       ++slot) {
    packet.rtHandles[slot] =
        (packet.rtMask & (1u << slot)) != 0 ? rtHandles[slot]
                                            : D9CWireHandle{};
  }

  packet.dsValid = pendingDepthStencil ? 1u : 0u;
  packet.dsHandle = pendingDepthStencil ? depthStencilHandle : D9CWireHandle{};
}

inline void populateDrawPacketAttachmentSnapshot(
    D9CDrawPrimitivePacket& packet,
    const PeRtWireHandles& rtHandles,
    const PeRtExplicitMask& rtExplicit,
    bool depthStencilValid,
    D9CWireHandle depthStencilHandle) noexcept {
  packet.rtMask = 0;
  for (std::uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_RENDER_TARGETS;
       ++slot) {
    if (rtExplicit[slot] || !wireHandleIsNull(rtHandles[slot])) {
      packet.rtMask |= 1u << slot;
      packet.rtHandles[slot] = rtHandles[slot];
    } else {
      packet.rtHandles[slot] = D9CWireHandle{};
    }
  }

  packet.dsValid = depthStencilValid ? 1u : 0u;
  packet.dsHandle = depthStencilValid ? depthStencilHandle : D9CWireHandle{};
}

}  // namespace dxmt9::d3d9::pe
