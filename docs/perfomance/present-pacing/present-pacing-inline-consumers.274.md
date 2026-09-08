---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2; PE32/PE64 inline capture
title: "Inline consumers: repeated CPU economy and capture boundaries"
type: evidence
status: partial
updated: 2026-09-08
source: experiments/output/pe-inline-consumers-20260908/analysis
related: present-pacing-inline-constant-economy.266.md; present-pacing-inline-constant-boundaries.270.md; present-pacing-pe-selected-masks.273.md
---

# Inline consumers and repeated economy

## Decision

Inline constants remain **default-off**. Eight same-image Warn runs strengthen
the small 05 FPS signal, but do not establish material total-CPU savings or a
repeatable 06 improvement. A new real PE capture fixture exposes and pins an
inline origin-preflight defect. The cold locator fix restores publication;
four PE32/PE64 OFF/ON bundles pass exact fresh-provider source/output oracles.
Capture teardown still fails independently in all four processes. Neither
reduced record count nor nominally reaching historical README numbers
authorizes promotion.

## Same-image economy

`runtime-build.json` freezes all five Wine-root images. Runtime source at
`026d03bb` is identical to `9263d1dd`; H273 retained only tests/docs. The H273
restored canonical PE DLLs have equal `.text` but different full hashes, so
these runs deliberately reuse the frozen Wine images with `--skip-stage`.
`run_economy.py`, `matrix-plan.json`, and `cpu-matrix-qualified.json` retain
commands, selectors, image hashes, source identity and raw-artifact hashes.

Recipe: Sikarugir cx-24.0.7; perf/Warn; replay worker ON; contiguous transport;
CPU-ready tape and opaque-depth index reorder OFF; frame sampling ON.
PE decimated clocks, stopped-PC sampling and native-thread probes are OFF.
05 is 1024×768; 06 is 1280×720. All eight use canonical catalogue runners;
no build, test or second Wine benchmark overlaps a measured scene.

| Product / order | Scene frames | Scene wall ms | FPS | Process CPU ms/frame | CPU frame-span coverage |
|---|---:|---:|---:|---:|---:|
| 05 OFF r1 | 1,910 | 64,633.391 | 29.5513 | 70.2607 | 96.44% |
| 05 ON r1 | 1,938 | 64,596.135 | 30.0018 | 70.2608 | 97.01% |
| 05 ON r2 | 1,953 | 64,569.892 | 30.2463 | 70.1977 | 93.29% |
| 05 OFF r2 | 1,892 | 64,631.113 | 29.2738 | 70.8957 | 96.83% |
| 06 ON r1 | 1,155 | 64,095.737 | 18.0199 | 96.1061 | 96.36% |
| 06 OFF r1 | 1,163 | 64,114.127 | 18.1395 | 95.0133 | 97.16% |
| 06 OFF r2 | 1,161 | 64,062.276 | 18.1230 | 95.3812 | 97.24% |
| 06 ON r2 | 1,174 | 64,054.966 | 18.3280 | 94.5863 | 96.85% |

Sample-weighted OFF→ON:

| Product | FPS | Process CPU ms/frame | Interpretation |
|---|---|---|---|
| 05 | 29.4126 → 30.1240 (+2.42%) | 70.5773 → 70.2297 (−0.49%) | Small throughput signal, little measured total-CPU reduction |
| 06 | 18.1313 → 18.1739 (+0.24%) | 95.1972 → 95.3381 (+0.15%) | Direction changes by repeat; no economy claim |

The order is counterbalanced, not randomized or thermally controlled. Each
mode has only two observations. Within-mode FPS ranges are 0.95%/0.81% for
05 OFF/ON and 0.09%/1.71% for 06; 06 ON CPU/frame varies 1.61%.
These are not confidence intervals. Historical README runs use a different
index-reorder recipe; these numbers are not a matched recovery verdict.
Earlier user-reported desktop-use-contaminated runs are not in this matrix.

FPS uses contiguous positive-wall scene samples; 06 also requires >100 draws
to exclude loading. CPU uses one cumulative same-PID first/last endpoint
delta, both in the scene and strictly before its last logged frame. `ps` is
sampled every two seconds with centisecond CPU resolution. Log/frame and CPU
endpoints are approximate; CPU includes all process threads, game, Wine and
synchronous Unix work, **not PE-exclusive CPU**. It is not obtained by dropping
and summing selected adjacent intervals. Raw endpoints and RSS in KiB remain
in the qualifier. The different partial frame populations preclude treating
CPU/FPS deltas as an exact additive critical-path budget.

05 records/reported Present fall from 2,698.61–2,700.69 to
1,701.60–1,703.25, approximately 37%; constant bytes/application still exist.
06 counter ratios mix loading and an incomplete tail and are not scene-only
counts. CB/frame stays about 4.001/4.002 for 05/06, pass/frame about
15.784–15.797/19.850–19.866. Summed GPU CB time is 1.988–2.000/
11.878–11.910 ms/frame, not GPU critical-path latency.

All eight runner results pass and eight retained renderer error counters are
zero. 05 exits normally. 06 uses a 90-second result-GUI timeout after the
catalogue's 70-second capture delay; its complete ~64-second scene is checked
independently. Last periodic counter Present is 2,160; unreported tails are
6/33/13/10 frames in run order and are not health-proven. No whole-scene pixel
equivalence is claimed from these performance runs.

## Consumer contract audit

