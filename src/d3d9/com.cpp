#include "dxmt9/com.hpp"

namespace dxmt9::com {

namespace {

template <typename Derived>
class RefCounted {
 protected:
  u32 addRef() { return ++refCount_; }

  u32 release() {
    const u32 next = --refCount_;
    if (next == 0) {
      delete static_cast<Derived*>(this);
    }
    return next;
  }

 private:
  std::atomic<u32> refCount_{1};
};

core::SwapDesc makeSwapDesc(const core::PresentParameters& params) {
  core::SwapDesc desc;
  desc.window = params.deviceWindow;
  desc.width = params.backBufferWidth;
  desc.height = params.backBufferHeight;
  desc.format = params.backBufferFormat;
  desc.interval = params.presentationInterval;
  desc.windowed = params.windowed;
  desc.displaySyncEnabled = params.presentationInterval != core::PresentInterval::Immediate;
  desc.multiSampleType = params.multiSampleType;
  return desc;
}

core::PresentParameters applyFullscreenMode(core::PresentParameters params, const core::DisplayModeEx* fullscreenMode) {
  if (!fullscreenMode) {
    return params;
  }
  params.windowed = false;
  if (fullscreenMode->width != 0) {
    params.backBufferWidth = fullscreenMode->width;
  }
  if (fullscreenMode->height != 0) {
    params.backBufferHeight = fullscreenMode->height;
  }
  if (fullscreenMode->format != core::Format::Unknown) {
    params.backBufferFormat = fullscreenMode->format;
  }
  return params;
}

core::DisplayModeEx makeDisplayModeEx(const core::DisplayMode& mode) {
  return {mode.width, mode.height, mode.refreshRate, mode.format, core::DisplayScanLineOrdering::Progressive};
}

core::Luid makeLuid(const core::AdapterInfo& adapter) {
  const core::u64 raw = adapter.registryId != 0 ? adapter.registryId : static_cast<core::u64>(adapter.ordinal + 1);
  return {static_cast<core::u32>(raw & 0xffffffffu), static_cast<core::i32>(raw >> 32)};
}

class Direct3DSwapChain9Impl final : public IDirect3DSwapChain9, public RefCounted<Direct3DSwapChain9Impl> {
 public:
  Direct3DSwapChain9Impl(std::shared_ptr<core::Device> device, std::shared_ptr<core::SwapChain> swapChain)
      : device_(std::move(device)), swapChain_(std::move(swapChain)) {}

  u32 AddRef() override { return this->addRef(); }

  u32 Release() override { return this->release(); }

  bool QueryInterface(InterfaceId iid, void** object) override {
    if (!object) {
      return false;
    }
    switch (iid) {
      case InterfaceId::IUnknown:
      case InterfaceId::Direct3DSwapChain9:
        *object = static_cast<IDirect3DSwapChain9*>(this);
        AddRef();
        return true;
      default:
        *object = nullptr;
        return false;
    }
  }

  core::SwapChain& coreSwapChain() override { return *swapChain_; }
  const core::SwapChain& coreSwapChain() const override { return *swapChain_; }
  const core::PresentParameters& presentParameters() const override { return swapChain_->params(); }
  std::shared_ptr<core::Surface> backBuffer() const override { return swapChain_->backBuffer(); }
  std::shared_ptr<core::Surface> depthStencilSurface() const override { return swapChain_->depthStencilSurface(); }

  core::HResult Present() override {
    if (!device_ || !swapChain_) {
      return core::D3DERR_INVALIDCALL;
    }
    return swapChain_->present(device_->backend(), makeSwapDesc(swapChain_->params()));
  }

 private:
  std::shared_ptr<core::Device> device_;
  std::shared_ptr<core::SwapChain> swapChain_;
};

class Direct3DDevice9Impl final : public IDirect3DDevice9Ex, public RefCounted<Direct3DDevice9Impl> {
 public:
  explicit Direct3DDevice9Impl(std::shared_ptr<core::Device> device, bool exSupported = false)
      : device_(std::move(device)), exSupported_(exSupported) {}

  u32 AddRef() override { return this->addRef(); }

  u32 Release() override { return this->release(); }

