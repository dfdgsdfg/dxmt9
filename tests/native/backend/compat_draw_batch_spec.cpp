// Compatibility-lane draw-run island batching (companion to R-BACK-2.102).
//
// What this pins: the ordinary replay sink used to call the queue once per
// draw -- one `CommandQueue::mutex_` acquisition, one command header, one
// DrawRunRecord and one FlatDrawStateRecord for every D3D9 draw record that a
// Direct final-slot lease did not own. `compatibilityDrawBatchAdmission` is the
// one decision that replaces it, and the two properties it must never lose are
// (1) a run folds only draws whose canonical state, uniforms and bindings are
// bit-identical, so `A -> B -> A` retains three runs, and (2) every draw the
// sink accepts is published exactly once, in source order. Runs may share one
// source-local publication transaction.
//
// Everything here is pure and value-only: no queue, no device, no Metal, no
// Wine. The stream model below is the production cut discipline written out --
// `replayResolvedChunk` cuts before every record that is not a batchable draw,
// and `submitCompatibilityReplayDrawBatched` cuts on the admission verdict.

#include "dxmt9/core_compat_draw_batch.hpp"

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

using namespace dxmt9::core;

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
  if (condition) return;
  std::fprintf(stderr, "FAIL: %s\n", message.c_str());
  ++failures;
}

static_assert(std::is_trivially_copyable_v<CompatibilityDrawBatchIdentity>);
static_assert(std::is_standard_layout_v<CompatibilityDrawBatchIdentity>);
static_assert(std::is_trivially_copyable_v<CompatibilityDrawBatchDecision>);

// Two identities that differ in exactly one dimension each, so a comparison
// that silently dropped a field would still be caught.
CompatibilityDrawBatchIdentity identityFor(unsigned stateClass) {
  CompatibilityDrawBatchIdentity identity{};
  identity.generations[static_cast<std::size_t>(
      CompatibilityDrawBatchGeneration::StableState)] = 7;
  identity.generations[static_cast<std::size_t>(
      CompatibilityDrawBatchGeneration::Uniform)] = 11;
  identity.streamBuffers[0] = 0x1000;
  identity.streamStrides[0] = 32;
  identity.streamMask = 1;
  identity.indexBuffer = 0x2000;
  identity.indexed = true;
  identity.indexBufferValid = true;
  identity.alphaTestStateValid = true;
  switch (stateClass) {
  case 0:
    break;
  case 1:
    // A different uniform generation: same bindings, different constants.
    identity.generations[static_cast<std::size_t>(
        CompatibilityDrawBatchGeneration::StableState)] = 8;
    identity.generations[static_cast<std::size_t>(
        CompatibilityDrawBatchGeneration::Uniform)] = 12;
    break;
  case 2:
    // Same generations, different stream binding. This is the case a
    // generation-only identity would wrongly fold: the binding-agnostic draw
    // state cache deliberately ignores bindings.
    identity.streamOffsets[0] = 64;
    break;
  case 3:
    // Same generations and bindings, different live stride on an unbound
    // stream -- what refreshShaderLayoutExtraStreamStrides reads.
    identity.streamStrides[3] = 16;
    break;
  default:
    identity.alphaTestRef = stateClass;
    break;
  }
  return identity;
}

// ---------------------------------------------------------------------------
// 1. Exhaustive truth table over the predicate itself.

