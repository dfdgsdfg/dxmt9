#pragma once

#include "render_tape_encode_draw_witness.hpp"
#include "render_tape_encode_original_context.hpp"
#include "render_tape_cpu_source_rows.hpp"
#include "device_c_common.hpp"
#include "device_c_render_tape_provider.hpp"
#include "dxmt9/dxmt9_command_queue.hpp"
#include "dxmt9/dxmt9_device.hpp"
#include "dxmt9/dxmt9_draw_state.hpp"
#include "dxmt9/dxmt9_ffp_shaders.hpp"
#include "dxmt9/dxmt9_resource_pool.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace dxmt9::test {

namespace encode_witness = tests::encode_draw_witness;

enum class OriginalEncodeSourceScope : std::uint8_t {
  SetupOutsideBody, OriginalBody, TeardownOutsideBody, InvalidSource,
  MissingOriginalBodyProjection,
};

// The provider's real arm follows its existing completed setup checkpoint;
// its real close follows its existing cleanup flush/join. Sources outside
// those boundaries are counted, never relabelled as original body origins.
// Main protects this shared producer/Encode value with the short arm mutex.
class OriginalEncodeObservationLifecycle {
public:
  enum class Phase : std::uint8_t { AwaitingArm, Armed, Closed };
  Phase phase() const noexcept { return phase_; }
  bool arm() noexcept {
    if (phase_ != Phase::AwaitingArm) return false;
    phase_ = Phase::Armed;
    return true;
  }
  bool close() noexcept {
    if (phase_ == Phase::Closed) return false;
    phase_ = Phase::Closed;
    return true;
  }
  bool acceptsOriginalBodyRecord() const noexcept { return phase_ == Phase::Armed; }
  bool acceptsOriginalDraw() const noexcept { return phase_ == Phase::Armed; }
  OriginalEncodeSourceScope classifySource(bool validSource, bool authenticatedProjection) const noexcept {
    if (!validSource) return OriginalEncodeSourceScope::InvalidSource;
    switch (phase_) {
    case Phase::AwaitingArm: return OriginalEncodeSourceScope::SetupOutsideBody;
    case Phase::Closed: return OriginalEncodeSourceScope::TeardownOutsideBody;
    case Phase::Armed: return authenticatedProjection ? OriginalEncodeSourceScope::OriginalBody
        : OriginalEncodeSourceScope::MissingOriginalBodyProjection;
    }
    return OriginalEncodeSourceScope::InvalidSource;
  }
private:
  Phase phase_ = Phase::AwaitingArm;
};

// Slot 5 is observed from the real synchronous setVertexBytes callback. The
// copied POD is address metadata, never shader/GPU or byte-read authority.
class OriginalEncodeVolatileObservation {
public:
  void beginDraw() noexcept { value_.reset(); }
  void observeBufferBind(std::uint8_t slot) noexcept {
    if (slot == 5u) value_.reset();
  }
  void observe(const void* bytes, std::uint64_t size, std::uint8_t slot) noexcept {
    if (slot != 5u) return;
    value_.reset();
    if (!bytes || size != sizeof(state::DrawVolatile)) return;
    state::DrawVolatile copied;
    std::memcpy(&copied, bytes, sizeof(copied));
    value_ = copied;
  }
  const std::optional<state::DrawVolatile>& value() const noexcept { return value_; }
private:
  std::optional<state::DrawVolatile> value_{};
};

enum class OriginalEncodeAddressStatus : std::uint8_t {
  Qualified, MissingVolatile, UnsupportedDomain, FinalBindingMismatch,
  IssueShapeMismatch, VolatileMismatch, BaseVertexMismatch,
  SignedVertexDomainRejected,
};

inline const char* originalEncodeAddressStatusName(OriginalEncodeAddressStatus status) noexcept {
  switch (status) {
  case OriginalEncodeAddressStatus::Qualified: return "qualified";
  case OriginalEncodeAddressStatus::MissingVolatile: return "missing-current-draw-volatile";
  case OriginalEncodeAddressStatus::UnsupportedDomain: return "unsupported-domain";
  case OriginalEncodeAddressStatus::FinalBindingMismatch: return "final-native-binding-mismatch";
  case OriginalEncodeAddressStatus::IssueShapeMismatch: return "issued-shape-mismatch";
  case OriginalEncodeAddressStatus::VolatileMismatch: return "draw-volatile-address-mismatch";
  case OriginalEncodeAddressStatus::BaseVertexMismatch: return "base-vertex-adjustment-mismatch";
  case OriginalEncodeAddressStatus::SignedVertexDomainRejected: return "signed-vertex-domain-rejected";
  }
  return "unknown";
}

struct OriginalEncodeAddressInput {
  core::DrawParam parameters{};
  std::uint32_t streamMask = 0u, streamFrequency = 0u;
  bool declarationSingleStream = false;
  std::uint64_t sourceBase = 0u, sourceStride = 0u;
  std::uint64_t expectedVertexBacking = 0u, expectedIndexBacking = 0u;
  const CpuTapeSourceRowsPlan* plan = nullptr;
  std::span<const std::uint8_t> expectedIndexBytes{};
  const encode_witness::Issue* issue = nullptr;
  std::optional<state::DrawVolatile> drawVolatile{};
};

