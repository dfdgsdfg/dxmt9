---
domain: present-pacing
workload: STALKER CoP Day
title: "H353: Exact rejected-source identities and bounded table policy"
type: evidence
status: partial-attribution
updated: 2026-09-23
source: experiments/output/prepared-cardinality-h353/{native-final.log,x86-tests.log,tla.log,h352-backcompat.json,stalker-runner.log,stalker-on.csv,stalker-on.json}; experiments/output/app-d3d9-stkcop-bench-prepared-cardinality-h353-day/{result.json,actual.png,daybenchmark.result}
related: present-pacing-prepared-reuse.352.md; present-pacing-stalker-access-cardinality.347.md
---

# H353: Complete rejected-source cardinality

Parent `1dbf6bd6` plus cardinality instrumentation. STALKER Day/high,
renderer_r2.5, 1280x720, Sikarugir cx24.0.7, perf; the same configuration as
H352 r3, with ordinary preparation observer ON and identity partition. The
bounded window remains encoded Presents 2450–2470. No builds/tests ran during
measurement. Unix artifact SHA-256 prefix `0435a5bb456520c1`; all five loaded
hashes are recorded in the runner result.

Runner PASS, return code 0, no timeout/capture error, 225.210s elapsed and zero
GPU command-buffer errors. The 150s screenshot shows the actual scene without
obvious black/repeated geometry, not a pixel-exact or whole-run flicker oracle.
Native benchmark average is 21.545542 FPS, descriptive only: no matched-input
A/B or speedup claim.

## What was counted

The existing builder still stops at capacity. Only capacity-rejected sources
receive a separate full-source pass under the diagnostic gate and queue mutex.
`visitPreparedEncodeShadow` is shared by both paths: the collector uses the same
draw read/write extraction and synchronous surface-to-texture canonicalization,
but gathers full-width handles and sorts/uniques without the 256-identity cap.
Access occurrences retain read/write roles in the builder; only dictionary
membership is deduplicated. Failed measurements expose no partial count.

This measures the projected draw-resource union, not an independent proof of
all source semantics. Unresolved rows are explicitly counted. In this sample
all 21 rejected sources have **zero unresolved rows**, so no missing-row caveat
applies within that projection. These live sources are not the same frozen
input as H347's per-pass measurement; their cardinalities need not match.

## Results

The trace closes with zero overflow/incomplete writers. It contains 1,205
qualified sources: 1,142 query skips, 21 no-work skips, 21 structural successes
and 21 capacity rejections. All 21 rejections have a complete exact transcript.

| Rejected-source dimension | Minimum | Median | Maximum | Sum over 21 sources |
|---|---:|---:|---:|---:|
| Canonical read/write identities | **412** | **417** | **429** | 8,787 |
| Access occurrences | 2,357 | 2,384 | 2,477 | 50,477 |
| Commands visited | 263 | 267 | 275 | 5,622 |
| Unresolved rows | 0 | 0 | 0 | 0 |

Source identity, sequence and every individual count are retained in
`stalker-on.json`. Commands and accesses remain far below 1024 and 8192;
ranges cannot exceed the 275 commands. Thus the 256-identity dimension alone
explains these capacity rejections. Within-source repeated identity occurrences
are 82.59% (`1 - 8787/50477`), not 82.59% removable semantic work.

Full cardinality observation costs 3.664 ms total (0.1745 ms/source), included
in 15.225 ms total preparation-observer CPU. It allocates diagnostic scratch,
re-extracts accesses and sorts; it is not a proposed production count pass or
the cost of a future indexed builder. Do not compare inclusive total directly
to H352 without separating this new interval and differing source populations.

## Policy decision (target, not enabled)

1. Use **512 source dictionary entries** for the next bounded implementation.
   This covers all measured sources with 83 entries above the observed maximum,
   not a guarantee for other workloads. Exceeding the cap must reject the
   unpublished plan atomically, retaining serial execution.
2. Keep full 64-bit canonical identities in a dense immutable dictionary and
   `uint16_t` access indices. Preserve first-occurrence index assignment and
   per-command read/write occurrences. Never deduplicate by low handle bits.
3. Replace per-occurrence linear interning with **1024 builder-only open-address
   slots**, storing dictionary index+1 in `uint16_t` (zero is empty), full-key
   equality on collision, and bounded probing. Load is at most 50%. The lookup
   index is construction scratch, not part of the immutable Encode payload.
   Hash collisions must affect cost only, never identity or truncation.
4. Dictionary storage is 4 KiB at 512 versus 2 KiB at 256; builder lookup adds
   2 KiB scratch. Do not double the 1024-command, 8192-access or 1024-range
   capacities. Preserve workspace reuse; separate/right-size those other
   dimensions only with a measured cost justification and unchanged rollback.
5. Prepared source admission and the current pass/certificate 256-entry limits
   are different contracts. Increasing the former does not qualify parallel
   execution or fix the latter. Shared-table consumer/refinement integration,
   alias lifetime, independent Encode differential and GPU exact tests remain
   required before production consumption.

Required next tests: reference-set differential at 256/257/512/513 identities,
adversarial collisions, generation/alias changes, stale scratch reuse,
every capacity dimension and failed-prefix rollback. Measure indexed versus
linear construction on identical source data before claiming CPU savings.
The current code retains its original bounds and linear interning; only the
measurement and this policy decision are implemented in this step.

## Verification

Native plan/shadow/supply tests 3/3, x86_64 plan/shadow/install-name tests 3/3,
TLA regression (198.74s), Unix and both PE canonical builds pass. Native pins
include a 600-command source with 300 full-generation canonical identities,
aliases, repeated reads and read/write overlap: 1,200 access occurrences,
one unresolved Present, while the bounded builder rejects at 256. Resolver
failure, diagnostic allocation-exception handling, malformed source range and
clipped/duplicate/partial cardinality transcripts are covered. Historical H352
traces still parse without inventing missing cardinality data. This read-only
instrumentation does not alter publication, lifetime or execution transitions;
TLA is regression evidence, not proof of the future table policy.
