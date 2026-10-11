---
domain: present-pacing
workload: STALKER Day high, frozen default policy, full-root CPU diagnostic ABBA
title: "Whole-root CPU measurement works but does not transfer to the default workload"
type: evidence
status: closed-default-transfer-rejected-no-owner-or-removal-verdict
updated: 2026-10-08
source: traces/stkcop-root-game-h477/{contract.md,run.py,endpoints.py,join.py,analyze.py,summary.json}; traces/stkcop-root-game-h477/{default-r1,diagnostic-r1,diagnostic-r2,default-r2}/{receipt.json,executed-runner.py,endpoints.json,thread-join.json,setup.sample.txt,progress.jsonl,analysis.json,tail.png}; traces/stkcop-root-game-h477/restore-{default-r1,diagnostic-r1,diagnostic-r2,default-r2}/{inputs.json,restored.json,recovered}; experiments/output/app-d3d9-stkcop-bench-root-game-h477-{default-r1,diagnostic-r1,diagnostic-r2,default-r2}/result.json
related: present-pacing-root-reader.476.md; present-pacing-root-objects.475.md; present-pacing-root-observer.474.md; present-pacing-visibility-budget.468.md; present-pacing-run-construction-removal.451.md
---

# Close the live whole-root attribution method

The finite default/diagnostic/diagnostic/default comparison completes normally
four times, without runtime default or production-source changes. H473's clock,
H475's actual-object interposer and H476-r2's reader operate in the game, but
their **default-transfer gate fails**. This experiment does not establish the
largest default dxmt9 single-thread owner or correctness-qualified removable CPU.
Do not repeat it, relax its thresholds, subtract synthetic hook costs or extend
it into another clock/reader qualification exercise.

## Frozen execution and endpoint scope

Both routes retain visibility OFF, Day/high 1280x720, `-max-threads 2`,
`snd_targets 16`, and draw limit 0. H468's five frozen renderer images remain
the control; diagnostic runs replace only both D3D9 DLLs with H475's images.
All four catalogue results pass under the perf profile with product counters,
frame sampling and validation OFF. The external frame/camera/native observer
is active in every run; these are not observer-OFF FPS results.

At scene elapsed 102 and 110 seconds, the collector accepts the first coherent
inactive root read within 250 ms. Complete native thread-counter snapshots
enclose each reader call. Default runs use the same external brackets without
the root hook. Diagnostic-r2's final endpoint takes two attempts; the others
take one. No selected slot has a clock fault or changes its process birth,
attachment, slot or guest/native thread identity.

Setup-only stack samples identify `ReplayOffloadWorker::run`. Numeric Darwin
TIDs are joined to opaque libproc thread handles by uniquely enclosing both
user/system counters; the identities are not equated by their numeric values.
All joins finish before the target interval. In diagnostic runs, independent
numeric-TID Primary queries also fit within the named handle's CPU envelopes.
These stack samples identify workers; their sample counts do not price CPU.

Native CPU bounds use `before_end - after_start` and
`after_end - before_start`, with a 2 us aggregate reference allowance. Completed
SASH vector entries are bracketed around the actual root endpoint envelopes.
Root raw sums are differenced, then normalized once by 125/3 with outward
rounding. Decimal precision in the machine report is not physical clock
accuracy. There is no interpolation of CPU and no empty-hook subtraction.

## Retained within-run observations

The rows below are **not matched-scene A/B savings**. The failed camera gates
prohibit interpreting their differences that way. Units are CPU milliseconds
per completed frame; scopes on different threads must not be summed as latency.

| Run | Completed frames | Whole Primary | Replay | Encode | Instrumented Primary D3D9 roots |
|---|---:|---:|---:|---:|---:|
| Default-r1 | 151–152 | 47.751–48.070 | 28.713–28.909 | 29.579–29.778 | Unavailable |
| Diagnostic-r1 | 102 | 68.557–68.560 | 29.632 | 29.317 | 22.028 |
| Diagnostic-r2 | 107 | 70.219–70.220 | 30.463–30.483 | 29.550–29.555 | 22.529 |
| Default-r2 | 128–129 | 54.254–54.678 | 31.011–31.259 | 32.112–32.367 | Unavailable |

