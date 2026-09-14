---
domain: mini-replay-bisection
workload: 3DMark05 GT2 fixed Render Tape
title: "Strict GPU policy qualification and live/replay state localization"
type: experiment
status: investigating
updated: 2026-09-14
source: experiments/output/gpu-tape-qualification-20260910; experiments/output/app-d3d9-3dmark05-gt2-gpu-qualification-20260910-r17-state-hashes
related: specs/experiments/harness/replay/fixed-input-experiment.md; docs/perfomance/mini-replay-bisection/cpu-policy-measurement.02.md; docs/perfomance/mini-replay-bisection/fixed-input-replay.01.md
---

# Strict GPU policy qualification and live/replay state localization

## Verdict

Superseded for the ten retained final-pixel failures by
[the explicit vertex-position invariance result](gpu-position-invariance.04.md):
r585 passes all ten originals twice, with exact source/output bytes. This
document preserves the prior localization chronology, controls and exclusions;
it is not the current failure count. The fresh r593 public exact-policy
transaction now publishes successfully; r598 independently reauthenticates
it, and the final native/x86/TLA gates pass. The interrupted ENOSPC transaction
r588 remains preserved, and the headroom-policy negative remains rejected.
No original was replaced and no tolerance was changed.

### Historical verdict through r553

The retained vertex discrepancy now has a short arithmetic discriminator.
r465/r466 detailed observation changes 139 replay vertices to match the r456
original exactly and repairs its entire first-bad depth. It fails passive
baseline calibration. Thirty-four standalone controls (r469–r477/r480) reproduce
the difference using explicit skinning MAD fusion: separate multiply/add
matches the original; explicit FMA reproduces all 139 replay differences.
Comment/whitespace, scope-only and projection-only controls retain the failure.
This explains the immutable-input discriminator, not every original GPU read.

On retained r405, the single-draw intervention repairs first-bad depth but
leaves the same final one-pixel error. A default-off, digest-selected compiler
control extends its exact shadow prefix from nine to twelve endpoints
(r483/r486/r488). A four-source control then makes **all 32 selected shadow
endpoints exact** (r492/r494), while preserving the identical wrong final pixel.
The next scene control localizes a target-pixel intervention to one VS
(`bad49ade...`) and source22283's first draw, global1380 / original ordinal788346
(r499–r522). Its no-contract form makes the target's four bytes exact but
introduces 790 other differing pixels. Pass-end and per-draw ROI observations
preserve both complete final images. This is a concrete intervention target,
not the first original/replay semantic divergence or a production fix. On r346,
the four shadow controls leave all 84 scene endpoints and the 69-pixel residual
unchanged; changing its two first-bracket scene VS sources instead increases
the residual to 1751 pixels (r496–r498/r515). Strict qualification remains open
and all ten retained failures remain mandatory. No arithmetic production policy
or pixel-tolerance change is promoted.

The scene target now has a replay-calibrated single-draw fixture (r524–r553).
Both baseline and matched-intervention seeds reproduce their **entire**
1024x768 production after-draw images exactly. A 46-pixel inherited seed
difference is disjoint from the 14-pixel local arithmetic response. Twenty-seven
small controls localize the target response to the last skinning MAD
`mad r1, c2, r1, r2`; explicit FMA reverses it even under global contract-off.
API/shader validation and repeat controls preserve the calibrated images.
This gives a subsecond replay discriminator, not original GPU-input equality,
exclusive original root cause, or a repaired retained final image.

Paired existing color/depth endpoints are now directly observable (r401–r412).
New original r405 and same-build replay r406 agree through nine shadow passes;
the next eleven-draw pass first differs in both R32F color and depth (121 depth
texels, maximum 2 ULP). This original also has one final RGB pixel mismatch.
Removing earlier replay depth observations leaves the first-bad depth/color
and final bytes identical (r409), so those earlier copies alone do not explain
the residual. GPU policy qualification remains blocked; r405 is an additional
required failing original, not a replacement for the older nine. An observer-free
normal replay retains the identical wrong final bytes and is rejected by the
public gate (r411/r412).

R414–r429 reduces that intermediate replay difference to one indexed draw and
then three triangles. A 1,501,552-byte standalone input with the unchanged
captured MSL reproduces all 121 replay depth values; triangles177/1194/1195
account for 68/51/2 coordinates respectively. Cold repetition and removal of
Metal API validation preserve the three-triangle depth/color bytes. This is a
small diagnostic discriminator, not the production replay path, original
GPU-input proof, or a fix for the final pixel.
R431/r438–r441 additionally recover seven standalone GPU vertex positions
without changing its full depth/color readbacks. Duplicate invocations agree
bit-for-bit. Four caller floating-point rounding modes preserve the same
positions and images. This excludes that narrow standalone caller-fenv
mechanism, not the original Wine/GPU execution context.

Actual direct-uniform bind/cache coherence now has a skipped-rebind stale-pixel
negative and production three-draw coverage. On retained r346, r373/r375/r376
each report 7,412 actual/cache matches, identical observed bound bytes and all
84 identical replay scene hashes, yet retain the same 69-pixel error. Metal
API/GPU validation reports no access error; forcing dedicated transient
buffers changes the real allocation-count signature but not the error. These
are same-build replay interventions against an older original, not original
binding or GPU-time-content proof. Replay-side missing rebind or slab reuse
alone is not supported as the cause. Strict qualification remains open.

Actual depth/stencil creation/binding/reference observation now has native/x86
pixel and production-row coverage (r353–r367). Two new GT2 originals r359/r364
are exact final-pixel positives with 1,857/1,859 equal issued depth descriptors;
they do not replace any retained failure. The untouched r346 input still gives
the identical 69-pixel failure on the new diagnostic build (r363). No rendering
policy or tolerance was changed. Original GPU-consumption inputs in the failing
bracket remain the next missing discriminator; passing depth pairs cannot
retroactively exclude a depth/content issue in r346.

The next bounded 2D-shadow capture r379 timed out before its 500-Present gate
and produced no Tape. A retained process sample finds replay Arena admission
and the exact open-session/empty-Ready encode wait. r383–r389 reduce and repair
a reachable page-latch composition defect with native and TLA counterexamples.
The repeated capture recipe now finishes and publishes r390; r391 replay has
exact final pixels but an authenticated intermediate R32F shadow difference.
Repeated replay r395 reproduces r391's exact shadow bytes and exact final
RGBA. The r393–r396 audit localizes the producers to 395 draws and excludes
the observed FFP material/light byte differences from their selected MSL
entrypoints and reachable helpers. This is source-level exclusion, not
GPU-time-read or driver-instruction proof; the first divergent original shadow
writer endpoint was still unobserved in that pair. A new bounded r397/r398
pair now narrows the first shadow hash divergence to source22267, eleven
draws260–270 after eleven equal stored endpoints. This frame's final RGBA is
also exact; neither positive replaces the retained final-pixel failures.
Exclusive live attribution remains open. Treat r379 as failed capture, not a positive
based on the catalogue's timeout-permitting `pass`. The repaired build's r387
replay still rejects the identical r346 69-pixel residual.

Actual texture-object/content joins are now available (r336–r350). Fresh
original r346 and replay r347 retain a 69-pixel failure; they provide 7,573
paired actual texture and sampler descriptors each, with no descriptor
differences or unknown entries under the expected stage mask. Six paired
512x512 R32F cube readbacks have the same 2,168 differing float texels.
Process-local object tokens, canonical generation, source/pass/draw joins and
explicit no-writer intervals qualify this as input-content evidence, not
sampled-texel or GPU-read proof. The first scene divergence precedes cube
sampling. Prefetch-off r349 makes the first-bad eight draws' actual PSO
creation inputs equal to the original, but leaves both the final wrong hash
and cube differences unchanged. All old originals and r346 remain open.

Actual fragment sampling descriptor observation now covers direct binds and
bounded argument-table writes (r328–r335). The native direct/table mip-view
pixel fixtures and invalidation negatives pass. Unchanged originals r263 and
r300 retain exactly the same wrong outputs, with 7,617 and 7,519 expected-mask
stage entries carrying actual direct texture/sampler descriptors. These are
replay-side coverage counts, not paired-original or GPU-read proof. Original
sampling receipts, resource identity/content joins and strict pixels remain open.

Actual selected-PSO creation lineage is now available (r310–r327): the trace
authenticates the source/function/descriptor associated with the actual bound
Metal pipeline, not its mutable label. Two new live/replay pairs are exact;
they do not replace retained failing originals. On the unchanged r263 input,
disabling prefetch changes the actually selected VS source on 872 draws but
preserves exactly the same wrong 72-pixel output. The first-bad four draws'
creation receipts are identical across that intervention. Prefetch-selected
source differences alone do not explain this failure. Actual texture/view/
sampler binding and GPU-time resource contents remain open.

Earlier same-build r300/r303 extends actual issued geometry to extra vertex
streams: all 107 extra-stream ranges and 1,884 indexed stream-0/index ranges
match, yet two final one-unit RGB bytes differ. Observer-off r308 completes two
cold iterations with the same wrong SHA. The first observed scene difference
is source 22190's eleven draws, after raw-exact source 22189. This rules out a
CPU-visible extra-stream mismatch for this pair, not selected compiled PSO,
actual texture/view/sampler binding or GPU-read/later-mutation differences.
Original r263 also retains its identical 72-pixel failure under the new observer
(r299); none of the retained counterexamples is closed.

Earlier failing pair r263/r264 retains complete sampled-input bytes and differs
at 72 final pixels (94 bytes, maximum RGB delta four, alpha exact). Its first
observed scene RGB difference is source 22237, four draws, after an exact
same-scene endpoint. A source-qualified wire audit connects the produced R32F
readback to that bracket without an intervening write: 1,415 positive finite
float values differ by one to four ULPs. This is not yet causal proof. Earlier
positive r261/r262 has 209 differing R32F values but exact final output.
Controlled intervention r274–r286 now rejects those R32F differences as a
sufficient explanation at source 22236: baseline, replay-input control and
original-input replacement retain exactly the same 72-pixel failure. The
replacement reads back byte-exactly and persists in ten selected-view dumps.
A zero-input sensitivity control changes 515,132 pixels, confirming a real
downstream effect. This narrows the hypothesis; it does not prove all actual
Metal bindings or resolve the earlier four-draw scene bracket.
Observer-off r266 reproduces the same r263 final bytes once; the provider
rejects that first iteration, so the requested second repeat is not completed.
All earlier failures remain open. No original or tolerance was replaced.

Earlier failing pair r254/r255 has 132 authenticated scene-sized stored
endpoints. The first observed RGB difference is captured source 22693,
encoder 0, eleven indexed draws (event 775, records 23–33); source 22692's
same scene endpoint is raw-exact. Final scene differs at one pixel and final
output at three pixels. Observer-off cold repeats r256 reproduce identical
wrong bytes twice. The wire audit establishes the scene's complete seed,
full Clear and final-composite read, but not actual GPU identity/inputs:
the candidate shader reads a produced 2048² R32F texture whose preceding
371-draw original output was not retained. This is a narrowed investigation,
not strict GPU qualification or a root-cause finding. Details follow below.

The cold source-boundary diagnostic now reproduces one original GT2 capture
exactly: all 230 observed physical pass starts and both source/output RGBA
bytes match. Turning it off with the same rebuilt provider restores the
1,749-pixel mismatch. Two further cold-device iterations without effect tracing
also match. A second older captured frame improves from 1,453 mismatching
pixels to six channel-byte differences of one, but still fails strict equality.
r53 restores all 15 missing intra-source DrawRun boundaries (836 command starts
versus 821 before), without changing the 224 replay physical pass starts. The
six-byte residual is unchanged, excluding that command partition difference
under this source-cut regime. The shared cold authenticated source-plan builder
now serves the diagnostic and the shared normal CPU/GPU source driver.
Ordinary compatibility/ProductionSlot GPU policy qualification is now joined
for the r65 input through the explicit v2 source contract (r103–r107 below).
A successful diagnostic is still not an accepted timing baseline, and the
older r28 strict-fidelity counterexample remains open.
No oracle was replaced and no tolerance was increased. These runs diagnose
correctness; they are not FPS or low-observer policy measurements.

A second trace-complete capture now also matches exactly (r65/r66): 1,379
issued draws, 136 source ranges and all 149 physical pass starts. The same
binary/input without source cuts differs at 6,943 pixels (r67); two cold-device
trace-off repeats retain exact original bytes (r68). The older r28 six-pixel
counterexample remains unresolved and is not superseded by these positives.
The typed queue-publication implementation also retains those exact bytes in
two cold-device iterations (r75). The normal driver now independently preserves
those original bytes on both ordinary routes (r91/r92), including 133 Direct
leases and 1387 Direct draws. Normal Direct also retains exact r36 pixels
twice (r95). These runs precede the public v2 policy join, and r28 still has
the same six-pixel residual on normal Direct (r94).

The working tree is based on `e47f9a83`. The optional
`cpu-policy-compare --gpu-qualify` gate authenticates the original source and
output bytes, runs two real-GPU iterations per canonical policy, and only
then permits CPU timing-only children. Final binary/image/receipt/readback
reauthentication can reject the aggregate even after measurement. CPU-only
comparisons retain their explicitly CPU-only scope.

## Fixed-input discriminators

Native artifacts are under
`experiments/output/gpu-tape-qualification-20260910/`.

| Input and run | Result | Interpretation |
|---|---|---|
| Historical `gt2-v2-20260909-r4-capture64`, `gt2-head-compat-r1` and `gt2-head-direct-r2` | Same replay SHA `91e03763…`; 2,378 differing pixels, max RGB delta 16, total 3,704 | Shared capture/replay failure; Direct executes 691 draws plus 729 compatibility draws |
| Fresh HEAD capture r4, `gt2-fresh-compat-r5` | 1,540 pixels, max 18, total 4,099; SHA `a88359a6…` | Fresh capture does not remove the mismatch |
| Same fresh input, r6 full constant uploads; r7 hybrid argument buffers disabled; r8 PSO prefetch disabled; r9 traditional backend; r10 mid-chunk commits off | All reproduce the exact r5 SHA | These selected perturbations do not fix the input; identical output alone does not prove each mechanism activated |
| Same input, `gt2-gpu-aa-r12` | CPU verification passes; both GPU sides reject; `measurement={}` | Actual fail-before-measurement evidence, not positive GPU qualification |
| Effect-traced fresh capture r13, `gt2-effect-compat-r14` | 2,268 aligned issued draws; 2,786 pixels, max 21, total 5,476 | Sampler, texture shape/format/LOD and traced blend/depth state agree |
| State-family traced capture r17, `gt2-state-hashes-compat-r18` | 2,196 aligned issued draws; 2,414 pixels, max 33, total 5,330 | More precise state localization below; still rejected |
| Selected-payload capture r28, `gt2-payload-bytes-compat-r29` | 2,267 aligned issued draws; all active uniform bytes/counts match; 1,453 pixels, max 27, total 3,548 | Run-level hash drift is not selected-payload drift; still rejected |
| Effective-binding capture r36, `gt2-effective-bindings-compat-r37` | 2,257 aligned issued draws; post-override slot 5 and observed slot 3 viewport/clip fields match without missing rows; 1,749 pixels, max 30, total 3,848 | Live has 230 physical pass starts versus replay's 38; replay starts are a strict subset, not yet a causal pixel attribution |
| Same r36 input, `gt2-source-boundaries-r44` | 217/217 cut requests; all 230 physical pass starts match by issued-draw ordinal; exact original source/output bytes | Source-boundary-preserving diagnostic restores capture fidelity for this frame |
| Same rebuilt provider/input, `gt2-boundaries-off-r45` | Original r37 SHA and 1,749-pixel mismatch reproduced | Default-off behavior preserved; this is a controlled input/binary comparison |
| Same r36 input, `gt2-source-boundaries-repeat-r46` | Two cold-device iterations, effect trace off, both exact original SHA `85b23cf4…` | Diagnostic fidelity is repeatable without the inline effect observer; not policy promotion |
| Older r28 input, `gt2-source-boundaries-r28input-r47` | Two cold iterations agree; 210/210 cut requests; six differing pixels, maximum RGB delta 1, total 6 | Major mismatch reduced, not strict fidelity; existing pixel-envelope acceptance is insufficient for qualification |

Different captures select different frames. Their pixel-difference counts
must not be interpreted as a numerical improvement or regression. In each
tested frame the source before Present and final replay output have identical
bytes, localizing the mismatch before Present. Historical x86_64 and ARM64
replays also agree; ARM-only floating-point behavior is not established.

## Live/replay state localization

Capture r17 is
`experiments/render-tapes/gt2-gpu-qualification-20260910-r17/frame-1374678625023200-1`.
Its identity sidecar selects 221 source rows, seq 7669–7889. Filtering the live
effect trace by those exact source IDs before aligning issued draw order gives:

| Traced field | Mismatching issued draws |
|---|---:|
| PS hash, vertex declaration, render state, viewport, world-view-projection, texture transforms, clip planes | 0 |
| Run-level VS constants hash | 210; first zero-based ordinal 107, live seq 7680 |
| Run-level PS constants hash | 2; ordinals 830 and 874, live seq 7754 and 7758 |
| FFP VS key | Final 10 draws |

Run-level constant hashes are not a byte oracle: a DrawRun can retain its
first hot key while a later draw selects another immutable uniform payload.
Additionally, ordinary and incremental constant hash algorithms have distinct
domains. The next cold effect trace reads the selected payload's hashes,
counts and exact float/int/bool arrays; it does not add diagnostic state to
production draw ownership or perform work when the existing trace is off.

The r28 bundle (`gt2-gpu-qualification-20260910-r28/frame-1376845826392000-1`)
then closes that discriminator: all 2,267 aligned issued draws across 210
sources (seq 8708–8917) have identical selected VS/PS payload hashes, active
float/int/bool counts and exact active bytes. Render state, viewport,
declaration, transforms and clip planes also agree. Run-level VS/PS hashes
still differ at 218/6 draws, and the last ten FFP VS keys differ, without any
selected programmable uniform discrepancy. This excludes stale selected
programmable constants for this trace, not shader binding, geometry, resource
contents or pass-boundary effects. Raw handle values are intentionally not
compared across processes. The retained `live-replay-payload-comparison.json`
and `compare_payload_traces.py` under the artifact root reproduce the join.
Original and replay before/after Present readbacks differ at 1,453 pixels
(maximum RGB delta 27, total 3,548, alpha 0); source/output replay SHA is
`3a3ef308…`. Compatibility executes 2,285 draw records in 71 batches with
seven unbatched draws, 26 encode chunks and zero GPU errors. Record coverage
is not interchangeable with the 2,267 actually issued draw observations.

The first full-array byte trace r23 ended the scene at 186 Presents, below its
skip-200 selector, so it published no tape. r25 reached skip-160 but repeated
`identity_query` rejection. Both unsuccessful attempts are retained. The
subsequent cold logger uses active spans and direct hex conversion instead of
per-byte stream formatting; r28 reaches capture with the original skip-200
selector. No failed run is an FPS measurement or positive capture receipt.

r34 published 136 sources at seq 12398–12533, outside its 6500–12000
trace window. It is a valid capture receipt but not complete live/replay
geometry or pass-boundary comparison evidence. `identity_query` is a collapsed
PE abort category, not a diagnosis: Unix finish can reject drain, exact-tail
settlement, ledger acknowledgement or copy preconditions. The queue settlement
wait has no timeout; an effect-trace window does not directly control capture
admission. A repeat must validate coverage against the successful sidecar.

A separate review identified an observation gap: shared render state does not
include the resolved per-draw alpha override. `encodeDraw` binds a post-override
`FsVolatile` at fragment slot 5, while the previous effect trace only observed
the shared state. All programmable vertex shaders also consume locally derived
half-pixel/viewport/clip fields in `FfpVsConsts` at slot 3. Neither is proved
equal by matching selected VS/PS payloads. The new default-off
`DXMT9_EFFECT_DRAW_TRACE_BINDINGS` observes those existing values at binding
points (including clean argbuf cache repoints); it does not recompute a proof
copy or change the rendering policy. Large programmable payload byte output is
now separately gated by `DXMT9_EFFECT_DRAW_TRACE_PAYLOAD_BYTES`. Host floating
point control remains a hypothesis until equal raw inputs produce unequal
actual derived bytes.

The r36 bundle is
`experiments/render-tapes/gt2-gpu-qualification-20260910-r36/frame-1378569056846700-1`.
Its 217 capture sources (seq 8362–8578) are fully inside the 6500–25000
trace window. r37 aligns 2,257 issued draws and reports no missing or unequal
post-override `FsVolatile` bytes or observed `FfpVsConsts` half-pixel, viewport,
clip-plane and clip-mask fields. This closes the alpha-immediate/derived-field
discriminator for that frame, not all FFP contents or actual GPU buffer memory.
Programmable byte output is deliberately off; its hashes/counts still agree,
while r28/r29 remains the separate exact-programmable-byte evidence.

The same trace establishes 230 live physical pass starts versus 38 in replay;
all replay starts are also live starts, with the first additional live boundary
at issued draw ordinal 11. `ActiveEncoderBreakdown::begin` is reached at actual
render-encoder opens, so this is not merely differing source ID labels.
`live-replay-bindings-comparison-bounded.json` preserves the complete boundary
lists and binding coverage. Its source-filtered parser agrees with the original
unbounded comparison; no missing bytes are treated as matching observations.

This exposed a harness limitation subsequently isolated by r44/r45 below.
`tools/dxmt9_render_tape_provider.cpp` validates/reports `--identity`, but
`device_c_render_tape_provider.cpp` replays each canonical tape chunk through
`replayPrevalidatedResolvedCommandChunk` without consuming the sidecar's
event-local source record intervals. Compatibility disables Arena/direct-slot
admission and uses provider-local source grouping. `--partition-mode identity`
selects scheduler policy; it does not mean capture-identical execution cuts.
Production-slot also generates fresh local source identities. Before the new
diagnostic, no option reconstructed all captured physical boundaries. A source
count alone still cannot establish physical pass equality: 217 source intervals
must not simply be called 230 physical passes.

r36 completed the catalogue lane with status pass, exit 0 and no timeout.
r37 remains output-mismatch with zero GPU errors, 2,275 draw records in 70
compatibility batches, seven unbatched draws and 25 encode chunks. Its replay
source/output SHA is `507fb32a568b2e43599f77790b0d27d7fa2ca46cfa53956e7bd2f4266871fbdc`;
both differ from the preserved original at 1,749 pixels, with alpha unchanged.
The full diagnostic log is large and the launcher postprocessing is slow;
these runs are not low-observer timing samples.

## Source-boundary discriminator and residual

`--audit-source-boundaries` revalidates Capture-authority `identity.bin`
against the prepared tape and verified blob catalogue, and plans an exact
cover of each event's source record ranges before effects. The same raw owner,
ledger-assigned replay ordinal, snapshot retention and production record sink
execute each range once. Each range requests queue publication without a
per-source GPU completion wait. State-only ranges complete without creating
empty GPU work. Errors stop the remaining stream; allocation exceptions
close the active transaction journal and preserve the completed prefix count.
There is no whole-raw fallback after a prefix executes.

