#pragma once

#include "util/log/log.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <optional>
#include <type_traits>

// Cold, bounded evidence for one query END.  This header deliberately carries
// no core or Metal types: the PE/core observer and the unix queue observer use
// the same value-only identity and marker witness.
namespace dxmt9::query::timeline_diag {

inline constexpr std::size_t kCapacity = 128;

struct Identity {
  std::uint64_t queue = 0;
  std::uint64_t query = 0;
  std::uint64_t issue = 0;
  std::uint64_t epoch = 0;
  friend constexpr bool operator==(Identity, Identity) = default;
};

struct MarkerWitness {
  std::uint64_t queue = 0;
  std::uint32_t index = std::numeric_limits<std::uint32_t>::max();
  std::uint64_t generation = 0;
  std::uint64_t storageGeneration = 0;
  constexpr bool valid() const noexcept {
    return queue != 0 && index != std::numeric_limits<std::uint32_t>::max() &&
           generation != 0 && storageGeneration != 0;
  }
  friend constexpr bool operator==(MarkerWitness, MarkerWitness) = default;
};

enum Stage : std::uint8_t {
  End = 1u << 0,
  Publication = 1u << 1,
  EncodeStart = 1u << 2,
  EncodeEnd = 1u << 3,
  ActualSubmit = 1u << 4,
  Completion = 1u << 5,
};

struct Summary {
  Identity identity{};
  std::uint32_t type = 0;
  std::uint8_t stages = 0;
  std::uint64_t fenceSequence = 0;
  MarkerWitness marker{};
  // The core callback runs after publication, so this is END-return evidence,
  // intentionally named to keep negative publication deltas explainable.
  std::uint64_t endObservedNs = 0;
  std::uint64_t fenceInitializer = 0;
  std::uint64_t publicationNs = 0;
  std::uint64_t encodeStartNs = 0;
  std::uint64_t encodeEndNs = 0;
  std::uint64_t submitNs = 0;
  std::uint64_t completionNs = 0;
  std::uint64_t firstPendingNs = 0;
  std::uint64_t lastPendingNs = 0;
  std::uint64_t firstOkNs = 0;
  std::uint64_t lastOkNs = 0;
  std::uint64_t polls = 0;
  std::uint64_t pendingPolls = 0;
  std::uint64_t okPolls = 0;
  std::optional<std::uint64_t> resultValue{};
  // Cold Ready-residence evidence is a queue snapshot, not a scheduling
  // decision. The frontier values describe the Ready state immediately before
  // this source (the just-published entry is excluded); zero means unavailable.
  bool readyFrontierKnown = false;
  std::uint64_t readyDepthBefore = 0;
  std::uint64_t readyOldestSeqBefore = 0;
  std::uint64_t encodingSeqBefore = 0;
  std::uint64_t submittedFrontierBefore = 0;
  std::uint64_t completedFrontierBefore = 0;
  std::uint64_t dequeueNs = 0;
  std::uint64_t predecessorEncodeSeq = 0;
  std::uint64_t predecessorEncodeStartNs = 0;
  std::uint64_t predecessorEncodeEndNs = 0;
  bool predecessorEncodeOverlap = false;

  constexpr bool has(Stage stage) const noexcept {
    return (stages & static_cast<std::uint8_t>(stage)) != 0;
  }
  constexpr std::uint8_t missing() const noexcept {
    return static_cast<std::uint8_t>(End | Publication | EncodeStart | EncodeEnd |
                                     ActualSubmit | Completion) &
           static_cast<std::uint8_t>(~stages);
  }
};

struct Stats {
  std::uint64_t ends = 0;
  std::uint64_t polls = 0;
  std::uint64_t unknownPolls = 0;
  std::uint64_t unknownStages = 0;
  std::uint64_t droppedSummaries = 0;
  std::uint64_t droppedEvidence = 0;
  std::uint64_t evictedEvidence = 0;
  std::uint64_t skippedEnds = 0;
  std::uint64_t stored = 0;
  std::uint64_t evictedOrdinaryEncode = 0;
};

class Ledger final {
 public:
  explicit Ledger(bool logging = true, std::uint64_t skipEnds = 0,
                  std::uint64_t stride = 1) noexcept
      : logging_(logging), skipEnds_(skipEnds), stride_(stride ? stride : 1) {}
  Ledger(const Ledger&) = delete;
  Ledger& operator=(const Ledger&) = delete;
  ~Ledger() { report(); }

