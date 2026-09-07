---
domain: present-pacing
workload: 3DMark05 GT2
title: "PE active-section dispatch and closed append effects"
type: evidence
status: native-codegen-and-single-run
updated: 2026-09-07
source: experiments/output/pe-append-effects-20260907/analysis; experiments/output/app-d3d9-3dmark05-pe-append-effects-b83d59e1-gt2-r1-20260907; experiments/output/app-d3d9-3dmark05-pe-sparse-work-77518738-gt2-r1-20260907
related: specs/d3d9/recorder/requirements.md; present-pacing-pe-sparse-work.250.md
---

# PE append effects

## Scope and contracts

Base: `4a0ee6bf`, with H250 runtime `77518738`. R-CORE-REC-7.48–7.51
refine existing synchronous PE transactions. Record cadence, wire ABI,
segmented selection, source identity, queue scheduling and provider defaults
are unchanged. Implementation commit: `b83d59e1`; its post-commit GT2 spot
check is recorded below separately from the native and linked evidence.

1. Compact layout gates absent categories at the call site. The writer
   dispatches the existing prepared section inventory instead of invoking
   every category emitter. Canonical order, padding, sizes and malformed
   absent-constant validation remain unchanged. The generic layout and
   canonical builder remain independent oracles; no active-mask cache is added.
2. Standalone constants use `PeSemanticConstantSettlement`, a concrete
   operation referencing only the constant shadows, read-only pending state
   and copied pending ticket. It validates the existing settlement contract
   and clears one represented interval. It has no owner access, arbitrary
   callback, virtual dispatch or mutable reentrancy flag. The unobserved
   owner transaction checkpoints four scalar frontiers, not 17 sparse quotas
   and six pin frontiers. Arbitrary callbacks and observed admission retain
   the full checkpoint. Production `appendSemanticRecord` calls this entry.
3. Scoped ledger overrides use zero-initialized `constinit` TLS slots, separate
   from the cached production pair. This removes dynamic TLS initialization
   checks, not all diagnostic overhead: TLS lookup, override/null branches
   and the cached production initialization guard remain. No per-record
   configuration lookup or clock is introduced on the disabled branch.
4. Recorder bookkeeping reads a fresh `PeSemanticEmissionFrontier` containing
   current record/handle/payload counts. Admission already proved the exact
   layout before publishing these private counters. Failed admission restores
   them; reset and settlement clear them. Copied counts confer no reusable
   admission authority. There is no persistent receipt, generation counter
   or recorder cache. Exact emission still validates layout, and the checked
   `emissionMetrics` query remains a cold reference. Initial draw destination
   finalization and cadence-byte thresholds are not removed.

## Verification boundaries

All four canonical full builds pass. Native PE/audit tests pass 30/30 and
x86_64 focused tests pass 9/9. Source-payload and owner tests pass in fresh
ledger-OFF and ledger-ON processes; constant flush tests pass at N=1/127.
The performance-document source audit and whitespace check pass. Existing
warnings remain for an unused variable in untouched `device_c_marshal.cpp`
and duplicate native libc++ link arguments.

Native coverage binds the real helpers and owner entry to the preserved
reference: all six constant families, A/B/A, stale ticket, wrong range,
malformed extent, capacity denial, failed settlement, same-ordinal retry,
prefix bytes and balanced pins. Both real owner observer configurations are
tested. Compile-time API checks reject an arbitrary callback at the narrowed
entry. A separate settlement fixture tests selected-only dirty clearing,
unselected state and empty tickets using legal dirty membership states.

Fresh frontier tests cover unavailable/empty owners, success, failed callback,
rollback, reset, settlement, A/B/A, CapacityPre and retry against checked
metrics and canonical bytes. Ledger tests exercise cold/warm lookups,
same-owner and cross-owner nesting, restoration and thread isolation in fresh
enabled/disabled processes. Existing compact all-family, source freshness,
independent quota and retain-fault tests remain in the owner suite.

The existing TLA suite is unchanged and passes; no new interleaving or queue
transition is introduced. Native differential evidence owns these local
effect refinements, not a new end-to-end proof of the private COM device.
Whole-scene correctness and performance remain separate wild gates.

## Actual linked code

The production constant branch reaches the new closed entry in both PE DLLs.
Its inner transaction frame reserves 204 → 116 bytes on PE32 and 344 → 152
bytes on PE64. The old sparse/pin checkpoint loads and stores disappear from
that body. These are individual frame reservations, not total call-chain
stack usage or CPU timings; helper outlining still exists.

| Linked body | PE32 static instructions, base → candidate | PE64 static instructions, base → candidate |
|---|---:|---:|
| Compact layout preparation | 508 → 602 | 529 → 632 |
| Compact writer entry | 397 → 451 | 379 → 396 |
| Recorder boundary bookkeeping | 213 → 178 | 193 → 178 |

