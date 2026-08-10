#pragma once

// Private mutable storage owned by EncodeChunkSessionState. The public session
// facade remains the sole creator/deleter; draw encoding borrows this complete
// definition only while synchronously operating on the coordinator-owned
// session.

#include "dxmt9_encode_session_internal.hpp"

#include "dxmt9_capture.hpp"
#include "dxmt9_draw_encoder_diagnostics.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace dxmt9::encoders::encode_session {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

struct AttachmentKey {
  std::array<u64, core::kMaxRenderTargets> colorHandles{};
  u64 depthHandle = 0;
  u32 sampleCount = 1;
  friend bool operator==(const AttachmentKey&, const AttachmentKey&) = default;
};

struct ArgbufPayloadDeltaKey {
  u64 hash = 0;
  u64 vertexConstantsHash = 0;
  u64 pixelConstantsHash = 0;
};

struct ArgbufPayloadDeltaComponentKey {
  u64 vsFloatHash = 0;
  u64 vsIntHash = 0;
  u64 vsBoolHash = 0;
  u64 psFloatHash = 0;
  u64 psIntHash = 0;
  u64 psBoolHash = 0;
};

struct StreamIbStagingCache {
  struct Entry {
    u64 sourceHandle = 0;
    WMT::Buffer buffer{};
    u64 offset = 0;
    std::size_t size = 0;
  };

  static constexpr std::size_t kMaxEntries = 512;

  bool enabled = false;
  std::array<Entry, kMaxEntries> entries{};
  std::size_t count = 0;

  void begin(bool active) noexcept {
    enabled = active;
    entries = {};
    count = 0;
  }

  CommandQueue::TransientBufferSlice findOrStage(
      EncodeContext& ctx,
      u64 seqId,
      u64 sourceHandle,
      const resources::BufferRecord* record,
      ActiveEncoderBreakdown* encoderBreakdown,
      bool indexBuffer) {
    if (!enabled || sourceHandle == 0 || !record) {
      return {};
    }
    for (std::size_t i = 0; i < count; ++i) {
      const auto& entry = entries[i];
      if (entry.sourceHandle == sourceHandle && entry.buffer) {
        return CommandQueue::TransientBufferSlice{
            .buffer = entry.buffer,
            .offset = entry.offset,
            .size = entry.size,
        };
      }
    }
    if (count >= entries.size()) {
      return {};
    }

    std::span<const u8> sourceBytes;
    if (!record->shadow.empty()) {
      sourceBytes = record->shadow;
    } else if (record->contents && record->desc.size > 0) {
      sourceBytes = std::span<const u8>(
          static_cast<const u8*>(record->contents),
          static_cast<std::size_t>(record->desc.size));
    }
    if (sourceBytes.empty()) {
      return {};
    }

    auto slice = ctx.queue.uploadTransientBuffer(
        std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(sourceBytes.data()),
            sourceBytes.size()),
        16,
        seqId);
    if (!slice) {
      return {};
    }
    entries[count++] = Entry{
        .sourceHandle = sourceHandle,
        .buffer = slice.buffer,
        .offset = slice.offset,
        .size = slice.size,
    };
    if (encoderBreakdown) {
      if (indexBuffer) {
        encoderBreakdown->addTransientIndexBytes(
            static_cast<u64>(slice.size),
            ActiveEncoderBreakdown::TransientIndexSource::StagedIb);
      } else {
        encoderBreakdown->addTransientVertexBytes(
            static_cast<u64>(slice.size),
            ActiveEncoderBreakdown::TransientVertexSource::StagedStream);
      }
    }
    return slice;
  }
};

struct ArgbufCbufCache {
  bool valid = false;
  u64 payloadHash = 0;
  dxmt9::argbuf_hybrid::ConstantBufferBindings bindings{};
  bool ffpVsValid = false;
  std::array<std::byte, sizeof(state::FfpVsConsts)> ffpVsBytes{};

  void reset() noexcept {
    valid = false;
    payloadHash = 0;
    bindings = {};
    ffpVsValid = false;
    ffpVsBytes = {};
  }

