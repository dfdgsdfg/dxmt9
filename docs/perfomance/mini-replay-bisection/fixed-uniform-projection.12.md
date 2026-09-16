---
domain: mini-replay-bisection
workload: 3DMark05 GT2 fixed Render Tape
title: "Direct fixed-uniform projection reuse: equivalent values, small local cost reduction"
type: experiment
status: bounded-equivalence
updated: 2026-09-16
source: experiments/output/gt2-fixed-uniform-projection-20260916-r626.noindex; experiments/output/gt2-fixed-uniform-projection-20260916-r627.noindex; experiments/output/gt2-fixed-uniform-projection-20260916-r628.noindex/campaign.json; experiments/output/gt2-fixed-uniform-projection-20260916-r628.noindex/campaign-complete.json; experiments/output/gt2-fixed-uniform-projection-20260916-r628.noindex/audit.py; experiments/output/gt2-fixed-uniform-projection-20260916-r628.noindex/audit.json
related: docs/perfomance/mini-replay-bisection/replay-cost-owners.11.md; specs/backend/requirements.md; specs/backend/spec.md
---

# Direct fixed-uniform projection reuse

## Result

The duplicate fixed-uniform extraction identified in the
[owner investigation](replay-cost-owners.11.md) is removed from Direct append.
Same-source CPU semantics and fresh exact GPU readbacks pass before and after.
Fixed lookup scopes fall from about 0.85 to 0.71–0.72ms per iteration, but
ordinary whole-iteration CPU changes direction with run order. This is a small
local simplification, **not an established whole-pipeline or FPS gain**.

## Implementation and authority

`src/d3d9/core_draw.cpp`, `submitDirectReplayDrawFromCurrentState`, now borrows
the existing `CachedBaseDrawState::uniformFixedPayload` alongside its full
uniform payload. `src/dxmt9/dxmt9_source_payload.cpp`,
`TransactionalChunkSlotAssembler::tryAppendDirectDraw`, consumes it during
the synchronous append. `src/dxmt9/dxmt9_backend_types.hpp`,
`ChunkSlot::appendDrawUniformPayloadWithFixedProjection`, performs unchanged
hash-and-content lookup and copies a missed component into Slot-owned storage.
The ordinary full-payload entry derives the projection once, sharing it between
lookup and append-on-miss instead of extracting it twice.

There is no retained pointer, new component-handle cache, changed invalidation
predicate, reordered command, relaxed capacity check or resource-lifetime rule.
No runtime knob, observer or PE wire/bridge change is added. R-BACK-2.15.2
requires the supplied value to equal the pure projection of that same refreshed
full payload. The reference is `append(payload, project(payload)) ==
append(payload)`, not a new cross-draw temporal reuse protocol.

Native coverage uses the production assembler for all 8³ fixed/VS/PS
two-valued traces, both ordinary hashes and forced fixed-hash collisions:
1,024 traces, each across rollback, commit and clear/reuse. Tests compare
materialized values, component counts, parameter order/handles and command
topology after mutating source temporaries; a capacity rejection must roll back
owned bytes. Real Device/cache tests bind the projection to the full payload
across the existing 64-family Direct matrix and fixed-state A/A/B/A setters.
This is bounded value equivalence, not proof of arbitrary driver behavior.

## Frozen comparison and qualification

- Baseline HEAD `87e98b2b` retains the r624 executable/provider/library hashes;
  candidate is that HEAD plus this uncommitted runtime/test patch. This is a
  **cross-image refactor comparison with the same policy**, not a same-binary
  policy comparison. Meson options match; both use the same current validator.
- Same r405 original, source-plan SHA-256
  `cc6b8312ea6abfc0a571741faca56aec729d8dac57e5585a3589bce29aac34f6`,
  ProductionSlot exact, zero headroom, 185 capture-source boundaries.
- Completed r628: eight CPU semantic verification iterations, four cold-device
  GPU iterations, 64 ordinary retained samples, 32 diagnostic retained samples
  and twelve warm-ups: **120 iterations**. CPU measurement order is baseline,
  candidate, candidate, baseline; each process discards only its two declared
  warm-ups and retains sixteen samples.
- All 1,866 ordered draws remain equivalent: 1,856 Direct / ten compatibility;
  185 sources submitted/completed/reclaimed, 182 leases and one SoA growth per
  CPU iteration. Uniform component work is identical: 1,862 payload records,
  184 fixed components, 870 VS components and 259 PS components.
- Both GPU images reproduce the original source/output RGBA exactly, twice:
  `d5a01daf5f2fcf66c90e08bf6e9064a5902eda58aa717d321ce4d11ee6701284`.
  Independent post-publication audit rehashes runtime images, original/staged
  bundles and readbacks, repeats shared acceptance predicates and recomputes
  statistics. Diagnostic receipts remain ineligible for ordinary ranking.
- Native 12/12 and x86 7/7 focused specs including the deterministic Direct
  Metal oracle, full native/x86 builds and all 196 existing TLC configurations
  pass. No new temporal model is claimed for the pure substitution.

Two controller setup failures are preserved, not relabelled complete. r626's
frozen arm64 validator lacked its native dylib and stopped before provider
execution. r627 passed baseline CPU/GPU work, but the controller omitted its
own subprocess exit/timeout metadata required by the GPU gate. r628 uses a
shared authenticated validator and actual wrapper execution sidecars without
editing published child receipts. No renderer fix or weaker oracle was needed.

## Cost, scope and limitations

| Ordinary process pair | Baseline CPU median | Candidate CPU median | Candidate change |
|---|---:|---:|---:|
| Baseline → candidate | 69.669ms | 70.372ms | +1.01% |
| Candidate → baseline | 69.643ms | 69.413ms | −0.33% |

The baseline brackets differ by −0.04%, while the two candidate process
medians differ by −1.36%. Whole-iteration means are also tail-sensitive
(baseline 71.592/69.794ms; candidate 75.277/71.200ms). These samples do not
establish either a repeatable overall win or comprehensive non-regression.

The separate diagnostic pair reports replay CPU means 15.843→15.596ms
(−0.247ms, −1.56%), with admission 19.277→19.288ms. This is one ordered
diagnostic pair, not a qualification statistic. Fresh object setup also varies
12.857→11.578ms; it must not be credited to this replay-only change.

Ordinary fixed lookup elapsed scopes report 0.8502/0.8507ms before versus
0.7174/0.7094ms after, normalized across all eighteen iterations including
warm-ups. Snapshot hit+miss scopes remain about 5.82–5.88ms. `_cpu_ms` scopes
use elapsed clocks, overlap parent scopes and include observer work. The
ordinary full-payload wrapper now derives outside the fixed-lookup scope;
therefore its small compatibility share is a timer-boundary change as well
as removal of a second projection on misses. Do not sum timer differences
into process CPU savings or claim the entire old lookup cost was removed.

CPU fixtures force perf counters, disable replay/encode workers and GPU
submission, use fresh resources and controlled completion. They are not
observer-off or live asynchronous service measurements. No new Wine wild run,
FPS measurement, non-GT2 qualification or renderer-policy promotion is claimed.
GPU correctness uses the existing strict readback oracle without additional
capture/validation instrumentation.

The larger remaining candidate is category-generation-qualified snapshot
construction. Persistent Slot component reuse is a separate owner/generation
protocol requiring rollback/replacement/stale-identity refinement; this patch
does not introduce it or close planning, closure or concurrency residuals.