void admissionTruthTable() {
  const auto a = identityFor(0);
  const auto b = identityFor(1);
  constexpr std::uint32_t kMax = 4;

  for (const bool batchable : {false, true}) {
    for (std::uint32_t open = 0; open <= kMax + 1u; ++open) {
      for (const bool same : {false, true}) {
        const auto& incoming = same ? a : b;
        const auto decision = compatibilityDrawBatchAdmission(
            batchable, open, a, incoming, kMax);
        const std::string label =
            "batchable=" + std::to_string(batchable) +
            " open=" + std::to_string(open) + " same=" + std::to_string(same);

        if (!batchable) {
          check(decision.admission ==
                    CompatibilityDrawBatchAdmission::Unbatchable,
                label + ": a non-batchable draw is never admitted to a run");
          check(decision.cut ==
                    (open != 0u ? CompatibilityDrawBatchCut::Unbatchable
                                : CompatibilityDrawBatchCut::None),
                label + ": an unbatchable draw cuts only an open run");
          continue;
        }
        if (open == 0u) {
          check(decision.admission == CompatibilityDrawBatchAdmission::Start &&
                    decision.cut == CompatibilityDrawBatchCut::None,
                label + ": no open run means Start with nothing published");
          continue;
        }
        if (!same) {
          check(decision.admission ==
                        CompatibilityDrawBatchAdmission::FlushAndStart &&
                    decision.cut == CompatibilityDrawBatchCut::Identity,
                label + ": a differing identity cuts on Identity");
          continue;
        }
        if (open >= kMax) {
          check(decision.admission ==
                        CompatibilityDrawBatchAdmission::FlushAndStart &&
                    decision.cut == CompatibilityDrawBatchCut::Capacity,
                label + ": a full run cuts on Capacity, not Identity");
          continue;
        }
        check(decision.admission == CompatibilityDrawBatchAdmission::Extend &&
                  decision.cut == CompatibilityDrawBatchCut::None,
              label + ": an identical in-budget draw extends silently");
      }
    }
  }

  // A zero ceiling must degenerate to the pre-batching lane rather than read as
  // "unbounded".
  const auto degenerate =
      compatibilityDrawBatchAdmission(true, 1, a, a, /*maxDraws=*/0);
  check(degenerate.admission == CompatibilityDrawBatchAdmission::FlushAndStart &&
            degenerate.cut == CompatibilityDrawBatchCut::Capacity,
        "a zero draw ceiling publishes every draw on its own");

  check(classifyCompatibilityDrawBatchRecord({
            .mutatesDeviceState = true,
            .referencesResources = false,
            .barrier = false,
        }) == CompatibilityDrawBatchRecordClass::PureProjection,
        "a resource-free constant projection may remain in an open island");
  check(classifyCompatibilityDrawBatchRecord({
            .mutatesDeviceState = true,
            .referencesResources = true,
        }) == CompatibilityDrawBatchRecordClass::ObservableEffect,
        "a resource-bearing StateApply always cuts an open island");
  check(classifyCompatibilityDrawBatchRecord({}) ==
            CompatibilityDrawBatchRecordClass::ObservableEffect,
        "an unknown record is fail-closed and cuts an open island");
  check(classifyCompatibilityDrawBatchRecord({
            .batchableDraw = true,
            .stateBlockRecording = true,
        }) == CompatibilityDrawBatchRecordClass::ObservableEffect,
        "state-block recording wins over contradictory draw eligibility");

  // Every distinct state class must be distinguishable from every other one.
  for (unsigned left = 0; left < 6; ++left) {
    for (unsigned right = 0; right < 6; ++right) {
      const auto decision = compatibilityDrawBatchAdmission(
          true, 1, identityFor(left), identityFor(right), kMax);
      const bool extended =
          decision.admission == CompatibilityDrawBatchAdmission::Extend;
      check(extended == (left == right),
            "identity class " + std::to_string(left) + " vs " +
                std::to_string(right) +
                ": exactly the equal pair may share a run");
    }
  }
}

// ---------------------------------------------------------------------------
// 2. The production cut discipline, as a model over record streams.

enum class Token : std::uint8_t {
  DrawA = 0,   // batchable draw, state class 0
  DrawB,       // batchable draw, state class 1
  DrawUp,      // UP / TriangleFan draw: replays alone through the legacy sink
  StateRecord, // resource-bearing/state-apply or observable non-draw record
  ConstantNoOp, // pure ConstantUpload projection with unchanged bytes
  ConstantChanged, // pure ConstantUpload projection that bumps uniform generation
  Count,
};

