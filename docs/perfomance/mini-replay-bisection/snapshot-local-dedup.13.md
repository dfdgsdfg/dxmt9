---
domain: mini-replay-bisection
workload: 3DMark05 GT2 fixed Render Tape
title: "Snapshot-local dedup: fitting flat states dominate the recovered CPU cost"
type: experiment
status: bounded-equivalence
updated: 2026-09-16
source: experiments/output/gt2-snapshot-local-dedup-20260916-r629.noindex/campaign.json; experiments/output/gt2-snapshot-local-dedup-20260916-r629.noindex/campaign-complete.json; experiments/output/gt2-snapshot-local-dedup-20260916-r629.noindex/audit.py; experiments/output/gt2-snapshot-local-dedup-20260916-r629.noindex/audit.json; experiments/output/gt2-snapshot-local-dedup-20260916-r629.noindex/native-tests.log; experiments/output/gt2-snapshot-local-dedup-20260916-r629.noindex/native-final-tests.log; experiments/output/gt2-snapshot-local-dedup-20260916-r629.noindex/x86-tests.log; experiments/output/gt2-snapshot-local-dedup-20260916-r629.noindex/x86-final-tests.log; experiments/output/gt2-snapshot-local-dedup-20260916-r629.noindex/tla-full.log
related: docs/perfomance/mini-replay-bisection/fixed-uniform-projection.12.md; docs/perfomance/mini-replay-bisection/replay-cost-owners.11.md; specs/backend/requirements.md; specs/backend/spec.md
---

# Snapshot-local redundant work

## Verdict

Eliminating redundant priority selection in fitting render-state tables is
the main improvement. Ordinary whole-iteration CPU medians decrease by
**1.85% and 1.78%** in opposite execution orders. The diagnostic replay phase
decreases by 0.989ms (6.25%); fresh original GT2 CPU semantics and exact GPU
readbacks remain unchanged. This is bounded, instrumented replay evidence,
not Wine FPS or observer-off/asynchronous service qualification.

The baseline is `dde43e4c`, which commits the prior
[fixed projection reuse](fixed-uniform-projection.12.md). The candidate adds
only the following local construction changes, not more temporal caching.

## Duplicate versus necessary work

| Owner in `src/d3d9/core_draw.cpp` | Classification | Action |
|---|---|---|
| `makePrioritizedFlatStateSet`: priority insertion and repeated linear duplicate checks even when all entries fit | Redundant membership selection; the source table already has unique keys | For `size <= capacity`, emit the table directly; preserve priority-first overflow unchanged |
| `makeFlatStateSet`: sorting the already ordered `StateValueTable` iterator | Redundant sorting | Restrict the helper to this table type and retain its ascending order; stop at capacity |
| `makeDrawUniformPayloadFromState`: scalar WORLD0 transforms repeated alongside blend slot zero | Same-call duplicate math | Reuse blend[0] world-view, inverse-transpose and WVP values; preserve multiplication association and singular fallback |
| Render-state key hash plus flat payload hash | Repeated but constant-cost count/rolling-hash combination, not a second full table hash | Keep; not the measured quadratic owner |
| Shader constant copy/hash and full/nonconstant uniform refresh | Generation- and shader-usage-dependent work, with existing reuse paths | Keep generation/layout authority and all bytes; no pointer/hash-only bypass |
| Repeated fixed-component lookup, nonconstant hashes, category construction across snapshots | Potential future reusable work, not automatically redundant | Needs separate ownership/dependency proof and low-observer cost evidence |

No new hot observer, heap allocation, runtime option, ABI, queue policy or
cache generation is introduced. `StateValueTable` iteration is unique and
ascending; when every entry fits, priority ordering cannot change membership.
The flat output still has sorted entries, zero-filled unused storage, the
complete input hash and the same overflow flag. R-BACK-2.15.3 records these
invariants and the pure-value refinement exemption from a new temporal model.

## Verification

- `state_draw_transform_spec.cpp` adds all 1,024 subsets of ten sparse keys,
  repeated after erase/reinsert/redundant writes, against a slow independent
  duplicate-search/sort reference. Counts 0–128 and the 129→128 boundary are
  checked; existing overflow fixtures still pin high backend-priority keys.
- Another 256 sampler/TSS subsets pin sorted entries and zero tails. All
  216 combinations of six world/view/projection matrices pin exact float bits,
  `(WORLD0 * VIEW) * PROJECTION`, blend[0] equality, missing-world identity
  and zero-matrix singular-normal fallback. Existing noncommuting and
  inverse-transpose expected-value tests remain enabled.
