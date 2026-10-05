#pragma once

#include "render_tape_cpu_v4_surface_roles.hpp"
#include "render_tape_cpu_source_observation.hpp"
#include "device_c_render_tape_identity_v3.hpp"
#include "device_c_render_tape_runtime_identity_v4.hpp"
#include "device_c_render_tape_runtime_objects.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace dxmt9::test {

struct CpuTapeV4OriginalSurfaceRoleEntry {
  render::TapeEmissionRecordLocator record{};
  CpuTapeV4SurfaceRoles roles{};
  std::uint32_t identityCommandKind = 0u;
};

struct CpuGeneralTapeControlledV4LogicalSemanticReceipt {
  render::TapeEmissionRecordLocator record{};
  core::MetalCommandKind commandKind{};
  std::uint64_t sequence = 0u;
  std::uint32_t physicalCommandIndex = UINT32_MAX, subdrawIndex = UINT32_MAX;
  std::uint64_t digest = detail::kCpuTapeFnvOffset;
  bool serialized = false, sourceRowsRequired = false;
  CpuTapeSourceObservation sourceRows{};
  std::string failure{};
};

// This separately versioned actual logical fingerprint stream has full field/
// item coverage only after all original keys and scoped row proofs pass. It is
// input to a later independent reference/candidate comparison, NOT equality
// against reconstructed original scalar/layout/uniform values. It never
// upgrades the metadata inventory or grants shader/GPU/future proof.
struct CpuGeneralTapeControlledV4LogicalSemanticResult {
  bool requested = false, admitted = false, accepted = false, complete = false;
  bool legalCandidateNoOverwriteContract = false, conditionalOnLegalNoOverwrite = false;
  bool logicalStreamComplete = false, fullLogicalSemanticOracleComplete = false;
  bool originalSemanticEquivalenceQualified = false, crossPolicyEquivalenceQualified = false;
  bool gpuResultOracle = false, policyQualified = false, timingEligible = false;
  bool futurePreservationQualified = false, fullShaderFetchCoverageQualified = false;
  std::string failure{};
  std::size_t sources = 0u, commands = 0u, draws = 0u, originalItems = 0u;
  std::size_t sourceRowsRequired = 0u, sourceRowsQualified = 0u;
  std::uint64_t logicalBufferHashBytes = 0u, digest = detail::kCpuTapeFnvOffset;
  std::vector<CpuGeneralTapeControlledV4LogicalSemanticReceipt> receipts{};
};

inline bool cpuTapeV4OriginalRecordLess(render::TapeEmissionRecordLocator a,
                                      render::TapeEmissionRecordLocator b) noexcept {
  return a.eventOrdinal < b.eventOrdinal ||
      (a.eventOrdinal == b.eventOrdinal && a.recordIndex < b.recordIndex);
}

// Validate the observed executable stream itself. Sorting afterward could
// conceal same-kind reordering, duplicates or drops across source boundaries.
class CpuTapeV4OriginalItemOrder {
public:
  explicit CpuTapeV4OriginalItemOrder(
      std::shared_ptr<const std::vector<CpuTapeV4OriginalSurfaceRoleEntry>> originals)
      : originals_(std::move(originals)) {}
  bool accept(render::TapeEmissionRecordLocator record, std::uint32_t identityKind) noexcept {
    skipState();
    if (!originals_ || cursor_ >= originals_->size() || (*originals_)[cursor_].record != record ||
        (*originals_)[cursor_].identityCommandKind != identityKind) return false;
    ++cursor_;
    ++accepted_;
    return true;
  }
  bool complete() noexcept { skipState(); return originals_ && cursor_ == originals_->size(); }
  std::size_t accepted() const noexcept { return accepted_; }
private:
  void skipState() noexcept {
    while (originals_ && cursor_ < originals_->size() && (*originals_)[cursor_].identityCommandKind ==
        static_cast<std::uint32_t>(d3d9::RenderTapeIdentityV3CommandKind::Invalid)) ++cursor_;
  }
  std::shared_ptr<const std::vector<CpuTapeV4OriginalSurfaceRoleEntry>> originals_{};
  std::size_t cursor_ = 0u, accepted_ = 0u;
};

