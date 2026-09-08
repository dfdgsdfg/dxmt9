---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2; controlled PE constant setters
title: "Actual PE constant-setter economy harness"
type: evidence
status: accepted-harness-candidate-not-promoted
updated: 2026-09-08
source: experiments/output/pe-constant-economy-20260908/analysis
related: present-pacing-pe-three-register-equality.279.md; present-pacing-pe-vs-constant-call-shape.278.md
---

# Actual PE constant-setter economy harness

H279 could not price the equal-path saving against changed-path overhead from
small whole-game CPU/FPS trends. H280 adds a reusable controlled actual-PE
measurement lane without changing production renderer code. It does not
reintroduce the withdrawn shortcut or change README FPS values.

## Measurement contract

`d3d9_constant_economy.cpp`, selected by the existing conformance executable's
test-only `setter-economy` case, calls all six public VS/PS float/int/bool
setters. Each family has counts 1/3/4 with equal, last-only and all-changed
inputs, a count-three middle-only cell and a count-zero control: 66 cells.
Each cell uses 10,000 warm-up and 50,000,000 measured calls. The inputs are
prebuilt, distinct A/B arrays; equal cells have equal bytes, changed cells
alternate the exact intended subset on every call. A six-register materialized
neighborhood includes surrounding canaries. Independent snapshots pin both
caller arrays and the getter oracle outside timing; every HRESULT contributes
to qualification. The timed loop has no draw, flush, clock, allocation or log.

QPC measures wall time; explicit `GetProcessTimes(GetCurrentProcess())`
measures **process CPU**, including harness/Wine/provider helper threads. The
calling thread ID identifies the setter source, not exclusive CPU ownership.
Count-zero controls are reported without automatic subtraction. Warm repeated
setters do not price dirty consumption, later sparse/final-wire emission,
StateBlock recording, allocation or real shader/draw scheduling. The fixture
reports no pixel oracle; H279's native refinement and actual PE pixel evidence
remain separate correctness layers.

`run_pe_constant_economy.py` delegates launch/staging to the canonical
conformance runner, disables renderer observers, clears ambient Wine/DYLD
overrides, and qualifies the complete 66-cell transcript, successful teardown,
full staged DLL/provider hashes and fixture identity. Receipts include the
controlled knobs, effective runner environment digest, runner/manifest hashes
and Wine Unix image hashes. Generic auxiliary verdicts must not be read as
evidence for unselected pixel/capture/lifecycle cases. The strict native parser
rejects missing, duplicate, malformed, wrong-scope, mutable-source or incomplete
receipts.

## Calibration and a corrected CPU-scope assumption

The initial one-million-call PE32 pilot completed, but its roughly 8–25 ms
cells approached the observed 10 ms CPU-time quantum. A second pilot returned
zero CPU for a count-zero cell and was rejected. Both remain calibration
artifacts, excluded from economy comparison.

Three subsequent 50-million-call runs (`*-long-r*`) exposed a more important
scope error: `GetThreadTimes(GetCurrentThread())` was initially labelled as
calling-thread CPU. Inspection of the **actual Sikarugir** `ntdll.so`, SHA-256
`81f81cf1bb70069e94fa89d53fb7327cfb52fb9629c5b84134c851e681a20235`, shows
`NtQueryInformationThread` reporting `get_thread_times` unimplemented and
calling process `times()` for the current-thread pseudo-handle. The saved
`sikarugir-NtQueryInformationThread.txt` binds this to the measured binary,
not just a different Wine checkout. Those preliminary records do not establish
thread CPU and are excluded from the final protocol. The harness now explicitly
uses process timing and requires `cpu_scope=process`; the new parser rejects
the old unqualified schema.

## Repeated comparison

Eight fresh processes run baseline → candidate → candidate → baseline for
PE32, then the same sequence for PE64. All 528 cells and 26.4 billion measured
calls qualify. PE32 processes take 70.6–71.4 seconds, PE64 44.9–46.0 seconds,
including setup/teardown. No build or other test overlaps a timed process.
`verified-comparison.json` rederives the complete matrix from raw transcripts,
checks their artifact hashes, and requires identical fixture, environment,
runner, manifest, bridge and provider identities across each comparison.

