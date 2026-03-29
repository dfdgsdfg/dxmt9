#pragma once

#include "dxmt9/core.hpp"

#include <atomic>

namespace dxmt9::com {

using core::HResult;
using core::u32;

inline constexpr u32 D3D_SDK_VERSION = 32;

enum class InterfaceId {
  IUnknown,
  Direct3D9,
  Direct3DDevice9,
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
                                        std::span<const core::u8> vertexData) = 0;
  virtual core::HResult DrawIndexedPrimitiveUP(core::PrimitiveType type, u32 primitiveCount,
                                               std::span<const core::u8> vertexData,
                                               std::span<const core::u8> indexData,
                                               core::IndexType indexType) = 0;
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
  virtual core::HResult SetIndices(std::shared_ptr<core::Buffer> buffer,
                                   core::IndexType indexType = core::IndexType::UInt16) = 0;
  virtual core::HResult SetFVF(u32 fvf) = 0;
  virtual core::HResult SetVertexDeclaration(std::vector<core::VertexElement> elements) = 0;
  virtual core::HResult SetVertexShader(const core::ShaderRef& shader) = 0;
  virtual core::HResult SetPixelShader(const core::ShaderRef& shader) = 0;
  virtual core::HResult SetClipPlane(u32 index, const core::ClipPlane& plane) = 0;
  virtual core::HResult SetViewport(const core::Viewport& viewport) = 0;
  virtual core::HResult SetScissorRect(const core::Rect& rect) = 0;
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
                                     u32 flags) const = 0;
  virtual core::HResult FillSurface(const std::shared_ptr<core::Surface>& surface, const core::Rect* rect,
                                    core::ColorRGBA color) = 0;
  virtual core::HResult StretchRect(const std::shared_ptr<core::Surface>& src, const core::Rect* srcRect,
                                    const std::shared_ptr<core::Surface>& dst, const core::Rect* dstRect,
                                    bool linear) = 0;
  virtual core::HResult UpdateSurface(const std::shared_ptr<core::Surface>& src,
                                      const std::shared_ptr<core::Surface>& dst) = 0;
  virtual core::HResult UpdateTexture(const std::shared_ptr<core::Texture>& src,
                                      const std::shared_ptr<core::Texture>& dst) = 0;
  virtual core::HResult GetRenderTargetData(const std::shared_ptr<core::Surface>& src,
                                            const std::shared_ptr<core::Surface>& dst) = 0;
};

IDirect3D9* Direct3DCreate9(u32 sdkVersion);

}  // namespace dxmt9::com
