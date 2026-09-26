---
domain: present-pacing
workload: STALKER CoP Day high max-threads 2 snd_targets 16
title: "H380: Capped-audio lane still has an upstream CPU supply limit"
type: evidence
status: partial-app-cost-attribution
updated: 2026-09-26
source: traces/app-d3d9-stkcop-audio-supply-h380/{run.py,receipt.jsonl,supply.csv,supply-analysis.json,wait.csv,sample.txt}; experiments/output/app-d3d9-stkcop-bench-audio-supply-h380/{result.json,dxmt9.log,stkcop-controls.txt}
related: present-pacing-audio-cap-abba.379.md; present-pacing-tuned-tail-wait.371.md
---

# H380: Audio control exposes more app CPU, not an encode saturation verdict

Run Day/high 1280x720 with `-max-threads 2`, `snd_targets 16`, bounded supply
and selected wait traces at Presents 2700–2720. A three-second native sample
starts after observed frame 2701. The run passes with zero reported GPU errors
and the same five staged artifacts as H379. No builds/tests run concurrently.
This diagnostic has observer overhead and is not another FPS qualification.

Trimmed supply bounds 2702–2718 contain 16 intervals over 804.803ms. Scene gate
passes for all selected frames. Trace analysis accepts the transcript; selected
wait trace reports closed=1, overflow/incomplete/unknown=0 and cv_entries=0.

| Metric | Capped-audio window |
|---|---:|
| App thread CPU / Present | 43.488ms |
| App core utilization | 0.866 |
| Replay thread CPU / Present | 23.253ms |
| Replay core utilization | 0.463 |
| Encode thread CPU / Present | 24.100ms |
| Encode core utilization | 0.479 |
| Import scope CPU / Present | 2.027ms |
| Replay input-wait wall / Present | 25.814ms |
| Replay queue empty fraction | 55.58% |
| Encode unpublished-input gap / Present | 25.170ms |
| Encode ready dispatch/tail gap / Present | 0.449ms |
| Replay ready→pop median / p95 | 0.803 / 4.220ms |
| Publication→encode median / p95 | 0.099 / 14.529ms |

Scopes overlap and must not be summed. Import is not all synchronous Unix;
app CPU includes game, Wine, PE and synchronous Unix. Thread endpoint envelopes
differ slightly. Queue-ready excursions and encode latency spikes exist even
though neither worker saturates across this bounded window.

## Native corroboration and replay sensitivity

Each selected thread has 132 native sample observations:

- Primary: 19 in `NtWaitForAlertByThreadId`, with the remaining observations
  requiring guest/owner attribution rather than a PE-only label.
- Secondary: 93 in that native wait API.
- Replay: 74 in the input queue's condition-variable pop wait.
- Encode: 68 in `reserveReadySlotBatchPrefix`'s input wait.

Native sampling covers a longer, perturbed interval than the supply trace.
Counts are stack observations, not CPU shares or exact wait-duration fractions.
The empty selected-site wait trace does not exclude game/audio waits.

The existing fixed-arrival model changes window throughput by approximately
zero for half/zero replay service (reported −0.010%/−0.011%, boundary-scale
effects, not proof that faster replay hurts). Median modeled Present advances
are 0.791/1.288ms. This conditional model holds source arrivals, dispatch and
encode work fixed and excludes feedback from contention; it is neither an
unconditional optimization ceiling nor measured FPS improvement.

## Relation to previous results

H371's original-audio window had app/replay/encode utilization
0.452/0.241/0.217 and replay input wait 73.128ms/Present. H380 shows much less
idle time and a busier primary. However ordinal 2700 is not a matched scene
across different-throughput runs; do not present the diagnostic-window FPS
10.03→19.88 as a cap speedup. The qualified effect remains H379's reversed-order
observer-OFF comparison (+7.48% mean reported FPS).

## Next gate

The direct remaining lead is **app-side CPU supply ownership** under the
controlled audio setting. Resolve the primary's execution into game/Wine/PE/
synchronous Unix using bounded guest module/PC evidence, with waits separately
classified. Only then select a PE or synchronous-import optimization. Do not
prioritize encode parallelism solely because an individual encode scope costs
about 22ms CPU/Present: input starvation remains substantial and stages overlap.
Residual audio waits and exact scene-aligned replication remain open.
