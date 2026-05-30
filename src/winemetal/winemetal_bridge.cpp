/* src/winemetal/winemetal_bridge.cpp — PE bridge bootstrap for the single
 * winemetal.so unixlib path.
 *
 * winemetal.so is the single unixlib root. It hosts the WMT wrapper surface,
 * shader-service handlers, and generated device_c provider/runtime thunks.
 * This PE bridge resolves Wine's unix-call dispatcher and forwards all
 * winemetal.dll exports into the paired builtin unixlib table.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <cstdarg>
#include <cstdint>
#include <cwchar>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#ifndef DXMT9_BRIDGE_PERF_COUNTERS
#define DXMT9_BRIDGE_PERF_COUNTERS 1
#endif

#if DXMT9_BRIDGE_PERF_COUNTERS
#include <array>
#include <atomic>
#include <chrono>
#endif

#include "util/dynamic_symbol.hpp"
#include "util/log/log.hpp"
#include "dxmt9/wineunixlib.h"
#include "dxmt9_bridge_ops.generated.h"

namespace {

using WineUnloadUnixLibFn = NTSTATUS (WINAPI *)(unixlib_module_t lib);
using WineUnixCallDispatcherVar = NTSTATUS (WINAPI *)(unixlib_handle_t handle,
                                                      unsigned int code,
                                                      void *args);
using WineInitUnixCallFn = NTSTATUS (WINAPI *)(void);
using NtQueryVirtualMemoryFn = NTSTATUS (WINAPI *)(HANDLE process,
                                                   const void *base_address,
                                                   ULONG info_class,
                                                   void *buffer,
                                                   SIZE_T size,
                                                   SIZE_T *result_size);
using RtlDosPathNameToNtPathNameWithStatusFn = NTSTATUS (WINAPI *)(PCWSTR dos_name,
                                                                    UNICODE_STRING *nt_name,
                                                                    PWSTR *file_part,
                                                                    void *curdir);
using RtlFreeUnicodeStringFn = void (WINAPI *)(UNICODE_STRING *string);

constexpr ULONG kMemoryWineLoadUnixLib = 1000;
constexpr ULONG kMemoryWineLoadUnixLibByName = 1002;

enum class BridgeLocatorMode {
  Builtin,
  AppLocal,
};

extern "C" IMAGE_DOS_HEADER __ImageBase;

HMODULE bridgeModuleHandle() {
  HMODULE module = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&bridgeModuleHandle),
                         &module) && module) {
    return module;
  }
  return reinterpret_cast<HMODULE>(&__ImageBase);
}

#if DXMT9_BRIDGE_PERF_COUNTERS
enum class BridgeClass : std::size_t {
  Factory,
  Lifecycle,
  State,
  Draw,
  Surface,
  Present,
  FrameWait,
  Resource,
  Shader,
  Query,
  StateBlock,
  Other,
  Count,
};

enum class BridgeDetail : std::size_t {
  SetRenderState,
  SetTexture,
  SetStreamSource,
  SetFVF,
  CommitChunk,
  DrawPrimitive,
  DrawPrimitivePacket,
  DrawPrimitiveChunk,
  DrawIndexedPrimitive,
  DrawPrimitiveUP,
  DrawIndexedPrimitiveUP,
  Present,
  SwapchainPresent,
  Clear,
  ResourceCreate,
  ResourceLock,
  ResourceUnlock,
  QueryGetData,
  FrameWait,
  ShaderCompile,
  Count,
};
#endif  // DXMT9_BRIDGE_PERF_COUNTERS

struct BridgeState {
  std::once_flag initialized;
  const WCHAR* module_name = nullptr;
  BridgeLocatorMode locator_mode = BridgeLocatorMode::AppLocal;
  HMODULE ntdll = nullptr;
  WineUnloadUnixLibFn unload_unix_lib = nullptr;
  WineUnixCallDispatcherVar dispatcher = nullptr;
  WineUnixCallDispatcherVar fallback_dispatcher = nullptr;
  WineInitUnixCallFn init_unix_call = nullptr;
  NtQueryVirtualMemoryFn nt_query_virtual_memory = nullptr;
  RtlDosPathNameToNtPathNameWithStatusFn rtl_dos_to_nt_path = nullptr;
  RtlFreeUnicodeStringFn rtl_free_unicode_string = nullptr;
  unixlib_handle_t* unixlib_handle_ptr = nullptr;
  unixlib_module_t module = 0;
  unixlib_handle_t handle = 0;
  unixlib_handle_t fallback_handle = 0;
  NTSTATUS status = DXMT9_STATUS_NOT_SUPPORTED;
};

#if DXMT9_BRIDGE_PERF_COUNTERS
struct BridgePerfBucket {
  std::atomic<std::uint64_t> calls{0};
  std::atomic<std::uint64_t> ns{0};
  std::atomic<std::uint64_t> maxNs{0};
};

struct BridgePerfCounters {
  BridgePerfBucket total{};
  std::array<BridgePerfBucket, static_cast<std::size_t>(BridgeClass::Count)> classes{};
  std::array<BridgePerfBucket, static_cast<std::size_t>(BridgeDetail::Count)> details{};
};
#endif  // DXMT9_BRIDGE_PERF_COUNTERS

BridgeState& winemetalUnixBridgeState() {
  static BridgeState state{};
  state.module_name = L"winemetal.so";
#if defined(DXMT9_WINE_BUILTIN_DLL)
  state.locator_mode = BridgeLocatorMode::Builtin;
#else
  state.locator_mode = BridgeLocatorMode::AppLocal;
#endif
  return state;
}

#if DXMT9_BRIDGE_PERF_COUNTERS
BridgePerfCounters& bridgePerfCounters() {
  static BridgePerfCounters value{};
  return value;
}

bool bridgePerfEnabledFlag() {
  static const bool value = [] {
    const char* env = std::getenv("DXMT_PERF_COUNTERS");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return value;
}

const char* bridgeClassName(BridgeClass klass) {
  switch (klass) {
  case BridgeClass::Factory: return "factory";
  case BridgeClass::Lifecycle: return "lifecycle";
  case BridgeClass::State: return "state";
  case BridgeClass::Draw: return "draw";
  case BridgeClass::Surface: return "surface";
  case BridgeClass::Present: return "present";
  case BridgeClass::FrameWait: return "frame_wait";
  case BridgeClass::Resource: return "resource";
  case BridgeClass::Shader: return "shader";
  case BridgeClass::Query: return "query";
  case BridgeClass::StateBlock: return "stateblock";
  case BridgeClass::Other: return "other";
  case BridgeClass::Count: break;
  }
  return "unknown";
}

const char* bridgeDetailName(BridgeDetail detail) {
  switch (detail) {
  case BridgeDetail::SetRenderState: return "set_render_state";
  case BridgeDetail::SetTexture: return "set_texture";
  case BridgeDetail::SetStreamSource: return "set_stream_source";
  case BridgeDetail::SetFVF: return "set_fvf";
  case BridgeDetail::CommitChunk: return "commit_chunk";
  case BridgeDetail::DrawPrimitive: return "draw_primitive";
  case BridgeDetail::DrawPrimitivePacket: return "draw_primitive_packet";
  case BridgeDetail::DrawPrimitiveChunk: return "draw_primitive_chunk";
  case BridgeDetail::DrawIndexedPrimitive: return "draw_indexed_primitive";
  case BridgeDetail::DrawPrimitiveUP: return "draw_primitive_up";
  case BridgeDetail::DrawIndexedPrimitiveUP: return "draw_indexed_primitive_up";
  case BridgeDetail::Present: return "present_call";
  case BridgeDetail::SwapchainPresent: return "swapchain_present";
  case BridgeDetail::Clear: return "clear";
  case BridgeDetail::ResourceCreate: return "resource_create";
  case BridgeDetail::ResourceLock: return "resource_lock";
  case BridgeDetail::ResourceUnlock: return "resource_unlock";
  case BridgeDetail::QueryGetData: return "query_get_data";
  case BridgeDetail::FrameWait: return "frame_wait_call";
  case BridgeDetail::ShaderCompile: return "shader_compile";
  case BridgeDetail::Count: break;
  }
  return "unknown";
}

void updateMax(std::atomic<std::uint64_t>& counter, std::uint64_t value) {
  auto current = counter.load(std::memory_order_relaxed);
  while (current < value &&
         !counter.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
  }
}

void addBridgePerf(BridgePerfBucket& bucket, std::uint64_t nanoseconds) {
  bucket.calls.fetch_add(1, std::memory_order_relaxed);
  bucket.ns.fetch_add(nanoseconds, std::memory_order_relaxed);
  updateMax(bucket.maxNs, nanoseconds);
}

BridgeClass classifyBridgeClass(unsigned int code) {
  if (code < DXMT9_WINEMETAL_BRIDGE_OP_BASE) {
    return BridgeClass::Shader;
  }

  using dxmt9::bridge::BridgeOpcode;
  switch (static_cast<BridgeOpcode>(code)) {
  case BridgeOpcode::dxmt9c_factory_create:
  case BridgeOpcode::dxmt9c_factory_addref:
  case BridgeOpcode::dxmt9c_factory_release:
  case BridgeOpcode::dxmt9c_factory_adapter_count:
  case BridgeOpcode::dxmt9c_factory_get_adapter_identifier:
  case BridgeOpcode::dxmt9c_factory_get_adapter_mode_count:
  case BridgeOpcode::dxmt9c_factory_enum_adapter_modes:
  case BridgeOpcode::dxmt9c_factory_get_adapter_display_mode:
  case BridgeOpcode::dxmt9c_factory_get_adapter_monitor:
  case BridgeOpcode::dxmt9c_factory_check_device_type:
  case BridgeOpcode::dxmt9c_factory_check_device_format:
  case BridgeOpcode::dxmt9c_factory_check_device_format2:
  case BridgeOpcode::dxmt9c_factory_check_device_multisample:
  case BridgeOpcode::dxmt9c_factory_get_caps:
  case BridgeOpcode::dxmt9c_factory_get_adapter_luid:
  case BridgeOpcode::dxmt9c_factory_create_device:
  case BridgeOpcode::dxmt9c_factory_create_device2:
    return BridgeClass::Factory;

  case BridgeOpcode::dxmt9c_device_addref:
  case BridgeOpcode::dxmt9c_device_release:
  case BridgeOpcode::dxmt9c_device_get_caps:
  case BridgeOpcode::dxmt9c_device_test_cooperative_level:
  case BridgeOpcode::dxmt9c_device_check_device_state:
  case BridgeOpcode::dxmt9c_device_reset:
  case BridgeOpcode::dxmt9c_device_reset_ex:
  case BridgeOpcode::dxmt9c_device_begin_scene:
  case BridgeOpcode::dxmt9c_device_end_scene:
  case BridgeOpcode::dxmt9c_device_set_gamma_ramp:
  case BridgeOpcode::dxmt9c_device_check_device_multisample:
    return BridgeClass::Lifecycle;

  case BridgeOpcode::dxmt9c_device_set_viewport:
  case BridgeOpcode::dxmt9c_device_get_viewport:
  case BridgeOpcode::dxmt9c_device_set_scissor_rect:
  case BridgeOpcode::dxmt9c_device_get_scissor_rect:
  case BridgeOpcode::dxmt9c_device_set_transform:
  case BridgeOpcode::dxmt9c_device_get_transform:
  case BridgeOpcode::dxmt9c_device_set_material:
  case BridgeOpcode::dxmt9c_device_get_material:
  case BridgeOpcode::dxmt9c_device_set_light:
  case BridgeOpcode::dxmt9c_device_light_enable:
  case BridgeOpcode::dxmt9c_device_set_render_state:
  case BridgeOpcode::dxmt9c_device_get_render_state:
  case BridgeOpcode::dxmt9c_device_set_texture_stage_state:
  case BridgeOpcode::dxmt9c_device_get_texture_stage_state:
  case BridgeOpcode::dxmt9c_device_set_sampler_state:
  case BridgeOpcode::dxmt9c_device_get_sampler_state:
  case BridgeOpcode::dxmt9c_device_set_clip_plane:
  case BridgeOpcode::dxmt9c_device_get_clip_plane:
  case BridgeOpcode::dxmt9c_device_set_fvf:
  case BridgeOpcode::dxmt9c_device_get_fvf:
  case BridgeOpcode::dxmt9c_device_set_vertex_declaration:
  case BridgeOpcode::dxmt9c_device_set_stream_source:
  case BridgeOpcode::dxmt9c_device_set_stream_source_freq:
  case BridgeOpcode::dxmt9c_device_set_indices:
  case BridgeOpcode::dxmt9c_device_set_texture:
  case BridgeOpcode::dxmt9c_device_set_vertex_shader:
  case BridgeOpcode::dxmt9c_device_set_pixel_shader:
  case BridgeOpcode::dxmt9c_device_set_vs_const_f:
  case BridgeOpcode::dxmt9c_device_get_vs_const_f:
  case BridgeOpcode::dxmt9c_device_set_ps_const_f:
  case BridgeOpcode::dxmt9c_device_get_ps_const_f:
  case BridgeOpcode::dxmt9c_device_set_vs_const_i:
  case BridgeOpcode::dxmt9c_device_set_ps_const_i:
  case BridgeOpcode::dxmt9c_device_set_vs_const_b:
  case BridgeOpcode::dxmt9c_device_set_ps_const_b:
  case BridgeOpcode::dxmt9c_device_set_render_target:
  case BridgeOpcode::dxmt9c_device_get_render_target:
  case BridgeOpcode::dxmt9c_device_set_depth_stencil:
  case BridgeOpcode::dxmt9c_device_get_depth_stencil:
    return BridgeClass::State;

  case BridgeOpcode::dxmt9c_device_commit_chunk:
  case BridgeOpcode::dxmt9c_device_draw_primitive:
  case BridgeOpcode::dxmt9c_device_draw_primitive_packet:
  case BridgeOpcode::dxmt9c_device_draw_primitive_chunk:
  case BridgeOpcode::dxmt9c_device_draw_indexed_primitive:
  case BridgeOpcode::dxmt9c_device_draw_primitive_up:
  case BridgeOpcode::dxmt9c_device_draw_indexed_primitive_up:
    return BridgeClass::Draw;

  case BridgeOpcode::dxmt9c_device_clear:
  case BridgeOpcode::dxmt9c_device_update_surface:
  case BridgeOpcode::dxmt9c_device_update_texture:
  case BridgeOpcode::dxmt9c_device_stretch_rect:
  case BridgeOpcode::dxmt9c_device_color_fill:
  case BridgeOpcode::dxmt9c_device_get_render_target_data:
    return BridgeClass::Surface;

  case BridgeOpcode::dxmt9c_device_present:
  case BridgeOpcode::dxmt9c_swapchain_present:
  case BridgeOpcode::dxmt9c_device_get_swap_chain:
  case BridgeOpcode::dxmt9c_device_get_swap_chain_count:
  case BridgeOpcode::dxmt9c_device_create_additional_swap_chain:
  case BridgeOpcode::dxmt9c_swapchain_addref:
  case BridgeOpcode::dxmt9c_swapchain_release:
  case BridgeOpcode::dxmt9c_swapchain_get_back_buffer:
  case BridgeOpcode::dxmt9c_swapchain_get_depth_stencil:
  case BridgeOpcode::dxmt9c_swapchain_get_present_params:
    return BridgeClass::Present;

  case BridgeOpcode::dxmt9c_device_set_maximum_frame_latency:
  case BridgeOpcode::dxmt9c_device_get_maximum_frame_latency:
  case BridgeOpcode::dxmt9c_device_wait_for_vblank:
    return BridgeClass::FrameWait;

  case BridgeOpcode::dxmt9c_device_create_texture:
  case BridgeOpcode::dxmt9c_device_create_cube_texture:
  case BridgeOpcode::dxmt9c_device_create_volume_texture:
  case BridgeOpcode::dxmt9c_device_create_vertex_buffer:
  case BridgeOpcode::dxmt9c_device_create_index_buffer:
  case BridgeOpcode::dxmt9c_device_create_render_target:
  case BridgeOpcode::dxmt9c_device_create_depth_stencil:
  case BridgeOpcode::dxmt9c_device_create_offscreen_surface:
  case BridgeOpcode::dxmt9c_texture_addref:
  case BridgeOpcode::dxmt9c_texture_release:
  case BridgeOpcode::dxmt9c_texture_lock_rect:
  case BridgeOpcode::dxmt9c_texture_unlock_rect:
  case BridgeOpcode::dxmt9c_texture_get_surface_level:
  case BridgeOpcode::dxmt9c_texture_get_level_count:
  case BridgeOpcode::dxmt9c_texture_get_level_desc:
  case BridgeOpcode::dxmt9c_texture_generate_mip_sublevels:
  case BridgeOpcode::dxmt9c_texture_set_lod:
  case BridgeOpcode::dxmt9c_texture_sample_2d:
  case BridgeOpcode::dxmt9c_texture_set_palette:
  case BridgeOpcode::dxmt9c_buffer_addref:
  case BridgeOpcode::dxmt9c_buffer_release:
  case BridgeOpcode::dxmt9c_buffer_lock:
  case BridgeOpcode::dxmt9c_buffer_unlock:
  case BridgeOpcode::dxmt9c_buffer_get_desc:
  case BridgeOpcode::dxmt9c_surface_addref:
  case BridgeOpcode::dxmt9c_surface_release:
  case BridgeOpcode::dxmt9c_surface_lock_rect:
  case BridgeOpcode::dxmt9c_surface_unlock_rect:
  case BridgeOpcode::dxmt9c_surface_get_desc:
  case BridgeOpcode::dxmt9c_surface_get_container_texture:
    return BridgeClass::Resource;

  case BridgeOpcode::dxmt9c_device_create_vertex_shader:
  case BridgeOpcode::dxmt9c_device_create_pixel_shader:
  case BridgeOpcode::dxmt9c_device_create_vertex_declaration:
  case BridgeOpcode::dxmt9c_shader_addref:
  case BridgeOpcode::dxmt9c_shader_release:
  case BridgeOpcode::dxmt9c_shader_get_bytecode:
  case BridgeOpcode::dxmt9c_vdecl_addref:
  case BridgeOpcode::dxmt9c_vdecl_release:
  case BridgeOpcode::dxmt9c_vdecl_get_declaration:
    return BridgeClass::Shader;

  case BridgeOpcode::dxmt9c_device_create_query:
  case BridgeOpcode::dxmt9c_query_addref:
  case BridgeOpcode::dxmt9c_query_release:
  case BridgeOpcode::dxmt9c_query_issue:
  case BridgeOpcode::dxmt9c_query_get_data:
  case BridgeOpcode::dxmt9c_query_get_data_size:
  case BridgeOpcode::dxmt9c_query_get_type:
    return BridgeClass::Query;

  case BridgeOpcode::dxmt9c_device_create_state_block:
  case BridgeOpcode::dxmt9c_device_begin_state_block:
  case BridgeOpcode::dxmt9c_device_end_state_block:
  case BridgeOpcode::dxmt9c_stateblock_addref:
  case BridgeOpcode::dxmt9c_stateblock_release:
  case BridgeOpcode::dxmt9c_stateblock_capture:
  case BridgeOpcode::dxmt9c_stateblock_apply:
    return BridgeClass::StateBlock;

  case BridgeOpcode::dxmt9c_bridge_op_count:
    break;
  }
  return BridgeClass::Other;
}

bool classifyBridgeDetail(unsigned int code, BridgeDetail& detail) {
  if (code == DXMT9_WINEMETAL_CALL_COMPILE_SHADER) {
    detail = BridgeDetail::ShaderCompile;
    return true;
  }
  if (code < DXMT9_WINEMETAL_BRIDGE_OP_BASE) {
    return false;
  }

  using dxmt9::bridge::BridgeOpcode;
  switch (static_cast<BridgeOpcode>(code)) {
  case BridgeOpcode::dxmt9c_device_set_render_state:
    detail = BridgeDetail::SetRenderState;
    return true;
  case BridgeOpcode::dxmt9c_device_set_texture:
    detail = BridgeDetail::SetTexture;
    return true;
  case BridgeOpcode::dxmt9c_device_set_stream_source:
    detail = BridgeDetail::SetStreamSource;
    return true;
  case BridgeOpcode::dxmt9c_device_set_fvf:
    detail = BridgeDetail::SetFVF;
    return true;
  case BridgeOpcode::dxmt9c_device_commit_chunk:
    detail = BridgeDetail::CommitChunk;
    return true;
  case BridgeOpcode::dxmt9c_device_draw_primitive:
    detail = BridgeDetail::DrawPrimitive;
    return true;
  case BridgeOpcode::dxmt9c_device_draw_primitive_packet:
    detail = BridgeDetail::DrawPrimitivePacket;
    return true;
  case BridgeOpcode::dxmt9c_device_draw_primitive_chunk:
    detail = BridgeDetail::DrawPrimitiveChunk;
    return true;
  case BridgeOpcode::dxmt9c_device_draw_indexed_primitive:
    detail = BridgeDetail::DrawIndexedPrimitive;
    return true;
  case BridgeOpcode::dxmt9c_device_draw_primitive_up:
    detail = BridgeDetail::DrawPrimitiveUP;
    return true;
  case BridgeOpcode::dxmt9c_device_draw_indexed_primitive_up:
    detail = BridgeDetail::DrawIndexedPrimitiveUP;
    return true;
  case BridgeOpcode::dxmt9c_device_present:
    detail = BridgeDetail::Present;
    return true;
  case BridgeOpcode::dxmt9c_swapchain_present:
    detail = BridgeDetail::SwapchainPresent;
    return true;
  case BridgeOpcode::dxmt9c_device_clear:
    detail = BridgeDetail::Clear;
    return true;
  case BridgeOpcode::dxmt9c_device_create_texture:
  case BridgeOpcode::dxmt9c_device_create_cube_texture:
  case BridgeOpcode::dxmt9c_device_create_volume_texture:
  case BridgeOpcode::dxmt9c_device_create_vertex_buffer:
  case BridgeOpcode::dxmt9c_device_create_index_buffer:
  case BridgeOpcode::dxmt9c_device_create_render_target:
  case BridgeOpcode::dxmt9c_device_create_depth_stencil:
  case BridgeOpcode::dxmt9c_device_create_offscreen_surface:
    detail = BridgeDetail::ResourceCreate;
    return true;
  case BridgeOpcode::dxmt9c_texture_lock_rect:
  case BridgeOpcode::dxmt9c_texture_sample_2d:
  case BridgeOpcode::dxmt9c_buffer_lock:
  case BridgeOpcode::dxmt9c_surface_lock_rect:
    detail = BridgeDetail::ResourceLock;
    return true;
  case BridgeOpcode::dxmt9c_texture_unlock_rect:
  case BridgeOpcode::dxmt9c_buffer_unlock:
  case BridgeOpcode::dxmt9c_surface_unlock_rect:
    detail = BridgeDetail::ResourceUnlock;
    return true;
  case BridgeOpcode::dxmt9c_query_get_data:
    detail = BridgeDetail::QueryGetData;
    return true;
  case BridgeOpcode::dxmt9c_device_set_maximum_frame_latency:
  case BridgeOpcode::dxmt9c_device_get_maximum_frame_latency:
  case BridgeOpcode::dxmt9c_device_wait_for_vblank:
    detail = BridgeDetail::FrameWait;
    return true;
  default:
    return false;
  }
}

void reportBridgePerfCounters() {
  if (!bridgePerfEnabledFlag()) {
    return;
  }

  const auto load = [](const std::atomic<std::uint64_t>& value) {
    return value.load(std::memory_order_relaxed);
  };
  const auto printBucket = [&](const char* name, const BridgePerfBucket& bucket) {
    std::fprintf(stderr,
                 " bridge_%s=%llu bridge_%s_ms=%.3f bridge_%s_max_ms=%.3f",
                 name,
                 static_cast<unsigned long long>(load(bucket.calls)),
                 name,
                 static_cast<double>(load(bucket.ns)) / 1000000.0,
                 name,
                 static_cast<double>(load(bucket.maxNs)) / 1000000.0);
  };

  const BridgePerfCounters& counters = bridgePerfCounters();
  std::fprintf(stderr,
               "[dxmt9-bridge-perf] bridge_total=%llu bridge_total_ms=%.3f "
               "bridge_total_max_ms=%.3f",
               static_cast<unsigned long long>(load(counters.total.calls)),
               static_cast<double>(load(counters.total.ns)) / 1000000.0,
               static_cast<double>(load(counters.total.maxNs)) / 1000000.0);

  for (std::size_t i = 0; i < static_cast<std::size_t>(BridgeClass::Count); ++i) {
    printBucket(bridgeClassName(static_cast<BridgeClass>(i)), counters.classes[i]);
  }
  for (std::size_t i = 0; i < static_cast<std::size_t>(BridgeDetail::Count); ++i) {
    printBucket(bridgeDetailName(static_cast<BridgeDetail>(i)), counters.details[i]);
  }
  std::fprintf(stderr, "\n");
}

void ensureBridgePerfRegistered() {
  static const bool registered = [] {
    if (bridgePerfEnabledFlag()) {
      std::atexit(reportBridgePerfCounters);
    }
    return true;
  }();
  (void)registered;
}

void recordBridgePerf(unsigned int code, std::uint64_t nanoseconds) {
  ensureBridgePerfRegistered();
  if (!bridgePerfEnabledFlag()) {
    return;
  }

  BridgePerfCounters& counters = bridgePerfCounters();
  addBridgePerf(counters.total, nanoseconds);

  const BridgeClass klass = classifyBridgeClass(code);
  addBridgePerf(counters.classes[static_cast<std::size_t>(klass)], nanoseconds);

  BridgeDetail detail = BridgeDetail::Count;
  if (classifyBridgeDetail(code, detail)) {
    addBridgePerf(counters.details[static_cast<std::size_t>(detail)], nanoseconds);
  }
}
#endif  // DXMT9_BRIDGE_PERF_COUNTERS

void bridgeDebugLog(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  dxmt9::util::vlogf(dxmt9::util::LogLevel::Debug, "winemetal-bridge", fmt, args);
  va_end(args);
}

void bridgeTraceLog(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  dxmt9::util::vlogf(dxmt9::util::LogLevel::Trace, "winemetal-bridge", fmt, args);
  va_end(args);
}

template <typename T>
T resolveProc(HMODULE module, const char *name) {
  return dxmt9::util::resolveModuleSymbol<T>(reinterpret_cast<void*>(module), name);
}

NTSTATUS initializeDispatcherOnlyFallback(BridgeState& state) {
  if (!state.dispatcher || !state.nt_query_virtual_memory) {
    return DXMT9_STATUS_NOT_SUPPORTED;
  }

  // PE callers use the base info class. Wine's 32-bit wow64 ntdll translates it
  // to the host-side wow64 query and selects __wine_unix_call_wow64_funcs.
  unixlib_handle_t handle = 0;
  const HMODULE module = bridgeModuleHandle();
  const NTSTATUS status = state.nt_query_virtual_memory(GetCurrentProcess(),
                                                        reinterpret_cast<void *>(module),
                                                        kMemoryWineLoadUnixLib,
                                                        &handle,
                                                        sizeof(handle),
                                                        nullptr);
  bridgeDebugLog("builtin unixlib lookup: info=%lu status=0x%08lx handle=0x%llx",
                 static_cast<unsigned long>(kMemoryWineLoadUnixLib),
                 static_cast<unsigned long>(status),
                 static_cast<unsigned long long>(handle));
  if (status != DXMT9_STATUS_SUCCESS || !handle) {
    return status == DXMT9_STATUS_SUCCESS ? DXMT9_STATUS_DLL_NOT_FOUND : status;
  }

  state.handle = handle;
  state.module = 0;
  return DXMT9_STATUS_SUCCESS;
}

bool runtimeProviderFallbackAllowed() {
  const char* env = std::getenv("DXMT9_ALLOW_RUNTIME_PROVIDER_FALLBACK");
  return env && env[0] != '\0' && env[0] != '0';
}

UNICODE_STRING makeUnicodeString(const std::wstring& value) {
  UNICODE_STRING result{};
  const auto bytes = value.size() * sizeof(WCHAR);
  if (bytes > 0xfffc) {
    return result;
  }
  result.Length = static_cast<USHORT>(bytes);
  result.MaximumLength = static_cast<USHORT>(bytes + sizeof(WCHAR));
  result.Buffer = const_cast<WCHAR*>(value.c_str());
  return result;
}

std::wstring getEnvironmentString(const WCHAR* name) {
  const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
  if (!required) {
    return {};
  }
  std::vector<WCHAR> buffer(required);
  const DWORD length = GetEnvironmentVariableW(name, buffer.data(), required);
  if (!length || length >= required) {
    return {};
  }
  return std::wstring(buffer.data(), length);
}

std::wstring moduleSiblingPath(HMODULE module, const WCHAR* leaf) {
  constexpr DWORD kBufferLength = 32768;
  WCHAR buffer[kBufferLength] = {};
  const DWORD length = GetModuleFileNameW(module, buffer, kBufferLength);
  if (!length || length >= kBufferLength) {
    return {};
  }

  std::wstring path(buffer, length);
  const auto slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) {
    return leaf;
  }
  path.resize(slash + 1);
  path += leaf;
  return path;
}

NTSTATUS loadUnixlibByName(BridgeState& state, const UNICODE_STRING& name, const char* label) {
  if (!state.dispatcher || !state.nt_query_virtual_memory) {
    return DXMT9_STATUS_NOT_SUPPORTED;
  }
  if (!name.Buffer) {
    return DXMT9_STATUS_INVALID_PARAMETER;
  }

  UINT64 result[2] = {};
  const NTSTATUS status = state.nt_query_virtual_memory(GetCurrentProcess(),
                                                        &name,
                                                        kMemoryWineLoadUnixLibByName,
                                                        result,
                                                        sizeof(result),
                                                        nullptr);
  bridgeDebugLog("provider candidate[%s]: info=%lu name=%ls status=0x%08lx module=0x%llx handle=0x%llx",
                 label,
                 static_cast<unsigned long>(kMemoryWineLoadUnixLibByName),
                 name.Buffer ? name.Buffer : L"(null)",
                 static_cast<unsigned long>(status),
                 static_cast<unsigned long long>(result[0]),
                 static_cast<unsigned long long>(result[1]));
  if (status != DXMT9_STATUS_SUCCESS || !result[1]) {
    return status == DXMT9_STATUS_SUCCESS ? DXMT9_STATUS_DLL_NOT_FOUND : status;
  }

  state.module = static_cast<unixlib_module_t>(result[0]);
  state.handle = static_cast<unixlib_handle_t>(result[1]);
  return DXMT9_STATUS_SUCCESS;
}

NTSTATUS loadUnixlibExplicitPath(BridgeState& state, const std::wstring& path, const char* label) {
  if (path.empty()) {
    return DXMT9_STATUS_DLL_NOT_FOUND;
  }

  UNICODE_STRING nt_name{};
  bool converted = false;
  if (state.rtl_dos_to_nt_path) {
    const NTSTATUS convert_status = state.rtl_dos_to_nt_path(path.c_str(), &nt_name, nullptr, nullptr);
    bridgeDebugLog("provider candidate[%s]: path=%ls nt-convert-status=0x%08lx",
                   label,
                   path.c_str(),
                   static_cast<unsigned long>(convert_status));
    converted = convert_status == DXMT9_STATUS_SUCCESS;
  }

  if (!converted) {
    nt_name = makeUnicodeString(path);
  }

  const NTSTATUS status = loadUnixlibByName(state, nt_name, label);
  if (converted && state.rtl_free_unicode_string) {
    state.rtl_free_unicode_string(&nt_name);
  }
  return status;
}

NTSTATUS loadAppLocalUnixlib(BridgeState& state) {
  NTSTATUS last_status = DXMT9_STATUS_DLL_NOT_FOUND;

  const std::wstring env_path = getEnvironmentString(L"DXMT9_WINEMETAL_SO");
  if (!env_path.empty()) {
    last_status = loadUnixlibExplicitPath(state, env_path, "env");
    if (last_status == DXMT9_STATUS_SUCCESS) {
      return last_status;
    }
  }

  const std::wstring module_path =
      moduleSiblingPath(bridgeModuleHandle(), L"winemetal.so");
  last_status = loadUnixlibExplicitPath(state, module_path, "module-dir");
  if (last_status == DXMT9_STATUS_SUCCESS) {
    return last_status;
  }

  const std::wstring exe_path = moduleSiblingPath(nullptr, L"winemetal.so");
  last_status = loadUnixlibExplicitPath(state, exe_path, "exe-dir");
  if (last_status == DXMT9_STATUS_SUCCESS) {
    return last_status;
  }

  if (runtimeProviderFallbackAllowed()) {
    const std::wstring name = L"winemetal.so";
    const UNICODE_STRING unixlib_name = makeUnicodeString(name);
    last_status = loadUnixlibByName(state, unixlib_name, "runtime-by-name");
    if (last_status == DXMT9_STATUS_SUCCESS) {
      return last_status;
    }
  } else {
    bridgeDebugLog("provider candidate[runtime-by-name]: skipped; DXMT9_ALLOW_RUNTIME_PROVIDER_FALLBACK is not set");
  }

  return last_status;
}

void initializeBridgeState(BridgeState& state) {
  state.ntdll = GetModuleHandleW(L"ntdll.dll");
  bridgeDebugLog("initialize(%ls): ntdll=%p", state.module_name, state.ntdll);
  if (!state.ntdll) {
    state.status = DXMT9_STATUS_DLL_NOT_FOUND;
    return;
  }

  const auto dispatcher_export =
      dxmt9::util::resolveModuleSymbol<void*>(reinterpret_cast<void*>(state.ntdll), "__wine_unix_call_dispatcher");
  bridgeDebugLog("initialize(%ls): dispatcher export=%p", state.module_name, dispatcher_export);
  if (dispatcher_export) {
    state.dispatcher = *reinterpret_cast<WineUnixCallDispatcherVar *>(dispatcher_export);
    bridgeDebugLog("initialize(%ls): dispatcher=%p", state.module_name, reinterpret_cast<void*>(state.dispatcher));
  }

  state.init_unix_call = resolveProc<WineInitUnixCallFn>(state.ntdll, "__wine_init_unix_call");
  state.unixlib_handle_ptr = dxmt9::util::resolveModuleSymbol<unixlib_handle_t*>(
      reinterpret_cast<void*>(state.ntdll), "__wine_unixlib_handle");
  state.unload_unix_lib = resolveProc<WineUnloadUnixLibFn>(state.ntdll, "__wine_unload_unix_lib");
  state.nt_query_virtual_memory = resolveProc<NtQueryVirtualMemoryFn>(state.ntdll, "NtQueryVirtualMemory");
  state.rtl_dos_to_nt_path = resolveProc<RtlDosPathNameToNtPathNameWithStatusFn>(
      state.ntdll, "RtlDosPathNameToNtPathName_U_WithStatus");
  state.rtl_free_unicode_string = resolveProc<RtlFreeUnicodeStringFn>(state.ntdll, "RtlFreeUnicodeString");

  if (!state.dispatcher || !state.nt_query_virtual_memory) {
    state.status = DXMT9_STATUS_NOT_SUPPORTED;
    bridgeDebugLog("initialize(%ls): missing dispatcher=%p or NtQueryVirtualMemory=%p",
                   state.module_name,
                   reinterpret_cast<void*>(state.dispatcher),
                   reinterpret_cast<void*>(state.nt_query_virtual_memory));
    return;
  }

  if (state.locator_mode == BridgeLocatorMode::Builtin) {
    state.status = initializeDispatcherOnlyFallback(state);
    if (state.status == DXMT9_STATUS_SUCCESS) {
      bridgeDebugLog("initialize(%ls): builtin provider handle=0x%llx dispatcher=%p",
                     state.module_name,
                     static_cast<unsigned long long>(state.handle),
                     reinterpret_cast<void*>(state.dispatcher));
      return;
    }
    bridgeDebugLog("initialize(%ls): builtin lookup failed status=0x%08lx; trying app-local candidates",
                   state.module_name,
                   static_cast<unsigned long>(state.status));
  }

  state.status = loadAppLocalUnixlib(state);
  bridgeDebugLog("initialize(%ls): final status=0x%08lx module=0x%llx handle=0x%llx dispatcher=%p",
                 state.module_name,
                 static_cast<unsigned long>(state.status),
                 static_cast<unsigned long long>(state.module),
                 static_cast<unsigned long long>(state.handle),
                 reinterpret_cast<void*>(state.dispatcher));
}

void initializeWinemetalUnixBridge() {
  initializeBridgeState(winemetalUnixBridgeState());
}

NTSTATUS ensureBridgeReady(BridgeState& state, void (*initializer)()) {
  std::call_once(state.initialized, initializer);
  return state.status;
}

}  // namespace

extern "C" NTSTATUS dxmt9_winemetal_unix_call(unsigned int code, void *args) {
  auto& state = winemetalUnixBridgeState();
  const NTSTATUS status = ensureBridgeReady(state, initializeWinemetalUnixBridge);
  if (status != DXMT9_STATUS_SUCCESS) {
    bridgeDebugLog("dxmt9_winemetal_unix_call: bridge not ready status=0x%08lx",
                   static_cast<unsigned long>(status));
    return status;
  }
  bridgeTraceLog("dxmt9_winemetal_unix_call: handle=0x%llx code=%u dispatcher=%p",
                 static_cast<unsigned long long>(state.handle),
                 code,
                 reinterpret_cast<void*>(state.dispatcher));
#if DXMT9_BRIDGE_PERF_COUNTERS
  const bool record_perf = bridgePerfEnabledFlag();
  const auto start = record_perf ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
  const NTSTATUS call_status = state.dispatcher(state.handle, code, args);
  if (record_perf) {
    const auto end = std::chrono::steady_clock::now();
    recordBridgePerf(code, static_cast<std::uint64_t>(
                               std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()));
  }
#else
  const NTSTATUS call_status = state.dispatcher(state.handle, code, args);
#endif  // DXMT9_BRIDGE_PERF_COUNTERS
  if (call_status != DXMT9_STATUS_SUCCESS) {
    bridgeDebugLog("dxmt9_winemetal_unix_call: code=%u args=%p status=0x%08lx",
                   code,
                   args,
                   static_cast<unsigned long>(call_status));
  }
  return call_status;
}