r44 uses the r36 bundle unchanged. Its 217 source publication requests produce
218 encode chunks; these counts are not equated to the 230 render-pass starts.
The retained `live-replay-bindings-comparison-corrected.json` separately proves
all 230 physical starts match the original issued-draw ordinals, with 2,257
draws aligned and no missing/different effective binding fields. Direct `cmp`
of original and provider source/output RGBA also passes; all four files have
SHA `85b23cf4372d9a67a664f0acd1571d10f858dd35d7637b0d932cf4dd01031bf6`.
r45 uses the identical provider binary with the diagnostic off and reproduces
r37's exact wrong SHA and 1,749-pixel difference. r46 repeats the successful
diagnostic twice with effect tracing off, retaining both independent readbacks.

This attributes the large discrepancy to the source-boundary replay regime
for this fixed input, not specifically to a proven Metal precision mechanism:
the regime changes queue publication, batching and physical pass cuts together.
The queue API itself does not promise a physical encoder end when session
coalescing is enabled; actual observed cuts remain required evidence.
The older r28 input in r47 is the counterexample to generalizing success:
both cold iterations reproduce SHA `e62abb80…`, with six one-unit RGB
differences and alpha equality. Its ordinary r29 path had 1,453 differing
pixels. No tolerance is widened and the existing envelope match does not
qualify this residual as strict correctness.

r48 repeats that residual with the full selected-payload trace enabled.
All 2,267 VS/PS float/int/bool active byte spans still equal the original
capture trace. The older live logger did not record physical pass IDs or
effective slot 3/5 bindings, so those comparisons are explicitly unobserved;
`live-replay-comparison-compatible.json` reports missing live bindings rather
than counting them as equal. `exact-pixel-residual.json` in r47 preserves all
six coordinates and original/replay RGBA values. No new live capture was
needed for these fixed-input discriminators.
The rebuilt x86_64 provider in r50 produces the same residual SHA and six
one-unit differences, excluding an ARM64-only replay difference for this
source-boundary diagnostic as well.

### Intra-source batch-cut discriminator

The live/r48 join identifies 15 lost DrawRun starts. Actual indexed-draw fusion
is not active: pair attempts and selected pairs are zero, and all 2,267 issued
draw primitive counts align. The cold `--audit-batch-cut EVENT:RECORD` driver
restores only the corresponding batch flushes, retaining source publications.
It uses existing production sink subranges within the same source journal;
no new hot-path diagnostic parameter, queue publication or explicit encoder
termination is added to the ordinary draw loop.

| Canonical event ordinal | Zero-based flush-before record ordinals |
|---|---|
| 293 | 11, 48 |
| 687 | 28, 51 |
| 791 | 34 |
| 907 | 41, 54 |
| 919 | 17, 54 |
| 1012 | 19, 39, 58 |
| 1024 | 11, 22, 33 |

r53 (`gt2-source-batch-cuts-r53`) executes 15/15 batch cuts and 210/210 source
publication requests. All 836 live DrawRun starts now match exactly (r48 had
821); command index, subdraw/count and run-level VS/PS constant hashes have
no live mismatches. Selected programmable payload bytes remain equal for all
2,267 draws. Physical pass starts are independently compared between r48 and
r53: both have the same 224 start ordinals. Original r28 physical pass ownership
remains unobserved, not inferred from either source or command starts.

Source/output bytes are identical to r48 (`e62abb80…`) with exactly the same
six one-unit channel differences. The command partition and run-level hash
drift are therefore excluded as the cause under this source-cut regime.
`partition-discriminator.json` preserves input/log hashes, complete start
arrays and exact residual coordinates; `live-replay-comparison.json` preserves
the active payload comparison. No capture input, oracle or tolerance changed.

r54 uses the same rebuilt provider without batch cuts and without tracing:
both cold iterations reproduce the same residual SHA. r55 replays the already
exact r36 input with the shared source planner and remains exactly equal to
original source/output SHA `85b23cf4…`, with 217/217 source requests. All these
diagnostic receipts remain `accepted=false`, including the exact r55 case.

The pure `CaptureSourceReplayPlan` owns authenticated source rows and a
versioned canonical digest, binds planned chunk bytes/event order to the tape,
and invalidates moved-from authority. It describes queue-publication cuts,
not physical Metal passes. The diagnostic now consumes this plan; normal CPU
and GPU policy execution have not yet adopted a shared cut contract. Native
tests enumerate every source partition and interior-cut subset over 1..7
records, reject stale bytes/event order and invalid cuts before effects, and
execute 31 local draw flushes with exactly three source publication requests
and the independent clipped-Clear pixel oracle. Integrated native/provider/
CPU/GPU CLI tests pass 8/8 in `batch-cut-integrated-r56.txt`.
The full x86_64 build and five focused provider/GPU/install-name gates also
pass (`batch-cut-x86-build-r57.txt`, `batch-cut-x86-integrated-r58.txt`).
All 188 existing TLA configurations pass in `batch-cut-tla-r57.txt` (170.89 s).
These prove the reused lifecycle/model gates, not the six remaining pixels.

The diagnostic remains default-off and excluded from policy ranking, even
for the exact r36 result. Native exact-cover enumeration (all partitions of
1..7 records plus gap/overlap/zero/overflow counterexamples), malformed/stale
sidecar rejection before object creation, and real 36-range state/Clear/Draw/
Present execution pass in `source-boundary-integrated-r43.txt` (8/8 integrated
tests). The fixture's draws are clipped; its independent oracle proves
partial-Clear/control ordering, not visible programmable geometry. The
existing TLA suite passes in `source-boundary-tla-r41.txt` (168.80 s).
Neither these models nor the bounded fixture proves the remaining six pixels.
The initial native r42 failure on a state-only source and the corrected
verification receipts remain preserved.
The full x86_64 build also passes, followed by provider, Python policy gate,
actual GPU CLI gate and install-name audit (4/4 in
`source-boundary-x86-integrated-r49.txt`).
The final TLA rerun also passes (163.94 s); all configuration output, including
the required counterexample checks, is preserved in
`source-boundary-tla-final-r50.txt` rather than only the mutable Meson test log.

The FFP key difference is associated with bootstrap defaults: PE light slots
and material are zero initialized, whereas native defaults contain directional
light types and white diffuse material. Those lights and lighting are disabled
in the inspected trace. The shader skips them, so this is not yet evidence of
the pixel cause. Fixing the key alone would not establish fidelity.
`core_ffp_state_key_spec` now compares actual production vertex-source strings
under every enable mask and each supported light type with lighting off, plus
all two-bit material-selector combinations. Active-light type/mask/material
positive controls do change source. This pins inactive lighting fields as
source-inert in the tested context, without changing shader cache lookup,
uniform upload, Metal work or the original oracle. It does not exclude an
unobserved binding/cache defect. The final native build and nine integrated
tests pass (`batch-cut-final-build-r59.txt`, `batch-cut-final-integrated-r60.txt`);
the final x86_64 build also passes (`batch-cut-final-x86-build-r60.txt`).
Its final FFP/provider/GPU/install-name gates pass 6/6 in
`batch-cut-final-x86-integrated-r61.txt`.

Capture r15 succeeded but its selected source interval (7049–7191) fell below
the configured trace minimum 7500; it is not a complete state comparison.
The r16 launch failed during automatic cross-build because SDKROOT was absent,
before running Wine. r17 reused the previously staged r15 binaries only after
checking their PE/provider hashes; the wrapper records `staged_build.skipped`.
Future builds use the configured Xcode SDK explicitly. No failed evidence is
relabelled as a successful retry.

## Independent partial-Clear counterexample

The new native GPU policy CLI fixture exposed a backend defect: full clear
`0xff102030`, then rectangle `(4,4)-(12,12)` clear `0xff506070` on a 16x16
target produced the second color over all 256 pixels. Exactly 192 exterior
pixels differed (max RGB delta 64, total 36,864). The independent analytic
source/output oracle SHA is `bc8f709e…`; the wrong uniform output is
`98ac8b85…`.

The canonical record, replay dispatch and Clear snapshot preserve the
rectangle. `encodeClearPassImpl` discarded it and used full-attachment load
action Clear. This is a production backend defect, not malformed tape or a
reason to weaken the gate. All nine Clears in the inspected r13 GT2 frame
have zero rectangles, so this does not explain that frame's mismatch.

The fixture contains 32 indexed draws with changing constant payloads, but
their geometry is clipped. Its nonuniform analytic image proves partial
Clear and gate control flow, not visible draw/uniform rendering. Its explicit
`test_input` scope must never be reported as production capture fidelity.
Failed CLI fixtures retain their original oracle, readbacks and receipts.

After rectangle encoding was implemented, r19 exposed a second defect:
`makeGenericFragmentSource` emitted integral float values as invalid MSL
tokens such as `1f`, so the fill pipeline library failed and the inset draw
was skipped (64 interior pixels differed). Classic-locale, round-trip-precision
float literals with a decimal point fix this; native source tests include
signed-zero and depth values. r20 matched the exact analytic pixels but its
two policies did not change physical batching, so its overall CLI test was
correctly rejected rather than accepted as policy evidence.

The r22 fixture alternates state on every indexed draw. Actual A/A and A/B
comparisons now pass: candidate command-limit 4 produces strictly more encode
chunks than reference limit 0, while both reproduce the independent original
source/output bytes in two real-GPU iterations each. The complete CLI test
took 4.20 seconds (`build/meson-logs/gpu-policy-positive-r22.txt`). This closes
the bounded synthetic gate, not GT2 capture fidelity or visible draw binding.
The separately added attachment oracle failed an incorrect composed-MSAA
expectation; its mip oracle also used 16x16 indexing for an 8x8 readback.
These test defects must be corrected before reporting partial-Clear attachment
coverage. Failed artifacts and logs remain available.

The corrected r24 oracle exposed a real second-clear MSAA failure: full Clear
used resolve-only store and discarded the multisample source before a later
Load. Full and clipped Clear now share an allocation-free store predicate
that preserves source samples and resolves when needed, pinned by its complete
two-case native truth table. The r27 Metal test passes (1.57 s): multiple
clipped MRT rectangles, mip and cube-face isolation, overlapping MSAA partial
clears, and independent depth/stencil aspect readbacks. The D24S8 fixture
explicitly selects Depth32Float_Stencil8 on Apple Silicon and compares every
float32 depth value exactly (0.25/0.75), stencil preservation, and low-byte
truncation (`0x1a5` to `0xa5`); it does not infer a packed physical layout or
loosen a depth tolerance. r26's invalid Depth24Unorm_Stencil8 default on M1
was a fixture capability error, corrected before accepting r27. The existing
188-configuration TLA suite also passes in r30 (179.54 s); those lifecycle
models do not prove these attachment pixels. Clear clipping/store policy is
bound separately through exhaustive native value checks and real readbacks.

## Remaining acceptance gates

### Latest capture and source-policy prerequisites

The r64 runner completed but produced no tape. Its PE log reports
`arm enabled=1 producer=0 publisher=0` and `requested without artifact
publisher; capture remains off`. The launch passed a host POSIX output path;
`safeRootText` requires a PE-visible absolute root. This was an invalid capture
invocation, not a GPU-fidelity result. R-HARN-REPLAY-7.12 already specifies the
Windows drive form; no path-safety rule was relaxed. The failed invocation's
logs and runner receipt remain preserved.

Corrected r65 uses the verified prefix Z: mapping and publishes
`experiments/render-tapes/gt2-gpu-qualification-20260911-r65/frame-1384072645252700-1`.
Earlier attempts in the same run reject `identity_query`; only the completed
bundle is authoritative. Its 136 source rows span seq 12022–12157, entirely
inside the configured 6500–18000 trace interval. Replay r66 reproduces all
1,379 issued draws and all 149 physical pass starts. The effective slot-5 and
observed slot-3 binding joins have no missing/differing rows. All 1,379 indexed
geometry discriminator rows agree; those CPU hashes are not full GPU-memory
proof. Selected active payload bytes were intentionally not logged and remain
unobserved, not implicitly equal. Both original source/output readbacks are
exact SHA-256 `65a34bf516b41bdd32014043bc0ef28b494719ea21eaba3f23b57755a5421c57`.

The same input/binary with source cuts disabled (r67) differs at 6,943 pixels,
maximum RGB delta 14 and total 12,072. Its SHA is
`365fcf1457c770af5e195c109f1d937be47976725da001e132de9ab14fe90434`.
Trace-off source-cut r68 runs two cold-device iterations; both retain exact
original source/output bytes and complete 136/136 requested cuts. All three
source-cut receipts remain diagnostic and `accepted=false`. The failed
standalone validator CLI attempt used an unsupported `--blob-root` option;
the canonical experiment wrapper then performed the actual digest-backed
validation. No FPS claim comes from these observed runs.

Source-aware ProductionSlot execution must not call the whole-raw span driver
independently per source: its pre-effect fallback replays the **entire raw**.
The new pure borrowed range planner preserves full handles/payload offsets and
returns raw-absolute record locators without copying the canonical wire. The
normal driver described below now uses it. The typed `publishReplaySourceBoundary`
seam now separately reports Published, Empty and Rejected; the existing
diagnostic is a thin wrapper rather than a second publication implementation.
Published carries the actual queue-issued source/storage/sequence receipt;
empty settlement never fabricates one. Direct span commit normally leaves a
writer open; Present is the
exception and owns its own publication. The actual Tape raw high-water rejects
repeated nonzero raw ordinals even in fresh slots. The new reference contract
R-HARN-REPLAY-8.14 therefore separates private raw/capture provenance from
queue-issued source metadata; it does not relax production high-water rules.

The typed seam rejects active leases, malformed/stale source/storage/sequence
witnesses and poisoned queues before publication. Native tests exercise two
ordinary Direct publications from the same private raw, actual completion and
reclaim, ordinary Clear, already-published Present, and failure after effects.
The range-planner tests enumerate every interval in a mixed state/Clear/UP/
query/Draw/Stretch/Present fixture, preserving full handle/payload offsets and
range-local lease metadata without changing the raw bytes.

Native full build r73 and x86_64 full build r74 pass. The integrated native
gate is 9/9 in `source-boundary-integrated-r74.txt`; x86_64 range/routing/
provider/source-plan/install-name gates pass 5/5 sequentially in
`source-boundary-x86-integrated-r76.txt`. Existing TLA verification
passes in 173.26 seconds (`source-boundary-tla-r71.txt`), with its full output
preserved separately in `source-boundary-tla-full-r71.txt`; this is reused
lifecycle evidence, not a new end-to-end source-policy proof. The failed r69
compile and r72 fixture assertion remain retained alongside the corrected
receipts.

Rebuilt diagnostic r75 (`gt2-typed-boundary-r75/result.json`) uses the r65
input with effect tracing off and two cold-device iterations. Both complete
136/136 source-cut requests with zero GPU errors. Independent file hashes of
all four saved readbacks equal the original source/output SHA `65a34bf5…`.
The provider exits zero; the experiment wrapper exits one because this is
diagnostic and `accepted=false`, not because its strict pixel check failed.
Its observed route is compatibility with zero Direct leases, so no
ProductionSlot or policy-ranking acceptance follows.

### Normal source-aware driver and real-Metal evidence

`replayPrevalidatedResolvedCommandChunkBySources` now uses the same complete
raw owner across authenticated record ranges. It plans all ranges and ordered
controls before effects; both pre-effect fallback sites replay only the current
range. Actual queue Ready receipts include implicit command-limit publications
and Present, not just explicit flush requests. Source callbacks run after the
queue lock is released. The CPU executor reauthenticates tape/sidecar/plan,
drains exact Ready receipts, leaves bootstrap on whole-chunk replay, and avoids
double-counting nested completion time. Cold admission caps one raw at 4096
records and receipt storage at 8193 entries; CPU queue credit is separately
bounded.

The public experiment mode is `--source-boundary-mode capture-source`, with
cold-device reset and an authenticated capture sidecar. It is not a diagnostic,
but cannot qualify an existing v1 policy: that schema does not bind boundary
mode and source-plan digest. Root and per-iteration acceptance guards reject
nondefault source mode, including a forged false `requested` flag with residual
source evidence. No diagnostic receipt is relabelled.

All rows below are trace-off, two cold iterations with original source/output
oracles; counts are per iteration. Draw path counters count submitted work and
must not be conflated with the earlier trace's issued-draw population.

| Input / run | Actual path | Source publications | Original pixel result |
|---|---|---|---|
| r65 / r91 | 1397 compatibility draws, no Direct | 136/136 | Exact source and output, SHA `65a34bf5…` |
| r65 / r92 | 133 Direct leases, 1387 Direct + 10 compatibility draws | 136/136 | Exact source and output, SHA `65a34bf5…` |
| r36 / r95 | 214 Direct leases, 2265 Direct + 10 compatibility draws | 217/217 | Exact source and output, SHA `85b23cf4…` |
| r28 / r94 | 207 Direct leases, 2275 Direct + 10 compatibility draws | 210/210 | Same six differing pixels, max RGB 1, total 6; SHA `e62abb80…` |

All GPU error counters are zero. Saved r91/r92/r95 readback hashes were checked
independently against the untouched original files; r94 matches the retained
six-pixel counterexample hash. `provider_exit_code=0`
in all four runs means execution completed; r28 still uses the historical
pixel-envelope result and **fails strict qualification**. The wrapper returns
one with `diagnostic=false, accepted=false` for every normal source run until
versioned policy receipt authentication is connected. Direct post-reserve
growth remains observed (38/61/62 for r92/r95/r94); these are correctness runs,
not zero-allocation or FPS evidence.

Native full build r88 and x86_64 full build r88 pass. Focused native r89 is
7/7, including actual Metal route counters and the independent clipped-Clear
oracle; x86_64 r93 is 6/6. Existing TLA r90 passes in 172.67 s, with full output
retained in `source-policy-tla-full-r90.txt`; it is reused queue refinement,
not a new whole-policy proof. Initial r81/r82 routing timeouts were the
no-worker fixture reaching Present pacing before controlled completion. The
saved r82 stack establishes that wait; isolating this fixture under the CPU
harness's existing pacing pins fixes it without removing production waits.
The r85 compile and r87 missing-counter fixture failures remain retained.
Public provider CLI, strict GPU policy Python and synthetic GPU A/A + A/B CLI
pass 3/3 in r97. The earlier r96 CLI failure was a stale expected identity
projection list: source-audit/normal-source fields now participate in both
the actual projection and the exact test expectation.

### Public v2 policy join and fixed-input qualification

`dxmt9.render_tape.cpu_policy.v2` adds capture-source boundary mode and an
expected source-plan SHA to the unchanged five v1 fields. V1 remains exactly
the historical chunk policy. The CPU CLI now consumes the same authenticated
plan as the GPU provider, including verify and low-observer measure modes.
Both retain full source rows, explicit empty/Present settlements and actual
queue-local source/storage/sequence publications. Python reconstructs the
native domain/version-qualified plan digest from the rows and original
components, rejects stale/truncated/reordered receipts, and joins CPU
publications to the actual completion trace. Final reauthentication requires
the caller's expected reference/candidate policies rather than deriving them
from stored receipts. Readback/effect-trace observers cannot qualify even if
their diagnostic flag is hidden.

The native identity inspection of r65 has 136 sources in 41 canonical command
events; the largest range has 64 records. Independent digest reconstruction
matches `324e90227ffde90f6e0ee77f392a3470790901ae37008add43f45e5243731c18`.
Thus the CPU queue bound is met without modifying the captured input or cuts.

| Evidence | Result |
|---|---|
| `source-policy-build-r98.txt`, `source-policy-build-r101.txt` | Full native builds pass |
| `source-policy-tests-r102.txt` | Capture-source, real Metal provider, source-plan and Python gate: 4/4 |
| `source-policy-cli-r112.txt` | Provider CLI, strict Python GPU gate, GPU CLI A/A+A/B and native policy: 4/4 |
| `gt2-source-policy-cpu-r103/result.json` | Public v2 Direct verification twice: 1397 draws, 136 submitted/completed/reclaimed sources each |
| `gt2-source-policy-gpu-r104/result.json` | Public v2 Direct GPU twice: 133 leases, 1387 Direct + 10 compatibility draws, 136 publications, strict original source/output equality |
| `gt2-source-policy-ab-r106/result.json` | Real Compatibility/Direct pair accepted; each has two CPU verification, two cold GPU, two warm-up and sixteen low-observer measured iterations |
| `gt2-source-policy-aa-r107/result.json` | Real Direct/Direct same-policy pair accepted through the same GPU/CPU gate |
| `source-policy-build-r109.txt`, `source-policy-x86-build-r111.txt` | Final optional-digest CLI fix builds on native and x86_64 |
| `source-policy-final-r113.txt` | Final focused native/CLI/Metal tests and existing TLA: 9/9; TLA 164.28 s |
| `source-policy-final-x86-r123.txt` | Final x86_64 partial-Clear, source-policy, CPU-policy, provider, source-plan and provider CLI: 6/6 |
| `gt2-source-policy-aa-r116/result.json`, `gt2-source-policy-ab-r117/result.json` | Fresh Direct A/A and Compatibility/Direct A/B pass after the final CLI and receipt-authentication changes; A/B retains sixteen measured iterations per policy |
| `gt2-source-policy-stale-gpu-r119/result.json`, `gt2-source-policy-stale-cpu-r120/result.json` | Altering the expected plan SHA rejects before replay: GPU has zero created objects, draws and encode chunks; CPU reports `source-plan-digest-mismatch` before acquiring its resource device |
| `gt2-r28-strict-policy-negative-r121/result.json` | Normal, non-diagnostic v2 Direct replay completes twice but the public strict gate rejects the same six-pixel original mismatch |

All artifact paths are under this leaf's output root. The original r65 source
and output SHA remains `65a34bf516b41bdd32014043bc0ef28b494719ea21eaba3f23b57755a5421c57`.
These are input-qualified policy experiments; no wild FPS or zero-growth
claim follows. The first integrated provider test r100 failed because copying
the pre-reserved zero-sized publication vector into the by-value replay owner
discarded capacity. Reservation now occurs on that final owner before effects;
the bounded callback still rejects overflow rather than allocating on failure.
The r114/r115/r118 standalone CPU negative invocations omitted required policy
lane/pending arguments and failed wrapper validation; they are retained but are
not digest-rejection evidence. The corrected r120 invocation is authoritative.

### r28 final-composite input discriminator

The six final postprocess draws form this chain:
scene 1024x768 -> tone-map 1024x768 -> downsample 512x384 -> downsample 320x240
-> horizontal blur -> vertical blur -> scene+bloom composite. All actual
postprocess textures are single-mip DEFAULT X8R8G8B8; bytecode confirms that
the first five sample only slot 0 and the final composite samples slots 0/1.
The six residual output coordinates do not directly sample texture edges;
blur edge clamping could affect only part of their upstream neighborhoods.
These facts narrow candidates but do not prove matching original intermediates.

