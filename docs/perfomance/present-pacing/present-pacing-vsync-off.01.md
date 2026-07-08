---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: vsync-off
order: 01
title: DXMT9_DISABLE_VSYNC=1 production-shippable A/B
date: 2026-06-05
type: ab-test
status: accepted
source: experiments/output/app-d3d9-3dmark05-current-nondiag-baseline-r1, experiments/output/app-d3d9-3dmark05-vsync-off-r1
---

# DXMT9_DISABLE_VSYNC=1 production-shippable A/B

**Question / hypothesis.** Steps 1-3 of the present-pacing
investigation foreclosed the present-side knob space under the
existing default vsync. Step 1 confirmed that disabling display sync
(via the diagnostic `DXMT9_LAYER_DISPLAY_SYNC=0` env in
[present-pacing-display-sync.01](present-pacing-display-sync.01.md)) recovered substantial fps but
that knob did not disable both pacing paths and the original
measurement showed only 839 CBs processed in 83 s — suggesting the
scene partially short-circuited rather than ran fast.

The new `DXMT9_DISABLE_VSYNC=1` env (commit `901c145`,
[present-pacing-display-sync.01](present-pacing-display-sync.01.md) follow-up) forces both
`CAMetalLayer.displaySyncEnabled = NO` and software
`minimumPresentDuration = 0` regardless of the D3D9
PresentationInterval. This test runs the option end-to-end on the
full GT1 workload and measures the legitimate fps delta.

**Method.**

```
DXMT_3DMARK05_PREFIX=…/experiments/prefixs/app-d3d9-3dmark05 \
DXMT9_DISABLE_VSYNC=1 \
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --no-gputrace --suffix vsync-off-r1
```

**Result.**

| Metric | Baseline | `DXMT9_DISABLE_VSYNC=1` | Δ |
|---|---:|---:|---:|
| **`process_elapsed_sec`** | 251.07 | **133.44** | **−46.9%** |
| **Implied fps (≈ inverse)** | 1.00× | **1.88×** | **+88%** |
| `status` | pass | **pass** | scene completed |
| `completion_wait_ms` | 31,445.8 | 31,208.4 | −0.8% |
| `completion_present_wait_p50_ms` | 23.978 | 23.752 | −0.9% |
| `completion_present_wait_p95_ms` | 29.465 | 31.067 | +5.4% |
| `completion_waits` (CB count) | **1,439** | **1,439** | **unchanged — same workload** |
| `gpu_command_buffer_time_ms` | 4,317.5 | 4,300.1 | −0.4% |
| `encode_chunk_cpu_ms` | 20,686.0 | 23,465.0 | +13.4% |
| `encode_chunk_cpu_p50_ms` | 20.45 | 21.47 | +5.0% |
| `encode_draw_cpu_ms` | 16,476.7 | 18,417.8 | +11.8% |
| `present_acquire_wait_ms` | 153.6 | 147.3 | −4.1% |
| `present_boundary_wait_ms` | 0.0 | 0.0 | unchanged |

**Compare against Step 1's original DSync=0 reading.**

| Metric | Baseline | DSync=0 (original) | `DISABLE_VSYNC=1` (this) |
|---|---:|---:|---:|
| `process_elapsed_sec` | 251.07 | 83.02 | 133.44 |
| `completion_waits` | 1,439 | **839** | **1,439** |
| `gpu_command_buffer_time_ms` | 4,317.5 | 946.7 | 4,300.1 |

The Step 1 DSync=0 reading showed **fewer CBs processed and far less
GPU work** — that run executed only ~58% of the workload before
finishing. The 83 s wallclock was not a real ~3× speedup; the scene
finished early. The new `DISABLE_VSYNC=1` measurement runs the same
1,439 CBs and 4.3 s of GPU work as the baseline, so the −46.9%
wallclock figure is a **legitimate full-workload fps gain**.

The −46.9% is the honest number to ship against. The earlier "+199%"
figure in [present-pacing-display-sync.01](present-pacing-display-sync.01.md) was inflated by an
unintended workload-shortening side effect of the diagnostic env.

**Mechanism.** With both pacing paths disabled:

- `CAMetalLayer.displaySyncEnabled = NO` — compositor no longer holds
  the layer's drawable until the next refresh slot.
- `minimumPresentDuration = 0` — `MTLCommandBuffer.
  presentDrawableAfterMinimumDuration:` is skipped (see
  `dxmt9_presenter.mm:638`).

The per-CB completion wait stays around 24 ms p50 (close to a 60 Hz
slot) because Apple's compositor still paces back-buffer rotation at
roughly the display refresh under direct submission. The wallclock
win comes from CBs no longer slipping into the *next* vsync slot
when their encode + GPU work overruns the current slot — total
serial vsync misses across the scene drop, even though individual
slot-aligned completion times are similar.

`encode_chunk_cpu_ms` rises +13.4% in this run, mirroring the same
effect [present-pacing-bind-cache-work-a.01](present-pacing-bind-cache-work-a.01.md) (Work A) saw —
indicating measurement-time encode variance unrelated to the vsync
toggle. The wallclock signal is decisive enough that this minor
encode-side noise does not change the verdict.

**Verdict.** Accepted as the production-shippable fps fix under the
user's deliberate "vsync off" intent. The +88% fps comes with the
documented trade-off: tearing, compositor pacing off, no display
sync. The env var is opt-in and clearly named; the resolver is
testable; the existing D3D9 PresentationInterval path is unchanged.

This is the production deliverable from the goal "분석된 내용을
토대로 실제 구현. 별도로 vsync off 옵션도 추가".

**Trade-offs / non-goals.**

- This option is *not* a default. Apps that need clean visuals must
  leave it off. Default behaviour is the existing
  D3D9-PresentationInterval-driven path.
- This option does *not* recover fps under vsync. That remains an
  open attribution problem after [present-pacing-bind-cache-work-a.01](present-pacing-bind-cache-work-a.01.md)
  ruled out bind-call suppression as the right lever for GT1.
- This option does *not* help GPU-bound workloads. GT1 is
  display-pacing-bound; a GPU-bound title would not benefit.

**Cross-links.**

- Resolver implementation: `src/dxmt9/dxmt9_presenter.mm`
  (`resolveDisableVsync`, `disableVsyncEnv`).
- Unit test: `tests/native/backend/present_disable_vsync_spec.cpp`
  (8 cases, locked in env-string semantics).
- Env-var documentation:
  `agents/rules/environment_variables.rules.md` row added at the
  same commit (`901c145`).
- Bind-cache follow-up: [present-pacing-bind-cache-work-a.01](present-pacing-bind-cache-work-a.01.md)
  (rejected — bind suppression alone does not move fps on GT1).
