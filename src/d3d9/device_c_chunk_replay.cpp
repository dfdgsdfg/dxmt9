// Batched-chunk C ABI: dxmt9c_device_commit_chunk and the packet->state
// replay machinery it invokes. Per-call dxmt9c_* setters live in
// device_c_draw.cpp (interactive entry points) and device_c_state.cpp
// (state setters); both are referenced via extern "C" forward decls.

#include "device_c_provider.hpp"
#include "device_c_chunk_v2_replay.hpp"
#include "device_c_record_utils.hpp"
#include "device_c_replay_offload.hpp"
#include "util/unixcall_marshal.hpp"

#include "../dxmt9/dxmt9_perf_counters.hpp"
// Need the full dxmt9::Device type to call markChunkResources on the
// upperDevice shared_ptr that the chunk importer's Phase 4-B path
// hands the per-chunk retention list to.
#include "dxmt9/dxmt9_device.hpp"
#include "util/log/log.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

#if defined(__APPLE__)
#include <pthread.h>
#endif

using namespace dxmt9::d3d9::devicec;

// Forward declarations for state-setter ABI entry points implemented in
// device_c_state.cpp. The packet-replay path (applyDrawPacketStateViaIface)
// and the chunk importer (dxmt9c_device_commit_chunk) call these to dispatch
// per-record state deltas. The provider macro renames apply uniformly via
// device_c_provider.hpp, so the linker resolves to the same symbols.
extern "C" int32_t dxmt9c_device_set_viewport(D9CDevice* d, const D9CViewport* vp);
extern "C" int32_t dxmt9c_device_set_scissor_rect(D9CDevice* d, const D9CRect* r);
extern "C" int32_t dxmt9c_device_set_transform(D9CDevice* d, uint32_t state,
                                               const D9CMatrix* m);
extern "C" int32_t dxmt9c_device_set_material(D9CDevice* d, const D9CMaterial* m);
extern "C" int32_t dxmt9c_device_set_light(D9CDevice* d, uint32_t idx, const D9CLight* l);
extern "C" int32_t dxmt9c_device_light_enable(D9CDevice* d, uint32_t i, uint32_t en);
extern "C" int32_t dxmt9c_device_set_texture_stage_state(D9CDevice* d, uint32_t st,
                                                         uint32_t type, uint32_t val);
extern "C" int32_t dxmt9c_device_set_sampler_state(D9CDevice* d, uint32_t s, uint32_t type,
                                                   uint32_t val);
extern "C" int32_t dxmt9c_device_set_clip_plane(D9CDevice* d, uint32_t idx,
                                                const float plane[4]);
extern "C" int32_t dxmt9c_device_set_vs_const_f(D9CDevice* d, uint32_t s, const float* data,
                                                uint32_t cnt);
extern "C" int32_t dxmt9c_device_set_ps_const_f(D9CDevice* d, uint32_t s, const float* data,
                                                uint32_t cnt);
extern "C" int32_t dxmt9c_device_set_vs_const_i(D9CDevice* d, uint32_t s, const int32_t* data,
                                                uint32_t cnt);
extern "C" int32_t dxmt9c_device_set_ps_const_i(D9CDevice* d, uint32_t s, const int32_t* data,
                                                uint32_t cnt);
extern "C" int32_t dxmt9c_device_set_vs_const_b(D9CDevice* d, uint32_t s,
                                                const uint32_t* data, uint32_t cnt);
extern "C" int32_t dxmt9c_device_set_ps_const_b(D9CDevice* d, uint32_t s,
                                                const uint32_t* data, uint32_t cnt);
extern "C" int32_t dxmt9c_device_set_render_state(D9CDevice* d, uint32_t state,
                                                   uint32_t value);
extern "C" int32_t dxmt9c_device_set_fvf(D9CDevice* d, uint32_t fvf);

// Forward declaration for the query-issue ABI entry implemented in
// device_c_swapchain_query_stateblock.cpp; chunk replay routes
// D9C_COMMAND_RECORD_QUERY_ISSUE through it.
extern "C" int32_t dxmt9c_query_issue(D9CQuery* q, uint32_t flags);

// Forward declarations for the per-call interactive C ABI entries that
// remain in device_c_draw.cpp. The chunk dispatcher routes the matching
// D9C_COMMAND_RECORD_* and the per-packet shader/render-target/depth/
// vertex-decl/index-buffer deltas through these so behavior matches the
// per-call path bit-for-bit.
extern "C" int32_t dxmt9c_device_present(D9CDevice* d, const D9CRect* src, const D9CRect* dst,
                                         uint64_t destWindow, const void* dirty, uint32_t flags);
extern "C" int32_t dxmt9c_device_clear(D9CDevice* d, uint32_t count, const D9CRect* rects,
                                       uint32_t flags, uint32_t colorARGB, float z,
                                       uint32_t stencil);
extern "C" int32_t dxmt9c_device_set_stream_source(D9CDevice* d, uint32_t stream,
                                                   D9CBuffer* buf, uint32_t off,
                                                   uint32_t stride);
extern "C" int32_t dxmt9c_device_set_vertex_declaration(D9CDevice* d, D9CVertexDecl* vd);
extern "C" int32_t dxmt9c_device_set_indices(D9CDevice* d, D9CBuffer* buf);
extern "C" int32_t dxmt9c_device_set_vertex_shader(D9CDevice* d, D9CShader* s);
extern "C" int32_t dxmt9c_device_set_pixel_shader(D9CDevice* d, D9CShader* s);
extern "C" int32_t dxmt9c_device_set_render_target(D9CDevice* d, uint32_t idx,
                                                   D9CSurface* surf);
extern "C" int32_t dxmt9c_device_set_depth_stencil(D9CDevice* d, D9CSurface* surf);
extern "C" int32_t dxmt9c_device_set_texture(D9CDevice* d, uint32_t stage,
                                              D9CTexture* texture);
extern "C" int32_t dxmt9c_device_draw_primitive(D9CDevice* d, uint32_t type,
                                                 uint32_t startVertex,
                                                 uint32_t count);
extern "C" int32_t dxmt9c_device_draw_indexed_primitive(
    D9CDevice* d, uint32_t type, int32_t baseVertex, uint32_t minVertex,
    uint32_t numVertices, uint32_t startIndex, uint32_t count);
extern "C" int32_t dxmt9c_device_draw_primitive_up(
    D9CDevice* d, uint32_t type, uint32_t count, const void* data,
    uint32_t stride);
extern "C" int32_t dxmt9c_device_draw_indexed_primitive_up(
    D9CDevice* d, uint32_t type, uint32_t minVertex, uint32_t numVertices,
    uint32_t count, const void* indexData, uint32_t indexFormat,
    const void* vertexData, uint32_t stride);
extern "C" int32_t dxmt9c_device_update_surface(D9CDevice* d, D9CSurface* src,
                                                const D9CRect*, D9CSurface* dst,
                                                const D9CRect*);
extern "C" int32_t dxmt9c_device_update_texture(D9CDevice* d, D9CTexture* src,
                                                D9CTexture* dst);
extern "C" int32_t dxmt9c_device_stretch_rect(D9CDevice* d, D9CSurface* src, const D9CRect* sr,
                                              D9CSurface* dst, const D9CRect* dr,
                                              uint32_t filter);
extern "C" int32_t dxmt9c_device_color_fill(D9CDevice* d, D9CSurface* surf, const D9CRect* r,
                                            uint32_t colorARGB);
extern "C" int32_t dxmt9c_device_get_render_target_data(D9CDevice* d, D9CSurface* rt,
                                                        D9CSurface* dst);

namespace {

struct ReplayScratchArena {
  std::vector<dxmt9::core::DrawRunSubmission> submissions;
  std::vector<dxmt9::core::DrawParam> runParams;
  std::vector<dxmt9::core::DrawBindingOverride> bindingOverrides;
  std::vector<dxmt9::core::DrawParamPayloadView> runPayloads;
  std::vector<dxmt9::core::ChunkHandleEntry> coreEntries;
  bool inUse = false;

  void clear() noexcept {
    submissions.clear();
    runParams.clear();
    bindingOverrides.clear();
    runPayloads.clear();
    coreEntries.clear();
  }
};

ReplayScratchArena& replayScratchArena() {
  static thread_local ReplayScratchArena scratch;
  return scratch;
}

class ScopedReplayScratchUse {
public:
  explicit ScopedReplayScratchUse(ReplayScratchArena& scratch) noexcept
      : scratch_(scratch) {
    DXMT_ASSERT(!scratch_.inUse);
    scratch_.inUse = true;
    scratch_.clear();
  }

  ~ScopedReplayScratchUse() {
    scratch_.clear();
    scratch_.inUse = false;
  }

