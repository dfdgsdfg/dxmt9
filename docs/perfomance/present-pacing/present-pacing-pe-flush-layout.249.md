---
domain: present-pacing
workload: 3DMark05 GT2
title: "PE constant-flush gating and prepared section-count reuse"
type: evidence
status: native-codegen-and-single-run
updated: 2026-09-07
source: experiments/output/pe-flush-layout-20260907/analysis; experiments/output/app-d3d9-3dmark05-pe-flush-layout-66cbcbab-gt2-r1-20260907; experiments/output/app-d3d9-3dmark05-pe-hot-specialization-gt2-r2-20260906
related: specs/d3d9/recorder/requirements.md; present-pacing-pe-identity-distribution.248.md
---

# PE constant-flush gating and prepared count reuse

## Scope and result

Base: `da1a4192`. R-CORE-REC-7.42–7.44 remove local repeated work; they do
not alter transport, chunk cadence, resource lifetime or queue policy.

- `D3D9DeviceImpl::flushConstShadow` in
  `src/d3d9/d3d9_pe_device_recorder.cpp` gates diagnostic dirty counting and
  shader hash lookup. Counting stays before append settlement; range reporting
  stays after successful append.
- `flushPendingConsts` uses the native-tested `dispatchPeConstFlushes` in
  `src/d3d9/d3d9_pe_const_shadow.hpp`. Clean families bypass the outlined call
  when decimation is off. Enabled decimation retains the six-call population,
  including empty calls. Each dirty check happens after preceding appends,
  preserving callback changes, first-failure order and exception propagation.
- `PeSemanticBatchOwner` in `src/d3d9/d3d9_pe_semantic_owner.hpp` reuses its
  existing sparse-layout quotas for four admission binding counts and seven
  canonical-emission mask counts. Source/callback, section shape, bounds,
  retention and rollback checks remain. No additional count cache is added.

## Evidence

All four canonical builds pass. Native PE/audit tests pass 29/29 and x86_64
focused tests pass 8/8. The unchanged TLA suite passes. Separate fresh-process
constant observer checks at N=1 and N=127 pass.

The new constant-flush test compares the actual production dispatch against
the old unconditional sequence over 4,032 traces: 64 dirty masks, seven failure
choices, three callback mutations and N=0/1/127. Each trace checks failure and
retry, accepted payload order, residual values/dirty state, and event/sample
populations. A separate throwing callback pins exception propagation. These
are fake-append local refinements, not native execution of the private PE COM
device or GPU pixels. Source contracts and linked branch inspection bind the
diagnostic gates and public dispatch call to the tested policy.

Compact tests compare canonical bytes for empty/sparse/full masks and a real
sparse/full/sparse A/B/A trace. Failed settlement releases speculative pins
before a successful retry of the same plan. Pin balance is checked after owner
destruction, not after emission while pins must still be held. A constrained
owner isolates sparse-capacity denial before callbacks or pin acquisition.

| Linked artifact | Baseline bytes | Candidate bytes | Compact writer static instructions, before → after | Software popcount expansions |
|---|---:|---:|---:|---:|
| PE32 | 1,869,824 | 1,870,336 | 514 → 394 | 6 → 0 |
| PE64 | 1,734,656 | 1,734,656 | 470 → 379 | 6 → 0 |

Counts include padding and cold branches, not executed instruction counts.
The seven source expressions include a cheaply lowered shader-mask count;
only six had the full software-popcount multiply sequence. The dispatch
template folds into the real caller's ordered checks/direct calls; it adds
no runtime family-switch layer. Guarded static initialization remains cold.
Exports and archive vtable/RTTI ownership match for both architectures. The
Unix provider and both bridge DLLs remain byte-identical to baseline.

Hash-qualified receipts, linked bodies and logs are in the analysis directory:
`baseline-linked.json`, `verified-linked.json`, `*-linked.asm`,
`build-{native,unix,pe32,pe64}.log`, `native-tests.log`, `unix-tests.log`,
`observer-n{1,127}.log`, and `tla.log`. Final PE32/PE64 SHA-256 values are
`4af665c4dce352e0cc9313a705a5a11ba6e3c04e5eef1bca6e3eb88a6d17958f`
and `80beffb53579b446552ad5628d6dcc4844724a7b513140b8e0fea71e0e75c115`.

## Post-commit GT2 spot check

Commit `66cbcbab` was rebuilt in all three canonical staging directories and
run once through `run_experiment.py`. No concurrent Wine or build process was
present at launch. The current artifact is
`app-d3d9-3dmark05-pe-flush-layout-66cbcbab-gt2-r1-20260907`; the prior H248
reference is `app-d3d9-3dmark05-pe-hot-specialization-gt2-r2-20260906`.

Both use named GT2, 1024x768, Sikarugir `sikarugir-cx-24.0.7`, perf/Warn,
validation OFF, frame sampling ON, and PE decimation/recorder stats/phase split
OFF, without encoder breakdown or GPU trace. The benchmark result filename
is auto-generated in the current run and explicit in the reference.

| Metric | Prior H248 | `66cbcbab` |
|---|---:|---:|
| Positive-wall sampled FPS | 29.048 | 28.835 |
| Positive-wall sample count | 1,875 | 1,863 |
| Sampled wall time (ms) | 64,547.602 | 64,608.102 |
| Encoded Presents | 1,876 | 1,864 |
| Command buffers / Present | 3.99947 | 3.99946 |
| Render passes / Present | 15.77132 | 15.77361 |
| Draw calls / Present | 1,681.526 | 1,682.174 |
| GPU command-buffer time / Present (ms) | 1.986 | 1.988 |
| Chunk rejects / GPU errors / no-pipeline skips | 0 / 0 / 0 | 0 / 0 / 0 |

FPS is positive-wall sample count divided by summed frame wall time, not
process elapsed time or a decoded `.3dr` score. The observed delta is -0.73%.
Both runs pass with exit 0, no timeout and no harness failures. The current
`result.json` staged PE hashes match the final linked receipts above; provider
and bridge hashes still match the reference. The fresh 3,356-byte `.3dr` was
recovered with SHA-256
`598ca666466ea639971650866b1931db94c72b9d004dc558eb5dc537f0530200`.
`actual.png` shows a rendered GT2 scene; one captured frame is not a
whole-scene flicker check or a pixel-equivalence oracle. Counter totals are in
`result.json`; the generated `3dmark05-perf-frames.csv` and summary retain the
sample denominator.

## Limits and next gate

This is a single run against a previous-day sample, not an interleaved A/B
experiment. The small negative delta establishes neither speedup nor a
regression attributable to this change. Locality ratios remain close, but no
strict locality or pixel-equivalence promotion is claimed. H248's event
frequencies remain prioritization evidence, not saved CPU time. Matched
repeatability and whole-scene correctness evidence remain open; this run does
not measure the current PE bottleneck share.