  // A caller supplies the clock sample, keeping the production disabled path
  // free of clock reads and making tests deterministic.
  void end(Identity identity, std::uint32_t type, std::uint64_t sequence,
           MarkerWitness marker, std::uint64_t nowNs,
           std::uint64_t initializer = 0) noexcept {
    std::lock_guard lock(mutex_);
    ++stats_.ends;
    if (skipEnds_ != 0) {
      --skipEnds_;
      ++stats_.skippedEnds;
      return;
    }
    if (sampleOrdinal_++ % stride_ != 0) {
      ++stats_.skippedEnds;
      return;
    }
    auto* summary = find(identity);
    if (!summary) {
      if (size_ == kCapacity) {
        ++stats_.droppedSummaries;
        return;
      }
      summary = &summaries_[size_++];
      *summary = Summary{};
      summary->identity = identity;
      summary->type = type;
      ++stats_.stored;
    }
    summary->stages |= End;
    summary->endObservedNs = nowNs;
    summary->fenceInitializer = initializer;
    summary->fenceSequence = sequence;
    summary->marker = marker;
    joinEvidence(*summary);
  }

  void publication(std::uint64_t queue, std::uint64_t sequence,
                   MarkerWitness marker, std::uint64_t nowNs) noexcept {
    stage(queue, sequence, marker, Publication, nowNs);
  }

  void readyPublication(std::uint64_t queue, std::uint64_t sequence,
                        MarkerWitness marker, std::uint64_t readyDepthBefore,
                        std::uint64_t readyOldestSeqBefore,
                        std::uint64_t encodingSeqBefore,
                        std::uint64_t submittedFrontierBefore,
                        std::uint64_t completedFrontierBefore,
                        std::uint64_t nowNs) noexcept {
    std::lock_guard lock(mutex_);
    stageLocked(queue, sequence, marker, Publication, nowNs);
    for (std::size_t i = 0; i < size_; ++i) {
      auto& summary = summaries_[i];
      if (!matchesSummary(summary, queue, sequence, marker)) continue;
      setReadyFrontier(summary, readyDepthBefore, readyOldestSeqBefore,
                       encodingSeqBefore, submittedFrontierBefore,
                       completedFrontierBefore);
      attachPredecessor(summary);
    }
    for (auto& evidence : evidence_) {
      if (!matches(evidence, queue, sequence, marker)) continue;
      setReadyFrontier(evidence, readyDepthBefore, readyOldestSeqBefore,
                       encodingSeqBefore, submittedFrontierBefore,
                       completedFrontierBefore);
      attachPredecessor(evidence);
    }
  }

  void dequeue(std::uint64_t queue, std::uint64_t sequence,
               std::uint64_t nowNs) noexcept {
    std::lock_guard lock(mutex_);
    for (std::size_t i = 0; i < size_; ++i) {
      auto& summary = summaries_[i];
      if (summary.identity.queue == queue && summary.fenceSequence == sequence) {
        summary.dequeueNs = nowNs;
        attachPredecessor(summary);
      }
    }
    for (auto& evidence : evidence_) {
      if (evidence.used && evidence.queue == queue &&
          evidence.sequence == sequence) {
        evidence.dequeueNs = nowNs;
        attachPredecessor(evidence);
      }
    }
  }

  void ordinaryEncodeStart(std::uint64_t queue, std::uint64_t sequence,
                            std::uint64_t nowNs) noexcept {
    if (queue == 0 || sequence == 0) return;
    std::lock_guard lock(mutex_);
    ordinaryEncodeStartLocked(queue, sequence, nowNs);
  }