`gt2-r28-final-inputs-r110/result.json` and its sibling
`gt2-r28-final-inputs-r110-observer/{scene,color}` retain the shader-visible
scene and final blurred bloom from existing pass-end dump hooks. No new
render-pass split is requested (`AFTER_DRAW=0`). Output/source still equal the
earlier residual SHA `e62abb80b0baaa9f46decff9ec874d8184a298680252a61287f22f74d5b4ed59`.
The probe is explicitly diagnostic and rejected; original equality remains
false. The first r108 invocation failed before effects because the CLI marked
an omitted optional expected digest as supplied; the absence flag is corrected.
The original bundle's arm-boundary RT seeds are not current-frame intermediate
oracles. Offline arithmetic sensitivity cannot turn them into such evidence.

The reproducible offline discriminator is retained in
`gt2-r28-final-composite-offline-20260911-second-opinion-v1/{analyze.py,result.json}`;
Main reran it in `gt2-r28-final-composite-offline-main-recheck-r122.txt`.
It verifies that the first three raw attachment bytes equal the replay oracle
across the full frame: these `.rgba` files preserve BGR byte order, with alpha
forced to 255. Shader-space arithmetic explicitly swaps channels.
Ideal bilinear sampling and instruction-ordered float32, float64, simulated
fused MAD and half-sampled-input variants all predict the replay bytes at the
six residual coordinates. The required changes to cross the original output's
quantization boundaries are 0.0086–0.1941 byte, versus approximately 1e-5 byte
for float32/double/FMA differences. Ordinary final-ALU precision variation is
therefore a weaker candidate, not a proved exclusion of Metal filtering or
compiler behavior. The predictor itself misses 6,469/746,790 pre-HUD interior
pixels (0.86624%), all by at most one byte, so it is not an acceptance oracle.

Two hypothetical scene-green byte changes explain four residuals without
extra changes in their respective four-pixel footprints. They are solely
non-identifying counterexamples: no original current-frame scene/bloom or
emitted MSL was retained, and no capture bytes or shader math were changed.
The next discriminator needs original current-frame pass inputs/outputs and
emitted shader/compile-option lineage. If a new trace-complete counterexample
is required, it supplements rather than replaces the preserved r28 failure.
r121 independently confirms that normal v2 Direct with observers disabled
still returns exactly six one-unit RGB differences and no alpha differences
in both cold iterations. Its zero GPU errors and complete source receipts do
not qualify it.

### Compiler lineage and bounded pass-input diagnostics

`DXMT_DUMP_SHADER_DIR/compiled` now records the exact UTF-8 source reaching
`MTLDevice_newLibraryFromSource`, its SHA-256, the actual null compile options,
compile success, process/attempt identity, architecture, OS and Metal device.
This is a cold observer: regenerated translator dumps alone are not evidence
that a source was compiled. Nonempty shader/readback output paths (including
the literal path `0`) make GPU policy receipts diagnostic and unqualified.

`gt2-r28-lineage-r131` (arm64) and `gt2-r28-x86-lineage-r134` (x86_64)
each complete with the unchanged six-pixel residual and output SHA `e62abb80…`.
Their audited actual compilation receipts have 107 attempts, 106 distinct
sources, and matching sets of all 56 translated VS and 41 translated FS
sources. `check_compiled_lineage.py` and the corresponding
`*-compile-audit.json` artifacts retain the independent verification. This
weakens a current-host-architecture explanation; it does not recover the
historical live binary's missing compiler lineage or qualify r28.

The draw-texture observer now intersects bytecode PS identity and logical
texture slots, optionally repeats per exact handle/view/stage/pass identity,
and requires explicit finite sequence bounds plus a 1–256 readback budget.
Shared pure parsers reject malformed numeric/slot selectors rather than
broadening collection. The first r131 probe selected a state-key summary
instead of the bytecode hash and produced no texture evidence. That observer
bug is fixed. `gt2-r28-textures-r145` now captures precisely scene slot 0 and
bloom slot 1 at source 210 / encoder 1 without an extra pass split; final
pixels still have the same six-byte residual. Both failed and successful
diagnostic artifacts are retained. Unit policy/parser tests and GPU Python
gate pass 2/2 in r126; CPU Python/CLI policy checks pass 2/2 in r130.

The new 16x16 visible indexed A→B→A fixture has explicit valid VS/PS bytecode,
POSITION declaration, stream stride, viewport, cull/depth/blend state and
independent strip/color oracle. Vertical overscan deliberately removes
top/bottom edge-tie coverage from this binding test. Separate PS-color and
stale-VS-width inputs must fail that original oracle. The actual-compilation
lineage test passes in r144, including exact pixels and diagnostic rejection.
r144 correctly rejects ProductionSlot qualification because its standalone
Clear populated a zero-headroom compatibility slot and all three draws fell
back. The visible fixture now seeds the complete background instead; the
separate patterned-Clear fixture remains unchanged. Native r148 and x86_64
r150 each pass provider, public GPU-policy CLI and actual-compilation-lineage
tests (3/3): both routes match the independent oracle, ProductionSlot executes
all three Direct draws, and both altered-PS and stale-VS inputs reject the
original pixels. This is non-vacuous draw/binding coverage, not proof that
Direct crosses a preceding standalone Clear or that arbitrary GT2 is exact.
Earlier r128/r136/r140/r142 fixture failures remain retained (event closure,
explicit initial state, canonical section order and edge-tie construction).
They are fixture-development failures, not changes to the captured GT2 oracle.

The live diagnostic r146 publishes
`gt2-gpu-lineage-20260911-r146/frame-1393431587717200-1`.
Independent bundle/identity inspection validates 222 source segments at
sequence IDs 8480–8701. The trace recipe bounds effect/encoder observations to
6500–18000 and final-composite PS texture readbacks to 256 dumps. The runner
was explicitly terminated during log finalization after game exit because the
volume was nearly full; the atomic tape is independently valid, but no full
wild-run/FPS/staged-build completion receipt is inferred. The captured pass
must be joined to actual dump metadata, not just assumed covered by the recipe.
The exact recipe, inspect/identity receipts and compiler audit remain under the
artifact root as `live-lineage-r146*`; r28 is still the unresolved strict
counterexample.

### Paired live/replay intermediate evidence (r146/r152/r157)

The bounded live texture budget did retain the selected final composite at
live source 8700 / encoder 1. The authenticated normal source plan
`a4f77bb29c115a1f082e8338406495c5102a99fa439f29185fdcbcb90471d29c`
maps that source to local replay sequence 222 (including the bootstrap).
The r152 x86_64 replay completes all 222 source publications with 2286 Direct
and 10 compatibility draws, but rejects strict equality: two pixels differ by
one at `(668,704)` and `(669,705)`. Its output SHA is `c15487d8…`, whereas
the original is `085e40e2…`. This new frame does not replace the r28 failure.

`compare_r146_pass_inputs.py` and `r146-r152-pass-input-comparison.json` preserve
the exact identity-qualified comparison:

| Final composite input | Live/replay difference |
|---|---|
| Scene, slot 0, 1024x768 | 7 bytes / 6 pixels, all one unit, bbox x669–681/y704–708; three RGB and four alpha bytes |
| Bloom, slot 1, 320x240 | Byte-identical, SHA `1ff904ee…` |
| Final PS actually compiled source | Byte-identical, SHA `07ea00af…` |

The scene difference already exists before the final composite. That excludes
the final composite as the sole origin of this frame's mismatch, not every
possible downstream arithmetic discrepancy. The texture dumps preserve raw
B/G/R/A channel order; Present oracle alpha normalization is a separate step.

The full r157 effect/encoder replay retains exactly the r152 final SHA and
two-pixel residual. A diagnostic coverage bug was fixed first: non-indexed
draws reached `recordedDrawPrimitives` without `traceEffectDraw`. The same
cached-bool-gated helper now observes the successful non-indexed issue path
outside issue/Metal timing scopes, without changing draw state or ordering.
The existing one-UP-draw textured fixture proves exactly one `indexed=0` row
when enabled, none when disabled, equal output hashes and zero GPU errors.
It has no independent captured pixel digest and is an observer-invariance pin,
not pixel qualification. Native r156 passes provider/provider-CLI/GPU-policy
CLI (3/3), and x86_64 r158 passes provider-CLI/GPU-policy Python+CLI/compiler
lineage (4/4). The failed r154 assertion incorrectly expected a captured digest
from that historical fixture and remains retained.

CPU/GPU input staging now shares the existing independent APFS clone helper;
the GPU wrapper previously deep-copied the full bundle. The staging/failure
and policy tests pass 6/6 in r151. No old evidence was deleted, input/oracle
hash checks remain mandatory, and diagnostic runs are still unqualified.

Actual source-plan-qualified alignment finds identical sets of all 236
physical encoder identities and byte-identical FS-volatile bindings on all
2296 draws. The 2278 common effect rows agree on selected payload hashes,
render/sampler/attachment descriptors and viewport; the 18 missing live rows
are non-indexed PointList draws, newly visible in r157. Run-level VS constant
hash differences on 19 draws do not contradict equal selected payload hashes.

The compiled-source audit initially found 12 VS variants differing only in
the dynamic stream-0 fallback literal (the runtime stride is nonzero), plus
two FFP VS comment-only differences. All fragment source variants agree.
The existing `DXMT9_DISABLE_ENCODE_SLOT_PSO_PREFETCH=1` discriminator in r159
reduces actual prefetch invocations from 223 to zero and commands from 866
to zero. Exactly 12 compiled sources are replaced with the corresponding
live sources, but scene, bloom and final output remain byte-identical to
r152/r157. This excludes those fallback-literal variants as the cause of
the r146 residual under the tested lane; it is not a reason to change shader
arithmetic or promote the prefetch override. The no-prefetch recipe and raw
compiler/pixel evidence are retained as `replay-no-prefetch-r159*`.

`r146-r157-second-opinion-audit.py` reconstructs the source join, compiled
variants and canonical late-draw geometry bounds from the retained inputs;
Main reran it as r160 and obtained byte-identical JSON to the independent
audit. The 18 PointList bounds and seven following indexed scene draws do not
reach the changed scene pixels in that CPU projection. The projection is only
spatial counterevidence (not clipping/depth/blend/rounding proof), and r157
does not retain raw uploaded VS matrix bytes. The first divergent scene draw
remains unidentified; earlier scene writers need a narrower input/output
discriminator. Neither this new failure nor these exclusions close r28.

### Replay-only scene ROI localization (r161)

The existing pass-end color summary copies a 13x5 bounding box (260 bytes per
pass) for six one-pixel RGB ROIs on scene surface `0x300000200000002`.
No raw image dump or after-draw split is requested. r161 records 738 rows
across 123 distinct scene passes; final output remains exactly r152/r157's
two-pixel failure SHA `c15487d8…`, with 222 publications, 2286 Direct draws,
10 compatibility draws and zero GPU errors. It is diagnostic and rejected.

`analyze_scene_roi_r161.py` validates CSV coverage and final RGB against the
retained r152 scene bytes, then joins changed pass endpoints through the
authenticated source receipts and r157 draw trace. Its retained output is
`r161-scene-rgb-writers.json` under the artifact root.

| RGB-changing replay pass | Captured source | Canonical draw interval |
|---|---|---|
| 77 / encoder 0 | 8555 | 743–753 |
| 183 / encoder 0 | 8661 | 1811–1862 |
| 205 / encoder 0 | 8683 | 2109–2119 |

All six sampled RGB values change only at these three observed endpoints
after the initial black value. This narrows replay writer candidates, not the
first live/replay divergence: original per-pass bytes are absent, the summary
omits alpha, and equal pass endpoints do not exclude transient writes inside
the pass. Paired original intermediate evidence is still required; r28 and
r146 remain strict counterexamples.

The GPU receipt observer gate now also rejects summary-only paths, including
literal `0`. Native r162 Python tests reject a forged non-diagnostic receipt;
r163 actual-GPU CLI coverage observes a one-pixel summary on the visible
three-Direct-draw fixture, retains exact original pixels and zero GPU errors,
but rejects qualification because the observer is active. The same Python/CLI
checks pass 2/2 on x86_64 in r164. `git diff --check` and the performance-doc
source audit pass; renderer binaries and original pixel oracles are unchanged.

### Bounded frame selector and the rejected original-frame join (r165–r172)

The encoded-frame selector adds a strict positive `FRAME` plus bounded
`MAX_DUMPS`, with pure owner/sequence-reset pins and actual GPU checks for
frame 1, an unreached frame and malformed input. Native/x86_64 full builds
and focused GPU checks pass in r165–r170; both PE staging builds complete.
These ordinals select an observation, not successful capture authority.

Live r171 completes the canonical Sikarugir GT2 runner with status `pass`,
return code 0, no timeout, staged-build receipt and a recovered benchmark
result. It is a diagnostic run, not an FPS result. Its published Tape is
`gt2-gpu-pass-frame-20260912-r171/frame-1544521788358200-1`, independently
valid with 174 sources, seq 9563–9736, 53 settlements and source plan SHA
`433183eb98169b67ae528d903ab17fce0c59f07d565d6e63032bae976282b5a0`.

However, the selected encoded frame 202 produced 135 original pass dumps at
seq 7007–7126: **zero source overlap with that Tape**. Its capture-local
`frame_id=11` is not a global encoded-frame number. Repeated capture-boundary
operations are observed, but their precise retry/abort reason is not proved
by the retained warn-level log. These earlier dumps are not paired original
intermediates for this bundle. `audit_r171_capture_join_r179.py` rechecks
component hashes and the disjoint source join without loading bulk raw images;
`r171-capture-join-r179.json` retains the rejected result. The 1,376,391,168
raw bytes remain evidence; the many 2048x2048 targets demonstrate why a
dump-count cap alone is not a small disk budget.

Observer-off r172 replays the actual r171 Tape twice using the authenticated
normal ProductionSlot source plan. Both iterations finish with zero GPU
errors, 1926 Direct plus 10 compatibility draws, and identical output SHA
`75ec91ed362bf48649006229f2f969a42e50e6533b225817abb1774e0a1dff09`.
The captured output is
`e369e124fa28fd9536f0a336012a8fb8e2999500952b1620117c849d93a8443b`.
Three pixels differ at `(349,218)`, `(350,218)` and `(350,219)`; maximum RGB
delta is 1, total delta 5, with no alpha differences. Internal pixel-envelope
acceptance does not override the public strict rejection (`accepted=false`,
`diagnostic=false`, provider exit 0). This is a third preserved strict
counterexample, not a replacement for r28 or r146.

### Source-qualified hash-only intermediate observation (r173 onward)

The next discriminator avoids a guessed frame and bulk raw files:

- `HASH_ONLY=1` retains SHA-256 and size/format metadata for completed tight
  GPU readbacks, without channel conversion, alpha normalization or `.bin`
  files. It requires an explicit finite count (maximum 16384); raw mode keeps
  its 256 cap. This bounds disk output, not GPU work or observer cost.
- `CAPTURE_SOURCES_ONLY=1` requires hash-only mode. Single-source and batch
  Arena publication register validated capture identities under the queue
  lock before Ready consumption. Actual encoder source/storage references,
  including generations, and pool owner/seq must match the cold table.
- Pass provenance stays in a bounded diagnostic-owned map keyed by the
  existing retained texture identity and pass coordinates, not a new fat
  member of per-pass execution state. Queue teardown clears that owner's
  observations. Missing/overwritten/invalid identity loses coverage, not
  rendering or a strict pixel failure.

Native r174 passes pure-policy and real GPU CLI tests (2/2). r176 and x86_64
r178 pass 5/5, including actual single/batch queue publication, absent or
invalid capture identity, owner teardown, every stale source/storage
coordinate, bounded collision and actual GPU hash/raw equality. Non-capture
GPU replay with the capture-only selector emits no dump; all observer runs
remain rejected by qualification even with exact pixels. The existing TLA
suite passes in r177 (170.07 s). Its queue semantics are unchanged; the new
observer's identity predicates are covered by native tests, not a claim that
TLA proves readback bytes. Initial test invocations r173/r175 used incomplete
Meson target names and did not execute; their corrected invocations are the
cited passing evidence. A Meson environment-composition mistake was corrected
before the successful integrated build.

Final observer-owned pass-provenance storage passes the same five tests in
native r180 and x86_64 r181 after full builds. Both PE staging builds also
complete with no pending work. Their test receipts are retained under this
leaf's artifact root as `native-gpu-capture-hash-r180.txt` and
`x86-gpu-capture-hash-r181.txt`; `git diff --check` and the performance source
audit pass. No new live capture, policy promotion, commit or push is implied.

At r181, the source-qualified **published live GT2 capture join remained unexecuted**.
A source can belong to a failed capture attempt, so even this observer must be
joined to the final independently validated published sidecar. After-draw
splitting remains off for the next
pass-end comparison. Hash equality localizes endpoints, not intra-pass
execution; preserve raw/ROI evidence for the first divergent endpoint before
changing shader arithmetic. Low disk space prevented the next live capture
until the user freed space on September 13; no existing raw images or Tape
bundles were deleted by this investigation.

### Positive native source-to-GPU observer join (r182–r184)

`dxmt9-capture-color-observer-metal-spec` now closes the bounded positive join
between real queue publication, encoder selection and completed GPU bytes.
Each fresh EventSerial/SegmentSerial process renders four 16x16 uniform-color
draws through the actual raw replay and capture queue profile. Only draws 2
and 4 carry capture identities. All four complete images must match independent
BGRA constants; exactly two hash-only records must match the actual settled
production identity ledger and independent SHA-256 values. Uncaptured draws
must emit no records, and no raw image files are written. Metal API validation
is enabled, and after-draw splitting is disabled.

Native r182 passes the new fixture. After full native and x86_64 builds,
r183/r184 each pass both this fixture and the existing Direct/compatibility
Metal oracle (2/2 per architecture). Logs are retained as
`native-capture-source-metal-r183.txt` and
`x86-capture-source-metal-r184.txt` under this leaf's artifact root.

This fixture has no PE publisher and produces no Render Tape bundle. It proves
the native positive observation path, not a successfully published Wine/GT2
capture join, nor resolution of any retained GT2 pixel counterexample. No
renderer semantics changed in this fixture step; the prior TLA evidence is
unchanged and does not prove these concrete pixels. The next required evidence
remains a published capture paired with its own original pass hashes, followed
by the first divergent original/replay pass and observer-off strict regression
checks for r28, r146 and r171.

### Published original pass hashes and same-input replay (r185–r187)

After the user freed disk space (6.8 GiB at preflight), live r185 completes the
canonical Sikarugir GT2 runner with return code 0, no timeout, a staged-build
receipt and a recovered `.3dr`. This is diagnostic, not an FPS result. The
existing raw counterexample evidence is unchanged. The bounded hash-only
capture omits the guessed encoded-frame selector and after-draw splitting.

`inspect_source_hash_r185.py` independently verifies the bundle components,
event grammar and queue-fenced identity sidecar, then joins seq, source ordinal
and actual raw ordinal against its settlement table. Of 556 observed pass
hashes, 211 belong to the published Tape (197 of 198 sources; the final source
is Present); 345 belong outside it and are excluded. The published interval is
7493–7690 and the authenticated source-plan SHA is
`8ff52ea7675208f4f8bb14ae408427e7916f4ee61f0c60399ef120e541eddc6d`.
The input is `gt2-gpu-source-hash-20260913-r185/frame-1603812129736400-1`.

Diagnostic r186 preserves those source cuts with actual publication receipts.
The original and replay final output SHA both equal
`82153d28ba24c542fde0c4d7ffa9f65ad6cbc00fe147dd72d9431780d6500e23`.
The public wrapper correctly rejects qualification because observers are on.
Observer-off r187 repeats twice with the same exact output and
`accepted=true`, `diagnostic=false`. This is input-qualified positive evidence,
not closure of r28, r146 or r171.

The retained `r185-r186-pass-hash-comparison.json` pairs all 211 observed
endpoints by authenticated source, physical encoder ordinal, attachment index
and matching dimensions/formats. It reports 166 equal and 45 different hashes:
25 R32F 2048x2048, 18 R32F 512x512 and two X8R8G8B8 1024x768 endpoints.
The first candidate difference is captured seq 7528 / encoder 0 (replay seq
37), after equal preceding endpoints on that target. Full raw hashes include
alpha/float bits; a hash difference is not by itself a visible RGB difference.
The comparison explicitly does not certify cross-process canonical attachment
identity or intra-pass execution. Some live surface handles map to more than
one replay generation, so a global raw-handle bijection is not assumed.
Original raw pass bytes were not retained in hash-only mode.

The source-qualified positive published-capture join is now exercised. The
remaining discriminator is a failing input with paired intermediate evidence,
not further passing frames or relaxed tolerances. Follow-ups r188 and r189
complete the benchmark but publish no Tape; their observations therefore cannot
be joined as original/replay evidence. r189's info log records 426
`snapshot-or-ledger-append` and 253 `event-settlement` failures followed by
`identity_query` capture rejection. Runner success is not capture success.

### Event-local pass identity composition repair (r190)

The batch FrameGraph restarts logical pass numbering at one per CommandChunk
event, while the capture ledger previously consumed those IDs as frame-global.
Two events with local pass one and different DAG/kind metadata reject; identical
metadata silently aliases unrelated passes. The smallest native counterexample
fails before the fix (`native-pass-namespace-before-r190.txt`) and passes after
the explicit `appendEventLocalPasses` namespace conversion
(`native-pass-namespace-after-r190.txt`). Same-event continuation and trusted
frame-ID append semantics remain intact. Bounded native cases additionally pin
equal-metadata separation, sparse local IDs, scope mixing, membership changes,
coverage, arithmetic overflow, and token reset. All six focused native capture,
source-plan, Arena and actual Metal observer/oracle tests pass on both native
and x86_64 builds. The actual Metal fixture also explicitly checks distinct
pass IDs for its two different captured events. Full native, x86_64 and both
PE staging builds pass; TLA r190 passes in 168.10 seconds. Logs remain under
this leaf's artifact root.

This establishes a capture composition defect independently of the coarse
r189 log reason; it does not retrospectively identify every failed append's
predicate. Live capture after the repair is required to check whether it removes
that abort. No renderer ordering, shader arithmetic, resource bytes or policy
acceptance tolerance changed, and r28/r146/r171 remain open.

### Successful post-repair capture and unchanged originals (r191–r194)

The same r189 recipe (skip 50, bounded hashes plus final-composite inputs) now
publishes `gt2-gpu-source-hash-20260913-r191/frame-1605840362338500-1`.
The canonical Sikarugir runner exits 0 without timeout, records the rebuilt
staging hashes and recovers its `.3dr`; no identity-append or first-abort error
is observed. Independent validation joins all 114 recorded pass hashes to
100 of the 101 published sources (2174–2274; final Present has no color pass),
with zero outside-attempt observations. Source-plan SHA:
`1033f455960dd95fc71d43770eb52bc240ad6de840fe650b842fba11e88f541b`.