  ScopedReplayScratchUse(const ScopedReplayScratchUse&) = delete;
  ScopedReplayScratchUse& operator=(const ScopedReplayScratchUse&) = delete;

private:
  ReplayScratchArena& scratch_;
};

// C1 helper: route per-record dirty marking through the dxmt9
// CommandQueue's pendingDirty_ accumulator. Returns nullptr on test /
// stub paths where no upperDevice exists; callers no-op in that case.
dxmt9::CommandQueue* findDirtyQueue(D9CDevice* d) {
  if (!d) return nullptr;
  auto upper = d->dev().upperDevice();
  if (!upper) return nullptr;
  return &upper->queue();
}

// C1 helper: D3D9 render-state IDs that affect each FFP PS uniform
// sub-block. Centralized so commit_chunk's APPLY_STATE handler and
// the per-call SetRenderState dispatcher (if it ever wires direct
// dirty-marking) stay in sync.
bool isFogRenderState(uint32_t state) {
  return state == dxmt9::core::RS_FOG_ENABLE ||
         state == dxmt9::core::RS_FOG_COLOR ||
         state == dxmt9::core::RS_FOG_TABLE_MODE ||
         state == dxmt9::core::RS_FOG_START ||
         state == dxmt9::core::RS_FOG_END ||
         state == dxmt9::core::RS_FOG_DENSITY ||
         state == dxmt9::core::RS_FOG_FROM_VERTEX ||
         state == dxmt9::core::RS_RANGE_FOG;
}

bool isAlphaRenderState(uint32_t state) {
  return state == dxmt9::core::RS_ALPHA_TEST_ENABLE ||
         state == dxmt9::core::RS_ALPHA_FUNC ||
         state == dxmt9::core::RS_ALPHA_REF;
}

uint64_t wireHandleValue(const D9CWireHandle& handle) {
  return static_cast<uint64_t>(handle.lo) |
         (static_cast<uint64_t>(handle.hi) << 32);
}

template <typename T>
T* wireValuePtr(uint64_t value) {
  if (!value) {
    return nullptr;
  }
  if (value <= 0xffffffffull) {
    if (auto* decoded =
            dxmt9::util::marshal::wow64::decodeHandle<T*>(static_cast<uint32_t>(value))) {
      return decoded;
    }
    if (isWow64NativePointerAllowed(value)) {
      return reinterpret_cast<T*>(static_cast<uintptr_t>(value));
    }
    if (requiresWow64PointerShadow()) {
      return nullptr;
    }
  }
  return reinterpret_cast<T*>(static_cast<uintptr_t>(value));
}

template <typename T>
T* wireHandlePtr(const D9CWireHandle& handle) {
  return wireValuePtr<T>(wireHandleValue(handle));
}

uint64_t bufferObjectHandleValue(const std::shared_ptr<dxmt9::core::Buffer>& buffer) {
  return buffer ? buffer->handle().value : 0ull;
}

uint64_t wireBufferObjectHandleValue(const D9CWireHandle& handle) {
  auto* buffer = wireHandlePtr<D9CBuffer>(handle);
  return buffer && buffer->obj ? buffer->obj->handle().value : 0ull;
}

bool failed(int32_t hr) {
  return hr < 0;
}

void countDurationSince(std::chrono::steady_clock::time_point start,
                        void (*counter)(std::uint64_t)) {
  counter(static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - start).count()));
}

bool commitChunkRecordAllowsPendingDrawBatchThrough(std::uint32_t type) {
  switch (type) {
  case D9C_COMMAND_RECORD_SET_VS_CONST_F:
  case D9C_COMMAND_RECORD_SET_VS_CONST_I:
  case D9C_COMMAND_RECORD_SET_VS_CONST_B:
  case D9C_COMMAND_RECORD_SET_PS_CONST_F:
  case D9C_COMMAND_RECORD_SET_PS_CONST_I:
  case D9C_COMMAND_RECORD_SET_PS_CONST_B:
    return true;
  default:
    return false;
  }
}

std::uint32_t commitChunkDrawDeltaMask(const D9CDrawPrimitivePacket& packet) {
  std::uint32_t mask = 0;
  using namespace dxmt9::perf;
  if (packet.renderStateCount != 0) mask |= CommitChunkDrawDeltaRenderState;
  if (packet.textureMask != 0) mask |= CommitChunkDrawDeltaTexture;
  if (packet.streamSourceMask != 0) mask |= CommitChunkDrawDeltaStream;
  if (packet.fvfValid != 0) mask |= CommitChunkDrawDeltaFvf;
  if (packet.vsValid != 0 || packet.psValid != 0) mask |= CommitChunkDrawDeltaShader;
  if (packet.vdeclValid != 0) mask |= CommitChunkDrawDeltaVertexDecl;
  if (packet.rtMask != 0) mask |= CommitChunkDrawDeltaRenderTarget;
  if (packet.dsValid != 0) mask |= CommitChunkDrawDeltaDepthStencil;
  if (packet.viewportValid != 0) mask |= CommitChunkDrawDeltaViewport;
  if (packet.scissorValid != 0) mask |= CommitChunkDrawDeltaScissor;
  if (packet.tssCount != 0) mask |= CommitChunkDrawDeltaTextureStageState;
  if (packet.samplerStateCount != 0) mask |= CommitChunkDrawDeltaSamplerState;
  if (packet.materialValid != 0) mask |= CommitChunkDrawDeltaMaterial;
  if (packet.clipPlaneMask != 0) mask |= CommitChunkDrawDeltaClipPlane;
  if (packet.transformCount != 0) mask |= CommitChunkDrawDeltaTransform;
  if (packet.lightSlotMask != 0) mask |= CommitChunkDrawDeltaLight;
  if (packet.lightEnableValidMask != 0) mask |= CommitChunkDrawDeltaLightEnable;
  return mask;
}

std::uint32_t drawStateInvalidationReasonFromCommitDeltaMask(std::uint32_t deltaMask) {
  using namespace dxmt9::core;
  using namespace dxmt9::perf;
  std::uint32_t reason = DrawStateInvalidationDrawPacket;
  if ((deltaMask & CommitChunkDrawDeltaRenderState) != 0) {
    reason |= DrawStateInvalidationRenderState;
  }
  if ((deltaMask & CommitChunkDrawDeltaTexture) != 0) {
    reason |= DrawStateInvalidationTexture;
  }
  if ((deltaMask & CommitChunkDrawDeltaStream) != 0) {
    reason |= DrawStateInvalidationStream;
  }
  if ((deltaMask & (CommitChunkDrawDeltaFvf |
                    CommitChunkDrawDeltaVertexDecl)) != 0) {
    reason |= DrawStateInvalidationFvfVdecl;
  }
  if ((deltaMask & CommitChunkDrawDeltaShader) != 0) {
    reason |= DrawStateInvalidationShader;
  }
  if ((deltaMask & (CommitChunkDrawDeltaRenderTarget |
                    CommitChunkDrawDeltaDepthStencil)) != 0) {
    reason |= DrawStateInvalidationRenderTargetDepth;
  }
  if ((deltaMask & (CommitChunkDrawDeltaViewport |
                    CommitChunkDrawDeltaScissor)) != 0) {
    reason |= DrawStateInvalidationViewportScissor;
  }
  if ((deltaMask & (CommitChunkDrawDeltaTextureStageState |
                    CommitChunkDrawDeltaSamplerState)) != 0) {
    reason |= DrawStateInvalidationTextureStageSampler;
  }
  if ((deltaMask & CommitChunkDrawDeltaTextureStageState) != 0) {
    reason |= DrawStateInvalidationTextureStageState;
  }
  if ((deltaMask & CommitChunkDrawDeltaSamplerState) != 0) {
    reason |= DrawStateInvalidationSamplerState;
  }
  if ((deltaMask & (CommitChunkDrawDeltaMaterial |
                    CommitChunkDrawDeltaTransform |
                    CommitChunkDrawDeltaLight |
                    CommitChunkDrawDeltaLightEnable)) != 0) {
    reason |= DrawStateInvalidationFfpState;
  }
  if ((deltaMask & CommitChunkDrawDeltaClipPlane) != 0) {
    reason |= DrawStateInvalidationClipPlane;
  }
  if ((deltaMask & CommitChunkDrawDeltaIndexBuffer) != 0) {
    reason |= DrawStateInvalidationIndexBuffer;
  }
  return reason;
}

void recordStateBlockRenderState(D9CDevice* d, uint32_t state, uint32_t value) {
  d->stateBlockRenderStates.insert(state);
  d->stateBlockRenderStateValues[state] = value;
}

dxmt9::core::Matrix4x4 matrixFromC(const D9CMatrix& m) {
  dxmt9::core::Matrix4x4 matrix;
  std::memcpy(matrix.m.data(), m.m, 16 * sizeof(float));
  return matrix;
}

dxmt9::core::Viewport viewportFromC(const D9CViewport& vp) {
  return {vp.x, vp.y, vp.width, vp.height, vp.minZ, vp.maxZ};
}

bool viewportValid(const dxmt9::core::Viewport& viewport) {
  return viewport.width != 0 && viewport.height != 0 &&
         std::isfinite(viewport.minZ) && std::isfinite(viewport.maxZ) &&
         viewport.minZ >= 0.0f && viewport.maxZ <= 1.0f &&
         viewport.minZ <= viewport.maxZ;
}

dxmt9::core::Rect rectFromC(const D9CRect& r) {
  return {r.left, r.top, r.right, r.bottom};
}

