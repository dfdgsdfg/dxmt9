---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Compact descriptor-table materialization economics"
type: evidence
status: rejected
updated: 2026-09-09
source: experiments/output/pe-descriptor-table-H292-20260909/analysis
related: present-pacing-pe-current-hotpath.291.md; present-pacing-pe-private-admission-proof.290.md; present-pacing-pe-wire-publication-fusion.289.md
---

# Compact descriptor-table materialization economics

H292 tests the candidate identified by
[H291](present-pacing-pe-current-hotpath.291.md): reuse the already prepared
compact descriptor table instead of rebuilding its destination offset and
copying one descriptor after each payload visitor. Source-level copy counts
and sampled PCs do not establish savings; actual PE economics decide whether
the candidate is retained. **The unconditional variable-size table copy is
rejected and the complete product-header change is withdrawn.** Small-table
append regressions outweigh the bounded dense-table gains. Only native
coverage, the optional contract, and evidence are retained.

## Reference and safety boundary

The frozen baseline is `1a37e153`, whose product images match H290/H291.
`baseline.json` binds all five images, source headers, build options, and thin
archive members. The candidate changes only the compact overload of
`PeSemanticBatchOwner::writeSparsePlanPayloadQualified` in
`src/d3d9/d3d9_pe_semantic_owner.hpp`.

In the tested candidate, the complete table range is checked before visitors. The same source,
payload, padding, resource identity, pin and remap checks remain in place.
After exact visitor/count/cursor conservation, one copy emits the active
descriptor prefix. Zero sections copy nothing. Compile-time size, alignment,
field-offset and representation checks bind prepared and wire descriptors.
The private bytes remain outside the committed frontier until the existing
append transaction publishes them. Generic emission is unchanged.

This is a value-materialization refinement inside the existing transaction,
not a new interleaving or lifetime policy. R-CORE-REC-7.66 owns its contract.
The added native tests compare compact bytes against the independent generic
builder, preserve ExactFixed/segmented role equality, and retain late
settlement failure/retry coverage. Lawful dense compact cases are indexed
23 sections and indexed-UP 24 sections: the latter has no index-buffer
section but has both UP payload sections. Generic 25-section structural
coverage is separate, not a fabricated compact timing input.

## Verification receipts

- `build-r1.json` records a test-only compilation failure: an invalid
  `SparseStatePlan::sparseLayout` reference. The test now obtains a public
  prepared-admission witness; no public mutating seam was added.
- `build-r3.json`: all four canonical build directories pass after final
  test additions.
- `native-r2.json`: eight focused native specs pass, including the compact
  owner, ledger on/off, producer differential and retainer suites.
- `tla-r1.json`: the existing TLA suite passes in 155.14 seconds. Models are
  unchanged; this is regression evidence, not a new proof of concrete bytes
  or CPU timing.
- After withdrawal, `build-r4.json` and `native-r3.json` pass all four builds
  and the same eight specs with the original product source. Two baseline PE
  readback processes pass 24 signatures each; these verify the restored
  default path, not the rejected candidate.
- `final-audit.json` proves the product-source restoration, preserved candidate
  evidence and all five frozen-baseline images restaged in the Wine root.
  Rebuilt canonical PE DLLs differ from that baseline only in their COFF and
  single CodeView debug-directory timestamps; the whole files match after
  normalizing those two fields. Bridges/provider remain byte-identical. Both
  raw and timestamp-qualified identities are retained, not conflated.

## Economics qualification

`economics-main.json` binds eight successful processes: baseline and candidate,
two repeats each, PE32 and PE64 under Sikarugir `cx-24.0.7`. All four
`fixture-build-*-rmain5.json` receipts bind the fixture, compiler, frozen image,
header version, unchanged generic-builder/producer source and thin archive
members. `freeze-audit-main.json` independently checks five-image provenance,
unchanged build options/provider/bridges, and the owner-only source delta.

Each process exercises 0/1/2/4/12/23/24 actual descriptors, not merely several
rows in one descriptor. The generic `CommandChunkBuilder` provides independent
expected payload bytes; loaded private-writer and public-append output agree,
including ExactFixed/segmented final-record payload parity. Binding rows are
lawful null bindings: this isolates descriptor economics, not real resource
retain/pin/remap cost or a workload-weighted PE total.

The loaded writer uses 1,000 warm calls followed by 100,000 timed calls per
shape. Matched-header public append uses eight warm groups and 2,000 timed
groups of 64 appends, with reset, planning, seed append and byte validation
outside each QPC pair. Empty-pair calibration is recorded, not subtracted.
The table reports percent changes of the two-process medians; positive means
slower. This is raw elapsed component time, not exclusive CPU or FPS.

| Active descriptors | PE32 writer | PE32 public append | PE64 writer | PE64 public append |
|---|---:|---:|---:|---:|
| 0 | -7.91% | +1.74% | +5.37% | +13.55% |
| 1 | +42.79% | +8.87% | +54.13% | +18.33% |
| 2 | +30.25% | +8.27% | +36.49% | +17.01% |
| 4 | +3.70% | +1.26% | +26.46% | +10.40% |
| 12 | -1.25% | +0.16% | -1.94% | -0.79% |
| 23, indexed | -3.70% | -3.00% | -7.33% | -3.26% |
| 24, indexed-UP | -4.17% | -3.60% | -4.61% | -1.36% |

For one descriptor, PE32 loaded-writer time grows from 31.84 to 45.46 ns;
public append grows from 175.76 to 191.35 ns. PE64 grows from 23.07 to
35.55 ns and from 114.28 to 135.23 ns respectively. These small-input
regressions are not offset by the dense-case result. They are not an estimate
of the whole game's FPS change.

The linked-code audit adds one external `memcpy` call to the writer main body
in both widths. Main/helper body code shrinks, but code-size reduction does
not establish economic improvement. Materializer and append/admission/public
root families in the DLL are byte/call identical; the complete append timing
is a separately compiled matched-header fixture, not a measurement of those
loaded parent functions. Its zero-section change must not be attributed to
an executed table copy, since that branch copies no bytes.

The first runtime attempt, `descriptor-baseline-x86-r1` using `rmain3`, failed
its shape oracle: zero-to-zero constant setters produced two sections where
the fixture requested four. It is excluded in full, including its partial
timings. `rmain5` uses changed constants and a valid view-transform key.
Earlier fixture compile/link failures are also not product failures.

Candidate GPU/wild promotion is stopped at the failed economic gate. No
candidate Metal pixel or GT2 FPS improvement is claimed. The candidate image,
source snapshot and all failed/successful artifacts remain available; the
runtime edit is not retained in production.

The next narrower hypothesis is fixed-size inline emission for small active
tables, or retaining the existing writer there while treating only sufficiently
large tables specially. It needs its own measured selection policy and must
preserve zero/single-section cost; this result does not justify implementing
an unmeasured threshold.

The latest independent observer-off FPS evidence remains H290's 05 GT2
29.8093 and H291's activity-excluding 06 GT2 remeasure 18.2167. These are not
measurements of this candidate; README remains unchanged.
