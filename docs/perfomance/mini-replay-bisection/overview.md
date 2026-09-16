---
domain: mini-replay-bisection
workload: 3DMark05 GT1
title: "Mini-Replay Bisection — the apparatus that isolated per-draw vertex-stage write amplification - Current Overview"
type: domain-overview
status: current
updated: 2026-09-16
source: docs/perfomance/mini-replay-bisection/log.md; docs/perfomance/overview-3dmark05-gt1.md
related: docs/perfomance/mini-replay-bisection/index.md; docs/perfomance/mini-replay-bisection/log.md
---

# Mini-Replay Bisection — the apparatus that isolated per-draw vertex-stage write amplification - Current Overview

> Current, compact view for this performance domain. Historical detail from the former
> top-level `mini-replay-bisection.md` overview is preserved in [log](log.md). Domain landing: [index](index.md).

## Scope

This domain owns the **methodology**: build a standalone, row-local mini-replay
harness that consumes real captured index/stream/cbuf/depth payloads plus dumped
shaders, reproduce the original hot-encoder `VS Buffer Device Memory Bytes Written`
pressure at encoder scale outside Wine/D3D9, then bisect it down to individual
shader-pair / draw windows. It answers: *which concrete inputs reproduce the GT1
hidden vertex-stage write bucket, and at what granularity is it owned?* This is
the apparatus that made [tvb-mechanism-proof](../tvb-mechanism-proof/index.md) possible and that backs the
[index-cache-locality](../index-cache-locality/index.md) win.

## Latest Conclusions

The default-off [known-setter dependency refinement](uniform-dependency-reuse.15.md)
reduces GT2 batch full-uniform builds from 422 to one: all 421 comparable
rebuilds had identical fixed values. Shared-predicate bounded refinement,
actual setter/cache tests and same-image CPU/GPU exact evidence pass. Batch
uniform-build scopes fall ~1.56→0.52ms; ordinary CPU medians fall 2.76–3.97%
in opposite orders with parent-only hash observation. No new cache or layout
authority is added. Broader FFP/clipping pixels, global observer-off,
asynchronous and wild/FPS qualification remain open; default promotion is
withheld. The remaining hash parent is ~0.73ms under this observation mode.

The [uniform-hash observer separation](uniform-dependencies-observer.14.md)
prices nested timing/publication at 0.467–0.481ms, or 27.7–28.3% of the
~1.7ms hash parent, without changing semantic work. Parent-only observation
leaves ~1.217ms; this is a residual envelope, not a proven removable cost.
Same-image forward/reverse mode runs preserve CPU semantics, exact original
GT2 pixels and replay/snapshot work. Constant generation plus usage and
nonconstant/clip-space dependencies remain required. Whole-iteration drift
prevents a precise overall-speedup claim; observer defaults are unchanged,
and global observer-off/asynchronous/wild evidence remains open.

The [snapshot-local dedup follow-up](snapshot-local-dedup.13.md) removes
priority/duplicate selection when all flat states fit, redundant sorting of
ordered tables and same-call WORLD0 matrix recomputation. Ordinary CPU medians
fall 1.85%/1.78% in opposite orders; diagnostic replay falls 15.828→14.839ms.
The largest scoped change is render-state flattening, ~1.20→0.125ms. Bounded
native value tests, unchanged cache/replay work counts and fresh exact GT2
CPU/GPU evidence pass. No generation, ownership or queue policy changes;
uniform hashing, observer-off costs and wild/FPS qualification remain open.

The [fixed-uniform projection follow-up](fixed-uniform-projection.12.md)
removes repeated extraction at Direct append without a new handle cache or
invalidation rule. Exhaustive bounded value tests, production-cache binding
and fresh GT2 exact CPU/GPU comparisons pass. Lookup elapsed scopes fall
~0.85→0.71–0.72ms, but ordinary whole-iteration medians change +1.01%/−0.33%
in opposite orders. No repeatable total-CPU or FPS gain is established.
The newer follow-up above reduces snapshot-local construction; retained
component reuse and asynchronous/wild evidence remain separate work.