// Why a published run ended. `RecordCut` is the one the published list alone
// cannot show, because a state record leaves no entry behind.
enum class Cause : std::uint8_t { RecordCut, Identity, Capacity, EndOfRange };

struct ModelResult {
  // One entry per published run, in publication order, holding the state class
  // of every draw it folded. A single-draw entry with class -1 is an unbatched
  // legacy submission.
  std::vector<std::vector<int>> published{};
  std::vector<CompatibilityDrawBatchIdentity> publishedIdentities{};
  std::vector<Cause> causes{};
  std::vector<std::vector<std::vector<int>>> transactions{};
  std::vector<bool> transactionCapacityCuts{};
  std::uint64_t queueAcquisitions = 0;
  std::uint64_t unbatchedDraws = 0;
};

// Mirrors replayResolvedChunk + submitCompatibilityReplayDrawBatched exactly:
// the record walk cuts before every ObservableEffect, allows a pure
// ConstantUpload projection to remain inside the island, the draw path cuts on
// the admission verdict, and the scope guard cuts at the end of the range.
ModelResult runModel(const std::vector<Token>& stream, std::uint32_t maxDraws) {
  ModelResult result{};
  std::vector<int> open{};
  std::vector<std::vector<int>> pendingTransaction{};
  bool pendingCapacityCut = false;
  CompatibilityDrawBatchIdentity openIdentity{};
  std::uint64_t uniformGenerationBump = 0;

  const auto flushBatch = [&]() {
    if (pendingTransaction.empty()) return;
    result.transactions.push_back(pendingTransaction);
    result.transactionCapacityCuts.push_back(pendingCapacityCut);
    pendingTransaction.clear();
    pendingCapacityCut = false;
    ++result.queueAcquisitions;
  };
  const auto pendingDrawCount = [&]() {
    std::size_t count = 0;
    for (const auto& run : pendingTransaction) count += run.size();
    return count;
  };
  const auto closeRun = [&](Cause cause) {
    if (open.empty()) return;
    result.published.push_back(open);
    result.publishedIdentities.push_back(openIdentity);
    result.causes.push_back(cause);
    pendingTransaction.push_back(open);
    open.clear();
    openIdentity = {};
  };
  const auto flush = [&](Cause cause) {
    closeRun(cause);
    flushBatch();
  };

  for (const auto token : stream) {
    if (token == Token::StateRecord || token == Token::DrawUp) {
      // replayResolvedChunk's pre-dispatch ObservableEffect cut.
      flush(Cause::RecordCut);
    }
    if (token == Token::StateRecord) continue;
    if (token == Token::ConstantNoOp) continue;
    if (token == Token::ConstantChanged) {
      // The pure projection itself does not cut. Its changed generation is
      // observed by the next draw, which then cuts on identity.
      ++uniformGenerationBump;
      continue;
    }
    if (token == Token::DrawUp) {
      // The sink publishes, then submits this draw on its own.
      const auto decision = compatibilityDrawBatchAdmission(
          false, static_cast<std::uint32_t>(open.size()), openIdentity,
          openIdentity, maxDraws);
      check(decision.admission == CompatibilityDrawBatchAdmission::Unbatchable,
            "a UP draw is never batchable");
      result.published.push_back({-1});
      result.publishedIdentities.push_back({});
      result.causes.push_back(Cause::RecordCut);
      pendingTransaction.push_back({-1});
      flushBatch();
      ++result.unbatchedDraws;
      continue;
    }
    const int stateClass = token == Token::DrawA ? 0 : 1;
    auto incoming = identityFor(static_cast<unsigned>(stateClass));
    incoming.generations[static_cast<std::size_t>(
        CompatibilityDrawBatchGeneration::Uniform)] += uniformGenerationBump;
    const auto decision = compatibilityDrawBatchAdmission(
        true, static_cast<std::uint32_t>(open.size()), openIdentity, incoming,
        std::numeric_limits<std::uint32_t>::max());
    if (pendingDrawCount() + open.size() >= maxDraws) {
      closeRun(Cause::Capacity);
      pendingCapacityCut = !pendingTransaction.empty();
      flushBatch();
    } else if (decision.admission == CompatibilityDrawBatchAdmission::FlushAndStart) {
      closeRun(Cause::Identity);
    }
    if (open.empty()) openIdentity = incoming;
    open.push_back(stateClass);
  }
  flush(Cause::EndOfRange);  // DrawBatchScope
  return result;
}

