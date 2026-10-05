#include "render_tape_encode_draw_witness.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
using namespace dxmt9::tests::encode_draw_witness;
constexpr DrawKey first{7u, 2u, 3u}, next{7u, 2u, 4u};
constexpr std::array<std::uint8_t, 4u> bytes{1u, 2u, 3u, 4u};
constexpr DestinationCheck equal{DestinationCheck::State::Equal, 0u, bytes.size()};

void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void testSelectedStorageAndOrphans() {
  Collector collector;
  collector.beginDraw(first);
  collector.observeSlice(BufferKind::TransientVertexUpload, 4u, 0u, bytes,
      {11u, 8u, bytes.size()}, equal);
  collector.observeSlice(BufferKind::TransientVertexUpload, 3u, 0u, bytes,
      {12u, 16u, bytes.size()}, equal);
  check(collector.issues().empty(), "successful uploads alone issue no draw witness");
  collector.bindVertex(12u, 16u, 1u);
  collector.issue(false, 3u, 3u, 0u, 1u, 0, 0u);
  check(!collector.failed() && collector.issues().size() == 1u &&
      collector.issues()[0].draw == first && collector.issues()[0].selectedStreamCount == 1u &&
      collector.issues()[0].streams[0].upload == 1u && collector.orphanUploads() == 1u,
      "only the final issued selected main slice qualifies; orphan extra upload does not");
  check(collector.uploads()[1].destinationCompared && collector.observedBytes() == 24u,
      "bounded immediate destination equality is distinct from input fingerprints");
}

void testRetainedBindingsAndUniformSlots() {
  Collector collector;
  collector.beginSourceFragment(7u);
  collector.beginDraw(first);
  collector.observeSlice(BufferKind::TransientVertexUpload, 3u, 0u, bytes,
      {11u, 8u, bytes.size()}, equal);
  collector.bindVertex(11u, 8u, 1u);
  collector.issue(false, 3u, 3u, 0u, 1u, 0, 0u);
  collector.beginDraw(next);
  collector.bindVertex(99u, 0u, 0u);
  collector.bindVertex(98u, 0u, 3u);
  collector.bindVertexBytes(5u);
  collector.issue(false, 3u, 3u, 0u, 1u, 0, 0u);
  collector.endSourceFragment(7u);
  collector.beginSourceFragment(8u);
  collector.beginDraw({8u, 0u, 0u});
  collector.issue(false, 3u, 3u, 0u, 1u, 0, 0u);
  check(!collector.failed() && collector.issues()[1].streams[0].upload == 0u &&
      collector.issues()[2].streams[0].upload == 0u,
      "unchanged bindings survive beginDraw and source fragments; uniform slots cannot erase stream0");
  collector.renderPassBoundary();
  collector.beginDraw({8u, 1u, 0u});
  collector.issue(false, 3u, 3u, 0u, 1u, 0, 0u);
  check(collector.issues().back().selectedStreamCount == 0u,
      "a genuine new render encoder clears inherited binding evidence");
  collector.bindVertex(11u, 8u, 6u);
  collector.bindVertexBytes(6u);
  collector.issue(false, 3u, 3u, 0u, 1u, 0, 0u);
  check(collector.issues().back().selectedStreamCount == 0u,
      "setVertexBytes to a stream slot invalidates the prior buffer identity");
}

