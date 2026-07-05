# Commit-Chunk Replay Offload Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move the ~8.5 ms/present commit-chunk record replay off the app thread onto a dedicated replay worker behind `DXMT9_OFFLOAD_COMMIT_REPLAY=1` (default off, byte-identical off path), re-anchoring present frame-latency pacing with an app-side present-ordinal wait.

**Architecture:** Keep validation/import/handle-marking synchronous in `dxmt9c_device_commit_chunk` (lines 2035–2233 of `src/d3d9/device_c_chunk_replay.cpp`, ~0.42 ms/present — preserves synchronous generation-check HRESULTs and resource retention before the app can release). Extract the replay phase (2234–3013, ~8.5 ms/present) into a reusable function; under the flag, copy the wire blob into a unix-owned `RawChunk` (with wrapper strong refs), push to a bounded queue consumed FIFO by a device-owned worker thread that calls the same extracted function. Present-bearing commits run an ordinal frame-latency wait on the app thread; `CommandQueue::submitPresent` suppresses its own boundary under the flag. Direct device calls get a drain-fence prologue in the bridge-wrapper layer.

**Tech Stack:** C++20 (src/d3d9 device_c layer + src/dxmt9 queue), Meson/Ninja, native specs in `tests/native/backend/`, TLA+ (`PresentFrameLatency`), dxmt9 perf-counter system with table/callsite audits.

## Global Constraints

- Spec: `docs/superpowers/specs/2026-07-05-commit-replay-offload-design.md`. Off path must stay byte-identical; env `DXMT9_OFFLOAD_COMMIT_REPLAY` read once at first use.
- 2-space indent, `#pragma once`, match local file style; no mass reformatting (agents/rules/codebase_conventions.rules.md).
- Every new `Counters` field needs a `kCounterTable` row and every new `count*()` decl needs a production callsite — enforced by `scripts/check/audit_perf_counter_table.py` / `audit_perf_counter_callsites.py` (meson tests).
- Queue changes require `dxmt9-verify-tla` green plus the backend native suite in `build-arm64-nowine`.
- 3DMark05 runs only via `run_3dmark05_perf_probe.sh` with positive timeout (metal_debugging rules); Wine runtime stays the manifest default.
- Commits: single-line imperative subject + the session's `Co-Authored-By` / `Claude-Session` trailers.
- PE side and the winemetal ABI are untouched — no abi-hash regen, no generated-dispatch edits.

## File Structure

- `src/d3d9/device_c_chunk_replay.cpp` — split entry into sync import + extracted `replayImportedChunk`; offload branch in `dxmt9c_device_commit_chunk`.
- `src/d3d9/device_c_replay_offload.hpp` / `.cpp` (new) — `RawCommandChunk`, `ReplayOffloadQueue` (bounded, drainable), `ReplayOffloadWorker`, env resolver, drain-fence helper.
- `src/d3d9/device_c_common.hpp` — `D9CDevice` gains `std::unique_ptr<dxmt9::d3d9::ReplayOffload> replayOffload;` + present-ordinal counter.
- `src/d3d9/device_c_bridge_device_state_draw.cpp` — drain-fence prologue on device exports.
- `src/dxmt9/dxmt9_command_queue.{hpp,cpp}` — `completedPresentOrdinal_`, `waitPresentOrdinalBoundary`, ordinal target math, boundary suppression in `submitPresent`.
- `src/dxmt9/dxmt9_queue.{hpp,cpp}` — `SubmissionBinding.completedPresentOrdinal` + increment in `drainCompletedSequence`.
- `src/dxmt9/dxmt9_device.cpp` + the `dxmt9::Device` interface header — `waitPresentOrdinalBoundary` plumbing for the frontend.
- `src/dxmt9/dxmt9_perf_counters.{hpp,cpp}` — new counter family.
- `tests/native/backend/replay_offload_queue_spec.cpp`, `present_ordinal_boundary_spec.cpp` (new) + `tests/native/backend/meson.build`.
- `specs/verification/tla/PresentFrameLatency.tla/.cfg` — ordinal variant.
- Docs: `agents/rules/environment_variables_bridge.rules.md`, `specs/backend/design.md`, knowledge-graph leaf + overview rows (proof task).

---

### Task 1: Extract the replay phase into `replayImportedChunk`

Pure mechanical split, flag-independent; behavior must be byte-identical.

**Files:**
- Modify: `src/d3d9/device_c_chunk_replay.cpp:2234-3013`

**Interfaces:**
- Produces: file-local `int32_t replayImportedChunk(D9CDevice* d, const ImportedWireChunkView& importedChunk, std::chrono::steady_clock::time_point bridgeCommitStart, std::chrono::steady_clock::time_point replayStageStart);` — runs everything currently between line 2234 (`noteCommitChunkReplayStartForCompletionGap`) and line 3012 (`return dxmt9::core::D3D_OK;`), including the pending-draw lambdas (2238–2407), the record loop (2409–2981), chunk-end flush (2983–2990), stats close-out (2991–3011). Later tasks call it from the worker.

- [ ] **Step 1: Perform the extraction.** In `device_c_chunk_replay.cpp`, define (above `dxmt9c_device_commit_chunk`, in the same anonymous/file scope as the other helpers):

```cpp
int32_t replayImportedChunk(D9CDevice* d,
                            const ImportedWireChunkView& importedChunk,
                            std::chrono::steady_clock::time_point bridgeCommitStart,
                            std::chrono::steady_clock::time_point replayStageStart) {
  auto commitChunkStageStart = replayStageStart;
  // ... body moved verbatim from lines 2234-3012 ...
}
```

Move lines 2234–3012 verbatim into it (the `noteCommitChunkReplayStartForCompletionGap` block, scratch setup, the four lambdas, the while loop, end flush, truncated-records guard, stats emission, `countBridgeCommitLatencyNs(bridgeCommitStart)`, final `return dxmt9::core::D3D_OK;`). The moved code references `d`, `importedChunk`, `bridgeCommitStart`, `commitChunkStageStart` only — all provided. In `dxmt9c_device_commit_chunk`, after the handle-import stats at line ~2233, replace the moved body with:

```cpp
  return replayImportedChunk(d, importedChunk, bridgeCommitStart,
                             commitChunkStageStart);
```