// Model/code binding: generated FFP and translated VS stream-0 fetches use
// volatile.offset + max(0, int(vertex_id) + volatile.base) * volatile.stride.
// Native indexed baseVertex affects vertex_id; the normal encoder instead
// carries the original base in slot 5. Qualify BOTH actual observations, not
// issue.baseVertex == original.baseVertex. No real bytes are read here.
inline OriginalEncodeAddressStatus qualifyOriginalEncodeAddressMapping(
    const OriginalEncodeAddressInput& input) noexcept {
  using Status = OriginalEncodeAddressStatus;
  const auto& draw = input.parameters;
  if (!input.plan || !input.issue || input.streamMask != 1u || input.streamFrequency != 1u ||
      !input.declarationSingleStream || draw.instanceCount != 1u ||
      draw.userVertexRange.size || draw.userIndexRange.size || !input.sourceStride ||
      input.sourceStride > UINT32_MAX)
    return Status::UnsupportedDomain;
  const auto& issue = *input.issue;
  const auto& plan = *input.plan;
  if (issue.boundStreamCount != 1u || issue.streams[0].slot != 1u ||
      !input.expectedVertexBacking || issue.streams[0].bindingBuffer != input.expectedVertexBacking)
    return Status::FinalBindingMismatch;
  if (!input.drawVolatile) return Status::MissingVolatile;
  const auto& vol = *input.drawVolatile;
  if (vol.vertexStreamStride != input.sourceStride || vol.vertexStreamOffset || vol._pad ||
      std::any_of(vol.streamInstanceDivisors.begin(), vol.streamInstanceDivisors.end(),
          [](std::uint32_t divisor) { return divisor != 0u; })) return Status::VolatileMismatch;
  std::uint32_t primitive = UINT32_MAX;
  switch (draw.primitiveType) {
  case core::PrimitiveType::PointList: primitive = WMTPrimitiveTypePoint; break;
  case core::PrimitiveType::LineList: primitive = WMTPrimitiveTypeLine; break;
  case core::PrimitiveType::LineStrip: primitive = WMTPrimitiveTypeLineStrip; break;
  case core::PrimitiveType::TriangleList: primitive = WMTPrimitiveTypeTriangle; break;
  case core::PrimitiveType::TriangleStrip: primitive = WMTPrimitiveTypeTriangleStrip; break;
  case core::PrimitiveType::TriangleFan: return Status::UnsupportedDomain;
  }
  if (issue.indexed != plan.effectiveIndexed || issue.primitiveType != primitive ||
      issue.count != plan.sourceElementCount || issue.instanceCount != 1u || issue.baseInstance)
    return Status::IssueShapeMismatch;
  if (!issue.indexed) {
    const auto displacement = encoders::encoderNonIndexedStartByteDisplacement(
        false, draw.startVertex, static_cast<std::uint32_t>(input.sourceStride));
    if (input.sourceBase > UINT64_MAX - displacement ||
        issue.streams[0].bindingOffset != input.sourceBase + displacement)
      return Status::FinalBindingMismatch;
    if (issue.vertexStart || issue.baseVertex) return Status::IssueShapeMismatch;
    return vol.vertexBaseIndex ? Status::BaseVertexMismatch : Status::Qualified;
  }
  if (!plan.indexRange || !input.expectedIndexBacking ||
      issue.indexSlice.buffer != input.expectedIndexBacking ||
      issue.indexSlice.offset != plan.indexRange->byteOffset ||
      issue.indexSlice.size != plan.indexRange->byteSize ||
      issue.indexElementBytes != encoders::encoderIndexElementSize(draw.indexType) ||
      issue.streams[0].bindingOffset != input.sourceBase)
    return Status::FinalBindingMismatch;
  const bool shaderBase = issue.baseVertex == 0 && vol.vertexBaseIndex == draw.baseVertexIndex;
  const bool nativeBase = draw.baseVertexIndex >= 0 &&
      issue.baseVertex == draw.baseVertexIndex && vol.vertexBaseIndex == 0;
  if (!shaderBase && !nativeBase) return Status::BaseVertexMismatch;
  for (std::uint64_t ordinal = 0u; ordinal < issue.count; ++ordinal) {
    const auto extent = encoders::qualifyEncoderIndexExtent(draw.indexType,
        static_cast<std::uint64_t>(draw.startIndex) + ordinal, 1u);
    if (!extent.qualified() || extent.byteEnd > input.expectedIndexBytes.size())
      return Status::SignedVertexDomainRejected;
    const auto index = encoders::decodeEncoderIndexValue(
        input.expectedIndexBytes.data() + static_cast<std::size_t>(extent.byteOffset), draw.indexType);
    const auto nativeVertex = static_cast<std::int64_t>(index) + issue.baseVertex;
    const auto shaderVertex = nativeVertex + vol.vertexBaseIndex;
    if (nativeVertex < 0 || nativeVertex > INT32_MAX || shaderVertex < 0 || shaderVertex > INT32_MAX ||
        shaderVertex != static_cast<std::int64_t>(index) + draw.baseVertexIndex)
      return Status::SignedVertexDomainRejected;
  }
  return Status::Qualified;
}

enum class OriginalEncodeDirectStatus : std::uint8_t {
  Qualified, UnsupportedDomain, SourceWindowRejected, MissingBufferVersion,
  MissingHistoricalFacts, SnapshotAssociationMismatch, PlannerRejected,
  AddressMappingRejected, InspectionBudgetExceeded, PhysicalByteProofRejected,
};

