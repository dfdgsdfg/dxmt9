---
domain: present-pacing
workload: STALKER CoP H481 original Normal input, frozen H495 serial replay
title: "Native base vertex exposes repeated VS immediate bytes without changing original readbacks"
type: evidence
status: mechanism-opportunity-not-cpu-saving-or-promotion
updated: 2026-10-11
source: traces/stkcop-volatile-multiplicity-h498/{run.py,run-receipt.json,diagnostic.stdout,diagnostic.stderr,analyze.py,diagnostic-analysis.json,native-base-vertex-run-receipt.json,native-base-vertex.stdout,native-base-vertex.stderr,native-base-vertex-analysis.json,compare.py,compare.json}; traces/stkcop-normal-cost-h486/prefetch-r1.stderr; traces/stkcop-binding-counter-gate-h495/candidate-build.json
related: present-pacing-command-raster-reuse.497.md; present-pacing-encode-callsite.496.md; present-pacing-root-game.477.md
---

# A representation change can expose reuse that a standalone cache cannot

H496/H497 documentation was committed as `2a292855`. H498 changes no runtime,
default selector, build image or Wine installation. It first checks existing
uniform-materialization evidence, then executes two finite original-input
diagnostics: current default and the existing experimental
`DXMT9_USE_NATIVE_METAL_BASE_VERTEX=1` path. Neither is a timing sample.

## Uniform reconstruction is not established as the dominant cost

The anchored, unique H486 `[dxmt9-perf]` row records 3,497 draws and 1,260 full
payload materializations, not one reconstruction per draw. The ledger accounts
for 13,658,400 bytes at 10,840 bytes per payload, with no materialization fallback.

| H486 diagnostic scope | Calls | Accumulated elapsed time |
|---|---:|---:|
| DrawRun command materialization | 551 | 0.246 ms |
| Parameter materialization | 709 | 0.264 ms |
| All payload materialization | 1,260 | 0.511 ms |
| Transient upload | 1,889 | 1.226 ms |
| Draw uniform build | — | 1.373 ms |

These counters use `steady_clock`, despite their `_cpu_ms` names. They include
observation; materialization includes its accounting before the ending clock.
Rounded child values need not sum exactly. Scopes may overlap and must not be
added or subtracted from native thread CPU. These are neither a default-tail
bound nor a measured removable budget.

The `a20c1a12..2a292855` comparison of the materializer and uniform consumers
shows only the later Slot-validation observer hook in
`dxmt9_backend_types.hpp`; materialization itself is unchanged. H486 is useful
for prioritization, not a new HEAD timing measurement. It gives no reason to
assume that a broad full-payload rewrite removes the dominant Encode cost.

The representation chain also depends on route. Direct cbuf builds full VS/PS
structures and uploads selected prefixes; `updateDirtyArgbufRegions` already
builds argument-table cbuf bytes directly into reserved shared storage. It is
incorrect to attribute a stack-plus-copy to both routes. Programmable VS still
needs FFPVS half-pixel, viewport and clip information; its name does not make
the whole payload dispensable.

## Same-input draw/push/issue multiplicity

Both diagnostics authenticate the same H495 executable/native/unix images and
the H481 manifest, use serial Normal replay, disable archives, and load the
intended images as checked by dyld output. Xcode 27 / Metal compiler / Apple M1
Metal 4 checks pass. Positive 180-second watchdogs bound execution. No new
capture, build, profiler, GUI action or pixel normalization is used.

The existing effect trace includes bindings, geometry with zero decoded-vertex
samples, non-alpha and untextured draws. No source/material filter drops events.
The parser joins the full source/pass/encoder/ordinal/command/subdraw identity
of all 3,497 draws to exactly one 80-byte VS push and one actual issued-raster
event. Repeated or missing keys fail analysis; split issues are not silently
coalesced. Each physical owner transition resets the comparison.

| Same original input | Default | Native base vertex |
|---|---:|---:|
| Draws / actual VS pushes / actual issued draws | 3,497 | 3,497 |
| Physical encoder owners | 18 | 18 |
| First push after owner change | 18 | 18 |
| Bytes changed from previous push in owner | 3,313 | 120 |
| Exact 80-byte repetitions in owner | 166 (4.75%) | 3,359 (96.06%) |

Default changed pushes consist of 3,193 base-index-only changes, 111 changes
to base index plus stream stride, and nine stride-only changes. Stream offset,
padding and instance divisors do not change between adjacent pushes here.
The native path moves a supported nonnegative indexed base into the actual
Metal draw argument, leaving shader-side base zero. Its remaining 120 byte
changes are stride changes. It does not itself suppress any push.

All 3,496 indexed geometry pairs have readable index/stream-0 data and a known
volatile binding. After validating identical three-source maps, comparison
finds only `metal_base_vertex` and `volatile_base_vertex` changed, on 3,440
draws; their sums and every other reported geometry field/hash agree. This
is a CPU-Shared geometry envelope, not proof of all GPU resource reads.

Both source and output readbacks exactly match original SHA-256
`f99e419861d3c4127e96d8ab56dca62d56421919a5962084ae4ecd95ee42ce5a`.
Both runs preserve 779 balanced wrappers, 16 surfaces, 12 aliases and all three
source boundaries. Original all-resource content authority remains unknown
and `timing_qualified=0`; one captured frame is not general compatibility.

## Decision and next bounded implementation

Do not add a standalone 80-byte cache on the strength of only 166 repetitions.
The stronger candidate combines the existing native base-vertex route with
an exact-byte, physical-encoder-local VS immediate shadow. In this input it
exposes 3,359 potential elisions, leaving 138 pushes; this is an opportunity
count, **not a measured call reduction, CPU saving or FPS gain**.

Before implementing/promoting it, bind the contract to all slot-5 writers,
encoder reopen/reset, standalone/child isolation and fake/suppressed recorder
semantics. Native eligibility must preserve negative-base, expanded/UP,
instancing, additional streams and zero-stride semantics; do not generalize
the current workload's nonnegative indexed coverage. Existing fragment
volatile shadowing is a lifetime precedent, not a proof for VS.

Then compare default, native-only and native-plus-shadow with focused native
refinement, original readback and frozen finite A/A plus AB/BA CPU gates.
Only an unobserved comparison can price net savings including comparisons and
the changed driver draw argument. No runtime switch is promoted here. The
largest default-tail dxmt9 single-thread owner and mixed Primary attribution
remain unresolved.
