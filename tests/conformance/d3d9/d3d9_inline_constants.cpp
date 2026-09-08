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
#include <cstring>

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

// Edge-layout VS: the changed float/int/bool families live at the register
// caps while the c4/c5 scale constants remain the same executable sources.
static const DWORD kVertexShaderEdge[] = {
    D3DVS_VERSION(3, 0),
    instruction(D3DSIO_DCL, 2), dcl_token(D3DDECLUSAGE_POSITION, 0),
        dst_token(D3DSPR_INPUT, 0),
    instruction(D3DSIO_DCL, 2), dcl_token(D3DDECLUSAGE_POSITION, 0),
        dst_token(D3DSPR_OUTPUT, 0),
    instruction(D3DSIO_DCL, 2), dcl_token(D3DDECLUSAGE_COLOR, 0),
        dst_token(D3DSPR_OUTPUT, 1),
    instruction(D3DSIO_MOV, 2), dst_token(D3DSPR_OUTPUT, 0), src_token(D3DSPR_INPUT, 0),
    instruction(D3DSIO_MOV, 2), dst_token(D3DSPR_TEMP, 0), src_token(D3DSPR_CONST, 255),
    instruction(D3DSIO_REP, 1), src_token(D3DSPR_CONSTINT, 15),
    instruction(D3DSIO_ADD, 3), dst_token(D3DSPR_TEMP, 0),
        src_token(D3DSPR_TEMP, 0), src_token(D3DSPR_CONST, 4),
    instruction(D3DSIO_ENDREP, 0),
    instruction(D3DSIO_IF, 1), src_token(D3DSPR_CONSTBOOL, 15),
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

// Edge-layout PS: same executable c4/c5 scales, with the three changed
// families sourced from the pixel-stage register caps.
static const DWORD kPixelShaderEdge[] = {
    D3DPS_VERSION(3, 0),
    instruction(D3DSIO_DCL, 2), dcl_token(D3DDECLUSAGE_COLOR, 0),
        dst_token(D3DSPR_INPUT, 0),
    instruction(D3DSIO_MOV, 2), dst_token(D3DSPR_TEMP, 0), src_token(D3DSPR_INPUT, 0),
    instruction(D3DSIO_ADD, 3), dst_token(D3DSPR_TEMP, 0),
        src_token(D3DSPR_TEMP, 0), src_token(D3DSPR_CONST, 223),
    instruction(D3DSIO_REP, 1), src_token(D3DSPR_CONSTINT, 15),
    instruction(D3DSIO_ADD, 3), dst_token(D3DSPR_TEMP, 0),
        src_token(D3DSPR_TEMP, 0), src_token(D3DSPR_CONST, 4),
    instruction(D3DSIO_ENDREP, 0),
    instruction(D3DSIO_IF, 1), src_token(D3DSPR_CONSTBOOL, 15),
    instruction(D3DSIO_ADD, 3), dst_token(D3DSPR_TEMP, 0),
        src_token(D3DSPR_TEMP, 0), src_token(D3DSPR_CONST, 5),
    instruction(D3DSIO_ENDIF, 0),
    instruction(D3DSIO_MOV, 2), dst_token(D3DSPR_COLOROUT, 0), src_token(D3DSPR_TEMP, 0),
    0x0000ffffu,
};

constexpr unsigned kWidth = 64;
constexpr unsigned kRowsPerPath = 6;
constexpr unsigned kPlainPathCount = 3;
constexpr unsigned kPlainRows = kRowsPerPath * kPlainPathCount;
constexpr unsigned kPlainHeight = kPlainRows * 16;
constexpr unsigned kEdgePathCount = 4;
constexpr unsigned kEdgeRows = kRowsPerPath * kEdgePathCount;
constexpr unsigned kEdgeHeight = kEdgeRows * 16;

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

Vertex make_vertex(unsigned row, unsigned slot, unsigned vertex,
                   unsigned height = kPlainHeight) {
  const float left = -1.0f + 2.0f * static_cast<float>(slot * 16u) /
      static_cast<float>(kWidth);
  const float right = -1.0f + 2.0f * static_cast<float>((slot + 1u) * 16u) /
      static_cast<float>(kWidth);
  const float top = 1.0f - 2.0f * static_cast<float>(row * 16u) /
      static_cast<float>(height);
  const float bottom = 1.0f - 2.0f * static_cast<float>((row + 1u) * 16u) /
      static_cast<float>(height);
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

IDirect3DDevice9 *create_device(IDirect3D9 *d3d9, HWND window, HRESULT *last_hr,
                               unsigned back_buffer_height = 64) {
  D3DPRESENT_PARAMETERS pp = {};
  pp.BackBufferWidth = 64;
  pp.BackBufferHeight = back_buffer_height;
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
  bool edge_layout = false;
  bool capture_mode = false;
  unsigned rows = kPlainRows;
  unsigned height = kPlainHeight;
  unsigned path_count = kPlainPathCount;
  unsigned device_height = 64;
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

  bool init(bool use_edge_layout = false, bool use_capture_mode = false) {
    edge_layout = use_edge_layout;
    capture_mode = use_capture_mode;
    rows = capture_mode ? kRowsPerPath : (edge_layout ? kEdgeRows : kPlainRows);
    height = capture_mode ? kRowsPerPath * 16u
                          : (edge_layout ? kEdgeHeight : kPlainHeight);
    path_count = capture_mode ? 1u
                              : (edge_layout ? kEdgePathCount : kPlainPathCount);
    device_height = capture_mode ? height : 64u;
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
    device = create_device(d3d9, window, &create_hr, device_height);
    if (!device) {
      std::printf("FAIL:%d: CreateDevice(HAL,HARDWARE_VERTEXPROCESSING) returned 0x%08lx\n",
          __LINE__, static_cast<unsigned long>(create_hr));
      ++failures;
      return false;
    }
    if (!capture_mode) {
      CHECK_HR(device->CreateRenderTarget(kWidth, height, D3DFMT_A8R8G8B8,
          D3DMULTISAMPLE_NONE, 0, FALSE, &render_target, nullptr), D3D_OK);
      CHECK_HR(device->CreateOffscreenPlainSurface(kWidth, height, D3DFMT_A8R8G8B8,
          D3DPOOL_SYSTEMMEM, &readback, nullptr), D3D_OK);
    }
    CHECK_HR(device->CreateVertexShader(
        edge_layout ? kVertexShaderEdge : kVertexShader, &vs), D3D_OK);
    CHECK_HR(device->CreatePixelShader(
        edge_layout ? kPixelShaderEdge : kPixelShader, &ps), D3D_OK);
    if (!vs || !ps || (!capture_mode && (!render_target || !readback))) return false;

    const D3DVERTEXELEMENT9 elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
         D3DDECLUSAGE_POSITION, 0},
        D3DDECL_END(),
    };
    CHECK_HR(device->CreateVertexDeclaration(elements, &declaration), D3D_OK);
    if (!declaration) return false;

    const unsigned vertex_count = rows * 4u * 3u;
    CHECK_HR(device->CreateVertexBuffer(vertex_count * sizeof(Vertex), 0,
        0, D3DPOOL_MANAGED, &vertices, nullptr), D3D_OK);
    CHECK_HR(device->CreateIndexBuffer(vertex_count * sizeof(std::uint16_t), 0,
        D3DFMT_INDEX16, D3DPOOL_MANAGED, &indices, nullptr), D3D_OK);
    if (!vertices || !indices) return false;

    Vertex *vertex_data = nullptr;
    CHECK_HR(vertices->Lock(0, 0, reinterpret_cast<void **>(&vertex_data), 0), D3D_OK);
    if (!vertex_data) return false;
    for (unsigned row = 0; row < rows; ++row) {
      for (unsigned slot = 0; slot < 4; ++slot) {
        const unsigned index = (row * 4u + slot) * 3u;
        vertex_data[index + 0] = make_vertex(row, slot, 0, height);
        vertex_data[index + 1] = make_vertex(row, slot, 1, height);
        vertex_data[index + 2] = make_vertex(row, slot, 2, height);
      }
    }
    CHECK_HR(vertices->Unlock(), D3D_OK);

    std::uint16_t *index_data = nullptr;
    CHECK_HR(indices->Lock(0, 0, reinterpret_cast<void **>(&index_data), 0), D3D_OK);
    if (!index_data) return false;
    for (unsigned i = 0; i < vertex_count; ++i)
      index_data[i] = static_cast<std::uint16_t>(i);
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
      // constant-state oracle. Capture mode validates pixels from the
      // Present-boundary source artifact instead of acquiring a backbuffer
      // getter or issuing a post-Present readback.
      D3DPRESENT_PARAMETERS pp = {};
      pp.BackBufferWidth = 64;
      pp.BackBufferHeight = device_height;
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

void set_baseline_constants(IDirect3DDevice9 *device, bool edge_layout = false) {
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

  CHECK_HR(device->SetVertexShaderConstantF(edge_layout ? 255u : 0u,
      vs_float, 1), D3D_OK);
  CHECK_HR(device->SetVertexShaderConstantF(4, vs_int_scale, 1), D3D_OK);
  CHECK_HR(device->SetVertexShaderConstantF(5, vs_bool_scale, 1), D3D_OK);
  CHECK_HR(device->SetVertexShaderConstantI(edge_layout ? 15u : 0u,
      vs_int, 1), D3D_OK);
  CHECK_HR(device->SetVertexShaderConstantB(edge_layout ? 15u : 0u,
      vs_bool, 1), D3D_OK);
  CHECK_HR(device->SetPixelShaderConstantF(edge_layout ? 223u : 0u,
      ps_float, 1), D3D_OK);
  CHECK_HR(device->SetPixelShaderConstantF(4, ps_int_scale, 1), D3D_OK);
  CHECK_HR(device->SetPixelShaderConstantF(5, ps_bool_scale, 1), D3D_OK);
  CHECK_HR(device->SetPixelShaderConstantI(edge_layout ? 15u : 0u,
      ps_int, 1), D3D_OK);
  CHECK_HR(device->SetPixelShaderConstantB(edge_layout ? 15u : 0u,
      ps_bool, 1), D3D_OK);
}

void set_family_value(IDirect3DDevice9 *device, Family family, bool changed,
                      bool edge_layout = false) {
  const UINT vs_float_register = edge_layout ? 255u : 0u;
  const UINT ps_float_register = edge_layout ? 223u : 0u;
  const UINT integer_register = edge_layout ? 15u : 0u;
  switch (family) {
    case Family::VsFloat: {
      const float changed_value[4] = {0.20f, 0.10f, 0.15f, 1.0f};
      const float base_value[4] = {0.08f, 0.12f, 0.16f, 1.0f};
      const float *value = changed ? changed_value : base_value;
      CHECK_HR(device->SetVertexShaderConstantF(vs_float_register, value, 1), D3D_OK);
      break;
    }
    case Family::VsInt: {
      const int value[4] = {changed ? 1 : 0, 0, 0, 0};
      CHECK_HR(device->SetVertexShaderConstantI(integer_register, value, 1), D3D_OK);
      break;
    }
    case Family::VsBool: {
      const BOOL value[1] = {changed ? TRUE : FALSE};
      CHECK_HR(device->SetVertexShaderConstantB(integer_register, value, 1), D3D_OK);
      break;
    }
    case Family::PsFloat: {
      const float changed_value[4] = {0.10f, 0.20f, 0.12f, 1.0f};
      const float base_value[4] = {0.08f, 0.12f, 0.16f, 1.0f};
      const float *value = changed ? changed_value : base_value;
      CHECK_HR(device->SetPixelShaderConstantF(ps_float_register, value, 1), D3D_OK);
      break;
    }
    case Family::PsInt: {
      const int value[4] = {changed ? 1 : 0, 0, 0, 0};
      CHECK_HR(device->SetPixelShaderConstantI(integer_register, value, 1), D3D_OK);
      break;
    }
    case Family::PsBool: {
      const BOOL value[1] = {changed ? TRUE : FALSE};
      CHECK_HR(device->SetPixelShaderConstantB(integer_register, value, 1), D3D_OK);
      break;
    }
  }
}

void check_family_value(IDirect3DDevice9 *device, Family family, bool changed,
                        const char *checkpoint, bool edge_layout = false) {
  const UINT vs_float_register = edge_layout ? 255u : 0u;
  const UINT ps_float_register = edge_layout ? 223u : 0u;
  const UINT integer_register = edge_layout ? 15u : 0u;
  switch (family) {
    case Family::VsFloat: {
      const float expected[4] = {
          changed ? 0.20f : 0.08f, changed ? 0.10f : 0.12f,
          changed ? 0.15f : 0.16f, 1.0f};
      float actual[4] = {};
      CHECK_HR(device->GetVertexShaderConstantF(vs_float_register, actual, 1), D3D_OK);
      check_at(__LINE__, std::memcmp(actual, expected, sizeof(actual)) == 0,
               checkpoint);
      break;
    }
    case Family::VsInt: {
      const int expected[4] = {changed ? 1 : 0, 0, 0, 0};
      int actual[4] = {};
      CHECK_HR(device->GetVertexShaderConstantI(integer_register, actual, 1), D3D_OK);
      check_at(__LINE__, std::memcmp(actual, expected, sizeof(actual)) == 0,
               checkpoint);
      break;
    }
    case Family::VsBool: {
      const BOOL expected[1] = {changed ? TRUE : FALSE};
      BOOL actual[1] = {};
      CHECK_HR(device->GetVertexShaderConstantB(integer_register, actual, 1), D3D_OK);
      check_at(__LINE__, std::memcmp(actual, expected, sizeof(actual)) == 0,
               checkpoint);
      break;
    }
    case Family::PsFloat: {
      const float expected[4] = {
          changed ? 0.10f : 0.08f, changed ? 0.20f : 0.12f,
          changed ? 0.12f : 0.16f, 1.0f};
      float actual[4] = {};
      CHECK_HR(device->GetPixelShaderConstantF(ps_float_register, actual, 1), D3D_OK);
      check_at(__LINE__, std::memcmp(actual, expected, sizeof(actual)) == 0,
               checkpoint);
      break;
    }
    case Family::PsInt: {
      const int expected[4] = {changed ? 1 : 0, 0, 0, 0};
      int actual[4] = {};
      CHECK_HR(device->GetPixelShaderConstantI(integer_register, actual, 1), D3D_OK);
      check_at(__LINE__, std::memcmp(actual, expected, sizeof(actual)) == 0,
               checkpoint);
      break;
    }
    case Family::PsBool: {
      const BOOL expected[1] = {changed ? TRUE : FALSE};
      BOOL actual[1] = {};
      CHECK_HR(device->GetPixelShaderConstantB(integer_register, actual, 1), D3D_OK);
      check_at(__LINE__, std::memcmp(actual, expected, sizeof(actual)) == 0,
               checkpoint);
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
        D3DPT_TRIANGLELIST, 0, 0, fixture.rows * 4u * 3u, vertex, 1));
  }
  Vertex up[3] = {make_vertex(row, slot, 0, fixture.height),
                  make_vertex(row, slot, 1, fixture.height),
                  make_vertex(row, slot, 2, fixture.height)};
  if (path == 3 && fixture.edge_layout) {
    const std::uint16_t indices[3] = {0u, 1u, 2u};
    return SUCCEEDED(fixture.device->DrawIndexedPrimitiveUP(
        D3DPT_TRIANGLELIST, 0, 3, 1, indices, D3DFMT_INDEX16, up,
        sizeof(Vertex)));
  }
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
                         const D3DLOCKED_RECT &locked, bool stateblock) {
  const auto *base = static_cast<const std::uint8_t *>(locked.pBits) +
      static_cast<std::size_t>(row * 16u + 4u) * locked.Pitch;
  const auto *pixels = reinterpret_cast<const D3DCOLOR *>(base);
  std::printf("PIXEL_SIGNATURE path=%s family=%s row=%u values=",
      path, family_name(family), row);
  for (unsigned slot = 0; slot < 4; ++slot) {
    const D3DCOLOR actual = pixels[slot * 16u + 4u];
    std::printf("0x%08lx%s", static_cast<unsigned long>(actual), slot == 3 ? "" : ",");
    const bool changed = stateblock ? slot == 1 : slot == 2;
    const D3DCOLOR expected = changed ? kChanged[static_cast<unsigned>(family)]
                                      : kBaseline;
    CHECK(color_near(actual, expected, 3u));
  }
  if (stateblock) {
    std::printf(" expected=0x%08lx,0x%08lx,0x%08lx,0x%08lx\n",
        static_cast<unsigned long>(kBaseline),
        static_cast<unsigned long>(kChanged[static_cast<unsigned>(family)]),
        static_cast<unsigned long>(kBaseline),
        static_cast<unsigned long>(kBaseline));
  } else {
    std::printf(" expected=0x%08lx,0x%08lx,0x%08lx,0x%08lx\n",
        static_cast<unsigned long>(kBaseline),
        static_cast<unsigned long>(kBaseline),
        static_cast<unsigned long>(kChanged[static_cast<unsigned>(family)]),
        static_cast<unsigned long>(kBaseline));
  }
}

void print_capture_expected_row(Family family, unsigned row) {
  const D3DCOLOR expected_b = kChanged[static_cast<unsigned>(family)];
  std::printf("CAPTURE_EXPECTED_SIGNATURE family=%s row=%u values=",
      family_name(family), row);
  for (unsigned slot = 0; slot < 3; ++slot) {
    const D3DCOLOR expected = slot == 1 ? expected_b : kBaseline;
    std::printf("0x%08lx%s", static_cast<unsigned long>(expected), slot == 2 ? "" : ",");
  }
  std::printf(" expected=0x%08lx,0x%08lx,0x%08lx\n",
      static_cast<unsigned long>(kBaseline),
      static_cast<unsigned long>(expected_b),
      static_cast<unsigned long>(kBaseline));
}

bool capture_environment_ready() {
  const char *capture = std::getenv("DXMT9_RENDER_TAPE_CAPTURE");
  const char *root = std::getenv("DXMT9_RENDER_TAPE_OUTPUT_ROOT");
  if (capture && *capture && std::strcmp(capture, "0") != 0 && root && *root) return true;
  std::printf("FAIL:capture selector requires DXMT9_RENDER_TAPE_CAPTURE and "
      "DXMT9_RENDER_TAPE_OUTPUT_ROOT\n");
  ++failures;
  return false;
}

void print_edge_layout_receipt(const Fixture &fixture) {
  std::printf("selected-layout: edge\n");
  std::printf("layout: edge registers=VS c255/i15/b15 PS c223/i15/b15 scales=c4/c5\n");
  std::printf("paths: vb,indexed,up,indexed_up index_format=INDEX16 draws=%u rows=%u\n",
      fixture.path_count * kRowsPerPath * 4u,
      fixture.path_count * kRowsPerPath);
}

void run_pixel_oracle(bool edge_layout = false) {
  const char *selected = std::getenv("DXMT9_PE_INLINE_CONST_DELTA");
  std::printf("selected-env: DXMT9_PE_INLINE_CONST_DELTA=%s (reported only)\n",
      selected ? selected : "<unset>");
  if (edge_layout) {
    std::printf("scope: real PE COM shader execution; paths=DrawPrimitive,DrawIndexedPrimitive,DrawPrimitiveUP,DrawIndexedPrimitiveUP\n");
  } else {
    std::printf("scope: real PE COM shader execution; paths=DrawPrimitive,DrawIndexedPrimitive,DrawPrimitiveUP\n");
  }

  Fixture fixture;
  if (!fixture.init(edge_layout)) return;
  if (edge_layout) print_edge_layout_receipt(fixture);

  CHECK_HR(fixture.device->SetRenderTarget(0, fixture.render_target), D3D_OK);
  CHECK_HR(fixture.device->SetDepthStencilSurface(nullptr), D3D_OK);
  const D3DVIEWPORT9 viewport = {0, 0, kWidth, fixture.height, 0.0f, 1.0f};
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
  set_baseline_constants(fixture.device, edge_layout);
  CHECK_HR(fixture.device->BeginScene(), D3D_OK);

  for (unsigned path = 0; path < fixture.path_count; ++path) {
    if (path >= 2) {
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
      set_family_value(fixture.device, family, true, edge_layout);
      check_draw_hr(draw_slot(fixture, row, 2, path), __LINE__);
      set_family_value(fixture.device, family, false, edge_layout);
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
    for (unsigned path = 0; path < fixture.path_count; ++path) {
      const char *path_name = path == 0 ? "vb" :
          (path == 1 ? "indexed" : (path == 2 ? "up" : "indexed_up"));
      for (unsigned family_index = 0; family_index < 6; ++family_index) {
        print_and_check_row(path_name, static_cast<Family>(family_index),
            path * kRowsPerPath + family_index, locked, false);
      }
    }
    CHECK_HR(fixture.readback->UnlockRect(), D3D_OK);
  }
}

void run_capture_oracle() {
  std::printf("capture-selector: capture\n");
  const char *selected = std::getenv("DXMT9_PE_INLINE_CONST_DELTA");
  std::printf("selected-env: DXMT9_PE_INLINE_CONST_DELTA=%s (reported only)\n",
      selected ? selected : "<unset>");
  std::printf("scope: Render Tape capture interval; 17 ordinary bound draws plus one indexed control; six A-B-A rows\n");
  std::printf("capture-oracle: expected-only; actual pixels validated by external captured-source oracle\n");
  if (!capture_environment_ready()) return;

  Fixture fixture;
  if (!fixture.init(false, true)) return;

  CHECK_HR(fixture.device->SetDepthStencilSurface(nullptr), D3D_OK);
  const D3DVIEWPORT9 viewport = {0, 0, kWidth, fixture.height, 0.0f, 1.0f};
  CHECK_HR(fixture.device->SetViewport(&viewport), D3D_OK);
  CHECK_HR(fixture.device->SetVertexDeclaration(fixture.declaration), D3D_OK);
  CHECK_HR(fixture.device->SetVertexShader(fixture.vs), D3D_OK);
  CHECK_HR(fixture.device->SetPixelShader(fixture.ps), D3D_OK);
  CHECK_HR(fixture.device->SetRenderState(D3DRS_ZENABLE, FALSE), D3D_OK);
  CHECK_HR(fixture.device->SetRenderState(D3DRS_LIGHTING, FALSE), D3D_OK);
  CHECK_HR(fixture.device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE), D3D_OK);
  CHECK_HR(fixture.device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE), D3D_OK);
  CHECK_HR(fixture.device->SetStreamSource(0, fixture.vertices, 0, sizeof(Vertex)), D3D_OK);
  CHECK_HR(fixture.device->SetIndices(fixture.indices), D3D_OK);
  set_baseline_constants(fixture.device, false);
  for (unsigned family_index = 0; family_index < 6; ++family_index) {
    set_family_value(fixture.device, static_cast<Family>(family_index), true, false);
  }

  // All resource creation and bootstrap state publication precede arming.
  CHECK_HR(fixture.device->Clear(0, nullptr, D3DCLEAR_TARGET,
      D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0), D3D_OK);
  CHECK_HR(fixture.device->Present(nullptr, nullptr, nullptr, nullptr), D3D_OK);

  // DISCARD does not preserve the prior Present's clear. Seed this captured
  // interval explicitly so fresh replay also defines the uncovered pixels.
  CHECK_HR(fixture.device->Clear(0, nullptr, D3DCLEAR_TARGET,
      D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0), D3D_OK);
  CHECK_HR(fixture.device->BeginScene(), D3D_OK);
  set_baseline_constants(fixture.device, false);
  for (unsigned family_index = 0; family_index < 6; ++family_index) {
    const Family family = static_cast<Family>(family_index);
    check_draw_hr(draw_slot(fixture, family_index, 0, 0), __LINE__);
    set_family_value(fixture.device, family, true, false);
    check_draw_hr(draw_slot(fixture, family_index, 1, 0), __LINE__);
    set_family_value(fixture.device, family, false, false);
    // The general production-provider grammar currently requires an indexed
    // draw. Keep one explicit control, without changing the expected A pixel.
    check_draw_hr(draw_slot(fixture, family_index, 2,
        family_index == 5u ? 1u : 0u), __LINE__);
  }
  CHECK_HR(fixture.device->EndScene(), D3D_OK);
  // Present #2 captures source/output bytes and closes the interval. The
  // external driver checks those artifacts; the fixture adds no CPU readback.
  CHECK_HR(fixture.device->Present(nullptr, nullptr, nullptr, nullptr), D3D_OK);

  for (unsigned family_index = 0; family_index < 6; ++family_index) {
    print_capture_expected_row(static_cast<Family>(family_index), family_index);
  }
}

