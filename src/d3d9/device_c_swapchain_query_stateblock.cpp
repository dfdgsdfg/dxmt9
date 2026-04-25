#include "device_c_provider.hpp"

using namespace dxmt9::d3d9::devicec;

extern "C" D9CSwapChain* dxmt9c_device_get_swap_chain(D9CDevice* d, uint32_t idx) {
  auto* swapChain = d->iface->GetSwapChain(idx);
  if (!swapChain) {
    return nullptr;
  }
  return new D9CSwapChain(swapChain);
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
  return new D9CSwapChain(swapChain);
}

extern "C" D9CQuery* dxmt9c_device_create_query(D9CDevice* d, uint32_t type) {
  dxmt9::core::QueryType queryType;
  switch (type) {
    case 8: queryType = dxmt9::core::QueryType::Occlusion; break;
    case 9: queryType = dxmt9::core::QueryType::Timestamp; break;
    case 10: queryType = dxmt9::core::QueryType::TimestampDisjoint; break;
    case 11: queryType = dxmt9::core::QueryType::TimestampFreq; break;
    default: queryType = dxmt9::core::QueryType::Event; break;
  }
  auto query = d->iface->CreateQuery(queryType);
  if (!query) {
    return nullptr;
  }
  return new D9CQuery{query, d};
}

extern "C" D9CStateBlock* dxmt9c_device_create_state_block(D9CDevice* d, uint32_t) {
  auto stateBlock = d->iface->CreateStateBlock();
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
  auto stateBlock = std::make_shared<dxmt9::core::StateBlock>();
  stateBlock->captureDelta(*d->stateBlockBaseState, d->dev().state(), d->stateBlockRenderStates);
  d->stateBlockBaseState.reset();
  d->stateBlockRenderStates.clear();
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
  return new D9CSurface{surface};
}

extern "C" D9CSurface* dxmt9c_swapchain_get_depth_stencil(D9CSwapChain* s) {
  auto surface = s->iface->depthStencilSurface();
  if (!surface) {
    return nullptr;
  }
  return new D9CSurface{surface};
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
  out->swapEffect = params.discardSwapEffect ? 1u : 2u;
  out->deviceWindow = params.deviceWindow.value;
  out->windowed = params.windowed;
  out->enableAutoDepthStencil = params.enableAutoDepthStencil;
  out->autoDepthStencilFormat = fmtToD3D(params.autoDepthStencilFormat);
  out->presentationInterval = presentIntervalToD3D(params.presentationInterval);
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
    case dxmt9::core::QueryType::Timestamp:
    case dxmt9::core::QueryType::TimestampFreq:
      return 8;
    default:
      return 0;
  }
}

extern "C" uint32_t dxmt9c_query_get_type(D9CQuery* q) {
  switch (q->obj->type()) {
    case dxmt9::core::QueryType::Occlusion: return 8;
    case dxmt9::core::QueryType::Timestamp: return 9;
    case dxmt9::core::QueryType::TimestampDisjoint: return 10;
    case dxmt9::core::QueryType::TimestampFreq: return 11;
    default: return 7;
  }
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
  s->obj->capture(s->device->dev().state());
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_stateblock_apply(D9CStateBlock* s) {
  s->obj->apply(s->device->dev());
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
