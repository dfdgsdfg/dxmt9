---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 194
title: Consolidation Long Confirm - Offload+IndexCache +10% Over The Full Demo
date: 2026-07-07
type: no-gputrace
status: accepted-long-confirm
source: experiments/output/app-d3d9-3dmark05-consol-baseline-long-20260707/result.json; experiments/output/app-d3d9-3dmark05-consol-candidate-long-r3-20260707/result.json; experiments/output/app-d3d9-3dmark05-consol-candidate-long-20260707/result.json; docs/perfomance/index-cache-locality/index-cache-locality-offload-synergy.19.md
related: docs/perfomance/present-pacing/index.md; specs/backend/requirements.md
---

# Present-Pacing H194 - Consolidation long confirm

## Question

The offload (+11-12%) and index-cache synergy results came from 120 s
timeout windows. Do they hold over the full GT1 demo, with a time-aligned
visual gate?

## Runs

`--timeout 150` pairs (the demo ends naturally at ~110 s; both runs
timeout-finalize at the documented final-frame hang, so `present_encoded`
is the full-demo total). Candidate = `DXMT9_OFFLOAD_COMMIT_REPLAY=1` +
`--optimize-opaque-depth-index-cache`. Two invalid attempts are kept as
method lessons: the first candidate ran concurrently with a code-building
subagent (CPU contention collapsed it to 1080 presents — never co-schedule
CPU-heavy agents with FPS-evidence runs) and the second hit the known
startup flake (`presents=0`, third occurrence; retry cleared it).

## Verdict

Accepted: **`1800 -> 1980` presents (`+10.0%`) over the full demo**,
`gpu_command_buffer_errors=0`, `reordered_index_cache_hits=332,785`.

Visual gate (time-aligned by construction — both `actual.png` captures fire
at the same 45 s wall delay, landing at demo time `t≈0:34.1` in both runs):
same scene, same effect classes (volumetric shafts, spotlights, floor
reflections, robot headlights), frame counters `653` vs `708` matching the
fps ratio exactly. The luma delta (`47.5` vs `32.3`) is animation phase,
not a defect class — confirming H190's lesson that frame-index comparisons
across different-fps runs are invalid while equal-wall-time comparisons are
sound.

## Promotion state

With this leaf: long-window FPS confirm done; hardening done
(`c02af448`: testable `PresentOrdinalGate` + cv regression tests,
`R-BACK-2.51` offload contract row, design-doc mechanism gate restated to
`offload_commit_app_cpu_ms`). Remaining before default-flip: the paired
offload+opt-in `.gputrace` proof through
`--require-opaque-depth-index-cache-proof` (requires a manual Xcode
encoder-counter export; the freshest existing frame60 baseline joined CSV
is `traces/app-d3d9-3dmark05-capture-layer-current-r3/analysis/` — June
HEAD, so the comparison carries a code-drift caveat) and the default-flip
decision itself.
