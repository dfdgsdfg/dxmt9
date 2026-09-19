---
domain: present-pacing
workload: STALKER CoP Day
title: "H324: promoted receipt polling census and sampled phase cost"
type: evidence
status: measured
updated: 2026-09-19
source: experiments/output/app-d3d9-stkcop-bench-receipt-poll-cost-r649-day/result.json; experiments/output/app-d3d9-stkcop-bench-receipt-poll-cost-r649-day/xrEngine_dxmt9.log; experiments/output/app-d3d9-stkcop-bench-receipt-poll-cost-r649-day/daybenchmark.result; experiments/output/app-d3d9-stkcop-bench-receipt-poll-cost-r649-day/actual.png
related: present-pacing-query-poll-receipts.323.md
---

# H324: fallback is negligible; captured-prefix wait and backend poll remain

The [H323](present-pacing-query-poll-receipts.323.md) promotion was committed as
`029cef81`. R649 uses that base plus the receipt observer changes. This is a
diagnostic run, not an FPS comparison or a claim of total removable CPU cost.

## Observer contract

`DXMT9_QUERY_RECEIPT_OBSERVER=counts` records all receipt attempts without clocks.
`=1` adds deterministic one-in-64 timing samples. OFF performs one cached mode
check; the disabled template has no diagnostic state, clocks, TLS accesses or
aggregation locks. A fixed census records FLUSH, result, route and qualified
snapshot kind. Kind is unknown before qualification; no mutable query state is
read merely for classification. Exceptions are counted in the error population
and rethrown unchanged, not converted to a new public HRESULT.

Sampled intervals partition capture, captured-raw replay wait, synchronized
snapshot acquisition/copy, backend poll and final checks/early-return remainder.
They are elapsed intervals, not thread CPU. Snapshot combines ledger and snapshot
lock work with copying; it is not isolated mutex contention. Backend S_FALSE
does not distinguish active-query status from pending ordinary/initializer GPU
completion. Nor does it prove GPU saturation. Aggregation/logging occurs after
the attempt under an observer-only mutex and is excluded from phase timings;
enabled census overhead still perturbs runtime. At most 64 periodic reports
plus one final report are emitted. The last complete report owns final totals.

One-in-64 samples can alias periodic work and miss rare stalls. Do not multiply
sampled sums by 64 to assert whole-run cost, divide them by all Presents as a
measured cost/Present, or treat sample percentages as whole-frame percentages.
PE preparation/crossing and the subsequent global-drain fallback are outside
the measured receipt attempt. The old query observer remains OFF here.

## R649 qualification

Sikarugir cx24.0.7, Day High, `renderer_r2.5`, 1280x720 windowed, perf profile,
Info logging, health counters ON, validation OFF. Receipts are unconditional;
actual occlusion and async fences are ON, END coalescing and legacy query/frame
observers OFF. The exact launch is:

```sh
PATH=/Users/dididi/llvm-mingw/bin:$PATH \
DXMT_EXPERIMENT_PROFILE=perf DXMT_LOG_LEVEL=info DXMT_VALIDATE=0 \
DXMT_PERF_COUNTERS=1 DXMT9_QUERY_RECEIPT_OBSERVER=1 \
DXMT9_METAL_OCCLUSION=1 DXMT9_ASYNC_QUERY_FENCES=1 \
DXMT9_COALESCE_QUERY_ENDS=0 DXMT9_QUERY_OBSERVER=0 \
DXMT9_PERF_FRAME_SAMPLING=0 DXMT_STKCOP_LANE=day DXMT_STKCOP_PRESET=high \
DXMT_STKCOP_RENDERER=renderer_r2.5 DXMT_STKCOP_VID_MODE=1280x720 \
scripts/run_python.sh scripts/run_apps/run_experiment.py run \
  app-d3d9-stkcop-bench --wine-id sikarugir-cx-24.0.7 --timeout 300 \
  --output-suffix receipt-poll-cost-r649-day --capture-delay-sec 120
```

