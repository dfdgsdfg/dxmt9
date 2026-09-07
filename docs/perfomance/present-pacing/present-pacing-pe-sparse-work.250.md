---
domain: present-pacing
workload: 3DMark05 GT2
title: "PE active sparse traversal and bounded dirty clearing"
type: evidence
status: native-codegen-and-single-run
updated: 2026-09-07
source: experiments/output/pe-sparse-work-20260907/analysis; experiments/output/app-d3d9-3dmark05-pe-sparse-work-77518738-gt2-r1-20260907; experiments/output/app-d3d9-3dmark05-pe-flush-layout-66cbcbab-gt2-r1-20260907
related: specs/d3d9/recorder/requirements.md; present-pacing-pe-flush-layout.249.md
---

# PE active sparse work

## Scope

Base: `cd979749` (runtime implementation `66cbcbab`). Recorder requirements
R-CORE-REC-7.45–7.47 reduce work over inactive input; they do not change record
cadence, canonical bytes, transport selection, lifetime or queue scheduling.

- Compact texture/stream/render-target plan checks, admission and emission
  traverse ascending set bits. Domain masking retains the old bounded-loop
  visitation set; existing malformed-source rejection remains authoritative.
  Dense full-snapshot discovery and generic reference builders stay unchanged.
- `ConstShadow::clear` clears the pending interval rather than the historical
  bitmap extent. Setters and StateBlock restoration must keep every dirty bit
  inside that interval, including disjoint writes with clean holes. Reset
  clears extents and bounds directly; it never applies old bounds to an empty
  vector. Values and reserved capacity are preserved by ordinary settlement.
- Compact capacity proof and logical quota publication reuse the prepared
  section inventory. No active mask, cache or new mutable storage is added.
  All 17 section-kind/quota mappings are compile-time coupled to owner counters.
  Constants and UP tails keep their separate capacity accounting.

## Verification

All four canonical full builds pass. Native PE/audit tests pass 29/29 and
x86_64 focused tests pass 8/8. Fresh constant observer processes at N=1/127
pass. The existing, unchanged TLA suite passes in 148.63 seconds. Local
iteration and storage refinements use native differential evidence, not a new
concurrency model. Constant focused Meson and ASan/UBSan runs pass.

The constant fixture compares actual fixed-width/generic writes and successful
settlement against full-bitmap clearing across all six families: high-to-low
reuse, equal/disjoint/A/B/A writes, failed flush/retry, restored ranges, and
reset while dirty. It also covers all 256 eight-register membership masks.
The StateBlock restoration adapter and append callback are bounded surrogates,
not execution of the private PE COM device. Existing 4,032 six-family dispatch
traces remain as independent order/population evidence.

Sparse iterator tests exhaust the 0/4/8/16/20-bit domains, comparing ascending
order and failure prefixes to a full scan. Actual compact owner admission
independently exhausts each of the 17 quota dimensions: context-retained
streams prevent an unrelated repair stream from masking the intended denial,
and each prepared delta array is asserted to contain only the tested category.
An all-family production layout compares active quota publication to a full
section-count reference. Existing empty/sparse/full canonical bytes, null and
duplicate bindings, stale qualification, A/B/A, rollback and retry tests pass.
The generic reference producer is unchanged.

## Linked PE evidence

| Actual linked body | PE32 static instructions, before → after | PE64 static instructions, before → after |
|---|---:|---:|
| `buildSparseStatePlan` | 543 → 505 | 584 → 422 |
| Compact `proveCapacity<true>` | 144 → 99 | 170 → 114 |
| `settleSemanticConstantRecord` | 60 → 58 | 49 → 50 |

These counts include padding and cold branches; they are not executed work,
CPU time or FPS. Initial codegen outlined six/eight iterator specializations
and added three plan calls. The final small iterator is explicitly inlined:
no standalone iterator symbols remain, and the plan retains six calls while
using three bit-scan loops. The prepared-layout builder itself is unchanged.
Quota publication folds into its caller; absence of a `copySparsePlan` symbol
does not mean publication work disappeared. The compact writer entry remains
nearly unchanged (PE32 394 → 397; PE64 379 → 379); its visitors own the sparse
iteration change.