  bool matches(u64 hash) const noexcept {
    return valid && payloadHash == hash && bindings.complete();
  }

  bool hasBinding(u32 argbufIndex) const noexcept {
    return argbufIndex < bindings.entries.size() &&
           static_cast<bool>(bindings.entries[argbufIndex]);
  }

  dxmt9::argbuf_hybrid::ConstantBufferBinding binding(
      u32 argbufIndex) const noexcept {
    return argbufIndex < bindings.entries.size()
               ? bindings.entries[argbufIndex]
               : dxmt9::argbuf_hybrid::ConstantBufferBinding{};
  }

  bool hasMatchingBinding(u32 argbufIndex,
                          u64 contentHash,
                          u64 bytes) const noexcept {
    return argbufIndex < bindings.entries.size() &&
           bindings.entries[argbufIndex].contentMatches(contentHash, bytes);
  }

  bool hasMatchingIdentity(u32 argbufIndex,
                           u64 identityHash,
                           u64 bytes) const noexcept {
    return argbufIndex < bindings.entries.size() &&
           bindings.entries[argbufIndex].identityMatches(identityHash, bytes);
  }

  bool hasMatchingFfpVs(const state::FfpVsConsts& host) const noexcept {
    const auto& binding =
        bindings.entries[dxmt9::argbuf_hybrid::kConstantBufferFfpVsIndex];
    return ffpVsValid && binding &&
           std::memcmp(ffpVsBytes.data(), &host, sizeof(state::FfpVsConsts)) ==
               0;
  }

  dxmt9::argbuf_hybrid::ConstantBufferBinding ffpVsBinding() const noexcept {
    return bindings.entries[
        dxmt9::argbuf_hybrid::kConstantBufferFfpVsIndex];
  }

  void merge(
      u64 hash,
      const dxmt9::argbuf_hybrid::ConstantBufferBindings& written) noexcept {
    for (std::size_t i = 0; i < bindings.entries.size(); ++i) {
      if (written.entries[i]) {
        bindings.entries[i] = written.entries[i];
      }
    }
    if (bindings.complete()) {
      payloadHash = hash;
      valid = true;
    } else {
      valid = false;
    }
  }

  void promotePayloadHash(u64 hash) noexcept {
    if (bindings.complete()) {
      payloadHash = hash;
      valid = true;
    }
  }

  void storeFfpVs(u64 hash,
                  const state::FfpVsConsts& host,
                  WMT::Buffer buffer,
                  u64 offset,
                  u64 bytes) noexcept {
    bindings.entries[dxmt9::argbuf_hybrid::kConstantBufferFfpVsIndex] =
        dxmt9::argbuf_hybrid::ConstantBufferBinding{
            .buffer = buffer,
            .offset = offset,
            .bytes = bytes,
            .contentHash = dxmt9::argbuf_hybrid::hashConstantBufferBytes(
                &host, bytes),
            .identityHash = 0,
        };
    std::memcpy(ffpVsBytes.data(), &host, sizeof(state::FfpVsConsts));
    ffpVsValid = true;
    if (bindings.complete()) {
      payloadHash = hash;
      valid = true;
    } else {
      valid = false;
    }
  }
};

struct EncoderState {
  WMT::Reference<WMT::RenderCommandEncoder> activeRenderEncoder{};
  WMT::Reference<WMT::BlitCommandEncoder> activeBlitEncoder{};
  bool hasActiveRender = false;
};

struct PassState {
  AttachmentKey activeKey{};
  HazardProbe activeWriteHazard{};
  RenderPassInstanceToken activeInstance{};
  // R-BACK-13.1 / 13.6: current render encoder's chosen FFP path.
  bool activePassUsesTileFfp = false;
  std::optional<core::ClearDesc> pendingClear;
  core::metalqueue::PublishedCommandRef pendingClearCommand{};
  // R-BACK-15.4: color attachment handles bound on the active render encoder.
  std::array<core::Handle, core::kMaxRenderTargets> activeColorHandles{};
  // Copied attachment identity/action facts for exactly-once late store-action
  // resolution. No source payload or borrowed lookahead storage escapes here.
  LateRenderPassStoreState lateStore{};
};

