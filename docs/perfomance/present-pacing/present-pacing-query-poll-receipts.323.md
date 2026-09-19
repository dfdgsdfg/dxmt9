---
type: "Performance Evidence"
title: "H323: immutable query polling and bounded replay drains"
description: "Promoted query replay receipts separate CPU-prefix ownership from GPU completion."
tags: [performance, queries, replay, verification]
source: "build/meson-logs/query-receipt-final-tests.log; build/meson-logs/query-receipt-final-x86-tests.log; build/meson-logs/query-receipt-final-tla.log; experiments/output/app-d3d9-stkcop-bench-poll-receipt-on-r648-day/result.json; experiments/output/app-d3d9-stkcop-bench-poll-receipt-on-r648-day/daybenchmark.result; experiments/output/app-d3d9-stkcop-bench-poll-receipt-on-r648-day/scene-{early,mid,late}.png"
---

# H323: immutable query polling and bounded replay drains

Date: 2026-09-18; follow-up 2026-09-19. Base: `af3d740e`; implementation
`b012b4b0`, bridge-test build correction `f42f83a7`.
This is implementation/correctness evidence, not a measured STALKER speedup.

**Current policy (2026-09-19):** after R648 and the user's confirmation of normal
rendering, receipt polling is unconditional at the user's explicit request.
The historical opt-in/default-OFF descriptions below refer to the original
implementation and R648 binary, not the promoted source.

## Motivation and boundary

[H322](present-pacing-query-ready-replay-frontier.322.md) observes 1,773
stable-END calls with later raw work and 5.751 seconds of summed global drain.
That scope is a candidate, not a removable-time estimate or FPS projection.
The existing core GetData reads and sometimes mutates the live Query, so simply
waiting for an earlier raw ordinal would create an ownership race.

`DXMT9_QUERY_POLL_RECEIPTS=1` instead uses two independent authorities:

1. A query-specific admission receipt proves successful CPU replay through the
   whole raw containing its latest admitted Issue (BEGIN as well as END).
2. A retained immutable snapshot proves that issue's ordinary and initializer
   GPU completion and successful result storage before exposing bytes.

The candidate remains default OFF. Eager asynchronous receipts are eligible;
missing/non-raw/synchronous/deferred-marker receipts retain the global-drain
reference. Same-raw records after END are deliberately not skipped.

## Ownership and synchronization

- `ReplayDrainLedger::captureReceipt` freezes the current raw ordinal and
  retains the target. `waitReceipt` never follows the query's future tail.
- Newer admission before the synchronized snapshot copy returns `S_FALSE`.
  Terminal failure wins. Neither case modifies caller bytes.
- `visitReadyReceipt` copies under ledger then snapshot mutex. Replay releases
  the snapshot mutex before publishing raw completion to the ledger. Backend
  polling holds neither lock.
- Provider Issue publishes the snapshot with replay TLS raw identity. Both
  worker and inline/provider paths carry that identity; direct Issue uses zero
  and falls back. Admission remains transactional across contiguous/segmented
  import; raw handle retention outlives completion publication.
- `Device::pollQuerySnapshot` consumes copied values, retained result/backend
  owners and atomic loss/reset/epoch controls. It never reads or resolves the
  live Query. Later reissue cannot redirect a copied snapshot.
- The final control check linearizes a successful poll before a subsequent
  reset/loss. Reset may overlap its return; an observed in-progress reset or
  changed epoch invalidates success. The bridge stages at most eight public
  bytes and rechecks terminal replay before copying them to the caller.
- There is no per-Issue allocation. Enabled query creation allocates its cold
  target/snapshot state; receipt copies retain existing owners. Query wire
  records and the PE/Unix ABI are unchanged.

## Verification

The shared receipt reducer has 128 exhaustive bounded native cases, invalid
frontiers and terminal-priority checks. Ledger tests cover fixed-prefix waits,
supersession, pre-effect rejection, target retention and ready-only visitors.
The snapshot fixture covers legacy fallback, result formatting, initializer
pending, immutable reissue/release, foreign owner, device/backend failure,
stale epoch and reset/loss injected between entry and final control checks.

