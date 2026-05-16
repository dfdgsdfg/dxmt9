/*
 * R-TEST-13 module-boundary PE probe.
 *
 * This is not a Wine-oracle conformance test. It is a deliberately small
 * loader/bridge smoke that runs beside staged d3d9.dll and winemetal.dll,
 * resolves public D3D9 exports, and calls the factory path so the harness can
 * route failures before broader conformance or wild experiments run.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using Direct3DCreate9Proc = IDirect3D9 *(WINAPI *)(UINT);
using Direct3DCreate9ExProc = HRESULT(WINAPI *)(UINT, IDirect3D9Ex **);

const char *g_last_phase = "startup";

struct ProbeResult {
  const char *failure_category = "none";
  int failures = 0;
  int skips = 0;
};

void emit_phase(const char *phase, const char *event) {
  g_last_phase = phase;
  std::printf("[dxmt9-module-boundary] phase=%s event=%s\n", phase, event);
  std::fflush(stdout);
}

void json_escape(const char *text) {
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p) {
    switch (*p) {
      case '\\': std::printf("\\\\"); break;
      case '"': std::printf("\\\""); break;
      case '\n': std::printf("\\n"); break;
      case '\r': std::printf("\\r"); break;
      case '\t': std::printf("\\t"); break;
      default:
        if (*p < 0x20) {
          std::printf("\\u%04x", static_cast<unsigned int>(*p));
        } else {
          std::putchar(*p);
        }
        break;
    }
  }
}

void strip_filename(char *path) {
  char *slash = std::strrchr(path, '\\');
  char *alt_slash = std::strrchr(path, '/');
  if (!slash || alt_slash > slash) slash = alt_slash;
  if (slash) *slash = '\0';
}

const char *load_mode() {
  const char *mode = std::getenv("DXMT9_MODULE_BOUNDARY_LOAD_MODE");
  return mode ? mode : "app-local";
}

HMODULE load_staged_d3d9(char *loaded_path, DWORD loaded_path_size) {
  char exe_path[MAX_PATH] = {};
  char candidate[MAX_PATH] = {};

  if (std::strcmp(load_mode(), "builtin") != 0 && GetModuleFileNameA(nullptr, exe_path, sizeof(exe_path))) {
    strip_filename(exe_path);
    std::snprintf(candidate, sizeof(candidate), "%s\\d3d9.dll", exe_path);
    HMODULE module = LoadLibraryA(candidate);
    if (module) {
      std::snprintf(loaded_path, loaded_path_size, "%s", candidate);
      return module;
    }
  }

  HMODULE module = LoadLibraryA("d3d9.dll");
  if (module) {
    DWORD len = GetModuleFileNameA(module, loaded_path, loaded_path_size);
    if (!len || len >= loaded_path_size) std::snprintf(loaded_path, loaded_path_size, "d3d9.dll");
  }
  return module;
}

FARPROC require_export(HMODULE module, const char *name, ProbeResult &result) {
  FARPROC proc = GetProcAddress(module, name);
  if (!proc) {
    result.failure_category = "pe-loader-export";
    ++result.failures;
  }
  return proc;
}

void print_json(const ProbeResult &result, const char *loaded_path, bool create9,
                bool create9ex, unsigned int adapter_count, HRESULT create9ex_hr) {
  std::printf("{\n");
  std::printf("  \"schema\": \"dxmt9.module_boundary.probe.v1\",\n");
  std::printf("  \"load_mode\": \"");
  json_escape(load_mode());
  std::printf("\",\n");
  std::printf("  \"last_phase\": \"");
  json_escape(g_last_phase);
  std::printf("\",\n");
  std::printf("  \"failure_category\": \"%s\",\n", result.failure_category);
  std::printf("  \"failures\": %d,\n", result.failures);
  std::printf("  \"skips\": %d,\n", result.skips);
  std::printf("  \"loaded_d3d9\": \"");
  json_escape(loaded_path);
  std::printf("\",\n");
  std::printf("  \"checks\": [\n");
  std::printf("    {\"name\": \"load_d3d9\", \"status\": \"%s\"},\n",
              loaded_path[0] ? "pass" : "fail");
  std::printf("    {\"name\": \"Direct3DCreate9_export\", \"status\": \"%s\"},\n",
              create9 ? "pass" : "fail");
  std::printf("    {\"name\": \"Direct3DCreate9Ex_export\", \"status\": \"%s\"},\n",
              create9ex ? "pass" : "fail");
  std::printf("    {\"name\": \"factory_smoke\", \"status\": \"%s\", \"adapter_count\": %u},\n",
              adapter_count > 0 ? "pass" : "skip", adapter_count);
  std::printf("    {\"name\": \"factory_ex_smoke\", \"status\": \"%s\", \"hr\": %ld}\n",
              create9ex_hr == D3D_OK ? "pass" : "skip", static_cast<long>(create9ex_hr));
  std::printf("  ]\n");
  std::printf("}\n");
}

}  // namespace

int main() {
  ProbeResult result;
  char loaded_path[MAX_PATH] = {};
  bool has_create9 = false;
  bool has_create9ex = false;
  unsigned int adapter_count = 0;
  HRESULT create9ex_hr = E_NOTIMPL;

  emit_phase("startup", "begin");
  emit_phase("load_d3d9", "begin");
  HMODULE module = load_staged_d3d9(loaded_path, sizeof(loaded_path));
  emit_phase("load_d3d9", module ? "pass" : "fail");
  if (!module) {
    result.failure_category = "pe-loader-export";
    ++result.failures;
    print_json(result, loaded_path, false, false, adapter_count, create9ex_hr);
    return EXIT_FAILURE;
  }

  emit_phase("export", "begin");
  auto create9 = reinterpret_cast<Direct3DCreate9Proc>(
      require_export(module, "Direct3DCreate9", result));
  auto create9ex = reinterpret_cast<Direct3DCreate9ExProc>(
      require_export(module, "Direct3DCreate9Ex", result));
  has_create9 = create9 != nullptr;
  has_create9ex = create9ex != nullptr;
  emit_phase("export", (has_create9 || has_create9ex) ? "pass" : "fail");

  if (create9) {
    emit_phase("factory", "Direct3DCreate9_begin");
    IDirect3D9 *d3d9 = create9(D3D_SDK_VERSION);
    if (d3d9) {
      adapter_count = d3d9->GetAdapterCount();
      d3d9->Release();
      emit_phase("factory", "Direct3DCreate9_pass");
    } else {
      result.failure_category = "public-d3d9-smoke";
      ++result.failures;
      emit_phase("factory", "Direct3DCreate9_fail");
    }
  }

  if (create9ex) {
    emit_phase("factory_ex", "Direct3DCreate9Ex_begin");
    IDirect3D9Ex *d3d9ex = nullptr;
    create9ex_hr = create9ex(D3D_SDK_VERSION, &d3d9ex);
    if (create9ex_hr == D3D_OK && d3d9ex) {
      d3d9ex->Release();
      emit_phase("factory_ex", "Direct3DCreate9Ex_pass");
    } else {
      emit_phase("factory_ex", "Direct3DCreate9Ex_skip");
    }
  }

  emit_phase("free_library", "begin");
  FreeLibrary(module);
  emit_phase("free_library", "pass");
  print_json(result, loaded_path, has_create9, has_create9ex, adapter_count, create9ex_hr);
  return result.failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
