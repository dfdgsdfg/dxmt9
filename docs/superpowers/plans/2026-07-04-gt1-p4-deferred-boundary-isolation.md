# GT1 P4 Deferred-Boundary Isolation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Measure whether the tightened `DXMT9_PRESENT_BOUNDARY_DEFERRED=1` policy, run standalone on the baseline shape, opens the P4 window and raises 3DMark05 GT1 FPS above a paired baseline without hurting locality — then harden on WIN or hand analysis to Phase B on LOSE.

**Architecture:** Two supervised paired no-gputrace scouts (R0 baseline, R1 candidate) on today's HEAD, judged by a scripted five-gate comparison over `result.json` counters. Conditional hardening hoists the pure boundary-target math into the header and locks it with native spec tests. Spec: `docs/superpowers/specs/2026-07-04-gt1-p4-deferred-boundary-design.md`.

**Tech Stack:** `scripts/tools/run_3dmark05_perf_probe.sh` (supervised runner), Python 3 (judgement tool), Meson/Ninja C++20 native specs, dxmt9 perf-counter system.

## Global Constraints

- Wine runtime: manifest default `sikarugir-cx-24.0.7` resolves automatically; never hardcode another Wine root (agents/rules/test_wild.rules.md).
- 3DMark05 launches ONLY through `run_3dmark05_perf_probe.sh` with a positive timeout; never wait for natural exit; timeout-finalized runs with complete artifacts are valid samples (agents/rules/metal_debugging.rules.md).
- Do not set `MTL_CAPTURE_ENABLED`/`METAL_CAPTURE_ENABLED` (black-screen risk).
- Desktop must be unlocked for runs; both FPS-evidence runs use `--keep-frontmost` and identical flags except the candidate env.
- Native tests run in `build-arm64-nowine`; `DXMT_ASSERT` aborts in debug builds.
- Evidence layering: run artifacts in `experiments/output/<id>/` + `traces/<id>/`; experiment records as `docs/perfomance/present-pacing/` leaves plus overview rows; mechanism changes land with tests in the same commit.
- Commits: single-line imperative subject + the session's `Co-Authored-By` / `Claude-Session` trailers.

---

### Task 1: Rebuild staged runtimes and preflight

**Files:** none modified (build + verification only).

**Interfaces:**
- Consumes: existing Meson build dirs `build-x86_64-builtin`, `build-win32-x64-builtin`, `build-win32-x86-builtin`, `build-arm64-nowine`.
- Produces: staged binaries current with HEAD; verified runner preflight for later tasks.

- [ ] **Step 1: Rebuild every staged build directory** (stale staged `winemetal.so`/PE DLLs are a documented false-signal source)

```bash
ninja -C build-x86_64-builtin && \
ninja -C build-win32-x64-builtin && \
ninja -C build-win32-x86-builtin && \
ninja -C build-arm64-nowine
```

Expected: each exits 0 (either "no work to do" or a short compile).

- [ ] **Step 2: Run the winemetal install-name audit**

```bash
meson test -C build-x86_64-builtin dxmt9-winemetal-install-name-audit --print-errorlogs
```

Expected: `1/1 ... OK`. A failure means a direct-ninja build left bare deps — follow the printed `install_name_tool` remediation before any Wine run.

- [ ] **Step 3: Dry-run the candidate probe command**

```bash
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix p4-deferred-iso-candidate-r1-20260704 \
  --no-gputrace --no-encoder-breakdown --frame-sampling \
  --timeout 120 --keep-frontmost \
  --capture-range 880:960:10 --capture-delay-sec 45 \
  --present-boundary-deferred --dry-run
```

Expected: printed `env:` line contains `DXMT9_PRESENT_BOUNDARY_DEFERRED=1`; `wine_root` under `experiments/wine/sikarugir-cx-24.0.7`; no free-space failure. No commit for this task.

---

### Task 2: Pair-judgement tool