Diagnostic replay r192 has 87 equal and 27 differing intermediate hashes
(25 R32F and two X8R8G8B8). The first candidate endpoint is original
2194/0 versus replay 22/0, R32F 2048x2048. This remains source/physical-pass
pairing, not cross-process canonical attachment or intra-pass proof.
The final-composite scene and bloom inputs at original 2273/1 versus replay
101/1 are byte-exact, including their raw alpha. Final source/output SHA is
also exact: `742ca3de6c5c22bab21885fceebf286f8e6224043ddefeb266d2a3aca03ba462`.
Observer-off r193 repeats twice with `accepted=true`, `diagnostic=false`.
This resolves the post-repair capture gate for this recipe, not pixel promotion
for the existing counterexamples.

r194 independently replays **each original r28, r146 and r171 twice** with
observers off and the same authenticated source plans. All six replays remain
strictly rejected, with the identical prior output SHAs and respectively
6, 2 and 3 differing pixels (maximum channel delta 1). No failure artifact or
oracle was replaced. `original-regressions-r194.json` records those results;
the retained scripts and result receipts reproduce them.

The skip-200 follow-up r195 also publishes on its first attempt:
`gt2-gpu-source-hash-20260913-r195/frame-1606096708088900-1`, 120 sources
(7054–7173), 2,005 records, 134 authenticated original pass hashes and zero
outside-attempt rows. Plan SHA is
`c03cb37e8ca569215ed17ba56cf9db8499a219aedd710af8425b9ff1de821b22`.
r196 has 89 equal/45 differing raw endpoints, first R32F 2048x2048 at
7071/0 versus replay 19/0. Final scene/bloom inputs and source/output are all
byte-exact; observer-off r197 repeats twice with `accepted=true`.
Output SHA: `aa27790c7e942d084c2af4e957ad154582fa7ba9cf2dadec75ef9d4e012647d4`.
This is another positive capture, not an explanation of the older failures.
The original r146/r171 have 3,733/2,957 records; the next diagnostic r198
selects skip 500 to seek a later, more complex failing frame with paired input
evidence, rather than repeating the same positive boundary.

r198 publishes 203 sources (22249–22451), 3,762 records and 215 joined original
pass hashes, again with no outside-attempt rows. Its source plan is
`7498c676d0677d5e0f905c6fec8830cdd43d799ec360800e1dc0951bc6098183`.
r199 has 170 equal/45 differing raw endpoints, but final scene/bloom and output
are exact. Observer-off r200 confirms twice, output SHA
`c32dadd9fa8e8866749f380241a85e3ee8b1ac69e43e1c84ba4b757088596add`.
Workload record count alone therefore does not distinguish the old residuals.

### Fresh failing input without per-pass readback (r201–r204)

r201 retains the skip-500 recipe and final scene/bloom input dumps but disables
the color-pass observer. It publishes on the current rebuilt provider:
`gt2-gpu-source-hash-20260913-r201/frame-1606665804083100-1`, 210 sources
(22636–22845), 2,797 records and 95 chunks. Source-plan SHA:
`5dedd7cb8351e8534e05021ceebddd055d42f07039f596283996dfab53b7e179`.
The runner exits 0 without timeout, with no failure and a recovered `.3dr`.
No original per-pass hashes exist; identity validation must not invent that
coverage. The final inputs are joined to captured 22844/1 and actual replay
210/1 by the settled source receipts.

Diagnostic r202 reproduces a new **six-pixel, seven-channel-byte** final
difference (max delta 1): original SHA
`dfb274ca7a926239ae32145a9a538d65802775ca0376b60857f18748be0438c4`
versus replay
`dec519eca13b206f16780ff381af5999a563f4a9d887b49297907959377f4e6d`.
Its scene input already differs at five pixels/seven bytes, maximum channel
delta 4; bloom is exact. Scene raw-BGRA differences are at (836,322) A,
(831,330) R, (826,333) A, (827,336) A and (202,545) B/G/R. Final RGB differences
occupy (830,330), (831,330), (201,544), (202,544), (201,545), (202,545).
`r201-r202-final-input-comparison.json` retains exact byte values and paths.

Observer-off r203 repeats the identical final SHA twice and remains
`accepted=false`, `diagnostic=false`. Re-enabling per-pass hashes on **this same
input** in r204 preserves the same failure SHA. The provider's legacy pixel
envelope may label the small delta equivalent internally; the public strict
acceptance gate still rejects it. No tolerance or oracle was changed.

This rules out the failure being restricted to historical capture builds and
again localizes it upstream of final compositing. Three pass-observed captures
passed while this unobserved capture failed, but they are different inputs;
that is an observer-perturbation candidate, not a causal proof. r204 excludes
replay-side pass readback as a cure for this input. Do not keep collecting
passing frames as a substitute for locating this scene-write divergence.

Intermediate hash attribution also needs the **effective** attachment store
action, including late resolution, and defined-content coverage. Through r204,
color hash metadata lacks that witness; `resolveLateStoreForClear` can still select
DontCare for an unresolved attachment, independently of the eager capture
preservation predicate. This is a concrete missing diagnostic qualification,
not proof that any recorded R32F difference came from a discarded attachment.
The Store witness is implemented below; defined-content and paired original
endpoint evidence remain separate requirements.

### Effective Store witness and scene writer candidates (r205–r208)

The cold color observer now copies the matching color/index/handle entry from
the resolved `LateRenderPassStoreState` at `LifecycleRuntime::flushRender`,
after late resolution. Completion owns that diagnostic copy rather than a
borrowed pass ledger. JSON adds `sampleCount`, `storeActionKnown`,
`effectiveStoreAction` and `storedAttachmentContents`. The pure
`colorAttachmentDumpHasStoredContents` predicate accepts only known Store or
StoreAndMultisampleResolve with sample count 1; unknown, discarded, multisample
and invalid actions do not qualify. This is a necessary Store condition, not
proof of initialized/live bytes or canonical cross-process attachment identity.
No Store action is forced and no after-draw pass split is introduced.

Final native and x86_64 full builds pass (r205). The truth table, actual
source-to-GPU observer, Direct Metal oracle, capture and source-plan specs pass
5/5 on each architecture (r206). Existing TLA passes in 167.41s (r207);
this does not prove concrete pixels. Preserved final logs include
`native-store-witness-tests-r206.txt`, `x86-store-witness-tests-r206.txt`
and `native-store-witness-tla-r207.txt`. Earlier build errors remain retained.

On unchanged r201, diagnostic r208 authenticates all 210 source settlements
and observes 80 scene endpoints, each with actual Store=1/sampleCount=1.
Its 160 RGB ROI rows join to those hash witnesses by seq/encoder, without
changing the CSV schema. The final output retains the same six-pixel failure.
Although raw `.bin` files are not retained, this simultaneous hash/ROI recipe
still reads the full attachment on the GPU; it is not a low-overhead timing run.

`r201-scene-writer-candidates-r208.json` identifies these replay-only RGB writers:

| Replay seq/encoder | Captured seq | Event | Event record range (first, count) | Canonical draw ordinals |
|---|---|---|---|---|
| 94/0 | 22728 | 697 | 48, 11 | 579–589 |
| 98/0 | 22732 | 740 | 41, 15 | 617–627 |
| 172/0 | 22806 | 936 | 0, 54 | 1412–1463 |
| 185/0 | 22819 | 963 | 0, 57 | 1573–1624 |
| 187/0 | 22821 | 964 | 11, 11 | 1636–1646 |
| 197/0 | 22831 | 974 | 42, 11 | 1735–1745 |
| 202/0 | 22836 | 976 | 36, 11 | 1790–1800 |

These are not first-divergence findings: original per-pass/clear-only endpoints,
alpha and intra-pass changes are absent. In particular, PS `c7b45187f5b1857d`
uses interpolated RGB and texture alpha, while `bc0c4191e29f8b02` computes RGB
from interpolants and constants. Texture-only or final-composite arithmetic
changes would not follow from this evidence.

### Same-input route and actual-compiler exclusions (r209–r212)

Observer-off compatibility r209 repeats the same r201 input twice. Both
iterations execute 1,875 compatibility draws, zero Direct draws, 214 batches
and 211 encode chunks, with zero GPU errors and the same rejected final SHA
`dec519eca13b206f16780ff381af5999a563f4a9d887b49297907959377f4e6d`.
This excludes a Direct-only explanation under the authenticated source cuts;
it does not prove the shared capture/replay path is correct.

The r210 compiler audit validates 121 live and 98 replay compilation receipts,
including exact submitted source SHA/length, success and null compile options.
Both use x86_64, Apple M1, the same device registry ID and OS build 25G83;
the OS description string is localized differently. All six candidate fragment
shader sources match actual compilations. Five candidate VS variants differ
only in the dynamic stride fallback literal (24 vs 20 or 64 vs 60).
Identical regenerated dumps that never reached compilation are explicitly
excluded from actual-compiler matches.

r211 disables the existing encode-slot PSO prefetch on the same input, retaining
actual compiler receipts and only the two final-composite input textures.
Prefetch invocations/commands change from 211/675 in r208 to 0/0. All original
variants of the eight candidate VS and six PS now have exact successful
compilation matches, but scene, bloom and final bytes remain identical to the
failing r208 replay. This excludes the observed fallback-literal variant
differences as the cause of this residual; it does not promote the prefetch
override or identify the selected live PSO at every draw.

`audit_discriminators_r212.py` reauthenticates the original bundle, source cuts,
settlements, actual readback bytes and identical provider/loaded-image receipts
across r208/r209/r211. `r201-discriminator-audit-r212.json` preserves the result,
with `qualified=false`. The original output oracle remains `dfb274ca…`, not the
replay digest. Full artifacts remain intact; no raw evidence was compressed or
deleted after the user freed disk space.

The next discriminator must obtain paired source-qualified scene endpoints and
actual geometry/uniform binding bytes, then isolate a differing writer. Current
evidence does not justify changing shader arithmetic or widening pixel tolerance.

### Tracked binding bytes and source-qualified live comparison (r213–r227)

The cold effect observer now distinguishes expected payload bytes from the
actual tracked VS/PS/FFPVS/FFPPS Shared Metal buffer slices. Direct-cbuf tracking
resets on actual encoder creation and follows successful binding events;
argument-table observation uses the existing slice cache. Bounds/storage
failures remain unknown. Vertex-slot-5 rows retain the actual 80-byte push.
Capture-only effect selection reuses pre-Ready owner/source/storage identity
without enabling color readbacks. It must still join the published sidecar.
These are CPU-visible witnesses, not proof of GPU read addresses, private
storage, or parallel-child bindings.

Native and x86_64 r218 each pass the Direct Metal oracle, capture observer and
compilation-lineage specs (3/3). The latter compares actual A→B→A constants and
independent pixels through compatibility, Direct-cbuf and argument-table paths.
r223 adds native malformed-scope, master-off (including malformed subordinate
scope), and disjoint-sequence negatives in both identity modes; 3/3 pass.
The disabled master now returns before subordinate validation/logging. TLA r222
passes in 201.75s. Earlier r213 build failure and r214 direct/argument-table
fixture assumption failure remain retained; neither is a passing test receipt.

r219 exits before device creation with only factory calls and no Tape. The
same staged images reach 1024×768 device creation/rendering in the r220 debug
diagnostic. No factory, display-mode or renderer fix was made, and this does
not establish the cause of r219's transient early exit. r221 and r225 use
automatic Enter injection disabled and complete through the canonical runner.
Their staged unix provider hash prefixes are respectively `b734bf8ec4a4cac4`
and `c783c9f8aee08849` (the installer rebuilt the disabled-scope guard for r225).
Each pair's observed unix image matches its own staged receipt; the native replay
driver's loaded-image receipt remains distinct from the Wine staging receipt.

| Input / replay | Published sources | Draw / indexed byte observations | Original/replay final output |
|---|---:|---:|---|
| r221 / r224 | 157; seq 24964–25120 | 1,624 / 1,606 | Exact SHA `999707c139cd45d2e30600b3f762f3e832eee6a0ae9dbf7140e2dba146f2357a` |
| r225 / r226 | 226; seq 22906–23131 | 2,231 / 2,213 | 56 pixels differ; max RGB delta 5, total RGB delta 138; alpha exact |

`audit_bind_capture_r221.py` authenticates the published Tape, queue-tail-fenced
identity and source-plan digest before retaining observation rows. Both runs
have zero outside-published-source rows. `compare_bind_r221_r224.py` then joins
actual local publications back to those cuts. Log parsing stops at the closing
bracket: a complete adjacent capture-alias message must not be interpreted as
extra vertex-binding fields.

For both inputs, the recorded VS/PS and FFPPS backing bytes, vertex/fragment
slot-5 bytes, observed FFP half-pixel/viewport/clip binding fields, and all
indexed stream-0/original-index hashes match. The geometry hashes cover their
bounded CPU backing ranges, not other streams, effective reordered indices or
proof of GPU reads. Local resource handles are excluded, not certified as
canonically equal. FFPVS full backing bytes differ on every observed draw;
25 expected VS full-state hashes and 10 FFP VS hashes also differ in r225.
Consequently this is not an all-uniform/all-shader equality claim. The same
classes of differences exist in pixel-exact r221; their causal relevance still
requires consumed-field/source analysis.

Final r228 x86_64 source-scope/lineage/Direct Metal verification passes 3/3;
native r229 source-plan, capture-policy, GPU-policy Python/actual-Metal CLI,
provider CLI and partial-Clear integration passes 6/6. Full native and x86_64
builds, documentation-source audit and `git diff --check` pass. These verify the
observer/gate implementation, not closure of the original GT2 counterexamples.

r225 preserves original SHA
`3b896cc079db4b2d1669bd029c4dd257207bf3a7e3b0fb10d25d506745074cf9`
against replay SHA
`9605f077806fc8b5353c9d0762e28fadc8a3ffb59e7152170c3a3ec0ee9f6573`.
r227 disables inline observers and reproduces those same 56 pixels with zero
GPU errors. Although two iterations were requested, the provider stops on the
first strict failure: this is **one** completed observer-off iteration, not two.
The public gate rejects both r226 and r227. The successful r221 does not replace
r28/r146/r171/r201 or this new counterexample.

The r225 original scene/bloom texture dump is missing: the 256-dump budget was
exhausted at seq 22644, before its composite at seq 23130. Replay final-input
textures exist, but must not be compared against those older live frames.
`compare_r225_pass_inputs.py` correctly rejects the missing original pair.
Next useful capture needs exact source-qualified texture selection before the
dump budget is charged, together with the binding witnesses. Preserve the
original failures while localizing the remaining FFP/selected-PSO, texture and
defined-content possibilities. No shader arithmetic, oracle or tolerance was
changed. The user freed disk space; no r171 raw evidence was compressed/deleted.

### Source-budgeted textures and FFP source-use discriminator (r230–r237)

`DXMT9_DUMP_DRAW_TEXTURE_CAPTURE_SOURCES_ONLY=1` selects the registered full
capture source before texture dedup/budget admission. It works independently
of effect/color logging. Completion metadata retains owner/source/raw/storage
generation witnesses; the published identity still owns qualification. Native
and x86_64 fixtures prove a budget of two selects the two captured draws rather
than earlier uncaptured ones, exact texture bytes, and malformed/nonrepeat/
unbounded/disjoint negatives. Full builds and focused tests pass (r231);
TLA r232 passes in 208.12s. Initial r230 fixture failure used an invalid
ColorFill on a non-RT managed texture; legal LockRect/UnlockRect setup fixes
the fixture without relaxing Metal validation or claiming a product fix.

r233 publishes `gt2-gpu-source-bind-20260913-r233/frame-1612042951287800-1`,
195 sources (24331–24525), 2,127 draw observations and 2,109 indexed geometry
hashes with no outside-published-source observations. Staged unix prefix is
`e916042abea2e3b7`, matching r234's actual loaded image. Source-plan SHA:
`10a659604fa4f67ae624ef6f1780dba7d3f3c92da29408a17d7a8a0ae3b5149e`.
Original/r234 final SHA is exactly
`a16c599068b381d029bf50e91399b1e72b4f36dc2fe37934f9497292eaf45ba9`.
The source-qualified original final inputs are now present at captured
24524/1 ↔ replay 195/1. `compare_bound_textures.py` authenticates that pair:
bloom is byte-exact and scene differs only at (54,124), BGRA alpha 236→237.
All scene RGB bytes match. This is positive final-output evidence with an
alpha-only intermediate difference, not closure of retained RGB failures.
Inline observers correctly leave the public receipt unqualified.

`audit_ffp_consumption_r236.py` authenticates actual compiler source bytes and
conservatively unions observed VS variants for every expected shader hash in
r225/r226 and r233/r234. No draw lacks a compiled variant. All differing
FFPVS backing bytes lie in the material/light region [192,1184); none of those
variant sources reads a field in that region. Reviewed escaping references
are declarations and the same-source texture-transform helper, which reads
only texture transforms. Both changed FFP VS pairs have identical text except
their final `// ffp vertex hash` comment (ten draws in each input). Raw shader
digests remain different and are preserved; no compiler input was changed.
This excludes those source-field differences under the observed variants,
not unexpected GPU reads or a per-draw selected-PSO mismatch.

Native public GPU-policy/source/texture/compiler integration r235 passes 6/6.
To distinguish RGB divergence from raw alpha differences without retaining
large pass images, r237 adds channel SHA-256 metadata over already completed,
tightly packed BGRA8/RGBA8 readbacks. Unsupported formats/extents have null
witnesses. It adds no GPU readback or pass split and never replaces the full
oracle. Native observer/public Metal gate passes 2/2; x86_64 Direct oracle,
observer and compiler-lineage tests pass 3/3, with full native/x86 and both PE
builds. This is a cold pure byte projection, not a lifecycle transition; the
source/lifetime TLA gate remains r232. Original r171 raw dumps remain intact.

### Paired channel endpoints and retained failing input (r238–r240)

r238 publishes `gt2-gpu-source-bind-20260913-r238/frame-1612952050628300-1`
with 166 sources (22232–22397), 1,853 draw observations and 1,835 indexed
geometry hashes. The staged unix prefix `dbeb00dc8544216b` matches r239's
loaded image. The source plan is
`77022802b8bc9f4cbb2693c9ddd3d9db17f4fe5f02d058c9badb45722519dc1c`.
Original/r239 scene, bloom and final output are exact; final SHA is
`ea851c919f1c8a6e45148c8fd07e74ee3193a4d5ea7f97c2838d9aefd45215a6`.
Actual replay coverage is 1,843 Direct plus ten compatibility draws, with
163 leases and zero GPU errors. This remains diagnostic, not policy promotion.

`compare_bound_color_channels.py` pairs all 179 recorded color endpoints,
with no outside-published-source rows. All have effective Store witnesses:
131 are raw-exact, two differ only in alpha, and 46 R32F endpoints differ.
No BGRA/RGBA endpoint differs in RGB. The first R32F candidate is captured
22256/0 ↔ replay 26/0, 2048×2048. Unlike older records, this candidate is not
an unknown/DontCare store observation; initialized/live contents and canonical
cross-process attachment identity are still unproven. Original per-pass raw
bytes are not retained. Do not label these 46 float differences as the cause
of an absent final RGB failure. r238 repeats the earlier FFP source-use result.

Observer-off r240 replays the **unchanged r225** input once on the final build:
`accepted=false`, `diagnostic=false`, 56 differing pixels, max RGB delta 5,
total RGB delta 138, alpha exact and zero GPU errors. Readback SHA remains
`9605f077806fc8b5353c9d0762e28fadc8a3ffb59e7152170c3a3ec0ee9f6573`.
The new positive captures do not close r225 or the older counterexamples.
Next discrimination needs failing-source scene writer/actual resource-use
evidence, while preserving the existing originals and exact public gate.

### Actual issued indexed geometry on the retained failure (r241–r243)

The source-geometry observer previously read the original logical index buffer,
not the final index argument after staging/reordering. The existing
effect/bindings/geometry flags now also observe successful indexed Metal calls
and actual stream-0/DrawVolatile bindings in cold encoder-local diagnostic
storage. Selection resets before draw filters; encoder creation resets the
binding shadow. Missing/private/unreadable/overflowing ranges remain unknown.
No draw payload, ABI field, pass boundary or runtime policy is changed.

The real A→B→A Metal fixture checks three issued rows on compatibility,
ProductionSlot/direct-cbuf and ProductionSlot/argument-table routes, against
independently hashed original index/vertex fixture files and exact pixels.
Master-off, bindings-off, rejected material filter and missing capture registry
negative runs retain exact pixels and emit no issued rows. Full native/x86_64
builds pass (r242); extended lineage/observer/Direct Metal tests pass 3/3 on
x86_64, native lineage and TLA pass 2/2 (TLA 187.03s, r243). These are
observer/binding pins; TLA does not prove geometry bytes or GPU output.

`replay-r225-issued-r243` runs the **unchanged failing r225** bundle once with
the authenticated source plan and capture-source cuts. Its 226 sources contain
2,231 observed draws and 2,213 indexed source/issued-geometry pairs:

| Comparison | Result |
|---|---|
| Replay logical index/stream-0 ranges versus retained live r225 rows | 2,213 aligned rows, no differing compared field |
| Actual issued index and bound stream-0 versus replay logical geometry | All 2,213 readable/known; no differing hash, count, stride, min/max or effective base vertex |
| Final index storage offset versus logical offset | Five staging rebases; index bytes and counts still match |
| Remaining observed draws | 18 without indexed-geometry coverage |
| Original output versus actual replay readback | 56 pixels / 74 bytes, max RGB 5, total RGB 138; alpha exact |

`audit_issued_geometry_r243.py` reauthenticates the manifest, source settlements,
loaded replay image, output/source readback hashes and original pixel deltas.
`r225-issued-geometry-r243.json` retains its comparison and exclusions. Replay
unix SHA is `f1febe037391726e73125fde998e7dc8492de51e68afccc20bbea5262e7d3928`;
it differs from r225's live image, so this is not a new same-build paired
capture. The final SHA is unchanged from observer-off r240:
`9605f077806fc8b5353c9d0762e28fadc8a3ffb59e7152170c3a3ec0ee9f6573`.
The provider reports zero GPU errors, 2,221 Direct and ten compatibility draws,
223 leases and 227 encode chunks; the public gate remains rejected.

This narrows a replay-side final VB/IB selection hypothesis, not GPU-read
lifetime, other streams, non-indexed draws, selected PSO/raster state or first
scene-writer divergence. FNV range equality is diagnostic rather than collision-
free proof. No new wild capture, oracle replacement, tolerance change or raw
evidence deletion/compression was performed in this step.

Final native integration r245 passes public GPU-policy Python/actual-Metal CLI,
compilation-lineage and color-observer tests (4/4). The r244 invocation used a
nonexistent shortened test name and ran no tests; its error log is retained.
The corrected result is `native-public-gpu-issued-r245.log`. Performance-doc
source audit and `git diff --check` pass. The requested Omni agent roles were
unavailable in this session, so this step has Main verification, not an
independent agent review.

