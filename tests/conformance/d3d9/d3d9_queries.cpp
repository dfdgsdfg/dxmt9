/*
 * Focused D3D9 query conformance scaffold.
 *
 * Wine behavioral oracle: dlls/d3d9/tests/device.c at 6e073d2:
 * - test_query_support()
 * - test_occlusion_query()
 * - test_timestamp_query()
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

int failures = 0;
int skips = 0;

void fail_at(int line, const char *message) {
  std::printf("FAIL:%d: %s\n", line, message);
  ++failures;
}

void check_at(int line, bool condition, const char *message) {
  if (!condition) fail_at(line, message);
}

void check_hr_at(int line, HRESULT actual, HRESULT expected, const char *call) {
  if (actual != expected) {
    std::printf("FAIL:%d: %s returned 0x%08lx, expected 0x%08lx\n",
        line, call, static_cast<unsigned long>(actual),
        static_cast<unsigned long>(expected));
    ++failures;
  }
}

void check_hr_any_at(int line, HRESULT actual, HRESULT expected_a,
    HRESULT expected_b, const char *call) {
  if (actual != expected_a && actual != expected_b) {
    std::printf("FAIL:%d: %s returned 0x%08lx, expected 0x%08lx or 0x%08lx\n",
        line, call, static_cast<unsigned long>(actual),
        static_cast<unsigned long>(expected_a),
        static_cast<unsigned long>(expected_b));
    ++failures;
  }
}

#define CHECK(condition) check_at(__LINE__, !!(condition), #condition)
#define CHECK_HR(actual, expected) check_hr_at(__LINE__, (actual), (expected), #actual)
#define CHECK_HR_ANY(actual, expected_a, expected_b) \
  check_hr_any_at(__LINE__, (actual), (expected_a), (expected_b), #actual)

HWND create_window() {
  RECT rect = {0, 0, 64, 64};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  return CreateWindowA("static", "dxmt9_d3d9_queries", WS_OVERLAPPEDWINDOW,
      0, 0, rect.right - rect.left, rect.bottom - rect.top,
      nullptr, nullptr, nullptr, nullptr);
}

IDirect3DDevice9 *create_device(IDirect3D9 *d3d9, HWND window) {
  D3DPRESENT_PARAMETERS pp = {};
  pp.BackBufferWidth = 64;
  pp.BackBufferHeight = 64;
  pp.BackBufferFormat = D3DFMT_A8R8G8B8;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = window;
  pp.Windowed = TRUE;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

  IDirect3DDevice9 *device = nullptr;
  const DWORD flags[] = {
      D3DCREATE_HARDWARE_VERTEXPROCESSING,
      D3DCREATE_MIXED_VERTEXPROCESSING,
      D3DCREATE_SOFTWARE_VERTEXPROCESSING,
  };

  for (DWORD flags_value : flags) {
    HRESULT hr = d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
        window, flags_value, &pp, &device);
    if (SUCCEEDED(hr)) return device;
  }

  return nullptr;
}

struct Fixture {
  HWND window = nullptr;
  IDirect3D9 *d3d9 = nullptr;
  IDirect3DDevice9 *device = nullptr;

  bool init(const char *test_name) {
    window = create_window();
    if (!window) {
      std::printf("SKIP:%s: failed to create a window\n", test_name);
      ++skips;
      return false;
    }

    d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d9) {
      std::printf("SKIP:%s: Direct3DCreate9 failed\n", test_name);
      ++skips;
      return false;
    }

    device = create_device(d3d9, window);
    if (!device) {
      std::printf("SKIP:%s: failed to create a D3D9 device\n", test_name);
      ++skips;
      return false;
    }

    return true;
  }

  ~Fixture() {
    if (device) device->Release();
    if (d3d9) d3d9->Release();
    if (window) DestroyWindow(window);
  }
};

void wait_query(IDirect3DQuery9 *query) {
  for (unsigned int i = 0; i < 10000; ++i) {
    HRESULT hr = query->GetData(nullptr, 0, D3DGETDATA_FLUSH);
    if (hr == S_OK) return;
    if (hr != S_FALSE) {
      CHECK_HR(hr, S_OK);
      return;
    }
    Sleep(1);
  }

  fail_at(__LINE__, "query did not complete before timeout");
}

void query_support_probe() {
  const D3DQUERYTYPE queries[] = {
      D3DQUERYTYPE_VCACHE,
      D3DQUERYTYPE_RESOURCEMANAGER,
      D3DQUERYTYPE_VERTEXSTATS,
      D3DQUERYTYPE_EVENT,
      D3DQUERYTYPE_OCCLUSION,
      D3DQUERYTYPE_TIMESTAMP,
      D3DQUERYTYPE_TIMESTAMPDISJOINT,
      D3DQUERYTYPE_TIMESTAMPFREQ,
      D3DQUERYTYPE_PIPELINETIMINGS,
      D3DQUERYTYPE_INTERFACETIMINGS,
      D3DQUERYTYPE_VERTEXTIMINGS,
      D3DQUERYTYPE_PIXELTIMINGS,
      D3DQUERYTYPE_BANDWIDTHTIMINGS,
      D3DQUERYTYPE_CACHEUTILIZATION,
  };

  Fixture fixture;
  if (!fixture.init("query_support_probe")) return;

  for (D3DQUERYTYPE type : queries) {
    IDirect3DQuery9 *query = nullptr;
    HRESULT support_hr = fixture.device->CreateQuery(type, nullptr);
    CHECK_HR_ANY(support_hr, D3D_OK, D3DERR_NOTAVAILABLE);

    HRESULT create_hr = fixture.device->CreateQuery(type, &query);
    CHECK_HR_ANY(create_hr, D3D_OK, D3DERR_NOTAVAILABLE);
    CHECK((support_hr == D3D_OK) == (query != nullptr));
    if (query) query->Release();
  }

  for (unsigned int i = 0; i < 40; ++i) {
    if ((D3DQUERYTYPE_VCACHE <= i && i <= D3DQUERYTYPE_MEMORYPRESSURE)
        || i == 0x16) {
      continue;
    }

    IDirect3DQuery9 *query =
        reinterpret_cast<IDirect3DQuery9 *>(static_cast<UINT_PTR>(0xdeadbeef));
    CHECK_HR(fixture.device->CreateQuery(static_cast<D3DQUERYTYPE>(i), nullptr),
        D3DERR_NOTAVAILABLE);
    CHECK_HR(fixture.device->CreateQuery(static_cast<D3DQUERYTYPE>(i), &query),
        D3DERR_NOTAVAILABLE);
    CHECK(query == reinterpret_cast<IDirect3DQuery9 *>(
        static_cast<UINT_PTR>(0xdeadbeef)));
  }
}

void occlusion_query_public_sizes() {
  Fixture fixture;
  if (!fixture.init("occlusion_query_public_sizes")) return;

  IDirect3DQuery9 *query = nullptr;
  HRESULT hr = fixture.device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &query);
  CHECK_HR_ANY(hr, D3D_OK, D3DERR_NOTAVAILABLE);
  if (!query) {
    std::printf("SKIP:occlusion_query_public_sizes: occlusion queries are not supported\n");
    ++skips;
    return;
  }

  CHECK(query->GetDataSize() == sizeof(DWORD));

  DWORD data[2] = {0xffffffffu, 0xffffffffu};
  CHECK_HR(query->GetData(nullptr, 0, D3DGETDATA_FLUSH), S_OK);
  CHECK_HR(query->GetData(data, sizeof(data[0]), D3DGETDATA_FLUSH), S_OK);
  CHECK(data[1] == 0xffffffffu);

  CHECK_HR(query->Issue(D3DISSUE_BEGIN), D3D_OK);
  CHECK_HR(query->Issue(D3DISSUE_END), D3D_OK);
  wait_query(query);

  std::memset(data, 0xff, sizeof(data));
  CHECK_HR(query->GetData(data, 0, D3DGETDATA_FLUSH), S_OK);
  CHECK(data[0] == 0xffffffffu && data[1] == 0xffffffffu);

  query->Release();
}

void timestamp_query_public_sizes() {
  Fixture fixture;
  if (!fixture.init("timestamp_query_public_sizes")) return;

  IDirect3DQuery9 *freq_query = nullptr;
  HRESULT hr = fixture.device->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ,
      &freq_query);
  CHECK_HR_ANY(hr, D3D_OK, D3DERR_NOTAVAILABLE);
  if (!freq_query) {
    std::printf("SKIP:timestamp_query_public_sizes: timestamp queries are not supported\n");
    ++skips;
    return;
  }
  CHECK(freq_query->GetDataSize() == sizeof(UINT64));

  IDirect3DQuery9 *disjoint_query = nullptr;
  CHECK_HR(fixture.device->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT,
      &disjoint_query), D3D_OK);
  CHECK(disjoint_query != nullptr);
  if (disjoint_query) CHECK(disjoint_query->GetDataSize() == sizeof(BOOL));

  IDirect3DQuery9 *query = nullptr;
  CHECK_HR(fixture.device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &query), D3D_OK);
  CHECK(query != nullptr);
  if (query) CHECK(query->GetDataSize() == sizeof(UINT64));

  DWORD data[2] = {0xffffffffu, 0xffffffffu};
  CHECK_HR(freq_query->GetData(nullptr, 0, D3DGETDATA_FLUSH), S_OK);
  CHECK_HR(freq_query->GetData(data, sizeof(data[0]), D3DGETDATA_FLUSH), S_OK);
  CHECK(data[1] == 0xffffffffu);

  if (disjoint_query) {
    WORD disjoint[2] = {0xffffu, 0xffffu};
    CHECK_HR(disjoint_query->GetData(nullptr, 0, D3DGETDATA_FLUSH), S_OK);
    CHECK_HR(disjoint_query->GetData(disjoint, sizeof(disjoint[0]),
        D3DGETDATA_FLUSH), S_OK);
    CHECK(disjoint[1] == 0xffffu);
  }

  if (query) {
    data[0] = 0xffffffffu;
    data[1] = 0xffffffffu;
    CHECK_HR(query->GetData(nullptr, 0, D3DGETDATA_FLUSH), S_OK);
    CHECK_HR(query->GetData(data, sizeof(data[0]), D3DGETDATA_FLUSH), S_OK);
    CHECK(data[1] == 0xffffffffu);
  }

  if (query) query->Release();
  if (disjoint_query) disjoint_query->Release();
  freq_query->Release();
}

}  // namespace

int main() {
  query_support_probe();
  occlusion_query_public_sizes();
  timestamp_query_public_sizes();

  if (failures) {
    std::printf("d3d9_queries_x64: %d failure(s), %d skip(s)\n", failures, skips);
    return EXIT_FAILURE;
  }

  std::printf("d3d9_queries_x64: passed (%d skip(s))\n", skips);
  return skips ? 77 : EXIT_SUCCESS;
}
