#include "dxmt9_draw_encoder.hpp"
#include "dxmt9_draw_encoder_internal.hpp"

namespace dxmt9::encoders {

HazardProbe makeAttachmentHazard(const core::FlatDrawStateRecord& hot) {
  HazardProbe hazard;
  for (const auto& attachment : hot.colorAttachments) hazard.add(attachment.handle.value);
  hazard.add(hot.depthStencil.handle.value);
  return hazard;
}

HazardProbe makeAttachmentHazard(const core::ClearDesc& clear) {
  HazardProbe hazard;
  if (clear.clearColor) {
    for (const auto& attachment : clear.colorAttachments) hazard.add(attachment.handle.value);
  }
  if (clear.clearDepth || clear.clearStencil) {
    hazard.add(clear.depthStencil.handle.value);
  }
  return hazard;
}

HazardProbe makeDrawReadHazard(core::FlatDrawStateView state) {
  HazardProbe hazard;
  const auto& hot = *state.hot;
  hazard.add(hot.indexBuffer.value);
  for (const auto& handle : hot.streamBuffers) {
    hazard.add(handle.value);
  }
  for (const auto& texture : hot.textures) hazard.add(texture.value);
  return hazard;
}

}  // namespace dxmt9::encoders
