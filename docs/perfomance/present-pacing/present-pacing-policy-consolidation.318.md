---
domain: present-pacing
workload: bounded replay / queue / constant-buffer fixtures
title: "Consolidate qualified redundant-work reductions and retire stale runtime hypotheses"
type: evidence
status: implemented
updated: 2026-09-17
source: build/meson-logs/policy-consolidation-r640-focused.txt; build/meson-logs/policy-consolidation-r640-metal-validation.txt; build-x86_64-builtin/meson-logs/policy-consolidation-r640-x86-runtime.txt; build/meson-logs/policy-consolidation-r640-tla.txt
related: overview.md; present-pacing-lock-scope-attribution.311.md; ../mini-replay-bisection/uniform-invalidation-qualification.16.md
---

# One implementation per accepted optimization

The policy decision is structural acceptance of bounded, behavior-preserving
redundant-work reductions, not a new FPS or statistical non-regression claim.
This change is based on `7ee75f1f`; no README performance value changes.
Four environment selectors are no longer read by runtime code. Their old
`0`/`1` values cannot select a legacy implementation.

## Runtime decisions

| Former selector | Decision | Preserved authority / removed work |
|---|---|---|
| `DXMT9_REPLAY_PRECISE_UNIFORM_INVALIDATION` | Unconditional | Known render-state setters use the shared dependency predicate. Generic mutation, StateBlock and reset retain conservative invalidation. Remove unnecessary known-setter nonconstant rebuilds and the runtime opt-out. |
| `DXMT9_LOCK_SCOPE_REFINEMENT` | Unconditional | Range uploads remain synchronous under the buffer-arena lock, without an unused queue-watermark read or outer queue lock. Captured concrete-backing stamps use bounded batches; immutable compatibility preflight runs before queue acquisition. Keep live snapshot capture, destination revalidation and actual Slot mutation synchronized. |
| `DXMT9_PREFETCH_UNPUBLISHED_SLOT_PSO` | Retired | Remove the unused synchronous unpublished-slot entry, partial-prefetch cursor/mode and exclusive counters. Normal sealed encode-slot PSO prefetch remains. |
| `DXMT9_ARGBUF_CBUF_CONTENT_HASH` | Retired | Remove the optional second uploaded-byte hash, exclusive binding metadata/helpers and timing counters. Keep authoritative payload/category identity and exact FFPVS byte comparison. |

The lock path retains per-command/effect flush fences, max-stamp semantics,
generation checks and source-local additive snapshot-byte accounting. It does
not move live-backing selection out of its synchronization domain. Full uploads,
managed rotation, completion and reclaim keep their existing protection.

## Decision provenance

- [Uniform qualification](../mini-replay-bisection/uniform-invalidation-qualification.16.md)
  retains exhaustive shared-predicate refinement, arm64/x86_64 GPU equality
  and eight same-image GT1/GT2 wild runs. The small mean FPS differences and
  repeat drift did not establish statistical non-regression.
- [H308](present-pacing-lock-scope-refinement.308.md) through
  [H311](present-pacing-lock-scope-attribution.311.md) retain exact buffer,
  captured-stamp, final-Slot and real-PE evidence. H311 reproduced roughly
  18 FPS on the original H309 provider too: the earlier large decline was
  not independently attributable to the lock change. Batch populations,
  contended tails and broad workload economics remain open.
- Historical H58 in the surviving [log](log.md) rejected synchronous
  unpublished prefetch as work relocation without overlap. Its original
  `.53.md` leaf is absent; the archived figures are not a fresh, independently
  recheckable experiment. The now-removed public entry also has no current
  callers.
- The old content-hash probe did not authorize reuse: current consumers use
  identity or exact FFPVS bytes. Removing the probe and unused hash storage
  does not replace an equality check with a weaker one.

Historical OFF/ON runs retain their original policy descriptions. They are
not silently relabelled as measurements of this unconditional implementation.

## Current verification

- Full native, x86_64 Unix, x64 PE and x86 PE builds pass. An initial native shell
  without SDKROOT failed to locate Foundation; no source workaround or build
  option change was needed. Both PE builds retain the pre-existing unrelated
  `g_ntAllocScanHint` unused-variable warning in `device_c_marshal.cpp`.
- Metal API validation passes six focused arm64 checks and ten x86_64
  runtime checks, including exact uniform pixels, range contention, captured
  stamps and query completion. These are native/Rosetta fixtures, not new
  Wine or supervised game runs.
- The focused native suite passes **29/29**, including resource lifetime,
  completion, range contention, queue batching/final-Slot comparisons, cbuf
  identity, query regression and retirement/perf-schema audits.
- The test-only conservative uniform reference produces 30 full rebuilds;
  each production run produces 21 builds and nine full reuses. All 30 frames
  match exactly with the former selector unset, `0`, and `1`. Transcript SHA-256:
  `ef83939770ce9b6e6728bd564b5fa8207f38a0113683d8d61df552ad3d955fd7`.
- The retirement audit scans `src/` and `include/` for selectors and exclusive
  obsolete symbols. It is a source-surface guard, not a semantic proof.
- Full TLA revalidation passes **62 safety + 4 progress + 144 expected
  counterexample configurations**, 210 total. No model transition changes in
  this consolidation; this rechecks the existing bounded contract inventory.
- Perf schema is 2,403 ordered unique rows. Besides removing nine obsolete
  emitted keys, its golden list catches up with four existing Query END/Poll
  publication reason/command counters; those counters were already in runtime.

The runtime source diff removes a net 283 lines, including comments, while
keeping test-only reference paths. No new wild FPS result is claimed by these
fixtures.

## Deliberately not folded into this cleanup

[H317 END coalescing](present-pacing-query-end-coalescing.317.md) remains a
default-OFF, active lifetime/publication policy experiment, not a superseded
hash or prefetch probe. CPU-ready Tape/headroom, segmented/inline transports,
independent parallel/resource-array providers and required capability
fallbacks are also unchanged. A correct picture alone does not discharge
their capacity, lifetime, progress or provider-compatibility contracts.

The [render-provider contract](../../../specs/backend/render-provider/requirements.md)
R-BACK-42.19 and [lock contract](../../../specs/backend/lock-scope-refinement.md)
R-BACK-SYNC-5 distinguish this consolidation from performance proof. Bounded
models do not prove arbitrary C++ interleavings or driver faults. Broader MT,
live pacing and workload non-regression remain in the owning gaps; a future
reproducible regression still requires investigation.
