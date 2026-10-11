#include "device_c_render_tape_initial_setup_v4.hpp"
#include "device_c_render_tape_capture_layout.hpp"
#include "device_c_render_tape_descriptors.hpp"
#include "device_c_render_tape_runtime_objects.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace dxmt9::d3d9 {
namespace {

bool sameSlot(const D9CWireObjectIdentity& a, const D9CWireObjectIdentity& b) noexcept {
  return a.kind == b.kind && a.objectId == b.objectId;
}

} // namespace

RenderTapeInitialSetupV4Plan::RenderTapeInitialSetupV4Plan(
    RenderTapeInitialSetupV4Plan&& other) noexcept {
  *this = std::move(other);
}

RenderTapeInitialSetupV4Plan& RenderTapeInitialSetupV4Plan::operator=(
    RenderTapeInitialSetupV4Plan&& other) noexcept {
  if (this == &other) return *this;
  entries_ = std::move(other.entries_);
  seeds_ = std::move(other.seeds_);
  aliases_ = std::move(other.aliases_);
  digest_ = other.digest_;
  totalSeedBytes_ = other.totalSeedBytes_;
  totalAllocationBytes_ = other.totalAllocationBytes_;
  firstIntervalEvent_ = other.firstIntervalEvent_;
  valid_ = other.valid_;
  other.entries_.clear();
  other.seeds_.clear();
  other.aliases_.clear();
  other.digest_ = {};
  other.totalSeedBytes_ = 0u;
  other.totalAllocationBytes_ = 0u;
  other.firstIntervalEvent_ = kRenderTapeIdentityV4NoIndex;
  other.valid_ = false;
  return *this;
}

bool renderTapeInitialSetupV4SnapshotPrefix(
    const render_tape_runtime::GeneralPreflightPlan& replay,
    std::uint32_t& out) noexcept {
  // Preserve the execution admission grammar, including its stop at a second
  // mutation of an already completed promise. Only publication ownership uses
  // this boundary; late-seed admission below is deliberately more restrictive.
  struct Seed {
    D9CWireObjectIdentity identity{};
    std::uint64_t bytes = 0u;
    std::uint32_t count = 0u;
    std::uint64_t recordedBytes = 0u;
    std::vector<std::uint32_t> subresources{};
  };
  try {
    std::vector<Seed> seeds;
    std::uint32_t count = 0u;
    for (const auto& event : replay.events) {
      if (event.eventIndex != count) return false;
      if (event.type == RenderTapeEventType::BootstrapState) {
        if (count != 0u) return false;
      } else if (event.type == RenderTapeEventType::ObjectDefine) {
        if (event.index >= replay.definitions.size()) return false;
        const auto& definition = replay.definitions[event.index].fixed;
        if (definition.expectedContentCount)
          seeds.push_back({definition.identity, definition.expectedContentBytes,
                           definition.expectedContentCount, 0u, {}});
      } else if (event.type == RenderTapeEventType::ResourceMutation) {
        if (event.index >= replay.mutations.size()) return false;
        const auto& mutation = replay.mutations[event.index];
        auto seed = std::find_if(seeds.begin(), seeds.end(), [&](const auto& value) {
          return renderTapeIdentityV3SameIdentity(value.identity, mutation.identity);
        });
        if (seed == seeds.end() || seed->subresources.size() == seed->count) break;
        if (mutation.byteOffset || seed->recordedBytes > seed->bytes ||
            mutation.byteSize > seed->bytes - seed->recordedBytes ||
            std::find(seed->subresources.begin(), seed->subresources.end(),
                      mutation.subresource) != seed->subresources.end()) return false;
        seed->subresources.push_back(mutation.subresource);
        seed->recordedBytes += mutation.byteSize;
      } else {
        break;
      }
      ++count;
    }
    if (count >= replay.events.size() || !std::all_of(seeds.begin(), seeds.end(),
        [](const auto& seed) {
          return seed.recordedBytes == seed.bytes && seed.subresources.size() == seed.count;
        })) return false;
    out = count;
    return true;
  } catch (...) {
    return false;
  }
}