void run_stateblock_oracle(bool edge_layout = false) {
  std::printf("selected-case: stateblock\n");
  const char *selected = std::getenv("DXMT9_PE_INLINE_CONST_DELTA");
  std::printf("selected-env: DXMT9_PE_INLINE_CONST_DELTA=%s (reported only)\n",
      selected ? selected : "<unset>");
  if (edge_layout) {
    std::printf("stateblock mode: expected slots=A,B,A,A; paths=DrawPrimitive,DrawIndexedPrimitive,DrawPrimitiveUP,DrawIndexedPrimitiveUP\n");
  } else {
    std::printf("stateblock mode: expected slots=A,B,A,A; paths=DrawPrimitive,DrawIndexedPrimitive,DrawPrimitiveUP\n");
  }

  Fixture fixture;
  if (!fixture.init(edge_layout)) return;
  if (edge_layout) print_edge_layout_receipt(fixture);

  CHECK_HR(fixture.device->SetRenderTarget(0, fixture.render_target), D3D_OK);
  CHECK_HR(fixture.device->SetDepthStencilSurface(nullptr), D3D_OK);
  const D3DVIEWPORT9 viewport = {0, 0, kWidth, fixture.height, 0.0f, 1.0f};
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

  set_baseline_constants(fixture.device, edge_layout);
  CHECK_HR(fixture.device->BeginScene(), D3D_OK);
  for (unsigned path = 0; path < fixture.path_count; ++path) {
    if (path >= 2) {
      // UP is deliberately standalone: it has no stream or index binding.
      CHECK_HR(fixture.device->SetStreamSource(0, nullptr, 0, 0), D3D_OK);
      CHECK_HR(fixture.device->SetIndices(nullptr), D3D_OK);
    } else {
      CHECK_HR(fixture.device->SetStreamSource(0, fixture.vertices, 0, sizeof(Vertex)), D3D_OK);
      CHECK_HR(fixture.device->SetIndices(fixture.indices), D3D_OK);
    }
    for (unsigned family_index = 0; family_index < 6; ++family_index) {
      const Family family = static_cast<Family>(family_index);
      const unsigned row = path * kRowsPerPath + family_index;
      set_baseline_constants(fixture.device, edge_layout);
      check_family_value(fixture.device, family, false,
          "stateblock baseline family bytes", edge_layout);

      HRESULT begin_hr = fixture.device->BeginStateBlock();
      CHECK_HR(begin_hr, D3D_OK);
      if (FAILED(begin_hr)) continue;
      set_family_value(fixture.device, family, true, edge_layout);
      check_family_value(fixture.device, family, false,
          "stateblock recording leaves live family at A", edge_layout);

      IDirect3DStateBlock9 *state_block = nullptr;
      const HRESULT end_hr = fixture.device->EndStateBlock(&state_block);
      CHECK_HR(end_hr, D3D_OK);
      if (FAILED(end_hr) || !state_block) {
        CHECK(state_block != nullptr);
        if (state_block) release_resource(state_block, "stateblock");
        continue;
      }
      check_family_value(fixture.device, family, false,
          "stateblock EndStateBlock leaves live family at A", edge_layout);

      check_draw_hr(draw_slot(fixture, row, 0, path), __LINE__);
      CHECK_HR(state_block->Apply(), D3D_OK);
      check_family_value(fixture.device, family, true,
          "stateblock Apply restores captured B", edge_layout);
      check_draw_hr(draw_slot(fixture, row, 1, path), __LINE__);

      set_family_value(fixture.device, family, false, edge_layout);
      check_family_value(fixture.device, family, false,
          "stateblock explicit A is visible before Capture", edge_layout);
      check_draw_hr(draw_slot(fixture, row, 2, path), __LINE__);

      CHECK_HR(state_block->Capture(), D3D_OK);
      check_family_value(fixture.device, family, false,
          "stateblock Capture records live A", edge_layout);
      set_family_value(fixture.device, family, true, edge_layout);
      check_family_value(fixture.device, family, true,
          "stateblock pending set exposes B before Apply", edge_layout);
      CHECK_HR(state_block->Apply(), D3D_OK);
      check_family_value(fixture.device, family, false,
          "stateblock Apply restores captured A", edge_layout);
      check_draw_hr(draw_slot(fixture, row, 3, path), __LINE__);

      // The state block must not outlive the fixture's device/resource teardown.
      release_resource(state_block, "stateblock");
    }
  }

  CHECK_HR(fixture.device->EndScene(), D3D_OK);
  CHECK_HR(fixture.device->GetRenderTargetData(fixture.render_target, fixture.readback), D3D_OK);
  D3DLOCKED_RECT locked = {};
  CHECK_HR(fixture.readback->LockRect(&locked, nullptr, D3DLOCK_READONLY), D3D_OK);
  CHECK(locked.pBits != nullptr);
  if (locked.pBits) {
    for (unsigned path = 0; path < fixture.path_count; ++path) {
      const char *path_name = path == 0 ? "vb" :
          (path == 1 ? "indexed" : (path == 2 ? "up" : "indexed_up"));
      for (unsigned family_index = 0; family_index < 6; ++family_index) {
        print_and_check_row(path_name, static_cast<Family>(family_index),
            path * kRowsPerPath + family_index, locked, true);
      }
    }
    CHECK_HR(fixture.readback->UnlockRect(), D3D_OK);
  }
}

}  // namespace

