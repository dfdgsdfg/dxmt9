#include "dxmt9/core.hpp"
#include "dxmt9/dxmt9_device.hpp"
#include "dxmt9/dxmt9_perf_counters.hpp"
#include "util/config/config.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <memory>

namespace dxmt9::core {

// Split from core_resources.cpp: Buffer class members and the corresponding
// Device factory/registration entry points. See core_resources.cpp for the
// shared anonymous-namespace helpers and Device cross-cutting state.

namespace {

// R-237.5 (present-pacing-bridge-crossing-decomposition.237): experimental
// candidate that routes a Default+Dynamic+DISCARD unlock through the range
// upload instead of the full upload. Default off (unset/"0"/empty), which is
// byte-identical current behavior; read once and cached, matching the
// dynamicBufferRenameEnabled()-style resolvers elsewhere in this codebase.
bool discardRangeUploadEnabled() noexcept {
  static const bool enabled =
      dxmt9::util::getenvFlag("DXMT9_DISCARD_RANGE_UPLOAD");
  return enabled;
}

} // namespace

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
  if (offset > std::numeric_limits<u64>::max() - size) {
    return {};
  }
  const u64 end = offset + size;
  if (end > std::numeric_limits<std::size_t>::max()) {
    return {};
  }
  if ((flags & UsageDiscard) != 0 && (desc_.usage & UsageDynamic) != 0) {
    if (std::max<u64>(size, desc_.size) >
        std::numeric_limits<std::size_t>::max()) {
      return {};
    }
    storage_.assign(static_cast<size_t>(std::max<u64>(size, desc_.size)), 0);
    offset = 0;
  } else if (storage_.size() < offset + size) {
    storage_.resize(static_cast<size_t>(end));
  }
  if (backend_ && handle_) {
    backend_->mapBuffer(handle_, flags);
  }
  locked_ = true;
  lockedOffset_ = offset;
  lockedSize_ = size;
  lockedFlags_ = flags;
  return {storage_.data() + offset, static_cast<u32>(size)};
}

void Buffer::unlock(bool upload) {
  if (upload && backend_ && handle_) {
    const bool exactNoOverwrite =
        locked_ && desc_.pool == Pool::Default &&
        (desc_.usage & UsageDynamic) != 0 &&
        (lockedFlags_ & UsageNoOverwrite) != 0 &&
        (lockedFlags_ & UsageDiscard) == 0 &&
        lockedOffset_ <= storage_.size() &&
        lockedSize_ <= storage_.size() - lockedOffset_;
    // R-237.5: env-gated second admitted range-upload contract. Unlike
    // exactNoOverwrite, this fires on a DISCARD lock; safe because the
    // Default+Dynamic backing was already rotated at lock time
    // (finalizeBufferMap, R-BACK-5.8), so the range write lands on the
    // freshly selected backing exactly as the full write does today.
    const bool discardRange =
        discardRangeUploadEnabled() &&
        locked_ && desc_.pool == Pool::Default &&
        (desc_.usage & UsageDynamic) != 0 &&
        (lockedFlags_ & UsageDiscard) != 0 &&
        lockedOffset_ <= storage_.size() &&
        lockedSize_ <= storage_.size() - lockedOffset_;
    if (exactNoOverwrite || discardRange) {
      const auto rangeStart = std::chrono::steady_clock::now();
      backend_->uploadBufferDataRange(
          handle_, storage_, lockedOffset_, lockedSize_);
      const auto rangeNs = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - rangeStart).count());
      dxmt9::perf::countD3D9BufferUploadRange(
          rangeNs, static_cast<std::uint64_t>(lockedSize_), discardRange);
    } else {
      const auto fullStart = std::chrono::steady_clock::now();
      backend_->uploadBufferData(handle_, storage_);
      const auto fullNs = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - fullStart).count());
      dxmt9::perf::countD3D9BufferUploadFull(
          fullNs, static_cast<std::uint64_t>(storage_.size()),
          static_cast<std::uint32_t>(desc_.pool));
    }
  }
  if (backend_ && handle_) {
    const auto unmapStart = std::chrono::steady_clock::now();
    backend_->unmapBuffer(handle_);
    const auto unmapNs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - unmapStart).count());
    dxmt9::perf::countD3D9BufferUnmap(unmapNs);
  }
  locked_ = false;
  lockedOffset_ = 0;
  lockedSize_ = 0;
  lockedFlags_ = 0;
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
