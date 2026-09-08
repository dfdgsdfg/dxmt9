// H300 public D3D9 draw-parent economy conformance fixture.
//
// This is a checked-in, on-demand PE fixture.  It deliberately uses only
// public IDirect3D9/IDirect3DDevice9 interfaces: the measured work is a small
// offscreen FFP draw, with state setters varied one family at a time.

#ifndef _WIN32
#error "d3d9_draw_parent_economy.cpp is a Windows PE fixture"
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

namespace {

constexpr UINT kSize = 32u;
constexpr UINT kIterations = 32u;
constexpr DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;
constexpr UINT kVertexStride = 20u;
constexpr UINT kSecondTriangleOffset = 3u * kVertexStride;
constexpr char kWindowClass[] = "Dxmt9H300DrawParentEconomy";

template <std::size_t N>
constexpr std::array<float, N> repeated(float value) noexcept {
  std::array<float, N> result{};
  result.fill(value);
  return result;
}

struct Vertex {
  float x;
  float y;
  float z;
  float rhw;
  DWORD color;
};

static_assert(sizeof(Vertex) == kVertexStride, "unexpected vertex ABI");

struct WindowFixture {
  HINSTANCE instance = nullptr;
  HWND window = nullptr;
  ATOM atom = 0;

  static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam,
                                     LPARAM lParam) noexcept {
    return DefWindowProcA(hwnd, message, wParam, lParam);
  }

  bool create() noexcept {
    instance = GetModuleHandleA(nullptr);
    WNDCLASSEXA klass{};
    klass.cbSize = sizeof(klass);
    klass.lpfnWndProc = &windowProc;
    klass.hInstance = instance;
    klass.lpszClassName = kWindowClass;
    atom = RegisterClassExA(&klass);
    if (atom == 0u && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    window = CreateWindowExA(0u, kWindowClass, "dxmt9-h300", WS_OVERLAPPED,
                             0, 0, kSize, kSize, nullptr, nullptr, instance,
                             nullptr);
    return window != nullptr;
  }

  ~WindowFixture() {
    if (window) DestroyWindow(window);
    if (atom) UnregisterClassA(kWindowClass, instance);
  }
};

template <typename T>
void release(T*& object) noexcept {
  if (object) {
    object->Release();
    object = nullptr;
  }
}

struct CallAggregate {
  std::uint64_t calls = 0u;
  std::uint64_t failures = 0u;
  std::uint32_t statusBits = 0u;

  void add(HRESULT hr) noexcept {
    ++calls;
    statusBits |= static_cast<std::uint32_t>(hr);
    if (FAILED(hr)) ++failures;
  }
};

struct Row {
  const char* name = nullptr;
  const char* path = nullptr;
  std::uint64_t groups = 0u;
  std::uint64_t calls = 0u;
  std::uint64_t failures = 0u;
  std::uint32_t statusBits = 0u;
  std::uint64_t qpcTicks = 0u;
  std::uint64_t emptyQpcTicks = 0u;
  std::uint64_t cpuUserStart = 0u;
  std::uint64_t cpuUserEnd = 0u;
  std::uint64_t cpuKernelStart = 0u;
  std::uint64_t cpuKernelEnd = 0u;
  DWORD pid = 0u;
  DWORD tid = 0u;
  DWORD centerPixel = 0u;
  DWORD cornerPixel = 0u;
  bool pixelValid = false;
  bool initial = false;
  bool finalState = false;
  bool inputs = false;
};

struct GroupSample {
  const char* name = nullptr;
  const char* path = nullptr;
  std::uint64_t group = 0u;
  bool measured = false;
  std::uint64_t qpcTicks = 0u;
  std::uint64_t tailQpcTicks = 0u;
  std::uint64_t tailGetDataCalls = 0u;
  bool tailDetail = false;
  std::uint64_t endSceneQpcTicks = 0u;
  std::uint64_t issueQpcTicks = 0u;
  std::uint64_t getDataQpcTicks = 0u;
  std::uint64_t otherQpcTicks = 0u;
};

struct TailDetail {
  std::uint64_t endSceneQpcTicks = 0u;
  std::uint64_t issueQpcTicks = 0u;
  std::uint64_t getDataQpcTicks = 0u;
};

struct Fixture {
  IDirect3D9* d3d = nullptr;
  IDirect3DDevice9* device = nullptr;
  IDirect3DVertexBuffer9* vertexBuffer = nullptr;
  IDirect3DIndexBuffer9* indexBuffer = nullptr;
  IDirect3DSurface9* renderTarget = nullptr;
  IDirect3DSurface9* readback = nullptr;
  IDirect3DQuery9* eventQuery = nullptr;
  D3DPRESENT_PARAMETERS present{};
  std::array<float, 4u> vsValuesOne = repeated<4u>(1.0f);
  std::array<float, 12u> vsF3One = repeated<12u>(1.0f);
  std::array<float, 12u> vsF3Equal = repeated<12u>(1.0f);
  std::array<float, 12u> vsF3ChangedOne = repeated<12u>(1.0f);
  std::array<float, 12u> vsF3ChangedTwo = repeated<12u>(2.0f);
  std::array<float, 16u> vsF4One = repeated<16u>(1.0f);
  std::array<float, 16u> vsF4Two = repeated<16u>(2.0f);
  std::array<float, 4u> constFOne = repeated<4u>(1.0f);
  std::array<float, 4u> constFTwo = repeated<4u>(2.0f);
  std::array<int, 4u> constIOne{{1, 1, 1, 1}};
  std::array<int, 4u> constITwo{{2, 2, 2, 2}};
  std::array<BOOL, 4u> constBOne{{TRUE, TRUE, TRUE, TRUE}};
  std::array<BOOL, 4u> constBTwo{{FALSE, FALSE, FALSE, FALSE}};
};

struct InputSnapshot {
  std::array<float, 4u> vsValuesOne{};
  std::array<float, 12u> vsF3One{};
  std::array<float, 12u> vsF3Equal{};
  std::array<float, 12u> vsF3ChangedOne{};
  std::array<float, 12u> vsF3ChangedTwo{};
  std::array<float, 16u> vsF4One{};
  std::array<float, 16u> vsF4Two{};
  std::array<float, 4u> constFOne{};
  std::array<float, 4u> constFTwo{};
  std::array<int, 4u> constIOne{};
  std::array<int, 4u> constITwo{};
  std::array<BOOL, 4u> constBOne{};
  std::array<BOOL, 4u> constBTwo{};
};

struct ProcessClock {
  enum class Status { Valid, Unsupported, NonMonotonic, Zero } status =
      Status::Unsupported;
  std::uint64_t user100ns = 0u;
  std::uint64_t kernel100ns = 0u;
};

enum class Case : std::uint8_t {
  Steady,
  Viewport,
  ViewportScissor,
  Scalar,
  StreamOffset,
  SixConstants,
  VsF3Equal,
  VsF3Changed,
  VsF4Changed,
};

constexpr std::array<const char*, 9u> kCaseNames{{
    "steady", "viewport", "viewport_scissor", "scalar", "stream_offset",
    "six_constants", "vs_f3_equal", "vs_f3_changed", "vs_f4_changed",
}};

enum class Path : std::uint8_t { SettersOnly, Primitive, Indexed };

constexpr std::array<const char*, 3u> kPathNames{{
    "setters_only", "primitive", "indexed"}};

bool qpcNow(std::uint64_t& result) noexcept {
  LARGE_INTEGER value{};
  if (!QueryPerformanceCounter(&value) || value.QuadPart < 0) return false;
  result = static_cast<std::uint64_t>(value.QuadPart);
  return true;
}

bool addQpcTicks(std::uint64_t& total, std::uint64_t ticks) noexcept {
  if (total > std::numeric_limits<std::uint64_t>::max() - ticks) return false;
  total += ticks;
  return true;
}

bool envBounded(const char* name, int fallback, int low, int high,
                int& result) noexcept {
  result = fallback;
  const char* value = std::getenv(name);
  if (!value || !*value) return true;
  char* end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  if (end == value || *end != '\0' || parsed < low || parsed > high)
    return false;
  result = static_cast<int>(parsed);
  return true;
}

void traceMarker(bool trace, const char* marker, const char* caseName,
                 const char* path, int group) noexcept {
  if (!trace) return;
  // The canonical runner concatenates stdout then stderr. All owned records
  // use the recorder logger's stderr stream to preserve temporal ordering.
  std::fprintf(stderr, "draw_parent_trace phase=%s case=%s path=%s group=%d\n", marker,
              caseName, path, group);
  std::fflush(stderr);
}

bool checked(HRESULT hr, const char* label) noexcept {
  if (SUCCEEDED(hr)) return true;
  std::fprintf(stderr, "FAIL: %s hr=0x%08lx\n", label,
               static_cast<unsigned long>(hr));
  return false;
}

bool createDevice(WindowFixture& window, Fixture& fixture) noexcept {
  fixture.d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!fixture.d3d) {
    std::fprintf(stderr, "FAIL: Direct3DCreate9\n");
    return false;
  }
  fixture.present.Windowed = TRUE;
  fixture.present.BackBufferWidth = kSize;
  fixture.present.BackBufferHeight = kSize;
  fixture.present.BackBufferCount = 1u;
  fixture.present.BackBufferFormat = D3DFMT_A8R8G8B8;
  fixture.present.SwapEffect = D3DSWAPEFFECT_DISCARD;
  fixture.present.hDeviceWindow = window.window;
  fixture.present.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  // The fixture is intentionally HARDWARE-only: a silent software fallback
  // would measure a different entry path and is a qualification failure.
  return checked(fixture.d3d->CreateDevice(
                     0u, D3DDEVTYPE_HAL, window.window,
                     D3DCREATE_HARDWARE_VERTEXPROCESSING, &fixture.present,
                     &fixture.device),
                 "CreateDevice(HARDWARE_VERTEXPROCESSING)");
}

