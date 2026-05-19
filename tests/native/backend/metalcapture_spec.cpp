#include "../../../src/dxmt9/dxmt9_capture.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

void testDisabledControllerDoesNotCountFrames() {
  dxmt9::core::metalcapture::MetalCaptureController capture(
      dxmt9::core::metalcapture::MetalCaptureConfig{});
  check(!capture.enabled(), "default env-free capture controller is disabled");
  check(!capture.maybeCapturePresentChunk(1).has_value(),
        "disabled capture controller should not emit requests");
  check(capture.observedPresentFrames() == 0,
        "disabled capture controller should not perturb frame accounting");
}

void testSingleRequestedPresentFrame() {
  dxmt9::core::metalcapture::MetalCaptureController capture({
      .targetFrame = 2,
      .path = "/tmp/dxmt9-test.gputrace",
  });

  check(capture.enabled(), "explicit target frame enables capture controller");
  check(!capture.maybeCapturePresentChunk(10).has_value(),
        "first present should not match target frame 2");

  auto request = capture.maybeCapturePresentChunk(11);
  check(request.has_value(), "second present should request capture");
  check(request->frame == 2, "request should record selected present frame");
  check(request->seqId == 11, "request should preserve selected chunk seqId");
  check(request->path == "/tmp/dxmt9-test.gputrace", "request should preserve configured path");

  check(!capture.maybeCapturePresentChunk(12).has_value(),
        "capture controller should only request once");
  check(capture.observedPresentFrames() == 2,
        "capture controller should stop counting after one-shot request");
}

void testChunkBeginSessionStartsOnTargetFrameBoundary() {
  dxmt9::core::metalcapture::MetalCaptureController capture({
      .targetFrame = 3,
      .path = "/tmp/dxmt9-session.gputrace",
  });

  check(!capture.maybeCaptureAtChunkBegin(20).has_value(),
        "chunk-begin capture is not armed before prior presents");
  check(!capture.maybePresentChunkClosesSession(21).has_value(),
        "frame 1 present cannot close a session");
  check(capture.observedPresentFrames() == 1,
        "frame 1 present advances capture counter");
  check(!capture.maybeCaptureAtChunkBegin(22).has_value(),
        "frame 2 chunk-begin is still before target frame");

  check(!capture.maybePresentChunkClosesSession(23).has_value(),
        "frame 2 present arms the next chunk but does not close");
  check(capture.observedPresentFrames() == 2,
        "frame 2 present advances capture counter");

  auto request = capture.maybeCaptureAtChunkBegin(24);
  check(request.has_value(),
        "first chunk after frame 2 starts target-frame capture");
  check(request->frame == 3, "chunk-begin request records target frame");
  check(request->seqId == 24, "chunk-begin request records start seq");
  check(request->path == "/tmp/dxmt9-session.gputrace",
        "chunk-begin request preserves configured path");

  check(!capture.maybeCaptureAtChunkBegin(25).has_value(),
        "same frame does not start a second capture session");
}

void testChunkBeginSessionClosesOnTargetPresent() {
  dxmt9::core::metalcapture::MetalCaptureController capture({
      .targetFrame = 2,
      .path = "/tmp/dxmt9-session-close.gputrace",
  });

  check(!capture.maybePresentChunkClosesSession(30).has_value(),
        "frame 1 present arms frame 2 chunk-begin only");

  auto started = capture.maybeCaptureAtChunkBegin(31);
  check(started.has_value(), "frame 2 first chunk starts capture session");
  check(started->seqId == 31, "start request records first target chunk");

  auto closing = capture.maybePresentChunkClosesSession(32);
  check(closing.has_value(), "frame 2 present closes active capture session");
  check(closing->frame == started->frame,
        "closing request preserves captured frame");
  check(closing->seqId == started->seqId,
        "closing request preserves capture start seq");
  check(closing->path == started->path,
        "closing request preserves capture path");
  check(capture.observedPresentFrames() == 2,
        "target present advances counter exactly once");

  check(!capture.maybePresentChunkClosesSession(33).has_value(),
        "closed one-shot session cannot emit a second close request");
  check(capture.observedPresentFrames() == 3,
        "post-close presents still update observed frame counter");
}

void testDefaultPathContainsFrameAndSeq() {
  dxmt9::core::metalcapture::MetalCaptureController capture({
      .targetFrame = 1,
  });

  auto request = capture.maybeCapturePresentChunk(99);
  check(request.has_value(), "capture without explicit path should still request");
  check(request->path.find("dxmt9_frame_1_seq_99.gputrace") != std::string::npos,
        "default capture path should include frame and seq");
}

}  // namespace

int main() {
  try {
    testDisabledControllerDoesNotCountFrames();
    testSingleRequestedPresentFrame();
    testChunkBeginSessionStartsOnTargetFrameBoundary();
    testChunkBeginSessionClosesOnTargetPresent();
    testDefaultPathContainsFrameAndSeq();
  } catch (const TestFailure& failure) {
    std::cerr << "metalcapture_spec: " << failure.what() << "\n";
    return 1;
  }
  return 0;
}