The [unchanged-image replay owner investigation](replay-cost-owners.11.md)
locates the largest source-driver sample owners in state/uniform snapshots
(38.1%) and Direct Slot append (21.0%), ahead of resource closure (9.5%),
planning (4.0%) and Slot admission (3.6%). Existing scoped timers corroborate
snapshot and uniform-component lookup work; planning totals only ~0.90ms
across both harness and production calls. These are diagnostic attribution,
not additive CPU shares or removable milliseconds: 17.5% of sampled source
stacks include clocks. The follow-up above implements only cached value
projection reuse, not persistent owner/generation-qualified component reuse.
Wrapper attribution, observer-off costs and asynchronous/wild evidence remain
open; the attribution alone does not authorize policy or FPS promotion.

The [corrected phase attribution](cpu-phase-attribution.10.md) now separates
replay from fresh setup/admission and resource reconstruction. Four ordered
diagnostic blocks locate Direct exact's benefit in replay (18.869→16.075ms
CPU mean), with admission unchanged at 19.270ms. Ordinary brackets repeat a
3.05–3.08% whole-iteration route benefit. The earlier 1MiB increment is not
stable: -0.52%/+0.16% in ordinary brackets and only -0.033ms mean replay.
Diagnostic overhead and fresh-object tails remain separate from policy ranking.
The owner follow-up above narrows the ~16ms replay envelope without treating
harness setup or resource reconstruction as production replay service.

The [corrected GT2 matrix](gt2-qualified-policy-matrix.09.md) passes ten fresh
CPU/GPU-qualified comparisons and independent receipt reauthentication.
Direct exact costs 3.48–3.69% less whole-iteration CPU than Compatibility in
both orders; 1MiB/64MiB has a smaller 1.10–2.97% signal versus exact. A/A
brackets differ by -0.77%/+0.08%. 5MiB shows no useful gain and retains
61.84MiB with 56 GPU optional denials per two iterations, versus 20.86MiB and
zero denials/growth for 1MiB. These are scoped CPU/capacity observations, not
FPS or default promotion. The follow-up above confirms the route benefit but
does not repeat the 1MiB whole-iteration gain; asynchronous residency/pacing
and repeated wild comparison remain open.

The [attachment-authority correction](attachment-authority.08.md) now removes
the CPU-only software Clear mismatch while preserving commands, identities,
seed writes and lifetimes. Three fresh GPU-qualified A/A pairs differ by
0.27–0.63% at 70.7–73.4ms per CPU iteration. All 1,866 ordered draw observations
match the old verification, and original source/output pixels remain exact.
This corrects measurement fidelity, not game FPS; old timing receipts are
not interchangeable with the new ownership contract.

The [CPU slow-mode follow-up](cpu-shadow-slowmode.07.md) localizes the variable
cost to replay: a same-policy 229→319ms process shift adds about 88.5ms there,
while completion stays near 0.93ms. Unchanged-image external samples spend
53–74% of main-thread samples in software attachment Clear. The CPU fixture
lacks GPU readback and performs shadow fills skipped by the normal provider
for GPU-owned attachments. Diagnostic-only phase accounting now separates
these owners and cannot qualify policy timing. The correction above closes
the attachment-authority gate; the exact old process-speed mechanism remains
unresolved, independently of corrected policy measurements.

The earlier [same-image GT2 policy matrix](gt2-policy-matrix.06.md) passed all
20 CPU/GPU-qualified forward/reverse and A/A comparisons (640 measured CPU
iterations, 80 exact GPU iterations). Its CPU ranking remains inconclusive:
identical-policy process medians shift by -10.28% and +113.16% in the starting
A/A controls, and several policy differences reverse with execution order.
The unchanged 1MiB/64MiB candidate uses 20.86MiB GPU capacity-ledger retention
with no denial or post-reserve growth; 5MiB/64MiB retains 61.84MiB, with 56
optional denials per two-iteration GPU process and nine growth events per
iteration. These are mechanism observations, not FPS or default promotion.
The follow-up above identifies a timing-fidelity gap; the controlled-completion
CPU lane also does not rank asynchronous supply/locality.