std::vector<int> drawSequence(const std::vector<Token>& stream) {
  std::vector<int> sequence;
  for (const auto token : stream) {
    if (token == Token::DrawA) sequence.push_back(0);
    if (token == Token::DrawB) sequence.push_back(1);
    if (token == Token::DrawUp) sequence.push_back(-1);
  }
  return sequence;
}

void modelStreamProperties() {
  constexpr std::uint32_t kMax = 3;
  constexpr std::size_t kMaxLength = 5;
  const auto tokenCount = static_cast<std::size_t>(Token::Count);

  std::size_t cases = 0;
  std::vector<Token> stream;
  for (std::size_t length = 1; length <= kMaxLength; ++length) {
    std::size_t total = 1;
    for (std::size_t i = 0; i < length; ++i) total *= tokenCount;
    for (std::size_t encoded = 0; encoded < total; ++encoded) {
      stream.clear();
      auto value = encoded;
      for (std::size_t i = 0; i < length; ++i) {
        stream.push_back(static_cast<Token>(value % tokenCount));
        value /= tokenCount;
      }
      ++cases;
      const auto result = runModel(stream, kMax);

      // (a) Conservation and order: flattening the published runs reproduces
      //     the source draw sequence exactly once, in order.
      std::vector<int> flattened;
      for (const auto& run : result.published) {
        for (const auto entry : run) flattened.push_back(entry);
      }
      check(flattened == drawSequence(stream),
            "published runs flatten to the exact source draw order");

      // (b) No run mixes identities, and no run is empty or over budget.
      for (const auto& run : result.published) {
        check(!run.empty(), "a published run is never empty");
        check(run.size() <= kMax, "a published run never exceeds the ceiling");
        for (const auto entry : run) {
          check(entry == run.front(),
                "a published run folds exactly one state identity");
          check(entry != -1 || run.size() == 1u,
                "an unbatched legacy draw is published alone");
        }
      }

      // (c) Maximality: two consecutive runs of the same identity exist only
      //     because a cut forced them -- an intervening record the published
      //     list cannot show, or the capacity ceiling. An Identity cut can
      //     never separate two runs of the SAME identity, which is what keeps
      //     `A, A` at one run while `A -> B -> A` stays at three.
      check(result.causes.size() == result.published.size(),
            "every published run records why it was cut");
      for (std::size_t i = 1; i < result.published.size(); ++i) {
        const auto& previous = result.published[i - 1];
        const auto& current = result.published[i];
        if (previous.front() != current.front() || previous.front() == -1 ||
            result.publishedIdentities[i - 1] !=
                result.publishedIdentities[i]) {
          continue;
        }
        const auto cause = result.causes[i - 1];
        check(cause == Cause::RecordCut || cause == Cause::Capacity,
              "adjacent same-identity runs are cut by a record or the ceiling");
      }

      // (d) Queue transactions: several state-separated runs may share one
      // source-local publication, but no transaction is empty and no draw is
      // published twice.
      check(result.queueAcquisitions == result.transactions.size(),
            "one queue acquisition per published batch/transaction");
      check(result.queueAcquisitions <= flattened.size() ||
                flattened.empty(),
            "batching never adds a queue acquisition");
      check(result.transactionCapacityCuts.size() == result.transactions.size(),
            "every transaction records whether capacity caused its publication");
      for (std::size_t transactionIndex = 0;
           transactionIndex < result.transactions.size(); ++transactionIndex) {
        std::size_t transactionDraws = 0;
        for (const auto& run : result.transactions[transactionIndex]) {
          transactionDraws += run.size();
        }
        check(transactionDraws <= kMax,
              "a published batch never exceeds the total draw ceiling");
        check(!result.transactionCapacityCuts[transactionIndex] ||
                  transactionDraws == kMax,
              "capacity publication follows a full batch, not a full run");
      }
    }
  }
  std::size_t expectedCases = 0;
  std::size_t expectedSequences = 1;
  for (std::size_t length = 1; length <= kMaxLength; ++length) {
    expectedSequences *= tokenCount;
    expectedCases += expectedSequences;
  }
  check(cases == expectedCases,
        "the stream truth table covered every sequence of length 1..5");
}

