#include "../../../src/dxmt9/dxmt9_query_timeline_observer.hpp"

#include <cstdlib>
#include <iostream>

namespace {

using dxmt9::query::timeline_diag::ActualSubmit;
using dxmt9::query::timeline_diag::Completion;
using dxmt9::query::timeline_diag::End;
using dxmt9::query::timeline_diag::EncodeEnd;
using dxmt9::query::timeline_diag::EncodeStart;
using dxmt9::query::timeline_diag::Identity;
using dxmt9::query::timeline_diag::kCapacity;
using dxmt9::query::timeline_diag::Ledger;
using dxmt9::query::timeline_diag::MarkerWitness;
using dxmt9::query::timeline_diag::Publication;

void check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "query_timeline_observer_spec: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

MarkerWitness marker() {
  return {.queue = 7, .index = 3, .generation = 4, .storageGeneration = 5};
}

void deterministicLifecycleAndReissue() {
  Ledger ledger(false);
  const Identity first{7, 42, 1, 9};
  ledger.end(first, 3, 10, {}, 100);
  ledger.publication(7, 10, {}, 110);
  ledger.encodeStart(7, 10, 115);
  ledger.encodeEnd(7, 10, 117);
  ledger.actualSubmit(7, 10, 120);
  ledger.completion(7, 10, true, 130);
  ledger.poll(first, 1, true, {}, 140);
  ledger.poll(first, 0, true, 55, 150);
  auto summaries = ledger.snapshotSummaries();
  check(ledger.size() == 1, "one identity is retained");
  const auto& summary = summaries[0];
  check(summary.stages ==
            (End | Publication | EncodeStart | EncodeEnd | ActualSubmit | Completion),
        "END, publication, submit, and completion are distinct stages");
  check(summary.encodeStartNs == 115 && summary.encodeEndNs == 117,
        "encode interval preserves stage ordering");
  check(summary.firstPendingNs == 140 && summary.lastPendingNs == 140,
        "pending range is deterministic");
  check(summary.firstOkNs == 150 && summary.lastOkNs == 150,
        "S_OK range is deterministic");
  check(summary.resultValue && *summary.resultValue == 55,
        "result is captured only after explicit completion");

  const Identity second{7, 42, 2, 9};
  ledger.end(second, 3, 20, {}, 200);
  check(ledger.size() == 2, "reissue ordinal creates a separate summary");

  // Chunk-granular completion applies to every END in the shared sequence.
  const Identity shared{7, 43, 1, 9};
  ledger.end(shared, 3, 30, {}, 210);
  ledger.publication(7, 30, {}, 220);
  ledger.encodeStart(7, 30, 225);
  ledger.encodeEnd(7, 30, 227);
  ledger.actualSubmit(7, 30, 230);
  ledger.completion(7, 30, true, 240);
  const Identity sharedPeer{7, 44, 1, 9};
  ledger.end(sharedPeer, 3, 30, {}, 250);
  ledger.poll(sharedPeer, 0, true, 1, 260);
  const auto sharedSummaries = ledger.snapshotSummaries();
  check(sharedSummaries[2].has(Completion) && sharedSummaries[3].has(Completion) &&
            sharedSummaries[3].has(EncodeStart) &&
            sharedSummaries[3].has(EncodeEnd),
        "one completed shared sequence settles every END identity");
}

void deferredMarkerAndUnknownPoll() {
  Ledger ledger(false);
  const auto witness = marker();
  const Identity identity{7, 100, 4, 11};
  // Publication can race the core observer's END callback for a deferred
  // marker. The bounded evidence join must retain the real publication time.
  ledger.publication(7, 33, witness, 300);
  ledger.end(identity, 5, 0, witness, 310);
  ledger.actualSubmit(7, 33, 320);
  ledger.poll(identity, 0, true, 88, 325);
  ledger.completion(7, 33, true, 330);
  ledger.poll(identity, 0, true, 99, 340);
  ledger.poll(Identity{7, 100, 999, 11}, 1, true, {}, 350);
  const auto summaries = ledger.snapshotSummaries();
  const auto& summary = summaries[0];
  check(summary.has(Publication) && summary.has(ActualSubmit) &&
            summary.has(Completion),
        "deferred marker joins publication before END");
  check(summary.publicationNs == 300 && summary.submitNs == 320 &&
            summary.completionNs == 330,
        "deferred marker preserves stage clocks");
  check(summary.resultValue && *summary.resultValue == 99,
        "a value is captured after completion evidence is joined");
  check(summary.firstOkNs == 325 && summary.lastOkNs == 340,
        "S_OK polling range retains both pre/post completion observations");
  check(ledger.stats().unknownPolls == 1,
        "unknown poll is counted separately from missing stages");
  check(summary.missing() == (EncodeStart | EncodeEnd),
        "missing encode stages remain explicit when no encode hook ran");

  // The common order is END returning before the deferred writer is bound.
  const Identity beforeBind{7, 101, 1, 11};
  const auto secondWitness = MarkerWitness{
      .queue = 7, .index = 4, .generation = 4, .storageGeneration = 5};
  ledger.end(beforeBind, 5, 0, secondWitness, 360);
  ledger.publication(7, 34, secondWitness, 370);
  ledger.encodeStart(7, 34, 375);
  ledger.encodeEnd(7, 34, 378);
  ledger.actualSubmit(7, 34, 380);
  ledger.completion(7, 34, true, 390);
  const auto afterBind = ledger.snapshotSummaries();
  check(afterBind[1].fenceSequence == 34 && afterBind[1].missing() == 0,
        "END-before-bind joins the later actual sequence and all stages");

  // A second END in an already completed chunk still receives retained,
  // bounded evidence rather than being left with inferred/missing stages.
  const Identity latePeer{7, 102, 1, 11};
  ledger.end(latePeer, 5, 34, {}, 400);
  const auto late = ledger.snapshotSummaries();
  check(late[2].missing() == 0,
        "late shared-sequence END joins completed evidence");
}

void boundedStorageAndMissingStages() {
  Ledger ledger(false);
  for (std::uint64_t i = 1; i <= kCapacity; ++i)
    ledger.end(Identity{1, i, 1, 1}, 1, i, {}, i);
  ledger.end(Identity{1, 1000, 1, 1}, 1, 1000, {}, 1000);
  check(ledger.size() == kCapacity, "summary storage is bounded");
  check(ledger.stats().droppedSummaries == 1, "overflow is counted");
  const auto summaries = ledger.snapshotSummaries();
  check(summaries[0].missing() ==
            (Publication | EncodeStart | EncodeEnd | ActualSubmit | Completion),
        "missing lifecycle stages remain explicit");
}

void recentEvidenceRingAndSkip() {
  Ledger skipped(false, 2);
  skipped.end(Identity{2, 1, 1, 1}, 1, 1, {}, 1);
  skipped.end(Identity{2, 2, 1, 1}, 1, 2, {}, 2);
  skipped.end(Identity{2, 3, 1, 1}, 1, 3, {}, 3);
  check(skipped.stats().skippedEnds == 2 && skipped.size() == 1,
        "skip count suppresses only the first summary rows and tracks them");

  Ledger ring(false);
  for (std::uint64_t sequence = 1; sequence <= kCapacity + 3; ++sequence)
    ring.publication(3, sequence, {}, sequence);
  const Identity recent{3, 99, 1, 1};
  ring.end(recent, 1, kCapacity + 3, {}, 1000);
  const auto summaries = ring.snapshotSummaries();
  check(summaries[0].has(Publication) &&
            ring.stats().evictedEvidence == 3,
        "evidence uses a bounded recent ring and keeps the newest row");

  const auto witness = MarkerWitness{
      .queue = 4, .index = 8, .generation = 9, .storageGeneration = 10};
  ring.publication(4, 77, {}, 1100);
  ring.publication(4, 77, witness, 1110);
  const Identity deferred{4, 88, 1, 2};
  ring.end(deferred, 1, 0, witness, 1120);
  const auto updated = ring.snapshotSummaries();
  check(updated[1].has(Publication) && updated[1].fenceSequence == 77,
        "sequence-only evidence upgrades to the deferred marker witness");
}

void readyResidenceFrontierAndPredecessor() {
  Ledger ledger(false);
  // The ordinary source immediately before the query overlaps the query's
  // publication-to-dequeue residence interval.
  ledger.ordinaryEncodeStart(7, 41, 90);
  ledger.ordinaryEncodeEnd(7, 41, 110);
  const Identity query{7, 200, 1, 1};
  ledger.readyPublication(7, 42, {}, 3, 19, 17, 40, 12, 100);
  ledger.dequeue(7, 42, 200);
  // END can arrive after the queue has already observed both Ready edges.
  ledger.end(query, 1, 42, {}, 210);
  auto summaries = ledger.snapshotSummaries();
  const auto& summary = summaries[0];
  check(summary.readyFrontierKnown && summary.readyDepthBefore == 3 &&
            summary.readyOldestSeqBefore == 19 &&
            summary.encodingSeqBefore == 17 &&
            summary.submittedFrontierBefore == 40 &&
            summary.completedFrontierBefore == 12,
        "late END joins the publication-time Ready frontier snapshot");
  check(summary.dequeueNs == 200 && summary.predecessorEncodeSeq == 41 &&
            summary.predecessorEncodeStartNs == 90 &&
            summary.predecessorEncodeEndNs == 110 &&
            summary.predecessorEncodeOverlap,
        "exact preceding ordinary encode interval is joined late");

  // Queue qualification prevents a same-sequence source on another queue
  // from becoming a predecessor witness.
  Ledger distinctQueue(false);
  distinctQueue.ordinaryEncodeStart(8, 41, 90);
  distinctQueue.ordinaryEncodeEnd(8, 41, 110);
  distinctQueue.readyPublication(7, 42, {}, 1, 0, 0, 0, 0, 100);
  distinctQueue.dequeue(7, 42, 200);
  distinctQueue.end(Identity{7, 201, 1, 1}, 1, 42, {}, 210);
  check(!distinctQueue.snapshotSummaries()[0].predecessorEncodeOverlap,
        "predecessor evidence remains queue-qualified");

  // A missing exact N-1 source and an interval outside residence are both
  // explicit coverage gaps, rather than inferred waiting evidence.
  Ledger gaps(false);
  gaps.ordinaryEncodeStart(7, 40, 90);
  gaps.ordinaryEncodeEnd(7, 40, 110);
  gaps.readyPublication(7, 42, {}, 1, 0, 0, 0, 0, 100);
  gaps.dequeue(7, 42, 200);
  gaps.end(Identity{7, 202, 1, 1}, 1, 42, {}, 210);
  check(!gaps.snapshotSummaries()[0].predecessorEncodeOverlap,
        "missing exact predecessor remains an explicit gap");

  Ledger nonoverlap(false);
  nonoverlap.ordinaryEncodeStart(7, 41, 10);
  nonoverlap.ordinaryEncodeEnd(7, 41, 20);
  nonoverlap.readyPublication(7, 42, {}, 0, 0, 0, 0, 0, 100);
  nonoverlap.dequeue(7, 42, 200);
  nonoverlap.end(Identity{7, 203, 1, 1}, 1, 42, {}, 210);
  check(!nonoverlap.snapshotSummaries()[0].predecessorEncodeOverlap,
        "non-overlapping predecessor interval remains a coverage gap");
}

void boundedOrdinaryEncodeLedger() {
  Ledger ledger(false);
  for (std::uint64_t sequence = 1; sequence <= kCapacity + 1; ++sequence) {
    ledger.ordinaryEncodeStart(9, sequence, sequence);
    ledger.ordinaryEncodeEnd(9, sequence, sequence + 1);
  }
  check(ledger.stats().evictedOrdinaryEncode == 1,
        "ordinary encode evidence uses a bounded eviction ring");
}

}  // namespace

int main() {
  {
    Ledger sampled(false, 2, 3);
    for (unsigned i = 1; i <= 11; ++i)
      sampled.end({7, i, 1, 1}, 3, i, {}, 100 + i);
    const auto rows = sampled.snapshotSummaries();
    check(sampled.size() == 3 && sampled.stats().skippedEnds == 8,
          "skip then stride bounds full-run sampling");
    check(rows[0].identity.query == 3 && rows[1].identity.query == 6 &&
          rows[2].identity.query == 9, "stride does not relabel identities");
    sampled.poll({7, 9, 1, 1}, 0, true, 42, 120);
    check(sampled.snapshotSummaries()[2].firstOkNs == 120,
          "late sampled snapshot result joins exact identity");
  }
  deterministicLifecycleAndReissue();
  deferredMarkerAndUnknownPoll();
  boundedStorageAndMissingStages();
  recentEvidenceRingAndSkip();
  readyResidenceFrontierAndPredecessor();
  boundedOrdinaryEncodeLedger();
  std::cout << "query_timeline_observer_spec passed\n";
}