void testDirectBindingsAndReplacements() {
  Collector collector;
  collector.beginSourceFragment(7u);
  collector.beginDraw(first);
  collector.bindVertex(11u, 8u, 1u);
  collector.bindVertex(12u, 32u, 6u);
  collector.bindVertex(90u, 0u, 0u);
  collector.bindVertex(91u, 0u, 3u);
  collector.bindVertex(92u, 0u, 5u);
  collector.bindVertex(93u, 0u, 21u);
  collector.issue(false, 3u, 3u, 0u, 1u, 0, 0u);
  const auto& firstIssue = collector.issues()[0];
  check(!collector.failed() && collector.uploads().empty() &&
      firstIssue.boundStreamCount == 2u && firstIssue.selectedStreamCount == 0u &&
      firstIssue.streams[0].slot == 1u && firstIssue.streams[0].bindingBuffer == 11u &&
      firstIssue.streams[0].bindingOffset == 8u && firstIssue.streams[0].upload == UINT32_MAX &&
      firstIssue.streams[1].slot == 6u && firstIssue.streams[1].bindingBuffer == 12u &&
      firstIssue.streams[1].bindingOffset == 32u && firstIssue.streams[1].upload == UINT32_MAX,
      "natural direct bindings survive without manufactured upload receipts");
  const auto stream0Id = firstIssue.streams[0].bufferObservationId;
  const auto extraId = firstIssue.streams[1].bufferObservationId;
  check(stream0Id && extraId && stream0Id != extraId && collector.nativeBufferCount() == 2u &&
      collector.bufferObservationId(90u) == 0u && collector.observedBytes() == 0u,
      "uniform/nonstream slots add no native IDs or byte observations");

  collector.beginDraw(next);
  collector.bindVertex(11u, 16u, 1u);
  collector.bindVertexBytes(0u);
  collector.bindVertexBytes(3u);
  collector.bindVertexBytes(5u);
  collector.issue(false, 3u, 3u, 0u, 1u, 0, 0u);
  check(collector.issues()[1].boundStreamCount == 2u &&
      collector.issues()[1].streams[0].bindingOffset == 16u &&
      collector.issues()[1].streams[0].bufferObservationId == stream0Id &&
      collector.issues()[0].streams[0].bindingOffset == 8u,
      "offset replacement updates the final bind without rewriting a previous issue");
  collector.bindVertex(13u, 16u, 1u);
  collector.issue(false, 3u, 3u, 0u, 1u, 0, 0u);
  check(collector.issues()[2].streams[0].bindingBuffer == 13u &&
      collector.issues()[2].streams[0].bufferObservationId != stream0Id &&
      collector.issues()[2].streams[0].upload == UINT32_MAX,
      "buffer replacement at the same offset is not the previous native binding");

  collector.endSourceFragment(7u);
  collector.beginSourceFragment(8u);
  collector.beginDraw({8u, 0u, 0u});
  collector.issue(false, 3u, 3u, 0u, 1u, 0, 0u);
  check(collector.issues().back().boundStreamCount == 2u &&
      collector.issues().back().streams[0].bindingBuffer == 13u &&
      collector.issues().back().streams[1].bufferObservationId == extraId,
      "direct bindings continue across source fragments in the same render encoder");
  collector.bindVertexBytes(1u);
  collector.bindVertex(0u, 32u, 6u);
  collector.issue(false, 3u, 3u, 0u, 1u, 0, 0u);
  check(collector.issues().back().boundStreamCount == 0u,
      "stream inline bytes and null binds invalidate direct native identities");
  collector.bindVertex(11u, 8u, 1u);
  collector.renderPassBoundary();
  collector.beginDraw({8u, 1u, 0u});
  collector.issue(false, 3u, 3u, 0u, 1u, 0, 0u);
  check(collector.issues().back().boundStreamCount == 0u,
      "render-pass boundaries clear direct bindings as well as upload selection");
  collector.bindVertex(11u, 24u, 1u);
  collector.issue(false, 3u, 3u, 0u, 1u, 0, 0u);
  check(!collector.failed() &&
      collector.issues().back().streams[0].bufferObservationId == stream0Id,
      "observation IDs remain run-local stable across render-pass boundaries");
  Collector allStreams;
  allStreams.beginDraw(first);
  allStreams.bindVertex(100u, 8u, 1u);
  for (std::uint8_t slot = 6u; slot <= 20u; ++slot)
    allStreams.bindVertex(100u + slot, slot, slot);
  allStreams.issue(false, 3u, 3u, 0u, 1u, 0, 0u);
  check(!allStreams.failed() && allStreams.issues()[0].boundStreamCount == 16u &&
      allStreams.issues()[0].streams[15].slot == 20u &&
      allStreams.issues()[0].selectedStreamCount == 0u,
      "all sixteen relevant native stream slots fit in the owned issue record");
}