// Pure pre-effect construction from authenticated immutable originals. The
// narrow baseline capability qualifies only fresh RT1..3/DS null defaults;
// RT0 stays Unknown until an original canonical binding establishes it.
inline bool buildCpuTapeV4OriginalSurfaceRoles(
    const d3d9::render_tape_runtime::GeneralPreflightPlan& plan,
    bool freshNoAutoDepthSurfaceDefaults,
    std::shared_ptr<const std::vector<CpuTapeV4OriginalSurfaceRoleEntry>>& out,
    std::string& failure) noexcept {
  try {
    CpuTapeV4SurfaceRoles initial;
    if (freshNoAutoDepthSurfaceDefaults) {
      for (std::size_t slot = 1u; slot < initial.renderTargets.size(); ++slot)
        initial.renderTargets[slot].known = true;
      initial.depthStencil.known = true;
    }
    CpuTapeV4SurfaceRoleFold fold(initial);
    for (const auto& chunk : plan.bootstrapChunks) {
      d3d9::ImportedChunkView imported;
      if (!d3d9::importPrevalidatedCommandChunk(chunk.bytes, chunk.envelope, imported)) {
        failure = "controlled-v4-logical-bootstrap-import"; return false;
      }
      for (std::size_t index = 0u; index < imported.records.size(); ++index) {
        CpuTapeV4SurfaceRoles roles;
        if (fold.fold(imported.record(index), roles) != CpuTapeV4SurfaceRoleStatus::Accepted) {
          failure = "controlled-v4-logical-bootstrap-roles"; return false;
        }
      }
    }
    auto candidate = std::make_shared<std::vector<CpuTapeV4OriginalSurfaceRoleEntry>>();
    for (const auto& event : plan.events) {
      if (event.type != d3d9::RenderTapeEventType::CommandChunk) continue;
      if (event.index >= plan.commandChunks.size()) {
        failure = "controlled-v4-logical-original-chunk"; return false;
      }
      const auto& chunk = plan.commandChunks[event.index];
      d3d9::ImportedChunkView imported;
      if (!d3d9::importPrevalidatedCommandChunk(chunk.bytes, chunk.envelope, imported) ||
          imported.records.size() > 65536u - candidate->size()) {
        failure = "controlled-v4-logical-original-role-cap"; return false;
      }
      const auto ordinal = plan.tape.event(event.eventIndex).header.ordinal;
      for (std::size_t index = 0u; index < imported.records.size(); ++index) {
        CpuTapeV4SurfaceRoles roles;
        const auto record = imported.record(index);
        if (fold.fold(record, roles) != CpuTapeV4SurfaceRoleStatus::Accepted) {
          failure = "controlled-v4-logical-original-roles"; return false;
        }
        candidate->push_back({{ordinal, static_cast<std::uint32_t>(index)}, roles,
            static_cast<std::uint32_t>(d3d9::renderTapeIdentityV3RecordCommandKind(record.header.type))});
      }
    }
    if (!std::is_sorted(candidate->begin(), candidate->end(), [](const auto& a, const auto& b) {
          return cpuTapeV4OriginalRecordLess(a.record, b.record);
        })) {
      failure = "controlled-v4-logical-original-order"; return false;
    }
    out = std::move(candidate);
    return true;
  } catch (...) {
    try { failure = "controlled-v4-logical-role-allocation"; } catch (...) {}
    return false;
  }
}