### Actual issued raster state and bounded scene selectors (r246–r253)

The existing effect/BINDINGS/GEOMETRY conjunction now tracks raster arguments
after actual Metal calls in cold encoder-local storage. Indexed and nonindexed
issued-draw rows carry exact viewport, scissor, raster-mode, depth-bias and
stencil-reference bytes, with separate known flags for optional bindings.
The actual pipeline object's runtime label is also recorded, but `encodeDraw`
rewrites that label from the desired variant: it is **not** selected compiled
source or PSO-descriptor identity. No hot payload, ABI or policy fields change.

The three-draw A→B→A fixture pins viewport/scissor/zero bias and matched draw
identities on compatibility, Direct-cbuf and Direct-argbuf paths. Disabled,
filtered and missing-source runs emit no raster observations and retain exact
fixture pixels. r246's forward-declaration compile failure and r248's incorrect
creation-time-label expectation are retained failed attempts. Native/x86_64
builds r247 pass; corrected x86_64 Direct/public CLI/lineage tests pass 3/3
and native lineage/TLA pass 2/2 in r249 (TLA 181.08s).

Unchanged r225 replay r250 retains its same 56-pixel failure and output SHA.
All 2,231 actual viewport XYWH values match retained live FFP viewport bytes;
original z-range/scissor/raster equality is not inferred from that check.
`r225-issued-raster-r250.json` records the known-state populations and
`replay-r225-raster-r250-geometry-audit.json` retains the independent indexed
geometry comparison.

Optional color width/height selectors now intersect existing identity/pass
selectors before readback/dedup/budget admission. Strict positive uint32 parsing
rejects zero, malformed and overflowing values by disabling the observer.
Extent equality is not canonical resource identity. Both event/segment Metal
fixtures cover equal/disjoint/invalid selectors with unchanged strict pixels.
Full native/x86_64 and both PE builds pass r251; color/lineage tests pass 2/2
on native r252 and x86_64 r253.

### First paired failing scene endpoints (r254–r256)

r254 completes canonical GT2 capture and publishes
`gt2-gpu-source-bind-20260913-r254/frame-1615719355935000-1`, with recovered
`.3dr`, no runner failures, 218 sources (22627–22844), and plan SHA
`1a916fe4f75cedbe9ebb548319b9a45b44a2cfa25e69e5a686b158997951e5c3`.
Original and replay use the same 10,028,432-byte unix image, full SHA
`37d6e116c4d8ca920598efdb170bf037832da48ea2ad9553a6f997c83dfac2d5`.
The scene selector retains hashes, not raw pass images; compiled sources and
final scene/bloom texture bytes are retained separately.

`r254-channel-comparison-r255.json` authenticates all 132 source/pass/index
pairs: 33 raw-exact, one alpha-only and 98 RGB-different endpoints, with no
observations outside published sources. All are Store-qualified, single-sample.
The first observed stored RGB difference is source 22693 / replay 68, encoder
0, attachment 0. Its immediate predecessor 22692 / 67 is raw-exact, SHA
`e1c315d9ec9108492dd9f610cdab1a3b54f8aa1a541c501a68b531f24cac1910`.
Source 22693 differs in B/G channels; no original raw pass image exists to
assert the earliest within-pass pixel or draw.

`r254-final-input-comparison-r255.json` narrows the downstream evidence:

| Cut | Original/replay comparison |
|---|---|
| Final scene texture, BGRA | One pixel `(794,629)`: B 16→15 and G 20→19 |
| Bloom texture | Raw-exact SHA `f64ff6aea0c6c69954f19aa312d651a5c9306f8c4c5558bf72beced85ad117b2` |
| Final RGBA | Three pixels/four bytes, maximum RGB delta 1, total RGB delta 4; alpha exact |

Original output SHA is
`856a6107d13c7b0ae8ea64141c1be3f15f0e59c50d0512870191cacab67f4652`;
replay is `982bd80e477e0a283bb96ad10ebd589c325698abced82f261bb59232c5f6bbc2`.
All runs remain rejected by the public strict gate even though the provider's
older envelope reports a match. r255 observes 2,241 Direct and ten compatibility
draws, 215 leases, 219 encode chunks and zero GPU errors. With all these
observers disabled, r256 completes **both** requested cold-device iterations
with the same wrong output SHA and exact same residual. This excludes a
replay-observer-only explanation; it does not prove that live capture was
unperturbed.

### Source-qualified binding and wire provenance audit (r257–r258)

The artifact comparator now joins binding events through each stream's unique
source/draw keys, not array position or raw process-local ordinal. Unequal event
populations remain explicit and are never padded with assumed state. The live
parser also recovers complete bracketed effect records appended to another
logger's line. This recovers the FS-volatile row at source 22719 / ordinal
804586; the original log is unchanged. Malformed/incomplete records are not
synthesized. The earlier failed comparator outputs remain failed attempts.

`r254-bound-comparison-r255-r257-summary.json` reports:

| Witness | Paired evidence |
|---|---|
| Actual indexed index/stream0 bytes | All 2,233 hashes and range arguments match; seven byte-preserving index-offset rebases |
| Source geometry | All 2,233 compared rows match |
| Actual viewport/scissor/raster/bias/stencil ref | All 2,251 draw rows match; blend binding remains explicitly unknown when unused |
| VS/FS volatile | 2,251 paired rows each, exact compared fields |
| FFP VS binding events | 230 paired exact events; replay-only event at source 22737, draw index 1163 is uncovered |
| Per-draw tracked VS/PS/FFPPS backing | Exact compared bytes; FFPVS differences remain in material/light `[192,1184)` |

Actual compiled-variant use audit `r254-ffp-consumption-r255-r236.json` finds
no material/light field use in its observed VS variants. Ten changed FFP VS
sources differ only in the final hash comment. This remains conservative source
inventory, not selected-PSO or GPU-read proof. At candidate source 22693 the
observed VS is `0xafb70b0c1c1d7e46`, PS `0x1d74cc2b0f5def18`. Its compiled VS
sources differ in the unused dynamic-stride fallback (64 versus 60); actual
issued stride is 64 in both runs. This is a discriminator, not an attribution
of the pixel failure to compilation.

`audit_scene_source_r258.py` validates the immutable bundle then partially
folds its canonical attachment/texture state. It joins all 2,251 draws by
source and primitive count, explicitly accounting for production TriangleFan
lowering. `r254-scene-source-r258.json` records the bounded candidate closure:

- Source 22693 is event 775 records 23–33, global zero-based draws 677–687.
- Scene surface identity `{kind=1,generation=155,objectId=4294967657}` aliases
  texture `{kind=0,generation=13,objectId=4294967525}`, subresource 0.
- The parent has a complete 3,145,728-byte seed (event 66), followed by full
  1024×768 color/depth Clear at event 100 record 1. The final composite at
  source 22843 reads that same canonical texture through slot 0.
- Candidate texture slot 0 is produced R32F 2048² identity
  `{kind=0,generation=1,objectId=4294967542}`. Before the candidate range,
  371 draws in sources 22654–22688 write it; the last is event 602 record 49.
  Original intermediate bytes for this input are **not** retained by r254's
  scene-sized hash selector. Other sampled texture descriptors promise full
  seeds, but that alone is not proof of the actual bound view/sampler contents.

This closes logical scene seed/alias/live-out provenance, not independently
exported runtime canonical identity or shader-visible input equality. The next
useful discriminator is the produced R32F input and actual selected PSO/view/
sampler at this failing scene-writing family, followed by a reduced pixel
counterexample. Do not relabel these eleven candidates as the first wrong draw
or inherit r238's unrelated R32F differences as r254 evidence. Original r171
raw evidence remains intact; no raw dump was compressed or deleted. Disk
headroom subsequently recovered to 15 GiB; r261 below resumes original capture.

Final native integration r259 passes strict GPU-policy Python, actual GPU-policy
CLI, compilation-lineage and color-observer tests (4/4), recorded in
`native-gpu-policy-r259.log`. The final binding comparator additionally requires
readable/nonempty index and stream0 ranges and known raster fields; matching
unknown flags or empty hashes cannot support its equality claims
(`r254-bound-comparison-r255-r259-summary.json`). Performance-doc source audit
and `git diff --check` pass. No additional renderer change is attributed to
this artifact-analysis step, and the strict GT2 failures remain open.

### Bounded producer/shader audit while capture is space-blocked (r260)

The retained wire audit shows a full 2048² color/depth Clear for the produced
R32F input at event 316 record 25 (source 22654), before the 371 writer draws.
This is not a missing-initialization finding. The scene candidate PS samples
exactly slots 0, 1 and 2: four point-filtered projected R32F comparisons,
one trilinear 2D material sample and one cube sample. The emitted fragment
source does not sample the other six slots present in the materialized mask.
Its four shadow comparison results are averaged in quarter steps before the
lighting contribution; a one-byte final change does not imply a one-unit R32F
input difference. Actual sampler object/view equality is still unproved.

`r254-candidate-compilers-r260.json` authenticates compilation receipts for the
seven VS and three PS families belonging to the bounded writer/candidate set.
Five VS families have a common actual source. Two unmatched VS variants
(`0x239b51372981bb37`, `0xafb70b0c1c1d7e46`) differ only in dynamic-stride
fallback constants. All three PS families have common actual compiled sources.
The 391 indexed draws in these source ranges all have known nonzero issued
strides (24, 32 or 64); that population includes other-target draws in shared
sources and must not be reported as the 371 R32F writers. As with prior
inventory audits, common compiled sources are not selected-PSO proof.

The next original capture can scope sampled input evidence to those three
shader-visible slots, including the complete R32F input, instead of all nine
materialized bindings. Original hashes/readbacks and strict rejection remain
unchanged. No new GPU run, original replacement, compression or deletion was
performed in r260; disk headroom and the requested evidence-compression
approval were unresolved at that checkpoint. No compression was authorized;
the subsequent free-space recovery permits capture without replacing evidence.

### Complete candidate-input capture after disk recovery (r261–r262)

r261 naturally completes canonical GT2 and publishes
`gt2-gpu-source-bind-20260913-r261/frame-1632909947149800-1`, with its `.3dr`
retained. Its 172 sources (22508–22679) contain 1,900 observed draws, including
1,882 indexed draws. The authenticated source-plan digest is
`3f3300640abab88f9e3e026d4706826047341f57c220dd5abd69687f82a7106b`.
The live and replay providers have identical image bytes; the renderer is
unchanged from r254/r255. Capture has a 3-GiB free-space guard and a finite
32-texture budget, no gputrace or raw color-pass dump. The observer tree is
173 MiB and the original Tape is 131 MiB. No prior evidence was deleted.

r262's final 3,145,728 bytes exactly match original SHA-256
`902602d872e07db1a8655fb29f13d66ad5fd17aa56f6814920dd86a822c930cb`.
Its 83 paired scene endpoints are 81 raw-exact and two X8 alpha-only
differences, with no observed RGB divergence. This is positive diagnostic
evidence, not a replacement for any failing frame or an accepted timing run.
The public wrapper still rejects diagnostic policy ranking.

`compare_candidate_inputs_r262.py` joins each sidecar by authenticated source,
physical encoder, logical slot and the exact aligned draw-index use set. A
source/pass/slot alone is insufficient: slot 1/2 can change resource inside a
pass. All 32 paired texture observations cover every selected candidate draw
and slot; all expected mip/face subresources exist and have equal extents.
The 22 DXT1/DXT3/cube observations are byte-exact. The ten R32F observations
contain the same 209 differing float values/bytes, maximum absolute difference
`4.172325134277344e-7`. These input differences coexist with exact final RGB,
so they cannot alone establish the cause of retained output failures.

This readback reconstructs the desired shader-read view and runs after the
existing pass; it is not an observation of the actual Metal texture/sampler
binding. Canonical resource/no-intervening-writer proof and sampled-location
causality remain separate obligations. No shader, policy, tolerance or original
oracle was changed during this artifact-analysis step.

### Failing input-complete pair and no-writer interval (r263–r267)

r263 naturally completes canonical GT2, recovers `.3dr`, and publishes
`gt2-gpu-source-bind-20260913-r263/frame-1633227956874000-1`. Its 173 sources
(22185–22357), 1,868 draws and 1,850 indexed draws authenticate against plan
`ad1f38ccdde74933dd9b9ec197e29529a03cf8433a666462f57eb092411bfa12`.
Same-image replay r264 fails exact equality: 72 pixels / 94 bytes, maximum RGB
delta four and total RGB delta 126; alpha is exact. Original output SHA-256 is
`e2cefc4ecbf415a79f0d36eb081b99b187b879cdf8bbc84f16fe9f20a88439ab`;
replay is `4a3a0faa925ad75b4499aa90cc1605b047648cc5ce6a2ed5c30f0a94371f9624`.

All 83 scene endpoints pair by source/pass/index: 17 raw-exact and 66
RGB-different. The first observed difference is source 22237 / encoder 0
(replay 54), preceded by a raw-exact same-scene endpoint at source 22236.
The bracket is four indexed draws, event 463 records 56, 57, 59 and 62
(zero-based global draws 564–567). Their three PS families differ from the
texture observer's selected `0x1d74cc2b0f5def18` family, whose first readback
occurs later at source 22239. Therefore the complete slot 1/2 material/cube
coverage for the selected family is **not** input coverage for these four draws.

`r263-scene-input-provenance-r265.json` separately transports the slot-0 R32F
observation back to this bracket through the immutable wire:

- Canonical texture is `{kind:0,generation:1,objectId:4294967542}`. A full
  2048² color/depth Clear precedes 396 writer draws. The final writer is event
  463 record 44, source 22236, global draw 554.
- From that writer through the source-22239 observation, 70 commands are
  35 draws and 35 VS/PS constant writes. No draw targets the texture's aliases;
  there is no Clear/copy/resolve/mipmap or unknown command in the interval.
- The 141 intervening non-chunk events are 23 definitions and 118 mutations
  of other qualified identities. None defines, destroys or mutates this
  texture or an alias. They are checked, not silently omitted.

Thirty paired texture observations cover every selected-family draw/slot and
all expected mip/face subresources: twenty DXT1/DXT3/cube observations are
byte-exact; ten R32F observations retain the same pair of raw hashes.
R32F differs at 1,415 texels / 1,422 bytes, maximum absolute float delta
`2.384185791015625e-7`. `r263-shadow-bit-delta-r267.json` shows positive finite
values in `[0.6881486773,0.9970389605]` and signed bit differences from -4 to +4
ULPs. This numeric shape does not establish rounding, shader codegen, sampling
or pass boundaries as the root cause. In particular r261's 209 changed R32F
texels coexist with exact final RGB.

The actual issued indexed ranges all pair (1,850; seven byte-preserving index
offset rebases); raster scalars and VS/FS volatile bytes pair at all 1,868
draws. One FS skipped-push decision differs without changing observed bytes.
One replay-only FFPVS binding event remains uncovered. FFP material/light
backing, ten FFP shader hashes and 21 VS constant hashes differ; neither full
state equality nor selected compiled PSO identity is claimed.

Observer-off cold-device r266 reproduces the identical 72-pixel result with
zero GPU errors. Although two iterations were requested, the provider rejects
the first source mismatch and stops: only **one** iteration completed. These
captures/replays are correctness diagnostics, not performance evidence.
The next discriminator is sampled-location/controlled-input causality for the
R32F difference, followed by its first diverging writer or actual selected
PSO/view/sampler lineage. No additional original capture is required merely to
repeat the already-retained r263 final failure.

Final unchanged-build verification r268 passes all four focused native targets:
GPU-policy Python, actual GPU-policy CLI, compilation-lineage and color-observer
Metal tests (`native-gpu-policy-r268.log`). Performance-doc source audit and
`git diff --check` also pass. No renderer change or TLA-model change is made in
r261–r268; these results validate the existing gate/observers, not the unresolved
original GT2 fidelity. No benchmark or replay process remains running.

### Controlled R32F intervention, r269–r295

The cold `--audit-texture-input` path replaces only texture identity
`{kind:0,generation:1,objectId:4294967542}` after captured source 22236, before
the first observed bad scene bracket. Admission authenticates the exact source
plan, generation, complete single-level 2048² extent and immutable digest.
It drains the source, reads the target, uploads and verifies the replacement
before allowing the next source. Nothing edits the Tape or either oracle.
All intervention receipts are diagnostic and cannot qualify a policy.

| Run | Input at cut | GPU post-upload bytes | Final difference from original |
|---|---|---|---|
| r274 | No intervention | Not requested | 72 pixels, RGB max 4, total delta 126 |
| r275 | Replay-produced R32F | Exact replay hash | Identical to r274, including SHA-256 |
| r276 | Original-produced R32F | Exact original hash | Identical to r274, including SHA-256 |
| r281 | Original input + selected-view observer | Ten dumps retain exact original hash | Identical to r274 |
| r286 | All-zero R32F sensitivity control | Exact zero-input hash | 515,132 pixels, RGB max 123 |

All five runs complete exactly one requested iteration and preserve zero GPU
errors. The normal/control/original final SHA-256 is
`4a3a0faa925ad75b4499aa90cc1605b047648cc5ce6a2ed5c30f0a94371f9624`;
the independent original remains
`e2cefc4ecbf415a79f0d36eb081b99b187b879cdf8bbc84f16fe9f20a88439ab`.
The target before hash is always
`e450c30208a056b7376eedf47e3a78983882d53a2cdb1a3231078d17226ff899`.
The original replacement/post-readback hash is
`844ffcd65306ef0088076a182a6670bb05f82da7aa3f1beac467c1757d12ca33`.
Raw receipts and readbacks are retained in `replay-bind-r274-base`,
`replay-bind-r275-control`, `replay-bind-r276-original`,
`replay-bind-r281-original-sampled` and `replay-bind-r286-zero` beneath the
artifact root. The r281 observer has its own sibling `-observer/textures` tree.
`audit_texture_intervention_r291.py` reauthenticates all five runs' identical
loaded-image identities, staged event/identity/oracle bytes, raw output/source
bytes and replacement digests; `r263-texture-intervention-r291.json` retains
the independently recomputed pixel/byte counts.

The same-path control excludes the added drain/upload alone as a change to
these final bytes. The original-input result, together with the sensitivity
control, lowers the priority of the observed R32F ULP difference for this
specific final mismatch. It does not establish a universally irrelevant input:
the observer reconstructs a view from the state/Pool, not from all actual Metal
bind calls. Different inputs of the earlier four PS draws and selected compiled
PSO/view/sampler lineage remain open. Prefer that bracket over another capture
that merely reproduces the known failure.

Failed development evidence is retained and excluded: r271 rejected a valid
wire descriptor because the new diagnostic compared D3D format 114 to the
internal enum; r273 corrects that domain mismatch. r279 dumped textures under
the final output path and collided with atomic publication. Its raw texture
files/log survive, but the historical wrapper deleted private staging on that
exception; its receipt is not claimed. The diagnostic now opts into rejected
artifact retention, and r281 uses an external observer directory. Native test
setup failures r278/r282 and identity-fixture mismatch r287 are likewise not
renderer counterexamples or passing tests. Original r171 data is untouched.

Final native/x86 builds r294 pass. The five focused tests in
`native-gpu-policy-r295.log` pass: provider Metal fixture, GPU-policy Python,
GPU-policy CLI, compilation lineage and color-observer Metal fixture. New
coverage includes twelve malformed request/mode cases plus unsupported format,
exact pre/post GPU bytes, owned CLI input staging/forwarding, an independently
qualified non-intervention baseline, permanent intervention rejection even
when pixels match, and populated evidence retention after an actual observer/
publication-directory collision. `tla-r289.log` passes the existing full TLA+
suite (180.58 seconds); it covers the existing lifetime/source contracts, not
driver pixels or a new input-intervention model. No interleaving policy is
changed by the final descriptor/CLI validation tightening. Performance-doc
source audit and `git diff --check` pass. No live benchmark, replay or build
process remains. None of these tests closes the retained GT2 mismatches.

### Actual extra-stream coverage: r296–r308

The first r263 four-draw bracket uses stream 1 as well as stream 0. In
particular, the first two VS variants read tangent/UV values from Metal slot 6.
Existing stream-0/index equality did not cover those inputs. All three VS and
three PS generated source candidates for that bracket are unique and byte-equal
between retained live/replay shader directories; generated-source equality is
still not selected-PSO provenance.

`EffectIssuedGeometry` now records actual `setVertexBuffer` buffers/offsets at
slots 6–20, clears those witnesses on `setBytes`, and resets on real encoder
creation. Selected indexed draws emit one `dxmt9-effect-issued-stream` row per
declared programmable extra stream. Issued index/base-vertex/instance parameters
select a bounded Shared range; stride is explicitly expected declaration
metadata, not compiled-PSO reflection. Absent/private/out-of-bounds bindings are
unknown, and rejected filters/suppressed calls do not emit issue witnesses.
No hot-path payload, lifetime policy or queue interleaving changed.

The three-draw native fixture uses stream 1 for POSITION at a nonzero offset
into the second half of one VB. Mutating only that half collapses its geometry
and changes pixels while actual stream-0/index hashes remain unchanged.
Compatibility and Direct paths both pin the actual extra-stream bytes, route
coverage and strict expected digest; master/binding/filter/provenance negatives
emit no issued rows. Native build r296, x86 build r298, PE builds r301 and the
five-test native group r306 pass; x86 compilation-lineage Metal test r307 passes.
Final native/x86 reruns r309 additionally pin unchanged stream-0/index witnesses
under the stream-1-only mutation. Performance-doc source audit and diff check pass.
The first PE build invocation r300 lacked llvm-mingw on PATH, preserved as
build-environment failure, then succeeded with the configured toolchain. These
are observer/fixture changes, so no new TLA interleaving model is claimed.

Fresh original r300 and replay r303 authenticate the same provider SHA
`212fa910a0836472fbd589cc2bbf83831b336eaa2f77ac0fd1dab2293b8aae78`.
The 179-source plan SHA is
`3c59e0cbdb33e954b9ef255ba1abd83a6b26d7a5246eb24c0ad2fc1e48949c9d`.
`live-bind-r300-audit.json` and `r300-issued-streams-r303-r305.json` retain the
complete source/draw join: 1,902 draws, 1,884 known indexed ranges and 107 known
extra-stream ranges, all compared indexed/extra fields equal. Hash equality
of Shared bytes at encode time does not prove GPU reads or later writes.

The original output SHA is
`b5c64edc68da125540cf2a6e419c0e3110e1fa8efe03a1ba911ef6e3394808a9`;
replay is `e8c71170c957ad3d01d4c6d8f00ea27d80973b5fa43f7b449978f04ae40f625e`.
Two bytes in two pixels differ, maximum RGB delta one and total delta two.
Both observer-off cold iterations r308 return the same latter SHA with zero
GPU errors. The lower diagnostic experiment reports `complete` under its
preexisting pixel envelope; that is **not** strict GPU qualification and its
public receipt remains diagnostic/unaccepted. No threshold was changed.