  void ordinaryEncodeStartLocked(std::uint64_t queue, std::uint64_t sequence,
                                  std::uint64_t nowNs) noexcept {
    OrdinaryEncode* destination = nullptr;
    for (auto& encode : ordinaryEncodes_) {
      if (encode.used && encode.queue == queue && encode.sequence == sequence) {
        destination = &encode;
        break;
      }
      if (!encode.used && !destination) destination = &encode;
    }
    if (!destination) {
      destination = &ordinaryEncodes_[0];
      for (auto& encode : ordinaryEncodes_)
        if (encode.ordinal < destination->ordinal) destination = &encode;
      ++stats_.evictedOrdinaryEncode;
    }
    *destination = OrdinaryEncode{true, queue, sequence, nowNs, 0,
                                  ++nextOrdinaryEncodeOrdinal_};
  }

  void ordinaryEncodeEnd(std::uint64_t queue, std::uint64_t sequence,
                          std::uint64_t nowNs) noexcept {
    if (queue == 0 || sequence == 0) return;
    std::lock_guard lock(mutex_);
    ordinaryEncodeEndLocked(queue, sequence, nowNs);
  }

  void ordinaryEncodeEndLocked(std::uint64_t queue, std::uint64_t sequence,
                                std::uint64_t nowNs) noexcept {
    for (auto& encode : ordinaryEncodes_) {
      if (!encode.used || encode.queue != queue ||
          encode.sequence != sequence) continue;
      encode.endNs = nowNs;
      encode.ordinal = ++nextOrdinaryEncodeOrdinal_;
      attachSuccessorPredecessorLocked(queue, sequence, encode);
      return;
    }
  }

  void actualSubmit(std::uint64_t queue, std::uint64_t sequence,
                    std::uint64_t nowNs) noexcept {
    stage(queue, sequence, {}, ActualSubmit, nowNs);
  }

  void encodeStart(std::uint64_t queue, std::uint64_t sequence,
                   std::uint64_t nowNs, bool ordinary = false) noexcept {
    std::lock_guard lock(mutex_);
    stageLocked(queue, sequence, {}, EncodeStart, nowNs);
    if (ordinary) ordinaryEncodeStartLocked(queue, sequence, nowNs);
  }

  void encodeEnd(std::uint64_t queue, std::uint64_t sequence,
                 std::uint64_t nowNs, bool ordinary = false) noexcept {
    std::lock_guard lock(mutex_);
    stageLocked(queue, sequence, {}, EncodeEnd, nowNs);
    if (ordinary) ordinaryEncodeEndLocked(queue, sequence, nowNs);
  }

  void completion(std::uint64_t queue, std::uint64_t sequence,
                  bool success, std::uint64_t nowNs) noexcept {
    if (!success) return;
    stage(queue, sequence, {}, Completion, nowNs);
  }

  void poll(Identity identity, int result, bool completed,
            std::optional<std::uint64_t> value, std::uint64_t nowNs) noexcept {
    std::lock_guard lock(mutex_);
    ++stats_.polls;
    auto* summary = find(identity);
    if (!summary) {
      ++stats_.unknownPolls;
      return;
    }
    ++summary->polls;
    if (result == 1 /* S_FALSE */) {
      ++summary->pendingPolls;
      if (summary->firstPendingNs == 0) summary->firstPendingNs = nowNs;
      summary->lastPendingNs = nowNs;
    } else if (result == 0 /* S_OK */) {
      ++summary->okPolls;
      const bool firstOk = summary->firstOkNs == 0;
      if (summary->firstOkNs == 0) summary->firstOkNs = nowNs;
      summary->lastOkNs = nowNs;
      // A successful read is useful only after the queue has supplied its
      // explicit completion stage. Never infer completion from S_OK alone.
      if (completed && summary->has(Completion) && value)
        summary->resultValue = value;
      // Preserve bounded evidence even when the app exits without running
      // unixlib static destructors. At most one live row per retained END.
      // Missing stages remain missing; S_OK never fabricates completion.
      if (firstOk && logging_) {
        const auto u = [](std::uint64_t v) {
          return static_cast<unsigned long long>(v);
        };
        util::logf(util::LogLevel::Info, "dxmt9-query-timeline-live",
            "queue=%llu query=%llu issue=%llu epoch=%llu type=%u missing=%u "
            "fence_seq=%llu end_observed_ns=%llu publication_ns=%llu "
            "encode_start_ns=%llu encode_end_ns=%llu submit_ns=%llu "
            "completion_ns=%llu first_ok_ns=%llu polls=%llu pending=%llu "
            "ready_depth_before=%llu encoding_seq_before=%llu",
            u(identity.queue), u(identity.query), u(identity.issue), u(identity.epoch),
            summary->type, unsigned(summary->missing()), u(summary->fenceSequence),
            u(summary->endObservedNs), u(summary->publicationNs),
            u(summary->encodeStartNs), u(summary->encodeEndNs),
            u(summary->submitNs), u(summary->completionNs), u(summary->firstOkNs),
            u(summary->polls), u(summary->pendingPolls),
            u(summary->readyDepthBefore), u(summary->encodingSeqBefore));
      }
    }
  }

