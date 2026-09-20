---
domain: present-pacing
workload: Native parallel pass and Render Tape fixtures
title: "H348: Shared pass identity table with synchronous child index coverage"
type: evidence
status: partial-attribution
updated: 2026-09-20
source: experiments/output/parallel-shared-identity-h348/{native-tests.log,native-final.log,x86-tests.log,install-audit.log,tla.log}
related: present-pacing-stalker-access-cardinality.347.md
---

# H348: Shared pass table, indexed child coverage

Base `20b5dc74` plus the shared-index overlay. The production certificate
validator now treats the snapshot's existing canonical read union as one
immutable table. Each independently resolved child uses private exact bits in
`ParallelPassReadIndices`; the expected union merges bits instead of rebuilding
another identity array. Table lookup compares full 64-bit identities. No
hash-only identity or approximate membership is accepted.

The table remains encode-coordinator owned. It is not yet constructed during
Replay, published in Slots, or serialized in Tape captures. This isolates the
representation change from future work relocation. Ordinary IdentitySerial
without the diagnostic does not construct these proofs; ExplicitParallel
remains opt-in. The pass admission budget remains 256, so H347's 433–458-read
passes still fail closed.

## Ownership and independent checks

Source/storage generation, sequence, replay range, pass-action epoch and
snapshot authority are checked before coverage. Each child still independently
reads source commands and canonicalizes access identities. It maps those facts
into the validated table, rejecting an unknown identity rather than dropping
it. Index coverage must bind to the exact same table object; equal values in
another table are not index authority. The combined children must cover every
table entry, and attachment write-alias checks remain exact.

All table borrows end inside synchronous certificate validation. The returned
certificate still owns its value snapshot, and workers receive no borrowed
index storage. Binding from a temporary table is deleted. No resource pin,
allocation, submission order or completion contract changes.

## Storage result

The native 64-bit test reports:

| Read metadata | Previous | Indexed |
|---|---:|---:|
| One resolved child | 2056 bytes | 48 bytes |
| Sixteen-child array, read metadata only | 32896 bytes | 768 bytes |

This removes 32,128 bytes from that array's read metadata and 2008 bytes from
the expected-union temporary. It is a `sizeof` result, not measured total stack
peak, allocation reduction or CPU/FPS improvement. Pass snapshot/certificate
copies remain. Identity-to-index lookup is still linear; reducing or relocating
canonicalization and lookup is a later independently measured step.

## Evidence and limits

Focused native/provider tests pass 5/5. The production index operations refine
an independent exact-set oracle over all 4096 six-command/two-child traces.
Tests cover missing identities, sticky poison, all write intersections,
cross-table rejection, full-generation equality, every bit-word boundary at
capacity, rebinding and empty coverage. Producer/certificate fixtures retain
wide-read, stale-source and omitted-late-read counterexamples. A separate
returned-certificate test checks value ownership after local borrows end;
the updated native spec also passes under x86_64/Rosetta.
The existing TLA regression passes in 195.71 seconds. The new representation
is checked by the exhaustive native refinement; no new worker interleaving or
resource-lifetime transition is introduced or claimed by that set check.

The existing Render Tape provider CLI test runs two fresh IdentitySerial and
two ExplicitParallel replays through production code, requires exact output
and nonzero selected/children/worker evidence, and passes. Its bounded
128-draw fixture is intentionally output-degenerate: this proves integration,
not a new non-degenerate wide-resource GPU oracle or STALKER qualification.
The Metal environment check passed on Apple M1 with Xcode 27. Native and
x86_64 Unix build paths compile; the Unix install-name audit passes.

Remaining gates: non-degenerate wide-resource exact GPU evidence, retained
proof/copy pricing, Replay-owned alias-stable table publication, and comparable
end-to-end CPU measurements. No wild run or performance promotion is claimed.