**Files:**
- Create: `scripts/tools/compare_3dmark05_p4_pair.py`

**Interfaces:**
- Consumes: two experiment output dirs each containing `result.json` with `dxmt9_perf_counters` and `image_metrics`.
- Produces: CLI `python3 scripts/tools/compare_3dmark05_p4_pair.py --baseline DIR --candidate DIR [--noise-pct 5] [--locality-slack-pct 2]`; prints a gate table; exit 0 = WIN, 1 = LOSE, 2 = REPEAT (FPS inside noise band while every other gate passes). Later tasks call it verbatim.

- [ ] **Step 1: Write the tool**

```python
#!/usr/bin/env python3
"""Judge a paired 3DMark05 GT1 P4 deferred-boundary scout.

Design: docs/superpowers/specs/2026-07-04-gt1-p4-deferred-boundary-design.md
Gates (candidate vs baseline):
  fps        present_encoded above baseline beyond --noise-pct
  p4         completion_wait_without_enqueue_ms/present <= 50% of baseline
             AND completion_wait_with_enqueue_ms/present > baseline
  locality   command_buffers, sub_command_buffers, render_pass_begin,
             render_pass_tile_preservation_bytes per present all
             <= baseline * (1 + --locality-slack-pct/100)
  correct    status == pass (timeout-finalized ok), gpu_command_buffer_errors == 0,
             completion_dequeue_status_error == 0, mean_luma >= 10 (both runs)
  semantics  candidate present_boundary_applied > 0 and
             present_boundary_deferred > 0
Exit: 0 WIN, 1 LOSE, 2 REPEAT.
"""

import argparse
import json
import sys
from pathlib import Path

LOCALITY_KEYS = [
    "command_buffers",
    "sub_command_buffers",
    "render_pass_begin",
    "render_pass_tile_preservation_bytes",
]


def load_run(path):
    result = json.loads((Path(path) / "result.json").read_text())
    counters = result.get("dxmt9_perf_counters") or {}
    presents = float(counters.get("present_encoded") or 0.0)
    image = result.get("image_metrics") or {}
    return result, counters, presents, image


def per_present(counters, key, presents):
    value = counters.get(key)
    if value is None or presents <= 0:
        return None
    return float(value) / presents


def run_correct(result, counters, image, label, rows):
    ok = True
    status = result.get("status")
    errors = float(counters.get("gpu_command_buffer_errors") or 0.0)
    dequeue_err = float(counters.get("completion_dequeue_status_error") or 0.0)
    luma = image.get("mean_luma")
    luma_ok = luma is not None and float(luma) >= 10.0
    for name, cond in [
        (f"{label} status=pass", status == "pass"),
        (f"{label} gpu_command_buffer_errors=0", errors == 0.0),
        (f"{label} completion_dequeue_status_error=0", dequeue_err == 0.0),
        (f"{label} mean_luma>=10 (non-black)", luma_ok),
    ]:
        rows.append((name, "PASS" if cond else "FAIL", ""))
        ok = ok and cond
    return ok


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--candidate", required=True)
    parser.add_argument("--noise-pct", type=float, default=5.0)
    parser.add_argument("--locality-slack-pct", type=float, default=2.0)
    args = parser.parse_args()

    b_res, b_ctr, b_presents, b_img = load_run(args.baseline)
    c_res, c_ctr, c_presents, c_img = load_run(args.candidate)
    rows = []

    correct = run_correct(b_res, b_ctr, b_img, "baseline", rows)
    correct = run_correct(c_res, c_ctr, c_img, "candidate", rows) and correct

    fps_delta_pct = (
        (c_presents - b_presents) / b_presents * 100.0 if b_presents else -100.0
    )
    fps_win = fps_delta_pct > args.noise_pct
    fps_lose = fps_delta_pct < -args.noise_pct
    rows.append((
        "fps presents",
        f"{b_presents:.0f} -> {c_presents:.0f}",
        f"{fps_delta_pct:+.2f}% (noise ±{args.noise_pct}%)",
    ))

    b_without = per_present(b_ctr, "completion_wait_without_enqueue_ms", b_presents)
    c_without = per_present(c_ctr, "completion_wait_without_enqueue_ms", c_presents)
    b_with = per_present(b_ctr, "completion_wait_with_enqueue_ms", b_presents)
    c_with = per_present(c_ctr, "completion_wait_with_enqueue_ms", c_presents)
    p4 = (
        None not in (b_without, c_without, b_with, c_with)
        and c_without <= 0.5 * b_without
        and c_with > b_with
    )
    rows.append((
        "p4 without-enqueue ms/present (<=50% of baseline)",
        f"{b_without:.3f} -> {c_without:.3f}" if None not in (b_without, c_without) else "missing",
        "PASS" if p4 else "FAIL",
    ))
    rows.append((
        "p4 with-enqueue ms/present (must rise)",
        f"{b_with:.3f} -> {c_with:.3f}" if None not in (b_with, c_with) else "missing",
        "",
    ))

    locality = True
    slack = 1.0 + args.locality_slack_pct / 100.0
    for key in LOCALITY_KEYS:
        b_v = per_present(b_ctr, key, b_presents)
        c_v = per_present(c_ctr, key, c_presents)
        ok = b_v is not None and c_v is not None and c_v <= b_v * slack
        locality = locality and ok
        shown = (
            f"{b_v:.3f} -> {c_v:.3f}" if None not in (b_v, c_v) else "missing"
        )
        rows.append((f"locality {key}/present", shown, "PASS" if ok else "FAIL"))

    applied = float(c_ctr.get("present_boundary_applied") or 0.0)
    deferred = float(c_ctr.get("present_boundary_deferred") or 0.0)
    deferred_waits = float(c_ctr.get("present_boundary_deferred_waits") or 0.0)
    semantics = applied > 0.0 and deferred > 0.0
    rows.append((
        "semantics deferred gate engaged",
        f"applied={applied:.0f} deferred={deferred:.0f} deferred_waits={deferred_waits:.0f}",
        "PASS" if semantics else "FAIL",
    ))

    width = max(len(r[0]) for r in rows)
    for name, value, note in rows:
        print(f"{name:<{width}}  {value}  {note}")

    hard_gates = correct and p4 and locality and semantics
    if not hard_gates:
        print("VERDICT: LOSE (non-FPS gate failed)")
        return 1
    if fps_win:
        print("VERDICT: WIN")
        return 0
    if fps_lose:
        print("VERDICT: LOSE (FPS regressed)")
        return 1
    print("VERDICT: REPEAT (FPS inside noise band)")
    return 2


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: Validate against known anchor runs (tool's red/green)**

```bash
python3 scripts/tools/compare_3dmark05_p4_pair.py \
  --baseline experiments/output/app-d3d9-3dmark05-h220-current-visual-p4-baseline-r1 \
  --candidate experiments/output/app-d3d9-3dmark05-encode-session-deferred-boundary-rerun-20260628; echo "exit=$?"
