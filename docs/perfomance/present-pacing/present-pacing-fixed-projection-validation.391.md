---
domain: present-pacing
workload: 3DMark05 GT2 Render Tape and STALKER Day
title: "H391: Fresh GPU exact and wild integration for fixed comparison"
type: evidence
status: correctness-qualified-performance-comparison-open
updated: 2026-09-27
source: traces/compat-uniform-h391/audit.py; traces/compat-uniform-h391/gpu-audit.json; traces/compat-uniform-h391/{gpu-compat,gpu-direct}/result.json; traces/compat-uniform-h391/{wild-early,wild-middle}.png; experiments/output/app-d3d9-stkcop-bench-fixed-projection-h391-retry/result.json
related: present-pacing-fixed-projection-integration.390.md
---

# H391: Fresh GPU and application verification

## GPU exact

Use H390's candidate binaries, GT2 r405 original, capture-source boundaries,
identity partitioning, zero headroom and two cold-device iterations per route.
The existing production Render Tape harness performs real GPU execution and
readback. No new gputrace or timing profiler is needed for this pixel oracle.
Toolchain doctor passes: Xcode/xctrace 27, Metal compiler 32023.921, Apple M1.

| Route | Draws per iteration | Result |
|---|---|---|
| Compatibility | 1,866 compatibility, 0 Direct | 2/2 accepted |
| ProductionSlot | 1,856 Direct, 10 compatibility | 2/2 accepted |

All four iterations report zero GPU errors. Eight source/output readback files
are independently rehashed by `audit.py`; every 3,145,728-byte RGBA file equals
the original SHA-256
`d5a01daf5f2fcf66c90e08bf6e9064a5902eda58aa717d321ce4d11ee6701284`.
The two runs share input component fingerprints and provider identity, with
stable loaded images. This closes this candidate's fresh captured-input GPU
exact gate, not arbitrary future state/workload coverage.

## Wild integration

STALKER Day, high, 1280×720, Sikarugir CX 24.0.7, `-max-threads 2`,
`snd_targets 16`, perf profile, append diagnostics OFF. Existing perf counters
remain enabled; no supply/lock diagnostic is requested. These game/audio
controls are experimental settings, not renderer defaults.

The canonical runner rebuilt/staged both PE architectures and the Unix provider.
Its result is **pass**, exit 0, no timeout, no failures, 3,251 submitted Presents,
zero chunk rejects and zero GPU command-buffer errors. The staged Unix hash
prefix `209fa3f6282589f6` matches the full image hash loaded by the GPU oracle.
Observed early/middle screenshots show rendered geometry and textures, without
the previously described black/repeated-pattern corruption. This is sampled
visual observation, not an exact STALKER pixel oracle or continuous inspection.

The first attempt stopped before app launch because the shell lacked the
MinGW compiler PATH; the retry used the documented toolchain PATH. During the
retry the initially unfocused app remained at loading, then advanced after
one window restore/focus operation. The runner's automatic capture occurred
around that loading interval; the two separately preserved scene screenshots
provide actual visual coverage. OpenAutomation `OA not initialized` messages
occur in the application log; they are not relabelled as Metal errors.

## Remaining performance evidence

Do not use the 217.15s process elapsed time as FPS: it includes loading/focus
delay. No order-reversed baseline/candidate performance pair was run here.
H390's baseline receipt exists, but its executable path was rebuilt in place;
the receipt's hash alone is not a preserved runnable baseline. A controlled
performance campaign must first reconstruct/freeze that revision and matching
build options/images, then perform A/A and A/B/B/A with semantic observers OFF.
The earlier ~0.0675ms/input lookup difference remains diagnostic, not accepted
whole-replay or FPS improvement. No README performance value is changed.