bool createResources(Fixture& fixture) noexcept {
  HRESULT hr = fixture.device->CreateRenderTarget(
      kSize, kSize, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0u, FALSE,
      &fixture.renderTarget, nullptr);
  if (!checked(hr, "CreateRenderTarget")) return false;
  hr = fixture.device->CreateOffscreenPlainSurface(
      kSize, kSize, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &fixture.readback,
      nullptr);
  if (!checked(hr, "CreateOffscreenPlainSurface")) return false;
  hr = fixture.device->CreateQuery(D3DQUERYTYPE_EVENT, &fixture.eventQuery);
  if (!checked(hr, "CreateQuery(EVENT)")) return false;

  static const Vertex vertices[6] = {
      {1.0f, 1.0f, 0.5f, 1.0f, 0xff00ff00u},
      {30.0f, 1.0f, 0.5f, 1.0f, 0xff00ff00u},
      {1.0f, 30.0f, 0.5f, 1.0f, 0xff00ff00u},
      {1.0f, 1.0f, 0.5f, 1.0f, 0xff00ff00u},
      {30.0f, 1.0f, 0.5f, 1.0f, 0xff00ff00u},
      {1.0f, 30.0f, 0.5f, 1.0f, 0xff00ff00u},
  };
  hr = fixture.device->CreateVertexBuffer(
      sizeof(vertices), D3DUSAGE_WRITEONLY, kFvf, D3DPOOL_MANAGED,
      &fixture.vertexBuffer, nullptr);
  if (!checked(hr, "CreateVertexBuffer")) return false;
  void* mapped = nullptr;
  hr = fixture.vertexBuffer->Lock(0u, sizeof(vertices), &mapped, 0u);
  if (!checked(hr, "VertexBuffer::Lock")) return false;
  std::memcpy(mapped, vertices, sizeof(vertices));
  if (!checked(fixture.vertexBuffer->Unlock(), "VertexBuffer::Unlock"))
    return false;
  const WORD indices[3] = {0u, 1u, 2u};
  hr = fixture.device->CreateIndexBuffer(
      sizeof(indices), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED,
      &fixture.indexBuffer, nullptr);
  if (!checked(hr, "CreateIndexBuffer")) return false;
  mapped = nullptr;
  if (!checked(fixture.indexBuffer->Lock(0u, sizeof(indices), &mapped, 0u),
               "IndexBuffer::Lock"))
    return false;
  std::memcpy(mapped, indices, sizeof(indices));
  return checked(fixture.indexBuffer->Unlock(), "IndexBuffer::Unlock");
}

