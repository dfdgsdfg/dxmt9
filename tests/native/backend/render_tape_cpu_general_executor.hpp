#pragma once

// A deliberately cold, resource-backed executor for the general Render Tape
// grammar.  This is test infrastructure, not a second provider.  Admission is
// performed by buildGeneralReplayPlan(); this type only consumes its immutable
// plan and exercises the same D9C object factory, lock/unlock paths, chunk
// replay, and CpuReady queue publication used by the provider.

#include "device_c_common.hpp"
#include "device_c_chunk_replay.hpp"
#include "device_c_cpu_ready_plan.hpp"
#include "device_c_render_tape_capture_layout.hpp"
#include "device_c_render_tape_provider.hpp"
#include "device_c_render_tape_source_plan.hpp"
#include "device_c_render_tape_runtime_objects.hpp"
#include "device_c_render_tape_completion.hpp"
#include "device_c_render_tape_cpu_window.hpp"
#include "device_c_render_tape_execution_v4.hpp"

#include "render_tape_cpu_semantics.hpp"
#include "render_tape_cpu_resource_observation.hpp"
#include "render_tape_cpu_snapshot_proof.hpp"
#include "render_tape_cpu_work_boundary.hpp"
#include "render_tape_cpu_mutation_boundary.hpp"
#include "render_tape_cpu_source_oracle.hpp"
#include "render_tape_cpu_source_admission.hpp"
#include "render_tape_cpu_source_observation.hpp"
#include "render_tape_cpu_policy.hpp"
#include "render_tape_cpu_diagnostic.hpp"
#include "render_tape_cpu_general_grammar.hpp"
#include "render_tape_cpu_initial_setup_v4.hpp"
#include "render_tape_cpu_resource_context.hpp"
#include "render_tape_cpu_producer_resource_ledger_v4.hpp"
#include "render_tape_cpu_v4_admission.hpp"
#include "render_tape_cpu_v4_consumer.hpp"
#include "render_tape_cpu_v4_logical_semantics.hpp"

#include "dxmt9/dxmt9_source_payload.hpp"
#include "dxmt9/attachment_contents.hpp"
#include "dxmt9/dxmt9_command_queue.hpp"
#include "dxmt9/dxmt9_device.hpp"
#include "dxmt9/dxmt9_perf_counters.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace dxmt9::test {

struct CpuGeneralTapeTrace {
  std::uint32_t event = 0xffffffffu;
  std::uint32_t source = 0u;
  std::uint64_t sourceGeneration = 0u;
  std::uint64_t submission = 0u;
  std::uint32_t storageFirstPage = 0u;
  std::uint32_t storagePageCount = 0u;
  std::uint64_t storageGeneration = 0u;
  bool submitted = false;
  bool completed = false;
  bool reclaimed = false;
  std::uint32_t releasedAtEvent = 0xffffffffu;
  std::vector<D9CWireObjectIdentity> bufferClosure;
  std::uint32_t heldMutationEvent = 0u;
  std::optional<D9CWireObjectIdentity> heldMutationIdentity;
};

struct CpuGeneralTapeDrawObservation {
  std::uint32_t ordinal = 0u;
  std::uint32_t publicationEvent = 0xffffffffu;
  std::uint64_t cumulativeSemanticDigest = 0u;
};

struct CpuGeneralTapeTombstone {
  D9CWireObjectIdentity identity{};
  core::Handle nativeHandle{};
};

struct CpuGeneralTapeDestroyWindow {
  std::uint32_t event = 0u;
  std::vector<std::uint64_t> pendingSubmissions;
  std::size_t ownersChecked = 0u;
  bool retentionValid = false;
};

struct CpuGeneralTapeExecutionResult {
  bool complete = false;
  std::string failure{};
  std::uint32_t failedEvent = 0xffffffffu;
  std::size_t events = 0u;
  std::size_t commands = 0u;
  std::size_t draws = 0u;
  std::size_t mipmapCommands = 0u;
  std::size_t retainedMipmapTexturesAfterDestroy = 0u;
  std::uint64_t digest = 1469598103934665603ull;
  std::size_t resourcesCreated = 0u;
  std::size_t resourcesReleased = 0u;
  std::size_t submitted = 0u;
  std::size_t completed = 0u;
  std::size_t reclaimed = 0u;
  std::size_t pendingPeak = 0u;
  std::size_t deferredReleases = 0u;
  std::size_t destroysWithPending = 0u;
  std::size_t retainedSnapshotsAfterDestroy = 0u;
  std::size_t deferredBufferMutations = 0u;
  std::size_t deferredDiscardMutations = 0u;
  std::size_t retainedBuffersAfterMutation = 0u;
  std::size_t mutationBoundaryReleases = 0u;
  std::vector<CpuGeneralTapeTrace> trace{};
  std::vector<CpuGeneralTapeDrawObservation> drawObservations{};
  // Retain native metadata after wrapper release for qualification only.
  std::vector<CpuGeneralTapeTombstone> tombstones{};
  std::vector<CpuGeneralTapeDestroyWindow> destroyWindows{};
  std::size_t windowPageSize = 0u;
  bool windowOffload = false;
  bool windowArena = false;
  bool windowWorkersDisabled = false;
  std::uint64_t setupNs = 0u;
  std::uint64_t replayNs = 0u;
  std::uint64_t observeNs = 0u;
  std::uint64_t cleanupNs = 0u;
  std::uint64_t completionNs = 0u;
  bool resetOk = false;
  std::size_t observedSnapshots = 0u;
  std::size_t observedLiveBindings = 0u;
  std::size_t sourceOracleCheckedSources = 0u;
  std::size_t observedMutations = 0u;
  std::size_t observedTextureUploads = 0u;
  // Cheap counts taken from each published source payload.  These remain
  // available when semantic observation is disabled for timing samples.
  std::size_t sourceSummaryCommands = 0u;
  std::size_t sourceSummaryDraws = 0u;
  std::size_t fullOracleCalls = 0u;
  bool logicalResourceOracleComplete = false;
  dxmt9::d3d9::RenderTapeDigest captureSourcePlanDigest{};
  std::size_t captureSourcePlanRanges = 0u;
  std::size_t captureSourcePlanPublications = 0u;
  std::size_t captureSourcePlanEmptySources = 0u;
  std::size_t captureSourcePlanPresentSources = 0u;
  std::vector<dxmt9::d3d9::CaptureSourceReplayRow> captureSourceRows{};
  // The serialized source-replay receipt deliberately mirrors the provider
  // evidence contract: authenticated source rows are retained alongside the
  // provider-local settlement facts and flat publication records.
  std::vector<dxmt9::d3d9::RenderTapeSourceReplayRowEvidence>
      captureSourceReplaySources{};
  std::vector<dxmt9::d3d9::RenderTapeSourceReplayPublicationEvidence>
      captureSourceReplayPublications{};
  bool captureSourceReplayStorageOverflow = false;
  dxmt9::d3d9::RenderTapeSourceReplayV3Evidence captureSourceReplayV3{};
  dxmt9::d3d9::RenderTapeQueryReplayEvidence queryReplay{};
  std::uint64_t querySemanticDigest = 1469598103934665603ull;
  d3d9::render_tape_runtime::GeneralCpuAdmissionResult inputAdmission{};
  d3d9::render_tape_runtime::GeneralCpuSourceDemand sourceDemand{};
  std::size_t sourcePublications = 0u;
  std::size_t progressSteps = 0u;
};

// Actual retained Replay payload values, not a predicted or executed Metal
// Encode route. No payload span, CPU address, shader owner or wrapper survives
// the synchronous source borrow. Canonical raw facts remain keyed by record.
struct CpuGeneralTapeControlledV4DrawInventory {
  std::uint32_t physicalCommandIndex = 0u, subdrawIndex = 0u;
  render::TapeEmissionRecordLocator record{};
  core::DrawParam parameters{};
  std::array<core::Handle, core::kMaxStreams> streamBuffers{};
  std::array<std::uint32_t, core::kMaxStreams> streamOffsets{}, streamStrides{}, streamFrequencies{};
  std::uint32_t streamMask = 0u;
  core::Handle indexBuffer{};
  std::uint64_t userVertexBytes = 0u, userIndexBytes = 0u;
  bool hasBindingOverride = false, hasBindingSnapshot = false;
  CpuTapeSourceObservation sourceRows{};
};

struct CpuGeneralTapeControlledV4InventoryOrigin {
  d3d9::RenderTapeRuntimePublishedSourceOriginV4 origin{};
  std::shared_ptr<const CpuTapeResourceContext> context{};
};

struct CpuGeneralTapeControlledV4InventorySource {
  core::metalqueue::ReplaySourcePublicationReceipt publication{};
  std::vector<d3d9::RenderTapeIdentityV4PhysicalCommand> commands{};
  std::vector<CpuGeneralTapeControlledV4InventoryOrigin> origins{};
  std::vector<CpuGeneralTapeControlledV4DrawInventory> draws{};
};

struct CpuGeneralTapeControlledV4CanonicalDrawRecord {
  render::TapeEmissionRecordLocator record{};
  std::uint32_t eventIndex = 0u, type = 0u;
  // Exact admitted original payload, including raw minVertex/vertexCount,
  // stream/base/index/instance-adjacent facts and any UP bytes. Owned values
  // only; the consumer never accesses this producer-built result table.
  std::vector<std::byte> payload{};
};

struct CpuGeneralTapeControlledV4BufferVersionInventory {
  D9CWireObjectIdentity identity{};
  std::uint64_t logicalContentEpoch = 0u;
  resources::BufferVersionObservation observation{};
};

struct CpuGeneralTapeControlledV4ResourceVersionFailure {
  const char* stage = nullptr;
  D9CWireObjectIdentity requestedIdentity{}, normalizedIdentity{};
  std::uint32_t nativeKind = 0u;
  core::Handle nativeHandle{};
  std::uint64_t priorLogicalContentEpoch = 0u;
  CpuTapeResourceContextStatus status = CpuTapeResourceContextStatus::InvalidInput;
  std::vector<D9CWireObjectIdentity> installedNativeOwners{};
};

// Genuine body replay diagnostics only. The selected provider policy is not a
// promise that every draw takes Direct: ProductionSlot retains natural fallback.
struct CpuGeneralTapeControlledV4RouteCoverage {
  d3d9::ProviderReplayPolicy requestedRoute = d3d9::ProviderReplayPolicy::Compatibility;
  d3d9::ProviderReplayPolicy effectiveRoute = d3d9::ProviderReplayPolicy::Compatibility;
  bool policySelected = false, observed = false, monotonic = false, complete = false;
  std::uint64_t directLeases = 0u, directDraws = 0u;
  std::uint64_t compatibilityDraws = 0u, compatibilityBatches = 0u;
  std::uint64_t compatibilityUnbatchedDraws = 0u, fallbackDraws = 0u;
};

enum class CpuGeneralTapeControlledV4ExecutionMode : std::uint8_t {
  Observe,
  ExecutionOnly,
};

struct CpuGeneralTapeControlledV4ClockScope {
  bool started = false, valid = false, complete = false;
  std::uint64_t wallNs = 0u, processCpuNs = 0u;
};

struct CpuGeneralTapeControlledV4Clocks {
  CpuGeneralTapeControlledV4ClockScope setup{}, body{}, teardown{};
};

// Omission applies to diagnostic shadows/observers, never the genuine
// provider resources, uploads, immutable original input or V4 provenance.
struct CpuGeneralTapeControlledV4ExecutionOnlyEvidence {
  bool requested = false, complete = false, lifecycleCoverageComplete = false;
  bool resourceVersionsOmitted = false, recordContextsOmitted = false;
  bool expectedImagesOmitted = false, canonicalDrawCopiesOmitted = false;
  bool payloadObserverOmitted = false, sourceRowReadsOmitted = false;
  bool logicalHashingOmitted = false, diagnosticMutationReceiptsOmitted = false;
  bool capturedBindingImagesOmitted = false, uploadByteComparisonsOmitted = false;
  std::size_t sources = 0u, commands = 0u, draws = 0u;
  std::size_t submitted = 0u, acknowledged = 0u, retired = 0u;
};

// Controlled driver result; complete never means full CPU semantics, GPU
// correctness, policy qualification or timing eligibility. V4 provenance
// semanticMappingComplete is a separate canonical-origin correspondence.
struct CpuGeneralTapeControlledV4InventoryResult {
  explicit CpuGeneralTapeControlledV4InventoryResult(ControlledCpuV4InputAdmission input)
      : inputAdmission(std::move(input)) {}
  ControlledCpuV4InputAdmission inputAdmission;
  bool complete = false, resetOk = false, metadataInventoryComplete = false;
  bool fullSemanticOracleComplete = false, actualMetal = false;
  bool gpuResultOracle = false, policyQualified = false, timingEligible = false;
  bool runtimeBudgetedAttempt = true, sourceProgressComplete = false;
  std::string failure{};
  std::uint32_t failedEvent = 0xffffffffu;
  std::size_t events = 0u, commands = 0u, draws = 0u;
  std::size_t resourcesCreated = 0u, resourcesReleased = 0u;
  std::size_t observedMutations = 0u, observedTextureUploads = 0u, recordContexts = 0u;
  std::size_t setupSubmissions = 0u, setupAcknowledgements = 0u;
  std::size_t bufferVersionsObserved = 0u, bufferVersionsUnavailable = 0u;
  d3d9::RenderTapeExecutionV4Evidence provenance{};
  d3d9::RenderTapeControlledCpuV4Evidence controlledCpu{};
  CpuInitialSetupV4Evidence initialSetup{};
  d3d9::RenderTapeQueryReplayEvidence queryReplay{};
  CpuV4ConsumerSnapshot consumer{};
  std::vector<CpuV4ConsumerReceipt> consumerReceipts{};
  CpuTapeExpectedBufferBudgetSnapshot expectedBufferBudget{};
  std::vector<CpuGeneralTapeControlledV4InventorySource> inventory{};
  std::vector<CpuGeneralTapeControlledV4CanonicalDrawRecord> canonicalDrawRecords{};
  std::vector<CpuGeneralTapeControlledV4BufferVersionInventory> bufferVersions{};
  std::optional<CpuGeneralTapeControlledV4ResourceVersionFailure> resourceVersionFailure{};
  CpuGeneralTapeControlledV4LogicalSemanticResult logicalSemantics{};
  CpuGeneralTapeControlledV4RouteCoverage routeCoverage{};
  CpuGeneralTapeControlledV4ExecutionMode executionMode = CpuGeneralTapeControlledV4ExecutionMode::Observe;
  CpuGeneralTapeControlledV4ExecutionOnlyEvidence executionOnly{};
  CpuGeneralTapeControlledV4Clocks clocks{};
};

namespace detail {

using dxmt9::d3d9::RenderTapeDigest;
using dxmt9::d3d9::RenderTapeProviderBlob;
using dxmt9::d3d9::RenderTapeSurfaceDescriptorV2;
using dxmt9::d3d9::RenderTapeBlockLayoutStatus;
using dxmt9::d3d9::RenderTapeBlockLockLayout;
using dxmt9::d3d9::RenderTapeLinearLayoutStatus;
using dxmt9::d3d9::RenderTapeLinearLockLayout;
using dxmt9::d3d9::renderTapeBlockLockLayout;
using dxmt9::d3d9::renderTapeLinearBytesPerPixel;
using dxmt9::d3d9::renderTapeLinearLockLayout;
using dxmt9::d3d9::writeRenderTapeLinearRows;
using dxmt9::d3d9::renderTapeTextureSubresourceDescriptor;
using dxmt9::d3d9::renderTapeLoadSurfaceDescriptorV2;

inline bool cpuGeneralSameIdentity(const D9CWireObjectIdentity& a,
                                   const D9CWireObjectIdentity& b) noexcept {
  return a.kind == b.kind && a.generation == b.generation &&
         a.objectId == b.objectId;
}

inline bool cpuGeneralSameDigest(const RenderTapeDigest& a,
                                 const RenderTapeDigest& b) noexcept {
  return std::memcmp(a.data(), b.data(), a.size()) == 0;
}

inline const RenderTapeProviderBlob* cpuGeneralFindBlob(
    std::span<const RenderTapeProviderBlob> blobs,
    const RenderTapeDigest& digest) noexcept {
  for (const auto& blob : blobs)
    if (cpuGeneralSameDigest(blob.digest, digest)) return &blob;
  return nullptr;
}

inline bool cpuGeneralWriteRows(const D9CSurfaceDesc& desc,
                                const D9CLockedRect& lock,
                                std::span<const std::byte> bytes) {
  if (renderTapeLinearBytesPerPixel(desc.format) != 0u) {
    RenderTapeLinearLockLayout layout{};
    if (renderTapeLinearLockLayout(desc, lock.pitch, nullptr, layout) !=
            RenderTapeLinearLayoutStatus::Accepted ||
        bytes.size() != layout.tightBytes)
      return false;
    return writeRenderTapeLinearRows(bytes, lock.bits, layout);
  }
  RenderTapeBlockLockLayout layout{};
  if (renderTapeBlockLockLayout(desc, lock.pitch, nullptr, layout) !=
          RenderTapeBlockLayoutStatus::Accepted ||
      bytes.size() != layout.tightBytes || !lock.bits)
    return false;
  auto* dst = static_cast<std::byte*>(lock.bits);
  for (std::uint32_t row = 0u; row < layout.rows; ++row) {
    std::memcpy(dst + static_cast<std::size_t>(row) * layout.pitch,
                bytes.data() + static_cast<std::size_t>(row) * layout.rowBytes,
                layout.rowBytes);
  }
  return true;
}

struct CpuGeneralTextureUploadShape {
  std::uint32_t width = 0u;
  std::uint32_t height = 0u;
  std::uint32_t depth = 0u;
  std::uint32_t pitch = 0u;
  std::uint32_t slicePitch = 0u;
  std::uint64_t bytes = 0u;
};

inline bool cpuGeneralTextureUploadShape(
    const D9CSurfaceDesc& desc, CpuGeneralTextureUploadShape& out) noexcept {
  out = {};
  const auto content = d3d9::renderTapeDeriveExpectedTextureSubresourceContent(desc);
  if (content.status != d3d9::RenderTapeExpectedContentStatus::Accepted)
    return false;
  const auto format = d3d9::devicec::fmtFromD3D(desc.format);
  const auto pitch = core::formatRowPitch(format, desc.width);
  const auto slicePitch = core::formatByteSize(format, desc.width, desc.height);
  if (pitch == 0u || slicePitch == 0u || desc.depth == 0u ||
      slicePitch > std::numeric_limits<std::uint32_t>::max() ||
      content.bytes != static_cast<std::uint64_t>(slicePitch) * desc.depth)
    return false;
  out = {desc.width, desc.height, desc.depth, pitch,
         static_cast<std::uint32_t>(slicePitch), content.bytes};
  return true;
}

inline bool cpuGeneralTextureUploadShapeMatches(
    const CpuGeneralTextureUploadShape& expected, std::uint32_t width,
    std::uint32_t height, std::uint32_t depth, std::uint32_t pitch,
    std::uint32_t slicePitch, std::size_t bytes) noexcept {
  return expected.bytes != 0u && width == expected.width &&
         height == expected.height && depth == expected.depth &&
         pitch == expected.pitch && slicePitch == expected.slicePitch &&
         bytes == expected.bytes;
}

inline bool cpuGeneralWriteTextureRows(
    const D9CSurfaceDesc& desc, const D9CTexture& texture,
    std::uint32_t subresource, const D9CLockedRect& lock,
    std::span<const std::byte> bytes) {
  if (desc.resourceType != 4u) return cpuGeneralWriteRows(desc, lock, bytes);
  d3d9::RenderTapeVolumeLockLayout canonical{};
  if (d3d9::renderTapeVolumeContentLayout(desc, canonical) !=
          d3d9::RenderTapeVolumeLayoutStatus::Accepted ||
      canonical.fullBytes != bytes.size())
    return false;
  // Native core storage packs compressed block-row slices. A real WoW64
  // pointer shadow, if present, carries its explicitly recorded slice stride.
  // Never infer either stride from the logical texel height.
  auto slicePitch = canonical.fullSliceBytes;
  if (const auto shadow = texture.wow64Locks.find(subresource);
      shadow != texture.wow64Locks.end() && shadow->second.active &&
      shadow->second.shadow.ptr == lock.bits)
    slicePitch = shadow->second.shadowSlicePitch;
  if (slicePitch > static_cast<std::uint64_t>(
                       std::numeric_limits<std::int64_t>::max()))
    return false;
  d3d9::RenderTapeVolumeLockLayout locked{};
  return d3d9::renderTapeVolumeLockLayout(desc, lock.pitch,
             static_cast<std::int64_t>(slicePitch), nullptr, locked) ==
             d3d9::RenderTapeVolumeLayoutStatus::Accepted &&
         d3d9::writeRenderTapeVolumeRows(bytes, lock.bits, locked);
}

inline bool cpuGeneralWriteTextureMutationRows(
    std::span<const std::byte> descriptor, const D9CTexture& texture,
    std::uint32_t subresource, std::uint64_t byteOffset,
    const D9CLockedRect& lock, std::span<const std::byte> bytes) {
  D9CSurfaceDesc desc{};
  if (byteOffset != 0u ||
      !d3d9::renderTapeTextureSubresourceDescriptor(descriptor, subresource, desc))
    return false;
  if (desc.format == 113u) {
    // Raw half-float checkpoint bytes are not general LockRect admission.
    // The complete parent descriptor, rather than a plausible level shape,
    // authorizes this snapshot-only full-subresource restoration path.
    return subresource == 0u &&
        d3d9::renderTapeArmColorSnapshotTextureSupported(descriptor) &&
        d3d9::writeRenderTapeColorSnapshotRows(bytes, lock.bits, lock.pitch, desc);
  }
  return cpuGeneralWriteTextureRows(desc, texture, subresource, lock, bytes);
}

using LogicalReceipt = CpuTapeResourceReceipt;

inline LogicalReceipt* cpuGeneralFindReceipt(
    std::vector<LogicalReceipt>& receipts,
    const D9CWireObjectIdentity& identity, std::uint32_t subresource) {
  for (auto& receipt : receipts)
    if (cpuGeneralSameIdentity(receipt.identity, identity) &&
        receipt.subresource == subresource)
      return &receipt;
  return nullptr;
}

inline void cpuGeneralRecordReceipt(
    std::vector<LogicalReceipt>& receipts,
    const D9CWireObjectIdentity& identity, std::uint32_t subresource,
    const RenderTapeDigest& digest) {
  auto* prior = cpuGeneralFindReceipt(receipts, identity, subresource);
  if (prior) {
    prior->digest = digest;
    ++prior->revision;
    return;
  }
  receipts.push_back({.identity = identity,
                      .subresource = subresource,
                      .digest = digest,
                      .revision = 1u});
}

inline void cpuGeneralHashBytes(std::uint64_t& hash,
                                std::span<const std::uint8_t> bytes) noexcept {
  constexpr std::uint64_t prime = 1099511628211ull;
  for (const auto byte : bytes) {
    hash ^= byte;
    hash *= prime;
  }
}

inline void cpuGeneralHashDigest(std::uint64_t& hash,
                                 const RenderTapeDigest& digest) noexcept {
  cpuGeneralHashBytes(hash, std::span<const std::uint8_t>(
                               reinterpret_cast<const std::uint8_t*>(
                                   digest.data()),
                               digest.size()));
}

// The original canonical event dispatcher is shared by legacy scheduling and
// the controlled inventory session. Each callback performs that event's real
// effect; this helper neither fabricates events nor changes their order.
template <class Bootstrap, class Define, class Mutation, class Chunk,
          class Destroy, class Control, class PresentComplete>
bool dispatchCpuGeneralTapeEvent(d3d9::RenderTapeEventType type,
    Bootstrap&& bootstrap, Define&& define, Mutation&& mutation, Chunk&& chunk,
    Destroy&& destroy, Control&& control, PresentComplete&& presentComplete) {
  switch (type) {
  case d3d9::RenderTapeEventType::BootstrapState: return bootstrap();
  case d3d9::RenderTapeEventType::ObjectDefine: return define();
  case d3d9::RenderTapeEventType::ResourceMutation: return mutation();
  case d3d9::RenderTapeEventType::CommandChunk: return chunk();
  case d3d9::RenderTapeEventType::ObjectDestroy: return destroy();
  case d3d9::RenderTapeEventType::OrderedControl: return control();
  case d3d9::RenderTapeEventType::PresentComplete: return presentComplete();
  }
  return false;
}

// Independent consumer-owned collector. Its only producer-visible dependency
// is the thread-safe write-once record store; it never reads the mutable live
// object, receipt, tombstone or current-version tables.
class CpuGeneralTapeControlledV4InventoryObserver {
 public:
  CpuGeneralTapeControlledV4InventoryObserver(
      std::shared_ptr<d3d9::RenderTapeRuntimeIdentityV4> root,
      render::TapeTopologyCaptureIdentity capture,
      const CpuTapeRecordContextStore& records,
      std::shared_ptr<CpuTapeExpectedBufferBudget> budget,
      CpuV4ConsumerLimits limits,
      std::shared_ptr<const std::vector<CpuTapeV4OriginalSurfaceRoleEntry>> logicalRoles = {},
      bool observeLogicalSemantics = false, bool legalCandidateNoOverwriteContract = false)
      : root_(std::move(root)), capture_(capture), records_(records),
        budget_(std::move(budget)), limits_(limits) {
    if (observeLogicalSemantics)
      logical_ = std::make_unique<CpuTapeV4LogicalSemanticObserver>(
          std::move(logicalRoles), budget_, legalCandidateNoOverwriteContract);
  }