// Consumer-only state. Every context was already authenticated by the inventory
// collector from the real retained source projection. No mutable producer,
// wrapper, Pool current lookup, native contents address or borrowed span lives
// here. The cache retains immutable versions already owned by record contexts,
// not copied buffer images; each complete logical image is hashed once.
class CpuTapeV4LogicalSemanticObserver {
public:
  CpuTapeV4LogicalSemanticObserver(
      std::shared_ptr<const std::vector<CpuTapeV4OriginalSurfaceRoleEntry>> roles,
      std::shared_ptr<CpuTapeExpectedBufferBudget> budget,
      bool legalCandidateNoOverwriteContract)
      : roles_(std::move(roles)), budget_(std::move(budget)), order_(roles_) {
    result_.requested = true;
    result_.legalCandidateNoOverwriteContract = legalCandidateNoOverwriteContract;
    result_.conditionalOnLegalNoOverwrite = legalCandidateNoOverwriteContract;
    result_.admitted = roles_ && budget_ && legalCandidateNoOverwriteContract;
    if (!result_.admitted) result_.failure = "controlled-v4-logical-session-contract";
  }

  template <class Payload, class SourceRowsFn>
  bool observe(const core::metalqueue::ReplaySourcePublicationReceipt& publication,
      const Payload& payload, const d3d9::RenderTapeRuntimePublishedSourceOriginsV4& projection,
      const CpuTapeRecordContextStore& records, SourceRowsFn&& sourceRows) {
    if (!result_.failure.empty()) return false;
    if (result_.sources >= 65536u || payload.commandCount() > 65536u - result_.commands)
      return fail("controlled-v4-logical-command-cap");
    result_.commands += payload.commandCount();
    std::optional<CpuGeneralTapeControlledV4LogicalSemanticReceipt> active;
    std::shared_ptr<const CpuTapeResourceContext> context;
    const CpuTapeV4OriginalSurfaceRoleEntry* selectedRoles = nullptr;
    std::uint64_t itemDigest = detail::kCpuTapeFnvOffset;
    bool drawObserved = false;
    const auto finishItem = [&](bool serialized) {
      if (!active) return true;
      active->digest = itemDigest;
      active->serialized = serialized;
      if (!serialized) active->failure = result_.failure;
      if (result_.receipts.size() >= 65536u) return fail("controlled-v4-logical-receipt-cap");
      result_.receipts.push_back(std::move(*active));
      active.reset();
      result_.originalItems = result_.receipts.size();
      return true;
    };
    const auto location = [&](std::size_t commandIndex, core::MetalCommandKind kind, std::size_t subdraw) {
      if (active && active->sourceRowsRequired && !drawObserved)
        return fail("controlled-v4-logical-draw-callback-coverage");
      if (!finishItem(true)) return false;
      itemDigest = detail::kCpuTapeFnvOffset;
      drawObserved = false;
      active.emplace();
      active->sequence = publication.metadata.seqId;
      active->physicalCommandIndex = static_cast<std::uint32_t>(commandIndex);
      active->subdrawIndex = subdraw == kCpuTapeNoSubdraw ? UINT32_MAX : static_cast<std::uint32_t>(subdraw);
      active->commandKind = kind;
      active->sourceRowsRequired = kind == core::MetalCommandKind::DrawRun;
      const d3d9::RenderTapeRuntimePublishedSourceOriginV4* found = nullptr;
      for (const auto& origin : projection.origins) {
        if (origin.physicalCommandIndex != commandIndex || origin.kind != render::tapeTopologyCommandKind(kind) ||
            (active->sourceRowsRequired ? origin.firstSubdraw != subdraw || origin.subdrawCount != 1u
                                       : origin.firstSubdraw || origin.subdrawCount)) continue;
        if (found) return fail("controlled-v4-logical-ambiguous-original");
        found = &origin;
      }
      if (!found) return fail("controlled-v4-logical-missing-original");
      active->record = found->record;
      if (!order_.accept(found->record, found->kind))
        return fail("controlled-v4-logical-observed-original-order");
      selectedRoles = findRoles(found->record);
      context = records.lookup(found->record);
      if (!selectedRoles || !context || selectedRoles->identityCommandKind != found->kind)
        return fail("controlled-v4-logical-original-context");
      if (active->sourceRowsRequired) {
        ++result_.draws;
        ++result_.sourceRowsRequired;
        const auto* rows = sourceRows(commandIndex, subdraw);
        if (!rows) return fail("controlled-v4-logical-missing-source-rows");
        active->sourceRows = *rows;
        if (!rows->qualified()) return fail("controlled-v4-logical-source-row-proof-rejected");
        ++result_.sourceRowsQualified;
      }
      return true;
    };
    const auto resource = [&](std::uint32_t kind, core::Handle handle, std::uint64_t& hash,
                              CpuTapeResourceRole role) {
      if (!context || !selectedRoles) return fail("controlled-v4-logical-resource-before-location");
      std::shared_ptr<const CpuTapeResourceVersion> version;
      if (role.kind != CpuTapeResourceRoleKind::General) {
        if (kind != D9C_CHUNK_HANDLE_KIND_SURFACE ||
            resolveCpuTapeV4SurfaceRole(selectedRoles->roles, *context, role, handle, version) !=
                CpuTapeV4SurfaceRoleStatus::Accepted)
          return fail("controlled-v4-logical-canonical-surface-role");
        if (!version) { detail::cpuTapeDigestScalar(hash, std::uint64_t{0}); return true; }
      } else version = context->find(kind, handle);
      if (!version || !version->usesExpectedBufferBudget(*budget_) ||
          !appendCpuTapeResourceObservation(version->observation(), hash))
        return fail("controlled-v4-logical-resource-normalization");
      detail::cpuTapeDigestScalar(hash, version->contentEpoch());
      return kind != D9C_CHUNK_HANDLE_KIND_BUFFER || appendLogicalBytes(version, hash);
    };
    const auto snapshot = [&](core::Handle buffer, const core::DrawBufferBindingSnapshot& value,
                              std::uint64_t& hash) {
      const auto version = context ? context->find(D9C_CHUNK_HANDLE_KIND_BUFFER, buffer) : nullptr;
      if (!version || !version->localBufferFacts() ||
          !cpu_source_observation_detail::snapshotAssociationMatches(*version->localBufferFacts(), value))
        return fail("controlled-v4-logical-historical-snapshot");
      return appendLogicalBytes(version, hash);
    };
    const auto live = [&](core::Handle buffer, std::uint64_t& hash, std::uint64_t& physicalRevision) {
      const auto version = context ? context->find(D9C_CHUNK_HANDLE_KIND_BUFFER, buffer) : nullptr;
      if (!version || !version->localBufferFacts() || version->localBufferFacts()->hasVersionedBacking)
        return fail("controlled-v4-logical-historical-live-binding");
      physicalRevision = version->localBufferFacts()->contentRevision;
      return appendLogicalBytes(version, hash);
    };
    const auto revision = [&](core::Handle buffer, CpuTapeBufferRevisionSource source,
                              std::uint64_t physical, std::uint64_t& logical) {
      const auto version = context ? context->find(D9C_CHUNK_HANDLE_KIND_BUFFER, buffer) : nullptr;
      if (!version || !version->localBufferFacts() || !version->contentEpoch() ||
          version->localBufferFacts()->contentRevision != physical ||
          version->localBufferFacts()->hasVersionedBacking != (source == CpuTapeBufferRevisionSource::BindingSnapshot))
        return fail("controlled-v4-logical-content-epoch-projection");
      logical = version->contentEpoch();
      return true;
    };
    const auto draw = [&](std::uint64_t) { drawObserved = true; return true; };
    std::string serializerFailure;
    const bool serialized = observeCpuTapePayload(payload, itemDigest,
        [](std::uint32_t, core::Handle, std::uint64_t&) { return false; }, draw, serializerFailure,
        nullptr, snapshot, nullptr, live, nullptr, location, revision, resource,
        CpuTapeSemanticProjection::LogicalV4);
    if (!serialized) {
      if (result_.failure.empty()) result_.failure = serializerFailure;
      finishItem(false);
      return false;
    }
    if (active && active->sourceRowsRequired && !drawObserved) {
      fail("controlled-v4-logical-draw-callback-coverage"); finishItem(false); return false;
    }
    if (!finishItem(true)) return false;
    ++result_.sources;
    return true;
  }