dxmt9::core::Material materialFromC(const D9CMaterial& m) {
  dxmt9::core::Material material;
  std::memcpy(&material.diffuse, &m.diffuse, sizeof(dxmt9::core::ColorRGBA));
  std::memcpy(&material.ambient, &m.ambient, sizeof(dxmt9::core::ColorRGBA));
  std::memcpy(&material.specular, &m.specular, sizeof(dxmt9::core::ColorRGBA));
  std::memcpy(&material.emissive, &m.emissive, sizeof(dxmt9::core::ColorRGBA));
  material.power = m.power;
  return material;
}

dxmt9::core::Light lightFromC(const D9CLight& l) {
  dxmt9::core::Light light;
  light.type = static_cast<dxmt9::core::LightType>(l.type);
  std::memcpy(&light.diffuse, &l.diffuse, sizeof(dxmt9::core::ColorRGBA));
  std::memcpy(&light.specular, &l.specular, sizeof(dxmt9::core::ColorRGBA));
  std::memcpy(&light.ambient, &l.ambient, sizeof(dxmt9::core::ColorRGBA));
  std::memcpy(light.position.data(), l.position, 3 * sizeof(float));
  std::memcpy(light.direction.data(), l.direction, 3 * sizeof(float));
  light.range = l.range;
  light.falloff = l.falloff;
  light.attenuation0 = l.attenuation0;
  light.attenuation1 = l.attenuation1;
  light.attenuation2 = l.attenuation2;
  light.theta = l.theta;
  light.phi = l.phi;
  return light;
}

dxmt9::core::RenderTargetAttachment attachmentFromSurface(
    const std::shared_ptr<dxmt9::core::Surface>& surface) {
  return surface ? dxmt9::core::RenderTargetAttachment{
                       surface->handle(), surface->level(), surface->multiSampleCount()}
                 : dxmt9::core::RenderTargetAttachment{};
}

bool drawPacketActualChangePerfEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PERF_DRAW_PACKET_ACTUAL_CHANGE");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

std::uint32_t drawPacketActualChangeMask(
    const dxmt9::core::DeviceState& state,
    const D9CDrawPrimitivePacket& packet) {
  std::uint32_t mask = 0;
  using namespace dxmt9::perf;

  auto addIf = [&](bool changed, std::uint32_t bit) {
    if (changed) {
      mask |= bit;
    }
  };

  for (uint32_t i = 0; i < packet.renderStateCount; ++i) {
    const auto& entry = packet.renderStates[i];
    bool changed = false;
    if (dxmt9::core::RenderStateTable::validKey(entry.state)) {
      changed = state.renderStates.valueOr(entry.state, 0u) != entry.value;
    }
    if (entry.state == dxmt9::core::RS_SCISSOR_TEST_ENABLE &&
        state.scissorEnabled != (entry.value != 0)) {
      changed = true;
    }
    addIf(changed, CommitChunkDrawDeltaRenderState);
  }

  for (uint32_t stage = 0;
       stage < std::min<std::uint32_t>(D9C_DRAW_PACKET_MAX_TEXTURES,
                                       dxmt9::core::kMaxTextures);
       ++stage) {
    if ((packet.textureMask & (1u << stage)) == 0) {
      continue;
    }
    auto* texture = wireHandlePtr<D9CTexture>(packet.textures[stage]);
    addIf(state.textures[stage] != (texture ? texture->obj : nullptr),
          CommitChunkDrawDeltaTexture);
  }

  for (uint32_t stream = 0;
       stream < std::min<std::uint32_t>(D9C_DRAW_PACKET_MAX_STREAMS,
                                        dxmt9::core::kMaxStreams);
       ++stream) {
    if ((packet.streamSourceMask & (1u << stream)) == 0) {
      continue;
    }
    const auto& source = packet.streamSources[stream];
    addIf(bufferObjectHandleValue(state.streamBuffers[stream]) !=
              wireBufferObjectHandleValue(source.buffer) ||
          state.streamOffsets[stream] != source.offset ||
          state.streamStrides[stream] != source.stride,
          CommitChunkDrawDeltaStream);
  }

  if (packet.fvfValid) {
    addIf(state.fvf != packet.fvf ||
          state.vertexDecl.fvf != packet.fvf ||
          !state.vertexDecl.elements.empty(),
          CommitChunkDrawDeltaFvf);
  }

  if (packet.vsValid) {
    auto* vs = wireHandlePtr<D9CShader>(packet.vsHandle);
    addIf(state.vertexShader != (vs ? vs->ref : dxmt9::core::ShaderRef{}),
          CommitChunkDrawDeltaShader);
  }
  if (packet.psValid) {
    auto* ps = wireHandlePtr<D9CShader>(packet.psHandle);
    addIf(state.pixelShader != (ps ? ps->ref : dxmt9::core::ShaderRef{}),
          CommitChunkDrawDeltaShader);
  }

  if (packet.vdeclValid) {
    auto* vd = wireHandlePtr<D9CVertexDecl>(packet.vdeclHandle);
    const bool elementsChanged = vd ? state.vertexDecl.elements != vd->elements
                                    : !state.vertexDecl.elements.empty();
    addIf(elementsChanged || state.vertexDecl.fvf != state.fvf,
          CommitChunkDrawDeltaVertexDecl);
  }

  for (uint32_t slot = 0;
       slot < std::min<std::uint32_t>(D9C_DRAW_PACKET_MAX_RENDER_TARGETS,
                                      dxmt9::core::kMaxRenderTargets);
       ++slot) {
    if ((packet.rtMask & (1u << slot)) == 0) {
      continue;
    }
    auto* rt = wireHandlePtr<D9CSurface>(packet.rtHandles[slot]);
    const auto surface = rt ? rt->obj : nullptr;
    bool changed = state.renderTargets[slot] != attachmentFromSurface(surface);
    if (slot == 0 && surface) {
      const auto& desc = surface->desc();
      const auto width = std::max(1u, desc.width);
      const auto height = std::max(1u, desc.height);
      const dxmt9::core::Viewport viewport{0, 0, width, height, 0.0f, 1.0f};
      const dxmt9::core::Rect scissor{0, 0, static_cast<int32_t>(width),
                                      static_cast<int32_t>(height)};
      changed = changed || state.viewport != viewport || state.scissorRect != scissor;
    }
    addIf(changed, CommitChunkDrawDeltaRenderTarget);
  }

  if (packet.dsValid) {
    auto* ds = wireHandlePtr<D9CSurface>(packet.dsHandle);
    addIf(state.depthStencil != attachmentFromSurface(ds ? ds->obj : nullptr),
          CommitChunkDrawDeltaDepthStencil);
  }

  if (packet.viewportValid) {
    addIf(state.viewport != viewportFromC(packet.viewport),
          CommitChunkDrawDeltaViewport);
  }
  if (packet.scissorValid) {
    addIf(state.scissorRect != rectFromC(packet.scissor),
          CommitChunkDrawDeltaScissor);
  }

  for (uint32_t i = 0; i < packet.tssCount; ++i) {
    const auto& e = packet.tss[i];
    const uint32_t stage = std::min(e.stage, dxmt9::core::kMaxTextureStages - 1);
    const uint32_t key = std::min(e.type, dxmt9::core::kMaxTextureStageStates - 1);
    addIf(state.textureStageStates[stage].valueOr(key, 0u) != e.value,
          CommitChunkDrawDeltaTextureStageState);
  }
  for (uint32_t i = 0; i < packet.samplerStateCount; ++i) {
    const auto& e = packet.samplerStates[i];
    const bool valid =
        e.sampler < dxmt9::core::kMaxSamplers &&
        dxmt9::core::SamplerStateTable::validKey(e.type);
    addIf(valid && state.samplerStates[e.sampler].valueOr(e.type, 0u) != e.value,
          CommitChunkDrawDeltaSamplerState);
  }

  if (packet.materialValid) {
    addIf(state.material != materialFromC(packet.material),
          CommitChunkDrawDeltaMaterial);
  }

  for (uint32_t i = 0; i < dxmt9::core::kMaxClipPlanes; ++i) {
    if ((packet.clipPlaneMask & (1u << i)) == 0) {
      continue;
    }
    const dxmt9::core::ClipPlane plane{packet.clipPlanes[i * 4 + 0],
                                       packet.clipPlanes[i * 4 + 1],
                                       packet.clipPlanes[i * 4 + 2],
                                       packet.clipPlanes[i * 4 + 3]};
    addIf(state.clipPlanes[i] != plane, CommitChunkDrawDeltaClipPlane);
  }

  for (uint32_t i = 0; i < packet.transformCount; ++i) {
    const auto& e = packet.transforms[i];
    const auto key = transformStateFromD3D(e.state);
    const auto matrix = matrixFromC(e.matrix);
    addIf(dxmt9::core::TransformTable::validKey(key) &&
              state.transforms.valueOr(key, dxmt9::core::Matrix4x4{}) != matrix,
          CommitChunkDrawDeltaTransform);
  }

  for (uint32_t i = 0; i < D9C_DRAW_PACKET_MAX_LIGHTS; ++i) {
    if ((packet.lightSlotMask & (1u << i)) == 0) {
      continue;
    }
    addIf(i < dxmt9::core::kMaxLights &&
              state.lights[i] != lightFromC(packet.lights[i]),
          CommitChunkDrawDeltaLight);
  }
  for (uint32_t i = 0; i < D9C_DRAW_PACKET_MAX_LIGHTS; ++i) {
    if ((packet.lightEnableValidMask & (1u << i)) == 0) {
      continue;
    }
    const bool enabled = (packet.lightEnableMask & (1u << i)) != 0;
    addIf(i < dxmt9::core::kMaxLights &&
              (state.lightEnabled[i] != enabled ||
               state.lights[i].enabled != enabled),
          CommitChunkDrawDeltaLightEnable);
  }

  return mask;
}

