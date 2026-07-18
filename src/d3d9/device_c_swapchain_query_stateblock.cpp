#include "device_c_provider.hpp"

// Needed so dxmt9c_stateblock_apply can mark the per-frequency uniform
// DirtyState on the queue. A state-block apply is bulk state mutation
// that bypasses per-record dirty marking; flagging every DirtyBit
// matches Wine's "stateblock apply re-derives derived state" oracle.
#include "dxmt9/dxmt9_device.hpp"

using namespace dxmt9::d3d9::devicec;

namespace {

std::optional<dxmt9::core::StateBlockType> stateBlockTypeFromD3D(uint32_t type) {
  switch (type) {
    case 1:
      return dxmt9::core::StateBlockType::All;
    case 2:
      return dxmt9::core::StateBlockType::PixelState;
    case 3:
      return dxmt9::core::StateBlockType::VertexState;
    default:
      return std::nullopt;
  }
}

std::optional<dxmt9::core::QueryType> queryTypeFromD3D(uint32_t type) {
  switch (type) {
    case 8:
      return dxmt9::core::QueryType::Event;
    case 9:
      return dxmt9::core::QueryType::Occlusion;
    case 10:
      return dxmt9::core::QueryType::Timestamp;
    case 11:
      return dxmt9::core::QueryType::TimestampDisjoint;
    case 12:
      return dxmt9::core::QueryType::TimestampFreq;
    default:
      return std::nullopt;
  }
}

}  // namespace

extern "C" D9CSwapChain* dxmt9c_device_get_swap_chain(D9CDevice* d, uint32_t idx) {
  auto* swapChain = d->iface->GetSwapChain(idx);
  if (!swapChain) {
    return nullptr;
  }
  auto* out = new D9CSwapChain(swapChain);
  out->owner = d;
  return out;
}

extern "C" uint32_t dxmt9c_device_get_swap_chain_count(D9CDevice* d) {
  return static_cast<uint32_t>(d->iface->GetSwapChainCount());
}

extern "C" D9CSwapChain* dxmt9c_device_create_additional_swap_chain(D9CDevice* d,
                                                                    const D9CPresentParams* pp) {
  if (!pp) {
    return nullptr;
  }
  auto* swapChain = d->iface->CreateAdditionalSwapChain(ppFromC(*pp));
  if (!swapChain) {
    return nullptr;
  }
  auto* out = new D9CSwapChain(swapChain);
  out->owner = d;
  return out;
}

extern "C" D9CQuery* dxmt9c_device_create_query(D9CDevice* d, uint32_t type) {
  const auto queryType = queryTypeFromD3D(type);
  if (!queryType) {
    return nullptr;
  }
  auto query = d->iface->CreateQuery(*queryType);
  if (!query) {
    return nullptr;
  }
  return new D9CQuery{query, d};
}

extern "C" D9CStateBlock* dxmt9c_device_create_state_block(D9CDevice* d, uint32_t type) {
  if (!d || d->stateBlockRecording) {
    return nullptr;
  }
  const auto stateBlockType = stateBlockTypeFromD3D(type);
  if (!stateBlockType) {
    return nullptr;
  }
  auto stateBlock = d->dev().createStateBlock(*stateBlockType);
  if (!stateBlock) {
    return nullptr;
  }
  return new D9CStateBlock{stateBlock, d};
}