```

Expected: exit=1 (LOSE). Table must show: fps `1784 -> 1200` (≈-32.7%), p4 without-enqueue `28.504 -> 4.977` PASS, locality `render_pass_begin` and `render_pass_tile_preservation_bytes` FAIL, semantics PASS (`applied=1200 deferred=1199`). Candidate `status=pass` even though `timed_out=true` — PASS row.

```bash
python3 scripts/tools/compare_3dmark05_p4_pair.py \
  --baseline experiments/output/app-d3d9-3dmark05-h220-current-visual-p4-baseline-r1 \
  --candidate experiments/output/app-d3d9-3dmark05-encode-session-stable-rerun-20260628b; echo "exit=$?"
```

Expected: exit=1 (LOSE), semantics FAIL (`applied`/`deferred` absent → 0), fps ≈-51.7%.

- [ ] **Step 3: Commit**

```bash
git add scripts/tools/compare_3dmark05_p4_pair.py
git commit -m "tools: add 3dmark05 P4 pair judgement script"
```

---

### Task 3: R0 baseline scout

**Files:** none (produces `experiments/output/app-d3d9-3dmark05-p4-deferred-iso-baseline-r0-20260704/`).

**Interfaces:**
- Consumes: Task 1 preflight; unlocked desktop.
- Produces: baseline `result.json` for Tasks 5/7/8.

- [ ] **Step 1: Run the baseline scout** (foreground, ~4 min; watchdog is internal)

```bash
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix p4-deferred-iso-baseline-r0-20260704 \
  --no-gputrace --no-encoder-breakdown --frame-sampling \
  --timeout 120 --keep-frontmost \
  --capture-range 880:960:10 --capture-delay-sec 45
