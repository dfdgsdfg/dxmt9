---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: pe-recorder
order: 192
title: PE Recording Cost Is Real (~10ms/present, Overhead-Corrected)
date: 2026-07-07
type: no-gputrace
status: accepted-pe-cost-verified
source: experiments/output/app-d3d9-3dmark05-pe-cost-stats-r7-20260707/result.json; experiments/output/app-d3d9-3dmark05-pe-cost-nostats-r8-20260707/result.json; docs/perfomance/present-pacing/present-pacing-offload-backpressure-attribution.191.md
related: docs/perfomance/present-pacing/index.md; docs/perfomance/present-pacing/present-pacing-pe-hotsetter-split.61.md; docs/perfomance/present-pacing/present-pacing-pe-const-apply-split.60.md
---

# Present-Pacing H192 - PE recording cost verification

## Question

H191 moved the average-FPS frontier to the producer: after the commit-replay
offload, dxmt9's unix-side app-thread cost is `1.1 ms` of a `~60 ms` frame.
Is the PE-side d3d9.dll recording cost real and large enough to attack, or is
the residual wall the game's own CPU? Prior PE numbers existed only under
`DXMT9_PE_RECORDER_STATS`, whose perturbation was unquantified.

## Run

Back-to-back offload-on pair: `--pe-recorder-stats` (r7) vs without (r8).

## Verdict

PE recording cost is real: **`~10 ± 3 ms/present`, 15-20% of the frame** —
but the stats mode's absolute numbers must not be read directly.

- **Perturbation is severe**: r7 `1320` presents vs r8 `1999` — stats mode
  costs `34%` of throughput (`+30.9 ms/present`). With `~20,400` timed-scope
  calls/present (sum of all `*Calls` counters), that is `~1.5 µs` per timed
  scope (QPC through wow64/Rosetta), so raw `*CpuMs` values include nested
  timer overhead.
- **Overhead-corrected structure** (measured − nested-timer cost):
  `constFlush` `6.279 -> ~4.3 ms` true (fully decomposes into
  `vsConstFFlush 5.090` + `psConstFFlush 1.189`, both inflated);
  record-append body `8.342 -> ~5.1 ms`; setter shells `~2.8 ms`
  (`vsConstFSetterCalls = 4205.8/present` — the app sets VS constants
  one-by-one; `psConstF 1328.6`, hot setters `~2800`). Total PE
  `~10 ± 3 ms/present`.
- **Volumes are exact** (counts don't perturb): `1385.9` record appends,
  `644.9` const flushes, `4205.8` VS-const setter calls per present.
- Frame accounting at r8 (`60.0 ms`): game + Wine thunking own the remaining
  `~45-48 ms` (75-80%) — consistent with the H68-H72 between-call gap
  attribution.

## Next owner

The const chain (`~5-7 ms/present`: flush `~4.3` + VS setter shell `~1.0` +
const-record payload copies inside append) is the largest reducible PE item;
`4206` one-register-at-a-time `SetVertexShaderConstantF` calls collapsing
into `645` flush records is the shape to attack (shadow-diff or lazier
flush). Proof discipline: any candidate is judged by paired-scout presents
deltas (offload-on), not by stats-mode numbers; if finer attribution is
needed first, build a decimated (every-Nth-event) stats mode rather than
trusting the full-instrumentation values. Ceiling honesty: removing half the
PE cost is `~+8-10%` FPS; beyond that the wall is the game's own
(Rosetta-translated) CPU on this SKU.
