#include "../common/dx9_fast_sanity.hpp"

#include <array>
#include <cstdio>
#include <cstring>

namespace {

constexpr UINT kWidth = 640;
constexpr UINT kHeight = 360;
constexpr UINT kBufferSize = 512;
constexpr UINT kTextureWidth = 16;
constexpr UINT kTextureHeight = 16;
constexpr char kWindowClass[] = "dxmt9_d9vk_lock_matrix_window";
constexpr char kWindowTitle[] = "conf-d3d9-lock-matrix";

struct TestStats {
  int passed = 0;
  int failed = 0;

  void pass(const char* name) {
    dxmt9::fastsanity::logf("PASS: %s", name);
    ++passed;
  }

  void fail(const char* name) {
    dxmt9::fastsanity::logf("FAIL: %s", name);
    ++failed;
  }

  bool ok() const {
    return failed == 0;
  }
};

struct BufferCase {
  const char* name;
  D3DPOOL pool;
  DWORD usage;
  DWORD flags;
};

struct TextureCase {
  const char* name;
  D3DPOOL pool;
  DWORD usage;
  DWORD flags;
  bool rect;
  bool readonly;
};

constexpr std::array<BufferCase, 12> kBufferCases = {{
    {"vb default plain", D3DPOOL_DEFAULT, 0, 0},
    {"vb default discard", D3DPOOL_DEFAULT, 0, D3DLOCK_DISCARD},
    {"vb default nooverwrite", D3DPOOL_DEFAULT, 0, D3DLOCK_NOOVERWRITE},
    {"vb default donotwait", D3DPOOL_DEFAULT, 0, D3DLOCK_DONOTWAIT},
    {"vb dynamic discard", D3DPOOL_DEFAULT, D3DUSAGE_DYNAMIC, D3DLOCK_DISCARD},
    {"vb dynamic donotwait", D3DPOOL_DEFAULT, D3DUSAGE_DYNAMIC, D3DLOCK_DONOTWAIT},
    {"vb dynamic discard nooverwrite", D3DPOOL_DEFAULT, D3DUSAGE_DYNAMIC, D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE},
    {"vb systemmem plain", D3DPOOL_SYSTEMMEM, 0, 0},
    {"vb systemmem discard ignored", D3DPOOL_SYSTEMMEM, 0, D3DLOCK_DISCARD},
    {"vb systemmem nooverwrite ignored", D3DPOOL_SYSTEMMEM, 0, D3DLOCK_NOOVERWRITE},
    {"vb systemmem readonly", D3DPOOL_SYSTEMMEM, 0, D3DLOCK_READONLY},
    {"vb systemmem discard readonly", D3DPOOL_SYSTEMMEM, 0, D3DLOCK_DISCARD | D3DLOCK_READONLY},
}};

constexpr std::array<TextureCase, 8> kTextureCases = {{
    {"texture systemmem full", D3DPOOL_SYSTEMMEM, 0, 0, false, false},
    {"texture systemmem rect", D3DPOOL_SYSTEMMEM, 0, 0, true, false},
    {"texture systemmem readonly", D3DPOOL_SYSTEMMEM, 0, D3DLOCK_READONLY, false, true},
    {"texture systemmem discard ignored", D3DPOOL_SYSTEMMEM, 0, D3DLOCK_DISCARD, false, false},
    {"texture dynamic default discard", D3DPOOL_DEFAULT, D3DUSAGE_DYNAMIC, D3DLOCK_DISCARD, false, false},
    {"surface systemmem full", D3DPOOL_SYSTEMMEM, 0, 0, false, false},
    {"surface systemmem rect", D3DPOOL_SYSTEMMEM, 0, 0, true, false},
    {"surface systemmem readonly", D3DPOOL_SYSTEMMEM, 0, D3DLOCK_READONLY, false, true},
}};

void makePattern(unsigned char* data, size_t size, unsigned int seed) {
  for (size_t i = 0; i < size; ++i) {
    data[i] = static_cast<unsigned char>((seed + i * 17u) & 0xffu);
  }
}

bool fillBuffer(IDirect3DVertexBuffer9* buffer, const unsigned char* pattern, const char* label) {
  using namespace dxmt9::fastsanity;
  void* mapped = nullptr;
  HRESULT hr = buffer->Lock(0, 0, &mapped, 0);
  if (FAILED(hr) || mapped == nullptr) {
    log_hresult(label, hr);
    return false;
  }
  std::memcpy(mapped, pattern, kBufferSize);
  hr = buffer->Unlock();
  if (FAILED(hr)) {
    log_hresult("VertexBuffer::Unlock(fill)", hr);
    return false;
  }
  return true;
}

bool verifyBuffer(IDirect3DVertexBuffer9* buffer, const unsigned char* pattern, const char* label) {
  using namespace dxmt9::fastsanity;
  void* mapped = nullptr;
  HRESULT hr = buffer->Lock(0, 0, &mapped, 0);
  if (FAILED(hr) || mapped == nullptr) {
    log_hresult(label, hr);
    return false;
  }
  const bool match = std::memcmp(mapped, pattern, kBufferSize) == 0;
  hr = buffer->Unlock();
  if (FAILED(hr)) {
    log_hresult("VertexBuffer::Unlock(verify)", hr);
    return false;
  }
  return match;
}

bool testVertexBuffer(IDirect3DDevice9Ex* device, const BufferCase& tc, const unsigned char* pattern) {
  using namespace dxmt9::fastsanity;
  ComPtr<IDirect3DVertexBuffer9> buffer;
  HRESULT hr = device->CreateVertexBuffer(kBufferSize, tc.usage, 0, tc.pool, buffer.put(), nullptr);
  if (FAILED(hr)) {
    log_hresult("CreateVertexBuffer", hr);
    return false;
  }

  if (!fillBuffer(buffer.ptr(), pattern, "VertexBuffer::Lock(prefill)")) {
    return false;
  }

  void* mapped = nullptr;
  hr = buffer->Lock(0, 0, &mapped, tc.flags);
  if (FAILED(hr) || mapped == nullptr) {
    log_hresult("VertexBuffer::Lock(matrix)", hr);
    return false;
  }

  if ((tc.flags & D3DLOCK_READONLY) == 0) {
    std::memcpy(mapped, pattern, kBufferSize);
  }

  hr = buffer->Unlock();
  if (FAILED(hr)) {
    log_hresult("VertexBuffer::Unlock(matrix)", hr);
    return false;
  }

  return verifyBuffer(buffer.ptr(), pattern, "VertexBuffer::Lock(verify)");
}

bool testIndexBuffer(IDirect3DDevice9Ex* device, const BufferCase& tc, const unsigned char* pattern) {
  using namespace dxmt9::fastsanity;
  ComPtr<IDirect3DIndexBuffer9> buffer;
  HRESULT hr = device->CreateIndexBuffer(kBufferSize, tc.usage, D3DFMT_INDEX16, tc.pool, buffer.put(), nullptr);
  if (FAILED(hr)) {
    log_hresult("CreateIndexBuffer", hr);
    return false;
  }

  void* mapped = nullptr;
  hr = buffer->Lock(0, 0, &mapped, 0);
  if (FAILED(hr) || mapped == nullptr) {
    log_hresult("IndexBuffer::Lock(prefill)", hr);
    return false;
  }
  std::memcpy(mapped, pattern, kBufferSize);
  hr = buffer->Unlock();
  if (FAILED(hr)) {
    log_hresult("IndexBuffer::Unlock(prefill)", hr);
    return false;
  }

  mapped = nullptr;
  hr = buffer->Lock(0, 0, &mapped, tc.flags);
  if (FAILED(hr) || mapped == nullptr) {
    log_hresult("IndexBuffer::Lock(matrix)", hr);
    return false;
  }
  if ((tc.flags & D3DLOCK_READONLY) == 0) {
    std::memcpy(mapped, pattern, kBufferSize);
  }
  hr = buffer->Unlock();
  if (FAILED(hr)) {
    log_hresult("IndexBuffer::Unlock(matrix)", hr);
    return false;
  }

  mapped = nullptr;
  hr = buffer->Lock(0, 0, &mapped, 0);
  if (FAILED(hr) || mapped == nullptr) {
    log_hresult("IndexBuffer::Lock(verify)", hr);
    return false;
  }
  const bool match = std::memcmp(mapped, pattern, kBufferSize) == 0;
  hr = buffer->Unlock();
  if (FAILED(hr)) {
    log_hresult("IndexBuffer::Unlock(verify)", hr);
    return false;
  }
  return match;
}

D3DCOLOR patternColor(UINT x, UINT y, unsigned int seed) {
  const unsigned int value = seed + x * 23u + y * 41u;
  return D3DCOLOR_ARGB(255,
                       static_cast<unsigned char>(value & 0xffu),
                       static_cast<unsigned char>((value >> 3) & 0xffu),
                       static_cast<unsigned char>((value >> 5) & 0xffu));
}

void writePattern(D3DLOCKED_RECT& lock, UINT width, UINT height, unsigned int seed) {
  for (UINT y = 0; y < height; ++y) {
    auto* row = reinterpret_cast<D3DCOLOR*>(
        static_cast<unsigned char*>(lock.pBits) + static_cast<size_t>(y) * lock.Pitch);
    for (UINT x = 0; x < width; ++x) {
      row[x] = patternColor(x, y, seed);
    }
  }
}

bool verifyPattern(const D3DLOCKED_RECT& lock, UINT width, UINT height, unsigned int seed) {
  for (UINT y = 0; y < height; ++y) {
    const auto* row = reinterpret_cast<const D3DCOLOR*>(
        static_cast<const unsigned char*>(lock.pBits) + static_cast<size_t>(y) * lock.Pitch);
    for (UINT x = 0; x < width; ++x) {
      if (row[x] != patternColor(x, y, seed)) {
        return false;
      }
    }
  }
  return true;
}

bool prefillTexture(IDirect3DTexture9* texture, unsigned int seed) {
  using namespace dxmt9::fastsanity;
  D3DLOCKED_RECT lock{};
  HRESULT hr = texture->LockRect(0, &lock, nullptr, 0);
  if (FAILED(hr) || lock.pBits == nullptr) {
    log_hresult("Texture::LockRect(prefill)", hr);
    return false;
  }
  writePattern(lock, kTextureWidth, kTextureHeight, seed);
  hr = texture->UnlockRect(0);
  if (FAILED(hr)) {
    log_hresult("Texture::UnlockRect(prefill)", hr);
    return false;
  }
  return true;
}

bool verifyTexture(IDirect3DTexture9* texture, unsigned int seed) {
  using namespace dxmt9::fastsanity;
  D3DLOCKED_RECT lock{};
  HRESULT hr = texture->LockRect(0, &lock, nullptr, D3DLOCK_READONLY);
  if (FAILED(hr) || lock.pBits == nullptr) {
    log_hresult("Texture::LockRect(verify)", hr);
    return false;
  }
  const bool match = verifyPattern(lock, kTextureWidth, kTextureHeight, seed);
  hr = texture->UnlockRect(0);
  if (FAILED(hr)) {
    log_hresult("Texture::UnlockRect(verify)", hr);
    return false;
  }
  return match;
}

bool verifyTextureRect(IDirect3DTexture9* texture, const RECT& rect, unsigned int seed) {
  using namespace dxmt9::fastsanity;
  D3DLOCKED_RECT lock{};
  HRESULT hr = texture->LockRect(0, &lock, &rect, D3DLOCK_READONLY);
  if (FAILED(hr) || lock.pBits == nullptr) {
    log_hresult("Texture::LockRect(verify rect)", hr);
    return false;
  }
  const bool match = verifyPattern(lock,
                                   static_cast<UINT>(rect.right - rect.left),
                                   static_cast<UINT>(rect.bottom - rect.top),
                                   seed);
  hr = texture->UnlockRect(0);
  if (FAILED(hr)) {
    log_hresult("Texture::UnlockRect(verify rect)", hr);
    return false;
  }
  return match;
}

bool testTexture(IDirect3DDevice9Ex* device, const TextureCase& tc, unsigned int seed) {
  using namespace dxmt9::fastsanity;
  ComPtr<IDirect3DTexture9> texture;
  HRESULT hr = device->CreateTexture(kTextureWidth,
                                     kTextureHeight,
                                     1,
                                     tc.usage,
                                     D3DFMT_A8R8G8B8,
                                     tc.pool,
                                     texture.put(),
                                     nullptr);
  if (FAILED(hr)) {
    log_hresult("CreateTexture", hr);
    return false;
  }

  if (tc.pool != D3DPOOL_DEFAULT && !prefillTexture(texture.ptr(), seed)) {
    return false;
  }

  D3DLOCKED_RECT lock{};
  RECT rect{2, 3, 11, 13};
  const RECT* lockRect = tc.rect ? &rect : nullptr;
  hr = texture->LockRect(0, &lock, lockRect, tc.flags);
  if (FAILED(hr) || lock.pBits == nullptr) {
    log_hresult("Texture::LockRect(matrix)", hr);
    return false;
  }

  if (!tc.readonly) {
    writePattern(lock, tc.rect ? 9u : kTextureWidth, tc.rect ? 10u : kTextureHeight, seed + 7u);
  }

  hr = texture->UnlockRect(0);
  if (FAILED(hr)) {
    log_hresult("Texture::UnlockRect(matrix)", hr);
    return false;
  }

  if (tc.pool == D3DPOOL_DEFAULT) {
    return true;
  }

  if (tc.rect && !tc.readonly) {
    return verifyTextureRect(texture.ptr(), rect, seed + 7u);
  }
  return verifyTexture(texture.ptr(), tc.readonly ? seed : seed + 7u);
}

bool prefillSurface(IDirect3DSurface9* surface, unsigned int seed) {
  using namespace dxmt9::fastsanity;
  D3DLOCKED_RECT lock{};
  HRESULT hr = surface->LockRect(&lock, nullptr, 0);
  if (FAILED(hr) || lock.pBits == nullptr) {
    log_hresult("Surface::LockRect(prefill)", hr);
    return false;
  }
  writePattern(lock, kTextureWidth, kTextureHeight, seed);
  hr = surface->UnlockRect();
  if (FAILED(hr)) {
    log_hresult("Surface::UnlockRect(prefill)", hr);
    return false;
  }
  return true;
}

bool verifySurface(IDirect3DSurface9* surface, unsigned int seed) {
  using namespace dxmt9::fastsanity;
  D3DLOCKED_RECT lock{};
  HRESULT hr = surface->LockRect(&lock, nullptr, D3DLOCK_READONLY);
  if (FAILED(hr) || lock.pBits == nullptr) {
    log_hresult("Surface::LockRect(verify)", hr);
    return false;
  }
  const bool match = verifyPattern(lock, kTextureWidth, kTextureHeight, seed);
  hr = surface->UnlockRect();
  if (FAILED(hr)) {
    log_hresult("Surface::UnlockRect(verify)", hr);
    return false;
  }
  return match;
}

bool verifySurfaceRect(IDirect3DSurface9* surface, const RECT& rect, unsigned int seed) {
  using namespace dxmt9::fastsanity;
  D3DLOCKED_RECT lock{};
  HRESULT hr = surface->LockRect(&lock, &rect, D3DLOCK_READONLY);
  if (FAILED(hr) || lock.pBits == nullptr) {
    log_hresult("Surface::LockRect(verify rect)", hr);
    return false;
  }
  const bool match = verifyPattern(lock,
                                   static_cast<UINT>(rect.right - rect.left),
                                   static_cast<UINT>(rect.bottom - rect.top),
                                   seed);
  hr = surface->UnlockRect();
  if (FAILED(hr)) {
    log_hresult("Surface::UnlockRect(verify rect)", hr);
    return false;
  }
  return match;
}

bool testSurface(IDirect3DDevice9Ex* device, const TextureCase& tc, unsigned int seed) {
  using namespace dxmt9::fastsanity;
  ComPtr<IDirect3DSurface9> surface;
  HRESULT hr = device->CreateOffscreenPlainSurface(kTextureWidth,
                                                   kTextureHeight,
                                                   D3DFMT_A8R8G8B8,
                                                   tc.pool,
                                                   surface.put(),
                                                   nullptr);
  if (FAILED(hr)) {
    log_hresult("CreateOffscreenPlainSurface", hr);
    return false;
  }

  if (!prefillSurface(surface.ptr(), seed)) {
    return false;
  }

  D3DLOCKED_RECT lock{};
  RECT rect{2, 3, 11, 13};
  const RECT* lockRect = tc.rect ? &rect : nullptr;
  hr = surface->LockRect(&lock, lockRect, tc.flags);
  if (FAILED(hr) || lock.pBits == nullptr) {
    log_hresult("Surface::LockRect(matrix)", hr);
    return false;
  }

  if (!tc.readonly) {
    writePattern(lock, tc.rect ? 9u : kTextureWidth, tc.rect ? 10u : kTextureHeight, seed + 7u);
  }

  hr = surface->UnlockRect();
  if (FAILED(hr)) {
    log_hresult("Surface::UnlockRect(matrix)", hr);
    return false;
  }

  if (tc.rect && !tc.readonly) {
    return verifySurfaceRect(surface.ptr(), rect, seed + 7u);
  }
  return verifySurface(surface.ptr(), tc.readonly ? seed : seed + 7u);
}

void runLockMatrix(IDirect3DDevice9Ex* device, TestStats& stats) {
  std::array<unsigned char, kBufferSize> data{};
  makePattern(data.data(), data.size(), 0x35u);

  for (const auto& tc : kBufferCases) {
    if (testVertexBuffer(device, tc, data.data())) {
      stats.pass(tc.name);
    } else {
      stats.fail(tc.name);
    }
  }

  for (const auto& tc : kBufferCases) {
    char name[128]{};
    std::snprintf(name, sizeof(name), "ib%s", tc.name + 2);
    if (testIndexBuffer(device, tc, data.data())) {
      stats.pass(name);
    } else {
      stats.fail(name);
    }
  }

  unsigned int seed = 0x51u;
  for (const auto& tc : kTextureCases) {
    const bool isSurface = std::strncmp(tc.name, "surface", 7) == 0;
    const bool ok = isSurface ? testSurface(device, tc, seed) : testTexture(device, tc, seed);
    if (ok) {
      stats.pass(tc.name);
    } else {
      stats.fail(tc.name);
    }
    seed += 0x21u;
  }
}

bool runLockMatrixApp(HINSTANCE instance) {
  using namespace dxmt9::fastsanity;

  const CaptureConfig capture = loadCaptureConfig();
  HWND hwnd = createWindow(instance, kWindowClass, kWindowTitle, static_cast<int>(kWidth), static_cast<int>(kHeight));
  if (!hwnd) {
    logf("FAIL: CreateWindowExA");
    return false;
  }

  ComPtr<IDirect3D9Ex> d3d;
  ComPtr<IDirect3DDevice9Ex> device;
  D3DPRESENT_PARAMETERS params{};
  if (!createDeviceEx(hwnd, kWidth, kHeight, d3d, device, params)) {
    destroyWindow(instance, kWindowClass, hwnd);
    return false;
  }

  TestStats stats;
  runLockMatrix(device.ptr(), stats);

  const D3DCOLOR clearColor = stats.ok() ? D3DCOLOR_XRGB(40, 190, 40) : D3DCOLOR_XRGB(190, 40, 40);
  HRESULT hr = device->BeginScene();
  if (FAILED(hr)) {
    log_hresult("BeginScene", hr);
    destroyWindow(instance, kWindowClass, hwnd);
    return false;
  }
  hr = device->Clear(0, nullptr, D3DCLEAR_TARGET, clearColor, 0.0f, 0);
  if (FAILED(hr)) {
    log_hresult("Clear", hr);
    destroyWindow(instance, kWindowClass, hwnd);
    return false;
  }
  hr = device->EndScene();
  if (FAILED(hr)) {
    log_hresult("EndScene", hr);
    destroyWindow(instance, kWindowClass, hwnd);
    return false;
  }

  if (!captureBackbuffer(device.ptr(), capture)) {
    destroyWindow(instance, kWindowClass, hwnd);
    return false;
  }

  hr = device->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
  if (FAILED(hr)) {
    log_hresult("PresentEx", hr);
    destroyWindow(instance, kWindowClass, hwnd);
    return false;
  }

  logf("SUMMARY: passed=%d failed=%d", stats.passed, stats.failed);
  destroyWindow(instance, kWindowClass, hwnd);
  return stats.ok();
}

}  // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
  return runLockMatrixApp(instance) ? 0 : 1;
}
