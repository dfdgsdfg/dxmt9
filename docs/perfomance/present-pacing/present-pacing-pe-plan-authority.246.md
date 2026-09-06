---
domain: present-pacing
workload: 3DMark05 GT2
title: "PE inactive storage and immutable plan authority"
type: evidence
status: partial
updated: 2026-09-06
source: src/d3d9/d3d9_pe_semantic_owner.hpp; experiments/output/app-d3d9-3dmark05-gt2-perf-plan-authority-{base-a1,base-a2,inactive-b1,inactive-b2}-20260905; experiments/output/app-d3d9-3dmark05-gt2-perf-plan-authority-inactive-c{1,2,3,4}-20260906; experiments/output/app-d3d9-3dmark05-gt2-perf-plan-authority-final-d{1,2}-20260906
related: specs/d3d9/recorder/requirements.md; present-pacing-pe-compact-reuse.245.md
---

# PE inactive storage and immutable plan authority

This follow-up separates inactive storage work (R-CORE-REC-7.13), private
source-selection authority (7.20), and fused retainer acquisition (7.21).
It preserves the mandatory semantic owner, canonical bytes, quotas, transport
defaults, chunk boundaries, and thread policy. H245's instrumented phase sums
are not a product-cost baseline.

## Measurement protocol

The baseline is committed `547a212d`; the canonical staged artifacts match
H245's final build exactly. All cells use GT2, 1024x768, Sikarugir
`sikarugir-cx-24.0.7`, `perf`, Info logging, segmented transport 0, PE stats
decimation 0, semantic-owner phase split 0, recorder stats 0, and thread
sampler 0. FPS is the benchmark-owned `.3dr` result. Existing Unix counters
remain enabled identically; this is a low-perturbation comparison, not a claim
of zero renderer instrumentation. Accepted measurement cells must not overlap
builds, native/TLA tests, other Wine apps, or GPU traces. The 35-second screenshot is a scene anchor,
not an exact-frame pixel differential or full-scene visual oracle.

The fresh same-build A/A precedes the isolated implementation B/B. This checks
short-term baseline repeatability but does not remove ordering/thermal bias;
the measurements are direction evidence, not a noise-qualified promotion.
All paths below have prefix `experiments/output/app-d3d9-3dmark05-`.

| Suffix | Presents | .3dr FPS | CB/P | Pass/P |
|---|---:|---:|---:|---:|
| `gt2-perf-plan-authority-base-a1-20260905` | 1,707 | 27.291 | 3.9994 | 15.7692 |
| `gt2-perf-plan-authority-base-a2-20260905` | 1,711 | 27.353 | 3.9994 | 15.7656 |
| `gt2-perf-plan-authority-inactive-b1-20260905` | 1,786 | 28.559 | 3.9994 | 15.7637 |
| `gt2-perf-plan-authority-inactive-b2-20260905` | 1,771 | 28.321 | 3.9994 | 15.7696 |
| `gt2-perf-plan-authority-inactive-c3-20260906` | 1,783 | 28.514 | 3.9994 | 15.7672 |
| `gt2-perf-plan-authority-inactive-c4-20260906` | 1,787 | 28.585 | 3.9994 | 15.7678 |
| `gt2-perf-plan-authority-final-d1-20260906` | 1,769 | 28.294 | 3.9994 | 15.7654 |
| `gt2-perf-plan-authority-final-d2-20260906` | 1,771 | 28.319 | 3.9994 | 15.7736 |

The completed cells have zero chunk rejects, GPU errors, pipeline-library
failures, and no-pipeline skips. The machine summary and source/codegen
checkpoints live in the base-a1 directory's `analysis/`; H245 artifacts are
unchanged. The pair harmonic means are about 27.322 and 28.440 FPS (+4.09%).
This does not independently price the removed stores: run-order effects remain
uncontrolled and H245's earlier same-binary OFF result was already 28.326 FPS.
After the overnight pause, C1 reuses the exact B1/B2 staged artifacts without
rebuilding (PE32 `e46cdfe9f7c13a86`, PE64 `328639fb1d095b07`, Unix
`f7935d35db31eb12`; SHA-256 prefixes). **C1 and C2 are excluded from performance
comparison**: a worker ran native builds/tests despite the coordinator's
measurement/build exclusion instructions. C1's 27.998 FPS is an artifact fact,
not a qualified same-binary reference. The runner passes with zero errors;
this does not remove the timing contamination. These artifacts are retained,
not overwritten. A/A and B/B are unaffected September 5 measurements.
After all workers were released, C3/C4 repeat the same phase-1 binaries with
no competing build/test. Their harmonic mean is 28.550 FPS; they are the
fresh reference for the integrated refinement, not additional base-A cells.
Integrated D1/D2 complete at 28.294/28.319 FPS, both below C3/C4. Their harmonic
mean is 28.306 FPS, about **0.85% lower** than the clean phase-1 reference. This
does not establish an incremental throughput gain or noise-qualified neutrality
from private plans/fused retention. The representation/authority refinements
have bounded correctness evidence; an FPS promotion is not accepted. The overall
sequence is higher than the original A/A, but day/run order and the lack of an
interleaved control prevent assigning that difference to any individual change.
Final PE32/PE64 hashes are `3b82e1068d1c1943` / `f7b2e9d6e3a7dfee`; the Unix
provider and both bridge DLL hashes are unchanged. Capture inspection is a
coherent forest/firefly scene anchor in both final runs, not a whole-scene or
exact-pixel oracle. CB/P stays 3.9994 and pass/P 15.765–15.774, with zero errors;
neither run changes transport, locality policy, or the Unix provider binary.

