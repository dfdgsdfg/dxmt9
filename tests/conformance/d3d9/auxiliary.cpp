/*
 * Focused D3D9 auxiliary export conformance checks.
 *
 * Wine provenance: distilled from Wine dlls/d3d9/tests/device.c
 * test_shader_validator(), test_d3d9on12(), and dlls/d3d9/d3d9_main.c at
 * 6e073d28dee3af7f4c965daec94644e0f9f92727.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

const GUID iid_iunknown =
    {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const GUID iid_idirect3ddevice9on12 =
    {0xe7fda234, 0xb589, 0x4049, {0x94, 0x0d, 0x88, 0x78, 0x97, 0x75, 0x31, 0xc8}};

using ShaderValidatorCallback = HRESULT(WINAPI *)(const char *, int, DWORD_PTR,
    DWORD_PTR, const char *, void *);

struct IDirect3DShaderValidator9;

struct IDirect3DShaderValidator9Vtbl {
  HRESULT(WINAPI *QueryInterface)(IDirect3DShaderValidator9 *, REFIID, void **);
  ULONG(WINAPI *AddRef)(IDirect3DShaderValidator9 *);
  ULONG(WINAPI *Release)(IDirect3DShaderValidator9 *);
  HRESULT(WINAPI *Begin)(IDirect3DShaderValidator9 *, ShaderValidatorCallback, void *, DWORD_PTR);
  HRESULT(WINAPI *Instruction)(IDirect3DShaderValidator9 *, const char *, int, const DWORD *, unsigned int);
  HRESULT(WINAPI *End)(IDirect3DShaderValidator9 *);
};

struct IDirect3DShaderValidator9 {
  const IDirect3DShaderValidator9Vtbl *vtbl;
};

using Direct3DShaderValidatorCreate9Proc = IDirect3DShaderValidator9 *(WINAPI *)(void);
using Direct3DCreate9On12Proc = IDirect3D9 *(WINAPI *)(UINT, void *, UINT);

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
        line, call, static_cast<unsigned long>(actual), static_cast<unsigned long>(expected));
    ++failures;
  }
}

#define CHECK(condition) check_at(__LINE__, !!(condition), #condition)
#define CHECK_HR(actual, expected) check_hr_at(__LINE__, (actual), (expected), #actual)

void strip_filename(char *path) {
  char *slash = std::strrchr(path, '\\');
  char *alt_slash = std::strrchr(path, '/');

  if (!slash || alt_slash > slash) slash = alt_slash;
  if (slash) *slash = '\0';
}

HMODULE load_d3d9_module() {
  char exe_path[MAX_PATH];
  char candidate[MAX_PATH];
  HMODULE module;

  if (GetModuleFileNameA(nullptr, exe_path, sizeof(exe_path))) {
    strip_filename(exe_path);

    std::snprintf(candidate, sizeof(candidate), "%s\\d3d9.dll", exe_path);
    module = LoadLibraryA(candidate);
    if (module) return module;

    std::snprintf(candidate, sizeof(candidate), "%s\\..\\..\\src\\win32\\d3d9.dll", exe_path);
    module = LoadLibraryA(candidate);
    if (module) return module;
  }

  return LoadLibraryA("d3d9.dll");
}

FARPROC require_export(HMODULE module, const char *name) {
  FARPROC proc = GetProcAddress(module, name);
  if (!proc) {
    std::printf("FAIL: missing d3d9 export %s\n", name);
    ++failures;
  }
  return proc;
}

HRESULT WINAPI validator_callback(const char *, int, DWORD_PTR, DWORD_PTR,
    const char *, void *context) {
  unsigned int *calls = static_cast<unsigned int *>(context);
  if (calls) ++*calls;
  return S_OK;
}

void shader_validator_stub_behavior(HMODULE module) {
  static const DWORD ps_3_0 = D3DPS_VERSION(3, 0);
  static const DWORD end_token = 0x0000ffff;

  auto create_validator = reinterpret_cast<Direct3DShaderValidatorCreate9Proc>(
      require_export(module, "Direct3DShaderValidatorCreate9"));
  if (!create_validator) return;

  IDirect3DShaderValidator9 *validator = create_validator();
  CHECK(validator != nullptr);
  CHECK(validator->vtbl != nullptr);
  if (!validator || !validator->vtbl) return;

  void *out = reinterpret_cast<void *>(0xdeadbeef);
  HRESULT hr = validator->vtbl->QueryInterface(validator, iid_iunknown, &out);
  CHECK(hr == S_OK || hr == E_NOINTERFACE);
  if (hr == S_OK) {
    CHECK(out == validator);
    validator->vtbl->Release(validator);
  } else {
    CHECK(out == nullptr);
  }

  CHECK(validator->vtbl->AddRef(validator) > 0);
  CHECK(validator->vtbl->Release(validator) > 0);

  unsigned int callback_calls = 0;
  CHECK_HR(validator->vtbl->Begin(validator, validator_callback, &callback_calls, 0), S_OK);
  CHECK_HR(validator->vtbl->Instruction(validator, "dxmt9", 1, &ps_3_0, 1), S_OK);
  CHECK_HR(validator->vtbl->Instruction(validator, "dxmt9", 2, &end_token, 1), S_OK);
  CHECK_HR(validator->vtbl->End(validator), S_OK);

  validator->vtbl->Release(validator);
}

void d3d9on12_loader_safe_failure(HMODULE module) {
  auto create9on12 = reinterpret_cast<Direct3DCreate9On12Proc>(
      require_export(module, "Direct3DCreate9On12"));
  if (!create9on12) return;

  IDirect3D9 *d3d9 = create9on12(D3D_SDK_VERSION, nullptr, 0);
  if (!d3d9) {
    std::printf("INFO: Direct3DCreate9On12 returned NULL for disabled 9On12 path\n");
    return;
  }

  void *out = reinterpret_cast<void *>(0xdeadbeef);
  HRESULT hr = d3d9->QueryInterface(iid_idirect3ddevice9on12, &out);
  CHECK_HR(hr, E_NOINTERFACE);
  CHECK(out == nullptr);
  d3d9->Release();
}

}  // namespace

int main() {
  HMODULE module = load_d3d9_module();
  if (!module) {
    std::printf("SKIP: failed to load d3d9.dll, GetLastError=%lu\n", GetLastError());
    ++skips;
  } else {
    shader_validator_stub_behavior(module);
    d3d9on12_loader_safe_failure(module);
    FreeLibrary(module);
  }

  if (failures) {
    std::printf("auxiliary: %d failure(s), %d skip(s)\n", failures, skips);
    return EXIT_FAILURE;
  }

  std::printf("auxiliary: passed (%d skip(s))\n", skips);
  return skips ? 77 : EXIT_SUCCESS;
}