void testDirectBindingsAndOptionalUploadSelection() {
  Collector collector;
  collector.beginDraw(first);
  collector.bindVertex(11u, 8u, 1u);
  const auto directId = collector.bufferObservationId(11u);
  collector.observeSlice(BufferKind::TransientVertexUpload, 3u, 0u, bytes,
      {11u, 8u, bytes.size()}, equal);
  collector.bindVertex(11u, 8u, 6u);
  collector.issue(false, 3u, 3u, 0u, 1u, 0, 0u);
  check(!collector.failed() && collector.uploads()[0].bufferObservationId == directId &&
      collector.issues()[0].streams[0].bufferObservationId == directId &&
      collector.issues()[0].streams[1].bufferObservationId == directId &&
      collector.nativeBufferCount() == 1u,
      "one run-local native ID is shared by direct binding and later upload evidence");
  collector.bindVertex(11u, 12u, 1u);
  collector.issue(false, 3u, 3u, 0u, 1u, 0, 0u);
  check(collector.issues()[1].boundStreamCount == 2u &&
      collector.issues()[1].selectedStreamCount == 1u &&
      collector.issues()[1].streams[0].bindingOffset == 12u &&
      collector.issues()[1].streams[0].upload == UINT32_MAX &&
      collector.issues()[1].streams[1].upload == 0u &&
      collector.uploads()[0].selectedIssues == 3u,
      "direct offsets outside the upload do not select it; later uploaded streams still count");
  collector.bindVertex(12u, 8u, 6u);
  collector.issue(false, 3u, 3u, 0u, 1u, 0, 0u);
  check(collector.issues()[2].boundStreamCount == 2u &&
      collector.issues()[2].selectedStreamCount == 0u &&
      collector.issues()[2].streams[1].bindingBuffer == 12u &&
      collector.issues()[2].streams[1].upload == UINT32_MAX &&
      collector.uploads()[0].selectedIssues == 3u,
      "replacing an uploaded native buffer does not inherit its upload receipt");
}

void testIndexedDirectBindingsAndIds() {
  Collector collector;
  collector.beginDraw(first);
  collector.bindVertex(11u, 8u, 1u);
  collector.issue(true, 3u, 2u, 0u, 1u, -2, 0u, {21u, 16u, 0u}, 2u);
  const auto indexId = collector.issues()[0].indexBufferObservationId;
  check(!collector.failed() && collector.uploads().empty() && indexId &&
      collector.issues()[0].indexUpload == UINT32_MAX &&
      collector.issues()[0].indexSlice == Slice{21u, 16u, 4u} &&
      collector.issues()[0].boundStreamCount == 1u &&
      collector.issues()[0].streams[0].upload == UINT32_MAX && collector.observedBytes() == 0u,
      "an indexed direct issue records its real IB identity and extent without upload proof");
  collector.observeSlice(BufferKind::TransientIndexUpload, 0u, 0u, bytes,
      {21u, 16u, bytes.size()});
  collector.issue(true, 3u, 2u, 0u, 1u, 0, 0u, {21u, 16u, 0u}, 2u);
  check(collector.uploads()[0].bufferObservationId == indexId &&
      collector.issues()[1].indexBufferObservationId == indexId &&
      collector.issues()[1].indexUpload == 0u,
      "later indexed input observations reuse the already issued native ID");
  collector.issue(true, 3u, 2u, 0u, 1u, 0, 0u, {21u, 20u, 0u}, 2u);
  collector.issue(true, 3u, 2u, 0u, 1u, 0, 0u, {22u, 16u, 0u}, 2u);
  check(collector.issues()[2].indexBufferObservationId == indexId &&
      collector.issues()[2].indexUpload == UINT32_MAX &&
      collector.issues()[3].indexBufferObservationId != indexId &&
      collector.issues()[3].indexUpload == UINT32_MAX,
      "indexed offset and buffer replacement cannot inherit an unrelated upload selection");
  collector.issue(false, 3u, 3u, 0u, 1u, 0, 0u);
  check(!collector.failed() && collector.issues().back().indexBufferObservationId == 0u &&
      collector.bufferObservationId(0u) == 0u && collector.bufferObservationId(99u) == 0u,
      "nonindexed and unseen/null buffers export no fabricated observation ID");
}