`r300-channel-comparison-r303.json` pairs 80 stored scene-sized endpoints:
36 raw-exact and 44 RGB-different. The first observed G-channel difference is
source 22190 / encoder 0 (replay seq 136), event 843 records 24–34, global draws
1457–1467. Source 22189 on the same scene is raw-exact. All eleven draws use VS
`e95ba3710cc86879` / PS `7833784ed50d98c3`. Candidate textures from the earlier
selected `1d74cc2b0f5def18` family are not proof of these draws' actual bindings.
Next discriminators are immutable selected-PSO provenance and actual
texture/view/sampler bindings at the first-bad brackets, not another broad
capture or a relaxed oracle. Original r171 raw evidence remains untouched;
free disk after this bounded capture and final tests remains about 11 GiB.

### Actual selected-PSO creation and causal intervention: r310–r327

The cold observer associates successfully published compilation-source receipts
with actual Metal libraries, propagates them through named functions, then
records the descriptor passed to successful render-pipeline creation. Issued
raster rows query the actual bound pipeline's immutable association. JSON
receipts in `shaders/pipelines/<sha256>.json` include source hashes, actual
function names, attachment/blend/write-mask arrays, buffer mutability, sample,
raster, topology, tessellation, depth/stencil formats and pipeline options.
Associations die with their Metal objects and do not retain those objects.
Missing publication, conflicting interned associations, binary-only libraries
and specialized functions remain unknown. This does not prove driver-internal
binary choice, reflection completeness or GPU execution.

| Original / replay | Draws with authenticated actual PSO chain | Final output | Actual VS source differences |
|---|---:|---|---:|
| r315 / r316 | 1,902; unknown 0 | Exact | 916: 906 fallback stride literal, 10 FFP hash comment |
| r319 / r320 | 1,898; unknown 0 | Exact | 906: 896 fallback stride literal, 10 FFP hash comment |

Both pairs use provider SHA-256
`c023b0f605b398f7164d5514add4c54c715e25799fb3214e8c6ac321fe3937e4`.
Their concrete pipeline descriptors and fragment sources match on every draw.
Every fallback-stride difference has identical, nonzero issued DrawVolatile
stride, making the differing fallback branch inactive; this is not a claim of
equal compiled machine code. `r315-actual-pso-r316-r318.json` and
`r319-actual-pso-r320-r318.json` authenticate creation/source receipts;
`r315-actual-shader-deltas-r316-r325.json` and
`r319-actual-shader-deltas-r320-r325.json` classify exact source deltas.
The initial r317 derived counter summary had a list/count bug and is superseded
by corrected r318 audits; the original evidence is retained.

The r315 source-plan digest is
`ccd01549a59b184d1de2dc5415ba6ac904b7d0fde53389fe80799ff8e40fa2ad`,
with final SHA `741a31f60e97263dffadc21070718f3abe777feec563445ca33670cbc80ce975`.
Its 75 scene endpoints are 73 raw-exact and two alpha-only. The r319 plan is
`4e9fe910383cf2e7d9084d37d2a746dd8105c303affd7a0b3f0cc8ad8bc38514`,
with final SHA `bdde3cb07b575eb4f73b18b5ef68e2f4d7eb08f8171f32a0a579e047a4b9091e`.
Its 73 endpoints are 20 raw-exact and 53 alpha-only. Neither has an RGB endpoint
divergence. Neither closes any earlier failing original.

The same-input prefetch intervention is stronger causal evidence than these
positive pairs. `r263-prefetch-pso-intervention-r327.json` independently checks
the unchanged source plan, identical loaded-image lists, actual PSO/source
receipt hashes, the single render/readback attempt and raw final bytes for explicit prefetch
on r326 and off r323. Of 1,868 draws, 872 select different vertex source SHA-256;
all other serialized creation fields match. Both outputs remain
`4a3a0faa925ad75b4499aa90cc1605b047648cc5ce6a2ed5c30f0a94371f9624`,
72 pixels different from the retained original. Global draws 564–567 in the
first-bad source 22237 bracket have identical creation receipts in both runs.
Original r263 predates this observer, so this is replay/replay causal evidence,
not retroactive proof of the original PSO. Original r300 also retains exactly
the same two-pixel output under prefetch-off r322. All are diagnostic runs;
no policy is GPU-qualified by this intervention.

Receipt status correction: r323/r326 both execute through final readback but
their provider exits 1 with `source-mismatch`, experiment status `failed` and
`reset_ok=false`. They are not successfully completed cold iterations. The
same-output and changed-PSO causal observation remains valid; the r327 audit
checked one emitted iteration record, not successful completion. The explicit
status audit is retained in `issued-sampling-r335.json`.

Final canonical native/x86 builds r313/r314 and PE builds r315 pass. The five
GPU-policy/provider/lineage/color-observer tests pass in r318. Native and x86
r324 add actual-render publication-failure negatives: absent shader dump or a
regular file occupying its directory yields unknown creation provenance while
pixels remain exact. Both compatibility/direct and constant argument-table
routes are covered. This is diagnostic creation metadata, not a rendering,
lifetime or interleaving change; no new TLA model is claimed.

### Actual fragment sampling descriptors: r328–r335

`winemetal_sampling_lineage.mm` snapshots texture view properties after actual
fragment texture binds and sampler creation descriptors after successful Metal
creation. Direct null binds replace prior snapshots. Actual argument-encoder
writes maintain an object-scoped table-slice witness; the actually bound slot-30
buffer/offset selects it at the draw observation. Shared table bytes must match
the recorded digest. Reopening, offset changes, setBytes and cache eviction
cannot reuse a prior slice witness. An unobserved raw mutation invalidates the
old entries before a subsequent observed write; that later write cannot bless
unobserved texture entries. All associations contain value snapshots/numeric
handles, not Metal resource retains. Missing sampler creation stays unknown.

The observer stores at most 16 direct stages, 32 argument indices, a 4 KiB table
byte envelope and 64 cached slices per buffer. `argument_table_known` means the
tracked byte envelope matches; individual missing entries remain unknown and
the flag does not assert complete sampling-stage coverage. Expected shader
stage masks are only audit selectors. No selected ABI, resource identity,
texture contents, residency or later GPU reads are inferred from descriptors.

The native fixture issues real bridge calls, samples mip 1 through an actual
view with a red base level and green mip 1, and reads exact green pixels through
both direct and argument-table fragment shaders. Observer on/off runs cover
direct null/A-to-B binding, missing sampler construction, exact LOD bits,
nonzero table offset, offset-only change, setBytes invalidation, table reopen,
unobserved-byte mutation followed by a sampler write and 65-slice eviction.
Each new real encoder begins without earlier descriptors. The production
three-draw fixture separately joins emitted sampling rows to exact draw IDs
and pins off/binding/filter/missing-source negatives.

| Retained input / replay r334 | Draw rows | Actual direct texture/sampler entries under expected stage mask | Original difference | Receipt status |
|---|---:|---:|---|---|
| r263 | 1,868 | 7,617 each; no unknown | Same 72 pixels and SHA-256 | Rejected source oracle, provider exit 1 |
| r300 | 1,902 | 7,519 each; no unknown | Same two pixels and SHA-256 | Lower diagnostic envelope completes, not strict acceptance |

No GT2 argument-table byte witness is present in these runs; its positive
coverage is the native pixel fixture, not GT2. `audit_sampling_r335.py`
reauthenticates source plans, original and replay bytes, loaded images, source
settlement joins and exact row counts. `issued-sampling-r335.json` retains the
first-bad four-draw r263 and eleven-draw r300 descriptors. In particular the
r300 bracket includes an actual single-level R32F cube view at stage 1; the
earlier r263 2D R32F intervention does not cover that different input. Both
retained originals predate sampling observation, so equality to original
bindings/contents is still unproven. Next evidence must join actual bound
resources to source-qualified contents in the first-bad bracket; descriptor
equality alone must not be treated as closure.

Canonical native/x86 builds r331 pass. Seven native focused tests and three
x86 tests pass in r333, including Metal validation. r332 is a retained test
invocation failure from nonexistent target names, not a test result. New test
warnings found in r329/r330 were fixed before r331. Existing unrelated bridge
warnings remain outside this change. No render scheduling/lifetime/ABI policy
or original pixel tolerance changed; no new TLA model is claimed for this cold
diagnostic. Performance-doc source audit and diff check pass. No old raw data
was compressed or removed; free disk remains about 10 GiB.

### Actual texture identity/content joins: r336–r350

The validated r336 wire fold identifies r300's single-level R32F cube
`(Texture, generation=4, objectId=4294967604)`: six initial mutations are
followed by six face Clears and 310 draw writers within the frame. Replacing
initial seed bytes cannot test the later dynamically rendered sampling input.
The earlier 2D R32F intervention is not a cube intervention.

The cold observer now gives each actual Metal texture/view a nonzero monotonic
process-local `object_token`; the existing selected-view readback preserves
`metalTextureObservationId`. The association dies with its object, does not
retain Metal resources, and cannot give a reused address its predecessor's
token. Zero is unknown. Cross-process matching still uses authenticated Tape
identity, not token equality. Native pins cover root/view/distinct-object
identity, repeated identity, null and disabled observation. The production
EventSerial/SegmentSerial fixture joins two actual issued fragment bindings
per mode to two completed 2x2 texture readbacks while retaining all four
strict pixel cuts. Texture-only observation has token zero as expected.

r337 builds caught the new metadata field at the wrong (depth dump) writer;
that placement was corrected before successful canonical native/x86 builds
r338/r339. Both PE build directories were rebuilt. Native 7/7 and x86 3/3
focused suites pass in r339; the extended end-to-end texture join passes
separately on native r343 and x86 r345. These are diagnostic identity tests,
not a new production lifetime/scheduling policy or a new TLA proof.

The first new live attempt r340 timed out before the 500-Present capture
selector and produced no Tape. Its catalogue `pass` is not capture success;
its screenshot shows Orca rather than the benchmark. A late window restore
observed a rendered GT2 frame, but does not independently prove the cause of
the stopped progress. The r344 probe-wrapper dry runs rejected unsupported
`--lane` and texture-selector combinations without launching. A fresh r346
uses the canonical catalogue runner, restores the benchmark window near
startup and exits naturally after 72.72 seconds with an authenticated Tape.
No old data was removed or compressed. No GPU trace or raw scene images were
added; scene endpoints are hash-only and cube readbacks have an explicit cap.

| Evidence | r346 original / r347 replay |
|---|---|
| Bundle | `gt2-gpu-source-stream-20260914-r346/frame-1641819752675600-1` |
| Source plan | `240c7917d78433f8e719985686efb5e5e5c48e2c87045189307554eb408c987b` |
| Coverage | 176 sources, seq 22296–22471; 1,853 draws |
| Original output SHA-256 | `90bd8a753323d9d5085677c9576922fee384dd352827b9b30a84335b38f02266` |
| Replay output SHA-256 | `1ad9883a32b47242d399d8434f3246e5fd0c9aa9be99ef068282feac36a12f33` |
| Final difference | 69 pixels, max RGB delta 4, total RGB delta 95; rejected source oracle |
| Stored scene endpoints | 20 raw-equal, 64 RGB-different |
| First observed scene difference | original seq22352/encoder0 -> replay58; global draws588–595; same-scene predecessor22351 raw-exact |
| Actual expected-mask descriptors | 7,573 texture pairs and 7,573 sampler pairs; no differing fields or unknown entries |
| Cube readbacks | Six pass pairs, original seq22423/encoder2 through22428/encoder0, all six 512x512 R32F faces each |
| Cube differences | Identical 2,168 float texels / 2,200 bytes in each pair |
| Actual texture tokens | Original43 and replay39; each matches its own process's issued binding, never compared across processes |
| Cube writer closure | 244 preceding draw writers, last global1366/source22423; explicit checked no-writer interval to each readback |

`audit_stream_capture_r342.py`, `audit_cube_join_r348.py` and
`audit_sampling_pair_r350.py` preserve the validated joins and exclusions.
The cube check covers all six canonical extents and every intervening record
and event; unmodelled commands/control or target mutations reject the join.
Unrelated Clear actions are checked using folded attachment identities. These
late cube samples do not explain the earlier source22352 divergence, and are
not original intermediate evidence for r300.

Prefetch-on r347 has 870 actual VS-source differences from r346: 860 inactive
fallback-stride literals and 10 final FFP hash comments. Prefetch-off r349
leaves only the 10 FFP comment differences. In particular all first-bad eight
draws now have equal serialized actual PSO creation inputs to the original.
Both replays keep the same wrong output SHA-256, cube contents and 69-pixel
failure. Both exit1 with `reset_ok=false`: they reach full readback but are
not successfully completed cold iterations or qualified policy candidates.
`cube-prefetch-r351.json` reauthenticates every retained cube face and both
final images, and checks equality of all 84 replay scene endpoints across
prefetch modes. Performance-doc source audit and diff check pass in r351.
The scene endpoint comparison and CPU-visible indexed/extra-stream witnesses
remain separately authenticated. Descriptor/source equality does not prove
driver machine code, actual depth/stencil creation, resource residency or
contents at GPU consumption. Localize those remaining inputs before changing
rendering behavior; do not widen the strict original-pixel gate.

### Bound inputs and actual depth/stencil evidence: r353–r358

`compare_bind_r353.py r346 r349` joins the authenticated source cuts and actual
draw ordinals. Viewport/scissor/raster and blend/stencil-reference fields match;
VS/PS/FFP-PS bound bytes and VS/FS volatile bytes also match. The entire FFP-VS
backing does **not** match: all 1,853 draws differ within the material/light
region (byte offsets 206–1175). The 187 paired FFP-VS bind events match, but
two replay-only events at global draws419/428 remain uncovered. Seven indexed
backing offsets differ while their observed geometry bytes match. Neither
unused-region reasoning nor CPU backing equality proves GPU consumption.

The cold depth observer now associates successful actual Metal state creation
with actual encoder binding and stencil-reference calls. It serializes depth
compare/write and both stencil faces. Fresh/unobserved, unknown creation,
defensive null and sticky conflicting creation are distinct; it does not
retain Metal resources or change rendering policy. Source-qualified
`dxmt9-effect-issued-depth` rows are checked beside the three production
draws on ordinary/direct/argument-buffer fixture routes. Disabled and filtered
rows must remain absent.

Native and x86 exact 4x4 Metal oracles cover nearer-depth rejection, unequal
stencil-reference rejection, and a later actual reference update allowing the
draw. Front/back masks and operations are independently checked. r355 retained
a test-only failure: nil depth-state binding is illegal with Metal validation;
null observation is now injected into the observer defense, not the Metal API.
No production rendering fix is inferred from that fixture error. Canonical
native/x86 r356 builds and both PE r358 builds pass; native 9/9 r356 and x86
5/5 r357 tests pass. New test warnings were removed; pre-existing bridge
warnings remain. These diagnostic changes require no new scheduling/lifetime
TLA model. Old originals have no actual depth rows and cannot gain them
retroactively. Strict original-pixel qualification is still open.

### Paired depth originals and retained residual: r359–r367

Both new originals use the existing bounded 500-Present, 64-record source
capture recipe. Each exits naturally (catalogue pass, no failures), with
hash-only scene endpoints and at most 16 selected cube readbacks. The actual
unix image is `a7f35a270fd8a89a38b3bd47c572191a7f1f6f64a570b34eadb6c886ee54a24d`
(10,073,008 bytes), equal between each new original and its replay. Replays use
identity/capture-source/production-slot, prefetch off and one cold iteration.

| Evidence | r359 / r361 | r364 / r365 |
|---|---|---|
| Published frame | `frame-1643595451541400-1` | `frame-1643854192908200-1` |
| Source count/range | 173; 22055–22227 | 180; 22125–22304 |
| Source plan SHA-256 | `0052dde58f8ddfeb8d93ccbb9e23e84218e961517198dc257517753962dfe70d` | `7041598544e50a30c2b2efed066760415f2bde53c2c0e50348d0c61d0ebcc22b` |
| Original/replay final SHA-256 | `7dd62e02a208439c3e61a336b6a250e93bfa83f73866da69640e37d469559c9a` | `323811c179b6513803d8bb72365df14e922589cab4a80ca8a9bfdf9e44d70a42` |
| Actual depth pairs | 1,857 equal; no unknowns | 1,859 equal; no unknowns |
| Actual texture/sampler pairs | 7,564 each, equal/no unknowns | 7,566 each, equal/no unknowns |
| Stored scene endpoints | 80 raw-equal, 2 alpha-only differences | 84 raw-equal, 2 alpha-only differences |
| Final strict pixels / reset | Exact / true | Exact / true |

`audit_stream_depth_r360.py` authenticates every original depth row against the
published source identity. `audit_depth_pair_r362.py` checks actual descriptor
status/reference and every draw join, current loaded-image hashes and final
readback bytes. Actual selected PSO creation audits and source classification
retain only ten FFP final-hash-comment differences in each pair. These remain
diagnostic, publicly unqualified receipts; no GT2 argument-table witness or
depth-attachment/GPU-test-result proof is inferred. Intermediate alpha-only
differences also remain visible rather than being called raw-equal.

r363 replays the retained r346 input on this same new binary. The final SHA
remains `1ad9883a32b47242d399d8434f3246e5fd0c9aa9be99ef068282feac36a12f33`:
69 RGB pixels, max4, total95, alpha exact, provider exit1 and `reset_ok=false`.
`audit_depth_outcomes_r367.py` independently rehashes both new positives and
the retained failing original, recomputes RGB/alpha differences, and verifies
source-plan receipts. r346 is explicitly a cross-build residual check, not a
new same-build paired-depth original. No old oracle was replaced. The first
r365 invocation preceded the source audit file and exited before launch; the
retained retry is the actual experiment. No evidence was removed/compressed.

### Actual direct-uniform binding and two interventions: r368–r378

The previous `bound_*_bytes` witness read the high-level tracked upload/cache
backing; it could not independently detect a skipped Metal bind. The cold
`metalDirectBufferMatch` witness now observes actual successful vertex/fragment
buffer and offset calls at slots 0/3, invalidates on setBytes, and compares a
non-reused process-local buffer token plus offset with the tracked binding.
Only value snapshots are associated with objects; no Metal resources are
retained. Fresh/null/inline/missing-origin/mismatched bindings do not match.
No hot execution payload, ABI, scheduling or resource-lifetime policy changes.

`metal_direct_buffer_lineage_spec.mm` uses actual Metal calls and six exact 4x4
pixel cuts. Its deliberately omitted B/256 rebind reports mismatch while the
GPU draws stale A/0 red; legal offset/rebind/setBytes cases draw green and
A/B/A returns red. Both stages and slots, equal-byte distinct objects and the
disabled path are covered. The three-draw production oracle requires four
matches per draw on ordinary/direct routes, never on its argument-table route.
Master/binding/geometry/filter/missing-capture negatives require witness absence.
r368 caught a const-pointer test compile error, fixed before canonical native
and x86 r369 builds. Native 11/11 r371, x86 7/7 r372 and both r374 focused
filter runs pass. The r378 recheck passes native 5/5 and x86 3/3, including the
public strict GPU-policy tests on native. Its first native invocation used an
incorrect test name and was rejected before execution; the corrected r378b
log is the completed result. This observer-only change does not claim a new
TLA proof.

All following runs load the same unix image
`2d09da2d6769c9559c10bf60c13c45a8e8d6c9a9ee5462d0b8810024faca29dd`
(10,078,000 bytes), with identical provider/native-library images. The retained
r346 original predates that build. Each intervention preserves its immutable
bundle and plan `240c7917d78433f8e719985686efb5e5e5c48e2c87045189307554eb408c987b`,
identity/capture-source/production-slot, prefetch off and one cold attempt.
Cube raw dumps are disabled; only bounded scene hashes and binding/shader
diagnostics are collected.

| Check | r373 baseline | r375 API + GPU validation | r376 dedicated transient buffers |
|---|---|---|---|
| Actual direct binding matches | 1,853 per VS/PS/FFPVS/FFPPS slot | Same | Same |
| All observed bound byte hashes vs r373 | Reference | Exact | Exact |
| All 84 replay scene endpoint hashes vs r373 | Reference | Exact | Exact |
| Original scene comparison | 20 raw-equal / 64 RGB-different | Same | Same |
| Original final RGB difference | 69 pixels; max4; total95; alpha exact | Same | Same |
| Provider/reset/strict gate | exit1 / false / rejected | Same | Same |
| Observed Metal buffers / transient uploads | 126 / 1,584 | 126 / 1,584 | 1,709 / 1,584 |

The final wrong SHA remains
`1ad9883a32b47242d399d8434f3246e5fd0c9aa9be99ef068282feac36a12f33`.
r375 stderr positively announces both validation layers; no failed-assertion,
error or out-of-bounds pattern is reported. This is not a memory-safety proof.
r376's `DXMT_DEBUG_FORCE_TRANSIENT_DEDICATED=1` reaches
`ResourceArena::uploadBuffer` through the direct uniform upload call. Dedicated
uploads use `newBufferWithBytes` and the existing seq-qualified retention; the
slab suballocation path is bypassed. At an equal-work counter snapshot,
1,584 uploads / 3,081,532 bytes and all four uniform-upload counts agree, while
buffer creation rises by 1,583 (1,584 dedicated allocations replacing one
slab). These are snapshot counts, not full-frame totals or FPS measurements.
The activation evidence is actual allocation movement, not merely the env var.

`audit_direct_cbuf_outcomes_r377.py` reauthenticates all three loaded images,
the source plan, original and replay readbacks, every bound byte hash, scene
metadata and effective env differences. Its retained JSON explicitly marks
cross-build-original evidence and `qualified=false`. Neither intervention
supports replay-side rebind omission or slab reuse as a sufficient explanation.
It does not prove the older original's binds, GPU-consumption contents, texture
history, original-side lifetime or all shader memory accesses. No tolerance or
original oracle changed; no evidence was deleted or compressed.

### First divergent bracket input inventory: r370

`audit_first_bracket_inputs_r370.py` joins the retained canonical wire fold to
the original's authenticated selected fragment source. Both shader groups in
source 22352 / global draws 588–595 actually contain texture samples: the first
two sample stages 0–6; the next six sample 0–2. Their stage 0 is a one-level
2048x2048 R32F shadow, canonical Texture/generation1/object4294967542, with
396 preceding draw writers and one Clear; the last writer is global draw554,
source22348. Both groups manually compare sampled shadow values to projected
depth. The remaining slots include static BC1/BC3/L8 textures, complete-looking
128x128 cube mutation sets and the first group's L16 lookup texture.

This is an actual selected-source sampling inventory, not Metal reflection,
sample coordinates or GPU content proof. Its partial fold finds no preceding
draw/Clear writer for the other inputs but does not assert exhaustive absence
of every control/mutation writer. It changes the next diagnostic scope from
the later dynamic cube to the earlier shared 2D shadow and scene/depth inputs;
the old r263 shadow-replacement negative cannot be transferred to r346 without
its own original-content evidence.

