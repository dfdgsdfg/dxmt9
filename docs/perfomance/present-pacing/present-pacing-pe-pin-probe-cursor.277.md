---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "PE pin-index cyclic probing"
type: evidence
status: implementation-qualified-runtime-economy-open
updated: 2026-09-08
source: experiments/output/pe-retention-miss-prefix-20260908/analysis
related: present-pacing-pe-post-lifetime-profile.276.md; present-pacing-pe-selected-masks.273.md
---

# PE pin-index cyclic probing

## Decision and scope

`PeSemanticBatchOwner::findPinIndex` and `insertPinIndex` now compute the
initial hash remainder once, then advance a bounded cyclic cursor. The old
`(start + probe) % N` recomputed a remainder on every probe, including the
first one. The production owner has 256 pins per physical family and an index
capacity of 1,025, not 4,097. No table size, hash, identity, ownership, wire,
transport, threading or feature default changes.

H276 identifies retention admission as an observed PE symbol, not a dominant
CPU cost. This increment removes actual compiled work; it does not infer
millisecond savings from stopped-PC counts or static instruction counts.

The first candidate returned the miss insertion prefix from `checkedPinHit`
instead of reading the family count again at its caller. Both complete linked
PE `.text` sections remained byte-identical to baseline. The compiler already
removed that source-level duplication, so the candidate was rejected before
Wine testing and its runtime edit was reverted. Its frozen images and section
hashes remain in the analysis root.

## Semantic refinement

For the physically bounded index, the invariant at probe `p` is
`cursor == (start + p) % N`, with `0 <= cursor < N`. Advancing from `N-1` to
zero and otherwise incrementing preserves that relation. The independent
`probe < N` bound still terminates a full current-generation table miss.
Lookup stops at exactly the same stale-generation or matching-object-ID row;
insertion selects and writes exactly the same first eligible row.

The index remains a locator, not identity authority. `checkedPinHit` still
checks the full kind/generation/object ID, physical pointer and live typed
prefix. Malformed hits do not become new acquisitions. Rollback, rebuild,
stamp wrap, failure settlement and retain/release order are unchanged.
This is the local representation refinement of R-CORE-REC-7.59, not a new
concurrency or resource-lifetime protocol.

`pinIndexCursorWrapDifferential` invokes the actual private production lookup
and insertion functions through the existing native friend seam, comparing
them to independent modulo-based reference loops. It covers all starting
buckets for table sizes 1, 3, 5 and 9, plus a three-object wrap-edge collision
chain at production size 1,025. Cases include empty/stale lookup, stale-row
insertion, full collision chains, same-ID overwrite and full-table miss.
Returned slots, untouched miss outputs and all table fields are compared.
These are 19 constructed traces, not exhaustive enumeration of all tables.

`retentionLookupMissUsesPerFamilyPrefix` separately covers six physical
families with A, failed-B settlement, successful B retry, then A again.
Accepted bytes match `CommandChunkBuilder`; rejected prefixes remain exact,
and owner and reference-builder refcounts balance separately. Existing owner
tests retain malformed-hit, alias, capacity, generation-wrap and partial
acquisition coverage. An initial fixture error omitted the comparison
builder's extra retained ref; it was corrected without changing runtime code.

Existing TLA regression passes (149.74 s); it is not a new cursor model or a
proof about Metal pixels. Native bounded differentials supply the new local
model/code binding. No shader, attachment or Metal action changes require a
new GPU-byte fixture for this arithmetic change. Wild pixel and economy
claims remain distinct from these deterministic checks.

Final native owner/views/producer differentials pass 3/3 on both arm64 and
x86_64, without compiler warnings. `build-native-final.log` and
`build-x86_64-builtin-native-final.log` preserve the final runs; the earlier
failed fixture run remains separate.

## Linked PE evidence

Main rebuilt both PE widths from the same configuration and froze complete
images before inspection. The audit includes each entire symbol body through
the next code-symbol boundary, not merely its first textual return.

| Width / admission | Instructions before → after | Code bytes before → after | Local stack subtraction before → after |
|---|---:|---:|---:|
| PE32 compact | 743 → 598 | 2,717 → 2,109 | 32 → 20 |
| PE32 generic | 746 → 599 | 2,716 → 2,109 | 32 → 20 |
| PE64 compact | 753 → 510 | 3,298 → 2,207 | 40 → 0 |
| PE64 generic | 753 → 510 | 3,298 → 2,207 | 40 → 0 |

Stack subtraction excludes saved-register pushes and is not a total stack
depth claim. The new loop uses compare/conditional-move wrapping; reciprocal
multiply work remains for the initial hash reduction but not each probe.
Jump-table dispatch remains, so the audit does not claim complete indirect-CFG
reachability. `baseline-linked.json`, `candidate-linked.json` (rejected
miss-prefix) and `cursor-linked.json` preserve the distinction.

Fresh named GT2 runs and post-change attribution are the next economy gate.
README remains unchanged; no FPS or CPU reduction is claimed at this point.
