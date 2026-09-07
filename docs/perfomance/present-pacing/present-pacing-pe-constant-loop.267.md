---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Call-local constant backing borrows and one dirty reduction"
type: evidence
status: verified-awaiting-runtime-economy
updated: 2026-09-08
source: experiments/output/pe-constant-loop-borrows-20260908/analysis; experiments/output/pe-inline-constant-pixels-20260908/analysis/loop-borrows-{x86,x64}-off-final
related: present-pacing-inline-constant-economy.266.md; present-pacing-pe-compact-binding-validation.262.md; present-pacing-pe-constant-copy.257.md
---

# One fixed-width Live loop reduction

## Decision and mechanism

Base `a6700464` retains H262's measured constant consumer: 66/61 unambiguous
stopped-PC samples in `ValidatedConstWrite<16>::applyNonEmpty` for 05/06 GT2.
Those are exclusive symbol buckets, not CPU milliseconds or copy-only costs.
The current PE32 image still reloads vector backing pointers after a changed
element and maintains both `changed` and `firstChanged` for the same outcome.

The Live kernel now borrows the two backing pointers after both materializing
resizes, for this call only. No resize, callback or publication occurs inside
the loop. `firstChanged == count` represents no changes because every visited
index is smaller than `count`; a second boolean is unnecessary. R-CORE-REC-7.40
and the existing compile-time state-write projection remain the policy owner.
Values, zero extension, dirty membership/range, input alignment, exact
self-alias, Recording, exception behavior and wire contracts are unchanged.
There is no new cache, field, diagnostic gate or PE/Unix crossing.

This is a local value refinement, not a new concurrency transition. The
independent old reducer is the differential oracle; no new TLA or universal
Wine/Metal proof is claimed. PE remains x86/x64 C++ under Wine/Rosetta: smaller
linked code is not evidence of native ARM64 PE execution.

The read-only retention audit did not justify deleting another pin check.
Record-unique identity admission and current owner-prefix validation answer
different questions. Capture-only pending-lease traversal is not a measured
steady-state GT2 hotspot and is not changed here.

## Prototype rejection and linked evidence

Pointer caching alone increased PE32's 16-byte kernel from 215 to 218 decoded
instructions and its prologue stack subtraction from 12 to 16 bytes. It was
not retained as the final candidate. Adding the sentinel reduction gives:

| PE / register bytes | Body bytes before / after | Instructions before / after | Prologue stack subtraction before / after |
|---|---:|---:|---:|
| 32 / 16 | 614 / 594 | 215 / 208 | 12 / 8 |
| 32 / 4 | 561 / 504 | 199 / 186 | 12 / 8 |
| 64 / 16 | 588 / 561 | 185 / 176 | 56 / 40 |
| 64 / 4 | 546 / 504 | 171 / 162 | 40 / 40 |

The prologue values exclude saved registers and return addresses. Full-symbol
inspection includes directly reachable blocks after the first textual return;
these totals are not dynamic instructions per setter. Existing cold growth
calls remain (11/10 direct calls in each PE32/PE64 body); the element loop adds
no call. The changed 16-byte source still reloads after equality, so that
separate H257 residual is not claimed removed.

`audit_constants.py`, `linked-constants.json` and frozen baseline, pointer-only
and final candidate images preserve the comparison. `audit_public.py` and
`public-linked.json` pin all twelve manifest hot-body metrics per width to
zero delta, equal export pairs and the declared vtable/RTTI/key-function
owners. The Unix provider remains byte-identical to H266.

## Verification

The new native Cartesian matrix independently seeds 784 rows per width:
16 changed masks for empty growth, plus 16 prior-dirty masks times 16 changed
masks for nonzero-start growth and both warm start positions. Both widths
give 1,568 rows and 3,136 actual factory/Live applications including repeats.
Every application compares exact bytes, extents and dirty effects with
`oldConstTransition`; source immutability and warm pointer stability are
checked. Growth cases do not assume stable addresses across resizing.
Existing null/empty/cap/overflow, unaligned, signed-zero/NaN-bit, self-alias
and A/B/A cases remain. This does not add allocation-fault or guard-page proof.

Canonical native, PE32, PE64 and x86_64 provider builds pass. Focused native
4/4 and x86_64 5/5 pass, including actual record replay and the provider
install-name audit. No whole-suite pass is inferred from these focused runs.
The final real PE32/PE64 pixel fixtures pass after the native gates: 144
sampled pixels, 18 signatures per process, standalone VS/PS counts 19/19,
and device Release zero. Both use inline OFF and worker replay; exact staged
image checks pass. These are bounded pixel receipts, not whole-scene evidence.
The preliminary PE32 run is retained
as `loop-borrows-x86-off-r1` but is not the final post-native receipt.

Runtime economy collection remains the next gate. H266 is the preceding
matched-image evidence, not a fresh counterbalanced baseline for this change.
No CPU saving, repeatable README recovery or inline-default promotion follows
from the linked-code reduction; inline remains OFF.