The current GPU conclusion is [explicit vertex-position invariance](gpu-position-invariance.04.md).
All ten retained GT2 failing originals now pass two normal-provider cold
replays each: exact original source/output bytes and zero GPU errors (r585).
Generated VS position outputs are explicitly invariant, with one shared
MSL2.4/fast compilation profile. Language-only and global contract-off attempts
were insufficient or incorrect; the original host-metadata control changed
minimum OS as well as SDK. No original or tolerance was replaced.

The fresh public compatibility-exact/production-exact comparison completes
CPU verification, both exact GPU qualifications and CPU measurement, then
atomically publishes the accepted outer result (r593). Post-publication r598
reauthenticates all inputs, policies, child receipts, runtime images and
readbacks. Native 14/14, x86 2/2, the expanded 28/28 regression set and all
190 TLA configurations pass. Interrupted r588 remains preserved and is not
relabelled accepted. The original 5MiB admission failure (r586) and a smaller
policy's compatibility-growth failure (r605) are now repaired without weakening
receipt provenance. Fresh 1MiB/16MiB, 5MiB/64MiB and zero-headroom policy pairs
all pass exact original CPU/GPU qualification (r610–r612), with post-publication
reauthentication in r614. Native 13/13, x86 5/5 and all 196 TLA configurations
pass. See [capacity ownership recovery](capacity-ownership-recovery.05.md).
This closes bounded final-pixel, policy-interface and ownership integration,
not FPS, all-intermediate equivalence, concurrent scheduling or arbitrary-policy
qualification; positive headroom remains opt-in.

### Prior localization evidence (through r553)

The following chronology retains the pre-fix findings and exclusions. Its
statements that the ten final-pixel failures remain open are superseded by
r585 above; separate intermediate and generalization limitations still apply.

The attempted shadow-input capture's Arena/session stall now has a concrete
low-water counterexample and repair: high-water-only session headroom could
hold 1025 pages while admission required 1024 or fewer to reopen. The fixed
queue-local cap and its TLA/native binding pass; GT2 r390 subsequently finishes
without timeout and publishes a 176-source Tape. This is progress evidence,
not GPU-fidelity or FPS promotion. Replaying retained r346 on the repaired
build still produces the byte-identical 69-pixel failure. New r390/r391 has
exact final RGBA but a directly joined R32F shadow difference: six readbacks
repeat the same 934 differing float texels (maximum 60 ULP), after writer
draw554. This is now a concrete intermediate-content investigation target,
not yet a sampled-texel/branch cause of any retained pixel failure. A second
replay reproduces the exact shadow bytes. The 395 shadow writers have equal
observed PSO/raster/depth/geometry inputs; their six selected VS entrypoints
and reachable helpers do not consume the differing FFP material/light bytes.
A subsequent bounded capture/replay localizes the first shadow hash divergence
to source22267's eleven draws260–270, after eleven equal stored endpoints.
That new frame's final RGBA is exact too; the first differing draw and GPU-time
contents remain open. The next paired color/depth capture r405/r406 agrees at
nine endpoints, then differs in both within eleven draws245–255: 121 depth
texels, maximum2 ULP. It also retains a new one-pixel final failure. The same
wrong first-bad depth/color and final bytes survive removing all earlier replay
readbacks (r409). Draw-prefix cuts then localize the replay writer to global
draw251. A small standalone replay of its three triangles177/1194/1195
reproduces all121 differing replay depth values (68/51/2 coordinates), using
1.43MiB of authenticated inputs and unchanged captured MSL. Repetition and
validation-off preserve the reduced depth/color bytes. This is an intermediate
discriminator, not original GPU-input/vertex-output or final-pixel cause proof.
Calibrated atomic observation recovers seven standalone GPU vertex positions
without changing depth/color; duplicate invocations agree, and all four caller
rounding modes preserve the result. This standalone exclusion alone does not
justify a production fenv or shader-math change; original-context evidence is
added in the next cut below.
Native/x86 observer tests
pass; observer-free normal replay retains the identical one-pixel failure and
the public GPU gate rejects it. All older failed originals remain required.
The bounded actual-draw collector is now connected and native/x86 Metal
calibrated. r455 preserves r405's entire wrong depth/final output while observing
all 1872 GPU vertex positions. New original r456 supplies ten source-qualified
position receipts. Its first differing pass has 807 depth texels (at most 2
ULP); r459/r460 finds **139 differing actual GPU clip positions** in the same
draw despite matching source/PSO receipts and issued CPU geometry/VS bytes.
The probe preserves replay depth/final bytes. r456's final image happens to
match, which does not close any old failure. r465 detailed input/intermediate
observation changes the replay to match the original and fails passive
calibration. Thirty-four calibrated standalone controls then isolate skinning
MAD fusion: separate multiply/add gives original positions; explicit FMA
reproduces the 139 replay differences. A digest-selected, default-off arithmetic
intervention repairs r405's first-bad depth and extends its exact shadow prefix
from 9 to 12 endpoints. The next three VS programs were localized to
source22179/replay30; a four-source control r492/r494 now makes **all 32 selected
shadow endpoint hashes exact**, but retains the identical final one-pixel error.
The next retained-input scan (r499–r522) isolates an intervention on the final
target pixel to VS `bad49ade...`, source22283's first draw: global1380 / original
ordinal788346, 4608 indices and a 1480-vertex span. Pass-end and per-draw
one-pixel observations preserve both complete final images. The arithmetic
control makes that pixel exact but introduces 790 other mismatches, so this is
a localization target, not a production fix or proof of original GPU inputs.
On retained r346, the shadow control leaves all 84 scene endpoints and the
69-pixel failure unchanged; changing the two first-bracket scene VS sources
increases the failure to 1751 pixels. No production math policy, tolerance
increase or policy qualification follows.
The target scene draw now has full-image calibrated standalone replay for both
baseline and matched-intervention inputs (r524–r553). A46-pixel inherited color
seed difference is disjoint from the14-pixel local response. Twenty-seven
controls localize the target response to the final skinning MAD; explicit FMA
reverses it. API/shader validation preserves both calibrated images after
correcting the fixture's PS declaration-sized allocation. This subsecond
discriminator does not prove original GPU-input equality or repair any retained
final-pixel failure. See the [r383–r553 evidence](gpu-policy-qualification.03.md).

