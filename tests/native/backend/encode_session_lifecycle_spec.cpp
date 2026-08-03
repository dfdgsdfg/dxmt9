// End-to-end native coverage for the deferred EncodeSession lifecycle.
//
// The native nowine test host has no Metal device. This spec therefore uses
// retained Foundation objects strictly as non-null WMT ownership tokens and an
// inert CommandQueue. A targetless deferred clear exercises session payload
// carry while guaranteeing that encodeChunk never sends a Metal command. The
// production encodeChunk and
// finalizeEncodeChunkSessionIntoSubmission implementations still execute in
// full, covering command-buffer carry, ordered source accumulation,
// publication into the tail submission, and session reset.

#include "../../../src/dxmt9/dxmt9_draw_encoder.hpp"
#include "../../../src/dxmt9/dxmt9_encode_session.hpp"
#include "../../../src/dxmt9/dxmt9_pipeline_cache.hpp"
#include "../../../src/dxmt9/dxmt9_resource_pool.hpp"
#include "../../../src/dxmt9/dxmt9_ring_arena.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string message) {
  throw TestFailure(std::move(message));
}

void check(bool condition, std::string_view message) {
  if (!condition) {
    fail(std::string(message));
  }
}

template <typename A, typename B>
void checkEq(const A& actual, const B& expected, std::string_view message) {
  if (!(actual == expected)) {
    std::ostringstream out;
    out << message << " (" << actual << " vs " << expected << ")";
    fail(out.str());
  }
}

template <typename WmtType>
WMT::Reference<WmtType> retainedToken(const char* label) {
  auto owner = WMT::MakeString(label, WMTUTF8StringEncoding);
  return WMT::Reference<WmtType>(WmtType{owner.handle});
}

struct Harness {
  dxmt9::core::BackendLimits limits{};
  dxmt9::resources::Pool pool{};
  dxmt9::pipeline::Cache cache{};
  dxmt9::scratch::FrameAllocators allocators{};
  WMT::Reference<WMT::Device> device =
      retainedToken<WMT::Device>("encode-session-device-token");
  dxmt9::CommandQueue queue;

  Harness()
      : queue(dxmt9::CommandQueue::InertTestQueueTag{},
              retainedToken<WMT::CommandQueue>("encode-session-queue-token"),
              limits) {}

  dxmt9::encoders::EncodeContext makeContext() {
    return dxmt9::encoders::EncodeContext{
        device,
        limits,
        pool,
        cache,
        allocators,
        nullptr,
        nullptr,
        queue,
    };
  }
};

dxmt9::encoders::EncodeChunkOptions deferredOptions(
    dxmt9::encoders::EncodeChunkSessionState& session,
    WMT::Reference<WMT::CommandBuffer> commandBuffer,
    dxmt9::core::metalqueue::QueueCompletionSource source) {
  dxmt9::encoders::EncodeChunkOptions options{};
  options.commandBuffer = std::move(commandBuffer);
  options.session = &session;
  options.deferSessionFinalization = true;
  options.sessionSource = source;
  return options;
}

