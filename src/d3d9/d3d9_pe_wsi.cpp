#include "d3d9_pe_wsi.hpp"

#include "util/log/log.hpp"

#include <atomic>

namespace {

using dxmt9::wsi::SurfaceProtocol;

std::atomic<bool> g_unsupportedLogged{false};

void logWsiFailure(const char* stage, HWND hwnd, int status) noexcept {
  dxmt9::util::logf(
      dxmt9::util::LogLevel::Error, "dxmt9-wsi",
      "layer_acquisition=unavailable hwnd=0x%llx stage=%s status=%d; "
      "requires Wine ExtEscape surface protocol or an exact "
      "legacy-macdrv-symbols runtime",
      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hwnd)),
      stage, status);
}

void logUnsupportedOnce(HWND hwnd, int status) noexcept {
  bool expected = false;
  if (g_unsupportedLogged.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel)) {
    logWsiFailure("query", hwnd, status);
  }
}

void logLayerAcquisition(const char* path, HWND hwnd) noexcept {
  dxmt9::util::logf(
      dxmt9::util::LogLevel::Info, "dxmt9-wsi",
      "layer_acquisition=%s hwnd=0x%llx", path,
      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hwnd)));
}

bool legacyRuntimeQualified() noexcept {
  char protocol[256]{};
  char runtimeId[256]{};
  const DWORD protocolCount = GetEnvironmentVariableA(
      "DXMT9_WINE_METAL_SURFACE_PROTOCOL", protocol, sizeof(protocol));
  const DWORD runtimeIdCount = GetEnvironmentVariableA(
      "DXMT9_WINE_MANIFEST_ID", runtimeId, sizeof(runtimeId));
  return protocolCount != 0u && protocolCount < sizeof(protocol) &&
         runtimeIdCount != 0u && runtimeIdCount < sizeof(runtimeId) &&
         dxmt9::wsi::isLegacyProtocolDeclaration(
             std::string_view(protocol, protocolCount),
             std::string_view(runtimeId, runtimeIdCount));
}

bool releaseSurfaceOnHdc(HDC hdc, std::uint64_t surfaceToken) noexcept {
  if (!hdc || surfaceToken == 0u) {
    return false;
  }
  macdrv_escape_surface release{
      .surface = surfaceToken,
      .layer = 0u,
  };
  return ExtEscape(
             hdc, MACDRV_ESCAPE_RELEASE_SURFACE, sizeof(release),
             reinterpret_cast<LPCSTR>(&release), 0, nullptr) > 0;
}

}  // namespace

D3D9PeWsiBinding dxmt9PeAcquireWsiBinding(HWND hwnd) noexcept {
  D3D9PeWsiBinding binding{};
  binding.hwnd = static_cast<std::uint64_t>(
      reinterpret_cast<std::uintptr_t>(hwnd));
  if (!hwnd) {
    logUnsupportedOnce(hwnd, 0);
    return binding;
  }

  HDC hdc = GetDC(hwnd);
  if (!hdc) {
    logWsiFailure("getdc", hwnd, 0);
    return binding;
  }

  const int escape = MACDRV_ESCAPE_GET_SURFACE;
  const int query = ExtEscape(
      hdc, QUERYESCSUPPORT, sizeof(escape),
      reinterpret_cast<LPCSTR>(&escape), 0, nullptr);
  binding.protocol = dxmt9::wsi::selectSurfaceProtocol(
      query > 0, legacyRuntimeQualified());

  if (binding.protocol == SurfaceProtocol::ExtEscapeV1) {
    macdrv_escape_surface response{};
    const int result = ExtEscape(
        hdc, MACDRV_ESCAPE_GET_SURFACE, 0, nullptr, sizeof(response),
        reinterpret_cast<LPSTR>(&response));
    if (dxmt9::wsi::validGetSurfaceEscapeCall(
            result, sizeof(response), response)) {
      binding.surfaceToken = response.surface;
      binding.layerToken = response.layer;
      binding.releaseCapability.hdc =
          reinterpret_cast<std::uintptr_t>(hdc);
      return binding;
    } else {
      if (response.surface != 0u) {
        (void)releaseSurfaceOnHdc(hdc, response.surface);
      }
      binding.protocol = SurfaceProtocol::Unsupported;
      logWsiFailure("get-surface", hwnd, result);
    }
  } else if (binding.protocol == SurfaceProtocol::Unsupported) {
    logUnsupportedOnce(hwnd, query);
  }

  if (!ReleaseDC(hwnd, hdc)) {
    logWsiFailure("releasedc", hwnd, 0);
  }
  return binding;
}

HRESULT dxmt9PeAdoptWsiBinding(
    D9CSwapChain* swapChain, D3D9PeWsiBinding& binding) noexcept {
  if (!swapChain || !dxmt9::wsi::validAdoptionCandidate(binding) ||
      (binding.protocol == SurfaceProtocol::ExtEscapeV1 &&
       !binding.releaseCapability.retained())) {
    return D3DERR_NOTAVAILABLE;
  }
  const D9CWsiSurfaceBinding wire{
      .structSize = sizeof(D9CWsiSurfaceBinding),
      .protocol = static_cast<std::uint32_t>(binding.protocol),
      .hwnd = binding.hwnd,
      .surfaceToken = binding.surfaceToken,
      .layerToken = binding.layerToken,
  };
  const HRESULT hr = static_cast<HRESULT>(
      dxmt9c_swapchain_adopt_wsi_surface(swapChain, &wire));
  if (SUCCEEDED(hr)) {
    binding.unixAdopted = true;
    logLayerAcquisition(
        binding.protocol == SurfaceProtocol::ExtEscapeV1
            ? "extescape-v1"
            : "legacy-macdrv-symbols",
        reinterpret_cast<HWND>(static_cast<std::uintptr_t>(binding.hwnd)));
  } else {
    logWsiFailure(
        "unix-adopt",
        reinterpret_cast<HWND>(static_cast<std::uintptr_t>(binding.hwnd)),
        static_cast<int>(hr));
  }
  return hr;
}