extern "C" int32_t dxmt9c_device_begin_state_block(D9CDevice* d) {
  if (!d || d->stateBlockRecording) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  d->stateBlockBaseState = d->dev().state();
  d->stateBlockRenderStates.clear();
  d->stateBlockRenderStateValues.clear();
  d->stateBlockRecording = true;
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_device_end_state_block(D9CDevice* d, D9CStateBlock** out) {
  if (!out) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  *out = nullptr;
  if (!d || !d->stateBlockRecording) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  d->stateBlockRecording = false;
  if (!d->stateBlockBaseState.has_value()) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  auto recordedState = *d->stateBlockBaseState;
  for (const auto& [state, value] : d->stateBlockRenderStateValues) {
    recordedState.renderStates[state] = value;
  }

  auto stateBlock = std::make_shared<dxmt9::core::StateBlock>();
  stateBlock->captureDelta(*d->stateBlockBaseState, recordedState, d->stateBlockRenderStates);
  d->stateBlockBaseState.reset();
  d->stateBlockRenderStates.clear();
  d->stateBlockRenderStateValues.clear();
  *out = new D9CStateBlock{stateBlock, d};
  return dxmt9::core::D3D_OK;
}

extern "C" void dxmt9c_swapchain_addref(D9CSwapChain* s) {
  if (s) {
    s->refs.fetch_add(1);
  }
}

extern "C" uint32_t dxmt9c_swapchain_release(D9CSwapChain* s) {
  if (!s) {
    return 0;
  }
  const uint32_t refs = s->refs.fetch_sub(1) - 1;
  if (refs == 0) {
    delete s;
  }
  return refs;
}

extern "C" int32_t dxmt9c_swapchain_present(D9CSwapChain* s, const D9CRect*, const D9CRect*,
                                            uint64_t, const void*, uint32_t) {
  return s->iface->Present();
}

extern "C" D9CSurface* dxmt9c_swapchain_get_back_buffer(D9CSwapChain* s, uint32_t, uint32_t) {
  auto surface = s->iface->backBuffer();
  if (!surface) {
    return nullptr;
  }
  return new D9CSurface{surface, nullptr, 0u, s->owner};
}

extern "C" D9CSurface* dxmt9c_swapchain_get_depth_stencil(D9CSwapChain* s) {
  auto surface = s->iface->depthStencilSurface();
  if (!surface) {
    return nullptr;
  }
  return new D9CSurface{surface, nullptr, 0u, s->owner};
}

extern "C" int32_t dxmt9c_swapchain_get_present_params(D9CSwapChain* s, D9CPresentParams* out) {
  if (!out) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  auto& params = s->iface->presentParameters();
  std::memset(out, 0, sizeof(*out));
  out->backBufferWidth = params.backBufferWidth;
  out->backBufferHeight = params.backBufferHeight;
  out->backBufferFormat = fmtToD3D(params.backBufferFormat);
  out->backBufferCount = params.backBufferCount;
  out->multiSampleType = msTypeToD3D(params.multiSampleType);
  out->swapEffect = params.swapEffect;
  out->deviceWindow = params.deviceWindow.value;
  out->windowed = params.windowed;
  out->enableAutoDepthStencil = params.enableAutoDepthStencil;
  out->autoDepthStencilFormat = fmtToD3D(params.autoDepthStencilFormat);
  out->presentationInterval = params.presentationIntervalRaw;
  return dxmt9::core::D3D_OK;
}

extern "C" void dxmt9c_query_addref(D9CQuery* q) {
  if (q) {
    q->refs.fetch_add(1);
  }
}

extern "C" uint32_t dxmt9c_query_release(D9CQuery* q) {
  if (!q) {
    return 0;
  }
  const uint32_t refs = q->refs.fetch_sub(1) - 1;
  if (refs == 0) {
    delete q;
  }
  return refs;
}

extern "C" int32_t dxmt9c_query_issue(D9CQuery* q, uint32_t flags) {
  return q->device->iface->IssueQuery(q->obj, (flags & 2) != 0);
}

extern "C" int32_t dxmt9c_query_get_data(D9CQuery* q, void* data, uint32_t size, uint32_t flags) {
  return q->device->iface->GetQueryData(q->obj, data, size, flags);
}

extern "C" uint32_t dxmt9c_query_get_data_size(D9CQuery* q) {
  switch (q->obj->type()) {
    case dxmt9::core::QueryType::Occlusion:
      return 4;
    case dxmt9::core::QueryType::Timestamp:
    case dxmt9::core::QueryType::TimestampFreq:
      return 8;
    case dxmt9::core::QueryType::Event:
    case dxmt9::core::QueryType::TimestampDisjoint:
      return 4;
    default:
      return 0;
  }
}

extern "C" uint32_t dxmt9c_query_get_type(D9CQuery* q) {
  switch (q->obj->type()) {
    case dxmt9::core::QueryType::Event: return 8;
    case dxmt9::core::QueryType::Occlusion: return 9;
    case dxmt9::core::QueryType::Timestamp: return 10;
    case dxmt9::core::QueryType::TimestampDisjoint: return 11;
    case dxmt9::core::QueryType::TimestampFreq: return 12;
  }
  return 0;
}

extern "C" void dxmt9c_stateblock_addref(D9CStateBlock* s) {
  if (s) {
    s->refs.fetch_add(1);
  }
}

extern "C" uint32_t dxmt9c_stateblock_release(D9CStateBlock* s) {
  if (!s) {
    return 0;
  }
  const uint32_t refs = s->refs.fetch_sub(1) - 1;
  if (refs == 0) {
    delete s;
  }
  return refs;
}

extern "C" int32_t dxmt9c_stateblock_capture(D9CStateBlock* s) {
  if (!s || !s->device || s->device->stateBlockRecording) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  s->obj->capture(s->device->dev().state());
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_stateblock_apply(D9CStateBlock* s) {
  if (!s || !s->device || s->device->stateBlockRecording) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  s->obj->apply(s->device->dev());
  // Derived-cache invalidation: the d3d9::core::Device flat
  // drawStateCache_{WithIndex,NoIndex} are already invalidated through
  // the mutableState() accessor inside StateBlock::apply, but the
  // dxmt9::CommandQueue's pendingDirty_ accumulator (per-frequency
  // uniform DirtyState) sits one layer above and must also be flagged
  // — otherwise the next encode chunk would observe a stale "no
  // uniforms changed" hint and skip re-uploading FFP/PSO uniforms that
  // the bulk state mutation altered. Mirrors Wine d3d9/stateblock.c
  // wined3d_stateblock_apply semantics.
  if (auto upper = s->device->dev().upperDevice()) {
    upper->queue().markPendingDirtyAll();
  }
  const auto& state = s->device->dev().state();
  const auto renderStateValue = [&](uint32_t key) -> uint32_t {
    const auto it = state.renderStates.find(key);
    return it != state.renderStates.end() ? it->second : 0u;
  };
  dxmt9DebugLog(
      "stateblock_apply device=%p alphaBlend=%u srcBlend=%u dstBlend=%u alphaTest=%u "
      "alphaFunc=%u alphaRef=%u lighting=%u colorOp0=%u alphaOp0=%u",
      static_cast<void*>(s->device), renderStateValue(dxmt9::core::RS_ALPHABLEND_ENABLE),
      renderStateValue(dxmt9::core::RS_SRC_BLEND),
      renderStateValue(dxmt9::core::RS_DEST_BLEND),
      renderStateValue(dxmt9::core::RS_ALPHA_TEST_ENABLE),
      renderStateValue(dxmt9::core::RS_ALPHA_FUNC),
      renderStateValue(dxmt9::core::RS_ALPHA_REF),
      renderStateValue(dxmt9::core::RS_LIGHTING),
      state.textureStageStates[0].contains(dxmt9::core::TSS_COLOR_OP)
          ? state.textureStageStates[0].at(dxmt9::core::TSS_COLOR_OP)
          : 0u,
      state.textureStageStates[0].contains(dxmt9::core::TSS_ALPHA_OP)
          ? state.textureStageStates[0].at(dxmt9::core::TSS_ALPHA_OP)
          : 0u);
  return dxmt9::core::D3D_OK;
}
