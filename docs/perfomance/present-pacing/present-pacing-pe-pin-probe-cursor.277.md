---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "PE pin-index cyclic probing"
type: evidence
status: measured-work-removal-no-claimed-gain
updated: 2026-09-08
source: experiments/output/pe-retention-miss-prefix-20260908/analysis
related: present-pacing-pe-post-lifetime-profile.276.md; present-pacing-pe-selected-masks.273.md
---

# PE pin-index cyclic probing

## Decision and scope

`PeSemanticBatchOwner::findPinIndex` and `insertPinIndex` now compute the
initial hash remainder once, then advance a bounded cyclic cursor. The old
`(start + probe) % N` recomputed a remainder on every probe, including the
first one. The production owner has 256 pins per physical family and an index
capacity of 1,025, not 4,097. No table size, hash, identity, ownership, wire,
transport, threading or feature default changes.

H276 identifies retention admission as an observed PE symbol, not a dominant
CPU cost. This increment removes actual compiled work; it does not infer
millisecond savings from stopped-PC counts or static instruction counts.

The first candidate returned the miss insertion prefix from `checkedPinHit`
instead of reading the family count again at its caller. Both complete linked
PE `.text` sections remained byte-identical to baseline. The compiler already
removed that source-level duplication, so the candidate was rejected before
Wine testing and its runtime edit was reverted. Its frozen images and section
hashes remain in the analysis root.

## Semantic refinement

For the physically bounded index, the invariant at probe `p` is
`cursor == (start + p) % N`, with `0 <= cursor < N`. Advancing from `N-1` to
zero and otherwise incrementing preserves that relation. The independent
`probe < N` bound still terminates a full current-generation table miss.
Lookup stops at exactly the same stale-generation or matching-object-ID row;
insertion selects and writes exactly the same first eligible row.

The index remains a locator, not identity authority. `checkedPinHit` still
checks the full kind/generation/object ID, physical pointer and live typed
prefix. Malformed hits do not become new acquisitions. Rollback, rebuild,
stamp wrap, failure settlement and retain/release order are unchanged.
This is the local representation refinement of R-CORE-REC-7.59, not a new
concurrency or resource-lifetime protocol.

`pinIndexCursorWrapDifferential` invokes the actual private production lookup
and insertion functions through the existing native friend seam, comparing
them to independent modulo-based reference loops. It covers all starting
buckets for table sizes 1, 3, 5 and 9, plus a three-object wrap-edge collision
chain at production size 1,025. Cases include empty/stale lookup, stale-row
insertion, full collision chains, same-ID overwrite and full-table miss.
Returned slots, untouched miss outputs and all table fields are compared.
These are 19 constructed traces, not exhaustive enumeration of all tables.

`retentionLookupMissUsesPerFamilyPrefix` separately covers six physical
families with A, failed-B settlement, successful B retry, then A again.
Accepted bytes match `CommandChunkBuilder`; rejected prefixes remain exact,
and owner and reference-builder refcounts balance separately. Existing owner
tests retain malformed-hit, alias, capacity, generation-wrap and partial
acquisition coverage. An initial fixture error omitted the comparison
builder's extra retained ref; it was corrected without changing runtime code.

Existing TLA regression passes (149.74 s); it is not a new cursor model or a
proof about Metal pixels. Native bounded differentials supply the new local
model/code binding. No shader, attachment or Metal action changes require a
new GPU-byte fixture for this arithmetic change. Wild pixel and economy
claims remain distinct from these deterministic checks.

Final native owner/views/producer differentials pass 3/3 on both arm64 and
x86_64, without compiler warnings. `build-native-final.log` and
`build-x86_64-builtin-native-final.log` preserve the final runs; the earlier
failed fixture run remains separate.

## Linked PE evidence

Main rebuilt both PE widths from the same configuration and froze complete
images before inspection. The audit includes each entire symbol body through
the next code-symbol boundary, not merely its first textual return.

| Width / admission | Instructions before → after | Code bytes before → after | Local stack subtraction before → after |
|---|---:|---:|---:|
| PE32 compact | 743 → 598 | 2,717 → 2,109 | 32 → 20 |
| PE32 generic | 746 → 599 | 2,716 → 2,109 | 32 → 20 |
| PE64 compact | 753 → 510 | 3,298 → 2,207 | 40 → 0 |
| PE64 generic | 753 → 510 | 3,298 → 2,207 | 40 → 0 |

Stack subtraction excludes saved-register pushes and is not a total stack
depth claim. The new loop uses compare/conditional-move wrapping; reciprocal
multiply work remains for the initial hash reduction but not each probe.
Jump-table dispatch remains, so the audit does not claim complete indirect-CFG
reachability. `baseline-linked.json`, `candidate-linked.json` (rejected
miss-prefix) and `cursor-linked.json` preserve the distinction.

## Post-commit CPU/FPS spots