  bool finish(std::size_t expectedSources, std::size_t expectedCommands) {
    if (!result_.failure.empty()) return false;
    std::size_t cursor = 0u;
    for (const auto& original : *roles_) {
      if (original.identityCommandKind == static_cast<std::uint32_t>(d3d9::RenderTapeIdentityV3CommandKind::Invalid)) continue;
      if (cursor >= result_.receipts.size() || result_.receipts[cursor].record != original.record ||
          !result_.receipts[cursor].serialized ||
          render::tapeTopologyCommandKind(result_.receipts[cursor].commandKind) != original.identityCommandKind)
        return fail("controlled-v4-logical-canonical-coverage");
      ++cursor;
    }
    if (!order_.complete() || order_.accepted() != cursor || cursor != result_.receipts.size() ||
        !cursor || result_.sources != expectedSources ||
        result_.commands != expectedCommands || result_.draws != result_.sourceRowsRequired ||
        result_.sourceRowsRequired != result_.sourceRowsQualified)
      return fail("controlled-v4-logical-complete-coverage");
    result_.digest = detail::kCpuTapeFnvOffset;
    for (const auto& receipt : result_.receipts) {
      detail::cpuTapeDigestScalar(result_.digest, receipt.record.eventOrdinal);
      detail::cpuTapeDigestScalar(result_.digest, receipt.record.recordIndex);
      detail::cpuTapeDigestScalar(result_.digest, receipt.commandKind);
      detail::cpuTapeDigestScalar(result_.digest, receipt.digest);
    }
    result_.complete = result_.accepted = result_.logicalStreamComplete = true;
    return true;
  }
  const char* failure() const noexcept { return result_.failure.empty() ? nullptr : result_.failure.c_str(); }
  CpuGeneralTapeControlledV4LogicalSemanticResult takeResult() {
    auto out = std::move(result_);
    result_ = {};
    result_.failure = "controlled-v4-logical-result-already-consumed";
    return out;
  }

private:
  struct CachedBytes {
    std::shared_ptr<const CpuTapeResourceVersion> version{};
    std::uint64_t digest = detail::kCpuTapeFnvOffset;
  };
  bool fail(const char* reason) {
    result_.accepted = result_.complete = result_.logicalStreamComplete = false;
    if (result_.failure.empty()) result_.failure = reason;
    return false;
  }
  const CpuTapeV4OriginalSurfaceRoleEntry* findRoles(render::TapeEmissionRecordLocator record) const noexcept {
    const auto found = std::lower_bound(roles_->begin(), roles_->end(), record, [](const auto& a, auto b) {
      return cpuTapeV4OriginalRecordLess(a.record, b);
    });
    return found != roles_->end() && found->record == record ? &*found : nullptr;
  }
  bool appendLogicalBytes(const std::shared_ptr<const CpuTapeResourceVersion>& version, std::uint64_t& hash) {
    const CachedBytes* cached = nullptr;
    for (const auto& item : bytes_) if (item.version == version) { cached = &item; break; }
    if (!cached) {
      const auto bytes = version->expectedBufferBytes();
      const auto limit = budget_->snapshot().maxRetainedExpectedBufferBytes;
      if (bytes_.size() >= 65536u || bytes.empty() || result_.logicalBufferHashBytes > limit ||
          bytes.size() > limit - result_.logicalBufferHashBytes)
        return fail("controlled-v4-logical-buffer-hash-cap");
      CachedBytes item{version};
      detail::cpuTapeDigestBytes(item.digest, bytes.data(), bytes.size());
      result_.logicalBufferHashBytes += bytes.size();
      bytes_.push_back(std::move(item));
      cached = &bytes_.back();
    }
    detail::cpuTapeDigestScalar(hash, version->expectedBufferBytes().size());
    detail::cpuTapeDigestScalar(hash, cached->digest);
    return true;
  }
  std::shared_ptr<const std::vector<CpuTapeV4OriginalSurfaceRoleEntry>> roles_{};
  std::shared_ptr<CpuTapeExpectedBufferBudget> budget_{};
  CpuTapeV4OriginalItemOrder order_;
  CpuGeneralTapeControlledV4LogicalSemanticResult result_{};
  std::vector<CachedBytes> bytes_{};
};

} // namespace dxmt9::test