```

Expected: run finishes (normal or timeout-finalized) and prints the output dir.

- [ ] **Step 2: Verify artifacts and health**

```bash
python3 - <<'EOF'
import json
d = "experiments/output/app-d3d9-3dmark05-p4-deferred-iso-baseline-r0-20260704"
r = json.load(open(f"{d}/result.json"))
c = r["dxmt9_perf_counters"]; im = r.get("image_metrics") or {}
print("status", r["status"], "timed_out", r["timed_out"])
print("presents", c.get("present_encoded"),
      "gpu_errors", c.get("gpu_command_buffer_errors"),
      "mean_luma", im.get("mean_luma"))
assert r["status"] == "pass"
assert float(c.get("gpu_command_buffer_errors") or 0) == 0
assert float(im.get("mean_luma") or 0) >= 10
EOF
```

Expected: prints status/presents/luma, no assertion failure. presents should land near the h220 anchor (~1700–1850); a large deviation is a finding to note, not silently accept. No commit (run artifacts are not committed).

---

### Task 4: R1 candidate scout

**Files:** none (produces `experiments/output/app-d3d9-3dmark05-p4-deferred-iso-candidate-r1-20260704/`).

**Interfaces:**
- Consumes: Task 1 preflight.
- Produces: candidate `result.json` for Task 5.

- [ ] **Step 1: Run the candidate scout** (identical flags + the candidate env)

```bash
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix p4-deferred-iso-candidate-r1-20260704 \
  --no-gputrace --no-encoder-breakdown --frame-sampling \
  --timeout 120 --keep-frontmost \
  --capture-range 880:960:10 --capture-delay-sec 45 \
  --present-boundary-deferred
```

- [ ] **Step 2: Verify artifacts, health, and gate engagement**

```bash
python3 - <<'EOF'
import json
d = "experiments/output/app-d3d9-3dmark05-p4-deferred-iso-candidate-r1-20260704"
r = json.load(open(f"{d}/result.json"))
c = r["dxmt9_perf_counters"]; im = r.get("image_metrics") or {}
print("status", r["status"], "timed_out", r["timed_out"])
print("presents", c.get("present_encoded"),
      "gpu_errors", c.get("gpu_command_buffer_errors"),
      "mean_luma", im.get("mean_luma"))
print("boundary applied", c.get("present_boundary_applied"),
      "deferred", c.get("present_boundary_deferred"),
      "deferred_waits", c.get("present_boundary_deferred_waits"),
      "wait_ms", c.get("present_boundary_wait_ms"))
assert r["status"] == "pass"
assert float(c.get("gpu_command_buffer_errors") or 0) == 0
assert float(c.get("present_boundary_deferred") or 0) > 0
EOF
```

Expected: deferred > 0 (tightened gate engaged). Record `deferred_waits`: nonzero proves the N+1 wait actually fires; zero requires the Task 5/7/8 write-up to explain it via completion progress. No commit.

---

### Task 5: Judge the pair

**Files:** none.

**Interfaces:**
- Consumes: Task 2 tool, Task 3/4 output dirs.
- Produces: WIN → Task 7; LOSE → Task 8; REPEAT → Task 6.

- [ ] **Step 1: Run the judgement**

```bash
python3 scripts/tools/compare_3dmark05_p4_pair.py \
  --baseline experiments/output/app-d3d9-3dmark05-p4-deferred-iso-baseline-r0-20260704 \
  --candidate experiments/output/app-d3d9-3dmark05-p4-deferred-iso-candidate-r1-20260704; echo "exit=$?"
