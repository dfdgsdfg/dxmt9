---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Complete bounded PE self-PC reporting"
type: evidence
status: runtime-coverage-verified-not-promoted
updated: 2026-09-07
source: experiments/output/pe-full-selfpc-20260907/analysis; experiments/output/pe-current-attribution-20260907/analysis; experiments/output/app-d3d9-3dmark{05,06}-pe-full-selfpc-b9713922-gt2-sampler-r1-20260907; experiments/output/app-d3d9-3dmark{05,06}-pe-full-selfpc-b9713922-gt2-perf-r1-20260907
related: present-pacing-pe-constant-bitcompare.254.md; ../../../specs/d3d9/recorder/spec.md
---

# Complete bounded PE self-PC reporting

## Decision and contract

At baseline `242da3cf`, H254's 05/06 GT2 samples report only 344/1,459 and
221/922 PE samples in their top-32 self-PC rows. Zero histogram overflow does
not mean the printed profile covers all recorded samples. The diagnostic
already retains up to 3,072 distinct 64-byte buckets, so recover those before
selecting another optimization from a minority of the distribution.

`DXMT9_PE_THREAD_SAMPLER_FULL_SELFPC=1`, with the sampler enabled, gives the
cold report call a nothrow, caller-owned 49,152-byte row buffer. It copies live
rows while holding the existing sampler lock and sorts/formats after release.
The sampler never holds this lock, allocates or logs while the target thread
is suspended. The suspension/aggregation loop, renderer policy, hot PE state
and default 32-row output selection are unchanged. Full reporting adds cold
allocation, sorting and logging cost; it is not a performance mode.

Each new group starts with `selfpc_coverage=1` and ends with a summary:
reported + omitted + histogram-overflow samples = independently captured
self-module samples. The self count is **not** reconstructed from the three
terms being checked. The footer records row/storage bounds and `top`, `full`
or allocation-failed `full_fallback` mode. Completeness requires zero omitted
and overflow samples and no allocation fallback. The parser rejects missing
footers, malformed totals and fallback instead of silently treating the prior
group as current. Legacy logs remain readable with unverified coverage.

This closes reporting truncation, not sample-time uncertainty, module-table
late loads, 64-byte symbol-boundary ambiguity, missing final tails, or the
difference between stopped-thread PC shares and exclusive CPU time. No runtime
CPU saving or README recovery follows merely from better observation.

## Verification

Four canonical builds pass. Native sampler, diagnostics, ABI-source and parser
targets pass (four unique Meson targets); the parser suite has 15 tests,
including full 3,072-row and malformed/truncated fixtures. The x86_64 sampler,
diagnostics and install-name targets pass 3/3. Native coverage pins full/short/
null capture, exact load cap, repeats, overflow versus omission, stable full
sorting and top-32 parity. An initial invocation used a nonexistent ABI test
name; the corrected `dxmt9-pe-device-abi-source-spec` passes. Logs retain both.

Both linked PE images preserve exports, vtable/RTTI owners, build options and
the 12 manifest hot-entry codegen metrics. Diagnostic code increases PE32 by
13,824 bytes and PE64 by 10,752; unchanged hot metrics are not byte-identical
images or an FPS-neutrality proof. The Unix provider and both bridges are
byte-identical to H254. No rendering/queue transition changed; no new TLA run
or Win32 scheduler proof is claimed.

Final staged candidate PE32 SHA-256:
`37bdb80ea01dbc0697cdbbb548081f021f9aa5d536618b20e333dad762a02855`.
PE64 SHA-256:
`038c46c081d0876b0205f7a8984c13877516cde23c79123b5bac8376cae5ee83`.
`candidate-linked-final.json` / `linked-comparison-final.json` supersede the
pre-final-rebuild receipts; the earlier hashes are not the wild-run binaries.

## Actual full-distribution evidence

Implementation `b9713922` has one full-report sampler run per product on
Sikarugir cx-24.0.7: 05 GT2 at 1024x768 and 06 GT2 at 1280x720. Both stage
the final hashes above and the unchanged H254 provider/bridges. These runs
use the perf profile with frame sampling, module map, 250-Hz PE sampling,
FULL_SELFPC and Info logging; detailed recorder timers, phase split and the
copy ledger are OFF. They are diagnostic runs, not FPS evidence.

| Product | Last reported Present | All thread samples | Self-PC buckets | Reported / all PE samples | Omitted / overflow |
|---|---:|---:|---:|---:|---:|
| 05 GT2 | 1,800 | 13,368 | 634 | 1,502 / 1,502 | 0 / 0 |
| 06 GT2 | 2,100 | 14,035 | 472 | 902 / 902 | 0 / 0 |

Both final reports pass the strict parser, with `mode=full`, `complete=1`,
zero suspend/context/resume failures and passing module-probe containment.
Both catalogue results pass, with zero chunk rejections, GPU errors, pipeline
build failures and no-pipeline draw skips in retained counter snapshots.
05 exits normally and emits a recovered `.3dr`. 06 finishes the selected
scene and the permitted timeout closes its result GUI; its N/A aggregate
score is expected for one graphics test without the full graphics/CPU suite.
Captures at 0:59.78 / 0:31.03 show the expected forest at those instants,
not whole-scene flicker or pixel-equivalence evidence.

