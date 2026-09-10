#include "render_tape_cpu_work_boundary.hpp"

#include "../../../src/d3d9/device_c_render_tape_completion.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using dxmt9::d3d9::RenderTapeCompletionDisposition;
using dxmt9::d3d9::RenderTapeCompletionSchedule;
using dxmt9::d3d9::RenderTapeEventType;
using dxmt9::test::releaseBeforeCpuTapeWork;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) throw TestFailure(std::string(message));
}

constexpr std::array<RenderTapeEventType, 7> kEvents{
    RenderTapeEventType::BootstrapState,
    RenderTapeEventType::ObjectDefine,
    RenderTapeEventType::ObjectDestroy,
    RenderTapeEventType::ResourceMutation,
    RenderTapeEventType::CommandChunk,
    RenderTapeEventType::OrderedControl,
    RenderTapeEventType::PresentComplete,
};

constexpr bool isDestroy(RenderTapeEventType event) noexcept {
  return event == RenderTapeEventType::ObjectDestroy;
}

constexpr bool isFrameWork(RenderTapeEventType event) noexcept {
  return !isDestroy(event) && event != RenderTapeEventType::PresentComplete;
}

struct QualifiedSource {
  RenderTapeCompletionSchedule::Key key{};
  std::uint32_t storageFirstPage = 0u;
  std::uint32_t storagePageCount = 0u;
  std::uint64_t storageGeneration = 0u;
};

struct BoundaryTraceModel {
  static constexpr std::size_t kSourceCount = 2u;

  RenderTapeCompletionSchedule schedule{};
  std::array<QualifiedSource, kSourceCount> sources{
      QualifiedSource{
          .key = {.iteration = 1u,
                  .submission = 1u,
                  .source = 17u,
                  .actionOrdinal = 0u,
                  .sourceGeneration = 101u},
          .storageFirstPage = 31u,
          .storagePageCount = 2u,
          .storageGeneration = 1001u},
      QualifiedSource{
          .key = {.iteration = 1u,
                  .submission = 2u,
                  .source = 17u,
                  .actionOrdinal = 0u,
                  .sourceGeneration = 202u},
          .storageFirstPage = 47u,
          .storagePageCount = 2u,
          .storageGeneration = 2002u},
  };
  std::size_t submitted = 0u;
  std::size_t released = 0u;
  std::size_t currentSource = 0u;
  std::size_t destroyedWithPending = 0u;
  bool workWhilePending = false;
  bool frameClosedWithPending = false;
  std::array<std::size_t, kSourceCount> releasedAtEvent{
      {static_cast<std::size_t>(-1), static_cast<std::size_t>(-1)}};

  BoundaryTraceModel() {
    check(sources[0].key.source == sources[1].key.source &&
              sources[0].key.sourceGeneration !=
                  sources[1].key.sourceGeneration &&
              sources[0].storageGeneration != sources[1].storageGeneration,
          "sequential sources are qualified by source and storage generations");
  }

  bool pending() const noexcept { return submitted != released; }

  void submitSource(std::size_t sourceIndex, bool beforeNextWork,
                    std::size_t eventIndex) {
    check(sourceIndex == submitted && sourceIndex < sources.size() &&
              !pending(),
          "only one source may be outstanding at a time");
    currentSource = sourceIndex;
    schedule = {};
    check(schedule.append({sources[sourceIndex].key}).accepted() &&
              schedule.beginIteration(1u).accepted() &&
              schedule.submit(sources[sourceIndex].key, [] { return true; })
                  .accepted(),
          "fresh schedule admits the next qualified source");
    ++submitted;
    if (!beforeNextWork) {
      // Immediate mode is the existing post-command-chunk completion point.
      releaseCurrent(eventIndex);
    }
  }