```

- [ ] **Step 2: Branch on the verdict**

Decision rule: exit 0 → skip Task 6, do Task 7 (WIN) and skip Task 8. exit 1 → skip Task 6/7, do Task 8 (LOSE). exit 2 → do Task 6; if the second pair is WIN → Task 7; LOSE → Task 8; a second REPEAT → classify as LOSE-flat ("policy engages but does not move FPS beyond noise") and do Task 8 with that classification.

- [ ] **Step 3: Visual spot-check** — open `actual.png` and one mid-window internal capture (`traces/app-d3d9-3dmark05-p4-deferred-iso-candidate-r1-20260704/analysis/captures/frame000920.bmp`) for both runs and confirm the candidate shows no current-only artifact class vs the baseline (black vertices, transparent weapon, missing geometry). `mean_luma` is a floor gate only; this eyeball check is the `v0.0.3`-anchor-class requirement from the spec.

---

### Task 6: Repeat pair (only on REPEAT)

**Files:** none.

- [ ] **Step 1: Run the second pair** (same two commands as Tasks 3–4 with suffixes `p4-deferred-iso-baseline-r0b-20260704` and `p4-deferred-iso-candidate-r1b-20260704`, run back-to-back)

```bash
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix p4-deferred-iso-baseline-r0b-20260704 \
  --no-gputrace --no-encoder-breakdown --frame-sampling \
  --timeout 120 --keep-frontmost \
  --capture-range 880:960:10 --capture-delay-sec 45
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix p4-deferred-iso-candidate-r1b-20260704 \
  --no-gputrace --no-encoder-breakdown --frame-sampling \
  --timeout 120 --keep-frontmost \
  --capture-range 880:960:10 --capture-delay-sec 45 \
  --present-boundary-deferred
```

- [ ] **Step 2: Judge the second pair and apply the Task 5 decision rule**

```bash
python3 scripts/tools/compare_3dmark05_p4_pair.py \
  --baseline experiments/output/app-d3d9-3dmark05-p4-deferred-iso-baseline-r0b-20260704 \
  --candidate experiments/output/app-d3d9-3dmark05-p4-deferred-iso-candidate-r1b-20260704; echo "exit=$?"
```

---

### Task 7: WIN hardening (only if Task 5/6 verdict is WIN)

**Files:**
- Modify: `src/dxmt9/dxmt9_command_queue.hpp` (after line 70, inside `namespace dxmt9`)
- Modify: `src/dxmt9/dxmt9_command_queue.cpp` (remove the two file-local target functions, ~lines 2647–2668)
- Modify: `tests/native/backend/present_boundary_policy_spec.cpp`
- Create: `docs/perfomance/present-pacing/present-pacing-deferred-boundary-isolated.189.md`
- Modify: `docs/perfomance/overview-3dmark05-gt1.md`, `docs/perfomance/present-pacing.md`, `agents/rules/environment_variables_present.rules.md`

**Interfaces:**
- Produces: `dxmt9::presentBoundaryTargetSeqId(std::uint64_t, std::uint32_t) -> std::uint64_t` and `dxmt9::deferredPresentBoundaryTargetSeqId(std::uint64_t, std::uint32_t) -> std::uint64_t`, declared inline in `dxmt9_command_queue.hpp` (bodies identical to the current cpp-local definitions).

- [ ] **Step 1: Write the failing tests** — append to `tests/native/backend/present_boundary_policy_spec.cpp` next to the existing `test*` functions, add the include below the existing presenter include, and register both tests in `main` alongside the existing calls:

```cpp
#include "../../../src/dxmt9/dxmt9_command_queue.hpp"
```

```cpp
void testPresentBoundaryTargetSeqIdMath() {
  using dxmt9::kMaxQueuedChunks;
  using dxmt9::presentBoundaryTargetSeqId;
  check(presentBoundaryTargetSeqId(0, 3) == 0, "present 0 never waits");
  check(presentBoundaryTargetSeqId(3, 3) == 0, "inside the latency window");
  check(presentBoundaryTargetSeqId(4, 3) == 1,
        "first past-window present waits on seq 1");
  check(presentBoundaryTargetSeqId(2, 0) == 1, "latency clamps up to 1");
  check(presentBoundaryTargetSeqId(1000, 64) ==
            1000 - static_cast<std::uint64_t>(kMaxQueuedChunks),
        "latency clamps down to kMaxQueuedChunks");
}

