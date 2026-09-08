---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Compact first-use wire publication: correct, but uneconomic"
type: evidence
status: measured-runtime-withdrawn
updated: 2026-09-08
source: experiments/output/pe-owner-wire-publication-20260908/analysis
related: present-pacing-pe-retainer-component-cost.288.md; present-pacing-pe-binding-wire-authority.286.md; ../../../specs/d3d9/recorder/requirements.md
---

# Compact first-use wire publication: correct, but uneconomic

H289 withdraws a compact writer fusion after actual-PE component measurements.
Native transaction tests and PE pixel parity pass, but moving handle publication
into each binding visitor adds calls, branches and live state. The separate
matched-header append fixture also gets slower on non-null binding cases.
The existing production path is retained; stronger native rollback/role checks
and R-CORE-REC-7.64 remain. There is no dormant runtime branch or new flag.

## Candidate and semantic boundary

At `b90d584a`, `PeSemanticBatchOwner::writeSparsePlanPayloadQualified` writes
binding rows, then `materializeCanonicalRecord` traverses the prepared identity
witness to write the handle table. Both are in
`src/d3d9/d3d9_pe_semantic_owner.hpp`. The candidate publishes each wire handle
at its first binding occurrence and removes only the compact post-writer loop.
A call-local prefix cursor enforces first-use order and final coverage.
Generic inputs, including generic UP/control records, retain their reference
path; selection is by compact input capability, not producer opcode.

Source qualification, locator validation, current identity/pointer checks,
physical pins and global remap bounds remain. Publication before the committed
frontier needs extra failure cleanup: the candidate revokes the reserved handle
suffix even when `emissionHandleCount_` has not advanced. It preserves earlier
committed handles, refcounts, ordinals and retry bytes. No queue, wake, pass,
GPU-lifetime or wire-ABI policy changes.

The retained native changes add read-only speculative-suffix inspection and
ExactFixed/segmented parity to existing all-seven-family, null/duplicate,
late-retain/source failure, settlement rejection and retry differentials.
They preserve the reference implementation as a regression oracle rather than
making a new mutating prepared/append test seam.

## Evidence and qualification

`baseline.json` and `candidate.json` bind the exact source inputs, runtime patch,
four canonical full builds and five frozen images. Build options match.
The candidate passes eight focused native specs and `dxmt9-verify-tla`:
51 safety, two progress and 109 expected-counterexample configurations.
This is a bounded transaction representation refinement; the existing TLA
suite is regression evidence, not a new proof of first-use wire-byte ordering.

The loaded writer fixture uses genuine matching-header `Owner`, `Prepared` and
lexically qualified source objects. Owner/prepared sizes are 240/2808 bytes in
PE32 and 424/2864 in PE64, unchanged between images. The exact DLL specialization
is bound by executable-section RVA, PE machine and 16-byte entry prefix;
PE32 uses explicit lowered `thiscall`, PE64 its unified calling convention.
Every resolved util thin-archive member, compiler and fixture binary is hashed.
No mirrored private storage or forged append authority is used.

Resources are real C-ABI textures/buffers. A public seed append holds their
physical pins before the timed calls. The probe checks independent payload
schema/remaps, prepared identity bytes, candidate handle entries, unchanged
committed prefix/frontiers and the following sentinel. Final resource, device
and factory cleanup must return zero. `run_d3d9_conformance.py` stages each
frozen image set into Sikarugir `sikarugir-cx-24.0.7`; full image identities are
reconciled before and after each process.

Four additional real-PE default-path pixel processes (baseline/candidate,
PE32/PE64) pass. Each has 24 four-pixel signatures: candidate readbacks match
baseline exactly and pass the independent channel-tolerance oracle. These
192 candidate pixel samples are not an all-family GPU or wild-scene proof.

Exploratory failures are retained, not counted as economy: early fixture builds
had a callback return-type error and an unaccompanied thin-archive copy. The
first baseline smoke rejected the fixture's incorrect null-row oracle.
An explicit unbind has `valid=1` plus `NULL_HANDLE_INDEX`, not `valid=0`.
The corrected final writer build is r5; append build is r7. Neither issue was
a product failure. Parser tests cover 30 negative controls plus valid inputs.

## Linked code

`codegen-report-qualified.json` is the authoritative bounded-disassembly report;
the earlier exploratory `codegen-report.json` is superseded. Bounds use the
next text symbol, with jump-table/data tails and overlap accounted for.

