---
domain: present-pacing
workload: STALKER CoP Day high max-threads 2 snd_targets 16
title: "H407: Observer-OFF baseline only partially recovers"
type: evidence
status: baseline-drift-unresolved
updated: 2026-09-28
source: traces/stkcop-baseline-h407/{run.py,analyze.py,invocation.json,completion.json,summary.json}; experiments/output/app-d3d9-stkcop-bench-baseline-h407/{result.json,daybenchmark.result,actual.png}; traces/stkcop-baseline-h407/baseline-h407-r2/{invocation.json,completion.json,summary.json}; experiments/output/app-d3d9-stkcop-bench-baseline-h407-r2/{result.json,daybenchmark.result,actual.png}
related: present-pacing-stalker-cross-renderer-waits.406.md; present-pacing-stalker-covered-pending-validation.403.md
---

# H407: Observer-OFF baseline only partially recovers

After this run, the user reported using the computer during measurement.
Treat the original result below as an interfered run, not a clean baseline
or evidence of a persistent regression. This reports an uncontrolled factor;
it does not quantify its causal contribution.

Run one Day/high 1280x720 windowed benchmark through the catalogue harness,
with Sikarugir CX24.0.7, two game threads and 16 sound targets. Keep actual
Metal occlusion and covered-Pending OFF, matching H403's reference policy.
No external CPU sampling, guest thread suspension or GPU tracing is used;
runtime perf/frame/query observers are OFF. The ordinary harness captures
one window image at the requested 120-second delay.

## Result

| Evidence | Result |
|---|---:|
| Runner | PASS, exit 0, no timeout |
| Engine average | 23.930506 FPS |
| Reconstructed scene duration | 119.637861 s |
| Frames | 2,863 |
| H403 OFF mean | 29.908487 FPS |
| Change from H403 OFF mean | -19.99% |
| Requested/final/control hashes versus H403 | All match |
| Five staged binary size/hash receipts versus H403 | All match |

The captured image shows the scene with buildings, vegetation and characters,
without obvious black output or gross corruption. This single image is not
a pixel oracle, continuous visual verification or proof of foreground focus.
AC power and a fully charged battery are recorded before and after the run;
there is no continuous focus, competing-load or thermal-state record.

Engine-relative 10-second windows include 38.6 FPS at 40–50 s, 14.5 at
80–90 s, 13.2 at 90–100 s and 16.8 at 100–110 s. The final partial window
is 14.42 FPS. These windows reconstruct time from frame durations; they are
not exact camera identities across runs.

## Interpretation and next gate

Removing external diagnostics does not restore the old baseline in this
single run. The scene duration is back near 119 s, unlike H406's 134.80 s
dxmt9 diagnostic, but average FPS remains materially lower. Consequently,
H406's slowdown cannot simply be assigned to profiler overhead, and H407
does not identify a code regression: the measured binary/config identity is
unchanged. Runtime conditions or workload timing remain unqualified.

Do not combine this value with historical D9VK FPS into a new renderer ratio
or use it to select an encode/queue optimization. Before another A/B, establish
a repeatable observer-OFF control with foreground/focus and host-condition
evidence. Then isolate actual visibility semantics as a separate policy axis;
common engine/audio waits alone still do not explain the renderer differential.

No runtime, default, README, audio or Wine scheduling changes. No commit/push.

## Repeat after user-reported interference

Repeat `baseline-h407-r2` uses the same observer-OFF invocation and matches
all H403 staged binary and configuration receipts. It exits successfully:
3,273 frames / 119.427773 s, engine average **27.405689 FPS**. This is about
14.52% above the interfered first run but remains 8.37% below H403's OFF mean.
The 40–50 s window reaches 46.1 FPS; 80–90 / 90–100 / 100–110 s are
14.9 / 12.2 / 15.0 FPS. This is recovery primarily before the late tail,
not evidence that the tail problem disappeared.

The visual/foreground qualification is incomplete: `capture_error` reports
`unable to find onscreen window matching title: S.T.A.L.K.E.R.` and the
harness falls back to `full_screen`. The saved image shows Orca, not the game.
Do not describe this repeat as visually verified or foreground-controlled,
and do not infer the duration or cause of any offscreen interval from this
single observation. The engine result is retained, but is not a replacement
fully qualified baseline. On 2026-09-29 the user explicitly accepted 27.405689
FPS as the working performance baseline so investigation can proceed. This
acceptance does not turn the failed capture into visual/foreground evidence.