The [strict GPU policy join](gpu-policy-qualification.03.md) passes independent
synthetic A/A and physically different A/B policies, but rejects real GT2 pixel
mismatches before measurement. Partial-Clear MRT/mip/cube/MSAA and depth/stencil
readbacks now pass. One GT2 trace matches all 2,267 active programmable uniform
payloads; a newer trace matches actual alpha-immediate and observed viewport/
clip binding fields across 2,257 draws. The latter has 230 live physical pass
starts versus 38 ordinary replay starts. The new authenticated source-boundary
diagnostic restores all 230 physical starts and exact original RGBA; two more
cold iterations without effect tracing also match. The same binary with the
diagnostic off restores the 1,749-pixel failure. An older captured frame improves
from 1,453 differing pixels to six one-unit RGB differences but still fails
strict equality. Restoring its 15 missing CPU DrawRun starts produces all 836
original command starts with unchanged physical pass starts and exactly the
same residual, excluding that partition difference. The pure authenticated
source plan initially served the diagnostic; a shared normal driver now serves
the CPU executor and GPU provider as well.
A second trace-complete frame (r65/r66) matches 1,379 draws, all 149 physical
pass starts and original bytes; two trace-off repeats also match. Disabling
cuts on that input restores a 6,943-pixel mismatch. The typed publication seam
retains exact bytes in two further cold repeats (r75); native integration is
9/9. That seam, the borrowed range planner and R-HARN-REPLAY-8.14 reference
contract now underpin normal source-aware replay. Real r65 input is exact in
two cold iterations per route (r91/r92); ProductionSlot exercises 133 Direct
leases and 1387 Direct draws plus 10 compatibility draws. Normal Direct also
preserves exact r36 pixels twice (r95), while r28 retains the same six-pixel
counterexample twice (r94). Native 7/7, x86_64 6/6 and existing TLA pass. The
public v2 policy/receipt join now binds source mode, plan digest and actual
source publications through CPU/GPU execution and final reauthentication.
The r65 input passes public CPU and GPU qualification (r103/r104), a real
Compatibility/Direct A/B with sixteen measured iterations per policy and
Direct A/A, freshly rechecked after the final CLI/authentication fixes in
r117/r116. Native/CLI/Metal and existing TLA pass 9/9 (r113); altered expected
plan digests reject before replay effects on both executors (r119/r120).
These are input-qualified experiments, not general GT2 fidelity or production/
FPS promotion. Normal non-diagnostic v2 Direct still rejects r28's same six
one-unit RGB differences twice (r121). Its final scene/bloom offline analysis
weakens ordinary float32/double final-ALU rounding as an explanation, but is
not a Metal pixel oracle. Original current-frame intermediate bytes and
emitted shader/compiler lineage remain unobserved; obtain that discriminator
before attributing the residual or changing arithmetic. Diagnostic readbacks
remain excluded from ranking.