bool setBaseState(Fixture& fixture, CallAggregate& a) noexcept {
  IDirect3DDevice9* d = fixture.device;
  D3DVIEWPORT9 viewport{0u, 0u, kSize, kSize, 0.0f, 1.0f};
  RECT scissor{0, 0, static_cast<LONG>(kSize), static_cast<LONG>(kSize)};
  D3DMATRIX identity{};
  identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
  std::array<float, 32u> baseF{};
  baseF.fill(1.0f);
  const int i[4] = {1, 1, 1, 1};
  const BOOL b[4] = {TRUE, TRUE, TRUE, TRUE};
  a.add(d->SetRenderTarget(0u, fixture.renderTarget));
  a.add(d->SetRenderState(D3DRS_LIGHTING, FALSE));
  a.add(d->SetRenderState(D3DRS_ZENABLE, FALSE));
  a.add(d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
  a.add(d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE));
  a.add(d->SetViewport(&viewport));
  a.add(d->SetScissorRect(&scissor));
  a.add(d->SetTransform(D3DTS_WORLD, &identity));
  a.add(d->SetTexture(0u, nullptr));
  a.add(d->SetTextureStageState(0u, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
  a.add(d->SetTextureStageState(0u, D3DTSS_COLORARG1, D3DTA_DIFFUSE));
  a.add(d->SetTextureStageState(0u, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1));
  a.add(d->SetTextureStageState(0u, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE));
  a.add(d->SetSamplerState(0u, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP));
  a.add(d->SetRenderState(D3DRS_TEXTUREFACTOR, 0xff00ff00u));
  a.add(d->SetTextureStageState(0u, D3DTSS_TEXCOORDINDEX, 0u));
  a.add(d->SetVertexShader(nullptr));
  a.add(d->SetPixelShader(nullptr));
  a.add(d->SetVertexShaderConstantF(0u, baseF.data(), 8u));
  a.add(d->SetPixelShaderConstantF(0u, baseF.data(), 8u));
  a.add(d->SetVertexShaderConstantI(0u, i, 1u));
  a.add(d->SetPixelShaderConstantI(0u, i, 1u));
  a.add(d->SetVertexShaderConstantB(0u, b, 1u));
  a.add(d->SetPixelShaderConstantB(0u, b, 1u));
  a.add(d->SetFVF(kFvf));
  a.add(d->SetStreamSource(0u, fixture.vertexBuffer, 0u, kVertexStride));
  a.add(d->SetIndices(fixture.indexBuffer));
  return a.failures == 0u;
}

void addViewport(Fixture& f, UINT width, CallAggregate& a) noexcept {
  D3DVIEWPORT9 v{0u, 0u, width, kSize, 0.0f, 1.0f};
  a.add(f.device->SetViewport(&v));
}

void addScissor(Fixture& f, UINT width, CallAggregate& a) noexcept {
  D3DVIEWPORT9 v{0u, 0u, width, kSize, 0.0f, 1.0f};
  RECT r{0, 0, static_cast<LONG>(width), static_cast<LONG>(kSize)};
  a.add(f.device->SetViewport(&v));
  a.add(f.device->SetScissorRect(&r));
}

void addScalar(Fixture& f, UINT iteration, CallAggregate& a) noexcept {
  const DWORD color = (iteration & 1u) == 0u ? 0xff00ff00u : 0xffff0000u;
  const DWORD address = (iteration & 1u) == 0u ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP;
  const DWORD coord = iteration & 1u;
  D3DMATRIX world{};
  world._11 = world._22 = world._33 = world._44 = 1.0f;
  world._41 = (iteration & 1u) == 0u ? 0.0f : 0.25f;
  a.add(f.device->SetRenderState(D3DRS_TEXTUREFACTOR, color));
  a.add(f.device->SetTextureStageState(0u, D3DTSS_TEXCOORDINDEX, coord));
  a.add(f.device->SetSamplerState(0u, D3DSAMP_ADDRESSU, address));
  a.add(f.device->SetTransform(D3DTS_WORLD, &world));
}

void addStreamOffset(Fixture& f, UINT iteration, CallAggregate& a) noexcept {
  const UINT offset = (iteration & 1u) == 0u ? 0u : kSecondTriangleOffset;
  a.add(f.device->SetStreamSource(0u, f.vertexBuffer, offset, kVertexStride));
}

void addConstants(Fixture& f, UINT iteration, CallAggregate& a) noexcept {
  const bool changed = (iteration & 1u) != 0u;
  const auto& vf = changed ? f.constFTwo : f.constFOne;
  const auto& vi = changed ? f.constITwo : f.constIOne;
  const auto& vb = changed ? f.constBTwo : f.constBOne;
  a.add(f.device->SetVertexShaderConstantF(0u, vf.data(), 1u));
  a.add(f.device->SetPixelShaderConstantF(0u, vf.data(), 1u));
  a.add(f.device->SetVertexShaderConstantI(0u, vi.data(), 1u));
  a.add(f.device->SetPixelShaderConstantI(0u, vi.data(), 1u));
  a.add(f.device->SetVertexShaderConstantB(0u, vb.data(), 1u));
  a.add(f.device->SetPixelShaderConstantB(0u, vb.data(), 1u));
}

void addVsF3Equal(Fixture& f, UINT iteration, CallAggregate& a) noexcept {
  // Distinct immutable addresses with equal values exercise the setter's
  // source-span path without changing the FFP pixel result.
  const auto& values = (iteration & 1u) == 0u ? f.vsF3One : f.vsF3Equal;
  a.add(f.device->SetVertexShaderConstantF(1u, values.data(), 3u));
}

void addVsF3Changed(Fixture& f, UINT iteration, CallAggregate& a) noexcept {
  const auto& values = (iteration & 1u) == 0u ? f.vsF3ChangedOne
                                               : f.vsF3ChangedTwo;
  a.add(f.device->SetVertexShaderConstantF(1u, values.data(), 3u));
}

void addVsF4Changed(Fixture& f, UINT iteration, CallAggregate& a) noexcept {
  const auto& values = (iteration & 1u) == 0u ? f.vsF4One : f.vsF4Two;
  a.add(f.device->SetVertexShaderConstantF(1u, values.data(), 4u));
}

void applyCase(Fixture& f, Case c, UINT iteration, CallAggregate& a) noexcept {
  switch (c) {
    case Case::Steady:
      return;
    case Case::Viewport:
      addViewport(f, (iteration & 1u) == 0u ? 31u : 32u, a);
      return;
    case Case::ViewportScissor:
      addScissor(f, (iteration & 1u) == 0u ? 31u : 32u, a);
      return;
    case Case::Scalar:
      addScalar(f, iteration, a);
      return;
    case Case::StreamOffset:
      addStreamOffset(f, iteration, a);
      return;
    case Case::SixConstants:
      addConstants(f, iteration, a);
      return;
    case Case::VsF3Equal:
      addVsF3Equal(f, iteration, a);
      return;
    case Case::VsF3Changed:
      addVsF3Changed(f, iteration, a);
      return;
    case Case::VsF4Changed:
      addVsF4Changed(f, iteration, a);
      return;
  }
}

bool drainEvent(Fixture& fixture, CallAggregate& a,
                TailDetail* detail = nullptr) noexcept {
  const ULONGLONG deadline = GetTickCount64() + 5000ull;
  for (;;) {
    std::uint64_t start = 0u;
    if (detail && !qpcNow(start)) {
      std::fprintf(stderr, "FAIL: QueryPerformanceCounter(GetData start)\n");
      return false;
    }
    const HRESULT hr = fixture.eventQuery->GetData(nullptr, 0u,
                                                    D3DGETDATA_FLUSH);
    a.add(hr);
    if (detail) {
      std::uint64_t end = 0u;
      if (!qpcNow(end) || end < start ||
          !addQpcTicks(detail->getDataQpcTicks, end - start)) {
        std::fprintf(stderr, "FAIL: GetData QPC span\n");
        return false;
      }
    }
    if (hr == S_OK) return true;
    if (hr != S_FALSE || GetTickCount64() >= deadline) {
      std::fprintf(stderr, "FAIL: event query drain hr=0x%08lx\n",
                   static_cast<unsigned long>(hr));
      return false;
    }
    Sleep(0);
  }
}

bool issueAndDrain(Fixture& fixture, CallAggregate& a,
                   TailDetail* detail = nullptr) noexcept {
  std::uint64_t start = 0u;
  if (detail && !qpcNow(start)) {
    std::fprintf(stderr, "FAIL: QueryPerformanceCounter(Issue start)\n");
    return false;
  }
  a.add(fixture.eventQuery->Issue(D3DISSUE_END));
  if (detail) {
    std::uint64_t end = 0u;
    if (!qpcNow(end) || end < start ||
        !addQpcTicks(detail->issueQpcTicks, end - start)) {
      std::fprintf(stderr, "FAIL: Issue QPC span\n");
      return false;
    }
  }
  if (a.failures != 0u) return false;
  return drainEvent(fixture, a, detail);
}

bool beginGroup(Fixture& fixture, Path path, CallAggregate& a) noexcept {
  IDirect3DDevice9* d = fixture.device;
  a.add(d->Clear(0u, nullptr, D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0u));
  a.add(d->BeginScene());
  if (path == Path::Indexed)
    a.add(d->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0u, 0u, 3u, 0u, 1u));
  else
    a.add(d->DrawPrimitive(D3DPT_TRIANGLELIST, 0u, 1u));
  return a.failures == 0u;
}

void seedChangedVs(Fixture& fixture, Case c, CallAggregate& a) noexcept {
  if (c == Case::VsF3Changed)
    a.add(fixture.device->SetVertexShaderConstantF(
        1u, fixture.vsF3ChangedTwo.data(), 3u));
  else if (c == Case::VsF4Changed)
    a.add(fixture.device->SetVertexShaderConstantF(
        1u, fixture.vsF4Two.data(), 4u));
}

bool pixelOracle(Fixture& fixture, DWORD& center, DWORD& corner) noexcept {
  if (!checked(fixture.device->GetRenderTargetData(fixture.renderTarget,
                                                    fixture.readback),
               "GetRenderTargetData"))
    return false;
  D3DLOCKED_RECT locked{};
  if (!checked(fixture.readback->LockRect(&locked, nullptr, D3DLOCK_READONLY),
               "Readback::LockRect"))
    return false;
  const auto* pixels = static_cast<const std::uint8_t*>(locked.pBits);
  const auto read = [&](UINT x, UINT y) {
    DWORD value = 0u;
    std::memcpy(&value, pixels + y * locked.Pitch + x * sizeof(DWORD),
                sizeof(value));
    return value;
  };
  center = read(4u, 4u);
  corner = read(31u, 31u);
  if (!checked(fixture.readback->UnlockRect(), "Readback::UnlockRect"))
    return false;
  return center == 0xff00ff00u && corner == 0xff000000u;
}

bool validateFinalState(Fixture& fixture, Case c, bool settersRan,
                        CallAggregate& a) noexcept {
  IDirect3DDevice9* d = fixture.device;
  D3DVIEWPORT9 viewport{};
  RECT scissor{};
  DWORD renderState = 0u;
  DWORD textureCoord = 0u;
  DWORD address = 0u;
  DWORD textureFactor = 0u;
  a.add(d->GetViewport(&viewport));
  a.add(d->GetRenderState(D3DRS_SCISSORTESTENABLE, &renderState));
  a.add(d->GetScissorRect(&scissor));
  a.add(d->GetTextureStageState(0u, D3DTSS_TEXCOORDINDEX, &textureCoord));
  a.add(d->GetSamplerState(0u, D3DSAMP_ADDRESSU, &address));
  a.add(d->GetRenderState(D3DRS_TEXTUREFACTOR, &textureFactor));
  if (a.failures != 0u) return false;

  const UINT expectedWidth = 32u;
  bool valid = viewport.Width == expectedWidth && viewport.Height == kSize &&
               renderState == FALSE && scissor.left == 0 && scissor.top == 0 &&
               scissor.right == static_cast<LONG>(kSize) &&
               scissor.bottom == static_cast<LONG>(kSize);
  if (c == Case::Scalar) {
    D3DMATRIX expected{};
    expected._11 = expected._22 = expected._33 = expected._44 = 1.0f;
    expected._41 = settersRan ? 0.25f : 0.0f;
    D3DMATRIX actual{};
    a.add(d->GetTransform(D3DTS_WORLD, &actual));
    if (a.failures != 0u) return false;
    valid = valid && textureCoord == (settersRan ? 1u : 0u) &&
            address == (settersRan ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP) &&
            textureFactor == (settersRan ? 0xffff0000u : 0xff00ff00u) &&
            std::memcmp(&actual, &expected, sizeof(actual)) == 0;
  }

  IDirect3DVertexBuffer9* actualBuffer = nullptr;
  UINT offset = std::numeric_limits<UINT>::max();
  UINT stride = std::numeric_limits<UINT>::max();
  a.add(d->GetStreamSource(0u, &actualBuffer, &offset, &stride));
  if (a.failures != 0u) return false;
  const UINT expectedOffset = c == Case::StreamOffset && settersRan
                                  ? kSecondTriangleOffset
                                  : 0u;
  valid = valid && actualBuffer == fixture.vertexBuffer &&
          offset == expectedOffset && stride == kVertexStride;
  release(actualBuffer);
  IDirect3DIndexBuffer9* actualIndex = nullptr;
  a.add(d->GetIndices(&actualIndex));
  if (a.failures != 0u) return false;
  valid = valid && actualIndex == fixture.indexBuffer;
  release(actualIndex);

  if (c == Case::SixConstants) {
    const float expectedValue = settersRan ? 2.0f : 1.0f;
    const int expectedInteger = settersRan ? 2 : 1;
    const float expectedF[4] = {expectedValue, expectedValue, expectedValue,
                                expectedValue};
    const int expectedI[4] = {expectedInteger, expectedInteger, expectedInteger,
                               expectedInteger};
    const BOOL expectedB[4] = {
        settersRan ? FALSE : TRUE, settersRan ? FALSE : TRUE,
        settersRan ? FALSE : TRUE, settersRan ? FALSE : TRUE};
    float vf[4]{};
    float pf[4]{};
    int vi[4]{};
    int pi[4]{};
    BOOL vb[4]{};
    BOOL pb[4]{};
    a.add(d->GetVertexShaderConstantF(0u, vf, 1u));
    a.add(d->GetPixelShaderConstantF(0u, pf, 1u));
    a.add(d->GetVertexShaderConstantI(0u, vi, 1u));
    a.add(d->GetPixelShaderConstantI(0u, pi, 1u));
    a.add(d->GetVertexShaderConstantB(0u, vb, 1u));
    a.add(d->GetPixelShaderConstantB(0u, pb, 1u));
    if (a.failures != 0u) return false;
    valid = valid &&
            std::memcmp(vf, expectedF, sizeof(vf)) == 0 &&
            std::memcmp(pf, expectedF, sizeof(pf)) == 0 &&
            std::memcmp(vi, expectedI, sizeof(vi)) == 0 &&
            std::memcmp(pi, expectedI, sizeof(pi)) == 0 &&
            vb[0] == expectedB[0] && pb[0] == expectedB[0];
  }
  float vsF[32]{};
  a.add(d->GetVertexShaderConstantF(0u, vsF, 8u));
  if (a.failures != 0u) return false;
  const float expectedPrefix = c == Case::SixConstants && settersRan ? 2.0f : 1.0f;
  for (std::size_t i = 0u; i < 4u; ++i)
    valid = valid && vsF[i] == expectedPrefix;
  for (std::size_t i = 20u; i < 32u; ++i)
    valid = valid && vsF[i] == 1.0f;
  if (c == Case::VsF4Changed) {
    for (std::size_t i = 4u; i < 20u; ++i)
      valid = valid && vsF[i] == 2.0f;
  } else if (c == Case::SixConstants) {
    const float expectedValue = 1.0f;
    for (std::size_t i = 4u; i < 20u; ++i)
      valid = valid && vsF[i] == expectedValue;
  } else if (c == Case::VsF3Changed) {
    for (std::size_t i = 4u; i < 16u; ++i)
      valid = valid && vsF[i] == 2.0f;
    for (std::size_t i = 16u; i < 20u; ++i)
      valid = valid && vsF[i] == 1.0f;
  } else {
    for (std::size_t i = 4u; i < 20u; ++i)
      valid = valid && vsF[i] == 1.0f;
  }
  if (!valid) {
    std::fprintf(stderr,
                 "FAIL: final getter state case=%s viewport=%lux%lu "
                 "scissor_enable=%lu texture_coord=%lu address=%lu "
                 "texture_factor=0x%08lx offset=%u stride=%u\n",
                 kCaseNames[static_cast<std::size_t>(c)], viewport.Width,
                 viewport.Height, static_cast<unsigned long>(renderState),
                 static_cast<unsigned long>(textureCoord),
                 static_cast<unsigned long>(address),
                 static_cast<unsigned long>(textureFactor), offset, stride);
  }
  return valid;
}

bool captureProcessClock(ProcessClock& clock) noexcept;

InputSnapshot captureInputs(const Fixture& fixture) noexcept {
  return {.vsValuesOne = fixture.vsValuesOne,
          .vsF3One = fixture.vsF3One,
          .vsF3Equal = fixture.vsF3Equal,
          .vsF3ChangedOne = fixture.vsF3ChangedOne,
          .vsF3ChangedTwo = fixture.vsF3ChangedTwo,
          .vsF4One = fixture.vsF4One,
          .vsF4Two = fixture.vsF4Two,
          .constFOne = fixture.constFOne,
          .constFTwo = fixture.constFTwo,
          .constIOne = fixture.constIOne,
          .constITwo = fixture.constITwo,
          .constBOne = fixture.constBOne,
          .constBTwo = fixture.constBTwo};
}

bool inputsSnapshotValid(const Fixture& fixture,
                         const InputSnapshot& snapshot) noexcept {
  return fixture.vsValuesOne == snapshot.vsValuesOne &&
         fixture.vsF3One == snapshot.vsF3One &&
         fixture.vsF3Equal == snapshot.vsF3Equal &&
         fixture.vsF3ChangedOne == snapshot.vsF3ChangedOne &&
         fixture.vsF3ChangedTwo == snapshot.vsF3ChangedTwo &&
         fixture.vsF4One == snapshot.vsF4One &&
         fixture.vsF4Two == snapshot.vsF4Two &&
         fixture.constFOne == snapshot.constFOne &&
         fixture.constFTwo == snapshot.constFTwo &&
         fixture.constIOne == snapshot.constIOne &&
         fixture.constITwo == snapshot.constITwo &&
         fixture.constBOne == snapshot.constBOne &&
         fixture.constBTwo == snapshot.constBTwo;
}

bool runRow(Fixture& fixture, Case c, Path path, int warmGroups, int groups,
            std::uint64_t emptyQpcTicks, bool trace, bool groupDetail,
            bool tailDetail,
            GroupSample* groupSamples, std::size_t groupSampleCapacity,
            std::size_t& groupSampleCount, const InputSnapshot& snapshot,
            Row& row) noexcept {
  row.name = kCaseNames[static_cast<std::size_t>(c)];
  row.path = kPathNames[static_cast<std::size_t>(path)];
  row.groups = static_cast<std::uint64_t>(groups);
  row.emptyQpcTicks = emptyQpcTicks;
  row.pid = GetCurrentProcessId();
  row.tid = GetCurrentThreadId();
  ProcessClock before{}, after{};
  if (!captureProcessClock(before)) return false;
  for (int group = 0; group < warmGroups + groups; ++group) {
    const bool measured = group >= warmGroups;
    traceMarker(trace, "begin", row.name, row.path, group);
    CallAggregate setup{};
    if (!setBaseState(fixture, setup) || !beginGroup(fixture, path, setup))
      return false;
    seedChangedVs(fixture, c, setup);
    if (setup.failures != 0u) return false;
    if (group == 0) {
      CallAggregate initialGetters{};
      const bool changedSeed = c == Case::VsF3Changed || c == Case::VsF4Changed;
      row.initial = validateFinalState(fixture, c, changedSeed, initialGetters) &&
                    inputsSnapshotValid(fixture, snapshot);
      if (!row.initial) return false;
    }
    if (path != Path::SettersOnly)
      setup.add(fixture.device->Clear(0u, nullptr, D3DCLEAR_TARGET,
                                      0xff000000u, 1.0f, 0u));
    if (setup.failures != 0u) return false;
    CallAggregate timed{};
    traceMarker(trace, "timed_begin", row.name, row.path, group);
    std::uint64_t start = 0u;
    if (!qpcNow(start)) {
      std::fprintf(stderr, "FAIL: QueryPerformanceCounter(start)\n");
      return false;
    }
    for (UINT iteration = 0u; iteration < kIterations; ++iteration) {
      applyCase(fixture, c, iteration, timed);
      if (path == Path::Primitive)
        timed.add(fixture.device->DrawPrimitive(
            D3DPT_TRIANGLELIST, 0u, 1u));
      else if (path == Path::Indexed)
        timed.add(fixture.device->DrawIndexedPrimitive(
            D3DPT_TRIANGLELIST, 0u, 0u, 3u, 0u, 1u));
    }
    std::uint64_t end = 0u;
    if (!qpcNow(end) || end < start) {
      std::fprintf(stderr, "FAIL: QueryPerformanceCounter(end)\n");
      return false;
    }
    traceMarker(trace, "timed_end", row.name, row.path, group);
    std::uint64_t tailStart = 0u;
    if (groupDetail && (!qpcNow(tailStart) || tailStart < end)) {
      std::fprintf(stderr, "FAIL: QueryPerformanceCounter(tail start)\n");
      return false;
    }
    CallAggregate tail{};
    TailDetail tailDetailData{};
    TailDetail* tailDetailPtr = tailDetail ? &tailDetailData : nullptr;
    std::uint64_t endSceneStart = 0u;
    if (tailDetailPtr && !qpcNow(endSceneStart)) {
      std::fprintf(stderr,
                   "FAIL: QueryPerformanceCounter(EndScene start)\n");
      return false;
    }
    tail.add(fixture.device->EndScene());
    if (tailDetailPtr) {
      std::uint64_t endSceneEnd = 0u;
      if (!qpcNow(endSceneEnd) || endSceneEnd < endSceneStart ||
          !addQpcTicks(tailDetailData.endSceneQpcTicks,
                        endSceneEnd - endSceneStart)) {
        std::fprintf(stderr, "FAIL: EndScene QPC span\n");
        return false;
      }
    }
    const bool drained = tail.failures == 0u &&
                         issueAndDrain(fixture, tail, tailDetailPtr);
    if (!drained || timed.failures != 0u || tail.failures != 0u) return false;
    std::uint64_t tailEnd = 0u;
    if (groupDetail && (!qpcNow(tailEnd) || tailEnd < tailStart)) {
      std::fprintf(stderr, "FAIL: QueryPerformanceCounter(tail end)\n");
      return false;
    }
    if (groupDetail) {
      if (groupSamples == nullptr || groupSampleCount >= groupSampleCapacity ||
          tail.calls < 3u) {
        std::fprintf(stderr, "FAIL: draw-parent group sample storage\n");
        return false;
      }
      // tail.calls is EndScene + Issue + every GetData poll; retain the
      // derived poll count without adding a second per-poll counter.
      GroupSample& sample = groupSamples[groupSampleCount++];
      sample.name = row.name;
      sample.path = row.path;
      sample.group = static_cast<std::uint64_t>(group);
      sample.measured = measured;
      sample.qpcTicks = end - start;
      sample.tailQpcTicks = tailEnd - tailStart;
      sample.tailGetDataCalls = tail.calls - 2u;
      sample.tailDetail = tailDetail;
      if (tailDetail) {
        std::uint64_t componentTicks = tailDetailData.endSceneQpcTicks;
        if (!addQpcTicks(componentTicks, tailDetailData.issueQpcTicks) ||
            !addQpcTicks(componentTicks, tailDetailData.getDataQpcTicks) ||
            componentTicks > sample.tailQpcTicks) {
          std::fprintf(stderr, "FAIL: tail QPC component sum\n");
          return false;
        }
        sample.endSceneQpcTicks = tailDetailData.endSceneQpcTicks;
        sample.issueQpcTicks = tailDetailData.issueQpcTicks;
        sample.getDataQpcTicks = tailDetailData.getDataQpcTicks;
        sample.otherQpcTicks = sample.tailQpcTicks - componentTicks;
      }
    }
    if (group == warmGroups + groups - 1) {
      CallAggregate finalGetters{};
      row.finalState = validateFinalState(fixture, c, c != Case::Steady,
                                           finalGetters);
      row.pixelValid = row.finalState && pixelOracle(fixture, row.centerPixel,
                                                 row.cornerPixel);
      row.inputs = inputsSnapshotValid(fixture, snapshot);
      if (!row.finalState || !row.pixelValid || !row.inputs) return false;
    }
    if (measured) {
      const std::uint64_t drawTicks = end - start;
      if (row.qpcTicks > std::numeric_limits<std::uint64_t>::max() -
                              drawTicks) {
        std::fprintf(stderr, "FAIL: draw-parent QPC sum overflow\n");
        return false;
      }
      row.calls += timed.calls;
      row.failures += timed.failures;
      row.statusBits |= timed.statusBits;
      row.qpcTicks += drawTicks;
    }
    traceMarker(trace, "end", row.name, row.path, group);
  }
  if (!captureProcessClock(after)) return false;
  row.cpuUserStart = before.user100ns;
  row.cpuUserEnd = after.user100ns;
  row.cpuKernelStart = before.kernel100ns;
  row.cpuKernelEnd = after.kernel100ns;
  return row.failures == 0u;
}

bool captureProcessClock(ProcessClock& clock) noexcept {
  FILETIME creation{}, exit{}, kernel{}, user{};
  if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
    return false;
  ULARGE_INTEGER kernelValue{};
  ULARGE_INTEGER userValue{};
  kernelValue.LowPart = kernel.dwLowDateTime;
  kernelValue.HighPart = kernel.dwHighDateTime;
  userValue.LowPart = user.dwLowDateTime;
  userValue.HighPart = user.dwHighDateTime;
  clock.status = ProcessClock::Status::Valid;
  clock.kernel100ns = kernelValue.QuadPart;
  clock.user100ns = userValue.QuadPart;
  return true;
}

bool inspectDrawPrimitiveBinding(IDirect3DDevice9* device) noexcept {
  HMODULE module = GetModuleHandleA("d3d9.dll");
  char modulePath[MAX_PATH]{};
  const DWORD pathLength = module
      ? GetModuleFileNameA(module, modulePath, sizeof(modulePath))
      : 0u;
  void** const vtable = *reinterpret_cast<void***>(device);
  void* const drawPrimitive = vtable[81u];
  void* const drawIndexedPrimitive = vtable[82u];
  const auto inModule = [&](void* address) {
    MEMORY_BASIC_INFORMATION memory{};
    return module && pathLength != 0u && address != nullptr &&
           VirtualQuery(address, &memory, sizeof(memory)) != 0 &&
           memory.AllocationBase == module;
  };
  if (!inModule(drawPrimitive) || !inModule(drawIndexedPrimitive)) {
    std::fprintf(stderr, "FAIL: DrawPrimitive/DrawIndexedPrimitive vtable binding\n");
    return false;
  }
  const auto* base = reinterpret_cast<const std::uint8_t*>(module);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  WORD machine = 0u;
  if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature == IMAGE_NT_SIGNATURE) machine = nt->FileHeader.Machine;
  }
  const auto primitiveRva = reinterpret_cast<const std::uint8_t*>(drawPrimitive) - base;
  const auto indexedRva = reinterpret_cast<const std::uint8_t*>(drawIndexedPrimitive) - base;
  std::fprintf(stderr, "draw_parent_binding module=%s machine=0x%04x primitive_rva=0x%zx indexed_rva=0x%zx\n",
              modulePath, static_cast<unsigned>(machine),
              static_cast<std::size_t>(primitiveRva),
              static_cast<std::size_t>(indexedRva));
  std::fflush(stderr);
  return true;
}