  std::array<Summary, kCapacity> snapshotSummaries() const noexcept {
    std::lock_guard lock(mutex_);
    return summaries_;
  }
  std::size_t size() const noexcept {
    std::lock_guard lock(mutex_);
    return size_;
  }
  Stats stats() const noexcept {
    std::lock_guard lock(mutex_);
    return stats_;
  }

  void report() noexcept {
    std::lock_guard lock(mutex_);
    if (!logging_ || reported_) return;
    reported_ = true;
    util::logf(util::LogLevel::Info, "dxmt9-query-timeline-summary",
               "stored=%llu ends=%llu polls=%llu unknown_polls=%llu "
               "unknown_stages=%llu dropped_summaries=%llu dropped_evidence=%llu "
               "evicted_evidence=%llu evicted_ordinary_encode=%llu "
               "skipped_ends=%llu",
               (unsigned long long)stats_.stored, (unsigned long long)stats_.ends,
               (unsigned long long)stats_.polls,
               (unsigned long long)stats_.unknownPolls,
               (unsigned long long)stats_.unknownStages,
               (unsigned long long)stats_.droppedSummaries,
               (unsigned long long)stats_.droppedEvidence,
               (unsigned long long)stats_.evictedEvidence,
               (unsigned long long)stats_.evictedOrdinaryEncode,
               (unsigned long long)stats_.skippedEnds);
    for (std::size_t i = 0; i < size_; ++i) {
      const auto& s = summaries_[i];
      util::logf(util::LogLevel::Info, "dxmt9-query-timeline",
                 "query=%llu issue=%llu epoch=%llu queue=%llu type=%u "
                 "stages=%u missing=%u fence_seq=%llu marker_queue=%llu "
                 "marker_index=%u marker_generation=%llu marker_storage_generation=%llu "
                 "end_observed_ns=%llu fence_initializer=%llu publication_ns=%llu "
                 "encode_start_ns=%llu encode_end_ns=%llu submit_ns=%llu "
                 "completion_ns=%llu first_pending_ns=%llu "
                 "last_pending_ns=%llu first_ok_ns=%llu last_ok_ns=%llu "
                 "polls=%llu pending=%llu ok=%llu value_known=%u value=%llu "
                 "ready_known=%u ready_depth_before=%llu "
                 "ready_oldest_seq_before=%llu encoding_seq_before=%llu "
                 "submitted_frontier_before=%llu completed_frontier_before=%llu "
                 "dequeue_ns=%llu predecessor_encode_seq=%llu "
                 "predecessor_encode_start_ns=%llu predecessor_encode_end_ns=%llu "
                 "predecessor_encode_overlap=%u",
                 (unsigned long long)s.identity.query,
                 (unsigned long long)s.identity.issue,
                 (unsigned long long)s.identity.epoch,
                 (unsigned long long)s.identity.queue, s.type,
                 static_cast<unsigned>(s.stages),
                 static_cast<unsigned>(s.missing()),
                 (unsigned long long)s.fenceSequence,
                 (unsigned long long)s.marker.queue, s.marker.index,
                 (unsigned long long)s.marker.generation,
                 (unsigned long long)s.marker.storageGeneration,
                 (unsigned long long)s.endObservedNs,
                 (unsigned long long)s.fenceInitializer,
                 (unsigned long long)s.publicationNs,
                 (unsigned long long)s.encodeStartNs,
                 (unsigned long long)s.encodeEndNs,
                 (unsigned long long)s.submitNs,
                 (unsigned long long)s.completionNs,
                 (unsigned long long)s.firstPendingNs,
                 (unsigned long long)s.lastPendingNs,
                 (unsigned long long)s.firstOkNs,
                 (unsigned long long)s.lastOkNs,
                 (unsigned long long)s.polls,
                 (unsigned long long)s.pendingPolls,
                 (unsigned long long)s.okPolls,
                 static_cast<unsigned>(s.resultValue.has_value()),
                 (unsigned long long)s.resultValue.value_or(0),
                 static_cast<unsigned>(s.readyFrontierKnown),
                 (unsigned long long)s.readyDepthBefore,
                 (unsigned long long)s.readyOldestSeqBefore,
                 (unsigned long long)s.encodingSeqBefore,
                 (unsigned long long)s.submittedFrontierBefore,
                 (unsigned long long)s.completedFrontierBefore,
                 (unsigned long long)s.dequeueNs,
                 (unsigned long long)s.predecessorEncodeSeq,
                 (unsigned long long)s.predecessorEncodeStartNs,
                 (unsigned long long)s.predecessorEncodeEndNs,
                 static_cast<unsigned>(s.predecessorEncodeOverlap));
    }
  }