`R-BACK-2.52` previously described `ProcessVertices` as a standalone-wire
constant drain. Production `D3D9DeviceImpl::ProcessVertices` instead passes
live `recorderState_.peConsts` to its PE interpreter. Raw bridge buffer locks
drain published Unix replay, not pending PE constants. Adding a wire flush
would not serve this consumer. The corrected contract requires live VS F/I/B
reads without settling pending recorder deltas; no ProcessVertices runtime
change or new dedicated ProcessVertices pixel proof is claimed.

UP/indexed-UP and chunk barriers/end drains remain standalone. Query Issue
does not consume constants; GetData and child Present/StateBlock paths flush.
FFP↔programmable switches must preserve later constant state. Code inspection
is not a consumer oracle: switch, failure/retry, cross-source and wider
deployment proofs remain separate gates.

## Capture evidence boundary

The native inline fixture now additionally sends real owner OFF/ON bytes
through `dxmt9c_device_commit_chunk` with nonzero capture token/event fields,
drains replay, checks independent final-Slot constants and retrieves complete
production identity rows. It distinguishes 22 standalone from four inline
records over four ordered draws. It binds descriptor/identity propagation,
not the PE capture-session publisher or arbitrary worker interleavings.

The actual PE `capture` selector uses a 64×96 implicit backbuffer and six
A-B-A draw rows. All six families change before the first captured draw.
The final fixture has 17 ordinary draws and one indexed control. Clear inside
the interval defines uncovered pixels after the preceding DISCARD Present.
The fixture neither explicitly acquires a backbuffer wrapper nor adds a public
CPU readback/control inside the interval. Expected values are printed as expectations only; the
external driver validates the published bundle and inspects actual
`source.rgba` bytes. A successful fixture process alone is insufficient.

### Preserved failures and correction

Pre-fix `capture-{x86,x64}-{off,on}-r1` uses the preserved r1 fixture patch
and pre-origin images. Both OFF processes publish valid bundles with 18 exact
sampled pixels and three standalone records per family. Both ON processes
abort at `command_chunk_produced_pass_preflight`: the origin locator treats a
valid zero-element-size inline constant section as a malformed binding table.
All four also retain two device references after Reset. These failures remain
preserved, not replaced by the passing content receipts below.

`origin-red-details.log` records the deterministic native failure before the
fix: unknown stream-binding origin lookup fails while traversing inline
constants. The fix dispatches by section kind before checking binding stride;
only texture/RT/depth sections are origin-binding tables. Both production
callers canonically validate first. Six individual constant families and all
six together now pass; malformed constant ranges and stream geometry still
fail canonical validation. No command grammar, import rejection, hot recorder
path or runtime default changes.

The r1 ordinary-only PE32 OFF bundle was independently rejected by fresh
provider preflight as `unsupported-grammar`, before object creation or any
readback. The generic `source-mismatch` field on that rejection is **not a
measured pixel mismatch**. The general provider currently requires an indexed
draw; r2 supplies one explicit control rather than broadening that grammar.
The r2 in-interval Clear also defines DISCARD contents. These fixture changes
are separate from the locator fix; there is no ordinary-only provider proof.

### Post-fix actual capture and fresh replay

| Source process | Canonical content / sampled pixels | Fresh provider source and output | PE teardown |
|---|---|---|---|
| PE32 OFF r2 | valid / 18 exact | strict match, 24,576 bytes each | fail: 2 refs |
| PE32 ON r2 | valid / 18 exact | strict match, 24,576 bytes each | fail: 2 refs |
| PE64 OFF r2 | valid / 18 exact | strict match, 24,576 bytes each | fail: 2 refs |
| PE64 ON r2 | valid / 18 exact | strict match, 24,576 bytes each | fail: 2 refs |

OFF emits three standalone records per family; ON emits three inline sections
per family and no standalone constant records. Each fresh native provider run
uses identity partitioning, zero warmups and one repeat, creates/releases all
six objects, and retains capture-authoritative identity/settlement rows.
The source and output digest is equal across all four runs:
`299de44bf226e55e6c7a77717e1dac116a0a08f1ace056514195d77fe255cf16`.
This is whole-buffer equality to the captured oracle plus 72 independently
expected source samples, not a proof of every possible shader or scene.

The conformance fixture intentionally still checks final Release==0 and fails.
Production `admitRenderTapePresentOutput` internally acquires the swapchain
backbuffer wrapper. Its cached surface retains the container and device;
the swapchain retains the surface. Reset does not break this known cycle.
The fixture adds no getter to create it and no extra Release to conceal it.
Fresh Unix provider object conservation does not establish PE COM teardown.
The manifest records capture as **failing**, separately from the existing
passing plain pixel selector.

## Verification and remaining gates

All four canonical builds pass after the cold fix; final native six-test and
x86_64 seven-test groups pass, including the install-name audit.
PE32/PE64 non-capture ON regression also passes 144 exact sampled pixels and
zero final device references. The capture-only failure remains distinct.
The native identity binding and canonical rejection tests are bounded executable pins;
no queue protocol or TLA transition changes, new TLC proof, exhaustive worker
interleaving or ProcessVertices pixel claim is made here.

Performance images precede this cold capture fix. Capture recipes freeze the
post-fix three-image set per PE width and the revised fixture independently;
they are not post-fix FPS samples. The committed compact receipt is
[inline-constant-capture-20260908.json](../../../tests/conformance/d3d9/evidence/inline-constant-capture-20260908.json).
Raw drivers, recipes, pre-fix failures, bundles and qualifiers remain under
the source artifact root. Next gates are the separately modeled/tested PE
capture ownership cycle, remaining consumer/switch/failure boundaries, and
repeatable CPU economy. Inline constants remain default-off; README is unchanged.
