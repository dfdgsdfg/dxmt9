#pragma once

#include "dxmt9/core.hpp"

#include <atomic>
#include <memory>

namespace dxmt9 {
// Upper-runtime Device — defined in src/dxmt9/dxmt9_device.hpp (runtime
// internal header, intentionally not in the public include tree). Only
// callers that construct a unique_ptr<Device> need the full type and
// include the header themselves.
class Device;
}  // namespace dxmt9

namespace dxmt9::com {

using core::HResult;
using core::DisplayModeEx;
using core::DisplayModeFilter;
using core::DisplayRotation;
using core::Luid;
using core::u32;

inline constexpr u32 D3D_SDK_VERSION = 32;

enum class InterfaceId {
  IUnknown,
  Direct3D9,
  Direct3D9Ex,
  Direct3DDevice9,
  Direct3DDevice9Ex,
  Direct3DSwapChain9,
};

class IUnknown {
 public:
  virtual ~IUnknown() = default;
  virtual u32 AddRef() = 0;
  virtual u32 Release() = 0;
  virtual bool QueryInterface(InterfaceId iid, void** object) = 0;
};

class IDirect3DDevice9;
class IDirect3DSwapChain9;
class IDirect3DDevice9Ex;
class IDirect3D9Ex;

class IDirect3D9 : public IUnknown {
 public:
  virtual size_t GetAdapterCount() const = 0;
  virtual const core::DeviceCaps& GetDeviceCaps(size_t adapterIndex) const = 0;
  virtual core::AdapterIdentifier GetAdapterIdentifier(size_t adapterIndex) const = 0;
  virtual std::vector<core::DisplayMode> EnumAdapterModes(size_t adapterIndex,
                                                          core::Format format) const = 0;
  virtual core::DisplayMode GetAdapterDisplayMode(size_t adapterIndex) const = 0;
  virtual u32 GetAdapterMonitor(size_t adapterIndex) const = 0;
  virtual core::HResult CheckDeviceType(size_t adapterIndex, core::DeviceType deviceType,
                                        core::Format adapterFormat, core::Format backBufferFormat,
                                        bool windowed) const = 0;
  virtual core::HResult CheckDeviceFormat(size_t adapterIndex, core::Format format, u32 usage) const = 0;
  virtual core::HResult CheckDeviceMultiSampleType(size_t adapterIndex, core::Format format,
                                                   core::MultiSampleType type) const = 0;
  virtual IDirect3DDevice9* CreateDevice(size_t adapterIndex, const core::PresentParameters& params,
                                         u32 behaviorFlags = 0) = 0;
};

class IDirect3D9Ex : public IDirect3D9 {
 public:
  virtual size_t GetAdapterModeCountEx(size_t adapterIndex, const DisplayModeFilter* filter = nullptr) const = 0;
  virtual bool EnumAdapterModesEx(size_t adapterIndex, const DisplayModeFilter* filter, size_t modeIndex,
                                  DisplayModeEx* mode) const = 0;
  virtual bool GetAdapterDisplayModeEx(size_t adapterIndex, DisplayModeEx* mode,
                                       DisplayRotation* rotation) const = 0;
  virtual bool GetAdapterLUID(size_t adapterIndex, Luid* luid) const = 0;
  virtual IDirect3DDevice9Ex* CreateDeviceEx(size_t adapterIndex, const core::PresentParameters& params,
                                             const DisplayModeEx* fullscreenMode = nullptr,
                                             u32 behaviorFlags = 0) = 0;
};

class IDirect3DSwapChain9 : public IUnknown {
 public:
  virtual core::SwapChain& coreSwapChain() = 0;
  virtual const core::SwapChain& coreSwapChain() const = 0;
  virtual const core::PresentParameters& presentParameters() const = 0;
  virtual std::shared_ptr<core::Surface> backBuffer() const = 0;
  virtual std::shared_ptr<core::Surface> depthStencilSurface() const = 0;
  virtual core::HResult Present() = 0;
};

class IDirect3DDevice9 : public IUnknown {
 public:
  virtual core::Device& coreDevice() = 0;
  virtual const core::Device& coreDevice() const = 0;