  template <class Payload>
  void observe(CommandQueue& queue,
               const core::metalqueue::ReplaySourcePublicationReceipt& publication,
               const Payload& payload) noexcept {
    try {
      std::lock_guard lock(mutex_);
      if (failure_) return;
      d3d9::RenderTapeRuntimePublishedSourceOriginsV4 projection;
      if (!root_ || !budget_ ||
          !root_->copyPublishedSourceOrigins(capture_, publication, projection) ||
          projection.commands.size() != payload.commandCount()) {
        failure_ = "controlled-v4-inventory-source-origins";
        return;
      }
      if (sources_.size() >= limits_.maxSources ||
          payload.commandCount() > limits_.maxCommands - commands_ ||
          projection.origins.size() > limits_.maxCommands - origins_) {
        failure_ = "controlled-v4-inventory-capacity";
        return;
      }
      // observeReady releases the queue mutex before this callback. Pull one
      // genuine window while the actual Represented source remains pinned;
      // no source submit/ACK/retirement can occur before the callback returns.
      const auto progress = queue.queryProgress();
      if (progress.queue != capture_.queue || progress.failed ||
          !publication.metadata.seqId || progress.completed >= publication.metadata.seqId) {
        failure_ = "controlled-v4-inventory-source-window";
        return;
      }
      CpuGeneralTapeControlledV4InventorySource candidate;
      candidate.publication = publication;
      candidate.commands = projection.commands;
      candidate.origins.reserve(projection.origins.size());
      for (const auto& origin : projection.origins) {
        const auto context = records_.lookup(origin.record);
        if (!context) {
          failure_ = "controlled-v4-inventory-missing-record-context";
          return;
        }
        if (context->purpose() != CpuTapeResourceContextPurpose::V4SurfaceAliasInventory ||
            context->hasSemanticUniqueNativeAuthority()) {
          failure_ = "controlled-v4-inventory-context-purpose";
          return;
        }
        for (const auto& version : context->entries()) {
          if (!version || !version->usesExpectedBufferBudget(*budget_)) {
            failure_ = "controlled-v4-inventory-foreign-version-budget";
            return;
          }
        }
        candidate.origins.push_back({origin, context});
      }
      std::size_t selectedOrigins = 0u;
      for (std::size_t commandIndex = 0u; commandIndex < payload.commandCount(); ++commandIndex) {
        const auto view = payload.commandAt(commandIndex);
        const auto kind = render::tapeTopologyCommandKind(view.kind());
        const auto& command = view.command;
        if (projection.commands[commandIndex].physicalCommandIndex != commandIndex ||
            projection.commands[commandIndex].kind != kind ||
            projection.commands[commandIndex].drawCount != command.drawParams.size()) {
          failure_ = "controlled-v4-inventory-command-shape";
          return;
        }
        const bool draw = view.kind() == core::MetalCommandKind::DrawRun;
        const auto locations = draw ? command.drawParams.size() : 1u;
        if (draw && (!command.drawState.hot || !locations)) {
          failure_ = "controlled-v4-inventory-draw-state";
          return;
        }
        for (std::size_t subdraw = 0u; subdraw < locations; ++subdraw) {
          const d3d9::RenderTapeRuntimePublishedSourceOriginV4* found = nullptr;
          for (const auto& origin : projection.origins) {
            if (origin.physicalCommandIndex != commandIndex || origin.kind != kind ||
                (draw ? origin.firstSubdraw != subdraw || origin.subdrawCount != 1u
                      : origin.firstSubdraw || origin.subdrawCount)) continue;
            if (found) {
              failure_ = "controlled-v4-inventory-ambiguous-origin";
              return;
            }
            found = &origin;
          }
          if (!found) {
            failure_ = "controlled-v4-inventory-unmapped-command";
            return;
          }
          ++selectedOrigins;
          if (!draw) continue;
          const auto& param = command.drawParams[subdraw];
          auto hot = *command.drawState.hot;
          std::optional<core::DrawShaderLayoutContext> effectiveLayout;
          if (command.drawState.shaderLayout) effectiveLayout = *command.drawState.shaderLayout;
          core::DrawBindingOverride binding{};
          core::DrawBindingSnapshot snapshot{};
          const auto validRange = [&](core::DrawPayloadRange range) {
            return !range.size || (range.offset <= command.drawPayloadBytes.size() &&
                range.size <= command.drawPayloadBytes.size() - range.offset);
          };
          if (!validRange(param.userVertexRange) || !validRange(param.userIndexRange) ||
              !validRange(param.bindingOverrideRange) || !validRange(param.bindingSnapshotRange) ||
              (param.bindingOverrideRange.size && param.bindingOverrideRange.size != sizeof(binding)) ||
              (param.bindingSnapshotRange.size &&
                  param.bindingSnapshotRange.size != sizeof(core::DrawBindingSnapshot))) {
            failure_ = "controlled-v4-inventory-payload-range";
            return;
          }
          const auto overrideBytes = core::drawPayloadRangeBytes(
              param.bindingOverrideRange, command.drawPayloadBytes);
          const auto snapshotBytes = core::drawPayloadRangeBytes(
              param.bindingSnapshotRange, command.drawPayloadBytes);
          if ((!overrideBytes.empty() && overrideBytes.size() != sizeof(binding)) ||
              (!snapshotBytes.empty() && snapshotBytes.size() != sizeof(core::DrawBindingSnapshot))) {
            failure_ = "controlled-v4-inventory-binding-shape";
            return;
          }
          if (!overrideBytes.empty()) {
            std::memcpy(&binding, overrideBytes.data(), sizeof(binding));
            encoders::applyDrawBindingOverride(hot,
                effectiveLayout ? &*effectiveLayout : nullptr, binding);
          }
          if (!snapshotBytes.empty())
            std::memcpy(&snapshot, snapshotBytes.data(), sizeof(snapshot));
          const auto context = records_.lookup(found->record);
          if (!context) {
            failure_ = "controlled-v4-inventory-missing-record-context";
            return;
          }
          CpuGeneralTapeControlledV4DrawInventory item;
          item.physicalCommandIndex = static_cast<std::uint32_t>(commandIndex);
          item.subdrawIndex = static_cast<std::uint32_t>(subdraw);
          item.record = found->record;
          item.parameters = param;
          item.streamBuffers = hot.streamBuffers;
          item.streamOffsets = hot.streamOffsets;
          item.streamStrides = hot.streamStrides;
          item.streamFrequencies = hot.streamFrequencies;
          item.streamMask = hot.streamMask;
          item.indexBuffer = hot.indexBuffer;
          item.userVertexBytes = core::drawPayloadRangeBytes(
              param.userVertexRange, command.drawPayloadBytes).size();
          item.userIndexBytes = core::drawPayloadRangeBytes(
              param.userIndexRange, command.drawPayloadBytes).size();
          item.hasBindingOverride = !overrideBytes.empty();
          item.hasBindingSnapshot = !snapshotBytes.empty();
          item.sourceRows = observeCpuTapeSourceRows(queue.pool(), hot,
              effectiveLayout ? &*effectiveLayout : nullptr, param,
              snapshotBytes.empty() ? nullptr : &snapshot, *context,
              publication.metadata.seqId, progress, capture_.queue);
          candidate.draws.push_back(std::move(item));
        }
      }
      if (selectedOrigins != projection.origins.size()) {
        failure_ = "controlled-v4-inventory-origin-coverage";
        return;
      }
      if (logical_) {
        const auto rows = [&](std::size_t commandIndex, std::size_t subdraw) -> const CpuTapeSourceObservation* {
          const auto found = std::find_if(candidate.draws.begin(), candidate.draws.end(), [&](const auto& draw) {
            return draw.physicalCommandIndex == commandIndex && draw.subdrawIndex == subdraw;
          });
          return found == candidate.draws.end() ? nullptr : &found->sourceRows;
        };
        // The exact retained payload goes through the existing full serializer.
        // Its source-row gate reuses the genuine scoped observation above;
        // logical hashing never replaces it or reads an entire live backing.
        if (!logical_->observe(publication, payload, projection, records_, rows)) {
          failure_ = "controlled-v4-logical-observation-rejected";
          return;
        }
      }
      commands_ += payload.commandCount();
      origins_ += projection.origins.size();
      sources_.push_back(std::move(candidate));
    } catch (...) {
      try {
        std::lock_guard lock(mutex_);
        failure_ = "controlled-v4-inventory-allocation";
      } catch (...) {}
    }
  }

  const char* failure() const noexcept {
    try { std::lock_guard lock(mutex_); return failure_; }
    catch (...) { return "controlled-v4-inventory-lock"; }
  }
  std::vector<CpuGeneralTapeControlledV4InventorySource> takeSources() {
    std::lock_guard lock(mutex_);
    return std::move(sources_);
  }
  bool finishLogical(std::size_t sources, std::size_t commands) {
    std::lock_guard lock(mutex_);
    if (!logical_) return true;
    if (failure_ || !logical_->finish(sources, commands)) {
      if (!failure_) failure_ = "controlled-v4-logical-coverage-rejected";
      return false;
    }
    return true;
  }
  CpuGeneralTapeControlledV4LogicalSemanticResult takeLogicalResult() {
    std::lock_guard lock(mutex_);
    return logical_ ? logical_->takeResult() : CpuGeneralTapeControlledV4LogicalSemanticResult{};
  }

 private:
  std::shared_ptr<d3d9::RenderTapeRuntimeIdentityV4> root_{};
  render::TapeTopologyCaptureIdentity capture_{};
  const CpuTapeRecordContextStore& records_;
  std::shared_ptr<CpuTapeExpectedBufferBudget> budget_{};
  CpuV4ConsumerLimits limits_{};
  mutable std::mutex mutex_{};
  const char* failure_ = nullptr;
  std::size_t commands_ = 0u, origins_ = 0u;
  std::vector<CpuGeneralTapeControlledV4InventorySource> sources_{};
  std::unique_ptr<CpuTapeV4LogicalSemanticObserver> logical_{};
};

// Thrown only into the controlled backend's existing exception/fail-stop seam
// after the synchronous borrow observer returns and releases its own lock.
struct CpuGeneralTapeControlledV4InventoryRejected {};

// The shared production Present preparation converts a rejected fixture flush
// into its real failed HRESULT rather than accepting a no-op flush adapter.
struct CpuGeneralTapeControlledV4FlushRejected {};

}  // namespace detail

// Fixture is intentionally duck-typed: it must expose cDevice (unique_ptr
// D9CDevice), routing (the resource-backed RoutingDevice), and device (the
// shared core Device). Access is the test-only CommandQueue access facade.
template <class Fixture, class Access>
class CpuGeneralTapeExecutor {
 public:
  using Plan = dxmt9::d3d9::render_tape_runtime::GeneralPreflightPlan;
  using Blob = dxmt9::d3d9::RenderTapeProviderBlob;
  using Result = CpuGeneralTapeExecutionResult;

  enum class PendingIdentityFault : std::uint8_t {
    None,
    Sequence,
    SourceIndex,
    SourceGeneration,
    StorageFirstPage,
    StoragePageCount,
    StorageGeneration,
  };

  enum class CheckpointFault : std::uint8_t {
    None,
    MissingEnd,
    StaleLocalIssue,
    DoubleRestore,
  };

  enum class ContinuationIdentityFault : std::uint8_t {
    None,
    StaleWriterGeneration,
    ForeignQueue,
  };

  struct Options {
    dxmt9::d3d9::ProviderReplayPolicy route =
        dxmt9::d3d9::ProviderReplayPolicy::ProductionSlot;
    bool beforeNextWork = false;
    std::size_t maxPendingSources = 1u;
    // Native qualification until authenticated CLI schedule metadata exists.
    bool holdSafeBufferMutation = false;
    std::span<const CpuSourceOracleRow> sourceOracle{};
    // Native negative control only; never enabled by the accepted CLI lane.
    bool corruptRetainedBytesForTest = false;
    bool corruptRetainedBytesAfterMutationForTest = false;
    bool deferOneObjectReleaseForResetTest = false;
    bool dropRetiredAliasReceiptForTest = false;
    PendingIdentityFault pendingIdentityFault = PendingIdentityFault::None;
    CheckpointFault checkpointFault = CheckpointFault::None;
    ContinuationIdentityFault continuationIdentityFault = ContinuationIdentityFault::None;
    // Measurement keeps production replay and lifetime work intact while
    // omitting the cold semantic/resource oracle.  It is intentionally
    // constrained to immediate, single-source publication until the caller
    // has a qualified batching policy.
    bool measurement = false;
    // Optional pre-authenticated capture source plan. The caller owns this
    // object for the duration of run(); the executor only borrows its
    // immutable ranges and digest.
    const dxmt9::d3d9::CaptureSourceReplayPlan* captureSourcePlan = nullptr;
    const dxmt9::d3d9::CaptureSourceReplayPlanV3* captureSourcePlanV3 = nullptr;
    // Input bytes used to bind the pre-authenticated plan to this exact
    // GeneralPreflightPlan before any device effects. Both spans must outlive
    // run() when captureSourcePlan is set.
    std::span<const std::byte> captureSourceTape{};
    std::span<const std::byte> captureSourceIdentitySidecar{};
    CpuDiagnosticLedger* diagnostic = nullptr;
    // Oracle probes are disclosed separately from the recorded transcript.
    // A zero budget is useful to prove exhaustion without an implicit drain.
    std::uint32_t maxQueryOraclePolls = 4u;
    d3d9::render_tape_runtime::GeneralCpuAdmissionCaps inputCaps{};
    d3d9::render_tape_runtime::GeneralCpuSourceCapacity sourceCapacity{};
  };

  struct ControlledV4InventoryOptions {
    d3d9::ProviderReplayPolicy route = d3d9::ProviderReplayPolicy::Compatibility;
    CpuGeneralTapeControlledV4ExecutionMode executionMode = CpuGeneralTapeControlledV4ExecutionMode::Observe;
    const d3d9::CaptureSourceReplayPlanV4* sourcePlan = nullptr;
    std::span<const std::byte> exactTape{};
    ControlledCpuV4InputProfile inputProfile = ControlledCpuV4InputProfile::Raw256;
    CpuTapeExpectedBufferBudgetLimits expectedBufferBudget{512u * 1024u * 1024u};
    CpuV4ConsumerLimits consumerLimits{};
    d3d9::RenderTapeInitialSetupV4Limits initialSetupLimits{};
    std::chrono::steady_clock::time_point deadline{};
    std::uint32_t maxQueryOraclePolls = 64u;
    // Explicit authenticated launcher attestation, never inferred from a
    // captured publication or Present. NONE remains the default.
    d3d9::RenderTapeExecutionV4ProducerContract producerContract =
        d3d9::RenderTapeExecutionV4ProducerContract::None;
    bool observeLogicalSemantics = false;
    bool legalCandidateNoOverwriteContract = false;
    // Caller-qualified fresh fixture with automatic depth creation disabled.
    // This grants only initial RT1..3/DS null roles, never RT0 alias identity.
    bool freshNoAutoDepthSurfaceDefaults = false;
  };

  CpuGeneralTapeExecutor(Fixture& fixture, const Plan& plan,
                         std::span<const Blob> blobs, Options options = {})
      : fixture_(fixture), plan_(plan), blobs_(blobs), options_(options) {}