 private:
  struct Evidence {
    bool used = false;
    std::uint64_t queue = 0;
    std::uint64_t sequence = 0;
    MarkerWitness marker{};
    std::uint8_t stages = 0;
    std::uint64_t publicationNs = 0;
    std::uint64_t encodeStartNs = 0;
    std::uint64_t encodeEndNs = 0;
    std::uint64_t submitNs = 0;
    std::uint64_t completionNs = 0;
    std::uint64_t ordinal = 0;
    bool readyFrontierKnown = false;
    std::uint64_t readyDepthBefore = 0;
    std::uint64_t readyOldestSeqBefore = 0;
    std::uint64_t encodingSeqBefore = 0;
    std::uint64_t submittedFrontierBefore = 0;
    std::uint64_t completedFrontierBefore = 0;
    std::uint64_t dequeueNs = 0;
    std::uint64_t predecessorEncodeSeq = 0;
    std::uint64_t predecessorEncodeStartNs = 0;
    std::uint64_t predecessorEncodeEndNs = 0;
    bool predecessorEncodeOverlap = false;
  };

  struct OrdinaryEncode {
    bool used = false;
    std::uint64_t queue = 0;
    std::uint64_t sequence = 0;
    std::uint64_t startNs = 0;
    std::uint64_t endNs = 0;
    std::uint64_t ordinal = 0;
  };

