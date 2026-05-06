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
    testDefaultPathContainsFrameAndSeq();
  } catch (const TestFailure& failure) {
    std::cerr << "metalcapture_spec: " << failure.what() << "\n";
    return 1;
  }
  return 0;
}