## Inactive binding locator refinement

`PeSemanticBindingLocator` default member initializers caused all 45 slots
(a 720-byte array) to be initialized on each sparse prepared-record creation.
The fields are now written completely only for active occurrences. Compact
admission publishes each family's active count only after successful writes.
Identity-witness storage was already active-prefix based; it was not an
80-identity tail initialization bug.

The canary/reset/reuse native pin covers untouched inactive storage and full
null-field replacement; existing owner differentials cover all binding and
producer families, failure/retry, and exact canonical emission. Native 7/7 and
x86_64 owner/batch/producer/install-name 4/4 pass, and all four canonical
builds pass. PE32/PE64 unobserved sparse prologues no longer contain the locator
initialization loop. Their stack frames remain 4,336/4,432 bytes and still
probe: eliminating stores does not eliminate reserved stack space.

The change is a bounded value/active-frontier refinement, not a new concurrent
transition. Existing transaction models remain applicable; native bytes and
ownership checks, not TLA, establish the concrete representation equivalence.
The unchanged `dxmt9-verify-tla` suite passes in 147.00 seconds
(`build/meson-logs/plan-authority-tla.txt`). This is protocol regression
evidence, not a new proof of private C++ fields or wire values.

## Private source authority and fused acquisition

`SparseStatePlan` now holds private producer-issued source facts and a separate
private destination overlay, with const-only accessors. Copy, move, public
reset, source rebinding, and writable fact views are closed; public invalidation
only revokes authority. Normal compact settlement reuses authenticated selection
instead of reconstructing texture/stream/shader/vertex/index/RT/boolean/clip/light
expectations. It retains four typed count checks, source tickets and recorder
borrows, constant dirty-range checks, destination dependency checks, schema and
identity validation, observer token projection, and actual pending consumption.
Full snapshots keep their reference validation.

The retainer's bounded typed overload performs one kind-qualified lookup for a
new owner pin. A warm hit refreshes its epoch even at the ceiling; a novel entry
is rejected before unique-acquisition fault consumption, AddRef, or index
publication. The private lookup index never escapes. Owner kind/generation/object
validation and its separate identity/object indexes are unchanged. This removes
one warm-set lookup on new owner pin registration, not one lookup on every draw.

New native coverage includes 192 producer/builder comparisons (three ordinary
families, every pair of three-bit texture/stream masks), exact section quotas,
constant-range mutation/rollback/retry, binding identity collisions, and stale
frontier/recorder authority. Existing destination A/B/A and CapacityPre fixtures
remain. The retainer compares all 9,604 length-four traces over seven operations
and capacities 0–3, including every shorter prefix; checkpoints cannot cross
epoch compaction. Opaque same-address/different-kind refs, no-allocation ceiling
behavior, and fresh-process capacity/duplicate/unique-fault rollback are pinned
separately. These bounded products are not exhaustive over arbitrary D3D9 data.

## Remaining gates

Integrated validation passes: all four canonical full builds, native 21/21
(`build/meson-logs/plan-authority-final.txt`), x86_64 7/7 (same receipt name in
`build-x86_64-builtin/meson-logs/`), and unchanged TLC 154.84 seconds
(`build/meson-logs/plan-authority-final-tla.txt`). The final release objects,
disassembly, source patch and build options are frozen under
`analysis/final-integrated-*`. Compact PE32/PE64 append frames remain
4,336/4,432 bytes. The bounded buffer retain body performs one warm hash probe
then refresh/return on hit; novel capacity and fault checks remain real runtime
work. There is no claim that retention itself is zero-cost.

Keep source-dependent checks distinct from redundant derivation: a
private plan does not make borrowed payload immutable. Broader Wine fault/capture
and workload visual/performance coverage remain separate.