int32_t validateDrawPacketStateDelta(const D9CDrawPrimitivePacket& packet) {
  if (packet.renderStateCount > D9C_DRAW_PACKET_MAX_RENDER_STATES ||
      packet.tssCount > D9C_DRAW_PACKET_MAX_TSS ||
      packet.samplerStateCount > D9C_DRAW_PACKET_MAX_SAMPLER ||
      packet.transformCount > D9C_DRAW_PACKET_MAX_TRANSFORMS) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  for (uint32_t i = 0; i < packet.samplerStateCount; ++i) {
    if (packet.samplerStates[i].sampler >= dxmt9::core::kMaxSamplers) {
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
  }
  if (packet.viewportValid && !viewportValid(viewportFromC(packet.viewport))) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  return dxmt9::core::D3D_OK;
}

int32_t commitChunkFail(const char* reason,
                        std::uint32_t index = 0xffffffffu,
                        std::uint32_t type = 0,
                        int32_t hr = dxmt9::core::D3DERR_INVALIDCALL) {
  dxmt9DebugLog("commit_chunk fail reason=%s index=%u type=%u hr=0x%08x",
                reason, index, type, static_cast<std::uint32_t>(hr));
  dxmt9::util::logf(dxmt9::util::LogLevel::Info, "dxmt9-device",
                    "commit_chunk_fail reason=%s index=%u type=%u hr=0x%08x",
                    reason ? reason : "unknown", index, type,
                    static_cast<std::uint32_t>(hr));
  // R-BACK-2.10: every commit_chunk reject path funnels through here.
  dxmt9::perf::countChunkReject();
  return hr;
}

int32_t applyDrawPacketStateViaIface(D9CDevice* d, const D9CDrawPrimitivePacket& packet) {
  if (packet.renderStateCount > D9C_DRAW_PACKET_MAX_RENDER_STATES) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }

  for (uint32_t i = 0; i < packet.renderStateCount; ++i) {
    const auto& state = packet.renderStates[i];
    if (d->stateBlockRecording) {
      recordStateBlockRenderState(d, state.state, state.value);
      continue;
    }
    const int32_t hr = d->iface->SetRenderState(state.state, state.value);
    if (failed(hr)) {
      return hr;
    }
  }

  for (uint32_t stage = 0; stage < D9C_DRAW_PACKET_MAX_TEXTURES; ++stage) {
    if ((packet.textureMask & (1u << stage)) == 0) {
      continue;
    }
    auto texture = wireHandlePtr<D9CTexture>(packet.textures[stage]);
    const int32_t hr = d->iface->SetTexture(stage, texture ? texture->obj : nullptr);
    if (failed(hr)) {
      return hr;
    }
  }

  for (uint32_t stream = 0; stream < D9C_DRAW_PACKET_MAX_STREAMS; ++stream) {
    if ((packet.streamSourceMask & (1u << stream)) == 0) {
      continue;
    }
    const auto& source = packet.streamSources[stream];
    auto buffer = wireHandlePtr<D9CBuffer>(source.buffer);
    const int32_t hr = d->iface->SetStreamSource(stream, buffer ? buffer->obj : nullptr,
                                                 source.offset, source.stride);
    if (failed(hr)) {
      return hr;
    }
  }

  if (packet.fvfValid) {
    const int32_t hr = d->iface->SetFVF(packet.fvf);
    if (failed(hr)) {
      return hr;
    }
  }

  // Phase 12: shader-handle delta dispatch. Wire handles are server-side
  // D9CShader* casts (rawVS/rawPS); decode + call dxmt9c_device_set_*_shader.
  if (packet.vsValid) {
    auto* vs = wireHandlePtr<D9CShader>(packet.vsHandle);
    const int32_t hr = dxmt9c_device_set_vertex_shader(d, vs);
    if (failed(hr)) return hr;
  }
  if (packet.psValid) {
    auto* ps = wireHandlePtr<D9CShader>(packet.psHandle);
    const int32_t hr = dxmt9c_device_set_pixel_shader(d, ps);
    if (failed(hr)) return hr;
  }

  // Phase 12: vertex-decl handle delta.
  if (packet.vdeclValid) {
    auto* vd = wireHandlePtr<D9CVertexDecl>(packet.vdeclHandle);
    const int32_t hr = dxmt9c_device_set_vertex_declaration(d, vd);
    if (failed(hr)) return hr;
  }

  // Phase 12: render-target deltas. One dispatch per set bit in rtMask.
  for (uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_RENDER_TARGETS; ++slot) {
    if ((packet.rtMask & (1u << slot)) == 0) continue;
    auto* rt = wireHandlePtr<D9CSurface>(packet.rtHandles[slot]);
    const int32_t hr = dxmt9c_device_set_render_target(d, slot, rt);
    if (failed(hr)) return hr;
  }

  // Phase 12: depth-stencil delta.
  if (packet.dsValid) {
    auto* ds = wireHandlePtr<D9CSurface>(packet.dsHandle);
    const int32_t hr = dxmt9c_device_set_depth_stencil(d, ds);
    if (failed(hr)) return hr;
  }

  // Phase 12: viewport / scissor deltas.
  if (packet.viewportValid) {
    const int32_t hr = dxmt9c_device_set_viewport(d, &packet.viewport);
    if (failed(hr)) return hr;
  }
  if (packet.scissorValid) {
    const int32_t hr = dxmt9c_device_set_scissor_rect(d, &packet.scissor);
    if (failed(hr)) return hr;
  }

  // Phase 12: TSS / SamplerState delta dispatch.
  if (packet.tssCount > D9C_DRAW_PACKET_MAX_TSS ||
      packet.samplerStateCount > D9C_DRAW_PACKET_MAX_SAMPLER) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  for (uint32_t i = 0; i < packet.tssCount; ++i) {
    const auto& e = packet.tss[i];
    const int32_t hr = dxmt9c_device_set_texture_stage_state(d, e.stage, e.type, e.value);
    if (failed(hr)) return hr;
  }
  for (uint32_t i = 0; i < packet.samplerStateCount; ++i) {
    const auto& e = packet.samplerStates[i];
    const int32_t hr = dxmt9c_device_set_sampler_state(d, e.sampler, e.type, e.value);
    if (failed(hr)) return hr;
  }

  // Phase 12: material delta.
  if (packet.materialValid) {
    const int32_t hr = dxmt9c_device_set_material(d, &packet.material);
    if (failed(hr)) return hr;
  }

  // Phase 12: clip-plane delta — one dispatch per set bit in
  // clipPlaneMask. Each plane is 4 floats stored contiguously at
  // clipPlanes[i*4..i*4+3].
  for (uint32_t i = 0; i < 6; ++i) {
    if ((packet.clipPlaneMask & (1u << i)) == 0) continue;
    const int32_t hr = dxmt9c_device_set_clip_plane(d, i, &packet.clipPlanes[i * 4]);
    if (failed(hr)) return hr;
  }

  // Phase 12: Transform delta — packet.transforms[0..transformCount).
  if (packet.transformCount > D9C_DRAW_PACKET_MAX_TRANSFORMS) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  for (uint32_t i = 0; i < packet.transformCount; ++i) {
    const auto& e = packet.transforms[i];
    const int32_t hr = dxmt9c_device_set_transform(d, e.state, &e.matrix);
    if (failed(hr)) return hr;
  }

  // Phase 12: Light delta — one dispatch per set bit in lightSlotMask.
  for (uint32_t i = 0; i < D9C_DRAW_PACKET_MAX_LIGHTS; ++i) {
    if ((packet.lightSlotMask & (1u << i)) == 0) continue;
    const int32_t hr = dxmt9c_device_set_light(d, i, &packet.lights[i]);
    if (failed(hr)) return hr;
  }

  // Phase 12: LightEnable delta. ValidMask says which slots have a fresh
  // value; lightEnableMask carries the new enabled bit per slot.
  for (uint32_t i = 0; i < D9C_DRAW_PACKET_MAX_LIGHTS; ++i) {
    if ((packet.lightEnableValidMask & (1u << i)) == 0) continue;
    const uint32_t enabled = (packet.lightEnableMask & (1u << i)) ? 1u : 0u;
    const int32_t hr = dxmt9c_device_light_enable(d, i, enabled);
    if (failed(hr)) return hr;
  }

  return dxmt9::core::D3D_OK;
}