inline const char* originalEncodeDirectStatusName(OriginalEncodeDirectStatus status) noexcept {
  switch (status) {
  case OriginalEncodeDirectStatus::Qualified: return "qualified";
  case OriginalEncodeDirectStatus::UnsupportedDomain: return "unsupported-domain";
  case OriginalEncodeDirectStatus::SourceWindowRejected: return "source-window-rejected";
  case OriginalEncodeDirectStatus::MissingBufferVersion: return "missing-original-buffer-version";
  case OriginalEncodeDirectStatus::MissingHistoricalFacts: return "missing-historical-buffer-facts";
  case OriginalEncodeDirectStatus::SnapshotAssociationMismatch: return "selected-snapshot-association-mismatch";
  case OriginalEncodeDirectStatus::PlannerRejected: return "source-row-planner-rejected";
  case OriginalEncodeDirectStatus::AddressMappingRejected: return "address-mapping-rejected";
  case OriginalEncodeDirectStatus::InspectionBudgetExceeded: return "inspection-budget-exceeded";
  case OriginalEncodeDirectStatus::PhysicalByteProofRejected: return "physical-byte-proof-rejected";
  }
  return "unknown";
}

struct OriginalEncodeDirectInput {
  OriginalEncodeAddressInput address{};
  const CpuTapeResourceVersion* vertex = nullptr;
  const CpuTapeResourceVersion* index = nullptr;
  std::optional<core::DrawBufferBindingSnapshot> vertexSnapshot{}, indexSnapshot{};
  std::uint64_t sourceSequence = 0u, expectedQueue = 0u;
  query::Progress progress{};
};

struct OriginalEncodeDirectObservation {
  OriginalEncodeDirectStatus status = OriginalEncodeDirectStatus::UnsupportedDomain;
  std::optional<CpuTapeSourceRowsStatus> plannerStatus{};
  std::optional<OriginalEncodeAddressStatus> addressStatus{};
  std::optional<resources::BufferReadProofBatchResult> readProof{};
  std::uint64_t plannedReadBytes = 0u, attemptedInspectionBytes = 0u;
  std::size_t uniqueVertexRows = 0u, vertexRanges = 0u;
  bool qualified() const noexcept { return status == OriginalEncodeDirectStatus::Qualified; }
};

inline bool originalEncodeHistoricalSnapshotMatches(const CpuTapeResourceVersion& version,
    const std::optional<core::DrawBufferBindingSnapshot>& snapshot) noexcept {
  if (!version.localBufferFacts()) return false;
  const auto& facts = *version.localBufferFacts();
  if (!facts.observed() || facts.buffer != version.nativeHandle() || !facts.contentRevision ||
      facts.byteSize != version.expectedBufferBytes().size() ||
      !facts.currentBacking.metalHandle || facts.currentBacking.byteSize != facts.byteSize ||
      facts.hasVersionedBacking != snapshot.has_value()) return false;
  return !snapshot || (snapshot->valid() && snapshot->byteSize == facts.byteSize &&
      snapshot->metalHandle == facts.currentBacking.metalHandle &&
      snapshot->contentRevision == facts.contentRevision);
}

// One arena-locked coherent instantaneous IB/VB comparison, AFTER exact
// source-snapshot -> final native binding -> slot-5 address closure. Requests
// use the ACTUAL selected snapshot copies, never a synthesized DrawBindingSnapshot
// or a current Pool backing substituted for an old captured backing. The caller
// supplies genuine live Encode source lifetime. Mapped/GPU writes remain excluded.
inline OriginalEncodeDirectObservation observeOriginalEncodeDirectBinding(
    const resources::Pool& pool, const OriginalEncodeDirectInput& input,
    std::uint64_t remainingInspectionBytes,
    CpuTapeSourceRowsLimits limits = {32768u, 32769u, 64u * 1024u * 1024u}) noexcept {
  using Status = OriginalEncodeDirectStatus;
  OriginalEncodeDirectObservation out;
  const auto reject = [&](Status status) { out.status = status; return out; };
  const auto& address = input.address;
  if (!address.issue || address.streamMask != 1u || address.streamFrequency != 1u ||
      !address.declarationSingleStream || address.parameters.instanceCount != 1u ||
      address.parameters.userVertexRange.size || address.parameters.userIndexRange.size ||
      (!address.issue->indexed && (input.index || input.indexSnapshot)) ||
      limits.maxRows > 32768u || limits.maxRanges > 32769u ||
      limits.maxReadBytes > 64u * 1024u * 1024u ||
      remainingInspectionBytes > 1024u * 1024u * 1024u)
    return reject(Status::UnsupportedDomain);
  if (!input.expectedQueue || input.progress.queue != input.expectedQueue || input.progress.failed ||
      !input.sourceSequence || input.progress.completed >= input.sourceSequence)
    return reject(Status::SourceWindowRejected);
  if (!input.vertex || (address.issue->indexed && !input.index)) return reject(Status::MissingBufferVersion);
  if (!input.vertex->localBufferFacts() || (input.index && !input.index->localBufferFacts()))
    return reject(Status::MissingHistoricalFacts);
  if (!originalEncodeHistoricalSnapshotMatches(*input.vertex, input.vertexSnapshot) ||
      (input.index && !originalEncodeHistoricalSnapshotMatches(*input.index, input.indexSnapshot)))
    return reject(Status::SnapshotAssociationMismatch);
  CpuTapeSourceRowsPlan plan;
  out.plannerStatus = buildCpuTapeSourceRowsPlan({.parameters = address.parameters,
      .actualEffectiveIndexed = address.issue->indexed, .hasBoundIndexBuffer = input.index != nullptr,
      .streamMask = address.streamMask, .streamFrequency = address.streamFrequency,
      .sourceBase = address.sourceBase, .sourceStride = address.sourceStride,
      .sourceByteSize = input.vertex->expectedBufferBytes().size(),
      .expectedIndexBytes = input.index ? input.index->expectedBufferBytes() : std::span<const std::uint8_t>{}},
      plan, limits);
  if (*out.plannerStatus != CpuTapeSourceRowsStatus::Qualified) return reject(Status::PlannerRejected);
  out.plannedReadBytes = plan.plannedReadBytes;
  out.uniqueVertexRows = plan.uniqueVertexRows;
  out.vertexRanges = plan.vertexRanges.size();
  auto actualAddress = address;
  actualAddress.plan = &plan;
  actualAddress.expectedIndexBytes = input.index ? input.index->expectedBufferBytes() : std::span<const std::uint8_t>{};
  actualAddress.expectedVertexBacking = input.vertexSnapshot ? input.vertexSnapshot->metalHandle
      : input.vertex->localBufferFacts()->currentBacking.metalHandle;
  actualAddress.expectedIndexBacking = !input.index ? 0u : input.indexSnapshot ? input.indexSnapshot->metalHandle
      : input.index->localBufferFacts()->currentBacking.metalHandle;
  out.addressStatus = qualifyOriginalEncodeAddressMapping(actualAddress);
  if (*out.addressStatus != OriginalEncodeAddressStatus::Qualified) return reject(Status::AddressMappingRejected);
  // Charge input + concrete bytes before entering the proof, even if it fails.
  // Repeated failed comparisons cannot evade the caller's aggregate cap.
  if (plan.plannedReadBytes > remainingInspectionBytes / 2u)
    return reject(Status::InspectionBudgetExceeded);
  out.attemptedInspectionBytes = plan.plannedReadBytes * 2u;
  std::array<resources::BufferReadProofRequest, 2u> requests{};
  std::size_t count = 0u;
  const resources::BufferReadProofLimits memberLimits{limits.maxRanges, limits.maxReadBytes};
  if (input.index) requests[count++] = {.buffer = input.index->nativeHandle(),
      .sourceSequence = input.sourceSequence, .completedSequence = input.progress.completed,
      .snapshot = input.indexSnapshot, .expectedBytes = input.index->expectedBufferBytes(),
      .ranges = std::span<const resources::BufferReadProofRange>(&*plan.indexRange, 1u), .limits = memberLimits};
  requests[count++] = {.buffer = input.vertex->nativeHandle(), .sourceSequence = input.sourceSequence,
      .completedSequence = input.progress.completed, .snapshot = input.vertexSnapshot,
      .expectedBytes = input.vertex->expectedBufferBytes(), .ranges = plan.vertexRanges, .limits = memberLimits};
  out.readProof = pool.proveBufferReadFootprints(std::span(requests.data(), count),
      {2u, limits.maxRanges, limits.maxReadBytes});
  return reject(out.readProof->proved() ? Status::Qualified : Status::PhysicalByteProofRejected);
}