  void releaseCurrent(std::size_t eventIndex) {
    check(pending() && submitted == released + 1u,
          "completion model has exactly one current source");
    check(schedule.release(sources[currentSource].key).accepted(),
          "completion release acknowledges the current qualified source");
    releasedAtEvent[currentSource] = eventIndex;
    ++released;
    check(schedule.release(sources[currentSource].key).disposition ==
              RenderTapeCompletionDisposition::DuplicateRelease,
          "a current source cannot be acknowledged twice");
    check(schedule.finishIteration().accepted(),
          "released source schedule reaches its terminal frontier");
  }

  void apply(RenderTapeEventType event, bool beforeNextWork,
             std::size_t eventIndex) {
    const auto pendingBefore = pending();
    if (releaseBeforeCpuTapeWork(event, beforeNextWork) && pending()) {
      releaseCurrent(eventIndex);
    }

    if (isFrameWork(event) && pending()) {
      workWhilePending = true;
    }
    if (isDestroy(event)) {
      // Destruction is the one event allowed to preserve a pending source.
      if (pendingBefore) ++destroyedWithPending;
      check(pending() == pendingBefore,
            "ObjectDestroy never releases a pending completion");
    }

    if (event == RenderTapeEventType::PresentComplete && pending()) {
      frameClosedWithPending = true;
    }
  }

  bool finishCurrent() {
    return schedule.finishIteration().accepted();
  }
};

void testPolicyTruthTable() {
  for (const auto event : kEvents) {
    check(!releaseBeforeCpuTapeWork(event, false),
          "immediate policy never releases before an event");
    check(releaseBeforeCpuTapeWork(event, true) == !isDestroy(event),
          "BeforeNextWork releases before every work or frame closure event");
  }
}

void testDestroyLeavesResidualScheduleUnresolved() {
  BoundaryTraceModel model;
  model.submitSource(0u, true, 0u);
  model.apply(RenderTapeEventType::ObjectDestroy, true, 1u);
  model.apply(RenderTapeEventType::ObjectDestroy, true, 2u);
  check(model.destroyedWithPending == 2u,
        "destruction events are legal with the current source pending");
  check(model.released == 0u,
        "destruction alone performs no completion acknowledgment");
  check(!model.finishCurrent(), "terminal delayed residual is rejected");

  BoundaryTraceModel immediate;
  immediate.submitSource(0u, false, 0u);
  immediate.apply(RenderTapeEventType::ObjectDestroy, false, 1u);
  check(!immediate.pending(),
        "immediate post-command completion leaves no pending source");
}

void testNoPrematureOrDuplicateRelease() {
  BoundaryTraceModel model;
  model.submitSource(0u, true, 0u);
  model.apply(RenderTapeEventType::ObjectDestroy, true, 1u);
  check(model.pending() && model.released == 0u,
        "destroy preserves the current qualified source");

  model.apply(RenderTapeEventType::CommandChunk, true, 2u);
  check(!model.pending() && model.released == 1u &&
            model.releasedAtEvent[0] == 2u,
        "next work releases the prior source before entering the chunk");

  model.submitSource(1u, true, 3u);
  model.apply(RenderTapeEventType::PresentComplete, true, 4u);
  check(!model.pending() && model.released == 2u &&
            model.releasedAtEvent[1] == 4u &&
            !model.frameClosedWithPending,
        "PresentComplete releases the next source before frame closure");
  check(model.sources[0].key.sourceGeneration !=
            model.sources[1].key.sourceGeneration &&
            model.sources[0].storageGeneration !=
                model.sources[1].storageGeneration,
        "sequential reuse remains generation-qualified");
}

