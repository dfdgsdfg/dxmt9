---
domain: present-pacing
workload: STALKER Day high 1280x720 max-threads 2 snd_targets 16
title: "Camera-qualified native CPU: substantial worker demand difference, command workload still open"
type: evidence
status: camera-qualified-single-pair-workload-census-open
updated: 2026-10-07
source: traces/stkcop-backend-cpu-h454/{observe.c,observe.exe,run.py,analyze.py,compare.py,comparison.json,check_completed_witness.py,completed-witness-model.json}; traces/stkcop-backend-cpu-h454/{dxmt9-r1,d9vk-r1}/{receipt.json,cpu-progress.jsonl,identity-join.json,identity.sample.txt,analysis.json,scene.png,tail.png}; experiments/output/app-d3d9-stkcop-bench-backend-cpu-h454-{dxmt9-r1,d9vk-r1}/{result.json,daybenchmark.result,stkcop-requested.ltx,stkcop-after-run.ltx,stkcop-controls.txt}
related: present-pacing-api-residency.453.md; present-pacing-default-tail-cpu.449.md; present-pacing-stalker-cross-renderer-waits.406.md; present-pacing-stalker-visibility-gap.401.md
---

# A large worker CPU difference survives camera qualification

H454 runs unchanged default dxmt9 followed by the archived D9VK release.
Both catalogue runs pass. This is one diagnostic pair, not a repeated
observer-OFF FPS baseline, equal-command replay, GPU exact comparison, or
measurement of removable cycles. No runtime/default/README changes are made.

Unlike H406's buffered-log trigger, a read-only external helper observes the
engine's benchmark-running flag, rendered-frame vector length, frame ordinal,
camera position/direction, readiness, precache and active-window state.
Native libproc supplies thread CPU. There are no renderer entry hooks,
per-draw timers, injected game calls, or thread suspensions in this observer.
A separate one-second stack sample identifies worker threads around scene
second 20; its entire action interval is excluded from comparison windows.

## Scene and image qualification

The installed engine SHA256 is
`96a4b27dd8e01ca119ddf2775e1b5032d19fcd21370203b3133dc1bb47889e7b`.
The helper validates a known code signature and reads module-relative values:

| Value | Engine RVA / layout |
|---|---|
| `Device` | `0x91E40` |
| Precache, ready, active | Device `+0x0C`, `+0x10`, `+0x14`, uint32 |
| Frame ordinal | Device `+0x18`, uint32 |
| Camera position / direction | Device `+0x30` / `+0x3C`, float3 |
| `g_SASH` | `0x92238` |
| Benchmark running | SASH `+0x03`, byte |
| Rendered-frame duration vector | SASH `+0x80/+0x84/+0x88`, PE32 begin/end/capacity |

`CRenderDevice::on_idle` starts SASH only after loading callbacks and precache,
before the next `FrameMove`. SASH appends float32 elapsed seconds in `End`.
It is not the same timer or vector as CDemoPlay's `.result` file. The latter
skips its first partial entry and writes reciprocal durations as FPS.
Do not equate the two vectors element by element.

Snapshots use repeated unchanged struct reads, bounded pointer ordering,
and finite duration validation; this is not an atomic engine frame snapshot.
CPU snapshots follow the progress response, so CPU/frame endpoints are close,
not simultaneous. In dxmt9 r1 the largest helper request bracket is 1.608 ms
and the largest response-to-CPU-end gap is 0.232 ms. All 230 running observations
are active, ready and non-precache; the selected intervals additionally check
these conditions across all recorded rows, not only accepted endpoints.
Unsampled focus changes are not excluded.

The running flag also covers part of shutdown: a final dxmt9 observation has
frame 3814 with unchanged SASH count 3705. Do not use this flag alone to price
the final camera frame. Selected windows stop well before this observed tail.

Camera matching uses seven knots per interval, a predeclared maximum two-metre
position error and three-degree direction error, monotonic correspondence,
and at most 1.1 seconds between bracketing candidate observations. Accepted
knots actually stay within 0.091 m and 0.340 degrees. Interpolation locates
camera correspondence only; CPU is **not** interpolated. All four surrounding
observed endpoint combinations supply the D9VK ranges below. These ranges
are endpoint sensitivity, not statistical confidence intervals.

The requested 102–110-second interval fails the camera gate at one knot and
is excluded. Thresholds are not relaxed to admit it. The benchmark harness's
own +120-second screenshot is outside the explicit action list: its file
timestamp places completion around dxmt9 scene +26.6 seconds, before selected
windows; D9VK is still loading then. Explicit later screenshots are excluded
from accepted comparison intervals.

## Native CPU demand per observed rendered frame

| Camera window | Thread | dxmt9 CPU ms/frame | D9VK CPU ms/frame |
|---|---|---:|---:|
| Approximately 40–55 s | Primary, whole game/Wine/renderer thread | 19.64 | 11.38–11.93 |
| Same | Replay / `dxvk-cs` respectively | 12.63 | 2.76–2.87 |
| Same | Encode / `dxvk-submit` respectively | 13.50 | 2.17–2.21 |
| Same | `dxvk-queue` | not mapped as an equivalent stage | 0.249–0.254 |
| Approximately 80–95 s | Primary, whole game/Wine/renderer thread | 38.15 | 24.52–24.66 |
| Same | Replay / `dxvk-cs` respectively | 21.35 | 4.24–4.27 |
| Same | Encode / `dxvk-submit` respectively | 22.26 | 2.750–2.757 |
| Same | `dxvk-queue` | not mapped as an equivalent stage | 0.349–0.350 |

