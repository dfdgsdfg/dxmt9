---
domain: mini-replay-bisection
workload: 3DMark05 GT2 fixed Render Tape
title: "Uniform hashing: dependency work versus nested observation"
type: experiment
status: bounded-attribution
updated: 2026-09-16
source: experiments/output/gt2-uniform-hash-observer-20260916-r630.noindex/campaign.json; experiments/output/gt2-uniform-hash-observer-20260916-r630.noindex/campaign-complete.json; experiments/output/gt2-uniform-hash-observer-20260916-r630.noindex/audit.py; experiments/output/gt2-uniform-hash-observer-20260916-r630.noindex/audit.json; experiments/output/gt2-uniform-hash-observer-20260916-r630.noindex/native-tests.log; experiments/output/gt2-uniform-hash-observer-20260916-r630.noindex/native-observer.log; experiments/output/gt2-uniform-hash-observer-20260916-r630.noindex/x86-tests.log; experiments/output/gt2-uniform-hash-observer-20260916-r630.noindex/perf-audits.log
related: docs/perfomance/mini-replay-bisection/snapshot-local-dedup.13.md; docs/perfomance/mini-replay-bisection/replay-cost-owners.11.md; specs/backend/requirements.md; specs/backend/spec.md
---

# Uniform hashing: dependencies and observer cost

## Verdict

The previous ~1.7ms uniform-hash parent is **not all hash work**. Same-image,
same-input forward/reverse ablation removes **0.467–0.481ms (27.7–28.3%)**
by suppressing nested observation while preserving hashes, payloads and reuse
predicates. The parent retains **1.217ms**, including real computation,
bookkeeping, its own observation and elapsed-time effects. That residual is
not a claim that every instruction is necessary, or that it is removable.

The preceding snapshot-local change is committed as `4e634bd9`. This follow-up
adds a diagnostic selector, not a new hash/cache optimization or default
policy. The normal `full` observer behavior remains unchanged.

## Dependency ownership

All symbols below are in `src/d3d9/core_draw.cpp` unless stated otherwise.

| Work | Actual authority / dependency | Classification |
|---|---|---|
| VS / PS hash reuse in `cachedBaseDrawStateForSubmissionBatch` | Prior valid cache, corresponding constant generation **and** shader usage bounds | Necessary proof; a generation-only or pointer-only shortcut is invalid |
| `syncShaderConstantHashIndex` | Index validity, stage generation and dirty float/int/bool ranges | Incremental byte hashing plus index updates; invalid/missing-range evidence rebuilds the index |
| `hashShaderConstantsIncrementallyForUsage` | Current index plus usage counts and unknown/indexed flags | Prefix selection/combine may be needed after a usage change even without changed constant bytes |
| Nonconstant hash/payload reuse | Nonconstant generation, clip-plane coordinate-space compatibility; complete payload validity for payload reuse | Current aggregate authority is conservative, not proof that every component changed |
| Complete uniform reuse | Both stages' generation/usage, nonconstant generation, clip mask, coordinate space and complete payload validity | Preserve the whole witness; hashes are not standalone generation authority |
| Shader-layout reuse | Dedicated `drawShaderLayoutGeneration_` | Preserve; the shared reason mask is consumed by either cache lane and previously lost shader/declaration invalidations |
| Nonconstant construction/hash | Matrices, material, lights, texture transforms and clip planes; FFP clip planes also depend on view/projection and programmable/FFP mode | Potential category-qualified future reuse; no new per-component authority is introduced here |
| Scalar WORLD0 versus blend[0] | Same-call production construction establishes value equality | Does not prove equality for arbitrary external payloads; no generalized hash shortcut |

Render/TSS/sampler key hashes use table size and rolling hash, not another
full table scan. Nonconstant generation can invalidate several fixed fields
together: unchanged material/light/texture components inside a rebuild are a
possible future opportunity, **not measured duplicate work by default**.

The unchanged work counters per iteration clarify the logical/physical split:

- 514 batch misses: four full uniform reuses, 88 nonconstant payload reuses,
  422 full builds; 883 total uniform build/refresh calls, including other paths.
- Batch VS hash reuse/build: 30/484; PS: 431/83.
- Actual indexed constant hashing reports **830,128 VS bytes** and **6,464 PS
  bytes** over all paths. The 474 `vs_const_hash_full_indexed_float` events
  describe the conservative logical usage domain, **not 474 full byte scans**.
  Full/unknown reasons and physical indexed-update bytes must not be conflated.
- These counts agree in `full` and `counts`. In `parent`, hash byte/reason
  counters intentionally read zero; snapshot path counts and output semantics
  remain identical. Zero suppressed counters do not mean zero work.

## Controlled separation

`DXMT9_PERF_UNIFORM_HASH_OBSERVER_MODE` is read once and only consulted when
snapshot perf is enabled. It introduces no hot signature, payload, queue or
bridge type. Unset/unknown values preserve `full`.

| Mode | Nested hash timers and their publication | Hash byte/reason counts | Enclosing hash timer and all semantic work |
|---|---|---|---|
| `full` | Existing behavior | Existing behavior | Preserved |
| `counts` | Suppressed | Preserved | Preserved |
| `parent` | Suppressed | Suppressed | Preserved |

