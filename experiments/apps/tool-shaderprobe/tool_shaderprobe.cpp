// tool-shaderprobe: feed raw D3D9 shader bytecode files to CreateVertexShader
// / CreatePixelShader on the active d3d9.dll and print the HRESULTs.
//
// Usage: tool-shaderprobe-<arch>.exe [--skip N] <file> [<file> ...]
//   --skip N   drop N leading bytes from each file before treating the rest
//              as bytecode (X-Ray shaders_cache files carry a 4-byte CRC).
//
// The stage (vertex vs pixel) is chosen from the blob's version token, and
// the opposite-stage create is attempted too, so a probe records both the
// expected accept and the expected reject. Output is one line per attempt:
//   probe file=<path> token0=0x%08x stage=vs|ps create_vs=0x%08x create_ps=0x%08x
// Exit code: 0 if every matching-stage create succeeded, 1 otherwise.

#include <windows.h>
#include <d3d9.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

std::vector<unsigned char> readFile(const char* path, size_t skip) {
  std::vector<unsigned char> bytes;
  FILE* f = std::fopen(path, "rb");
  if (!f) {
    return bytes;
  }
  std::fseek(f, 0, SEEK_END);
  long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size > 0) {
    bytes.resize(static_cast<size_t>(size));
    if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) {
      bytes.clear();
    }
  }
  std::fclose(f);
  if (bytes.size() <= skip) {
    bytes.clear();
  } else if (skip > 0) {
    bytes.erase(bytes.begin(), bytes.begin() + static_cast<long>(skip));
  }
  return bytes;
}

}  // namespace

int main(int argc, char** argv) {
  size_t skip = 0;
  int argi = 1;
  if (argi + 1 < argc && std::strcmp(argv[argi], "--skip") == 0) {
    skip = static_cast<size_t>(std::strtoul(argv[argi + 1], nullptr, 10));
    argi += 2;
  }
  if (argi >= argc) {
    std::fprintf(stderr, "usage: tool-shaderprobe [--skip N] <file>...\n");
    return 2;
  }

  IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) {
    std::fprintf(stderr, "Direct3DCreate9 failed\n");
    return 2;
  }

  WNDCLASSA wc = {};
  wc.lpfnWndProc = DefWindowProcA;
  wc.hInstance = GetModuleHandleA(nullptr);
  wc.lpszClassName = "dxmt9ShaderProbe";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowA(wc.lpszClassName, "shaderprobe", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64,
                            nullptr, nullptr, wc.hInstance, nullptr);
  if (!hwnd) {
    std::fprintf(stderr, "CreateWindow failed\n");
    return 2;
  }

  D3DPRESENT_PARAMETERS pp = {};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferWidth = 64;
  pp.BackBufferHeight = 64;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.hDeviceWindow = hwnd;

  IDirect3DDevice9* dev = nullptr;
  HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                 D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
  if (FAILED(hr) || !dev) {
    std::fprintf(stderr, "CreateDevice failed hr=0x%08lx\n", static_cast<unsigned long>(hr));
    return 2;
  }

  int failures = 0;
  for (int i = argi; i < argc; ++i) {
    std::vector<unsigned char> blob = readFile(argv[i], skip);
    if (blob.size() < 8 || (blob.size() & 3u) != 0u) {
      std::printf("probe file=%s unreadable-or-misaligned size=%zu\n", argv[i], blob.size());
      ++failures;
      continue;
    }
    const DWORD* code = reinterpret_cast<const DWORD*>(blob.data());
    const unsigned token0 = static_cast<unsigned>(code[0]);
    const bool isVs = (token0 >> 16) == 0xFFFEu;
    const bool isPs = (token0 >> 16) == 0xFFFFu;

    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    const HRESULT hrVs = dev->CreateVertexShader(code, &vs);
    const HRESULT hrPs = dev->CreatePixelShader(code, &ps);
    std::printf("probe file=%s token0=0x%08x stage=%s create_vs=0x%08lx create_ps=0x%08lx\n",
                argv[i], token0, isVs ? "vs" : (isPs ? "ps" : "??"),
                static_cast<unsigned long>(hrVs), static_cast<unsigned long>(hrPs));
    if (vs) vs->Release();
    if (ps) ps->Release();
    if ((isVs && FAILED(hrVs)) || (isPs && FAILED(hrPs)) || (!isVs && !isPs)) {
      ++failures;
    }
  }

  dev->Release();
  d3d->Release();
  return failures == 0 ? 0 : 1;
}
