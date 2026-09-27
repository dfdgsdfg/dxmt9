---
domain: present-pacing
workload: STALKER CoP Day high max-threads 2 snd_targets 16
title: "H398: Capture-only ingress post-promotion wild run"
type: evidence
status: single-run-operational-pass
updated: 2026-09-28
source: traces/ingress-h398/{run.py,analyze.py,summary.json,middle.png,late.png}; experiments/output/app-d3d9-stkcop-bench-ingress-h398-baseline/result.json
related: present-pacing-ingress-promotion.397.md; present-pacing-stalker-cpu-recheck.392.md
---

# H398: STALKER Day completes after default promotion

Commit `694427f3` promotes capture-only ingress and removes its OFF selector.
Rebuild all three staged builtin directories before running the catalogue
launcher. The staged hashes match the current binaries, including Unix image
`2d518bab652ba941` (SHA-256 prefix), also used by H397's GPU oracle.

Sikarugir CX 24.0.7, Day/high, 1280x720, `-max-threads 2`, `snd_targets 16`.
Inherited DXMT/MTL settings are cleared. Perf counters and frame sampling stay
enabled, as in H392; supply/append diagnostics are off. No parallel benchmark
or build was launched. Computer Use restores focus during loading and samples
the middle and late building scene. These screenshots show no obvious black
or repeated-geometry corruption, but are not a continuous visual proof.
The runner's earlier automatic screenshot captured loading rather than scene
content; use the retained middle/late images for sampled visual evidence.

The run passes, exits normally, and reports zero GPU command-buffer errors.
The engine reports average **27.84**, minimum **8.49**, maximum **55.92 FPS**.
There are 3,390 sampled Presents including non-scene frames.

| Selected frame window | H392 historical baseline FPS | H398 FPS | H398 draws/frame | H398 passes/frame |
|---|---:|---:|---:|---:|
| Early 300–599 | 32.43 | 32.56 | 2,888 | 18.00 |
| Middle 1500–1799 | 47.81 | 47.65 | 1,394 | 28.22 |
| Late 3000–3199 | 15.35 | 16.70 | 4,360 | 79.81 |

Window FPS is frame count divided by summed wall time. Every selected frame
has more than 100 draws. This is not matched-input A/B: frame ordinals differ
in scene placement, with late draws/frame 4,202 versus 4,360. Early/middle
behavior is similar and the tail still slows substantially; do not attribute
the higher late window FPS to ingress alone or claim a calibrated speedup.
README reference scores are unchanged.

This supplies one asynchronous STALKER operational run after H397's synchronous
GPU oracle. Arbitrary schedules, all-family/Reset composition and repeatable
performance attribution remain open.