Diagnostic-r1/r2 close 1,589,495 / 1,660,642 outer roots, approximately
15,500–15,600 per completed frame. Their root CPU includes observer work inside
the bracket, original function ancestry and synchronous Wine/provider calls.
It is neither exclusive PE payload nor removable work. Whole-Primary changes
also contain changed workload/scheduling effects and must not be labeled pure
timer overhead.

## Rejection is decisive, not an endpoint arithmetic failure

The frozen camera gate compares seven corresponding endpoint-relative knots,
including both surrounding frame/camera observations, against 2 m / 3 degrees.
It does not select a new CPU interval after examining costs. Every pair fails:

| Pair | Maximum camera separation | Additional CPU gate evidence |
|---|---:|---|
| Default-r1 → diagnostic-r1 | 2.466 m / 3.131 degrees | Whole Primary ratio 1.426–1.436, outside +/-5% |
| Diagnostic-r2 → default-r2 | 23.083 m / 30.514 degrees | Primary and Encode outside +/-5% |
| Default-r1 → default-r2 | 22.768 m / 29.887 degrees | Primary, Replay and Encode outside +/-5% |
| Diagnostic-r1 → diagnostic-r2 | 2.026 m / 2.574 degrees | Three CPU ratios fit +/-5%; camera still fails |

Default-r2 reaches the observed running end at 122.999 elapsed seconds,
versus 119.055–119.654 in the other runs. Equal elapsed seconds therefore do
not establish equal late-scene work. The retained observations do not determine
why its scene progressed differently. No pause/inactive state or observer UI
action overlaps the target intervals. Do not attribute the drift to user input
or a specific driver/game stall without further evidence.

The first pair's Replay/Encode ratios individually fit +/-5%, but cannot rescue
the failed Primary and camera gates. An independent read-only endpoint review
agrees with the arithmetic and this scope limitation.

## What remains unknown

Default-r1's whole-Primary upper bound is 48.070 ms/frame, above Encode's
29.579 ms/frame lower bound. To rule Primary out as the largest dxmt9 owner
through subtraction would require an independently justified **default non-root
lower bound above 18.491 ms/frame**. H477 does not provide one. Its diagnostic
non-root residual cannot be transplanted after the failed transfer gate.

An unnamed default-r1 native thread also consumes 47.276–47.591 ms/frame without
a renderer ownership join. Disappearing endpoint threads and the zero-valued
Rosetta handle remain explicitly unpriced, not zero CPU. The reducer initially
stopped at those coverage failures; its final report preserves them as
`unpriced_threads` and leaves `all_thread_coverage=false` while reporting valid
persistent workers. This is reporting recovery, not a relaxed owner gate.

Encode is slightly above Replay in each default observation, but the gap is
small and the two default windows are not scene-matched. Neither a robust global
owner ranking nor a FPS critical-path ranking follows. H468's visibility
removal budget remains rejected; H451's approximately 1 ms captured-body saving
remains a different-input local result, not the missing tail budget.

## Consequence and recovery

The live all-call clock method is closed for default attribution on this
workload. Further optimization evidence must use an identity-matched late
input and an actual correctness-preserving work-removal comparison; another
elapsed-time benchmark or a smaller clock fixture cannot answer that question.
Do not silently substitute the older capture29 for this tail, or call a local
delta the globally largest removable cost. The original owner/removal objective
remains incomplete.

All four catalogue runs terminate normally; no game/helper remains live.
After each run, all five actual
prelaunch shared Wine renderer files are restored and hash-checked, including
the older prelaunch DLL/provider set. Four new clock-service/stub files per run
are moved to that run's recovery directory. No user file is deleted, no process
is left running, and no product default, README FPS or production binary is
changed by this experiment.