  bool QueryInterface(InterfaceId iid, void** object) override {
    if (!object) {
      return false;
    }
    switch (iid) {
      case InterfaceId::IUnknown:
      case InterfaceId::Direct3DDevice9:
        *object = static_cast<IDirect3DDevice9*>(this);
        AddRef();
        return true;
      case InterfaceId::Direct3DDevice9Ex:
        if (exSupported_) {
          *object = static_cast<IDirect3DDevice9Ex*>(this);
          AddRef();
          return true;
        }
        [[fallthrough]];
      default:
        *object = nullptr;
        return false;
    }
  }

  core::Device& coreDevice() override { return *device_; }
  const core::Device& coreDevice() const override { return *device_; }

  const core::DeviceCaps& GetDeviceCaps() const override { return device_->caps(); }
  core::HResult TestCooperativeLevel() const override { return device_->testCooperativeLevel(); }
  core::HResult Reset(const core::PresentParameters& params) override { return device_->reset(params); }

  IDirect3DSwapChain9* CreateAdditionalSwapChain(const core::PresentParameters& params) override {
    auto swapChain = device_->createAdditionalSwapChain(params);
    if (!swapChain) {
      return nullptr;
    }
    return new Direct3DSwapChain9Impl(device_, std::move(swapChain));
  }

  size_t GetSwapChainCount() const override { return device_->swapChainCount(); }

  IDirect3DSwapChain9* GetSwapChain(size_t index = 0) const override {
    auto swapChain = device_->swapChain(index);
    if (!swapChain) {
      return nullptr;
    }
    return new Direct3DSwapChain9Impl(device_, std::move(swapChain));
  }