### Rejected shadow-input capture and progress evidence: r379–r382

All four canonical build directories are current before r379. Its recipe
selects only fragment shader `0xcf79d05af22c79e8`, slot0 and at most eight
post-pass readbacks, alongside scene hashes and one Tape after 500 Presents.
The runner records `status=pass`, but also `timed_out=true`, `returncode=-15`
and 251.25 seconds process elapsed. No Tape manifest was published. It is a
failed capture regardless of the catalogue status, not a rendering/FPS result.

The last periodic counter snapshot records 120 submitted Presents, one Arena
admission waiter, one session lease, seven Ready entries and 1,768 resident
pages (peak2,048), with zero recorded GPU errors. A later one-second process
sample places the replay worker in `waitForCpuReadyArenaAdmission`, encode in
`runCpuReadySessionEncodeLoop`, and finish/completion threads in condition
waits. The periodic counters are not a simultaneous queue-lock snapshot with
the sample, so no precise predicate/credit cycle or GPU-hang cause is claimed.
The run is terminal; no benchmark/replay process remains. No additional kill
was issued after the runner's timeout had already removed the benchmark.

`audit_capture_stall_r382.py` checks the missing manifest, timeout disposition,
loaded-image receipt prefix, snapshot and process sample, and hashes all retained
evidence into `capture-stall-r382.json` with `qualified=false`. The adapted
`audit_shadow_join_r381.py` is only prepared, not an executed shadow-pair proof.
Reduce the admission/session wait to a deterministic native progress case
before another long capture; the earlier r346 pixel residual remains separate
and unchanged. About 8.1 GiB remains free; no retained evidence was compressed
or deleted.

### Low-water/session-cap composition repair: r383–r389

The r379 executable is preserved as `winemetal_dxmt9-r379.so` with SHA-256
`2d09da2d6769c9559c10bf60c13c45a8e8d6c9a9ee5462d0b8810024faca29dd`.
The sampled return PC `0x2122132e3` minus image base `0x2120ba000` is
`0x1592e3`: disassembly binds it to the open-session/empty-Ready CV wait,
not first-lease denial. Only Ready work, stop, ordered release or an explicit
producer sequence wait satisfies that branch; admission pressure alone does not.
Before the 500-Present capture gate, `replayPlannedChunk` only enables capture
SegmentSerial after both the capture token and event ordinal are nonzero.
The recipe's identity-mode setting alone does not establish a multi-source
admission request in the sampled pre-capture stall.

`productionLoopReopensLatchedPagesWithPendingClear` fills the real capture
Arena with six sources (512,448,63,512,512,1 pages). Its fake backend consumes
the first three synchronously and carries valid deferred-Clear borrows for
the tail. The production coordinator/Tape retire 1023 pages, then hold 1025
with Ready empty and admission closed above lowWaterPages=1024. The unfixed
native test fails in 2.68 seconds, preserving this exact state. This is a
reachable composition defect consistent with the production wait, not proof
that r379 held precisely these pages at its later sample.

The fixed queue-local cap is `min(lowWater, highWater-successorHeadroom)`:
capture becomes 1024 pages and ordinary streaming 320. A source that would
cross that deterministic cap stays Ready and causes the existing cap release.
There is no timed/pressure release and no change to wire, GPU lifetime or
pixel tolerance. The repaired test submits before source six, completes and
reclaims the first five in order, reopens ordinary admission, and drains the
last source once without residual residency. The scalar algebra has 38,148
exhaustive native combinations. `SessionAdmissionHysteresis` passes safety and
writer/terminal progress (13 distinct states); its high-water-only companion
reproduces `NoClosedEmptySessionCycle`. The full TLA gate passes (58 normal,
4 progress and 128 expected-negative configurations); focused native 5/5,
native/GPU 7/7 and x86 focused 5/5 pass. Both canonical PE staging builds are
current, with no PE ABI change.

The new provider/unix images replay untouched r346 in r387 and remain strictly
rejected: RGB69 pixels, max4/total95, alpha0, output SHA-256
`1ad9883a32b47242d399d8434f3246e5fd0c9aa9be99ef068282feac36a12f33`,
byte-identical to r373. This is cross-build original evidence, not a newly
qualified input. `audit_low_water_repair_r389.py` authenticates the retained
executable/sample, before/after native logs, TLA outcomes and actual replay
readback into `low-water-repair-r389.json` (`qualified=false`). No wild FPS
or exclusive stall attribution is claimed. Existing failed originals remain
required; a bounded fresh shadow-input pair is still the next discriminator.

### Resumed capture and actual shadow-input difference: r390–r392

The same bounded GT2 capture recipe on the repaired staging set exits with
returncode0, `timed_out=false`, no runner failures, no chunk rejects and no GPU
errors. It publishes one bundle, 176 settled sources (`22148`–`22323`) and
1862 observed draws. This is a single successful progress run, not a FPS
measurement or an exclusive proof of r379's cause. The source plan is
`bb714f3d9d7df411d094deeb02f0fe3e1c2e336dc953b0d447b84004e45897aa`;
original RGBA is
`8a1aa382d9a8b2e138238beb7705e259cd503d6de065f73e7315fcbe28ac20e6`.
`audit_stream_depth_r360.py r390` validates source identity before accepting
original binding rows. Replay r391 uses those exact source boundaries and
the same unix image (`71e8658643c21516790bd97edfaab03f1a56b9126e66bdca4f140872a8164c17`).
Its source/final readbacks are byte-exact, provider exit0 and resettrue.
The public experiment still reports `accepted=false` because this is a
diagnostic-observer run, not a measurement-qualified policy.

The formerly prepared `audit_shadow_join_r381.py` now executes on r390/r391:

- Each side has 7448 actual direct-buffer/cache matches across 1862 draws.
- Six post-pass shadow readbacks join actual Metal object tokens to canonical
  Texture/generation1/id4294967542, source/pass/draw and no-intervening-writer
  evidence. Their shader is `cf79d05af22c79e8`, fragment texture slot0.
- All six repeat **one** 2048x2048 R32F content difference: 934 float texels /
  945 bytes, maximum absolute delta `3.5762786865234375e-6` (60 ULP). They
  are not six independent failures. The difference's pixel-coordinate bounding
  box is `(633,533)`–`(2011,1692)`.
- Last preceding shadow draw writer is global draw554. The named sampling
  draws are 568–627 (16 draws across six passes); no-writer checks extend from
  that writer through each sampled pass's readback.
- Original shadow digest is
  `8bb185d0d1b5eb4cf7828f64841265b301a4a1012e1900c4dd526d4fbc258d16`;
  replay is `95b9a304d5b6b7b30fe47b2de63f70a18e4403463213512fe8c2586602ff9c40`.

`audit_capture_resume_r392.py` rechecks all five staged artifacts, the normal
run disposition, exact final raw bytes and every joined shadow byte digest.
It retains the per-texel coordinates/float deltas in
`shadow-float-differences-r392.json` and authenticates the group in
`capture-resume-r392.json` (`qualified=false`). This proves an intermediate
resource-content difference even in a final-pixel-positive frame. It does
not recover sampled texel coordinates or depth-comparison branch outcomes,
and does not by itself explain r346 or the other retained failures.

The r393–r396 follow-up below localizes the producer inputs. Do not widen
tolerance or substitute r390 for an older failure. No evidence was deleted or
compressed; approximately 7.5 GiB remained free after this pair.

### Stable shadow difference and selected-source exclusion: r393–r396

Repeated cold identity/capture-source replay r395 preserves r391's exact
16,777,216 shadow bytes in each of six post-pass readbacks. Both replays have
exact original final RGBA, provider exit0 and resettrue, while the diagnostic
experiment remains `accepted=false`. This is two-run replay stability, not
universal determinism or evidence that older failed originals are fixed.

The canonical writer interval is global draws160–554, 395 contiguous indexed
draws. The preceding recorded Clear targets both the 2048x2048 R32F color and
depth attachments: flags3, white, depth1, zero explicit rectangles. The
authenticated observations establish:

| Producer input | r390 original versus r391 replay |
|---|---|
| Actual selected PSO creation/source/function inputs | Equal on all 395 writers; the frame's ten source differences occur elsewhere |
| Issued viewport/scissor/raster/depth state | Equal on all 395 writers |
| Issued index/stream0 CPU-shared ranges | Readable, equal counts/ranges/hashes on all 395 writers; not GPU-time contents |
| VS/PS constant backing and VS volatile values | Equal on all 395 writers |
| FFP-VS backing | Differences confined to material/light offsets206–1174 |
| Fragment volatile values | Equal bytes; draw408 has different `skipped_push` bookkeeping |
| Extra-stream rows | Absent on both sides; this row covers extra streams only and adds no independent stream0 proof |

`audit_shadow_repro_shader_use_r396.py` authenticates the original PSO receipts
and identical source bytes in both replay directories. It walks the six
selected VS and two FS entrypoints through their reachable `dxmt9_*` helpers,
rejecting unknown helpers and whole-FFP-reference escapes rather than guessing
liveness. Its checker has field-use positive and unknown/escape negative
controls. All six VS use only `halfPixelFixup` (offsets2560–2567) and
`pointSize`/minimum/maximum (2608–2619) from FFP-VS. The fragment entries do not
use FFP-VS. These ranges do not intersect the differing bytes. Uncalled common
prelude helpers are not counted as shader consumers.

The authenticated receipts are `r390-shadow-producer-r391-r394.json`,
`r390-shadow-join-r395-r381.json` and `shadow-repro-shader-use-r396.json` under
the artifact root. The source walk is case-specific, not a general MSL proof.
Actual GPU-time buffer/alpha-texture contents, per-pass attachment history and
driver arithmetic remain separate hypotheses. No evidence supports changing
production shader math or pixel tolerances from this source exclusion alone.
The next bounded observation was original/replay R32F hashes at **existing**
writer pass ends; r397–r400 below executes it without forced draw splits.

### First differing shadow writer pass: r397–r400

The bounded r397 capture changes only the color-observer dimensions to
2048x2048, retaining hash-only pass-end observation and at most eight selected
shadow input copies. It does not force a Store or split a pass. GT2 exits0,
without timeout/failures, using Sikarugir and the same repaired unix image.
It publishes `frame-1649348364888400-1`: 175 sources22240–22414, 1882 draws,
source-plan digest
`b7f5c456ff93c86af5e97bc66caa5b9283b6b7df502ea93de2526a699641fb80`.
The r398 replay has exact final/source RGBA
`accdb1291bfcc5aec7e583e1b3101513e87abe287131cf863b7e55695ab14213`,
exit0/resettrue and zero GPU errors. Public diagnostic acceptance remains false.

The source/store-qualified comparison has 74 paired R32F endpoints: 45 hash
equal and 29 different. The first eleven endpoints on the canonical shadow
attachment are equal. Its next endpoint is the first difference:

| Cut | Original source / replay source | Global draw interval | Stored SHA-256 |
|---|---|---|---|
| Last equal | 22266 / 28, encoder0 | draw259 | `ee9503b1106a909ccdf311d7469b2ff925000fff1daff901d7b956f85e30e24f` |
| First different | 22267 / 29, encoder0 | draws260–270 | original `6e25d7f5ff4590e5f5f54b57ea02b746d7f1f03336c2a35960269821bd0244f5`; replay `11739e69b595e18c0a56a6ec57dad6b329a2dc94b221b3d806a3a64b6812df46` |

`audit_shadow_first_pass_r399.py` validates the Tape and folds canonical
attachment state, joining these endpoint handles to the original draw rows.
The bracket contains eleven indexed draws and ten delta records, with no
intervening Clear. It retains both alpha-input Texture definitions (generation1,
ids4294967466/4294967510): DXT3 1024x1024/11 mips and 512x128/10 mips, with
complete recorded CPU-visible seed extents. These descriptors/seed receipts
do not establish actual GPU-time alpha texture contents.

The eleven draws have equal actual selected PSO/raster/depth, issued index/
stream0 range hashes, and VS/FS volatile values. FFP backing still differs.
The report also retains `encoder_draw_index` differences: that value is derived
from optional `ActiveEncoderBreakdown::stats.drawCalls`, enabled in the live
recipe but not replay, and is not used as physical-pass identity. Commands,
command-local draw indices and source/pass endpoints remain separately joined.
Five sampled-shadow readbacks repeat one final shadow difference of 357 float
texels/358 bytes; no corresponding final-RGBA difference occurs in this frame.

The first audit attempt retained a parser-projection `KeyError` in
`first-shadow-pass-r399.log`; reading primitive counts from the full joined
draw rows corrects the diagnostic, and `first-shadow-pass-r399-final.log` /
`first-shadow-pass-r399.json` are the successful receipts. Native public GPU
policy Python, synthetic GPU policy CLI and compilation-lineage tests pass
3/3 (`test-gpu-shadow-r400.log`, unique Meson logbase `gpu-shadow-r400`).
No new production implementation, tolerance, or default-policy change is made.

Next: recover the first differing draw or GPU-consumption distinction inside
this eleven-draw bracket, accounting for depth history and the two alpha-input
textures. Existing per-pass original **raw bytes** were deliberately not saved;
hashes cannot reconstruct its changed pixels or attribute a single draw.
Avoid another whole-frame parameter sweep. The old nine failing originals
remain required; a final-positive frame is an intermediate discriminator only.
All old artifacts remain uncompressed/undeleted; about 6.7 GiB remains free.

### Paired pass-end depth: r401–r412

`DXMT9_DUMP_COLOR_ATTACHMENT_DEPTH_PAIR` is a default-off, bounded companion
to the existing selected hash-only color pass. It reuses the selector, source
witness and budget, without forced Store, draw cuts, source publication or
numeric conversion. It only copies stored, single-sample, same-extent 2D
float depth. The implementation checks the actual native texture descriptor.
Native Depth32Float is format **252**; GT2's format **260** is
Depth32Float_Stencil8. Only logical D24X8 may use depth-plane extraction from
that combined format. Logical stencil, unresolved/discarded Store,
MSAA and unsupported shapes/formats produce no depth witness.

The first fixture runs exposed two diagnostic assumptions, retained in r401/
r402 test logs: D24X8 is not a plain-depth texture on this device, and 260 is
not native Depth32Float. The corrected fixture uses D32F_LOCKABLE for the
plain path and D24X8 for the depth-plane path, with independently expected
float32 bytes and unchanged exact color pixels. Native tests pass 5/5 and
x86 tests 2/2 in `test-depth-pair{,-x86}-r404.log`. Both source identity modes
cover disabled, absent-depth, logical-stencil, disjoint selection, malformed
configuration, count limit and forbidden after-draw observation. Pure native
truth tables cover unresolved/discarded/resolve-only/MSAA/overflow rejection.
All four staging builds succeed, with no PE bridge ABI change.
Full TLA, compilation lineage and install-name checks pass 3/3 in
`test-depth-final-r410.log`; this diagnostic reuses the existing completion
mechanism and does not introduce a new ordering/lifetime policy model.

The r405 original exits normally with no timeout/failures under Sikarugir,
publishing `frame-1651063776832900-1`: 185 sources22151–22335 and 1866 draws.
Its source-plan digest is
`cc6b8312ea6abfc0a571741faca56aec729d8dac57e5585a3589bce29aac34f6`.
Original and replay each retain exactly 32 paired endpoints (512 MiB of depth
bytes each), using the same unix image
`37bf3a35b32269ad2276dbbd938b98e9530d64e14f03a58e3a307e30ea6d499f`.
`audit_paired_depth_r407.py` revalidates source authority, Store, exact color
metadata linkage, canonical depth generation, issued handle, raw extents and
SHA-256. Nine pairs have equal color/depth; the remaining 23 differ in both.

| Endpoint | Original / replay source | Draws | Depth result |
|---|---|---|---|
| Last equal | 22175 / 26, encoder0 | 234–244 | `d4e5c4802ada29ef7179061d42a795613c1f871cc68632b4101a1e61e3d01a4e` |
| First different | 22176 / 27, encoder0 | 245–255 | 121 texels; maximum 2 ULP / 1.1920928955078125e-7 |

First-bad original depth is
`3b52b9739d67691188edce16a3c57317cdfa37d7a045e6cfcdb5f217e1e4fa79`,
replay depth is
`0fe6c8d8aac5f6af8e2b000444c0df08d62b31a17d29f162f692dc77b2940764`.
The raw difference coordinates and bit patterns survive in
`r405-paired-depth-r406-r407.json`. All 1866 issued depth/stencil descriptors
match. `depth-bracket-inputs-r408.json` checks the eleven first-bad draws:
actual selected PSO inputs, raster/depth descriptors, issued geometry and
VS/FS volatile values match. Observed FFP-VS backing still differs; prior
source-use exclusions are not silently treated as GPU-read proof here.

The original final RGBA digest is
`d5a01daf5f2fcf66c90e08bf6e9064a5902eda58aa717d321ce4d11ee6701284`;
r406 produces
`2f9d46178b2821c48733011ff4009ef387d60bf9ecc46f8acf6d32742634cded`:
one RGB pixel, maximum/total channel delta1, alpha exact. Provider exit0,
resettrue and its historical pixel envelope do **not** make this exact.
The strict public gate stays unaccepted and diagnostic.

Cold r409 selects only the first-bad pass, removing all nine earlier paired
readbacks. `depth-observer-intervention-r410.json` authenticates identical
first-bad depth, color hash and final RGBA. This is an observation-count
intervention, not unchanged-environment A/A. It rejects earlier replay copies
alone as the cause, not all observer perturbation or driver behavior.

R411 removes all inline color/depth/texture/shader/effect observers and Metal
validation. The normal source-aware experiment still returns the identical
wrong source/output bytes, with `diagnostic=false`, `accepted=false`, exit0,
resettrue and zero GPU errors. `unobserved-original-gate-r412.json` reauthenticates
the original, plan, loaded images and actual readbacks, and calls the shared
public GPU predicate (`gpu-artifact-rejected`). The retained one-pixel error
is not manufactured solely by replay-side observation or accepted through the
provider's legacy pixel envelope. Original-side observation remains a separate
perturbation boundary; no claim about every unobserved live frame follows.

Next: localize the eleven-draw bracket using its concrete depth coordinates,
GPU-visible position/uniform/alpha inputs, and raster interpolation/depth bias.
The paired endpoints do not yet identify the first divergent draw, prove
sampled texel values, or link the intermediate difference causally to the final
one-pixel failure. No tolerance, rendering default or performance claim changes.
All original artifacts remain uncompressed/undeleted; about 4.6 GiB remains
free after these bounded runs.

### Draw-prefix and three-triangle discriminator: r414–r429

The existing `DXMT9_DRAW_ORDINAL_MAX` filter narrows the first-bad r405 pass
without rebuilding the provider. `audit_depth_prefix_r415.py` reauthenticates
the unchanged source plan, all185 source settlements, the exact issued prefix,
loaded images and stored depth bytes. At the 121 differing coordinates, the
preceding original endpoint is exactly depth1 on every coordinate; neither
the original nor full replay first-bad endpoint retains that prior value.

| Diagnostic | Inclusive replay ordinal limit | Selected pass draws | Coordinates equal to full replay / still depth1 |
|---|---|---|---|
| r414 | 251 | First6 | 0 / 121 |
| r416 | 254 | First9 | 121 / 0 |
| r417 | 253 | First8 | 121 / 0 |
| r418 | 252 | First7 | 121 / 0 |

The localized replay writer is global draw251 (zero-based), replay ordinal252,
original ordinal787217, source22176/replay27. It contains 1248 indexed triangles.
Each prefix deliberately changes the final frame: provider exit1,
`reset_ok=false`, `oracle_failure=source-mismatch`, with complete source
settlement and zero GPU errors. They are not successful replay iterations or
proof of unchanged physical-pass execution. This identifies a replay writer,
not the first differing draw on the original GPU timeline.

`audit_depth_writer_geometry_r419.py` independently folds the STREAM and
INDEX_BUFFER sections to event267/record53 and restores exact canonical
generation1 buffer contents. VB `(Buffer,1,4294967485)` has 59,904 bytes; IB
`(Buffer,1,4294967487)` has 7,488 bytes. Their reconstructed FNV hashes match
the actual issued stream0/index CPU-range witnesses. Bound VS, FFP and volatile
bytes and selected MSL are retained. Float64 CPU projection is only a geometric
locator: it does not emulate GPU clipping, alpha tests, raster edge precision
or depth bias. It must not become a numerical oracle.

`export_depth_writer_r420.py` packages 22 authenticated files (1,501,552 bytes),
including all11 generation-qualified DXT3 alpha mips. The standalone
`depth_writer_r421.mm` helper uses those unchanged MSL sources, original input
bytes and one indexed draw, clearing independent R32F/depth attachments. Its
first validation attempt r421 correctly rejects a 2632-byte allocation for an
MSL FFP-VS structure with 2640-byte aligned extent. The corrected r422 helper
allocates 16-byte-aligned buffers, preserves the captured bytes and zero-fills
only the unobserved tail. This fixture repair is not evidence of a production
out-of-bounds access: production observations covered a semantic slice in a
larger backing allocation. Its build log, failed run log and executable remain.

| Standalone diagnostic | Indexed triangles | Original-difference coordinates equal to full replay |
|---|---|---|
| r422 | All1248 | 121 / 121 |
| r423 | 177,1194,1195 | 121 / 121 |
| r424 | 177 | 68; other53 untouched |
| r425 | 1194 | 51; other70 untouched |
| r426 | 1195 | 2; other119 untouched |

Triangle numbers are zero-based within this draw. Each of the 121 coordinates
belongs to exactly one of the three individual diagnostic outputs. None equals
the original depth bits. The same three-triangle cold repetition r427 preserves
all depth/color bytes; r428 removes Metal API validation and preserves those
bytes too. All successful helpers complete one Metal draw with zero GPU errors.
`audit_single_writer_r429.py` authenticates the input, executable, receipts,
readbacks and per-coordinate writer partition in
`single-writer-discriminator-r429.json`. The source/null-handle and blob-path
mistakes in the first r419 audit attempts remain in their failed logs; the
corrected final audit succeeds.

The 121-coordinate replay difference can therefore be studied with a small
standalone draw instead of another whole-frame parameter sweep. No full-image
or full-attachment equality is implied: the helper has a different execution
context and starts with cleared attachments. Source equality and CPU hashes
still do not establish original GPU-read contents or driver vertex outputs.
Next: compare the localized draw's GPU-consumed buffers/vertex outputs in the
original execution context with this reduced discriminator, then test any
justified fix against every retained original. No production binary, tolerance,
default policy or FPS claim changed in r414–r429. All old evidence is preserved
without compression/deletion; approximately4 GiB remains free.

The native public GPU-policy Python/CLI and pure dump-policy tests pass 3/3
in `test-gpu-writer-r430.log` (Meson logbase `gpu-writer-r430`). The performance
source audit and `git diff --check` pass. These checks do not promote the
standalone discriminator into a production-path or original-pixel oracle.

### Calibrated GPU vertex outputs and caller fenv: r431–r441

