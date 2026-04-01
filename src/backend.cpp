#include "dxmt9/core.hpp"
#include "dxmt9/assert.hpp"

#include <algorithm>
#include <deque>
#include <unordered_map>

namespace dxmt9::core {

#if defined(__APPLE__)
std::shared_ptr<BackendDevice> makeMetalBackendDevice(const BackendLimits& limits);
#endif

namespace {

enum class CommandKind {
  Draw,
  Clear,
  SurfaceCopy,
  StretchRect,
  Readback,
  ColorFill,
  Present,
};

struct CommandRecord {
  CommandKind kind = CommandKind::Draw;
  DrawDesc draw{};
  ClearDesc clear{};
  SurfaceCopyDesc surfaceCopy{};
  StretchRectDesc stretchRect{};
  ReadbackDesc readback{};
  ColorFillDesc colorFill{};
  SwapDesc present{};
};

struct BufferRecord {
  BufferDesc desc{};
  std::vector<u8> storage;
};

struct TextureRecord {
  TextureDesc desc{};
};

struct SurfaceRecord {
  SurfaceDesc desc{};
  TextureHandle texture{};
  u32 level = 0;
};

class SimBackendDevice final : public BackendDevice {
 public:
  void setDeviceLostObserver(DeviceLostObserver observer) override {
    deviceLostObserver_ = std::move(observer);
  }

  void setPresentationStatusObserver(PresentationStatusObserver observer) override {
    presentationStatusObserver_ = std::move(observer);
  }

  void setMaxFrameLatency(u32 latency) override {
    maxFrameLatency_ = std::clamp(latency, 1u, 3u);
  }

  HResult waitForVBlank(const SwapDesc& desc) override {
    (void)desc;
    flush();
    return D3D_OK;
  }

  BufferHandle createBuffer(const BufferDesc& desc) override {
    const Handle handle{nextHandle_++};
    BufferRecord record;
    record.desc = desc;
    record.storage.resize(static_cast<size_t>(desc.size));
    buffers_[handle.value] = std::move(record);
    return handle;
  }

  TextureHandle createTexture(const TextureDesc& desc) override {
    const Handle handle{nextHandle_++};
    TextureRecord record;
    record.desc = desc;
    textures_[handle.value] = std::move(record);
    return handle;
  }

  SurfaceHandle createSurface(const SurfaceDesc& desc) override {
    const Handle handle{nextHandle_++};
    SurfaceRecord record;
    record.desc = desc;
    surfaces_[handle.value] = std::move(record);
    return handle;
  }

  SurfaceHandle createSurfaceForTexture(TextureHandle texture, u32 level, const SurfaceDesc& desc) override {
    const Handle handle{nextHandle_++};
    SurfaceRecord record;
    record.desc = desc;
    record.texture = texture;
    record.level = level;
    surfaces_[handle.value] = std::move(record);
    return handle;
  }

  void destroyBuffer(BufferHandle handle) override {
    buffers_.erase(handle.value);
  }

  void destroyTexture(TextureHandle handle) override {
    textures_.erase(handle.value);
  }

  void destroySurface(SurfaceHandle handle) override {
    surfaces_.erase(handle.value);
  }

  void* mapBuffer(BufferHandle handle, u32 flags) override {
    auto it = buffers_.find(handle.value);
    if (it == buffers_.end()) {
      return nullptr;
    }
    if ((flags & UsageDiscard) != 0) {
      it->second.storage.assign(static_cast<size_t>(it->second.desc.size), 0);
    } else if (it->second.storage.size() < it->second.desc.size) {
      it->second.storage.resize(static_cast<size_t>(it->second.desc.size), 0);
    }
    if (it->second.storage.empty()) {
      return nullptr;
    }
    return it->second.storage.data();
  }

  void unmapBuffer(BufferHandle) override {}

  void uploadBufferData(BufferHandle handle, std::span<const u8> bytes) override {
    auto it = buffers_.find(handle.value);
    if (it == buffers_.end()) {
      return;
    }
    it->second.storage.assign(bytes.begin(), bytes.end());
  }

  void uploadTextureLevel(TextureHandle, u32, u32, u32, u32, std::span<const u8>) override {}

  void submitDraw(const DrawDesc& desc) override {
    CommandRecord record;
    record.kind = CommandKind::Draw;
    record.draw = desc;
    pending_.push_back(std::move(record));
  }

  void submitClear(const ClearDesc& desc) override {
    CommandRecord record;
    record.kind = CommandKind::Clear;
    record.clear = desc;
    pending_.push_back(std::move(record));
  }

  void submitSurfaceCopy(const SurfaceCopyDesc& desc) override {
    CommandRecord record;
    record.kind = CommandKind::SurfaceCopy;
    record.surfaceCopy = desc;
    pending_.push_back(std::move(record));
  }

  void submitStretchRect(const StretchRectDesc& desc) override {
    CommandRecord record;
    record.kind = CommandKind::StretchRect;
    record.stretchRect = desc;
    pending_.push_back(std::move(record));
  }

  void submitReadback(const ReadbackDesc& desc) override {
    CommandRecord record;
    record.kind = CommandKind::Readback;
    record.readback = desc;
    pending_.push_back(std::move(record));
  }

  void submitColorFill(const ColorFillDesc& desc) override {
    CommandRecord record;
    record.kind = CommandKind::ColorFill;
    record.colorFill = desc;
    pending_.push_back(std::move(record));
  }

  void present(const SwapDesc& desc) override {
    CommandRecord record;
    record.kind = CommandKind::Present;
    record.present = desc;
    pending_.push_back(std::move(record));
    if (presentationStatusObserver_) {
      presentationStatusObserver_(false);
    }
    commitPendingFrame();
  }

  void flush() override {
    commitPendingFrame();
  }

 private:
  void commitPendingFrame() {
    if (pending_.empty()) {
      return;
    }
    committedFrames_.push_back(std::move(pending_));
    pending_.clear();
    while (committedFrames_.size() > maxFrameLatency_) {
      committedFrames_.pop_front();
    }
    // BoundedInflight: the simulated backend never keeps more frames in flight
    // than the configured frame-latency ceiling.
    DXMT_ASSERT(committedFrames_.size() <= maxFrameLatency_);
  }

  u64 nextHandle_ = 1;
  u32 maxFrameLatency_ = 3;
  std::unordered_map<u64, BufferRecord> buffers_;
  std::unordered_map<u64, TextureRecord> textures_;
  std::unordered_map<u64, SurfaceRecord> surfaces_;
  DeviceLostObserver deviceLostObserver_;
  PresentationStatusObserver presentationStatusObserver_;
  std::vector<CommandRecord> pending_;
  std::deque<std::vector<CommandRecord>> committedFrames_;
};

}  // namespace

std::shared_ptr<BackendDevice> makeBackendDevice(const BackendLimits& limits) {
  (void)limits;
#if defined(__APPLE__)
  if (auto backend = makeMetalBackendDevice(limits)) {
    return backend;
  }
#endif
  return makeSimBackendDevice();
}

std::shared_ptr<BackendDevice> makeSimBackendDevice() {
  return std::make_shared<SimBackendDevice>();
}

}  // namespace dxmt9::core