Keep the `ResetSkipDrawMarkGuard resetGuard` (currently ~2211–2228) in the CALLER if its RAII scope must cover replay — check: it un-sets `setSkipDrawResourceMarking` at scope exit, and replay's draw submissions rely on it being active. Move the guard INTO `replayImportedChunk` (declare it first in the new function, before the scratch setup) and pass `didBulkMarkResources` as a fifth parameter `bool skipDrawResourceMarking`:

```cpp
int32_t replayImportedChunk(D9CDevice* d,
                            const ImportedWireChunkView& importedChunk,
                            bool skipDrawResourceMarking,
                            std::chrono::steady_clock::time_point bridgeCommitStart,
                            std::chrono::steady_clock::time_point replayStageStart) {
  ResetSkipDrawMarkGuard resetGuard(d, skipDrawResourceMarking);
  ...
```

(Adjust the guard's constructor usage to match its current definition at ~2211 — preserve exact semantics: guard active only when bulk marking succeeded.)

- [ ] **Step 2: Build and run the focused suites**

```bash
ninja -C build-arm64-nowine && \
meson test -C build-arm64-nowine dxmt9-queue-completion-sources-spec \
  dxmt9-chunk-record-micro-spec dxmt9-dod-replay-observer-spec --print-errorlogs
```

Expected: build OK, all listed tests OK.

- [ ] **Step 3: Run the full backend directory suite**

```bash
meson test -C build-arm64-nowine $(rg -o "test\('([^']+)'" -r '$1' tests/native/backend/meson.build | tr '\n' ' ')
```

Expected: all OK.

- [ ] **Step 4: Commit**

```bash
git add src/d3d9/device_c_chunk_replay.cpp
git commit -m "d3d9: extract commit-chunk replay phase"
```

---

### Task 2: RawCommandChunk + bounded ReplayOffloadQueue (pure, TDD)

**Files:**
- Create: `src/d3d9/device_c_replay_offload.hpp`, `src/d3d9/device_c_replay_offload.cpp`
- Modify: `src/d3d9/meson.build` (add the .cpp to the provider library sources next to `device_c_chunk_replay.cpp`)
- Test: `tests/native/backend/replay_offload_queue_spec.cpp` (new) + `tests/native/backend/meson.build`

**Interfaces:**
- Produces (namespace `dxmt9::d3d9`):

```cpp
struct RawCommandChunk {
  std::vector<dxmt9::core::u8> recordBlob;   // owned copy of chunk->records
  uint32_t recordCount = 0;
  uint32_t recordBytes = 0;
  bool hasPresent = false;
  bool skipDrawResourceMarking = false;
  std::vector<void*> retainedWrappers;        // opaque addref'd wrapper ptrs
  std::chrono::steady_clock::time_point bridgeCommitStart{};
};

class ReplayOffloadQueue {
 public:
  explicit ReplayOffloadQueue(std::size_t maxChunks, std::size_t maxBytes);
  bool push(RawCommandChunk&& chunk);       // blocks while full; false if stopped
  bool pop(RawCommandChunk& out);           // blocks while empty; false if stopped+empty
  void waitDrained();                        // blocks until empty AND no chunk in flight
  void markReplayDone();                     // worker calls after finishing a popped chunk
  void stop();                               // wake all; push/pop return false
  bool stopped() const;
  std::size_t depth() const;
};
```

`pop` marks a chunk in flight; `waitDrained` returns only when `depth()==0` and no in-flight chunk (drain-fence + shutdown primitive).

- [ ] **Step 1: Write the failing spec** `tests/native/backend/replay_offload_queue_spec.cpp` (same idiom as `queue_completion_sources_spec.cpp`: local `check`/`TestFailure`, `main` with try/catch):

```cpp
// Pure-data spec for dxmt9::d3d9::ReplayOffloadQueue — bounded FIFO with a
// drain fence used by the commit-replay offload path.
#include "../../../src/d3d9/device_c_replay_offload.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {
struct TestFailure : std::runtime_error { using std::runtime_error::runtime_error; };
void check(bool c, std::string_view m) { if (!c) throw TestFailure(std::string(m)); }

dxmt9::d3d9::RawCommandChunk makeChunk(uint32_t bytes) {
  dxmt9::d3d9::RawCommandChunk c;
  c.recordBlob.resize(bytes);
  c.recordBytes = bytes;
  c.recordCount = 1;
  return c;
}

void testFifoPushPop() {
  dxmt9::d3d9::ReplayOffloadQueue q(4, 1 << 20);
  check(q.push(makeChunk(16)), "push 1");
  check(q.push(makeChunk(32)), "push 2");
  dxmt9::d3d9::RawCommandChunk out;
  check(q.pop(out) && out.recordBytes == 16, "fifo order 1");
  q.markReplayDone();
  check(q.pop(out) && out.recordBytes == 32, "fifo order 2");
  q.markReplayDone();
  check(q.depth() == 0, "drained depth");
}

void testDrainWaitsForInFlight() {
  dxmt9::d3d9::ReplayOffloadQueue q(4, 1 << 20);
  check(q.push(makeChunk(8)), "push");
  dxmt9::d3d9::RawCommandChunk out;
  check(q.pop(out), "pop");
  bool drained = false;
  std::thread waiter([&] { q.waitDrained(); drained = true; });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  check(!drained, "drain must wait for in-flight chunk");
  q.markReplayDone();
  waiter.join();
  check(drained, "drain released after markReplayDone");
}

void testBoundedPushBlocksUntilPop() {
  dxmt9::d3d9::ReplayOffloadQueue q(1, 1 << 20);
  check(q.push(makeChunk(8)), "push fills bound");
  bool pushed = false;
  std::thread producer([&] { pushed = q.push(makeChunk(8)); });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  check(!pushed, "push blocks while full");
  dxmt9::d3d9::RawCommandChunk out;
  check(q.pop(out), "pop frees slot");
  q.markReplayDone();
  producer.join();
  check(pushed, "blocked push completes");
}

void testStopReleasesEverything() {
  dxmt9::d3d9::ReplayOffloadQueue q(1, 1 << 20);
  std::thread popper([&] {
    dxmt9::d3d9::RawCommandChunk out;
    check(!q.pop(out), "pop returns false after stop with empty queue");
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  q.stop();
  popper.join();
  check(!q.push(makeChunk(8)), "push refused after stop");
}
}  // namespace

int main() {
  try {
    testFifoPushPop();
    testDrainWaitsForInFlight();
    testBoundedPushBlocksUntilPop();
    testStopReleasesEverything();
  } catch (const TestFailure& e) {
    std::cerr << "replay_offload_queue_spec failed: " << e.what() << '\n';
    return 1;
  }
  std::cout << "replay_offload_queue_spec passed\n";
  return 0;
}
```

Register in `tests/native/backend/meson.build` (copy the `present_boundary_policy_spec` block shape):

```meson
replay_offload_queue_spec = executable(
  'dxmt9-replay-offload-queue-spec',
  'replay_offload_queue_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-replay-offload-queue-spec', replay_offload_queue_spec)
```

(Match the exact dependency/env argument shape used by the neighboring blocks in that file at HEAD.)

- [ ] **Step 2: Run to verify failure**

```bash
ninja -C build-arm64-nowine 2>&1 | tail -3
```

Expected: compile FAILURE — `device_c_replay_offload.hpp` not found.

- [ ] **Step 3: Implement.** `src/d3d9/device_c_replay_offload.hpp`:

```cpp
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include "dxmt9/types.h"  // or the local u8 alias header used by neighbors — match device_c_record_validate.cpp's include for dxmt9::core::u8

namespace dxmt9::d3d9 {

struct RawCommandChunk {
  std::vector<dxmt9::core::u8> recordBlob;
  uint32_t recordCount = 0;
  uint32_t recordBytes = 0;
  bool hasPresent = false;
  bool skipDrawResourceMarking = false;
  std::vector<void*> retainedWrappers;
  std::chrono::steady_clock::time_point bridgeCommitStart{};
};

class ReplayOffloadQueue {
 public:
  ReplayOffloadQueue(std::size_t maxChunks, std::size_t maxBytes)
      : maxChunks_(maxChunks), maxBytes_(maxBytes) {}

  bool push(RawCommandChunk&& chunk) {
    std::unique_lock lock(mutex_);
    spaceCv_.wait(lock, [&] {
      return stop_ || (queue_.size() < maxChunks_ &&
                       queuedBytes_ + chunk.recordBytes <= maxBytes_);
    });
    if (stop_) return false;
    queuedBytes_ += chunk.recordBytes;
    queue_.push_back(std::move(chunk));
    workCv_.notify_one();
    return true;
  }

  bool pop(RawCommandChunk& out) {
    std::unique_lock lock(mutex_);
    workCv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
    if (queue_.empty()) return false;  // stop_ with empty queue
    out = std::move(queue_.front());
    queue_.pop_front();
    queuedBytes_ -= out.recordBytes;
    inFlight_ = true;
    spaceCv_.notify_all();
    return true;
  }

  void markReplayDone() {
    std::lock_guard lock(mutex_);
    inFlight_ = false;
    drainCv_.notify_all();
  }

  void waitDrained() {
    std::unique_lock lock(mutex_);
    drainCv_.wait(lock, [&] { return queue_.empty() && !inFlight_; });
  }

  void stop() {
    std::lock_guard lock(mutex_);
    stop_ = true;
    workCv_.notify_all();
    spaceCv_.notify_all();
    drainCv_.notify_all();
  }

  bool stopped() const {
    std::lock_guard lock(mutex_);
    return stop_;
  }

  std::size_t depth() const {
    std::lock_guard lock(mutex_);
    return queue_.size() + (inFlight_ ? 1 : 0);
  }

 private:
  const std::size_t maxChunks_;
  const std::size_t maxBytes_;
  mutable std::mutex mutex_;
  std::condition_variable workCv_;
  std::condition_variable spaceCv_;
  std::condition_variable drainCv_;
  std::deque<RawCommandChunk> queue_;
  std::size_t queuedBytes_ = 0;
  bool inFlight_ = false;
  bool stop_ = false;
};

}  // namespace dxmt9::d3d9
```

Note: `waitDrained` must also observe in-flight chunks popped before the wait started — covered by `inFlight_`. One nuance the tests pin: `drainCv_` must also be notified when the queue empties via `pop` if nothing is in flight — it is, indirectly, because `pop` sets `inFlight_=true` and the subsequent `markReplayDone` notifies. Header-only is fine; create `device_c_replay_offload.cpp` only if the worker (Task 4) needs a TU — if empty at this task, skip the .cpp and the meson.build source addition until Task 4.

- [ ] **Step 4: Run tests to verify pass**

```bash
ninja -C build-arm64-nowine && \
meson test -C build-arm64-nowine dxmt9-replay-offload-queue-spec --print-errorlogs
```

Expected: `1/1 OK`.

- [ ] **Step 5: Commit**

```bash
git add src/d3d9/device_c_replay_offload.hpp tests/native/backend/replay_offload_queue_spec.cpp tests/native/backend/meson.build
git commit -m "d3d9: add bounded replay offload queue"
```

---

### Task 3: Present-ordinal boundary (queue side, TDD)

**Files:**
- Modify: `src/dxmt9/dxmt9_command_queue.hpp` (ordinal math near `kMaxQueuedChunks` at :70; members near `presentCompletedSeqId_` :444; method decls near `presentBoundary` :215)
- Modify: `src/dxmt9/dxmt9_command_queue.cpp` (`submitPresent` :3885-4015 suppression; new `waitPresentOrdinalBoundary`; `bindSelfLifecycle` :5020)
- Modify: `src/dxmt9/dxmt9_queue.hpp` (`SubmissionBinding` :575-595 new field), `src/dxmt9/dxmt9_queue.cpp` (`drainCompletedSequence` :1811-1819 increment)
- Modify: `src/dxmt9/dxmt9_perf_counters.hpp/.cpp` (counters), `src/dxmt9/dxmt9_device.cpp` + `dxmt9::Device` interface (frontend plumbing)
- Modify: `specs/verification/tla/PresentFrameLatency.tla` + `.cfg`
- Test: `tests/native/backend/present_ordinal_boundary_spec.cpp` (new) + meson.build

**Interfaces:**
- Produces:
  - `dxmt9_command_queue.hpp` (namespace `dxmt9`, after `kMaxQueuedChunks`): `inline std::uint64_t presentOrdinalBoundaryTarget(std::uint64_t presentOrdinal, std::uint32_t maxFrameLatency)` — same math as the cpp-local `presentBoundaryTargetSeqId` (`dxmt9_command_queue.cpp:2647`): 0 for ordinal 0; clamp latency to `[1, kMaxQueuedChunks]`; 0 while `presentOrdinal <= latency`; else `presentOrdinal - latency`.
  - `CommandQueue::waitPresentOrdinalBoundary(std::uint64_t presentOrdinal, std::uint32_t maxFrameLatency)` — honors `resolveBoundaryPolicyFromEnv()`: `Disabled` → return; `DeferredPresentCompletion` → wait the previously stored deferred ordinal target then store `presentOrdinalBoundaryTarget(presentOrdinal + 1, latency)`; otherwise wait now on `presentCompletedCv_` until `completedPresentOrdinal_ >= presentOrdinalBoundaryTarget(presentOrdinal, latency)` or `stop_`.
  - `CommandQueue::offloadSuppressPresentBoundary()` — env-derived static; when true, `submitPresent` skips lines 3890-3893 and takes the `countPresentBoundarySkipped()` branch instead of 4005-4014.
  - Frontend: `dxmt9::Device::waitPresentOrdinalBoundary(std::uint64_t ordinal)` virtual (latency from `DeviceImpl::maxFrameLatency_`, mirroring `present()` at `dxmt9_device.cpp:296-306`), so `src/d3d9` can call `d->dev().upperDevice()->waitPresentOrdinalBoundary(n)` exactly like `markChunkResources` at `device_c_chunk_replay.cpp:2203`.
  - Counters: `present_ordinal_boundary_waits` (UnsignedCount), `present_ordinal_boundary_wait_ms` (nanos-accumulating like `present_boundary_wait_ms` — copy that row's Kind), `completed_present_ordinal` (UnsignedCount, incremented per retired present).

- [ ] **Step 1: Write the failing spec** `tests/native/backend/present_ordinal_boundary_spec.cpp` (pure math + policy mapping; same check idiom):

```cpp
// Pure spec for dxmt9::presentOrdinalBoundaryTarget — the app-side ordinal
// frame-latency target used by the commit-replay offload path.
#include "../../../src/dxmt9/dxmt9_command_queue.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
struct TestFailure : std::runtime_error { using std::runtime_error::runtime_error; };
void check(bool c, std::string_view m) { if (!c) throw TestFailure(std::string(m)); }

void testOrdinalTargetMath() {
  using dxmt9::kMaxQueuedChunks;
  using dxmt9::presentOrdinalBoundaryTarget;
  check(presentOrdinalBoundaryTarget(0, 3) == 0, "ordinal 0 never waits");
  check(presentOrdinalBoundaryTarget(3, 3) == 0, "inside latency window");
  check(presentOrdinalBoundaryTarget(4, 3) == 1, "first past-window waits on 1");
  check(presentOrdinalBoundaryTarget(2, 0) == 1, "latency clamps up to 1");
  check(presentOrdinalBoundaryTarget(1000, 64) ==
            1000 - static_cast<std::uint64_t>(kMaxQueuedChunks),
        "latency clamps down to kMaxQueuedChunks");
}

void testOrdinalTargetMatchesSeqIdShape() {
  using dxmt9::presentOrdinalBoundaryTarget;
  // The ordinal math must be the exact shape of the boundary seqId math so
  // the offload pacing is order-isomorphic to the inline boundary.
  for (std::uint64_t n : {1ull, 2ull, 5ull, 31ull, 32ull, 100ull}) {
    const auto t = presentOrdinalBoundaryTarget(n, 3);
    check(t == (n <= 3 ? 0 : n - 3), "target follows n - latency with floor");
  }
}
}  // namespace

int main() {
  try {
    testOrdinalTargetMath();
    testOrdinalTargetMatchesSeqIdShape();
  } catch (const TestFailure& e) {
    std::cerr << "present_ordinal_boundary_spec failed: " << e.what() << '\n';
    return 1;
  }
  std::cout << "present_ordinal_boundary_spec passed\n";
  return 0;
}
```

Meson registration: copy the Task 2 block with names `present_ordinal_boundary_spec` / `dxmt9-present-ordinal-boundary-spec`.

- [ ] **Step 2: Run to verify failure** — `ninja -C build-arm64-nowine` → compile error: `presentOrdinalBoundaryTarget` is not a member of `dxmt9`.

- [ ] **Step 3: Implement the header math** in `dxmt9_command_queue.hpp` right after `kMaxQueuedChunks` (:70):

```cpp
// App-side present-ordinal frame-latency target for the commit-replay
// offload path (TLA+: PresentFrameLatency ordinal variant). Ordinals count
// present-bearing commits 1,2,3...; the math is the exact shape of the
// boundary seqId target so pacing stays order-isomorphic to the inline
// present boundary.
inline std::uint64_t presentOrdinalBoundaryTarget(std::uint64_t presentOrdinal,
                                                  std::uint32_t maxFrameLatency) {
  if (presentOrdinal == 0) {
    return 0;
  }
  const std::uint64_t latency = std::clamp<std::uint64_t>(
      maxFrameLatency, 1u, kMaxQueuedChunks);
  if (presentOrdinal <= latency) {
    return 0;
  }
  return presentOrdinal - latency;
}
```

(Add `#include <algorithm>` / `<cstdint>` to the header if missing.)

- [ ] **Step 4: Run the spec** — expected PASS. Commit checkpoint:

```bash
git add src/dxmt9/dxmt9_command_queue.hpp tests/native/backend/present_ordinal_boundary_spec.cpp tests/native/backend/meson.build
git commit -m "backend: add present-ordinal boundary target math"
```

- [ ] **Step 5: Wire the ordinal counter through the queue.**
  1. `dxmt9_command_queue.hpp`: next to `presentCompletedSeqId_` (:444) add `std::uint64_t completedPresentOrdinal_ = 0;  // presents retired (offload ordinal wait)` and next to `deferredPresentBoundaryTargetSeqId_` add `std::uint64_t deferredPresentOrdinalTarget_ = 0;`. Method decls next to `presentBoundary` (:215): `void waitPresentOrdinalBoundary(std::uint64_t presentOrdinal, std::uint32_t maxFrameLatency);`.
  2. `dxmt9_queue.hpp` `SubmissionBinding` (:575-595): add `std::uint64_t* completedPresentOrdinal = nullptr;` after `presentCompletedSeqId`.
  3. `dxmt9_queue.cpp` `drainCompletedSequence` — inside the while loop at :1812-1816, after the `pop_front()`, add:

```cpp
        if (submissionBinding_.completedPresentOrdinal) {
          ++*submissionBinding_.completedPresentOrdinal;
          perf::countCompletedPresentOrdinal();
        }
```

  4. `dxmt9_command_queue.cpp` `bindSelfLifecycle` (:5020): add `.completedPresentOrdinal = &completedPresentOrdinal_,` next to `.presentCompletedSeqId`.
  5. Implement in `dxmt9_command_queue.cpp` (near `presentBoundary` :4017):

```cpp
void CommandQueue::waitPresentOrdinalBoundary(std::uint64_t presentOrdinal,
                                              std::uint32_t maxFrameLatency) {
  const BoundaryPolicy policy = resolveBoundaryPolicyFromEnv();
  if (policy == BoundaryPolicy::Disabled) {
    return;
  }
  std::uint64_t targetOrdinal = 0;
  if (policy == BoundaryPolicy::DeferredPresentCompletion) {
    std::unique_lock lock(mutex_);
    targetOrdinal = deferredPresentOrdinalTarget_;
    deferredPresentOrdinalTarget_ = std::max(
        deferredPresentOrdinalTarget_,
        presentOrdinalBoundaryTarget(presentOrdinal + 1, maxFrameLatency));
    if (targetOrdinal == 0 || completedPresentOrdinal_ >= targetOrdinal) {
      return;
    }
    perf::countPresentOrdinalBoundaryWait();
    const auto waitStarted = std::chrono::steady_clock::now();
    presentCompletedCv_.wait(lock, [&] {
      return stop_ || completedPresentOrdinal_ >= targetOrdinal;
    });
    perf::countPresentOrdinalBoundaryWaitNs(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - waitStarted).count()));
    return;
  }
  targetOrdinal = presentOrdinalBoundaryTarget(presentOrdinal, maxFrameLatency);
  if (targetOrdinal == 0) {
    return;
  }
  std::unique_lock lock(mutex_);
  if (completedPresentOrdinal_ >= targetOrdinal) {
    return;
  }
  perf::countPresentOrdinalBoundaryWait();
  const auto waitStarted = std::chrono::steady_clock::now();
  presentCompletedCv_.wait(lock, [&] {
    return stop_ || completedPresentOrdinal_ >= targetOrdinal;
  });
  perf::countPresentOrdinalBoundaryWaitNs(static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - waitStarted).count()));
}
```

  6. Counters (follow the four-touch pattern from `present_boundary_deferred_waits`): `Counters` fields `presentOrdinalBoundaryWaits`, `presentOrdinalBoundaryWaitNs` (match the Kind used by `present_boundary_wait_ms`'s field — copy its struct field type and table row Kind verbatim), `completedPresentOrdinal`; table rows `present_ordinal_boundary_waits`, `present_ordinal_boundary_wait_ms`, `completed_present_ordinal`; decls `countPresentOrdinalBoundaryWait()`, `countPresentOrdinalBoundaryWaitNs(std::uint64_t)`, `countCompletedPresentOrdinal()`; definitions mirroring `countPresentBoundaryDeferredWait` (cpp:9996) and `countPresentBoundaryWait`.
  7. `submitPresent` suppression: add file-local resolver next to `peRecorderStatsEnabled`-style helpers in `dxmt9_command_queue.cpp`:

```cpp
bool offloadCommitReplayEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("DXMT9_OFFLOAD_COMMIT_REPLAY");
    return value && value[0] != '\0' && !(value[0] == '0' && value[1] == '\0');
  }();
  return enabled;
}
```

At :3890 guard the deferred drain with `if (!offloadCommitReplayEnabled() && boundaryPolicy == BoundaryPolicy::DeferredPresentCompletion)`, and at :4005 make the chain `if (offloadCommitReplayEnabled()) { perf::countPresentBoundarySkipped(); } else if (boundaryPolicy == ...) { ... } else if (shouldApplyPresentBoundary(...)) { ... } else { ... }`.
  8. Frontend plumbing: add pure-virtual-with-default or virtual `waitPresentOrdinalBoundary(std::uint64_t ordinal)` to the `dxmt9::Device` interface (same header that declares `present(const core::SwapDesc&)` — find it via `rg -n "virtual void present" src/dxmt9`), implement in `DeviceImpl` (`dxmt9_device.cpp` next to `present` :296):

```cpp
void waitPresentOrdinalBoundary(std::uint64_t ordinal) override {
  queue_.waitPresentOrdinalBoundary(ordinal, maxFrameLatency_);
}
```

- [ ] **Step 6: TLA ordinal variant.** In `specs/verification/tla/PresentFrameLatency.tla`, add an ordinal-counting variable advanced by the same present-retire action that advances the modeled present completion, and a `WaitOrdinal` guard mirroring the existing latency-bound wait; add an invariant that the ordinal wait admits exactly the same states as the seqId wait (order isomorphism). Keep the state space bounded with the existing `.cfg` constants. Run:

```bash
meson test -C build-arm64-nowine dxmt9-verify-tla --timeout-multiplier 4
```

Expected: OK. If the module needs a new constant, mirror the existing `.cfg` entries.

- [ ] **Step 7: Full check + commit**

```bash
ninja -C build-arm64-nowine && \
meson test -C build-arm64-nowine dxmt9-present-ordinal-boundary-spec dxmt9-present-boundary-policy-spec dxmt9-queue-completion-sources-spec dxmt9-verify-tla --timeout-multiplier 4 --print-errorlogs && \
git diff --check
```

Expected: all OK. Note the counter audits (`dxmt9-perf-counter-table-audit` style names — find via `meson test -C build-arm64-nowine --list | rg audit`) and run them too; expected OK.

```bash
git add -A src/dxmt9 specs/verification/tla tests/native/backend
git commit -m "backend: add offload present-ordinal boundary"
```

---

### Task 4: Offload worker + commit-path integration

**Files:**
- Modify: `src/d3d9/device_c_replay_offload.hpp` (+ create `.cpp` if worker needs a TU; add to `src/d3d9/meson.build`)
- Modify: `src/d3d9/device_c_common.hpp:143-196` (`D9CDevice` members), `src/d3d9/device_c_state.cpp:83-92` (release/join), `src/d3d9/device_c_chunk_replay.cpp` (commit branch)
- Modify: `src/dxmt9/dxmt9_perf_counters.{hpp,cpp}` (offload counters)

**Interfaces:**
- Consumes: Task 1 `replayImportedChunk(...)`, Task 2 `ReplayOffloadQueue`, Task 3 `waitPresentOrdinalBoundary`.
- Produces: `dxmt9::d3d9::ReplayOffloadWorker` with `start(D9CDevice*)`, `stop()` (drain + join), owned by `D9CDevice`; commit-path branch; env resolver `offloadCommitReplayEnabled()` shared via `device_c_replay_offload.hpp` (declare there, define once — the queue-side resolver in Task 3 stays TU-local by design, same env string).

- [ ] **Step 1: Worker.** In `device_c_replay_offload.hpp` add (implementation in the new `device_c_replay_offload.cpp`):

```cpp
struct D9CDevice;  // fwd (global-namespace struct)

namespace dxmt9::d3d9 {

bool offloadCommitReplayEnabled();  // getenv("DXMT9_OFFLOAD_COMMIT_REPLAY"), read-once

class ReplayOffloadWorker {
 public:
  // queue bound: 64 chunks / 8 MiB ≈ 2+ frames of GT1 chunks (14/present, ~200KB/present)
  ReplayOffloadWorker() : queue_(64, 8u << 20) {}
  ~ReplayOffloadWorker() { stop(); }

  void start(D9CDevice* device);   // spawns thread_ running run(device)
  void stop();                     // queue_.stop(); join; idempotent
  ReplayOffloadQueue& queue() { return queue_; }
  bool failed() const { return failed_.load(std::memory_order_acquire); }

 private:
  void run(D9CDevice* device);     // pop loop -> replayRawChunk -> markReplayDone
  ReplayOffloadQueue queue_;
  std::thread thread_;
  std::atomic<bool> failed_{false};
};

// Implemented in device_c_chunk_replay.cpp (needs the file-local replay
// machinery): rebuilds ImportedWireChunkView over chunk.recordBlob, calls
// replayImportedChunk, releases chunk.retainedWrappers, returns HRESULT.
int32_t replayRawChunk(D9CDevice* d, RawCommandChunk& chunk);

}  // namespace dxmt9::d3d9
```

`run()` body: `RawCommandChunk c; while (queue_.pop(c)) { const int32_t hr = replayRawChunk(device, c); queue_.markReplayDone(); if (hr < 0) { failed_.store(true, std::memory_order_release); DXMT_ASSERT(false && "deferred commit replay failed"); queue_.stop(); return; } }` — fail-stop matches the batch-suffix precedent: debug aborts, release stops the worker and poisons subsequent commits.

- [ ] **Step 2: D9CDevice members + lifetime.** In `device_c_common.hpp` after `chunkEndSubmissionCarry` (:186):

```cpp
  std::unique_ptr<dxmt9::d3d9::ReplayOffloadWorker> replayOffload;
  std::uint64_t presentOrdinal = 0;  // present-bearing commits, offload pacing
```

(forward-declare `namespace dxmt9::d3d9 { class ReplayOffloadWorker; }` above the struct; include the header in the .cpps that touch it). In the D9CDevice destructor (:189) or in `dxmt9c_device_release` (`device_c_state.cpp:83-92`) before `delete d;`: `if (d->replayOffload) d->replayOffload->stop();` — stop() drains via join after `queue_.stop()`; to preserve pending work on clean shutdown call `d->replayOffload->queue().waitDrained()` BEFORE `stop()`.

- [ ] **Step 3: `replayRawChunk` + commit branch** in `device_c_chunk_replay.cpp`:

```cpp
int32_t dxmt9::d3d9::replayRawChunk(D9CDevice* d, dxmt9::d3d9::RawCommandChunk& chunk) {
  const auto replayStart = std::chrono::steady_clock::now();
  auto imported = makeImportedWireChunkBlobView(chunk.recordBlob.data(),
                                                chunk.recordBytes);
  if (!imported) {
    releaseRetainedWrappers(chunk);
    return commitChunkFail("offload-bad-wire-blob");
  }
  dxmt9::perf::PerfScope scope(dxmt9::perf::countOffloadReplayCpuTime);
  const int32_t hr = replayImportedChunk(d, *imported,
                                         chunk.skipDrawResourceMarking,
                                         chunk.bridgeCommitStart, replayStart);
  releaseRetainedWrappers(chunk);
  return hr;
}
```

(`makeImportedWireChunkBlobView` returns the view type used at :2067 — mirror its exact error handling; `releaseRetainedWrappers` releases every wrapper addref'd at commit — see Step 4.)

In `dxmt9c_device_commit_chunk`, after the synchronous import phase (post :2233, replacing the Task 1 tail call):

```cpp
  if (dxmt9::d3d9::offloadCommitReplayEnabled()) {
    dxmt9::perf::PerfScope rawScope(dxmt9::perf::countCommitChunkRawEnqueueCpuTime);
    if (!d->replayOffload) {
      d->replayOffload = std::make_unique<dxmt9::d3d9::ReplayOffloadWorker>();
      d->replayOffload->start(d);
    }
    if (d->replayOffload->failed()) {
      return commitChunkFail("offload-worker-failed");
    }
    dxmt9::d3d9::RawCommandChunk raw;
    raw.recordBlob.assign(records, records + chunk->recordBytes);
    raw.recordCount = chunk->recordCount;
    raw.recordBytes = chunk->recordBytes;
    raw.skipDrawResourceMarking = didBulkMarkResources;
    raw.bridgeCommitStart = bridgeCommitStart;
    raw.hasPresent = importedChunkHasPresentRecord(importedChunk);
    retainWrappersForOffload(importedChunk, raw);   // Step 4
    perf::countOffloadReplayQueueDepth(
        static_cast<std::uint64_t>(d->replayOffload->queue().depth()));
    if (!d->replayOffload->queue().push(std::move(raw))) {
      return commitChunkFail("offload-queue-stopped");
    }
    if (raw.hasPresent) {  // NOTE: capture hasPresent BEFORE the move
      ++d->presentOrdinal;
      d->dev().upperDevice()->waitPresentOrdinalBoundary(d->presentOrdinal);
    }
    return dxmt9::core::D3D_OK;
  }
  return replayImportedChunk(d, importedChunk, didBulkMarkResources,
                             bridgeCommitStart, commitChunkStageStart);
```

(Fix the noted move-order bug when writing the real code: read `hasPresent` into a local before `std::move(raw)`.) `importedChunkHasPresentRecord` = small helper iterating `nextImportedRecord` checking `header.type == D9C_COMMAND_RECORD_PRESENT` — or reuse the existing shape summary from :2085 if it already exposes a present count (`summarizeNoEnqueueCommitChunkRecordShape` — check its return struct; prefer reuse).

Also: the sync `ResetSkipDrawMarkGuard` at :2211-2228 must NOT reset the skip flag before deferred replay uses it — in the offload branch the guard's scope-exit fires at commit return. Pass `didBulkMarkResources` in the RawChunk (done above) and make the worker path re-assert the flag inside `replayImportedChunk` (Task 1 already moved the guard inside with the `skipDrawResourceMarking` parameter, so the flag state is self-contained per replay — verify the guard constructor sets AND restores).

- [ ] **Step 4: Wrapper retention.** `retainWrappersForOffload(importedChunk, raw)`: iterate the same handle table the generation-check loop walks (:2106-2162); for each stamped texture/surface/buffer wrapper call its addref (`dxmt9c_texture_addref`-family, resolved via `wireValuePtr` exactly as :2106 does) and store the opaque pointer + kind in `raw.retainedWrappers` (store as tagged `void*` pairs or a small struct `{uint32_t kind; void* ptr;}` — adjust `RawCommandChunk::retainedWrappers` accordingly). `releaseRetainedWrappers` calls the matching release. **Verification sub-step (required):** confirm every record-embedded object reference resolves through the chunk handle TABLE (not raw embedded pointers) by inspecting the STRETCH_RECT (:2814), COLOR_FILL (:2825), UPDATE_TEXTURE (:2834), indexed-run IB (:2644) resolution sources; if any record bypasses the table, extend `retainWrappersForOffload` to walk that record type's handles too. Record findings as a code comment above `retainWrappersForOffload`.

- [ ] **Step 5: Offload counters** (four-touch pattern + audits): `commit_chunk_raw_enqueue_cpu_ms` (CpuTime kind — copy `commit_chunk_import_cpu_ms` row), `offload_replay_cpu_ms` (CpuTime), `offload_replay_queue_depth` (copy the Kind used by `encode_dequeue_ready_depth` — value-distribution counter). Callsites: raw enqueue + queue depth in this task's commit branch; `countOffloadReplayCpuTime` in `replayRawChunk`. The two drain-fence counters are added in Task 5 together with their callsites so the callsite audit passes at every commit.

- [ ] **Step 6: Build + suites + commit**

```bash
ninja -C build-arm64-nowine && \
meson test -C build-arm64-nowine dxmt9-replay-offload-queue-spec dxmt9-present-ordinal-boundary-spec dxmt9-queue-completion-sources-spec $(meson test -C build-arm64-nowine --list 2>/dev/null | rg -o "dxmt9-.*audit.*" | tr '\n' ' ') --print-errorlogs && git diff --check
```

Expected: all OK (audits pass because every counter has table row + callsite).

```bash
git add -A src/d3d9 src/dxmt9
git commit -m "d3d9: offload commit-chunk replay behind env flag"
```

---

### Task 5: Drain-fence prologue on bridge exports

**Files:**
- Modify: `src/d3d9/device_c_bridge_device_state_draw.cpp` (all 66 wrappers except `dxmt9c_device_commit_chunk`)
- Modify: `src/d3d9/device_c_bridge_swapchain_query_stateblock.cpp` (`dxmt9c_swapchain_present` + swapchain/query/stateblock CREATE calls that take `D9CDevice*`)
- Modify: `src/d3d9/device_c_bridge_resources.cpp` (create_* calls taking `D9CDevice*`)
- Modify: `src/d3d9/device_c_replay_offload.{hpp,cpp}` (fence helper)

**Interfaces:**
- Produces: `dxmt9::d3d9::drainDeferredReplay(D9CDevice* d)` — no-op when flag off / no worker / queue empty; otherwise `PerfScope(countOffloadDrainFenceCpuTime)` + `countOffloadDrainFenceWait()` + `d->replayOffload->queue().waitDrained()`.

- [ ] **Step 1: Fence helper + counters.** Add the two counters with the four-touch pattern: `offload_drain_fence_waits` (UnsignedCount, mirror `present_boundary_deferred_waits`) and `offload_drain_fence_wait_ms` (copy the `commit_chunk_import_cpu_ms` CpuTime row shape for `countOffloadDrainFenceCpuTime`). Then the helper in `device_c_replay_offload.cpp`:

```cpp
void dxmt9::d3d9::drainDeferredReplay(D9CDevice* d) {
  if (!d || !d->replayOffload) {
    return;
  }
  auto& queue = d->replayOffload->queue();
  if (queue.depth() == 0) {
    return;
  }
  dxmt9::perf::countOffloadDrainFenceWait();
  dxmt9::perf::PerfScope scope(dxmt9::perf::countOffloadDrainFenceCpuTime);
  queue.waitDrained();
}
```

- [ ] **Step 2: Prologue insertion.** In `device_c_bridge_device_state_draw.cpp`, every `extern "C" ... dxmt9c_device_*(D9CDevice* arg0, ...)` body except `dxmt9c_device_commit_chunk` becomes:

```cpp
extern "C" int32_t dxmt9c_device_set_render_state(D9CDevice* arg0, uint32_t a, uint32_t b) {
  dxmt9::d3d9::drainDeferredReplay(arg0);
  return dxmt9p_device_set_render_state(arg0, a, b);
}
```

Mechanically apply to all 65 device wrappers (the fence self-checks the flag, so the off path stays a branch + return). In the other two bridge files, add the same prologue to every wrapper whose first parameter is `D9CDevice*` (the create_* family) — object-method wrappers (texture/buffer/surface/query/stateblock/swapchain methods) are NOT fenced in v1: releases are safe via Task 4 wrapper retention, locks are ordered via records, queries/readbacks arrive as records. EXCEPTION: `dxmt9c_swapchain_present` — fence it via its device backpointer if `D9CSwapChain` (see `device_c_common.hpp`) stores one (check for a `D9CDevice*` or `iface` member with device access); if it has no device path, add `D9CDevice* owner` to `D9CSwapChain` at creation (`dxmt9c_device_create_additional_swapchain` / factory create) and fence through it. GT1's PE path presents via the PRESENT record (ordered in-queue), so this fence only protects the alternate direct path.

- [ ] **Step 3: Coverage check.** Verify no unfenced device wrapper remains:

```bash
rg -c "drainDeferredReplay" src/d3d9/device_c_bridge_device_state_draw.cpp   # expect 65 (66 exports - commit_chunk)
rg -n "extern \"C\"" src/d3d9/device_c_bridge_device_state_draw.cpp | wc -l  # expect 66
```

- [ ] **Step 4: Build + suites + commit**

```bash
ninja -C build-arm64-nowine && \
meson test -C build-arm64-nowine $(rg -o "test\('([^']+)'" -r '$1' tests/native/backend/meson.build | tr '\n' ' ') --print-errorlogs
```

Expected: all OK.

```bash
git add -A src/d3d9
git commit -m "d3d9: drain deferred replay before direct device calls"
```

---

### Task 6: Docs + full verification sweep

**Files:**
- Modify: `agents/rules/environment_variables_bridge.rules.md` (new row), `specs/backend/design.md` (offload paragraph: raw handoff, ordinal pacing, drain fences, fail-stop, per-record HRESULT contract change)

- [ ] **Step 1: Env rules row** — add to the PE bridge/recorder table:

`| DXMT9_OFFLOAD_COMMIT_REPLAY | Experimental producer-serial reduction: dxmt9c_device_commit_chunk keeps validation/import/handle-marking synchronous but defers record replay to a device-owned worker through a bounded raw-chunk queue. Present-bearing commits wait an app-side present-ordinal frame-latency boundary (honoring the DXMT9_PRESENT_BOUNDARY_* policy resolution) and CommandQueue::submitPresent suppresses its inline boundary. Direct device calls drain the queue first. Replay failures fail-stop the worker and poison later commits instead of returning per-record HRESULTs. Opt-in for paired GT1 scouts; not a default until the offload proof gates pass. Note: with this flag set, non-PE (direct COM) clients of the same process lose the inline present boundary — PE experiment use only. | 0 |`

- [ ] **Step 2: specs/backend/design.md** — add a short subsection near the deferred-boundary paragraph describing the offload contract (sync import boundary at `noteCommitChunkReplayStartForCompletionGap`, FIFO raw queue, ordinal pacing isomorphism, drain-fence surface, fail-stop). Cite the spec doc path.

- [ ] **Step 3: Full sweep**

```bash
ninja -C build-arm64-nowine && \
meson test -C build-arm64-nowine $(rg -o "test\('([^']+)'" -r '$1' tests/native/backend/meson.build | tr '\n' ' ') dxmt9-verify-tla --timeout-multiplier 4 --print-errorlogs && \
git diff --check
```

Expected: all OK.

- [ ] **Step 4: Commit**

```bash
git add agents/rules/environment_variables_bridge.rules.md specs/backend/design.md
git commit -m "docs: document commit-replay offload contract"
```

---

### Task 7: Paired scout proof + knowledge-graph record

**Files:**
- Create: `docs/perfomance/present-pacing/present-pacing-commit-replay-offload.190.md`
- Modify: `docs/perfomance/overview-3dmark05-gt1.md`, `docs/perfomance/present-pacing.md`, optionally `scripts/tools/compare_3dmark05_p4_pair.py` (add `--mode offload` gates)

- [ ] **Step 1: Rebuild staged Wine-facing dirs** (the offload lives in the unix provider):

```bash
ninja -C build-x86_64-builtin && ninja -C build-win32-x64-builtin && ninja -C build-win32-x86-builtin && \
meson test -C build-x86_64-builtin dxmt9-winemetal-install-name-audit --print-errorlogs
```

Expected: builds OK, audit `1/1 OK`.

- [ ] **Step 2: Paired scouts** (desktop unlocked; ~4 min each):

```bash
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix replay-offload-baseline-r0-$(date +%Y%m%d) \
  --no-gputrace --no-encoder-breakdown --frame-sampling \
  --timeout 120 --keep-frontmost \
  --capture-range 880:960:10 --capture-delay-sec 45
DXMT9_OFFLOAD_COMMIT_REPLAY=1 bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix replay-offload-candidate-r1-$(date +%Y%m%d) \
  --no-gputrace --no-encoder-breakdown --frame-sampling \
  --timeout 120 --keep-frontmost \
  --capture-range 880:960:10 --capture-delay-sec 45
```

(If the wrapper does not pass arbitrary env through, check `run_3dmark05_perf_probe.sh` for its env passthrough mechanism — it exports caller env to the launcher; verify with `--dry-run` that `DXMT9_OFFLOAD_COMMIT_REPLAY=1` appears in the printed `env:` line; if not, add a `--offload-commit-replay` wrapper flag following the `--present-boundary-deferred` pattern at wrapper lines ~49/713/1806/4380.)

- [ ] **Step 3: Judge.** Run `compare_3dmark05_p4_pair.py` for the correctness/locality/fps view, then the offload mechanism checks:

```bash
python3 scripts/tools/compare_3dmark05_p4_pair.py \
  --baseline experiments/output/app-d3d9-3dmark05-replay-offload-baseline-r0-<date> \
  --candidate experiments/output/app-d3d9-3dmark05-replay-offload-candidate-r1-<date>; echo "exit=$?"
python3 - <<'EOF'
import json
d = "experiments/output/app-d3d9-3dmark05-replay-offload-candidate-r1-<date>"
c = json.load(open(f"{d}/result.json"))["dxmt9_perf_counters"]
p = float(c["present_encoded"])
bridge = float(c.get("bridge_commit_latency_ns") or 0)/1e6/p
ow = float(c.get("present_ordinal_boundary_wait_ms") or 0)/p
print("bridge_commit ms/present", round(bridge,3), "ordinal_wait ms/present", round(ow,3))
print("offload_replay cpu ms/present", round(float(c.get("offload_replay_cpu_ms") or 0)/p,3))
print("drain fences", c.get("offload_drain_fence_waits"))
assert bridge - ow <= 2.0, "mechanism gate: raw handoff must be <=2ms/present"
EOF
```

Gates (spec §Proof Protocol): FPS above noise, mechanism ≤2 ms/present, locality flat, correctness/visual pass (spot-check `frame000920.bmp` pair), pacing sane (`completion_pending_depth_max` ≤ baseline + latency). Note: the pair tool's P4-halving gate is EXPECTED to behave differently here — judge FPS/mechanism/locality/correctness primarily; record the P4 movement observationally.

- [ ] **Step 4: Record leaf 190 + overview/domain rows** (follow the H189 leaf shape; status `accepted-offload-win` or `rejected-...` with failing-gate numbers), commit docs:

```bash
git add docs/perfomance
git commit -m "docs: record commit-replay offload scout"
```

- [ ] **Step 5: Stop and report** — present numbers to the user; default-flip and any follow-up (PE recording track, replay-thread pipelining) are separate decisions.
