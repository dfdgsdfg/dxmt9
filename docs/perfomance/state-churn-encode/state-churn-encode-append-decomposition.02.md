---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 02
title: Retiring The Legacy Record Format Buys +2.1% GT2 Scene FPS, About A Third Of Its Ceiling
date: 2026-07-31
type: experiment-run
status: accepted-fps-win-small
source: experiments/output/app-d3d9-3dmark05-step7-base-r1; experiments/output/app-d3d9-3dmark05-step7-base-r2; experiments/output/app-d3d9-3dmark05-step7-base-r3; experiments/output/app-d3d9-3dmark05-step7-head-r1; experiments/output/app-d3d9-3dmark05-step7-head-r2; experiments/output/app-d3d9-3dmark05-step7-head-r3
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.01.md; docs/perfomance/present-pacing/present-pacing-post-defselect-cpu-attribution.04.md
---

# Retiring The Legacy Record Format Buys +2.1% GT2 Scene FPS, About A Third Of Its Ceiling

**Question / hypothesis.**
[append-decomposition.01](state-churn-encode-append-decomposition.01.md) opened
`appendRecordDirect` — `14.5%` of GT2's frame and `90%` of all PE recording cost
per
[attribution.04](../present-pacing/present-pacing-post-defselect-cpu-attribution.04.md)
— and found a serialize-parse-reserialize round trip: each record was written
into a scratch buffer in the **legacy** wire format, then re-parsed and
re-encoded into V2, at `9x` the cost of building it. It closed by naming the
only remaining move: "removing the round trip itself means retiring the legacy
intermediate format, which is a structural change, not a local one."

That structural change landed as Tasks 1-10 of
`docs/superpowers/plans/2026-07-29-pe-legacy-record-removal.md`: `peState_` now
emits `SparseStateV2Input` directly, and the legacy record format and the fat
`D9CDrawPrimitivePacket` are gone. Does it move GT2?

**Method.** Paired A/B, HEAD (`deda6fbe`) against `3276bf6e`, the last commit
before Task 1 — 29 commits apart. GT2, `perf` profile,
`DXMT9_PERF_FRAME_SAMPLING=1`, no gputrace, `--keep-frontmost`, 120 s runner
timeout, encoder breakdown off. Scene fps from per-frame `wall_ms`, never
presents-at-kill (H231). Steady body = frames `>= 30` and `<= 200 ms`.

The protocol exists because the first attempt at this measurement was voided for
confounding: the plan requires interleaving A/B/A/B/A/B, three runs per side, and
p95/p99 beside the median. Two additions were needed to actually satisfy it:

- **A prebuilt worktree, switched with `--build-root`** (`deda6fbe`). Interleaving
  across two commits otherwise means a rebuild at every switch, which re-injects
  the build thermal load immediately before each run.
- **A verified-quiet precondition.** The first run of the fixed sequence still
  failed it: `ninja -n` reported `34` pending targets in the HEAD tree (three
  commits had landed after it was last built), so its first staging compiled `26`
  objects immediately before the run while the freshly-built baseline worktree
  staged as a no-op. That is the same asymmetry in the same direction, just
  moved. Both trees were fully built and re-checked to `no work to do`, the
  three contaminated runs were discarded, and the sequence restarted. The final
  journal contains zero `Compiling C++` lines across all six runs.

Every run additionally got an equal `300 s` cooldown before launch, and the
staged `d3d9.dll` in the app prefix was hashed against both build trees rather
than trusting the flag.

**Result.** All six: `status=pass`, `profile=perf`,
`gpu_command_buffer_errors=0`, sampled wall `66.8-67.0 s` — GT2's fixed ~68 s
timeline in every run, so all six rendered the same scene.

| run | frames | median frame | **scene fps** | p95 | p99 |
|---|---:|---:|---:|---:|---:|
| base r1 | `1,124` | `54.47 ms` | `18.360` | `76.2 ms` | `105.1 ms` |
| base r2 | `1,123` | `54.50 ms` | `18.348` | `74.0 ms` | `105.2 ms` |
| base r3 | `1,099` | `55.13 ms` | `18.138` | `76.4 ms` | `116.5 ms` |
| head r1 | `1,154` | `52.52 ms` | `19.039` | `72.2 ms` | `106.8 ms` |
| head r2 | `1,141` | `53.36 ms` | `18.740` | `73.1 ms` | `110.5 ms` |
| head r3 | `1,142` | `53.40 ms` | `18.727` | `73.8 ms` | `104.6 ms` |

| | base | head | delta |
|---|---:|---:|---:|
| median of medians | `18.348` | `18.740` | **`+2.14%`** |
| mean of medians | `18.282` | `18.835` | `+3.03%` |
| `sampled_avg_fps` (mid) | `16.772` | `17.079` | `+1.83%` |
| median frame | `54.50 ms` | `53.36 ms` | `-1.14 ms` |

**The spreads are disjoint**: base `18.138-18.360`, head `18.727-19.039`. Every
head run beat every base run. Frame counts say the same thing independently —
`1,141-1,154` against `1,099-1,124` over the same fixed timeline.

**Verdict.** ACCEPTED as a small FPS win: **`+2.1%` GT2 scene fps**, with the
tail moving the same way (p95 `74.0-76.4` -> `72.2-73.8 ms`, also disjoint).
p99 overlaps and supports no claim.

**How strong is it, stated honestly.** Perfect separation of three against three
is the *most* an exact permutation test can report at this sample size:
`p = 1/20 = 0.05`. That is the floor of the design, not a strong result — it is
the difference between "consistent with a real effect" and "demonstrated." Three
independent facts point the same way (median, p95, frame count), and the
protocol removed the two confounds that voided the first attempt, but a claim
stronger than "small and consistent" needs more runs.

**It is about a third of the predicted ceiling, which is the expected shape.**
The design's §1 put the removable cost at *at most* `3.62 ms` of a `53.2 ms`
frame (`6.8%`), noting a fraction would survive because section encoding
remains. The realized wall saving is `1.14 ms`, `2.1%` — roughly `30%` of that
bound. This is the third instance of the pattern H193 and H214 record: under
Rosetta, PE CPU-ms removed over-credits wall-clock value by roughly `3x`. It is
now predictive rather than anecdotal, and should be applied as a discount to the
next PE-side estimate before the work is scheduled, not after it is measured.

**What this does not say.** Nothing here was measured on GT1, GT3, or SFIV;
those workloads have different PE call mixes and this A/B says nothing about
them. The Xcode/encoder-counter mechanism proof that would attribute the
`1.14 ms` specifically to the removed round trip was not run — the mechanism was
already measured directly in
[append-decomposition.01](state-churn-encode-append-decomposition.01.md), and
this run answers the separate question of what reaches wall clock.

**And the reason the refactor is not judged by this number.** Nothing in Tasks
1-10 was justified by a perf claim; the plan says so explicitly and instructs
that a null be reported as a null. The result happens to be positive and small.
The structural case — one wire format instead of two, no intermediate to keep in
sync — stands on its own either way.

**Related.**
[append-decomposition.01](state-churn-encode-append-decomposition.01.md) ·
[attribution.04](../present-pacing/present-pacing-post-defselect-cpu-attribution.04.md) ·
[state-churn-encode](index.md) · [overview-3dmark05-gt2](../overview-3dmark05-gt2.md)
