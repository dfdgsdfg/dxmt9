---
domain: present-pacing
workload: STALKER Day high 1280x720 max-threads2 snd_targets16 late spatial interval
title: "H615 — Replay record loop exceeds queue batch in both live censuses"
type: experiment
status: bounded-attribution
updated: 2026-10-16
source: traces/stkcop-replay-owner-h615/{contract.md,live-contract.md,build.json,preflight-verified.json,comparison.json,verification.json,analysis-correction.md}; traces/stkcop-replay-owner-h615/{O0,N0,N1,O1}/analysis.json; traces/stkcop-replay-owner-h615/{N0,N1}/attribution.json
related: present-pacing-replay-owner-preflight.614.md; present-pacing-encode-census.613.md; present-pacing-queue-partition.603.md
---

# H615 — the largest observed Replay region is outside queue batch

In both live censuses, the record-loop CPU lower bound exceeds the upper
bound of every other individual region, including all uncovered CPU.
The loop excludes nested `CommandQueue::submitDrawRunBatch` time. It owns
46.48–55.36% of the observed Replay thread CPU; queue batch owns22.62–30.38%.
This closes much of the outside-queue attribution gap left by H603.

It does not prove that the entire loop is redundant, removable or globally
larger than Encode's draw loop. Observer-OFF Replay and Encode CPU intervals
overlap in both runs. H613's phase times come from a different population;
do not subtract or rank their absolute values against H615 as a matched test.

## Fixed protocol and qualification

Use H602 default archives with three private observer objects; no product
edits, rendering-policy changes, new cache, altered batching or permanent
staging. H614 first qualifies source identity and nested scopes. H615 admits
actual worker roots only while the endpoint request epoch equals one, after
the start poll and before each end poll. This avoids whole-game collection
and expands the bounded buffer to131072 rows. Boundaries racing a request
remain in whole-thread residual. The inline provider adapter is ungated for
original-input effect tests and is not a live worker CPU oracle.

The same-image fixed order is O0,N0,N1,O1. Both arms retain generation-qualified
whole-thread endpoints at spatial knots1/6. Settings are Day/high/1280x720,
max-threads2, snd_targets16, Clear OFF, product perf OFF and H602 detail OFF.
The inherited comparison lane also fixes `DXMT9_METAL_OCCLUSION=0`; this is
not a measurement of GPU occlusion enabled or a different query policy.
There are no replacement runs, outlier exclusions or population extensions.
Runtime image SHA-256 is
`f124b02dd5df43fa695049069743a34681f8af71d682bdcb858236951225dd47`.

Native controls cover sparse/all selection, overflow, unwind, idempotent
finish, inactive children, epoch gating and identity. Sixteen controls and
32 parser corruptions pass across arm64/x86_64; process-exit qualification
adds nine identity/missing-witness negatives. Byte reversal restores all
three baseline translation units. Archive checks confirm only their objects
changed. Original observer OFF/ON tests preserve exact original source/output
pixels, seven mapped sources, nine query issues, five polls, four results,
five readiness checks and670 balanced wrappers. Ordinary queue controls pass.

All four live runs pass seven active spatial knots, natural game exit0,
post-tail screenshot and14-file restoration. Only the predeclared catalogue
`missing_capture` failure is waived. All four images were individually
inspected and are coherent; they are not exact whole-game pixel proofs.
No builds, tests, profiler or UI run inside the selected intervals. Startup
focus and post-tail screenshots use computer-use. Independent verification
checks113290 raw rows,56 restored files and no remaining Wine process.

## Whole-thread CPU and observer burden

Milliseconds per completed camera-episode frame; Replay/Encode overlap and
must not be summed as frame time. Episode FPS is not the whole Day average.

| Run | Frames | Replay CPU/frame | Encode CPU/frame | Episode FPS |
|---|---:|---:|---:|---:|
| O0 | 119 | 29.373–29.492 | 29.314–29.560 | 17.407–17.414 |
| N0 | 119 | 31.150–31.303 | 30.107–30.439 | 17.452–17.459 |
| N1 | 115 | 31.961–32.113 | 30.042–30.633 | 16.570–16.577 |
| O1 | 121 | 30.285–30.408 | 30.387–30.680 | 17.608–17.613 |