struct BindingState {
  // R-BACK-12.22 / 12.24: current render encoder's sticky argbuf state.
  bool activePassUsesArgbufHybrid = false;
  // R-BACK-12.22..12.26: resource-array sub-mode of the sticky argbuf table.
  bool activePassUsesArgbufResourceArray = false;
  bool activePassUsesArgbufDirectCbuf = false;
  std::optional<core::FlatDrawStateKey> activeDrawStateKey;
  bool activeDrawStateUsesPrefetchedPsoLayout = false;
  uniform::DirtyState uniformDirty{};
  std::optional<u64> lastArgbufPayloadHash;
  std::optional<ArgbufPayloadDeltaKey> lastArgbufPayloadDeltaKey;
  std::optional<ArgbufPayloadDeltaComponentKey>
      lastArgbufPayloadDeltaComponentKey;
  std::optional<core::DrawUniformPayload> lastArgbufPayloadDeltaPayload;
  ArgbufCbufCache argbufCbufCache;
  StreamIbStagingCache activeStreamIbStaging;
  TextureSamplerBindShadow textureSamplerShadow{};
  bool initialized = false;
};

struct DiagnosticsState {
  std::optional<core::metalcapture::MetalCaptureRequest> metalCaptureRequest;
  ActiveColorAttachmentDump activeColorAttachmentDump{};
  ActiveDepthAttachmentDump activeDepthAttachmentDump{};
  std::vector<ActiveDrawTextureDump> activeDrawTextureDumps;
  ActiveEncoderBreakdown activeEncoderBreakdown;
  std::optional<VisibilityScoutPass> activeVisibilityScout;
  u64 renderEncoderIndex = 0;
  WMT::Reference<WMT::CounterSampleBuffer> renderEncoderGpuSampleBuffer{};
  std::vector<core::metalqueue::QueueSubmissionRecord::RenderEncoderGpuSample>
      renderEncoderGpuSamples;
  std::uint32_t renderEncoderGpuSampleCursor = 0;
  std::uint32_t requestedRenderEncoderGpuSamples = 0;
};

struct CompletionState {
  std::vector<std::function<void()>> postCommitCallbacks;
  std::vector<std::function<void()>> completionCallbacks;
  std::uint64_t committedSubCommandBuffers = 0;
  // Split-policy state belongs to the command-buffer chain, not one
  // encodeChunk call. A source fragment is only a replay range edge and must
  // not make an already-populated tail look empty or reset PerNRecords.
  bool tailCommandBufferHasWork = false;
  std::uint32_t recordsSinceLastSplit = 0;
};

struct EncodeChunkSessionStorage {
  // Session layout order is intentional: encoder/pass lifetime precedes
  // binding shadows, diagnostics, completion owners, then the non-owning tail.
  EncoderState encoder{};
  PassState pass{};
  BindingState binding{};
  DiagnosticsState diagnostics{};
  CompletionState completion{};
  // Identity of the current tail command buffer in this session's in-order
  // chain. This is non-owning: the queue's pending submission carrier owns the
  // WMT::Reference. It changes only when the encoder legitimately replaces the
  // tail during a mid-chunk split.
  obj_handle_t commandBufferChainTail = NULL_OBJECT_HANDLE;
};

void initializeStorage(EncodeChunkSessionStorage& storage,
                       const uniform::DirtyState& dirty);
EncodeChunkSessionStorage makeStorage(const uniform::DirtyState& dirty);

void initializeGpuSamplingStorage(EncodeChunkSessionStorage& storage,
                                  WMT::Device device,
                                  std::size_t commandCount);
// payload and lookaheadSources are synchronous call-local borrows and are not
// retained in session storage or completion callbacks.
std::size_t gpuSamplingCommandCount(
    core::SourcePayloadView payload,
    std::uint64_t sourceSeqId,
    std::size_t currentCommandCount,
    std::span<const core::metalqueue::ResolvedPublishedSource>
        lookaheadSources) noexcept;

}  // namespace dxmt9::encoders::encode_session
