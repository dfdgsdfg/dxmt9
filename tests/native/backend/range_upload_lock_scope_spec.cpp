#include "../../../src/dxmt9/dxmt9_device.hpp"
#include "../../../src/dxmt9/dxmt9_resource_pool.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "../../../src/winemetal/Metal.hpp"

namespace {

using namespace std::chrono_literals;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

bool refinementEnabled() {
  const char* value = std::getenv("DXMT9_LOCK_SCOPE_REFINEMENT");
  return value && value[0] != '\0' && value[0] != '0';
}

struct MutexHolder {
  explicit MutexHolder(dxmt9::CommandQueue& queue) : queue_(queue) {
    thread_ = std::thread([this] {
      std::unique_lock queueLock(queue_.mutex_);
      {
        std::lock_guard stateLock(stateMutex_);
        locked_ = true;
      }
      stateCv_.notify_one();
      std::unique_lock stateLock(stateMutex_);
      stateCv_.wait(stateLock, [this] { return release_; });
    });

    std::unique_lock stateLock(stateMutex_);
    stateCv_.wait(stateLock, [this] { return locked_; });
  }

  ~MutexHolder() {
    {
      std::lock_guard stateLock(stateMutex_);
      release_ = true;
    }
    stateCv_.notify_one();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  dxmt9::CommandQueue& queue_;
  std::mutex stateMutex_;
  std::condition_variable stateCv_;
  bool locked_ = false;
  bool release_ = false;
  std::thread thread_;
};

void testProductionRangeUpload() {
  auto devices = WMT::CopyAllDevices();
  if (!devices || devices.count() == 0u) {
    std::cerr << "SKIP: no Metal device\n";
    std::exit(77);
  }

  auto upper = dxmt9::CreateDXMT9Device(
      dxmt9::DEVICE_DESC{.device = devices.object(0u)});
  if (!upper) {
    std::cerr << "SKIP: production dxmt9 device is unavailable\n";
    std::exit(77);
  }

  dxmt9::core::BufferDesc desc{};
  desc.size = 64u;
  desc.pool = dxmt9::core::Pool::Default;
  desc.usage = dxmt9::core::UsageDynamic;
  const auto handle = upper->createBuffer(desc);
  check(static_cast<bool>(handle), "production dynamic buffer is created");

  std::array<std::uint8_t, 64> initial{};
  initial.fill(0x1cu);
  upper->uploadBufferData(handle, initial);

  auto* record = upper->pool()->findBuffer(handle.value);
  check(record != nullptr, "production buffer record is reachable");
  check(record->shadow.size() == initial.size(),
        "production buffer shadow has the logical extent");

  const auto beforeRevision = record->contentRevision;

  const std::array<std::uint8_t, 8> patch = {0xa1u, 0xa2u, 0xa3u, 0xa4u,
                                              0xa5u, 0xa6u, 0xa7u, 0xa8u};
  auto uploadBytes = initial;
  std::copy(patch.begin(), patch.end(), uploadBytes.begin() + 16);
  if (refinementEnabled()) {
    bool completed = false;
    std::mutex completionMutex;
    std::condition_variable completionCv;
    std::thread uploader;
    bool completedWhileQueueWasHeld = false;
    {
      MutexHolder holder(upper->queue());
      uploader = std::thread([&] {
        upper->uploadBufferDataRange(handle, uploadBytes, 16u, patch.size());
        {
          std::lock_guard lock(completionMutex);
          completed = true;
        }
        completionCv.notify_one();
      });
      {
        std::unique_lock lock(completionMutex);
        completedWhileQueueWasHeld =
            completionCv.wait_for(lock, 2s, [&] { return completed; });
      }
    }
    uploader.join();
    check(completedWhileQueueWasHeld,
          "range upload completes while queue mutex is held elsewhere");
  } else {
    upper->uploadBufferDataRange(handle, uploadBytes, 16u, patch.size());
  }

  record = upper->pool()->findBuffer(handle.value);
  check(record != nullptr, "record survives the valid range upload");
  check(record->contents != nullptr, "real shared backing is required for the byte oracle");
  check(record->contentRevision == beforeRevision + 1u,
        "valid range upload bumps contentRevision exactly once");
  check(std::equal(patch.begin(), patch.end(), record->shadow.begin() + 16),
        "valid range upload patches the shadow at the requested offset");
  check(std::all_of(record->shadow.begin(), record->shadow.begin() + 16,
                    [](std::uint8_t value) { return value == 0x1cu; }),
        "valid range upload preserves the shadow prefix");
  check(std::all_of(record->shadow.begin() + 24, record->shadow.end(),
                    [](std::uint8_t value) { return value == 0x1cu; }),
        "valid range upload preserves the shadow suffix");
  if (record->contents) {
    const auto* contents = static_cast<const std::uint8_t*>(record->contents);
    check(std::equal(patch.begin(), patch.end(), contents + 16),
          "valid range upload patches the shared backing at the offset");
    check(std::all_of(contents, contents + 16,
                      [](std::uint8_t value) { return value == 0x1cu; }),
          "valid range upload preserves the backing prefix");
    check(std::all_of(contents + 24, contents + initial.size(),
                      [](std::uint8_t value) { return value == 0x1cu; }),
          "valid range upload preserves the backing suffix");
  }

  const auto beforeRejectedRevision = record->contentRevision;
  const auto beforeRejectedShadow = record->shadow;
  std::vector<std::uint8_t> beforeRejectedContents(initial.size());
  if (record->contents) {
    const auto* contents = static_cast<const std::uint8_t*>(record->contents);
    std::copy(contents, contents + beforeRejectedContents.size(),
              beforeRejectedContents.begin());
  }
  upper->uploadBufferDataRange(handle, uploadBytes, 63u, 2u);
  record = upper->pool()->findBuffer(handle.value);
  check(record->contentRevision == beforeRejectedRevision,
        "invalid range leaves contentRevision unchanged");
  check(record->shadow == beforeRejectedShadow,
        "invalid range leaves the CPU shadow unchanged");
  if (record->contents) {
    const auto* contents = static_cast<const std::uint8_t*>(record->contents);
    check(std::equal(beforeRejectedContents.begin(),
                     beforeRejectedContents.end(), contents),
          "invalid range leaves the shared backing unchanged");
  }
}

}  // namespace

int main() {
  try {
    testProductionRangeUpload();
  } catch (const TestFailure& failure) {
    std::cerr << "range_upload_lock_scope_spec failed: " << failure.what()
              << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& failure) {
    std::cerr << "range_upload_lock_scope_spec unexpected exception: "
              << failure.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
