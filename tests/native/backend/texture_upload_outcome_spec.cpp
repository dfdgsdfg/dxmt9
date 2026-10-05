#include "dxmt9/dxmt9_texture_upload_outcome.hpp"
#include "dxmt9/dxmt9_command_queue.hpp"
#include "dxmt9/dxmt9_resource_initializer.hpp"
#include "dxmt9/query_upload_tracker.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace dxmt9 {
struct SchedulingProgressTestAccess {
  static void disableQueryTracking(resources::Initializer& initializer) {
    initializer.queryTrackingEnabled_ = false;
  }
};
} // namespace dxmt9

namespace {
using namespace dxmt9::resources;
using Outcome = TextureUploadStageOutcome;
using Type = dxmt9::core::TextureType;

void check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

void stageOutcomeTruthTable() {
  const TextureUploadStageFacts shared{true, true, true, true, false, false};
  auto privateQueued = shared;
  privateQueued.stagingRequired = privateQueued.stagingProduced = true;
  check(textureUploadStageOutcome(shared) == Outcome::ImmediateShared,
      "valid Shared nullopt represents the real immediate producer effect");
  check(textureUploadStageOutcome(privateQueued) == Outcome::DeferredStaging,
      "valid Private staging receipt remains owned until queue completion");
  for (auto facts : {shared, privateQueued}) {
    auto absent = facts;
    absent.destinationPresent = false;
    check(textureUploadStageOutcome(absent) == Outcome::Failed,
        "a missing logical destination cannot authenticate bytes");
    absent = facts;
    absent.physicalBackingPresent = false;
    check(textureUploadStageOutcome(absent) == Outcome::Failed,
        "a wrapper without Metal backing is not an upload destination");
    absent = facts;
    absent.subresourceValid = false;
    check(textureUploadStageOutcome(absent) == Outcome::Failed,
        "an invalid flattened subresource fails rather than posing as Shared success");
  }
  auto allocationFailed = privateQueued;
  allocationFailed.stagingProduced = false;
  check(textureUploadStageOutcome(allocationFailed) == Outcome::Failed,
      "failed staging allocation is not the Shared nullopt success branch");
  auto inconsistent = shared;
  inconsistent.stagingProduced = true;
  check(textureUploadStageOutcome(inconsistent) == Outcome::Failed,
      "an incompatible producer result cannot be relabeled a queued receipt");
  for (const bool privateBacking : {false, true}) {
    auto empty = shared;
    empty.sourceNonempty = false;
    empty.stagingRequired = privateBacking;
    check(textureUploadStageOutcome(empty) == Outcome::NoUpload,
        "valid empty input preserves its no-op but proves no seed upload");
    empty.stagingProduced = true;
    check(textureUploadStageOutcome(empty) == Outcome::Failed,
        "an empty source cannot authenticate an unexpected staging result");
  }

  dxmt9::query::UploadTracker<std::uint32_t, 2u> tracker;
  check(tracker.append(1u, 1u), "an unrelated earlier upload has a real receipt");
  if (textureUploadStageOutcome(allocationFailed) == Outcome::Failed) tracker.fail();
  check(tracker.failed() && tracker.poll(1u, [](auto) {
        return dxmt9::query::UploadStatus::Complete;
      }) == 0u,
      "a stage failure is sticky even before a capture sink and cannot borrow earlier completion");
}

void flattenedSubresourceBoundsStayExact() {
  for (const auto type : {Type::TwoD, Type::Cube, Type::Volume, Type::Array2D}) {
    for (const std::uint32_t levels : {0u, 1u, 4u}) {
      const auto mipLevels = levels ? levels : 1u;
      const auto total = type == Type::Cube ? mipLevels * 6u : mipLevels;
      for (std::uint32_t subresource = 0u; subresource <= total; ++subresource) {
        check(textureUploadSubresourceValid(type, levels, subresource) == (subresource < total),
            "logical cube faces and volume/2D mips match the actual Pool flattening");
      }
    }
  }
  constexpr auto max = std::numeric_limits<std::uint32_t>::max();
  check(textureUploadSubresourceValid(Type::Cube, max, max) &&
      textureUploadSubresourceValid(Type::Cube, max / 6u + 1u, max) &&
      !textureUploadSubresourceValid(Type::Cube, max / 6u, max) &&
      !textureUploadSubresourceValid(Type::TwoD, max, max) &&
      !textureUploadSubresourceValid(Type::Volume, max, max),
      "large mip counts use division bounds without wrapping a six-face extent");
}

void productionFailureCannotQualifyAnEmptyNontrackingPrefix() {
  for (const bool zeroInitialize : {false, true}) {
    for (const bool missingDestination : {false, true}) {
      dxmt9::CommandQueue queue(dxmt9::CommandQueue::ArenaLeaseTestQueueTag{},
          dxmt9::core::BackendLimits{});
      dxmt9::resources::Pool pool;
      dxmt9::resources::Initializer initializer(queue, pool, {});
      dxmt9::SchedulingProgressTestAccess::disableQueryTracking(initializer);
      dxmt9::core::TextureHandle handle{};
      if (!missingDestination) {
        dxmt9::core::TextureDesc desc{};
        desc.width = desc.height = 2u;
        desc.levels = 1u;
        desc.pool = dxmt9::core::Pool::Default;
        desc.usage = dxmt9::core::UsageRenderTarget;
        desc.format = dxmt9::core::Format::A8R8G8B8;
        handle = pool.createTexture(WMT::Device{}, dxmt9::core::BackendLimits{}, desc);
        const auto* record = pool.findTexture(handle.value);
        check(bool(handle) && record && !record->texture,
            "null-device production Pool creates a logical record without Metal backing");
      }
      if (zeroInitialize) initializer.initializeTextureZero(handle);
      else {
        const std::array<std::uint8_t, 16u> bytes{};
        initializer.uploadTextureLevel(handle, 0u, 2u, 2u, 1u, 8u, 16u, bytes);
      }
      std::lock_guard lock(queue.mutex_);
      check(!initializer.hasPendingUploadsUnlocked(),
          "failed actual producer never fabricated a pending upload receipt");
      const auto first = initializer.queryProgressUnlocked();
      check(first.failed && first.published == 0u && first.completed == 0u,
          "actual upload/zero-init failure reaches nontracking progress before capture attach");
      check(!initializer.publishQueryPrefixUnlocked(),
          "an empty pending stream cannot conceal failed actual resource staging");
      check(initializer.queryProgressUnlocked().failed &&
          !initializer.publishQueryPrefixUnlocked(),
          "capture-only flag cleanup cannot erase the persistent query failure latch");
    }
  }
}
} // namespace

int main() {
  try {
    stageOutcomeTruthTable();
    flattenedSubresourceBoundsStayExact();
    productionFailureCannotQualifyAnEmptyNontrackingPrefix();
    std::cout << "texture upload outcome: truth table and bounds passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "texture upload outcome: " << error.what() << '\n';
    return 1;
  }
}