  Summary* find(Identity identity) noexcept {
    for (std::size_t i = 0; i < size_; ++i)
      if (summaries_[i].identity == identity) return &summaries_[i];
    return nullptr;
  }
  const Summary* find(Identity identity) const noexcept {
    for (std::size_t i = 0; i < size_; ++i)
      if (summaries_[i].identity == identity) return &summaries_[i];
    return nullptr;
  }
  bool matches(const Evidence& evidence, std::uint64_t queue,
               std::uint64_t sequence, MarkerWitness marker) const noexcept {
    if (!evidence.used || evidence.queue != queue) return false;
    if (sequence != 0 && evidence.sequence == sequence) return true;
    return marker.valid() && evidence.marker.valid() && evidence.marker == marker;
  }
  bool matchesSummary(const Summary& summary, std::uint64_t queue,
                      std::uint64_t sequence,
                      MarkerWitness marker) const noexcept {
    if (summary.identity.queue != queue) return false;
    if (sequence != 0 && summary.fenceSequence == sequence) return true;
    return marker.valid() && summary.marker.valid() && summary.marker == marker;
  }
  template <typename T>
  static void setReadyFrontier(T& destination,
                               std::uint64_t readyDepthBefore,
                               std::uint64_t readyOldestSeqBefore,
                               std::uint64_t encodingSeqBefore,
                               std::uint64_t submittedFrontierBefore,
                               std::uint64_t completedFrontierBefore) noexcept {
    destination.readyFrontierKnown = true;
    destination.readyDepthBefore = readyDepthBefore;
    destination.readyOldestSeqBefore = readyOldestSeqBefore;
    destination.encodingSeqBefore = encodingSeqBefore;
    destination.submittedFrontierBefore = submittedFrontierBefore;
    destination.completedFrontierBefore = completedFrontierBefore;
  }
  template <typename T>
  static void setPredecessor(T& destination,
                             const OrdinaryEncode& predecessor) noexcept {
    destination.predecessorEncodeSeq = predecessor.sequence;
    destination.predecessorEncodeStartNs = predecessor.startNs;
    destination.predecessorEncodeEndNs = predecessor.endNs;
    destination.predecessorEncodeOverlap = true;
  }
  template <typename T>
  void attachPredecessor(T& destination,
                         const OrdinaryEncode* directPredecessor = nullptr) noexcept {
    const auto destinationSequence = [&] {
      if constexpr (std::is_same_v<T, Summary>) {
        return destination.fenceSequence;
      } else {
        return destination.sequence;
      }
    }();
    const auto destinationQueue = [&] {
      if constexpr (std::is_same_v<T, Summary>) {
        return destination.identity.queue;
      } else {
        return destination.queue;
      }
    }();
    if (!destination.readyFrontierKnown || destination.dequeueNs == 0 ||
        destination.predecessorEncodeOverlap ||
        destinationSequence == 0) return;
    const OrdinaryEncode* predecessor = directPredecessor;
    if (!predecessor) {
      for (const auto& encode : ordinaryEncodes_) {
        if (!encode.used || encode.queue != destinationQueue ||
            encode.sequence != destinationSequence - 1u || encode.endNs == 0 ||
            encode.startNs == 0) continue;
        predecessor = &encode;
        break;
      }
    }
    if (!predecessor || predecessor->startNs >= destination.dequeueNs ||
        predecessor->endNs <= destination.publicationNs) return;
    setPredecessor(destination, *predecessor);
  }
  void attachSuccessorPredecessorLocked(
      std::uint64_t queue, std::uint64_t sequence,
      const OrdinaryEncode& predecessor) noexcept {
    if (sequence == std::numeric_limits<std::uint64_t>::max()) return;
    const auto successor = sequence + 1u;
    for (std::size_t i = 0; i < size_; ++i) {
      auto& summary = summaries_[i];
      if (summary.identity.queue == queue && summary.fenceSequence == successor)
        attachPredecessor(summary, &predecessor);
    }
    for (auto& evidence : evidence_) {
      if (evidence.used && evidence.queue == queue &&
          evidence.sequence == successor)
        attachPredecessor(evidence, &predecessor);
    }
  }
  void joinEvidence(Summary& summary) noexcept {
    for (auto& evidence : evidence_) {
      if (!matches(evidence, summary.identity.queue, summary.fenceSequence,
                   summary.marker)) continue;
      summary.stages |= evidence.stages;
      if (summary.fenceSequence == 0) summary.fenceSequence = evidence.sequence;
      if (evidence.publicationNs) summary.publicationNs = evidence.publicationNs;
      if (evidence.encodeStartNs) summary.encodeStartNs = evidence.encodeStartNs;
      if (evidence.encodeEndNs) summary.encodeEndNs = evidence.encodeEndNs;
      if (evidence.submitNs) summary.submitNs = evidence.submitNs;
      if (evidence.completionNs) summary.completionNs = evidence.completionNs;
      if (evidence.readyFrontierKnown) {
        setReadyFrontier(summary, evidence.readyDepthBefore,
                         evidence.readyOldestSeqBefore,
                         evidence.encodingSeqBefore,
                         evidence.submittedFrontierBefore,
                         evidence.completedFrontierBefore);
      }
      if (evidence.dequeueNs) summary.dequeueNs = evidence.dequeueNs;
      if (evidence.predecessorEncodeOverlap) {
        setPredecessor(summary, OrdinaryEncode{
            true, summary.identity.queue, evidence.predecessorEncodeSeq,
            evidence.predecessorEncodeStartNs, evidence.predecessorEncodeEndNs,
            0});
      }
      evidence.ordinal = ++nextEvidenceOrdinal_;
    }
  }
  void stage(std::uint64_t queue, std::uint64_t sequence, MarkerWitness marker,
             Stage stage, std::uint64_t nowNs) noexcept {
    std::lock_guard lock(mutex_);
    stageLocked(queue, sequence, marker, stage, nowNs);
  }
  void stageLocked(std::uint64_t queue, std::uint64_t sequence,
                   MarkerWitness marker, Stage stage,
                   std::uint64_t nowNs) noexcept {
    for (std::size_t i = 0; i < size_; ++i) {
      auto& summary = summaries_[i];
      if (summary.identity.queue != queue) continue;
      if ((marker.valid() && summary.marker == marker) ||
          (!marker.valid() && sequence != 0 && summary.fenceSequence == sequence)) {
        if (sequence != 0) summary.fenceSequence = sequence;
        summary.stages |= stage;
        if (stage == Publication) summary.publicationNs = nowNs;
        if (stage == EncodeStart) summary.encodeStartNs = nowNs;
        if (stage == EncodeEnd) summary.encodeEndNs = nowNs;
        if (stage == ActualSubmit) summary.submitNs = nowNs;
        if (stage == Completion) summary.completionNs = nowNs;
      }
    }
    // Preserve one bounded evidence row even when a summary already matched:
    // a later END in the same completed chunk must still join every stage.
    for (auto& evidence : evidence_) {
      if (!evidence.used || evidence.queue != queue) continue;
      if ((sequence != 0 && evidence.sequence == sequence) ||
          (marker.valid() && evidence.marker.valid() && evidence.marker == marker)) {
        if (sequence != 0) evidence.sequence = sequence;
        if (marker.valid()) evidence.marker = marker;
        evidence.stages |= static_cast<std::uint8_t>(stage);
        if (stage == Publication) evidence.publicationNs = nowNs;
        if (stage == EncodeStart) evidence.encodeStartNs = nowNs;
        if (stage == EncodeEnd) evidence.encodeEndNs = nowNs;
        if (stage == ActualSubmit) evidence.submitNs = nowNs;
        if (stage == Completion) evidence.completionNs = nowNs;
        evidence.ordinal = ++nextEvidenceOrdinal_;
        return;
      }
    }
    // Several END records can share one chunk sequence. One actual queue edge
    // therefore settles every identity-qualified summary at that sequence.
    Evidence* destination = nullptr;
    for (auto& evidence : evidence_) {
      if (!evidence.used) {
        destination = &evidence;
        break;
      }
    }
    if (!destination) {
      destination = &evidence_[0];
      for (auto& evidence : evidence_)
        if (evidence.ordinal < destination->ordinal) destination = &evidence;
      ++stats_.evictedEvidence;
    }
    *destination = Evidence{true, queue, sequence, marker,
                            static_cast<std::uint8_t>(stage),
                            stage == Publication ? nowNs : 0,
                            stage == EncodeStart ? nowNs : 0,
                            stage == EncodeEnd ? nowNs : 0,
                            stage == ActualSubmit ? nowNs : 0,
                            stage == Completion ? nowNs : 0,
                            ++nextEvidenceOrdinal_};
  }