- Native focused 5/5 and x86 focused 6/6 pass, including real cache/replay
  binding and the x86 Direct Metal oracle. Expanded final transform tests pass
  again on both architectures. Full native/x86 builds and all 196 existing
  TLC configurations pass; TLC is a protocol regression check, not a proof
  of the new numerical implementation.
- r629 runs eight CPU verification iterations, four cold-device GPU iterations,
  64 retained ordinary CPU samples, 32 retained diagnostic samples and twelve
  warm-ups: 120 total. Each measurement process retains sixteen after two
  warm-ups, with ordinary order baseline/candidate/candidate/baseline.
- Same r405 original and 185 capture-source boundaries, plan SHA-256
  `cc6b8312ea6abfc0a571741faca56aec729d8dac57e5585a3589bce29aac34f6`.
  Both images use ProductionSlot exact with zero headroom and the same current
  validator. Meson options match and baseline images match r628's candidate.
- Both GPU images match original source/output bytes twice, digest
  `d5a01daf5f2fcf66c90e08bf6e9064a5902eda58aa717d321ce4d11ee6701284`.
  The independent audit reauthenticates all images, input components, readbacks,
  subprocess outcomes, source settlement and semantics before recomputing costs.
  No extra capture or Metal validation instrumentation is enabled.
- Every CPU iteration keeps 1,866 draws (1,856 Direct / ten compatibility),
  185 submitted/completed/reclaimed sources, 182 leases and one SoA growth.
  All snapshot, uniform-component and replay work counters remain identical:
  514 batch misses, render-state reuse 92/422, TSS reuse 346/168, sampler reuse
  462/52, uniform payload paths 4 full reuse / 88 nonconstant reuse / 422 builds.
  The reduction does not come from dropping draws or weakening cache refresh.

## Costs and attribution

| Ordinary process pair | Baseline CPU median | Candidate CPU median | Change |
|---|---:|---:|---:|
| Baseline → candidate | 70.307ms | 69.004ms | −1.85% |
| Candidate → baseline | 70.265ms | 69.016ms | −1.78% |

Baseline brackets differ by −0.06%; candidate brackets differ by +0.02%.
Whole-iteration means are more tail-sensitive: 72.128/70.390ms baseline and
68.998/69.191ms candidate. No retained sample is removed as an outlier.

Existing elapsed scopes, divided by all eighteen iterations including warm-ups:

| Snapshot scope | Baseline ms/iteration | Candidate ms/iteration |
|---|---:|---:|
| Render-state flattening | 1.198–1.211 | 0.125–0.126 |
| TSS flattening | 0.093–0.094 | 0.092–0.093 |
| Sampler flattening | 0.044 | 0.036 |
| FFP matrix construction | 0.156–0.157 | 0.123–0.124 |
| Uniform hashing | 1.696–1.706 | 1.701–1.702 |
| Snapshot hit + miss, inclusive | 5.924–5.986 | 4.737–4.781 |

The removed membership searches dominate the local reduction. Matrix reuse
has a much smaller signal; this combined comparison is not an isolated
attribution of every instruction change. Scope clocks and parent/child work
overlap, so these rows must not be added into a process-CPU budget.

The separate ordered diagnostic pair measures replay CPU 15.828→14.839ms,
admission 19.605→19.593ms and publication 7.514→7.534ms. Candidate object setup
has a fresh-allocation tail (9.437→14.304ms mean), making its diagnostic
whole-iteration mean worse despite lower replay. That is why diagnostic
receipts remain excluded from ordinary ranking and why setup is not attributed
to this snapshot change.

## Remaining scope

Uniform hashing is now more prominent, but its 1.7ms parent includes nested
clock/atomic costs and legitimate generation/usage-dependent work. A larger
optimization needs category-qualified dependencies or measured same-call
hash reuse; it must not generalize scalar/blend[0] equality to arbitrary
externally constructed payloads. Snapshot cache invalidation, field counts,
shader usage, resource closure and completion authority remain unchanged.

This campaign uses x86_64/O3 through Rosetta, CPU-visible Metal resources,
fresh setup per iteration and controlled immediate CPU completion. Workers
and GPU submission are disabled only in the CPU fixture; the separate GPU
oracle uses real completion. Broader inputs, observer-off service costs, live
concurrency and repeated Wine wild/FPS comparison remain unmeasured here.