struct OriginalEncodeIssuedReceipt {
  encode_witness::DrawKey draw{};
  render::TapeEmissionRecordLocator original{};
  std::uint32_t sourceIndex = UINT32_MAX;
  std::uint64_t sourceOrdinal = 0u;
  D9CWireObjectIdentity vertexIdentity{}, indexIdentity{};
  std::uint64_t vertexEpoch = 0u, indexEpoch = 0u;
  std::uint64_t sourceBase = 0u, sourceStride = 0u;
  std::uint32_t vertexObservationId = 0u, indexObservationId = 0u;
  std::optional<state::DrawVolatile> drawVolatile{};
  OriginalEncodeDirectObservation observation{};
};

// One private diagnostic owner. Provider callbacks are serialized on the
// ordinary producer, source/draw callbacks on the admitted serial Encode lane.
// Their ONLY shared mutable interfaces are the write-once context store and a
// short arm mutex. No callback retains wrappers/payload spans or changes routes,
// drains, ACK/publication/completion, resource contents or renderer policy.
class RenderTapeEncodeOriginalAdapter {
public:
  static constexpr std::size_t kExpectedByteBudget = 512u * 1024u * 1024u;
  static constexpr std::uint64_t kInspectionByteBudget = 1024u * 1024u * 1024u;
  static constexpr std::size_t kMaxIssues = 65536u;
  using Status = CpuTapeResourceContextStatus;

  RenderTapeEncodeOriginalAdapter(std::vector<std::vector<std::byte>> storage,
      std::vector<d3d9::RenderTapeProviderBlob> blobs) noexcept
      : storage_(std::move(storage)), budget_(makeBudget()), contexts_(budget_) {
    try {
      if (!budget_ || storage_.size() != blobs.size()) {
        rejectContext("original-buffer-input-owner-invalid"); return;
      }
      digests_.reserve(blobs.size());
      for (std::size_t index = 0u; index < blobs.size(); ++index) {
        if (blobs[index].bytes.data() != storage_[index].data() ||
            blobs[index].bytes.size() != storage_[index].size()) {
          rejectContext("original-buffer-input-owner-association"); return;
        }
        digests_.push_back(blobs[index].digest);
      }
      facts_.reserve(1024u);
      receipts_.reserve(kMaxIssues);
      observer_.context = this;
      observer_.buffer = [](void* raw, D9CDevice* device,
          const d3d9::RenderTapePreparedBufferEventV4& event) noexcept {
        return static_cast<RenderTapeEncodeOriginalAdapter*>(raw)->buffer(device, event);
      };
      observer_.beforeRecord = [](void* raw, D9CDevice* device, std::uint64_t event,
          std::uint32_t record, std::uint32_t,
          std::span<const d3d9::RenderTapePreparedHandleAssociationV4> associations) noexcept {
        return static_cast<RenderTapeEncodeOriginalAdapter*>(raw)->beforeRecord(
            device, {event, record}, associations);
      };
      observer_.armed = [](void* raw, D9CDevice* device, std::int32_t result,
          const D9CRenderTapeIdentityCaptureV4ArmResult& arm,
          const std::shared_ptr<d3d9::RenderTapeRuntimeIdentityV4>& root) noexcept {
        return static_cast<RenderTapeEncodeOriginalAdapter*>(raw)->armed(device, result, arm, root);
      };
      observer_.closed = [](void* raw, D9CDevice* device) noexcept {
        return static_cast<RenderTapeEncodeOriginalAdapter*>(raw)->closed(device);
      };
    } catch (...) { rejectContext("original-buffer-owner-allocation"); }
  }