  core::HResult Present() override { return device_->present(); }
  core::HResult BeginScene() override { return device_->beginScene(); }
  core::HResult EndScene() override { return device_->endScene(); }
  core::HResult Clear(const core::ClearDesc& desc) override { return device_->clear(desc); }
  core::HResult DrawPrimitive(core::PrimitiveType type, u32 primitiveCount, u32 startVertex) override {
    return device_->drawPrimitive(type, primitiveCount, startVertex);
  }
  core::HResult DrawIndexedPrimitive(core::PrimitiveType type, u32 primitiveCount, u32 startVertex,
                                     core::i32 baseVertexIndex, u32 startIndex,
                                     core::IndexType indexType) override {
    (void)indexType;
    return device_->drawIndexedPrimitive(type, primitiveCount, startVertex, baseVertexIndex, startIndex,
                                         indexType);
  }
  core::HResult DrawPrimitiveUP(core::PrimitiveType type, u32 primitiveCount,
                                std::span<const core::u8> vertexData) override {
    return device_->drawPrimitiveUP(type, primitiveCount, vertexData);
  }
  core::HResult DrawIndexedPrimitiveUP(core::PrimitiveType type, u32 primitiveCount,
                                       std::span<const core::u8> vertexData,
                                       std::span<const core::u8> indexData,
                                       core::IndexType indexType) override {
    return device_->drawIndexedPrimitiveUP(type, primitiveCount, vertexData, indexData, indexType);
  }
  core::HResult SetRenderState(u32 key, u32 value) override { return device_->setRenderState(key, value); }
  u32 GetRenderState(u32 key) const override { return device_->getRenderState(key); }
  core::HResult SetRenderStateFloat(u32 key, float value) override {
    return device_->setRenderStateFloat(key, value);
  }
  float GetRenderStateFloat(u32 key, float defaultValue) const override {
    return device_->getRenderStateFloat(key, defaultValue);
  }
  core::HResult SetTextureStageState(u32 stage, u32 key, u32 value) override {
    return device_->setTextureStageState(stage, key, value);
  }
  u32 GetTextureStageState(u32 stage, u32 key) const override {
    return device_->getTextureStageState(stage, key);
  }
  core::HResult SetSamplerState(u32 sampler, u32 key, u32 value) override {
    return device_->setSamplerState(sampler, key, value);
  }
  u32 GetSamplerState(u32 sampler, u32 key) const override {
    return device_->getSamplerState(sampler, key);
  }
  core::HResult SetTransform(u32 key, const core::Matrix4x4& matrix) override {
    return device_->setTransform(key, matrix);
  }
  core::HResult SetLight(u32 index, const core::Light& light) override { return device_->setLight(index, light); }
  core::HResult LightEnable(u32 index, bool enable) override { return device_->lightEnable(index, enable); }
  core::HResult SetMaterial(const core::Material& material) override { return device_->setMaterial(material); }
  core::HResult SetTexture(u32 stage, std::shared_ptr<core::Texture> texture) override {
    return device_->setTexture(stage, std::move(texture));
  }
  core::HResult SetStreamSource(u32 stream, std::shared_ptr<core::Buffer> buffer, u32 offset,
                                u32 stride) override {
    return device_->setStreamSource(stream, std::move(buffer), offset, stride);
  }
  core::HResult SetIndices(std::shared_ptr<core::Buffer> buffer, core::IndexType indexType) override {
    return device_->setIndices(std::move(buffer), indexType);
  }
  core::HResult SetFVF(u32 fvf) override { return device_->setFVF(fvf); }
  core::HResult SetVertexDeclaration(std::vector<core::VertexElement> elements) override {
    return device_->setVertexDeclaration(std::move(elements));
  }
  core::HResult SetVertexShader(const core::ShaderRef& shader) override { return device_->setVertexShader(shader); }
  core::HResult SetPixelShader(const core::ShaderRef& shader) override { return device_->setPixelShader(shader); }
  core::HResult SetClipPlane(u32 index, const core::ClipPlane& plane) override {
    return device_->setClipPlane(index, plane);
  }
  core::HResult SetViewport(const core::Viewport& viewport) override { return device_->setViewport(viewport); }
  core::Viewport GetViewport() const override { return device_->viewport(); }
  core::HResult SetScissorRect(const core::Rect& rect) override { return device_->setScissorRect(rect); }
  core::Rect GetScissorRect() const override { return device_->scissorRect(); }
  core::HResult SetRenderTarget(u32 index, std::shared_ptr<core::Surface> surface) override {
    return device_->setRenderTarget(index, std::move(surface));
  }
  core::HResult SetDepthStencilSurface(std::shared_ptr<core::Surface> surface) override {
    return device_->setDepthStencilSurface(std::move(surface));
  }
  std::shared_ptr<core::Buffer> CreateBuffer(const core::BufferDesc& desc) override {
    return device_->createBuffer(desc);
  }
  std::shared_ptr<core::Texture> CreateTexture(const core::TextureDesc& desc) override {
    return device_->createTexture(desc);
  }
  std::shared_ptr<core::Surface> CreateSurface(const core::SurfaceDesc& desc) override {
    return device_->createSurface(desc);
  }
  std::shared_ptr<core::Query> CreateQuery(core::QueryType type) override { return device_->createQuery(type); }
  std::shared_ptr<core::StateBlock> CreateStateBlock() override { return device_->createStateBlock(); }
  core::HResult CheckDeviceMultiSampleType(core::Format format, core::MultiSampleType type) const override {
    return device_->checkDeviceMultiSampleType(format, type);
  }
  core::HResult IssueQuery(const std::shared_ptr<core::Query>& query, bool begin) override {
    return device_->issueQuery(query, begin);
  }
  core::HResult GetQueryData(const std::shared_ptr<core::Query>& query, void* output, size_t size,
                             u32 flags) const override {
    return device_->getQueryData(query, output, size, flags);
  }
  core::HResult FillSurface(const std::shared_ptr<core::Surface>& surface, const core::Rect* rect,
                            core::ColorRGBA color) override {
    return device_->fillSurface(surface, rect, color);
  }
  core::HResult StretchRect(const std::shared_ptr<core::Surface>& src, const core::Rect* srcRect,
                            const std::shared_ptr<core::Surface>& dst, const core::Rect* dstRect,
                            bool linear) override {
    return device_->stretchRect(src, srcRect, dst, dstRect, linear);
  }
  core::HResult UpdateSurface(const std::shared_ptr<core::Surface>& src,
                              const std::shared_ptr<core::Surface>& dst) override {
    return device_->updateSurface(src, dst);
  }
  core::HResult UpdateTexture(const std::shared_ptr<core::Texture>& src,
                              const std::shared_ptr<core::Texture>& dst) override {
    return device_->updateTexture(src, dst);
  }
  core::HResult GetRenderTargetData(const std::shared_ptr<core::Surface>& src,
                                    const std::shared_ptr<core::Surface>& dst) override {
    return device_->getRenderTargetData(src, dst);
  }

  core::HResult CheckDeviceState(core::Handle destinationWindow) const override {
    (void)destinationWindow;
    return device_->checkDeviceState();
  }

  core::HResult ResetEx(const core::PresentParameters& params,
                        const DisplayModeEx* fullscreenMode = nullptr) override {
    return device_->resetEx(params, fullscreenMode);
  }