Actual Metal tests run with validation enabled in portable and forced-tile
configurations. A real command-buffer event gate proves pending bytes remain
unchanged. Candidate and reference report exactly 1,024 visible samples or zero;
old snapshots survive reissue/release, reset invalidates them, and all 32×32
BGRA framebuffer pixels match green exactly. This is a bounded core/backend
oracle, not a Wine/game-image equivalence claim.

Historical verification logs (no longer present in the current workspace;
the results below are retained session records, not independently re-readable
artifacts):

- `build/meson-logs/query-receipt-build.log`
- `build/meson-logs/query-receipt-native.log`: 10/10 focused tests.
- `build/meson-logs/query-receipt-metal.log`: 4/4 Metal reference/candidate tests.
- `build/meson-logs/query-receipt-x86-tests.log`: 8/8 including x86_64 Metal and
  Unix install-name audit.
- `build/meson-logs/query-receipt-{unix,pe64,pe32}-build.log`: canonical full
  cross builds. No Wine runtime was staged or launched for these checks.

## Evidence limits and next experiment

### Final-HEAD follow-up (2026-09-19)

At `f42f83a7`, native build and five focused receipt/bridge/snapshot/ledger
tests pass. The full x86_64 build and six focused tests, including the Unix
install-name audit, also pass. The current SDK is selected via
`xcrun --sdk macosx --show-sdk-path`; the historical 26.5 SDK path is no longer
installed. This tooling change is not a runtime code change.

The strengthened bridge fixtures explicitly qualify the candidate route in
contiguous and segmented import, verify BEGIN supersession, inject terminal
failure during backend polling without modifying caller bytes, and gate actual
worker completion. A later unrelated raw blocks the global reference but not
the candidate; holding the query's own raw completion still blocks its poll.
Both inline and offload configurations pass.

The full TLA gate passes at this HEAD: 64 safety configurations, five progress
configurations and 152 expected counterexamples (221 total). The receipt model
has 4,674 distinct states; its five mutations reject missing CPU completion,
missing initializer completion, ignored supersession, mutable receipt
redirection and ignored terminal/reset invalidation. Fair replay/snapshot/poll
actions prove termination of a captured attempt, not eventual GPU success or
bounded wall time. Log: `build/meson-logs/query-receipt-final-tla.log`.

Current logs are `build/meson-logs/query-receipt-final-{build,tests,unix-build,x86-tests}.log`.
These checks do not rerun the historical Metal fixtures or establish FPS gains.

On 2026-09-19 the user reported compiling and running successfully in a wild
workload. The workload, selector environment and staged-binary receipt were
not supplied. Since this candidate defaults OFF, that report is recorded as
user-observed runtime success, not confirmed candidate-ON qualification.
The subsequent R648 run below explicitly enables the selector. No matched
OFF/ON performance pair is claimed. Broader
public PE concurrency/reset/fault evidence and full-sequence image equality
remain promotion gates. The bounded TLA/native correspondence does not prove
arbitrary C++ scheduling or driver fault recovery.

H322's observer populations cannot be compared unchanged: when the candidate
handles a poll, the mutable core/global-drain observers are bypassed, not run
concurrently or used to restore a global wait. Their enabled rows describe
fallback calls only. A future matched STALKER experiment must identify candidate
versus fallback call populations explicitly, separate observer-OFF visual/FPS
qualification from diagnostics, and retain identical binary/prefix/settings.

### R648: selector-ON STALKER Day wild run (2026-09-19)

At `f42f83a7`, all three canonical staging builds and the Unix install-name
audit pass before launch. The five staged artifact size/hash prefixes in
`result.json` match the canonical build outputs. The ABI handshake succeeds.
The catalogue runner exits with `returncode=0`, `status=pass`, no timeout,
no failure markers, 2,559 benchmark rows and 2,648 encoded Presents.
All eight health counters are present and zero: `chunk_reject`,
`command_chunk_rejects`, `gpu_command_buffer_errors`,
`draw_skipped_no_pipeline` and the four `pipeline_build_fail_*` counters
(`draw`, `function`, `library`, `pso`).

