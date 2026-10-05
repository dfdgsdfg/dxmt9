#pragma once

// Second, CPU-specific admission for an already-built general-provider plan.
// This is deliberately a narrowing gate: the general provider owns tape and
// command validation, while this predicate only checks the bounded CPU lane's
// grammar and capacity before any effect is allowed.

#include "../../../src/d3d9/device_c_render_tape_runtime_objects.hpp"
#include "../../../src/d3d9/device_c_render_tape_descriptors.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>

namespace dxmt9::d3d9::render_tape_runtime {

inline constexpr std::uint32_t kGeneralCpuNoIndex = 0xffffffffu;

struct GeneralCpuAdmissionCaps {
  // These are bounded-input grammar caps, independent of the command queue's
  // in-flight ring. The production caller must reserve queue/source capacity
  // separately before executing an admitted plan.
  std::size_t maxEvents = 2048u;
  std::size_t maxObjects = 512u;
  std::size_t maxMutations = 1024u;
  std::size_t maxChunks = 128u;
  std::size_t maxRecords = 4096u;
  std::uint64_t maxBlobBytes = 256ull * 1024ull * 1024ull;
  // Capture-cadence input bound for this first grammar. This is not a proof
  // of queue progress or source-slot availability; production routing must
  // perform its own pre-effect queue/source reservation.
  std::size_t maxRecordsPerChunk = 64u;
  std::size_t maxDraws = 4096u;
  std::size_t maxBlobs = 2048u;
  std::size_t maxQueryDefinitions = 512u;
  std::size_t maxQueryIssues = 4096u;
  std::size_t maxQueryPolls = 2048u;
  // Retain the legacy per-domain record bound; bootstrap and body also share
  // an explicit total work bound. Chunks historically shared their bound.
  std::size_t maxTotalRecords = 8192u;
  std::size_t maxTotalChunks = 128u;

  constexpr bool operator==(const GeneralCpuAdmissionCaps&) const = default;
};

// Explicit opt-in input profile. Queue capacity and publication cadence are
// independent and must still be admitted before execution.
inline constexpr GeneralCpuAdmissionCaps generalCpuExtendedInputCaps() noexcept {
  return {.maxEvents = 8192u,
          .maxObjects = 1024u,
          .maxMutations = 8192u,
          .maxChunks = 256u,
          .maxRecords = 8192u,
          .maxBlobBytes = 512ull * 1024ull * 1024ull,
          .maxRecordsPerChunk = 64u,
          .maxDraws = 4096u,
          .maxBlobs = 8192u,
          .maxQueryDefinitions = 1024u,
          .maxQueryIssues = 8192u,
          .maxQueryPolls = 8192u,
          .maxTotalRecords = 8192u,
          .maxTotalChunks = 256u};
}

struct GeneralCpuSourceCapacity {
  std::size_t maxPublications = 4096u;
  // One submission or one query probe consumes one progress step. This is
  // total session work, distinct from the queue's simultaneous resident ring.
  std::size_t maxProgressSteps = 4096u;
};

struct GeneralCpuSourceDemand {
  std::size_t publications = 0u;
  std::size_t progressSteps = 0u;
};

enum class GeneralCpuSourceAdmission : std::uint8_t {
  Accepted, InvalidCapacity, PublicationLimit, ProgressLimit,
};

inline constexpr GeneralCpuSourceAdmission admitGeneralCpuSourceDemand(
    const GeneralCpuSourceDemand& demand,
    const GeneralCpuSourceCapacity& capacity) noexcept {
  constexpr auto indexLimit = std::numeric_limits<std::uint32_t>::max() - 1u;
  if (capacity.maxPublications == 0u || capacity.maxProgressSteps == 0u ||
      capacity.maxPublications > indexLimit ||
      capacity.maxProgressSteps > indexLimit)
    return GeneralCpuSourceAdmission::InvalidCapacity;
  if (demand.publications > capacity.maxPublications)
    return GeneralCpuSourceAdmission::PublicationLimit;
  if (demand.progressSteps < demand.publications ||
      demand.progressSteps > capacity.maxProgressSteps)
    return GeneralCpuSourceAdmission::ProgressLimit;
  return GeneralCpuSourceAdmission::Accepted;
}

struct GeneralCpuAdmissionCounts {
  // The primary counters intentionally match general-provider coverage:
  // chunks and records exclude bootstrap overlays. Their separate counters
  // make the bounded bootstrap contribution observable without changing the
  // r4 capture accounting.
  std::size_t events = 0u;
  std::size_t objects = 0u;
  std::size_t mutations = 0u;
  std::size_t chunks = 0u;
  std::size_t records = 0u;
  std::size_t draws = 0u;
  std::size_t blobs = 0u;
  std::uint64_t blobBytes = 0u;
  std::size_t bootstrapChunks = 0u;
  std::size_t bootstrapRecords = 0u;
  std::size_t presents = 0u;
  std::size_t presentCompletions = 0u;
  std::size_t queryDefinitions = 0u;
  std::size_t queryIssues = 0u;
  std::size_t queryPolls = 0u;
  std::size_t queryCheckpoints = 0u;