int main() {
  const char *selected_case = std::getenv("DXMT9_TEST_INLINE_CONSTANT_CASE");
  if (selected_case && std::strcmp(selected_case, "stateblock") != 0 &&
      std::strcmp(selected_case, "capture") != 0) {
    std::printf("FAIL: invalid DXMT9_TEST_INLINE_CONSTANT_CASE=%s\n",
        selected_case);
    return EXIT_FAILURE;
  }
  const char *selected_layout =
      std::getenv("DXMT9_TEST_INLINE_CONSTANT_LAYOUT");
  if (selected_layout && std::strcmp(selected_layout, "edge") != 0) {
    std::printf("FAIL: invalid DXMT9_TEST_INLINE_CONSTANT_LAYOUT=%s\n",
        selected_layout);
    return EXIT_FAILURE;
  }
  if (selected_case && std::strcmp(selected_case, "capture") == 0 && selected_layout) {
    std::printf("FAIL: capture selector only supports the base SM3 layout; "
        "DXMT9_TEST_INLINE_CONSTANT_LAYOUT=%s is not valid\n", selected_layout);
    return EXIT_FAILURE;
  }
  const bool edge_layout = selected_layout != nullptr;
  if (selected_case && std::strcmp(selected_case, "capture") == 0) {
    run_capture_oracle();
  } else if (selected_case) {
    run_stateblock_oracle(edge_layout);
  } else {
    run_pixel_oracle(edge_layout);
  }
  if (failures) {
    std::printf("inline_constants: %d failure(s)\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("PASS:inline_constants\n");
  if (selected_case && std::strcmp(selected_case, "capture") == 0) {
    std::printf("inline_constants: capture submitted (external bundle oracle required)\n");
  } else {
    std::printf("inline_constants: passed (real PE pixel oracle)\n");
  }
  return EXIT_SUCCESS;
}