  core::HResult PresentEx(const core::Rect* sourceRect = nullptr, const core::Rect* destRect = nullptr,
                          core::Handle destinationWindowOverride = {}, const void* dirtyRegion = nullptr,
                          u32 flags = 0) override {
    return device_->presentEx(sourceRect, destRect, destinationWindowOverride, dirtyRegion, flags);
  }

  core::HResult SetMaximumFrameLatency(u32 latency) override { return device_->setMaximumFrameLatency(latency); }

  u32 GetMaximumFrameLatency() const override { return device_->maximumFrameLatency(); }

  core::HResult WaitForVBlank(size_t swapChainIndex = 0) override {
    return device_->waitForVBlank(swapChainIndex);
  }

  core::HResult CheckResourceResidency(std::span<void* const> resources = {}) const override {
    return device_->checkResourceResidency(resources);
  }

  DisplayModeEx GetDisplayModeEx(size_t swapChainIndex = 0) const override {
    return device_->getDisplayModeEx(swapChainIndex);
  }

  core::HResult GetGPUThreadPriority(core::i32* priority) const override {
    return device_->getGPUThreadPriority(priority);
  }

  core::HResult SetGPUThreadPriority(core::i32 priority) override {
    return device_->setGPUThreadPriority(priority);
  }

  core::HResult SetConvolutionMonoKernel() override { return device_->setConvolutionMonoKernel(); }

  core::HResult ComposeRects() override { return device_->composeRects(); }

  std::shared_ptr<core::Surface> CreateRenderTargetEx(const core::SurfaceDesc& desc,
                                                      core::Handle* sharedHandle = nullptr) override {
    if (sharedHandle) {
      *sharedHandle = {};
    }
    auto rtDesc = desc;
    rtDesc.renderTarget = true;
    rtDesc.depthStencil = false;
    rtDesc.usage |= core::UsageRenderTarget;
    return device_->createSurface(rtDesc);
  }

  std::shared_ptr<core::Surface> CreateOffscreenPlainSurfaceEx(const core::SurfaceDesc& desc,
                                                               core::Handle* sharedHandle = nullptr) override {
    if (sharedHandle) {
      *sharedHandle = {};
    }
    auto surfaceDesc = desc;
    surfaceDesc.renderTarget = false;
    surfaceDesc.depthStencil = false;
    return device_->createSurface(surfaceDesc);
  }

  std::shared_ptr<core::Surface> CreateDepthStencilSurfaceEx(const core::SurfaceDesc& desc,
                                                             core::Handle* sharedHandle = nullptr) override {
    if (sharedHandle) {
      *sharedHandle = {};
    }
    auto dsDesc = desc;
    dsDesc.renderTarget = false;
    dsDesc.depthStencil = true;
    dsDesc.usage |= core::UsageDepthStencil;
    return device_->createSurface(dsDesc);
  }

 private:
  std::shared_ptr<core::Device> device_;
  bool exSupported_ = false;
};

class Direct3D9Impl final : public IDirect3D9Ex, public RefCounted<Direct3D9Impl> {
 public:
  explicit Direct3D9Impl(core::BackendLimits limits = {}, std::shared_ptr<core::BackendDevice> backend = {},
                         bool exSupported = false)
      : factory_(limits, std::move(backend)), exSupported_(exSupported) {}

  u32 AddRef() override { return this->addRef(); }

  u32 Release() override { return this->release(); }

  bool QueryInterface(InterfaceId iid, void** object) override {
    if (!object) {
      return false;
    }
    switch (iid) {
      case InterfaceId::IUnknown:
      case InterfaceId::Direct3D9:
        *object = static_cast<IDirect3D9*>(this);
        AddRef();
        return true;
      case InterfaceId::Direct3D9Ex:
        if (exSupported_) {
          *object = static_cast<IDirect3D9Ex*>(this);
          AddRef();
          return true;
        }
        [[fallthrough]];
      default:
        *object = nullptr;
        return false;
    }
  }

