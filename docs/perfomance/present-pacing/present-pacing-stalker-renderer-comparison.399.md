---
domain: present-pacing
workload: STALKER CoP Day high max-threads 2 snd_targets 16
title: "H399: Observer-OFF dxmt9 versus D9VK/MoltenVK 1.4.1"
type: evidence
status: repeated-config-matched-performance-gap-owner-open
updated: 2026-09-28
source: traces/stkcop-compare-h399/{run.py,sequence.py,analyze.py,summary.json}; experiments/output/app-d3d9-stkcop-bench-compare-h399-{dxmt9-r1,dxmt9-r2,d9vk-r1,d9vk-r2retry,d9vk-r2}/result.json
related: present-pacing-ingress-wild.398.md; present-pacing-stalker-cpu-recheck.392.md
---

# H399: The residual gap is real under matched settings

HEAD `dbc216d5`, runtime promotion `694427f3`. Same Apple M1, Sikarugir
CX 24.0.7 Wine root and prefix, Day/high `renderer_r2.5`, 1280x720 window,
`-max-threads 2`, `snd_targets 16`. Requested config, after-run config and
control receipt hashes match across all successful runs. This is a controlled
configuration comparison, not identical command streams or a cross-backend
pixel oracle.

Clear inherited renderer/Metal/DYLD settings. Both routes explicitly disable
perf counters and frame sampling; no `[dxmt9-perf` records are emitted.
Use benchmark-owned `daybenchmark.result`, checked against the reconstructed
sum of reciprocal per-frame FPS. No build or second benchmark is scheduled
alongside a run. Computer Use restores the window during startup; sampled
scene screenshots establish visible progress, not continuous absence of user
interference. Metal skill environment checks pass with Xcode/xctrace 27,
Metal compiler 32023.921, M1/Metal 4. No GPU trace or validation layer is used.

## Renderer identity and staging

- dxmt9 uses the current builtin PE/Unix images. Staged build identities are
  recorded in each runner receipt; loaded-image evidence for the warm-up is
  in `traces/stkcop-compare-h399/dxmt9-warm/loaded-images.txt`.
- D9VK is the original Sikarugir macOS async release
  `v1.10.3-20250511`, x86 DLL SHA-256
  `22511d1fbb15cdbc5365dfcb231f028af427533bfb6957505720dac0aca98e3e`.
  It is placed next to `xrEngine.exe` only for that run and removed in cleanup.
  `--skip-stage` preserves the same Wine root. Consequently its runner's
  `staged_build` still lists installed dxmt9 files: those are **not** proof of
  the active D3D9 provider. App-local loaded-image evidence and the DXVK startup
  banner establish the override.
- MoltenVK is the existing vendor library, version 1.4.1 in the runtime log,
  SHA-256 `3e365bfdfb4e9292d9cd1d896330c6d639721015d0443c6391ef86c104c17568`.
  `loaded-images.txt` in the D9VK run directories identifies that actual path.
  Both routes use the same vendor library search directory. Wine/app probing
  can also load MoltenVK during dxmt9 startup; that alone does not identify the
  active renderer.
- D9VK uses its own persistent state-cache directory, HUD disabled and startup
  info logging. Both implementations retain normal caches. No async policy
  override is added to the released D9VK build.

## Order, failure retention and results

Warm up D9VK and dxmt9 once; exclude both. Successful scored order is
dxmt9 A1, D9VK B1, D9VK B2-retry, dxmt9 A2. An intervening B2 run times out
after prolonged preparation (return code 143); retain it as failed evidence,
not an FPS sample. Retry only extends the runner timeout from 480 to 900
seconds, with the unchanged 120-second automatic screenshot delay. All four
accepted runs exit normally with `status=pass` and no timeout.

| Route | First FPS | Second FPS | Mean FPS | Process elapsed seconds |
|---|---:|---:|---:|---:|
| dxmt9 / Metal | 29.575857 | 29.508417 | **29.542137** | 212.24 / 212.24 |
| D9VK / MoltenVK 1.4.1 | 53.641953 | 54.193829 | **53.917891** | 507.89 / 657.91 |

D9VK is approximately **1.825x** the scene throughput; dxmt9 is **45.2% lower**.
The two dxmt9 scores differ by 0.23%; the D9VK scores differ by 1.03%.
Two repetitions do not establish a confidence interval, but the gap is much
larger than this observed repeat spread and survives the reversed final run.

Scene durations reconstructed from game results are about 119 seconds.
Process elapsed time includes preparation and shutdown, not just that scene.
D9VK preparation is substantially longer here, even with state-cache reuse.
A one-second sample in its excluded first warm-up contains MoltenVK pipeline
and shader generation; it does not quantify all later startup delays. Do not
describe the scene throughput result as faster total launch-to-exit time.

## Where the scene gap appears

Average of each route's two per-run reconstructed window FPS:

| Benchmark-relative window | dxmt9 FPS | D9VK FPS |
|---|---:|---:|
| 0–30 seconds | 36.10 | 57.87 |
| 30–60 seconds | 39.17 | 77.67 |
| 60–90 seconds | 26.22 | 42.73 |
| 90 seconds–end | 16.15 | 36.75 |
| Last full 10 seconds | 17.10 | 36.25 |

The last row overlaps the previous row. Windows count frame completions using
the existing `summarize_stkcop_fps.py` convention; they are neither source
identities nor proof of exact camera/input alignment. In particular, each
renderer produces a different number of game frames. Nevertheless the gap is
not confined to the final building view. The stable 40–50-second window is
about 47 FPS in A1 versus 91 FPS in both B runs, making mid-scene throughput a
useful next diagnostic target before chasing tail-only stalls.

## Interpretation and next gate

The old README D9VK 30.4 FPS reference is not a current matched baseline.
H398's 27.84 FPS also used counters/frame sampling. This run does not isolate
why the historical D9VK result differs, or calibrate H398 observer cost.
README scores remain unchanged because this series uses the explicit game
thread/audio tuning and only tests Day.

The result excludes neither extra CPU work nor shader/pass/GPU differences.
Counters are OFF, so zero GPU errors cannot be inferred from absent counters.
Visible D9VK scene evidence is retained in `d9vk-r1/scene.png`; both dxmt9
scored automatic screenshots contain scene content. Some automatic D9VK
captures and the retry's `observed.png` are loading screens, not visual gates.
No cross-backend exact image comparison has been performed.

Next measure native app/worker CPU and supply/wait ownership in the stable
mid-scene window on separate diagnostic runs. Compare game/Wine/provider CPU
and wait populations across renderers, and the dxmt9 PE/import/Replay/Encode
critical path. Do not label the gap a PE, Replay, GPU or parallel-encode issue
without that ownership evidence. No runtime optimization is implemented here.

After measurement the temporary D9VK DLL is removed and the final dxmt9 run
restages current binaries. All successful and failed results remain on disk.
