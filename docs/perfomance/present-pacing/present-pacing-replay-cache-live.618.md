---
domain: present-pacing
workload: STALKER Day high 1280x720 max-threads2 snd_targets16 late spatial interval
title: "H618 — live uniform work exceeds other cache components"
type: experiment
status: bounded-attribution
updated: 2026-10-16
source: traces/stkcop-replay-cache-live-h618/{contract.md,live-contract.md,build.json,preflight-verified.json,native-tests.json,extra-tests.json,detail-tests.json,source-verified.json,process-exit-tests.json,comparison.json,verification.json,groups.json}; traces/stkcop-replay-cache-live-h618/{O0,N0,N1,O1}/{receipt.json,analysis.json,post-tail.png,restore/restored.json}; traces/stkcop-replay-cache-live-h618/{N0,N1}/attribution.json
related: present-pacing-replay-cache.617.md; present-pacing-replay-owner-live.615.md; present-pacing-encode-census.613.md; present-pacing-owned-run-local.600.md; present-pacing-occlusion-timeline-stateblock.320.md
---

# H618 — live cache components now separate

In both observed late intervals, uniform build/refresh's CPU lower bound
exceeds the upper bounds of shader-layout and hot-state reconstruction.
H617's cold capture did not establish that ordering. Owned-run construction
is larger than each individual cache component, and whole queue batch is
larger than every individual region in this finer partition.

This is **observed ownership, not removal**. The observer increases Replay
CPU, OFF Replay/Encode still overlap, and H613's Encode loop was measured
in another population. The largest default-live operation across all threads
and a correctness-qualified removable CPU amount remain unproved.

## Fixed protocol and provenance

Four private objects derive from H617 with H615's worker request-epoch gate.
The trace expands to 131,072 rows. Broad sparse/draw/binding/non-draw detail
scopes are omitted; their work remains in the record-loop remainder. Core
cache hit/miss, layout, uniform, hot-state, owned-run and compatibility scopes
remain. There are no per-draw CPU syscalls or candidate optimizations.
The canonical core baseline and H602 dormant-scope distinction remain as
documented in H617. No product source, ABI, shared class or lifecycle policy
changes. Formal policy refinement is inapplicable to this observation-only
change; native tests cover observer nesting, failure and identity.

Same-image order is O0,N0,N1,O1, without replacements or extensions. Both arms
retain whole-thread generation-qualified CPU endpoints; only N enables H618
roots/detail. Day/high/1280x720, max-threads2, snd_targets16, Clear OFF, product
perf OFF, H602 detail OFF and `DXMT9_METAL_OCCLUSION=0` match H615. The latter
matches `src/dxmt9/dxmt9_query_policy.hpp`, `metalOcclusionEnabled`'s default;
this does not measure actual GPU occlusion enabled.

Unix image SHA-256:
`12bfb5a9b85f69135af8e53da6e1ae024dce0ad2faefe299552dced07ccfb81f`.

Twelve native, four extra, six detail-switch controls and 32 parser negatives
pass on arm64/x86_64. Process-exit identity tests add nine negatives. Source
derivation, canonical object parity, archive member and install-name checks
pass. Original OFF/ON GPU tests preserve exact source/output digest
`1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`,
670 balanced wrappers, seven mapped sources and the 9/5/4/5 query transcript.
Two ordinary queue controls verify inactive observer children.

All four live runs reach seven active spatial knots, exit naturally and restore
14 shared files each. Only catalogue `missing_capture` is waived. Computer-use
is limited to startup focus and post-tail screenshots; all four stored images
were individually inspected and appear coherent, not exact whole-game proofs.
No builds, tests or parsers run during selected intervals. A separate verifier
decodes 112,591 raw rows without importing the observer parser, reproduces
phase/boundary arithmetic, checks 56 restorations and confirms no Wine remains.

## Whole-thread CPU and observation burden

Milliseconds per completed camera-episode frame. Concurrent threads must not
be summed as frame latency. Episode FPS is not the whole Day benchmark value.

| Run | Frames | Replay CPU/frame | Encode CPU/frame | Episode FPS |
|---|---:|---:|---:|---:|
| O0 | 132 | 28.770–28.888 | 28.574–28.816 | 19.395–19.405 |
| N0 | 111 | 33.836–33.996 | 30.552–30.893 | 16.493–16.499 |
| N1 | 119 | 33.205–33.359 | 30.650–30.876 | 17.842–17.849 |
| O1 | 110 | 31.713–31.860 | 31.418–31.727 | 16.065–16.070 |