PE thread-PC shares are 11.24% / 6.43%; EXE shares are 68.24% / 75.04%,
and PE-bridge shares 11.48% / 10.15%. The bridge can include time inside a
Unix call, and none of these shares is measured exclusive CPU time. The 06
population includes loading. `sampler_phase_delta.py` validates all 35 full
groups and subtracts the last complete report before the first heavy frame
(Present 1,020; first heavy frame 1,022). Through Present 2,100 this retains
896 PE samples / 13,280 thread samples in 469 buckets, with conserved
per-bucket counts. Its 6.75% PE share is an approximate scene-prefix delta;
the unreported final tail remains absent.

`join_full_selfpc.py` joins every exported bucket to the frozen staged
`candidate-pe32.dll`; `05-pe-symbols.json` and `06-pe-symbols.json` retain
full names and offsets. A bucket crossing another symbol start is left
unassigned: 406 / 271 samples (27.0% / 30.0% of PE samples). Interior padding,
inlining and asynchronous Wine PC mapping still limit attribution. The
following counts include only non-crossing buckets, not inclusive call costs:

| Outlined function | 05 samples | 06 samples |
|---|---:|---:|
| `applyConstShadowLiveFixedWidth<16>` | 111 | 64 |
| `buildSparseStatePlan` | 35 | 37 |
| `acceptSparseStatePlan` | 56 | 31 |
| `accumulateRetentionDelta<45, CompactPlan>` | 55 | 27 |
| `writeSparsePlanPayloadQualified<CompactPrepared>` | 36 | 16 |
| `PeSemanticPreparedIdentityWitnessT<45>::add` | 28 | 19 |
| `generateBoundAutogenMipmaps` | 14 | 9 |

The JSON lexical family groups contain template-argument and lambda owner
names; they must not be interpreted as exclusive subsystem timing. In
particular, 603/326 owner-related-name samples do not prove that the owner
alone consumes that fraction of PE CPU. The full distribution establishes
coverage and narrows code review, not a new measured savings estimate.

### Bounded follow-up audit

At this HEAD, `accumulateRetentionDelta` performs one canonical owner pin
lookup per newly retained record identity. Its typed-prefix, generation and
pointer checks reject malformed hits; the prepared witness already reuses
the result. Do not re-propose deleting a second owner index or second wire
dedup pass: H253 and earlier work removed them. Record-local linear identity
search remains, but H248's 2.588 mean unique identities does not justify a
hash-table replacement from these PC counts.

`generateBoundAutogenMipmaps` still initializes three 20-entry arrays and
examines all bound slots on each draw. Non-null textures pass wrapper RTTI
before the immutable AUTOGEN-enabled and mutable dirty checks. A derived
bound-AUTOGEN mask could skip known ineligible slots, but must close normal
SetTexture, StateBlock Apply, Recording non-publication and reset paths,
while preserving first-slot dedup, append-before-clean and exact wire
identity. The small direct sample counts do not justify a large FPS promise.
Duplicate-binding physical-pin validation reuse is another hypothesis, not
an accepted optimization: occurrence distribution, rollback/failure pins and
fresh evidence are still required.

## Separate sampler-OFF performance spot checks

The same committed/staged images have one sequential perf run per product,
with frame sampling ON, warn logging and PE module-map/sampler/full-report,
detailed timers, phase split and copy ledger OFF. No GPU trace or build runs
alongside the scene. A read-only `ps` observer records process CPU/RSS every
two seconds; these are not entirely observer-free runs. Resolution, runtime
and index-cache selection match H254's current recipe.

| Product | Sampled FPS | README FPS | Delta to rounded README | Rows / wall ms | CB / frame | Pass / frame |
|---|---:|---:|---:|---:|---:|---:|
| 05 GT2 | 29.316 | 30.1 | -2.60% | 1,894 / 64,606.040 | 4.001 | 15.789 |
| 06 GT2 | 18.065 | 18.1 | -0.19% | 1,157 / 64,046.536 | 4.002 | 19.858 |

05 retains every positive-wall row (frames 1–1,894). 06 retains positive-wall
rows with more than 100 draws (990–2,146), excluding loading, not initial
slow scene frames. Relative to H254's 29.500/17.958 spot checks, these are
-0.62% / +0.59%. Neither change establishes causality or qualified neutrality.
06 rounds to README 18.1, but historical index-cache selection differs and
repeatability is unproven. The README remains unchanged; CPU savings and
noise-qualified recovery of both targets remain open.

Both results pass with matching hashes and zero retained chunk/GPU/pipeline
failure counters. 05 exits normally; 06's accepted timeout closes the GUI
after the complete scene. Captures at 1:00.09 / 0:30.77 are coherent at those
instants only. Per-frame GPU command-buffer time is 2.042 / 11.846ms; this
completion-attributed total is not an exclusive GPU wall interval.

`summarize_perf.py` reproduces `perf-measurements.json`, including partial
process-CPU intervals. Endpoints must advance, be draw-heavy and remain before
the final frame; final-frame/teardown pairs are excluded.

| Product | Observed frame increments (endpoints) | CPU seconds / elapsed seconds | Process CPU ms / frame | Average cores | Sampled RSS peak, KiB |
|---|---:|---:|---:|---:|---:|
| 05 GT2 | 1,809 (31–1,840) | 127.50 / 60.275 | 70.481 | 2.115 | 3,461,296 |
| 06 GT2 | 1,142 (996–2,138) | 108.53 / 62.281 | 95.035 | 1.743 | 3,859,284 |

These are approximate process/log joins and all-thread process totals, not
PE-exclusive costs. H254 has different endpoints and no matched baseline;
do not interpret the per-frame or RSS differences as savings. Sampled RSS
is not the exact lifetime high-water mark. Ordinary desktop services were
active; no system-load control or repeated A/B was performed.