  CpuGeneralTapeControlledV4InventoryResult runControlledV4Inventory(
      ControlledV4InventoryOptions options) {
    namespace runtime = d3d9::render_tape_runtime;
    using Status = CpuTapeResourceContextStatus;
    using Object = runtime::OwnedObject;
    using Clock = std::chrono::steady_clock;
    CpuGeneralTapeControlledV4InventoryResult result(
        admitControlledCpuV4Input(plan_, options.inputProfile));
    result.executionMode = options.executionMode;
    const bool executionOnly = options.executionMode == CpuGeneralTapeControlledV4ExecutionMode::ExecutionOnly;
    result.executionOnly.requested = executionOnly;
    result.routeCoverage.requestedRoute = options.route;
    result.logicalSemantics.requested = options.observeLogicalSemantics;
    result.logicalSemantics.legalCandidateNoOverwriteContract = options.observeLogicalSemantics &&
        options.legalCandidateNoOverwriteContract;
    result.logicalSemantics.conditionalOnLegalNoOverwrite = result.logicalSemantics.legalCandidateNoOverwriteContract;
    result.events = plan_.events.size();
    const auto fail = [&](std::uint32_t event, const char* reason) {
      if (result.failure.empty()) {
        result.failure = reason;
        result.failedEvent = event;
      }
      return false;
    };
    if (!result.inputAdmission.accepted()) {
      fail(result.inputAdmission.eventIndex(), "controlled-v4-inventory-input-admission");
      return result;
    }
    if (options.executionMode != CpuGeneralTapeControlledV4ExecutionMode::Observe && !executionOnly) {
      fail(0xffffffffu, "controlled-v4-inventory-execution-mode");
      return result;
    }
    if (executionOnly && options.observeLogicalSemantics) {
      fail(0xffffffffu, "controlled-v4-execution-observer-contract");
      return result;
    }
    if (options.route != d3d9::ProviderReplayPolicy::Compatibility &&
        options.route != d3d9::ProviderReplayPolicy::ProductionSlot) {
      fail(0xffffffffu, "controlled-v4-inventory-route-policy");
      return result;
    }
    if (options.observeLogicalSemantics && !options.legalCandidateNoOverwriteContract) {
      result.logicalSemantics.failure = "controlled-v4-logical-legal-nooverwrite-contract-required";
      fail(0xffffffffu, result.logicalSemantics.failure.c_str());
      return result;
    }
    // This is a bounded live-budget attempt. Input acceptance cannot grant
    // source occupancy or promise a universal candidate emission upper bound.
    if (!options.sourcePlan || !options.sourcePlan->valid() || options.exactTape.empty() ||
        !options.consumerLimits.valid() ||
        options.consumerLimits.maxSources > options.consumerLimits.maxLifecycleActions / 4u ||
        options.deadline <= Clock::now() || options.maxQueryOraclePolls > 64u ||
        !fixture_.cDevice || !fixture_.routing || !fixture_.device ||
        fixture_.cDevice->replayOffload || !Access::cpuTapeWorkersDisabled(fixture_.routing->queue_) ||
        fixture_.routing->supportsGpuReadback()) {
      fail(0xffffffffu, "controlled-v4-inventory-session-contract");
      return result;
    }
    const bool productionSlot = options.route == d3d9::ProviderReplayPolicy::ProductionSlot;
    if (fixture_.routing->supportsDirectChunkSlotReplay() != productionSlot ||
        fixture_.routing->supportsCpuReadyArenaReplay() != !productionSlot) {
      fail(0xffffffffu, "controlled-v4-inventory-route-contract");
      return result;
    }
    result.routeCoverage.observed = perf::enabled();
    if (executionOnly && !result.routeCoverage.observed) {
      fail(0xffffffffu, "controlled-v4-execution-route-counters-disabled");
      return result;
    }
    if (options.observeLogicalSemantics && !result.routeCoverage.observed) {
      result.logicalSemantics.failure = "controlled-v4-logical-route-counters-disabled";
      fail(0xffffffffu, result.logicalSemantics.failure.c_str());
      return result;
    }
    if (fixture_.routing->flushHook) {
      fail(0xffffffffu, "controlled-v4-inventory-flush-adapter-preexisting");
      return result;
    }
    if (options_.measurement || options_.diagnostic || options_.captureSourcePlan ||
        options_.captureSourcePlanV3 || options_.beforeNextWork || options_.holdSafeBufferMutation ||
        !options_.sourceOracle.empty() || options_.dropRetiredAliasReceiptForTest ||
        options_.corruptRetainedBytesForTest || options_.corruptRetainedBytesAfterMutationForTest ||
        options_.deferOneObjectReleaseForResetTest ||
        options_.pendingIdentityFault != PendingIdentityFault::None ||
        options_.checkpointFault != CheckpointFault::None ||
        options_.continuationIdentityFault != ContinuationIdentityFault::None) {
      fail(0xffffffffu, "controlled-v4-inventory-legacy-options");
      return result;
    }
    d3d9::RenderTapeExecutionV4 execution;
    if (!execution.admit(*options.sourcePlan, plan_, options.exactTape, result.provenance,
            d3d9::RenderTapeExecutionV4PublicationMode::SemanticMapping, options.producerContract)) {
      fail(0xffffffffu, execution.failure());
      return result;
    }
    d3d9::RenderTapeInitialSetupV4Plan setupPlan;
    if (d3d9::buildRenderTapeInitialSetupV4(*options.sourcePlan, plan_, options.exactTape,
            execution.snapshotPrefixEvents(), setupPlan, options.initialSetupLimits) !=
        d3d9::RenderTapeInitialSetupV4Status::Valid) {
      fail(0xffffffffu, "controlled-v4-inventory-initial-setup-admission");
      return result;
    }
    std::shared_ptr<CpuTapeExpectedBufferBudget> budget;
    if (!executionOnly && createCpuTapeExpectedBufferBudget(options.expectedBufferBudget, budget) != Status::Accepted) {
      fail(0xffffffffu, "controlled-v4-inventory-expected-byte-budget");
      return result;
    }
    std::shared_ptr<const std::vector<CpuTapeV4OriginalSurfaceRoleEntry>> logicalRoles;
    if (options.observeLogicalSemantics && !buildCpuTapeV4OriginalSurfaceRoles(plan_,
            options.freshNoAutoDepthSurfaceDefaults, logicalRoles, result.logicalSemantics.failure)) {
      fail(0xffffffffu, result.logicalSemantics.failure.c_str());
      return result;
    }
    const auto& inputCaps = result.inputAdmission.inputCaps();
    std::optional<CpuTapeProducerResourceLedgerV4> versions;
    if (!executionOnly)
      versions.emplace(budget,
          CpuTapeProducerResourceLedgerV4Limits{inputCaps.maxObjects, inputCaps.maxTotalRecords},
          CpuTapeResourceContextPurpose::V4SurfaceAliasInventory);
    std::vector<Object> objects;
    std::vector<detail::LogicalReceipt> receipts;
    std::vector<CpuGeneralTapeTombstone> tombstones;
    struct QueryCycle {
      D9CWireObjectIdentity identity{};
      d3d9::RenderTapeQueryCycleState state{};
      core::QueryPollSnapshot snapshot{};
      std::uint64_t expectedLocalIssueOrdinal = 0u;
    };
    struct HiddenCheckpoint {
      Object object{};
      QueryCycle cycle{};
      std::size_t evidenceIndex = 0u;
      bool activated = false;
    };
    std::vector<QueryCycle> queries;
    std::vector<HiddenCheckpoint> checkpoints;
    struct SetupReleaseContext {
      CpuGeneralTapeControlledV4InventoryResult& result;
      static void release(void* opaque, Object& object) noexcept {
        if (!object.value) return;
        runtime::releaseOwnedObject(object);
        ++static_cast<SetupReleaseContext*>(opaque)->result.resourcesReleased;
      }
    } setupRelease{result};
    CpuInitialSetupV4 setup({&setupRelease, &SetupReleaseContext::release});
    d3d9::RenderTapeControlledCpuV4Capability capability;
    using ConsumerHandle = decltype(fixture_.makeControlledCpuConsumer(
        capability, result.provenance.localArm, options.consumerLimits, options.deadline));
    ConsumerHandle handle;
    std::shared_ptr<detail::CpuGeneralTapeControlledV4InventoryObserver> inventory;
    core::Handle expectedUploadHandle{};
    std::uint32_t expectedUploadLevel = 0u;
    detail::CpuGeneralTextureUploadShape expectedUploadShape{};
    std::span<const std::byte> expectedUploadBytes;
    bool observedUpload = false, bootstrapDone = false, bootstrapPending = false;
    bool armed = false, cleaned = false, controlledFlushInstalled = false;
    bool consumerEvidenceTaken = false;
    bool routeStarted = false;
    perf::RenderTapeReplaySnapshot routeBefore{};
    struct ClockStart {
      Clock::time_point wall{};
      std::optional<std::uint64_t> processCpu{};
      bool active = false;
    } setupClock, bodyClock, teardownClock;
    const auto sampleProcessCpu = []() noexcept -> std::optional<std::uint64_t> {
#if defined(CLOCK_PROCESS_CPUTIME_ID)
      timespec value{};
      if (::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) != 0 ||
          value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= 1000000000l)
        return std::nullopt;
      constexpr std::uint64_t scale = 1000000000u;
      const auto seconds = static_cast<std::uint64_t>(value.tv_sec);
      const auto nanoseconds = static_cast<std::uint64_t>(value.tv_nsec);
      if (seconds > (std::numeric_limits<std::uint64_t>::max() - nanoseconds) / scale)
        return std::nullopt;
      return seconds * scale + nanoseconds;
#else
      return std::nullopt;
#endif
    };
    const auto startClock = [&](CpuGeneralTapeControlledV4ClockScope& scope, ClockStart& start) noexcept {
      if (!executionOnly) return true;
      scope.started = true;
      start.wall = Clock::now();
      start.processCpu = sampleProcessCpu();
      start.active = true;
      return start.processCpu.has_value();
    };
    const auto stopClock = [&](CpuGeneralTapeControlledV4ClockScope& scope,
                               ClockStart& start, bool complete) noexcept {
      if (!executionOnly || !start.active) return true;
      const auto wall = Clock::now();
      const auto cpu = sampleProcessCpu();
      start.active = false;
      const auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(wall - start.wall).count();
      if (duration >= 0) scope.wallNs = static_cast<std::uint64_t>(duration);
      scope.valid = duration >= 0 && start.processCpu && cpu && *cpu >= *start.processCpu;
      if (scope.valid) scope.processCpuNs = *cpu - *start.processCpu;
      scope.complete = scope.valid && complete;
      return scope.valid;
    };
    std::size_t publicationCursor = 0u, canonicalPayloadBytes = 0u;
    std::vector<core::metalqueue::ReplaySourcePublicationReceipt> publications;
    std::optional<core::metalqueue::ReplaySourcePublicationReceipt> lastPublication;
    const auto nativeHandle = [](const Object& object) -> core::Handle {
      if (!object.value) return {};
      if (object.identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER) {
        const auto* value = static_cast<const D9CBuffer*>(object.value);
        return value->obj ? value->obj->handle() : core::Handle{};
      }
      if (object.identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
        const auto* value = static_cast<const D9CTexture*>(object.value);
        return value->obj ? value->obj->handle() : core::Handle{};
      }
      if (object.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE) {
        const auto* value = static_cast<const D9CSurface*>(object.value);
        return value->obj ? value->obj->handle() : core::Handle{};
      }
      return {};
    };
    const auto clearControlledFlushAdapter = [&]() noexcept {
      if (!controlledFlushInstalled) return;
      fixture_.routing->flushHook = {};
      controlledFlushInstalled = false;
    };
    const auto cleanup = [&]() noexcept {
      if (cleaned) return;
      cleaned = true;
      // Join/poison before revocation, observer detachment or wrapper release.
      // Quarantine is not a successful drain/reset and never fabricates ACKs.
      try { if (handle) handle->cancelAndJoin(); } catch (...) {}
      stopClock(result.clocks.body, bodyClock, false);
      stopClock(result.clocks.setup, setupClock, false);
      startClock(result.clocks.teardown, teardownClock);
      clearControlledFlushAdapter();
      execution.cancel();
      capability.revoke();
      setup.releaseHidden();
      for (auto& owner : checkpoints) {
        if (owner.object.value) {
          runtime::releaseOwnedObject(owner.object);
          ++result.resourcesReleased;
        }
      }
      for (auto it = objects.rbegin(); it != objects.rend(); ++it) {
        if (!it->value) continue;
        const auto native = nativeHandle(*it);
        if (native) {
          try { tombstones.push_back({it->identity, native}); } catch (...) {}
        }
        runtime::releaseOwnedObject(*it);
        ++result.resourcesReleased;
      }
      fixture_.routing->generalTextureUpload = {};
      try {
        fixture_.releaseCoreForInspection();
        Access::reclaimCompletedPool(fixture_.routing->queue_);
        bool resourcesGone = true;
        for (const auto& retired : tombstones) {
          const auto& pool = fixture_.routing->queue_.pool();
          if (retired.identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER)
            resourcesGone &= !pool.findBuffer(retired.nativeHandle.value);
          else if (retired.identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE)
            resourcesGone &= !pool.findTexture(retired.nativeHandle.value);
          else if (retired.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE)
            resourcesGone &= !pool.findSurface(retired.nativeHandle.value);
        }
        result.resetOk = resourcesGone && !Access::readyCount(fixture_.routing->queue_) &&
            !Access::pendingCompletionCount(fixture_.routing->queue_) &&
            !Access::writingCommandCount(fixture_.routing->queue_) &&
            !Access::residentCount(fixture_.routing->queue_) &&
            !Access::controlledCpuQueueStopped(fixture_.routing->queue_);
      } catch (...) { result.resetOk = false; }
      result.initialSetup = setup.evidence();
      if (versions) result.recordContexts = versions->recordCount();
      if (budget) result.expectedBufferBudget = budget->snapshot();
      stopClock(result.clocks.teardown, teardownClock, result.resetOk && result.failure.empty());
    };
    struct CleanupGuard {
      const decltype(cleanup)& action;
      ~CleanupGuard() { action(); }
    } cleanupGuard{cleanup};
    const auto live = [&](std::uint32_t event) {
      if (Clock::now() >= options.deadline) return fail(event, "controlled-v4-inventory-deadline");
      if (inventory && inventory->failure()) return fail(event, inventory->failure());
      if (handle && handle->consumer->snapshot().failure != CpuV4ConsumerFailure::None)
        return fail(event, "controlled-v4-inventory-consumer-failed");
      return true;
    };
    const auto mutate = [&](std::uint32_t event, const auto& mutation,
                            std::span<const Object> owners) {
      return replayCpuMutation(event, mutation, owners, receipts, result,
          expectedUploadHandle, expectedUploadLevel, expectedUploadShape,
          expectedUploadBytes, observedUpload, fail, !executionOnly);
    };
    const auto normalize = [&](std::uint32_t event, const Object& object) {
      if (executionOnly) return true;
      const auto native = nativeHandle(object);
      if (!native) return true; // Queries/shaders/declarations are not resources.
      CpuTapeResourceObservation observation;
      if (!captureResourceObservation(objects, receipts, tombstones,
              object.identity.kind, native, observation, &object.identity))
        return fail(event, "controlled-v4-inventory-resource-normalization");
      const auto previous = versions->find(object.identity);
      const auto versionFailure = [&](const char* stage, Status status) {
        CpuGeneralTapeControlledV4ResourceVersionFailure facts;
        facts.stage = stage;
        facts.requestedIdentity = object.identity;
        facts.normalizedIdentity = observation.identity();
        facts.nativeKind = object.identity.kind;
        facts.nativeHandle = native;
        facts.priorLogicalContentEpoch = previous.version ? previous.version->contentEpoch() : 0u;
        facts.status = status;
        for (const auto& candidate : objects) {
          const auto installed = versions->find(candidate.identity);
          if (installed.version && installed.version->nativeKind() == object.identity.kind &&
              installed.version->nativeHandle() == native)
            facts.installedNativeOwners.push_back(installed.version->identity());
        }
        result.resourceVersionFailure = std::move(facts);
        return fail(event, "controlled-v4-inventory-version-conflict");
      };
      if (!detail::cpuGeneralSameIdentity(observation.identity(), object.identity))
        return versionFailure("normalized-identity", Status::Conflict);
      if (previous.version && previous.version->contentEpoch() == std::numeric_limits<std::uint64_t>::max())
        return fail(event, "controlled-v4-inventory-content-epoch");
      const auto epoch = previous.version ? previous.version->contentEpoch() + 1u : 1u;
      const auto* buffer = object.identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER
          ? static_cast<const D9CBuffer*>(object.value) : nullptr;
      // One producer-side arena observation, after the real successful
      // definition/mutation and before immutable version/context construction.
      // Facts become part of that exact historical version, not a consumer
      // lookup in the mutable current ledger. A failed observation is absent;
      // local revision still does not authenticate the logical content epoch.
      std::optional<resources::BufferVersionObservation> localFacts;
      if (buffer) {
        const auto actual = fixture_.routing->queue_.pool().observeBufferVersion(native);
        result.bufferVersions.push_back({object.identity, epoch, actual});
        if (actual.observed()) { localFacts = actual; ++result.bufferVersionsObserved; }
        else ++result.bufferVersionsUnavailable; // Explicit absence, no backing/byte claim.
      }
      const CpuTapeResourceVersionInput input{.nativeKind = object.identity.kind,
          .nativeHandle = native, .contentEpoch = epoch,
          .expectedBufferByteSize = buffer ? buffer->desc.size : 0u,
          .expectedBufferBytes = buffer ? buffer->obj->bytes() : std::span<const std::uint8_t>{},
          .localBufferFacts = localFacts};
      std::shared_ptr<const CpuTapeResourceVersion> version;
      if (buildBudgetedCpuTapeResourceVersion(input, observation, budget, version) != Status::Accepted)
        return fail(event, "controlled-v4-inventory-version-budget-or-allocation");
      const auto status = previous.version
          ? versions->replace(object.identity, previous.version->contentEpoch(), version)
          : versions->define(version);
      return status == Status::Accepted || versionFailure(previous.version ? "replace" : "define", status);
    };
    const auto refreshMutation = [&](std::uint32_t event, D9CWireObjectIdentity identity) {
      if (executionOnly) return true;
      for (const auto& object : objects) {
        if (!object.value) continue;
        bool affected = detail::cpuGeneralSameIdentity(object.identity, identity);
        if (object.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE) {
          const auto* surface = static_cast<const D9CSurface*>(object.value);
          if (surface->ownerTex) {
            const auto definition = std::find_if(plan_.definitions.begin(), plan_.definitions.end(),
                [&](const auto& candidate) {
                  return detail::cpuGeneralSameIdentity(candidate.fixed.identity, object.identity);
                });
            d3d9::RenderTapeSurfaceDescriptorV2 descriptor{};
            if (definition == plan_.definitions.end() ||
                !d3d9::renderTapeLoadSurfaceDescriptorV2(definition->descriptor, descriptor))
              return fail(event, "controlled-v4-inventory-alias-descriptor");
            affected |= detail::cpuGeneralSameIdentity(descriptor.parentTexture, identity);
          }
        }
        if (affected && !normalize(event, object)) return false;
      }
      return true;
    };
    const auto bootstrap = [&](std::uint32_t event) {
      if (bootstrapDone) return true;
      std::vector<Object> owners(objects.begin(), objects.end());
      for (const auto& hidden : setup.hiddenObjects()) if (hidden.value) owners.push_back(hidden);
      for (const auto& chunk : plan_.bootstrapChunks) {
        std::vector<void*> resolved(chunk.envelope.handleCount);
        std::size_t count = 0u;
        if (!runtime::resolveCommandHandles(chunk, owners, resolved, count) ||
            d3d9::replayPrevalidatedResolvedCommandChunk(fixture_.cDevice.get(), chunk.bytes,
                chunk.envelope, std::span<void* const>(resolved.data(), count),
                options.route) != core::D3D_OK)
          return fail(event, "controlled-v4-inventory-bootstrap");
        result.commands += chunk.envelope.recordCount;
      }
      bootstrapDone = true;
      return true;
    };
    const auto pullPublications = [&](std::uint32_t event) {
      if (!execution.publishedSources(publicationCursor, publications)) return fail(event, execution.failure());
      if (!publications.empty()) lastPublication = publications.back();
      return live(event);
    };
    const auto waitLastPublication = [&](std::uint32_t event) {
      if (!pullPublications(event)) return false;
      if (!lastPublication) return true;
      const auto& receipt = *lastPublication;
      const CpuV4ConsumerIdentity target{.queue = result.provenance.localArm.queue,
          .captureToken = result.provenance.localArm.captureToken,
          .captureEpoch = result.provenance.localArm.captureEpoch,
          .sequence = receipt.metadata.seqId, .sourceIndex = receipt.source.id.index,
          .sourceGeneration = receipt.source.id.generation,
          .storageFirstPage = receipt.source.storage.firstPage,
          .storagePageCount = receipt.source.storage.pageCount,
          .storageGeneration = receipt.source.storage.generation};
      return handle && handle->consumer->waitThrough(target, options.deadline)
          ? live(event) : fail(event, "controlled-v4-inventory-publication-wait");
    };
    fixture_.routing->controlledQueries = result.inputAdmission.counts().queryDefinitions != 0u;
    fixture_.routing->attachmentOwner = AttachmentContentOwner::BackendCommands;
    if (executionOnly) {
      fixture_.routing->recordBoundedCapturedBindings = false;
      auto& omitted = result.executionOnly;
      omitted.resourceVersionsOmitted = omitted.recordContextsOmitted = omitted.expectedImagesOmitted = true;
      omitted.canonicalDrawCopiesOmitted = omitted.payloadObserverOmitted = omitted.sourceRowReadsOmitted = true;
      omitted.logicalHashingOmitted = omitted.diagnosticMutationReceiptsOmitted = true;
      omitted.capturedBindingImagesOmitted = omitted.uploadByteComparisonsOmitted = true;
    }
    fixture_.routing->generalTextureUpload = [&](core::TextureHandle handle, std::uint32_t level,
        std::uint32_t width, std::uint32_t height, std::uint32_t depth, std::uint32_t pitch,
        std::uint32_t slicePitch, std::span<const std::uint8_t> bytes) {
      const bool exact = handle == expectedUploadHandle && level == expectedUploadLevel &&
          detail::cpuGeneralTextureUploadShapeMatches(expectedUploadShape,
              width, height, depth, pitch, slicePitch, bytes.size()) &&
          bytes.size() == expectedUploadBytes.size() && !bytes.empty() &&
          (executionOnly || std::memcmp(bytes.data(), expectedUploadBytes.data(), bytes.size()) == 0);
      observedUpload |= exact;
      result.observedTextureUploads += exact;
      return exact;
    };
    try {
      objects.reserve(plan_.definitions.size());
      if (!executionOnly) receipts.reserve(plan_.mutations.size());
      tombstones.reserve(plan_.definitions.size());
      queries.reserve(result.inputAdmission.counts().queryDefinitions);
      checkpoints.reserve(result.inputAdmission.counts().queryDefinitions);
      auto& queryEvidence = result.queryReplay;
      queryEvidence.present = result.inputAdmission.counts().queryDefinitions != 0u;
      queryEvidence.queryDefinitions = result.inputAdmission.counts().queryDefinitions;
      queryEvidence.issueRecords = result.inputAdmission.counts().queryIssues;
      queryEvidence.recordedPolls = result.inputAdmission.counts().queryPolls;
      queryEvidence.maxOraclePolls = options.maxQueryOraclePolls *
          static_cast<std::uint32_t>(queryEvidence.recordedPolls);
      if (!startClock(result.clocks.setup, setupClock)) {
        fail(0xffffffffu, "controlled-v4-execution-clock");
      }
      if (result.failure.empty() && (!setup.adopt(std::move(setupPlan), options.sourcePlan->digest(), options.initialSetupLimits) ||
          !setup.prepare([&](const auto& entry, std::span<const Object> hidden) {
            if (!live(entry.definitionEventIndex)) return static_cast<void*>(nullptr);
            auto* value = runtime::createRenderTapeObject(fixture_.cDevice.get(),
                entry.definition, entry.descriptor, blobs_, hidden);
            result.resourcesCreated += value != nullptr;
            return value;
          }, [&](const auto&, const auto& seed, const Object& object) {
            return live(seed.eventIndex) && mutate(seed.eventIndex, seed.mutation, {&object, 1u});
          }))) {
        fail(0xffffffffu, "controlled-v4-inventory-hidden-setup");
      }
      // Separate genuine pre-arm EVENT setup. This is not interval source
      // replay, and its actual receipts are never projected as captured work.
      if (result.failure.empty()) for (const auto& definition : plan_.definitions) {
        if (definition.fixed.identity.kind != D9C_CHUNK_HANDLE_KIND_QUERY) continue;
        d3d9::RenderTapeQueryDescriptorDecoded descriptor{};
        if (!d3d9::renderTapeLoadQueryDescriptor(definition.descriptor, descriptor)) {
          fail(0xffffffffu, "controlled-v4-inventory-checkpoint-descriptor"); break;
        }
        if (descriptor.initialState != static_cast<std::uint32_t>(
                d3d9::RenderTapeQueryInitialState::CompletedEventCheckpoint)) continue;
        auto* query = static_cast<D9CQuery*>(runtime::createRenderTapeObject(fixture_.cDevice.get(),
            definition.fixed, definition.descriptor, blobs_, objects));
        if (!query || !query->obj) { fail(0xffffffffu, "controlled-v4-inventory-checkpoint-create"); break; }
        ++result.resourcesCreated;
        ++queryEvidence.checkpointDefinitions;
        checkpoints.push_back({.object = {definition.fixed.identity, query},
            .cycle = {.identity = definition.fixed.identity,
                .state = d3d9::renderTapeInitialQueryCycle(descriptor)},
            .evidenceIndex = queryEvidence.checkpoints.size()});
        auto& owner = checkpoints.back();
        const auto before = fixture_.routing->queryProgress();
        if (dxmt9c_query_issue(query, 1u) != core::D3D_OK) {
          fail(0xffffffffu, "controlled-v4-inventory-checkpoint-end"); break;
        }
        ++queryEvidence.checkpointSetupEnds;
        owner.cycle.expectedLocalIssueOrdinal = 1u;
        owner.cycle.snapshot = fixture_.cDevice->dev().captureQueryPollSnapshot(query->obj);
        const auto& snapshot = owner.cycle.snapshot;
        const auto fence = snapshot.backendFence.snapshot();
        const auto after = fixture_.routing->queryProgress();
        queryEvidence.checkpointSetupPublications += static_cast<std::uint32_t>(after.published - before.published);
        d3d9::RenderTapeQueryCheckpointReplayRow row{.identity = definition.fixed.identity,
            .capturedIssueCycle = descriptor.checkpoint.issueCycle,
            .capturedIssueOrdinal = descriptor.checkpoint.issueOrdinal,
            .capturedDeviceEpoch = descriptor.checkpoint.deviceEpoch,
            .localQueryIdentity = snapshot.identity, .localIssueOrdinal = snapshot.issueOrdinal,
            .localDeviceEpoch = snapshot.issueDeviceEpoch, .localQueue = fence.queue,
            .localSequence = fence.sequence, .localInitializer = fence.initializer, .setupIssued = true};
        if (!snapshot.eligible || snapshot.active || snapshot.failed || snapshot.issueOrdinal != 1u ||
            snapshot.identity != query->obj->identity() || !snapshot.issueDeviceEpoch || !fence.queue) {
          fail(0xffffffffu, "controlled-v4-inventory-checkpoint-authority"); break;
        }
        auto hr = fixture_.cDevice->dev().pollQuerySnapshot(snapshot, nullptr, 0u, 0u);
        ++row.setupPolls; ++queryEvidence.checkpointSetupPolls;
        while (hr == core::S_FALSE && row.setupPolls <= options.maxQueryOraclePolls && live(0xffffffffu)) {
          if (!Access::peekReadyCompletion(fixture_.routing->queue_).seqId) {
            fail(0xffffffffu, "controlled-v4-inventory-checkpoint-progress-unavailable"); break;
          }
          const auto source = Access::submitOneWithheld(fixture_.routing->queue_);
          if (!source.submitted) { fail(0xffffffffu, "controlled-v4-inventory-checkpoint-submit"); break; }
          ++result.setupSubmissions;
          if (!Access::acknowledgeOne(fixture_.routing->queue_)) {
            fail(0xffffffffu, "controlled-v4-inventory-checkpoint-acknowledge"); break;
          }
          ++result.setupAcknowledgements;
          if (!Access::finishQueue(fixture_.routing->queue_)) {
            fail(0xffffffffu, "controlled-v4-inventory-checkpoint-fifo"); break;
          }
          Access::reclaimCompletedPool(fixture_.routing->queue_);
          ++queryEvidence.checkpointSetupWaits;
          ++row.setupPolls; ++queryEvidence.checkpointSetupPolls;
          hr = fixture_.cDevice->dev().pollQuerySnapshot(snapshot, nullptr, 0u, 0u);
        }
        const auto progress = fixture_.routing->queryProgress();
        row.completedSequence = progress.completed;
        row.completedInitializer = progress.initializerCompleted;
        if (!result.failure.empty() || hr != core::D3D_OK ||
            dxmt9::query::classifyEnd(snapshot.backendFence, progress) != dxmt9::query::Poll::Complete) {
          fail(0xffffffffu, "controlled-v4-inventory-checkpoint-incomplete"); break;
        }
        row.completed = true;
        ++queryEvidence.checkpointCompleted;
        queryEvidence.checkpoints.push_back(row);
      }
      // Hidden host preparation and the genuine completed EVENT setup must
      // join before ANY managed original definition/seed activation. The
      // later pre-arm bootstrap boundary performs its own live quiescence
      // check; it cannot repeat or retroactively grant this ownership join.
      if (result.failure.empty() && !setup.joinHostCheckpoint([&] {
            return !Access::readyCount(fixture_.routing->queue_) &&
                !Access::writingCommandCount(fixture_.routing->queue_) &&
                !Access::pendingCompletionCount(fixture_.routing->queue_) &&
                !Access::residentCount(fixture_.routing->queue_);
          })) fail(0xffffffffu, "controlled-v4-inventory-hidden-host-checkpoint");
      const auto replayPoll = [&](std::uint32_t event, std::uint32_t index) {
        if (index >= plan_.controls.size() || index >= plan_.controlBytes.size() ||
            plan_.controlBytes[index].size() != sizeof(d3d9::RenderTapeQueryGetDataControlV2))
          return fail(event, "controlled-v4-inventory-query-control");
        const auto& fixed = plan_.controls[index];
        d3d9::RenderTapeQueryGetDataControlV2 poll{};
        std::memcpy(&poll, plan_.controlBytes[index].data(), sizeof(poll));
        const auto cycle = std::find_if(queries.begin(), queries.end(), [&](const auto& value) {
          return detail::cpuGeneralSameIdentity(value.identity, fixed.identity);
        });
        auto* query = static_cast<D9CQuery*>(runtime::findExactObject(objects, fixed.identity));
        if (cycle == queries.end() || !query || !query->obj ||
            !d3d9::renderTapeQueryPollCycleAdmitted(cycle->state, poll.issueCycle, fixed.resultCode))
          return fail(event, "controlled-v4-inventory-query-cycle");
        const auto snapshot = fixture_.cDevice->dev().captureQueryPollSnapshot(query->obj);
        if (!snapshot.eligible || snapshot.identity != query->obj->identity() ||
            snapshot.issueOrdinal != cycle->expectedLocalIssueOrdinal ||
            (snapshot.issueOrdinal && snapshot.issueDeviceEpoch != cycle->snapshot.issueDeviceEpoch))
          return fail(event, "controlled-v4-inventory-query-snapshot");
        d3d9::RenderTapeQueryReplayRow row{.eventIndex = event,
            .eventOrdinal = plan_.tape.event(event).header.ordinal, .identity = fixed.identity,
            .issueCycle = poll.issueCycle, .flags = poll.flags, .hasOutput = poll.hasOutput != 0u,
            .dataSize = poll.dataSize, .capturedResult = fixed.resultCode,
            .definedBytes = poll.definedBytes, .capturedBytes = poll.result,
            .resultRequired = fixed.resultCode == core::D3D_OK};
        row.localQueryIdentity = snapshot.identity;
        row.localIssueOrdinal = snapshot.issueOrdinal;
        row.localDeviceEpoch = snapshot.issueDeviceEpoch;
        const auto fence = snapshot.backendFence.snapshot();
        row.localQueue = fence.queue; row.localSequence = fence.sequence; row.localInitializer = fence.initializer;
        std::array<std::byte, 8> output;
        output.fill(std::byte{0xa5});
        row.replayResult = dxmt9c_query_get_data(query, poll.hasOutput ? output.data() : nullptr,
            poll.dataSize, poll.flags);
        ++queryEvidence.replayedPolls;
        queryEvidence.capturedPending += fixed.resultCode == core::S_FALSE;
        queryEvidence.capturedReady += fixed.resultCode == core::D3D_OK;
        queryEvidence.replayPending += row.replayResult == core::S_FALSE;
        queryEvidence.replayReady += row.replayResult == core::D3D_OK;
        row.oracleResult = row.replayResult;
        if ((row.replayResult < 0 && fixed.resultCode >= 0) ||
            (fixed.resultCode < 0 && row.replayResult != fixed.resultCode) ||
            (row.replayResult != core::D3D_OK && std::any_of(output.begin(), output.end(),
                [](auto byte) { return byte != std::byte{0xa5}; })))
          return fail(event, "controlled-v4-inventory-query-result");
        if (row.resultRequired) {
          ++queryEvidence.readinessRequired;
          queryEvidence.requiredResults += poll.definedBytes != 0u;
          if (row.oracleResult == core::S_FALSE && options.maxQueryOraclePolls) {
            // Only the already-published genuine prefix can be waited here.
            // There is no raw ACK/after-raw wait inside a record callback.
            if (!waitLastPublication(event)) return false;
            ++row.oraclePolls; ++queryEvidence.supplementalPolls;
            row.supplemental = true;
            row.oracleResult = fixture_.cDevice->dev().pollQuerySnapshot(snapshot,
                poll.hasOutput ? output.data() : nullptr, poll.dataSize, poll.flags);
          }
          const auto progress = fixture_.routing->queryProgress();
          if (row.oracleResult != core::D3D_OK ||
              (!snapshot.constant() && !snapshot.preIssue() &&
                  dxmt9::query::classifyEnd(snapshot.backendFence, progress) != dxmt9::query::Poll::Complete))
            return fail(event, "controlled-v4-inventory-query-progress-incomplete");
          row.completedSequence = progress.completed;
          row.completedInitializer = progress.initializerCompleted;
          row.readinessSatisfied = true;
          ++queryEvidence.readinessSatisfied;
          row.resultSubstituted = cycle->state.type == 9u && poll.definedBytes != 0u;
          queryEvidence.substitutedResults += row.resultSubstituted;
          if (poll.definedBytes) {
            std::copy_n(output.begin(), poll.definedBytes, row.replayBytes.begin());
            if (!row.resultSubstituted) {
              row.resultMatched = std::equal(poll.result.begin(), poll.result.begin() + poll.definedBytes,
                  output.begin());
              if (!row.resultMatched) return fail(event, "controlled-v4-inventory-query-bytes");
              ++queryEvidence.matchedResults;
            }
          }
        }
        queryEvidence.rows.push_back(row);
        return true;
      };
      for (const auto& event : plan_.events) {
        if (!result.failure.empty() || !live(event.eventIndex)) break;
        if (event.eventIndex == execution.snapshotPrefixEvents()) {
          if (!bootstrap(event.eventIndex) || Access::readyCount(fixture_.routing->queue_) ||
              Access::writingCommandCount(fixture_.routing->queue_) ||
              Access::pendingCompletionCount(fixture_.routing->queue_) ||
              Access::residentCount(fixture_.routing->queue_)) {
            fail(event.eventIndex, "controlled-v4-inventory-pre-arm-quiescence"); break;
          }
          for (const auto& object : objects)
            if (object.value && !normalize(event.eventIndex, object)) break;
          if (!result.failure.empty()) break;
          // Setup is quiescent before this snapshot. The same original body
          // runs once, and the final snapshot follows the consumer join.
          if (result.routeCoverage.observed) routeBefore = perf::snapshotRenderTapeReplay();
          routeStarted = true;
          std::vector<d3d9::RenderTapeExecutionV4QueryBaselineInput> baselines;
          for (const auto& owner : checkpoints) {
            auto* query = owner.object.value ? static_cast<D9CQuery*>(owner.object.value)
                : static_cast<D9CQuery*>(runtime::findExactObject(objects, owner.object.identity));
            baselines.push_back({owner.object.identity, query, owner.cycle.snapshot});
          }
          capability = fixture_.issueControlledCpuCapability();
          if (!execution.armControlledCpu(fixture_.cDevice.get(), capability, baselines)) {
            fail(event.eventIndex, execution.failure()); break;
          }
          armed = true;
          const auto& arm = result.provenance.localArm;
          if (executionOnly) {
            handle = fixture_.makeControlledCpuConsumer(capability, arm, options.consumerLimits, options.deadline);
          } else {
            inventory = std::make_shared<detail::CpuGeneralTapeControlledV4InventoryObserver>(
                d3d9::acquireRenderTapeRuntimeIdentityV4(fixture_.cDevice.get()),
                render::TapeTopologyCaptureIdentity{arm.captureToken, arm.captureEpoch, arm.queue},
                versions->recordContexts(), budget, options.consumerLimits, logicalRoles,
                options.observeLogicalSemantics, options.legalCandidateNoOverwriteContract);
            handle = fixture_.makeControlledCpuConsumer(capability, arm, options.consumerLimits, options.deadline,
                [observer = inventory](auto& queue, const auto& publication, const auto& payload) {
                  observer->observe(queue, publication, payload);
                  if (observer->failure()) throw detail::CpuGeneralTapeControlledV4InventoryRejected{};
                });
          }
          if (!handle || !handle->start()) { fail(event.eventIndex, "controlled-v4-inventory-consumer-start"); break; }
          if (options.producerContract == d3d9::RenderTapeExecutionV4ProducerContract::PePresentReservationV1) {
            // RoutingDevice's default flush is deliberately inert. Only this
            // live controlled session adapts the original production flush to
            // the real queue; its concurrent consumer services publication and
            // completion while submitFlush waits unchanged.
            fixture_.routing->flushHook = [this, &capability] {
              struct Context {
                D9CDevice* expectedDevice;
                dxmt9::Device* expectedBackend;
              } context{fixture_.cDevice.get(), fixture_.routing};
              const bool flushed = capability.lease().visitBackend(
                  d3d9::RenderTapeControlledCpuV4Boundary::Replay, &context,
                  [](void* opaque, D9CDevice* actual, dxmt9::Device& backend) noexcept {
                    const auto& input = *static_cast<Context*>(opaque);
                    if (actual != input.expectedDevice || &backend != input.expectedBackend) return false;
                    try {
                      backend.queue().submitFlush();
                      return true;
                    } catch (...) { return false; }
                  });
              if (!flushed) throw detail::CpuGeneralTapeControlledV4FlushRejected{};
            };
            controlledFlushInstalled = true;
          }
          if (!stopClock(result.clocks.setup, setupClock, true) ||
              !startClock(result.clocks.body, bodyClock)) {
            fail(event.eventIndex, "controlled-v4-execution-clock"); break;
          }
        }
        if (!execution.beginEvent(event.eventIndex)) { fail(event.eventIndex, execution.failure()); break; }
        const bool dispatched = detail::dispatchCpuGeneralTapeEvent(event.type, [&] {
          bootstrapPending = true;
          for (const auto& gamma : plan_.bootstrapGamma) {
            if (gamma.empty()) continue;
            if (gamma.size() != d3d9::kRenderTapeGammaRampBytes)
              return fail(event.eventIndex, "controlled-v4-inventory-gamma");
            dxmt9c_device_set_gamma_ramp(fixture_.cDevice.get(),
                reinterpret_cast<const std::uint16_t*>(gamma.data()));
          }
          return true;
        }, [&] {
          if (event.index >= plan_.definitions.size()) return fail(event.eventIndex, "controlled-v4-inventory-definition");
          const auto& definition = plan_.definitions[event.index];
          auto managed = setup.activate(event.index, event.eventIndex, definition, objects);
          if (managed == CpuInitialSetupV4EventResult::Rejected)
            return fail(event.eventIndex, "controlled-v4-inventory-activation");
          if (managed == CpuInitialSetupV4EventResult::NotManaged) {
            auto hidden = std::find_if(checkpoints.begin(), checkpoints.end(), [&](const auto& owner) {
              return detail::cpuGeneralSameIdentity(owner.object.identity, definition.fixed.identity);
            });
            void* value = nullptr;
            if (hidden != checkpoints.end()) {
              if (hidden->activated || !hidden->object.value)
                return fail(event.eventIndex, "controlled-v4-inventory-query-double-activation");
              objects.push_back(hidden->object);
              value = std::exchange(hidden->object.value, nullptr);
              hidden->activated = true;
              queries.push_back(hidden->cycle);
              ++queryEvidence.checkpointActivated;
              queryEvidence.checkpoints[hidden->evidenceIndex].activated = true;
            } else {
              value = runtime::createRenderTapeObject(fixture_.cDevice.get(), definition.fixed,
                  definition.descriptor, blobs_, objects);
              if (!value) return fail(event.eventIndex, "controlled-v4-inventory-object-create");
              objects.push_back({definition.fixed.identity, value});
              ++result.resourcesCreated;
              if (definition.fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_QUERY) {
                d3d9::RenderTapeQueryDescriptorDecoded descriptor{};
                if (!d3d9::renderTapeLoadQueryDescriptor(definition.descriptor, descriptor))
                  return fail(event.eventIndex, "controlled-v4-inventory-query-descriptor");
                queries.push_back({.identity = definition.fixed.identity,
                    .state = d3d9::renderTapeInitialQueryCycle(descriptor)});
              }
            }
            if (definition.fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_QUERY &&
                !execution.bindQueryObject(fixture_.cDevice.get(), definition.fixed.identity,
                    static_cast<D9CQuery*>(value))) return fail(event.eventIndex, execution.failure());
          }
          const auto alias = setup.qualifyAlias(event.index, event.eventIndex, definition, objects);
          if (alias == CpuInitialSetupV4EventResult::Rejected)
            return fail(event.eventIndex, "controlled-v4-inventory-alias-activation");
          return !armed || normalize(event.eventIndex, objects.back());
        }, [&] {
          if (event.index >= plan_.mutations.size()) return fail(event.eventIndex, "controlled-v4-inventory-mutation");
          const auto& mutation = plan_.mutations[event.index];
          const auto managed = setup.consumeSeed(event.index, event.eventIndex, mutation, objects);
          if (managed == CpuInitialSetupV4EventResult::Rejected)
            return fail(event.eventIndex, "controlled-v4-inventory-seed-consumption");
          if (managed == CpuInitialSetupV4EventResult::Accepted) return true;
          return mutate(event.eventIndex, mutation, objects) &&
              (!armed || refreshMutation(event.eventIndex, mutation.identity));
        }, [&] {
          if (!armed || event.index >= plan_.commandChunks.size())
            return fail(event.eventIndex, "controlled-v4-inventory-raw-boundary");
          if (bootstrapPending && !bootstrap(event.eventIndex)) return false;
          const auto& chunk = plan_.commandChunks[event.index];
          d3d9::ImportedChunkView imported;
          if (!d3d9::importPrevalidatedCommandChunk(chunk.bytes, chunk.envelope, imported))
            return fail(event.eventIndex, "controlled-v4-inventory-raw-import");
          std::vector<void*> resolved(chunk.envelope.handleCount);
          std::size_t count = 0u;
          if (!runtime::resolveCommandHandles(chunk, objects, resolved, count) ||
              !execution.bindCommandChunk(chunk.bytes, chunk.envelope, {resolved.data(), count}))
            return fail(event.eventIndex, execution.failed() ? execution.failure() : "controlled-v4-inventory-raw-handles");
          const auto ordinal = plan_.tape.event(event.eventIndex).header.ordinal;
          if (!executionOnly) for (std::uint32_t index = 0u; index < imported.records.size(); ++index) {
            const auto type = imported.records[index].type;
            if (type != D9C_COMMAND_RECORD_DRAW_PRIMITIVE && type != D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE) continue;
            const auto view = imported.record(index);
            constexpr std::size_t payloadLimit = 512u * 1024u * 1024u;
            if (view.payload.size() > payloadLimit - canonicalPayloadBytes)
              return fail(event.eventIndex, "controlled-v4-inventory-canonical-payload-cap");
            CpuGeneralTapeControlledV4CanonicalDrawRecord raw{{ordinal, index}, event.eventIndex, type};
            raw.payload.assign(view.payload.begin(), view.payload.end());
            canonicalPayloadBytes += view.payload.size();
            result.canonicalDrawRecords.push_back(std::move(raw));
          }
          CpuTapeProducerResourceLedgerV4::Context eventContext;
          if (!executionOnly && versions->freezeCurrent(eventContext) != Status::Accepted)
            return fail(event.eventIndex, "controlled-v4-inventory-event-context");
          const auto bindRecord = [&](std::uint32_t index, std::uint32_t) {
            if (executionOnly) return true;
            CpuTapeProducerResourceLedgerV4::Context bound;
            return versions->bindRecord({ordinal, index}, eventContext, bound) == Status::Accepted ||
                fail(event.eventIndex, "controlled-v4-inventory-record-context");
          };
          const auto afterRecord = [&](std::uint32_t index, std::uint32_t type, std::int32_t hr) {
            if (hr != core::D3D_OK || type != D9C_COMMAND_RECORD_QUERY_ISSUE) return true;
            D9CCommandChunkWireQueryIssue issue{};
            const auto view = imported.record(index);
            if (view.payload.size() < sizeof(issue)) return fail(event.eventIndex, "controlled-v4-inventory-query-issue");
            std::memcpy(&issue, view.payload.data(), sizeof(issue));
            if (issue.queryHandleIndex >= count) return fail(event.eventIndex, "controlled-v4-inventory-query-handle");
            const auto& wire = imported.handles[issue.queryHandleIndex];
            const D9CWireObjectIdentity identity{wire.kind, wire.generation, wire.objectId};
            const auto cycle = std::find_if(queries.begin(), queries.end(), [&](const auto& value) {
              return detail::cpuGeneralSameIdentity(value.identity, identity);
            });
            auto* query = static_cast<D9CQuery*>(resolved[issue.queryHandleIndex]);
            if (cycle == queries.end() || !query || !query->obj ||
                !d3d9::renderTapeAdvanceQueryCycle(cycle->state, issue.flags, true))
              return fail(event.eventIndex, "controlled-v4-inventory-query-transition");
            ++cycle->expectedLocalIssueOrdinal;
            cycle->snapshot = fixture_.cDevice->dev().captureQueryPollSnapshot(query->obj);
            ++queryEvidence.replayedIssueRecords;
            return cycle->snapshot.issueOrdinal == cycle->expectedLocalIssueOrdinal ||
                fail(event.eventIndex, "controlled-v4-inventory-query-issue-coverage");
          };
          struct ObservationContext {
            d3d9::ProviderReplayObservationV4 original;
            const decltype(bindRecord)& bind;
            const decltype(afterRecord)& after;
            static bool rawStarted(void* opaque, std::uint64_t event, std::uint64_t raw, std::uint32_t count) noexcept {
              auto& self = *static_cast<ObservationContext*>(opaque);
              return self.original.rawStarted && self.original.rawStarted(self.original.context, event, raw, count);
            }
            static bool beforeRecord(void* opaque, std::uint32_t index, std::uint32_t type) noexcept {
              auto& self = *static_cast<ObservationContext*>(opaque);
              bool bound = false;
              try { bound = self.bind(index, type); } catch (...) {}
              const bool forwarded = self.original.beforeRecord &&
                  self.original.beforeRecord(self.original.context, index, type);
              return bound && forwarded;
            }
            static bool beforeDispatch(void* opaque, std::uint32_t index, std::uint32_t type) noexcept {
              auto& self = *static_cast<ObservationContext*>(opaque);
              return self.original.beforeDispatch && self.original.beforeDispatch(self.original.context, index, type);
            }
            static bool recordDispatched(void* opaque, std::uint32_t index, std::uint32_t type, std::int32_t hr) noexcept {
              auto& self = *static_cast<ObservationContext*>(opaque);
              const bool forwarded = self.original.recordDispatched &&
                  self.original.recordDispatched(self.original.context, index, type, hr);
              bool observed = false;
              try { observed = self.after(index, type, hr); } catch (...) {}
              return forwarded && observed;
            }
            static bool rawSettled(void* opaque, std::uint64_t raw, std::uint32_t count, bool succeeded) noexcept {
              auto& self = *static_cast<ObservationContext*>(opaque);
              return self.original.rawSettled && self.original.rawSettled(self.original.context, raw, count, succeeded);
            }
          } observer{execution.observation(), bindRecord, afterRecord};
          auto observation = observer.original;
          observation.context = &observer;
          observation.rawStarted = &ObservationContext::rawStarted;
          observation.beforeRecord = &ObservationContext::beforeRecord;
          observation.beforeDispatch = &ObservationContext::beforeDispatch;
          observation.recordDispatched = &ObservationContext::recordDispatched;
          observation.rawSettled = &ObservationContext::rawSettled;
          d3d9::ProviderReplayObservationV4Result observed;
          const auto hr = d3d9::replayPrevalidatedResolvedCommandChunkObservedV4(fixture_.cDevice.get(),
              chunk.bytes, chunk.envelope, {resolved.data(), count},
              options.route, observation, observed);
          if (hr != core::D3D_OK || !observed.captureAccepted || !observed.qualificationSucceeded ||
              !observed.ledgerSettlementObserved || !observed.localRawOrdinal || execution.failed())
            return fail(event.eventIndex, execution.failed() ? execution.failure() : "controlled-v4-inventory-raw-replay");
          result.routeCoverage.effectiveRoute = options.route;
          result.routeCoverage.policySelected = true;
          result.commands += imported.records.size();
          for (const auto& record : imported.records)
            result.draws += record.type == D9C_COMMAND_RECORD_DRAW_PRIMITIVE ||
                record.type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
          return pullPublications(event.eventIndex);
        }, [&] {
          if (event.index >= plan_.destroys.size()) return fail(event.eventIndex, "controlled-v4-inventory-destroy");
          const auto identity = plan_.destroys[event.index].identity;
          const auto owner = std::find_if(objects.begin(), objects.end(), [&](const auto& value) {
            return value.value && detail::cpuGeneralSameIdentity(value.identity, identity);
          });
          if (owner == objects.end()) return fail(event.eventIndex, "controlled-v4-inventory-stale-destroy");
          const auto native = nativeHandle(*owner);
          if (native) tombstones.push_back({identity, native});
          runtime::releaseOwnedObject(*owner);
          ++result.resourcesReleased;
          if (!executionOnly && armed && native) {
            const auto previous = versions->find(identity);
            if (!previous.version || versions->destroy(identity, previous.version->contentEpoch()) != Status::Accepted)
              return fail(event.eventIndex, "controlled-v4-inventory-destroy-version");
          }
          return true;
        }, [&] {
          return replayPoll(event.eventIndex, event.index);
        }, [] { return true; });
        if (!dispatched) { fail(event.eventIndex, "controlled-v4-inventory-event-dispatch"); break; }
        if (event.type != d3d9::RenderTapeEventType::CommandChunk && !execution.replayedEvent(event.eventIndex)) {
          fail(event.eventIndex, execution.failure()); break;
        }
      }
      const auto lastEvent = plan_.events.empty() ? 0xffffffffu : plan_.events.back().eventIndex;
      if (result.failure.empty()) {
        queryEvidence.transcriptComplete = queryEvidence.replayedIssueRecords == queryEvidence.issueRecords &&
            queryEvidence.replayedPolls == queryEvidence.recordedPolls;
        queryEvidence.oracleComplete = queryEvidence.transcriptComplete &&
            queryEvidence.readinessRequired == queryEvidence.readinessSatisfied &&
            queryEvidence.checkpointDefinitions == queryEvidence.checkpointCompleted &&
            queryEvidence.checkpointDefinitions == queryEvidence.checkpointActivated;
        if (!armed || !setup.intervalComplete() || !queryEvidence.oracleComplete)
          fail(lastEvent, "controlled-v4-inventory-interval-incomplete");
        else if (!waitLastPublication(lastEvent) || !fixture_.closeControlledCpuProducer() ||
            !handle->consumer->closeProducer() || !handle->consumer->stopAndJoin())
          fail(lastEvent, "controlled-v4-inventory-consumer-close");
        else {
          handle->join();
          if (!stopClock(result.clocks.body, bodyClock, true))
            fail(lastEvent, "controlled-v4-execution-clock");
          clearControlledFlushAdapter();
          result.consumer = handle->consumer->snapshot();
          result.consumerReceipts = handle->consumer->takeReceipts();
          if (inventory) result.inventory = inventory->takeSources();
          consumerEvidenceTaken = true;
          if (inventory && inventory->failure()) fail(lastEvent, inventory->failure());
          else if (!result.consumer.workerExited || !result.consumer.cleanupComplete ||
              result.consumer.cleanupFailed ||
              (!executionOnly && result.inventory.size() != result.consumer.state.retired) ||
              result.consumer.state.retired != result.consumerReceipts.size())
            fail(lastEvent, "controlled-v4-inventory-consumer-coverage");
          else if (!execution.finishControlledCpu(capability, result.controlledCpu))
            fail(lastEvent, execution.failure());
          else {
            result.metadataInventoryComplete = !executionOnly;
            result.sourceProgressComplete = true;
            result.complete = result.failure.empty();
          }
        }
      }
    } catch (...) { fail(result.failedEvent, "controlled-v4-inventory-exception"); }
    if (handle && !result.complete) {
      handle->cancelAndJoin();
      if (!consumerEvidenceTaken) {
        result.consumer = handle->consumer->snapshot();
        result.consumerReceipts = handle->consumer->takeReceipts();
        if (inventory) result.inventory = inventory->takeSources();
        consumerEvidenceTaken = true;
      }
    }
    if (!stopClock(result.clocks.body, bodyClock, false) ||
        !stopClock(result.clocks.setup, setupClock, false)) {
      fail(result.failedEvent, "controlled-v4-execution-clock");
      result.complete = false;
    }
    if (executionOnly) {
      auto& cheap = result.executionOnly;
      cheap.sources = result.consumer.state.observed;
      cheap.commands = result.consumer.state.commands;
      cheap.submitted = result.consumer.state.submitted;
      cheap.acknowledged = result.consumer.state.acknowledged;
      cheap.retired = result.consumer.state.retired;
      std::size_t receiptCommands = 0u;
      bool receiptCoverage = true;
      for (const auto& receipt : result.consumerReceipts) {
        if (!receipt.observation) { receiptCoverage = false; continue; }
        receiptCoverage &= receipt.submitted && receipt.acknowledged && receipt.retired && receipt.cleanupComplete;
        for (const auto& command : receipt.observation->commands) {
          ++receiptCommands;
          if (command.drawCount > std::numeric_limits<std::size_t>::max() - cheap.draws) {
            receiptCoverage = false;
            break;
          }
          cheap.draws += command.drawCount;
        }
      }
      cheap.lifecycleCoverageComplete = result.complete && receiptCoverage &&
          cheap.sources == result.consumerReceipts.size() && cheap.sources == cheap.submitted &&
          cheap.sources == cheap.acknowledged && cheap.sources == cheap.retired &&
          cheap.commands == receiptCommands && cheap.draws == result.draws;
      if (result.complete && !cheap.lifecycleCoverageComplete) {
        fail(result.failedEvent, "controlled-v4-execution-lifecycle-coverage");
        result.complete = false;
      }
    }
    if (routeStarted && result.routeCoverage.observed) {
      const auto after = perf::snapshotRenderTapeReplay();
      auto& route = result.routeCoverage;
      route.monotonic = after.directLeases >= routeBefore.directLeases &&
          after.directLeaseDraws >= routeBefore.directLeaseDraws &&
          after.compatibilityDraws >= routeBefore.compatibilityDraws &&
          after.compatibilityBatches >= routeBefore.compatibilityBatches &&
          after.compatibilityUnbatchedDraws >= routeBefore.compatibilityUnbatchedDraws &&
          after.fallbackDraws >= routeBefore.fallbackDraws;
      if (route.monotonic) {
        route.directLeases = after.directLeases - routeBefore.directLeases;
        route.directDraws = after.directLeaseDraws - routeBefore.directLeaseDraws;
        route.compatibilityDraws = after.compatibilityDraws - routeBefore.compatibilityDraws;
        route.compatibilityBatches = after.compatibilityBatches - routeBefore.compatibilityBatches;
        route.compatibilityUnbatchedDraws =
            after.compatibilityUnbatchedDraws - routeBefore.compatibilityUnbatchedDraws;
        route.fallbackDraws = after.fallbackDraws - routeBefore.fallbackDraws;
      }
      route.complete = result.complete && route.policySelected && route.monotonic &&
          route.directDraws <= std::numeric_limits<std::uint64_t>::max() - route.compatibilityDraws &&
          route.directDraws + route.compatibilityDraws == result.draws &&
          (productionSlot || route.directDraws == 0u);
      if (result.complete && !route.complete) {
        fail(result.failedEvent, "controlled-v4-inventory-route-coverage");
        result.complete = false;
      }
    }
    if (options.observeLogicalSemantics && inventory) {
      if (result.complete && !inventory->finishLogical(result.consumer.state.retired, result.consumer.state.commands)) {
        fail(result.failedEvent, inventory->failure());
        result.complete = false;
      }
      result.logicalSemantics = inventory->takeLogicalResult();
      if (!result.complete) {
        result.logicalSemantics.accepted = result.logicalSemantics.complete =
            result.logicalSemantics.logicalStreamComplete = false;
        if (result.logicalSemantics.failure.empty()) result.logicalSemantics.failure = result.failure;
      }
    }
    cleanup();
    result.complete = result.complete && result.resetOk;
    if (executionOnly && (!result.clocks.setup.valid || !result.clocks.body.valid || !result.clocks.teardown.valid)) {
      fail(result.failedEvent, "controlled-v4-execution-clock");
      result.complete = false;
    }
    result.executionOnly.complete = executionOnly && result.complete && result.executionOnly.lifecycleCoverageComplete;
    result.executionOnly.lifecycleCoverageComplete = result.executionOnly.lifecycleCoverageComplete && result.complete;
    if (!result.complete) {
      result.clocks.setup.complete = result.clocks.body.complete = result.clocks.teardown.complete = false;
    }
    result.routeCoverage.complete = result.routeCoverage.complete && result.complete;
    if (options.observeLogicalSemantics && !result.complete) {
      result.logicalSemantics.accepted = result.logicalSemantics.complete =
          result.logicalSemantics.logicalStreamComplete = false;
      if (result.logicalSemantics.failure.empty()) result.logicalSemantics.failure = "controlled-v4-logical-reset-incomplete";
    }
    if (!result.resetOk && result.failure.empty()) fail(result.failedEvent, "controlled-v4-inventory-reset-incomplete");
    return result;
  }

  Result run() {
    CpuDiagnosticLedger::Scope diagnostic(options_.diagnostic, CpuDiagnosticPhase::Admission);
    Result result;
    d3d9::RenderTapeExecutionV3 executionV3;
    if (options_.diagnostic && !options_.measurement) {
      result.failure = "general-executor-diagnostic-requires-measurement";
      return result;
    }
    result.events = plan_.events.size();
    if (options_.measurement &&
        (options_.beforeNextWork || options_.maxPendingSources != 1u ||
         !options_.sourceOracle.empty() || options_.holdSafeBufferMutation ||
         options_.corruptRetainedBytesForTest ||
         options_.corruptRetainedBytesAfterMutationForTest ||
         options_.deferOneObjectReleaseForResetTest ||
         options_.dropRetiredAliasReceiptForTest ||
         options_.pendingIdentityFault != PendingIdentityFault::None ||
         options_.checkpointFault != CheckpointFault::None)) {
      result.failure = "general-executor-measurement-policy";
      return result;
    }
    if ((options_.captureSourcePlan || options_.captureSourcePlanV3) &&
        (options_.beforeNextWork || options_.maxPendingSources != 1u ||
         !options_.sourceOracle.empty() || options_.holdSafeBufferMutation)) {
      result.failure = "general-executor-capture-source-policy";
      return result;
    }
    if (options_.maxPendingSources < 1u || options_.maxPendingSources > 2u) {
      result.failure = "general-executor-max-pending-sources-bound";
      return result;
    }
    if (options_.maxPendingSources > 1u &&
        (!options_.beforeNextWork || options_.holdSafeBufferMutation)) {
      result.failure = "general-executor-max-pending-sources-policy";
      return result;
    }
    if (options_.holdSafeBufferMutation && !options_.beforeNextWork) {
      result.failure = "general-executor-mutation-policy-needs-delayed-completion";
      return result;
    }
    if (!admit(result)) return result;
    if (!validateSourceOracle(result)) return result;
    if (!fixture_.cDevice || !fixture_.routing || !fixture_.device) {
      result.failure = "general-executor-fixture-unavailable";
      return result;
    }
    if (!admitCaptureSourcePlan(result)) return result;
    if (options_.captureSourcePlanV3) {
      d3d9::CaptureSourceReplayPlanV3 rebound;
      if (options_.captureSourcePlan || options_.route != d3d9::ProviderReplayPolicy::Compatibility ||
          !Access::cpuTapeWorkersDisabled(fixture_.routing->queue_) ||
          fixture_.cDevice->replayOffload || fixture_.routing->supportsCpuReadyArenaReplay() ||
          d3d9::buildCaptureSourceReplayPlanV3(options_.captureSourceTape, plan_.catalogue,
              options_.captureSourceIdentitySidecar, rebound) !=
              d3d9::CaptureSourceReplayPlanV3Status::Valid ||
          !detail::cpuGeneralSameDigest(rebound.digest(), options_.captureSourcePlanV3->digest()) ||
          !executionV3.admit(*options_.captureSourcePlanV3, plan_, options_.captureSourceTape,
                            result.captureSourceReplayV3)) {
        result.failure = "general-executor-v3-capture-source-admission";
        return result;
      }
    }
    if (!admitEmission(result)) return result;
    const auto& queryAdmission = result.inputAdmission;
    const bool queryBearing = std::any_of(plan_.definitions.begin(), plan_.definitions.end(),
        [](const auto& definition) {
          return definition.fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_QUERY;
        });
    if (queryBearing && (!queryAdmission.accepted() ||
                         options_.maxPendingSources != 1u ||
                         options_.holdSafeBufferMutation ||
                         options_.maxQueryOraclePolls > 64u)) {
      result.failure = "general-executor-query-policy-or-transcript";
      return result;
    }
    if (!admitSourceCapacity(result)) return result;
    const auto publicationBaseline = fixture_.routing->queue_.queryProgress().published;
    fixture_.routing->controlledQueries = queryBearing;
    if (queryBearing) {
      result.queryReplay.present = true;
      result.queryReplay.queryDefinitions = queryAdmission.counts.queryDefinitions;
      result.queryReplay.issueRecords = queryAdmission.counts.queryIssues;
      result.queryReplay.recordedPolls = queryAdmission.counts.queryPolls;
      result.queryReplay.maxOraclePolls = options_.maxQueryOraclePolls *
          static_cast<std::uint32_t>(queryAdmission.counts.queryPolls);
      result.queryReplay.maxOracleWallNanos = 0u;
      if (options_.measurement) result.querySemanticDigest = 0u;
      if (!options_.measurement)
        result.queryReplay.rows.reserve(queryAdmission.counts.queryPolls);
    }
    // The admitted grammar has no CPU read/control operations. Seed writes
    // remain real; post-render evidence is commands, never stale shadows.
    fixture_.routing->attachmentOwner = AttachmentContentOwner::BackendCommands;
    if (fixture_.routing->supportsGpuReadback()) {
      result.failure = "general-executor-unexpected-gpu-readback";
      return result;
    }
    diagnostic.set(CpuDiagnosticPhase::HarnessSetup);
    result.windowPageSize = fixture_.routing->queue_.cpuReadyArenaPlanLimits().pageSize;
    result.windowOffload = bool(fixture_.cDevice->replayOffload);
    result.windowArena = fixture_.routing->supportsCpuReadyArenaReplay();
    result.windowWorkersDisabled = Access::cpuTapeWorkersDisabled(fixture_.routing->queue_);
    // The CLI/native entry establishes zero auto-publication limits BEFORE
    // the first device (cached production policy). These live predicates
    // exclude asynchronous mutation, offload drains and Arena admission waits.
    if (options_.maxPendingSources > 1u &&
        (result.windowOffload || result.windowArena || result.windowPageSize !=
             d3d9::kRenderTapeCpuWindowPageSize ||
         !result.windowWorkersDisabled)) {
      result.failure = "general-executor-window-runtime-not-isolated";
      return result;
    }
    const auto overlapEligible = [&](const auto& chunk, std::uint32_t event) {
      d3d9::ImportedChunkView imported;
      if (!d3d9::importPrevalidatedCommandChunk(chunk.bytes, chunk.envelope, imported) ||
          imported.records.empty() || imported.records.size() > 64u) return false;
      const auto shape = d3d9::planRenderTapeCpuWindowSource(
          imported, static_cast<d3d9::RawOrdinal>(event + 1u),
          fixture_.routing->queue_.cpuReadyArenaPlanLimits().pageSize);
      // One bounded ordinary draw/state source, not UP/Fan/control fallback
      // or an internal rotation. The production aggregate capacity accounts
      // for worst-case uniforms/state, not just command/vertex byte counts.
      // With <=64 records and the zero publication limits, the compatibility
      // path's U32 payload/range capacity cannot require an internal split.
      return shape.eligible();
    };
    std::vector<dxmt9::d3d9::render_tape_runtime::OwnedObject> objects;
    std::vector<detail::LogicalReceipt> receipts;
    objects.reserve(plan_.definitions.size());
    struct QueryCycle {
      D9CWireObjectIdentity identity{};
      d3d9::RenderTapeQueryCycleState state{};
      core::QueryPollSnapshot snapshot{};
      std::uint64_t expectedLocalIssueOrdinal = 0u;
    };
    std::vector<QueryCycle> queryCycles;
    queryCycles.reserve(queryAdmission.counts.queryDefinitions);
    struct HiddenCheckpoint {
      dxmt9::d3d9::render_tape_runtime::OwnedObject object{};
      QueryCycle cycle{};
      std::size_t evidenceIndex = 0u;
      bool activated = false;
    };
    std::vector<HiddenCheckpoint> hiddenCheckpoints;
    hiddenCheckpoints.reserve(queryAdmission.counts.queryDefinitions);
    struct CheckpointCleanup {
      std::vector<HiddenCheckpoint>& owners;
      ~CheckpointCleanup() {
        for (auto& owner : owners)
          dxmt9::d3d9::render_tape_runtime::releaseOwnedObject(owner.object);
      }
    } checkpointCleanup{hiddenCheckpoints};
    receipts.reserve(plan_.mutations.size());
    result.tombstones.reserve(plan_.definitions.size());
    auto& tombstones = result.tombstones;
    CpuTapeSnapshotProofCache snapshotProofs;
    struct PendingSourceProof {
      struct MipmapTexture {
        D9CWireObjectIdentity identity{};
        core::Handle handle{};
        std::uintptr_t recordAddress = 0u;
      };
      struct Snapshot {
        core::Handle buffer{};
        CpuTapeLiveBufferView view{};
        std::uint64_t digest = 0u;
        std::optional<core::DrawBufferBindingSnapshot> captured;
      };

      d3d9::RenderTapeCompletionSchedule schedule{};
      d3d9::RenderTapeCompletionKey key{};
      std::uint64_t submission = 0u;
      std::uint32_t source = 0u;
      std::uint64_t sourceGeneration = 0u;
      std::uint32_t storageFirstPage = 0u;
      std::uint32_t storagePageCount = 0u;
      std::uint64_t storageGeneration = 0u;
      std::size_t traceOrdinal = 0u;
      std::vector<Snapshot> snapshots;
      std::vector<core::Handle> resourceBuffers;
      std::vector<D9CWireObjectIdentity> bufferIdentities;
      std::vector<MipmapTexture> mipmapTextures;
      bool mutationHeld = false;
      bool windowEligible = false;

      void bind(const core::metalqueue::QueueCompletionSource& completion) noexcept {
        submission = completion.seqId;
        source = completion.source.id.index;
        sourceGeneration = completion.source.id.generation;
        storageFirstPage = completion.source.storage.firstPage;
        storagePageCount = completion.source.storage.pageCount;
        storageGeneration = completion.source.storage.generation;
      }

      bool matches(const core::metalqueue::QueueCompletionSource& completion) const noexcept {
        return submission == completion.seqId &&
               source == completion.source.id.index &&
               sourceGeneration == completion.source.id.generation &&
               storageFirstPage == completion.source.storage.firstPage &&
               storagePageCount == completion.source.storage.pageCount &&
               storageGeneration == completion.source.storage.generation;
      }

      bool matches(const CpuGeneralTapeTrace& trace) const noexcept {
        return submission == trace.submission && source == trace.source &&
               sourceGeneration == trace.sourceGeneration &&
               storageFirstPage == trace.storageFirstPage &&
               storagePageCount == trace.storagePageCount &&
               storageGeneration == trace.storageGeneration;
      }
    };
    std::deque<PendingSourceProof> pendingSources;
    const auto pendingWindowEligible = [&] {
      return std::all_of(pendingSources.begin(), pendingSources.end(),
                         [](const auto& owner) { return owner.windowEligible; });
    };
    const auto expectedBuffer = [&](core::Handle buffer) {
      const D9CBuffer* expected = nullptr;
      for (const auto& object : objects) {
        if (object.identity.kind != D9C_CHUNK_HANDLE_KIND_BUFFER || !object.value) continue;
        const auto* candidate = static_cast<const D9CBuffer*>(object.value);
        if (candidate->obj && candidate->obj->handle() == buffer) { expected = candidate; break; }
      }
      return expected;
    };
    const auto recordProof = [&](PendingSourceProof& owner, core::Handle buffer,
                                 CpuTapeLiveBufferView view,
                                 std::uint64_t content, std::uint64_t& digest,
                                 std::optional<core::DrawBufferBindingSnapshot> captured = std::nullopt) {
      if (options_.beforeNextWork && std::none_of(
              owner.snapshots.begin(), owner.snapshots.end(), [&](const auto& prior) {
                return prior.buffer == buffer &&
                    prior.view.contentsAddress == view.contentsAddress &&
                    prior.view.contentRevision == view.contentRevision;
              })) {
        if (owner.snapshots.size() == CpuTapeSnapshotProofCache::kCapacity) return false;
        owner.snapshots.push_back({buffer, view, content, captured});
      }
      detail::cpuTapeDigestScalar(digest, view.byteSize);
      detail::cpuTapeDigestScalar(digest, content);
      return true;
    };
    const auto observeSnapshot = [&](PendingSourceProof& owner,
                                     core::Handle buffer,
                                     const core::DrawBufferBindingSnapshot& snapshot,
                                     std::uint64_t& digest) {
      auto& pool = fixture_.routing->queue_.pool();
      const auto* expected = expectedBuffer(buffer);
      std::uint64_t content = 0u;
      if (cpuTapeBufferObservationRoute(pool.bufferHasVersionedBacking(buffer.value),
              true, snapshot.valid()) != CpuTapeBufferObservationRoute::Captured ||
          pool.validateCapturedBufferSnapshot(buffer, snapshot) !=
              resources::CapturedBufferSnapshotStatus::Valid ||
          !expected || !snapshotProofs.prove(buffer, snapshot, expected->obj->bytes(), content))
        return false;
      return recordProof(owner, buffer,
                         {snapshot.metalHandle, snapshot.contentsAddress,
                          snapshot.byteSize, snapshot.contentRevision},
                         content, digest, snapshot);
    };
    const auto observeLive = [&](PendingSourceProof& owner, core::Handle buffer,
                                 std::uint64_t& digest,
                                 std::uint64_t& revision) {
      // Cold single-thread executor, inside the queue observer. No mutation
      // or completion can run here; do not reacquire the queue mutex.
      const auto* record = fixture_.routing->queue_.pool().findBuffer(buffer.value);
      const auto* expected = expectedBuffer(buffer);
      if (!record || cpuTapeBufferObservationRoute(record->hasVersionedBacking(),
              false, bool(record->buffer) && record->contents) != CpuTapeBufferObservationRoute::Live)
        return false;
      const CpuTapeLiveBufferView view{record->buffer.handle,
          static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(record->contents)),
          record->desc.size, record->contentRevision};
      std::uint64_t content = 0u;
      if (!expected || !snapshotProofs.proveLive(buffer, view, expected->obj->bytes(), content))
        return false;
      revision = view.contentRevision;
      return recordProof(owner, buffer, view, content, digest);
    };
    core::Handle expectedUploadHandle{};
    std::uint32_t expectedUploadLevel = 0u;
    detail::CpuGeneralTextureUploadShape expectedUploadShape{};
    std::span<const std::byte> expectedUploadBytes;
    bool observedUpload = false;
    fixture_.routing->generalTextureUpload =
        [&](core::TextureHandle handle, std::uint32_t level,
            std::uint32_t width, std::uint32_t height, std::uint32_t depth,
            std::uint32_t pitch, std::uint32_t slicePitch,
            std::span<const std::uint8_t> bytes) {
          const bool exact = handle == expectedUploadHandle &&
              level == expectedUploadLevel &&
              detail::cpuGeneralTextureUploadShapeMatches(expectedUploadShape,
                  width, height, depth, pitch, slicePitch, bytes.size()) &&
              bytes.size() == expectedUploadBytes.size() &&
              !bytes.empty() &&
              (options_.measurement ||
               std::memcmp(bytes.data(), expectedUploadBytes.data(), bytes.size()) == 0);
          observedUpload |= exact;
          if (exact) ++result.observedTextureUploads;
          return exact;
        };
    struct UploadCleanup {
      Fixture& fixture;
      ~UploadCleanup() { fixture.routing->generalTextureUpload = {}; }
    } uploadCleanup{fixture_};
    struct ObjectCleanup {
      std::vector<dxmt9::d3d9::render_tape_runtime::OwnedObject>& objects;
      ~ObjectCleanup() {
        for (auto it = objects.rbegin(); it != objects.rend(); ++it)
          dxmt9::d3d9::render_tape_runtime::releaseOwnedObject(*it);
      }
    } objectCleanup{objects};
    bool bootstrapPending = false;
    bool bootstrapDone = false;

    const auto fail = [&](std::uint32_t event, std::string reason) {
      if (result.failure.empty()) {
        result.failure = std::move(reason);
        result.failedEvent = event;
      }
      return false;
    };
    bool recovering = false;
    const auto checkPublications = [&](std::uint32_t event) {
      const auto published = fixture_.routing->queue_.queryProgress().published;
      if (published < publicationBaseline ||
          (!recovering && published - publicationBaseline >
              options_.sourceCapacity.maxPublications))
        return fail(event, "general-executor-publication-cap");
      if (!recovering && published - publicationBaseline > result.sourceDemand.publications)
        return fail(event, "general-executor-publication-demand-exceeded");
      result.sourcePublications = static_cast<std::size_t>(published - publicationBaseline);
      return true;
    };
    const auto consumeProgress = [&](std::uint32_t event) {
      if (!recovering && result.progressSteps >= options_.sourceCapacity.maxProgressSteps)
        return fail(event, "general-executor-progress-cap");
      if (!recovering && result.progressSteps >= result.sourceDemand.progressSteps)
        return fail(event, "general-executor-progress-demand-exceeded");
      ++result.progressSteps;
      return true;
    };

    const auto setupBegin = options_.measurement
        ? std::chrono::steady_clock::time_point{}
        : std::chrono::steady_clock::now();
    const auto create = [&](const auto& definition) {
      if (objects.size() >= options_.inputCaps.maxObjects)
        return static_cast<void*>(nullptr);
      return dxmt9::d3d9::render_tape_runtime::createRenderTapeObject(
          fixture_.cDevice.get(), definition.fixed, definition.descriptor,
          blobs_, objects);
    };
    if (!options_.measurement) result.setupNs += elapsed(setupBegin);

    // Pending sources are retained in submission order. Every potentially
    // blocking work entry remains quiescent; wrapper destruction and
    // explicitly proved no-wait buffer mutations may run before
    // acknowledgment.
    std::size_t sourceOracleRow = 0u;
    bool pendingIdentityFaultInjected = false;
    const auto releaseOne = [&](std::uint32_t event) {
      CpuDiagnosticLedger::Scope phase(options_.diagnostic, CpuDiagnosticPhase::Completion);
      if (pendingSources.empty()) {
        if (result.completed == result.submitted &&
            Access::pendingCompletionCount(fixture_.routing->queue_) == 0u)
          return true;
        return fail(event, "general-executor-pending-identity-count");
      }
      const auto completionBegin = options_.measurement
          ? std::chrono::steady_clock::time_point{}
          : std::chrono::steady_clock::now();
      struct CompletionTiming {
        Result& result;
        std::chrono::steady_clock::time_point begin;
        bool enabled = true;
        ~CompletionTiming() {
          if (enabled) result.completionNs += elapsed(begin);
        }
      } completionTiming{result, completionBegin, !options_.measurement};
      if (result.submitted < result.completed ||
          result.submitted - result.completed != pendingSources.size() ||
          Access::pendingCompletionCount(fixture_.routing->queue_) !=
              pendingSources.size())
        return fail(event, "general-executor-pending-identity-count");
      auto& owner = pendingSources.front();
      if (owner.traceOrdinal >= result.trace.size() ||
          owner.traceOrdinal != result.completed)
        return fail(event, "general-executor-pending-trace-order");
      auto& trace = result.trace[owner.traceOrdinal];
      if (!pendingIdentityFaultInjected &&
          options_.pendingIdentityFault != PendingIdentityFault::None) {
        switch (options_.pendingIdentityFault) {
        case PendingIdentityFault::Sequence:
          ++owner.submission;
          break;
        case PendingIdentityFault::SourceIndex:
          ++owner.source;
          break;
        case PendingIdentityFault::SourceGeneration:
          ++owner.sourceGeneration;
          break;
        case PendingIdentityFault::StorageFirstPage:
          ++owner.storageFirstPage;
          break;
        case PendingIdentityFault::StoragePageCount:
          ++owner.storagePageCount;
          break;
        case PendingIdentityFault::StorageGeneration:
          ++owner.storageGeneration;
          break;
        case PendingIdentityFault::None:
          break;
        }
        pendingIdentityFaultInjected = true;
      }
      if (!owner.matches(trace))
        return fail(event, "general-executor-release-identity-changed");
      const auto beforeResident = Access::residentCount(fixture_.routing->queue_);
      if (!owner.schedule.release(owner.key, [&] {
            return Access::acknowledgeOne(fixture_.routing->queue_);
          }).accepted())
        return fail(event, "general-executor-acknowledge-failed");
      trace.completed = true;
      trace.releasedAtEvent = event;
      ++result.completed;
      result.deferredReleases += event > trace.event;
      if (!Access::finishQueue(fixture_.routing->queue_))
        return fail(event, "general-executor-finish-failed");
      Access::reclaimCompletedPool(fixture_.routing->queue_);
      trace.reclaimed = Access::residentCount(fixture_.routing->queue_) < beforeResident;
      if (trace.reclaimed) ++result.reclaimed;
      if (!trace.reclaimed || !owner.schedule.finishIteration().accepted())
        return fail(event, "general-executor-scheduled-reclaim-failed");
      pendingSources.pop_front();
      return true;
    };
    const auto releasePending = [&](std::uint32_t event) {
      while (!pendingSources.empty()) {
        if (!releaseOne(event)) return false;
      }
      if (result.completed == result.submitted &&
          Access::pendingCompletionCount(fixture_.routing->queue_) == 0u)
        return true;
      return fail(event, "general-executor-pending-identity-count");
    };
    const auto publishReady = [&](
                                   std::uint32_t event,
                                   bool windowEligible = false,
                                   const std::span<const core::metalqueue::ReplaySourcePublicationReceipt>* strictExpected = nullptr,
                                   bool preserveWriter = false) {
      CpuDiagnosticLedger::Scope phase(options_.diagnostic, CpuDiagnosticPhase::Publication);
      const bool strictMode = strictExpected != nullptr;
      if (!checkPublications(event)) return false;
      if (!recovering && !preserveWriter &&
          Access::writingCommandCount(fixture_.routing->queue_) != 0u &&
          result.sourcePublications >= options_.sourceCapacity.maxPublications)
        return fail(event, "general-executor-publication-cap");
      if (strictMode && !preserveWriter && Access::writingCommandCount(fixture_.routing->queue_) != 0u)
        return fail(event, "general-executor-source-writing-preexisting");
      if (!preserveWriter && !Access::publishWriting(fixture_.routing->queue_))
        return fail(event, "general-executor-publish-failed");
      if (!checkPublications(event)) return false;
      const std::size_t expectedReady =
          strictMode ? strictExpected->size() : 0u;
      if (strictMode && (preserveWriter
              ? Access::readyCount(fixture_.routing->queue_) < expectedReady
              : Access::readyCount(fixture_.routing->queue_) != expectedReady))
        return fail(event, "general-executor-source-ready-count");
      std::size_t drained = 0u;
      while (strictMode ? drained < expectedReady
                        : Access::readyCount(fixture_.routing->queue_) != 0u) {
        if (strictMode && (preserveWriter
                ? Access::readyCount(fixture_.routing->queue_) < expectedReady - drained
                : Access::readyCount(fixture_.routing->queue_) != expectedReady - drained))
          return fail(event, "general-executor-source-ready-count");
        // Keep one actual submitted source outstanding for recorded polls.
        // Earlier publications settle in order before the bounded queue
        // credit is reused; query-bearing raws may cut more than one source.
        if (queryBearing && !pendingSources.empty() &&
            result.submitted - result.completed >= options_.maxPendingSources &&
            !releaseOne(event)) return false;
        if (result.submitted < result.completed ||
            result.submitted - result.completed >= options_.maxPendingSources)
          return fail(event, "general-executor-pending-budget");
        const auto expected = Access::peekReadyCompletion(fixture_.routing->queue_);
        if (!expected.completionIdentityValid())
          return fail(event, "general-executor-invalid-completion-identity");
        if (strictMode) {
          const auto& receipt = (*strictExpected)[drained];
          if (!(expected.source == receipt.source) ||
              expected.seqId != receipt.metadata.seqId ||
              expected.slotIndex != receipt.controlIndex)
            return fail(event, "general-executor-source-receipt-mismatch");
        }
        PendingSourceProof candidate;
        candidate.windowEligible = windowEligible;
        candidate.bind(expected);
        candidate.key = {1u, candidate.submission, candidate.source, 0u,
                         candidate.sourceGeneration};
        if (!candidate.schedule.append({candidate.key}).accepted() ||
            !candidate.schedule.beginIteration(1u).accepted() ||
            !candidate.schedule.submit(candidate.key).accepted())
          return fail(event, "general-executor-schedule-submit-failed");
        if (!consumeProgress(event)) return false;
        auto submission = Access::submitOneWithheld(
            fixture_.routing->queue_, [&](const auto& payload) {
              const auto observeBegin = options_.measurement
                  ? std::chrono::steady_clock::time_point{}
                  : std::chrono::steady_clock::now();
              {
                for (std::size_t command = 0u; command < payload.commandCount(); ++command) {
                  const auto sourceCommand = payload.commandAt(command);
                  ++result.sourceSummaryCommands;
                  if (sourceCommand.kind() == core::MetalCommandKind::DrawRun)
                    result.sourceSummaryDraws += sourceCommand.command.drawParams.size();
                  if (options_.measurement &&
                      sourceCommand.kind() == core::MetalCommandKind::GenerateMipmaps)
                    ++result.mipmapCommands;
                  if (!options_.measurement &&
                      sourceCommand.kind() == core::MetalCommandKind::GenerateMipmaps &&
                      sourceCommand.command.generateMipmaps) {
                    const auto handle = sourceCommand.command.generateMipmaps->texture;
                    const auto object = std::find_if(objects.begin(), objects.end(), [&](const auto& value) {
                      if (!value.value || value.identity.kind != D9C_CHUNK_HANDLE_KIND_TEXTURE)
                        return false;
                      const auto* texture = static_cast<const D9CTexture*>(value.value);
                      return texture->obj && texture->obj->handle() == handle;
                    });
                    const auto* record = fixture_.routing->queue_.pool().findTexture(handle.value);
                    if (object == objects.end() || !record ||
                        record->lastUsedSeqId < candidate.submission) {
                      fail(event, "general-executor-unqualified-mipmap-texture");
                    } else {
                      candidate.mipmapTextures.push_back({object->identity, handle,
                          reinterpret_cast<std::uintptr_t>(record)});
                    }
                  }
                  if (options_.measurement) continue;
                  core::visitSourceCommandResources(payload.commandAt(command), [&](const auto& ref) {
                    if (ref.entry.kind != core::ChunkHandleKind::Buffer ||
                        std::find(candidate.resourceBuffers.begin(), candidate.resourceBuffers.end(),
                                  ref.entry.handle) != candidate.resourceBuffers.end()) return;
                    if (candidate.resourceBuffers.size() == CpuTapeSnapshotProofCache::kCapacity) {
                      fail(event, "general-executor-mutation-closure-cap");
                      return;
                    }
                    candidate.resourceBuffers.push_back(ref.entry.handle);
                    const auto object = std::find_if(objects.begin(), objects.end(), [&](const auto& candidate) {
                      if (candidate.identity.kind != D9C_CHUNK_HANDLE_KIND_BUFFER || !candidate.value)
                        return false;
                      const auto* buffer = static_cast<const D9CBuffer*>(candidate.value);
                      return buffer->obj && buffer->obj->handle() == ref.entry.handle;
                    });
                    if (object == objects.end()) {
                      fail(event, "general-executor-unqualified-source-buffer");
                      return;
                    }
                    candidate.bufferIdentities.push_back(object->identity);
                  });
                }
                std::sort(candidate.bufferIdentities.begin(), candidate.bufferIdentities.end(),
                    [](const auto& a, const auto& b) {
                      return std::tie(a.kind, a.generation, a.objectId) <
                             std::tie(b.kind, b.generation, b.objectId);
                    });
                if (!options_.sourceOracle.empty()) {
                  if (sourceOracleRow >= options_.sourceOracle.size() ||
                      options_.sourceOracle[sourceOracleRow].event != event + 1u ||
                      !cpuSourceOracleMatches(candidate.bufferIdentities,
                                               options_.sourceOracle[sourceOracleRow])) {
                    fail(event, "general-executor-source-oracle-mismatch");
                    return;
                  }
                  ++sourceOracleRow;
                  ++result.sourceOracleCheckedSources;
                }
              }
              if (!options_.measurement) {
                std::string observeFailure;
                ++result.fullOracleCalls;
                auto resource = [&](std::uint32_t kind, core::Handle handle,
                                    std::uint64_t& hash) {
                  return observeResource(objects, receipts, tombstones,
                                         kind, handle, hash);
                };
                auto draw = [&](std::uint64_t cumulativeHash) {
                  if (result.drawObservations.size() >= options_.inputCaps.maxDraws) {
                    observeFailure = "general-executor-draw-cap";
                    return false;
                  }
                  result.drawObservations.push_back({
                      .ordinal = static_cast<std::uint32_t>(
                          result.drawObservations.size() + 1u),
                      .publicationEvent = event,
                      .cumulativeSemanticDigest = cumulativeHash});
                  return true;
                };
                const auto observeSnapshotForSource =
                    [&](core::Handle buffer,
                        const core::DrawBufferBindingSnapshot& snapshot,
                        std::uint64_t& digest) {
                      return observeSnapshot(candidate, buffer, snapshot, digest);
                    };
                const auto observeLiveForSource =
                    [&](core::Handle buffer, std::uint64_t& digest,
                        std::uint64_t& revision) {
                      return observeLive(candidate, buffer, digest, revision);
                    };
                if (!observeCpuTapePayload(payload, result.digest, resource, draw,
                                           observeFailure, &result.observedSnapshots,
                                           observeSnapshotForSource,
                                           &result.observedLiveBindings,
                                           observeLiveForSource,
                                           &result.mipmapCommands) &&
                    result.failure.empty()) {
                  result.failure = observeFailure.empty()
                                       ? "general-executor-observe-failed"
                                       : observeFailure;
                  result.failedEvent = event;
                }
              }
              if (!options_.measurement)
                result.observeNs += elapsed(observeBegin);
            });
        if (!submission.dequeued || !submission.submitted) {
          return fail(event, "general-executor-submit-failed");
        }
        ++result.submitted;
        const auto& completion = submission.completion;
        if (!candidate.matches(completion))
          return fail(event, "general-executor-submission-identity-changed");
        candidate.traceOrdinal = result.trace.size();
        CpuGeneralTapeTrace trace{
            .event = event,
            .source = completion.source.id.index,
            .sourceGeneration = completion.source.id.generation,
            .submission = completion.seqId,
            .storageFirstPage = completion.source.storage.firstPage,
            .storagePageCount = completion.source.storage.pageCount,
            .storageGeneration = completion.source.storage.generation,
            .submitted = true};
        trace.bufferClosure = candidate.bufferIdentities;
        pendingSources.push_back(std::move(candidate));
        result.trace.push_back(trace);
        result.pendingPeak = std::max(result.pendingPeak, result.submitted - result.completed);
        ++drained;
        if (!options_.beforeNextWork && !queryBearing && !releasePending(event)) return false;
      }
      if (strictMode && (drained != expectedReady ||
                         (!preserveWriter && Access::readyCount(fixture_.routing->queue_) != 0u)))
        return fail(event, "general-executor-source-ready-drain");
      return result.failure.empty();
    };

    const auto replayChunk = [&](std::uint32_t event,
                                 const dxmt9::d3d9::render_tape_runtime::CommandChunkView& chunk,
                                 std::optional<std::size_t> commandChunkIndex =
                                     std::nullopt) {
      CpuDiagnosticLedger::Scope phase(options_.diagnostic, CpuDiagnosticPhase::Replay);
      dxmt9::d3d9::ImportedChunkView imported;
      if (!dxmt9::d3d9::importPrevalidatedCommandChunk(chunk.bytes,
                                                        chunk.envelope,
                                                        imported))
        return fail(event, "general-executor-command-import-failed");
      if (imported.records.size() > options_.inputCaps.maxTotalRecords ||
          result.commands > options_.inputCaps.maxTotalRecords - imported.records.size())
        return fail(event, "general-executor-command-cap");
      const auto replayBegin = options_.measurement
          ? std::chrono::steady_clock::time_point{}
          : std::chrono::steady_clock::now();
      const bool windowEligible = options_.maxPendingSources > 1u && overlapEligible(chunk, event);
      const bool queryOnly = queryBearing && !imported.records.empty() &&
          std::all_of(imported.records.begin(), imported.records.end(), [](const auto& record) {
            return record.type == D9C_COMMAND_RECORD_QUERY_ISSUE;
          });
      std::optional<d3d9::RenderTapeExecutionV3::WritingContinuation> writing;
      if (options_.captureSourcePlanV3)
        writing = Access::writingContinuation(fixture_.routing->queue_);
      if (writing) {
        if (options_.continuationIdentityFault == ContinuationIdentityFault::StaleWriterGeneration)
          ++writing->writer.generation;
        else if (options_.continuationIdentityFault == ContinuationIdentityFault::ForeignQueue)
          ++writing->writer.queue;
      }
      const bool ownedV3Writer = options_.captureSourcePlanV3 && writing &&
          executionV3.ownsWritingContinuation(*writing);
      if (Access::readyCount(fixture_.routing->queue_) ||
          (Access::writingCommandCount(fixture_.routing->queue_) && !ownedV3Writer) ||
          Access::pendingCompletionCount(fixture_.routing->queue_) != pendingSources.size() ||
          Access::residentCount(fixture_.routing->queue_) !=
              pendingSources.size() + std::size_t(ownedV3Writer) ||
          (!pendingSources.empty() &&
           !options_.captureSourcePlanV3 && !queryOnly && (!windowEligible || !pendingWindowEligible())))
        return fail(event, "general-executor-nonquiescent-chunk-entry");
      if (options_.maxPendingSources > 1u &&
          !Access::noWaitHeadroom(fixture_.routing->queue_).admitsOne())
        return fail(event, "general-executor-window-source-admission-denied");
      std::vector<void*> resolved(imported.handles.size());
      std::size_t resolvedCount = 0u;
      if (!dxmt9::d3d9::render_tape_runtime::resolveCommandHandles(
              chunk, objects, resolved, resolvedCount) ||
          resolvedCount != imported.handles.size())
        return fail(event, "general-executor-command-handle-failed");
      bool sourceDriven = false;
      if (options_.captureSourcePlan && commandChunkIndex.has_value()) {
        sourceDriven = true;
        if (!commandChunkIndex.has_value() ||
            *commandChunkIndex >= options_.captureSourcePlan->commandChunkCount())
          return fail(event, "general-executor-capture-source-chunk-index");
        const auto ranges = options_.captureSourcePlan->rangesForCommandChunk(
            *commandChunkIndex);
        const auto sourceRowsIt = std::find_if(
            result.captureSourceReplaySources.begin(),
            result.captureSourceReplaySources.end(),
            [&](const auto& row) {
              return row.commandChunkIndex == *commandChunkIndex;
            });
        if (sourceRowsIt == result.captureSourceReplaySources.end() ||
            static_cast<std::size_t>(result.captureSourceReplaySources.end() -
                                     sourceRowsIt) < ranges.size())
          return fail(event, "general-executor-capture-source-row-map");
        std::size_t sourceRowCursor = static_cast<std::size_t>(
            sourceRowsIt - result.captureSourceReplaySources.begin());
        const auto sourceRowEnd = sourceRowCursor + ranges.size();
        std::size_t settledRange = 0u;
        std::uint64_t callbackNs = 0u;
        const auto settle = [&](
            const dxmt9::d3d9::ProviderReplaySourceSettlement& settlement) {
          if (settledRange >= ranges.size() ||
              sourceRowCursor >= sourceRowEnd ||
              settlement.rangeIndex != settledRange ||
              settlement.range.firstRecord != ranges[settledRange].firstRecord ||
              settlement.range.recordCount != ranges[settledRange].recordCount)
            return false;
          auto& sourceRow = result.captureSourceReplaySources[sourceRowCursor];
          if (sourceRow.settled ||
              sourceRow.firstRecord != settlement.range.firstRecord ||
              sourceRow.recordCount != settlement.range.recordCount ||
              settlement.publications.size() >
                  result.captureSourceReplayPublications.capacity() -
                      result.captureSourceReplayPublications.size() ||
              settlement.publications.size() >
                  std::numeric_limits<std::uint32_t>::max()) {
            result.captureSourceReplayStorageOverflow = true;
            return false;
          }
          sourceRow.actualRawOrdinal = settlement.rawOrdinal;
          sourceRow.rangeIndex = settlement.rangeIndex;
          sourceRow.publicationOffset = static_cast<std::uint32_t>(
              result.captureSourceReplayPublications.size());
          sourceRow.publicationCount = static_cast<std::uint32_t>(
              settlement.publications.size());
          sourceRow.settled = true;
          sourceRow.settlement =
              settlement.kind ==
                      dxmt9::d3d9::ProviderReplaySourceSettlementKind::PresentPublished
                  ? dxmt9::d3d9::RenderTapeSourceReplaySettlement::PresentPublished
                  : settlement.kind ==
                            dxmt9::d3d9::ProviderReplaySourceSettlementKind::Published
                      ? dxmt9::d3d9::RenderTapeSourceReplaySettlement::Published
                      : dxmt9::d3d9::RenderTapeSourceReplaySettlement::Empty;
          for (const auto& publication : settlement.publications) {
            const auto source = publication.source;
            const auto metadata = publication.metadata;
            result.captureSourceReplayPublications.push_back({
                .sourceIdIndex = source.id.index,
                .sourceIdGeneration = source.id.generation,
                .storageFirstPage = source.storage.firstPage,
                .storagePageCount = source.storage.pageCount,
                .storageGeneration = source.storage.generation,
                .metadataRawOrdinal = metadata.rawOrdinal,
                .metadataSourceOrdinal = metadata.sourceOrdinal,
                .metadataSeqId = metadata.seqId,
                .metadataBuildGeneration = metadata.buildGeneration,
                .metadataUsedBytes = metadata.usedBytes,
                .metadataPageCount = metadata.pageCount,
                .metadataPaddingPagesBefore = metadata.paddingPagesBefore,
                .metadataStrictAdmission = metadata.strictAdmission,
                .controlIndex = static_cast<std::uint64_t>(publication.controlIndex),
                .hasPresent = publication.hasPresent,
            });
          }
          ++sourceRowCursor;
          ++settledRange;
          ++result.captureSourcePlanRanges;
          result.captureSourcePlanPublications += settlement.publications.size();
          if (settlement.kind ==
              dxmt9::d3d9::ProviderReplaySourceSettlementKind::Empty)
            ++result.captureSourcePlanEmptySources;
          if (settlement.kind ==
              dxmt9::d3d9::ProviderReplaySourceSettlementKind::PresentPublished)
            ++result.captureSourcePlanPresentSources;
          const auto callbackBegin = options_.measurement
              ? std::chrono::steady_clock::time_point{}
              : std::chrono::steady_clock::now();
          const bool published = publishReady(event, false, &settlement.publications);
          if (!options_.measurement) callbackNs += elapsed(callbackBegin);
          return published;
        };
        struct SourceSettlementContext {
          using Settle = decltype(settle);
          const Settle* settle = nullptr;

          static bool invoke(
              void* opaque,
              const dxmt9::d3d9::ProviderReplaySourceSettlement& settlement) noexcept {
            auto* self = static_cast<SourceSettlementContext*>(opaque);
            if (!self || !self->settle) return false;
            try {
              return (*self->settle)(settlement);
            } catch (...) {
              return false;
            }
          }
        } settlementContext{.settle = &settle};
        const dxmt9::d3d9::ProviderReplaySourceConsumer consumer{
            .context = &settlementContext,
            .settled = &SourceSettlementContext::invoke};
        if (dxmt9::d3d9::replayPrevalidatedResolvedCommandChunkBySources(
                fixture_.cDevice.get(), chunk.bytes, chunk.envelope, resolved,
                options_.route, ranges, consumer) != core::D3D_OK ||
            settledRange != ranges.size() || sourceRowCursor != sourceRowEnd)
          return fail(event, "general-executor-command-source-replay-failed");
        if (!options_.measurement) {
          const auto elapsedReplay = elapsed(replayBegin);
          result.replayNs += elapsedReplay >= callbackNs
              ? elapsedReplay - callbackNs
              : 0u;
        }
      } else if (options_.captureSourcePlanV3 && commandChunkIndex.has_value()) {
        sourceDriven = true;
        if (!executionV3.bindCommandChunk(chunk.bytes, chunk.envelope, resolved) ||
            d3d9::replayPrevalidatedResolvedCommandChunkV3(
                fixture_.cDevice.get(), chunk.bytes, chunk.envelope, resolved,
                options_.route, executionV3.recordConsumer()) != core::D3D_OK)
          return fail(event, executionV3.failed() ? executionV3.failure()
              : "general-executor-v3-command-replay");
      } else if (dxmt9::d3d9::replayPrevalidatedResolvedCommandChunk(
                     fixture_.cDevice.get(), chunk.bytes, chunk.envelope, resolved,
                     options_.route) != core::D3D_OK) {
        return fail(event, "general-executor-command-replay-failed");
      }
      if (!options_.measurement && !sourceDriven)
        result.replayNs += elapsed(replayBegin);
      result.commands += imported.records.size();
      for (std::size_t recordIndex = 0u; recordIndex < imported.records.size(); ++recordIndex) {
        const auto& record = imported.records[recordIndex];
        if (record.type == D9C_COMMAND_RECORD_DRAW_PRIMITIVE ||
            record.type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE)
          ++result.draws;
        if (record.type == D9C_COMMAND_RECORD_QUERY_ISSUE) {
          D9CCommandChunkWireQueryIssue issue{};
          const auto view = imported.record(recordIndex);
          if (view.payload.size() < sizeof(issue))
            return fail(event, "general-executor-query-issue-payload");
          std::memcpy(&issue, view.payload.data(), sizeof(issue));
          if (issue.queryHandleIndex >= imported.handles.size())
            return fail(event, "general-executor-query-issue-handle");
          const auto& handle = imported.handles[issue.queryHandleIndex];
          const D9CWireObjectIdentity identity{handle.kind, handle.generation, handle.objectId};
          auto cycle = std::find_if(queryCycles.begin(), queryCycles.end(), [&](const auto& candidate) {
            return detail::cpuGeneralSameIdentity(candidate.identity, identity);
          });
          auto* query = static_cast<D9CQuery*>(resolved[issue.queryHandleIndex]);
          if (cycle == queryCycles.end() || !query || !query->obj ||
              !d3d9::renderTapeAdvanceQueryCycle(cycle->state, issue.flags, true))
            return fail(event, "general-executor-query-cycle-transition");
          ++cycle->expectedLocalIssueOrdinal;
          cycle->snapshot = fixture_.cDevice->dev().captureQueryPollSnapshot(query->obj);
          ++result.queryReplay.replayedIssueRecords;
          if (!options_.measurement) {
            detail::cpuTapeDigestScalar(result.querySemanticDigest, identity.kind);
            detail::cpuTapeDigestScalar(result.querySemanticDigest, identity.generation);
            detail::cpuTapeDigestScalar(result.querySemanticDigest, identity.objectId);
            detail::cpuTapeDigestScalar(result.querySemanticDigest, issue.flags);
            detail::cpuTapeDigestScalar(result.querySemanticDigest, cycle->state.issueCycle);
          }
        }
      }
      if (queryBearing) {
        for (const auto& cycle : queryCycles) {
          if (cycle.snapshot.issueOrdinal != cycle.expectedLocalIssueOrdinal ||
              (cycle.expectedLocalIssueOrdinal && cycle.snapshot.active != cycle.state.active))
            return fail(event, "general-executor-query-local-issue-coverage");
        }
      }
      if (sourceDriven) return result.failure.empty();
      return publishReady(event, windowEligible);
    };

    const auto replayBootstrap = [&](std::uint32_t event) {
      if (bootstrapDone) return true;
      bootstrapDone = true;
      for (const auto& chunk : plan_.bootstrapChunks)
        if (!replayChunk(event, chunk)) return false;
      return true;
    };

    std::uint32_t v3PublicationEvent = 0xffffffffu;
    const auto drainV3Source = [&](const core::metalqueue::ReplaySourcePublicationReceipt& receipt) {
      const std::span<const core::metalqueue::ReplaySourcePublicationReceipt> receipts(&receipt, 1u);
      return publishReady(v3PublicationEvent, false, &receipts, true);
    };
    struct V3SourceContext {
      const decltype(drainV3Source)* drain = nullptr;
      static bool consume(void* opaque,
          const core::metalqueue::ReplaySourcePublicationReceipt& receipt) noexcept {
        try { return (*static_cast<V3SourceContext*>(opaque)->drain)(receipt); }
        catch (...) { return false; }
      }
    } v3SourceContext{&drainV3Source};

    const auto mutation = [&](std::uint32_t event,
                              const auto& fixed) {
      // Cached byte proofs never survive a possible CPU mutation, including
      // DISCARD that legally reuses an address after immediate completion.
      snapshotProofs.invalidate();
      return replayCpuMutation(event, fixed, objects, receipts, result,
          expectedUploadHandle, expectedUploadLevel, expectedUploadShape,
          expectedUploadBytes, observedUpload, fail);
    };

    struct MutationBoundary {
      CpuTapeMutationResult decision = CpuTapeMutationResult::ReleaseBeforeMutation;
      core::Handle target{};
      D9CWireObjectIdentity identity{};
      std::uint64_t priorRevision = 0u;
    };
    const auto mutationBoundary = [&](const auto& fixed) {
      MutationBoundary boundary;
      if (!options_.holdSafeBufferMutation || result.submitted == result.completed ||
          pendingSources.size() != 1u ||
          Access::pendingCompletionCount(fixture_.routing->queue_) != 1u ||
          fixed.identity.kind != D9C_CHUNK_HANDLE_KIND_BUFFER) return boundary;
      const auto* pendingSource = &pendingSources.front();
      const auto disposition = static_cast<d3d9::RenderTapeBufferMutationDisposition>(
          fixed.bufferDisposition);
      // NoOverwrite and managed rotation need separate witnesses. Never
      // classify an arbitrary non-DISCARD operation as Plain.
      if (disposition != d3d9::RenderTapeBufferMutationDisposition::Plain &&
          disposition != d3d9::RenderTapeBufferMutationDisposition::Discard) return boundary;
      auto* buffer = static_cast<D9CBuffer*>(
          d3d9::render_tape_runtime::findExactObject(objects, fixed.identity));
      d3d9::RenderTapeBufferMutationReplayPlan replayPlan{};
      if (!buffer || !buffer->obj || !d3d9::renderTapeBufferMutationReplayPlan(fixed, replayPlan))
        return boundary;
      boundary.target = buffer->obj->handle();
      boundary.identity = fixed.identity;
      auto& pool = fixture_.routing->queue_.pool();
      const auto* record = pool.findBuffer(boundary.target.value);
      if (!record) return boundary;
      boundary.priorRevision = record->contentRevision;
      const bool discard = disposition == d3d9::RenderTapeBufferMutationDisposition::Discard;
      const bool referenced = pendingSource &&
          std::find(pendingSource->resourceBuffers.begin(),
                    pendingSource->resourceBuffers.end(), boundary.target) !=
              pendingSource->resourceBuffers.end();
      bool hasCaptured = false, validCaptured = true;
      if (pendingSource) for (const auto& retained : pendingSource->snapshots) {
        if (retained.buffer != boundary.target) continue;
        hasCaptured |= retained.captured.has_value();
        validCaptured &= retained.captured &&
            pool.validateCapturedBufferSnapshot(boundary.target, *retained.captured) ==
                resources::CapturedBufferSnapshotStatus::Valid;
      }
      boundary.decision = reduceCpuTapeMutationAdmission({
          .exactlyOnePending = result.submitted - result.completed == 1u &&
              Access::pendingCompletionCount(fixture_.routing->queue_) == 1u,
          .alreadyDeferredMutation = pendingSource && pendingSource->mutationHeld,
          .bufferResolved = true,
          .defaultPool = buffer->desc.pool == 0u,
          .referencedByPending = referenced,
          .versioned = record->hasVersionedBacking(),
          .discard = discard,
          .noWait = pool.mapWaitSeqId(boundary.target, discard ? core::UsageDiscard : 0u) == 0u,
          .capturedBackingValid = referenced && hasCaptured && validCaptured});
      return boundary;
    };
    const auto verifyDeferredMutation = [&](std::uint32_t event, const MutationBoundary& boundary) {
      auto& pool = fixture_.routing->queue_.pool();
      const auto* after = pool.findBuffer(boundary.target.value);
      const auto* expected = expectedBuffer(boundary.target);
      if (!after || !expected || after->contentRevision <= boundary.priorRevision)
        return fail(event, "general-executor-deferred-mutation-revision");
      const CpuTapeLiveBufferView view{after->buffer.handle,
          static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(after->contents)),
          after->desc.size, after->contentRevision};
      std::uint64_t content = 0u;
      if (!snapshotProofs.proveLive(boundary.target, view, expected->obj->bytes(), content))
        return fail(event, "general-executor-deferred-mutation-upload");
      if (pendingSources.size() != 1u)
        return fail(event, "general-executor-deferred-mutation-without-source");
      auto& pendingSource = pendingSources.front();
      for (const auto& retained : pendingSource.snapshots) {
        if (!Access::bufferAlive(fixture_.routing->queue_, retained.buffer))
          return fail(event, "general-executor-mutation-premature-retirement");
        if (retained.buffer == boundary.target &&
            boundary.decision == CpuTapeMutationResult::DeferCapturedDiscard &&
            retained.view.metalHandle == after->buffer.handle)
          return fail(event, "general-executor-discard-reused-pending-backing");
        if (options_.corruptRetainedBytesAfterMutationForTest) {
          auto* bytes = reinterpret_cast<std::uint8_t*>(
              static_cast<std::uintptr_t>(retained.view.contentsAddress));
          bytes[0] ^= 1u;
        }
        std::uint64_t digest = 1469598103934665603ull;
        detail::cpuGeneralHashBytes(digest, {
            reinterpret_cast<const std::uint8_t*>(
                static_cast<std::uintptr_t>(retained.view.contentsAddress)),
            static_cast<std::size_t>(retained.view.byteSize)});
        if (digest != retained.digest)
          return fail(event, "general-executor-mutation-changed-pending-bytes");
        ++result.retainedBuffersAfterMutation;
      }
      ++result.deferredBufferMutations;
      result.deferredDiscardMutations +=
          boundary.decision == CpuTapeMutationResult::DeferCapturedDiscard;
      pendingSource.mutationHeld = true;
      if (pendingSource.traceOrdinal >= result.trace.size())
        return fail(event, "general-executor-held-mutation-without-source");
      result.trace[pendingSource.traceOrdinal].heldMutationEvent = event + 1u;
      result.trace[pendingSource.traceOrdinal].heldMutationIdentity = boundary.identity;
      return true;
    };

    const auto replayQueryPoll = [&](std::uint32_t event, std::uint32_t index) {
      using Failure = d3d9::RenderTapeQueryReplayFailure;
      auto& evidence = result.queryReplay;
      if (index >= plan_.controls.size() || index >= plan_.controlBytes.size() ||
          plan_.controlBytes[index].size() != sizeof(d3d9::RenderTapeQueryGetDataControlV2))
        return fail(event, "general-executor-query-control-index");
      const auto& fixed = plan_.controls[index];
      d3d9::RenderTapeQueryGetDataControlV2 poll{};
      std::memcpy(&poll, plan_.controlBytes[index].data(), sizeof(poll));
      auto cycle = std::find_if(queryCycles.begin(), queryCycles.end(), [&](const auto& candidate) {
        return detail::cpuGeneralSameIdentity(candidate.identity, fixed.identity);
      });
      auto* wrapped = static_cast<D9CQuery*>(d3d9::render_tape_runtime::findExactObject(
          objects, fixed.identity));
      d3d9::RenderTapeQueryReplayRow row{
          .eventIndex = event, .eventOrdinal = plan_.tape.event(event).header.ordinal,
          .identity = fixed.identity, .issueCycle = poll.issueCycle, .flags = poll.flags,
          .hasOutput = poll.hasOutput != 0u, .dataSize = poll.dataSize,
          .capturedResult = fixed.resultCode, .definedBytes = poll.definedBytes,
          .capturedBytes = poll.result, .resultRequired = fixed.resultCode == core::D3D_OK};
      const auto queryFail = [&](Failure reason, const char* name) {
        evidence.failure = reason;
        if (!options_.measurement) evidence.rows.push_back(row);
        return fail(event, name);
      };
      if (!wrapped || !wrapped->obj || cycle == queryCycles.end() ||
          !d3d9::renderTapeQueryPollCycleAdmitted(cycle->state, poll.issueCycle,
                                                fixed.resultCode))
        return queryFail(Failure::CycleMismatch, "general-executor-query-cycle-mismatch");
      const auto snapshot = fixture_.cDevice->dev().captureQueryPollSnapshot(wrapped->obj);
      if (!snapshot.eligible || snapshot.identity != wrapped->obj->identity() ||
          cycle->snapshot.issueOrdinal != snapshot.issueOrdinal ||
          (snapshot.issueOrdinal && snapshot.issueDeviceEpoch != cycle->snapshot.issueDeviceEpoch))
        return queryFail(Failure::SnapshotUnavailable, "general-executor-query-snapshot-unavailable");
      row.localQueryIdentity = snapshot.identity;
      row.localIssueOrdinal = snapshot.issueOrdinal;
      row.localDeviceEpoch = snapshot.issueDeviceEpoch;
      const auto fence = snapshot.backendFence.snapshot();
      row.localQueue = fence.queue;
      row.localSequence = fence.sequence;
      row.localInitializer = fence.initializer;
      std::array<std::byte, 8> output{};
      output.fill(std::byte{0xa5});
      const auto transcriptBegin = options_.measurement
          ? std::chrono::steady_clock::time_point{}
          : std::chrono::steady_clock::now();
      if (!consumeProgress(event)) return false;
      row.replayResult = dxmt9c_query_get_data(wrapped,
          poll.hasOutput ? output.data() : nullptr,
          static_cast<std::uint32_t>(poll.dataSize), poll.flags);
      if (!options_.measurement) evidence.transcriptWallNanos += elapsed(transcriptBegin);
      ++evidence.replayedPolls;
      evidence.capturedPending += fixed.resultCode == core::S_FALSE;
      evidence.capturedReady += fixed.resultCode == core::D3D_OK;
      evidence.replayPending += row.replayResult == core::S_FALSE;
      evidence.replayReady += row.replayResult == core::D3D_OK;
      row.oracleResult = row.replayResult;
      if (row.replayResult != core::D3D_OK &&
          std::any_of(output.begin(), output.end(), [](auto byte) { return byte != std::byte{0xa5}; }))
        return queryFail(Failure::PollFailed, "general-executor-query-undefined-output-written");
      if (fixed.resultCode < 0 && row.replayResult != fixed.resultCode)
        return queryFail(Failure::PollFailed, "general-executor-query-captured-failure-mismatch");
      if (row.replayResult < 0 && fixed.resultCode >= 0)
        return queryFail(Failure::PollFailed, "general-executor-query-poll-failed");
      if (row.resultRequired) {
        ++evidence.readinessRequired;
        evidence.requiredResults += poll.definedBytes != 0u;
        const auto oracleBegin = options_.measurement
            ? std::chrono::steady_clock::time_point{}
            : std::chrono::steady_clock::now();
        while (row.oracleResult == core::S_FALSE && row.oraclePolls < options_.maxQueryOraclePolls) {
          if (!pendingSources.empty()) {
            ++evidence.supplementalDrains;
            if (!releasePending(event))
              return queryFail(Failure::PollFailed, "general-executor-query-progress-release");
          }
          ++row.oraclePolls;
          ++evidence.supplementalPolls;
          row.supplemental = true;
          if (!consumeProgress(event)) return false;
          row.oracleResult = fixture_.cDevice->dev().pollQuerySnapshot(snapshot,
              poll.hasOutput ? output.data() : nullptr, poll.dataSize, poll.flags);
        }
        if (!options_.measurement) evidence.oracleWallNanos += elapsed(oracleBegin);
        if (row.oracleResult == core::S_FALSE) {
          evidence.progressLimit = d3d9::RenderTapeQueryProgressLimit::PollBudget;
          return queryFail(Failure::ProgressExhausted, "general-executor-query-progress-exhausted");
        }
        if (row.oracleResult != core::D3D_OK)
          return queryFail(Failure::PollFailed, "general-executor-query-oracle-poll-failed");
        const auto progress = fixture_.routing->queryProgress();
        row.completedSequence = progress.completed;
        row.completedInitializer = progress.initializerCompleted;
        if (!snapshot.constant() && !snapshot.preIssue() &&
            dxmt9::query::classifyEnd(snapshot.backendFence, progress) != dxmt9::query::Poll::Complete)
          return queryFail(Failure::SnapshotUnavailable, "general-executor-query-premature-result");
        row.readinessSatisfied = true;
        ++evidence.readinessSatisfied;
        const bool substituted = cycle->state.type == 9u && poll.definedBytes != 0u;
        row.resultSubstituted = substituted;
        evidence.substitutedResults += substituted;
        if (poll.definedBytes != 0u) {
          std::copy_n(output.begin(), poll.definedBytes, row.replayBytes.begin());
          if (!substituted) {
            row.resultMatched = std::equal(row.capturedBytes.begin(),
                row.capturedBytes.begin() + poll.definedBytes, output.begin());
            if (!row.resultMatched)
              return queryFail(Failure::ResultMismatch, "general-executor-query-result-mismatch");
            ++evidence.matchedResults;
          }
        }
      }
      if (!options_.measurement) {
        detail::cpuTapeDigestScalar(result.querySemanticDigest, fixed.identity.kind);
        detail::cpuTapeDigestScalar(result.querySemanticDigest, fixed.identity.generation);
        detail::cpuTapeDigestScalar(result.querySemanticDigest, fixed.identity.objectId);
        detail::cpuTapeDigestScalar(result.querySemanticDigest, poll.issueCycle);
        detail::cpuTapeDigestScalar(result.querySemanticDigest, poll.flags);
        detail::cpuTapeDigestScalar(result.querySemanticDigest, poll.hasOutput);
        detail::cpuTapeDigestScalar(result.querySemanticDigest, poll.dataSize);
        detail::cpuTapeDigestScalar(result.querySemanticDigest, fixed.resultCode);
        detail::cpuGeneralHashBytes(result.querySemanticDigest,
            {reinterpret_cast<const std::uint8_t*>(poll.result.data()), poll.definedBytes});
        evidence.rows.push_back(row);
      }
      return true;
    };

    const auto executeEvents = [&]() -> bool {
      for (const auto& replayEvent : plan_.events) {
        const auto kind = replayEvent.type;
        CpuDiagnosticLedger::Scope phase(options_.diagnostic,
            kind == d3d9::RenderTapeEventType::ObjectDefine ? CpuDiagnosticPhase::ObjectSetup :
            kind == d3d9::RenderTapeEventType::ResourceMutation ? CpuDiagnosticPhase::Mutation :
            kind == d3d9::RenderTapeEventType::CommandChunk ? CpuDiagnosticPhase::Replay :
            kind == d3d9::RenderTapeEventType::ObjectDestroy ? CpuDiagnosticPhase::Cleanup :
            CpuDiagnosticPhase::Bookkeeping);
        const auto event = replayEvent.eventIndex;
        v3PublicationEvent = event;
        if (options_.captureSourcePlanV3) {
          if (event == executionV3.snapshotPrefixEvents()) {
            if (!replayBootstrap(event) ||
                !executionV3.arm(fixture_.cDevice.get(),
                    {.context = &v3SourceContext, .published = &V3SourceContext::consume}))
              return fail(event, executionV3.failed() ? executionV3.failure()
                  : "general-executor-v3-snapshot-setup");
          }
          if (!executionV3.beginEvent(event)) return fail(event, executionV3.failure());
        }
        MutationBoundary boundary;
        const bool pendingMutation = replayEvent.type == d3d9::RenderTapeEventType::ResourceMutation &&
            result.submitted != result.completed;
        if (pendingMutation && replayEvent.index < plan_.mutations.size())
          boundary = mutationBoundary(plan_.mutations[replayEvent.index]);
        const bool holdMutation = boundary.decision != CpuTapeMutationResult::ReleaseBeforeMutation;
        result.mutationBoundaryReleases += options_.holdSafeBufferMutation && pendingMutation && !holdMutation;
        if (!holdMutation) {
          bool queryOnly = false;
          if (queryBearing && replayEvent.type == d3d9::RenderTapeEventType::CommandChunk &&
              replayEvent.index < plan_.commandChunks.size()) {
            const auto& chunk = plan_.commandChunks[replayEvent.index];
            d3d9::ImportedChunkView imported;
            queryOnly = d3d9::importPrevalidatedCommandChunk(chunk.bytes, chunk.envelope, imported) &&
                !imported.records.empty() &&
                std::all_of(imported.records.begin(), imported.records.end(), [](const auto& record) {
                  return record.type == D9C_COMMAND_RECORD_QUERY_ISSUE;
                });
          }
          const bool eligible = options_.maxPendingSources > 1u &&
              replayEvent.type == d3d9::RenderTapeEventType::CommandChunk &&
              replayEvent.index < plan_.commandChunks.size() &&
              overlapEligible(plan_.commandChunks[replayEvent.index], event);
          const auto action = cpuTapeWindowAction(pendingSources.size(),
              options_.maxPendingSources,
              queryOnly || (replayEvent.type == d3d9::RenderTapeEventType::OrderedControl && queryBearing)
                  ? false
                  : releaseBeforeCpuTapeWork(replayEvent.type,
                                             options_.beforeNextWork || queryBearing),
              eligible, pendingWindowEligible());
          if (action == CpuTapeWindowAction::Reject)
            return fail(event, "general-executor-window-invalid-state");
          if (action == CpuTapeWindowAction::ReleaseOldest && !releaseOne(event)) return false;
          if (action == CpuTapeWindowAction::DrainAll && !releasePending(event)) return false;
        }
        const auto eventBegin = options_.measurement
            ? std::chrono::steady_clock::time_point{}
            : std::chrono::steady_clock::now();
        if (!detail::dispatchCpuGeneralTapeEvent(replayEvent.type, [&] {
          bootstrapPending = true;
          for (const auto& gamma : plan_.bootstrapGamma) {
            if (gamma.empty()) continue;
            if (gamma.size() != dxmt9::d3d9::kRenderTapeGammaRampBytes)
            {
              fail(event, "general-executor-bootstrap-gamma-size");
              return false;
            }
            dxmt9c_device_set_gamma_ramp(
                fixture_.cDevice.get(),
                reinterpret_cast<const std::uint16_t*>(gamma.data()));
          }
          return true;
        }, [&] {
          snapshotProofs.invalidate();
          if (replayEvent.index >= plan_.definitions.size())
          {
            fail(event, "general-executor-definition-index");
            return false;
          }
          const auto& definition = plan_.definitions[replayEvent.index];
          auto hidden = std::find_if(hiddenCheckpoints.begin(), hiddenCheckpoints.end(),
              [&](const auto& owner) {
                return detail::cpuGeneralSameIdentity(owner.object.identity,
                                                     definition.fixed.identity);
              });
          if (hidden != hiddenCheckpoints.end() && (hidden->activated || !hidden->object.value))
            return fail(event, "general-executor-checkpoint-double-activation");
          void* value = hidden == hiddenCheckpoints.end()
              ? create(definition) : std::exchange(hidden->object.value, nullptr);
          if (!value) {
            fail(event, "general-executor-object-create-failed");
            return false;
          }
          objects.push_back({.identity = definition.fixed.identity, .value = value});
          if (definition.fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_QUERY) {
            d3d9::RenderTapeQueryDescriptorDecoded desc{};
            if (!d3d9::renderTapeLoadQueryDescriptor(definition.descriptor, desc) ||
                !d3d9::renderTapeQueryDescriptorIdentityMatches(desc, definition.fixed.identity))
              return fail(event, "general-executor-query-descriptor");
            if (hidden != hiddenCheckpoints.end()) {
              hidden->activated = true;
              queryCycles.push_back(hidden->cycle);
              ++result.queryReplay.checkpointActivated;
              if (!options_.measurement)
                result.queryReplay.checkpoints[hidden->evidenceIndex].activated = true;
            } else {
              if (desc.initialState == static_cast<std::uint32_t>(
                      d3d9::RenderTapeQueryInitialState::CompletedEventCheckpoint))
                return fail(event, "general-executor-checkpoint-setup-missing");
              queryCycles.push_back({.identity = definition.fixed.identity,
                  .state = d3d9::renderTapeInitialQueryCycle(desc)});
            }
          }
          ++result.resourcesCreated;
          return true;
        }, [&] {
          if (replayEvent.index >= plan_.mutations.size() ||
              !mutation(event, plan_.mutations[replayEvent.index]))
            return false;
          if (holdMutation && !verifyDeferredMutation(event, boundary)) return false;
          return true;
        }, [&] {
          if (bootstrapPending && !replayBootstrap(event)) return false;
          if (replayEvent.index >= plan_.commandChunks.size() ||
              !replayChunk(event, plan_.commandChunks[replayEvent.index],
                           replayEvent.index))
            return false;
          return true;
        }, [&] {
          snapshotProofs.invalidate();
          result.destroysWithPending += result.submitted != result.completed;
          if (bootstrapPending && !replayBootstrap(event)) return false;
          if (replayEvent.index >= plan_.destroys.size())
          {
            fail(event, "general-executor-destroy-index");
            return false;
          }
          const auto& destroy = plan_.destroys[replayEvent.index];
          auto object = std::find_if(
              objects.begin(), objects.end(), [&](const auto& candidate) {
                return candidate.value && detail::cpuGeneralSameIdentity(
                    candidate.identity, destroy.identity);
              });
          if (object == objects.end())
          {
            fail(event, "general-executor-stale-destroy");
            return false;
          }
          if (destroy.identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER) {
            auto* value = static_cast<D9CBuffer*>(object->value);
            if (value && value->obj)
              tombstones.push_back({destroy.identity, value->obj->handle()});
          } else if (destroy.identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
            auto* value = static_cast<D9CTexture*>(object->value);
            if (value && value->obj)
              tombstones.push_back({destroy.identity, value->obj->handle()});
          } else if (destroy.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE) {
            auto* value = static_cast<D9CSurface*>(object->value);
            if (value && value->obj)
              tombstones.push_back({destroy.identity, value->obj->handle()});
          }
          dxmt9::d3d9::render_tape_runtime::releaseOwnedObject(*object);
          ++result.resourcesReleased;
          if (result.submitted != result.completed) {
            const auto retentionBegin = options_.measurement
                ? std::chrono::steady_clock::time_point{}
                : std::chrono::steady_clock::now();
            CpuGeneralTapeDestroyWindow window{.event = event + 1u};
            // No mutation/work is allowed in this pending epoch. A wrapper
            // release must not destroy a submitted draw's concrete bytes.
            if (pendingSources.empty())
              return fail(event, "general-executor-destroy-without-source-proof");
            // Exercise real retirement at the actual completion waterline;
            // merely finding an entry before reclaim would not prove retention.
            Access::reclaimCompletedPool(fixture_.routing->queue_);
            for (const auto& owner : pendingSources) {
              if (owner.traceOrdinal >= result.trace.size() ||
                  !owner.matches(result.trace[owner.traceOrdinal]))
                return fail(event, "general-executor-destroy-owner-identity");
              window.pendingSubmissions.push_back(owner.submission);
              for (const auto& retained : owner.mipmapTextures) {
                const auto* record = fixture_.routing->queue_.pool().findTexture(retained.handle.value);
                if (!record || reinterpret_cast<std::uintptr_t>(record) != retained.recordAddress ||
                    record->lastUsedSeqId < owner.submission)
                  return fail(event, "general-executor-premature-mipmap-retirement");
                ++result.retainedMipmapTexturesAfterDestroy;
              }
              for (const auto& retained : owner.snapshots) {
                if (!Access::bufferAlive(fixture_.routing->queue_, retained.buffer))
                  return fail(event, "general-executor-premature-buffer-retirement");
                if (options_.corruptRetainedBytesForTest) {
                  auto* bytes = reinterpret_cast<std::uint8_t*>(
                      static_cast<std::uintptr_t>(retained.view.contentsAddress));
                  bytes[0] ^= 1u;
                }
                std::uint64_t digest = 1469598103934665603ull;
                detail::cpuGeneralHashBytes(digest, {
                    reinterpret_cast<const std::uint8_t*>(
                        static_cast<std::uintptr_t>(retained.view.contentsAddress)),
                    static_cast<std::size_t>(retained.view.byteSize)});
                if (digest != retained.digest)
                  return fail(event, "general-executor-retained-buffer-bytes-changed");
                ++result.retainedSnapshotsAfterDestroy;
              }
              ++window.ownersChecked;
            }
            window.retentionValid = true;
            result.destroyWindows.push_back(std::move(window));
            if (!options_.measurement) result.observeNs += elapsed(retentionBegin);
          }
          return true;
        }, [&] {
          return replayQueryPoll(event, replayEvent.index);
        }, [] { return true; })) return false;
        if (options_.captureSourcePlanV3 &&
            replayEvent.type != d3d9::RenderTapeEventType::CommandChunk &&
            !executionV3.replayedEvent(event)) return fail(event, executionV3.failure());
        if (!checkPublications(event)) return false;
        if (replayEvent.type == d3d9::RenderTapeEventType::ObjectDefine ||
            replayEvent.type == d3d9::RenderTapeEventType::ResourceMutation) {
          if (!options_.measurement) result.setupNs += elapsed(eventBegin);
        }
      }
      if (bootstrapPending && !bootstrapDone && !replayBootstrap(
                                      plan_.events.empty()
                                          ? 0xffffffffu
                                          : plan_.events.back().eventIndex))
        return false;
      if (!options_.sourceOracle.empty() &&
          sourceOracleRow != options_.sourceOracle.size())
        return fail(plan_.events.empty() ? 0xffffffffu
                                         : plan_.events.back().eventIndex,
                    "general-executor-source-oracle-missing-row");
      if (result.completed != result.submitted)
        return fail(plan_.events.back().eventIndex, "general-executor-unresolved-schedule");
      if (queryBearing) {
        auto& evidence = result.queryReplay;
        evidence.transcriptComplete = evidence.replayedIssueRecords == evidence.issueRecords &&
            evidence.replayedPolls == evidence.recordedPolls;
        evidence.oracleComplete = evidence.transcriptComplete &&
            evidence.readinessRequired == evidence.readinessSatisfied &&
            evidence.checkpointDefinitions == evidence.checkpointSetupEnds &&
            evidence.checkpointDefinitions == evidence.checkpointCompleted &&
            evidence.checkpointDefinitions == evidence.checkpointActivated;
        if (!evidence.oracleComplete)
          return fail(plan_.events.back().eventIndex, "general-executor-query-incomplete");
      }
      return result.failure.empty();
    };
    diagnostic.set(CpuDiagnosticPhase::Bookkeeping);
    try {
    const auto restoreCheckpoints = [&]() {
      CpuDiagnosticLedger::Scope phase(options_.diagnostic, CpuDiagnosticPhase::HarnessSetup);
      auto& evidence = result.queryReplay;
      std::chrono::steady_clock::time_point setupBegin{};
      struct SetupTiming {
        d3d9::RenderTapeQueryReplayEvidence& evidence;
        std::chrono::steady_clock::time_point& begin;
        ~SetupTiming() {
          if (evidence.checkpointDefinitions)
            evidence.checkpointSetupWallNanos = elapsed(begin);
        }
      } setupTiming{evidence, setupBegin};
      for (const auto& definition : plan_.definitions) {
        if (definition.fixed.identity.kind != D9C_CHUNK_HANDLE_KIND_QUERY) continue;
        d3d9::RenderTapeQueryDescriptorDecoded descriptor{};
        if (!d3d9::renderTapeLoadQueryDescriptor(definition.descriptor, descriptor))
          return fail(0xffffffffu, "general-executor-checkpoint-descriptor");
        if (descriptor.initialState != static_cast<std::uint32_t>(
                d3d9::RenderTapeQueryInitialState::CompletedEventCheckpoint)) continue;
        if (evidence.checkpointDefinitions++ == 0u) setupBegin = std::chrono::steady_clock::now();
        if (!d3d9::renderTapeQueryDescriptorIdentityMatches(descriptor, definition.fixed.identity))
          return fail(0xffffffffu, "general-executor-checkpoint-identity");
        auto* query = static_cast<D9CQuery*>(create(definition));
        if (!query || !query->obj)
          return fail(0xffffffffu, "general-executor-checkpoint-create");
        hiddenCheckpoints.push_back({
            .object = {.identity = definition.fixed.identity, .value = query},
            .cycle = {.identity = definition.fixed.identity,
                      .state = d3d9::renderTapeInitialQueryCycle(descriptor)},
            .evidenceIndex = evidence.checkpoints.size()});
        auto& owner = hiddenCheckpoints.back();
        const auto before = fixture_.routing->queryProgress();
        if (options_.checkpointFault != CheckpointFault::MissingEnd) {
          if (dxmt9c_query_issue(query, 1u) != core::D3D_OK)
            return fail(0xffffffffu, "general-executor-checkpoint-end");
          ++evidence.checkpointSetupEnds;
        }
        if (options_.checkpointFault == CheckpointFault::DoubleRestore) {
          (void)dxmt9c_query_issue(query, 1u);
          ++evidence.checkpointSetupEnds;
        }
        owner.cycle.snapshot = fixture_.cDevice->dev().captureQueryPollSnapshot(query->obj);
        if (options_.checkpointFault == CheckpointFault::StaleLocalIssue) {
          (void)dxmt9c_query_issue(query, 1u);
          ++evidence.checkpointSetupEnds;
        }
        owner.cycle.expectedLocalIssueOrdinal = 1u;
        const auto& snapshot = owner.cycle.snapshot;
        const auto fence = snapshot.backendFence.snapshot();
        const auto after = fixture_.routing->queryProgress();
        if (!checkPublications(0xffffffffu)) return false;
        evidence.checkpointSetupPublications += static_cast<std::uint32_t>(after.published - before.published);
        d3d9::RenderTapeQueryCheckpointReplayRow row{
            .identity = definition.fixed.identity,
            .capturedIssueCycle = descriptor.checkpoint.issueCycle,
            .capturedIssueOrdinal = descriptor.checkpoint.issueOrdinal,
            .capturedDeviceEpoch = descriptor.checkpoint.deviceEpoch,
            .localQueryIdentity = snapshot.identity,
            .localIssueOrdinal = snapshot.issueOrdinal,
            .localDeviceEpoch = snapshot.issueDeviceEpoch,
            .localQueue = fence.queue, .localSequence = fence.sequence,
            .localInitializer = fence.initializer,
            .setupIssued = options_.checkpointFault != CheckpointFault::MissingEnd};
        const auto current = fixture_.cDevice->dev().captureQueryPollSnapshot(query->obj);
        if (!snapshot.eligible || snapshot.active || snapshot.identity != query->obj->identity() ||
            snapshot.issueOrdinal != 1u || !snapshot.issueDeviceEpoch || !fence.queue ||
            current.issueOrdinal != snapshot.issueOrdinal ||
            current.issueDeviceEpoch != snapshot.issueDeviceEpoch)
          return fail(0xffffffffu, "general-executor-checkpoint-local-authority");
        ++row.setupPolls;
        ++evidence.checkpointSetupPolls;
        if (!consumeProgress(0xffffffffu)) return false;
        auto hr = fixture_.cDevice->dev().pollQuerySnapshot(snapshot, nullptr, 0u, 0u);
        while (hr == core::S_FALSE && row.setupPolls <= options_.maxQueryOraclePolls) {
          // Both production dequeue and acknowledgment can wait. In this
          // single-thread controller, only an existing ready source may be
          // submitted; initializer-only Pending cannot invent queue work.
          if (!Access::peekReadyCompletion(fixture_.routing->queue_).seqId) {
            evidence.failure = d3d9::RenderTapeQueryReplayFailure::ProgressExhausted;
            evidence.progressLimit = d3d9::RenderTapeQueryProgressLimit::PollBudget;
            return fail(0xffffffffu, "general-executor-checkpoint-progress-unavailable");
          }
          if (!consumeProgress(0xffffffffu)) return false;
          const auto source = Access::submitOneWithheld(fixture_.routing->queue_);
          if (!source.submitted || !Access::acknowledgeOne(fixture_.routing->queue_) ||
              !Access::finishQueue(fixture_.routing->queue_))
            return fail(0xffffffffu, "general-executor-checkpoint-completion");
          ++evidence.checkpointSetupWaits;
          ++row.setupPolls;
          ++evidence.checkpointSetupPolls;
          if (!consumeProgress(0xffffffffu)) return false;
          hr = fixture_.cDevice->dev().pollQuerySnapshot(snapshot, nullptr, 0u, 0u);
        }
        const auto progress = fixture_.routing->queryProgress();
        row.completedSequence = progress.completed;
        row.completedInitializer = progress.initializerCompleted;
        if (hr != core::D3D_OK ||
            dxmt9::query::classifyEnd(snapshot.backendFence, progress) != dxmt9::query::Poll::Complete)
          return fail(0xffffffffu, "general-executor-checkpoint-premature-result");
        row.completed = true;
        ++evidence.checkpointCompleted;
        if (!options_.measurement) evidence.checkpoints.push_back(row);
      }
      return true;
    };
    result.complete = restoreCheckpoints() && executeEvents();
    if (result.complete && options_.captureSourcePlanV3 && !executionV3.finish())
      result.complete = fail(plan_.events.back().eventIndex, executionV3.failure());
    } catch (...) {
      if (result.failure.empty()) {
        result.failure = "general-executor-exception";
        result.failedEvent = plan_.events.empty() ? 0xffffffffu
                                                  : plan_.events.back().eventIndex;
      }
    }

    diagnostic.set(CpuDiagnosticPhase::Cleanup);
    const auto cleanupBegin = options_.measurement
        ? std::chrono::steady_clock::time_point{}
        : std::chrono::steady_clock::now();
    // Failed execution is not an accepted sample, but it must still settle
    // every actually published no-GPU source before dropping retained owners.
    if (!result.complete) {
      // Detach the cold observer before any recovery can release owners or
      // deliver further completion edges into a rejected execution session.
      executionV3.cancel();
      recovering = true;
      (void)publishReady(result.failedEvent);
      for (std::size_t pending = 0u;
           pending < dxmt9::kMaxQueuedChunks &&
           Access::pendingCompletionCount(fixture_.routing->queue_) != 0u;
           ++pending) {
        if (!Access::acknowledgeOne(fixture_.routing->queue_) ||
            !Access::finishQueue(fixture_.routing->queue_)) break;
      }
    }
    for (auto& owner : hiddenCheckpoints)
      dxmt9::d3d9::render_tape_runtime::releaseOwnedObject(owner.object);
    dxmt9::d3d9::render_tape_runtime::OwnedObject* deferredResetObject = nullptr;
    for (auto it = objects.rbegin(); it != objects.rend(); ++it) {
      if (!it->value) continue;
      if (it->identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER) {
        auto* value = static_cast<D9CBuffer*>(it->value);
        if (value && value->obj)
          tombstones.push_back({it->identity, value->obj->handle()});
      } else if (it->identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
        auto* value = static_cast<D9CTexture*>(it->value);
        if (value && value->obj)
          tombstones.push_back({it->identity, value->obj->handle()});
      } else if (it->identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE) {
        auto* value = static_cast<D9CSurface*>(it->value);
        if (value && value->obj)
          tombstones.push_back({it->identity, value->obj->handle()});
      }
      if (options_.deferOneObjectReleaseForResetTest &&
          deferredResetObject == nullptr) {
        deferredResetObject = &*it;
        continue;
      }
      dxmt9::d3d9::render_tape_runtime::releaseOwnedObject(*it);
      ++result.resourcesReleased;
    }
    // Only Tape-owned wrappers must be gone here. Core bindings/caches may
    // legitimately retain Pool entries until releaseCoreForInspection below.
    // Sample real outstanding ownership before recovery, not the fault flag.
    const bool resetOutstandingObjectObserved = std::any_of(
        objects.begin(), objects.end(), [](const auto& object) {
          return object.value != nullptr;
        });
    if (deferredResetObject != nullptr) {
      dxmt9::d3d9::render_tape_runtime::releaseOwnedObject(*deferredResetObject);
      ++result.resourcesReleased;
    }
    executionV3.cancel();
    fixture_.releaseCoreForInspection();
    Access::reclaimCompletedPool(fixture_.routing->queue_);
    bool resourcesGone = true;
    for (const auto& object : tombstones) {
      const auto& pool = fixture_.routing->queue_.pool();
      if (object.identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER)
        resourcesGone &= !pool.findBuffer(object.nativeHandle.value);
      else if (object.identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE)
        resourcesGone &= !pool.findTexture(object.nativeHandle.value);
      else if (object.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE)
        resourcesGone &= !pool.findSurface(object.nativeHandle.value);
    }
    result.resetOk = !resetOutstandingObjectObserved && resourcesGone &&
        Access::pendingCompletionCount(fixture_.routing->queue_) == 0u &&
        Access::readyCount(fixture_.routing->queue_) == 0u &&
        Access::writingCommandCount(fixture_.routing->queue_) == 0u &&
        Access::residentCount(fixture_.routing->queue_) == 0u;
    if (!options_.measurement) result.cleanupNs = elapsed(cleanupBegin);
    if (options_.measurement) {
      // A successful lifecycle/reset sample is not a semantic or pixel
      // oracle; the omitted observer counters must remain zero.
      if (result.sourceSummaryDraws != result.draws && result.failure.empty()) {
        result.failure = "general-executor-measurement-draw-conservation";
        result.complete = false;
      }
      result.digest = 0u;
      result.logicalResourceOracleComplete = false;
    } else {
      result.logicalResourceOracleComplete = result.failure.empty() &&
          result.observedMutations == plan_.mutations.size() &&
          result.drawObservations.size() == result.draws;
    }
    result.complete = result.complete && result.resetOk;
    if (!result.resetOk && result.failure.empty())
      result.failure = resetOutstandingObjectObserved
          ? "general-executor-reset-object-release-incomplete"
          : "general-executor-reset-ambiguous";
    return result;
  }

 private:

  // Both CPU execution lanes use the genuine provider lock/copy/unlock path.
  // Publication/progress and immutable context normalization belong to their
  // respective callers; this helper never submits or acknowledges a source.
  template <class ExecutionResult, class Fail>
  bool replayCpuMutation(std::uint32_t event,
      const d3d9::RenderTapeResourceMutationHeader& fixed,
      std::span<const d3d9::render_tape_runtime::OwnedObject> objects,
      std::vector<detail::LogicalReceipt>& receipts, ExecutionResult& result,
      core::Handle& expectedUploadHandle, std::uint32_t& expectedUploadLevel,
      detail::CpuGeneralTextureUploadShape& expectedUploadShape,
      std::span<const std::byte>& expectedUploadBytes, bool& observedUpload,
      Fail&& fail, bool retainLogicalReceipts = true) {
    const auto* blob = detail::cpuGeneralFindBlob(blobs_, fixed.digest);
    void* value = dxmt9::d3d9::render_tape_runtime::findExactObject(
        objects, fixed.identity);
    if (!blob || !value || blob->bytes.size() != fixed.byteSize)
      return fail(event, "general-executor-mutation-object-or-blob");
    bool ok = false;
    if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER) {
      dxmt9::d3d9::RenderTapeBufferMutationReplayPlan replayPlan{};
      void* bits = nullptr;
      auto* buffer = static_cast<D9CBuffer*>(value);
      ok = dxmt9::d3d9::renderTapeBufferMutationReplayPlan(fixed,
                                                            replayPlan) &&
           dxmt9c_buffer_lock(buffer, replayPlan.byteOffset,
                              replayPlan.byteSize, &bits,
                              replayPlan.lockFlags) == core::D3D_OK &&
           bits != nullptr;
      if (ok) {
        std::memcpy(bits, blob->bytes.data(), blob->bytes.size());
        ok = dxmt9c_buffer_unlock(buffer) == core::D3D_OK;
      }
    } else if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
      const auto definition = std::find_if(
          plan_.definitions.begin(), plan_.definitions.end(),
          [&](const auto& candidate) {
            return detail::cpuGeneralSameIdentity(candidate.fixed.identity,
                                                   fixed.identity);
          });
      D9CSurfaceDesc desc{};
      D9CLockedRect lock{};
      auto* texture = static_cast<D9CTexture*>(value);
      expectedUploadHandle = texture->obj->handle();
      expectedUploadLevel = fixed.subresource;
      expectedUploadBytes = blob->bytes;
      observedUpload = false;
      ok = definition != plan_.definitions.end() && fixed.byteOffset == 0u &&
           dxmt9::d3d9::renderTapeTextureSubresourceDescriptor(
               definition->descriptor, fixed.subresource, desc) &&
           detail::cpuGeneralTextureUploadShape(desc, expectedUploadShape) &&
           dxmt9c_texture_lock_rect(texture, fixed.subresource, &lock,
                                    nullptr, 0u) == core::D3D_OK &&
           lock.bits != nullptr;
      const bool copied = ok && detail::cpuGeneralWriteTextureMutationRows(
          definition->descriptor, *texture, fixed.subresource, fixed.byteOffset,
          lock, blob->bytes);
      const bool unlocked = ok &&
          dxmt9c_texture_unlock_rect(texture, fixed.subresource) ==
          core::D3D_OK;
      ok = copied && unlocked && observedUpload && !fixture_.routing->textureUploadFailed;
      expectedUploadBytes = {};
      expectedUploadShape = {};
    } else if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE) {
      const auto definition = std::find_if(
          plan_.definitions.begin(), plan_.definitions.end(),
          [&](const auto& candidate) {
            return detail::cpuGeneralSameIdentity(candidate.fixed.identity,
                                                   fixed.identity);
          });
      dxmt9::d3d9::RenderTapeSurfaceDescriptorV2 surface{};
      auto* wrapped = static_cast<D9CSurface*>(value);
      ok = definition != plan_.definitions.end() && fixed.subresource == 0u &&
           fixed.byteOffset == 0u &&
           dxmt9::d3d9::renderTapeLoadSurfaceDescriptorV2(definition->descriptor, surface);
      const auto disposition = ok
          ? static_cast<dxmt9::d3d9::RenderTapeInitialContentDisposition>(
                surface.initialContentDisposition)
          : dxmt9::d3d9::RenderTapeInitialContentDisposition::Unavailable;
      if (ok && disposition ==
                    dxmt9::d3d9::RenderTapeInitialContentDisposition::
                        CompleteDepthStencilFloat32UInt8V1) {
        dxmt9::d3d9::RenderTapeD24S8SnapshotLayout layout{};
        // Both canonical planes belong to this exact logical surface. They
        // are not packed D3D9 pixels or physical Metal backing bytes, and
        // the CPU lane must not issue a lock, seed blit or GPU submission.
        ok = wrapped && wrapped->obj &&
             dxmt9::d3d9::renderTapeDeriveD24S8SnapshotLayout(
                 surface.surface, layout) &&
             blob->bytes.size() == layout.byteCount;
      } else if (ok && disposition ==
                    dxmt9::d3d9::RenderTapeInitialContentDisposition::
                        CompleteDepthFloat32V1) {
        const auto expected = static_cast<std::uint64_t>(surface.surface.width) *
                              surface.surface.height * 4u;
        // D24X8's tape bytes are canonical logical float depth, not the
        // backend's packed/physical representation.  The CPU executor
        // records this receipt for semantic observation; it deliberately
        // does not invoke a GPU seed/blit or claim physical depth bytes.
        ok = wrapped && wrapped->obj && blob->bytes.size() == expected;
      } else if (ok && disposition ==
                           dxmt9::d3d9::RenderTapeInitialContentDisposition::
                               CompleteSeed) {
        D9CLockedRect lock{};
        ok = wrapped &&
             dxmt9c_surface_lock_rect(wrapped, &lock, nullptr, 0u) ==
                 core::D3D_OK &&
             lock.bits != nullptr;
        const bool copied = ok &&
            detail::cpuGeneralWriteRows(surface.surface, lock, blob->bytes);
        const bool unlocked = ok && dxmt9c_surface_unlock_rect(wrapped) ==
            core::D3D_OK;
        ok = copied && unlocked;
      } else {
        ok = false;
      }
    }
    if (!ok) return fail(event, "general-executor-mutation-failed");
    if (retainLogicalReceipts)
      detail::cpuGeneralRecordReceipt(receipts, fixed.identity,
                                       fixed.subresource, fixed.digest);
    ++result.observedMutations;
    return true;
  }


  static std::uint64_t elapsed(std::chrono::steady_clock::time_point begin) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<
        std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin)
                                            .count());
  }

  bool validateSourceOracle(Result& result) const {
    if (options_.sourceOracle.empty()) return true;
    if (options_.sourceOracle.size() > kCpuSourceOracleMaxRows) {
      result.failure = "general-executor-source-oracle-row-cap";
      return false;
    }
    std::size_t row = 0u;
    for (const auto& replayEvent : plan_.events) {
      if (replayEvent.type != d3d9::RenderTapeEventType::CommandChunk) continue;
      if (row >= options_.sourceOracle.size() ||
          options_.sourceOracle[row].event != replayEvent.eventIndex + 1u) {
        result.failure = "general-executor-source-oracle-command-events";
        result.failedEvent = replayEvent.eventIndex;
        return false;
      }
      ++row;
    }
    if (row != options_.sourceOracle.size()) {
      result.failure = "general-executor-source-oracle-extra-row";
      return false;
    }
    return true;
  }

  bool admit(Result& result) const {
    result.inputAdmission = d3d9::render_tape_runtime::admitGeneralCpuPlan(
        plan_, options_.inputCaps);
    if (!result.inputAdmission.accepted()) {
      result.failure = std::string("general-executor-input-") +
          d3d9::render_tape_runtime::generalCpuAdmissionReasonName(
              result.inputAdmission.reason);
      result.failedEvent = result.inputAdmission.eventIndex;
      return false;
    }
    return true;
  }

  bool admitSourceCapacity(Result& result) const {
    namespace runtime = d3d9::render_tape_runtime;
    auto& demand = result.sourceDemand;
    // Capture range replay already established its conservative publication
    // bound. Ordinary/V3 replay uses the admitted raws, never the event count
    // or equality with a captured source table to predict policy candidates.
    if (!options_.captureSourcePlan) {
      for (const auto& chunk : plan_.commandChunks) {
        const auto bound = cpuTapeConservativeMaxPendingPerRaw(
            chunk.envelope.recordCount, kCpuTapeMinDrawCommandLimit);
        if (bound == 0u || !runtime::addGeneralCpuSourcePublications(bound, demand)) {
          result.failure = "general-executor-source-demand-overflow";
          return false;
        }
      }
    }
    // END can first cut existing work and then publish its own marker. These
    // are real route triggers even when the enclosing raw is otherwise tiny.
    for (const auto& chunk : plan_.commandChunks) {
      d3d9::ImportedChunkView imported;
      if (!d3d9::importPrevalidatedCommandChunk(chunk.bytes, chunk.envelope, imported)) {
        result.failure = "general-executor-source-demand-import";
        return false;
      }
      for (std::size_t index = 0u; index < imported.records.size(); ++index) {
        const auto record = imported.record(index);
        if (record.header.type != D9C_COMMAND_RECORD_QUERY_ISSUE) continue;
        if (record.payload.size() < sizeof(D9CCommandChunkWireQueryIssue)) {
          result.failure = "general-executor-source-demand-query";
          return false;
        }
        D9CCommandChunkWireQueryIssue issue{};
        std::memcpy(&issue, record.payload.data(), sizeof(issue));
        if (issue.flags == 1u && !runtime::addGeneralCpuSourcePublications(2u, demand)) {
          result.failure = "general-executor-source-demand-overflow";
          return false;
        }
      }
    }
    const auto& counts = result.inputAdmission.counts;
    std::size_t checkpointPublications = 0u;
    if (!runtime::detail::generalCpuMultiply(counts.queryCheckpoints, 3u,
                                            checkpointPublications) ||
        !runtime::addGeneralCpuSourcePublications(checkpointPublications, demand) ||
        !runtime::addGeneralCpuSourcePublications(counts.queryPolls, demand) ||
        !runtime::addGeneralCpuQueryProgressDemand(counts,
            options_.maxQueryOraclePolls, demand)) {
      result.failure = "general-executor-query-budget-overflow";
      return false;
    }
    const auto admission = runtime::admitGeneralCpuSourceDemand(demand,
                                                              options_.sourceCapacity);
    if (admission != runtime::GeneralCpuSourceAdmission::Accepted) {
      result.failure = admission == runtime::GeneralCpuSourceAdmission::InvalidCapacity
          ? "general-executor-source-capacity-invalid"
          : admission == runtime::GeneralCpuSourceAdmission::PublicationLimit
              ? "general-executor-source-publication-limit"
              : "general-executor-source-progress-limit";
      return false;
    }
    return true;
  }

  bool admitCaptureSourcePlan(Result& result) const {
    const auto* sourcePlan = options_.captureSourcePlan;
    if (!sourcePlan) return true;
    if (!sourcePlan->valid() ||
        sourcePlan->commandChunkCount() != plan_.commandChunks.size() ||
        options_.route == d3d9::ProviderReplayPolicy::Historical ||
        options_.captureSourceTape.empty() ||
        options_.captureSourceIdentitySidecar.empty()) {
      result.failure = "general-executor-capture-source-plan";
      return false;
    }
    d3d9::CaptureSourceReplayPlan reboundPlan;
    if (d3d9::buildCaptureSourceReplayPlan(
            options_.captureSourceTape, plan_,
            options_.captureSourceIdentitySidecar, reboundPlan) !=
            d3d9::CaptureSourceReplayPlanStatus::Valid ||
        !detail::cpuGeneralSameDigest(reboundPlan.digest(),
                                      sourcePlan->digest())) {
      result.failure = "general-executor-capture-source-binding";
      return false;
    }
    if (!Access::cpuTapeWorkersDisabled(fixture_.routing->queue_) ||
        fixture_.cDevice->replayOffload ||
        fixture_.routing->supportsCpuReadyArenaReplay()) {
      result.failure = "general-executor-capture-source-runtime";
      return false;
    }
    const auto limits = fixture_.routing->queue_.cpuReadyArenaPlanLimits();
    const auto pageSize = limits.pageSize == 0u ? 4096u : limits.pageSize;
    std::vector<bool> seen(plan_.commandChunks.size(), false);
    std::size_t publicationReserve = 0u;
    for (const auto& replayEvent : plan_.events) {
      if (replayEvent.type != d3d9::RenderTapeEventType::CommandChunk)
        continue;
      if (replayEvent.index >= plan_.commandChunks.size() ||
          seen[replayEvent.index]) {
        result.failure = "general-executor-capture-source-event-map";
        result.failedEvent = replayEvent.eventIndex;
        return false;
      }
      if (replayEvent.eventIndex == std::numeric_limits<std::uint32_t>::max()) {
        result.failure = "general-executor-capture-source-event-ordinal";
        result.failedEvent = replayEvent.eventIndex;
        return false;
      }
      seen[replayEvent.index] = true;
      const auto& chunk = plan_.commandChunks[replayEvent.index];
      d3d9::ImportedChunkView imported;
      if (!d3d9::importPrevalidatedCommandChunk(chunk.bytes, chunk.envelope,
                                                 imported)) {
        result.failure = "general-executor-capture-source-import";
        result.failedEvent = replayEvent.eventIndex;
        return false;
      }
      const auto ranges = sourcePlan->rangesForCommandChunk(replayEvent.index);
      if (!d3d9::validateProviderReplayRecordRanges(
              chunk.envelope.recordCount, ranges)) {
        result.failure = "general-executor-capture-source-coverage";
        result.failedEvent = replayEvent.eventIndex;
        return false;
      }
      for (const auto& range : ranges) {
        const auto emission = d3d9::planReplayEmissionRange(
            imported, static_cast<d3d9::RawOrdinal>(replayEvent.eventIndex + 1u),
            range.firstRecord, range.recordCount, pageSize);
        if (!emission.partitioned()) {
          result.failure = "general-executor-capture-source-emission";
          result.failedEvent = replayEvent.eventIndex;
          return false;
        }
        // Compatibility replay can split at the runtime draw-command limit,
        // even when the emission partition itself has one span. Reuse the
        // established conservative CPU-lane proof: the admitted 64-record
        // maximum, minimum valid command limit (3), and one terminal source
        // (including Present) stay below the 31-slot queue bound.
        const auto publicationBound = cpuTapeConservativeMaxPendingPerRaw(
            range.recordCount, kCpuTapeMinDrawCommandLimit);
        if (!cpuTapeGeneralQueueBounded(
                range.recordCount, kCpuTapeMinDrawCommandLimit,
                dxmt9::kMaxQueuedChunks) ||
            publicationBound == 0u) {
          result.failure = "general-executor-capture-source-capacity";
          result.failedEvent = replayEvent.eventIndex;
          return false;
        }
        if (publicationBound > options_.sourceCapacity.maxPublications ||
            publicationReserve > options_.sourceCapacity.maxPublications - publicationBound) {
          result.failure = "general-executor-capture-source-capacity";
          result.failedEvent = replayEvent.eventIndex;
          return false;
        }
        publicationReserve += publicationBound;
      }
    }
    if (std::any_of(seen.begin(), seen.end(), [](bool value) { return !value; })) {
      result.failure = "general-executor-capture-source-missing-chunk";
      return false;
    }
    result.captureSourcePlanDigest = sourcePlan->digest();
    result.captureSourceRows.assign(sourcePlan->rows().begin(),
                                    sourcePlan->rows().end());
    result.captureSourceReplaySources.clear();
    result.captureSourceReplaySources.reserve(result.captureSourceRows.size());
    for (const auto& row : result.captureSourceRows) {
      result.captureSourceReplaySources.push_back({
          .eventOrdinal = row.eventOrdinal,
          .sourceOrdinal = row.sourceOrdinal,
          .capturedSeqId = row.capturedSeqId,
          .commandChunkIndex = row.commandChunkIndex,
          .firstRecord = row.firstRecord,
          .recordCount = row.recordCount,
      });
    }
    result.captureSourceReplayPublications.clear();
    result.captureSourceReplayPublications.reserve(publicationReserve);
    result.sourceDemand = {.publications = publicationReserve,
                           .progressSteps = publicationReserve};
    result.captureSourceReplayStorageOverflow = false;
    return true;
  }

  bool admitEmission(Result& result) const {
    if (options_.captureSourcePlan || options_.captureSourcePlanV3) return true;
    const auto pageSize = fixture_.routing->queue_.cpuReadyArenaPlanLimits().pageSize;
    if (pageSize == 0u) {
      result.failure = "general-executor-queue-page-size";
      return false;
    }
    const auto inspect = [&](const auto& chunk, std::uint32_t event) {
      dxmt9::d3d9::ImportedChunkView imported;
      if (!dxmt9::d3d9::importPrevalidatedCommandChunk(chunk.bytes,
                                                        chunk.envelope,
                                                        imported)) {
        result.failure = "general-executor-emission-import";
        result.failedEvent = event;
        return false;
      }
      const auto emission = dxmt9::d3d9::planReplayEmission(
          imported, static_cast<dxmt9::d3d9::RawOrdinal>(event + 1u), pageSize);
      if (!emission.partitioned()) {
        result.failure = "general-executor-emission-plan";
        result.failedEvent = event;
        return false;
      }
      // A whole-raw compatibility fallback cannot split internally under
      // this fixture's pinned zero-limit policy. If production will actually
      // enter the direct span driver, require one exact whole-raw lease.
      if (options_.route == d3d9::ProviderReplayPolicy::ProductionSlot &&
          emission.rotationFreeProductionEligible() &&
          (!emission.leaseEligible() || emission.leaseSpans.size() != 1u ||
           !emission.leaseSpans.front().ownsLease ||
           emission.leaseSpans.front().firstRecordIndex != 0u ||
           emission.leaseSpans.front().recordCount != imported.records.size() ||
           !emission.leaseSpans.front().finalLeaseSpan)) {
        result.failure = "general-executor-emission-source-shape";
        result.failedEvent = event;
        return false;
      }
      return true;
    };
    for (const auto& replayEvent : plan_.events) {
      if (replayEvent.type != dxmt9::d3d9::RenderTapeEventType::CommandChunk)
        continue;
      if (replayEvent.index >= plan_.commandChunks.size() ||
          !inspect(plan_.commandChunks[replayEvent.index], replayEvent.eventIndex))
        return false;
    }
    return true;
  }

  bool observePayload(const core::SourcePayloadView& payload,
                      std::uint64_t& hash, auto&& resource, auto&& draw,
                      std::string& failure) {
    return observeCpuTapePayload(payload, hash,
                                 std::forward<decltype(resource)>(resource),
                                 std::forward<decltype(draw)>(draw), failure);
  }

 public:
  // Producer-only qualification. The admitted value owns normalized receipts
  // and alias/depth metadata; it carries no wrapper or physical backing view.
  bool captureResourceObservation(
      const std::vector<d3d9::render_tape_runtime::OwnedObject>& objects,
      const std::vector<detail::LogicalReceipt>& receipts,
      std::span<const CpuGeneralTapeTombstone> tombstones, std::uint32_t kind,
      core::Handle handle, CpuTapeResourceObservation& observation,
      const D9CWireObjectIdentity* exactIdentity = nullptr) const {
    for (const auto& object : objects) {
      if (!object.value || object.identity.kind != kind ||
          (exactIdentity && !detail::cpuGeneralSameIdentity(object.identity, *exactIdentity))) continue;
      core::Handle actual{};
      D9CWireObjectIdentity contentIdentity = object.identity;
      std::optional<std::uint32_t> aliasLevel;
      if (kind == D9C_CHUNK_HANDLE_KIND_BUFFER) {
        const auto* value = static_cast<const D9CBuffer*>(object.value);
        if (value->obj) actual = value->obj->handle();
      } else if (kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
        const auto* value = static_cast<const D9CTexture*>(object.value);
        if (value->obj) actual = value->obj->handle();
      } else if (kind == D9C_CHUNK_HANDLE_KIND_SURFACE) {
        const auto* value = static_cast<const D9CSurface*>(object.value);
        if (value->obj) actual = value->obj->handle();
        if (actual == handle && value->ownerTex) {
          // A surface owns its parent wrapper independently of the Tape's
          // live-object registry. Parent ObjectDestroy (or later ABA reuse)
          // must not drop or reattribute the surviving alias's seed receipts.
          const auto definition = std::find_if(
              plan_.definitions.begin(), plan_.definitions.end(),
              [&](const auto& candidate) {
                return detail::cpuGeneralSameIdentity(candidate.fixed.identity,
                                                       object.identity);
              });
          d3d9::RenderTapeSurfaceDescriptorV2 alias{};
          if (definition == plan_.definitions.end() ||
              !d3d9::renderTapeLoadSurfaceDescriptorV2(
                  definition->descriptor, alias) ||
              alias.storage != static_cast<std::uint32_t>(
                  d3d9::RenderTapeSurfaceStorage::TextureSubresource) ||
              alias.subresource != value->ownerLevel)
            return false;
          contentIdentity = alias.parentTexture;
          aliasLevel = alias.subresource;
          if (!value->ownerTex->obj) return false;
          const auto actualParent = value->ownerTex->obj->handle();
          const bool liveParentMatches = std::any_of(
              objects.begin(), objects.end(), [&](const auto& parent) {
                if (!parent.value || !detail::cpuGeneralSameIdentity(
                        parent.identity, contentIdentity)) return false;
                const auto* texture = static_cast<const D9CTexture*>(parent.value);
                return texture->obj && texture->obj->handle() == actualParent;
              });
          const bool retiredParentMatches = std::any_of(
              tombstones.begin(), tombstones.end(), [&](const auto& parent) {
                return detail::cpuGeneralSameIdentity(
                           parent.identity, contentIdentity) &&
                       parent.nativeHandle == actualParent;
              });
          if (!liveParentMatches && !retiredParentMatches) return false;
          if (options_.dropRetiredAliasReceiptForTest && !liveParentMatches) {
            // Reproduce the old live-registry-only attribution, without
            // changing actual resource ownership or the reference digest.
            contentIdentity = object.identity;
            aliasLevel.reset();
          }
        }
      }
      if (actual != handle) continue;
      CpuTapeResourceObservationInput input{.identity = object.identity,
          .contentIdentity = contentIdentity, .aliasSubresource = aliasLevel,
          .receipts = receipts};
      if (kind == D9C_CHUNK_HANDLE_KIND_SURFACE) {
        const auto definition = std::find_if(
            plan_.definitions.begin(), plan_.definitions.end(),
            [&](const auto& candidate) {
              return detail::cpuGeneralSameIdentity(candidate.fixed.identity,
                                                     object.identity);
            });
        d3d9::RenderTapeSurfaceDescriptorV2 surface{};
        if (definition == plan_.definitions.end() ||
            !d3d9::renderTapeLoadSurfaceDescriptorV2(
                definition->descriptor, surface)) return false;
        if (surface.initialContentDisposition == static_cast<std::uint32_t>(
                d3d9::RenderTapeInitialContentDisposition::
                    CompleteDepthStencilFloat32UInt8V1)) {
          d3d9::RenderTapeD24S8SnapshotLayout layout{};
          if (!d3d9::renderTapeDeriveD24S8SnapshotLayout(surface.surface, layout))
            return false;
          input.depthStencil = CpuTapeDepthStencilObservation{
              .disposition = surface.initialContentDisposition,
              .format = surface.surface.format, .width = surface.surface.width,
              .height = surface.surface.height, .depthPitch = layout.depthPitch,
              .stencilPitch = layout.stencilPitch, .stencilOffset = layout.stencilOffset,
              .byteCount = layout.byteCount};
        }
      }
      // A resource produced by captured GPU commands has no CPU pixel bytes.
      // Its qualified identity, seed receipts (if any), and the complete
      // producer command stream form the logical oracle, not a fake readback.
      return buildCpuTapeResourceObservation(input, observation,
          {.maxInputReceipts = plan_.mutations.size()}) == CpuTapeResourceObservationStatus::Accepted;
    }
    return false;
  }

 private:
  bool observeResource(
      const std::vector<d3d9::render_tape_runtime::OwnedObject>& objects,
      const std::vector<detail::LogicalReceipt>& receipts,
      std::span<const CpuGeneralTapeTombstone> tombstones, std::uint32_t kind,
      core::Handle handle, std::uint64_t& hash) const {
    CpuTapeResourceObservation observation;
    return captureResourceObservation(objects, receipts, tombstones, kind, handle, observation) &&
        appendCpuTapeResourceObservation(observation, hash);
  }

  Fixture& fixture_;
  const Plan& plan_;
  std::span<const Blob> blobs_;
  Options options_{};
};

}  // namespace dxmt9::test
