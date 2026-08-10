#include "../../../src/dxmt9/dxmt9_draw_encoder_internal.hpp"
#include "../../../src/dxmt9/dxmt9_encode_session.hpp"
#include "../../../src/dxmt9/dxmt9_post_encode_retirement.hpp"
#include "../../../src/dxmt9/render/encode_session_admission.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

using dxmt9::encoders::CompletionSpanAppendResult;
using dxmt9::encoders::EncodedCommandId;
using dxmt9::encoders::SessionCompletionAccumulator;
using dxmt9::core::metalqueue::QueueCompletionSource;
using dxmt9::core::metalqueue::QueueSubmissionRecord;
using dxmt9::core::metalqueue::PostEncodeCompletionLedger;
using dxmt9::core::metalqueue::PostEncodeReceiptResult;
using dxmt9::core::metalqueue::PostEncodeReceiptState;

std::string readFile(const std::filesystem::path& path) {
  std::ifstream input(path);
  check(input.good(), "source-contract input opens");
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

void encodedCommandIdIsLocatorFree() {
  static_assert(std::is_trivially_copyable_v<EncodedCommandId>);
  static_assert(std::is_standard_layout_v<EncodedCommandId>);
  static_assert(sizeof(EncodedCommandId) == 16u);

  const dxmt9::core::metalqueue::PublishedCommandRef published{
      .source = {
          .id = {.index = 7u, .generation = 11u},
          .storage = {.firstPage = 13u, .pageCount = 2u, .generation = 17u},
      },
      .seqId = 23u,
      .slotIndex = 29u,
      .commandIndex = 31u,
  };
  const auto encoded =
      dxmt9::encoders::encodedCommandIdAtSynchronousEncodeSeam(published);
  check(encoded.has_value() &&
            *encoded == EncodedCommandId{.seqId = 23u, .commandIndex = 31u},
        "encode seam drops source/storage/slot locators");

  auto invalid = published;
  invalid.source = {};
  check(!dxmt9::encoders::encodedCommandIdAtSynchronousEncodeSeam(invalid),
        "encode seam rejects an invalid source locator");
}

void singletonAndDenseRangeKeepCheckedSourceCount() {
  SessionCompletionAccumulator accumulator;
  check(accumulator.append(10u) == CompletionSpanAppendResult::Appended,
        "first source candidate appends");
  check(accumulator.append(11u, true) ==
            CompletionSpanAppendResult::Appended,
        "exact successor appends to a dense completion candidate");

  const auto span = accumulator.summary();
  check(span && span->firstSeqId() == 10u && span->lastSeqId() == 11u &&
            span->sourceCount() == 2u && span->tailHasPresent(),
        "summary checks explicit source count against endpoint width");
}

void invalidDuplicateAndTailPresentRejectWithoutMutation() {
  SessionCompletionAccumulator invalid;
  check(invalid.append(0u) ==
            CompletionSpanAppendResult::InvalidSequence &&
            invalid.empty(),
        "zero seqId cannot create completion authority");

  SessionCompletionAccumulator accumulator;
  check(accumulator.append(8u) == CompletionSpanAppendResult::Appended,
        "duplicate fixture head appends");
  check(accumulator.append(8u) ==
            CompletionSpanAppendResult::NotStrictlyIncreasing,
        "duplicate seqId is rejected");
  check(accumulator.append(7u) ==
            CompletionSpanAppendResult::NotStrictlyIncreasing,
        "reverse seqId is rejected");
  check(accumulator.append(10u) ==
            CompletionSpanAppendResult::NonContiguous,
        "numeric source gap is rejected");
  const auto unchanged = accumulator.summary();
  check(unchanged && unchanged->firstSeqId() == 8u &&
            unchanged->lastSeqId() == 8u && unchanged->sourceCount() == 1u,
        "ordered rejection leaves accumulator unchanged");

  SessionCompletionAccumulator presentTail;
  check(presentTail.append(20u, true) ==
            CompletionSpanAppendResult::Appended,
        "Present singleton seals the tail");
  check(presentTail.append(21u) ==
            CompletionSpanAppendResult::TailHasPresent,
        "no younger completion source follows Present");
}

void mergePreservesOrderPresentAndBounds() {
  SessionCompletionAccumulator head;
  SessionCompletionAccumulator tail;
  check(head.append(2u) == CompletionSpanAppendResult::Appended &&
            tail.append(3u) == CompletionSpanAppendResult::Appended &&
            tail.append(4u, true) == CompletionSpanAppendResult::Appended,
        "merge fixtures build from checked singleton appends");
  check(head.merge(tail) == CompletionSpanAppendResult::Appended,
        "strictly ordered accumulators merge");
  const auto merged = head.summary();
  check(merged && merged->firstSeqId() == 2u &&
            merged->lastSeqId() == 4u && merged->sourceCount() == 3u &&
            merged->tailHasPresent(),
        "merge keeps explicit count and tail Present");

  SessionCompletionAccumulator duplicateHead;
  SessionCompletionAccumulator duplicateTail;
  check(duplicateHead.append(4u) == CompletionSpanAppendResult::Appended &&
            duplicateTail.append(4u) == CompletionSpanAppendResult::Appended,
        "overlap fixtures build");
  check(duplicateHead.merge(duplicateTail) ==
            CompletionSpanAppendResult::NotStrictlyIncreasing,
        "merge rejects overlap or duplicate coverage");

  SessionCompletionAccumulator gapHead;
  SessionCompletionAccumulator gapTail;
  check(gapHead.append(4u) == CompletionSpanAppendResult::Appended &&
            gapTail.append(6u) == CompletionSpanAppendResult::Appended &&
            gapHead.merge(gapTail) ==
                CompletionSpanAppendResult::NonContiguous,
        "merge rejects a numeric source gap");

  SessionCompletionAccumulator sealedHead;
  SessionCompletionAccumulator youngerTail;
  check(sealedHead.append(12u, true) ==
            CompletionSpanAppendResult::Appended &&
            youngerTail.append(13u) == CompletionSpanAppendResult::Appended &&
            sealedHead.merge(youngerTail) ==
                CompletionSpanAppendResult::TailHasPresent,
        "merge rejects a source after a Present tail");

  SessionCompletionAccumulator bounded(/*maxSourceCount=*/2u);
  check(bounded.append(1u) == CompletionSpanAppendResult::Appended &&
            bounded.append(2u) == CompletionSpanAppendResult::Appended &&
            bounded.append(3u) == CompletionSpanAppendResult::CountOverflow,
        "bounded source-count overflow rejects without wrap");
}

void receiptLedgerRejectsDuplicateStaleAndAbaUse() {
  PostEncodeCompletionLedger ledger;
  const auto first = ledger.activate(1u, false);
  check(first.result == PostEncodeReceiptResult::Succeeded &&
            first.receipt.valid() && ledger.depth() == 1u &&
            ledger.peak() == 1u,
        "first post-encode receipt activates in bounded queue storage");
  check(ledger.activate(1u, false).result ==
            PostEncodeReceiptResult::Duplicate,
        "duplicate activation is rejected without growing the ledger");
  check(ledger.markSubmitted(first.receipt, false) ==
            PostEncodeReceiptResult::Succeeded &&
            ledger.markSubmitted(first.receipt, false) ==
                PostEncodeReceiptResult::WrongState,
        "receipt submit is exactly once");
  check(ledger.markCompleted(first.receipt, false) ==
            PostEncodeReceiptResult::Succeeded &&
            ledger.markCompleted(first.receipt, false) ==
                PostEncodeReceiptResult::WrongState,
        "receipt completion is exactly once");
  check(ledger.finishAndRelease(1u) ==
            PostEncodeReceiptResult::Succeeded &&
            ledger.depth() == 0u,
        "ordered finish releases the completed receipt");
  check(ledger.markSubmitted(first.receipt, false) ==
            PostEncodeReceiptResult::Stale,
        "released receipt cannot submit again");

  const std::uint64_t reusedSeq =
      1u + dxmt9::core::metalqueue::kMaxPostEncodeCompletionReceipts;
  const auto reused = ledger.activate(reusedSeq, false);
  check(reused.result == PostEncodeReceiptResult::Succeeded &&
            reused.receipt.slot == first.receipt.slot &&
            reused.receipt.generation != first.receipt.generation,
        "ring-slot reuse advances the ABA generation");
  check(ledger.markCompleted(first.receipt, false) ==
            PostEncodeReceiptResult::Stale &&
            ledger.matches(reused.receipt, PostEncodeReceiptState::Active,
                           false),
        "stale generation cannot affect the reused slot");

  PostEncodeCompletionLedger collision;
  const auto occupied = collision.activate(7u, false);
  check(occupied.result == PostEncodeReceiptResult::Succeeded &&
            collision.activate(
                         7u + dxmt9::core::metalqueue::
                                  kMaxPostEncodeCompletionReceipts,
                         false)
                    .result == PostEncodeReceiptResult::Capacity,
        "live ring collision fails deterministically at fixed capacity");
  check(collision.cancelBeforeActivationEffects(occupied.receipt) ==
            PostEncodeReceiptResult::Succeeded &&
            collision.cancelBeforeActivationEffects(occupied.receipt) ==
                PostEncodeReceiptResult::Stale,
        "only a pre-effect active receipt can roll back once");
}

enum class DeferredSuffixReplayPath : std::uint8_t {
  NaturalDrain,
  Join,
};

struct DeferredSuffixRetirementModel {
  using SessionAdmissionCandidate =
      dxmt9::render::SessionAdmissionCandidate;
  using SessionCapacityVector = dxmt9::render::SessionCapacityVector;

  static constexpr std::array<std::uint64_t, 2> kSeqIds{101u, 102u};
  static constexpr std::array<std::uint32_t, 2> kCommandCounts{3u, 1u};

  std::array<std::array<bool, 3>, 2> encodedCommands{};
  std::array<SessionAdmissionCandidate, 2> candidates{};
  std::array<SessionCapacityVector, 2> residencyCharges{};
  std::array<SessionCapacityVector, 2> workCharges{};
  std::array<dxmt9::core::metalqueue::PostEncodeCompletionReceipt, 2>
      receipts{};
  dxmt9::render::EncodeSessionAdmissionState admission{};
  SessionCapacityVector residency{};
  SessionCapacityVector encodedWork{};
  PostEncodeCompletionLedger ledger{};
  std::vector<std::uint64_t> detachOrder;
  std::vector<std::uint64_t> finishOrder;
  std::uint32_t payloadBorrows = 0;
  bool suffixComplete = false;
  bool effectsComplete = false;

  DeferredSuffixRetirementModel() {
    candidates[0].seqId = kSeqIds[0];
    candidates[0].reservationPages = 2u;
    candidates[0].residencyBytes.value = 128u;
    candidates[1].seqId = kSeqIds[1];
    candidates[1].reservationPages = 1u;
    candidates[1].residencyBytes.value = 64u;
    residencyCharges = {{
        {
            .sources = 1u,
            .pages = 2u,
            .bytes = 128u,
            .payloadBlocks = 1u,
            .readyEntries = 1u,
            .retentionEntries = 1u,
            .allocatorTickets = 1u,
        },
        {
            .sources = 1u,
            .pages = 1u,
            .bytes = 64u,
            .payloadBlocks = 1u,
            .readyEntries = 1u,
            .retentionEntries = 1u,
            .allocatorTickets = 1u,
        },
    }};
    workCharges = {{
        {.sources = 1u, .draws = 2u, .commandBuffers = 1u},
        {.sources = 1u, .draws = 1u, .commandBuffers = 1u},
    }};
    const auto totalResidency = dxmt9::render::addSessionCapacity(
        residencyCharges[0], residencyCharges[1]);
    const auto totalWork = dxmt9::render::addSessionCapacity(
        workCharges[0], workCharges[1]);
    check(totalResidency.has_value() && totalWork.has_value(),
          "deferred suffix fixture capacity sums are representable");
    residency = *totalResidency;
    encodedWork = *totalWork;
    admission.flags = dxmt9::render::EncodeSessionAdmissionValid;
    admission.sources = 2u;
    admission.residentSources = 2u;
    admission.pages = 3u;
    admission.draws = 3u;
    admission.commandBuffers = 2u;
    admission.residencyBytes.value = 192u;
  }

  bool encode(std::size_t sourceIndex,
              std::size_t commandIndex) noexcept {
    if (sourceIndex >= encodedCommands.size() ||
        commandIndex >= kCommandCounts[sourceIndex] ||
        encodedCommands[sourceIndex][commandIndex]) {
      return false;
    }
    encodedCommands[sourceIndex][commandIndex] = true;
    return true;
  }

  bool allCommandsEncoded() const noexcept {
    return encodedCommands[0][0] && encodedCommands[0][1] &&
        encodedCommands[0][2] && encodedCommands[1][0];
  }

  bool retirementEligible(std::size_t sourceIndex) const noexcept {
    if (sourceIndex >= candidates.size() || !effectsComplete ||
        payloadBorrows != 0u) {
      return false;
    }
    return sourceIndex != 0u || suffixComplete;
  }

  bool detach(std::size_t sourceIndex) {
    if (!retirementEligible(sourceIndex) ||
        sourceIndex != detachOrder.size()) {
      return false;
    }
    const auto activation = ledger.activate(kSeqIds[sourceIndex], false);
    if (activation.result != PostEncodeReceiptResult::Succeeded) {
      return false;
    }
    const auto remaining = dxmt9::render::subtractSessionCapacity(
        residency, residencyCharges[sourceIndex]);
    if (!remaining || !dxmt9::render::retireSessionAdmissionResidency(
                          admission, candidates[sourceIndex])) {
      return false;
    }
    residency = *remaining;
    receipts[sourceIndex] = activation.receipt;
    detachOrder.push_back(kSeqIds[sourceIndex]);
    return true;
  }

  bool finish(std::size_t sourceIndex) {
    if (sourceIndex != finishOrder.size() ||
        sourceIndex >= receipts.size() ||
        ledger.markSubmitted(receipts[sourceIndex], false) !=
            PostEncodeReceiptResult::Succeeded ||
        ledger.markCompleted(receipts[sourceIndex], false) !=
            PostEncodeReceiptResult::Succeeded ||
        ledger.finishAndRelease(kSeqIds[sourceIndex]) !=
            PostEncodeReceiptResult::Succeeded) {
      return false;
    }
    const auto remaining = dxmt9::render::subtractSessionCapacity(
        encodedWork, workCharges[sourceIndex]);
    if (!remaining) {
      return false;
    }
    encodedWork = *remaining;
    finishOrder.push_back(kSeqIds[sourceIndex]);
    return true;
  }
};

void deferredSuffixRetirementWaitsForAllEffectsAndConservesState() {
  constexpr std::array paths{
      DeferredSuffixReplayPath::NaturalDrain,
      DeferredSuffixReplayPath::Join,
  };
  for (const auto path : paths) {
    DeferredSuffixRetirementModel model;
    check(model.encode(0u, 0u),
          "held source encodes its current A prefix once");
    ++model.payloadBorrows;
    check(!model.retirementEligible(0u) && !model.detach(0u) &&
              model.ledger.depth() == 0u &&
              model.admission.residentSources == 2u,
          "prefix-only current source cannot activate or detach");

    if (path == DeferredSuffixReplayPath::Join) {
      check(model.encode(1u, 0u) && model.encode(0u, 1u) &&
                model.encode(0u, 2u),
            "join encodes successor head before the complete current suffix");
    } else {
      check(model.encode(0u, 1u) && model.encode(0u, 2u) &&
                model.encode(1u, 0u),
            "natural drain consumes the current suffix before successor");
    }
    model.suffixComplete = true;
    check(!model.retirementEligible(0u) && !model.detach(0u),
          "final suffix command still cannot detach through a live borrow");
    --model.payloadBorrows;
    check(!model.retirementEligible(0u) && !model.detach(0u),
          "borrow release alone cannot retire before all effects fold");
    model.effectsComplete = model.allCommandsEncoded();

    check(model.allCommandsEncoded() &&
              !model.encode(0u, 0u) && !model.encode(0u, 1u) &&
              !model.encode(0u, 2u) && !model.encode(1u, 0u),
          "prefix, successor head, and suffix cover each command once");
    check(!model.detach(1u),
          "successor receipt cannot overtake the current source");
    check(model.detach(0u) && !model.detach(0u) &&
              model.detach(1u) && !model.detach(1u) &&
              model.detachOrder ==
                  std::vector<std::uint64_t>({101u, 102u}) &&
              model.ledger.depth() == 2u,
          "receipt activation and detach are FIFO exactly once");
    check(model.residency ==
                  dxmt9::render::SessionCapacityVector{} &&
              model.admission.residentSources == 0u &&
              model.admission.pages == 0u &&
              model.admission.residencyBytes.value == 0u &&
              model.encodedWork.sources == 2u &&
              model.encodedWork.draws == 3u,
          "detach releases residency while encoded work remains charged");

    check(!model.finish(1u),
          "successor completion cannot overtake current completion");
    check(model.finish(0u) && !model.finish(0u) &&
              model.finish(1u) && !model.finish(1u) &&
              model.finishOrder ==
                  std::vector<std::uint64_t>({101u, 102u}) &&
              model.ledger.depth() == 0u &&
              model.encodedWork ==
                  dxmt9::render::SessionCapacityVector{},
          "FIFO completion settles receipts and encoded work exactly once");
  }
}

QueueCompletionSource completionSource(std::uint64_t seqId,
                                       bool hasPresent = false) {
  return QueueCompletionSource{
      .source = {
          .id = {
              .index = static_cast<std::uint32_t>(seqId),
              .generation = seqId,
          },
          .storage = {
              .firstPage = static_cast<std::uint32_t>(seqId),
              .pageCount = 1u,
              .generation = seqId,
          },
      },
      .slotIndex = static_cast<std::size_t>(seqId),
      .seqId = seqId,
      .hasPresent = hasPresent,
      .commandBegin = 0u,
      .commandCount = 0u,
  };
}

void queueSealedShadowMatchesLegacyListDomain() {
  static_assert(
      !std::is_default_constructible_v<
          dxmt9::encoders::EncodedCompletionSpan>);

  QueueSubmissionRecord empty;
  check(empty.assignFixedCompletionSources(
            std::span<const QueueCompletionSource>{}) &&
            empty.explicitCompletionSourceSpan().empty() &&
            !empty.completionSpanShadow.has_value() &&
            empty.completionSpanShadowMatchesSources(),
        "empty legacy list remains the canonical empty shadow state");

  const std::array singleton{completionSource(1u, true)};
  QueueSubmissionRecord record;
  check(record.assignFixedCompletionSources(singleton) &&
            record.completionSpanShadow.has_value() &&
            record.completionSpanShadow->firstSeqId() == 1u &&
            record.completionSpanShadow->lastSeqId() == 1u &&
            record.completionSpanShadow->sourceCount() == 1u &&
            record.completionSpanShadow->tailHasPresent() &&
            record.completionSpanShadowMatchesSources(),
        "queue authority seals a singleton completion shadow");

  std::array<QueueCompletionSource, 31> capSources{};
  for (std::size_t i = 0; i < capSources.size(); ++i) {
    capSources[i] = completionSource(i + 1u, i + 1u == capSources.size());
  }
  check(record.assignFixedCompletionSources(capSources) &&
            record.completionSpanShadow->sourceCount() == capSources.size() &&
            record.completionSpanShadow->firstSeqId() == 1u &&
            record.completionSpanShadow->lastSeqId() == 31u &&
            record.completionSpanShadow->tailHasPresent(),
        "30+1 completion list seals without changing its zero-range domain");

  auto uncheckedPayloadDomain = completionSource(41u);
  uncheckedPayloadDomain.slotIndex = std::numeric_limits<std::size_t>::max();
  uncheckedPayloadDomain.commandBegin =
      std::numeric_limits<std::size_t>::max();
  uncheckedPayloadDomain.commandCount =
      std::numeric_limits<std::size_t>::max();
  const std::array uncheckedPayloadSources{uncheckedPayloadDomain};
  check(record.assignFixedCompletionSources(uncheckedPayloadSources) &&
            record.completionSpanShadowMatchesSources(),
        "shadow preserves the old list domain for unchecked slot/range bounds");

  auto gapSources = capSources;
  ++gapSources[10].seqId;
  check(!record.assignFixedCompletionSources(gapSources),
        "queue shadow rejects the same dense-sequence violation as old list");
  auto earlyPresent = capSources;
  earlyPresent[0].hasPresent = true;
  check(!record.assignFixedCompletionSources(earlyPresent),
        "queue shadow rejects the same non-tail Present as old list");
  auto invalidLocator = capSources;
  invalidLocator[0].source = {};
  check(!record.assignFixedCompletionSources(invalidLocator),
        "queue shadow rejects the same invalid locator as old list");

  check(record.assignFixedCompletionSources(capSources),
        "shadow mismatch fixture restores valid list");
  record.fixedCompletionSources.entries[4].source = {};
  record.fixedCompletionSources.entries[4].slotIndex =
      std::numeric_limits<std::size_t>::max();
  record.fixedCompletionSources.entries[4].commandBegin =
      std::numeric_limits<std::size_t>::max();
  record.fixedCompletionSources.entries[4].commandCount =
      std::numeric_limits<std::size_t>::max();
  check(record.completionSpanShadowMatchesSources(),
        "shadow validation ignores locator, slot, and command range fields");

  check(record.assignFixedCompletionSources(capSources),
        "projection mismatch fixture restores valid list");
  ++record.fixedCompletionSources.entries[4].seqId;
  check(!record.completionSpanShadowMatchesSources(),
        "mutated legacy list cannot validate against its sealed shadow");

  dxmt9::core::metalqueue::QueueLifecycleController::PendingCompletion pending;
  check(pending.assignFixedCompletionSources(capSources) &&
            pending.completionSpanShadow == record.completionSpanShadow,
        "pending handoff creates an equivalent sealed value independently");
  check(pending.completionSpanShadowMatchesSources(),
        "pending completion list and shadow remain equivalent");
}

void mixedRetiredAndLocatorSourcesStayDenseAndOrdered() {
  std::array<QueueCompletionSource, 64> sources{};
  for (std::size_t i = 0; i < sources.size(); ++i) {
    sources[i] = completionSource(i + 1u, i + 1u == sources.size());
    if ((i & 1u) != 0u) {
      sources[i].source = {};
      sources[i].receipt = {
          .seqId = i + 1u,
          .generation = i + 11u,
          .slot = static_cast<std::uint32_t>(i),
      };
    }
  }
  QueueSubmissionRecord record;
  check(record.assignFixedCompletionSources(sources) &&
            record.explicitCompletionSourceSpan().size() == sources.size() &&
            record.completionSpanShadowMatchesSources(),
        "mixed locator and receipt completion sources share one bounded list");
  for (std::size_t i = 0; i < sources.size(); ++i) {
    const auto& stored = record.explicitCompletionSourceSpan()[i];
    check(stored.seqId == i + 1u &&
              (((i & 1u) == 0u && stored.locatorBacked()) ||
               ((i & 1u) != 0u && stored.receiptBacked())),
          "mixed completion identity preserves source order and kind");
  }
  check(record.explicitCompletionSourceSpan().back().hasPresent,
        "legacy Present tail remains ordered after retired predecessors");
}

void borrowedViewAsyncEscapeSourceContractIsPinned(
    const std::filesystem::path& sourceRoot) {
  const std::string queue =
      readFile(sourceRoot / "src/dxmt9/dxmt9_queue.hpp");
  const std::string encoder =
      readFile(sourceRoot / "src/dxmt9/dxmt9_draw_encoder.mm");
  const std::string queueImplementation =
      readFile(sourceRoot / "src/dxmt9/dxmt9_queue.cpp");
  const std::string sessionCoordinator = readFile(
      sourceRoot / "src/dxmt9/dxmt9_command_queue_cpu_ready_session.cpp");

  const auto submissionBegin = queue.find("struct QueueSubmissionRecord");
  const auto submissionEnd = queue.find(
      "bool hasExactRedundantFixedCompletionSources", submissionBegin);
  const auto pendingBegin = queue.find("struct PendingCompletion");
  const auto pendingEnd = queue.find("void notifyPendingCompletionStop",
                                     pendingBegin);
  const auto storageBegin = encoder.find("struct EncodeChunkSessionStorage");
  const auto storageEnd = encoder.find("EncodeChunkSessionStorage* createStorage",
                                       storageBegin);
  const auto passBegin = encoder.find("struct PassState");
  const auto passEnd = encoder.find("struct BindingState", passBegin);
  check(submissionBegin != std::string::npos &&
            submissionEnd != std::string::npos &&
            pendingBegin != std::string::npos &&
            pendingEnd != std::string::npos &&
            storageBegin != std::string::npos &&
            storageEnd != std::string::npos &&
            passBegin != std::string::npos && passEnd != std::string::npos,
        "asynchronous owner bodies are locatable");

  const std::string_view submissionBody(queue.data() + submissionBegin,
                                        submissionEnd - submissionBegin);
  const std::string_view pendingBody(queue.data() + pendingBegin,
                                     pendingEnd - pendingBegin);
  const std::string_view storageBody(encoder.data() + storageBegin,
                                     storageEnd - storageBegin);
  const std::string_view passBody(encoder.data() + passBegin,
                                  passEnd - passBegin);
  check(submissionBody.find("SourcePayloadView") == std::string_view::npos &&
            pendingBody.find("SourcePayloadView") == std::string_view::npos &&
            storageBody.find("SourcePayloadView") == std::string_view::npos &&
            passBody.find("SourcePayloadView") == std::string_view::npos,
        "borrowed SourcePayloadView does not enter async owner storage");
  check(submissionBody.find("EncodedCommandId sourceCommand") !=
            std::string_view::npos &&
            submissionBody.find("PublishedCommandRef sourceCommand") ==
                std::string_view::npos &&
            passBody.find("PublishedCommandRef pendingClearCommand") !=
                std::string_view::npos,
        "GPU attribution is locator-free while pending clear stays audited");

  const auto firstAttachmentBegin = encoder.find(
      "RenderEncoderGpuAttachment makeRenderEncoderGpuAttachment(");
  const auto attachmentBegin = encoder.find(
      "RenderEncoderGpuAttachment makeRenderEncoderGpuAttachment(",
      firstAttachmentBegin + 1u);
  const auto attachmentEnd = encoder.find(
      "void recordRenderEncoderGpuAttachment", attachmentBegin);
  check(firstAttachmentBegin != std::string::npos &&
            attachmentBegin != std::string::npos &&
            attachmentEnd != std::string::npos,
        "GPU sample attachment production body is locatable");
  const std::string_view attachmentBody(
      encoder.data() + attachmentBegin, attachmentEnd - attachmentBegin);
  const auto conversion = attachmentBody.find(
      "encodedCommandIdAtSynchronousEncodeSeam(command)");
  const auto cursorConsumption = attachmentBody.find(
      "renderEncoderGpuSampleCursor++");
  check(conversion != std::string_view::npos &&
            cursorConsumption != std::string_view::npos &&
            conversion < cursorConsumption &&
            attachmentBody.find("if (!sourceCommand)") !=
                std::string_view::npos &&
            attachmentBody.find(".sourceCommand = *sourceCommand") !=
                std::string_view::npos &&
            attachmentBody.find("value_or") == std::string_view::npos,
        "invalid encoded attribution returns inactive before consuming GPU "
        "sample indices");

  const auto completionBegin = queueImplementation.find(
      "bool QueueLifecycleController::processOnePendingCompletion");
  const auto completionEnd = queueImplementation.find(
      "void QueueLifecycleController::enqueuePendingCompletionForTest",
      completionBegin);
  check(completionBegin != std::string::npos &&
            completionEnd != std::string::npos,
        "pending completion production body is locatable");
  const std::string_view completionBody(
      queueImplementation.data() + completionBegin,
      completionEnd - completionBegin);
  check(completionBody.find("completionSpanShadowMatchesSources()") !=
                std::string_view::npos &&
            completionBody.find(
                "const auto completionSources = "
                "pending.explicitCompletionSourceSpan();") !=
                std::string_view::npos,
        "pending completion validates the shadow then names the old list");
  check(completionBody.find("cpuReadyTape->completeAll") !=
                std::string_view::npos &&
            completionBody.find("appendCompletionSourcesToQueues") !=
                std::string_view::npos &&
            completionBody.find("completionSpanShadow->") ==
                std::string_view::npos &&
            completionBody.find("completionSpanShadow.value") ==
                std::string_view::npos,
        "Tape completion and waterline publication consume only the old list");

  const auto retireBegin = queueImplementation.find(
      "PostEncodeReceiptResult "
      "QueueLifecycleController::retireEncodedSourcePayload");
  const auto retireEnd = queueImplementation.find(
      "size_t QueueLifecycleController::retainEncodedSourcesForPendingTail",
      retireBegin);
  const auto tryRetireBegin = sessionCoordinator.find(
      "const auto tryRetireEncodedSource =");
  const auto tryRetireEnd = sessionCoordinator.find(
      "for (std::size_t i = 0; i < count; ++i)", tryRetireBegin);
  check(retireBegin != std::string::npos && retireEnd != std::string::npos &&
            tryRetireBegin != std::string::npos &&
            tryRetireEnd != std::string::npos,
        "receipt activation and coordinator retirement bodies are locatable");
  const std::string_view retireBody(
      queueImplementation.data() + retireBegin, retireEnd - retireBegin);
  const std::string_view tryRetireBody(
      sessionCoordinator.data() + tryRetireBegin,
      tryRetireEnd - tryRetireBegin);
  check(retireBody.find("countPostEncodeReceiptFailure") !=
                std::string_view::npos &&
            tryRetireBody.find("countPostEncodeReceiptFailure") ==
                std::string_view::npos &&
            tryRetireBody.find("ReceiptCapacity") != std::string_view::npos,
        "receipt activation owns failure counting once while the coordinator "
        "keeps capacity ineligibility attribution");
  const auto retiringControl = retireBody.find(
      "control.state = ChunkSlot::State::Retiring;");
  const auto unlockForDestruction = retireBody.find("lock.unlock();");
  const auto retiringFinishCheck = retireBody.find(
      "control.state != ChunkSlot::State::Retiring");
  check(retiringControl != std::string_view::npos &&
            unlockForDestruction != std::string_view::npos &&
            retiringFinishCheck != std::string_view::npos &&
            retiringControl < unlockForDestruction &&
            unlockForDestruction < retiringFinishCheck,
        "two-phase payload destruction publishes an explicit non-reusable "
        "control state before unlocking and requires it after relock");
}

void defaultMetalFactorySourceContractIsPinned(
    const std::filesystem::path& sourceRoot) {
  const std::string wrapper =
      readFile(sourceRoot / "src/winemetal/Metal.hpp");
  const std::string privateApi = readFile(
      sourceRoot / "src/winemetal/unix/winemetal_private_api.mm");
  const std::string commandQueue =
      readFile(sourceRoot / "src/dxmt9/dxmt9_command_queue.cpp");

  const auto wrapperBegin = wrapper.find("class CommandQueue : public Object");
  const auto wrapperEnd = wrapper.find("class Function", wrapperBegin);
  check(wrapperBegin != std::string::npos && wrapperEnd != std::string::npos,
        "WMT CommandQueue wrapper body is locatable");
  const std::string_view wrapperBody(wrapper.data() + wrapperBegin,
                                     wrapperEnd - wrapperBegin);
  check(wrapperBody.find(
            "MTLCommandQueue_commandBuffer(handle)") !=
            std::string_view::npos &&
            wrapperBody.find("UnretainedReferences") == std::string_view::npos,
        "WMT default factory routes only to the ordinary commandBuffer API");

  const auto apiBegin = privateApi.find(
      "extern \"C\" obj_handle_t MTLCommandQueue_commandBuffer");
  const auto apiEnd = privateApi.find("extern \"C\"", apiBegin + 1u);
  check(apiBegin != std::string::npos && apiEnd != std::string::npos,
        "private command-buffer factory body is locatable");
  const std::string_view apiBody(privateApi.data() + apiBegin,
                                apiEnd - apiBegin);
  check(apiBody.find("[(id<MTLCommandQueue>)queue commandBuffer]") !=
                std::string_view::npos &&
            apiBody.find("commandBufferWithUnretainedReferences") ==
                std::string_view::npos,
        "private API uses Apple's retained default selector");

  const auto queueBegin = commandQueue.find(
      "WMT::Reference<WMT::CommandBuffer> CommandQueue::newCommandBuffer()");
  const auto queueEnd = commandQueue.find(
      "CommandQueue::TransientBufferSlice", queueBegin);
  check(queueBegin != std::string::npos && queueEnd != std::string::npos,
        "production newCommandBuffer body is locatable");
  const std::string_view queueBody(commandQueue.data() + queueBegin,
                                  queueEnd - queueBegin);
  check(queueBody.find("queue_.commandBuffer()") != std::string_view::npos &&
            queueBody.find("UnretainedReferences") == std::string_view::npos,
        "production queue uses the WMT retained-default factory seam");
}

void invalidLeaseSnapshotFailStopsBeforeCapacityWait(
    const std::filesystem::path& sourceRoot) {
  const std::string coordinator = readFile(
      sourceRoot /
      "src/dxmt9/dxmt9_command_queue_cpu_ready_session.cpp");
  const auto invalidFlag = coordinator.find(
      "bool invalidCapacitySnapshot = false;");
  const auto invalidBranch = coordinator.find(
      "if (!snapshot.valid || !unavailable.has_value())", invalidFlag);
  const auto zeroSelection = coordinator.find(
      "if (count == 0)", invalidBranch);
  const auto failStop = coordinator.find(
      "if (invalidCapacitySnapshot)", zeroSelection);
  const auto capacityWait = coordinator.find(
      "if (leaseDenied && !pendingRecord.has_value())", zeroSelection);
  check(invalidFlag != std::string::npos &&
            invalidBranch != std::string::npos &&
            zeroSelection != std::string::npos &&
            failStop != std::string::npos &&
            capacityWait != std::string::npos && failStop < capacityWait,
        "invalid capacity snapshot fail-stop precedes the transient waiter");
  const std::string_view failStopBody(
      coordinator.data() + failStop, capacityWait - failStop);
  check(failStopBody.find("queueLifecycle_.poisonTapeFailure()") !=
                std::string_view::npos &&
            failStopBody.find("return;") != std::string_view::npos,
        "invalid snapshot poisons and returns without entering the capacity "
        "generation wait");
}

void emptySessionLifetimeIsPinned() {
  dxmt9::core::metalqueue::QueueSubmissionRecord record;
  auto session = dxmt9::encoders::makeEncodeChunkSession();
  check(dxmt9::encoders::encodeChunkSessionSources(*session).empty() &&
            !dxmt9::encoders::encodeChunkSessionHasDeferredSubmissionPayload(
                *session),
        "fresh session is empty before retention probe");
  check(dxmt9::encoders::retainEncodeChunkSessionUntilSubmissionComplete(
            std::move(session), record) &&
            record.retainedPayloads.size() == 1u,
        "retention helper can own an otherwise empty session");

  std::weak_ptr<void> retainedSession = record.retainedPayloads.front();
  check(!retainedSession.expired(),
        "submission record retains empty session owner");
  record.retainedPayloads.clear();
  check(retainedSession.expired(),
        "dropping submission retention runs the empty-session owner deleter");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    check(argc == 2, "source root argument is required");
    encodedCommandIdIsLocatorFree();
    singletonAndDenseRangeKeepCheckedSourceCount();
    invalidDuplicateAndTailPresentRejectWithoutMutation();
    mergePreservesOrderPresentAndBounds();
    receiptLedgerRejectsDuplicateStaleAndAbaUse();
    deferredSuffixRetirementWaitsForAllEffectsAndConservesState();
    queueSealedShadowMatchesLegacyListDomain();
    mixedRetiredAndLocatorSourcesStayDenseAndOrdered();
    defaultMetalFactorySourceContractIsPinned(argv[1]);
    invalidLeaseSnapshotFailStopsBeforeCapacityWait(argv[1]);
    borrowedViewAsyncEscapeSourceContractIsPinned(argv[1]);
    emptySessionLifetimeIsPinned();
  } catch (const std::exception& error) {
    std::cerr << "post-encode payload retirement spec failed: "
              << error.what() << '\n';
    return 1;
  }
  return 0;
}
