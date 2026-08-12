---
domain: state-churn-encode
workload: 3DMark05 GT2
title: "Parallel Render-Pass Worker Gate"
type: experiment
status: rejected-default
updated: 2026-08-12
source: experiments/output/app-d3d9-3dmark05-parallel-stage2b-matched-identity-gt2-r3-20260812/result.json; experiments/output/app-d3d9-3dmark05-parallel-stage2b-matched-parallel-gt2-r3-20260812/result.json; experiments/output/app-d3d9-3dmark05-parallel-economics-post-gate-matched-identity-gt2-r3-20260812/result.json; experiments/output/app-d3d9-3dmark05-parallel-economics-post-gate-matched-parallel-gt2-r3-20260812/result.json
related: specs/backend/encode-scheduling/requirements.md; specs/backend/encode-scheduling/gap.md; docs/perfomance/state-churn-encode/overview.md
---

# Parallel Render-Pass Worker Gate

## Result

The earlier pre-Stage2b GT2 pair and GT1/GT3/SFIV smoke artifacts quoted in
this section are no longer available in the worktree; these figures are
historical measurements that cannot currently be re-checked. The surviving
`source:` artifacts above belong to the attributable Stage2b follow-up and the
first post-gate pair.

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
The regression instead came from extra child-local first-state work, Stage 2
to Stage 1 conversion, worker/cache contention, and parent/child encoder
overhead. The summed draw CPU increase was larger than the coordinator wall-time
saving.

## Correctness Scope

Historical opt-in wild smokes passed with zero GPU command-buffer errors on
GT1, GT2, and GT3. SFIV also passed, but selected zero parallel passes because
its eligible rendering route remains outside the current portable-child policy.
Those smoke artifacts are unavailable, so these older claims cannot currently
be re-checked. The native Metal fixture passed with `MTL_DEBUG_LAYER=1`.

## Decision

Keep `DXMT9_RENDER_PARTITION_MODE=parallel` as an explicit production provider
and keep `identity` as the default. Do not promote from the local encode-wall
reduction. Revisit only after the Stage 2b lane and attributable economics produce
matched evidence that amortizes child setup and executor overhead. The matched
follow-up below did not do so, and therefore strengthens the no-promotion
decision.

## Stage 2b Economics Follow-up

The provider now retains direct-cbuf Stage 2b in child-local binding shadows at
VS/PS slots 0 and FFP slots 3. A complete pre-effect pass proof rejects missing
PSO metadata, slot-30 tables, resource arrays, mixed Stage 1/Stage 2b ABIs, and
PSO-rebuilding draw overrides. The queue argument encoder, mutable table shadow,
and argument-buffer constant cache remain outside child ownership.

The attributable matched GT2 pair measured identity at `21.087975 fps` and
parallel at `19.729740 fps`, a `-6.44%` regression. Command-buffer, render-pass,
and tile-preservation rates were conserved. The encode stage wall improved
`8.9%`, but summed `encode_draw` CPU increased from `14.22` to `31.92
ms/Present`, while parallel workers consumed `26.91 ms/Present`.

All `2,670` selected passes were safe Stage 2b, with zero GPU or binding errors.
They contained `33,244` children and `1,017,361` draws: `12.45` children/pass
and only `30.60` draws/child. The existing pure classifier rejected every pass
as `thin_child`. This isolates child granularity and first-bind amplification
as the relevant economics failure rather than a binding-correctness failure.

The follow-up increment therefore enforces that classifier after complete
locator/ABI/PSO/uniform re-resolution but before render-pass preparation or
parent Metal effects. Rejected passes return to exact serial replay. The
ExplicitParallel sealed-pass builder uses at most `floor(draws / 64)` children
in the existing two-to-16 range, with every child at least 64 draws; serial
production constants and hardware-independent capacity remain unchanged. Perf
counters report exact accepted versus
`serial_fallback` production outcomes with considered/reason conservation.

This is a measured safety/economics gate, not a speedup result. Identity remains
the default, parallel remains opt-in, and fresh post-gate matched wild evidence
is required before any promotion claim.

## First Post-gate Pair and Planner Correction

The first post-gate matched artifacts measured identity at `24.188999176 fps`
and parallel at `24.190946579 fps`, with `1,515` Presents and zero GPU errors
in each lane. That parity is not provider economics evidence: the parallel run
observed `23,888` sealed candidates but selected zero passes, recorded zero
economics considerations, and launched zero worker tasks. Inspection localized
the zero-selection result to the multi-command child planner, which stopped the
final group after one command and could skip earlier valid jagged cuts.

The next attributable implementation increment replaces that search with a
deterministic fixed-capacity earliest-prefix grouping that preserves whole
commands, absorbs a final sub-64 suffix, validates exact ordered coverage, and
separates no-two-child work from planner invariant and storage-capacity
rejections. Its economics gate additionally rejects child draw imbalance over
the existing 64-draw quantum and requires both PSO and uniform identity changes
at every actual child boundary; internal A-B-A churn no longer pays for a
child first-bind reset. New counters expose minimum, maximum, and imbalance
draw totals plus child-boundary transitions.

No wild application was run for this correction, so it makes no speed or
promotion claim. A meaningful next GT2 pair requires nonzero economics
considered and accepted counts, complete counter conservation, zero GPU errors,
and conserved locality before performance can be assessed. Identity remains
the default and `parallel` remains opt-in.