An initial staging attempt failed before Wine launch because llvm-mingw was not
on PATH. Explicit canonical PE rebuilds corrected this tooling issue. Native,
Unix and both PE builds completed before the successful launch. All five staged
size/hash prefixes match canonical outputs; the Unix image is
`16118dfdd22f2477`. ABI handshake succeeds. No session-scheduled builds, tests,
TLC, agents or profilers run during the engine lifetime; spot process checks
find no competing compute. User/OS interference is not comprehensively excluded.
No session-driven focus restore was performed; foreground residency is not proved.

The runner reports PASS, returncode 0, no timeout/capture error/failure markers,
2,481 benchmark rows and 2,570 encoded Presents. All eight health counters are
present and zero: chunk_reject, command_chunk_rejects, gpu_command_buffer_errors,
draw_skipped_no_pipeline and pipeline_build_fail_{draw,function,library,pso}.
`actual.png` shows the courtyard, buildings and vegetation, not loading. This
single spot is not continuous/full-sequence or frame-aligned correctness proof.
The benchmark average is **19.085360 FPS**, diagnostic only; do not compare it
as a policy regression against R648's differently observed 20.381388 FPS.

## Complete census

| Population | Calls |
|---|---:|
| Receipt attempts / FLUSH attempts | 488,901 / 488,901 |
| Qualified handled | 488,900 |
| Fallback, non-raw/unissued | 1 |
| S_OK | 25,898 |
| S_FALSE | 463,002 (94.70%) |
| Error, superseded, not-ready recheck | 0 |
| Qualified occlusion / event | 486,362 / 2,538 |
| Unknown kind | 1 |

Result, route and kind populations each sum to the same 488,901 attempts.
That is 190.23 calls per encoded Present (whole-run normalization, including
loading). Occlusion accounts for 99.48% of attempts. In this workload, broad
fallback support expansion is not the next plausible payoff: only one call
falls back. Repeated pending polling is real, but its wall-time opportunity
still requires separating required completion latency from polling overhead.

## Sampled phase costs

There are 7,640 valid samples, zero invalid clocks: 398 S_OK, 7,241 S_FALSE,
one fallback. Their disjoint intervals total 112.419090 ms.

| Phase | Sampled sum, ms | Share of sampled attempt time |
|---|---:|---:|
| Admission capture | 1.480388 | 1.32% |
| Captured-raw replay wait | 67.392973 | 59.95% |
| Snapshot acquisition + copy | 1.785219 | 1.59% |
| Backend poll | 40.883219 | 36.37% |
| Final checks/remainder | 0.877291 | 0.78% |

These results do not support making snapshot copying the next primary target.
The replay-wait sample sum is strongly nonuniform: at periodic report 64
(262,144 attempts), its sum is only 0.536292 ms; the final report adds about
66.857 ms. There are no per-sample maxima/histograms or source identities yet,
so a small number of late waits cannot be distinguished from a broader change.

The backend owner is `Device::pollQuerySnapshot` in `src/d3d9/core_resources.cpp`,
then `CommandQueue::pollQueryFence` in `src/dxmt9/dxmt9_command_queue.cpp`.
The latter acquires the queue mutex, reads ordinary/initializer progress,
classifies the receipt and, for pending FLUSH, requests submission. No change
to those obligations is made here. Next: bounded wait-tail/source attribution
and backend queue-acquisition versus classification/submission-request split,
before proposing cached pending answers or removing a FLUSH action.

## Verification

Native sampled-mode focused tests pass 5/5, counts-only 3/3, observer-OFF 3/3;
x86_64 sampled-mode bridge/snapshot/install-name checks pass 4/4. Native census
tests pin disjoint fake-clock intervals, result/sample denominators, fallback,
invalid clocks, nested observer restoration, exactly-once calls and unchanged
exceptions. Bridge fixtures retain sentinel/terminal/whole-raw/later-tail checks
with diagnostics enabled. Full TLA verification passes; no model/policy change
was made for this observational surface. Logs:
`build/meson-logs/query-receipt-observer-{tests,counts-tests,off-tests,x86-tests,tla}.log`.
