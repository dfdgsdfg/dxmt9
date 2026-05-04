/*
 * Focused D3D9 PE export conformance checks.
 *
 * Wine provenance: distilled from Wine dlls/d3d9/d3d9.spec and
 * dlls/d3d9/d3d9_main.c at 6e073d28dee3af7f4c965daec94644e0f9f92727.
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

using D3DPERF_BeginEventProc = int(WINAPI *)(D3DCOLOR, const WCHAR *);
using D3DPERF_EndEventProc = int(WINAPI *)(void);
using D3DPERF_GetStatusProc = DWORD(WINAPI *)(void);
using D3DPERF_QueryRepeatFrameProc = BOOL(WINAPI *)(void);
using D3DPERF_SetMarkerProc = void(WINAPI *)(D3DCOLOR, const WCHAR *);
using D3DPERF_SetOptionsProc = void(WINAPI *)(DWORD);
using D3DPERF_SetRegionProc = void(WINAPI *)(D3DCOLOR, const WCHAR *);
using DebugSetMuteProc = void(WINAPI *)(void);

void fail_at(int line, const char *message) {
  std::printf("FAIL:%d: %s\n", line, message);
  ++failures;
}

void check_at(int line, bool condition, const char *message) {
  if (!condition) fail_at(line, message);
}

#define CHECK(condition) check_at(__LINE__, !!(condition), #condition)

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

void export_smoke_and_perf_noops(HMODULE module) {
  require_export(module, "Direct3DCreate9");
  require_export(module, "Direct3DCreate9Ex");
  require_export(module, "Direct3DShaderValidatorCreate9");
  require_export(module, "Direct3DCreate9On12");

  auto begin_event = reinterpret_cast<D3DPERF_BeginEventProc>(
      require_export(module, "D3DPERF_BeginEvent"));
  auto end_event = reinterpret_cast<D3DPERF_EndEventProc>(
      require_export(module, "D3DPERF_EndEvent"));
  auto get_status = reinterpret_cast<D3DPERF_GetStatusProc>(
      require_export(module, "D3DPERF_GetStatus"));
  auto query_repeat_frame = reinterpret_cast<D3DPERF_QueryRepeatFrameProc>(
      require_export(module, "D3DPERF_QueryRepeatFrame"));
  auto set_marker = reinterpret_cast<D3DPERF_SetMarkerProc>(
      require_export(module, "D3DPERF_SetMarker"));
  auto set_options = reinterpret_cast<D3DPERF_SetOptionsProc>(
      require_export(module, "D3DPERF_SetOptions"));
  auto set_region = reinterpret_cast<D3DPERF_SetRegionProc>(
      require_export(module, "D3DPERF_SetRegion"));
  auto debug_set_mute = reinterpret_cast<DebugSetMuteProc>(
      require_export(module, "DebugSetMute"));

  if (begin_event && end_event) {
    CHECK(begin_event(0xff00ff00u, L"dxmt9-export-smoke") == 0);
    CHECK(end_event() == 0);
  }
  if (get_status) CHECK(get_status() == 0);
  if (query_repeat_frame) CHECK(query_repeat_frame() == FALSE);
  if (set_marker) set_marker(0xff0000ffu, L"dxmt9-marker");
  if (set_options) set_options(0);
  if (set_region) set_region(0xffff0000u, L"dxmt9-region");
  if (debug_set_mute) debug_set_mute();
}

}  // namespace

int main() {
  HMODULE module = load_d3d9_module();
  if (!module) {
    std::printf("SKIP: failed to load d3d9.dll, GetLastError=%lu\n", GetLastError());
    ++skips;
  } else {
    export_smoke_and_perf_noops(module);
    FreeLibrary(module);
  }

  if (failures) {
    std::printf("exports: %d failure(s), %d skip(s)\n", failures, skips);
    return EXIT_FAILURE;
  }

  std::printf("exports: passed (%d skip(s))\n", skips);
  return skips ? 77 : EXIT_SUCCESS;
}
