---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "PE three-register equality candidate"
type: evidence
status: correctness-qualified-economy-pending
updated: 2026-09-08
source: experiments/output/pe-three-register-equality-20260908/analysis
related: present-pacing-pe-vs-constant-call-shape.278.md; present-pacing-pe-held-constant-value.269.md
---

# PE three-register equality candidate

## Boundary and decision gate

H278 measures three-register calls as 85.05%/79.73% of live VS-F calls in
05/06 GT2; 87.89%/93.05% of those are already equal. H279 tests one bounded
shortcut in `ValidatedConstWrite<16>::applyNonEmpty`: after both value and
dirty-store materializations, compare exactly 48 bytes using unaligned-safe
integer loads and XOR/OR reduction. An equal range returns without entering
the register loop. Prior dirty membership/bounds remain untouched. Changed
ranges retain the original loop; other lengths retain their original update
semantics. Four-byte bool handling is unchanged.

The specialization applies to all 16-byte register families, not just VS-F.
It adds no storage, persistent equality cache, ABI field or runtime flag.
Validation, allocation failure, Recording routing and lifetime policy remain
unchanged. The measured workload population selects the candidate, but does
not prove that its precomparison is cheaper. Mismatching three-register calls
pay both the precomparison and the original loop.

## Bounded refinement and actual pixels

The production fixed-width operation is compared with the independent serial
reference for 64 prior dirty masks, eight changed masks and 16 unaligned input
offsets: 8,192 new combinations. Nonzero start, outside-range bytes/dirty
obligations, exact bits including NaN/signed-zero patterns, source immutability,
all-zero/nonzero growth, stable warm storage and exact self-alias are covered.
Middle-only and last-only A-A-B-A sequences preserve earlier dirty work. Old
matrix and guard-page tests remain enabled. Arbitrary partially overlapping
caller/private-shadow storage is not a new supported API contract.

Native arm64 passes four focused suites; native x86_64 passes those four and
the install-name audit. This local deterministic shadow refinement uses the
production-bound exhaustive native layer; it introduces no interleaving,
queue or lifetime transition requiring a new TLA model. These host tests do
not establish native arm64 PE execution.

The actual PE conformance fixture adds six three-register VS-F/PS-F rows,
each making public A-A-B-A setter calls and four ordinary VB draws. Separate
unchanged, middle-only and last-only rows make stale constants observable.
Both PE widths pass using the ordinary default path with inline constants and
decimated diagnostics OFF. Frozen H278 baseline and candidate produce exactly
matching 96 sampled pixels per width (72 old, 24 new), bound to the actual
staged images in `pixel-differential.json`. The fixture's literal tolerance
remains three channel units: one VS last-only red channel differs by one unit
from its literal in both images. This is exact image-to-image agreement, not
exact agreement with every literal or whole-workload pixel proof.

The first baseline x86 attempt stopped before launch because the frozen DLL
layout lacked builtin postprocess stamps. The actual builtin postprocessor
was run before adding stamps; full DLL hashes remained identical. Only the
qualified `baseline-pixels-x86-r2` attempt is used.

## Linked PE cost and rejected variants

An initial 48-byte `memcmp` emitted an external call on PE32 and was rejected
before Wine. Short-circuit element compares added more branches. Boolean-AND
and the chosen word-XOR form produce the same inspected reduction shape.
Intermediate DLLs, source patches and full linked-symbol reports are retained
under the analysis root.

| Complete 16-byte kernel | H278 bytes / instructions | Candidate bytes / instructions |
|---|---:|---:|
| PE32 | 594 / 208 | 660 / 224 |
| PE64 | 561 / 176 | 655 / 194 |

No new equality/copy library call is introduced. Public setter metrics,
exports, TU ownership and the four-byte kernel remain unchanged. Local stack
subtraction stays 8/40 bytes, **but that is not a no-spill proof**: PE32's
fallback loop now reloads the source argument and compares a stack-held count
per iteration. The equal-three path returns before that loop. Both bridges
and the Unix provider remain byte-identical. The actual GT2 programs execute
PE32 under Sikarugir, not arm64 PE.

## Economy evidence

Pending fresh sampler-OFF, detailed-observer-OFF 05/06 GT2 CPU/FPS comparisons
against the frozen H278 images. `run_economy.py` records all five full image
hashes, exact recipes, scene frame intervals and two-second process CPU
endpoints. CPU is process-inclusive, not PE-exclusive. The candidate is not
accepted as a performance improvement by code size, population or pixel
evidence; README remains unchanged.