The first two bodies grow because call-site gates and active-section dispatch
replace unconditional calls to helpers that previously returned early. The
intended saving is skipped empty work, not a smaller function. Static call
counts include cold branches and do not count runtime invocations. The normal
constant ledger path no longer calls `___tls_init`; Windows TLS runtime
support elsewhere in the DLL is not removed. Recorder frontier consumers no
longer reconstruct unused exact wire sizes.

PE32 exports and vtable/RTTI archive owners match baseline; so do PE64's.
PE32 DLL size is 1,870,848 → 1,868,800 bytes; PE64 stays 1,734,144 bytes.
Both bridge DLLs are byte-identical. The Unix provider changes because it also
includes the shared ledger header; it is not claimed byte-identical.

- PE32 SHA-256: `67f7688cab6a4d3b14d7f1e1112e6d21352cabbbced66088daca56593f4119a0`
- PE64 SHA-256: `5cdb262c24e724ac3c5e1aafbb53f74cd2e18d013d699c2b6eb074ef403bcaa0`
- Unix SHA-256: `e4543457d6992f1a2a8a088a590e75170360d8938e4c4aeda582ab3b1da4a09a`

The analysis directory preserves `baseline-linked.json`,
`pe32-intermediate-linked.json`, `candidate-linked.json`, their linked assembly,
`codegen-summary.txt`, build/test logs and `tla.log`. The intermediate receipt
has an old PE64 build and is not integrated evidence. Failed iterations are
retained: initial missing SDK setup, querying a new Meson target before
regeneration, fixture pin accounting and prematurely sealing the canonical
oracle, and the source assertion tied to the previous TLS representation.
The initially proposed token-plus-arbitrary-callback interface was rejected
during main/independent review, not accepted as proof of closed effects.

## Post-commit GT2 spot check

The clean `b83d59e1` worktree was checked through all three canonical staging
builds, then run once through `run_experiment.py`. The artifact is
`app-d3d9-3dmark05-pe-append-effects-b83d59e1-gt2-r1-20260907`; the reference is
H250's `app-d3d9-3dmark05-pe-sparse-work-77518738-gt2-r1-20260907`.

Both use named GT2, 1024x768, Sikarugir `sikarugir-cx-24.0.7`, perf/Warn,
validation OFF, frame sampling ON, and PE decimation/recorder stats/semantic
phase observation OFF. Copy/materialization observation is OFF; no GPU trace
or PE sampler was run. Actual staged PE32/PE64/Unix hashes match the linked
receipts above. The bridge DLLs match the reference, but the shared ledger
header changes the Unix provider as well as the PE binaries.

| Metric | H250 `77518738` | `b83d59e1` |
|---|---:|---:|
| Positive-wall sampled FPS | 28.846 | 29.034 |
| Positive-wall sample count | 1,861 | 1,876 |
| Sampled wall time (ms) | 64,514.336 | 64,614.120 |
| Encoded Presents | 1,862 | 1,877 |
| Command buffers / Present | 3.99946 | 3.99947 |
| Render passes / Present | 15.77712 | 15.77890 |
| Draw calls / Present | 1,683.132 | 1,681.425 |
| GPU command-buffer time / Present (ms) | 2.050 | 2.032 |
| Chunk rejects / GPU errors / no-pipeline skips | 0 / 0 / 0 | 0 / 0 / 0 |

FPS is positive-wall sample count divided by summed frame wall time, not
process elapsed FPS or a decoded `.3dr` score. The observed change is +0.650%.
The run passes with exit 0, no timeout, no harness failures and zero pipeline
draw/function/library/PSO failures. `actual.png` shows a coherent forest scene
at overlay time 1:00.51; it is one frame, not whole-scene flicker or
pixel-equivalence evidence. The runner recovered a fresh 3,346-byte `.3dr`,
SHA-256 `57a6b792c3931b62157d539ad55bbd95a90565e9ae2749e4c6e15fff85057592`.
`result.json`, `3dmark05-perf-frames.csv`, the generated summary and the
analysis `wild-run-receipt.md` preserve the run and denominator.

No other Wine or build task was present at preflight. One live snapshot
showed WindowServer at 24.2% CPU and an Orca renderer at 18.6%, but not the
high `duetexpertd`/`mds_stores` load seen in H250's snapshot. These are isolated
observations, not continuous CPU traces or causal attribution. Background
conditions therefore differ. This non-interleaved, one-run comparison does
not establish an FPS gain or qualified neutrality. CB/pass shape is close;
no strict locality promotion follows from these ratios.

## Limits and next gate

Matched repeatability, whole-scene correctness and current PE CPU attribution
remain open. Section presence frequency, helper outlining, branch cost and
remaining ledger lookup overhead require actual CPU attribution. This spot
check and historical PE phase times cannot price the individual changes or
establish completion of PE optimization. Provider defaults are unchanged;
CPU-ready Tape promotion is outside this change.
