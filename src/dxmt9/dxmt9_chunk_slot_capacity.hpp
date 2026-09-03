#pragma once

#include "dxmt9_backend_types.hpp"

#include <cstddef>
#include <limits>

namespace dxmt9::core {

// Physical bytes retained by one persistent compatibility payload. This is the
// complete payload footprint, including exact-fit-only dimensions such as
// readback records; lookup arrays are real retained storage even when their
// logical heads are empty. Provisioning price remains deliberately separate
// and fail-closed for readback because the direct branch has no readback
// appender.
inline std::size_t chunkSlotPhysicalRetainedBytes(
    const ChunkSlot& slot) noexcept {
  const auto bytes = [](std::size_t count, std::size_t element) noexcept {
    return count > std::numeric_limits<std::size_t>::max() / element
        ? std::numeric_limits<std::size_t>::max()
        : count * element;
  };
  std::size_t total = 0;
  const auto add = [&](std::size_t value) noexcept {
    total = value > std::numeric_limits<std::size_t>::max() - total
        ? std::numeric_limits<std::size_t>::max()
        : total + value;
  };
  add(bytes(slot.commandHeaders.capacity(), sizeof(MetalCommandHeader)));
  add(bytes(slot.drawHotStates.capacity(), sizeof(FlatDrawStateRecord)));
  add(bytes(slot.drawShaderLayouts.capacity(), sizeof(DrawShaderLayoutContext)));
  add(slot.detachedResourceOwnerRetainedBytes);
  add(bytes(slot.drawDebugSnapshots.capacity(), sizeof(DrawDebugSnapshot)));
  add(bytes(slot.drawPsoSubviews.capacity(), sizeof(DrawPsoSubview)));
  add(bytes(slot.drawUniformFixedPayloads.capacity(),
            sizeof(DrawUniformFixedPayloadRecord)));
  add(bytes(slot.drawUniformVertexConstants.capacity(),
            sizeof(DrawUniformVertexConstantsRecord)));
  add(slot.drawUniformVertexConstantBytes.capacity());
  add(bytes(slot.drawUniformPixelConstants.capacity(),
            sizeof(DrawUniformPixelConstantsRecord)));
  add(slot.drawUniformPixelConstantBytes.capacity());
  add(bytes(slot.drawUniformPayloads.capacity(),
            sizeof(DrawUniformPayloadRecord)));
  add(bytes(slot.drawUniformPayloadLookupHeads.capacity(), sizeof(std::uint32_t)));
  add(bytes(slot.drawUniformPayloadLookupTails.capacity(), sizeof(std::uint32_t)));
  add(bytes(slot.drawUniformPayloadLookupNext.capacity(), sizeof(std::uint32_t)));
  add(bytes(slot.drawUniformVertexConstantsLookupHeads.capacity(), sizeof(std::uint32_t)));
  add(bytes(slot.drawUniformVertexConstantsLookupTails.capacity(), sizeof(std::uint32_t)));
  add(bytes(slot.drawUniformVertexConstantsLookupNext.capacity(), sizeof(std::uint32_t)));
  add(bytes(slot.drawUniformPixelConstantsLookupHeads.capacity(), sizeof(std::uint32_t)));
  add(bytes(slot.drawUniformPixelConstantsLookupTails.capacity(), sizeof(std::uint32_t)));
  add(bytes(slot.drawUniformPixelConstantsLookupNext.capacity(), sizeof(std::uint32_t)));
  add(bytes(slot.drawParams.capacity(), sizeof(DrawParam)));
  add(slot.drawPayloadArena.capacity());
  add(bytes(slot.drawRunRecords.capacity(), sizeof(DrawRunCommandRecord)));
  add(bytes(slot.clearRecords.capacity(), sizeof(ClearDesc)));
  add(bytes(slot.surfaceCopyRecords.capacity(), sizeof(SurfaceCopyDesc)));
  add(bytes(slot.stretchRectRecords.capacity(), sizeof(StretchRectDesc)));
  add(bytes(slot.colorFillRecords.capacity(), sizeof(ColorFillDesc)));
  add(bytes(slot.depthResolveRecords.capacity(), sizeof(DepthResolveDesc)));
  add(bytes(slot.generateMipmapsRecords.capacity(), sizeof(GenerateMipmapsDesc)));
  add(bytes(slot.readbackRecords.capacity(), sizeof(ReadbackDesc)));
  add(bytes(slot.presentRecords.capacity(), sizeof(PresentCommandRecord)));
  return total;
}

}  // namespace dxmt9::core