Actual driver-compilation observers now show the same translated sources and
six-pixel r28 residual on ARM64 and x86_64 (r131/r134), without recovering the
historical live compiler identity. The bounded scene/bloom probe r145 retains
the same residual. A visible indexed A→B→A fixture now proves both routes with
an independent color/geometry oracle and rejects altered PS or stale VS input;
provider/CLI/compiler-lineage tests pass 3/3 on native r148 and x86_64 r150.
These positives do not close r28. Live r146 has an independently valid
222-source tape, but its runner was terminated during finalization for low
disk space. Same-input r152/r157 replay now exposes a new two-pixel residual:
bloom input is exact, while scene input already differs at six pixels (seven
one-unit channel bytes), before the final composite. All 236 observed physical
encoders and 2296 FS-volatile binding rows align. The missing non-indexed effect
observer is fixed and covered by a small on/off GPU test. Disabling actual PSO
prefetch in r159 replaces all 12 differing VS fallback-literal variants with
their live sources but leaves intermediate/final bytes unchanged, excluding
that source-text difference for this frame. Earlier scene writes remain the
localization target; neither r28 nor r146 qualifies strict GPU promotion.

The bounded pass-end ROI probe r161 preserves the two-pixel failure while
reducing observed RGB-changing scene endpoints to three of 123 passes
(captured sources 8555, 8661 and 8683). It is replay-only RGB evidence, not
the first divergent draw: original intermediate and alpha comparisons remain
open. Summary-only readbacks now also invalidate GPU policy qualification;
the small actual-GPU fixture remains pixel-exact but diagnostic and rejected.