// Normal chunk replay path: apply the packet's flat delta directly to the
// core DeviceState and invalidate derived draw-state caches once. The iface
// replay above remains only for state-block recording semantics.
//
// This routine is mode-agnostic: it observes valid/mask bits and applies
// whatever is set. A delta packet sets only what changed since the last
// draw; a full-snapshot packet (DXMT9_PE_DRAW_FULL_SNAPSHOT=1, produced
// in d3d9_pe_device.cpp::buildDrawPrimitivePacket) sets every field from
// the PE shadow. Both yield the same DeviceState — proved by
// tests/native/bridge/pe_full_snapshot_equivalence_spec.cpp.
int32_t applyDrawPacketStateDirect(D9CDevice* d, const D9CDrawPrimitivePacket& packet) {
  const int32_t validationHr = validateDrawPacketStateDelta(packet);
  if (failed(validationHr)) {
    return validationHr;
  }

  if (packetHasNoStateDelta(packet)) {
    return dxmt9::core::D3D_OK;
  }

  const std::uint32_t deltaMask = commitChunkDrawDeltaMask(packet);
  if (drawPacketActualChangePerfEnabled()) {
    dxmt9::perf::countDrawPacketActualChange(
        deltaMask, drawPacketActualChangeMask(d->dev().state(), packet));
  }

  auto& state = d->dev().mutableState(
      drawStateInvalidationReasonFromCommitDeltaMask(deltaMask));

  for (uint32_t i = 0; i < packet.renderStateCount; ++i) {
    const auto& entry = packet.renderStates[i];
    state.renderStates.set(entry.state, entry.value);
    if (entry.state == dxmt9::core::RS_SCISSOR_TEST_ENABLE) {
      state.scissorEnabled = entry.value != 0;
    }
  }

  for (uint32_t stage = 0; stage < D9C_DRAW_PACKET_MAX_TEXTURES; ++stage) {
    if ((packet.textureMask & (1u << stage)) == 0) {
      continue;
    }
    auto* texture = wireHandlePtr<D9CTexture>(packet.textures[stage]);
    state.textures[stage] = texture ? texture->obj : nullptr;
  }

  for (uint32_t stream = 0; stream < D9C_DRAW_PACKET_MAX_STREAMS; ++stream) {
    if ((packet.streamSourceMask & (1u << stream)) == 0) {
      continue;
    }
    const auto& source = packet.streamSources[stream];
    auto* buffer = wireHandlePtr<D9CBuffer>(source.buffer);
    state.streamBuffers[stream] = buffer ? buffer->obj : nullptr;
    state.streamOffsets[stream] = source.offset;
    state.streamStrides[stream] = source.stride;
  }

  if (packet.fvfValid) {
    state.fvf = packet.fvf;
    state.vertexDecl.fvf = packet.fvf;
    state.vertexDecl.elements.clear();
  }

  if (packet.vsValid) {
    auto* vs = wireHandlePtr<D9CShader>(packet.vsHandle);
    state.vertexShader = vs ? vs->ref : dxmt9::core::ShaderRef{};
  }
  if (packet.psValid) {
    auto* ps = wireHandlePtr<D9CShader>(packet.psHandle);
    state.pixelShader = ps ? ps->ref : dxmt9::core::ShaderRef{};
  }

  if (packet.vdeclValid) {
    auto* vd = wireHandlePtr<D9CVertexDecl>(packet.vdeclHandle);
    if (vd) {
      state.vertexDecl.elements = vd->elements;
    } else {
      state.vertexDecl.elements.clear();
    }
    state.vertexDecl.fvf = state.fvf;
  }

  for (uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_RENDER_TARGETS; ++slot) {
    if ((packet.rtMask & (1u << slot)) == 0) {
      continue;
    }
    auto* rt = wireHandlePtr<D9CSurface>(packet.rtHandles[slot]);
    const auto surface = rt ? rt->obj : nullptr;
    state.renderTargets[slot] = attachmentFromSurface(surface);
    if (slot == 0 && surface) {
      const auto& desc = surface->desc();
      const auto width = std::max(1u, desc.width);
      const auto height = std::max(1u, desc.height);
      state.viewport = {0, 0, width, height, 0.0f, 1.0f};
      state.scissorRect = {0, 0, static_cast<int32_t>(width),
                           static_cast<int32_t>(height)};
    }
  }

  if (packet.dsValid) {
    auto* ds = wireHandlePtr<D9CSurface>(packet.dsHandle);
    state.depthStencil = attachmentFromSurface(ds ? ds->obj : nullptr);
  }

  if (packet.viewportValid) {
    state.viewport = viewportFromC(packet.viewport);
  }
  if (packet.scissorValid) {
    state.scissorRect = rectFromC(packet.scissor);
  }

  for (uint32_t i = 0; i < packet.tssCount; ++i) {
    const auto& e = packet.tss[i];
    const uint32_t stage = std::min(e.stage, dxmt9::core::kMaxTextureStages - 1);
    const uint32_t key = std::min(e.type, dxmt9::core::kMaxTextureStageStates - 1);
    state.textureStageStates[stage].set(key, e.value);
  }
  for (uint32_t i = 0; i < packet.samplerStateCount; ++i) {
    const auto& e = packet.samplerStates[i];
    state.samplerStates[e.sampler].set(e.type, e.value);
  }

  if (packet.materialValid) {
    state.material = materialFromC(packet.material);
  }

  for (uint32_t i = 0; i < dxmt9::core::kMaxClipPlanes; ++i) {
    if ((packet.clipPlaneMask & (1u << i)) == 0) {
      continue;
    }
    state.clipPlanes[i] = {packet.clipPlanes[i * 4 + 0],
                           packet.clipPlanes[i * 4 + 1],
                           packet.clipPlanes[i * 4 + 2],
                           packet.clipPlanes[i * 4 + 3]};
  }

  for (uint32_t i = 0; i < packet.transformCount; ++i) {
    const auto& e = packet.transforms[i];
    state.transforms.set(transformStateFromD3D(e.state), matrixFromC(e.matrix));
  }

  for (uint32_t i = 0; i < D9C_DRAW_PACKET_MAX_LIGHTS; ++i) {
    if ((packet.lightSlotMask & (1u << i)) == 0) {
      continue;
    }
    state.lights[i] = lightFromC(packet.lights[i]);
  }
  for (uint32_t i = 0; i < D9C_DRAW_PACKET_MAX_LIGHTS; ++i) {
    if ((packet.lightEnableValidMask & (1u << i)) == 0) {
      continue;
    }
    const bool enabled = (packet.lightEnableMask & (1u << i)) != 0;
    state.lightEnabled[i] = enabled;
    state.lights[i].enabled = enabled;
  }

  return dxmt9::core::D3D_OK;
}

int32_t applyDrawPacketState(D9CDevice* d, const D9CDrawPrimitivePacket& packet) {
  if (d->stateBlockRecording) {
    return applyDrawPacketStateViaIface(d, packet);
  }
  return applyDrawPacketStateDirect(d, packet);
}

int32_t timedApplyDrawPacketState(D9CDevice* d, const D9CDrawPrimitivePacket& packet) {
  const auto start = std::chrono::steady_clock::now();
  const int32_t hr = applyDrawPacketState(d, packet);
  countDurationSince(start, dxmt9::perf::countCommitChunkApplyDrawStateCpuTime);
  return hr;
}

// Direct core::Device dispatch for the legacy direct packet bridge entry.
int32_t applyDrawPrimitivePacket(D9CDevice* d, const D9CDrawPrimitivePacket& packet) {
  const int32_t stateHr = timedApplyDrawPacketState(d, packet);
  if (failed(stateHr)) {
    return stateHr;
  }
  return d->dev().drawPrimitive(ptFromD3D(packet.primitiveType),
                                packet.primitiveCount,
                                packet.startVertex);
}

}  // namespace

// V2 admission retains every resolved wrapper before inline or offloaded
// replay. Both replay completion and queue teardown release the same list.
void dxmt9::d3d9::releaseRetainedWrappers(dxmt9::d3d9::RawCommandChunk& chunk) {
  for (const auto& entry : chunk.retainedWrappers) {
    switch (entry.kind) {
    case D9C_CHUNK_HANDLE_KIND_TEXTURE:
      dxmt9c_texture_release(static_cast<D9CTexture*>(entry.ptr));
      break;
    case D9C_CHUNK_HANDLE_KIND_SURFACE:
      dxmt9c_surface_release(static_cast<D9CSurface*>(entry.ptr));
      break;
    case D9C_CHUNK_HANDLE_KIND_BUFFER:
      dxmt9c_buffer_release(static_cast<D9CBuffer*>(entry.ptr));
      break;
    case D9C_CHUNK_HANDLE_KIND_SHADER:
      dxmt9c_shader_release(static_cast<D9CShader*>(entry.ptr));
      break;
    case D9C_CHUNK_HANDLE_KIND_VERTEX_DECL:
      dxmt9c_vdecl_release(static_cast<D9CVertexDecl*>(entry.ptr));
      break;
    case D9C_CHUNK_HANDLE_KIND_QUERY:
      dxmt9c_query_release(static_cast<D9CQuery*>(entry.ptr));
      break;
    default:
      break;
    }
  }
  chunk.retainedWrappers.clear();
}