void printRow(const Row& row) noexcept {
  std::fprintf(stderr,
      "draw_parent_row case=%s path=%s groups=%llu iterations=%u "
      "qpc_ticks=%llu emptyqpc_ticks=%llu calls=%llu "
      "hresult_failures=%llu status=0x%08x initial=%s final=%s inputs=%s "
      "pixel_center=0x%08lx pixel_corner=0x%08lx "
      "cpu_scope=process cpu_includes=setup_warm_drain_oracle "
      "cpu_user_start_100ns=%llu cpu_user_end_100ns=%llu "
      "cpu_kernel_start_100ns=%llu cpu_kernel_end_100ns=%llu pid=%lu tid=%lu\n",
      row.name, row.path, static_cast<unsigned long long>(row.groups),
      kIterations, static_cast<unsigned long long>(row.qpcTicks),
      static_cast<unsigned long long>(row.emptyQpcTicks),
      static_cast<unsigned long long>(row.calls),
      static_cast<unsigned long long>(row.failures),
      row.statusBits, row.initial ? "pass" : "fail",
      row.finalState ? "pass" : "fail", row.inputs ? "pass" : "fail",
      static_cast<unsigned long>(row.centerPixel),
      static_cast<unsigned long>(row.cornerPixel),
      static_cast<unsigned long long>(row.cpuUserStart),
      static_cast<unsigned long long>(row.cpuUserEnd),
      static_cast<unsigned long long>(row.cpuKernelStart),
      static_cast<unsigned long long>(row.cpuKernelEnd), row.pid, row.tid);
}