Live r171 completes and publishes a valid 174-source Tape, but its fixed
encoded-frame dumps are disjoint from that Tape (zero joined sources).
Observer-off r172 reproduces a new strict three-pixel failure twice, alongside
the preserved r28/r146 failures. Bounded hash-only readback and pre-Ready
capture-source registration now have native and actual-GPU pins (r176/r178,
5/5 per architecture, reconfirmed after cold pass-storage changes in r180/r181;
existing TLA r177 passes). No new fields enter runtime
payloads/tickets; pass provenance is diagnostic-owned. The new native positive
source-to-completed-GPU-hash fixture passes EventSerial and SegmentSerial,
with independent full-image constants and exact production-ledger joins
(r183/r184, 2/2 per architecture including the existing Metal oracle). It has
no PE publisher. After the user freed space, live r185 authenticates 211 original
pass hashes against its published 198-source Tape; 345 out-of-frame observations
are excluded. r186 preserves exact final pixels, and observer-off r187 repeats
that result twice. Of 211 paired intermediate endpoints, 45 raw hashes differ
(mostly R32F); canonical cross-process attachment identity and raw difference
localization are still open. The r190 capture-only repair now namespaces
event-local pass IDs before frame-wide publication; native/Metal pins and TLA
pass. r191 then publishes a trace-complete 101-source capture, and r192/r193
preserve its exact scene/bloom and final pixels. r194 repeats all three original
inputs twice and retains their identical 6/2/3-pixel failures. These passing
new inputs do not close r28/r146/r171. The next
current-build capture without per-pass hashes (r201) also fails by six pixels:
scene input already differs at five pixels, bloom is exact, and observer-off
repeats plus replay-side pass hashes retain the same failure SHA (r202–r204).
Effective late-resolved Store witnesses now have native/actual-GPU pins
(r205–r207); r208 identifies seven replay-only RGB writer candidates among
80 stored scene endpoints. Observer-off compatibility r209 retains the same
failure twice, excluding a Direct-only explanation. Restoring the original
candidate VS compilation variants by actual prefetch suppression also leaves
scene/bloom/final bytes unchanged (r210–r212). Actual tracked binding observation
now has A→B→A, source-scope and Metal pins (r213–r223). Paired r221/r224 is exact;
r225/r226 instead fails by 56 pixels despite equal observed VS/PS backing,
volatile bindings and all 2,213 indexed stream-0/index hashes. FFPVS full
backing and some FFP shader hashes differ; all-state equality is not proven.
Observer-off r227 retains the failure once. The original r225 texture dump
budget expired before its published composite. Source-qualified texture
budgeting now has native/x86_64 Metal pins and a published r233/r234 pair:
2,127 draws, identical final pixels and bloom, with only one scene alpha byte
different. This positive does not close the RGB counterexamples. Conservative
compiled-source audit finds no reads of the differing FFP material/light fields
in r225/r233 VS variants; the ten changed FFP VS sources differ only in a hash
comment. Selected GPU instructions/addresses remain outside that proof.
Channel-separated completed-readback hashes have actual Metal pins (r237).
r238/r239 pairs 179 Store-qualified endpoints: 131 raw-exact, two alpha-only
differences and 46 R32F differences, no BGRA/RGBA RGB difference; final inputs
and output are exact. Observer-off r240 still reproduces r225's identical
56-pixel failure. Actual indexed-call/bound-stream0 observation r243 preserves
that failure: all 2,213 indexed ranges match replay logical and retained live
geometry, including five byte-preserving index staging rebases. The 18 remaining
draws and actual GPU reads are outside that observer. New failing r254/r255
pairs 132 scene-sized stored endpoints: the first observed RGB difference is
source 22693 (eleven draws), preceded by an exact same-scene endpoint. Actual
indexed geometry (2,233), raster state (2,251) and VS/FS volatile (2,251 each)
match. Final scene differs at one pixel; observer-off r256 repeats the same
three-pixel final failure twice. Wire audit r258 closes logical scene seed,
Clear, alias and final-read provenance; the candidate's produced 2048² R32F
input (371 preceding writer draws), actual PSO/view/sampler and intra-pass
localization remain open. After disk recovery, r261/r262 retains all three
selected-family texture inputs: R32F differs at 209 values but final output is
exact. New failing r263/r264 retains 72 wrong final pixels; the first observed
scene difference is a four-draw bracket. A canonical wire no-writer interval
connects its R32F input to later readback (1,415 differing float values), while
the selected family's other mip/cube inputs are exact. Those material/cube
inputs do not cover the four earlier draws' different PS families. Observer-off
r266 repeats identical wrong bytes once, then stops on provider rejection.
Controlled R32F replacement r274–r286 preserves that same 72-pixel failure
under both replay-produced and original-produced input, with authenticated
post-upload readbacks. Ten selected-view dumps retain the original input;
an all-zero control changes 515,132 pixels. This lowers the priority of the
observed ULP differences, not the strict GPU gate. The earlier four draws'
other inputs and actual selected PSO/view/sampler lineage remain open.
Actual extra-stream coverage r296–r308 adds a visible stream-1 POSITION pin
and paired same-build GT2 evidence: r300/r303 has 107 equal extra-stream ranges
and 1,884 equal indexed stream-0/index ranges, yet two final one-unit RGB
differences. The first observed scene divergence is source 22190's eleven draws
after exact source 22189. Both observer-off cold repeats retain that wrong SHA;
r263 still fails by 72 pixels under the new observer. CPU Shared byte equality
does not prove selected compiled PSO, actual texture/view/sampler binding or
later GPU reads. Actual selected-PSO creation provenance r310–r327 now joins
actual bound pipelines to immutable source/function/descriptor receipts. Fresh
r315/r316 and r319/r320 are pixel-exact positive pairs, not replacements for
old failures. On the unchanged r263 input, prefetch-off changes the selected
VS source on 872 draws but preserves the identical wrong 72-pixel output;
the four first-bad draws' creation receipts do not change. Original r300 also
retains its two-pixel failure with prefetch off. This lowers the priority of
prefetch variant selection as the sole explanation. Actual texture/view/
sampler bindings and GPU-time contents remain open; older originals lack
these newly added PSO receipts. Fragment sampling descriptor observation
r328–r335 now covers actual direct calls and bounded argument-table writes,
with exact native mip-view pixels and invalidation negatives on both routes.
Replayed r263/r300 still have identical wrong output hashes, with respectively
7,617/7,519 expected-mask stage entries carrying actual direct descriptors and
no unknown entries. Older originals lack sampling receipts, and descriptors do
not prove resource identity or contents. r263 executes through readback but is
rejected by its source oracle; r300's lower envelope completion is not strict
GPU qualification. Fresh r346/r347 adds an actual object/content join:
7,573 paired texture/sampler descriptors each are equal, but six R32F cube
pass pairs each differ by 2,168 float texels and the final output by 69 pixels.
Object tokens are same-process only; canonical generation and explicit
no-writer intervals connect the readbacks across processes. The first scene
difference precedes cube sampling. Prefetch-off r349 makes the first-bad eight
actual PSO construction inputs equal to the original without changing cube or
final wrong bytes. These are rejected readback attempts, not completed cold
iterations. Actual depth/stencil creation/binding/reference is now observed
and pinned by native/x86 pixels and production-row tests. New r359/r364 pairs
have 1,857/1,859 equal issued depth descriptors and exact final pixels, but the
same new binary retains r346's identical 69-pixel failure. Older failing
originals have no depth receipts; contents at GPU consumption remain unproven.
Actual direct-buffer calls now independently qualify cache coherence, with a
native stale-rebind pixel negative. r373/r375/r376 each report 7,412 matches
and identical observed bound bytes/84 replay scene hashes, yet preserve r346's
69-pixel failure. API/GPU validation reports no access error; dedicated
transient buffers change the real equal-work allocation count 126→1,709
without changing output. These cross-build-original checks do not support
replay rebind omission or slab reuse alone as the cause. r370 identifies the
first-bad eight draws' common sampled 2048² R32F shadow; original GPU input
contents there remain missing. Strict qualification remains open.
See the owning GPU leaf for scoped
exclusions; no tolerance was widened or original oracle replaced.