void encodeChunkThenFinalizerPublishesAndResetsOneSession() {
  Harness harness;
  auto ctx = harness.makeContext();
  auto session = dxmt9::encoders::makeEncodeChunkSession();
  check(static_cast<bool>(session), "session owner is created");

  dxmt9::core::ChunkSlot headSlot{};
  headSlot.seqId = 41;
  dxmt9::core::ClearDesc headClear{};
  headClear.clearColor = true;
  headSlot.appendClear(headClear);
  const dxmt9::core::metalqueue::QueueCompletionSource headSource{
      .slotIndex = 2,
      .seqId = headSlot.seqId,
      .hasPresent = false,
      .commandBegin = 0,
      .commandCount = 1,
  };
  auto commandBuffer =
      retainedToken<WMT::CommandBuffer>("encode-session-cb-token");
  const obj_handle_t commandBufferHandle = commandBuffer.handle;

  auto head = dxmt9::encoders::encodeChunk(
      ctx, headSource.slotIndex, headSlot,
      deferredOptions(*session, std::move(commandBuffer), headSource));
  check(head.has_value(), "head encodeChunk returns a deferred submission");
  checkEq(head->commandBuffer.handle, commandBufferHandle,
          "head submission carries the injected command buffer");
  check(head->explicitCompletionSourceSpan().empty(),
        "deferred head does not publish session sources");
  check(dxmt9::encoders::encodeChunkSessionHasDeferredSubmissionPayload(
            *session),
        "head clear remains as deferred session payload");

  const auto sourcesAfterHead =
      dxmt9::encoders::encodeChunkSessionSources(*session);
  checkEq(sourcesAfterHead.size(), std::size_t{1},
          "head source is retained by the session");
  checkEq(sourcesAfterHead[0].seqId, headSource.seqId,
          "session retains the head sequence");

  dxmt9::core::ChunkSlot tailSlot{};
  tailSlot.seqId = 42;
  const dxmt9::core::metalqueue::QueueCompletionSource tailSource{
      .slotIndex = 3,
      .seqId = tailSlot.seqId,
      .hasPresent = false,
      .commandBegin = 0,
      .commandCount = 0,
  };
  auto tail = dxmt9::encoders::encodeChunk(
      ctx, tailSource.slotIndex, tailSlot,
      deferredOptions(*session, std::move(head->commandBuffer), tailSource));
  check(tail.has_value(), "tail encodeChunk returns a deferred submission");
  check(!head->commandBuffer,
        "tail encodeChunk consumes the head command-buffer carrier");
  checkEq(tail->commandBuffer.handle, commandBufferHandle,
          "tail submission carries the same command buffer");
  check(tail->explicitCompletionSourceSpan().empty(),
        "deferred tail still does not publish session sources");
  check(dxmt9::encoders::encodeChunkSessionHasDeferredSubmissionPayload(
            *session),
        "tail preserves the deferred clear until finalization");

  const auto sourcesBeforeFinalize =
      dxmt9::encoders::encodeChunkSessionSources(*session);
  checkEq(sourcesBeforeFinalize.size(), std::size_t{2},
          "both sources remain ordered in the session");
  checkEq(sourcesBeforeFinalize[0].seqId, headSource.seqId,
          "head remains first before finalization");
  checkEq(sourcesBeforeFinalize[1].seqId, tailSource.seqId,
          "tail remains second before finalization");

  check(dxmt9::encoders::finalizeEncodeChunkSessionIntoSubmission(
            ctx, *session, *tail),
        "finalizer publishes the deferred session");
  checkEq(tail->commandBuffer.handle, commandBufferHandle,
          "finalized submission keeps the command buffer");

  const auto published = tail->explicitCompletionSourceSpan();
  checkEq(published.size(), std::size_t{2},
          "finalizer publishes both completion sources");
  checkEq(published[0].slotIndex, headSource.slotIndex,
          "published head slot matches");
  checkEq(published[0].seqId, headSource.seqId,
          "published head sequence matches");
  check(!published[0].hasPresent,
        "published head remains non-present");
  checkEq(published[0].commandBegin, headSource.commandBegin,
          "published head command begin matches");
  checkEq(published[0].commandCount, headSource.commandCount,
          "published head command count matches");
  checkEq(published[1].slotIndex, tailSource.slotIndex,
          "published tail slot matches");
  checkEq(published[1].seqId, tailSource.seqId,
          "published tail sequence matches");
  check(!published[1].hasPresent,
        "published tail remains non-present");
  checkEq(published[1].commandBegin, tailSource.commandBegin,
          "published tail command begin matches");
  checkEq(published[1].commandCount, tailSource.commandCount,
          "published tail command count matches");

  check(dxmt9::encoders::encodeChunkSessionSources(*session).empty(),
        "successful finalization resets session sources");
  check(!dxmt9::encoders::encodeChunkSessionHasActiveRender(*session),
        "successful finalization leaves no active render encoder");
  check(!dxmt9::encoders::encodeChunkSessionHasDeferredSubmissionPayload(
            *session),
        "successful finalization clears deferred session payload");
}

}  // namespace

int main() {
  setenv("DXMT9_PERF_ENCODER_GPU_TIME", "0", 1);
  setenv("DXMT_METAL_CAPTURE_FRAME", "0", 1);
  try {
    encodeChunkThenFinalizerPublishesAndResetsOneSession();
  } catch (const TestFailure& error) {
    std::cerr << "encode_session_lifecycle_spec failed: " << error.what()
              << '\n';
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "encode_session_lifecycle_spec unexpected exception: "
              << error.what() << '\n';
    return 1;
  }
  return 0;
}