| Linked body | PE32 bytes, before → candidate | PE64 bytes, before → candidate |
|---|---:|---:|
| Compact writer main | 1733 → 1879 | 2110 → 2191 |
| Compact materializer | 474 → 363 | 492 → 392 |

Writer stack subtraction grows 276→304 bytes in PE32 and 536→584 in PE64.
The materializer's stack is unchanged. Binding helper families grow and gain
a publication call. Removing a tight post-loop therefore does not establish
less executed work; static code sizes are not dynamic cycle counts.

## Separate writer and append prices

All numbers below are nanoseconds per call, the arithmetic mean of two
separate process means per image/PE width. Runs are serial, with no concurrent
build or other benchmark. They use QPC elapsed time, not exclusive CPU.

The loaded writer runs seven shapes, 1000 warm calls followed by 1,000,000
timed calls per shape. The interval includes the loop, indirect call and
checksum. **Its baseline excludes the parent handle loop**, so these deltas
cannot be treated as net append deltas.

| Loaded writer shape | PE32 before → candidate | PE64 before → candidate |
|---|---:|---:|
| Empty | 26.33 → 27.37 | 18.03 → 40.72 |
| One texture | 74.52 → 81.99 | 44.29 → 68.54 |
| Two unique textures | 96.77 → 111.17 | 56.67 → 85.15 |
| Two rows, same texture | 97.22 → 109.52 | 56.35 → 84.64 |
| Explicit null texture | 59.97 → 60.52 | 35.22 → 56.80 |
| Two streams | 103.35 → 114.16 | 56.49 → 85.92 |
| Twenty unique textures | 497.27 → 635.18 | 277.46 → 367.33 |

The auxiliary append fixture compiles the matching public
`Owner::tryAppendOwnedRecord` path into an actual PE executable, including
preparation, physical-pin hit checks, materialization and settlement with a
counted accepting callback. **It is not the loaded production parent**, nor
the production PendingDelta/capture callback. This distinction explains why
its deltas must not be subtracted from the loaded-writer table.

After eight warm groups, each shape runs 2000 groups of 64 appends. Reset,
seed, resource acquisition, output/count oracles and emission are outside
each timer. Empty QPC calibration is reported separately and not subtracted.
Final 65-record count, handle count/remap, identity, payload and first/final
group ExactFixed/segmented equality must hold. This prices warm existing pins,
not all-family traffic, retain misses or actual game arrival distributions.

| Matched-header append shape | PE32 before → candidate | PE64 before → candidate |
|---|---:|---:|
| Empty | 167.29 → 166.20 | 114.65 → 113.11 |
| One texture | 262.41 → 266.47 | 165.58 → 170.13 |
| Two unique textures | 322.87 → 331.79 | 195.69 → 203.32 |
| Two rows, same texture | 304.59 → 313.69 | 188.11 → 194.16 |
| Explicit null texture | 223.29 → 224.57 | 148.66 → 146.95 |
| Two streams | 321.48 → 337.13 | 197.93 → 202.72 |
| Twenty unique textures | 1573.63 → 1645.44 | 805.78 → 875.82 |

`comparison-qualified.json` reconciles all 16 long processes, their matching
checksums, receipts, native/TLA and pixel evidence. Two repeats are enough
to reject this candidate conservatively, not to claim a statistical confidence
interval or a measured FPS effect.

## Decision

Keep the separate compact handle loop. Binding append cases cost more in both
widths: the twenty-texture fixture grows 4.56% in PE32 and 8.69% in PE64.
The small empty/null advantages do not establish a weighted production benefit.
The runtime patch is withdrawn; `restored.json` qualifies unchanged sources
and rebuilt images against the baseline. Both `d3d9.dll` files differ only in
their COFF/debug-directory timestamps (all other bytes identical); both bridges and the
unix provider are fully byte-identical. No new GT2 FPS run is justified by
this failed component gate, and README is unchanged.

This rejects the measured out-of-line first-use implementation, not every
possible fusion. An alternative must remove the added per-binding work and
repeat these gates. The other unmeasured candidate is reusing the immediately
preceding private admission's source qualification while preserving the
envelope and fresh writer checks; its O(1) opportunity must also be priced.
Neither is an established whole-PE CPU reduction or README recovery.
