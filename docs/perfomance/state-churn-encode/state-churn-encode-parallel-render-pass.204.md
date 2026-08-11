---
domain: state-churn-encode
workload: 3DMark05 GT2
title: "Parallel Render-Pass Worker Gate"
type: experiment
status: rejected-default
updated: 2026-08-11
source: experiments/output/app-d3d9-3dmark05-parallel-samebuild-identity-gt2-r10-20260811/result.json; experiments/output/app-d3d9-3dmark05-parallel-samebuild-worker-gt2-r11-20260811/result.json; experiments/output/app-d3d9-3dmark05-parallel-worker-gt1-smoke-20260811/result.json; experiments/output/app-d3d9-3dmark05-parallel-worker-gt3-smoke-20260811/result.json; experiments/output/app-d3d9-sfiv-benchmark-parallel-worker-smoke-20260811/result.json
related: specs/backend/encode-scheduling/requirements.md; specs/backend/encode-scheduling/gap.md; docs/perfomance/state-churn-encode/overview.md
---

# Parallel Render-Pass Worker Gate

## Result

The production `MTLParallelRenderCommandEncoder` lane is correct enough to
retain as an explicit provider, but it does not pass the default-promotion
gate. A same-build 60-second GT2 pair measured lower coordinator encode wall
time and real worker overlap, while end-to-end Present throughput regressed
`5.28%`.

| Metric | Identity | Parallel worker | Delta / reading |
|---|---:|---:|---:|
| Presents / 60 s | `1,573` | `1,490` | `-5.28%` |
| command buffers / Present | `3.99936` | `3.99933` | unchanged |
| render passes / Present | `15.76732` | `15.76846` | unchanged |
| tile preservation MiB / Present | `100.247` | `100.248` | unchanged |
| `encode_chunk_cpu_ms` / Present | `19.081` | `15.321` | `-19.71%` wall |
| summed `encode_draw_cpu_ms` / Present | `14.711` | `28.491` | `+93.67%` CPU |
| worker CPU / joined wall per Present | n/a | `23.203 / 3.628 ms` | real overlap |
| worker batches / tasks / peak active | `0 / 0 / 0` | `3,022 / 37,601 / 8` | concurrent |
| GPU command-buffer errors | `0` | `0` | pass |

The parallel lane preserves the command-buffer, pass, and tile-locality shape.
The regression instead comes from extra child-local first-state work, Stage 2
to Stage 1 conversion, worker/cache contention, and parent/child encoder
overhead. The summed draw CPU increase is larger than the coordinator wall-time
saving.

## Correctness Scope

Opt-in wild smokes passed with zero GPU command-buffer errors on GT1, GT2, and
GT3. SFIV also passed, but selected zero parallel passes because its eligible
rendering route remains outside the current portable-child policy. The native
Metal fixture passed with `MTL_DEBUG_LAYER=1`.

## Decision

Keep `DXMT9_RENDER_PARTITION_MODE=parallel` as an explicit production provider
and keep `identity` as the default. Do not promote from the local encode-wall
reduction. Revisit only after profiles justify child-local Stage 2 packets or a
higher eligibility threshold that amortizes child setup and executor overhead.
