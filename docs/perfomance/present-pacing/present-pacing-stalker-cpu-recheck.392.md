---
domain: present-pacing
workload: STALKER CoP Day high max-threads 2 snd_targets 16
title: "H392: Recheck CPU supply and late-scene costs after fixed comparison"
type: evidence
status: mixed-supply-and-service-pressure-next-owner-open
updated: 2026-09-28
source: traces/stkcop-recheck-h392/{run.py,frames.py,frames.json,owners.py,owners.json}; traces/stkcop-recheck-h392/{diagnostic,diagnostic-tail-short}/{supply.csv,cpu.json}; traces/stkcop-recheck-h392/diagnostic-tail/supply.csv; experiments/output/app-d3d9-stkcop-bench-recheck-h392-{baseline,diagnostic,diagnostic-tail,diagnostic-tail-short}/result.json
related: present-pacing-fixed-projection-validation.391.md; present-pacing-mark-ticket-coverage.386.md
---

# H392: The remaining problem is not one uniform-lookup bottleneck

## Method and qualification

HEAD `fd2fafa5`, same staged images in all four runs; Sikarugir CX 24.0.7,
Day/high 1280×720, `-max-threads 2`, `snd_targets 16`. All runs pass with zero
GPU command-buffer errors. Baseline disables supply/append diagnostic scopes;
existing perf counters and per-frame snapshots remain ON. It is not an
instrumentation-free renderer measurement. No concurrent benchmark/build was
launched. Computer Use restores the game window during startup and provides
sampled visual supervision; it does not prove continuous absence of interference.

Runs are baseline, supply window 2700–2720, supply window 3000–3020, and a
narrowed 3000–3008 retry. The wide final trace overflows by 29,763 events and
is **rejected**, not partially analyzed. The retry is closed with no overflow
or incomplete writers; trimming one endpoint on each side retains six intervals.
The earlier valid window retains sixteen intervals, 2702–2718. Both analyzers
require actual scene draws, complete identity/clock evidence and separate
app/replay/encode threads. No runtime policy or implementation changed.

Same Present ordinal is **not exact scene/input equivalence between runs**.
For example, baseline 2702–2717 averages 3,147 draws, diagnostic 2,369. Do not
use their FPS difference as calibrated observer cost or a code regression.
The baseline has 3,328 sampled Presents versus 3,185 in the first diagnostic.
Incomplete fixed frame windows remain explicitly unqualified in `frames.json`.

## Baseline progression

FPS is selected frame count divided by summed positive frame wall time, not
the mean of instantaneous FPS. All selected frames exceed 100 draws. CPU-named
runtime scopes are elapsed counters, not native CPU or additive frame costs.

| Baseline frame interval | FPS | Draws/frame | Passes/frame | CB/frame | Encode scope ms/frame | GPU CB sum ms/frame |
|---|---:|---:|---:|---:|---:|---:|
| 300–599 | 32.43 | 2,875 | 18.0 | 5.0 | 19.06 | 8.59 |
| 1500–1799 | 47.81 | 1,377 | 27.9 | 5.94 | 11.61 | 8.54 |
| 2702–2717 | 24.40 | 3,147 | 18.0 | 5.0 | 22.47 | 9.15 |
| 3000–3199 | 15.35 | 4,202 | 80.85 | 10.16 | 34.62 | 22.04 |

Late median/p95 frame wall is 59.67/80.45ms. EncodeDraw scope rises to
22.66ms/frame. These show increased scene and pass work, not proof of GPU
saturation or attribution of every cut to queries. No README score is changed.

## Native CPU and supply, diagnostic only

Thread CPU envelopes have slightly different endpoints; scopes overlap and
must not be summed. The app includes game, Wine, PE and synchronous Unix.
High-frequency tracing itself contributes cost and changes scheduling.

| Observation | 2702–2718 (16 intervals) | 3001–3007 (6 intervals) |
|---|---:|---:|
| Window wall ms | 949.35 | 380.18 |
| App native CPU ms/Present | 33.08 | **55.67** |
| Replay native CPU ms/Present | 20.16 | 42.64 |
| Encode native CPU ms/Present | 19.27 | 41.06 |
| App / Replay / Encode core occupancy | .558 / .340 / .325 | **.888 / .676 / .648** |
| Replay queue empty | **67.8%** | **33.5%** |
| Average replay queue depth | 2.93 | 12.04 |
| Encode gap awaiting unpublished input ms/Present | 39.56 | 20.73 |
| Publication→encode median / p95 ms | .069 / 15.36 | 18.62 / 25.85 |

The final diagnostic actually contains high-pass work: 4,200 draws, 78.5
passes, 10.5 CBs per frame. It is not numerically identical to baseline's six
corresponding frame ordinals (4,897 draws, 84.5 passes).

In the earlier window downstream supply gaps dominate elapsed time. The final
window has both higher application CPU demand and real queued downstream work,
but Replay still exhausts its input and Encode still has unpublished-input
gaps. Neither window supports declaring Replay the sole throughput limiter.

The existing fixed-arrival model's half/zero Replay sensitivities are near
zero (within 0.04% in both windows). This only says scaling local Replay with
arrivals, dispatch gaps and GPU feedback fixed barely changes these short
window endpoints. It is **not an unconditional speedup ceiling**: reduced
producer contention and feedback are explicitly excluded.

## Lock attribution remains real, but not the whole explanation

| Whole-window observation | Earlier window | Final short window |
|---|---:|---:|
| Mark-ticket attempts | 1,173 | 1,397 |
| Acquisition elapsed total ms | 38.343 | 34.383 |
| Same-mutex other-thread holder union ms | 27.690 | 25.799 |
| Compatibility binding snapshot overlap ms | 6.010 | 3.726 |
| Compatibility reserve/mark/append parent overlap ms | 20.148 | 17.060 |
| ↳ Append child overlap ms | 14.359 | 12.186 |
| ↳ Resource mark child overlap ms | 5.122 | 3.937 |

Parents include children: do not add these rows. In the final window mark
acquisition averages 5.73ms/Present; the observed append child overlaps
2.03ms/Present. Neither is guaranteed removable time. StateBlock queue-space
waits add 4.35ms over six intervals (155 attempts); mutex-holder attribution
does not apply to that wait kind. No complete writer-slot or inflight-cap wait
occurs inside either trimmed window. This does not prove their global absence.
Both valid lock analyses report zero incomplete attempts/holds.

## Decision

Pause blind fixed-component/index micro-optimization. The evidence does not
show that the remaining ~0.30ms GT2 component search limits STALKER supply.
Next isolate **late application native CPU** into game/Wine/PE/synchronous
Unix using low-perturbation stack sampling with supply tracing OFF, and bind
samples to scene/draw/pass signatures rather than assuming equal frame IDs
mean identical input. The current .888-core app envelope is the strongest
new lead, but is not PE-exclusive or observer-free.

Keep late Encode/pass work as the second axis. More commands, pass boundaries
and queued encoding explain why the tail differs from the preceding input-gap
window. Parallel encode is not yet established as the best fix: no measured
parallelizable fraction or changed critical path was demonstrated here.
The third axis is compatibility lock residence, with actual STALKER ownership
and semantic boundaries preserved; do not infer its removable cost from GT2.
