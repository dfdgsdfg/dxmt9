---
domain: present-pacing
workload: STALKER CoP Day high 1280x720, controlled default production lane
title: "Admission-authority live transfer does not clear baseline variation"
type: evidence
status: live-transfer-inconclusive-runtime-restored
updated: 2026-10-14
source: traces/stkcop-validation-live-h509/{contract.md,amendment-1.md,supplemental-join.md,inputs.json,run.py,analyze.py,verify.py,join_all.py,summary.json,verification.json}; traces/stkcop-validation-live-h509/{A1,B1,B2,A2}/{receipt.json,progress.jsonl,thread-join.json,analysis.json,loaded-images.txt,post-tail.png,restore/restored.json}; experiments/output/app-d3d9-stkcop-bench-validation-live-h509-{a1,b1,b2,a2}/{result.json,daybenchmark.result,stkcop-controls.txt,stkcop-requested.ltx}
related: present-pacing-validation-authority.508.md; present-pacing-validation-callers.507.md; present-pacing-root-game.477.md; ../../../specs/backend/encode-scheduling/gap.md
---

# Offline work removal is real; live economy is not established

**Endpoint correction:** [H512](present-pacing-cpu-counter-freshness.512.md)
falsifies instantaneous libproc-counter freshness, including three examples
in this experiment's B1 raw data. CPU ranges below are historical reported-
counter arithmetic, not certified true-CPU enclosures. The outcome remains
no promotion; the narrow pair bounds and regression guard are not qualified.

H508's archived adjacent admission-authority candidate was tested in four
complete live Day episodes, fixed **A1/B1/B2/A2** order. Both pairs show lower
Replay CPU, but their mean saving **0.505 ms/completed frame** is below the
baseline A1/A2 difference **1.090 ms/frame**. The predeclared exploratory
transfer gate fails. This does not invalidate H508's same-input result, prove
that removed work is free, or establish a live regression. No runtime promotion
or FPS claim follows; source stays restored and shared images were restored.

## Fixed conditions and actual image identity

- A: H506 `candidate-r2`, Unix SHA-256
  `0bf898b61b9da5240936e0a676b1e5d734337f35429c4e60e53a04bca71904e6`.
- B: H508 normal-diagnostics `candidate`, Unix SHA-256
  `98cbddf50864f1d16affd8fd3565ca11e092f8270f6f9c056b5a62be2f0bf7d1`.
  This is not the private validator-timer-suppressed build.
- Same frozen x86/x64 PE files, game, renderer, observer and camera reference.
  All staged images are hashed; `vmmap` confirms the expected live Unix path.
  The Unix image has no `libdxmt9_native.dylib` runtime dependency.
- Sikarugir CX 24.0.7; Apple M1, macOS 27.0.1, Xcode 27.0 / Metal 32023.921.
  Catalogue `perf`, high, 1280x720, `-max-threads 2`, `snd_targets=16`.
  Requested game controls/configuration are byte-identical in all runs.
  This is the established controlled lane, not unrestricted game defaults;
  the audio cap changes quality.
- Production-counter occlusion (`DXMT9_METAL_OCCLUSION=0`), async replay;
  product counters/frame sampling, capture, validation and GPU tracing OFF.
  No injected query results. No build or additional workload is launched during
  the measurement. Host scheduling/thermal interference is not proven absent.

External read-only game observation uses seeded 15--25 ms jitter. Native
libproc thread counters are read every 0.5 s. A setup-only one-second sample
identifies the current process's Primary and Replay; unique bracketing native
counters join numeric Darwin TIDs to opaque libproc handles. No earlier run's
handle is reused, and sample populations never price CPU.

## Spatial selection and qualifications

The seven H477 default-r1 camera knots select the late building scene by
position/direction, not elapsed time. First sequential stable entries must be
within 1 m / 1.5 degrees, with a 2 m / 3 degree corridor between knots. Each
endpoint brackets a stable game observation with native CPU snapshots, spanning
at most 10 ms. Frame and completed-result counts must be monotone; observed
inactive/paused/reset state rejects the interval. All four runs reach all seven
knots. Maximum paired knot differences are 0.260 m / 0.350 degrees.

CPU ranges below use native counter enclosure, 2 us rounding allowance, and
conservative completed-frame counts from the endpoint outer observations. No
interpolation or observer-cost subtraction is used. All four frame enclosures
collapse to one count. These were intended as deterministic enclosures, not
confidence intervals, but H512 invalidates their CPU-freshness premise. They
are retained as reported-counter differences only. A zero opaque handle remains unpriced in each run;
all-thread coverage is false. Named/identified workers below are covered.