  size_t GetAdapterCount() const override { return factory_.adapterCount(); }
  const core::DeviceCaps& GetDeviceCaps(size_t adapterIndex) const override { return factory_.caps(adapterIndex); }
  core::AdapterIdentifier GetAdapterIdentifier(size_t adapterIndex) const override {
    return factory_.getAdapterIdentifier(adapterIndex);
  }
  std::vector<core::DisplayMode> EnumAdapterModes(size_t adapterIndex, core::Format format) const override {
    return factory_.enumAdapterModes(adapterIndex, format);
  }
  core::DisplayMode GetAdapterDisplayMode(size_t adapterIndex) const override {
    return factory_.getAdapterDisplayMode(adapterIndex);
  }
  u32 GetAdapterMonitor(size_t adapterIndex) const override { return factory_.getAdapterMonitor(adapterIndex); }
  core::HResult CheckDeviceType(size_t adapterIndex, core::DeviceType deviceType, core::Format adapterFormat,
                                core::Format backBufferFormat, bool windowed) const override {
    return factory_.checkDeviceType(adapterIndex, deviceType, adapterFormat, backBufferFormat, windowed);
  }
  core::HResult CheckDeviceFormat(size_t adapterIndex, core::Format format, u32 usage) const override {
    return factory_.checkDeviceFormat(adapterIndex, format, usage);
  }
  core::HResult CheckDeviceMultiSampleType(size_t adapterIndex, core::Format format,
                                           core::MultiSampleType type) const override {
    return factory_.checkDeviceMultiSampleType(adapterIndex, format, type);
  }

  IDirect3DDevice9* CreateDevice(size_t adapterIndex, const core::PresentParameters& params,
                                 u32 behaviorFlags = 0) override {
    return CreateDeviceEx(adapterIndex, params, nullptr, behaviorFlags);
  }

  size_t GetAdapterModeCountEx(size_t adapterIndex, const DisplayModeFilter* filter = nullptr) const override {
    if (adapterIndex >= factory_.adapterCount()) {
      return 0;
    }
    const auto format = filter && filter->format != core::Format::Unknown ? filter->format
                                                                           : factory_.getAdapterDisplayMode(adapterIndex).format;
    return factory_.enumAdapterModes(adapterIndex, format).size();
  }

  bool EnumAdapterModesEx(size_t adapterIndex, const DisplayModeFilter* filter, size_t modeIndex,
                          DisplayModeEx* mode) const override {
    if (!mode) {
      return false;
    }
    if (adapterIndex >= factory_.adapterCount()) {
      return false;
    }
    const auto format = filter && filter->format != core::Format::Unknown ? filter->format
                                                                           : factory_.getAdapterDisplayMode(adapterIndex).format;
    const auto modes = factory_.enumAdapterModes(adapterIndex, format);
    if (modeIndex >= modes.size()) {
      return false;
    }
    *mode = makeDisplayModeEx(modes[modeIndex]);
    return true;
  }

  bool GetAdapterDisplayModeEx(size_t adapterIndex, DisplayModeEx* mode, DisplayRotation* rotation) const override {
    if (!mode) {
      return false;
    }
    if (adapterIndex >= factory_.adapterCount()) {
      return false;
    }
    *mode = makeDisplayModeEx(factory_.getAdapterDisplayMode(adapterIndex));
    if (rotation) {
      *rotation = DisplayRotation::Identity;
    }
    return true;
  }

  bool GetAdapterLUID(size_t adapterIndex, Luid* luid) const override {
    if (!luid) {
      return false;
    }
    if (adapterIndex >= factory_.adapterCount()) {
      return false;
    }
    *luid = makeLuid(factory_.adapter(adapterIndex));
    return true;
  }

  IDirect3DDevice9Ex* CreateDeviceEx(size_t adapterIndex, const core::PresentParameters& params,
                                     const DisplayModeEx* fullscreenMode = nullptr,
                                     u32 behaviorFlags = 0) override {
    auto adjusted = applyFullscreenMode(params, fullscreenMode);
    auto device = factory_.createDevice(adapterIndex, adjusted, behaviorFlags);
    if (!device) {
      return nullptr;
    }
    return new Direct3DDevice9Impl(std::move(device), exSupported_);
  }

 private:
  core::Factory factory_;
  bool exSupported_ = false;
};

}  // namespace

IDirect3D9* Direct3DCreate9(u32 sdkVersion, std::shared_ptr<core::BackendDevice> backend) {
  if (sdkVersion != D3D_SDK_VERSION) {
    return nullptr;
  }
  return new Direct3D9Impl({}, std::move(backend), false);
}

IDirect3D9Ex* Direct3DCreate9Ex(u32 sdkVersion, std::shared_ptr<core::BackendDevice> backend) {
  if (sdkVersion != D3D_SDK_VERSION) {
    return nullptr;
  }
  return new Direct3D9Impl({}, std::move(backend), true);
}

}  // namespace dxmt9::com