  bool bindActualQueue(CommandQueue& queue) noexcept {
    if (queue_) return rejectContext("original-buffer-queue-already-bound");
    queue_ = &queue;
    return !failed();
  }
  d3d9::RenderTapePreparedObserverV4& observer() noexcept { return observer_; }
  bool failed() const noexcept { return contextFailure_.load() || sourceFailure_.load(); }
  const char* contextFailure() const noexcept {
    const auto reason = contextFailure_.load(); return reason ? reason : "none";
  }
  const char* sourceFailure() const noexcept {
    const auto reason = sourceFailure_.load(); return reason ? reason : "none";
  }
  Status contextStatus() const noexcept { return contextStatus_; }
  bool contextComplete() const noexcept { return closed_ && !contextFailure_.load() && contexts_.recordCount(); }
  bool observationComplete() const noexcept {
    return closed_ && !failed() && sourceFragments_ && !receipts_.empty();
  }
  std::size_t recordCount() const noexcept { return contexts_.recordCount(); }
  CpuTapeExpectedBufferBudgetSnapshot expectedBudget() const noexcept {
    return budget_ ? budget_->snapshot() : CpuTapeExpectedBufferBudgetSnapshot{};
  }
  std::size_t sourceFragments() const noexcept { return sourceFragments_; }
  std::size_t setupSourceFragments() const noexcept { return setupSourceFragments_; }
  std::size_t teardownSourceFragments() const noexcept { return teardownSourceFragments_; }
  std::size_t setupDrawAttempts() const noexcept { return setupDrawAttempts_; }
  std::size_t teardownDrawAttempts() const noexcept { return teardownDrawAttempts_; }
  std::size_t outsideBodyRecordCallbacks() const noexcept { return outsideBodyRecordCallbacks_; }
  std::uint64_t attemptedInspectionBytes() const noexcept { return attemptedInspectionBytes_; }
  std::span<const OriginalEncodeIssuedReceipt> receipts() const noexcept { return receipts_; }

  void observeSource(core::CpuReadyTape::SourceRef source, std::uint64_t sequence) noexcept {
    if (failed()) return;
    try {
      std::shared_ptr<d3d9::RenderTapeRuntimeIdentityV4> root;
      render::TapeTopologyCaptureIdentity capture;
      OriginalEncodeObservationLifecycle lifecycle;
      {
        std::lock_guard lock(armMutex_);
        root = root_; capture = capture_;
        lifecycle = lifecycle_;
      }
      const bool validSource = source.valid() && sequence;
      const auto scope = lifecycle.classifySource(validSource, false);
      if (scope == OriginalEncodeSourceScope::SetupOutsideBody ||
          scope == OriginalEncodeSourceScope::TeardownOutsideBody) {
        auto& count = scope == OriginalEncodeSourceScope::SetupOutsideBody
            ? setupSourceFragments_ : teardownSourceFragments_;
        if (count >= 65536u) { rejectSource("original-encode-outside-body-source-cap"); return; }
        ++count;
        sourceScope_ = scope;
        projection_ = {};
        return;
      }
      // No queue/arm/root/record-store lock is held while the genuine root pull
      // copies the already-published original map. Never wait for raw ACK.
      d3d9::RenderTapeRuntimePublishedSourceOriginsV4 candidate;
      if (!validSource || !queue_ || !root || !capture.valid() ||
          !root->copyPublishedSourceOrigins(capture, source, sequence, candidate) ||
          candidate.capture != capture ||
          candidate.source.source != source || candidate.source.sequence != sequence ||
          candidate.commands.size() != candidate.source.commandCount ||
          candidate.commands.size() > 65536u || candidate.origins.size() > 65536u) {
        rejectSource("original-encode-source-origin-projection"); return;
      }
      if (sourceFragments_ >= 65536u) { rejectSource("original-encode-source-fragment-cap"); return; }
      projection_ = std::move(candidate);
      sourceScope_ = lifecycle.classifySource(true, true);
      if (sourceScope_ != OriginalEncodeSourceScope::OriginalBody) {
        rejectSource("original-encode-body-source-lifecycle"); return;
      }
      ++sourceFragments_;
    } catch (...) { rejectSource("original-encode-source-origin-allocation"); }
  }

  void beginDraw(encode_witness::DrawKey draw) noexcept {
    current_ = {};
    current_.draw = draw;
    volatile_.beginDraw();
    if (failed()) return;
    try {
      OriginalEncodeObservationLifecycle lifecycle;
      { std::lock_guard lock(armMutex_); lifecycle = lifecycle_; }
      if (!lifecycle.acceptsOriginalDraw()) {
        if (lifecycle.phase() == OriginalEncodeObservationLifecycle::Phase::AwaitingArm) ++setupDrawAttempts_;
        else ++teardownDrawAttempts_;
        rejectSource("original-encode-draw-outside-armed-body");
      }
    } catch (...) { rejectSource("original-encode-draw-lifecycle-lock"); }
  }
  void observeVertexBuffer(std::uint8_t slot) noexcept { volatile_.observeBufferBind(slot); }
  void observeVertexBytes(const void* bytes, std::uint64_t size, std::uint8_t slot) noexcept {
    volatile_.observe(bytes, size, slot);
  }