The paired worker labels are **not equivalent architectural responsibilities**.
D9VK cs includes work divided between dxmt9 Replay and Encode; submit is not
simply an alternate Encode implementation. Unnamed/unresolved threads retain
their separate populations in the artifacts. Their CPU must not be silently
assigned to either renderer or excluded from a claimed renderer-total budget.
Only endpoint-persistent successfully read threads are listed; this is not a
reconciled total-process CPU census.

Observed rendered-frame rates in the camera windows are 44.68 versus
80.74–84.44, and 22.34 versus 32.66–33.17. These are diagnostic SASH interval
rates, not new README scores. The full benchmark-owned diagnostic averages
are 30.513 and 53.563 FPS, with approximately 121-second durations; the stack
observer visibly disturbs the early 20–30-second region. No global FPS
improvement or regression is claimed from this pair.

## Interpretation and next discriminator

The dedicated dxmt9 workers each consume about 21–22 CPU ms/rendered frame in
the qualified late interval. Encode is numerically larger in this pair, but
the difference from Replay is small. Primary's 38.15 ms contains game/Wine
and synchronous renderer work; H453 does not yet split the default Primary
budget reliably. Thus the largest **overall dxmt9-owned** single-thread cost
and removable amount are not established.

The worker difference is large enough to stop treating another small-copy
candidate as the main gap explanation. However, it is not a removable-cost
estimate: equal camera trajectories do not imply equal D3D9 draw/query work.
Current `src/dxmt9/dxmt9_query_policy.hpp` / `metalOcclusionEnabled` still defaults
OFF, so H401's primitive-count versus actual visibility distinction remains
applicable. Earlier whole-scene averages do not price its effect here.

The next discriminator is **camera-matched completed-frame game submission
counts**, then primitive and query work. More submitted work would direct the
investigation toward visibility/semantic workload; comparable counts with
large worker demand would direct it toward translation/materialization cost.
Do not infer either branch from the CPU ratio alone or sum overlapping thread
CPU times into frame critical-path time.

## Read-only completed-frame counter feasibility

The installed R2 DLL SHA256 is
`6e1914e047075409b727bf9fd3087ae6cb2404ac85fb8cfd2e797550ef3cc776`.
Its native statistics expose module-relative uint32 primitives (`0xDF170`),
submitted vertex-count sum (`0xDF174`), and DIP/DP count (`0xDF178`). Inspected
draw paths increment them before the D3D9 call. They represent native counted
submission attempts, not GPU execution, unique vertices, or query work.
Begin clears the block; no separate completed snapshot was found. Ordinary
polling of these fields, or checking only `g_bRendering==0`, is insufficient.

A stronger read-only witness follows the inspected order:

```text
FrameMove: frame++
Begin: reset native counters; rendering = 1
draws: accumulate native counters
End: rendering = 0; append SASH sample
```

With a run-qualified constant `K`, completed counted work can be sampled when
`rendering == 0 && frame - SASH_count == K`. The next Begin/reset is excluded
because FrameMove increments the frame first, making the difference `K+1`.
The additional loading Begin path also increments the frame first. H454's
observed complete/in-progress differences are consistent with K=88/89;
88 must not be hardcoded for future runs.

Future observation must establish K from an observed transition, bracket the
counter reads with unchanged frame/rendering/running/header reads, require
ready/active/non-precache and native SASH mode, and deduplicate by frame/count.
Never silently rebase after a skipped frame. Engine `g_bRendering` is RVA
`0x91CCC`. Require engine `g_dedicated_server` at `0x91C71` to be zero: the
counter reset routine returns early for a dedicated server. Also require the
R2 overdraw/debug guard at `0xCBBA0` to be zero:
that optional End branch submits raw DrawPrimitiveUP after SASH publication
and bypasses these counters.

The small phase checker confirms the composite witness under the modeled
ordering and produces counterexamples for rendering-zero alone. It does not
verify a runtime reader or every draw path in the closed-source game. H454
did not read these counters, so it supplies no retrospective draw census.
Sparse accepted completed-frame samples can be dwell-time biased; report
coverage and camera matching, not an unqualified whole-window average.

The bounded disassembly extracts are archived in
`traces/stkcop-backend-cpu-h454/{engine-active-gate,engine-end-publication,r2-counter-reset,r2-begin,r2-draw-counters}.txt`.

Renderer identity is unchanged baseline x86 D3D9 SHA prefix `534ea6f17128354e`
and Unix provider `7f45561ddf0fd22f`, versus original D9VK x86 release
`22511d1fbb15cdbc` with MoltenVK 1.4.1. D9VK's `--skip-stage` manifest describes
installed dxmt9 files, not the active provider; its DLL, startup banner and
loaded-image sample identify the app-local route. On completion the
hash-validated temporary app-local DLL is moved into the run artifact, leaving
the application directory clear and the shared Wine baseline hashes intact.