Finite all-pair ON-minus-OFF contrasts are +0.742–2.740ms/frame for Replay,
−0.638..+1.319 for Encode and −0.172..+3.574 for camera time. OFF Replay's
envelope spans1.035ms/frame. Observation changes CPU; these are not calibrated
per-phase taxes and must not be subtracted from each region. Same-image OFF
still contains a private observer skeleton, not a proven zero-cost shipping
observer. The frozen rejection screen—both ON Replay upper bounds exceeding
both OFF upper bounds by more than5%—does not trigger. That finite screen
does not establish significance, exact default-phase costs or zero overhead.

## Source-qualified coverage and disjoint owners

Join process birth, queue owner, lifetime domain, FIFO ordinal and worker tid.
The interior window is `[first.after,last.before]`. All admitted roots are
complete and contained; none straddle the endpoint window. No loss, fault,
or orphan is reported. CPU uncertainty is bounded per root, not inferred
from elapsed time or multiplied from a sparse sample.

| Quantity | N0 | N1 |
|---|---:|---:|
| FIFO roots | 57,532 | 55,758 |
| Raw replay roots | 29,299 | 28,391 |
| StateBlock roots | 28,233 | 27,367 |
| Mutation roots | 0 | 0 |
| Queue batch calls | 32,916 | 31,909 |
| CPU coverage | 86.955–95.098% | 86.941–94.973% |
| Whole interior Replay CPU, ms | 3714.216–3714.564 | 3683.921–3683.950 |
| Uncovered CPU, ms | 182.062–484.557 | 185.194–481.096 |

Disjoint interior CPU totals, milliseconds:

| Owner | N0 | N1 |
|---|---:|---:|
| Record loop, excluding queue batch | 1743.005–2056.028 | 1712.379–2027.660 |
| Queue batch, whole function | 844.392–1126.935 | 833.335–1119.228 |
| Planned routing remainder | 64.809–261.261 | 66.487–265.163 |
| StateBlock application | 11.852–131.658 | 12.132–129.491 |
| Legacy resource marking | 3.333–46.720 | 3.325–49.246 |
| Root settlement remainder | 0.223–38.739 | 0.302–41.529 |
| Resolved replay setup/cleanup remainder | 0.050–13.762 | 0.010–14.133 |
| Raw replay remainder | 0.011–12.539 | 0.015–12.450 |

With explicit camera-boundary uncertainty, the loop is14.582–17.366ms/frame
in N0 and14.817–17.711 in N1. Queue batch is7.031–9.558 /7.174–9.812;
planning remainder is0.480–2.283 /0.505–2.385. These are observed CPU bounds,
not removable budgets. Individual upper bounds share uncertainty and should
not be added as independent measured costs. Uncovered CPU includes pop/item
replacement, outside-root destruction, root observer work and endpoint gaps;
it is not idle waiting.

The first post-exit analyzer had a quadratic crossing-membership expression.
It was interrupted before output, with no active game. A separate linear,
exhaustively equivalent predicate reparses the same bytes; the frozen original
script is preserved. Independent raw-row verification reproduces the result.
No game or measurement was repeated for this analysis correction.

## Next discriminant, not another queue micro-optimization

`src/d3d9/device_c_chunk_replay.cpp` / `replayResolvedChunk` places these
operations inside the dominant record loop:

- record projection, compatibility classification and transaction bookkeeping;
- sparse state/constant application through `DeviceReplaySink`, including
  prior-value journal capture and dirty-state propagation;
- draw preparation: captured binding attachment, state-derived draw fields,
  `submitCompatibilityReplayDrawBatched` or Direct state-to-Slot construction;
- non-draw/ordered-control dispatch and any nested publication not covered by
  the specific `submitDrawRunBatch` scope.

The next bounded split should separate draw preparation/projection from
sparse state application and residual dispatch/transaction work. Qualify it
on the same original input first, explicitly pricing added observation; avoid
unqualified per-draw CPU reads or another broad wild-run search.

Small standalone StateBlock apply time does not exclude its later invalidation
and snapshot-reconstruction costs inside draws. Likewise small legacy marking
does not exclude resource marking inside queue batch. Nothing here establishes
that all locking, retention or state work is unnecessary.

Global default-thread dominance and source-matched removable CPU remain open.
No product optimization, promotion, commit or FPS-improvement claim follows.
