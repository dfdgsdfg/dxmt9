---
domain: present-pacing
workload: STALKER CoP Day
title: "H334: bounded identity-qualified CPU wait evidence"
type: evidence
status: implementation-qualification
updated: 2026-09-20
source: experiments/output/wait-trace-validation-h334/dxmt9-wait-trace-final-native-tests.log; experiments/output/wait-trace-validation-h334/dxmt9-wait-trace-final-x86-tests.log; experiments/output/wait-trace-validation-h334/hardened-native-tests.log; experiments/output/wait-trace-validation-h334/hardened-x86-tests.log; experiments/output/wait-trace-validation-h334/reviewed-native-tests.log; experiments/output/wait-trace-validation-h334/reviewed-x86-tests.log; experiments/output/wait-trace-validation-h334/native-waits.csv; experiments/output/wait-trace-validation-h334/dxmt9-wait-trace-tla.log; experiments/output/app-d3d9-stkcop-bench-identity-wait-r670-day/launch-failure.log; experiments/output/app-d3d9-stkcop-bench-identity-wait-r671-day/run-policy.json; experiments/output/app-d3d9-stkcop-bench-identity-wait-r671-day/result.json; experiments/output/app-d3d9-stkcop-bench-identity-wait-r672-day/{result.json,actual.png,run-policy.json,dxmt9.log,waits.csv,thread-cpu.jsonl}
related: present-pacing-stalker-pre-split-wait-owner.333.md
---

# H334: record waits instead of sampling repeated Pending polls

Base `82272698` commits H332/H333. The new observer implements
[R-BACK-WAIT-1](../../../specs/backend/identity-qualified-wait-trace.md).
It does not change rendering, lifetime or submission policy and is default OFF.

## Coverage and identity

- Actual `ReplayDrainLedger::waitReceipt()` and resource-scoped `wait()`
  predicates retain their original mutex, CV, terminal priority and final
  recheck. Diagnostic-only noinline helpers record CV entries without a
  sampling stride. Ready calls are omitted unless mutex acquisition is slow.
- Each row carries a registered ledger incarnation, target, raw ordinal,
  before/after queued and completed frontiers and result classification.
  Receipt attempts propagate immutable query identity to the backend queue
  acquisition, which also records actual/requested queue and END ordinary /
  initializer prefixes. Buffer lock/unlock paths have distinct site labels.
- Mutex acquisition is separate from the CV interval. CV time includes
  predicate evaluation, spurious wakeups, lock reacquisition and scheduling.
  A completed frontier is not the identity of an OS wake notification.
- The 32,768-row cold sink allocates before the window, performs no file
  output or sink locking while measuring, and exports two Presents after it.
  The 256-entry owner registry never reuses incarnation IDs. Output refuses
  overwrite and reports unknown identity, overflow and incomplete writers.
  The close path samples active writers before the reservation frontier so a
  concurrently finishing mutex-only row cannot be silently omitted.
- `summarize_wait_trace.py` rejects invalid closure, storage, identity, clock,
  row/CV coverage or trailer evidence, and excludes cross-window rows from
  interior totals. Aggregated elapsed sums are not exclusive CPU or FPS costs.

Global replay drains, queue-space waits/holders, GPU resource waits and
game/Wine waits are not covered. Absence of these sites is not zero cost.
No extra production fields, wire ABI, resource pins or wake obligations are
introduced. Enabled observation still perturbs scheduling; no observer-OFF
performance claim follows from its timings.

## Verification

Native tests exercise real pending receipt settlement without sleep-based
synchronization, immediate Ready, terminal failure, supersession, spurious
CV wakeups, owner-address reuse, capacity overflow, cross-window/incomplete
closure, truncated evidence, overwrite refusal and disabled execution.
The existing replay queue and 128-cell receipt predicate specs run alongside.
Initial arm64 focused 3/3 and x86_64 focused/install-name 4/4 pass. Existing
full TLA inventory passes in 226.45s; this is regression evidence, not a new
proof of the diagnostic sink or a pixel proof. Canonical native, Unix and both
PE build directories were rebuilt.
Post-export-hardening focused repeats also pass arm64 3/3 and x86_64 4/4.
The archived `native-waits.csv` has complete closure, zero overflow/incomplete/
unknown counts and a real production receipt wait qualified by owner 1,
query 71 and raw 1. Its additional resource row is the synthetic spurious-CV
fixture, not an application workload timing. Queue acquisition exceptions
remain catchable by the existing backend failure handler; the observer must
not introduce a `noexcept` termination boundary there.
Final exception-contract/static-assert review repeats pass arm64 3/3 and
x86_64 4/4 (`reviewed-*-tests.log`).

## Wild evidence boundary

R670 fails before Wine starts: staging notices changed PE provider dependencies
but its PATH lacks llvm-mingw. Canonical PE builds with the explicit toolchain
PATH resolve that setup issue. The failed launch log is retained, not an FPS
sample.