void testDeferredBoundaryTargetGatesNextPresentTail() {
  using dxmt9::deferredPresentBoundaryTargetSeqId;
  using dxmt9::presentBoundaryTargetSeqId;
  const std::uint64_t seqs[] = {1, 3, 4, 100};
  for (const auto seq : seqs) {
    check(deferredPresentBoundaryTargetSeqId(seq, 3) ==
              presentBoundaryTargetSeqId(seq + 1, 3),
          "deferred target equals the next present's normal target");
  }
  const auto maxSeq = std::numeric_limits<std::uint64_t>::max();
  check(deferredPresentBoundaryTargetSeqId(maxSeq, 3) ==
            presentBoundaryTargetSeqId(maxSeq, 3),
        "u64-max present saturates instead of overflowing");
}
```

- [ ] **Step 2: Run to verify failure**

```bash
meson test -C build-arm64-nowine dxmt9-present-boundary-policy-spec --print-errorlogs
```

Expected: compile FAILURE — `presentBoundaryTargetSeqId` is not a member of namespace `dxmt9` (it is still cpp-local).

- [ ] **Step 3: Hoist the helpers** — in `dxmt9_command_queue.hpp`, directly after the `kMaxQueuedChunks` line, add (bodies verbatim from the cpp, plus `inline`; add `#include <algorithm>` / `#include <limits>` to the header if not already present):

```cpp
// Present frame-latency boundary target math (TLA+: PresentFrameLatency).
// Returns the present seqId that must have completed before the present
// numbered `presentSeqId` may pass the boundary under `maxFrameLatency`
// outstanding presents (clamped to [1, kMaxQueuedChunks]); 0 = no wait.
inline std::uint64_t presentBoundaryTargetSeqId(std::uint64_t presentSeqId,
                                                std::uint32_t maxFrameLatency) {
  if (presentSeqId == 0) {
    return 0;
  }
  maxFrameLatency = std::clamp<std::uint32_t>(
      maxFrameLatency, 1u, kMaxQueuedChunks);
  if (presentSeqId <= maxFrameLatency) {
    return 0;
  }
  return presentSeqId - maxFrameLatency;
}

// DeferredPresentCompletion variant: gate the *next* present tail
// (presentSeqId + 1) so frame N+1 offscreen work may run ahead while the
// configured frame latency still holds at N+1's own present tail.
inline std::uint64_t deferredPresentBoundaryTargetSeqId(
    std::uint64_t presentSeqId, std::uint32_t maxFrameLatency) {
  if (presentSeqId == std::numeric_limits<std::uint64_t>::max()) {
    return presentBoundaryTargetSeqId(presentSeqId, maxFrameLatency);
  }
  return presentBoundaryTargetSeqId(presentSeqId + 1, maxFrameLatency);
}
```

Then delete the two identical file-local definitions from `dxmt9_command_queue.cpp` (keep `presentBoundaryWaitPolicy` there).

- [ ] **Step 4: Run tests to verify pass**

