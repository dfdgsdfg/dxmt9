---
domain: present-pacing
workload: STALKER CoP Normal replay diagnostic
title: "Original Normal source on x86: cold prefetch versus DrawRun CPU"
type: evidence
status: bounded-source-attribution-not-removable-cost
updated: 2026-10-11
source: traces/stkcop-normal-cost-h486/{source_probe.cpp,prefetch_probe.cpp,analyze.py,binary-sha256.txt,commands.md,off-r1.txt,off-r2.txt,trace-r1.txt,trace-r2.txt,command-r1.txt,prefetch-r1.txt,prefetch-r1.stderr,draw-r1.txt,images-r1.txt,images-r2.txt,images-r2.stderr}; traces/stkcop-normal-cost-h486/trace-r1/analysis-v2.json; traces/stkcop-normal-cost-h486/{trace-r2,command-r1,prefetch-r1,draw-r1}/analysis.json; traces/stkcop-normal-finish-h481/tape/frame-10699344711800-1/manifest.json
related: present-pacing-normal-provider.485.md; present-pacing-root-game.477.md; ../../../specs/experiments/gap.md
---

# Original Normal source CPU decomposition on x86

The unchanged H485 provider reproduces H481's original source/output on an
x86_64 release binary under Rosetta. The large original source spends roughly
34 ms of Encode-thread CPU in cold PSO prefetch and 19–21 ms in command
execution in the coarse traces. The former includes fresh-cache builds and is
not a steady-state tail cost. Within execution, DrawRun dominates; FrameGraph
planning is about 1.1 ms. This selects the next investigation surface, not a
globally largest default owner or a correctness-qualified removal budget.

## Input, binary and oracle

Product revision is `a20c1a12`. The private runner authenticates the unchanged
H481 manifest, components and every blob before device creation. It selects
ProductionSlot, serial partitioning, the original `PePresentReservationV1`
producer contract and a fresh device. Shader archives and prewarm are disabled.
There is no Wine staging, PE execution, new capture or product-code change.

Nine separate processes complete: `off-r1`, `trace-r1`, `trace-r2`, `off-r2`,
`command-r1`, `prefetch-r1`, `images-r1`, `draw-r1`, `images-r2`. All restore 16
required surfaces, activate 12 retained aliases, create/release 779 wrappers,
preserve Normal policy and map the same three original source cuts. Every
source/output readback equals the original 3,686,400 bytes, SHA-256
`f99e419861d3c4127e96d8ab56dca62d56421919a5962084ae4ecd95ee42ce5a`.
The bytes are BGRX (format 22), without conversion, masking or tolerance.
`content_authority=unknown` and `timing_qualified=0` remain explicit.

The runner and native library are x86_64; retained hashes still match after
execution. `images-r2` records the actual child PID 95428 loading both
`build-x86_64-builtin/src/libdxmt9_native.dylib` and its
`src/winemetal/unix/winemetal_dxmt9.so`. `images-r1` completed the oracle but
emitted no dyld image rows: setting the variable above the launcher was not
image provenance. The second attempt sets it at child exec. Neither image run
is a timing reference. Environment doctor: Apple M1, Xcode 27 / Metal 4.

## Source-qualified accounting

Captured source indices 0/1/2 map to local queue sequences 3/4/5. Source 1
contains 556 commands; its siblings are one-command control and Present cuts.
Setup sequences 1/2 are excluded by the actual V4 mapping, not by elapsed-time
guessing. The trace's Present 1/4 markers open/close a diagnostic window; they
are not engine frames. No per-Present or FPS normalization is valid here.

The reducer requires unique queue ownership, complete lifecycle endpoints and
same-thread clocks. FrameGraph joins use the backend identity carried by
`encode_backend_begin`, not the queue pointer. The first `trace-r1/analysis.json`
used the wrong planning join and has an empty planning list; its corrected
`analysis-v2.json` is authoritative and the original file is retained.
All five CPU traces are closed, with no overflow or incomplete writers and
native CPU-clock support. Numbers below are observer-inclusive native thread
CPU, not GPU time or wall-time sums across threads.

