#pragma once

// Call-local value contracts shared by the encode coordinator, render
// backends, and encodeChunk. Spans borrow synchronously represented source or
// planner storage and must not escape the enclosing encode call.

#include "../winemetal/Metal.hpp"
#include "dxmt9_encode_session.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

namespace dxmt9::encoders {

// A call-local command fragment whose completion source was registered on the
// EncodeSession before any fragment is encoded. This keeps replay coverage
// independent from completion publication: slot/seq/source attribution still
// comes from the encodeChunk call and partitionSource, while this value selects
// only the command subrange consumed by the call.
struct PreRegisteredEncodeChunkFragment {
  std::size_t commandBegin = 0;
  std::size_t commandCount = 0;
  std::uint32_t sourceFragmentOrdinal = 0;
  std::uint32_t sourceFragmentCount = 1;
  std::uint32_t transactionFragmentOrdinal = 0;
  std::uint32_t transactionFragmentCount = 1;

  constexpr bool firstSourceFragment() const noexcept {
    return sourceFragmentOrdinal == 0u;
  }

  constexpr bool lastSourceFragment() const noexcept {
    return sourceFragmentCount != 0u &&
           sourceFragmentOrdinal + 1u == sourceFragmentCount;
  }


  constexpr bool firstTransactionFragment() const noexcept {
    return transactionFragmentOrdinal == 0u;
  }
};

// Call-local source accumulator owned by the qualified replay transaction.
// Multiple fragments of one retained source share one instance; encodeChunk
// emits source-wide diagnostics only from the last fragment.
struct PreRegisteredEncodeSourceFragmentAccumulator {
  std::uint64_t committedSubCommandBuffers = 0;
};

// Call-local diagnostic provenance for a bounded multi-source planning
// window. It never authorizes replay or pass continuation: the queue assigns
// it only after the existing planner/preflight decision, and encodeChunk uses
// it solely to attribute physical render-pass starts and re-entry history.
enum class ReplayWindowDisposition : std::uint8_t {
  Ordinary,
  NaturalAfterMergeFallback,
  PermutationRejectedFallback,
  PlannedComposite,
  EligibilityPresent,
  EligibilityOther,
};

struct ReplayWindowProvenance {
  ReplayWindowDisposition disposition = ReplayWindowDisposition::Ordinary;
  std::uint64_t windowId = 0;
  std::uint32_t sourceIndex = 0;
  std::uint32_t sourceCount = 0;

  constexpr bool valid() const noexcept {
    return disposition != ReplayWindowDisposition::Ordinary &&
        windowId != 0u && sourceCount >= 2u && sourceIndex < sourceCount;
  }