namespace {

void retainV2Wrapper(std::uint32_t kind, void* object) noexcept {
  if (!object) return;
  switch (kind) {
  case D9C_CHUNK_HANDLE_KIND_TEXTURE:
    dxmt9c_texture_addref(static_cast<D9CTexture*>(object));
    break;
  case D9C_CHUNK_HANDLE_KIND_SURFACE:
    dxmt9c_surface_addref(static_cast<D9CSurface*>(object));
    break;
  case D9C_CHUNK_HANDLE_KIND_BUFFER:
    dxmt9c_buffer_addref(static_cast<D9CBuffer*>(object));
    break;
  case D9C_CHUNK_HANDLE_KIND_SHADER:
    dxmt9c_shader_addref(static_cast<D9CShader*>(object));
    break;
  case D9C_CHUNK_HANDLE_KIND_VERTEX_DECL:
    dxmt9c_vdecl_addref(static_cast<D9CVertexDecl*>(object));
    break;
  case D9C_CHUNK_HANDLE_KIND_QUERY:
    dxmt9c_query_addref(static_cast<D9CQuery*>(object));
    break;
  default:
    break;
  }
}

class DeviceReplaySinkV2 final : public dxmt9::d3d9::NonDrawReplaySinkV2,
                                 public dxmt9::d3d9::SparseReplaySinkV2 {
public:
  DeviceReplaySinkV2(
      D9CDevice* device, bool pacedByPresentOrdinal,
      std::vector<dxmt9::core::DrawRunSubmission>* pendingDrawSubmissions)
      : device_(device),
        pacedByPresentOrdinal_(pacedByPresentOrdinal),
        pendingDrawSubmissions_(pendingDrawSubmissions) {}

  void setBatchCurrentDraw(bool value) noexcept {
    batchCurrentDraw_ = value;
  }

  std::int32_t setConstants(
      std::uint32_t type, const D9CCommandChunkWireSetConstV2& fixed,
      std::span<const std::byte> bytes) override {
    return setConstantBytes(type, fixed.startRegister, fixed.registerCount,
                            bytes);
  }

  std::int32_t clear(const D9CCommandChunkWireClearV2& fixed,
                     std::span<const D9CRect> rects) override {
    return dxmt9c_device_clear(device_, fixed.rectCount, rects.data(),
                               fixed.flags, fixed.colorARGB, fixed.z,
                               fixed.stencil);
  }

  std::int32_t present(
      const D9CCommandChunkWirePresentV2& fixed) override {
    device_->dev().setNextPresentPacedByOrdinal(pacedByPresentOrdinal_);
    return dxmt9c_device_present(device_, fixed.hasSrc ? &fixed.src : nullptr,
                                 fixed.hasDst ? &fixed.dst : nullptr,
                                 fixed.hwnd, nullptr, fixed.flags);
  }

  std::int32_t stretchRect(
      const D9CCommandChunkWireStretchRectV2& fixed, void* src,
      void* dst) override {
    return dxmt9c_device_stretch_rect(
        device_, static_cast<D9CSurface*>(src),
        fixed.hasSrcRect ? &fixed.srcRect : nullptr,
        static_cast<D9CSurface*>(dst),
        fixed.hasDstRect ? &fixed.dstRect : nullptr, fixed.filter);
  }

  std::int32_t colorFill(
      const D9CCommandChunkWireColorFillV2& fixed, void* surface) override {
    return dxmt9c_device_color_fill(
        device_, static_cast<D9CSurface*>(surface),
        fixed.hasRect ? &fixed.rect : nullptr, fixed.colorARGB);
  }

  std::int32_t updateTexture(
      const D9CCommandChunkWireUpdateTextureV2&, void* src,
      void* dst) override {
    return dxmt9c_device_update_texture(device_, static_cast<D9CTexture*>(src),
                                        static_cast<D9CTexture*>(dst));
  }

  std::int32_t updateSurface(
      const D9CCommandChunkWireUpdateSurfaceV2& fixed, void* src,
      void* dst) override {
    return dxmt9c_device_update_surface(
        device_, static_cast<D9CSurface*>(src),
        fixed.hasSrcRect ? &fixed.srcRect : nullptr,
        static_cast<D9CSurface*>(dst),
        fixed.hasDstPoint ? &fixed.dstPoint : nullptr);
  }

  std::int32_t queryIssue(
      const D9CCommandChunkWireQueryIssueV2& fixed, void* query) override {
    return dxmt9c_query_issue(static_cast<D9CQuery*>(query), fixed.flags);
  }

  std::int32_t readback(
      const D9CCommandChunkWireReadbackV2&, void* src, void* dst) override {
    return dxmt9c_device_get_render_target_data(
        device_, static_cast<D9CSurface*>(src),
        static_cast<D9CSurface*>(dst));
  }

  std::int32_t reszDepthResolve(
      const D9CCommandChunkWireReszDepthResolveV2&, void* msaaDepth,
      void* intzDest) override {
    auto* surface = static_cast<D9CSurface*>(msaaDepth);
    auto* texture = static_cast<D9CTexture*>(intzDest);
    return surface && texture
               ? device_->dev().reszDepthResolve(surface->obj, texture->obj)
               : dxmt9::core::D3D_OK;
  }

  std::int32_t applyState(
      const dxmt9::d3d9::ResolvedRecordV2View& record) override {
    return dxmt9::d3d9::replaySparseRecordV2(record, *this);
  }

  std::int32_t setRenderStates(
      std::span<const D9CCommandChunkWireRenderStateV2> values) override {
    for (const auto& value : values) {
      const auto hr = dxmt9c_device_set_render_state(
          device_, value.state, value.value);
      if (failed(hr)) return hr;
      if (auto* queue = findDirtyQueue(device_)) {
        if (isFogRenderState(value.state)) queue->applyDirtyRenderStateFog();
        if (isAlphaRenderState(value.state)) queue->applyDirtyRenderStateAlpha();
        if (value.state == dxmt9::core::RS_TEXTURE_FACTOR) {
          queue->applyDirtyRenderStateTexFactor();
        }
      }
    }
    return dxmt9::core::D3D_OK;
  }

  std::int32_t setTexture(std::uint32_t slot, void* texture) override {
    return dxmt9c_device_set_texture(
        device_, slot, static_cast<D9CTexture*>(texture));
  }

  std::int32_t setStream(
      const D9CCommandChunkWireStreamBindingV2& value,
      void* buffer) override {
    return dxmt9c_device_set_stream_source(
        device_, value.slot, static_cast<D9CBuffer*>(buffer), value.offset,
        value.stride);
  }

  std::int32_t setShader(std::uint32_t stage, void* shader) override {
    return stage == D9C_COMMAND_CHUNK_V2_SHADER_STAGE_VERTEX
               ? dxmt9c_device_set_vertex_shader(
                     device_, static_cast<D9CShader*>(shader))
               : dxmt9c_device_set_pixel_shader(
                     device_, static_cast<D9CShader*>(shader));
  }

  std::int32_t setVertexInput(std::uint32_t kind, std::uint32_t value,
                              void* declaration) override {
    if (kind == D9C_COMMAND_CHUNK_V2_VERTEX_INPUT_FVF) {
      return dxmt9c_device_set_fvf(device_, value);
    }
    const auto fvfHr = dxmt9c_device_set_fvf(device_, value);
    return failed(fvfHr)
               ? fvfHr
               : dxmt9c_device_set_vertex_declaration(
                     device_, static_cast<D9CVertexDecl*>(declaration));
  }

  std::int32_t setIndexBuffer(void* buffer) override {
    return dxmt9c_device_set_indices(device_, static_cast<D9CBuffer*>(buffer));
  }

  std::int32_t setRenderTarget(std::uint32_t slot, void* surface) override {
    return dxmt9c_device_set_render_target(
        device_, slot, static_cast<D9CSurface*>(surface));
  }

  std::int32_t setDepthStencil(void* surface) override {
    return dxmt9c_device_set_depth_stencil(
        device_, static_cast<D9CSurface*>(surface));
  }

  std::int32_t setViewport(const D9CViewport& value) override {
    if (auto* queue = findDirtyQueue(device_)) {
      queue->applyDirtyViewportChange();
    }
    return dxmt9c_device_set_viewport(device_, &value);
  }

  std::int32_t setScissor(const D9CRect& value) override {
    return dxmt9c_device_set_scissor_rect(device_, &value);
  }

  std::int32_t setMaterial(const D9CMaterial& value) override {
    if (auto* queue = findDirtyQueue(device_)) {
      queue->applyDirtyTransformChange();
    }
    return dxmt9c_device_set_material(device_, &value);
  }

  std::int32_t setClipPlane(
      const D9CCommandChunkWireClipPlaneV2& value) override {
    if (auto* queue = findDirtyQueue(device_)) {
      queue->applyDirtyClipPlaneChange();
    }
    return dxmt9c_device_set_clip_plane(device_, value.slot, value.values);
  }

  std::int32_t setTextureStageStates(
      std::span<const D9CDrawPacketTextureStageState> values) override {
    for (const auto& value : values) {
      const auto hr = dxmt9c_device_set_texture_stage_state(
          device_, value.stage, value.type, value.value);
      if (failed(hr)) return hr;
      if (value.type == dxmt9::core::TSS_CONSTANT) {
        if (auto* queue = findDirtyQueue(device_)) {
          queue->applyDirtyTextureStageConstant();
        }
      }
    }
    return dxmt9::core::D3D_OK;
  }

  std::int32_t setSamplerStates(
      std::span<const D9CDrawPacketSamplerState> values) override {
    for (const auto& value : values) {
      const auto hr = dxmt9c_device_set_sampler_state(
          device_, value.sampler, value.type, value.value);
      if (failed(hr)) return hr;
    }
    return dxmt9::core::D3D_OK;
  }

  std::int32_t setTransforms(
      std::span<const D9CDrawPacketTransform> values) override {
    if (!values.empty()) {
      if (auto* queue = findDirtyQueue(device_)) {
        queue->applyDirtyTransformChange();
      }
    }
    for (const auto& value : values) {
      const auto hr = dxmt9c_device_set_transform(
          device_, value.state, &value.matrix);
      if (failed(hr)) return hr;
    }
    return dxmt9::core::D3D_OK;
  }

  std::int32_t setLights(
      std::span<const D9CCommandChunkWireLightV2> values) override {
    if (!values.empty()) {
      if (auto* queue = findDirtyQueue(device_)) {
        queue->applyDirtyTransformChange();
      }
    }
    for (const auto& value : values) {
      const auto hr = dxmt9c_device_set_light(
          device_, value.slot, &value.light);
      if (failed(hr)) return hr;
    }
    return dxmt9::core::D3D_OK;
  }

  std::int32_t setLightEnables(
      std::span<const D9CCommandChunkWireLightEnableV2> values) override {
    if (!values.empty()) {
      if (auto* queue = findDirtyQueue(device_)) {
        queue->applyDirtyTransformChange();
      }
    }
    for (const auto& value : values) {
      const auto hr = dxmt9c_device_light_enable(
          device_, value.slot, value.enabled);
      if (failed(hr)) return hr;
    }
    return dxmt9::core::D3D_OK;
  }

  std::int32_t setConstants(
      std::uint16_t sectionKind,
      const D9CCommandChunkWireConstantRangeV2& range,
      std::span<const std::byte> bytes) override {
    std::uint32_t type = 0u;
    switch (sectionKind) {
    case D9C_COMMAND_CHUNK_V2_SECTION_VS_CONST_F:
      type = D9C_COMMAND_RECORD_SET_VS_CONST_F;
      break;
    case D9C_COMMAND_CHUNK_V2_SECTION_VS_CONST_I:
      type = D9C_COMMAND_RECORD_SET_VS_CONST_I;
      break;
    case D9C_COMMAND_CHUNK_V2_SECTION_VS_CONST_B:
      type = D9C_COMMAND_RECORD_SET_VS_CONST_B;
      break;
    case D9C_COMMAND_CHUNK_V2_SECTION_PS_CONST_F:
      type = D9C_COMMAND_RECORD_SET_PS_CONST_F;
      break;
    case D9C_COMMAND_CHUNK_V2_SECTION_PS_CONST_I:
      type = D9C_COMMAND_RECORD_SET_PS_CONST_I;
      break;
    case D9C_COMMAND_CHUNK_V2_SECTION_PS_CONST_B:
      type = D9C_COMMAND_RECORD_SET_PS_CONST_B;
      break;
    default:
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
    return setConstantBytes(type, range.startRegister, range.registerCount,
                            bytes);
  }

  std::int32_t finishApplyState(std::uint32_t) override {
    return dxmt9::core::D3D_OK;
  }

  std::int32_t draw(const dxmt9::d3d9::SparseDrawCallV2& call) override {
    if (!call.payload.userIndexData.empty()) {
      return device_->dev().drawIndexedPrimitiveUP(
          call.param.primitiveType, call.param.primitiveCount,
          call.payload.userVertexData, call.payload.userIndexData,
          call.param.indexType, call.stride);
    }
    if (!call.payload.userVertexData.empty()) {
      return device_->dev().drawPrimitiveUP(
          call.param.primitiveType, call.param.primitiveCount,
          call.payload.userVertexData, call.stride);
    }
    auto draw = call.param;
    if (draw.indexed) {
      draw.indexType = device_->dev().state().indexType;
    }
    if (batchCurrentDraw_ && pendingDrawSubmissions_) {
      const std::size_t previousIndex = pendingDrawSubmissions_->size();
      auto& submission = pendingDrawSubmissions_->emplace_back();
      const auto* previous =
          previousIndex == 0u
              ? nullptr
              : &(*pendingDrawSubmissions_)[previousIndex - 1u];
      const auto hr = device_->dev().snapshotDrawSubmissionFromCurrentState(
          draw, submission, previous);
      if (failed(hr)) {
        pendingDrawSubmissions_->pop_back();
      }
      return hr;
    }
    if (draw.indexed) {
      return device_->dev().drawIndexedPrimitive(
          draw.primitiveType, draw.primitiveCount, 0,
          draw.baseVertexIndex, draw.startIndex, draw.indexType);
    }
    return device_->dev().drawPrimitive(
        draw.primitiveType, draw.primitiveCount, draw.startVertex);
  }

private:
  std::int32_t setConstantBytes(std::uint32_t type, std::uint32_t start,
                                std::uint32_t count,
                                std::span<const std::byte> bytes) {
    const void* data = bytes.data();
    switch (type) {
    case D9C_COMMAND_RECORD_SET_VS_CONST_F:
      return dxmt9c_device_set_vs_const_f(
          device_, start, static_cast<const float*>(data), count);
    case D9C_COMMAND_RECORD_SET_VS_CONST_I:
      return dxmt9c_device_set_vs_const_i(
          device_, start, static_cast<const std::int32_t*>(data), count);
    case D9C_COMMAND_RECORD_SET_VS_CONST_B:
      return dxmt9c_device_set_vs_const_b(
          device_, start, static_cast<const std::uint32_t*>(data), count);
    case D9C_COMMAND_RECORD_SET_PS_CONST_F:
      return dxmt9c_device_set_ps_const_f(
          device_, start, static_cast<const float*>(data), count);
    case D9C_COMMAND_RECORD_SET_PS_CONST_I:
      return dxmt9c_device_set_ps_const_i(
          device_, start, static_cast<const std::int32_t*>(data), count);
    case D9C_COMMAND_RECORD_SET_PS_CONST_B:
      return dxmt9c_device_set_ps_const_b(
          device_, start, static_cast<const std::uint32_t*>(data), count);
    default:
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
  }

  D9CDevice* device_ = nullptr;
  bool pacedByPresentOrdinal_ = false;
  std::vector<dxmt9::core::DrawRunSubmission>* pendingDrawSubmissions_ = nullptr;
  bool batchCurrentDraw_ = false;
};

bool v2RecordCanBatchDraw(
    D9CDevice* device,
    const dxmt9::d3d9::ImportedRecordV2View& record) noexcept {
  if (!device || device->stateBlockRecording ||
      (record.header.type != D9C_COMMAND_RECORD_DRAW_PRIMITIVE &&
       record.header.type != D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE)) {
    return false;
  }
  return static_cast<dxmt9::core::PrimitiveType>(
             record.drawHeader.primitiveType - 1u) !=
         dxmt9::core::PrimitiveType::TriangleFan;
}

int32_t replayResolvedV2Chunk(
    D9CDevice* device, dxmt9::d3d9::RawCommandChunk& raw,
    bool pacedByPresentOrdinal) {
  const auto bytes = std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(raw.recordBlob.data()),
      raw.recordBlob.size());
  dxmt9::d3d9::ImportedChunkV2View imported;
  const dxmt9::d3d9::V2ChunkEnvelope envelope{
      .version = raw.wireVersion,
      .recordCount = raw.recordCount,
      .handleCount = raw.handleCount,
  };
  if (!raw.preflightValidated ||
      !dxmt9::d3d9::importPrevalidatedCommandChunkV2(
          bytes, envelope, imported) ||
      raw.resolvedObjects.size() != imported.handles.size()) {
    return commitChunkFail("v2-replay-preflight-view");
  }

  dxmt9::d3d9::ResolvedChunkV2View resolved{
      .wire = imported,
      .objects = raw.resolvedObjects,
  };
  auto& replayScratch = replayScratchArena();
  ScopedReplayScratchUse replayScratchUse(replayScratch);
  auto& pendingDrawSubmissions = replayScratch.submissions;
  pendingDrawSubmissions.reserve(
      std::min<std::uint32_t>(raw.recordCount, 256u));
  const auto flushPendingDrawSubmissions = [&]() {
    if (pendingDrawSubmissions.empty()) {
      return;
    }
    dxmt9::perf::countCommitChunkDrawSubmissionBatch(
        static_cast<std::uint32_t>(pendingDrawSubmissions.size()));
    device->dev().submitDrawSubmissionBatch(pendingDrawSubmissions);
    pendingDrawSubmissions.clear();
  };
  DeviceReplaySinkV2 sink(
      device, pacedByPresentOrdinal, &pendingDrawSubmissions);
  for (std::size_t index = 0u; index < imported.records.size(); ++index) {
    const auto record = resolved.record(index);
    const bool batchableDraw = v2RecordCanBatchDraw(device, record.wire);
    sink.setBatchCurrentDraw(batchableDraw);
    if (!batchableDraw &&
        !commitChunkRecordAllowsPendingDrawBatchThrough(
            record.wire.header.type)) {
      flushPendingDrawSubmissions();
    }
    const auto hr = dxmt9::d3d9::isSparseRecordV2(record.wire.header.type)
                        ? dxmt9::d3d9::replaySparseRecordV2(record, sink)
                        : dxmt9::d3d9::replayNonDrawRecordV2(record, sink);
    if (failed(hr)) {
      flushPendingDrawSubmissions();
      return commitChunkFail("v2-replay", static_cast<std::uint32_t>(index),
                             record.wire.header.type, hr);
    }
  }
  flushPendingDrawSubmissions();
  dxmt9::perf::countChunkAdmit();
  return dxmt9::core::D3D_OK;
}

void markResolvedV2Resources(
    D9CDevice* device, const dxmt9::d3d9::RawCommandChunk& raw,
    const dxmt9::d3d9::ImportedChunkV2View& imported) {
  auto& scratch = replayScratchArena();
  ScopedReplayScratchUse scratchUse(scratch);
  for (std::size_t i = 0u; i < imported.handles.size(); ++i) {
    dxmt9::core::Handle handle{};
    switch (imported.handles[i].kind) {
    case D9C_CHUNK_HANDLE_KIND_TEXTURE: {
      auto* value = static_cast<D9CTexture*>(raw.resolvedObjects[i]);
      if (value && value->obj) handle = value->obj->handle();
      break;
    }
    case D9C_CHUNK_HANDLE_KIND_SURFACE: {
      auto* value = static_cast<D9CSurface*>(raw.resolvedObjects[i]);
      if (value && value->obj) handle = value->obj->handle();
      break;
    }
    case D9C_CHUNK_HANDLE_KIND_BUFFER: {
      auto* value = static_cast<D9CBuffer*>(raw.resolvedObjects[i]);
      if (value && value->obj) handle = value->obj->handle();
      break;
    }
    default:
      break;
    }
    if (handle.value == 0u) continue;
    const auto kind = static_cast<dxmt9::core::ChunkHandleKind>(
        imported.handles[i].kind);
    const bool duplicate = std::any_of(
        scratch.coreEntries.begin(), scratch.coreEntries.end(),
        [&](const dxmt9::core::ChunkHandleEntry& existing) {
          return existing.kind == kind && existing.handle == handle;
        });
    if (!duplicate) {
      scratch.coreEntries.push_back({.kind = kind, .handle = handle});
    }
  }
  if (!scratch.coreEntries.empty()) {
    if (auto upper = device->dev().upperDevice()) {
      upper->markChunkResources(scratch.coreEntries);
    }
  }
}

bool v2ChunkRequiresInlineReplay(
    const dxmt9::d3d9::ImportedChunkV2View& imported) noexcept {
  return std::any_of(
      imported.records.begin(), imported.records.end(),
      [](const D9CCommandChunkWireRecordHeaderV2& record) {
        return replayInfoForCommandRecordType(record.type)
            .synchronousReadBoundary;
      });
}

}  // namespace

// Runs on the ReplayOffloadWorker thread. V2 admission has already validated
// the blob and resolved/retained every object, so the worker replays only that
// owned representation and releases wrappers on both success and failure.
int32_t dxmt9::d3d9::replayRawChunk(D9CDevice* d, dxmt9::d3d9::RawCommandChunk& chunk) {
  if (chunk.wireVersion != D9C_COMMAND_CHUNK_VERSION_V2) {
    releaseRetainedWrappers(chunk);
    dxmt9::perf::countCommandChunkV2Reject();
    return commitChunkFail("offload-unsupported-wire-version");
  }
  const auto replayCpuStart = std::chrono::steady_clock::now();
  const int32_t hr = replayResolvedV2Chunk(
      d, chunk, /*pacedByPresentOrdinal=*/true);
  countDurationSince(replayCpuStart, dxmt9::perf::countOffloadReplayCpuTime);
  releaseRetainedWrappers(chunk);
  if (failed(hr)) {
    dxmt9::perf::countCommandChunkV2Reject();
  }
  return hr;
}

extern "C" int32_t dxmt9c_device_commit_chunk(D9CDevice* d, const D9CCommandChunk* chunk) {
  // Boundary B2 — wall-clock latency of one commit_chunk bridge call.
  // This includes importer validation, handle/resource marking, record
  // replay, and queue submission construction. It excludes asynchronous
  // encode/GPU work after this call returns.
  const auto bridgeCommitStart = std::chrono::steady_clock::now();
  if (!d || !chunk) {
    return commitChunkFail("bad-header");
  }
  if (chunk->version != D9C_COMMAND_CHUNK_VERSION_V2) {
    dxmt9::perf::countCommandChunkV2Reject();
    return commitChunkFail("unsupported-wire-version", chunk->version);
  }
  {
    if (wireHandleValue(chunk->handles) != 0u ||
        chunk->recordBytes == 0u) {
      dxmt9::perf::countCommandChunkV2Reject();
      return commitChunkFail("v2-bad-outer");
    }
    const auto* records = wireHandlePtr<const std::byte>(chunk->records);
    if (!records) {
      dxmt9::perf::countCommandChunkV2Reject();
      return commitChunkFail("v2-missing-records");
    }
    const auto blob = std::span<const std::byte>(records,
                                                 chunk->recordBytes);
    const dxmt9::d3d9::V2ChunkEnvelope envelope{
        .version = chunk->version,
        .recordCount = chunk->recordCount,
        .handleCount = chunk->handleCount,
    };
    dxmt9::d3d9::RawCommandChunk raw;
    if (!dxmt9::d3d9::prepareV2OffloadChunk(
            blob, envelope, d->wireObjects, retainV2Wrapper, raw)) {
      dxmt9::perf::countCommandChunkV2Reject();
      return commitChunkFail("v2-admission");
    }
    dxmt9::d3d9::ImportedChunkV2View imported;
    const auto ownedBytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(raw.recordBlob.data()),
        raw.recordBlob.size());
    if (!raw.preflightValidated ||
        !dxmt9::d3d9::importPrevalidatedCommandChunkV2(
            ownedBytes, envelope, imported)) {
      dxmt9::d3d9::releaseRetainedWrappers(raw);
      dxmt9::perf::countCommandChunkV2Reject();
      return commitChunkFail("v2-owned-preflight-view");
    }
    markResolvedV2Resources(d, raw, imported);
    dxmt9::perf::countCommandChunkWire(
        D9C_COMMAND_CHUNK_VERSION_V2, chunk->recordCount,
        chunk->recordBytes, chunk->handleCount);

    if (auto* q = findDirtyQueue(d)) {
      q->noteCommitChunkEntryForCompletionGap();
    }
    if (dxmt9::d3d9::offloadCommitReplayEnabled() &&
        !v2ChunkRequiresInlineReplay(imported)) {
      if (!d->replayOffload) {
        d->replayOffload =
            std::make_unique<dxmt9::d3d9::ReplayOffloadWorker>();
        d->replayOffload->start(d);
      }
      if (d->replayOffload->failed()) {
        dxmt9::d3d9::releaseRetainedWrappers(raw);
        dxmt9::perf::countCommandChunkV2Reject();
        return commitChunkFail("v2-offload-worker-failed");
      }
      const bool hasPresent = raw.hasPresent;
      dxmt9::perf::countOffloadReplayQueueDepth(
          static_cast<std::uint64_t>(d->replayOffload->queue().depth()));
      if (!d->replayOffload->queue().push(std::move(raw))) {
        dxmt9::d3d9::releaseRetainedWrappers(raw);
        dxmt9::perf::countCommandChunkV2Reject();
        return commitChunkFail("v2-offload-queue-stopped");
      }
      if (hasPresent) {
        ++d->presentOrdinal;
        if (auto upper = d->dev().upperDevice()) {
          upper->waitPresentOrdinalBoundary(
              d->presentOrdinal,
              d->dev().presentParameters().backBufferCount);
        }
      }
      countDurationSince(bridgeCommitStart,
                         dxmt9::perf::countOffloadCommitAppCpuTime);
      return dxmt9::core::D3D_OK;
    }

    const int32_t hr = replayResolvedV2Chunk(
        d, raw, /*pacedByPresentOrdinal=*/false);
    dxmt9::d3d9::releaseRetainedWrappers(raw);
    if (!failed(hr)) {
      const auto elapsed = std::chrono::steady_clock::now() - bridgeCommitStart;
      dxmt9::perf::countBridgeCommitLatencyNs(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed)
              .count()));
    }
    if (failed(hr)) {
      dxmt9::perf::countCommandChunkV2Reject();
    }
    return hr;
  }
}

extern "C" int32_t dxmt9c_device_draw_primitive_packet(D9CDevice* d,
                                                       const D9CDrawPrimitivePacket* packet) {
  if (!d || !packet) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  return applyDrawPrimitivePacket(d, *packet);
}

extern "C" int32_t dxmt9c_device_draw_primitive_chunk(D9CDevice* d,
                                                      const D9CDrawPrimitivePacket* packets,
                                                      uint32_t packetCount) {
  if (!d || (!packets && packetCount != 0)) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  for (uint32_t i = 0; i < packetCount; ++i) {
    const int32_t hr = applyDrawPrimitivePacket(d, packets[i]);
    if (failed(hr)) {
      return hr;
    }
  }
  return dxmt9::core::D3D_OK;
}