  void observeDrawContext(const core::FlatDrawStateRecord& hot,
      const core::DrawShaderLayoutContext& layout, const core::DrawParam& parameters,
      const core::DrawBufferBindingSnapshot* vertex, const core::DrawBufferBindingSnapshot* index) noexcept {
    if (failed()) return;
    try {
      if (sourceScope_ != OriginalEncodeSourceScope::OriginalBody ||
          current_.draw.sequence != projection_.source.sequence ||
          current_.draw.command >= projection_.commands.size()) {
        rejectSource("original-encode-draw-source-location"); return;
      }
      constexpr auto drawKind = static_cast<std::uint32_t>(d3d9::RenderTapeIdentityV3CommandKind::DrawRun);
      const auto& command = projection_.commands[current_.draw.command];
      if (command.sourceIndex != projection_.source.sourceIndex ||
          command.physicalCommandIndex != current_.draw.command || command.kind != drawKind ||
          current_.draw.subdraw >= command.drawCount) {
        rejectSource("original-encode-draw-command-shape"); return;
      }
      const d3d9::RenderTapeRuntimePublishedSourceOriginV4* selected = nullptr;
      for (const auto& origin : projection_.origins) {
        if (origin.physicalCommandIndex != current_.draw.command ||
            origin.firstSubdraw != current_.draw.subdraw || origin.subdrawCount != 1u) continue;
        if (selected) { rejectSource("original-encode-ambiguous-draw-origin"); return; }
        selected = &origin;
      }
      if (!selected || selected->kind != drawKind || !selected->record.eventOrdinal ||
          selected->record.recordIndex == UINT32_MAX) {
        rejectSource("original-encode-missing-draw-origin"); return;
      }
      current_.original = selected->record;
      current_.context = contexts_.lookupRecord(selected->record);
      if (!current_.context || current_.context->purpose() != CpuTapeResourceContextPurpose::V4SurfaceAliasInventory ||
          current_.context->hasSemanticUniqueNativeAuthority()) {
        rejectSource("original-encode-missing-immutable-record-context"); return;
      }
      current_.parameters = parameters;
      current_.vertex = hot.streamBuffers[0];
      current_.index = encoders::encoderEffectiveIndexed(parameters.indexed, bool(hot.indexBuffer),
          parameters.userIndexRange.size != 0u) ? hot.indexBuffer : core::Handle{};
      current_.streamMask = hot.streamMask;
      current_.streamFrequency = hot.streamFrequencies[0];
      current_.sourceBase = hot.streamOffsets[0];
      current_.declarationSingleStream = layout.vertexDecl.streams[0].offset == hot.streamOffsets[0] &&
          layout.vertexDecl.streams[0].stride == hot.streamStrides[0] &&
          (layout.vertexShader.kind == core::ShaderRef::Kind::FixedFunctionVertex ||
           layout.vertexShader.kind == core::ShaderRef::Kind::Bytecode);
      for (const auto& element : layout.vertexDecl.elements)
        if (element.stream != 0u) current_.declarationSingleStream = false;
      const auto ff = ffp::decodeFixedFunctionVertexLayout(layout.vertexDecl);
      const bool fixed = layout.vertexShader.kind == core::ShaderRef::Kind::FixedFunctionVertex;
      if (fixed && !ff) current_.declarationSingleStream = false;
      current_.sourceStride = fixed
          ? encoders::encoderStream0SourceStride(layout.vertexDecl.streams[0].stride, true, ff ? ff->stride : 0u, 0u)
          : encoders::encoderStream0SourceStride(ff ? layout.vertexDecl.streams[0].stride : 0u, ff.has_value(),
              ff ? ff->stride : 0u, ff ? 0u : ffp::computeVertexDeclStride(layout.vertexDecl));
      if (vertex) current_.vertexSnapshot = *vertex;
      if (index && current_.index) current_.indexSnapshot = *index;
      current_.observed = true;
    } catch (...) { rejectSource("original-encode-draw-context-inspection"); }
  }