void testReuseRequiresOriginalReceipt() {
  Collector collector;
  collector.beginDraw(first);
  collector.observeSlice(BufferKind::StagedVertexUpload, 5u, 33u, bytes,
      {11u, 8u, bytes.size()}, equal);
  const auto digest = collector.uploads()[0].inputDigest;
  collector.beginDraw(next);
  collector.observeSlice(BufferKind::StagedVertexReuse, 5u, 33u, {}, {11u, 8u, bytes.size()});
  collector.bindVertex(11u, 8u, 1u);
  collector.issue(false, 3u, 3u, 0u, 1u, 0, 0u);
  check(!collector.failed() && collector.reuses().size() == 1u &&
      collector.reuses()[0].upload == 0u && collector.uploads()[0].inputDigest == digest &&
      collector.observedBytes() == 12u,
      "reuse resolves its genuine previous upload and never rereads current source bytes");
  Collector absent;
  absent.beginDraw(first);
  absent.observeSlice(BufferKind::StagedVertexReuse, 5u, 33u, {}, {11u, 8u, bytes.size()});
  check(absent.failure() == Failure::MissingReuseUpload && absent.reuses().empty(),
      "a cache slice cannot manufacture an upload receipt");
  Collector wrong;
  wrong.beginDraw(first);
  wrong.observeSlice(BufferKind::StagedVertexUpload, 5u, 33u, bytes,
      {11u, 8u, bytes.size()}, equal);
  wrong.observeSlice(BufferKind::StagedVertexReuse, 5u, 34u, {}, {11u, 8u, bytes.size()});
  check(wrong.failure() == Failure::MissingReuseUpload, "reuse also authenticates logical source handle");
  Collector index;
  index.beginDraw(first);
  index.observeSlice(BufferKind::StagedIndexUpload, 5u, 44u, bytes,
      {21u, 16u, bytes.size()}, equal);
  index.beginDraw(next);
  index.observeSlice(BufferKind::StagedIndexReuse, 5u, 44u, {}, {21u, 16u, bytes.size()});
  index.issue(true, 3u, 2u, 0u, 1u, 0, 0u, {21u, 16u, 0u}, 2u);
  check(!index.failed() && index.reuses()[0].upload == 0u && index.issues()[0].indexUpload == 0u,
      "staged index reuse selects its original upload without a new input observation");
}

void testIndexedSelectionAndInputOnly() {
  Collector collector;
  collector.beginDraw(first);
  collector.observeSlice(BufferKind::TransientIndexUpload, 0u, 0u, bytes,
      {21u, 16u, bytes.size()});
  collector.issue(true, 3u, 2u, 0u, 1u, -2, 0u, {21u, 16u, 0u}, 2u);
  check(!collector.failed() && collector.issues()[0].indexUpload == 0u &&
      !collector.uploads()[0].destinationCompared && collector.observedBytes() == 4u,
      "selected indexed input-only evidence does not claim destination equality");
  collector.issue(true, 3u, 3u, 0u, 1u, 0, 0u, {21u, 16u, 0u}, 2u);
  check(collector.issues()[1].indexUpload == UINT32_MAX,
      "index issue bytes exceeding a candidate slice do not select that receipt");
}