The [qualified low-observer CPU policy interface](cpu-policy-measurement.02.md)
now compares unchanged GT2 input with separate semantic qualification and
measurement processes. A/A, compatibility command-limit 0/4 and Direct
exact/headroom comparisons pass with 1,420 identical ordered draws. Actual
source-count and provisioning changes are observed; iteration service timing
omits detailed proof work but retains runtime counters. This is a bounded
replay/Slot experiment interface, not an encode/GPU or wild FPS result.

The [fixed-input replay experiment](fixed-input-replay.01.md) adds a separate
production replay/Slot policy loop: authenticated PE Tape, same-process warm
Metal repetition and bounded completion fixtures with actual Metal-buffer
bytes but no GPU submissions. Direct/compatibility, external completion
schedules, a real Managed allocation-failure/retry case, and the grouped
seven-Direct/two-compatibility capacity fallback pass; small captured
Metal paths also pass. A fresh full-frame GT2 identity-v2 capture now
validates at the explicit 64-record cadence. The unchanged full frame now
passes the CPU comparator 16 times per route: 1420 draws, including 1410 Direct
and 10 compatibility draws in the candidate, with matching ordered semantics
and actual buffer-byte observations. An explicit source-projection oracle now
qualifies a one-source/one-mutation completion window: GT2 r144 passes 16
repetitions per route, with three deferred DISCARDs and 20 retained-buffer byte
checks per replay. Every draw also matches the immediate-completion reference;
the production visitor rechecks all 39 source closures. Actual A-to-B-to-A
reset and surviving-surface/retired-parent attribution controls now pass, with
decisive native faults; validation-enabled CPU runs are diagnostic only.
Source-owned pending proofs now reject six stale identity dimensions. Native
two-source FIFO lifetime and real 31-source capacity saturation/reclaim pass
on both routes. Public two-source scheduling now passes GT2 r102 at actual
pending peak two: all 16 repeats per route conserve 39 sources and match every
draw against a separate immediate-completion control. Input-derived whole-source
eligibility, per-owner FIFO proofs and actual isolated-runtime admission bound
this CPU-only window; noneligible sources drain before a later eligible source.
CPU input staging now requests independent copy-on-write copies on macOS,
allowing the fresh GT2 comparison without deleting prior evidence. Canonical
mipmap command/lifetime tests also pass. Active negotiated explicit mip capture
now records a canonical source and uses its exact completion ticket; unsupported
pre-arm/direct calls remain explicitly rejected. The completion helper and
capture-abort fence pass CPU native tests, while actual PE COM CapacityPost
fault integration remains separate. A conditional progress model detects the
missing-release wedge without claiming Metal driver progress. A subsequent
native production-loop test exposed lost exact-event identity during singleton
physical retirement. Identity now transfers through the queue receipt and
ordered finish FIFO; native source reuse, receipt ABA, mixed resident/receipt
ordering and actual C API acknowledgment tests pass. This proves completion of
an unsubmitted production source with a CPU-controlled backend, not GPU progress
or observation of the coordinator asleep on its CV. The bounded CPU R8
policy/lifetime harness is now qualified: explicit reset/cache/device sharing,
phase/process-CPU receipts, 16-repeat external plans and retained failure
reproduction pass. Bounded phases are exclusive and include teardown; general
phase observations retain their own accounting semantics. The final gate passes
25 native tests and five x86_64 tests; the lifecycle models retain their full
188-configuration TLA pass. This is a CPU experiment tool, not a promotion gate.
Both Metal replay policies still produce
the same capture-oracle mismatch; strict pixels and broader delayed
multi-object lifetime qualification remain open. It does not replace the GPU-slice
methodology below or measure game FPS.

