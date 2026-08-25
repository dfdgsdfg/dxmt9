#include "device_c_provider_api.hpp"
#include "device_c_replay_offload.hpp"

#define DXMT9_DRAIN_OR_RETURN(...)                                      \
  do {                                                                  \
    if (!dxmt9::d3d9::drainDeferredReplay(__VA_ARGS__)) {               \
      return dxmt9::d3d9::ReplayDrainFailure{};                         \
    }                                                                   \
  } while (false)
#define DXMT9_TERMINAL_OR_RETURN(owner)                                 \
  do {                                                                  \
    if (dxmt9::d3d9::replayTerminal(owner)) {                            \
      return dxmt9::d3d9::ReplayDrainFailure{};                         \
    }                                                                   \
  } while (false)

extern "C" D9CSwapChain* dxmt9c_device_get_swap_chain(D9CDevice* arg0, uint32_t index) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_get_swap_chain");
  return dxmt9p_device_get_swap_chain(arg0, index);
}

extern "C" uint32_t dxmt9c_device_get_swap_chain_count(D9CDevice* arg0) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_get_swap_chain_count");
  return dxmt9p_device_get_swap_chain_count(arg0);
}

extern "C" D9CSwapChain* dxmt9c_device_create_additional_swap_chain(D9CDevice* arg0, const D9CPresentParams* arg1) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_create_additional_swap_chain");
  return dxmt9p_device_create_additional_swap_chain(arg0, arg1);
}

extern "C" int32_t dxmt9c_swapchain_adopt_wsi_surface(
    D9CSwapChain* arg0, const D9CWsiSurfaceBinding* arg1) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_swapchain_adopt_wsi_surface");
  return dxmt9p_swapchain_adopt_wsi_surface(arg0, arg1);
}

extern "C" int32_t dxmt9c_swapchain_teardown_wsi_surface(D9CSwapChain* arg0) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_swapchain_teardown_wsi_surface");
  return dxmt9p_swapchain_teardown_wsi_surface(arg0);
}

extern "C" D9CQuery* dxmt9c_device_create_query(D9CDevice* arg0, uint32_t type) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_create_query");
  return dxmt9p_device_create_query(arg0, type);
}

extern "C" D9CStateBlock* dxmt9c_device_create_state_block(D9CDevice* arg0, uint32_t type) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_create_state_block");
  return dxmt9p_device_create_state_block(arg0, type);
}

extern "C" int32_t dxmt9c_device_begin_state_block(D9CDevice* arg0) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_begin_state_block");
  return dxmt9p_device_begin_state_block(arg0);
}

extern "C" int32_t dxmt9c_device_end_state_block(D9CDevice* arg0, D9CStateBlock** arg1) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_device_end_state_block");
  return dxmt9p_device_end_state_block(arg0, arg1);
}

// Lifetime-only addref/release calls in this file deliberately remain
// reachable after terminal publication so fail-stop cannot prevent teardown.
extern "C" void dxmt9c_swapchain_addref(D9CSwapChain* arg0) {
  dxmt9p_swapchain_addref(arg0);
}

extern "C" uint32_t dxmt9c_swapchain_release(D9CSwapChain* arg0) {
  return dxmt9p_swapchain_release(arg0);
}

extern "C" int32_t dxmt9c_swapchain_present(D9CSwapChain* arg0, const D9CRect* src, const D9CRect* dst, uint64_t destWindow, const void* dirtyRegion, uint32_t flags) {
  // D9CSwapChain is opaque here (this TU only sees the ABI-facing
  // dxmt9/device_c.h), so this calls the D9CSwapChain* overload of
  // drainDeferredReplay -- defined in device_c_replay_offload.cpp, which
  // resolves the `owner` backpointer set at swapchain creation (see
  // device_c_common.hpp) and fences through it. GT1's PE path presents via
  // the PRESENT record (already ordered in-queue by commit_chunk), so this
  // only protects the alternate direct-present path.
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_swapchain_present");
  return dxmt9p_swapchain_present(arg0, src, dst, destWindow, dirtyRegion, flags);
}

extern "C" D9CSurface* dxmt9c_swapchain_get_back_buffer(D9CSwapChain* arg0, uint32_t index, uint32_t type) {
  DXMT9_TERMINAL_OR_RETURN(arg0);
  return dxmt9p_swapchain_get_back_buffer(arg0, index, type);
}

extern "C" D9CSurface* dxmt9c_swapchain_get_depth_stencil(D9CSwapChain* arg0) {
  DXMT9_TERMINAL_OR_RETURN(arg0);
  return dxmt9p_swapchain_get_depth_stencil(arg0);
}

extern "C" int32_t dxmt9c_swapchain_get_present_params(D9CSwapChain* arg0, D9CPresentParams* arg1) {
  DXMT9_TERMINAL_OR_RETURN(arg0);
  return dxmt9p_swapchain_get_present_params(arg0, arg1);
}

extern "C" void dxmt9c_query_addref(D9CQuery* arg0) {
  dxmt9p_query_addref(arg0);
}

extern "C" uint32_t dxmt9c_query_release(D9CQuery* arg0) {
  return dxmt9p_query_release(arg0);
}

extern "C" int32_t dxmt9c_query_get_wire_identity(
    D9CQuery* arg0, D9CWireObjectIdentity* out) {
  DXMT9_TERMINAL_OR_RETURN(arg0);
  return dxmt9p_query_get_wire_identity(arg0, out);
}

extern "C" int32_t dxmt9c_query_issue(D9CQuery* arg0, uint32_t flags) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_query_issue");
  return dxmt9p_query_issue(arg0, flags);
}

extern "C" int32_t dxmt9c_query_get_data(D9CQuery* arg0, void* data, uint32_t size, uint32_t flags) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_query_get_data");
  return dxmt9p_query_get_data(arg0, data, size, flags);
}

extern "C" uint32_t dxmt9c_query_get_data_size(D9CQuery* arg0) {
  DXMT9_TERMINAL_OR_RETURN(arg0);
  return dxmt9p_query_get_data_size(arg0);
}

extern "C" uint32_t dxmt9c_query_get_type(D9CQuery* arg0) {
  DXMT9_TERMINAL_OR_RETURN(arg0);
  return dxmt9p_query_get_type(arg0);
}

extern "C" void dxmt9c_stateblock_addref(D9CStateBlock* arg0) {
  dxmt9p_stateblock_addref(arg0);
}

extern "C" uint32_t dxmt9c_stateblock_release(D9CStateBlock* arg0) {
  return dxmt9p_stateblock_release(arg0);
}

extern "C" int32_t dxmt9c_stateblock_capture(D9CStateBlock* arg0) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_stateblock_capture");
  return dxmt9p_stateblock_capture(arg0);
}

extern "C" int32_t dxmt9c_stateblock_apply(D9CStateBlock* arg0) {
  DXMT9_DRAIN_OR_RETURN(arg0, "dxmt9c_stateblock_apply");
  return dxmt9p_stateblock_apply(arg0);
}

#undef DXMT9_TERMINAL_OR_RETURN
#undef DXMT9_DRAIN_OR_RETURN