  void observeIssue(const encode_witness::Issue& issue) noexcept {
    if (failed()) return;
    if (!queue_ || !current_.observed || issue.draw != current_.draw) {
      rejectSource("original-encode-issue-without-source-context"); return;
    }
    if (receipts_.size() >= kMaxIssues) { rejectSource("original-encode-issue-cap"); return; }
    try {
      OriginalEncodeIssuedReceipt receipt;
      receipt.draw = issue.draw;
      receipt.original = current_.original;
      receipt.sourceIndex = projection_.source.sourceIndex;
      receipt.sourceOrdinal = projection_.source.sourceOrdinal;
      receipt.sourceBase = current_.sourceBase;
      receipt.sourceStride = current_.sourceStride;
      receipt.drawVolatile = volatile_.value();
      if (issue.boundStreamCount) receipt.vertexObservationId = issue.streams[0].bufferObservationId;
      receipt.indexObservationId = issue.indexBufferObservationId;
      const auto vertex = current_.context->find(D9C_CHUNK_HANDLE_KIND_BUFFER, current_.vertex);
      const auto index = issue.indexed ? current_.context->find(D9C_CHUNK_HANDLE_KIND_BUFFER, current_.index) : nullptr;
      if (vertex) { receipt.vertexIdentity = vertex->identity(); receipt.vertexEpoch = vertex->contentEpoch(); }
      if (index) { receipt.indexIdentity = index->identity(); receipt.indexEpoch = index->contentEpoch(); }
      std::uint64_t expectedQueue = 0u;
      { std::lock_guard lock(armMutex_); expectedQueue = capture_.queue; }
      OriginalEncodeDirectInput input{
          .address = {.parameters = current_.parameters, .streamMask = current_.streamMask,
              .streamFrequency = current_.streamFrequency, .declarationSingleStream = current_.declarationSingleStream,
              .sourceBase = current_.sourceBase, .sourceStride = current_.sourceStride,
              .issue = &issue, .drawVolatile = receipt.drawVolatile},
          .vertex = vertex.get(), .index = index.get(), .vertexSnapshot = current_.vertexSnapshot,
          .indexSnapshot = issue.indexed ? current_.indexSnapshot : std::nullopt,
          .sourceSequence = issue.draw.sequence, .expectedQueue = expectedQueue,
          .progress = queue_->queryProgress()};
      // queryProgress and all source/record/arm locks are released BEFORE the
      // arena read. Immutable versions remain owned by this exact record context.
      receipt.observation = observeOriginalEncodeDirectBinding(queue_->pool(), input,
          kInspectionByteBudget - attemptedInspectionBytes_);
      attemptedInspectionBytes_ += receipt.observation.attemptedInspectionBytes;
      receipts_.push_back(std::move(receipt));
    } catch (...) { rejectSource("original-encode-issue-inspection"); }
  }

private:
  struct Current {
    encode_witness::DrawKey draw{};
    render::TapeEmissionRecordLocator original{};
    std::shared_ptr<const CpuTapeResourceContext> context{};
    core::DrawParam parameters{};
    core::Handle vertex{}, index{};
    std::uint32_t streamMask = 0u, streamFrequency = 0u;
    std::uint64_t sourceBase = 0u, sourceStride = 0u;
    bool declarationSingleStream = false, observed = false;
    std::optional<core::DrawBufferBindingSnapshot> vertexSnapshot{}, indexSnapshot{};
  };
  static std::shared_ptr<CpuTapeExpectedBufferBudget> makeBudget() noexcept {
    std::shared_ptr<CpuTapeExpectedBufferBudget> budget;
    return createCpuTapeExpectedBufferBudget({kExpectedByteBudget}, budget) == Status::Accepted ? budget : nullptr;
  }
  bool rejectContext(const char* reason) noexcept {
    const char* expected = nullptr; contextFailure_.compare_exchange_strong(expected, reason); return false;
  }
  bool rejectSource(const char* reason) noexcept {
    const char* expected = nullptr; sourceFailure_.compare_exchange_strong(expected, reason); return false;
  }
  bool acceptStatus(Status status) noexcept {
    contextStatus_ = status;
    return status == Status::Accepted || rejectContext("original-buffer-context-rejected");
  }
  bool ownsDevice(D9CDevice* device) const noexcept {
    if (!device || !queue_) return false;
    const auto upper = device->dev().upperDevice();
    return upper && &upper->queue() == queue_;
  }
  static bool sameWireIdentity(D9CWireObjectIdentity a, D9CWireObjectIdentity b) noexcept {
    return a.kind == b.kind && a.generation == b.generation && a.objectId == b.objectId;
  }
  std::span<const std::byte> blob(d3d9::RenderTapeDigest digest) const noexcept {
    for (std::size_t index = 0u; index < digests_.size(); ++index)
      if (digests_[index] == digest) return storage_[index];
    return {};
  }
  bool buffer(D9CDevice* device, const d3d9::RenderTapePreparedBufferEventV4& event) noexcept {
    if (failed() || !ownsDevice(device) || !event.buffer || !event.buffer->obj)
      return rejectContext("original-buffer-runtime-association");
    const auto native = event.buffer->obj->handle();
    using Phase = d3d9::RenderTapePreparedBufferPhaseV4;
    switch (event.phase) {
    case Phase::HiddenDefined:
    case Phase::Defined: {
      D9CBufferDesc descriptor;
      if (!event.definition || event.descriptor.size() != sizeof(descriptor))
        return rejectContext("original-buffer-definition-shape");
      std::memcpy(&descriptor, event.descriptor.data(), sizeof(descriptor));
      const auto actual = event.buffer->desc;
      if (descriptor.size != actual.size || descriptor.usage != actual.usage || descriptor.pool != actual.pool ||
          descriptor.fvf != actual.fvf || descriptor.format != actual.format)
        return rejectContext("original-buffer-runtime-descriptor-mismatch");
      return acceptStatus(contexts_.define(*event.definition, event.descriptor, native,
          event.phase == Phase::HiddenDefined));
    }
    case Phase::Activated:
      return event.definition ? acceptStatus(contexts_.activate(event.definition->identity, native))
          : rejectContext("original-buffer-activation-shape");
    case Phase::HiddenSeedApplied:
      return event.mutation ? acceptStatus(contexts_.hiddenSeedApplied(event.eventOrdinal,
          *event.mutation, blob(event.mutation->digest))) : rejectContext("original-buffer-mutation-shape");
    case Phase::SeedConsumed:
      return event.mutation ? acceptStatus(contexts_.seedConsumed(event.eventOrdinal, *event.mutation))
          : rejectContext("original-buffer-mutation-shape");
    case Phase::MutationApplied:
      return event.mutation ? acceptStatus(contexts_.mutationApplied(event.eventOrdinal,
          *event.mutation, blob(event.mutation->digest))) : rejectContext("original-buffer-mutation-shape");
    case Phase::Retiring: {
      const auto identity = event.definition ? event.definition->identity : event.buffer->wireIdentity;
      return acceptStatus(contexts_.retire(identity, native));
    }
    }
    return rejectContext("original-buffer-phase-invalid");
  }
  bool beforeRecord(D9CDevice* device, render::TapeEmissionRecordLocator locator,
      std::span<const d3d9::RenderTapePreparedHandleAssociationV4> associations) noexcept {
    if (failed() || !ownsDevice(device) || associations.size() > 65536u)
      return rejectContext("original-buffer-record-association");
    try {
      std::lock_guard lock(armMutex_);
      if (!lifecycle_.acceptsOriginalBodyRecord()) {
        ++outsideBodyRecordCallbacks_;
        return rejectContext("original-buffer-record-outside-armed-body");
      }
    } catch (...) { return rejectContext("original-buffer-record-lifecycle-lock"); }
    facts_.clear();
    for (const auto& association : associations) {
      if (association.identity.kind != D9C_CHUNK_HANDLE_KIND_BUFFER) continue;
      const auto* wrapper = static_cast<const D9CBuffer*>(association.resolvedWrapper);
      const auto entry = contexts_.find(association.identity);
      if (!wrapper || !wrapper->obj || entry.nativeHandle != wrapper->obj->handle() ||
          entry.byteSize != wrapper->desc.size || entry.hidden || entry.pendingSeed)
        return rejectContext("original-buffer-record-runtime-mismatch");
      bool alreadyObserved = false;
      for (const auto& prior : facts_) {
        if (prior.nativeHandle != entry.nativeHandle) continue;
        if (!sameWireIdentity(prior.identity, association.identity))
          return rejectContext("original-buffer-record-canonical-alias");
        alreadyObserved = true;
        break;
      }
      if (alreadyObserved) continue;
      // Only the provider's existing post-normal-drain beforeRecord boundary
      // finalizes optional historical backing facts. Never read wrapper bytes.
      const auto observation = queue_->pool().observeBufferVersion(entry.nativeHandle);
      if (!observation.observed()) continue;
      if (facts_.size() >= 1024u) return rejectContext("original-buffer-facts-cap");
      try { facts_.push_back({association.identity, entry.nativeHandle, observation}); }
      catch (...) { return rejectContext("original-buffer-facts-allocation"); }
    }
    RenderTapeEncodeOriginalBufferContexts::Context context;
    return acceptStatus(contexts_.beforeRecord(locator, facts_, context));
  }
  bool armed(D9CDevice* device, std::int32_t result,
      const D9CRenderTapeIdentityCaptureV4ArmResult& arm,
      const std::shared_ptr<d3d9::RenderTapeRuntimeIdentityV4>& root) noexcept {
    if (failed() || !ownsDevice(device) || result != core::D3D_OK ||
        arm.status != D9C_RENDER_TAPE_IDENTITY_CAPTURE_COMPLETE || !root ||
        !arm.captureToken || !arm.captureEpoch || arm.queue != queue_->queryProgress().queue)
      return rejectSource("original-encode-arm-binding");
    try {
      std::lock_guard lock(armMutex_);
      if (root_ || !lifecycle_.arm()) return rejectSource("original-encode-arm-repeated");
      root_ = root;
      capture_ = {arm.captureToken, arm.captureEpoch, arm.queue};
      return true;
    } catch (...) { return rejectSource("original-encode-arm-lock"); }
  }
  bool closed(D9CDevice* device) noexcept {
    try {
      std::lock_guard lock(armMutex_);
      if (!lifecycle_.close()) return rejectContext("original-buffer-close-repeated");
      closed_ = true;
    } catch (...) { return rejectContext("original-buffer-close-lock"); }
    return ownsDevice(device) && !failed();
  }