The selector only affects `hashDrawUniformShaderConstantSnapshots`,
`hashDrawUniformNonConstantComponents` and `combineDrawUniformPayloadHashes`.
`makeDrawUniformPayloadFromState` / constant refresh retain their parent
scope, payload writes, fixed hash combination and snapshot path counters.
Other replay, setup, queue and hash-adjacent observers remain enabled.

Two clocks and timing-counter publication form one observed unit here.
`full − counts` is **not pure clock latency**; `counts − parent` also includes
the corresponding control-flow/publication footprint. No nested scope is
added to its parent to construct a synthetic CPU budget.

## Measurements

Same frozen x86_64/O3 image through Rosetta, ProductionSlot exact/zero headroom,
same original r405 GT2 Tape and capture-source boundaries. Each measurement
process uses two warm-ups and sixteen retained samples. Mode order is
`full → counts → parent → parent → counts → full`, repeated separately with
diagnostic replay phase accounting enabled.

Parent elapsed scopes include all eighteen iterations, divided by eighteen:

| Run order | `full` | `counts` | `parent` | Child timing footprint | Child count footprint |
|---|---:|---:|---:|---:|---:|
| Forward | 1.699ms | 1.265ms | 1.217ms | 0.434ms | 0.047ms |
| Reverse | 1.684ms | 1.264ms | 1.217ms | 0.420ms | 0.047ms |

The separate diagnostic brackets repeat 1.675/1.683ms (`full`) versus
1.215/1.218ms (`parent`): 0.460/0.465ms observation footprint. Their replay
CPU means fall **14.468→13.994ms** and **14.486→14.085ms**; admission remains
19.25–19.33ms and publication 7.34–7.40ms across the six diagnostic processes.
The reverse `counts` replay mean is lower than `parent`, illustrating why
small count-only differences must not be ranked from phase totals alone.

Whole-iteration CPU medians are less stable:

| Order | `full` | `counts` | `parent` | `full → parent` |
|---|---:|---:|---:|---:|
| Forward | 69.324ms | 67.772ms | 67.755ms | −2.26% |
| Reverse | 68.165ms | 67.712ms | 67.800ms | −0.54% |

The full-mode brackets themselves differ by ~1.67%; do **not** attribute the
entire forward 1.57ms difference to an observer whose local scope footprint
is ~0.48ms. Fresh object-setup means in diagnostic runs also range 9.13–12.65ms.
All samples are retained. The stable conclusion is local observer attribution,
not a 2.26% renderer improvement or an FPS prediction.

## Verification and provenance

- Native focused 5/5 plus the new process-isolated observer test pass; x86
  focused 7/7 pass, including the Direct Metal oracle. Full native/x86 builds
  and all three perf-counter audits pass. The mode parser truth table covers
  empty/unknown fallback and exact `full`/`counts`/`parent` selection.
- `dxmt9-uniform-hash-observer-spec` executes the complete transform fixture
  in five perf-enabled processes (unset/full/counts/parent/invalid), checks
  nonvacuous production build/byte counts, unchanged snapshot counts and
  suppressed child rows, then repeats three modes with perf disabled.
- 234 campaign iterations: twelve CPU semantic verifications, six GPU exact
  iterations, 96 retained measurements, 96 retained phase diagnostics and
  24 warm-ups. Qualification precedes measurement; CPU verification repeats
  after the reverse sequence. CPU ordered draw digests also match r629.
- All modes preserve 1,866 draws (1,856 Direct / ten compatibility), 185
  submitted/completed/reclaimed sources, 182 leases, one SoA growth, 1,862
  uniform records and fixed/VS/PS component counts 184/870/259.
- Source plan SHA-256:
  `cc6b8312ea6abfc0a571741faca56aec729d8dac57e5585a3589bce29aac34f6`.
  Each GPU mode matches the original source/output bytes twice, digest
  `d5a01daf5f2fcf66c90e08bf6e9064a5902eda58aa717d321ce4d11ee6701284`.
- The independent artifact audit reauthenticates frozen provider and loaded
  library images, shared validator, immutable components, execution statuses,
  source settlement, readbacks, mode receipts and controller hash before
  recomputing all costs. Cross-mode policy binding is intentionally rejected;
  phase diagnostic receipts remain excluded from ordinary policy ranking.
- R-BACK-2.15.4 classifies this as observation-only refinement. No lifecycle,
  generation, shader/layout, queue, allocation or completion protocol changes;
  no new TLA+ model or fresh TLC run is claimed for this diagnostic change.

## Next boundary

First use a consistent lower-detail observer configuration when evaluating a
future hash optimization. The ~1.217ms residual is an **envelope**, not the
benefit available from removing nonconstant duplication. Any category-qualified
reuse must price its bookkeeping and prove all relevant setter, StateBlock,
reset, shader-mode, usage and generation transitions before changing the hot
path. Existing hash/content interning and exactly-once source ownership remain
unchanged. Fine timer opt-in/default policy is a separate decision; this patch
deliberately preserves the current default.

These are bounded instrumented CPU-fixture results with fresh resources,
disabled workers/GPU submission and controlled CPU completion, plus a separate
real GPU oracle. No Wine wild run, asynchronous service qualification,
global observer-off measurement or FPS claim is made.