  virtual const core::DeviceCaps& GetDeviceCaps() const = 0;
  virtual core::HResult TestCooperativeLevel() const = 0;
  virtual core::HResult Reset(const core::PresentParameters& params) = 0;
  virtual IDirect3DSwapChain9* CreateAdditionalSwapChain(const core::PresentParameters& params) = 0;
  virtual size_t GetSwapChainCount() const = 0;
  virtual IDirect3DSwapChain9* GetSwapChain(size_t index = 0) const = 0;
  virtual core::HResult Present() = 0;
  virtual core::HResult BeginScene() = 0;
  virtual core::HResult EndScene() = 0;
  virtual core::HResult Clear(const core::ClearDesc& desc) = 0;
  virtual core::HResult DrawPrimitive(core::PrimitiveType type, u32 primitiveCount, u32 startVertex = 0) = 0;
  virtual core::HResult DrawIndexedPrimitive(core::PrimitiveType type, u32 primitiveCount, u32 startVertex,
                                              core::i32 baseVertexIndex, u32 startIndex,
                                              core::IndexType indexType) = 0;
  virtual core::HResult DrawPrimitiveUP(core::PrimitiveType type, u32 primitiveCount,
                                        std::span<const core::u8> vertexData,
                                        u32 vertexStride = 0) = 0;
  virtual core::HResult DrawIndexedPrimitiveUP(core::PrimitiveType type, u32 primitiveCount,
                                               std::span<const core::u8> vertexData,
                                               std::span<const core::u8> indexData,
                                               core::IndexType indexType,
                                               u32 vertexStride = 0) = 0;
  virtual core::HResult SetRenderState(u32 key, u32 value) = 0;
  virtual u32 GetRenderState(u32 key) const = 0;
  virtual core::HResult SetRenderStateFloat(u32 key, float value) = 0;
  virtual float GetRenderStateFloat(u32 key, float defaultValue = 0.0f) const = 0;
  virtual core::HResult SetTextureStageState(u32 stage, u32 key, u32 value) = 0;
  virtual u32 GetTextureStageState(u32 stage, u32 key) const = 0;
  virtual core::HResult SetSamplerState(u32 sampler, u32 key, u32 value) = 0;
  virtual u32 GetSamplerState(u32 sampler, u32 key) const = 0;
  virtual core::HResult SetTransform(u32 key, const core::Matrix4x4& matrix) = 0;
  virtual core::HResult SetLight(u32 index, const core::Light& light) = 0;
  virtual core::HResult LightEnable(u32 index, bool enable) = 0;
  virtual core::HResult SetMaterial(const core::Material& material) = 0;
  virtual core::HResult SetTexture(u32 stage, std::shared_ptr<core::Texture> texture) = 0;
  virtual core::HResult SetStreamSource(u32 stream, std::shared_ptr<core::Buffer> buffer, u32 offset,
                                        u32 stride) = 0;
  virtual core::HResult SetStreamSourceFreq(u32 stream, u32 frequency) = 0;
  virtual core::HResult SetIndices(std::shared_ptr<core::Buffer> buffer,
                                   core::IndexType indexType = core::IndexType::UInt16) = 0;
  virtual core::HResult SetFVF(u32 fvf) = 0;
  virtual core::HResult SetVertexDeclaration(std::vector<core::VertexElement> elements) = 0;
  virtual core::HResult SetVertexShader(const core::ShaderRef& shader) = 0;
  virtual core::HResult SetPixelShader(const core::ShaderRef& shader) = 0;
  virtual core::HResult SetClipPlane(u32 index, const core::ClipPlane& plane) = 0;
  virtual core::HResult SetViewport(const core::Viewport& viewport) = 0;
  virtual core::Viewport GetViewport() const = 0;
  virtual core::HResult SetScissorRect(const core::Rect& rect) = 0;
  virtual core::Rect GetScissorRect() const = 0;
  virtual core::HResult SetRenderTarget(u32 index, std::shared_ptr<core::Surface> surface) = 0;
  virtual core::HResult SetDepthStencilSurface(std::shared_ptr<core::Surface> surface) = 0;
  virtual std::shared_ptr<core::Buffer> CreateBuffer(const core::BufferDesc& desc) = 0;
  virtual std::shared_ptr<core::Texture> CreateTexture(const core::TextureDesc& desc) = 0;
  virtual std::shared_ptr<core::Surface> CreateSurface(const core::SurfaceDesc& desc) = 0;
  virtual std::shared_ptr<core::Query> CreateQuery(core::QueryType type) = 0;
  virtual std::shared_ptr<core::StateBlock> CreateStateBlock() = 0;
  virtual core::HResult CheckDeviceMultiSampleType(core::Format format, core::MultiSampleType type) const = 0;
  virtual core::HResult IssueQuery(const std::shared_ptr<core::Query>& query, bool begin) = 0;
  virtual core::HResult GetQueryData(const std::shared_ptr<core::Query>& query, void* output, size_t size,
                                     u32 flags) = 0;
  virtual core::HResult FillSurface(const std::shared_ptr<core::Surface>& surface, const core::Rect* rect,
                                    core::ColorRGBA color) = 0;
  virtual core::HResult StretchRect(const std::shared_ptr<core::Surface>& src, const core::Rect* srcRect,
                                    const std::shared_ptr<core::Surface>& dst, const core::Rect* dstRect,
                                    bool linear) = 0;
  virtual core::HResult UpdateSurface(const std::shared_ptr<core::Surface>& src,
                                      const core::Rect* srcRect,
                                      const std::shared_ptr<core::Surface>& dst,
                                      core::i32 dstX, core::i32 dstY) = 0;
  virtual core::HResult UpdateTexture(const std::shared_ptr<core::Texture>& src,
                                      const std::shared_ptr<core::Texture>& dst) = 0;
  virtual core::HResult GetRenderTargetData(const std::shared_ptr<core::Surface>& src,
                                            const std::shared_ptr<core::Surface>& dst) = 0;
};

class IDirect3DDevice9Ex : public IDirect3DDevice9 {
 public:
  virtual core::HResult CheckDeviceState(core::Handle destinationWindow) const = 0;
  virtual core::HResult ResetEx(const core::PresentParameters& params,
                                const DisplayModeEx* fullscreenMode = nullptr) = 0;
  virtual core::HResult PresentEx(const core::Rect* sourceRect = nullptr, const core::Rect* destRect = nullptr,
                                  core::Handle destinationWindowOverride = {}, const void* dirtyRegion = nullptr,
                                  u32 flags = 0) = 0;
  virtual core::HResult SetMaximumFrameLatency(u32 latency) = 0;
  virtual u32 GetMaximumFrameLatency() const = 0;
  virtual core::HResult WaitForVBlank(size_t swapChainIndex = 0) = 0;
  virtual core::HResult CheckResourceResidency(std::span<void* const> resources = {}) const = 0;
  virtual DisplayModeEx GetDisplayModeEx(size_t swapChainIndex = 0) const = 0;
  virtual core::HResult GetGPUThreadPriority(core::i32* priority) const = 0;
  virtual core::HResult SetGPUThreadPriority(core::i32 priority) = 0;
  virtual core::HResult SetConvolutionMonoKernel() = 0;
  virtual core::HResult ComposeRects() = 0;
  virtual std::shared_ptr<core::Surface> CreateRenderTargetEx(const core::SurfaceDesc& desc,
                                                              core::Handle* sharedHandle = nullptr) = 0;
  virtual std::shared_ptr<core::Surface> CreateOffscreenPlainSurfaceEx(const core::SurfaceDesc& desc,
                                                                       core::Handle* sharedHandle = nullptr) = 0;
  virtual std::shared_ptr<core::Surface> CreateDepthStencilSurfaceEx(const core::SurfaceDesc& desc,
                                                                      core::Handle* sharedHandle = nullptr) = 0;
};

// The upper-runtime dxmt9::Device is the consumer shape. Callers build the
// Device via dxmt9::CreateDXMT9Device() and hand it to the factory by value.
// See include/dxmt9/dxmt9_device.hpp.
IDirect3D9* Direct3DCreate9(u32 sdkVersion, std::unique_ptr<dxmt9::Device> device);
IDirect3D9Ex* Direct3DCreate9Ex(u32 sdkVersion, std::unique_ptr<dxmt9::Device> device);

// Test-only overloads: wrap an existing BackendDevice in a stub dxmt9::Device.
// Default-constructed variants build an empty factory (no devices enumerable
// until a dxmt9::Device is supplied).
IDirect3D9* Direct3DCreate9(u32 sdkVersion);
IDirect3D9Ex* Direct3DCreate9Ex(u32 sdkVersion);
IDirect3D9* Direct3DCreate9(u32 sdkVersion, std::shared_ptr<core::BackendDevice> backend);
IDirect3D9Ex* Direct3DCreate9Ex(u32 sdkVersion, std::shared_ptr<core::BackendDevice> backend);

}  // namespace dxmt9::com