  constexpr bool operator==(const GeneralCpuAdmissionCounts&) const = default;
};

enum class GeneralCpuAdmissionReason : std::uint8_t {
  Accepted,
  InvalidBudget,
  EventLimit,
  ObjectLimit,
  MutationLimit,
  ChunkLimit,
  RecordLimit,
  PerChunkRecordLimit,
  BlobBytesLimit,
  BlobCountLimit,
  DrawLimit,
  QueryDefinitionLimit,
  QueryIssueLimit,
  QueryPollLimit,
  InvalidEventReference,
  UnsupportedEvent,
  UnsupportedObject,
  DuplicateBootstrap,
  GammaUnsupported,
  OrderedControlUnsupported,
  CommandChunkUnavailable,
  UnsupportedCommandRecord,
  PresentCount,
  PresentCompleteCount,
  PresentCompleteNotFinal,
  OutputDescriptorMissing,
  QueryTranscriptUnavailable,
};

struct GeneralCpuAdmissionResult {
  GeneralCpuAdmissionReason reason = GeneralCpuAdmissionReason::Accepted;
  std::uint32_t eventIndex = kGeneralCpuNoIndex;
  std::uint32_t chunkIndex = kGeneralCpuNoIndex;
  std::uint32_t recordIndex = kGeneralCpuNoIndex;
  GeneralCpuAdmissionCounts counts{};
  GeneralCpuAdmissionCaps inputCaps{};