void printGroupSamples(const GroupSample* samples,
                       std::size_t sampleCount) noexcept {
  for (std::size_t i = 0u; i < sampleCount; ++i) {
    const GroupSample& sample = samples[i];
    std::fprintf(stderr,
                 "draw_parent_group case=%s path=%s group=%llu measured=%d "
                 "qpc_ticks=%llu tail_qpc_ticks=%llu tail_getdata_calls=%llu\n",
                 sample.name, sample.path,
                 static_cast<unsigned long long>(sample.group),
                 sample.measured ? 1 : 0,
                 static_cast<unsigned long long>(sample.qpcTicks),
                 static_cast<unsigned long long>(sample.tailQpcTicks),
                 static_cast<unsigned long long>(sample.tailGetDataCalls));
    if (sample.tailDetail) {
      std::fprintf(stderr,
                   "draw_parent_tail case=%s path=%s group=%llu "
                   "end_scene_qpc_ticks=%llu issue_qpc_ticks=%llu "
                   "getdata_qpc_ticks=%llu other_qpc_ticks=%llu\n",
                   sample.name, sample.path,
                   static_cast<unsigned long long>(sample.group),
                   static_cast<unsigned long long>(sample.endSceneQpcTicks),
                   static_cast<unsigned long long>(sample.issueQpcTicks),
                   static_cast<unsigned long long>(sample.getDataQpcTicks),
                   static_cast<unsigned long long>(sample.otherQpcTicks));
    }
  }
}

