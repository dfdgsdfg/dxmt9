#pragma once

// Public surface shared by the three implementation TUs that replaced
// device_c_common.cpp:
//   - device_c_marshal.cpp       (WoW64 / native pointer marshaling +
//                                 dxmt9DebugLog definition)
//   - device_c_shader_dump.cpp   (D3D9 shader bytecode debug dump)
//   - device_c_format_utils.cpp  (D3D <-> core enum/struct translation,
//                                 transformStateFromD3D, setShaderFloatConst)
// The header itself is intentionally not split — all three areas share the
// devicec namespace and the D9C* wrapper structs declared below.

#include "dxmt9/device_c.h"
#include "dxmt9/com.hpp"
#include "dxmt9/core.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dxmt9::d3d9::devicec {

struct Low4GBAllocation {
  void* ptr = nullptr;
  size_t size = 0;
  bool viaNt = false;
  bool viaHeap = false;

  constexpr explicit operator bool() const noexcept { return ptr != nullptr; }
};

struct ShadowLock {
  void* nativePtr = nullptr;
  uint32_t nativePitch = 0;
  uint32_t rowBytes = 0;
  uint32_t rows = 0;
  bool active = false;
  Low4GBAllocation shadow{};
};

void dxmt9DebugLog(const char* fmt, ...);
void maybeDumpShaderBytecode(const char* label, const uint32_t* bytecode, size_t wordCount, uint64_t hash);

uint32_t usageFromD3D(uint32_t usage);
uint32_t checkDeviceFormatUsageFromD3D(uint32_t usage);
uint32_t usageToD3D(uint32_t usage);
dxmt9::core::Format fmtFromD3D(uint32_t d3d);
uint32_t fmtToD3D(dxmt9::core::Format format);
dxmt9::core::MultiSampleType msTypeFromD3D(uint32_t d3d);
bool isSupportedD3DMultisample(uint32_t d3d);
uint32_t msTypeToD3D(dxmt9::core::MultiSampleType ms);
dxmt9::core::PresentInterval presentIntervalFromD3D(uint32_t d3d);
uint32_t presentIntervalToD3D(dxmt9::core::PresentInterval interval);
dxmt9::core::Pool poolFromD3D(uint32_t d3d);
uint32_t poolToD3D(dxmt9::core::Pool pool);
uint32_t textureTypeToResourceType(dxmt9::core::TextureType type);
dxmt9::core::PrimitiveType ptFromD3D(uint32_t d3d);
dxmt9::core::IndexType idxTypeFromD3D(uint32_t d3d);
dxmt9::core::PresentParameters ppFromC(const D9CPresentParams& c);
dxmt9::core::DisplayModeEx dmExFromC(const D9CDisplayModeEx& c);
void fillCCaps(const dxmt9::core::DeviceCaps& src, D9CCaps* out);
bool computeShaderBytecodeWordCount(const uint32_t* bytecode, size_t* outWords);
bool pointerFits32Bit(const void* ptr);
Low4GBAllocation allocateLow4GB(size_t size);
void freeLow4GB(Low4GBAllocation alloc);
void releaseShadowLock(ShadowLock& lock);
bool requiresWow64PointerShadow();
bool isWow64NativePointerAllowed(uint64_t value);

// Wow64 lock-rect shadow allocation upper bound.
//
// `nativePitch` is the byte stride between rows returned by the underlying
// backend lock; `rectHeight` is the locked rect height in texels (clamped
// to the level's logical height). `blockHeight` is the format's block
// height in texels (1 for uncompressed formats, 4 for BC/DXT formats).
//
// The result is the worst-case number of bytes a game may write through
// the lock pointer. It is the max of:
//   1. `nativePitch * blockHeight-aligned(rectHeight)` — the natural
//      block-row span padded to a full block boundary.
//   2. `nativePitch * blockHeight * kCompressedMipMinBlockRows` — a
//      compatibility floor for tiny BC mips where the parent-level
//      pitch is reported by Metal (e.g. 1024 for a BC3 1x1 mip of a
//      256x256 base) and games walk the lock pointer far past the
//      strict block-row bound. Observed in SFIV: writes faulted at
//      offsets 0x1000 and 0x2000 from a single-page shadow. The floor
//      reserves four block rows (16 KB for this case) so the
//      post-buffer page stays unmapped only well past any plausible
//      game write.
// See R-BACK fix for the SFIV BC3 level-9 page-fault (2026-05-10).
size_t computeShadowBytesUpperBound(uint32_t nativePitch, uint32_t rectHeight,
                                    uint32_t blockHeight);

class ScopedWow64ClientCall {
 public:
  ScopedWow64ClientCall();
  ~ScopedWow64ClientCall();
  ScopedWow64ClientCall(const ScopedWow64ClientCall&) = delete;
  ScopedWow64ClientCall& operator=(const ScopedWow64ClientCall&) = delete;
};

