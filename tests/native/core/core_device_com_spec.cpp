#include "core_spec_fixtures.hpp"
#include "device_c_common.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>

using namespace dxmt9::core;
using namespace dxmt9::core::fixture;
using namespace dxmt9::core::spec;

namespace {

void testComWrappers() {
  using namespace dxmt9::com;

  auto* d3d = Direct3DCreate9(D3D_SDK_VERSION);
  check(d3d != nullptr, "Direct3DCreate9");
  checkEq(d3d->AddRef(), 2u, "factory addref");
  checkEq(d3d->Release(), 1u, "factory release after addref");
  checkEq(d3d->GetAdapterCount(), size_t{1}, "factory adapter count");
  checkEq(d3d->CheckDeviceType(0, DeviceType::Hal, Format::A8R8G8B8, Format::A8R8G8B8, true), D3D_OK,
          "factory device type");
  checkEq(d3d->CheckDeviceType(0, DeviceType::Hal, Format::A8R8G8B8, Format::A8R8G8B8, false), D3D_OK,
          "factory fullscreen device type");
  check(!d3d->EnumAdapterModes(0, Format::R8G8B8).size(), "unsupported adapter modes");
  checkEq(d3d->GetAdapterDisplayMode(0).width, 1920u, "factory adapter mode width");

  void* unknown = nullptr;
  check(d3d->QueryInterface(InterfaceId::IUnknown, &unknown), "factory query interface");
  auto* queriedFactory = static_cast<IDirect3D9*>(unknown);
  check(queriedFactory != nullptr, "factory query result");
  checkEq(queriedFactory->Release(), 1u, "factory qi release");
  void* exUnknown = nullptr;
  check(!d3d->QueryInterface(InterfaceId::Direct3D9Ex, &exUnknown), "base factory must not expose ex");

  PresentParameters params{};
  params.backBufferWidth = 320;
  params.backBufferHeight = 240;
  params.windowed = true;

  auto* device = d3d->CreateDevice(0, params);
  check(device != nullptr, "wrapper device create");
  check(device->coreDevice().swapChain() != nullptr, "wrapper core device swap chain");
  checkEq(device->GetDeviceCaps().maxTextureWidth, 16384u, "wrapper caps");
  checkEq(device->GetDeviceCaps().declTypes, 0x000003ffu, "wrapper vertex declaration type caps");
  checkEq(device->TestCooperativeLevel(), D3D_OK, "wrapper cooperative level");
  checkEq(device->GetSwapChainCount(), size_t{1}, "wrapper swap chain count");
  auto* primarySwapChain = device->GetSwapChain();
  check(primarySwapChain != nullptr, "wrapper primary swap chain");
  check(primarySwapChain->backBuffer() != nullptr, "wrapper primary back buffer");
  checkEq(primarySwapChain->Present(), D3D_OK, "wrapper primary swap present");
  checkEq(primarySwapChain->Release(), 0u, "wrapper primary swap release");
  auto* extraSwapChain = device->CreateAdditionalSwapChain(params);
  check(extraSwapChain != nullptr, "wrapper additional swap chain");
  check(extraSwapChain->backBuffer() != nullptr, "wrapper additional back buffer");
  checkEq(extraSwapChain->presentParameters().backBufferWidth, 320u, "wrapper additional params");
  checkEq(extraSwapChain->Present(), D3D_OK, "wrapper additional swap present");
  checkEq(device->GetSwapChainCount(), size_t{2}, "wrapper swap chain count after create");
  checkEq(extraSwapChain->Release(), 0u, "wrapper additional swap release");
  checkEq(device->AddRef(), 2u, "device addref");
  checkEq(device->Release(), 1u, "device release after addref");

  void* deviceUnknown = nullptr;
  check(device->QueryInterface(InterfaceId::Direct3DDevice9, &deviceUnknown), "device query interface");
  auto* queriedDevice = static_cast<IDirect3DDevice9*>(deviceUnknown);
  check(queriedDevice != nullptr, "device query result");
  checkEq(queriedDevice->Release(), 1u, "device qi release");
  check(!device->QueryInterface(InterfaceId::Direct3DDevice9Ex, &deviceUnknown),
        "base device must not expose ex");
  checkEq(device->Release(), 0u, "device release");
  checkEq(d3d->Release(), 0u, "factory release");
}

void testComWrappersEx() {
  using namespace dxmt9::com;

  auto backend = std::make_shared<RecordingBackend>();
  BackendLimits limits{};
  limits.maxTextureSize = 8192;
  limits.maxColorAttachments = 4;
  limits.maxAnisotropy = 16;
  limits.supportsBgr10A2 = true;
  limits.supportsDepth32FloatStencil8 = true;

  auto* d3d = Direct3DCreate9Ex(D3D_SDK_VERSION, backend);
  check(d3d != nullptr, "Direct3DCreate9Ex");
  checkEq(d3d->AddRef(), 2u, "factory ex addref");
  checkEq(d3d->Release(), 1u, "factory ex release after addref");

  void* exUnknown = nullptr;
  check(d3d->QueryInterface(InterfaceId::Direct3D9Ex, &exUnknown), "factory ex query interface");
  auto* queriedFactory = static_cast<IDirect3D9Ex*>(exUnknown);
  check(queriedFactory != nullptr, "factory ex query result");
  checkEq(queriedFactory->Release(), 1u, "factory ex qi release");

  checkEq(d3d->GetAdapterModeCountEx(0, nullptr), d3d->EnumAdapterModes(0, Format::A8R8G8B8).size(),
          "ex adapter mode count");
  DisplayModeFilter filter{};
  filter.format = Format::A8R8G8B8;
  DisplayModeEx mode{};
  check(d3d->EnumAdapterModesEx(0, &filter, 0, &mode), "ex enum adapter modes");
  checkEq(mode.scanLineOrdering, DisplayScanLineOrdering::Progressive, "ex scanline ordering");
  DisplayModeEx currentMode{};
  DisplayRotation rotation = DisplayRotation::Rotate90;
  check(d3d->GetAdapterDisplayModeEx(0, &currentMode, &rotation), "ex adapter display mode");
  checkEq(rotation, DisplayRotation::Identity, "ex rotation");
  Luid luid0{};
  Luid luid1{};
  check(d3d->GetAdapterLUID(0, &luid0), "ex adapter luid");
  check(d3d->GetAdapterLUID(0, &luid1), "ex adapter luid stable");
  checkEq(luid0.lowPart, luid1.lowPart, "luid low stable");
  checkEq(luid0.highPart, luid1.highPart, "luid high stable");
  check(luid0.lowPart != 0 || luid0.highPart != 0, "luid non-zero");

  PresentParameters params{};
  params.windowed = false;
  params.backBufferWidth = 1024;
  params.backBufferHeight = 768;
  params.backBufferFormat = Format::Unknown;
  params.presentationInterval = PresentInterval::Default;
  params.deviceWindow = Handle{202};
  DisplayModeEx fullscreenMode{};
  fullscreenMode.width = 1024;
  fullscreenMode.height = 768;
  fullscreenMode.format = Format::A8R8G8B8;

  auto* device = d3d->CreateDeviceEx(0, params, &fullscreenMode);
  check(device != nullptr, "wrapper ex device create");
  check(device->coreDevice().swapChain() != nullptr, "wrapper ex core device swap chain");
  void* deviceUnknown = nullptr;
  check(device->QueryInterface(InterfaceId::Direct3DDevice9Ex, &deviceUnknown), "device ex query interface");
  auto* queriedDevice = static_cast<IDirect3DDevice9Ex*>(deviceUnknown);
  check(queriedDevice != nullptr, "device ex query result");
  checkEq(queriedDevice->Release(), 1u, "device ex qi release");

  {
    device->AddRef();
    D9CDevice cDevice(device);
    check(dxmt9c_device_create_state_block(&cDevice, 0) == nullptr, "reject invalid state block type");
    auto* cStateBlock = dxmt9c_device_create_state_block(&cDevice, 1);
    check(cStateBlock != nullptr, "create c state block");
    checkEq(dxmt9c_device_begin_state_block(&cDevice), D3D_OK, "begin c state block");
    check(dxmt9c_device_create_state_block(&cDevice, 1) == nullptr, "reject create state block while recording");
    checkEq(dxmt9c_device_begin_state_block(&cDevice), D3DERR_INVALIDCALL, "reject nested state block recording");
    checkEq(dxmt9c_stateblock_capture(cStateBlock), D3DERR_INVALIDCALL, "reject state block capture while recording");
    checkEq(dxmt9c_stateblock_apply(cStateBlock), D3DERR_INVALIDCALL, "reject state block apply while recording");
    D9CStateBlock* recordedStateBlock = nullptr;
    checkEq(dxmt9c_device_end_state_block(&cDevice, &recordedStateBlock), D3D_OK, "end c state block");
    check(recordedStateBlock != nullptr, "recorded c state block");
    checkEq(dxmt9c_stateblock_release(recordedStateBlock), 0u, "release recorded c state block");
    checkEq(dxmt9c_stateblock_release(cStateBlock), 0u, "release c state block");

    auto* dxt5Texture = dxmt9c_device_create_texture(
        &cDevice, 16, 16, 1, 0, dxmt9::d3d9::devicec::fmtToD3D(Format::DXT5), 1);
    check(dxt5Texture != nullptr, "create c dxt5 texture");
    D9CLockedRect locked{};
    D9CRect partialRect{4, 4, 12, 12};
    checkEq(dxmt9c_texture_lock_rect(dxt5Texture, 0, &locked, &partialRect, 0), D3D_OK,
            "lock c dxt5 partial rect");
    check(locked.bits != nullptr, "c dxt5 partial lock bits");
    checkEq(locked.pitch, static_cast<int32_t>(formatRowPitch(Format::DXT5, 16)),
            "c dxt5 partial lock exposes level pitch");
    auto* lockedBytes = static_cast<u8*>(locked.bits);
    lockedBytes[0] = 0x7au;
    lockedBytes[static_cast<size_t>(locked.pitch)] = 0x7bu;
    checkEq(dxmt9c_texture_unlock_rect(dxt5Texture, 0), D3D_OK, "unlock c dxt5 partial rect");
    const auto dxt5Bytes = dxt5Texture->obj->levelBytes(0);
    checkEq(dxt5Bytes[80], static_cast<u8>(0x7a), "c dxt5 partial first block copied");
    checkEq(dxt5Bytes[144], static_cast<u8>(0x7b), "c dxt5 partial second block copied");

    auto* dxt5Surface = dxmt9c_texture_get_surface_level(dxt5Texture, 0);
    check(dxt5Surface != nullptr, "create c dxt5 surface level");
    D9CLockedRect surfaceLocked{};
    checkEq(dxmt9c_surface_lock_rect(dxt5Surface, &surfaceLocked, &partialRect, 0), D3D_OK,
            "lock c dxt5 surface partial rect");
    check(surfaceLocked.bits != nullptr, "c dxt5 surface partial lock bits");
    checkEq(surfaceLocked.pitch, static_cast<int32_t>(formatRowPitch(Format::DXT5, 16)),
            "c dxt5 surface partial lock exposes level pitch");
    auto* surfaceLockedBytes = static_cast<u8*>(surfaceLocked.bits);
    surfaceLockedBytes[2] = 0x4au;
    surfaceLockedBytes[static_cast<size_t>(surfaceLocked.pitch) + 2u] = 0x4bu;
    checkEq(dxmt9c_surface_unlock_rect(dxt5Surface), D3D_OK, "unlock c dxt5 surface partial rect");
    const auto dxt5SurfaceBytes = dxt5Texture->obj->levelBytes(0);
    checkEq(dxt5SurfaceBytes[82], static_cast<u8>(0x4a), "c dxt5 surface partial first block copied");
    checkEq(dxt5SurfaceBytes[146], static_cast<u8>(0x4b), "c dxt5 surface partial second block copied");
    checkEq(dxmt9c_surface_release(dxt5Surface), 0u, "release c dxt5 surface level");
    checkEq(dxmt9c_texture_release(dxt5Texture), 0u, "release c dxt5 texture");

    constexpr uint32_t d3dLockDiscard = 0x00002000u;
    auto* discardTexture = dxmt9c_device_create_texture(
        &cDevice, 4, 4, 1, 0, dxmt9::d3d9::devicec::fmtToD3D(Format::A8R8G8B8), 1);
    check(discardTexture != nullptr, "create c discard texture");
    D9CLockedRect discardLocked{};
    checkEq(dxmt9c_texture_lock_rect(discardTexture, 0, &discardLocked, nullptr, 0), D3D_OK,
            "lock c discard texture initial");
    auto* discardBytes = static_cast<u8*>(discardLocked.bits);
    discardBytes[0] = 0xabu;
    checkEq(dxmt9c_texture_unlock_rect(discardTexture, 0), D3D_OK, "unlock c discard texture initial");
    checkEq(dxmt9c_texture_lock_rect(discardTexture, 0, &discardLocked, nullptr, d3dLockDiscard), D3D_OK,
            "lock c texture with d3d discard flag");
    discardBytes = static_cast<u8*>(discardLocked.bits);
    checkEq(discardBytes[0], static_cast<u8>(0x00), "d3d discard lock maps to core discard");
    checkEq(dxmt9c_texture_unlock_rect(discardTexture, 0), D3D_OK, "unlock c discard texture");
    checkEq(dxmt9c_texture_release(discardTexture), 0u, "release c discard texture");

    auto* sampleTexture = dxmt9c_device_create_texture(
        &cDevice, 2, 2, 1, 0, dxmt9::d3d9::devicec::fmtToD3D(Format::A8R8G8B8), 1);
    check(sampleTexture != nullptr, "create c sample texture");
    D9CLockedRect sampleLocked{};
    checkEq(dxmt9c_texture_lock_rect(sampleTexture, 0, &sampleLocked, nullptr, 0), D3D_OK,
            "lock c sample texture");
    auto* samplePixels = static_cast<uint32_t*>(sampleLocked.bits);
    samplePixels[0] = 0xff112233u;
    samplePixels[1] = 0xff445566u;
    samplePixels[2] = 0xff778899u;
    samplePixels[3] = 0xffaabbccu;
    checkEq(dxmt9c_texture_unlock_rect(sampleTexture, 0), D3D_OK, "unlock c sample texture");

    const auto checkSample = [](float actual, uint32_t byte, std::string_view message) {
      const float expected = static_cast<float>(byte) / 255.0f;
      check(std::fabs(actual - expected) < 0.0001f, message);
    };
    float rgba[4]{};
    checkEq(dxmt9c_texture_sample_2d(sampleTexture, 0, 0.10f, 0.10f, rgba), D3D_OK,
            "sample c texture top-left");
    checkSample(rgba[0], 0x11u, "sample top-left red");
    checkSample(rgba[1], 0x22u, "sample top-left green");
    checkSample(rgba[2], 0x33u, "sample top-left blue");
    checkSample(rgba[3], 0xffu, "sample top-left alpha");
    checkEq(dxmt9c_texture_sample_2d(sampleTexture, 0, 0.75f, 0.10f, rgba), D3D_OK,
            "sample c texture top-right");
    checkSample(rgba[0], 0x44u, "sample top-right red");
    checkSample(rgba[1], 0x55u, "sample top-right green");
    checkSample(rgba[2], 0x66u, "sample top-right blue");
    checkEq(dxmt9c_texture_sample_2d(sampleTexture, 0, -1.0f, 2.0f, rgba), D3D_OK,
            "sample c texture clamps coords");
    checkSample(rgba[0], 0x77u, "sample clamp red");
    checkSample(rgba[1], 0x88u, "sample clamp green");
    checkSample(rgba[2], 0x99u, "sample clamp blue");
    checkEq(dxmt9c_texture_sample_2d(sampleTexture, 1, 0.0f, 0.0f, rgba), D3DERR_INVALIDCALL,
            "sample c texture rejects invalid level");
    checkEq(dxmt9c_texture_sample_2d(sampleTexture, 0, 0.0f, 0.0f, nullptr), D3DERR_INVALIDCALL,
            "sample c texture rejects null output");
    checkEq(dxmt9c_texture_release(sampleTexture), 0u, "release c sample texture");

    constexpr uint32_t d3dUsageRenderTarget = 0x00000001u;
    auto* renderTargetTexture = dxmt9c_device_create_texture(
        &cDevice, 64, 64, 1, d3dUsageRenderTarget,
        dxmt9::d3d9::devicec::fmtToD3D(Format::A8R8G8B8), 0);
    check(renderTargetTexture != nullptr, "create c render target texture");
    auto* renderTargetSurface = dxmt9c_texture_get_surface_level(renderTargetTexture, 0);
    check(renderTargetSurface != nullptr, "get c render target surface level");
    checkEq(dxmt9c_device_set_render_target(&cDevice, 0, renderTargetSurface), D3D_OK,
            "set c render target surface");
    auto* currentRenderTarget = dxmt9c_device_get_render_target(&cDevice, 0);
    check(currentRenderTarget != nullptr, "get c current render target");
    check(currentRenderTarget->obj == renderTargetSurface->obj,
          "c get render target returns currently bound surface");
    checkEq(dxmt9c_surface_release(currentRenderTarget), 0u, "release c current render target");

    checkEq(dxmt9c_device_set_render_target(&cDevice, kMaxRenderTargets, renderTargetSurface),
            D3DERR_INVALIDCALL, "reject c render target index past max");
    check(dxmt9c_device_get_render_target(&cDevice, kMaxRenderTargets) == nullptr,
          "get c render target index past max returns null");

    auto* renderTargetTexture1 = dxmt9c_device_create_texture(
        &cDevice, 64, 64, 1, d3dUsageRenderTarget,
        dxmt9::d3d9::devicec::fmtToD3D(Format::A8R8G8B8), 0);
    check(renderTargetTexture1 != nullptr, "create c render target texture slot 1");
    auto* renderTargetSurface1 = dxmt9c_texture_get_surface_level(renderTargetTexture1, 0);
    check(renderTargetSurface1 != nullptr, "get c render target surface level slot 1");
    checkEq(dxmt9c_device_set_render_target(&cDevice, 1, renderTargetSurface1), D3D_OK,
            "set c render target surface slot 1");
    auto* currentRenderTarget1 = dxmt9c_device_get_render_target(&cDevice, 1);
    check(currentRenderTarget1 != nullptr, "get c current render target slot 1");
    check(currentRenderTarget1->obj == renderTargetSurface1->obj,
          "c get render target slot 1 returns currently bound surface");
    checkEq(dxmt9c_surface_release(currentRenderTarget1), 0u, "release c current render target slot 1");

    checkEq(dxmt9c_device_set_render_target(&cDevice, 1, nullptr), D3D_OK,
            "detach c render target slot 1");
    check(dxmt9c_device_get_render_target(&cDevice, 1) == nullptr,
          "c get render target slot 1 after detach returns null");
    checkEq(dxmt9c_device_set_render_target(&cDevice, 0, nullptr), D3D_OK,
            "detach c render target slot 0");
    check(dxmt9c_device_get_render_target(&cDevice, 0) == nullptr,
          "c get render target slot 0 after explicit detach returns null");

    D9CPresentParams resetParams{};
    resetParams.backBufferWidth = 320;
    resetParams.backBufferHeight = 240;
    resetParams.backBufferFormat = dxmt9::d3d9::devicec::fmtToD3D(Format::A8R8G8B8);
    resetParams.backBufferCount = 1;
    resetParams.swapEffect = 1;
    resetParams.windowed = 1;
    resetParams.enableAutoDepthStencil = 1;
    resetParams.autoDepthStencilFormat = dxmt9::d3d9::devicec::fmtToD3D(Format::D24S8);
    checkEq(dxmt9c_device_reset(&cDevice, &resetParams), D3D_OK, "reset clears c render target cache");
    auto* currentRenderTargetAfterReset = dxmt9c_device_get_render_target(&cDevice, 0);
    check(currentRenderTargetAfterReset != nullptr, "get c render target after reset");
    check(currentRenderTargetAfterReset->obj != renderTargetSurface->obj,
          "c get render target after reset returns new backbuffer");
    checkEq(dxmt9c_surface_release(currentRenderTargetAfterReset), 0u,
            "release c current render target after reset");
    check(dxmt9c_device_get_render_target(&cDevice, 1) == nullptr,
          "c get render target slot 1 after reset returns null");

    checkEq(dxmt9c_surface_release(renderTargetSurface1), 0u, "release c render target surface slot 1");
    checkEq(dxmt9c_texture_release(renderTargetTexture1), 0u, "release c render target texture slot 1");
    checkEq(dxmt9c_surface_release(renderTargetSurface), 0u, "release c render target surface");
    checkEq(dxmt9c_texture_release(renderTargetTexture), 0u, "release c render target texture");
  }

  checkEq(device->GetMaximumFrameLatency(), 4u, "default max frame latency");
  checkEq(device->SetMaximumFrameLatency(0), D3D_OK, "set frame latency default");
  checkEq(device->GetMaximumFrameLatency(), 4u, "zero latency maps to default");
  checkEq(backend->maxFrameLatencyCalls.back(), 4u, "backend received default frame latency");
  checkEq(device->SetMaximumFrameLatency(30), D3D_OK, "set max valid frame latency");
  checkEq(device->GetMaximumFrameLatency(), 30u, "stored max valid frame latency");
  checkEq(backend->maxFrameLatencyCalls.back(), 30u, "backend received max valid frame latency");
  checkEq(device->SetMaximumFrameLatency(31), D3DERR_INVALIDCALL, "reject invalid frame latency");
  checkEq(device->GetMaximumFrameLatency(), 30u, "invalid frame latency leaves previous value");
  checkEq(backend->maxFrameLatencyCalls.back(), 30u, "backend not updated for invalid frame latency");

  checkEq(device->CheckDeviceState(params.deviceWindow), D3D_OK, "initial device state");
  backend->triggerPresentationOccluded(true);
  checkEq(device->CheckDeviceState(params.deviceWindow), S_PRESENT_OCCLUDED, "occluded device state");
  backend->triggerDeviceLost(true);
  checkEq(device->CheckDeviceState(params.deviceWindow), D3DERR_DEVICELOST, "lost beats occluded");
  checkEq(device->ResetEx(params, &fullscreenMode), D3D_OK, "device ex reset");
  checkEq(device->CheckDeviceState(params.deviceWindow), D3D_OK, "device recovered after reset");
  checkEq(device->coreDevice().presentParameters().backBufferWidth, 1024u, "reset ex width");
  checkEq(device->coreDevice().presentParameters().backBufferHeight, 768u, "reset ex height");
  const auto displayModeEx = device->GetDisplayModeEx();
  checkEq(displayModeEx.width, 1024u, "device ex display mode width");
  checkEq(displayModeEx.height, 768u, "device ex display mode height");
  checkEq(displayModeEx.scanLineOrdering, DisplayScanLineOrdering::Progressive,
          "device ex display mode scanline");

  checkEq(device->WaitForVBlank(0), D3D_OK, "wait for vblank");
  checkEq(backend->waitForVBlankCalls.size(), size_t{1}, "backend wait for vblank call");
  checkEq(device->CheckResourceResidency(), S_OK, "check resource residency");
  i32 priority = 123;
  checkEq(device->GetGPUThreadPriority(&priority), D3D_OK, "get gpu priority");
  checkEq(priority, 0, "gpu priority zero");
  checkEq(device->SetGPUThreadPriority(7), D3D_OK, "set gpu priority");
  checkEq(device->SetConvolutionMonoKernel(), E_NOTIMPL, "mono kernel not impl");
  checkEq(device->ComposeRects(), E_NOTIMPL, "compose rects not impl");

  // ── gap_d3d9 §D: regression gates for silent-S_OK COM stubs ──────────────
  // These methods are documented no-ops on Metal/Apple Silicon that match
  // Wine's S_OK contract. They have no observable side effect, so without an
  // explicit assertion a regression (e.g. an accidental E_NOTIMPL, or a stale
  // out-param) would pass silently. Pin the exact documented return contract
  // verified on master so a behavioral drift is caught here.

  // CheckResourceResidency is a no-op that ignores its resource span entirely;
  // a non-empty span must still return S_OK (gap_d3d9 D — residency stub). The
  // stub never dereferences the entries, so opaque sentinel pointers suffice.
  int residencySentinel = 0;
  void* residencyResources[2] = {device, &residencySentinel};
  checkEq(device->CheckResourceResidency(std::span<void* const>(residencyResources, 2)), S_OK,
          "check resource residency ignores non-empty span");

  // SetGPUThreadPriority accepts any value as a no-op; GetGPUThreadPriority is
  // hardwired to report 0 and does NOT track prior Set calls (gap_d3d9 D —
  // gpu thread priority stub). Pin both the zero-priority round-trip across a
  // Set(0) and the always-zero readback after a non-zero Set.
  checkEq(device->SetGPUThreadPriority(0), D3D_OK, "set gpu priority zero");
  priority = 999;
  checkEq(device->GetGPUThreadPriority(&priority), D3D_OK, "get gpu priority after set zero");
  checkEq(priority, 0, "gpu priority still zero after set zero");
  checkEq(device->SetGPUThreadPriority(7), D3D_OK, "set gpu priority non-zero");
  priority = 999;
  checkEq(device->GetGPUThreadPriority(&priority), D3D_OK, "get gpu priority after set non-zero");
  checkEq(priority, 0, "gpu priority not tracked: still zero after non-zero set");

  Handle sharedHandle{123};
  auto rt = device->CreateRenderTargetEx({128, 64, Format::A8R8G8B8, Pool::Default, UsageRenderTarget, true,
                                          false, MultiSampleType::Four},
                                         &sharedHandle);
  check(rt != nullptr, "render target ex");
  checkEq(sharedHandle.value, 0u, "render target shared handle cleared");
  check(rt->desc().renderTarget, "render target flagged");

  sharedHandle = Handle{123};
  auto offscreen = device->CreateOffscreenPlainSurfaceEx({32, 32, Format::A8R8G8B8, Pool::SystemMem, 0, false,
                                                          false, MultiSampleType::None},
                                                         &sharedHandle);
  check(offscreen != nullptr, "offscreen ex");
  checkEq(sharedHandle.value, 0u, "offscreen shared handle cleared");

  sharedHandle = Handle{123};
  auto depth = device->CreateDepthStencilSurfaceEx({64, 64, Format::D24S8, Pool::Default, UsageDepthStencil, false,
                                                    true, MultiSampleType::Two},
                                                   &sharedHandle);
  check(depth != nullptr, "depth ex");
  checkEq(sharedHandle.value, 0u, "depth shared handle cleared");
  check(depth->desc().depthStencil, "depth flagged");

  checkEq(device->Release(), 0u, "device ex release");
  checkEq(d3d->Release(), 0u, "factory ex release");
}

void testPalettizedTextureExpansion() {
  using namespace dxmt9::com;

  auto backend = std::make_shared<RecordingBackend>();
  auto* d3d = Direct3DCreate9Ex(D3D_SDK_VERSION, backend);
  check(d3d != nullptr, "factory for p8 texture");

  {
    d3d->AddRef();
    D9CFactory cFactory(d3d);
    checkEq(dxmt9c_factory_check_device_format2(&cFactory, 0, 41u, 0, 3u),
            D3D_OK, "P8 texture CheckDeviceFormat support");
    checkEq(dxmt9c_factory_check_device_format2(&cFactory, 0, 40u, 0, 3u),
            D3D_OK, "A8P8 texture CheckDeviceFormat support");
    checkEq(dxmt9c_factory_check_device_format2(&cFactory, 0, 41u, 0, 5u),
            D3D_OK, "P8 cube texture CheckDeviceFormat support");
    checkEq(dxmt9c_factory_check_device_format2(&cFactory, 0, 40u, 0, 5u),
            D3D_OK, "A8P8 cube texture CheckDeviceFormat support");
    checkEq(dxmt9c_factory_check_device_format2(&cFactory, 0, 41u, 0, 4u),
            D3D_OK, "P8 volume texture CheckDeviceFormat support");
    checkEq(dxmt9c_factory_check_device_format2(&cFactory, 0, 40u, 0, 4u),
            D3D_OK, "A8P8 volume texture CheckDeviceFormat support");
    checkEq(dxmt9c_factory_check_device_format2(&cFactory, 0, 41u,
                                                0x00000001u, 3u),
            D3DERR_NOTAVAILABLE, "P8 render-target query rejected");
    checkEq(dxmt9c_factory_check_device_format2(&cFactory, 0, 40u,
                                                0x00000001u, 3u),
            D3DERR_NOTAVAILABLE, "A8P8 render-target query rejected");
  }

  PresentParameters params{};
  params.backBufferWidth = 320;
  params.backBufferHeight = 240;
  params.windowed = true;

  auto* device = d3d->CreateDeviceEx(0, params, nullptr);
  check(device != nullptr, "device for p8 texture");
  const auto checkSample = [](float actual, uint32_t byte, std::string_view message) {
    const float expected = static_cast<float>(byte) / 255.0f;
    check(std::fabs(actual - expected) < 0.0001f, message);
  };

  {
    device->AddRef();
    D9CDevice cDevice(device);
    {
      auto* texture = dxmt9c_device_create_texture(&cDevice, 2, 2, 1, 0,
                                                   41u, 1u);
      check(texture != nullptr, "create P8 texture");
      checkEq(texture->obj->desc().format, Format::A8R8G8B8,
              "P8 texture uses RGBA backing");

      D9CSurfaceDesc desc{};
      checkEq(dxmt9c_texture_get_level_desc(texture, 0, &desc), D3D_OK,
              "P8 level desc");
      checkEq(desc.format, 41u, "P8 public format preserved");

      D9CLockedRect locked{};
      checkEq(dxmt9c_texture_lock_rect(texture, 0, &locked, nullptr, 0), D3D_OK,
              "P8 lock");
      checkEq(locked.pitch, int32_t{2}, "P8 lock pitch is one byte per texel");
      auto* indices = static_cast<uint8_t*>(locked.bits);
      indices[0] = 1;
      indices[1] = 2;
      indices[2] = 3;
      indices[3] = 4;
      checkEq(dxmt9c_texture_unlock_rect(texture, 0), D3D_OK, "P8 unlock");

      std::array<uint32_t, 256> palette{};
      palette[1] = 0xff112233u;
      palette[2] = 0xff445566u;
      palette[3] = 0xff778899u;
      palette[4] = 0xffaabbccu;
      checkEq(dxmt9c_texture_set_palette(texture, palette.data(),
                                          static_cast<uint32_t>(palette.size())),
              D3D_OK, "P8 palette upload");

      auto bytes = texture->obj->levelBytes(0);
      check(bytes.size() >= 16, "P8 expanded backing has four BGRA pixels");
      checkEq(bytes[0], uint8_t{0x33}, "P8 pixel0 blue");
      checkEq(bytes[1], uint8_t{0x22}, "P8 pixel0 green");
      checkEq(bytes[2], uint8_t{0x11}, "P8 pixel0 red");
      checkEq(bytes[3], uint8_t{0xff}, "P8 pixel0 alpha");
      checkEq(bytes[12], uint8_t{0xcc}, "P8 pixel3 blue");
      checkEq(bytes[13], uint8_t{0xbb}, "P8 pixel3 green");
      checkEq(bytes[14], uint8_t{0xaa}, "P8 pixel3 red");
      checkEq(bytes[15], uint8_t{0xff}, "P8 pixel3 alpha");
      check(!backend->textureUploads.empty(),
            "P8 expansion uploads converted BGRA bytes to backend");
      const auto& upload = backend->textureUploads.back();
      checkEq(upload.width, 2u, "P8 backend upload width");
      checkEq(upload.height, 2u, "P8 backend upload height");
      checkEq(upload.pitch, 8u, "P8 backend upload pitch");
      check(upload.bytes.size() >= 16,
            "P8 backend upload has four expanded BGRA pixels");
      checkEq(upload.bytes[0], uint8_t{0x33}, "P8 backend pixel0 blue");
      checkEq(upload.bytes[1], uint8_t{0x22}, "P8 backend pixel0 green");
      checkEq(upload.bytes[2], uint8_t{0x11}, "P8 backend pixel0 red");
      checkEq(upload.bytes[3], uint8_t{0xff}, "P8 backend pixel0 alpha");
      checkEq(upload.bytes[12], uint8_t{0xcc}, "P8 backend pixel3 blue");
      checkEq(upload.bytes[13], uint8_t{0xbb}, "P8 backend pixel3 green");
      checkEq(upload.bytes[14], uint8_t{0xaa}, "P8 backend pixel3 red");
      checkEq(upload.bytes[15], uint8_t{0xff}, "P8 backend pixel3 alpha");
      float rgba[4]{};
      checkEq(dxmt9c_texture_sample_2d(texture, 0, 0.10f, 0.10f, rgba),
              D3D_OK, "P8 sample reads expanded top-left texel");
      checkSample(rgba[0], 0x11u, "P8 sample top-left red");
      checkSample(rgba[1], 0x22u, "P8 sample top-left green");
      checkSample(rgba[2], 0x33u, "P8 sample top-left blue");
      checkSample(rgba[3], 0xffu, "P8 sample top-left alpha");
      checkEq(dxmt9c_texture_sample_2d(texture, 0, 0.75f, 0.75f, rgba),
              D3D_OK, "P8 sample reads expanded bottom-right texel");
      checkSample(rgba[0], 0xaau, "P8 sample bottom-right red");
      checkSample(rgba[1], 0xbbu, "P8 sample bottom-right green");
      checkSample(rgba[2], 0xccu, "P8 sample bottom-right blue");
      checkSample(rgba[3], 0xffu, "P8 sample bottom-right alpha");

      checkEq(dxmt9c_texture_release(texture), 0u, "P8 texture release");
    }
    {
      auto* texture = dxmt9c_device_create_texture(&cDevice, 2, 1, 1, 0,
                                                   41u, 1u);
      check(texture != nullptr, "create locked P8 palette texture");

      std::array<uint32_t, 256> palette{};
      palette[1] = 0xff010203u;
      palette[2] = 0xff040506u;
      checkEq(dxmt9c_texture_set_palette(texture, palette.data(),
                                          static_cast<uint32_t>(palette.size())),
              D3D_OK, "locked P8 initial palette upload");

      D9CLockedRect locked{};
      checkEq(dxmt9c_texture_lock_rect(texture, 0, &locked, nullptr, 0),
              D3D_OK, "locked P8 lock before palette switch");
      auto* indices = static_cast<uint8_t*>(locked.bits);
      indices[0] = 1;
      indices[1] = 2;

      const size_t uploadCount = backend->textureUploads.size();
      palette[1] = 0xff102030u;
      palette[2] = 0xff405060u;
      checkEq(dxmt9c_texture_set_palette(texture, palette.data(),
                                          static_cast<uint32_t>(palette.size())),
              D3D_OK, "locked P8 palette switch");
      checkEq(backend->textureUploads.size(), uploadCount,
              "locked P8 palette switch skips locked backend upload");

      checkEq(dxmt9c_texture_unlock_rect(texture, 0), D3D_OK,
              "locked P8 unlock after palette switch");
      auto bytes = texture->obj->levelBytes(0);
      check(bytes.size() >= 8,
            "locked P8 expanded backing has two BGRA pixels");
      checkEq(bytes[0], uint8_t{0x30},
              "locked P8 pixel0 blue from latest palette");
      checkEq(bytes[1], uint8_t{0x20},
              "locked P8 pixel0 green from latest palette");
      checkEq(bytes[2], uint8_t{0x10},
              "locked P8 pixel0 red from latest palette");
      checkEq(bytes[4], uint8_t{0x60},
              "locked P8 pixel1 blue from latest palette");
      checkEq(bytes[5], uint8_t{0x50},
              "locked P8 pixel1 green from latest palette");
      checkEq(bytes[6], uint8_t{0x40},
              "locked P8 pixel1 red from latest palette");
      float rgba[4]{};
      checkEq(dxmt9c_texture_sample_2d(texture, 0, 0.10f, 0.10f, rgba),
              D3D_OK, "locked P8 sample reads latest palette pixel0");
      checkSample(rgba[0], 0x10u, "locked P8 sample pixel0 red");
      checkSample(rgba[1], 0x20u, "locked P8 sample pixel0 green");
      checkSample(rgba[2], 0x30u, "locked P8 sample pixel0 blue");
      checkSample(rgba[3], 0xffu, "locked P8 sample pixel0 alpha");
      checkEq(dxmt9c_texture_sample_2d(texture, 0, 0.75f, 0.10f, rgba),
              D3D_OK, "locked P8 sample reads latest palette pixel1");
      checkSample(rgba[0], 0x40u, "locked P8 sample pixel1 red");
      checkSample(rgba[1], 0x50u, "locked P8 sample pixel1 green");
      checkSample(rgba[2], 0x60u, "locked P8 sample pixel1 blue");
      checkSample(rgba[3], 0xffu, "locked P8 sample pixel1 alpha");

      checkEq(dxmt9c_texture_release(texture), 0u,
              "locked P8 palette texture release");
    }
    {
      auto* srcTexture = dxmt9c_device_create_texture(&cDevice, 2, 1, 1, 0,
                                                      41u, 1u);
      auto* dstTexture = dxmt9c_device_create_texture(&cDevice, 2, 1, 1, 0,
                                                      41u, 1u);
      check(srcTexture != nullptr, "create source P8 update texture");
      check(dstTexture != nullptr, "create destination P8 update texture");

      D9CLockedRect locked{};
      checkEq(dxmt9c_texture_lock_rect(srcTexture, 0, &locked, nullptr, 0),
              D3D_OK, "P8 update source lock");
      auto* indices = static_cast<uint8_t*>(locked.bits);
      indices[0] = 1;
      indices[1] = 2;
      checkEq(dxmt9c_texture_unlock_rect(srcTexture, 0), D3D_OK,
              "P8 update source unlock");

      std::array<uint32_t, 256> sourcePalette{};
      sourcePalette[1] = 0xff8899aau;
      sourcePalette[2] = 0xffbbccddu;
      checkEq(dxmt9c_texture_set_palette(srcTexture, sourcePalette.data(),
                                          static_cast<uint32_t>(sourcePalette.size())),
              D3D_OK, "P8 update source palette");
      std::array<uint32_t, 256> destinationPalette{};
      destinationPalette[1] = 0xff102030u;
      destinationPalette[2] = 0xff405060u;
      checkEq(dxmt9c_texture_set_palette(dstTexture, destinationPalette.data(),
                                          static_cast<uint32_t>(destinationPalette.size())),
              D3D_OK, "P8 update destination palette before copy");
      checkEq(dxmt9c_device_update_texture(&cDevice, srcTexture, dstTexture),
              D3D_OK, "P8 UpdateTexture");

      auto bytes = dstTexture->obj->levelBytes(0);
      check(bytes.size() >= 8, "P8 update copied expanded backing");
      checkEq(bytes[0], uint8_t{0x30}, "P8 update pixel0 blue");
      checkEq(bytes[1], uint8_t{0x20}, "P8 update pixel0 green");
      checkEq(bytes[2], uint8_t{0x10}, "P8 update pixel0 red");
      checkEq(bytes[4], uint8_t{0x60}, "P8 update pixel1 blue");
      checkEq(bytes[5], uint8_t{0x50}, "P8 update pixel1 green");
      checkEq(bytes[6], uint8_t{0x40}, "P8 update pixel1 red");

      destinationPalette[1] = 0xff010203u;
      destinationPalette[2] = 0xffa0b0c0u;
      checkEq(dxmt9c_texture_set_palette(dstTexture, destinationPalette.data(),
                                          static_cast<uint32_t>(destinationPalette.size())),
              D3D_OK, "P8 update destination palette switch");
      bytes = dstTexture->obj->levelBytes(0);
      check(bytes.size() >= 8, "P8 update palette switch expanded backing");
      checkEq(bytes[0], uint8_t{0x03},
              "P8 update shadow pixel0 blue after palette switch");
      checkEq(bytes[2], uint8_t{0x01},
              "P8 update shadow pixel0 red after palette switch");
      checkEq(bytes[4], uint8_t{0xc0},
              "P8 update shadow pixel1 blue after palette switch");
      checkEq(bytes[6], uint8_t{0xa0},
              "P8 update shadow pixel1 red after palette switch");
      float rgba[4]{};
      checkEq(dxmt9c_texture_sample_2d(dstTexture, 0, 0.10f, 0.10f, rgba),
              D3D_OK, "P8 update sample reads switched palette pixel0");
      checkSample(rgba[0], 0x01u, "P8 update sample pixel0 red");
      checkSample(rgba[1], 0x02u, "P8 update sample pixel0 green");
      checkSample(rgba[2], 0x03u, "P8 update sample pixel0 blue");
      checkSample(rgba[3], 0xffu, "P8 update sample pixel0 alpha");
      checkEq(dxmt9c_texture_sample_2d(dstTexture, 0, 0.75f, 0.10f, rgba),
              D3D_OK, "P8 update sample reads switched palette pixel1");
      checkSample(rgba[0], 0xa0u, "P8 update sample pixel1 red");
      checkSample(rgba[1], 0xb0u, "P8 update sample pixel1 green");
      checkSample(rgba[2], 0xc0u, "P8 update sample pixel1 blue");
      checkSample(rgba[3], 0xffu, "P8 update sample pixel1 alpha");

      checkEq(dxmt9c_texture_lock_rect(dstTexture, 0, &locked, nullptr, 0),
              D3D_OK, "P8 update destination lock");
      indices = static_cast<uint8_t*>(locked.bits);
      checkEq(indices[0], uint8_t{1},
              "P8 update copied destination index0 shadow");
      checkEq(indices[1], uint8_t{2},
              "P8 update copied destination index1 shadow");
      checkEq(dxmt9c_texture_unlock_rect(dstTexture, 0), D3D_OK,
              "P8 update destination unlock");

      checkEq(dxmt9c_texture_release(dstTexture), 0u,
              "destination P8 update texture release");
      checkEq(dxmt9c_texture_release(srcTexture), 0u,
              "source P8 update texture release");
    }
    {
      auto* texture = dxmt9c_device_create_texture(&cDevice, 2, 1, 1, 0,
                                                   40u, 1u);
      check(texture != nullptr, "create A8P8 texture");
      checkEq(texture->obj->desc().format, Format::A8R8G8B8,
              "A8P8 texture uses RGBA backing");

      D9CSurfaceDesc desc{};
      checkEq(dxmt9c_texture_get_level_desc(texture, 0, &desc), D3D_OK,
              "A8P8 level desc");
      checkEq(desc.format, 40u, "A8P8 public format preserved");

      D9CLockedRect locked{};
      checkEq(dxmt9c_texture_lock_rect(texture, 0, &locked, nullptr, 0), D3D_OK,
              "A8P8 lock");
      checkEq(locked.pitch, int32_t{4},
              "A8P8 lock pitch is two bytes per texel");
      auto* texels = static_cast<uint8_t*>(locked.bits);
      texels[0] = 5;
      texels[1] = 0x80;
      texels[2] = 6;
      texels[3] = 0x40;
      checkEq(dxmt9c_texture_unlock_rect(texture, 0), D3D_OK, "A8P8 unlock");

      std::array<uint32_t, 256> palette{};
      palette[5] = 0xff102030u;
      palette[6] = 0xff405060u;
      checkEq(dxmt9c_texture_set_palette(texture, palette.data(),
                                          static_cast<uint32_t>(palette.size())),
              D3D_OK, "A8P8 palette upload");

      auto bytes = texture->obj->levelBytes(0);
      check(bytes.size() >= 8, "A8P8 expanded backing has two BGRA pixels");
      checkEq(bytes[0], uint8_t{0x30}, "A8P8 pixel0 blue");
      checkEq(bytes[1], uint8_t{0x20}, "A8P8 pixel0 green");
      checkEq(bytes[2], uint8_t{0x10}, "A8P8 pixel0 red");
      checkEq(bytes[3], uint8_t{0x80}, "A8P8 pixel0 alpha from texel");
      checkEq(bytes[4], uint8_t{0x60}, "A8P8 pixel1 blue");
      checkEq(bytes[5], uint8_t{0x50}, "A8P8 pixel1 green");
      checkEq(bytes[6], uint8_t{0x40}, "A8P8 pixel1 red");
      checkEq(bytes[7], uint8_t{0x40}, "A8P8 pixel1 alpha from texel");
      check(!backend->textureUploads.empty(),
            "A8P8 expansion uploads converted BGRA bytes to backend");
      const auto& upload = backend->textureUploads.back();
      checkEq(upload.width, 2u, "A8P8 backend upload width");
      checkEq(upload.height, 1u, "A8P8 backend upload height");
      checkEq(upload.pitch, 8u, "A8P8 backend upload pitch");
      check(upload.bytes.size() >= 8,
            "A8P8 backend upload has two expanded BGRA pixels");
      checkEq(upload.bytes[0], uint8_t{0x30}, "A8P8 backend pixel0 blue");
      checkEq(upload.bytes[1], uint8_t{0x20}, "A8P8 backend pixel0 green");
      checkEq(upload.bytes[2], uint8_t{0x10}, "A8P8 backend pixel0 red");
      checkEq(upload.bytes[3], uint8_t{0x80},
              "A8P8 backend pixel0 alpha from texel");
      checkEq(upload.bytes[4], uint8_t{0x60}, "A8P8 backend pixel1 blue");
      checkEq(upload.bytes[5], uint8_t{0x50}, "A8P8 backend pixel1 green");
      checkEq(upload.bytes[6], uint8_t{0x40}, "A8P8 backend pixel1 red");
      checkEq(upload.bytes[7], uint8_t{0x40},
              "A8P8 backend pixel1 alpha from texel");
      float rgba[4]{};
      checkEq(dxmt9c_texture_sample_2d(texture, 0, 0.10f, 0.10f, rgba),
              D3D_OK, "A8P8 sample reads expanded first texel");
      checkSample(rgba[0], 0x10u, "A8P8 sample first red");
      checkSample(rgba[1], 0x20u, "A8P8 sample first green");
      checkSample(rgba[2], 0x30u, "A8P8 sample first blue");
      checkSample(rgba[3], 0x80u, "A8P8 sample first alpha");
      checkEq(dxmt9c_texture_sample_2d(texture, 0, 0.75f, 0.10f, rgba),
              D3D_OK, "A8P8 sample reads expanded second texel");
      checkSample(rgba[0], 0x40u, "A8P8 sample second red");
      checkSample(rgba[1], 0x50u, "A8P8 sample second green");
      checkSample(rgba[2], 0x60u, "A8P8 sample second blue");
      checkSample(rgba[3], 0x40u, "A8P8 sample second alpha");

      checkEq(dxmt9c_texture_release(texture), 0u, "A8P8 texture release");
    }
    {
      auto* texture = dxmt9c_device_create_texture(&cDevice, 2, 1, 1, 0,
                                                   40u, 1u);
      check(texture != nullptr, "create locked A8P8 palette texture");

      std::array<uint32_t, 256> palette{};
      palette[5] = 0xff010203u;
      palette[6] = 0xff040506u;
      checkEq(dxmt9c_texture_set_palette(texture, palette.data(),
                                          static_cast<uint32_t>(palette.size())),
              D3D_OK, "locked A8P8 initial palette upload");

      D9CLockedRect locked{};
      checkEq(dxmt9c_texture_lock_rect(texture, 0, &locked, nullptr, 0),
              D3D_OK, "locked A8P8 lock before palette switch");
      auto* texels = static_cast<uint8_t*>(locked.bits);
      texels[0] = 5;
      texels[1] = 0x90;
      texels[2] = 6;
      texels[3] = 0x50;

      const size_t uploadCount = backend->textureUploads.size();
      palette[5] = 0xff102030u;
      palette[6] = 0xff405060u;
      checkEq(dxmt9c_texture_set_palette(texture, palette.data(),
                                          static_cast<uint32_t>(palette.size())),
              D3D_OK, "locked A8P8 palette switch");
      checkEq(backend->textureUploads.size(), uploadCount,
              "locked A8P8 palette switch skips locked backend upload");

      checkEq(dxmt9c_texture_unlock_rect(texture, 0), D3D_OK,
              "locked A8P8 unlock after palette switch");
      auto bytes = texture->obj->levelBytes(0);
      check(bytes.size() >= 8,
            "locked A8P8 expanded backing has two BGRA pixels");
      checkEq(bytes[0], uint8_t{0x30},
              "locked A8P8 pixel0 blue from latest palette");
      checkEq(bytes[1], uint8_t{0x20},
              "locked A8P8 pixel0 green from latest palette");
      checkEq(bytes[2], uint8_t{0x10},
              "locked A8P8 pixel0 red from latest palette");
      checkEq(bytes[3], uint8_t{0x90},
              "locked A8P8 pixel0 alpha from texel");
      checkEq(bytes[4], uint8_t{0x60},
              "locked A8P8 pixel1 blue from latest palette");
      checkEq(bytes[5], uint8_t{0x50},
              "locked A8P8 pixel1 green from latest palette");
      checkEq(bytes[6], uint8_t{0x40},
              "locked A8P8 pixel1 red from latest palette");
      checkEq(bytes[7], uint8_t{0x50},
              "locked A8P8 pixel1 alpha from texel");
      float rgba[4]{};
      checkEq(dxmt9c_texture_sample_2d(texture, 0, 0.10f, 0.10f, rgba),
              D3D_OK, "locked A8P8 sample reads latest palette pixel0");
      checkSample(rgba[0], 0x10u, "locked A8P8 sample pixel0 red");
      checkSample(rgba[1], 0x20u, "locked A8P8 sample pixel0 green");
      checkSample(rgba[2], 0x30u, "locked A8P8 sample pixel0 blue");
      checkSample(rgba[3], 0x90u, "locked A8P8 sample pixel0 alpha");
      checkEq(dxmt9c_texture_sample_2d(texture, 0, 0.75f, 0.10f, rgba),
              D3D_OK, "locked A8P8 sample reads latest palette pixel1");
      checkSample(rgba[0], 0x40u, "locked A8P8 sample pixel1 red");
      checkSample(rgba[1], 0x50u, "locked A8P8 sample pixel1 green");
      checkSample(rgba[2], 0x60u, "locked A8P8 sample pixel1 blue");
      checkSample(rgba[3], 0x50u, "locked A8P8 sample pixel1 alpha");

      checkEq(dxmt9c_texture_release(texture), 0u,
              "locked A8P8 palette texture release");
    }
    {
      auto* srcTexture = dxmt9c_device_create_texture(&cDevice, 2, 1, 1, 0,
                                                      40u, 1u);
      auto* dstTexture = dxmt9c_device_create_texture(&cDevice, 2, 1, 1, 0,
                                                      40u, 1u);
      check(srcTexture != nullptr, "create source A8P8 update texture");
      check(dstTexture != nullptr, "create destination A8P8 update texture");

      D9CLockedRect locked{};
      checkEq(dxmt9c_texture_lock_rect(srcTexture, 0, &locked, nullptr, 0),
              D3D_OK, "A8P8 update source lock");
      auto* texels = static_cast<uint8_t*>(locked.bits);
      texels[0] = 5;
      texels[1] = 0x70;
      texels[2] = 6;
      texels[3] = 0x30;
      checkEq(dxmt9c_texture_unlock_rect(srcTexture, 0), D3D_OK,
              "A8P8 update source unlock");

      std::array<uint32_t, 256> sourcePalette{};
      sourcePalette[5] = 0xff8899aau;
      sourcePalette[6] = 0xffbbccddu;
      checkEq(dxmt9c_texture_set_palette(srcTexture, sourcePalette.data(),
                                          static_cast<uint32_t>(sourcePalette.size())),
              D3D_OK, "A8P8 update source palette");
      std::array<uint32_t, 256> destinationPalette{};
      destinationPalette[5] = 0xff102030u;
      destinationPalette[6] = 0xff405060u;
      checkEq(dxmt9c_texture_set_palette(dstTexture, destinationPalette.data(),
                                          static_cast<uint32_t>(destinationPalette.size())),
              D3D_OK, "A8P8 update destination palette before copy");
      checkEq(dxmt9c_device_update_texture(&cDevice, srcTexture, dstTexture),
              D3D_OK, "A8P8 UpdateTexture");

      auto bytes = dstTexture->obj->levelBytes(0);
      check(bytes.size() >= 8, "A8P8 update copied expanded backing");
      checkEq(bytes[0], uint8_t{0x30}, "A8P8 update pixel0 blue");
      checkEq(bytes[2], uint8_t{0x10}, "A8P8 update pixel0 red");
      checkEq(bytes[3], uint8_t{0x70},
              "A8P8 update pixel0 alpha from texel");
      checkEq(bytes[4], uint8_t{0x60}, "A8P8 update pixel1 blue");
      checkEq(bytes[6], uint8_t{0x40}, "A8P8 update pixel1 red");
      checkEq(bytes[7], uint8_t{0x30},
              "A8P8 update pixel1 alpha from texel");

      destinationPalette[5] = 0xff010203u;
      destinationPalette[6] = 0xffa0b0c0u;
      checkEq(dxmt9c_texture_set_palette(dstTexture, destinationPalette.data(),
                                          static_cast<uint32_t>(destinationPalette.size())),
              D3D_OK, "A8P8 update destination palette switch");
      bytes = dstTexture->obj->levelBytes(0);
      check(bytes.size() >= 8, "A8P8 update palette switch backing");
      checkEq(bytes[0], uint8_t{0x03},
              "A8P8 update shadow pixel0 blue after palette switch");
      checkEq(bytes[2], uint8_t{0x01},
              "A8P8 update shadow pixel0 red after palette switch");
      checkEq(bytes[3], uint8_t{0x70},
              "A8P8 update shadow pixel0 alpha after palette switch");
      checkEq(bytes[4], uint8_t{0xc0},
              "A8P8 update shadow pixel1 blue after palette switch");
      checkEq(bytes[6], uint8_t{0xa0},
              "A8P8 update shadow pixel1 red after palette switch");
      checkEq(bytes[7], uint8_t{0x30},
              "A8P8 update shadow pixel1 alpha after palette switch");
      float rgba[4]{};
      checkEq(dxmt9c_texture_sample_2d(dstTexture, 0, 0.10f, 0.10f, rgba),
              D3D_OK, "A8P8 update sample reads switched palette pixel0");
      checkSample(rgba[0], 0x01u, "A8P8 update sample pixel0 red");
      checkSample(rgba[1], 0x02u, "A8P8 update sample pixel0 green");
      checkSample(rgba[2], 0x03u, "A8P8 update sample pixel0 blue");
      checkSample(rgba[3], 0x70u, "A8P8 update sample pixel0 alpha");
      checkEq(dxmt9c_texture_sample_2d(dstTexture, 0, 0.75f, 0.10f, rgba),
              D3D_OK, "A8P8 update sample reads switched palette pixel1");
      checkSample(rgba[0], 0xa0u, "A8P8 update sample pixel1 red");
      checkSample(rgba[1], 0xb0u, "A8P8 update sample pixel1 green");
      checkSample(rgba[2], 0xc0u, "A8P8 update sample pixel1 blue");
      checkSample(rgba[3], 0x30u, "A8P8 update sample pixel1 alpha");

      checkEq(dxmt9c_texture_lock_rect(dstTexture, 0, &locked, nullptr, 0),
              D3D_OK, "A8P8 update destination lock");
      texels = static_cast<uint8_t*>(locked.bits);
      checkEq(texels[0], uint8_t{5},
              "A8P8 update copied destination index0 shadow");
      checkEq(texels[1], uint8_t{0x70},
              "A8P8 update copied destination alpha0 shadow");
      checkEq(texels[2], uint8_t{6},
              "A8P8 update copied destination index1 shadow");
      checkEq(texels[3], uint8_t{0x30},
              "A8P8 update copied destination alpha1 shadow");
      checkEq(dxmt9c_texture_unlock_rect(dstTexture, 0), D3D_OK,
              "A8P8 update destination unlock");

      checkEq(dxmt9c_texture_release(dstTexture), 0u,
              "destination A8P8 update texture release");
      checkEq(dxmt9c_texture_release(srcTexture), 0u,
              "source A8P8 update texture release");
    }
    {
      auto* texture = dxmt9c_device_create_cube_texture(&cDevice, 2, 1, 0,
                                                        41u, 1u);
      check(texture != nullptr, "create cube P8 texture");
      checkEq(texture->obj->desc().format, Format::A8R8G8B8,
              "cube P8 texture uses RGBA backing");
      checkEq(texture->obj->subresourceCount(), 6u,
              "cube P8 has one level per face");

      D9CSurfaceDesc desc{};
      checkEq(dxmt9c_texture_get_level_desc(texture, 0, &desc), D3D_OK,
              "cube P8 level desc");
      checkEq(desc.format, 41u, "cube P8 public format preserved");

      std::array<uint32_t, 256> palette{};
      palette[7] = 0xff010203u;
      palette[8] = 0xffa0b0c0u;
      palette[9] = 0xff112233u;
      palette[10] = 0xff445566u;
      checkEq(dxmt9c_texture_set_palette(texture, palette.data(),
                                          static_cast<uint32_t>(palette.size())),
              D3D_OK, "cube P8 palette upload");

      D9CLockedRect locked{};
      const uint32_t faceSubresource = 2u;
      checkEq(dxmt9c_texture_lock_rect(texture, faceSubresource, &locked,
                                       nullptr, 0),
              D3D_OK, "cube P8 lock");
      checkEq(locked.pitch, int32_t{2},
              "cube P8 lock pitch is one byte per texel");
      auto* indices = static_cast<uint8_t*>(locked.bits);
      indices[0] = 7;
      indices[1] = 8;
      indices[2] = 9;
      indices[3] = 10;
      checkEq(dxmt9c_texture_unlock_rect(texture, faceSubresource), D3D_OK,
              "cube P8 unlock");

      auto bytes = texture->obj->levelBytes(faceSubresource);
      check(bytes.size() >= 16,
            "cube P8 expanded backing has four BGRA pixels");
      checkEq(bytes[0], uint8_t{0x03}, "cube P8 pixel0 blue");
      checkEq(bytes[1], uint8_t{0x02}, "cube P8 pixel0 green");
      checkEq(bytes[2], uint8_t{0x01}, "cube P8 pixel0 red");
      checkEq(bytes[12], uint8_t{0x66}, "cube P8 pixel3 blue");
      checkEq(bytes[13], uint8_t{0x55}, "cube P8 pixel3 green");
      checkEq(bytes[14], uint8_t{0x44}, "cube P8 pixel3 red");
      check(!backend->textureUploads.empty(),
            "cube P8 expansion uploads converted BGRA bytes to backend");
      const auto& upload = backend->textureUploads.back();
      checkEq(upload.level, faceSubresource, "cube P8 backend upload face");
      checkEq(upload.width, 2u, "cube P8 backend upload width");
      checkEq(upload.height, 2u, "cube P8 backend upload height");
      checkEq(upload.depth, 1u, "cube P8 backend upload depth");
      checkEq(upload.pitch, 8u, "cube P8 backend upload pitch");

      checkEq(dxmt9c_texture_release(texture), 0u, "cube P8 texture release");
    }
    {
      auto* texture = dxmt9c_device_create_cube_texture(&cDevice, 2, 1, 0,
                                                        40u, 1u);
      check(texture != nullptr, "create cube A8P8 texture");
      checkEq(texture->obj->desc().format, Format::A8R8G8B8,
              "cube A8P8 texture uses RGBA backing");
      checkEq(texture->obj->subresourceCount(), 6u,
              "cube A8P8 has one level per face");

      D9CSurfaceDesc desc{};
      checkEq(dxmt9c_texture_get_level_desc(texture, 0, &desc), D3D_OK,
              "cube A8P8 level desc");
      checkEq(desc.format, 40u, "cube A8P8 public format preserved");

      std::array<uint32_t, 256> palette{};
      palette[15] = 0xff102030u;
      palette[16] = 0xff405060u;
      palette[17] = 0xff708090u;
      palette[18] = 0xffa0b0c0u;
      checkEq(dxmt9c_texture_set_palette(texture, palette.data(),
                                          static_cast<uint32_t>(palette.size())),
              D3D_OK, "cube A8P8 palette upload");

      D9CLockedRect locked{};
      const uint32_t faceSubresource = 4u;
      checkEq(dxmt9c_texture_lock_rect(texture, faceSubresource, &locked,
                                       nullptr, 0),
              D3D_OK, "cube A8P8 lock");
      checkEq(locked.pitch, int32_t{4},
              "cube A8P8 lock pitch is two bytes per texel");
      auto* texels = static_cast<uint8_t*>(locked.bits);
      texels[0] = 15;
      texels[1] = 0x10;
      texels[2] = 16;
      texels[3] = 0x20;
      texels[4] = 17;
      texels[5] = 0x30;
      texels[6] = 18;
      texels[7] = 0x40;
      checkEq(dxmt9c_texture_unlock_rect(texture, faceSubresource), D3D_OK,
              "cube A8P8 unlock");

      auto bytes = texture->obj->levelBytes(faceSubresource);
      check(bytes.size() >= 16,
            "cube A8P8 expanded backing has four BGRA pixels");
      checkEq(bytes[0], uint8_t{0x30}, "cube A8P8 pixel0 blue");
      checkEq(bytes[1], uint8_t{0x20}, "cube A8P8 pixel0 green");
      checkEq(bytes[2], uint8_t{0x10}, "cube A8P8 pixel0 red");
      checkEq(bytes[3], uint8_t{0x10}, "cube A8P8 pixel0 alpha");
      checkEq(bytes[12], uint8_t{0xc0}, "cube A8P8 pixel3 blue");
      checkEq(bytes[13], uint8_t{0xb0}, "cube A8P8 pixel3 green");
      checkEq(bytes[14], uint8_t{0xa0}, "cube A8P8 pixel3 red");
      checkEq(bytes[15], uint8_t{0x40}, "cube A8P8 pixel3 alpha");
      check(!backend->textureUploads.empty(),
            "cube A8P8 expansion uploads converted BGRA bytes to backend");
      const auto& upload = backend->textureUploads.back();
      checkEq(upload.level, faceSubresource, "cube A8P8 backend upload face");
      checkEq(upload.width, 2u, "cube A8P8 backend upload width");
      checkEq(upload.height, 2u, "cube A8P8 backend upload height");
      checkEq(upload.depth, 1u, "cube A8P8 backend upload depth");
      checkEq(upload.pitch, 8u, "cube A8P8 backend upload pitch");
      check(upload.bytes.size() >= 16,
            "cube A8P8 backend upload has expanded face");
      checkEq(upload.bytes[3], uint8_t{0x10},
              "cube A8P8 backend pixel0 alpha");
      checkEq(upload.bytes[15], uint8_t{0x40},
              "cube A8P8 backend pixel3 alpha");

      checkEq(dxmt9c_texture_release(texture), 0u,
              "cube A8P8 texture release");
    }
    {
      auto* texture = dxmt9c_device_create_volume_texture(&cDevice, 2, 1, 2,
                                                          1, 0, 41u, 1u);
      check(texture != nullptr, "create volume P8 texture");
      checkEq(texture->obj->desc().format, Format::A8R8G8B8,
              "volume P8 texture uses RGBA backing");

      D9CSurfaceDesc desc{};
      checkEq(dxmt9c_texture_get_level_desc(texture, 0, &desc), D3D_OK,
              "volume P8 level desc");
      checkEq(desc.format, 41u, "volume P8 public format preserved");
      checkEq(desc.depth, 2u, "volume P8 depth preserved");

      std::array<uint32_t, 256> palette{};
      palette[19] = 0xff010203u;
      palette[20] = 0xff405060u;
      palette[21] = 0xff708090u;
      palette[22] = 0xffa0b0c0u;
      checkEq(dxmt9c_texture_set_palette(texture, palette.data(),
                                          static_cast<uint32_t>(palette.size())),
              D3D_OK, "volume P8 palette upload");

      D9CLockedRect locked{};
      checkEq(dxmt9c_texture_lock_rect(texture, 0, &locked, nullptr, 0),
              D3D_OK, "volume P8 lock");
      checkEq(locked.pitch, int32_t{2},
              "volume P8 lock pitch is one byte per texel");
      auto* indices = static_cast<uint8_t*>(locked.bits);
      indices[0] = 19;
      indices[1] = 20;
      indices[2] = 21;
      indices[3] = 22;
      checkEq(dxmt9c_texture_unlock_rect(texture, 0), D3D_OK,
              "volume P8 unlock");

      auto bytes = texture->obj->levelBytes(0);
      check(bytes.size() >= 16,
            "volume P8 expanded backing has two BGRA slices");
      checkEq(bytes[0], uint8_t{0x03}, "volume P8 slice0 pixel0 blue");
      checkEq(bytes[2], uint8_t{0x01}, "volume P8 slice0 pixel0 red");
      checkEq(bytes[3], uint8_t{0xff}, "volume P8 slice0 pixel0 alpha");
      checkEq(bytes[8], uint8_t{0x90}, "volume P8 slice1 pixel0 blue");
      checkEq(bytes[10], uint8_t{0x70}, "volume P8 slice1 pixel0 red");
      checkEq(bytes[15], uint8_t{0xff}, "volume P8 slice1 pixel1 alpha");
      check(!backend->textureUploads.empty(),
            "volume P8 expansion uploads converted BGRA bytes to backend");
      const auto& upload = backend->textureUploads.back();
      checkEq(upload.width, 2u, "volume P8 backend upload width");
      checkEq(upload.height, 1u, "volume P8 backend upload height");
      checkEq(upload.depth, 2u, "volume P8 backend upload depth");
      checkEq(upload.pitch, 8u, "volume P8 backend upload pitch");
      checkEq(upload.slicePitch, 8u, "volume P8 backend upload slice pitch");
      check(upload.bytes.size() >= 16,
            "volume P8 backend upload has two expanded slices");
      checkEq(upload.bytes[8], uint8_t{0x90},
              "volume P8 backend slice1 pixel0 blue");
      checkEq(upload.bytes[15], uint8_t{0xff},
              "volume P8 backend slice1 pixel1 alpha");

      checkEq(dxmt9c_texture_release(texture), 0u,
              "volume P8 texture release");
    }
    {
      auto* texture = dxmt9c_device_create_volume_texture(&cDevice, 2, 1, 2,
                                                          1, 0, 40u, 1u);
      check(texture != nullptr, "create volume A8P8 texture");
      checkEq(texture->obj->desc().format, Format::A8R8G8B8,
              "volume A8P8 texture uses RGBA backing");

      D9CSurfaceDesc desc{};
      checkEq(dxmt9c_texture_get_level_desc(texture, 0, &desc), D3D_OK,
              "volume A8P8 level desc");
      checkEq(desc.format, 40u, "volume A8P8 public format preserved");
      checkEq(desc.depth, 2u, "volume A8P8 depth preserved");

      std::array<uint32_t, 256> palette{};
      palette[11] = 0xff102030u;
      palette[12] = 0xff405060u;
      palette[13] = 0xff708090u;
      palette[14] = 0xffa0b0c0u;
      checkEq(dxmt9c_texture_set_palette(texture, palette.data(),
                                          static_cast<uint32_t>(palette.size())),
              D3D_OK, "volume A8P8 palette upload");

      D9CLockedRect locked{};
      checkEq(dxmt9c_texture_lock_rect(texture, 0, &locked, nullptr, 0),
              D3D_OK, "volume A8P8 lock");
      checkEq(locked.pitch, int32_t{4},
              "volume A8P8 lock pitch is two bytes per texel");
      auto* texels = static_cast<uint8_t*>(locked.bits);
      texels[0] = 11;
      texels[1] = 0x10;
      texels[2] = 12;
      texels[3] = 0x20;
      texels[4] = 13;
      texels[5] = 0x30;
      texels[6] = 14;
      texels[7] = 0x40;
      checkEq(dxmt9c_texture_unlock_rect(texture, 0), D3D_OK,
              "volume A8P8 unlock");

      auto bytes = texture->obj->levelBytes(0);
      check(bytes.size() >= 16,
            "volume A8P8 expanded backing has two BGRA slices");
      checkEq(bytes[0], uint8_t{0x30}, "volume A8P8 slice0 pixel0 blue");
      checkEq(bytes[3], uint8_t{0x10}, "volume A8P8 slice0 pixel0 alpha");
      checkEq(bytes[8], uint8_t{0x90}, "volume A8P8 slice1 pixel0 blue");
      checkEq(bytes[11], uint8_t{0x30}, "volume A8P8 slice1 pixel0 alpha");
      checkEq(bytes[12], uint8_t{0xc0}, "volume A8P8 slice1 pixel1 blue");
      checkEq(bytes[15], uint8_t{0x40}, "volume A8P8 slice1 pixel1 alpha");
      check(!backend->textureUploads.empty(),
            "volume A8P8 expansion uploads converted BGRA bytes to backend");
      const auto& upload = backend->textureUploads.back();
      checkEq(upload.width, 2u, "volume A8P8 backend upload width");
      checkEq(upload.height, 1u, "volume A8P8 backend upload height");
      checkEq(upload.depth, 2u, "volume A8P8 backend upload depth");
      checkEq(upload.pitch, 8u, "volume A8P8 backend upload pitch");
      checkEq(upload.slicePitch, 8u, "volume A8P8 backend upload slice pitch");
      check(upload.bytes.size() >= 16,
            "volume A8P8 backend upload has two expanded slices");
      checkEq(upload.bytes[8], uint8_t{0x90},
              "volume A8P8 backend slice1 pixel0 blue");
      checkEq(upload.bytes[15], uint8_t{0x40},
              "volume A8P8 backend slice1 pixel1 alpha");

      checkEq(dxmt9c_texture_release(texture), 0u,
              "volume A8P8 texture release");
    }
  }
  checkEq(device->Release(), 0u, "P8 device release");
  checkEq(d3d->Release(), 0u, "P8 factory release");
}

void testReadOnlyBufferUnlockSkipsUpload() {
  using namespace dxmt9::com;

  auto backend = std::make_shared<RecordingBackend>();
  auto* d3d = Direct3DCreate9Ex(D3D_SDK_VERSION, backend);
  check(d3d != nullptr, "factory for readonly buffer unlock");

  PresentParameters params{};
  params.backBufferWidth = 320;
  params.backBufferHeight = 240;
  params.windowed = true;

  auto* device = d3d->CreateDeviceEx(0, params, nullptr);
  check(device != nullptr, "device for readonly buffer unlock");

  {
    device->AddRef();
    D9CDevice cDevice(device);
    auto* buffer = dxmt9c_device_create_vertex_buffer(&cDevice, 4, 0, 0, 2u);
    check(buffer != nullptr, "create readonly unlock buffer");

    void* data = nullptr;
    checkEq(dxmt9c_buffer_lock(buffer, 0, 4, &data, 0), D3D_OK,
            "initial write lock");
    auto* bytes = static_cast<uint8_t*>(data);
    bytes[0] = 0x11;
    bytes[1] = 0x22;
    bytes[2] = 0x33;
    bytes[3] = 0x44;
    checkEq(dxmt9c_buffer_unlock(buffer), D3D_OK, "initial write unlock");
    checkEq(backend->bufferUploads.size(), size_t{1},
            "write unlock uploads buffer");

    data = nullptr;
    checkEq(dxmt9c_buffer_lock(buffer, 0, 4, &data, 0x10u), D3D_OK,
            "readonly lock");
    check(data != nullptr, "readonly lock returns data");
    checkEq(dxmt9c_buffer_unlock(buffer), D3D_OK, "readonly unlock");
    checkEq(backend->bufferUploads.size(), size_t{1},
            "readonly unlock does not upload buffer");

    data = nullptr;
    checkEq(dxmt9c_buffer_lock(buffer, 0, 4, &data, 0), D3D_OK,
            "second write lock");
    bytes = static_cast<uint8_t*>(data);
    bytes[0] = 0x55;
    checkEq(dxmt9c_buffer_unlock(buffer), D3D_OK, "second write unlock");
    checkEq(backend->bufferUploads.size(), size_t{2},
            "write unlock after readonly uploads buffer");

    checkEq(dxmt9c_buffer_release(buffer), 0u, "readonly unlock buffer release");
  }
  checkEq(device->Release(), 0u, "readonly unlock device release");
  checkEq(d3d->Release(), 0u, "readonly unlock factory release");
}

void testRedundantShaderConstSetKeepsUniformSnapshotReusable() {
  using namespace dxmt9::com;

  auto backend = std::make_shared<RecordingBackend>();
  auto* d3d = Direct3DCreate9Ex(D3D_SDK_VERSION, backend);
  check(d3d != nullptr, "factory for redundant shader const set");

  PresentParameters params{};
  params.backBufferWidth = 320;
  params.backBufferHeight = 240;
  params.windowed = true;

  auto* device = d3d->CreateDeviceEx(0, params, nullptr);
  check(device != nullptr, "device for redundant shader const set");

  {
    device->AddRef();
    D9CDevice cDevice(device);

    const std::array<float, 4> vsFloat{1.0f, 2.0f, 3.0f, 4.0f};
    checkEq(dxmt9c_device_set_vs_const_f(&cDevice, 3u, vsFloat.data(), 1u),
            D3D_OK, "initial VS float const set");

    DrawParam draw{};
    draw.primitiveCount = 1u;
    DrawSubmissionUniformScratch scratch{};
    DrawRunSubmission first{};
    checkEq(device->coreDevice().snapshotDrawSubmissionFromCurrentState(
                draw, first, nullptr, &scratch),
            D3D_OK, "first draw submission snapshot");
    check(first.uniforms.has_value(), "first snapshot owns uniforms");

    checkEq(dxmt9c_device_set_vs_const_f(&cDevice, 3u, vsFloat.data(), 1u),
            D3D_OK, "redundant VS float const set");
    DrawRunSubmission second{};
    checkEq(device->coreDevice().snapshotDrawSubmissionFromCurrentState(
                draw, second, &first, &scratch),
            D3D_OK, "second draw submission snapshot");
    check(!second.uniforms.has_value(),
          "redundant VS float const set does not invalidate uniform snapshot");

    const std::array<int32_t, 4> psInt{5, 6, 7, 8};
    checkEq(dxmt9c_device_set_ps_const_i(&cDevice, 2u, psInt.data(), 1u),
            D3D_OK, "initial PS int const set");
    DrawRunSubmission third{};
    checkEq(device->coreDevice().snapshotDrawSubmissionFromCurrentState(
                draw, third, &second, &scratch),
            D3D_OK, "third draw submission snapshot");
    check(third.uniforms.has_value(), "changed PS int const refreshes uniforms");
    check(third.uniformPayload().vsConst == first.uniformPayload().vsConst,
          "changed PS const keeps cached VS constants");
    checkEq(third.uniformPayload().vertexConstantsHash,
            first.uniformPayload().vertexConstantsHash,
            "changed PS const keeps cached VS constant hash");

    checkEq(dxmt9c_device_set_ps_const_i(&cDevice, 2u, psInt.data(), 1u),
            D3D_OK, "redundant PS int const set");
    DrawRunSubmission fourth{};
    checkEq(device->coreDevice().snapshotDrawSubmissionFromCurrentState(
                draw, fourth, &third, &scratch),
            D3D_OK, "fourth draw submission snapshot");
    check(!fourth.uniforms.has_value(),
          "redundant PS int const set does not invalidate uniform snapshot");

    const std::array<uint32_t, 2> vsBool{0u, 42u};
    checkEq(dxmt9c_device_set_vs_const_b(&cDevice, 4u, vsBool.data(), 2u),
            D3D_OK, "initial VS bool const set");
    DrawRunSubmission fifth{};
    checkEq(device->coreDevice().snapshotDrawSubmissionFromCurrentState(
                draw, fifth, &fourth, &scratch),
            D3D_OK, "fifth draw submission snapshot");
    check(fifth.uniforms.has_value(), "changed VS bool const refreshes uniforms");
    check(fifth.uniformPayload().psConst == third.uniformPayload().psConst,
          "changed VS const keeps cached PS constants");
    checkEq(fifth.uniformPayload().pixelConstantsHash,
            third.uniformPayload().pixelConstantsHash,
            "changed VS const keeps cached PS constant hash");

    checkEq(dxmt9c_device_set_vs_const_b(&cDevice, 4u, vsBool.data(), 2u),
            D3D_OK, "redundant VS bool const set");
    DrawRunSubmission sixth{};
    checkEq(device->coreDevice().snapshotDrawSubmissionFromCurrentState(
                draw, sixth, &fifth, &scratch),
            D3D_OK, "sixth draw submission snapshot");
    check(!sixth.uniforms.has_value(),
          "redundant VS bool const set does not invalidate uniform snapshot");
  }
  checkEq(device->Release(), 0u, "redundant shader const device release");
  checkEq(d3d->Release(), 0u, "redundant shader const factory release");
}

void testSnapshotDrawSubmissionCompactUniformScratch() {
  using namespace dxmt9::com;

  auto backend = std::make_shared<RecordingBackend>();
  auto* d3d = Direct3DCreate9Ex(D3D_SDK_VERSION, backend);
  check(d3d != nullptr, "factory for compact uniform snapshot");

  PresentParameters params{};
  params.backBufferWidth = 320;
  params.backBufferHeight = 240;
  params.windowed = true;

  auto* device = d3d->CreateDeviceEx(0, params, nullptr);
  check(device != nullptr, "device for compact uniform snapshot");

  {
    device->AddRef();
    D9CDevice cDevice(device);

    const std::array<float, 8> vsFloats{
        1.0f, 2.0f, 3.0f, 4.0f,
        5.0f, 6.0f, 7.0f, 8.0f};
    checkEq(dxmt9c_device_set_vs_const_f(&cDevice, 0u, vsFloats.data(), 2u),
            D3D_OK, "set VS float prefix before compact snapshot");
    const std::array<uint32_t, 1> vsBool{1u};
    checkEq(dxmt9c_device_set_vs_const_b(&cDevice, 0u, vsBool.data(), 1u),
            D3D_OK, "set VS bool requiring ABI float/int prefix");

    const std::array<float, 4> psFloat{9.0f, 10.0f, 11.0f, 12.0f};
    checkEq(dxmt9c_device_set_ps_const_f(&cDevice, 0u, psFloat.data(), 1u),
            D3D_OK, "set PS float prefix before compact snapshot");
    const std::array<int32_t, 4> psInt{13, 14, 15, 16};
    checkEq(dxmt9c_device_set_ps_const_i(&cDevice, 0u, psInt.data(), 1u),
            D3D_OK, "set PS int requiring ABI float prefix");

    DrawParam draw{};
    draw.primitiveCount = 1u;
    DrawSubmissionUniformScratch scratch{};
    DrawRunSubmission submission{};
    checkEq(device->coreDevice().snapshotDrawSubmissionFromCurrentState(
                draw, submission, nullptr, &scratch, true),
            D3D_OK, "compact draw submission snapshot");

    check(!submission.uniforms.has_value(),
          "compact snapshot does not copy full DrawUniformPayload");
    check(submission.compactUniforms.has_value(),
          "compact snapshot owns a compact uniform record");
    checkEq(scratch.fixedPayloads.size(), std::size_t{1},
            "compact snapshot stores one fixed payload in scratch");

    const auto& compact = submission.compactUniformPayload();
    checkEq(compact.fixedPayloadIndex, 0u,
            "compact snapshot points at the scratch fixed payload");
    check(compact.valid(), "compact snapshot carries valid byte ranges");
    checkEq(compact.vertexConstantsRange.size,
            static_cast<u32>(compact.vertexConstants.byteSize),
            "compact VS range tracks the compact VS byte width");
    checkEq(compact.pixelConstantsRange.size,
            static_cast<u32>(compact.pixelConstants.byteSize),
            "compact PS range tracks the compact PS byte width");

    const std::array<float, 8> nextVsFloats{
        21.0f, 22.0f, 23.0f, 24.0f,
        25.0f, 26.0f, 27.0f, 28.0f};
    checkEq(dxmt9c_device_set_vs_const_f(&cDevice, 0u,
                                         nextVsFloats.data(), 2u),
            D3D_OK, "change shader constants before second compact snapshot");
    DrawRunSubmission second{};
    checkEq(device->coreDevice().snapshotDrawSubmissionFromCurrentState(
                draw, second, &submission, &scratch, true),
            D3D_OK, "second compact draw submission snapshot");
    check(!second.uniforms.has_value(),
          "second compact snapshot does not copy full DrawUniformPayload");
    check(second.compactUniforms.has_value(),
          "second compact snapshot owns a compact uniform record");
    checkEq(scratch.fixedPayloads.size(), std::size_t{1},
            "second compact snapshot reuses the scratch fixed payload");
    checkEq(second.compactUniformPayload().fixedPayloadIndex, 0u,
            "second compact snapshot points at the reused fixed payload");

    D9CMatrix world{};
    world.m[0] = 2.0f;
    world.m[5] = 1.0f;
    world.m[10] = 1.0f;
    world.m[15] = 1.0f;
    constexpr uint32_t kD3dtsWorld = 256u;
    checkEq(dxmt9c_device_set_transform(&cDevice, kD3dtsWorld, &world),
            D3D_OK, "change fixed transform before third compact snapshot");
    DrawRunSubmission third{};
    checkEq(device->coreDevice().snapshotDrawSubmissionFromCurrentState(
                draw, third, &second, &scratch, true),
            D3D_OK, "third compact draw submission snapshot");
    check(third.compactUniforms.has_value(),
          "third compact snapshot owns a compact uniform record");
    checkEq(scratch.fixedPayloads.size(), std::size_t{2},
            "third compact snapshot appends changed fixed payload");
    checkEq(third.compactUniformPayload().fixedPayloadIndex, 1u,
            "third compact snapshot points at the changed fixed payload");

    DrawSubmissionUniformScratch compactScratch{};
    DrawRunCompactSubmission compactFirst{};
    checkEq(device->coreDevice().snapshotDrawSubmissionFromCurrentState(
                draw, compactFirst, nullptr, &compactScratch),
            D3D_OK, "direct compact draw submission snapshot");
    check(compactFirst.compactUniforms.has_value(),
          "direct compact snapshot owns a compact uniform record");
    check(compactFirst.stateMaterialized,
          "direct compact first snapshot materializes state");
    checkEq(compactScratch.fixedPayloads.size(), std::size_t{1},
            "direct compact snapshot stores one fixed payload");
    const auto& directCompact = compactFirst.compactUniformPayload();
    checkEq(directCompact.vertexConstants, third.compactUniformPayload().vertexConstants,
            "direct compact VS span matches full compact span");
    checkEq(directCompact.pixelConstants, third.compactUniformPayload().pixelConstants,
            "direct compact PS span matches full compact span");

    DrawRunCompactSubmission compactSecond{};
    checkEq(device->coreDevice().snapshotDrawSubmissionFromCurrentState(
                draw, compactSecond, &compactFirst, &compactScratch),
            D3D_OK, "direct compact second draw submission snapshot");
    check(!compactSecond.compactUniforms.has_value(),
          "direct compact second snapshot elides unchanged uniforms");
    check(!compactSecond.stateMaterialized,
          "direct compact second snapshot elides unchanged state");
    checkEq(compactSecond.uniformGeneration, compactFirst.uniformGeneration,
            "direct compact second snapshot keeps uniform generation");
    checkEq(compactScratch.fixedPayloads.size(), std::size_t{1},
            "direct compact elision does not append fixed payloads");
  }
  checkEq(device->Release(), 0u, "compact uniform snapshot device release");
  checkEq(d3d->Release(), 0u, "compact uniform snapshot factory release");
}

void testDrawRunSubmissionCarrierFootprintCounters() {
  const auto carrierBytes = drawRunSubmissionCarrierBytes();
  const auto stateStorageBytes = drawRunSubmissionCarrierStateStorageBytes();
  const auto uniformStorageBytes = drawRunSubmissionCarrierUniformStorageBytes();
  const auto compactUniformStorageBytes =
      drawRunSubmissionCarrierCompactUniformStorageBytes();
  const auto compactCarrierBytes = drawRunCompactSubmissionCarrierBytes();
  const auto compactCarrierStateStorageBytes =
      drawRunCompactSubmissionCarrierStateStorageBytes();
  const auto compactCarrierUniformStorageBytes =
      drawRunCompactSubmissionCarrierUniformStorageBytes();
  const auto compactCarrierCompactUniformStorageBytes =
      drawRunCompactSubmissionCarrierCompactUniformStorageBytes();

  checkEq(carrierBytes, static_cast<std::uint64_t>(sizeof(DrawRunSubmission)),
          "carrier counter matches DrawRunSubmission storage");
  checkEq(stateStorageBytes,
          static_cast<std::uint64_t>(sizeof(std::optional<CanonicalDrawState>)),
          "state storage counter matches inline optional storage");
  checkEq(uniformStorageBytes,
          static_cast<std::uint64_t>(sizeof(std::optional<DrawUniformPayload>)),
          "uniform storage counter matches inline optional storage");
  checkEq(compactUniformStorageBytes,
          static_cast<std::uint64_t>(
              sizeof(std::optional<DrawUniformCompactSubmissionPayload>) +
              sizeof(DrawUniformCompactPayloadArenaView)),
          "compact uniform storage counter matches inline compact storage");
  check(carrierBytes >= stateStorageBytes + uniformStorageBytes +
                            compactUniformStorageBytes,
        "carrier includes the measured inline storage lanes");
  check(uniformStorageBytes > compactUniformStorageBytes,
        "compact uniforms do not shrink the current inline full-uniform carrier");
  checkEq(compactCarrierBytes,
          static_cast<std::uint64_t>(sizeof(DrawRunCompactSubmission)),
          "compact carrier counter matches DrawRunCompactSubmission storage");
  checkEq(compactCarrierStateStorageBytes, stateStorageBytes,
          "compact carrier keeps the same inline state storage lane");
  checkEq(compactCarrierUniformStorageBytes, 0ull,
          "compact carrier has no inline full-uniform storage lane");
  checkEq(compactCarrierCompactUniformStorageBytes,
          compactUniformStorageBytes,
          "compact carrier keeps the compact uniform storage lane");
  check(compactCarrierBytes < carrierBytes,
        "compact carrier removes the inline full-uniform lane");
}

void testZeroSizeBufferLockUsesTailRange() {
  using namespace dxmt9::com;

  auto backend = std::make_shared<RecordingBackend>();
  auto* d3d = Direct3DCreate9Ex(D3D_SDK_VERSION, backend);
  check(d3d != nullptr, "factory for zero-size buffer lock");

  PresentParameters params{};
  params.backBufferWidth = 320;
  params.backBufferHeight = 240;
  params.windowed = true;

  auto* device = d3d->CreateDeviceEx(0, params, nullptr);
  check(device != nullptr, "device for zero-size buffer lock");

  {
    device->AddRef();
    D9CDevice cDevice(device);
    auto* buffer = dxmt9c_device_create_vertex_buffer(&cDevice, 16, 0, 0, 2u);
    check(buffer != nullptr, "create zero-size lock buffer");

    void* data = nullptr;
    checkEq(dxmt9c_buffer_lock(buffer, 0, 16, &data, 0), D3D_OK,
            "initial full lock");
    const std::array<uint8_t, 16> initial{0, 1, 2, 3, 4, 5, 6, 7,
                                          8, 9, 10, 11, 12, 13, 14, 15};
    std::memcpy(data, initial.data(), initial.size());
    checkEq(dxmt9c_buffer_unlock(buffer), D3D_OK, "initial full unlock");

    data = nullptr;
    checkEq(dxmt9c_buffer_lock(buffer, 4, 0, &data, 0), D3D_OK,
            "zero-size lock from offset");
    auto* bytes = static_cast<uint8_t*>(data);
    bytes[0] = 0xaa;
    bytes[11] = 0xbb;
    checkEq(dxmt9c_buffer_unlock(buffer), D3D_OK, "zero-size lock unlock");

    checkEq(backend->bufferUploads.size(), size_t{2},
            "zero-size offset lock uploads once");
    const auto& upload = backend->bufferUploads.back().second;
    checkEq(upload.size(), size_t{16}, "zero-size offset lock keeps buffer extent");
    checkEq(upload[0], initial[0], "zero-size offset lock preserves prefix byte 0");
    checkEq(upload[3], initial[3], "zero-size offset lock preserves prefix byte 3");
    checkEq(upload[4], uint8_t{0xaa}, "zero-size offset lock writes first tail byte");
    checkEq(upload[15], uint8_t{0xbb}, "zero-size offset lock writes last tail byte");

    checkEq(dxmt9c_buffer_release(buffer), 0u, "zero-size lock buffer release");
  }
  checkEq(device->Release(), 0u, "zero-size lock device release");
  checkEq(d3d->Release(), 0u, "zero-size lock factory release");
}

}  // namespace

int main() {
  try {
    testComWrappers();
    testComWrappersEx();
    testPalettizedTextureExpansion();
    testReadOnlyBufferUnlockSkipsUpload();
    testRedundantShaderConstSetKeepsUniformSnapshotReusable();
    testSnapshotDrawSubmissionCompactUniformScratch();
    testDrawRunSubmissionCarrierFootprintCounters();
    testZeroSizeBufferLockUsesTailRange();
  } catch (const TestFailure& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "unexpected exception: " << error.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
