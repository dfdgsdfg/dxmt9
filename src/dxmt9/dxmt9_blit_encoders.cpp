#include "dxmt9_blit_encoders.hpp"

#include <algorithm>
#include <cstdint>

namespace dxmt9::encoders {

void encodeReadback(WMT::CommandBuffer& commandBuffer,
                    resources::Pool& pool,
                    const core::ReadbackDesc& readback) {
  auto* src = pool.findSurface(readback.source.value);
  auto* dst = pool.findSurface(readback.destination.value);
  if (!src || !dst || !src->texture) {
    return;
  }
  auto blit = commandBuffer.blitCommandEncoder();
  if (!blit) return;
  WMT::Texture sourceTexture{src->resolveTexture ? src->resolveTexture.handle : src->texture.handle};
  const uint32_t w =
      static_cast<uint32_t>(std::max(1, readback.sourceRect.right - readback.sourceRect.left));
  const uint32_t h =
      static_cast<uint32_t>(std::max(1, readback.sourceRect.bottom - readback.sourceRect.top));
  if (!dst->texture) {
    blit.endEncoding();
    return;
  }
  WMTOrigin srcOrigin{static_cast<uint64_t>(readback.sourceRect.left),
                       static_cast<uint64_t>(readback.sourceRect.top), 0};
  WMTSize srcSize{w, h, 1};
  WMTOrigin dstOrigin{0, 0, 0};
  blit.copyFromTextureToTexture(sourceTexture, 0, readback.sourceLevel,
                                 srcOrigin, srcSize,
                                 WMT::Texture{dst->texture.handle}, 0, 0, dstOrigin);
  blit.endEncoding();
}

}  // namespace dxmt9::encoders