class ScopedWow64NativePointerAllowance {
 public:
  ScopedWow64NativePointerAllowance(const void* ptr, size_t size);
  ~ScopedWow64NativePointerAllowance();
  ScopedWow64NativePointerAllowance(const ScopedWow64NativePointerAllowance&) = delete;
  ScopedWow64NativePointerAllowance& operator=(const ScopedWow64NativePointerAllowance&) = delete;

 private:
  const void* ptr_ = nullptr;
  size_t size_ = 0;
};

uint32_t transformStateFromD3D(uint32_t state);
int32_t setShaderFloatConst(D9CDevice* device, uint32_t start, const float* data, uint32_t count,
                            bool pixelShader);

}  // namespace dxmt9::d3d9::devicec

struct D9CFactory {
  dxmt9::com::IDirect3D9Ex* iface;
  std::atomic<uint32_t> refs{1};

  explicit D9CFactory(dxmt9::com::IDirect3D9Ex* i) : iface(i) {}
  ~D9CFactory() {
    if (iface) {
      iface->Release();
    }
  }
};

struct D9CDevice {
  dxmt9::com::IDirect3DDevice9Ex* iface;
  std::atomic<uint32_t> refs{1};
  std::array<std::shared_ptr<dxmt9::core::Surface>, dxmt9::core::kMaxRenderTargets> renderTargets;
  std::array<bool, dxmt9::core::kMaxRenderTargets> renderTargetExplicit{};
  bool stateBlockRecording = false;
  std::optional<dxmt9::core::DeviceState> stateBlockBaseState;
  std::unordered_set<uint32_t> stateBlockRenderStates;
  std::unordered_map<uint32_t, uint32_t> stateBlockRenderStateValues;

  explicit D9CDevice(dxmt9::com::IDirect3DDevice9Ex* i) : iface(i) {}
  ~D9CDevice() {
    if (iface) {
      iface->Release();
    }
  }

  dxmt9::core::Device& dev() { return iface->coreDevice(); }
};

struct D9CSwapChain {
  dxmt9::com::IDirect3DSwapChain9* iface;
  std::atomic<uint32_t> refs{1};

  explicit D9CSwapChain(dxmt9::com::IDirect3DSwapChain9* i) : iface(i) {}
  ~D9CSwapChain() {
    if (iface) {
      iface->Release();
    }
  }
};

struct D9CTexture {
  std::shared_ptr<dxmt9::core::Texture> obj;
  D9CDevice* device;
  std::atomic<uint32_t> refs{1};
  std::unordered_map<uint32_t, dxmt9::d3d9::devicec::ShadowLock> wow64Locks;
  std::unordered_set<uint32_t> lockedLevels;
  uint32_t d3dFormat = 0;
  bool palettized = false;
  std::vector<std::vector<uint8_t>> p8Levels;
  std::array<uint32_t, 256> p8Palette{};

  D9CTexture(std::shared_ptr<dxmt9::core::Texture> o, D9CDevice* d)
      : obj(std::move(o)), device(d) {}
};

struct D9CBuffer {
  std::shared_ptr<dxmt9::core::Buffer> obj;
  std::atomic<uint32_t> refs{1};
  dxmt9::d3d9::devicec::ShadowLock wow64Lock;
  D9CBufferDesc desc{};
  bool lastLockReadOnly = false;

  explicit D9CBuffer(std::shared_ptr<dxmt9::core::Buffer> o) : obj(std::move(o)) {}
};

struct D9CSurface {
  std::shared_ptr<dxmt9::core::Surface> obj;
  D9CTexture* ownerTex{nullptr};
  uint32_t ownerLevel = 0;
  std::atomic<uint32_t> refs{1};
  dxmt9::d3d9::devicec::ShadowLock wow64Lock;
  bool locked = false;

  explicit D9CSurface(std::shared_ptr<dxmt9::core::Surface> o,
                      D9CTexture* owner = nullptr,
                      uint32_t level = 0)
      : obj(std::move(o)), ownerTex(owner), ownerLevel(level) {}
};

struct D9CShader {
  dxmt9::core::ShaderRef ref;
  std::vector<uint32_t> bytecodeWords;
  std::atomic<uint32_t> refs{1};
};

struct D9CVertexDecl {
  std::vector<dxmt9::core::VertexElement> elements;
  std::vector<D9CVertexElement> raw;
  std::atomic<uint32_t> refs{1};
};

struct D9CQuery {
  std::shared_ptr<dxmt9::core::Query> obj;
  D9CDevice* device;
  std::atomic<uint32_t> refs{1};
};

struct D9CStateBlock {
  std::shared_ptr<dxmt9::core::StateBlock> obj;
  D9CDevice* device;
  std::atomic<uint32_t> refs{1};
};
