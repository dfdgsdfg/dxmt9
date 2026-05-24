#include "core_spec_fixtures.hpp"

#include <memory>

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

}  // namespace

int main() {
  try {
    testComWrappers();
    testComWrappersEx();
  } catch (const TestFailure& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "unexpected exception: " << error.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
