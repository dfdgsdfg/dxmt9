#include "traditional_backend.hpp"

namespace dxmt9::render {

std::optional<core::metalqueue::QueueSubmissionRecord>
TraditionalBackend::onChunkReady(encoders::EncodeContext& ctx,
                                 std::size_t slotIndex,
                                 const core::ChunkSlot& slot) {
  // Backend-agnostic DAG observe + export side-channel (R-BACK-39.7). Reads only
  // `slot`, writes only debug dump files when DXMT9_RENDERER_DUMP_DAG is set, and
  // early-outs otherwise — so the traditional encode below stays byte-identical.
  observer_.observeAndExport(slot);

  // Byte-identical traditional path: forward straight to the free function.
  return encoders::encodeChunk(ctx, slotIndex, slot);
}

bool TraditionalBackend::emitDraw(encoders::EncodeContext& ctx,
                                  WMT::CommandBuffer& commandBuffer,
                                  WMT::RenderCommandEncoder& encoder,
                                  core::FlatDrawStateView drawState,
                                  std::uint64_t seqId,
                                  const core::DrawParam& param) {
  // The exposed 6 params map onto encodeDraw's leading subset; `param` is the
  // `paramOverride` pointer arg. All trailing encode knobs keep their defaults.
  return encoders::encodeDraw(ctx, commandBuffer, encoder, drawState, seqId,
                              /*skipBaseStateBind=*/false,
                              /*preUploaded=*/nullptr,
                              /*paramOverride=*/&param);
}

void TraditionalBackend::emitClearWithinPass(encoders::EncodeContext& ctx,
                                             WMT::CommandBuffer& commandBuffer,
                                             const core::ClearDesc& clear) {
  // encodeClearPass opens its own LoadActionClear render pass; the pool comes
  // from the encode context. Trailing sampleBufferAttachments span defaults.
  encoders::encodeClearPass(commandBuffer, ctx.pool, clear);
}

}  // namespace dxmt9::render