RenderTapeInitialSetupV4Status buildRenderTapeInitialSetupV4(
    const CaptureSourceReplayPlanV4& sourcePlan,
    const render_tape_runtime::GeneralPreflightPlan& replay,
    std::span<const std::byte> exactTape, std::uint32_t firstIntervalEvent,
    RenderTapeInitialSetupV4Plan& out, RenderTapeInitialSetupV4Limits limits,
    std::span<const D9CWireObjectIdentity> allocationOnlyProducedParents) noexcept {
  using Status = RenderTapeInitialSetupV4Status;
  if (!render_tape_runtime::generalReplayPlanMatchesAuthenticatedV4(
          exactTape, sourcePlan, replay)) return Status::InvalidBinding;
  std::uint32_t prefix = 0u;
  if (!renderTapeInitialSetupV4SnapshotPrefix(replay, prefix) ||
      prefix != firstIntervalEvent) return Status::InvalidBoundary;
  if (allocationOnlyProducedParents.size() > limits.maxObjects)
    return Status::CapacityExceeded;
  for (std::size_t i = 0u; i < allocationOnlyProducedParents.size(); ++i) {
    const auto& identity = allocationOnlyProducedParents[i];
    if (identity.kind != D9C_CHUNK_HANDLE_KIND_TEXTURE || !identity.objectId ||
        !identity.generation) return Status::InvalidBinding;
    for (std::size_t j = 0u; j < i; ++j)
      if (sameSlot(identity, allocationOnlyProducedParents[j])) return Status::InvalidBinding;
  }
  try {
    RenderTapeInitialSetupV4Plan staged;
    std::size_t producedPrepared = 0u;
    for (std::size_t eventIndex = prefix; eventIndex < replay.events.size(); ++eventIndex) {
      const auto& event = replay.events[eventIndex];
      if (event.type != RenderTapeEventType::ObjectDefine) continue;
      const auto& definition = replay.definitions[event.index];
      const auto& fixed = definition.fixed;
      const bool producedRequested = std::any_of(allocationOnlyProducedParents.begin(),
          allocationOnlyProducedParents.end(), [&](const auto& identity) {
            return renderTapeIdentityV3SameIdentity(identity, fixed.identity);
          });
      if (producedRequested) {
        RenderTapeTextureDescriptorV2 texture{};
        D9CSurfaceDesc surface{};
        if (!renderTapeLoadTextureDescriptorV2(definition.descriptor, texture) ||
            texture.initialContentDisposition != static_cast<std::uint32_t>(
                RenderTapeInitialContentDisposition::ProducedByCapturedPass) ||
            texture.dimension != static_cast<std::uint32_t>(RenderTapeTextureDimension::Texture2D) ||
            texture.mipLevelCount != 1u || texture.subresourceCount != 1u ||
            !renderTapeTextureSubresourceDescriptor(definition.descriptor, 0u, surface) ||
            surface.resourceType != 3u || surface.pool != 0u || surface.depth != 1u ||
            surface.multiSampleType || surface.multiSampleQuality ||
            !((surface.usage == 1u && (surface.format == 21u || surface.format == 22u)) ||
              renderTapeProducedTexturePolicy(definition.descriptor) ==
                  RenderTapeProducedTextureContentPolicy::Df24DepthOnly))
          return Status::UnsupportedClosure;
      }
      const bool allocationOnly = producedRequested || renderTapeNullRtDescriptorOnly(
          fixed.identity.kind, definition.descriptor);
      if (!allocationOnly && !fixed.expectedContentCount && !fixed.expectedContentBytes) continue;
      if (staged.entries_.size() >= limits.maxObjects ||
          fixed.expectedContentCount > limits.maxSeeds - staged.seeds_.size())
        return Status::CapacityExceeded;
      if ((!allocationOnly && (!fixed.expectedContentCount || !fixed.expectedContentBytes)) ||
          (allocationOnly && (fixed.expectedContentCount || fixed.expectedContentBytes)))
        return Status::IncompleteClosure;
      for (const auto& other : replay.definitions) {
        if (sameSlot(fixed.identity, other.fixed.identity) &&
            fixed.identity.generation != other.fixed.identity.generation)
          return Status::UnsupportedClosure;
      }

      if (allocationOnly) {
        D9CSurfaceDesc surface{};
        if (!renderTapeTextureSubresourceDescriptor(definition.descriptor, 0u, surface))
          return Status::UnsupportedClosure;
        std::uint64_t allocationBytes = surface.width;
        for (const auto factor : {surface.height, surface.depth, 4u}) {
          if (!factor || allocationBytes > std::numeric_limits<std::uint64_t>::max() / factor)
            return Status::CapacityExceeded;
          allocationBytes *= factor;
        }
        if (allocationBytes > limits.maxAllocationBytes - staged.totalAllocationBytes_)
          return Status::CapacityExceeded;
        staged.entries_.push_back({fixed,
            std::vector<std::byte>(definition.descriptor.begin(), definition.descriptor.end()),
            RenderTapeInitialSetupV4Family::Texture2D, event.index, event.eventIndex,
            kRenderTapeIdentityV4NoIndex, 0u, 0u,
            RenderTapeInitialSetupV4Mode::AllocationOnly, allocationBytes});
        staged.totalAllocationBytes_ += allocationBytes;
        if (producedRequested) ++producedPrepared;
        continue;
      }

      auto family = RenderTapeInitialSetupV4Family::Texture2D;
      if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER) {
        D9CBufferDesc buffer{};
        if (definition.descriptor.size() != sizeof(buffer)) return Status::UnsupportedClosure;
        std::memcpy(&buffer, definition.descriptor.data(), sizeof(buffer));
        if (!buffer.size || fixed.expectedContentCount != 1u ||
            fixed.expectedContentBytes != buffer.size) return Status::IncompleteClosure;
        if (buffer.format == 0u) family = RenderTapeInitialSetupV4Family::VertexBuffer;
        else if (buffer.format == 101u || buffer.format == 102u)
          family = RenderTapeInitialSetupV4Family::IndexBuffer;
        else return Status::UnsupportedClosure;
      } else if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
        RenderTapeTextureDescriptorV2 texture{};
        if (!renderTapeLoadTextureDescriptorV2(definition.descriptor, texture) ||
            texture.initialContentDisposition != static_cast<std::uint32_t>(
                RenderTapeInitialContentDisposition::CompleteSeed))
          return Status::UnsupportedClosure;
        if (texture.subresourceCount != fixed.expectedContentCount)
          return Status::IncompleteClosure;
        switch (static_cast<RenderTapeTextureDimension>(texture.dimension)) {
        case RenderTapeTextureDimension::Texture2D: break;
        case RenderTapeTextureDimension::Cube:
          family = RenderTapeInitialSetupV4Family::TextureCube;
          break;
        case RenderTapeTextureDimension::Volume:
          family = RenderTapeInitialSetupV4Family::TextureVolume;
          break;
        default: return Status::UnsupportedClosure;
        }
      } else {
        // Prefix standalone surfaces retain their existing setup path. Late
        // independently seeded surfaces are not silently interval uploads.
        return Status::UnsupportedClosure;
      }

      std::vector<std::uint64_t> extents(fixed.expectedContentCount);
      const auto contract = renderTapeDeriveExpectedContentContract(
          fixed.identity.kind, definition.descriptor, extents);
      if (contract.status != RenderTapeExpectedContentStatus::Accepted ||
          contract.count != fixed.expectedContentCount ||
          contract.bytes != fixed.expectedContentBytes) return Status::IncompleteClosure;
      if (contract.bytes > limits.maxSeedBytes - staged.totalSeedBytes_)
        return Status::CapacityExceeded;
      if (fixed.expectedContentCount > replay.events.size() - eventIndex - 1u)
        return Status::IncompleteClosure;
      std::vector<bool> seen(fixed.expectedContentCount);
      const auto ownerIndex = static_cast<std::uint32_t>(staged.entries_.size());
      const auto firstSeed = static_cast<std::uint32_t>(staged.seeds_.size());
      for (std::uint32_t relative = 0u; relative < fixed.expectedContentCount; ++relative) {
        const auto& seedEvent = replay.events[eventIndex + 1u + relative];
        if (seedEvent.type != RenderTapeEventType::ResourceMutation)
          return Status::IncompleteClosure;
        const auto& mutation = replay.mutations[seedEvent.index];
        if (!renderTapeIdentityV3SameIdentity(mutation.identity, fixed.identity) ||
            mutation.kind != static_cast<std::uint32_t>(RenderTapeMutationKind::Upload) ||
            mutation.byteOffset || mutation.subresource >= extents.size() ||
            seen[mutation.subresource] || !mutation.byteSize ||
            mutation.byteSize != extents[mutation.subresource] ||
            mutation.bufferDisposition != static_cast<std::uint32_t>(
                RenderTapeBufferMutationDisposition::Plain) ||
            replay.catalogue.lookup(mutation.digest, mutation.byteSize) !=
                RenderTapeBlobLookup::Exact) return Status::IncompleteClosure;
        seen[mutation.subresource] = true;
        staged.seeds_.push_back({mutation, seedEvent.index, seedEvent.eventIndex, ownerIndex});
      }
      staged.entries_.push_back({fixed,
          std::vector<std::byte>(definition.descriptor.begin(), definition.descriptor.end()),
          family, event.index, event.eventIndex, firstSeed, fixed.expectedContentCount,
          contract.bytes});
      staged.totalSeedBytes_ += contract.bytes;
    }

    // Explicit requests may not silently disappear (prefix, absent or stale
    // identity). A partial closure never replaces the previously admitted plan.
    if (producedPrepared != allocationOnlyProducedParents.size()) return Status::InvalidBinding;
    for (const auto& event : replay.events) {
      if (event.type != RenderTapeEventType::ObjectDefine) continue;
      const auto& definition = replay.definitions[event.index];
      if (definition.fixed.identity.kind != D9C_CHUNK_HANDLE_KIND_SURFACE) continue;
      RenderTapeSurfaceDescriptorV2 alias{};
      if (!renderTapeLoadSurfaceDescriptorV2(definition.descriptor, alias) ||
          alias.storage != static_cast<std::uint32_t>(
              RenderTapeSurfaceStorage::TextureSubresource)) continue;
      const auto parent = std::find_if(staged.entries_.begin(), staged.entries_.end(),
          [&](const auto& entry) { return sameSlot(entry.definition.identity, alias.parentTexture); });
      if (parent == staged.entries_.end()) continue;
      const auto parentClosureEvent = parent->mode == RenderTapeInitialSetupV4Mode::AllocationOnly
          ? parent->definitionEventIndex
          : staged.seeds_[parent->firstSeedIndex + parent->seedCount - 1u].eventIndex;
      if (definition.fixed.expectedContentCount || definition.fixed.expectedContentBytes ||
          !renderTapeIdentityV3SameIdentity(parent->definition.identity, alias.parentTexture) ||
          parent->definitionEventIndex >= event.eventIndex ||
          parentClosureEvent >= event.eventIndex ||
          !renderTapeSurfaceAliasMatchesTextureSubresource(parent->descriptor,
              parent->definition.identity, alias)) return Status::InvalidAlias;
      if (staged.aliases_.size() >= limits.maxObjects) return Status::CapacityExceeded;
      staged.aliases_.push_back({definition.fixed.identity, alias.parentTexture,
          static_cast<std::uint32_t>(parent - staged.entries_.begin()),
          event.index, event.eventIndex, alias.subresource});
    }
    staged.digest_ = sourcePlan.digest();
    staged.firstIntervalEvent_ = prefix;
    staged.valid_ = true;
    out = std::move(staged);
    return Status::Valid;
  } catch (...) {
    return Status::AllocationFailed;
  }
}

} // namespace dxmt9::d3d9