bool cleanup(Fixture& fixture) noexcept {
  bool valid = true;
  CallAggregate aggregate{};
  if (fixture.device) {
    valid = checked(fixture.device->SetStreamSource(0u, nullptr, 0u, 0u),
                    "cleanup SetStreamSource(null)") && valid;
    valid = checked(fixture.device->SetIndices(nullptr),
                    "cleanup SetIndices(null)") && valid;
    // Fence the final public state transition before releasing resources.
    if (fixture.eventQuery) {
      valid = issueAndDrain(fixture, aggregate) && valid;
    }
  }
  release(fixture.eventQuery);
  release(fixture.readback);
  // Drop caller refs before Reset detaches device-owned bindings. In
  // particular RT0 cannot be set to null, and its Release need not be zero
  // while the device still owns the attachment.
  release(fixture.renderTarget);
  release(fixture.indexBuffer);
  release(fixture.vertexBuffer);
  if (fixture.device) {
    // Do not acquire an implicit backbuffer merely for cleanup.  Reset is the
    // established D3D9 fixture boundary that detaches the swapchain-owned
    // default target after caller-owned resources have been released.
    valid = checked(fixture.device->Reset(&fixture.present),
                    "cleanup Reset(detach default target)") && valid;
    IDirect3DDevice9* device = fixture.device;
    fixture.device = nullptr;
    const ULONG references = device->Release();
    if (references != 0u) valid = false;
  }
  ULONG d3dRefs = 0u;
  if (fixture.d3d) {
    d3dRefs = fixture.d3d->Release();
    fixture.d3d = nullptr;
    valid = d3dRefs == 0u && valid;
  }
  std::fprintf(stderr, "draw_parent_release refs=%lu\n",
              static_cast<unsigned long>(
                  valid ? 0u : std::max<ULONG>(1u, d3dRefs)));
  return valid;
}

}  // namespace