| Run | Completed frames | Mixed Primary CPU ms/frame | Replay CPU ms/frame | Encode CPU ms/frame | Selected-tail FPS | Whole-Day reported FPS |
|---|---:|---:|---:|---:|---:|---:|
| A1 baseline | 132 | 54.026--54.031 | 32.000--32.005 | 31.701--31.715 | 16.888--16.894 | 27.487 |
| B1 candidate | 132 | 54.075--54.086 | 31.783--31.789 | 31.247--31.247 | 16.456--16.460 | 27.370 |
| B2 candidate | 134 | 54.924--54.936 | 32.299--32.299 | 32.571--32.588 | 17.045--17.051 | 26.837 |
| A2 baseline | 121 | 57.221--57.234 | 33.089--33.097 | 32.709--32.715 | 14.695--14.698 | 26.451 |

CPU is per completed benchmark frame, not an exact Present/source identity.
Thread costs overlap and must not be summed as frame wall time. Whole-Day
averages are independently reconstructed from the original `.result` per-frame
durations; they are not interchangeable with selected-tail FPS. Screenshots
were taken after the selected interval and before episode completion. They
show no obvious corruption, but are not same-frame exact pixel oracles.
Camera matching does not prove identical draw/query/source work, and captures
were deliberately disabled. H508 remains the exact original-image/query proof.

## Predeclared result and retained administrative failure

The exploratory gate requires both conservative Replay pair savings positive,
mean pair midpoint saving above the absolute A1/A2 midpoint difference, and
less than 5% upper Primary/Encode regression in both pairs.

| Test | Result |
|---|---|
| A1 minus B1 Replay | 0.211--0.222 ms/frame |
| A2 minus B2 Replay | 0.789--0.797 ms/frame |
| Mean paired midpoint saving | 0.505 ms/frame |
| Baseline A1/A2 midpoint difference | 1.090 ms/frame |
| Other-worker regression guard | Pass |
| Exploratory live transfer | **Fail: saving below baseline variation** |

There are only two pairs, no extra runs/outlier removal, and no statistical
significance claim. Candidate tail FPS decreases in the first pair and
increases in the reverse pair. Baseline Secondary CPU changes from
7.139 to 15.974--15.982 ms/frame; a large unnamed worker changes from
49.783--49.793 to 57.469--57.478 ms/frame. Their variation cannot be assigned
to this Replay scan removal, nor can this experiment identify its cause.

All four catalogue results retain `status=fail` with **only `missing_capture`**:
automatic capture was intentionally delayed to 600 s to keep it outside the
measurement, while the game exited normally sooner. Game returncode is zero,
no run timed out, original benchmark results exist, and external post-tail
screenshots exist. A1's initial supervisor treated catalogue exit 1 as a hard
failure. Before CPU analysis or B1, `amendment-1.md` narrowed completion to
explicitly allow this administrative-only result. A1's failure receipt and all
catalogue failures remain untouched; no scene/CPU/performance gate was relaxed.
These must not be presented as four unqualified catalogue passes.

## Remaining owner gap, not another optimization claim

An additional A2-only setup lookup reused the existing sample's TIDs, without
another stack sample or target mutation. It completed **70.716 s before** the
first endpoint and uniquely joined 28/29 TIDs. This asymmetry is recorded in
`supplemental-join.md`. Two large unnamed workers resolve to numeric TIDs
4729527 and 4729824; their early stacks are dominated by poorly unwound Wine
syscall frames. That proves neither their late executing function nor Wine CPU
ownership. They stay owner-unresolved; the join is not transferred to A1/B1/B2.

The largest default dxmt9 single-thread CPU owner is **still unresolved**.
Primary includes game, Wine, PE and synchronous Unix. Replay and Encode are
both around 31--33 ms/frame here, and their order changes between runs. H508's
2.839 ms per captured interval must not be substituted for a live per-frame
removal estimate or converted directly to FPS.

The next useful evidence is an independently qualified split of mixed Primary
and the large unresolved workers, not another broad policy sweep or an
unbounded repeat of this candidate. A lower-perturbation root-residency witness
is a possible design subject, not an implemented/approved result; the failed
H477 whole-root per-call clock experiment must not be recycled as evidence.

`verify.py` independently rederives CPU arithmetic, original game averages,
image/config identity and all final restoration hashes. Shared Wine/prefix
images and prior results were restored; no game/helper remains. Runtime source
and canonical baseline images are unchanged. H501--H508's existing dirty work
is preserved; no commit/push or candidate promotion was performed in H509.

The explicit H509 provenance audit and `git diff --check` pass. The broader
default provenance audit still fails on eleven missing historical build-log
paths in three unrelated leaves (`capacity-ownership-recovery.05`,
`gpu-position-invariance.04`, and `query-default-promotion.316`). Those older
documents/evidence were not rewritten or waived by this experiment.