  mutable std::mutex mutex_;
  bool logging_ = true;
  bool reported_ = false;
  std::uint64_t skipEnds_ = 0;
  std::uint64_t stride_ = 1, sampleOrdinal_ = 0;
  std::uint64_t nextEvidenceOrdinal_ = 0;
  std::uint64_t nextOrdinaryEncodeOrdinal_ = 0;
  std::array<Summary, kCapacity> summaries_{};
  std::array<Evidence, kCapacity> evidence_{};
  std::array<OrdinaryEncode, kCapacity> ordinaryEncodes_{};
  std::size_t size_ = 0;
  Stats stats_{};
};

inline bool enabled() noexcept {
  static const bool value = [] {
    const char* setting = std::getenv("DXMT9_QUERY_OBSERVER");
    return setting && setting[0] && setting[0] != '0';
  }();
  return value;
}

inline std::uint64_t nowNs() noexcept {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<
      std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline std::uint64_t skipEndCount() noexcept {
  static const std::uint64_t value = [] {
    const char* setting = std::getenv("DXMT9_QUERY_OBSERVER_SKIP_ENDS");
    if (!setting || !setting[0] || setting[0] == '0') return std::uint64_t{0};
    char* end = nullptr;
    const auto parsed = std::strtoull(setting, &end, 10);
    return end == setting || *end != '\0' ? std::uint64_t{0}
                          : static_cast<std::uint64_t>(parsed);
  }();
  return value;
}

inline Ledger& ledger() {
  static const std::uint64_t stride = [] {
    const char* setting = std::getenv("DXMT9_QUERY_OBSERVER_END_STRIDE");
    if (!setting || !setting[0] || setting[0] == '-') return std::uint64_t{1};
    char* end = nullptr;
    const auto parsed = std::strtoull(setting, &end, 10);
    return end == setting || *end || !parsed ? std::uint64_t{1}
        : static_cast<std::uint64_t>(parsed);
  }();
  static Ledger value(true, skipEndCount(), stride);
  return value;
}

inline void end(Identity identity, std::uint32_t type, std::uint64_t sequence,
                MarkerWitness marker, std::uint64_t initializer = 0) noexcept {
  if (!enabled()) return;
  ledger().end(identity, type, sequence, marker, nowNs(), initializer);
}

inline void publication(std::uint64_t queue, std::uint64_t sequence) noexcept {
  if (!enabled()) return;
  ledger().publication(queue, sequence, {}, nowNs());
}

inline void publication(std::uint64_t queue, std::uint64_t sequence,
                        MarkerWitness marker) noexcept {
  if (!enabled()) return;
  ledger().publication(queue, sequence, marker, nowNs());
}

inline void readyPublication(std::uint64_t queue, std::uint64_t sequence,
                             MarkerWitness marker,
                             std::uint64_t readyDepthBefore,
                             std::uint64_t readyOldestSeqBefore,
                             std::uint64_t encodingSeqBefore,
                             std::uint64_t submittedFrontierBefore,
                             std::uint64_t completedFrontierBefore) noexcept {
  if (!enabled()) return;
  ledger().readyPublication(queue, sequence, marker, readyDepthBefore,
                            readyOldestSeqBefore, encodingSeqBefore,
                            submittedFrontierBefore, completedFrontierBefore,
                            nowNs());
}

inline void dequeue(std::uint64_t queue, std::uint64_t sequence) noexcept {
  if (!enabled()) return;
  ledger().dequeue(queue, sequence, nowNs());
}

inline void ordinaryEncodeStart(std::uint64_t queue,
                                 std::uint64_t sequence) noexcept {
  if (!enabled()) return;
  ledger().ordinaryEncodeStart(queue, sequence, nowNs());
}

inline void ordinaryEncodeEnd(std::uint64_t queue,
                               std::uint64_t sequence) noexcept {
  if (!enabled()) return;
  ledger().ordinaryEncodeEnd(queue, sequence, nowNs());
}

inline void actualSubmit(std::uint64_t queue, std::uint64_t sequence) noexcept {
  if (!enabled()) return;
  ledger().actualSubmit(queue, sequence, nowNs());
}

inline void encodeStart(std::uint64_t queue, std::uint64_t sequence) noexcept {
  if (!enabled()) return;
  ledger().encodeStart(queue, sequence, nowNs());
}

inline void encodeEnd(std::uint64_t queue, std::uint64_t sequence) noexcept {
  if (!enabled()) return;
  ledger().encodeEnd(queue, sequence, nowNs());
}

inline void completion(std::uint64_t queue, std::uint64_t sequence,
                       bool success = true) noexcept {
  if (!enabled()) return;
  ledger().completion(queue, sequence, success, nowNs());
}

inline void poll(Identity identity, int result, bool completed,
                 std::optional<std::uint64_t> value = {}) noexcept {
  if (!enabled()) return;
  ledger().poll(identity, result, completed, value, nowNs());
}

}  // namespace dxmt9::query::timeline_diag