  // Input blobs are owned and never exposed mutably. Logical expected bytes
  // derive only from these authenticated admitted blobs, not wrappers or Pool.
  std::vector<std::vector<std::byte>> storage_{};
  std::vector<d3d9::RenderTapeDigest> digests_{};
  std::shared_ptr<CpuTapeExpectedBufferBudget> budget_{};
  RenderTapeEncodeOriginalBufferContexts contexts_;
  d3d9::RenderTapePreparedObserverV4 observer_{};
  CommandQueue* queue_ = nullptr; // Fresh real queue, valid through device join.
  std::mutex armMutex_{};
  OriginalEncodeObservationLifecycle lifecycle_{};
  std::shared_ptr<d3d9::RenderTapeRuntimeIdentityV4> root_{};
  render::TapeTopologyCaptureIdentity capture_{};
  std::atomic<const char*> contextFailure_{nullptr}, sourceFailure_{nullptr};
  Status contextStatus_ = Status::Accepted;
  bool closed_ = false;
  std::vector<RenderTapeEncodeOriginalBufferFacts> facts_{}; // Producer only.
  d3d9::RenderTapeRuntimePublishedSourceOriginsV4 projection_{}; // Encode only.
  Current current_{};
  OriginalEncodeVolatileObservation volatile_{};
  std::size_t sourceFragments_ = 0u;
  std::size_t setupSourceFragments_ = 0u, teardownSourceFragments_ = 0u;
  std::size_t setupDrawAttempts_ = 0u, teardownDrawAttempts_ = 0u;
  std::size_t outsideBodyRecordCallbacks_ = 0u; // Producer only.
  OriginalEncodeSourceScope sourceScope_ = OriginalEncodeSourceScope::InvalidSource; // Encode only.
  std::uint64_t attemptedInspectionBytes_ = 0u;
  std::vector<OriginalEncodeIssuedReceipt> receipts_{};
};

} // namespace dxmt9::test
