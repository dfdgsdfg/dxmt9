---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 37
title: Synchronous Admission Phase Split Prices Worker Transfer Below One Percent
date: 2026-08-25
type: experiment-run
status: accepted-mechanism
source: experiments/output/app-d3d9-3dmark05-current-cap-gt2-r1-20260825; experiments/output/app-d3d9-3dmark05-current-admission-phase-split-gt2-r1-20260825
related: docs/perfomance/present-pacing/present-pacing-current-bottleneck-pe-symbol.236.md; specs/backend/requirements.md
---

# Synchronous Admission Phase Split Prices Worker Transfer Below One Percent

## Question

How much current GT2 producer time can move from the synchronous half of
`dxmt9c_device_commit_chunk` to the existing replay worker without changing
Unlock/upload semantics or weakening resource-lifetime admission?

## Method

The current-cap OFF artifact at `e32da591` supplies the uninstrumented control.
A same-runtime-code ON run enabled only:

```sh
DXMT9_PERF_COMMIT_CHUNK_PHASE_SPLIT=1
```

Both runs used the `perf` profile, GT2, frame sampling, no gputrace, no encoder
breakdown, and the production identity partition provider. The ON run did not
enable queue-mutex attribution. It completed `29,278` commit calls and `1,850`
encoded Presents with zero command-chunk rejects, GPU command-buffer errors,
or offload push-backpressure wait.

The phase observer adds five clock pairs per commit. Its parent result is
`0.958ms/Present` versus `1.011ms/Present` in the OFF run, while sampled FPS is
`28.438` versus `28.311` (`+0.45%`). The observer therefore does not show a
measurable wall perturbation in this pair; the FPS delta is noise, not an
optimization result.

## Result

| synchronous phase | total | ms / Present | us / commit | parent share |
|---|---:|---:|---:|---:|
| prepare: unix-owned copy, validation, resolve, retain | `618.283ms` | `0.3342` | `21.118` | `34.9%` |
| import prevalidated view | `2.195ms` | `0.0012` | `0.075` | `0.1%` |
| mark and backing capture | `855.648ms` | `0.4625` | `29.225` | `48.3%` |
| enqueue | `49.760ms` | `0.0269` | `1.700` | `2.8%` |
| present-ordinal wait | `193.291ms` | `0.1045` | `6.602` | `10.9%` |
| parent-minus-named residual | `53.239ms` | `0.0288` | `1.818` | `3.0%` |
| **synchronous parent** | **`1,772.416ms`** | **`0.9581`** | **`60.538`** | **`100%`** |

The mark/capture phase further separates as follows. `mark_lock` is a subset
of `mark_core`, so these rows do not sum independently.

| mark/capture child | ms / Present | us / commit | interpretation |
|---|---:|---:|---|
| canonical identity / ledger dedup | `0.1031` | `6.514` | must remain synchronous unless admission publishes an equivalent typed access summary |
| core exact mark plus backing capture | `0.3300` | `20.852` | mixes deferrable exact marking with mandatory backing capture |
| queue-ticket lock wait | `0.1471` | `9.295` | subset of core; disappears from the producer only when exact marking is deferred safely |
| snapshot sort | `0.0213` | `1.347` | mechanically movable after immutable capture |

The run processes `15.826` commit calls, `4,408.8` handles, and `2,610.0`
buffer handles per Present. This is high-frequency admission work rather than
one large per-frame call.

## Verdict

The phase split narrows the worker-transfer opportunity below the earlier
coarse estimate. Prepare (`0.334ms/Present`) is mostly the immutable admission
boundary: wire ownership, range validation, handle-generation resolution,
wrapper retention, and storage needed before the application can mutate or
destroy its next state. Present wait and enqueue are also not removable by
moving semantic replay.

Under the existing `R-BACK-2.51` Direct-admission contract, the plausible
synchronous reduction is the deferred exact-mark ticket/loop plus snapshot
sort, while canonical identities and backing capture remain synchronous. The
observed target is approximately `0.17-0.30ms/Present`. Against GT2's roughly
`35.2ms` average frame, that prices the likely throughput effect at about
`+0.5%` to `+0.9%`. Moving the entire `0.463ms/Present` mark/capture parent is
an invalid semantic assumption and only a `~+1.3%` mathematical ceiling.

Do not implement a replay-worker transfer as the next FPS lever from this
evidence alone. A larger result requires redesigning prepare ownership — for
example a unix-owned shared admission slab — or eliminating replay
snapshot/materialization. Either is a separate contract and must preserve
the same handle, backing-generation, ledger, ordered-control, and fail-stop
obligations.