The preserved H279 candidate is staged independently of the final fixture.
Baseline is restored H278 code at `72f343ea`; candidate is `6ffdf357`:

| Artifact | Baseline SHA-256 prefix | Candidate SHA-256 prefix |
|---|---|---|
| PE32 `d3d9.dll` | `af8e1fde12b6b7ce` | `74c3a38de273b09c` |
| PE64 `d3d9.dll` | `c694f8565e44427d` | `7df9b181f6656d06` |
| Unix provider, unchanged | `98f16a64a310aba5` | `98f16a64a310aba5` |

The shared environment digest is
`3c2effc494bf68033988708a146054932ccafa54d15fa1bf076f2a22f6a3492e`.
Full identities and all six families' raw cells remain in the run receipts.
CPU deltas retain the observed 10 ms quantum, or 0.2 ns/call at this denominator;
means below average two observations, not a claim of finer timer resolution.

| VS float cell | PE32 baseline → candidate process ns/call | PE64 baseline → candidate process ns/call |
|---|---:|---:|
| Count 3, equal | 17.9 → 16.5 | 10.7 → 9.3 |
| Count 3, last-only changed | 21.1 → 22.9 | 11.5 → 13.1 |
| Count 3, middle-only changed | 20.7 → 22.8 | 11.5 → 13.2 |
| Count 3, all changed | 22.7 → 23.7 | 12.2 → 13.9 |
| Count 4, equal | 20.0 → 20.6 | 11.7 → 11.6 |
| Count 0 control, not subtracted | 8.2 → 8.1 | 4.2 → 4.2 |

This prices the intended trade-off: count-three equal calls save 1.4 ns in
both PE widths, while changed VS-F cells add 1.0–2.1 ns (PE32) or 1.6–1.7 ns
(PE64). The other float/int families show the same broad equal/changed split;
the unchanged bool kernel is a separate control. H278's equality-heavy VS-F
population makes this a plausible local saving, but it is a whole-log
population, not a scene-qualified weighted CPU or FPS result. The three-register
precomparison also changes shared-kernel codegen outside count three. Do not
ignore those companion cells or infer that native PE64 results describe the
PE32 GT2 application.

## Verification and decision

Both canonical PE builds and the full Unix-provider build pass. The native
parser/manifest targets pass (eight parser unit tests). Final linked-loop
receipts show the six public COM setter calls with 32-bit loop counts and no
clock in the measured loop. Independent review caught source-oracle aliasing
and incomplete environment receipts; both are fixed before the final cohort.

Two additional default-selector PE32/PE64 pixel runs pass after timing. Their
192 sampled pixels exactly match H279's baseline receipts, with the existing
literal tolerance unchanged. They use the prior canonical pixel driver with
fresh `h280-restored-pixels-{x86,x64}` outputs, not generic auxiliary verdicts
for unselected cases. `final-audit.json` pins those receipts and confirms all
five Wine-root renderer images are restored to current canonical builds.
Production `src/` and `include/` remain unchanged.

Retain the harness, **not** the H279 runtime shortcut. This is now measured
local CPU economy with a measured fallback tax, rather than an unpriced code
shape. It does not establish repeatable whole-workload CPU savings or README
recovery. The next bounded candidate should reuse the exact comparison result
for changed-register updates, avoiding precomparison followed by a second
generic walk; PE32 register pressure and non-three cells are explicit gates.
Existing native state refinement and pixels remain mandatory, followed by
this local economy matrix and separate observer-OFF wild CPU/FPS evidence.

Re-run through the checked-in driver, using a new output directory:

```sh
scripts/run_python.sh scripts/tools/run_pe_constant_economy.py --arch x86 \
  --output experiments/output/pe-constant-economy-<unique-id>
```
