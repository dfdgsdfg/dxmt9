---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "PE VS constant call-shape population"
type: evidence
status: measured-population-no-shortcut-promotion
updated: 2026-09-08
source: experiments/output/pe-vs-constant-call-shape-20260908/analysis
related: present-pacing-pe-pin-probe-cursor.277.md; present-pacing-pe-held-constant-value.269.md
---

# PE VS constant call-shape population

## Question and diagnostic boundary

H277 still observes `ValidatedConstWrite<16>::applyNonEmpty` in current PE
samples. Before adding an all-equal range shortcut, distinguish short calls
from long ones and all-equal calls from mixed updates. A whole-range compare
can duplicate the original comparison work on changed ranges; samples alone
do not justify that trade.

The existing `DXMT9_PERF_VS_CONST_SETTER_RANGE` observer summed changed
registers and changed-span width, but discarded the equal/mixed/all-changed
call classification. Its shader-key table can overflow and then loses exact
lengths. Flush rows describe accumulated dirty membership, not current setter
equality; a full changed span can still contain unchanged interior registers.

At `86bca521`, `recordVsConstSetterRange` updates an additive, fixed 256-row
call-shape sidecar in optional `PeDiagnosticsState`, only for Call phase and
behind the existing gate. It consumes the already computed **pre-apply**
changed-register count. StateBlock Recording calls are counted separately,
because their actual destination is recorded state rather than Live shadow.
The classifier does not inspect or modify constant bytes or dirty state.

`[dxmt9-perf-vs-const-call-shape]` reports equal/mixed/all-changed live calls,
Recording calls, live range registers and changed registers by exact count
1–256. Invalid/overlength inputs have explicit count-zero totals. Both the
old and new sidecars emit/reset at successful Present and teardown. No new
environment switch, per-call allocation, hot signature, wire field or
rendering-policy change is introduced. The new sidecar adds 12,320 bytes to
the optional diagnostic owner, not to every production device.

The summary tool emits `3dmark05-perf-vs-const-call-shapes.csv` and labels it
**whole-log call population**, including loading/teardown where logged. It
strictly rejects truncated/malformed rows, invalid uint64 values, inconsistent
range sums and impossible mixed/changed counts. Missing-log fallback retains
the validated CSV. The old range schema is unchanged.

## Deterministic and linked evidence

The actual cold reducer is tested with counts 1/2/4/256, all three live
classes, Recording exclusion, invalid/overflow inputs, interval reset and
no allocations after optional-owner construction. Disabled diagnostics still
allocate no owner. Native diagnostics pass 3/3 on arm64 and 3/3 on x86_64;
the Unix install-name audit also passes. Parser and gate suites pass 2/2,
including the exact normal emitter schema and populated CSV fallback.

The first diagnostic test run exposed an older source pin that still expected
non-atomic device COM refcounts. H275 (`37621df1`) already changed those to
atomic ownership. The test now checks the real relaxed AddRef/acquire-release
Release contract; ordinary child wrappers retain their separate expectation.
No COM runtime change was made in H278.

Main rebuilt both PE widths and the full canonical Unix provider. Frozen
before/after inspection verifies the complete VS-F public setter and both
4/16-byte fixed kernels retain identical normalized instruction streams,
instruction/code sizes, direct-call counts and local stack subtraction.
Public manifest metrics, exports and TU ownership also pass. Each PE DLL
grows by 1,024 bytes from cold diagnostics; both bridges and the Unix provider
remain byte-identical. `linked-default-off.json` preserves complete symbol
bodies, not first-return prefixes. These are static default-disabled checks,
not dynamic CPU savings. No new concurrency transition or GPU action exists
to model; H277's existing TLA run is not presented as a new H278 proof.

## Actual GT2 population

The dedicated runs use Sikarugir cx-24.0.7, 1024x768 (05) and 1280x720 (06),
perf/Info with the heavy VS range gate ON and stopped-PC sampling OFF.
Offload replay stays ON; inline constants, segmented transport, CPU-ready
Tape and index reorder stay OFF. Main runs the scenes sequentially without
overlapping builds or tests. The five actual Wine-staged image identities,
commands, environment and raw hashes are preserved in each recipe and
`populations-qualified.json`.

The 05 run logs 31,097,625 calls: 26,532,095 equal, 29,248 mixed and
4,536,282 all-changed; Recording is zero. Only counts 1, 3 and 4 occur.
Count 3 owns 26,448,386 calls (85.05% of live calls), of which 23,245,605
(87.89%) are all-equal. The new sidecar has no invalid/overflow inputs;
15,802,596 calls overflow the old shader-key table but remain represented
in the new exact-length sidecar. Whole-log call and range-register totals
match the independent old Call rows including overflow; changed-register
totals also match because no Recording calls occur.

06 logs 19,748,579 calls: 17,479,223 equal, 192,321 mixed and 2,077,035
all-changed; again no Recording or invalid/overflow inputs. Only lengths
1/3/4 occur. Count 3 owns 15,746,138 calls, of which 14,652,412 are equal.
The old shader-key table overflows for 8,164,102 Call events; all remain
represented and the independent call/range/changed totals reconcile exactly.

| Range length | 05 live-call share | 05 equal within length | 06 live-call share | 06 equal within length |
|---:|---:|---:|---:|---:|
| 1 | 5.86% | 84.32% | 9.29% | 80.51% |
| 3 | 85.05% | 87.89% | 79.73% | 93.05% |
| 4 | 9.09% | 61.91% | 10.98% | 62.26% |

05 completes 1,558 logged scene frames over 64,666.647 ms, with eight error
counters zero through the final logged frame. Its 4,674 shape rows are
Present reports, not teardown reports. 06 completes 1,042 heavy scene frames
949–1,990 over 64,072.309 ms; 3,126 shape rows are Present reports. Its last
health counter is at 1,980: eight counters are zero through there, but ten
tail frames remain unproven. It finishes the scene then uses the explicitly
allowed result-GUI timeout. Both benchmark processes are terminal. These
are heavy diagnostic runs, not FPS/CPU economy samples or whole-scene pixel
proofs. README is unchanged.

Run directories are
`app-d3d9-3dmark{05,06}-pe-vs-call-shape-86bca521-gt2-diagnostic-r1-20260908`.
The actual PE32 image SHA-256 is
`05b8f65108128c248746c5d85771bfb19573796ce92f5b88d253bc134acab797`;
PE64 is `9e830e46b4923e4dc6db36eaac28e27df6aadd487588ba5c29767163f83da20e`.
Both products exercise PE32 under Wine, not native arm64 PE. Recipes bind
all five full image hashes, and qualification rejects malformed/truncated tagged
rows before reconciling the old and new totals. See `run_population.py`,
`qualify_population.py` and `populations-qualified.json` in the analysis root.

## Next candidate gate

Neither observed population supports a general long-range memcmp
optimization. A bounded **three-register all-equal** shortcut is the narrower
candidate. It must still compare exact bits and preserve zero extension,
prior dirty bounds, self/overlapping-source behavior and allocation failure
semantics. Keep both materializations before stable borrows; retain the
original changed-range path and all non-selected lengths. Do not introduce
a persistent equality cache or skip validation/ownership.

An equal call already avoids stores today. Its population is not an amount
of work that can simply be deleted: equality still has to be established.
Any prototype must beat the existing compiled loop on PE32 as well as PE64
without repeating H269's spill regression, then pass bounded byte/dirty
differentials and fresh sampler-OFF CPU/FPS measurements. No shortcut is
implemented or promoted by this diagnostic stage.