// ---------------------------------------------------------------------------
// 3. Named regressions the property sweep would only catch indirectly.

void namedShapes() {
  constexpr std::uint32_t kMax = 8;
  const auto sizes = [](const ModelResult& result) {
    std::vector<std::size_t> out;
    for (const auto& run : result.published) out.push_back(run.size());
    return out;
  };

  {
    const auto result =
        runModel({Token::DrawA, Token::DrawA, Token::DrawA}, kMax);
    check(sizes(result) == std::vector<std::size_t>{3},
          "three identical adjacent draws take one queue acquisition");
  }
  {
    // The A->B->A case named in the requirement: the second A compares against
    // the pending B, not against a history, so it must not rejoin the first.
    const auto result =
        runModel({Token::DrawA, Token::DrawB, Token::DrawA}, kMax);
    check(sizes(result) == std::vector<std::size_t>{1, 1, 1},
          "A -> B -> A stays three distinct runs");
    check(result.queueAcquisitions == 1u && result.transactions.size() == 1u,
          "A -> B -> A publishes three runs in one transaction");
  }
  {
    // A state record between two identical draws is the interesting shape: the
    // record walk cuts before it because replaying it may mutate the device
    // state the open run borrows.
    const auto result = runModel(
        {Token::DrawA, Token::StateRecord, Token::DrawA}, kMax);
    check(sizes(result) == std::vector<std::size_t>{1, 1},
          "a record between two draws cuts the run at its exact position");
  }
  {
    const auto result =
        runModel({Token::DrawA, Token::ConstantNoOp, Token::DrawA}, kMax);
    check(sizes(result) == std::vector<std::size_t>{2},
          "a no-op constant projection stays inside the draw island");
  }
  {
    const auto result = runModel(
        {Token::DrawA, Token::ConstantChanged, Token::DrawB}, kMax);
    check(sizes(result) == std::vector<std::size_t>{1, 1},
          "a changed constant cuts the island on the next draw");
    check(result.causes.front() == Cause::Identity,
          "a changed constant is reported as an identity cut on the next draw");
  }
  {
    const auto result = runModel(
        {Token::DrawA, Token::DrawA, Token::DrawUp, Token::DrawA}, kMax);
    check(sizes(result) == std::vector<std::size_t>{2, 1, 1},
          "a UP draw publishes the open run and then submits alone");
    check(result.unbatchedDraws == 1u,
          "the UP draw is the only per-draw fallback in that stream");
  }
  {
    const auto result = runModel(
        {Token::DrawA, Token::DrawA, Token::DrawA, Token::DrawA}, 2);
    check(sizes(result) == std::vector<std::size_t>{2, 2},
          "the ceiling splits a long identical run without losing a draw");
  }
  {
    const auto result = runModel({Token::StateRecord, Token::StateRecord}, kMax);
    check(result.published.empty() && result.queueAcquisitions == 0u,
          "a draw-free record range takes no queue acquisition at all");
  }
}

}  // namespace

int main() {
  admissionTruthTable();
  modelStreamProperties();
  namedShapes();
  if (failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
