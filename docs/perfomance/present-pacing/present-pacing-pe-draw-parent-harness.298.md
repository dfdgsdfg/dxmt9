---
domain: present-pacing
workload: actual PE public COM; 3DMark05 GT2; 3DMark06 GT2
title: "Public draw-parent harness: indexed coverage and noisy calibration"
type: evidence
status: fixture-qualified-economics-unqualified
updated: 2026-09-09
source: experiments/output/pe-draw-parent-economy-H298-20260909
related: present-pacing-pe-identity-call-abi.297.md; present-pacing-pe-public-com-parent.294.md; present-pacing-pe-vs-constant-call-shape.278.md
---

# Public draw-parent harness

H298 starts at `c2e1d6cb`. It changes no production renderer source and retains
all five H297 runtime images byte-for-byte. Before pricing another private
binding helper, it promotes the ignored H294 fixture into a checked-in,
on-demand public COM lane with indexed coverage. R-BENCH-7.1–7.4 own the
measurement and evidence boundaries.

## Implemented scope

- Nine cases × setter-only / DrawPrimitive / DrawIndexedPrimitive: 27 cells.
- A hardware-only 32×32 FFP target, managed VB/INDEX16 IB, event drain and
  exact green/black readback; no private helper invocation.
- Viewport, scissor, scalar, stream-offset and six constant-family cases,
  plus single VS-F calls of count 3 equal, count 3 changed and count 4 changed.
  The latter lengths are motivated by H278's historical call distribution,
  not claimed as a fresh population measurement.
- Mutable caller storage is logically immutable and checked against independent
  pre-call snapshots. Initial/final getters cover exact constant extents and
  surrounding registers, VB/IB bindings and selected states. All timed calls
  contribute to the HRESULT aggregate; setup/drain calls are checked separately.
- Setup, seed draw and re-clear precede QPC. The extra clear prevents the seed
  from making dropped measured draws pass. Setter-only pixels remain the seed;
  setter state and FFP-inactive shader constants are qualified by getters.
- Raw QPC ticks/frequency and exact call denominators; explicit process CPU
  start/end values include setup, warm-up, query drains and oracle work.
- Dynamic loaded COM slots 81/82 are checked against the selected PE image's
  linked vtable, exact public symbols and executable sections. Source/build,
  fixture/tool/environment, five product-image and actual Wine-image receipts
  are retained per run.

The fixed timing recipe is 512 measured groups × 32 iterations, eight warm
groups, 256 records / 1,310,720 bytes, replay offload on, and inline constants,
segmented transport, tape, index reordering, perf counters and PE diagnostics
off. The separate trace uses two measured groups and one warm group with
recorder logging. PE32 is Sikarugir WoW64/Rosetta; PE64 is not native ARM64 PE.
No build, native test or second benchmark overlaps a timing process.

## Verification and provenance

All four canonical builds complete; final PE builds are warning-free. Four
focused Meson checks pass: the draw-parent parser/CLI suite (13 tests), the
existing constant economy parser, conformance runner regression and the
conformance manifest audit. The performance-leaf source audit also passes. Negative
cases catch independent state-check failures, bad counts/ticks/identities,
missing or duplicated evidence, a timed flush, absent trace evidence and
default-suite accidental economy selection. No renderer transition or resource
ownership policy changed, so a new TLA+ model or TLA regression run is not
claimed for this fixture-only round.

The first eight actual PE processes pass 216 cells: two traces and six timing
processes, three timing repeats per architecture. Each trace has 324 ordered
markers and 195 recorder chunks, with zero chunks between any timed pair.
The strict parser binds every row and final Release/termination marker.
`repeated-summary-r1-r3.json` preserves the matched inputs and raw values.

Independent review caught an initial cross-stream ordering defect. The final
fixture emits all owned records on flushed stderr with the recorder logs,
because the canonical runner concatenates stdout and stderr separately.
H294–H297's original markers already used stderr; their marker/chunk ordering
remains valid. This correction does not withdraw their independent evidence.

After the eight calibration processes, the canonical runner gained the
manifest's `on_demand` filter. Explicit `--aux-exe` selection retains the same
execution path; default conformance omits this long matrix. The final
`x86-trace-final` and `x64-trace-final` runs requalify that integration, adding
54 cells. Calibration receipts identify the pre-filter runner from `c2e1d6cb`;
final traces identify the updated runner. They are not silently conflated.

## Economics verdict: not calibrated for small changes

Longer accumulation did **not** establish a stable parent-cost baseline.
Every primitive/indexed row exceeds the desired repeatability band:

| Three same-image timing processes | PE32 | PE64 |
|---|---:|---:|
| Canonical runner wall, including staging/startup/teardown | 30.16–36.13 s | 28.15–38.26 s |
| Summed row process CPU, including warm-up/drains/oracles | 16.54–22.75 s | 14.36–24.23 s |
| Draw-row range / median across repeats | 14.22–272.82% | 20.37–229.72% |

These ranges are `(max − min) / median`, not standard deviations or confidence
intervals. Process CPU is not PE-exclusive or equal in scope to QPC. Longer
groups-by-count alone do not identify whether scheduling, query-drain feedback,
cache state or a slow-path incidence caused the variation. The existing
aggregate-only receipts cannot locate individual long groups.

The harness is accepted for bounded public-entry/state/pixel/trace coverage,
**not** for small-delta performance decisions. No helper optimization, CPU
reduction, performance neutrality or FPS improvement is promoted from these
numbers. README is unchanged; H297 remains the last wild FPS measurement,
with its documented 06 health-tail limitation.

Next: retain each already-read group QPC interval outside the timed window and
report its distribution alongside the unfiltered sum. Separate long intervals
and drain behavior before pricing the unused pin-output candidate. Do not
discard slow groups or choose a favorable median as a replacement FPS result.
