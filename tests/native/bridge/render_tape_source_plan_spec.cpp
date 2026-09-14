#include "device_c_render_tape_source_plan.hpp"

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace dxmt9::d3d9;

struct Failure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) throw Failure(std::string(message));
}

void rejectsEmptyInputWithoutPublishing() {
  render_tape_runtime::GeneralPreflightPlan plan;
  CaptureSourceReplayPlan output;
  const auto status = buildCaptureSourceReplayPlan({}, plan, {}, output);
  check(status == CaptureSourceReplayPlanStatus::InvalidInput,
        "empty source-plan input is rejected");
  check(!output.valid() && output.status() ==
                              CaptureSourceReplayPlanStatus::InvalidInput,
        "failed source-plan build leaves the destination unchanged");
}

void rejectsUnauthenticatedTapeWithoutPublishing() {
  render_tape_runtime::GeneralPreflightPlan plan;
  plan.commandChunks.resize(1u);
  plan.events.push_back({.type = RenderTapeEventType::CommandChunk,
                         .eventIndex = 0u,
                         .index = 0u});
  CaptureSourceReplayPlan output;
  const auto status = buildCaptureSourceReplayPlan(
      std::vector<std::byte>{std::byte{0x7f}}, plan,
      std::vector<std::byte>{std::byte{0x01}}, output);
  check(status == CaptureSourceReplayPlanStatus::InvalidIdentity,
        "unauthenticated tape is rejected before planning");
  check(!output.valid() && output.rows().empty() &&
            output.rangesForCommandChunk(0u).empty(),
        "invalid identity cannot publish partial rows or cuts");
}

void statusNamesAreStable() {
  check(std::string_view(captureSourceReplayPlanStatusName(
            CaptureSourceReplayPlanStatus::Valid)) == "valid",
        "valid status has a stable diagnostic name");
  check(std::string_view(captureSourceReplayPlanStatusName(
            CaptureSourceReplayPlanStatus::SourceCoverageMismatch)) ==
            "source-coverage-mismatch",
        "source coverage status has a stable diagnostic name");
}

void movedFromPlanIsNotAuthoritative() {
  CaptureSourceReplayPlan source;
  CaptureSourceReplayPlan destination(std::move(source));
  check(!source.valid() && !destination.valid(),
        "a moved-from source plan cannot retain valid authority");
  CaptureSourceReplayPlan assigned;
  assigned = std::move(destination);
  check(!destination.valid() && !assigned.valid(),
        "move assignment also closes moved-from plan authority");
}

}  // namespace

int main() {
  try {
    rejectsEmptyInputWithoutPublishing();
    rejectsUnauthenticatedTapeWithoutPublishing();
    statusNamesAreStable();
    movedFromPlanIsNotAuthoritative();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "render tape source plan spec passed\n";
  return 0;
}