```bash
meson test -C build-arm64-nowine dxmt9-present-boundary-policy-spec dxmt9-queue-completion-sources-spec --print-errorlogs
```

Expected: all OK.

- [ ] **Step 5: Run the backend suite and TLA verification**

```bash
meson test -C build-arm64-nowine dxmt9-verify-tla --timeout-multiplier 4 && \
git diff --check
```

Expected: `1/1 OK`, clean whitespace.

- [ ] **Step 6: Commit the mechanism change**

```bash
git add src/dxmt9/dxmt9_command_queue.hpp src/dxmt9/dxmt9_command_queue.cpp \
        tests/native/backend/present_boundary_policy_spec.cpp
git commit -m "backend: unit-test deferred present-boundary target math"
```

- [ ] **Step 7: Record the knowledge-graph evidence** — write `docs/perfomance/present-pacing/present-pacing-deferred-boundary-isolated.189.md` following the frontmatter + `Question / Run / Verdict / Interpretation` section shape of `present-pacing-encode-session-deferred-boundary.188.md`: frontmatter (`domain: present-pacing`, `order: 189`, `type: no-gputrace`, `status:` `accepted-isolated-p4-win` on WIN), `source:` pointing at both runs' `result.json`/summary/`actual.png`, the exact R0/R1 commands, the judged gate table copied from the Task 5 tool output, and an Interpretation stating the isolated-policy result plus the promotion caveat (default flip is a separate decision). Then add matching rows: one row in the `docs/perfomance/overview-3dmark05-gt1.md` present-pacing section and one in `docs/perfomance/present-pacing.md` citing `[[present-pacing-deferred-boundary-isolated.189]]`, and update the `DXMT9_PRESENT_BOUNDARY_DEFERRED` row in `agents/rules/environment_variables_present.rules.md` to record the isolated-scout result while keeping default-off wording. Also re-read the deferred-boundary paragraph in `specs/backend/design.md` (added by 9c0960f5) and extend it only if the isolated result contradicts or sharpens a documented behavior claim.

- [ ] **Step 8: Commit the docs**

```bash
git add docs/perfomance/present-pacing/present-pacing-deferred-boundary-isolated.189.md \
        docs/perfomance/overview-3dmark05-gt1.md docs/perfomance/present-pacing.md \
        agents/rules/environment_variables_present.rules.md
git commit -m "docs: record isolated deferred-boundary P4 scout"
```

- [ ] **Step 9: Stop and report** — present the numbers to the user and ask for the default-flip / longer-confirm-run decision (out of scope for this plan).

---

### Task 8: LOSE analysis leaf (only if verdict is LOSE)

**Files:**
- Create: `docs/perfomance/present-pacing/present-pacing-deferred-boundary-isolated.189.md`
- Modify: `docs/perfomance/overview-3dmark05-gt1.md`, `docs/perfomance/present-pacing.md`

- [ ] **Step 1: Write the leaf** — same file/shape as Task 7 Step 7 but `status: rejected-isolated-p4` (or `rejected-flat-fps` for the double-REPEAT case). The Verdict section must name exactly which gate(s) failed with the judged numbers, and the Interpretation must state what this implies for Phase B (e.g. "P4 opens but FPS stays flat → producer cadence, not completion pacing, owns the residual wall" or "locality moved without session cutting → deferred run-ahead itself perturbs CB shape; Phase B must bound run-ahead depth").

- [ ] **Step 2: Commit**

```bash
git add docs/perfomance/present-pacing/present-pacing-deferred-boundary-isolated.189.md \
        docs/perfomance/overview-3dmark05-gt1.md docs/perfomance/present-pacing.md
git commit -m "docs: record isolated deferred-boundary P4 rejection"
```

- [ ] **Step 3: Hand off to Phase B** — per the spec, Phase B (session locality restoration) starts its own brainstorm/spec/plan cycle seeded with this leaf's failing-gate evidence.