HRESULT dxmt9PeAdoptDeviceWsiBinding(
    D9CDevice* device, D3D9PeWsiBinding& binding) noexcept {
  D9CSwapChain* swapChain = device
      ? dxmt9c_device_get_swap_chain(device, 0u)
      : nullptr;
  if (!swapChain) {
    return D3DERR_NOTAVAILABLE;
  }
  const HRESULT hr = dxmt9PeAdoptWsiBinding(swapChain, binding);
  dxmt9c_swapchain_release(swapChain);
  return hr;
}

void dxmt9PeReleaseWsiBindingAfterQuiescence(
    D3D9PeWsiBinding& binding) noexcept {
  HWND hwnd = reinterpret_cast<HWND>(
      static_cast<std::uintptr_t>(binding.hwnd));
  HDC hdc = reinterpret_cast<HDC>(binding.releaseCapability.hdc);
  bool surfaceReleased = true;
  bool dcReleased = true;
  const auto disposition = dxmt9::wsi::dischargeWineRelease(
      binding, binding.releaseCapability.retained(), true,
      [&] {
        surfaceReleased = releaseSurfaceOnHdc(hdc, binding.surfaceToken);
      },
      [&] { dcReleased = hdc && ReleaseDC(hwnd, hdc); });
  if (disposition == dxmt9::wsi::WineReleaseDisposition::MissingCapability ||
      disposition ==
          dxmt9::wsi::WineReleaseDisposition::AwaitingUnixQuiescence) {
    logWsiFailure("release-capability", hwnd, 0);
    return;
  }
  if (!surfaceReleased) {
    logWsiFailure("release-surface", hwnd, 0);
  }
  if (!dcReleased) {
    logWsiFailure("release-dc", hwnd, 0);
  }
  binding.releaseCapability.hdc = 0u;
  binding = {};
}

HRESULT dxmt9PeTeardownAndReleaseWsiBinding(
    D9CSwapChain* swapChain, D3D9PeWsiBinding& binding) noexcept {
  if (binding.protocol == SurfaceProtocol::Unsupported) {
    binding = {};
    return D3D_OK;
  }
  if (binding.unixAdopted) {
    const HRESULT hr = swapChain
        ? static_cast<HRESULT>(
              dxmt9c_swapchain_teardown_wsi_surface(swapChain))
        : D3DERR_NOTAVAILABLE;
    if (FAILED(hr)) {
      logWsiFailure(
          "unix-teardown",
          reinterpret_cast<HWND>(static_cast<std::uintptr_t>(binding.hwnd)),
          static_cast<int>(hr));
      return hr;
    }
  }
  dxmt9PeReleaseWsiBindingAfterQuiescence(binding);
  return D3D_OK;
}

HRESULT dxmt9PeTeardownDeviceAndReleaseWsiBinding(
    D9CDevice* device, D3D9PeWsiBinding& binding) noexcept {
  D9CSwapChain* swapChain = device
      ? dxmt9c_device_get_swap_chain(device, 0u)
      : nullptr;
  const HRESULT hr = dxmt9PeTeardownAndReleaseWsiBinding(swapChain, binding);
  if (swapChain) {
    dxmt9c_swapchain_release(swapChain);
  }
  return hr;
}

void dxmt9PeFinalizeAndReleaseWsiBinding(
    D9CSwapChain* swapChain, D3D9PeWsiBinding& binding) noexcept {
  HRESULT hr = D3D_OK;
  std::uint32_t attempts = 0u;
  do {
    hr = dxmt9PeTeardownAndReleaseWsiBinding(swapChain, binding);
    ++attempts;
    if (SUCCEEDED(hr)) {
      return;
    }
    // Terminal ownership cannot be discarded. The unix finalizer is blocking
    // in production. Bound transient bridge retries, then leak the retained
    // Wine capability rather than hang the process or release it before unix
    // quiescence was proved.
    SwitchToThread();
  } while (dxmt9::wsi::mayRetryFinalWsiTeardown(attempts));
  logWsiFailure(
      "unix-finalize-abandoned",
      reinterpret_cast<HWND>(static_cast<std::uintptr_t>(binding.hwnd)),
      static_cast<int>(hr));
}

void dxmt9PeFinalizeDeviceAndReleaseWsiBinding(
    D9CDevice* device, D3D9PeWsiBinding& binding) noexcept {
  HRESULT hr = D3D_OK;
  std::uint32_t attempts = 0u;
  do {
    hr = dxmt9PeTeardownDeviceAndReleaseWsiBinding(device, binding);
    ++attempts;
    if (SUCCEEDED(hr)) {
      return;
    }
    SwitchToThread();
  } while (dxmt9::wsi::mayRetryFinalWsiTeardown(attempts));
  logWsiFailure(
      "unix-device-finalize-abandoned",
      reinterpret_cast<HWND>(static_cast<std::uintptr_t>(binding.hwnd)),
      static_cast<int>(hr));
}