R671 selects encoded Presents 1750–2100, threshold 50us, actual occlusion /
async fences / covered Pending / publication dedup ON, Tape / scoped join /
receipt and backend sampling / PE sampler OFF. Split census, frame sampling
and external native app-thread CPU observation are enabled. The screenshot
shows Orca, not the game: reject visual/performance qualification and any
claim about an interference-free scene bottleneck. It also uses the collector
before the export-order hardening; preserve it as preliminary evidence, not
final-image qualification.

The runner ultimately returns PASS with no process failure, but reports
`unable to find onscreen window matching title: S.T.A.L.K.E.R.` and only
2,080 encoded Presents. The selected window therefore never closes; the
CSV is empty and the analyzer rejects it as missing a versioned header.
Do not interpret this as zero waits, successful visual qualification, or an
owner attribution. A foreground-controlled final-image repeat with a
reachable scene window remains open. No additional wild run is started while
foreground availability is unconfirmed.

## R672: final-image bounded wild evidence

Commit `5cf3874c`, Sikarugir cx24.0.7, Day/high 1280x720, perf profile.
After the user moved to desktop 0, the canonical runner completed with PASS,
return code 0, no timeout, no failures and no capture error. `actual.png`
contains the game, not the desktop. This single screenshot is not a continuous
pixel oracle. Evidence is retained in
`experiments/output/app-d3d9-stkcop-bench-identity-wait-r672-day/`:
`result.json`, `actual.png`, `run-policy.json`, `dxmt9.log`, `waits.csv` and
`thread-cpu.jsonl`.

The final collector closes the inclusive 1400–1800 encoded-Present window:
14 rows, 14 CV entries, one owner, zero overflow/incomplete/unknown identity
and zero excluded cross-window rows. These are totals over the window:

| Hook | Recorded rows | CV entries | Mutex acquisition | CV plus reacquisition |
|---|---:|---:|---:|---:|
| Query receipt | 3 | 14 | 1.059542ms | 2.816542ms |
| Query queue acquisition | 11 | 0 | 1.722540ms | — |

Two receipt calls enter the CV: query 6/raw 265026 at Present 1424
(five entries, 0.713875ms) and query 6/raw 265962 at Present 1432
(nine entries, 2.102667ms). Both settle to their captured raw frontier.
The third receipt row is already Ready after a 1.059417ms mutex acquisition
and does not enter the CV. All recorded rows belong to native thread 1059096.
Queue rows refer to queue 2, queries 5/6; their maximum acquisition is
0.348666ms. No hooked resource/lock/unlock CV row is recorded in this window.
Mutex-only rows below 50us are omitted by policy, not zero-cost acquisitions.

Frame-log brackets confirm a later slowdown outside the closed wait window:

| Encoded frame bracket | Samples | Wall-derived diagnostic FPS | CB/frame | Pass/frame | Maximum frame wall |
|---|---:|---:|---:|---:|---:|
| 1400–1800 | 401 | 31.605 | 7.623 | 20.653 | 80.486ms |
| 1801–2100 | 300 | 20.678 | 7.817 | 23.230 | 259.583ms |
| 2101–2500 | 400 | 11.465 | 37.847 | 51.343 | 297.741ms |
| 2501–2684 | 184 | 13.216 | 57.772 | 69.277 | 851.003ms |

FPS here is sample count / summed frame wall, not the benchmark result or an
observer-OFF performance comparison. Total encoded Presents are 2685.
The external CPU helper was started too late: it rejects the log with
`log read bound exceeded` before sampling. No CPU or CPU/wait decomposition
can be inferred from that failed companion artifact.

Verdict: hooked receipt blocking and recorded query-queue acquisitions are
small in 1400–1800; this does not attribute the later hitch. The next bounded
repeat should cover 1800–2600 (requiring 2602 Presents for closure), start the
CPU helper at process startup, and retain the uncovered global-drain,
queue-space/holder, GPU-resource and game/Wine wait hypotheses. Do not delete
synchronization or claim a GPU bottleneck from these diagnostic totals.

## Invocation

Set `DXMT9_WAIT_TRACE` to a fresh absolute CSV path, `DXMT_PERF_COUNTERS=1`,
and FIRST/LAST bounds through `DXMT9_WAIT_TRACE_FIRST_PRESENT` and
`DXMT9_WAIT_TRACE_LAST_PRESENT`. Launch through the canonical experiment
runner. Require the last selected Present plus two more to close the output.
Frame ordinals are process-global encoded Presents, not benchmark elapsed
seconds; select and validate the scene region anew if throughput changes.

Analyze with:

```sh
scripts/run_python.sh scripts/tools/summarize_wait_trace.py /absolute/run/waits.csv
```

Keep the CSV, `result.json` staged images, run policy, split/frame log and
external thread CPU evidence together. A missing trailer is unqualified,
not evidence that no waits occurred. The next attribution joins long raw
receipt waits to replay publication/space ownership, then investigates the
uncovered synchronization surfaces rather than assigning all residual wall
time to GPU or Wine.
