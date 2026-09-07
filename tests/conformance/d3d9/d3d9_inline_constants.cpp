/*
 * Real PE pixel oracle for the opt-in inline shader-constant path.
 *
 * This fixture deliberately uses authored, executable SM3 bytecode instead of
 * shader getters or a scanner-only blob.  Each shader consumes one float, int,
 * and bool register file; the six family cases change exactly one file in an
 * A-A-B-A draw sequence.  All pixels are read back once after the scene, so a
 * stale inline binding cannot hide behind an intermediate flush.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

int failures = 0;

void fail_at(int line, const char *message) {
  std::printf("FAIL:%d: %s\n", line, message);
  ++failures;
}

void check_at(int line, bool condition, const char *message) {
  if (!condition) fail_at(line, message);
}

void check_hr_at(int line, HRESULT actual, HRESULT expected, const char *call) {
  if (actual != expected) {
    std::printf("FAIL:%d: %s returned 0x%08lx, expected 0x%08lx\n", line, call,
        static_cast<unsigned long>(actual), static_cast<unsigned long>(expected));
    ++failures;
  }
}

#define CHECK(condition) check_at(__LINE__, !!(condition), #condition)
#define CHECK_HR(actual, expected) check_hr_at(__LINE__, (actual), (expected), #actual)

template <typename T>
void release_resource(T *object, const char *name) {
  if (!object) return;
  const ULONG remaining = object->Release();
  std::printf("REACHED:inline_constants:release_%s=%lu\n", name,
      static_cast<unsigned long>(remaining));
}

// D3D9 token helpers.  They encode the public shader bytecode format directly
// so this test has no dependency on D3DCompiler at runtime.
constexpr DWORD reg_token(DWORD type, DWORD index) {
  return 0x80000000u | ((type << 28u) & 0x70000000u) |
      ((type << 8u) & 0x00001800u) | (index & 0x7ffu);
}

constexpr DWORD dst_token(DWORD type, DWORD index, DWORD mask = 0xfu) {
  return reg_token(type, index) | ((mask & 0xfu) << 16u);
}

constexpr DWORD src_token(DWORD type, DWORD index) {
  return reg_token(type, index) | D3DSP_NOSWIZZLE;
}

constexpr DWORD instruction(DWORD opcode, DWORD operands) {
  return (opcode & D3DSI_OPCODE_MASK) | (operands << D3DSI_INSTLENGTH_SHIFT);
}

constexpr DWORD dcl_token(DWORD usage, DWORD usage_index) {
  return 0x80000000u | (usage & 0xfu) | ((usage_index & 0xfu) << 16u);
}

// vs_3_0: v0 is a FLOAT4 POSITION in clip space.  o1 carries VS float plus
// one scaled VS-int REP iteration and one scaled VS-bool IF contribution.
static const DWORD kVertexShader[] = {
    D3DVS_VERSION(3, 0),
    instruction(D3DSIO_DCL, 2), dcl_token(D3DDECLUSAGE_POSITION, 0),
        dst_token(D3DSPR_INPUT, 0),
    instruction(D3DSIO_DCL, 2), dcl_token(D3DDECLUSAGE_POSITION, 0),
        dst_token(D3DSPR_OUTPUT, 0),
    instruction(D3DSIO_DCL, 2), dcl_token(D3DDECLUSAGE_COLOR, 0),
        dst_token(D3DSPR_OUTPUT, 1),
    instruction(D3DSIO_MOV, 2), dst_token(D3DSPR_OUTPUT, 0), src_token(D3DSPR_INPUT, 0),
    instruction(D3DSIO_MOV, 2), dst_token(D3DSPR_TEMP, 0), src_token(D3DSPR_CONST, 0),
    instruction(D3DSIO_REP, 1), src_token(D3DSPR_CONSTINT, 0),
    instruction(D3DSIO_ADD, 3), dst_token(D3DSPR_TEMP, 0),
        src_token(D3DSPR_TEMP, 0), src_token(D3DSPR_CONST, 4),
    instruction(D3DSIO_ENDREP, 0),
    instruction(D3DSIO_IF, 1), src_token(D3DSPR_CONSTBOOL, 0),
    instruction(D3DSIO_ADD, 3), dst_token(D3DSPR_TEMP, 0),
        src_token(D3DSPR_TEMP, 0), src_token(D3DSPR_CONST, 5),
    instruction(D3DSIO_ENDIF, 0),
    instruction(D3DSIO_MOV, 2), dst_token(D3DSPR_OUTPUT, 1), src_token(D3DSPR_TEMP, 0),
    0x0000ffffu,
};

// ps_3_0: v0 is the interpolated VS color.  It adds PS float plus executable
// REP/IF contributions from PS integer and bool constant registers.
static const DWORD kPixelShader[] = {
    D3DPS_VERSION(3, 0),
    instruction(D3DSIO_DCL, 2), dcl_token(D3DDECLUSAGE_COLOR, 0),
        dst_token(D3DSPR_INPUT, 0),
    instruction(D3DSIO_MOV, 2), dst_token(D3DSPR_TEMP, 0), src_token(D3DSPR_INPUT, 0),
    instruction(D3DSIO_ADD, 3), dst_token(D3DSPR_TEMP, 0),
        src_token(D3DSPR_TEMP, 0), src_token(D3DSPR_CONST, 0),
    instruction(D3DSIO_REP, 1), src_token(D3DSPR_CONSTINT, 0),
    instruction(D3DSIO_ADD, 3), dst_token(D3DSPR_TEMP, 0),
        src_token(D3DSPR_TEMP, 0), src_token(D3DSPR_CONST, 4),
    instruction(D3DSIO_ENDREP, 0),
    instruction(D3DSIO_IF, 1), src_token(D3DSPR_CONSTBOOL, 0),
    instruction(D3DSIO_ADD, 3), dst_token(D3DSPR_TEMP, 0),
        src_token(D3DSPR_TEMP, 0), src_token(D3DSPR_CONST, 5),
    instruction(D3DSIO_ENDIF, 0),
    instruction(D3DSIO_MOV, 2), dst_token(D3DSPR_COLOROUT, 0), src_token(D3DSPR_TEMP, 0),
    0x0000ffffu,
};

constexpr unsigned kWidth = 64;
constexpr unsigned kRowsPerPath = 6;
constexpr unsigned kPathCount = 3;
constexpr unsigned kRows = kRowsPerPath * kPathCount;
constexpr unsigned kHeight = kRows * 16;

enum class Family : unsigned {
  VsFloat,
  VsInt,
  VsBool,
  PsFloat,
  PsInt,
  PsBool,
};

const char *family_name(Family family) {
  switch (family) {
    case Family::VsFloat: return "vs_float";
    case Family::VsInt: return "vs_int";
    case Family::VsBool: return "vs_bool";
    case Family::PsFloat: return "ps_float";
    case Family::PsInt: return "ps_int";
    case Family::PsBool: return "ps_bool";
  }
  return "unknown";
}

// Independent expected colors for baseline A and each isolated B mutation.
// Values are ARGB8, not derived from the output readback.
constexpr D3DCOLOR kBaseline = 0xff293d52u;
constexpr std::array<D3DCOLOR, 6> kChanged = {
    0xff47384fu,  // VS float: (.28, .22, .31)
    0xff4f3d52u,  // VS int:   (.31, .24, .32)
    0xff295752u,  // VS bool:  (.16, .34, .32)
    0xff2e5247u,  // PS float: (.18, .32, .28)
    0xff293d6eu,  // PS int:   (.16, .24, .43)
    0xff3d3d52u,  // PS bool:  (.24, .24, .32)
};

struct Vertex {
  float x;
  float y;
  float z;
  float w;
};

Vertex make_vertex(unsigned row, unsigned slot, unsigned vertex) {
  const float left = -1.0f + 2.0f * static_cast<float>(slot * 16u) /
      static_cast<float>(kWidth);
  const float right = -1.0f + 2.0f * static_cast<float>((slot + 1u) * 16u) /
      static_cast<float>(kWidth);
  const float top = 1.0f - 2.0f * static_cast<float>(row * 16u) /
      static_cast<float>(kHeight);
  const float bottom = 1.0f - 2.0f * static_cast<float>((row + 1u) * 16u) /
      static_cast<float>(kHeight);
  if (vertex == 0) return {left, top, 0.5f, 1.0f};
  if (vertex == 1) return {right, top, 0.5f, 1.0f};
  return {left, bottom, 0.5f, 1.0f};
}

HWND create_window() {
  RECT rect = {0, 0, 64, 64};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  return CreateWindowA("static", "dxmt9_d3d9_inline_constants", WS_OVERLAPPEDWINDOW,
      0, 0, rect.right - rect.left, rect.bottom - rect.top,
      nullptr, nullptr, nullptr, nullptr);
}

IDirect3DDevice9 *create_device(IDirect3D9 *d3d9, HWND window, HRESULT *last_hr) {
  D3DPRESENT_PARAMETERS pp = {};
  pp.BackBufferWidth = 64;
  pp.BackBufferHeight = 64;
  pp.BackBufferFormat = D3DFMT_A8R8G8B8;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = window;
  pp.Windowed = TRUE;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

  IDirect3DDevice9 *device = nullptr;
  const HRESULT hr = d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
      D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device);
  if (last_hr) *last_hr = hr;
  if (SUCCEEDED(hr)) {
    return device;
  }
  return nullptr;
}

struct Fixture {
  HWND window = nullptr;
  IDirect3D9 *d3d9 = nullptr;
  IDirect3DDevice9 *device = nullptr;
  IDirect3DSurface9 *render_target = nullptr;
  IDirect3DSurface9 *readback = nullptr;
  IDirect3DVertexBuffer9 *vertices = nullptr;
  IDirect3DIndexBuffer9 *indices = nullptr;
  IDirect3DVertexDeclaration9 *declaration = nullptr;
  IDirect3DVertexShader9 *vs = nullptr;
  IDirect3DPixelShader9 *ps = nullptr;

  bool init() {
    window = create_window();
    if (!window) {
      fail_at(__LINE__, "CreateWindowA failed");
      return false;
    }
    d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d9) {
      fail_at(__LINE__, "Direct3DCreate9 failed");
      return false;
    }
    HRESULT create_hr = E_FAIL;
    device = create_device(d3d9, window, &create_hr);
    if (!device) {
      std::printf("FAIL:%d: CreateDevice(HAL,HARDWARE_VERTEXPROCESSING) returned 0x%08lx\n",
          __LINE__, static_cast<unsigned long>(create_hr));
      ++failures;
      return false;
    }
    CHECK_HR(device->CreateRenderTarget(kWidth, kHeight, D3DFMT_A8R8G8B8,
        D3DMULTISAMPLE_NONE, 0, FALSE, &render_target, nullptr), D3D_OK);
    CHECK_HR(device->CreateOffscreenPlainSurface(kWidth, kHeight, D3DFMT_A8R8G8B8,
        D3DPOOL_SYSTEMMEM, &readback, nullptr), D3D_OK);
    CHECK_HR(device->CreateVertexShader(kVertexShader, &vs), D3D_OK);
    CHECK_HR(device->CreatePixelShader(kPixelShader, &ps), D3D_OK);
    if (!render_target || !readback || !vs || !ps) return false;

    const D3DVERTEXELEMENT9 elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
         D3DDECLUSAGE_POSITION, 0},
        D3DDECL_END(),
    };
    CHECK_HR(device->CreateVertexDeclaration(elements, &declaration), D3D_OK);
    if (!declaration) return false;

    constexpr unsigned kVertexCount = kRows * 4u * 3u;
    CHECK_HR(device->CreateVertexBuffer(kVertexCount * sizeof(Vertex), 0,
        0, D3DPOOL_MANAGED, &vertices, nullptr), D3D_OK);
    CHECK_HR(device->CreateIndexBuffer(kVertexCount * sizeof(std::uint16_t), 0,
        D3DFMT_INDEX16, D3DPOOL_MANAGED, &indices, nullptr), D3D_OK);
    if (!vertices || !indices) return false;

    Vertex *vertex_data = nullptr;
    CHECK_HR(vertices->Lock(0, 0, reinterpret_cast<void **>(&vertex_data), 0), D3D_OK);
    if (!vertex_data) return false;
    for (unsigned row = 0; row < kRows; ++row) {
      for (unsigned slot = 0; slot < 4; ++slot) {
        const unsigned index = (row * 4u + slot) * 3u;
        vertex_data[index + 0] = make_vertex(row, slot, 0);
        vertex_data[index + 1] = make_vertex(row, slot, 1);
        vertex_data[index + 2] = make_vertex(row, slot, 2);
      }
    }
    CHECK_HR(vertices->Unlock(), D3D_OK);

    std::uint16_t *index_data = nullptr;
    CHECK_HR(indices->Lock(0, 0, reinterpret_cast<void **>(&index_data), 0), D3D_OK);
    if (!index_data) return false;
    for (unsigned i = 0; i < kVertexCount; ++i) index_data[i] = static_cast<std::uint16_t>(i);
    CHECK_HR(indices->Unlock(), D3D_OK);
    return failures == 0;
  }

  ~Fixture() {
    if (device) {
      // Detach the explicitly created resources before dropping their public
      // references. The bound PE wrappers can otherwise keep the device alive
      // past the fixture and lose its final diagnostic/cleanup receipt.
      CHECK_HR(device->SetVertexShader(nullptr), D3D_OK);
      CHECK_HR(device->SetPixelShader(nullptr), D3D_OK);
      CHECK_HR(device->SetVertexDeclaration(nullptr), D3D_OK);
      CHECK_HR(device->SetStreamSource(0, nullptr, 0, 0), D3D_OK);
      CHECK_HR(device->SetIndices(nullptr), D3D_OK);
    }
    release_resource(ps, "ps");
    release_resource(vs, "vs");
    release_resource(declaration, "declaration");
    release_resource(indices, "indices");
    release_resource(vertices, "vertices");
    release_resource(readback, "readback");
    release_resource(render_target, "render_target");
    if (device) {
      // Drop caller-owned default-pool references before Reset detaches the
      // bound target. Do not acquire an implicit backbuffer merely for cleanup:
      // that would introduce a separate swapchain/surface lifetime into this
      // constant-state oracle. This reset follows the single pixel readback.
      D3DPRESENT_PARAMETERS pp = {};
      pp.BackBufferWidth = 64;
      pp.BackBufferHeight = 64;
      pp.BackBufferFormat = D3DFMT_A8R8G8B8;
      pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
      pp.hDeviceWindow = window;
      pp.Windowed = TRUE;
      pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
      CHECK_HR(device->Reset(&pp), D3D_OK);
      const ULONG remaining = device->Release();
      std::printf("REACHED:inline_constants:device_release=%lu\n",
          static_cast<unsigned long>(remaining));
      CHECK(remaining == 0);
    }
    if (d3d9) d3d9->Release();
    if (window) DestroyWindow(window);
  }
};

void set_baseline_constants(IDirect3DDevice9 *device) {
  const float vs_float[4] = {0.08f, 0.12f, 0.16f, 1.0f};
  const float vs_int_scale[4] = {0.15f, 0.0f, 0.0f, 0.0f};
  const float vs_bool_scale[4] = {0.0f, 0.10f, 0.0f, 0.0f};
  const int vs_int[4] = {0, 0, 0, 0};
  const BOOL vs_bool[1] = {FALSE};
  const float ps_float[4] = {0.08f, 0.12f, 0.16f, 1.0f};
  const float ps_int_scale[4] = {0.0f, 0.0f, 0.11f, 0.0f};
  const float ps_bool_scale[4] = {0.08f, 0.0f, 0.0f, 0.0f};
  const int ps_int[4] = {0, 0, 0, 0};
  const BOOL ps_bool[1] = {FALSE};

  CHECK_HR(device->SetVertexShaderConstantF(0, vs_float, 1), D3D_OK);
  CHECK_HR(device->SetVertexShaderConstantF(4, vs_int_scale, 1), D3D_OK);
  CHECK_HR(device->SetVertexShaderConstantF(5, vs_bool_scale, 1), D3D_OK);
  CHECK_HR(device->SetVertexShaderConstantI(0, vs_int, 1), D3D_OK);
  CHECK_HR(device->SetVertexShaderConstantB(0, vs_bool, 1), D3D_OK);
  CHECK_HR(device->SetPixelShaderConstantF(0, ps_float, 1), D3D_OK);
  CHECK_HR(device->SetPixelShaderConstantF(4, ps_int_scale, 1), D3D_OK);
  CHECK_HR(device->SetPixelShaderConstantF(5, ps_bool_scale, 1), D3D_OK);
  CHECK_HR(device->SetPixelShaderConstantI(0, ps_int, 1), D3D_OK);
  CHECK_HR(device->SetPixelShaderConstantB(0, ps_bool, 1), D3D_OK);
}

void set_family_value(IDirect3DDevice9 *device, Family family, bool changed) {
  switch (family) {
    case Family::VsFloat: {
      const float changed_value[4] = {0.20f, 0.10f, 0.15f, 1.0f};
      const float base_value[4] = {0.08f, 0.12f, 0.16f, 1.0f};
      const float *value = changed ? changed_value : base_value;
      CHECK_HR(device->SetVertexShaderConstantF(0, value, 1), D3D_OK);
      break;
    }
    case Family::VsInt: {
      const int value[4] = {changed ? 1 : 0, 0, 0, 0};
      CHECK_HR(device->SetVertexShaderConstantI(0, value, 1), D3D_OK);
      break;
    }
    case Family::VsBool: {
      const BOOL value[1] = {changed ? TRUE : FALSE};
      CHECK_HR(device->SetVertexShaderConstantB(0, value, 1), D3D_OK);
      break;
    }
    case Family::PsFloat: {
      const float changed_value[4] = {0.10f, 0.20f, 0.12f, 1.0f};
      const float base_value[4] = {0.08f, 0.12f, 0.16f, 1.0f};
      const float *value = changed ? changed_value : base_value;
      CHECK_HR(device->SetPixelShaderConstantF(0, value, 1), D3D_OK);
      break;
    }
    case Family::PsInt: {
      const int value[4] = {changed ? 1 : 0, 0, 0, 0};
      CHECK_HR(device->SetPixelShaderConstantI(0, value, 1), D3D_OK);
      break;
    }
    case Family::PsBool: {
      const BOOL value[1] = {changed ? TRUE : FALSE};
      CHECK_HR(device->SetPixelShaderConstantB(0, value, 1), D3D_OK);
      break;
    }
  }
}

bool draw_slot(Fixture &fixture, unsigned row, unsigned slot, unsigned path) {
  const unsigned vertex = (row * 4u + slot) * 3u;
  if (path == 0) {
    return SUCCEEDED(fixture.device->DrawPrimitive(D3DPT_TRIANGLELIST, vertex, 1));
  }
  if (path == 1) {
    return SUCCEEDED(fixture.device->DrawIndexedPrimitive(
        D3DPT_TRIANGLELIST, 0, 0, kRows * 4u * 3u, vertex, 1));
  }
  Vertex up[3] = {make_vertex(row, slot, 0), make_vertex(row, slot, 1),
                  make_vertex(row, slot, 2)};
  return SUCCEEDED(fixture.device->DrawPrimitiveUP(
      D3DPT_TRIANGLELIST, 1, up, sizeof(Vertex)));
}

void check_draw_hr(bool success, unsigned line) {
  if (!success) fail_at(static_cast<int>(line), "draw call failed");
}

bool color_near(D3DCOLOR actual, D3DCOLOR expected, unsigned tolerance) {
  for (unsigned shift : {0u, 8u, 16u, 24u}) {
    const int a = static_cast<int>((actual >> shift) & 0xffu);
    const int e = static_cast<int>((expected >> shift) & 0xffu);
    if (std::abs(a - e) > static_cast<int>(tolerance)) return false;
  }
  return true;
}

void print_and_check_row(const char *path, Family family, unsigned row,
                         const D3DLOCKED_RECT &locked) {
  const auto *base = static_cast<const std::uint8_t *>(locked.pBits) +
      static_cast<std::size_t>(row * 16u + 4u) * locked.Pitch;
  const auto *pixels = reinterpret_cast<const D3DCOLOR *>(base);
  std::printf("PIXEL_SIGNATURE path=%s family=%s row=%u values=",
      path, family_name(family), row);
  for (unsigned slot = 0; slot < 4; ++slot) {
    const D3DCOLOR actual = pixels[slot * 16u + 4u];
    std::printf("0x%08lx%s", static_cast<unsigned long>(actual), slot == 3 ? "" : ",");
    const D3DCOLOR expected = slot == 2 ? kChanged[static_cast<unsigned>(family)] : kBaseline;
    CHECK(color_near(actual, expected, 3u));
  }
  std::printf(" expected=0x%08lx,0x%08lx,0x%08lx,0x%08lx\n",
      static_cast<unsigned long>(kBaseline),
      static_cast<unsigned long>(kBaseline),
      static_cast<unsigned long>(kChanged[static_cast<unsigned>(family)]),
      static_cast<unsigned long>(kBaseline));
}

void run_pixel_oracle() {
  const char *selected = std::getenv("DXMT9_PE_INLINE_CONST_DELTA");
  std::printf("selected-env: DXMT9_PE_INLINE_CONST_DELTA=%s (reported only)\n",
      selected ? selected : "<unset>");
  std::printf("scope: real PE COM shader execution; paths=DrawPrimitive,DrawIndexedPrimitive,DrawPrimitiveUP\n");

  Fixture fixture;
  if (!fixture.init()) return;

  CHECK_HR(fixture.device->SetRenderTarget(0, fixture.render_target), D3D_OK);
  CHECK_HR(fixture.device->SetDepthStencilSurface(nullptr), D3D_OK);
  const D3DVIEWPORT9 viewport = {0, 0, kWidth, kHeight, 0.0f, 1.0f};
  CHECK_HR(fixture.device->SetViewport(&viewport), D3D_OK);
  CHECK_HR(fixture.device->SetVertexDeclaration(fixture.declaration), D3D_OK);
  CHECK_HR(fixture.device->SetVertexShader(fixture.vs), D3D_OK);
  CHECK_HR(fixture.device->SetPixelShader(fixture.ps), D3D_OK);
  CHECK_HR(fixture.device->SetRenderState(D3DRS_ZENABLE, FALSE), D3D_OK);
  CHECK_HR(fixture.device->SetRenderState(D3DRS_LIGHTING, FALSE), D3D_OK);
  CHECK_HR(fixture.device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE), D3D_OK);
  CHECK_HR(fixture.device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE), D3D_OK);
  CHECK_HR(fixture.device->Clear(0, nullptr, D3DCLEAR_TARGET,
      D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0), D3D_OK);

  CHECK_HR(fixture.device->SetStreamSource(0, fixture.vertices, 0, sizeof(Vertex)), D3D_OK);
  CHECK_HR(fixture.device->SetIndices(fixture.indices), D3D_OK);
  set_baseline_constants(fixture.device);
  CHECK_HR(fixture.device->BeginScene(), D3D_OK);

  for (unsigned path = 0; path < kPathCount; ++path) {
    if (path == 2) {
      CHECK_HR(fixture.device->SetStreamSource(0, nullptr, 0, 0), D3D_OK);
      CHECK_HR(fixture.device->SetIndices(nullptr), D3D_OK);
    } else {
      CHECK_HR(fixture.device->SetStreamSource(0, fixture.vertices, 0, sizeof(Vertex)), D3D_OK);
      CHECK_HR(fixture.device->SetIndices(fixture.indices), D3D_OK);
    }
    for (unsigned family_index = 0; family_index < 6; ++family_index) {
      const Family family = static_cast<Family>(family_index);
      const unsigned row = path * kRowsPerPath + family_index;
      check_draw_hr(draw_slot(fixture, row, 0, path), __LINE__);
      check_draw_hr(draw_slot(fixture, row, 1, path), __LINE__);
      set_family_value(fixture.device, family, true);
      check_draw_hr(draw_slot(fixture, row, 2, path), __LINE__);
      set_family_value(fixture.device, family, false);
      check_draw_hr(draw_slot(fixture, row, 3, path), __LINE__);
      // Every case ends at A, so the next family starts from the known baseline.
    }
  }

  CHECK_HR(fixture.device->EndScene(), D3D_OK);
  CHECK_HR(fixture.device->GetRenderTargetData(fixture.render_target, fixture.readback), D3D_OK);
  D3DLOCKED_RECT locked = {};
  CHECK_HR(fixture.readback->LockRect(&locked, nullptr, D3DLOCK_READONLY), D3D_OK);
  CHECK(locked.pBits != nullptr);
  if (locked.pBits) {
    for (unsigned path = 0; path < kPathCount; ++path) {
      const char *path_name = path == 0 ? "vb" : (path == 1 ? "indexed" : "up");
      for (unsigned family_index = 0; family_index < 6; ++family_index) {
        print_and_check_row(path_name, static_cast<Family>(family_index),
            path * kRowsPerPath + family_index, locked);
      }
    }
    CHECK_HR(fixture.readback->UnlockRect(), D3D_OK);
  }
}

}  // namespace

int main() {
  run_pixel_oracle();
  if (failures) {
    std::printf("inline_constants: %d failure(s)\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("PASS:inline_constants\n");
  std::printf("inline_constants: passed (real PE pixel oracle)\n");
  return EXIT_SUCCESS;
}