Main alone ran four fresh named GT2 processes at `267e7439`: two perf/Warn
CPU/FPS spots, then two separate perf/Info full-self-PC diagnostics. Every
run stages the same five verified images; no build, test or second benchmark
overlaps a scene. Sikarugir cx-24.0.7 is used at 1024x768 (05) and 1280x720
(06). Offload replay is ON; inline constants, segmented transport, CPU-ready
Tape and index reorder remain OFF. The 05 repeat-focus helper is explicitly
disabled to avoid H276's late log append. All four processes are terminal.

The Warn runs retain frame sampling and a two-second process CPU observer,
but no stopped-PC or detailed PE probes:

| Product | Scene frames / wall ms | Scene FPS | Process CPU ms/frame | CPU-covered frame increments | CB / pass per frame |
|---|---:|---:|---:|---:|---:|
| 05 GT2 | 1,914 / 64,570.689 | 29.6419 | 69.6880 | 1,859 (97.13%) | 4.0010 / 15.7842 |
| 06 GT2 | 1,159 / 64,045.131 | 18.0966 | 95.3880 | 1,121 (96.72%) | 4.0017 / 19.8628 |

These CPU values use a single cumulative same-PID endpoint delta, not a sum
of selected intervals, and include all process threads. They are not
PE-exclusive CPU. Summed GPU CB time is 2.009/12.140 ms/frame, not a GPU
critical path. Eight reported error counters are zero; 05 covers its logged
scene, while 06's last counter at Present 2,040 leaves 42 final frames
unproven. 06 completes its scene then uses the catalogue-allowed result-GUI
timeout. No whole-scene pixel equivalence is claimed.

05 remains close to H275's 29.648/69.806 FPS/process-CPU spot; the focus-helper
recipe difference prevents calling this a matched A/B. The immediate earlier
06 recheck at `60931189` was 18.1561 FPS and 95.3333 process CPU ms/frame
(`app-d3d9-3dmark06-pe-cached-com-60931189-gt2-off-perf-r2-cleanrerun-20260908`).
The new 06 spot differs by approximately -0.33% FPS / +0.06% CPU. Single
observations do not establish a gain, regression attribution or neutrality.
The compiled work removal is real; a material end-to-end CPU saving is not
demonstrated. README remains unchanged, and 05's 30.1 target is still open.

`run_economy.py` records commands, environment, driver/source identities and
raw artifacts. `validate_measurements.py` independently checks all preserved
raw hashes, recomputes the frames and cumulative CPU envelopes, and verifies
the runtime files still staged in Wine after all four runs. See
`measurements-verified.json`; the raw run directories use
`app-d3d9-3dmark{05,06}-pe-pin-cursor-267e7439-gt2-off-perf-r1-20260908`.

## Fresh PE attribution

The two separate 250-Hz full-self-PC diagnostics bind against the exact
frozen PE32 DLL, SHA-256
`db8d14412ef400bf01fc49c6c301070fd7a59ad451c28d6cae9c50119ae76fa9`.
Both PE widths were rebuilt, but these actual workloads exercise PE32 under
Wine; this is not native arm64 PE execution. Provider/bridge images remain
byte-identical to H276.

| Product | Reports / final Present | Cumulative thread / PE samples | Exported buckets | Approximate scene-prefix thread / PE samples |
|---|---:|---:|---:|---:|
| 05 GT2 | 31 / 1,860 | 13,494 / 1,323 | 598 | 13,494 / 1,323 |
| 06 GT2 | 33 / 1,980 | 14,087 / 844 | 471 | 13,371 / 840 |

Every report has zero omitted/overflow samples and zero suspend/context/resume
failures. Module rows sum to their populations. 06 subtracts the report at
Present 840; its first heavy frame is 891 and its final frame is 2,019.
Thus the prefix includes a pre-scene gap and excludes the last 39 frames;
it is not exact timestamp isolation. 05's sampler prefix ends 14 frames before
its 1,874-frame diagnostic scene, though its final health counter covers the
logged scene. 06 health also leaves its final 39 frames unproven.

The PE module owns about 9.8%/6.3% of these thread-stop populations, versus
69.7%/78.6% in the game and 11.7%/8.6% in the bridge. They are not CPU-time
fractions. The symbol join leaves 306/210 cross-symbol samples unassigned.

| Selected unambiguous scene-prefix body | 05 samples | 06 samples |
|---|---:|---:|
| `ValidatedConstWrite<16>::applyNonEmpty` | 83 | 62 |
| `accumulateRetentionDelta` | 42 | 31 |
| `writeSparsePlanPayloadQualified` (main body) | 38 | 19 |
| `acceptSparseStatePlan` | 36 | 20 |
| `buildSparseStatePlan` | 33 | 10 |

These counts do not establish an inclusive hotspot ranking. H276 retention
was 59/24; the mixed direction and small populations are not a measured
retention speedup. `profile-{05,06}-qualified.json`, `symbols-qualified.json`
and the frozen-image CSV/JSON joins preserve coverage and ambiguity. Raw
diagnostic run names replace `perf` with `fullselfpc` in the names above.

The next constant-path gate is current range-length and all-equal/mixed-change
population evidence. An unconditional whole-range equality prepass could
double-scan mixed updates; H269's rejected held-value variants must not be
reintroduced merely because constant samples remain visible. Source identity,
admission authority and physical pin validation remain separate obligations.