| # | Hypothesis | Verdict | Evidence |
|---|---|---|---|
| H18 | Primitive-conflict metrics can split rank-1 visible failure from rank2-4 exact owner-masked passes | rejected; only final-color metrics separate | mini-replay-bisection-texture.07 |
| H19 | Existing D3D9 occlusion query or winemetal visibility plumbing can supply the missing runtime oracle as-is | rejected as production oracle; diagnostic scout added separately | [mini-replay-bisection-texture.08](mini-replay-bisection-texture.08.md) |
| H20 | A diagnostic Metal visibility scout can supply per-Metal-draw sample-visible counts after GPU completion | implemented diagnostic; not final-color proof; old rank-1 `36..37` is sample-visible | [mini-replay-bisection-texture.09](mini-replay-bisection-texture.09.md) |
| H21 | No-sample visibility rows are the hidden-backend hot locality owner | rejected; zero rows are small and only `-2,016` of `-182,856` LRU32 delta | mini-replay-bisection-texture.10 |
| H22 | Positive Metal visibility samples can be used as the scoped depth-read final-color oracle | rejected; rank2 has `39,835` samples but `0` final-color pixels, and rank1/rank3 are both sample-positive but fail/pass diverge | mini-replay-bisection-texture.11 |

## Current Navigation

- [Domain index](index.md)
- [Historical log](log.md)
- [Root 3DMark05 GT1 map](../overview-3dmark05-gt1.md)

## Recent Leaf Documents

- [mini-replay-bisection-texture.09 - Metal Visibility Scout Wiring](mini-replay-bisection-texture.09.md)
- [mini-replay-bisection-texture.08 - Occlusion Oracle Feasibility Gate](mini-replay-bisection-texture.08.md)
