---
domain: present-pacing
workload: serialized occlusion query burst
title: "H418: Actual repeated ClosePass no-ops on identical serialized input"
type: evidence
status: same-input-mechanism-validated-default-off
updated: 2026-09-30
source: traces/query-ranges-h418/{native-initial.log,native-final.log,native-final-detail.log,x64-final.log,tla.log}; tests/scripts/test_query_release_burst.py
related: present-pacing-session-release-cost.417.md; ../../../specs/backend/query-scope-ranges.md
---

# H418: Repeated pass-close transport reaches an already closed pass

Base `c7499fad` commits H417. This follow-up changes observation and tests only;
it does not skip production releases or change the default-OFF range policy.

## Identical-input oracle

The serialized fixture publishes a real older draw without submitting it, then
imports Draw / BEGIN / Draw / END / Draw plus sixteen empty query reissues.
All eight range OFF/ON × replay inline/offload × contiguous/segmented cases
run with Metal API validation. The wrapper requires 34 complete caller releases,
no clipped/stopped/terminal caller groups, actual helper outcomes, exact 1,024
samples for the main query, zero for the final empty reissue and green pixels.
Existing Pending, publication, reset and reissue gates remain active.

| Mode | Actual helper outcome population | Retained same-prefix repeats |
|---|---|---:|
| Range OFF | 2 Closed, 31 NoActivePass, 1 no-session | 0 |
| Range ON | 1 Closed, 33 NoActivePass | 33 |

The observed OFF population is descriptive; the ON wrapper additionally gates
every repeated result as `NoActivePass`. Synthetic trace Presents delimit the
fixture, not game frames. No timing or FPS comparison is made. This demonstrates
idempotent repeated helper calls, not permission to bypass FIFO/raw progress.

## Conditional admission, not production authority

`ClosedPrefixWitness` requires matching queue, epoch, work generation, exact
publication, quiescence, nonterminal state, retained policy and the adjacent
current FIFO head. Native coverage checks 2,048 invalidation combinations plus
pre-ack, invalid-outcome, wrong-coverage, duplicate-ack and submission-action
rejection. These are predicate tests, not a complete interleaving proof.

The transport records the acknowledged ordinal, not its full prior snapshot.
The candidate therefore trusts the supplied snapshot, helper outcome and
coordinator context. It is unused in production. Before integration, the queue
must own and authenticate those values and invalidate them on every possible
encoder mutation/reset/publication. Selected sequence coverage is insufficient
because it can precede actual execution. FLUSH and completion must stay separate.

## Validation and limits

The first focused run passed the sixteen GPU entries and release algebra, but
the existing lifecycle fixture failed a diagnostic activation-counter assertion.
Its command-order assertions had passed; it omitted the opt-in
`DXMT9_FRAMEGRAPH_RETURN_DIAGNOSTICS=1`. The Meson fixture now sets that flag
explicitly. Production diagnostics remain disabled by default.

Final native results pass 54/54 (three repetitions); x86_64 results pass 19/19
including the install-name audit. Python analyzer tests pass 40/40.
The stronger first-`Closed` gate also passes 24/24 repeated burst tests.
The unchanged TLA prerequisite suite exits successfully (65 safety, five
progress and 157 expected counterexample configurations). Results are
retained under `traces/query-ranges-h418/`. No new TLA refinement is claimed for
receipt reuse. No Wine wild run, performance promotion or production ack elision
is part of H418. H416's negative performance verdict still stands.

The repository-wide performance provenance audit reports eleven missing older
artifacts across three untouched leaves (capacity-ownership-recovery.05,
gpu-position-invariance.04 and query-default-promotion.316). H418's explicit
leaf audit passes; no historical evidence was fabricated or relabelled.