Finite ON-minus-OFF Replay contrasts are +1.346–5.226ms/frame. OFF Replay's
envelope spans 3.090ms/frame. Encode contrasts are −1.175..+2.319 and camera
time −6.224..+9.100ms/frame. These are not calibrated observer taxes or a
stable throughput treatment. The inherited frozen rejection screen—both ON
Replay upper bounds exceeding every OFF upper bound by more than 5%—does
not trigger. Passing that screen is not significance or zero observer cost.
Do not use the ON Replay lead to declare a permanent default bottleneck.

## Live disjoint owners

Both traces have zero loss/fault/orphan or endpoint-straddling roots. N0 has
55,333 roots (28,163 raw, 27,170 StateBlock); N1 has 57,258 (29,165 raw,
28,093 StateBlock). Root CPU covers 87.004–94.765% / 87.262–94.945% of the
whole interior Replay thread. Uncovered CPU is retained, not labelled idle.

Camera-boundary-adjusted CPU bounds, milliseconds per frame:

| Region | N0 | N1 |
|---|---:|---:|
| Whole queue batch | 7.103–9.714 | 7.077–9.658 |
| Owned-run construction | 4.286–6.071 | 4.323–6.104 |
| Record-loop remainder | 3.318–5.271 | 3.217–5.039 |
| Uniform build/refresh | 1.769–3.693 | 1.737–3.570 |
| Shader-layout rebuild | 0.416–1.564 | 0.462–1.558 |
| Hot-state rebuild | 0.050–1.014 | 0.075–1.009 |
| Cache-miss remainder | 0.023–0.859 | 0.044–0.862 |
| Cache-hit remainder | 0.014–0.307 | 0.047–0.330 |
| Compatibility preparation remainder | 0.351–1.323 | 0.331–1.281 |
| Planned routing remainder | 0.468–2.325 | 0.482–2.296 |
| StateBlock application | 0.002–1.200 | 0.022–1.189 |

The uniform scope includes actual full rebuild/constant refresh and final
fixed-payload/hash/validity settlement, not just memcpy or hashing. Shader
layout measures the actual `!reuseShaderLayout` branch. Owned-run includes
local construction, vector insertion, identity assignment and local destruction.
Whole queue batch includes marking and Slot work, not only mutex contention.

| Actual calls | N0 | N1 |
|---|---:|---:|
| Compatibility preparation | 453,674 | 479,133 |
| Owned-run construction | 382,273 | 402,698 |
| Cache hit / miss | 41,202 / 91,099 | 44,506 / 95,107 |
| Shader-layout rebuild | 90,743 | 94,732 |
| Uniform build/refresh | 125,557 | 132,371 |
| Hot-state rebuild | 91,099 | 95,107 |

Changing the partition explains why queue batch now ranks first among
individual regions: H615's large outside-queue loop has been divided into
cache, construction and remainder. Recombining those same exclusive regions
gives 1,854.367–2,160.255ms / 1,944.289–2,265.246ms total interior CPU, still
larger than queue batch. Cache-total union bounds are 545.552–792.014ms /
578.291–826.825ms. Group bounds are recomputed per root, not formed by adding
independent child uncertainty ranges. None of these choices makes an arbitrary
enclosing region the largest removable operation.

## Next discriminant and closed hypotheses

Uniform is now the largest measured cache component, not evidence that another
broad invalidation shortcut is valid. H320 already rejects whole-snapshot
StateBlock reuse; H597–H600's constructor-removal screen remains closed.
The known-RenderState dependency work in
[uniform dependency reuse](../mini-replay-bisection/uniform-dependency-reuse.15.md)
must not be rediscovered as a new optimization.

Current `src/d3d9/core_draw.cpp`,
`cachedBaseDrawStateForSubmissionBatch`, always rebuilds the cached fixed
projection after the full-reuse, constant-only-refresh and full-rebuild arms.
`refreshDrawUniformPayloadShaderConstantsFromState` changes constants and
hash/count metadata, not fixed values. That identifies a distinct source-level
question: how much actual live uniform work is necessary reconstruction/hash,
and how much is unchanged fixed-component rematerialization? H618 does not
separate those branches or price their removal. Check actual-path branch and
compiled-copy evidence before selecting a candidate; preserve full-payload
validity, clipping coordinate semantics and constant-usage identity.

No optimization, default promotion, README FPS update or commit follows from
this attribution. Global default-live dominance and source-matched removable
CPU remain the completion gates.