  friend constexpr bool operator==(const ReplayWindowProvenance&,
                                   const ReplayWindowProvenance&) = default;
};

static_assert(std::is_trivially_copyable_v<ReplayWindowProvenance>);
static_assert(std::is_standard_layout_v<ReplayWindowProvenance>);

struct EncodeChunkOptions {
  // Optional open command buffer supplied by an encoded-pending-tail carrier.
  // When present, encodeChunk appends work into this command buffer. Internal
  // semantic pass/barrier splits remain disabled unless the caller opts into
  // allowInjectedCommandBufferMidChunkCommits below.
  WMT::Reference<WMT::CommandBuffer> commandBuffer{};
  // Force-disable the normal mid-chunk split policy. Used only by callers that
  // intentionally want one command buffer for the whole logical unit.
  bool disableMidChunkCommits = false;
  // Semantic pass/barrier boundaries may commit an injected CB and continue
  // into a new tail CB when the caller can merge the already-committed prefix
  // into the final completion source list without violating locality.
  bool allowInjectedCommandBufferMidChunkCommits = false;
  // Optional backend-planned immutable serial partition ranges. Snapshot
  // values contain no live Metal object or borrowed source storage; this span
  // itself is call-local and must not be retained. Empty selects allocation-
  // free identity traversal of the effective replay stream.
  std::span<const EncodePartitionRangeSnapshot> partitionRanges{};
  // Stable Tape identity used by partition snapshots independently of
  // EncodeSession participation. Required for any published partition entry.
  core::CpuReadyTape::SourceRef partitionSource{};
  // Optional session owner for render-pass carry candidates. When a final
  // submission is returned, the caller must transfer the session owner into the
  // QueueSubmissionRecord via retainEncodeChunkSessionUntilSubmissionComplete()
  // so session-owned Metal references live until command-buffer completion.
  EncodeChunkSessionState* session = nullptr;
  // Deferred render-session path. When true and `session` is non-null,
  // encodeChunk returns after appending commands without ending the active
  // session or publishing session-owned callbacks / GPU samples into the
  // returned record. A later encodeChunk call on the same session must
  // finalize it before the shared command buffer is submitted.
  bool deferSessionFinalization = false;
  // Compact source metadata represented by this encodeChunk call when a
  // session is active. The session publishes the ordered list during final
  // submission so one Metal tail can expand to per-source seqId completion.
  std::optional<core::metalqueue::QueueCompletionSource> sessionSource{};
  // Explicit fragment mode for a source already registered transactionally on
  // the session. It is mutually exclusive with sessionSource: the latter both
  // selects its range and appends completion metadata after replay, whereas a
  // pre-registered fragment must never append the source again. The optional
  // type makes this ownership mode impossible to confuse with an ordinary
  // partial sessionSource range.
  std::optional<PreRegisteredEncodeChunkFragment> preRegisteredFragment{};
  PreRegisteredEncodeSourceFragmentAccumulator*
      preRegisteredSourceAccumulator = nullptr;
  // Call-local selected FIFO source suffix used for load/store and FrameGraph
  // lookahead proofs, and as partitionRanges' retained source table. The span
  // points at synchronously resolved, Represented-pinned sources and must not
  // be retained by encodeChunk or EncodeSession.
  std::span<const core::metalqueue::ResolvedPublishedSource>
      sessionLookaheadSources{};
  // Observability-only identity of the bounded planner/fallback window that
  // owns this synchronous call. The value is copied into render-pass history;
  // it must not influence replay, encoder lifetime, or completion ownership.
  ReplayWindowProvenance replayWindow{};
  // Observation-only exact active-seed merge tickets. The queue installs
  // these only for a revalidated NaturalAfterMerge + SeedMerged fallback.
  // The span borrows planner storage for this synchronous encode call.
  ActiveSeedMergeTicketContext activeSeedMergeTicket{};
  std::span<const ActiveSeedMergeTargetWitness> activeSeedMergeTargets{};
  // Optional validated source-command replay plan produced by the Frame Graph.
  // Passcoalesce supplies a complete permutation; DCE may supply an ordered
  // subset, including an empty subset when every command belongs to a proven
  // dead pass. `replayCommandPlanActive` distinguishes that case from the
  // source-order default. Records still execute through the existing canonical
  // encodeChunk switch. The span is call-local and must not be retained.
  bool replayCommandPlanActive = false;
  std::span<const std::uint32_t> replayCommandOrder{};
  // Commands from the full optimized plan already encoded into `session`.
  // The final DCE lookahead call validates that this sequence is an exact
  // prefix of the fresh optimized plan, then replays only the remaining tail.
  // The span is call-local and must not be retained.
  std::span<const std::uint32_t> replayCommandsAlreadyEncoded{};
  // The queue already supplied a validated optimized prefix for this partial
  // call. Skip backend observation and replanning until the final suffix call,
  // which observes the complete graph and validates that prefix.
  bool skipBackendPlanning = false;

  bool hasInjectedCommandBuffer() const noexcept {
    return static_cast<bool>(commandBuffer);
  }
};

}  // namespace dxmt9::encoders