void testFailureAndBudgets() {
  Collector noUpload;
  noUpload.beginDraw(first);
  noUpload.issue(false, 3u, 3u, 0u, 1u, 0, 0u);
  check(!noUpload.failed() && noUpload.uploads().empty() &&
      noUpload.issues()[0].selectedStreamCount == 0u, "nontransient draws need no invented upload");
  Collector skipped;
  skipped.beginDraw(first);
  check(skipped.draws().size() == 1u && skipped.issues().empty(),
      "draw begin followed by failure/early skip is not an issued draw");
  Collector suppressed;
  suppressed.suppression(true, false, false);
  suppressed.beginDraw(first);
  check(suppressed.failure() == Failure::Suppressed && suppressed.draws().empty(),
      "suppressed Metal callbacks cannot qualify actual draw evidence");
  Collector sideEffects;
  sideEffects.suppression(false, true, false);
  check(sideEffects.failure() == Failure::Suppressed, "command-side-effect suppression rejects");
  Collector lookup;
  lookup.suppression(false, false, true);
  check(lookup.failure() == Failure::Suppressed, "fake base-state lookup rejects");
  Collector mismatch;
  mismatch.beginDraw(first);
  mismatch.observeSlice(BufferKind::TransientVertexUpload, 3u, 0u, bytes,
      {11u, 8u, bytes.size()}, {DestinationCheck::State::Rejected, 8u, 0u});
  check(mismatch.failure() == Failure::DestinationMismatch && mismatch.uploads().empty() &&
      mismatch.destinationFailureStatus() == 8u, "destination mismatch preserves no accepted receipt");
  Collector overflow;
  overflow.beginDraw(first);
  overflow.observeSlice(BufferKind::TransientVertexUpload, 3u, 0u, bytes,
      {11u, UINT64_MAX - 1u, bytes.size()});
  check(overflow.failure() == Failure::ArithmeticOverflow, "slice endpoint overflows before reading bytes");
  Collector indexOverflow;
  indexOverflow.beginDraw(first);
  indexOverflow.issue(true, 3u, UINT64_MAX, 0u, 1u, 0, 0u, {11u, 0u, 0u}, 4u);
  check(indexOverflow.failure() == Failure::ArithmeticOverflow && indexOverflow.issues().empty(),
      "issued index-byte multiplication is checked");
  Limits limits{.maxDraws = 1u, .maxUploads = 1u, .maxReuses = 1u, .maxIssues = 1u,
                .maxMemberBytes = 4u, .maxObservedBytes = 12u};
  Collector bounded(limits);
  bounded.beginDraw(first);
  bounded.observeSlice(BufferKind::TransientVertexUpload, 3u, 0u, bytes,
      {11u, 8u, bytes.size()}, equal);
  bounded.beginDraw(next);
  check(bounded.failure() == Failure::CapacityExceeded && bounded.draws().size() == 1u,
      "bounded draw storage never grows after its explicit cap");
  limits.maxObservedBytes = 11u;
  Collector byteLimit(limits);
  check(byteLimit.destinationBudget() == 3u,
      "pre-comparison budget excludes a four-byte memcmp before aggregate rejection");
  byteLimit.beginDraw(first);
  byteLimit.observeSlice(BufferKind::TransientVertexUpload, 3u, 0u, bytes,
      {11u, 8u, bytes.size()}, equal);
  check(byteLimit.failure() == Failure::CapacityExceeded && byteLimit.uploads().empty(),
      "hash plus input/destination comparison costs are bounded in aggregate");
  limits.maxUploads = 65537u;
  Collector invalidLimits(limits);
  check(invalidLimits.failure() == Failure::InvalidLimits, "hard row caps reject oversized caller limits");
  limits.maxUploads = 1u;
  limits.maxMemberBytes = 64u * 1024u * 1024u + 1u;
  limits.maxObservedBytes = 1024u * 1024u * 1024u;
  Collector hardByteCap(limits);
  check(hardByteCap.failure() == Failure::InvalidLimits, "hard member byte cap cannot be raised by a caller");
  Limits nativeLimits{.maxNativeBuffers = 1u};
  Collector nativeLimit(nativeLimits);
  nativeLimit.beginDraw(first);
  nativeLimit.bindVertex(11u, 8u, 1u);
  nativeLimit.bindVertex(90u, 0u, 3u);
  nativeLimit.issue(false, 3u, 3u, 0u, 1u, 0, 0u);
  nativeLimit.issue(true, 3u, 2u, 0u, 1u, 0, 0u, {21u, 16u, 0u}, 2u);
  check(nativeLimit.failure() == Failure::CapacityExceeded &&
      nativeLimit.nativeBufferCount() == 1u && nativeLimit.issues().size() == 1u,
      "native ID storage is bounded and a rejected IB cannot publish a partial issue");
  nativeLimits.maxNativeBuffers = 65537u;
  Collector invalidNativeLimits(nativeLimits);
  check(invalidNativeLimits.failure() == Failure::InvalidLimits,
      "native observation hard cap cannot be raised by a caller");
}
} // namespace

int main() {
  try {
    testSelectedStorageAndOrphans();
    testRetainedBindingsAndUniformSlots();
    testDirectBindingsAndReplacements();
    testDirectBindingsAndOptionalUploadSelection();
    testIndexedDirectBindingsAndIds();
    testReuseRequiresOriginalReceipt();
    testIndexedSelectionAndInputOnly();
    testFailureAndBudgets();
  } catch (const std::exception& failure) {
    std::cerr << "render-tape-encode-draw-witness: " << failure.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