The standalone helper now has an explicit `--vertex-output` diagnostic mode.
It transforms only the authenticated VS entry to add bounded atomic min/max
observations of the final clip-space `out.position` bits, with a per-vertex
invocation count. Min/max avoids an ordinary-store race for duplicated indices
and detects conflicting outputs; it is not a deterministic scheduler claim.
The fixed capacity is1872 vertices, each nine uint32 words. The altered MSL is
retained separately; it is never represented as an unchanged original shader.

R431 uses both Metal API and GPU validation. Its full 2048x2048 depth and R32F
color bytes remain identical to the uninstrumented three-triangle r423 output.
It records exactly seven vertices410/705/809/887/888/1682/1847 and nine
invocations. Vertices410/705 each execute twice with identical min/max bits;
the other five execute once. This calibrates the output observation for this
small input only, not arbitrary shaders or the unobserved original execution.

The caller-FPU discriminator sets and verifies `fesetround` before creating
the Metal device/libraries, then records both `fegetround` and x86 MXCSR.

| Corrected diagnostic | Caller rounding | `fegetround` | Position / depth / color bytes |
|---|---|---|---|
| r438 | nearest | 0 | Identical to r431 |
| r439 | down | 1024 | Identical to r431 |
| r440 | up | 2048 | Identical to r431 |
| r441 | toward zero | 3072 | Identical to r431 |

These four runs retain Metal API validation but not GPU validation. Their
instrumented sources and the entire atomic-output buffers also match r431.
Only the calling thread's rounding control is tested; compiler-worker FPU
state, original Wine thread state and original GPU inputs are not observed.
No production fenv reset or shader-arithmetic change is justified by this cut.

The first helper r433 compiled with an extra-token preprocessor warning that
omitted the `schema`/`qualified` metadata keys. The auditor rejected its build
log before accepting the r433–r436 results. Those four incomplete receipts and
all bytes survive and are explicitly excluded. Corrected r438 compiles without
warnings; all four experiments were rerun rather than retroactively editing
old metadata. `audit_vertex_outputs_r437.py` reauthenticates manifests, shader
and executable bytes, GPU completion, validation logs, vertex counts/min/max,
rounding state and full images. Its successful receipt is
`standalone-vertex-positions-r437.json`, with a separately retained first-failed
audit log and final successful log.

At this point, the standalone helper had not implemented a production-side
collector. The following cut adds it without turning the standalone success
into original fidelity or relaxing any original pixel tolerance.

### Actual original-context GPU vertex positions: r443–r460

The cold, default-off collector now clones the actual selected PSO and inserts
bounded atomic position min/max/count stores at the generated VS's terminal
return. Slot 29 must be unused. Selection requires exact source SHA-256,
triangle-list count, one instance, zero bases and bounded Shared IB indices.
It issues the draw once, restores the original PSO immediately, and pins
readback to that actual CB's completion. Original receipts carry a frozen
capture source/ordinal/storage witness; replay-local sequences require the
separate authenticated source-plan mapping. This is explicitly shader/PSO
perturbation, not passive historical GPU-read proof. The public policy gate
rejects every nonempty probe directory, including `0`, independently of master
flags and forged non-diagnostic receipts.

Native r451 passes four policy/Metal tests; x86 r454 passes those four plus
the install-name audit. Real WMT-created PSOs exercise clone registration,
independent exact pixels and positions, duplicate indices, one issue on every
preflight rejection, restore/budget, disabled/malformed settings and absence of
files before GPU completion. r450 passes public GPU Python/CLI, full TLA and
install-name checks. All canonical staging builds pass. Initial r444 test
compile errors and r446 fixture validation failures (`maxTessellationFactor=0`)
are preserved; the corrected fixture sets the legal value 1. Explicit failed
GPU-completion fault injection remains untested, not silently claimed closed.

`actual-vertex-probe-calibration-r457.json` authenticates two replays of the
unchanged r405 bundle. Both observe all 1872 vertices/3744 invocations with
zero min/max conflicts; the seven reduced-fixture positions are exact. The
entire depth image remains `0fe6c8d8...`, including all 121 original/replay
differences. With validation off, r455 also preserves the old final
`2f9d4617...` output and its one-pixel failure. Validation-on r453 gives the
same depth/positions but a two-pixel final residual; preserve this separately
as validation perturbation, not baseline-equivalent evidence.

Original r456 exits normally under Sikarugir with a 172-source/1897-draw
bundle and ten source-qualified GPU position receipts. The source plan is
`1060054fffbd29f0afe7f2d5e2805e332a17998717983bffc821a0947715769e`.
The r458 replay and r459 selected-draw probe both reproduce its final
`c69a10eb...` image exactly, but the 32 paired shadow endpoints reveal an
intermediate difference: the first nine are exact; source22174/replay27 is the
first differing color/depth pass, with 807 depth texels differing by at most
2 ULP. A final image match therefore does not erase the intermediate residual.

`original-vertex-position-comparison-r460.json` joins original
source22174/ordinal787286 to replay27/ordinal260 (global draw259):

| Actual GPU position observation | Result |
|---|---|
| Vertex coverage / invocations | 1872 / 3744 on both sides; identical counts |
| Conflicting duplicate values | 0 on both sides |
| Original/instrumented VS and PSO receipts | Identical across processes |
| Vertices with different clip-position bits | **139** |
| Different components, X / Y / Z / W | 101 / 87 / 18 / 5 |
| Maximum raw bit distance in a component | 256; not the depth ULP bound |
| Replay probe versus replay without probe | Entire selected depth and final images identical |

Issued CPU VB/IB range hashes, VS/PS/FFPPS backing and VS volatile fields
match. FFPVS backing still differs in material/light data; its relevant
half-pixel/viewport tail matches on the CPU. None of these CPU observations
proves the bytes read by every original GPU invocation. The new distinguishing
evidence is that the discrepancy already exists in actual VS clip output;
it is not only a depth-readback/raster endpoint discrepancy. Input lifetime,
GPU-read bytes and compilation/arithmetic remain candidates, not conclusions.

Next at r460: capture the localized shader's GPU-consumed input bits and intermediate
skinning/projection values, with its own perturbation calibration, before
changing resource lifetime or arithmetic. All ten historical failing originals
remain mandatory; r456's final success cannot replace any of them. No policy
default, original tolerance, production rendering fix, compression or evidence
deletion occurred in this cut.

### Orthogonal MAD arithmetic controls: r465–r480

The optional detailed observer records actual input/intermediate values in
23 four-word groups, with min/max/count layout 185 words per vertex. The
synthetic Metal fixture independently verifies every field. On r465 the
observer changes 139 of the 1872 positions relative to r459; every changed
position now equals r456's original, with zero duplicate conflicts and 3744
invocations. The selected original depth is also exact; 807 texels differ from
the replay baseline by at most 2 ULP. Final RGBA remains unchanged and exact.
`audit_vertex_input_perturbation_r466.py` authenticates these facts. The observer
therefore is an intervention, not passive GPU input-equivalence evidence.

`vertex-controls-r468-input` contains 1,662,504 bytes of retained inputs and
orthogonal source controls. The static mesh's decoded position/UV/weights are
checked against r465 GPU input fields. r405 fragment texture fixtures are reused
only for standalone rendering, not presented as r456 attachment or input proof.
Both standalone baselines calibrate: unchanged position-only source gives all
139 replay differences; detailed source gives the original positions exactly.

`audit_vertex_arithmetic_controls_r480.py` authenticates 34 completed GPU runs,
source/input hashes, duplicate-safe outputs and the following discriminator:

| Control | Vertices different from r456 original | From r459 replay |
|---|---:|---:|
| Position-only baseline | 139 | 0 |
| Comment/whitespace, scope-only, projection-only no-contract | 139 | 0 |
| Bone-row locals, skinning-only no-contract, MAD-only no-contract | 0 | 139 |
| Explicit FMA for all three MADs, including under global contract-off | 139 | 0 |
| Contract-off with explicit row0 FMA only | 54 | 107 |
| Contract-off with explicit row1 FMA only | 109 | 52 |
| Contract-off with explicit row2 FMA only | 0 | 139 |

All three observed blended rows match separately rounded float32 multiply/add.
The float64-evaluated/final-float32 fused candidate differs in 419/195/0
vertices by row. The actual GPU explicit-FMA controls, not that CPU numerical
approximation alone, supply the reversible discriminator. Input lifetime is
not required to reproduce this standalone arithmetic difference; exclusive
attribution of the original process is still not proved.

The same detailed probe on r405's selected draw (r478) restores its entire
original depth `3b52b973...`, but final `2f9d4617...` still differs from original
`d5a01daf...` at one RGB pixel. An intermediate repair is not final closure.

### Digest-selected whole-shader intervention: r479–r488

`DXMT9_PROBE_NO_CONTRACT_SOURCE_SHA256` selects one to eight distinct original
generated vertex source digests and prepends `#pragma clang fp contract(off)` before normal library
creation. It does not insert output buffers or change draw order. The source
guard, actual compiled-source/PSO lineage and original-to-transformed digest
receipt are tested through real WMT library creation. Any nonempty request,
including malformed `0` and unmatched digests, independently disqualifies the
public GPU result. This diagnostic is not a proposed production math default.

r483 transforms `722a7de2...` into `0f912632...` for all uses of that source.
The final one-pixel error remains byte-identical. r485 mistakenly selected
exact sequence zero and has no color observations; its unchanged final result
is retained but cannot serve as shadow coverage. The corrected r486 unsets
that selector and collects 32 existing shadow-endpoint hashes without large
raw color/depth copies.

`audit_no_contract_shadow_r488.py` authenticates the result: the exact original
prefix grows from nine to twelve endpoints. The next differing endpoint is
original source22179/replay30, global draws268–278 (ordinals787234–787244).
These use three other VS source digests, not the selected `722a7de2...`:

- `b2155079...`: position projection;
- `b1b32f95...`: textured position projection;
- `22bafcc3...`: a different, longer skinning program with multiple MADs.

The pass is localized; which draw first diverges and whether its cause is the
same arithmetic issue remain open. The final mismatch alone does not show
which shadow discrepancy contributes to it. No new live capture, original
replacement, tolerance change, compression or evidence deletion was used.

Native r481c is 7/7; x86 r482b is 4/4 including actual-Metal no-contract
creation and install-name audit. An initial new test expected the pre-transform
source digest and failed on both architectures; it was corrected to verify
the actual compiled digest, not bypassed. Public GPU CLI/lineage and full TLC
r484 are 3/3 (TLC 167.94 seconds). These guard diagnostic exclusion and existing
lifecycle contracts; they do not prove production shader rounding or restore
any of the ten final-pixel failures.

### Four-source shadow repair with unchanged final failure: r490–r494

The next single-source control, `22bafcc3...` (r490), keeps the same final
one-pixel failure. r492 selects that source, `722a7de2...` and the two projection
sources together. All four source-to-transformed digest receipts are present;
the actual loaded binaries are reauthenticated in
`audit_shadow_family_control_r494.py`. **All 32 paired 2048x2048 R32F shadow
endpoint hashes now equal the retained original.** The unchanged final hash is
still `2f9d4617...`, not original `d5a01daf...`; the differing pixel index is
recorded in `shadow-family-no-contract-r494.json`.

This is an important separation, not full repair. The observed shadow color
endpoints are recoverable by bounded arithmetic interventions, while another
scene/attachment/shader path still produces the final error. No claim is made
about unobserved depth endpoints, every original GPU input, or causality of the
final pixel. The retained frame uses 55 actual VS source digests; the four
selected shadow programs are not an all-VS arithmetic trial.

The selection is bounded at eight unique digests: invalid delimiters, duplicate
digests and over-bound requests reject before transformation; any nonempty
request remains disqualified. Native and x86 r493 each pass 3/3, including the
actual second-token selection, source guard and compilation path. r485's
missing observations remain explicitly excluded. Existing raw evidence and
all ten historical failures are retained; no new original capture was needed.
The final r495 native/public sweep passes 9/9, including all probe modes,
strict GPU Python/CLI rejection and compilation lineage. `git diff --check`
and the changed performance-document source audit pass. No commit is made in
this investigation cut.

### Retained scene controls and target-draw localization: r496–r522

No new original capture, trace, compression or deletion is needed for this cut.
Existing APFS copy-on-write input staging preserves separate replay ownership.
The native provider and unix image are unchanged from r491; each trial checks
the current loaded-image digests, immutable source plan, completed source stream,
actual selected-source transformation and final readback. GPU errors remain zero.
All receipts are diagnostic and publicly unaccepted, including provider-exit0
runs. These trials change no runtime code, source-selection bound or default.

**r346 negative controls.** `audit_scene_controls_r515.py` authenticates the
same-build controls against the retained original and all 84 scene endpoints:

| Run | Exact-source intervention | Final different pixels | Changed scene endpoints vs old replay |
|---|---|---:|---:|
| r498 | None; current-build baseline | 69 | 0 / 84 |
| r497 | Four shadow VS from r492 | 69 | 0 / 84 |
| r496 | First-bracket scene VS `6acb3a5c...`, `c7bc2e09...` | 1751 | 64 / 84 |

The last equal scene endpoint22351 and first unequal endpoint22352 do not move.
The baseline/shadow-control final SHA remains
`1ad9883a32b47242d399d8434f3246e5fd0c9aa9be99ef068282feac36a12f33`;
the scene intervention gives
`9656b01a85e9d6da70a7c83f664436ee8ee698efedb3b4b626e06a56115bf57e`.
All three have `reset_ok=false` and provider exit1. They are retained failure
discriminators, not successful cold iterations. This does not prove the r346
shadow contents were all repaired, or exclude a finer scene arithmetic control.

**r405 bounded source scan.** r499 reproduces the old one-pixel failure.
Seven groups test the 51 actual original VS sources outside the four prior
shadow controls, without increasing the eight-digest selector bound. Actual
compilation receipts confirm 49 transforms; two final source digests do not
match and are not tested. Only r503's eight-source group makes target pixel
(855,386) exact. Eight singleton trials then isolate `bad49ade...` in r508;
the other seven retain byte-identical baseline output.

| Evidence | Result |
|---|---|
| Exact selected VS | `bad49ade23c9ef634d7e6a3a13040c52620a0c35db243444ffd0b12eb9514d48` |
| Original target bytes / baseline bytes | `[79,63,44,255]` / `[79,63,45,255]` |
| Selected intervention | Target exact; 790 other differing pixels; reset fails |
| Baseline final SHA | `2f9d46178b2821c48733011ff4009ef387d60bf9ecc46f8acf6d32742634cded` |
| Intervention final SHA | `50fdc730765762ff691a7f7eb88d27f894a715bc8ae101647eddf8519695e7ee` |
| Exact selected FS at localized draw | `e52bcd907024148c0d76f8720f135c8ebdd905e26d2dee3c92e99484b5594c94` |

**Calibrated target history.** One-pixel pass-end ROI observations r516/r517
retain the full baseline/intervention final bytes and supply 89 paired rows.
The first observed intervention difference is source22283/replay134, encoder0:
the logged triplet changes `[15,33,28]` to `[15,32,28]`. Source22282 is still
equal. The original recorded VS is used by 59 draws across eight sources, but
this endpoint narrows the target to eleven draws in source22283.

r518/r519 requested after-draw observation without a required draw/command
split selector and emitted no CSV; they are explicitly excluded as per-draw
coverage. r520/r521 add exact source134 and command range0–32. Both preserve
their complete preceding final image and emit all eleven command-subdraw rows.
The difference is already present after subdraw0 and persists through subdraw10.
The joined candidate is **global draw1380 (zero-based), original ordinal788346,
command0/subdraw0: 4608 indices and a 1480-vertex span**.

`audit_target_localization_r522.py` reauthenticates the scan, singleton trials,
source mapping, original draw/PSO witnesses, complete outputs and both ROI
calibrations in `target-localization-r522.json`. The CSV's reported triplets
are format-specific diagnostics, not normalized final-file channel semantics;
handles are not cross-process identity. Per-draw observation splits passes.
Most importantly, this is the first **intervention** difference at the target,
not independent original intermediate evidence. Previously divergent sampled
textures/depth and a changed VS arithmetic result remain separate hypotheses.
The next bounded fixture can target this draw's VS outputs and sampled inputs;
a blanket scene no-contract policy is not supported. All ten old failures stay
open. `scene-arithmetic-controls-r515.json` retains the independent r346
negative controls.

Both independent artifact audits pass, as do the public GPU-policy Python
rejection tests (`gpu-policy-native-python-r523.log`), launcher shell syntax,
changed performance-document source audit and `git diff --check`. No new build
or TLA run is claimed: this cut uses existing, previously tested diagnostics
without changing runtime ownership, scheduling or shader translation defaults.

### Replay-calibrated scene draw and reversible MAD discriminator: r524–r553

The bounded r524–r527 and r533/r534 observations preserve the complete prior
baseline/intervention final images, their source-plan digest and loaded binaries.
They export the selected draw's pre-color/depth, post-color, actual uniforms,
raster/depth/sampling descriptors and three shader-consumed textures. The
textures include two cube maps; all 76 subresources match between the two
intervention contexts. The same-source no-writer interval permits their
end-of-pass readbacks to seed the first draw. These are replay inputs, not
original GPU-consumption evidence.

`export_scene_draw_r528.py` restores the original generation-qualified index
and vertex ranges, checks their observed hashes, and writes an authenticated
fixture. The referenced VB range starts at byte1966080, contains1480 vertices
at stride64, and preserves the actual binding offset. The fixture retains
replay FFP-VS backing, not an invented original material/light snapshot.
Unreferenced buffer prefixes/tails are zero; this reduction does not prove
original whole-allocation contents.

The initial standalone helper is calibrated against **all** pixels, not only
the target. Its winding is Clockwise, as required by the actual raster receipt.
r530 reproduces production r526 exactly. r531 uses baseline seeds with the
no-contract VS and differs from production r527 at46 pixels. Intervention-matched
color seeds explain all46: they were already different before this draw, and
the baseline draw modifies none of those coordinates. Depth, all sampled
subresources and observed bound uniform/volatile bytes are unchanged. With
matched seeds, r536 reproduces production r527 exactly. The local14-pixel
response includes target(855,386) and is disjoint from the inherited46.

| Scope | Full after-draw SHA-256 |
|---|---|
| Production baseline / standalone baseline | `25dd17a7c087fe31355f85f277550d4ef162d01a5660675172610570b9a21df5` |
| Production intervention / matched-seed standalone | `f86b6e337f4c1342ed861533edc183f8724de8af051773c32b627ac5b664d2bc` |
| Single-draw intervention with unchanged baseline seeds | `880708401a225be0bdc51a07bd7de34de06e2b2ba39c2002a9aed2e28cb082da` |

An API-validation control r540 fails before a completed GPU result: the
standalone PS buffer contains the80 logical payload bytes while the declared
argument occupies3904 bytes. Its failure log is retained. The revised helper
`scene_draw_r541.mm` allocates the declaration-sized buffer with a zero tail;
the selected FS only references constant registers0–2. This is a fixture
allocation correction, not evidence of a production buffer overread. All six
r543–r548 baseline/matched-intervention controls, with validation off, API-only,
and API+shader validation, preserve their full expected images. API/GPU enable
receipts are checked, not inferred from requested environment variables.

The27 r550/r552 controls keep the same immutable baseline inputs:

| Arithmetic control | Different pixels vs baseline | Target BGRA |
|---|---:|---|
| Scope-only, projection/light/half-pixel contract-off, explicit last-MAD FMA | 0 | `[28,33,15,194]` |
| Global, skinning-only or all-MAD contract-off | 14 | `[28,32,15,194]` |
| MAD2-only or MAD3-only contract-off | 3 | `[28,33,15,194]` |
| Last MAD9-only contract-off | 11 | `[28,32,15,194]` |
| Global contract-off + explicit MAD9 FMA | 3 | `[28,33,15,194]` |
| MAD2+9 or MAD3+9 contract-off | 14 | `[28,32,15,194]` |

MAD ordinals are zero-based among the ten actual MAD instructions. MAD9 is
`mad r1, c2, r1, r2`, the final skinning row accumulation. Its validated control
preserves all11 changed pixels. Projection-only and light-only scopes do not
exclude a downstream projection/sampling response to changed skinning values;
they only exclude those narrow standalone arithmetic interventions. The
explicit FMA reversal distinguishes arithmetic rounding from scope-only edits.
It does not show which original GPU input or compilation context selected the
different rounding, nor that all11 local changes should be promoted.

`audit_scene_draw_reduction_r553.py` reauthenticates captures, source plans,
loaded images, fixture bytes, full output hashes, enabled validation and27
controls in `scene-draw-reduction-r553.json`. Standalone subprocess wall is
typically0.16–0.36 seconds after cache warm-up; this is investigation latency,
not product FPS or pure GPU time. r549 initially rejected a wrong expected
MAD count before creating inputs or running the GPU; the exact count is ten.
No retained original was deleted, compressed or replaced; no new live capture
was required. All ten final-pixel failures and strict public rejection remain
open. This cut changes diagnostic artifacts and documentation only, not runtime
ownership, shader defaults or scheduling; no additional TLA proof is claimed.
The final native/public r554 sweep passes3/3: strict GPU Python contracts,
GPU policy CLI qualification/rejection and compilation lineage. Its retained
log is `gpu-policy-r554.txt`. The artifact audit, changed performance-document
source audit and `git diff --check` pass.

### Acceptance checklist

1. Maintain the passing bounded partial-Clear clipped-region, aspect, mip/cube
   and MSAA readbacks; broader D3D9 viewport and attachment semantics are not
   inferred from this scoped backend fixture.
2. Keep the passing actual A/A and nonidentical A/B GPU-qualified synthetic
   gate; extend visible draw/uniform coverage separately from its Clear oracle.
3. Close the remaining six-pixel r28 counterexample: its older live trace lacks
   physical pass IDs, so recover independent boundary evidence or capture a
   newly trace-complete counterexample before asserting equal cuts. Localize
   pass output/resource bytes if equal cuts leave a residual. Preserve the
   exact r36 reference and ordinary off-path negative. Also close the retained
   r146 two-pixel, r171 three-pixel, current-build r201 six-pixel, r225
   56-pixel, r254 three-pixel, r263 72-pixel, r300 two-pixel, r346 69-pixel and
   r405 one-pixel failures; do not replace them with
   passing new frames or use the disjoint r171 dumps as original intermediates.
4. Maintain the now-connected versioned public policy binding: boundary mode,
   source-plan digest, route and actual settlement receipts must survive
   CPU/GPU execution and final reauthentication. Treat merging/repartitioning as a
   policy change that must pass the original oracle, not as a silent baseline
   transformation. Do not merely relabel diagnostic receipts as qualified.
5. Require exact original source/output bytes and non-vacuous path coverage on
   both routes before using the coupled interface for policy cost ranking.

TLA/native lifecycle proofs remain valuable for ordering and lifetime, but do
not prove driver behavior, shader arithmetic, resource contents or final pixels.
