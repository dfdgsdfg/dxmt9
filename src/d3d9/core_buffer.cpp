#include "dxmt9/core.hpp"
#include "dxmt9/dxmt9_device.hpp"

#include <algorithm>
#include <memory>

namespace dxmt9::core {

// Split from core_resources.cpp: Buffer class members and the corresponding
// Device factory/registration entry points. See core_resources.cpp for the
// shared anonymous-namespace helpers and Device cross-cutting state.

Buffer::Buffer(std::shared_ptr<Device> owner, BufferHandle handle,
               BufferDesc desc)
    : owner_(std::move(owner)), handle_(handle), desc_(desc),
      storage_(static_cast<size_t>(desc.size)) {
  if (auto ownerPtr = owner_.lock()) {
    backend_ = ownerPtr->upperDevice();
  }
}

Buffer::~Buffer() { invalidate(); }

LockedRegion Buffer::lock(u64 offset, u64 size, u32 flags) {
  if (!valid_) {
    return {};
  }
  if ((flags & UsageDiscard) != 0 && (desc_.usage & UsageDynamic) != 0) {
    storage_.assign(static_cast<size_t>(std::max<u64>(size, desc_.size)), 0);
    offset = 0;
  } else if (storage_.size() < offset + size) {
    storage_.resize(static_cast<size_t>(offset + size));
  }
  if (backend_ && handle_) {
    backend_->mapBuffer(handle_, flags);
  }
  locked_ = true;
  return {storage_.data() + offset, static_cast<u32>(size)};
}

void Buffer::unlock(bool upload) {
  if (upload && backend_ && handle_) {
    backend_->uploadBufferData(handle_, storage_);
  }
  if (backend_ && handle_) {
    backend_->unmapBuffer(handle_);
  }
  locked_ = false;
}

void Buffer::invalidate() {
  if (!valid_) {
    return;
  }
  valid_ = false;
  if (backend_ && handle_) {
    backend_->destroyBuffer(handle_);
  }
  handle_ = {};
}

std::shared_ptr<Buffer> Device::createBuffer(const BufferDesc &desc) {
  auto handle =
      upperDevice_ ? upperDevice_->createBuffer(desc) : BufferHandle{};
  if (!handle) {
    handle = Handle{nextHandle_++};
  }
  auto buffer = std::make_shared<Buffer>(shared_from_this(), handle, desc);
  registerBuffer(buffer);
  return buffer;
}

std::shared_ptr<Buffer> Device::openSharedBuffer(
    const std::shared_ptr<Buffer>& source) {
  if (!source || !source->valid()) {
    return {};
  }
  if (source->device().get() == this) {
    return source;
  }
  if (!upperDevice_ || !source->backend_) {
    return {};
  }
  dxmt9::SharedBufferBacking backing;
  if (!source->backend_->exportSharedBuffer(source->handle(), backing)) {
    return {};
  }
  const auto handle = upperDevice_->importSharedBuffer(source->desc(), backing);
  if (!handle) {
    return {};
  }
  auto buffer = std::make_shared<Buffer>(shared_from_this(), handle, source->desc());
  registerBuffer(buffer);
  return buffer;
}

void Device::registerBuffer(const std::shared_ptr<Buffer> &buffer) {
  buffers_.push_back(buffer);
}

} // namespace dxmt9::core