int runDrawParentEconomy() {
  LARGE_INTEGER frequency{};
  if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
    std::fprintf(stderr, "FAIL: QueryPerformanceFrequency\n");
    return 1;
  }
  const std::uint64_t qpcFrequency = static_cast<std::uint64_t>(frequency.QuadPart);
  int groups = 512;
  int warmGroups = 8;
  int traceValue = 0;
  int groupDetailValue = 0;
  int tailDetailValue = 0;
  if (!envBounded("DXMT9_TEST_DRAW_PARENT_GROUPS", 512, 1, 8192, groups) ||
      !envBounded("DXMT9_TEST_DRAW_PARENT_WARM_GROUPS", 8, 0, 128,
                  warmGroups) ||
      !envBounded("DXMT9_TEST_DRAW_PARENT_TRACE", 0, 0, 1, traceValue) ||
      !envBounded("DXMT9_TEST_DRAW_PARENT_GROUP_DETAIL", 0, 0, 1,
                  groupDetailValue) ||
      !envBounded("DXMT9_TEST_DRAW_PARENT_TAIL_DETAIL", 0, 0, 1,
                  tailDetailValue)) {
    std::fprintf(stderr, "FAIL: invalid draw-parent environment bounds\n");
    return 1;
  }
  const bool trace = traceValue != 0;
  const bool groupDetail = groupDetailValue != 0;
  const bool tailDetail = tailDetailValue != 0;
  if (tailDetail && !groupDetail) {
    std::fprintf(stderr,
                 "FAIL: DXMT9_TEST_DRAW_PARENT_TAIL_DETAIL requires "
                 "DXMT9_TEST_DRAW_PARENT_GROUP_DETAIL=1\n");
    return 1;
  }
  std::uint64_t emptyStart = 0u;
  std::uint64_t emptyEnd = 0u;
  if (!qpcNow(emptyStart) || !qpcNow(emptyEnd) || emptyEnd < emptyStart) {
    std::fprintf(stderr, "FAIL: QueryPerformanceCounter(empty pair)\n");
    return 1;
  }
  const std::uint64_t emptyQpcTicks = emptyEnd - emptyStart;
  const char* arch = sizeof(void*) == 8u ? "x64" : "x86";
  if (tailDetail) {
    std::fprintf(stderr,
                 "draw_parent_meta schema=3 arch=%s qpc_frequency=%llu "
                 "groups=%d warm_groups=%d iterations=%u trace=%d "
                 "group_detail=1 tail_detail=1 pid=%lu tid=%lu\n",
                 arch, static_cast<unsigned long long>(qpcFrequency), groups,
                 warmGroups, kIterations, trace ? 1 : 0,
                 static_cast<unsigned long>(GetCurrentProcessId()),
                 static_cast<unsigned long>(GetCurrentThreadId()));
  } else if (groupDetail) {
    std::fprintf(stderr,
                 "draw_parent_meta schema=2 arch=%s qpc_frequency=%llu "
                 "groups=%d warm_groups=%d iterations=%u trace=%d "
                 "group_detail=1 pid=%lu tid=%lu\n",
                 arch, static_cast<unsigned long long>(qpcFrequency), groups,
                 warmGroups, kIterations, trace ? 1 : 0,
                 static_cast<unsigned long>(GetCurrentProcessId()),
                 static_cast<unsigned long>(GetCurrentThreadId()));
  } else {
    std::fprintf(stderr,
                 "draw_parent_meta schema=1 arch=%s qpc_frequency=%llu "
                 "groups=%d warm_groups=%d iterations=%u trace=%d pid=%lu "
                 "tid=%lu\n",
                 arch, static_cast<unsigned long long>(qpcFrequency), groups,
                 warmGroups, kIterations, trace ? 1 : 0,
                 static_cast<unsigned long>(GetCurrentProcessId()),
                 static_cast<unsigned long>(GetCurrentThreadId()));
  }
  std::fflush(stderr);

  WindowFixture window{};
  Fixture fixture{};
  if (!window.create() || !createDevice(window, fixture) ||
      !createResources(fixture) || !inspectDrawPrimitiveBinding(fixture.device)) {
    if (!cleanup(fixture)) std::fprintf(stderr, "FAIL: cleanup\n");
    return 1;
  }

  const std::size_t groupCountPerRow =
      static_cast<std::size_t>(warmGroups) + static_cast<std::size_t>(groups);
  const std::size_t rowCount = kCaseNames.size() * kPathNames.size();
  const std::size_t groupSampleCapacity =
      groupDetail ? rowCount * groupCountPerRow : 0u;
  GroupSample* groupSamples = nullptr;
  if (groupDetail) {
    groupSamples = new (std::nothrow) GroupSample[groupSampleCapacity];
    if (!groupSamples) {
      std::fprintf(stderr, "FAIL: draw-parent group sample allocation\n");
      if (!cleanup(fixture)) std::fprintf(stderr, "FAIL: cleanup\n");
      return 1;
    }
  }
  std::size_t groupSampleCount = 0u;
  const InputSnapshot inputSnapshot = captureInputs(fixture);
  std::array<Row, 27u> rows{};
  std::size_t rowIndex = 0u;
  bool success = true;
  for (std::size_t caseIndex = 0u;
       caseIndex < kCaseNames.size() && success; ++caseIndex) {
    for (const Path path : {Path::SettersOnly, Path::Primitive, Path::Indexed}) {
      if (!runRow(fixture, static_cast<Case>(caseIndex), path, warmGroups,
                  groups, emptyQpcTicks, trace, groupDetail, tailDetail,
                  groupSamples,
                  groupSampleCapacity, groupSampleCount, inputSnapshot,
                  rows[rowIndex])) {
        success = false;
        break;
      }
      ++rowIndex;
    }
  }
  if (!success) {
    delete[] groupSamples;
    if (!cleanup(fixture)) std::fprintf(stderr, "FAIL: cleanup\n");
    return 1;
  }
  if (groupDetail && groupSampleCount != groupSampleCapacity) {
    std::fprintf(stderr, "FAIL: incomplete draw-parent group samples\n");
    delete[] groupSamples;
    if (!cleanup(fixture)) std::fprintf(stderr, "FAIL: cleanup\n");
    return 1;
  }
  if (groupDetail) printGroupSamples(groupSamples, groupSampleCount);
  for (const Row& row : rows) printRow(row);
  delete[] groupSamples;
  if (!cleanup(fixture)) return 1;
  if (tailDetail)
    std::fputs("draw_parent_complete schema=3 rows=27 status=pass\n", stderr);
  else if (groupDetail)
    std::fputs("draw_parent_complete schema=2 rows=27 status=pass\n", stderr);
  else
    std::fputs("draw_parent_complete schema=1 rows=27 status=pass\n", stderr);
  return 0;
}

int main() {
  return runDrawParentEconomy();
}
