#pragma once

#include "dxmt9/device_c.h"
#include "dxmt9/com.hpp"
#include "dxmt9/core.hpp"

#include <algorithm>
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
  Low4GBAllocation shadow{};
};

void dxmt9DebugLog(const char* fmt, ...);
void maybeDumpShaderBytecode(const char* label, const uint32_t* bytecode, size_t wordCount, uint64_t hash);

uint32_t usageFromD3D(uint32_t usage);
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
  bool stateBlockRecording = false;
  std::optional<dxmt9::core::DeviceState> stateBlockBaseState;
  std::unordered_set<uint32_t> stateBlockRenderStates;

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

  D9CTexture(std::shared_ptr<dxmt9::core::Texture> o, D9CDevice* d)
      : obj(std::move(o)), device(d) {}
};

struct D9CBuffer {
  std::shared_ptr<dxmt9::core::Buffer> obj;
  std::atomic<uint32_t> refs{1};
  dxmt9::d3d9::devicec::ShadowLock wow64Lock;
  D9CBufferDesc desc{};

  explicit D9CBuffer(std::shared_ptr<dxmt9::core::Buffer> o) : obj(std::move(o)) {}
};

struct D9CSurface {
  std::shared_ptr<dxmt9::core::Surface> obj;
  D9CTexture* ownerTex{nullptr};
  std::atomic<uint32_t> refs{1};
  dxmt9::d3d9::devicec::ShadowLock wow64Lock;

  explicit D9CSurface(std::shared_ptr<dxmt9::core::Surface> o, D9CTexture* owner = nullptr)
      : obj(std::move(o)), ownerTex(owner) {}
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