The real constant settlement still calls `memset`, now with bitmap base plus
dirty start and accepted register count, rather than bitmap base and full
historical extent. PE32/PE64 total DLL sizes change by +512/-512 bytes.
Exports and vtable/RTTI archive owners match. The Unix provider and both
bridge DLLs remain byte-identical to baseline.

Final PE32 SHA-256:
`e6bc5a372221894b879af4126038286aa70dc15ad2a6f1446affb227d9529765`.
Final PE64 SHA-256:
`720c91e5e2e6711d5416c8184603076be5d81fd1c75f4e8eec34bd251900517c`.
The analysis directory preserves `baseline-linked.json`, initial
`candidate-linked.json`, final `verified-linked.json`, their linked assembly,
`codegen-summary-final.txt`, `build-*-inline.log`, `*-tests-inline.log`,
`const-sanitized-r2.log`, `observer-n{1,127}.log` and `tla.log`. The initial
constant test link failure is retained; linking the existing `dxmt9_util_dep`
supplies the production setter's `getenvU32` dependency without a test stub.

## Post-commit GT2 spot check

Implementation commit `77518738` was checked through all three canonical full
staging builds, then run once through `run_experiment.py`. The current artifact
is `app-d3d9-3dmark05-pe-sparse-work-77518738-gt2-r1-20260907`; the reference is
H249's `app-d3d9-3dmark05-pe-flush-layout-66cbcbab-gt2-r1-20260907`.

Both use named GT2, 1024x768, Sikarugir `sikarugir-cx-24.0.7`, perf/Warn,
validation OFF, frame sampling ON, and PE decimation/recorder stats/semantic
phase observation OFF, without encoder breakdown or GPU trace. The staged
PE hashes match the final linked receipts above. The provider and bridge DLLs
remain byte-identical to the reference; no concurrent Wine or build process
was present before launch.

| Metric | H249 `66cbcbab` | `77518738` |
|---|---:|---:|
| Positive-wall sampled FPS | 28.835 | 28.846 |
| Positive-wall sample count | 1,863 | 1,861 |
| Sampled wall time (ms) | 64,608.102 | 64,514.336 |
| Encoded Presents | 1,864 | 1,862 |
| Command buffers / Present | 3.99946 | 3.99946 |
| Render passes / Present | 15.77361 | 15.77712 |
| Draw calls / Present | 1,682.174 | 1,683.132 |
| GPU command-buffer time / Present (ms) | 1.988 | 2.050 |
| Chunk rejects / GPU errors / no-pipeline skips | 0 / 0 / 0 | 0 / 0 / 0 |

FPS is positive-wall sample count divided by summed frame wall time, not
process elapsed FPS or a decoded `.3dr` score. The observed change is +0.038%.
The run passes with exit 0, no timeout, no harness failures, and zero pipeline
draw/function/library/PSO failures. `actual.png` shows a rendered GT2 scene;
it is one frame, not whole-scene flicker or pixel-equivalence evidence.
The runner recovered a fresh 3,341-byte `.3dr`, SHA-256
`e8753f4514075a4ac7471692a25e08fc7f2be09ebc5f7ebe988d0841a6b235e3`.
`result.json`, `3dmark05-perf-frames.csv`, the summary, and the analysis
`wild-run-receipt.md` preserve the run and denominator.

One live process snapshot showed unrelated `duetexpertd` at 91.4% CPU and
`mds_stores` at 60.3%; neither was interrupted. This is a confound, not an
attribution of lost FPS or a sustained-load measurement. Together with the
non-interleaved single-run comparison, it prevents claiming either a speedup
or neutrality qualification. CB/pass shape is close; no strict locality
promotion follows from these rounded ratios.

## Limits and next gate

This spot check does not establish a measurable FPS gain or show that PE is
exhausted. H248's historical event frequencies prioritize this work but cannot
price its current CPU cost. Matched repeatability, whole-scene correctness and
current PE symbol attribution remain open. Transport/provider defaults,
inline-constant delta and CPU-ready Tape promotion are outside this change.
