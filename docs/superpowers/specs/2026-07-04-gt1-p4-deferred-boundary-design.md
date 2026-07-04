# GT1 Average-FPS Bottleneck (P4) — Deferred Present-Boundary Isolation Design

Date: 2026-07-04
Status: approved (Phase A first, Phase B committed as follow-up cycle)
Owner axis: 3DMark05 GT1 average FPS / wallclock (P4 no-enqueue overlap)

## Problem

The GT1 knowledge graph identifies the average-FPS wall as the closed P4
window: on the h220 baseline the completion thread spends
`28.504 ms/present` in `completion_wait_without_enqueue_ms` while GPU
command-buffer time is only `3.287 ms/present`. Every open-CB
`EncodeSession` carrier tested so far opens the P4 window but loses more
FPS to carrier-induced locality damage than it recovers
(`docs/perfomance/present-pacing/present-pacing-encode-session-*.md`,
H147–H188).

Anchor runs (same 110 s no-gputrace window, per-present values from
`result.json:dxmt9_perf_counters`):

| Run | presents | CB | sub-CB | passes | tile MiB | wait with-enq ms | wait without-enq ms |
|---|---|---|---|---|---|---|---|
| h220 baseline (`app-d3d9-3dmark05-h220-current-visual-p4-baseline-r1`) | 1784 | 4.010 | 2.998 | 11.766 | 120.222 | 0.128 | 28.504 |
| H187 stable carrier (`...-encode-session-stable-rerun-20260628b`) | 861 | 1.009 | 0.002 | 11.718 | 115.558 | 0.048 | 32.989 |
| H188 carrier + loose deferred (`...-encode-session-deferred-boundary-rerun-20260628`) | 1200 | 2.433 | 0.096 | 12.740 | 130.240 | 29.563 | 4.977 |

Key readings:

- The deferred present-boundary policy is the only mechanism so far that
  actually converts the no-enqueue wait into overlapped wait
  (H188 `with_enqueue = 29.563 ms/present`).
- It has only ever been tested stacked on the open-CB carrier family,
  whose own cost halves FPS (H187: 16.2 → 7.8 presents/s vs baseline),
  so the pacing win has never been measured in isolation.
- The tightened implementation (deferred target = next present tail,
  `presentSeqId + 1`; commit `9c0960f5`) has never been run: H188 used the
  loose prototype (`present_boundary_deferred_waits = 0`, i.e. the gate
  never actually engaged).

## Phase A — Isolate the deferred boundary on the baseline shape

### Question

Does `DXMT9_PRESENT_BOUNDARY_DEFERRED=1` (tightened, standalone, no
open-CB carrier flags) open the P4 window and raise FPS above the paired
baseline while preserving baseline command-buffer/pass/tile shape?

### Protocol

Two back-to-back supervised runs on today's HEAD (removes h220-vs-HEAD
code drift), identical flags except the candidate env:

```sh
# R0 — baseline re-anchor (same shape as h220)
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix p4-deferred-iso-baseline-r0-20260704 \
  --no-gputrace --no-encoder-breakdown --frame-sampling \
  --timeout 120 --keep-frontmost \
  --capture-range 880:960:10 --capture-delay-sec 45

# R1 — candidate
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix p4-deferred-iso-candidate-r1-20260704 \
  --no-gputrace --no-encoder-breakdown --frame-sampling \
  --timeout 120 --keep-frontmost \
  --capture-range 880:960:10 --capture-delay-sec 45 \
  --present-boundary-deferred
```

Judgement is scripted over `result.json` counters (extend the session
comparison script). If the FPS delta is within ~5%, run one more pair and
judge on medians. Timeout-finalized runs with complete artifacts are
valid samples per the runner discipline.

### Gates (all required for WIN)

1. **FPS**: candidate presents and sampled FPS strictly above paired
   baseline.
2. **P4**: `completion_wait_without_enqueue_ms/present` at or below 50%
   of the paired baseline value;
   `completion_wait_with_enqueue_ms/present` up (window actually opens).
3. **Locality (non-increase)**: `command_buffers/present`,
   `sub_command_buffers/present`, `render_pass_begin/present`, and
   `render_pass_tile_preservation` MiB/present all at or below baseline.
   No session cutting is active, so violations are findings, not noise.
4. **Correctness**: `status=pass`, `gpu_command_buffer_errors=0`,
   `completion_dequeue_status_error=0`, no `INVALIDCALL`-class log
   strings; `actual.png` and internal captures `880..960:10` pass the
   `v0.0.3` visual-anchor class check (non-black, luma/variance sanity,
   no current-only artifact class).
5. **Semantics**: `present_boundary_*` counters must show the tightened
   gate engaging (deferred applied on presents; `deferred_waits`
   accounting consistent with N+1 tail gating — an all-zero
   `deferred_waits` run must be explainable by actual completion
   progress, not by a missing gate).

### Branches

- **WIN** → hardening track:
  - Add native spec coverage for the tightened deferred semantics
    (extend `tests/native/backend/present_boundary_policy_spec.cpp`
    family: resolver priority vs `Disabled`/`PresentCompletion`,
    N+1 target selection, frame-latency backpressure preserved).
    Commit `9c0960f5` shipped no test file; red-green required.
  - Strengthen `specs/backend` rows; run `dxmt9-verify-tla` and the
    backend native suite.
  - Record knowledge-graph leaves (next `present-pacing` order numbers)
    plus root/domain overview rows.
  - Default-flip is a separate decision after a longer confirm run
    (R-BACK-2.34 default-flip precedent).
- **LOSE / partial** → record the failing-gate counter analysis as a
  leaf; feed the evidence into Phase B design refinement.

## Phase B — Session locality restoration (committed follow-up)

Working hypothesis from the anchor numbers: the H187 FPS collapse is the
open-CB carrier destroying the baseline's per-render-pass mid-chunk
pipelining (4 CB/present → 1.009 CB/present), so GPU start serializes
behind full-frame encode (GPU CB time 3.287 → 32.471 ms/present).

Direction: let the session commit prefix command buffers at pass-safe
boundaries under the existing cap discipline while making carried
first-draw continuation engage on the stable flag set
(`encode_session_carry_first_draw_continue_active` was 0 in H187 but
11204 in H188, so the mechanism exists and fires under run-ahead).
Recovery targets: passes/present at or below 11.766 and tile at or below
120.222 MiB/present with the P4 window open, then re-add the deferred
boundary on top.

Phase B gets its own spec/plan cycle after Phase A's evidence lands;
its gates will be recalibrated against Phase A's result (whichever of
R0/R1 wins becomes the new comparison anchor).

## Discipline

- Runner rules: positive timeouts, `--keep-frontmost`, unlocked-session
  guard, artifacts under `experiments/output/` and `traces/`.
- Evidence layering: experiment records go to `docs/perfomance/`
  leaves + overview rows; mechanism changes land with `specs/` rows and
  native tests in the same commit; TDD red-green for hardening tests.
- Known risks: deferred boundary changes effective input latency
  (watch `present_boundary_deferred_waits` distribution and visual
  gates); scene-progress FPS noise (mitigated by paired same-session
  runs with identical flags); h220 drift (mitigated by R0 re-anchor).