Invocation (inherited DXMT/MTL/Wine overrides were absent before launch):

```sh
DXMT_EXPERIMENT_PROFILE=perf DXMT_LOG_LEVEL=info DXMT_VALIDATE=0 \
DXMT_PERF_COUNTERS=1 DXMT9_QUERY_POLL_RECEIPTS=1 \
DXMT9_METAL_OCCLUSION=1 DXMT9_ASYNC_QUERY_FENCES=1 \
DXMT9_COALESCE_QUERY_ENDS=0 DXMT9_QUERY_OBSERVER=0 \
DXMT9_QUERY_DRAIN_OBSERVER=0 DXMT9_PERF_FRAME_SAMPLING=0 \
DXMT_STKCOP_LANE=day DXMT_STKCOP_PRESET=high \
DXMT_STKCOP_RENDERER=renderer_r2.5 DXMT_STKCOP_VID_MODE=1280x720 \
scripts/run_python.sh scripts/run_apps/run_experiment.py run \
  app-d3d9-stkcop-bench --wine-id sikarugir-cx-24.0.7 --timeout 300 \
  --output-suffix poll-receipt-on-r648-day --capture-delay-sec 90
```

`DXMT9_QUERY_DRAIN_OBSERVER` is not a consumed selector; the actual drain
observer follows `DXMT9_QUERY_OBSERVER=0`. The invocation establishes requested
candidate configuration, not a route-hit census. No selector override occurs
in the runner/launcher, but this run has no candidate/fallback population
instrumentation. Health counters remain enabled; this is not instrumentation-OFF.

Computer Use restored the initially non-visible window once during loading.
`actual.png` and `loading-restored.png` show loading only. Separately retained
`scene-early.png`, `scene-mid.png` and `scene-late.png` show characters,
vegetation, ground and building geometry without obvious black-frame or
missing-building artifacts at those spots. They are not a frame-aligned oracle
or continuous flicker/full-sequence test. The OpenAutomate plugin warning in
the log did not prevent the benchmark sequence or result production.

The benchmark reports **20.381388 average FPS** (min 4.599072, max 44.594929).
Treat it as a single visual-qualification run with a window restore, not an
isolated performance gain or non-regression proof. No session-scheduled build,
test, profiler or subagent work overlapped the engine; sampled process checks
found no competing build/profiler, without excluding all user/OS interference.
Default selectors and README performance values are unchanged.

### Unconditional production promotion

The user confirms R648 rendered normally and requests promotion plus removal
of the OFF code. `queryPollReceiptsEnabled` and the runtime environment lookup
are deleted. Query construction always owns its replay target/snapshot state;
raw admission always attaches Issue targets, replay always establishes its
ordinal scope, and GetData always attempts qualified immutable polling.
Unsupported/missing/non-raw/synchronous/deferred-marker receipts still require
the existing safe global drain. That fallback is semantic coverage, not a
disabled alternative, and is intentionally retained. Actual Metal occlusion
and deferred-END selectors are separate policies and are unchanged.

Bridge tests now run without an enable environment variable and cannot silently
skip. The independent Metal oracle uses a test-executable argument to select
snapshot checks; no production opt-out is retained for it. The runtime-policy
retirement audit rejects reintroduction of the deleted selector/helper.

Promotion does not establish an FPS gain, whole-sequence pixel equality or
arbitrary driver-fault recovery. Those limits remain explicit in the owning
gaps rather than being erased by the policy decision.

Post-promotion verification passes: full native and all three canonical cross
builds; nine native focused tests (including four actual Metal oracle runs and
the retirement audit); nine x86_64 tests (including the same Metal oracles and
install-name audit); and the full TLA gate. Both inline/offload bridge tests
also pass with the retired variable explicitly set to `0`, demonstrating that
it no longer disables the production route. No post-promotion wild rerun or
new FPS claim is made. Logs are
`build/meson-logs/query-receipt-promotion-{build,unix,pe64,pe32,tests,x86-tests,tla}.log`
and `build/meson-logs/query-receipt-retired-zero-tests.log`.