void testExhaustiveBoundedTraces(bool beforeNextWork) {
  constexpr std::array<RenderTapeEventType, 6> kWorkEvents{
      RenderTapeEventType::BootstrapState,
      RenderTapeEventType::ObjectDefine,
      RenderTapeEventType::ObjectDestroy,
      RenderTapeEventType::ResourceMutation,
      RenderTapeEventType::CommandChunk,
      RenderTapeEventType::OrderedControl,
  };
  constexpr std::size_t kEventsPerSource = 2u;
  constexpr std::size_t kTraceCount =
      kWorkEvents.size() * kWorkEvents.size() * kWorkEvents.size() *
      kWorkEvents.size();
  std::size_t accepted = 0u;
  std::size_t residual = 0u;
  std::size_t destroyedPending = 0u;

  for (std::size_t encoded = 0u; encoded < kTraceCount; ++encoded) {
    auto value = encoded;
    std::array<RenderTapeEventType, kEventsPerSource * 2u> trace{};
    for (auto& event : trace) {
      event = kWorkEvents[value % kWorkEvents.size()];
      value /= kWorkEvents.size();
    }

    BoundaryTraceModel model;
    model.submitSource(0u, beforeNextWork, 0u);
    for (std::size_t index = 0u; index < kEventsPerSource; ++index) {
      model.apply(trace[index], beforeNextWork, index);
      if (beforeNextWork && isFrameWork(trace[index])) {
        check(!model.workWhilePending,
              "BeforeNextWork never enters mutating work with a pending source");
      }
    }
    destroyedPending += model.destroyedWithPending;
    if (model.pending()) {
      check(!model.finishCurrent(),
            "a source left pending before reuse is terminally unresolved");
      ++residual;
      continue;
    }

    model.submitSource(1u, beforeNextWork, kEventsPerSource);
    for (std::size_t index = kEventsPerSource; index < trace.size(); ++index) {
      model.apply(trace[index], beforeNextWork, index);
      if (beforeNextWork && isFrameWork(trace[index])) {
        check(!model.workWhilePending,
              "BeforeNextWork never enters mutating work with a pending source");
      }
    }
    model.apply(RenderTapeEventType::PresentComplete, beforeNextWork,
                trace.size());
    check(!model.frameClosedWithPending,
          "PresentComplete closes only after prior source completion");
    destroyedPending += model.destroyedWithPending;
    if (model.pending()) {
      check(!model.finishCurrent(),
            "a residual second source is rejected at frame termination");
      ++residual;
    } else {
      check(model.released == 2u,
            "sequential exhaustive trace releases both qualified sources");
      ++accepted;
    }
  }

  check(accepted != 0u &&
            (beforeNextWork ? residual != 0u : residual == 0u),
        "bounded exhaustive traces classify complete and residual paths");
  check(beforeNextWork ? destroyedPending != 0u : destroyedPending == 0u,
        "bounded traces classify destruction with a pending source");
}

void testMissingPreWorkReleaseCounterexample() {
  BoundaryTraceModel mutant;
  mutant.submitSource(0u, true, 0u);
  // Deliberately use the immediate policy's no-pre-event release after a
  // delayed submission. This is the integration error the entry guard must
  // reject, not a legal alternative completion schedule.
  mutant.apply(RenderTapeEventType::ResourceMutation, false, 1u);
  check(mutant.workWhilePending && mutant.pending() && !mutant.finishCurrent(),
        "omitted delayed release exposes pending work and unresolved termination");
}

}  // namespace

int main() {
  try {
    testPolicyTruthTable();
    testDestroyLeavesResidualScheduleUnresolved();
    testNoPrematureOrDuplicateRelease();
    testExhaustiveBoundedTraces(false);
    testExhaustiveBoundedTraces(true);
    testMissingPreWorkReleaseCounterexample();
  } catch (const TestFailure& failure) {
    std::cerr << "render_tape_cpu_work_boundary_spec failed: "
              << failure.what() << '\n';
    return 1;
  } catch (const std::exception& ex) {
    std::cerr << "render_tape_cpu_work_boundary_spec unexpected exception: "
              << ex.what() << '\n';
    return 1;
  }
  std::cout << "render_tape_cpu_work_boundary_spec passed\n";
  return 0;
}