| Large original source, CPU ms | trace-r1 | trace-r2 |
|---|---:|---:|
| Whole Encode | 54.531 | 57.181 |
| PSO prefetch | 34.002 | 34.430 |
| Backend | 20.500 | 22.715 |
| FrameGraph planning/setup, inside backend | 1.146 | 1.113 |
| Command body, inside backend | 18.704 | 20.876 |

Detailed `trace-r2` planning milestones total 1.080 ms: graph 0.458,
replay-selection 0.586 and optimizer 0.034 ms. These are nested in the
planning/setup scope, not additive to it.

### Cold prefetch is not a removable steady-state budget

The separate counter-enabled `prefetch-r1` reports 73 draw pipeline cache
misses/builds, 544 draw cache hits and one Present pipeline build. Its native
source prefetch CPU is 33.753 ms. The aggregate counter named
`encode_slot_pso_prefetch_cpu_ms` is actually **elapsed time**, 33.845 ms;
its nested draw lookup is 31.904 ms, key resolution 0.573 ms, depth lookup
0.483 ms and state copying 0.019 ms. These elapsed counters must not be
subtracted from source thread CPU.

`Cache::getOrBuildDrawPipelineHandle` in
`src/dxmt9/dxmt9_pipeline_cache.cpp` includes miss-side source generation,
library/PSO submission and future management. Some compilation executes on
workers. Thus 31.904 ms is neither exclusively key construction nor exclusively
Metal compiler CPU. Fresh-device builds dominate this diagnostic's lookup
path; warming or reusing a V4 device requires separate state/content/identity
qualification. The counter companion records zero GPU CB errors; counter-OFF
runs cannot provide a measured error counter merely because the oracle passes.

### DrawRun is the execution-body owner in this input

`command-r1` adds command-level endpoints without per-draw endpoints:

| Command body partition | Calls | CPU ms |
|---|---:|---:|
| DrawRun | 551 | 18.827 |
| Non-draw | 5 | 0.019 |
| Split policy | 556 | 0.314 |
| Dispatch residual | — | 0.902 |
| Total body | — | 20.062 |

`draw-r1` adds per-draw endpoints and all five existing DrawRun milestones.
All 551 DrawRuns classify, with 3,497 nested `encodeDraw` calls. Its DrawRun
CPU partitions into 14.927 ms encodeDraw, 6.620 ms loop residual, 3.322 ms pass
entry, 0.682 ms uniform setup, 0.412 ms flush-blit/hazard setup, 0.348 ms UP
setup and 0.313 ms binding-plan setup. Rounding aside these sum to 26.624 ms.

The larger 26.624 versus 18.827 ms scope is a warning against pricing the
fine-grained scopes as pure product cost. The runs are not an accepted
OFF/ON overhead transfer pair; do not subtract their difference as calibrated
instrumentation cost. Likewise the OFF replay elapsed times (90.608 and
62.589 ms) are not a warm-repeat distribution or observer-overhead estimate.

## Decision and next gate

Keep the runtime unchanged. Investigate the DrawRun loop and encodeDraw body
before another FrameGraph micro-optimization. In
`src/dxmt9/dxmt9_draw_encoder_chunk.mm`, the loop residual includes per-draw
uniform selection, binding override/snapshot setup, binding identity/dirty
transition and post-draw shadow updates; it is not synonymous with allocation
or removable duplication. Pass entry separately includes hazard decisions and
encoder transitions. Existing zero-initialized scratch values are only code
candidates until release-code inspection proves actual work remains.

Next evidence must distinguish one real repeated operation from required
semantic processing, remove it under the same source/oracle, and compare
coarse observer scopes or a qualified OFF measurement. Do not add more
per-draw clocks merely to explain the clocks already present. Native Replay
CPU remains unseparated from the runner's replay elapsed scope. Read-first
content authority, default-tail representativeness, Primary/Wine/PE attribution
and the unnamed large native thread from H477 remain open. No full Wine
parity, whole-tail bottleneck rank, FPS gain or promotion follows from H486.