  constexpr bool accepted() const noexcept {
    return reason == GeneralCpuAdmissionReason::Accepted;
  }
};

inline const char* generalCpuAdmissionReasonName(
    GeneralCpuAdmissionReason reason) noexcept {
  switch (reason) {
  case GeneralCpuAdmissionReason::Accepted: return "accepted";
  case GeneralCpuAdmissionReason::InvalidBudget: return "invalid-budget";
  case GeneralCpuAdmissionReason::EventLimit: return "event-limit";
  case GeneralCpuAdmissionReason::ObjectLimit: return "object-limit";
  case GeneralCpuAdmissionReason::MutationLimit: return "mutation-limit";
  case GeneralCpuAdmissionReason::ChunkLimit: return "chunk-limit";
  case GeneralCpuAdmissionReason::RecordLimit: return "record-limit";
  case GeneralCpuAdmissionReason::PerChunkRecordLimit:
    return "per-chunk-record-limit";
  case GeneralCpuAdmissionReason::BlobBytesLimit: return "blob-bytes-limit";
  case GeneralCpuAdmissionReason::BlobCountLimit: return "blob-count-limit";
  case GeneralCpuAdmissionReason::DrawLimit: return "draw-limit";
  case GeneralCpuAdmissionReason::QueryDefinitionLimit: return "query-definition-limit";
  case GeneralCpuAdmissionReason::QueryIssueLimit: return "query-issue-limit";
  case GeneralCpuAdmissionReason::QueryPollLimit: return "query-poll-limit";
  case GeneralCpuAdmissionReason::InvalidEventReference:
    return "invalid-event-reference";
  case GeneralCpuAdmissionReason::UnsupportedEvent: return "unsupported-event";
  case GeneralCpuAdmissionReason::UnsupportedObject: return "unsupported-object";
  case GeneralCpuAdmissionReason::DuplicateBootstrap:
    return "duplicate-bootstrap";
  case GeneralCpuAdmissionReason::GammaUnsupported: return "gamma-unsupported";
  case GeneralCpuAdmissionReason::OrderedControlUnsupported:
    return "ordered-control-unsupported";
  case GeneralCpuAdmissionReason::CommandChunkUnavailable:
    return "command-chunk-unavailable";
  case GeneralCpuAdmissionReason::UnsupportedCommandRecord:
    return "unsupported-command-record";
  case GeneralCpuAdmissionReason::PresentCount: return "present-count";
  case GeneralCpuAdmissionReason::PresentCompleteCount:
    return "present-complete-count";
  case GeneralCpuAdmissionReason::PresentCompleteNotFinal:
    return "present-complete-not-final";
  case GeneralCpuAdmissionReason::OutputDescriptorMissing:
    return "output-descriptor-missing";
  case GeneralCpuAdmissionReason::QueryTranscriptUnavailable:
    return "query-transcript-unavailable";
  }
  return "unknown";
}

namespace detail {

inline GeneralCpuAdmissionResult generalCpuReject(
    GeneralCpuAdmissionReason reason, const GeneralCpuAdmissionCounts& counts,
    std::uint32_t eventIndex = kGeneralCpuNoIndex,
    std::uint32_t chunkIndex = kGeneralCpuNoIndex,
    std::uint32_t recordIndex = kGeneralCpuNoIndex) noexcept {
  return {.reason = reason,
          .eventIndex = eventIndex,
          .chunkIndex = chunkIndex,
          .recordIndex = recordIndex,
          .counts = counts};
}

inline bool generalCpuSameIdentity(const D9CWireObjectIdentity& left,
                                   const D9CWireObjectIdentity& right) noexcept {
  return left.kind == right.kind && left.generation == right.generation &&
         left.objectId == right.objectId;
}

inline bool generalCpuOutputSurfaceSupported(const D9CSurfaceDesc& desc) noexcept {
  return desc.resourceType == 1u && desc.width != 0u && desc.height != 0u &&
         desc.depth == 1u && desc.pool == 0u && desc.multiSampleType == 0u &&
         desc.multiSampleQuality == 0u && (desc.usage & 1u) != 0u &&
         (desc.usage & 2u) == 0u && (desc.format == 21u || desc.format == 22u);
}

inline bool generalCpuCommandRecordSupported(std::uint32_t type) noexcept {
  switch (type) {
  case D9C_COMMAND_RECORD_DRAW_PRIMITIVE:
  case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE:
  case D9C_COMMAND_RECORD_SET_VS_CONST_F:
  case D9C_COMMAND_RECORD_SET_PS_CONST_F:
  case D9C_COMMAND_RECORD_APPLY_STATE:
  case D9C_COMMAND_RECORD_CLEAR:
  case D9C_COMMAND_RECORD_PRESENT:
  case D9C_COMMAND_RECORD_GENERATE_MIPMAPS:
  case D9C_COMMAND_RECORD_QUERY_ISSUE:
    return true;
  default:
    return false;
  }
}

inline bool generalCpuAdd(std::size_t value, std::size_t limit,
                          std::size_t& total) noexcept {
  if (value > limit || total > limit - value) return false;
  total += value;
  return true;
}

inline bool generalCpuAddBlob(std::uint64_t value, std::uint64_t limit,
                              std::uint64_t& total) noexcept {
  if (value > limit || total > limit - value) return false;
  total += value;
  return true;
}

inline bool generalCpuMultiply(std::size_t left, std::size_t right,
                               std::size_t& product) noexcept {
  if (right != 0u && left > std::numeric_limits<std::size_t>::max() / right)
    return false;
  product = left * right;
  return true;
}

inline std::uint32_t generalCpuEventForOrdinal(
    const GeneralPreflightPlan& plan, RenderTapeEventType type,
    std::size_t ordinal) noexcept {
  std::size_t seen = 0u;
  for (const auto& event : plan.events) {
    if (event.type != type) continue;
    if (seen == ordinal) return event.eventIndex;
    ++seen;
  }
  return kGeneralCpuNoIndex;
}

inline std::uint32_t generalCpuEventAtPosition(
    const GeneralPreflightPlan& plan, std::size_t position) noexcept {
  return position < plan.events.size() ? plan.events[position].eventIndex
                                       : kGeneralCpuNoIndex;
}

inline std::uint32_t generalCpuEventForChunk(
    const GeneralPreflightPlan& plan, bool bootstrap,
    std::size_t chunkIndex) noexcept {
  for (const auto& event : plan.events) {
    if (event.type == (bootstrap ? RenderTapeEventType::BootstrapState
                                 : RenderTapeEventType::CommandChunk) &&
        event.index == chunkIndex)
      return event.eventIndex;
  }
  return kGeneralCpuNoIndex;
}

}  // namespace detail

// Includes the original query probe and its bounded supplemental probes.
// The transcript's uint32 evidence contract is checked before any narrowing.
inline bool addGeneralCpuQueryProgressDemand(
    const GeneralCpuAdmissionCounts& counts, std::uint32_t oraclePollsPerQuery,
    GeneralCpuSourceDemand& demand) noexcept {
  if (oraclePollsPerQuery > 64u) return false;
  std::size_t supplemental = 0u;
  std::size_t queryCycles = counts.queryPolls;
  if (!detail::generalCpuAdd(counts.queryCheckpoints,
                            std::numeric_limits<std::size_t>::max(), queryCycles) ||
      !detail::generalCpuMultiply(queryCycles, oraclePollsPerQuery, supplemental) ||
      supplemental > std::numeric_limits<std::uint32_t>::max())
    return false;
  auto candidate = demand;
  if (!detail::generalCpuAdd(queryCycles, std::numeric_limits<std::size_t>::max(),
                            candidate.progressSteps) ||
      !detail::generalCpuAdd(supplemental, std::numeric_limits<std::size_t>::max(),
                            candidate.progressSteps))
    return false;
  demand = candidate;
  return true;
}

inline bool addGeneralCpuSourcePublications(std::size_t publications,
                                           GeneralCpuSourceDemand& demand) noexcept {
  auto candidate = demand;
  if (!detail::generalCpuAdd(publications, std::numeric_limits<std::size_t>::max(),
                            candidate.publications) ||
      !detail::generalCpuAdd(publications, std::numeric_limits<std::size_t>::max(),
                            candidate.progressSteps))
    return false;
  demand = candidate;
  return true;
}

// `plan` must be the immutable result of buildGeneralReplayPlan. This helper
// intentionally does not validate tape events, descriptors, mutation bytes,
// or command record bodies; those are the general-provider contract. The only
// command inspection is importPrevalidatedCommandChunk(), which reconstructs
// safe views over the already-validated bytes.
namespace detail {

// Input-only specialization. This tag grants no source capacity or progress
// authority, and the legacy public wrapper always selects its original policy.
enum class GeneralCpuInputValidationPolicy : std::uint8_t {
  Legacy,
  ControlledV4Raw256,
};

inline GeneralCpuAdmissionResult admitGeneralCpuPlanWithPolicy(
    const GeneralPreflightPlan& plan,
    const GeneralCpuAdmissionCaps& caps,
    GeneralCpuInputValidationPolicy policy) noexcept {
  GeneralCpuAdmissionCounts counts{
      .events = plan.events.size(),
      .objects = plan.definitions.size(),
      .mutations = plan.mutations.size(),
      .chunks = plan.commandChunks.size(),
      .blobs = plan.catalogue.blobs.size(),
      .blobBytes = 0u,
      .bootstrapChunks = plan.bootstrapChunks.size(),
  };
  if (policy != GeneralCpuInputValidationPolicy::Legacy &&
      policy != GeneralCpuInputValidationPolicy::ControlledV4Raw256)
    return detail::generalCpuReject(GeneralCpuAdmissionReason::InvalidBudget, counts);
  const auto bootstrapRecordLimit = policy == GeneralCpuInputValidationPolicy::ControlledV4Raw256
      ? 64u : caps.maxRecordsPerChunk;

  constexpr auto indexLimit = std::numeric_limits<std::uint32_t>::max() - 1u;
  for (const auto cap : {caps.maxEvents, caps.maxObjects, caps.maxMutations,
                        caps.maxChunks, caps.maxRecords, caps.maxRecordsPerChunk,
                        caps.maxDraws, caps.maxBlobs, caps.maxQueryDefinitions,
                        caps.maxQueryIssues, caps.maxQueryPolls,
                        caps.maxTotalRecords, caps.maxTotalChunks}) {
    if (cap > indexLimit)
      return detail::generalCpuReject(GeneralCpuAdmissionReason::InvalidBudget, counts);
  }

  // All aggregate bounds are established before semantic iteration. This is
  // important for a plan assembled from untrusted capture metadata: no later
  // loop may grow or index a derived collection past a CPU-lane cap.
  if (counts.events > caps.maxEvents)
    return detail::generalCpuReject(GeneralCpuAdmissionReason::EventLimit,
                                    counts,
                                    detail::generalCpuEventAtPosition(
                                        plan, caps.maxEvents));
  if (counts.objects > caps.maxObjects)
    return detail::generalCpuReject(GeneralCpuAdmissionReason::ObjectLimit,
                                    counts,
                                    detail::generalCpuEventForOrdinal(
                                        plan, RenderTapeEventType::ObjectDefine,
                                        caps.maxObjects));
  if (counts.mutations > caps.maxMutations)
    return detail::generalCpuReject(GeneralCpuAdmissionReason::MutationLimit,
                                    counts,
                                    detail::generalCpuEventForOrdinal(
                                        plan, RenderTapeEventType::ResourceMutation,
                                        caps.maxMutations));
  const auto commandChunkLimit = caps.maxChunks < caps.maxTotalChunks
      ? caps.maxChunks : caps.maxTotalChunks;
  if (counts.chunks > caps.maxChunks || counts.bootstrapChunks > caps.maxChunks ||
      counts.chunks > caps.maxTotalChunks ||
      counts.bootstrapChunks > caps.maxTotalChunks - counts.chunks) {
    if (counts.chunks > commandChunkLimit) {
      return detail::generalCpuReject(
          GeneralCpuAdmissionReason::ChunkLimit, counts,
          detail::generalCpuEventForChunk(plan, false, commandChunkLimit),
          static_cast<std::uint32_t>(commandChunkLimit));
    }
    return detail::generalCpuReject(
        GeneralCpuAdmissionReason::ChunkLimit, counts,
        detail::generalCpuEventForChunk(
            plan, true, counts.bootstrapChunks > caps.maxChunks
                ? caps.maxChunks : caps.maxTotalChunks - counts.chunks),
        static_cast<std::uint32_t>(counts.bootstrapChunks > caps.maxChunks
            ? caps.maxChunks : caps.maxTotalChunks - counts.chunks));
  }

  if (counts.blobs > caps.maxBlobs)
    return detail::generalCpuReject(GeneralCpuAdmissionReason::BlobCountLimit, counts);
  for (const auto& blob : plan.catalogue.blobs) {
    if (!detail::generalCpuAddBlob(blob.size, caps.maxBlobBytes,
                                   counts.blobBytes))
      return detail::generalCpuReject(GeneralCpuAdmissionReason::BlobBytesLimit,
                                      counts);
  }

  // Envelope counts are immutable plan metadata and are checked before any
  // record view is imported. Bootstrap overlays are counted for capacity but
  // remain separate from the general-provider command coverage counters.
  std::size_t totalRecords = 0u;
  for (std::size_t chunkIndex = 0u; chunkIndex < plan.bootstrapChunks.size();
       ++chunkIndex) {
    const auto& chunk = plan.bootstrapChunks[chunkIndex];
    if (chunk.envelope.recordCount > bootstrapRecordLimit)
      return detail::generalCpuReject(
          GeneralCpuAdmissionReason::PerChunkRecordLimit, counts,
          detail::generalCpuEventForChunk(plan, true, chunkIndex),
          static_cast<std::uint32_t>(chunkIndex));
    if (!detail::generalCpuAdd(chunk.envelope.recordCount, caps.maxRecords,
                              counts.bootstrapRecords) ||
        !detail::generalCpuAdd(chunk.envelope.recordCount, caps.maxTotalRecords, totalRecords))
      return detail::generalCpuReject(GeneralCpuAdmissionReason::RecordLimit,
          counts, detail::generalCpuEventForChunk(plan, true, chunkIndex),
          static_cast<std::uint32_t>(chunkIndex));
    ImportedChunkView bootstrap;
    if (!importPrevalidatedCommandChunk(chunk.bytes, chunk.envelope, bootstrap))
      return detail::generalCpuReject(GeneralCpuAdmissionReason::CommandChunkUnavailable,
          counts, detail::generalCpuEventForChunk(plan, true, chunkIndex),
          static_cast<std::uint32_t>(chunkIndex));
    for (std::size_t record = 0u; record < bootstrap.records.size(); ++record) {
      const auto type = bootstrap.records[record].type;
      if (type != D9C_COMMAND_RECORD_APPLY_STATE &&
          type != D9C_COMMAND_RECORD_SET_VS_CONST_F &&
          type != D9C_COMMAND_RECORD_SET_PS_CONST_F)
        return detail::generalCpuReject(GeneralCpuAdmissionReason::UnsupportedCommandRecord,
            counts, detail::generalCpuEventForChunk(plan, true, chunkIndex),
            static_cast<std::uint32_t>(chunkIndex), static_cast<std::uint32_t>(record));
    }
  }
  for (std::size_t chunkIndex = 0u; chunkIndex < plan.commandChunks.size();
       ++chunkIndex) {
    const auto& chunk = plan.commandChunks[chunkIndex];
    if (chunk.envelope.recordCount > caps.maxRecordsPerChunk)
      return detail::generalCpuReject(
          GeneralCpuAdmissionReason::PerChunkRecordLimit, counts,
          detail::generalCpuEventForChunk(plan, false, chunkIndex),
          static_cast<std::uint32_t>(chunkIndex));
    if (!detail::generalCpuAdd(chunk.envelope.recordCount, caps.maxRecords,
                               counts.records) ||
        !detail::generalCpuAdd(chunk.envelope.recordCount, caps.maxTotalRecords, totalRecords))
      return detail::generalCpuReject(GeneralCpuAdmissionReason::RecordLimit,
                                      counts,
                                      detail::generalCpuEventForChunk(
                                          plan, false, chunkIndex),
                                      static_cast<std::uint32_t>(chunkIndex));
  }
  if (caps.maxEvents == 0u || caps.maxObjects == 0u || caps.maxMutations == 0u ||
      caps.maxChunks == 0u || caps.maxRecords == 0u || caps.maxBlobBytes == 0u ||
      caps.maxRecordsPerChunk == 0u || caps.maxDraws == 0u || caps.maxBlobs == 0u ||
      caps.maxQueryDefinitions == 0u || caps.maxQueryIssues == 0u ||
      caps.maxQueryPolls == 0u || caps.maxTotalRecords == 0u ||
      caps.maxTotalChunks == 0u || caps.maxRecordsPerChunk > caps.maxRecords ||
      caps.maxRecords > caps.maxTotalRecords || caps.maxChunks > caps.maxTotalChunks ||
      caps.maxQueryDefinitions > caps.maxObjects)
    return detail::generalCpuReject(GeneralCpuAdmissionReason::InvalidBudget, counts);

  if (plan.controls.size() != plan.controlBytes.size())
    return detail::generalCpuReject(
        GeneralCpuAdmissionReason::OrderedControlUnsupported, counts);
  for (const auto& gamma : plan.bootstrapGamma) {
    bool supported = gamma.empty() || gamma.size() == kRenderTapeGammaRampBytes;
    if (policy == GeneralCpuInputValidationPolicy::Legacy) {
      for (std::size_t offset = 0u; supported && offset < gamma.size(); offset += 2u) {
        std::uint16_t value = 0u;
        std::memcpy(&value, gamma.data() + offset, sizeof(value));
        supported = value == static_cast<std::uint16_t>(((offset / 2u) % 256u) << 8u);
      }
    }
    if (!supported)
      return detail::generalCpuReject(GeneralCpuAdmissionReason::GammaUnsupported,
                                      counts);
  }

  bool sawBootstrap = false;
  bool sawPresentComplete = false;
  std::size_t referencedDefinitions = 0u;
  std::size_t referencedMutations = 0u;
  std::size_t referencedBootstrapChunks = 0u;
  std::size_t referencedCommandChunks = 0u;
  std::size_t referencedDestroys = 0u;
  std::size_t referencedControls = 0u;
  std::uint32_t presentEventIndex = kGeneralCpuNoIndex;

  for (std::size_t eventPosition = 0u; eventPosition < plan.events.size();
       ++eventPosition) {
    const auto& event = plan.events[eventPosition];
    const auto eventIndex = event.eventIndex;
    if (eventIndex != eventPosition ||
        (eventPosition != 0u &&
         eventIndex <= plan.events[eventPosition - 1u].eventIndex))
      return detail::generalCpuReject(
          GeneralCpuAdmissionReason::InvalidEventReference, counts, eventIndex);
    if (sawPresentComplete && event.type == RenderTapeEventType::PresentComplete)
      return detail::generalCpuReject(
          GeneralCpuAdmissionReason::PresentCompleteCount, counts, eventIndex);
    if (sawPresentComplete)
      return detail::generalCpuReject(
          GeneralCpuAdmissionReason::PresentCompleteNotFinal, counts,
          eventIndex);

    switch (event.type) {
    case RenderTapeEventType::BootstrapState:
      if (sawBootstrap)
        return detail::generalCpuReject(
            GeneralCpuAdmissionReason::DuplicateBootstrap, counts, eventIndex);
      sawBootstrap = true;
      if (event.index > plan.bootstrapChunks.size() ||
          event.count > plan.bootstrapChunks.size() - event.index ||
          event.index != 0u || event.count == 0u ||
          event.aux >= plan.bootstrapGamma.size())
        return detail::generalCpuReject(
            GeneralCpuAdmissionReason::InvalidEventReference, counts, eventIndex);
      referencedBootstrapChunks += event.count;
      break;
    case RenderTapeEventType::ObjectDefine:
      if (event.index >= plan.definitions.size())
        return detail::generalCpuReject(
          GeneralCpuAdmissionReason::InvalidEventReference, counts, eventIndex);
      if (plan.definitions[event.index].fixed.identity.kind ==
          D9C_CHUNK_HANDLE_KIND_QUERY) {
        RenderTapeQueryDescriptorDecoded query{};
        if (!renderTapeLoadQueryDescriptor(
                plan.definitions[event.index].descriptor, query) ||
            !renderTapeQueryDescriptorIdentityMatches(
                query, plan.definitions[event.index].fixed.identity))
          return detail::generalCpuReject(
              GeneralCpuAdmissionReason::QueryTranscriptUnavailable, counts,
              eventIndex);
        ++counts.queryDefinitions;
        if (counts.queryDefinitions > caps.maxQueryDefinitions)
          return detail::generalCpuReject(
              GeneralCpuAdmissionReason::QueryDefinitionLimit, counts, eventIndex);
        counts.queryCheckpoints += query.initialState == static_cast<std::uint32_t>(
            RenderTapeQueryInitialState::CompletedEventCheckpoint);
      } else if (plan.definitions[event.index].fixed.identity.kind >
                 D9C_CHUNK_HANDLE_KIND_VERTEX_DECL)
        return detail::generalCpuReject(GeneralCpuAdmissionReason::UnsupportedObject,
                                        counts, eventIndex);
      for (std::size_t prior = 0u; prior < eventPosition; ++prior) {
        if (plan.events[prior].type == event.type &&
            plan.events[prior].index == event.index)
          return detail::generalCpuReject(
              GeneralCpuAdmissionReason::InvalidEventReference, counts, eventIndex);
      }
      ++referencedDefinitions;
      break;
    case RenderTapeEventType::ResourceMutation:
      if (event.index >= plan.mutations.size())
        return detail::generalCpuReject(
            GeneralCpuAdmissionReason::InvalidEventReference, counts, eventIndex);
      for (std::size_t prior = 0u; prior < eventPosition; ++prior) {
        if (plan.events[prior].type == event.type &&
            plan.events[prior].index == event.index)
          return detail::generalCpuReject(
              GeneralCpuAdmissionReason::InvalidEventReference, counts, eventIndex);
      }
      ++referencedMutations;
      break;
    case RenderTapeEventType::ObjectDestroy:
      if (event.index >= plan.destroys.size())
        return detail::generalCpuReject(
            GeneralCpuAdmissionReason::InvalidEventReference, counts, eventIndex);
      for (std::size_t prior = 0u; prior < eventPosition; ++prior) {
        if (plan.events[prior].type == event.type &&
            plan.events[prior].index == event.index)
          return detail::generalCpuReject(
              GeneralCpuAdmissionReason::InvalidEventReference, counts, eventIndex);
      }
      ++referencedDestroys;
      break;
    case RenderTapeEventType::CommandChunk: {
      if (event.index >= plan.commandChunks.size())
        return detail::generalCpuReject(
            GeneralCpuAdmissionReason::CommandChunkUnavailable, counts, eventIndex);
      for (std::size_t prior = 0u; prior < eventPosition; ++prior) {
        if (plan.events[prior].type == event.type &&
            plan.events[prior].index == event.index)
          return detail::generalCpuReject(
              GeneralCpuAdmissionReason::InvalidEventReference, counts, eventIndex);
      }
      ++referencedCommandChunks;
      const auto& chunk = plan.commandChunks[event.index];
      ImportedChunkView imported;
      if (!importPrevalidatedCommandChunk(chunk.bytes, chunk.envelope, imported))
        return detail::generalCpuReject(
            GeneralCpuAdmissionReason::CommandChunkUnavailable, counts,
            eventIndex, event.index);
      for (std::size_t recordIndex = 0u; recordIndex < imported.records.size();
           ++recordIndex) {
        if (counts.presents != 0u)
          return detail::generalCpuReject(GeneralCpuAdmissionReason::PresentCount,
              counts, eventIndex, event.index, static_cast<std::uint32_t>(recordIndex));
        const auto record = imported.record(recordIndex);
        if (!detail::generalCpuCommandRecordSupported(record.header.type))
          return detail::generalCpuReject(
              GeneralCpuAdmissionReason::UnsupportedCommandRecord, counts,
              eventIndex, event.index, static_cast<std::uint32_t>(recordIndex));
        if (record.header.type == D9C_COMMAND_RECORD_PRESENT) {
          ++counts.presents;
          if (counts.presents > 1u)
            return detail::generalCpuReject(GeneralCpuAdmissionReason::PresentCount,
                                            counts, eventIndex, event.index,
                                            static_cast<std::uint32_t>(recordIndex));
        }
        if (record.header.type == D9C_COMMAND_RECORD_DRAW_PRIMITIVE ||
            record.header.type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE) {
          ++counts.draws;
          if (counts.draws > caps.maxDraws)
            return detail::generalCpuReject(GeneralCpuAdmissionReason::DrawLimit,
                counts, eventIndex, event.index, static_cast<std::uint32_t>(recordIndex));
        }
        if (record.header.type == D9C_COMMAND_RECORD_QUERY_ISSUE) {
          ++counts.queryIssues;
          if (counts.queryIssues > caps.maxQueryIssues)
            return detail::generalCpuReject(GeneralCpuAdmissionReason::QueryIssueLimit,
                counts, eventIndex, event.index, static_cast<std::uint32_t>(recordIndex));
        }
      }
      break;
    }
    case RenderTapeEventType::OrderedControl: {
      if (event.index >= plan.controls.size() ||
          event.index != referencedControls)
        return detail::generalCpuReject(
            GeneralCpuAdmissionReason::InvalidEventReference, counts, eventIndex);
      const auto& control = plan.controls[event.index];
      if (control.kind != static_cast<std::uint32_t>(RenderTapeControlKind::QueryGetData))
        return detail::generalCpuReject(
            GeneralCpuAdmissionReason::OrderedControlUnsupported, counts, eventIndex);
      if (control.controlBytes != sizeof(RenderTapeQueryGetDataControlV2) ||
          plan.controlBytes[event.index].size() != sizeof(RenderTapeQueryGetDataControlV2))
        return detail::generalCpuReject(
            GeneralCpuAdmissionReason::QueryTranscriptUnavailable, counts, eventIndex);
      RenderTapeQueryGetDataControlV2 poll{};
      std::memcpy(&poll, plan.controlBytes[event.index].data(), sizeof(poll));
      if (poll.schemaVersion != kRenderTapeQueryTranscriptVersion2)
        return detail::generalCpuReject(
            GeneralCpuAdmissionReason::QueryTranscriptUnavailable, counts, eventIndex);
      ++referencedControls;
      ++counts.queryPolls;
      if (counts.queryPolls > caps.maxQueryPolls)
        return detail::generalCpuReject(GeneralCpuAdmissionReason::QueryPollLimit,
                                        counts, eventIndex);
      break;
    }
    case RenderTapeEventType::PresentComplete:
      if (sawPresentComplete || eventPosition + 1u != plan.events.size())
        return detail::generalCpuReject(
            GeneralCpuAdmissionReason::PresentCompleteNotFinal, counts, eventIndex);
      sawPresentComplete = true;
      ++counts.presentCompletions;
      presentEventIndex = eventIndex;
      break;
    default:
      return detail::generalCpuReject(GeneralCpuAdmissionReason::UnsupportedEvent,
                                      counts, eventIndex);
    }
  }

  counts.presentCompletions = sawPresentComplete ? 1u : 0u;
  if (!sawBootstrap || referencedDefinitions != plan.definitions.size() ||
      referencedMutations != plan.mutations.size() ||
      referencedBootstrapChunks != plan.bootstrapChunks.size() ||
      referencedCommandChunks != plan.commandChunks.size() ||
      referencedDestroys != plan.destroys.size() ||
      referencedControls != plan.controls.size())
    return detail::generalCpuReject(GeneralCpuAdmissionReason::InvalidEventReference,
                                    counts);
  if (!sawPresentComplete)
    return detail::generalCpuReject(
        GeneralCpuAdmissionReason::PresentCompleteCount, counts);
  if (counts.presents != 1u)
    return detail::generalCpuReject(GeneralCpuAdmissionReason::PresentCount, counts,
                                    presentEventIndex);

  bool outputFound = false;
  if (plan.outputIdentity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE &&
      plan.outputIdentity.generation != 0u && plan.outputIdentity.objectId != 0u &&
      detail::generalCpuOutputSurfaceSupported(plan.outputDesc)) {
    for (const auto& definition : plan.definitions) {
      if (detail::generalCpuSameIdentity(definition.fixed.identity,
                                         plan.outputIdentity) &&
          definition.fixed.descriptorKind ==
              static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface) &&
          definition.fixed.descriptorBytes != 0u &&
          definition.descriptor.size() == definition.fixed.descriptorBytes) {
        outputFound = true;
        break;
      }
    }
  }
  if (!outputFound)
    return detail::generalCpuReject(
        GeneralCpuAdmissionReason::OutputDescriptorMissing, counts);

  return {.reason = GeneralCpuAdmissionReason::Accepted,
          .eventIndex = kGeneralCpuNoIndex,
          .chunkIndex = kGeneralCpuNoIndex,
          .recordIndex = kGeneralCpuNoIndex,
          .counts = counts,
          .inputCaps = caps};
}

}  // namespace detail

inline GeneralCpuAdmissionResult admitGeneralCpuPlanImpl(
    const GeneralPreflightPlan& plan,
    const GeneralCpuAdmissionCaps& caps) noexcept {
  return detail::admitGeneralCpuPlanWithPolicy(
      plan, caps, detail::GeneralCpuInputValidationPolicy::Legacy);
}

inline GeneralCpuAdmissionResult admitGeneralCpuPlan(
    const GeneralPreflightPlan& plan,
    const GeneralCpuAdmissionCaps& caps = {}) noexcept {
  auto result = admitGeneralCpuPlanImpl(plan, caps);
  result.inputCaps = caps;
  return result;
}

}  // namespace dxmt9::d3d9::render_tape_runtime
