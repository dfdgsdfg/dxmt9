---
domain: present-pacing
workload: 3DMark05 GT1
title: "Present-Pacing — display sync, frame latency, and the wallclock cap - Current Overview"
type: domain-overview
status: current
updated: 2026-10-17
source: docs/perfomance/present-pacing/log.md; docs/perfomance/overview-3dmark05-gt1.md; docs/perfomance/present-pacing/present-pacing-current-bottleneck-pe-symbol.236.md; docs/perfomance/present-pacing/present-pacing-cpu-ready-next-source-intent.239.md; docs/perfomance/present-pacing/present-pacing-pe-direct-canonical.242.md
related: docs/perfomance/present-pacing/index.md; docs/perfomance/present-pacing/log.md
---

# Present-Pacing — display sync, frame latency, and the wallclock cap - Current Overview

[H656](present-pacing-scheduler-full.656.md) prevents an unconditional Replay
ranking: its OFF arm has Encode29.53–29.82 > Replay28.83–29.03ms/frame, while
the ON arm reverses this order. Full-template trace stop times out despite disk
headroom; the fixed population stops, without replacing the failed run.
[H657](present-pacing-scheduler-minimal.657.md) qualifies minimal scheduler plus
signpost instruments on a native fixture, not yet the game. Default transfer
and the current global maximum remain open.

[H655](present-pacing-scheduler-live.655.md) retains original-game Replay CPU
42.470–42.774ms/frame, above all observed composed owners. Raw trace recovery
succeeds, but only2.79s and no exported clock markers survive, so full6.87s
scheduler attribution is rejected. No default/global ranking or new saving is
claimed; the next separate capture must preserve the full non-rolling trace.

[H654](present-pacing-external-clock.654.md) connects external marker clocks
to a separate Rosetta target without modifying its binary. The helper-only
mapped CPU upper contains target self CPU; native qualification does not yet
close actual-game coverage or default-owner transfer.

[H653](present-pacing-scheduler-clock.653.md) qualifies short attached-process
clock mapping in ARM64/Rosetta, with offset apertures1.708/9.958µs. Both busy
self-CPU deltas fit Running+Interrupted+Unknown over outward-mapped windows;
Running alone undercounts. This enables same-window native attribution but is
not original-game coverage, losslessness, or observer-OFF transfer.

[H651](present-pacing-owner-coverage.651.md) checks the12 exports/326 known
slots and conditional launch selectors against frozen evidence; it does not
invent an in-process configuration receipt. [H652](present-pacing-scheduler-preflight.652.md)
successfully exports System Trace scheduler intervals in ARM64/Rosetta fixtures.
Running-only is an observed undercount; Interrupted/Unknown must remain in a
conservative upper. Same-camera clock mapping and live coverage are the next
gate for otherwise-unobserved native threads, not another per-draw observer.

[H650](present-pacing-retirement-live.650.md) qualifies actual live archive
retirement in four fixed original-game arms. Replay is34.48–38.92ms/frame and
Encode32.20–33.78 across the four episodes; each episode separately has
Replay lower above Encode upper. In both observed arms Replay also exceeds
all observed composed thread uppers, with archive uncertainty below0.118ms/frame
per worker. Effective-policy/entry/platform coverage and default transfer still
gate global completion. H641 alone supplies the separate qualified removal
screen; no new FPS or saving claim is made.

[H649](present-pacing-retirement-owner.649.md) adds terminal-qualified owner
envelopes in a coherent private binary. Four original-capture arms preserve
pixels/query diagnostics/lifetimes; native tests close six threads before join
and reject reinitialization. H650 supplies its live archive application, without
a production promotion.

[H646](present-pacing-owner-lifecycle-live.646.md) completes the original-game
owner population. Replay lower exceeds Encode upper in all six O/I/R arms;
same-episode Primary-root uppers19.42–22.95ms/frame in R are below Replay33.51–40.36.
Finish/Completion are small. [H647](present-pacing-owner-composition.647.md)
closes the sparse/lifecycle postprocessing omission; the archive workers still
lack actual retirement witnesses. C0 does not reverse worker ordering and
O0→O1 drifts substantially, so this population adds no new removal/FPS claim.
[H648](present-pacing-thread-retirement.648.md) qualifies a native retained-port
mechanism, not yet a product terminal authority. H641's separate removal screen
remains the strongest removable-cost evidence; the global goal is still open.

[H645](present-pacing-owner-lifecycle-live.645.md) finds close baseline worker
costs (Replay32.34–32.68, Encode31.68–32.17ms/frame across two controls), then
rejects combined attribution because the original EXE cannot locate the clock
service. PE32/PE64 loader controls confirm the app-local deployment omission.
Correct deployment before a new population; H645 does not close global ranking.

[H644](present-pacing-foreign-disposal.644.md) closes the original-block disposal
observation gap in a private provider. Actual API OFF/ON controls and original
capture controls pass. It does not bound all platform post-release work or
establish new live CPU savings; combine it with PE-root and H643 lifecycles next.

[H643](present-pacing-worker-lifecycle.643.md) connects H642 to actual native
worker creation and successful joins, with original-capture OFF/ON/candidate
controls passing. All22 observed Encode/Finish/Completion/compile lifecycles
close. Replay-offload/WSI/archive live paths and foreign callbacks remain
unqualified here; this does not alter H641's live result or close global ranking.

[H642](present-pacing-thread-envelope.642.md) closes a measurement-design gap:
worker-body return does not cover callable or TLS destruction. A conservative
launch/self-sample/join envelope passes finite-model and native controls; it
retains unknown teardown rather than assuming zero. This is preflight only,
not evidence that teardown is a live bottleneck or that global ranking is closed.

[H641](present-pacing-snapshot-performance.641.md) passes a fixed audit-off
live screen: two order-reversed blocks reduce Replay by5.645–6.029 and
6.295–6.670 CPU ms/frame. Median lower5.970 exceeds sham5.153; the margin is
only0.818, and these are finite enclosures, not statistical confidence bounds.
Reference Replay is above Encode in8/8 episodes; candidates reverse it in4/4.
The four treatment-pair means are36.900→30.740ms/frame, not an FPS guarantee.
Same-episode app/auxiliary accounting remains necessary for global attribution.

[H640](present-pacing-snapshot-live.640.md) carries the candidate into original
live game execution with an actual hot/layout/uniform equality audit. Q1 has
5,609,053 clean checks, all seven camera knots and natural exit; capture
controls still match original pixels/query/lifetimes. Q0's log-level gate
failure is retained. Diagnostic Replay42.428–42.671ms/frame includes audit
work and cannot establish a default bottleneck or improvement. The next gate
is the same binary with audit off and matched reference/candidate controls.

[H639](present-pacing-snapshot-consumer.639.md) advances snapshot sharing from
shadow to actual borrowed queue delivery and final SoA ownership. Original
pixels/query transcript/lifetimes pass, including all42 fixed timing runs.
Same-input Replay CPU point means are63.918→59.587ms; the median paired
reduction is4.1775ms. Its lower-bound screen clears sham by only0.1593ms.
The denominator is260 provider roots including bootstrap, not a live frame.
Live transfer and generation-writer/Arena coverage remain open; H636 still
does not identify a unique original-path Replay/Encode winner.

[H638](present-pacing-snapshot-owner.638.md) advances that candidate to a
private owned-storage refinement and actual-input shadow: 726 base owners
preserve 2,343 run-specific debug/binding/range descriptors. Foreign-owner
tokens reject after an independently found epoch-only counterexample is fixed.
The original renderer still emits commands; GPU equality does not qualify an
unconnected candidate consumer. Borrowed-view queue integration, measured CPU
removal and default-live attribution remain required.

[H637](present-pacing-run-sharing.637.md) supplies a concrete removal target
without repeating a failed micro-copy screen. Same-batch binding churn creates
1,617 additional runs whose generation/stride key and actual hot/layout/uniform
values match the predecessor, among 2,343 total runs. All four original-input
controls pass. Snapshot owners and per-run binding/debug descriptors could be
separated, but ownership/view refinement, CPU savings and live transfer are
not yet implemented or proved.

[H636](present-pacing-minimal-owner.636.md) completes original/inert/active
minimal controls in both order directions. Original O1 Replay31.551–31.697 and
Encode31.420–31.802ms/frame overlap, as do O0's wider bounds. Active PE-root
uppers21.844/17.838 are below both workers, but outside-root Primary CPU is
not automatically game-only work. Finite contrasts include workload/layout
variation, not calibrated observer taxes. Both workers remain candidates;
same-input semantic-work elimination and live transfer are still required.

[H635](present-pacing-guest-live.635.md) reaches actual live collection:
528 identity-qualified rows cover 132 complete late-scene render paths.
Dispatch dominates the coarse Primary samples at 43.347ms CPU/path; Begin and
End are small. This includes game/Wine/PE/synchronous Unix, so it cannot be
called a dxmt9 bottleneck or compared by subtraction with H629. Next qualify
marker controls and compose minimal PE-root/worker-self observations; default
ownership and removable CPU are still unproved.

[H634](present-pacing-guest-install.634.md) passes original/inert/armed
synthetic cold-install controls and prepares a private game image without
altering original files. All 2,048 machine snapshots per arm match; 1,408 armed
CPU rows validate. The catalogue launcher override bug is corrected. This is
installation preparation, not live-game qualification: loaded-image, screen,
closure and observer-cost controls still precede default-path attribution.

[H633](present-pacing-guest-observer.633.md) qualifies the integrated synthetic
observer: guest markers → native CPU service → identity/phase checks → fixed
storage. Wine passes 512 state comparisons and all 1,408 raw packet checks.
Its bridge-call median of 1.541µs is not total marker overhead or a transferable
live correction. The game remains unmodified; safe cold installation, live
unmodified/inert/armed controls and actual CPU removal evidence remain open.

[H632](present-pacing-guest-phases.632.md) closes bounded continuation and
event-pairing checks, not live attribution. The real caller can bypass Begin
when inactive; its continuation must not manufacture a completed frame. Wine
passes 1,152 paired state cases; both native architectures pass 87,381 event
words and detect order/identity mutants. The callbacks and reducer are not yet
integrated with the game or CPU clock service. Default owner and removable
budget remain unproved.

[H631](present-pacing-guest-marker.631.md) advances guest instrumentation only
to synthetic ABI qualification. A before-call marker recreates the exact
original CALL return address; 256 paired cases and three preservation-omission
controls pass under Wine. No game image is changed. Final continuation,
bounded event storage, safe installation and live perturbation controls remain
required; no new default-owner ranking or removable CPU estimate follows.

[H630](present-pacing-guest-boundaries.630.md) makes game-side logging concrete
without claiming a new CPU result. Three authenticated caller sites delimit
Begin, callback dispatch and End; End's tail call and the dispatcher's EAX ABI
rule out naive epilogue/C++ wrappers. This coarse observer could explain supply
phases but includes game/Wine work and is not complete D3D9-root coverage.
Runtime shim qualification is still required; no target patch was made.

[H629](present-pacing-owner-union-live.629.md) closes the observed Primary
overlap: Encode draw-loop lower bounds exceed the new PE union uppers in both
ON episodes by 0.173/0.800ms/frame. This is an individual-partition ranking,
not a whole-thread default bottleneck or saving. All four fixed runs and raw,
identity, image and restoration checks pass. OFF Replay/Encode still overlap;
the combined observer's Replay contrast is +2.39–4.43ms/frame. Next reduce
observation for default transfer and retain lifecycle-tail exclusions; only
a separate correctness-qualified intervention can establish removable CPU.

[H628](present-pacing-root-union.628.md) advances the unresolved Primary
exclusion with uncertainty-preserving interval union. Both native architectures
and real PE fixtures pass, including 16 corrupt-map rejections and complete
staging restoration. Tight-loop bounds improve without reduced endpoint
allowance or removed CPU work. H629 adds live same-camera composition;
do not re-price old H621 episodes from the fixture or claim the global default
maximum/removable budget is closed.

[H627](present-pacing-override-projection.627.md) closes lexical binding-override
projection without promotion. Full context copies fall 81.0%, native reset
counterexamples and original GPU/query/lifetime checks pass, but the 42-run
CPU screen fails: median +0.722ms, sham bound 0.937ms, two negative blocks.
Do not assign the caller-loop budget to copying. Prioritize the remaining
maximum-owner comparison: H621 Primary's upper bound contains 3.1ms/frame of
endpoint allowance. A tighter bound requires new qualified evidence, not a
smaller assumed allowance. Live removable CPU remains unproved.

[H626](present-pacing-layout-reuse.626.md) closes the declaration-decoder reuse
candidate without promotion. Actual calls fall 3,982→757 and original GPU,
query and lifetime effects remain exact. The 42-run frozen root-CPU screen
fails: median OFF-minus-ON +0.172ms is below the 1.814ms sham bound, and only
four of eight block lower bounds are positive. This is not a live saving or
proof of zero decoder cost. Investigate larger caller/preparation and pass
work only after identifying a concrete redundant operation; global ownership
and the removable budget remain open.

[H623](present-pacing-encode-setup-live.623.md) rejects assuming per-draw
PSO lookup: the conditional base block is already skipped on about 82% of
live draws. Its observed CPU upper bound is 1.94ms/frame; actual recorded
draw wrappers are below 1.79. Common setup remains 1.76–3.27, without a
separated ranking or removal proof. Four fixed runs and raw/restoration
checks pass; Encode detail contrast is +0.15–1.18ms/frame. Next screen a
concrete DrawRun-local declaration decoder reuse candidate, not another broad
breakdown. Its prior secondary cost and stride dependencies remain binding.

[H622](present-pacing-encode-live-detail.622.md) decomposes the actual-live
Encode loop. Setup, index/issue and caller preparation have overlapping
substantial bounds; setup/index each exceed argument/uniform and stream-prep
upper bounds, but not each other or pass management. Four fixed runs pass
raw and restoration checks. Fine observation adds a +2.08–4.13ms/frame Encode
contrast, so observed subphase cost is not removable default cost. Next split
per-draw setup from conditional base-state lookup/binding; existing DrawRun
reuse must be counted before introducing a second cache. Global default
dominance and an experimentally demonstrated removal remain open.

[H621](present-pacing-owner-composition.621.md) closes the cross-episode gap:
simultaneous observation places Encode's draw loop at 18.15–19.78ms/frame,
above every individual Replay partition and its unassigned budget. Primary
renderer-root upper bounds still overlap; wrapper/closure tails and default
transfer remain open. Four fixed runs and independent raw/restoration checks
pass. Replay's finite observation contrast is +2.42–3.79ms/frame, while OFF
whole-thread rankings are not stably separated. No removable CPU is proved.
Next investigate a concrete redundant operation inside the actual-live draw
loop, retaining the separate obligation to tighten the Primary exclusion.

[H620](present-pacing-uniform-owner.620.md) prices the real cache paths on the
original input: 190 full builds versus 1,112 settlements, with fixed projection
only 0.187–0.189ms observed wall. Hash processing leads exclusive wall, not a
separated CPU ranking. Four exact controls and independent raw/parent checks
pass. No new copy candidate or live saving is qualified. The remaining global
comparison needs large Replay/Encode regions in one episode and current
Primary/other-owner exclusions, rather than combining different populations.

[H619](present-pacing-uniform-fixed.619.md) confirms fixed-payload stack
gathering and a second cache copy survive compilation. A private direct writer
removes the final 2,468-byte memcpy at both sites and passes native/original
GPU checks. The frozen captured-body CPU screen fails advancement: median
OFF-minus-ON +2.941ms, two reversed blocks, OFF/OFF envelope 8.155ms. This
does not establish zero intrinsic cost, but does not justify live promotion
or assigning H618's whole uniform budget to projection. No product changes.

[H618](present-pacing-replay-cache-live.618.md) establishes uniform build/refresh
above the other individual cache components in both observed live intervals.
Whole queue batch remains 7.08–9.71ms/frame and owned-run 4.29–6.10; these are
cost envelopes, not savings. The finer partition does not contradict H615's
larger aggregate outside-queue loop. Four fixed runs and independent raw/file
verification pass, but observer burden and OFF drift block an exact default
budget. Distinguish actual uniform changes from fixed-projection materialization
before choosing a new candidate; global dominance/removal remain open.

[H617](present-pacing-replay-cache.617.md) narrows cache reconstruction on the
original capture without selecting a new optimization: shader-layout, uniform
and hot-state CPU bounds overlap. Owned-run construction is larger in this
capture, but H597–H600's removal verdict remains closed. Four exact controls,
1,040 raw rows and 3,988 hash checks pass. Different cold/live route mixes and
detail overhead prevent live-budget extrapolation. Next qualify actual-live
layout/uniform/hot-state costs inside compatibility preparation, retaining
owned-run construction as a competing region.

[H615](present-pacing-replay-owner-live.615.md) localizes Replay outside-queue
work: the record-loop lower CPU bound exceeds every individual competitor's
upper bound, including uncovered CPU, in both observed late windows. Its
camera-adjusted14.58–17.71ms/frame is not removable time; observation itself
adds CPU and OFF Replay/Encode still overlap. Four fixed runs, exact captured
controls and56 restoration hashes pass. Next distinguish draw preparation,
sparse state application and loop bookkeeping before selecting a removal.

[H613](present-pacing-encode-census.613.md) replaces sparse live attribution
with complete roots: about97% Encode CPU coverage, zero boundary straddlers,
and a60–65% draw-loop CPU share in both ON windows. Four fixed runs qualify
identity, natural exit, screen and56-file restoration. The observer adds
0.220–1.531ms/frame Encode CPU in finite contrasts; no exact tax subtraction
or removal claim. Encode's largest region is localized, but comparable OFF
Replay CPU and its outside-queue work still block a global-largest conclusion.

[H612](present-pacing-encode-issue.612.md) shows that actual recorded draw-call
scopes occupy only42–43% of index/issue wall on the original input. Index
preparation and volatile push are smaller; the remaining policy/control path
is about1.06–1.07ms. Four exact controls and36 raw root checks pass, but detailed
observation adds CPU. No removal or live rank is proved. The next gate is
complete late-episode root/loop coverage with a qualified coarse observer,
not another micro-optimization selected from intrusive captured timings.

[H611](present-pacing-encode-setup.611.md) completes the two setup questions:
declaration decode and PSO naming total about0.969ms, approximately7% of the
captured loop wall. All four original oracles pass. Their CPU lower bounds
remain zero and detail overhead overlaps variation; these are secondary owners,
not established savings. Next divide index/issue into actual recorded draw
emission and surrounding preparation. No product or live-policy change.

[H610](present-pacing-encode-phases.610.md) narrows H609's loop on the original
capture: outer preparation, setup and index/issue have similar costs, not one
dominant texture-binding phase. Four original oracles and36 raw root checks
pass. Detail increases loop wall by1.342–2.056ms versus coarse controls;
do not transfer phase percentages into unobserved live savings. Declaration
decoding and base-rebind PSO naming are bounded next setup questions; prior
scratch/raster/VS-shadow candidate verdicts remain closed.

[H609](present-pacing-encode-owner.609.md) closes two sparse live Encode
attribution runs. Whole-loop CPU bounds exceed every other individual phase
in both sampled source populations; pass preparation is the next larger
component. Exact captured effects and identity/conservation/restoration checks
pass. Replay leads one whole-window comparison and overlaps Encode in the other.
The small sample CPU coverage forbids ms/frame extrapolation: pursue in-loop
work ownership, not a claimed removable60% budget or another small queue trial.

[H608](present-pacing-pair-live.608.md) fails the fixed live-removal screen despite
[H607](present-pacing-pair-production.607.md)'s actual-call-site correctness.
Replay contrast is mixed and smaller than baseline variation; CPU nonregression
passes, pacing nonregression does not. Encode leads four windows, with one near
tie, so detailed Replay diagnostics must not define the permanent bottleneck.
All five runs, independent raw arithmetic and 70 restoration hashes pass. Keep
the candidate private and move to source-qualified live Encode whole-loop
attribution; global largest operation and removable CPU remain unproved.

[H606](present-pacing-pair-shortcut.606.md) qualifies a private exact-prefix
shortcut locally: 52–64ns/entry reduction, roughly one tenth of warmed batch
preparation, exceeds aggregate sham 5.52ns in both orders/processes. Exhaustive
short-state and seeded differential tests pass on both architectures; candidate
scratch outputs match. Rendering remains reference-only. Production-call-site
GPU exact verification and default-live CPU transfer are next; this does not
close the global CPU-owner/removal objective or justify broad FPS repetitions.

[H605](present-pacing-prepare-deferred.605.md) narrows preparation to state/layout
pair work in warmed actual-input scratch (about 550ns/entry). Disabled counter
clocks add 50–61ns/entry; four order/process lower bounds exceed aggregate sham
5.77ns. Exact controls and independent raw verification pass. Test an equivalent
exact-state shortcut next; live transfer and the global removable CPU budget
remain unproved. [H604](present-pacing-prepare-local.604.md) is excluded after
original-pixel mismatch, not reused as timing evidence.

[H603](present-pacing-queue-partition.603.md) narrows ordinary queue candidates
to append, immutable preparation and marking. O1 CPU bounds are1.284–3.615,
0.539–2.870 and0–1.921ms/frame respectively; they do not establish unique rank.
Reservation/planning are small. Process-exit closure corrects an overstrict
destructor-only observer contract; original rejections and O0's large start
allowance remain visible. At least21.969ms/frame Replay CPU lies outside this
queue envelope. No global maximum, removable budget or promotion is claimed.

[H602](present-pacing-generation-cpu.602.md) repairs the diagnostic identity gap:
Replay samples and phase aggregates carry lifecycle domain, with explicit
Bind/Retire validation. Native reuse/failure controls and exact capture effects
pass; no product policy changes. Next combine the repair with queue-body cost
partitioning rather than repeating coarse FPS trials. No H601 result is rescued
retroactively and no removable CPU amount is certified.

[H601](present-pacing-queue-cpu-owner.601.md) advances one-episode attribution:
ordinary queue CPU 6.888–8.483ms/frame exceeds cache/run individual ceilings.
The queue scope includes marking, unlike H596's exclusive group. O1's normal
exit is not a qualification pass: address reuse across lifecycle domains fails
the frozen parser. Preserve rejection, fix observer generation composition,
then split the queue body. Global maximum and removable CPU remain unproved.

[H600](present-pacing-owned-run-local.600.md) separates actual-input constructor
CPU: 67 selected state/layout/uniform inputs, warmed reserved scratch, both
orders, same original capture. Mean reduction bounds are 0.36–0.66us/run;
four original pixel/query/lifetime controls and independent raw verification
pass. Worst-individual-sham screening fails; this is not a default-live
ms/frame estimate. Keep this mechanism priced locally and resolve H596's
larger cache-miss/final-queue ownership overlap next.

[H599](present-pacing-owned-run-live.599.md) completes the owned-run live contrast.
All four Replay comparisons favor the candidate, but conservative reductions
1.262/0.860ms/frame remain below OFF variation 1.389ms/frame. Whole-window pacing
passes 3% nonregression, not repeated improvement; Encode CPU fails nonregression.
Five valid exits, eight effect controls, independent raw arithmetic and seventy
restoration hashes pass. No production promotion or removable amount is certified.
Use operation-local construction costs next, not an extension of these five runs.

[H598](present-pacing-owned-run-warm-replay.598.md) records reference iteration1
output mismatch in same-Device repeated capture before candidate execution.
It supplies no warmed CPU estimate; H599 is a separately frozen live experiment.

[H597](present-pacing-owned-run-construction.597.md) qualifies a private
final-location owned-run constructor with exhaustive native traces and eight
effect controls. It avoids shared-header ODR drift and keeps resource ownership.
Generated hot-state copies fall from three to one, but default-initialization
zero-fill remains. Measure net CPU before live transfer; no removal amount or
production promotion is established.

[H596](present-pacing-replay-cpu-owner.596.md) completes two fixed live sampled
CPU diagnostics and independent raw verification. Cache miss and owned-run
construction dominate compatibility preparation; final queue batch remains
large and overlaps cache's bounds. These are sampled totals, not extrapolated
per-frame savings. The canonical binary copies the 8,112-byte hot state three
times through constructor/local-run/vector storage. Test final-location owned
construction next; no product change or removal amount is yet certified.

[H595](present-pacing-cache-clock.595.md) closes the fixed cache-clock contrast.
Both ON Replay CPU bounds are below adjacent OFFs, but conservative reductions
0.590/0.888ms/frame do not exceed OFF variation 1.210ms/frame. All five runs,
eight effect controls and 70 restoration hash checks pass. Pacing worsens in
run order, including OFFs; no causal FPS gain/regression or removable CPU amount
is certified. Attribute the large cache/rebuild/owned-run body locally next.

[H594](present-pacing-replay-operations.594.md) splits H593's resolved envelope:
compatibility draw preparation is the largest exclusive elapsed operation at
11.819/12.700ms/frame; sparse dispatch is 1.450/1.592ms and binding attachment
0.762/0.832ms. Two fixed live runs and independent conservation/restoration
checks pass. The measured cache function retains clocks before the disabled
counter gate. Price that diagnostic cost, then split cache rebuilding from
owned-run construction; elapsed is not yet removable CPU or an FPS benefit.

[H593](present-pacing-replay-partition.593.md) locates the largest exclusive
late Replay elapsed phase: compatibility resolved-record execution, excluding
queue batch, is 15.901/17.290ms/frame. Planning is 1.110/1.192ms and cleanup
0.194/0.210ms. Live Direct resolved calls are below 1%, unlike cold capture's
199/260; do not transfer that route mix. Both fixed live episodes and independent
raw/restoration checks pass. Next split sparse state versus draw preparation;
exclusive elapsed is not yet pure CPU or removable work.

[H592](present-pacing-live-batch.592.md) measures two default-live episodes.
O1's ordinary compatibility CPU upper is 9.620ms/frame versus Replay
29.410–29.539ms/frame; at least 19.790ms/frame lies outside that envelope.
Actual append upper is 3.610ms/frame; Tape calls are zero in both episodes.
Exact controls, raw verification and restoration pass. Attribute the Replay
complement next; these are instrumented bounds, not removable CPU or FPS gain.

[H591](present-pacing-append-tail.591.md) identifies the approximately6ms
append residual as Tape verification, not default rendering. Actual append is
3.09–4.12ms CPU per fixed captured input; ordinary route has zero verifier calls.
Four exact controls and independent raw checks pass. Together with H588's tiny
warmed reserve, this rejects the two apparent large compatibility-removal
budgets. Production live operation attribution remains open.

[H590](present-pacing-append-nesting.590.md) corrects
[H589](present-pacing-batch-residual.589.md)'s scope: its9.26–10.33ms includes
post-callback `mutateWritingPayload` settlement and observation, not just the
append loop. Initial queue acquisition and scratch cleanup remain small.

[H588](present-pacing-reserve-recurrence.588.md) rejects recurring ordinary
reserve as the major late CPU owner. Two fixed live episodes pass with complete
restoration; independent raw verification reproduces CPU/storage arithmetic.
O1's conservative full-scene reserve upper is 0.146ms/frame against near-peer
Replay/Encode at about30ms/frame. O0's slow start remains explicitly bounded.
Next attribute H585's compatibility-batch residual to CPU versus wait; no
headroom policy, FPS saving or unique Encode bottleneck follows.

[H587](present-pacing-reserve-live-failure.587.md) is a rejected diagnostic run,
not a product slowdown: the inline observer reached abort and lost destructor-
exported rows. Cleanup/restoration are verified; the second run was not started.
H588 replaces fatal/full-detail collection with durable counters and bounded
growth rows; native fault tests pass. H588 subsequently closes the warmed
ordinary-reserve recurrence question with exact-image and live controls.

[H586](present-pacing-reserve-owner.586.md) identifies storage growth as the
fixed-input reserve CPU owner. Two largest events relocate large state/layout
arrays; uniform reservation is not their cause. All four original controls
pass. Because these are fresh-process replays and normal clear preserves vector
capacity, the next gate is recurrence in warmed live late Slots, not a new
headroom policy or claimed 5ms/frame saving. H585's batch residual stays open.

[H585](present-pacing-replay-census.585.md) qualifies the actual fixed STALKER
Replay route: 1,154 Direct and 2,828 compatibility draws; 51 capacity fallbacks,
no rotations or unbatched compatibility draws. Reserve exceeds append in both
parent-only runs; the approximately 9.4ms batch residual remains unattributed.
GT2's uniform-dominant fraction does not transfer. Next separate actual storage
growth/rehash from no-growth reserve and batch preparation/wait; no CPU-saving
or policy-promotion claim follows from observer-inclusive elapsed scopes.

[H584](present-pacing-sparse-cpu.584.md) closes the sparse whole-policy CPU
comparison with amended selected-queue identity qualification. Encode falls
against both adjacent OFFs, but worst-adjacent lower reductions 0.179/0.349
ms/frame fail the 1.446ms/frame variation screen. Default Replay/Encode are
near peers; both ONs have larger Replay CPU. Whole-window pacing nonregression
passes, not repeated improvement. No promotion or global-owner completion.
Review existing fixed-input Replay-to-Slot/Encode attribution next, rather
than extending broad FPS populations or assuming a permanent Encode bottleneck.

[H583](present-pacing-unobserved-transfer.583.md) completes same-image,
observer-free whole-policy transfer: both ON runs meet finite 3% nonregression,
not repeated FPS improvement. H582's local CPU contrast is not an all-ON CPU
estimate. H584 supplies the subsequent sparse whole-thread comparison; neither
result supports a permanent Encode-only bottleneck or more broad FPS sampling.

[H582](present-pacing-paired-source.582.md) closes the bounded paired experiment:
58/56 treatment pairs give 1.333/1.266 ms/source CPU contrasts and pass both
predeclared sham/endpoint screens. Four live runs and raw verification pass.
Whole-run pacing does not demonstrate improvement; Replay and Encode remain
near peers. Next gate is observer-free all-OFF/all-ON transfer, not another
unstratified test for a local effect. No default promotion or global-owner claim.

[H581](present-pacing-source-shape.581.md) narrows the comparison unit:
Draw-plus-Clear sources account for 73.1% of observed source CPU, with 120
eligible sources in 121 frames. Encode/Replay bounds overlap, so a fixed global
ordering remains unproved. Pairing consecutive eligible sources gives a much
smaller baseline sharp-null assignment variance; execute that bounded design
before claiming live removal. The 73.1% cost itself is not removable work.

[H580](present-pacing-foreign-owner-live.580.md) now covers actual foreign
invocations: no main-dispatch or shared-event body overlaps the selected live
window. Encode narrowly leads Replay (29.697–29.944 vs 29.074–29.472 ms/frame)
over 116 frames, with four of five component bounds overlapping. Do not repeat
foreign-body absence as an open question for this episode. Observer-free
transfer, lifecycle tails and live removable cost remain unproved.

[H579](present-pacing-foreign-owner-preflight.579.md) prepares missing
foreign-owner observations: actual main dispatch and shared-event callbacks
retain request identity; native negative tests and original GPU/query/lifetime
OFF/ON pass. Zero requests in standalone replay do not bound the live scene.
Compiler per-job destruction is already inside the worker scope; thread-wrapper
and archive capture tails remain distinct coverage gaps. No new timing claim.

[H575/H576](present-pacing-clear-crossover-preflight.576.md) add seven exact
original-input controls and a private immutable source-policy image. OFF/ON
and even/odd policies pass, but all 65 opportunities occur in source5; parity
CPU averages would be biased. No new timing claim.
[H578](present-pacing-clear-default-route.578.md) verifies both Tape and DCE
are OFF on the real default Device; four ordinary-queue controls pass. The
failed forced-DCE H577 fixture is not a default-path measurement gate.
Content-independent timing and foreign callback coverage remain open;
H574 is not extended.

[H574](present-pacing-current-clear-live.574.md) closes the twelve-run live
comparison with valid exits/restoration and 108 self/72 scene raw checks,
not a qualified saving. Both Encode reduction lower bounds remain below
2.605 ms/frame A/A noise; OFF forward improvement is not repeated in reverse.
Only 3/6 whole-window observer controls meet ±3% pacing equivalence. Encode
leads Replay in all four observed canonical whole windows and trails it in
both candidates, but global/default ownership and removable budget remain
open. Preserve H573's local result; do not promote or extend this population.

[H573](present-pacing-current-clear-self.573.md) confirms current-image local
Clear-fusion savings without product supply tracing: 2.291 ms paired-median
source-5 full Encode iteration reduction, all eight pairs positive versus
0.249 ms maximum A/A. Seven-source reduction is 2.5885 ms versus 0.321 ms A/A.
All 28 original-input oracles and 252 raw Encode-pair calculations pass.
This replaces the rejected H572 supply-trace population, not H571's live
owner evidence. Current live ms/frame, observer-free transfer and global
ownership scope remain open; do not import H559's old live savings.

[H571](present-pacing-default-owner.571.md) provides same-episode default-policy
diagnostic ownership without the dormant Clear candidate or failure logging.
Across 128 completed frames, Encode is 29.390–29.727 and Replay 28.144–28.522
ms/frame; renderer roots are bounded by 17.502 and every other observed body
by 0.6945. Encode separates in the full window and two of five components.
All 15 bodies have finite bounds, original-capture checks pass, and live exit
and restoration pass. Observer-free transfer, foreign callbacks/closure scope
and current-image removable cost are still unqualified; no FPS claim follows.

[H570](present-pacing-shader-count-repair.570.md) identifies the observed
`font2` failure as a Unix ps_1_1 operand-count bug: TEX consumes the following
MAD opcode and skips END. The narrow creation-scanner repair passes actual
cached-bytecode, native regression and original-capture GPU exact checks.
The new live episode exits 0 and closes all 15 observed worker-body bounds,
including the eight formerly missing compiler endpoints. This is correctness
and coverage evidence; it does not qualify a new CPU maximum or FPS.

[H566](present-pacing-owned-worker-preflight.566.md) adds qualified private
self checkpoints for remaining worker bodies; original-input GPU/readback
OFF/ON passes. [H567](present-pacing-owned-worker-live.567.md) reaches all
seven knots but exits 1 after a `font2` shader-create error. Its 15-instance
trace exposes eight unclosed compile-worker bounds. The run remains rejected,
shared staging is restored, and no new default-owner/FPS conclusion follows.

[H564](present-pacing-owner-live.564.md) restores current live root attribution:
the Primary renderer upper is 17.866 ms/frame, below Replay 28.473–28.880 and
Encode 29.030–29.261 throughout the full window and its components. Encode's
small full-window lead is not a universal ordering. The attempted aggregate
residual bound is **rejected** by [H565](present-pacing-process-freshness.565.md):
getrusage inside its own process also delivers delayed active-thread CPU.
Remaining owned-worker self endpoints, default transfer and a qualified
global maximum remain open; no process-derived savings budget is admitted.

[H562](present-pacing-clock-bridge.562.md) closes the current PE32/PE64
clock-domain prerequisite without enlarging the 100 ns allowance.
[H563](present-pacing-owner-domains.563.md) privately integrates explicit clock
mapping and process CPU into the root observer. Both actual-object/epoch
fixtures and 17 negative cases pass. Current renderer Primary and residual
worker bounds still require a new live ownership episode; no new removal or
FPS verdict follows from these infrastructure tests.

[H560](present-pacing-owner-clock-rejection.560.md) supplies no valid current
Primary renderer bound: the first root checkpoint rejects a mixed
continuous/absolute clock-domain contract. The failed run remains preserved.
[H561](present-pacing-process-accounting.561.md) validates finite process-CPU
accounting on ARM64/Rosetta using self `proc_pidinfo` (16 cases / 160 samples).
H565 later rejects the aggregate endpoint premise, so use qualified worker
self/lifecycle evidence rather than deriving a residual upper from H561 totals.

[H559](present-pacing-clear-live.559.md) supplies positive matched live Encode
economy: 1.579–2.010 ms/frame AB and 1.614–2.072 ms/frame BA, both lower bounds
above 1.189 ms A/A. The baseline Replay/Encode costs are close; Replay exceeds
Encode in both candidate runs. Mixed Primary remains larger but includes game
and Wine. Current renderer-only Primary/other-worker bounds and common-observer
transfer remain open. Drift/host activity excludes an FPS or promotion claim.

[H557](present-pacing-clear-net-economy.557.md) confirms positive local net
economy after private failure repair: whole source-5 Encode saves 1.9955 ms
paired median (8/8 positive; A/A maximum 0.402 ms). All 28 original oracles pass.
[H556](present-pacing-clear-failure-drain.556.md) retains session ownership
through an already-submitted prefix's drain.
[H558](present-pacing-clear-failure-boundary.558.md) closes both deferred-entry
bypasses in private images, with 32 coordinator cases and two original captures.
Price live transfer next; neither default maximum nor live removable ms/frame
is closed. H557's timing does not automatically apply to H558's changed image.

[H554](present-pacing-clear-failure-relay.554.md) fixes H553's finite failure
population in private images: explicit terminal status replaces inline success,
including after prior unsubmitted Clear work. All 16 coordinator fixtures and
two original-capture oracles pass. Already-submitted effects, live wake/reset,
success-path cost and default-live CPU removal remain unqualified.

[H553](present-pacing-clear-queue-failure.553.md) turns the candidate failure
concern into a real-coordinator counterexample: failed Clear opening yields
successful source completion without GPU work. H552's non-failing local CPU
result stands, but the private candidate cannot be promoted unchanged. An
explicit failed-versus-inline outcome and prior-carrier settlement are next.

[H552](present-pacing-clear-economy.552.md) establishes a local Clear-fusion
Encode saving: paired median 1.8055 ms/source 5, all eight pairs positive,
versus maximum A/A difference 0.482 ms. [H551](present-pacing-clear-backed-null.551.md)
closes the backed-NullRt projection gap and witnesses 65/65 fusions. All 28
timing executions preserve original pixels/source/query/lifetime checks.
This does not price default-live removal or FPS; queue terminal settlement,
ordinary query qualification and live ownership/transfer remain open.

[H549/H550](present-pacing-clear-caller.549.md) validates a private actual
Clear-fusion caller, including independent attachment readbacks and original
STALKER capture. Its useful coverage is only 2/65: 63 depth Clears encounter
texture-backed NullRt excluded by the initial adapter. Resolve that semantic
distinction before claiming the measured standalone-Clear cost is removable.

[H548](present-pacing-clear-adapter.548.md) adds real-resource fact projection
and closes vertex-sampler/physical-extent coverage gaps. Native tests pass;
the encoder still uses its original policy. Caller failure handling, independent
attachment pixels and removal economics remain unproven.

[H547](present-pacing-clear-model.547.md) supplies a shared Clear-fusion
candidate predicate and bounded aspect/load/store refinement, passing native
arm64 and x86_64 tests. It excludes unsupported first-use and pending-discard
cases rather than weakening the serial reference. No runtime binding, GPU
promotion or removal-budget claim follows yet.

[H546](present-pacing-clear-feasibility.546.md) confirms the pending-Clear
owner directly: 65 calls cost 2.071–2.243 ms/captured source. All 65 satisfy
the observed necessary fusion-shape conditions in an independent unpriced run.
This selects a concrete Clear/Draw fusion refinement; it does not certify
safety, saved CPU or the largest default-live owner.

[H545](present-pacing-pass-outer.545.md) narrows the outer-pass residual to
the pre-close gap after 63 depth-only and two color-only Clears (about 2.0 ms
per captured source). Strict full attachment-key equality is a concrete
fusion limitation to test, not yet safely removable CPU. Encoder close,
labels and argbuf-open are smaller; the 13.2–13.6 ms whole draw loop and
canonical live attribution remain open.

[H544](present-pacing-pass-owner.544.md) narrows the pass owner: nested
store-proof traversal is only 0.476–0.542 ms per observed source 5; encoder
creation is 2.501–2.571 ms and outer transition work remains 3.167–3.239 ms.
Same-run identity/endpoint joins and all five original oracles pass. These
observer-inclusive scopes select further investigation, not removable CPU.

[H543](present-pacing-run-milestones.543.md) separates source 5's whole draw
loop (14.268–14.943 ms) from pass entry (6.600–6.816 ms), with no nested draw
clocks. Both remain observer-inclusive captured-source CPU. Pass-start indices
are the next preparation/driver discriminator, not a proven removable budget;
canonical live ownership remains open.

[H542](present-pacing-encode-command-owner.542.md) places approximately 94%
of source 5's command-observed body in 526 DrawRuns, with no single dominant
command. An independent unpriced transcript confirms 3,125 draws, including
2,774 in 175 multi-draw runs. This selects run preparation versus draw-loop
work; it does not price removal or close canonical live attribution.

[H541](present-pacing-encode-source-selection.541.md) separates existing fresh-
device Encode evidence: median 29.358 ms cold-inclusive prefetch, 1.695 ms
planning/setup and 27.173 ms command body per captured interval. Source 5 owns
74.16% of body CPU across the 18 baseline traces. These are not default-frame
or removable costs. A same-device probe violates the documented V4 precondition
and fails original readbacks; both planned warm timing traces remain unrun.

[H540](present-pacing-prefix-live-result.540.md) closes H538's live attempt:
six fixed runs complete, with authenticated delayed-server restoration and
108 independently recomputed thread intervals. Prefix removal does not pass
the amended live Replay economy screen. Encode is roughly 29.4–31.7 ms/frame
and exceeds Replay in all six diagnostic intervals; residual-observer and
exclusive Primary attribution still prevent a largest-default-owner claim.
No positive canonical removal budget or default promotion is established.

[H539](present-pacing-stalker-query-consumers.539.md) verifies installed-game
result-dependent work: five occlusion consumers test unsigned >4, two test
nonzero, through a shared cooperative FLUSH polling helper. The default
primitive-count mismatch is already known; H408 did not establish visibility
ON alone as the FPS fix. Next evidence needs the helper's caller identity,
not just GetData's shared call site. No default CPU/removal claim follows.

[H538](present-pacing-prefix-live-preflight.538.md) closes the post-reboot
offline preflight: both matched images pass original pixel/source/query checks;
199 suffix prepares skip 75,300 old commands and inspect 392 new commands.
The launch symptom is resolved, not causally diagnosed. H540 later completes
the live comparison with a negative economy screen; default transfer remains open.

[H537](present-pacing-prefix-economy.537.md) measures a net same-input suffix-
prepare effect: 3.379 ms paired-median planned Replay CPU reduction, all eight
pairs positive and all 30 original oracles exact. Native/TLA evidence and this
offline screen do not establish live removal or the largest default CPU owner.
Supplemental execution witnessing later passes in H538 after reboot.

[H536](present-pacing-prefix-prepare.536.md) advances prepare-prefix removal to
a private capability-gated candidate with native and bounded-model evidence.
Normal builds retain full validation. H537 subsequently qualifies same-input
replay and the offline CPU screen; canonical live transfer remains open.

[H535](present-pacing-removal-scope.535.md) selects ordinary chunk replay over
StateBlock call count and identifies repeated prepare-prefix scans. Native
counterexamples rule out topology-only reuse; current-TU assembler tests support
a bounded transaction-local prefix hypothesis. Build ownership is thread-pinned,
not an uninterrupted queue lock, so capability/refinement and CPU removal
evidence remain required. No runtime optimization or default saving yet.

[H534](present-pacing-boundary-comparison.534.md) passes the predeclared finite
boundary-control/all-root screen in four fixed-order runs. Primary's diagnostic
D3D9-root upper CPU is 19.85/20.28 ms/frame, below both approximately 32–33 ms
workers in both episodes and all components. A2/B2 Primary still differs by
1.79 ms/frame; a passed screen is not zero observer cost. Common instrumentation,
host load, canonical/global ownership and actual removable CPU remain unresolved.

[H532–H533](present-pacing-boundary-control.533.md) close the proposed native
suspension route with a counterexample, not a live promotion: a sleeper's held
counter exceeds the frozen 2 us allowance. A boundary-only PE self-report
control passes PE32/PE64 fixtures while leaving Draw/Set/getter entries intact.
Matched all-root-observer interference, canonical transfer and actual removable
CPU remain open; the earlier suspension fixture results below do not waive them.

[H530–H531](present-pacing-native-endpoint.531.md) reach actual Wine targets
with a native-thread endpoint observer while preserving canonical renderer
images. Guest-only suspension cannot cover production native Replay/Encode.
Native and attached console fixtures pass; multi-worker safety and matched
live interference remain gates. No default-owner or removal result follows.

[H526–H529](present-pacing-quiescent-counter.529.md) test a canonical-binary
endpoint route with no per-call hooks. External Wine suspension passes PE32/
PE64 fixture consistency checks after Mach access is denied. This is not
freshness proof or default attribution: matched scene coherence and pause
disturbance remain unresolved. H525's CPU/removal limitations still apply.

[H525](present-pacing-root-wall-live.525.md) bounds inclusive Primary renderer
CPU below both dedicated workers in one same-window diagnostic episode:
≤19.69 versus Replay 31.46–31.84 / Encode 31.77–32.00 ms/observed completed
frame. The bound includes waits, callbacks and 200 ns per root uncertainty;
all five components agree. This qualifies the method's utility, not default
ownership or removable cost. Next run a matched default-interference gate;
do not convert whole-thread CPU into a savings budget.

[H523–H524](present-pacing-root-wall-abi.524.md) close actual-object ingress and
activation prerequisites for the root-wall diagnostic. Twenty thousand getters
add no self-CPU queries; nested callback Present is not double-counted. This is
not a largest-owner or removal result. Next test whether a wait-inclusive
whole-root envelope is tight enough in the same late scene as the workers.

[H522](present-pacing-root-wall-preflight.522.md) finds an inexpensive elapsed
clock candidate and tests disjoint outer-root accounting. Its proposed CPU
upper envelope includes waits and callbacks; it is not exact CPU pricing.
Rosetta/PE console clock checks and 20 reducer cases per native architecture
pass, but runtime coverage, short-interval uncertainty and default-transfer
remain unqualified. No largest-owner or removable-cost conclusion follows.

[H521](present-pacing-primary-gap-live.521.md) completes the combined diagnostic
episode after [H520](present-pacing-primary-activation.520.md) qualifies current
epoch activation. App Primary totals 56.75–57.85 ms/observed completed frame;
Replay and Encode overlap around 32 ms. Post-Present gaps contribute only
0.28 ms/frame and cannot isolate renderer ownership. Close this narrow subset
route; default transfer, interleaved app-thread attribution and actual removable
work remain unresolved. No production optimization or FPS claim follows.

[H519](present-pacing-primary-gap-epoch.519.md) rejects the first combined live
episode before Primary arming: observed image epoch 2 violates its fixed epoch
1 prerequisite. No gap/CPU result. Preserve the failed contract; implement and
test an epoch-qualified activation receipt rather than merely accepting any
reload. Mixed Primary and default-qualified removal remain unresolved.

[H518](present-pacing-primary-gap-abi.518.md) closes actual PE32/PE64 observer
integration: canonical-identical baseline relinks, four object runs and
16 parser tests pass. Nested application callback Present is not misclassified
as non-root work. No live STALKER run or default attribution occurs; the next
gate is same-episode Primary/worker coverage and observer interference.

[H517](present-pacing-primary-gap-preflight.517.md) adds a tested, isolated
post-Present root-gap prototype; it is not yet a live observer. Existing cadence
markers precede remaining PE work. The new envelope would still contain hook
cost and only a subset of non-root work, so it cannot yet exclude Primary as
the largest dxmt9 owner. Runtime coverage and default transfer remain open.

[H516](present-pacing-source-self-thin.516.md) thins Replay reporting 256-fold
without extrapolating sampled duration. Whole-worker cursor bounds overlap at
roughly 32 ms/observed frame in the late window and all five subintervals.
H515's small ordering is not robust; stop chasing that rank. Mixed Primary,
default OFF/ON transfer and a semantic-preserving removal mechanism remain.

[H515](present-pacing-source-self-live.515.md) obtains valid live self-counter
brackets: diagnostic Replay 32.1147--32.1453 versus Encode 31.4408--31.5835 ms
per observed completed frame. This is not a default ranking: Replay emits
126,065 pairs, nearly half StateBlock, so reporting is not low frequency.
Next reduce endpoint frequency while retaining whole-thread cumulative bounds.
Primary, default transfer and removable amount remain unresolved.

[H514](present-pacing-source-self-report.514.md) passes the original late-input
OFF/ON observer oracle and bounded endpoint/parser tests. Exact publication
receipts cover seven captured Encode sources, but raw-zero compatibility
metadata and the synchronous provider do not establish a live Replay join.
Temporary product hooks and canonical images are restored. No default CPU
ranking, removal amount or performance promotion follows.

[H513](present-pacing-self-counter-boundary.513.md) passes bounded current-host
self-query qualification on ARM64/Rosetta. H512 applies to external snapshots,
not automatically to the supply trace's calling-thread queries. Use coarse
identity-qualified self reports for the next accounting boundary; neither
Primary root attribution nor default-qualified removal is established yet.

[H512](present-pacing-cpu-counter-freshness.512.md) corrects the premise behind
H509--H511 CPU bounds: external thread counters can report 10 ms of CPU increase
inside 0.34 ms of wall time. Three counterexamples also exist in H509 B1.
Reported differences remain descriptive, but narrow true-CPU enclosures,
the Encode/Replay ordering and Primary exclusion threshold are not qualified.
The current gate is accounting freshness, not another long ownership run.

[H511](present-pacing-guest-attribution-feasibility.511.md) closes the immediate
observer feasibility check without another live run: a recovered game-module
PC and truncated raw stack do not establish non-renderer CPU. Frozen PE code
contains private-IUnknown callback calls; the existing sampler measures PC
residency, not running-CPU ancestry. The required >20.775 ms/frame Primary
exclusion bound and default-qualified removal amount remain unproved.

[H510](present-pacing-current-thread-owner.510.md) closes current-process
identity for the two large unnamed workers with fresh guest/TEB/native joins:
xrCPU_Pipe and WinMM start provenance, not dedicated dxmt9 workers. This narrows
the ownership gap without proving all their CPU is outside renderer callbacks.
Primary's renderer share still needs separation; the leading dedicated
workers differ by only 0.206--0.223 ms/frame in this one interval.

[H509](present-pacing-validation-live.509.md) does not establish H508's live
economy. Four fixed A/B/B/A episodes pass spatial/worker enclosure checks,
but 0.505 ms/frame mean Replay saving is smaller than 1.090 ms baseline
variation. Tail FPS reverses direction between pairs; Primary/unnamed-worker
ownership remains unresolved. Product sources and shared images stay restored.
The catalogue's administrative `missing_capture` failures remain disclosed.

[H508](present-pacing-validation-authority.508.md) establishes a local removal
candidate rather than another rejected cache: transaction-local admission
authority eliminates 197 reserve scans, with 60 original oracles passing.
Validator-timer-OFF planned replay saves 2.839 ms versus a 1.165 ms A/A gate,
7/8 positive pairs. Runtime is restored pending live default transfer; this
does not resolve mixed Primary/native ownership or rank the global bottleneck.

[H507](present-pacing-validation-callers.507.md) separates product checks from
V4 qualification: all 646 observed full-Slot scans come from Direct admission,
assembler reserve and prepare. 197 adjacent admission/reserve pairs repeat
75,300 command visits. Reusing only that uninterrupted admission authority is
a narrower next candidate than cross-transaction prefix caching. No runtime
change or time saving is accepted; live-default owner ranking remains open.

[H506](present-pacing-production-query-diagnostic.506.md) closes H505's private
same-backend replay gate. All seven source cuts, nine Issues, five polls, four
query results and both original images match in four fresh processes. Hardware
occlusion qualification remains false. Two bounded CPU traces show 29–30 ms
Encode body and 9.5–9.8 ms full-Slot validation, but replay includes V4 hooks
and prefetch includes cold-cache work. Separate those costs before a product
removal comparison; this does not establish the largest live default-tail owner.

[H505](present-pacing-closed-query-history.505.md) closes the ordinal-loss capture
failure without relaxing identity checks. The late-camera input authenticates
7 sources / 3,982 draws, but same-backend replay rejects `query-oracle-unsupported`:
capture uses the production occlusion counter while the provider requires
hardware visibility. Enabling Metal occlusion instead breaks exact source
publication at the first occlusion-bearing raw. Separate a source-bound default
query diagnostic from GPU-query qualification before collecting CPU timings.
Original-image replay and default-tail ownership/removal remain open.

[H504](present-pacing-closed-query-ordinal.504.md) isolates a concrete capture
schema gap: the closed V2 query descriptor loses the pre-cut absolute Issue
ordinal. The same BEGIN has cycle 509 on both sides but canonical ordinal 1
against real Unix ordinal 1017. A native counterexample reproduces the failure;
neither provenance equality nor scene selection was relaxed. H505 preserves
explicit closed-query history and maps it to a fresh local owner. Its published
input supersedes this capture blocker, not the default-tail CPU ownership gap.

[H502](present-pacing-scene-request-preflight.502.md) exercises the late
camera-envelope request twice. Request/arm succeeds; final V4 identity
attachment rejects `InvalidCoordinator`. Runtime restoration passes, but no
bundle is published. Isolate coordinator/attempt provenance before another live
retry; source/image joins and original replay remain required for the CPU goal.

[H501](present-pacing-arm-request.501.md) implements the external one-shot
capture request and passes native refinement plus x86/x64 controlled Wine
transport. No game/FPS/GPU run occurred. Next join a real scene envelope to
accepted request, armed token/epoch/queue, published sources and original images;
the selector alone does not close the default-tail ownership or transfer gap.

[H500](present-pacing-tail-input-scope.500.md) closes a scope question, not the
CPU goal: H481's complete input predates its 102–110-second target window,
with publication observed no later than 94.035 s after the bracketed initial
running transition. Skip-count selection is not an engine/camera identity gate.
Next qualify one-shot late capture selection and source provenance; do not
use additional H481 micro-optimizations as a substitute for default-tail work.

[H499](present-pacing-vertex-shadow.499.md) completes H498's combined experiment.
Actual pushes fall 3,497 -> 138 with 45 original oracles passing. Incremental
body saving is 0.370 ms, positive 8/8, but below the 0.523 ms A/A gate; combined
default saving is 0.301 ms, positive 5/8. Runtime restored. This is a small
directional signal, not zero-benefit proof, an upper bound or default-tail
ownership. Isolated binding reuse has not established the large missing cost.

[H498](present-pacing-volatile-multiplicity.498.md) narrows the next work to
native base vertex plus physical-encoder-local VS immediate reuse. In the same
original input, exact repeated pushes increase from 166 to 3,359 of 3,497,
while both source/output oracles match. Actual pushes remain 3,497: no elision,
CPU saving or default promotion yet. Historical full-payload materialization
is 0.511 ms observer-inclusive elapsed, not a demonstrated dominant CPU owner.

[H497](present-pacing-command-raster-reuse.497.md) implements H496's bounded
raster-target reuse candidate. All 30 original oracles and bounded native
refinement pass, but paired command-body saving is −0.177 ms versus 0.787 ms
A/A noise (3/8 positive). Runtime restored; this cache does not establish a
material saving. Next distinguish complete representation work from driver
encoding, while retaining the unresolved default mixed-Primary boundary.

[H496](present-pacing-encode-callsite.496.md) verifies the frozen image's call
instructions and import targets. Five of twelve other-encodeChunk observations
are at encodeDraw calls with missing callee ancestry, not proven outer-loop
CPU. The next bounded candidate is command-local raster-target metadata reuse;
uniform representation reduction is wider. No default owner or saving is priced.

[H495](present-pacing-binding-counter-gate.495.md) removes repeated enable
decisions and zero-valued atomics from three binding counter families. Exact
counter tests and 30 original GPU oracles pass, but the 0.272 ms paired body
delta fails the 1.483 ms A/A noise gate. Retained diagnostic cleanup is not a
proven time-saving budget or explanation of the tail gap.

[H494](present-pacing-encode-cpu-stack.494.md) obtains 62 native Encode
Running/Timer-Fired observations on the original input. A binding counter
still executes ten enable checks when OFF, confirmed in release code. This
is an actual product candidate, unlike H493's capture-only saving. Active
signposts and cold prefetch prohibit treating the profile as default CPU prices.

[H493](present-pacing-v4-pending-lookup.493.md) removes repeated DrawRun locator
scans from capture qualification. All 30 original-input oracles pass; planned
replay saves a paired median 8.29 ms, exceeding the 2.20 ms A/A envelope in
8/8 positive pairs. The new approximately 42.94 ms scope still includes V4
verification and tracing. No default-game owner, FPS or removal budget follows.

[H492](present-pacing-validation-reuse.492.md) tests invocation-local uniform
validation reuse. All 30 original-input oracles pass, but its 0.312 ms validator
and 0.397 ms planned-replay paired medians fail the frozen A/A noise gates.
The runtime candidate is reverted. Do not call H491's whole envelope a proven
removal budget; capture-qualification overhead and default Primary attribution
remain unresolved.

[H491](present-pacing-slot-validation.491.md) measures 208 full-Slot checks and
84,587 successful command visits per original interval, with 3.17–3.39 ms of
observer-inclusive validator CPU. All three original GPU oracles pass. Checks
remain intact; prefix-witness safety, OFF savings and default-game transfer
remain unproved.

[H490](present-pacing-provider-stack.490.md) confirms capture/identity hooks
inside the observed planned-replay envelope, including command-emission record
lookup. Wall-stack counts are not CPU proportions. A distinct product-path
candidate is repeated complete Slot payload validation at admission/reserve/
prepare; no removal, savings or default-game dominance is yet established.

[H489](present-pacing-provider-phases.489.md) places about 94% of the observed
provider-call CPU inside planned replay, including V4 hooks. Raw preparation
is about 1.32 ms and settlement 1.37–1.47 ms against 51.1–51.6 ms planned replay.
All three original-input image oracles pass. This weakens raw setup/settlement
as the next removal target, but does not resolve capture-hook cost or global
default-game CPU ownership.

[H488](present-pacing-prepared-replay-cpu.488.md) measures the prepared caller,
which replays synchronously rather than through ReplayOffloadWorker. Its 92
observed replay calls cost about 52.1 ms CPU, inside 59.8 ms raw envelopes;
another 58.6–58.8 ms lies in original interval services/qualification, plus
48.6–50.8 ms readback/oracle CPU. These are harness-inclusive scopes, not a
default worker or removable budget. Separate V4 qualification from ordinary
replay before transferring this input's CPU prices to the real game.

[H487](present-pacing-draw-loop-scratch.487.md) removes real per-draw scratch
initialization in an archived candidate. All 30 original-input GPU oracles
pass; all 8 A/B pairs favor it, but the 0.907 ms paired body-CPU median is below
the 1.123 ms maximum A/A difference. The frozen gate fails; product source and
images are restored. This narrows one small redundant operation, not the
largest default owner or a qualified tail-removal budget.

[H486](present-pacing-normal-source-cost.486.md) reproduces the original Normal
input on x86/Rosetta and joins CPU to its three source cuts. The large source's
roughly 34 ms prefetch includes fresh-cache PSO builds, not a steady-state
removal budget. DrawRun owns 18.8 ms of its 20.1 ms command body in the command
diagnostic; planning is about 1.1 ms. Fine draw clocks perturb the scope, so
investigate repeated loop work without treating intrusive timings as savings.
Content/default-tail/global-owner and removable-cost gates remain open.

[H485](present-pacing-normal-provider.485.md) integrates retained Normal setup
and history restoration. Two fresh native Metal replays of H481 reproduce the
original source/output bytes exactly, with three source cuts and balanced
779-wrapper ownership. First-read content authority, representative default-tail
CPU attribution and removable cost remain open; this is not timing promotion.

[H484](present-pacing-normal-produced-setup.484.md) closes the missing DF24
parent's metadata admission with an explicit allocation-only request. The
same input plans 647 parents and 12 late aliases, with unchanged seeds and
semantic cuts; H485 supplies the subsequent retained-wrapper/provider evidence.

[H483](present-pacing-normal-setup-closure.483.md) narrows Normal integration
to 12 retained late wrappers, one unstaged produced DF24 parent, and independent
history restoration after actual setup completion. Production offline preflight
accepts the existing input; no Normal GPU replay or CPU savings are qualified.

[H482](present-pacing-normal-restore.482.md) closes the cold queue primitive for
seed-independent original touch/discard restoration. It is still disconnected
from policy qualification: all-owner setup, Normal provider admission and GPU
comparison remain before any same-input CPU-removal measurement.

[H481](present-pacing-normal-history-input.481.md) supplies authenticated
Normal history for all 16 defined surface generations. The same-input locator
now handles constant-range headers and null unbinds. Read-first contents,
original touch/discard restoration and physical aliases remain the replay gate;
successful publication does not explain H480 failures or establish CPU savings.

[H480](present-pacing-normal-history.480.md) adds retained, generation-qualified
arm history and authenticated publication/inspection. Native tests pass, but
the actual-game run publishes no bundle: old occlusion cycles fail admission
and 16 later intervals fail the identity finish without detailed attribution.
The next gate is stage-qualified finish diagnosis with bounded attempts, not
a policy timing run. Defined contents and Normal replay remain unproven.

[H479](present-pacing-normal-capture.479.md) supplies the first authenticated
Normal-action STALKER interval and original source/output readbacks. The queue
remains Normal at construction/arm/freeze, revision zero; no Exact qualification
gate is relaxed. Next is a bounded attachment-history checkpoint (touched,
post-Present discard, defined contents) and Normal replay/original comparison,
not an FPS claim or another policy timing matrix.

[H478](present-pacing-default-capture-scope.478.md) establishes a prerequisite
to the proposed fixed-input follow-up: current capture/replay qualification is
intentionally stable-exact, not normal attachment policy. Late arming does not
remove capture-policy prehistory, PE cache changes or Present reservation cuts.
A default-tail removal experiment needs an explicit default-policy authority;
another existing-profile capture cannot fill that gap or isolate Primary CPU.

[H477](present-pacing-root-game.477.md) closes actual-game whole-root CPU
attribution after four normal runs: the observer/default transfer and camera
gates fail. In the first default tail observation Replay costs 28.713–28.909
and Encode 29.579–29.778 ms/completed frame, but Primary's renderer portion
remains unseparated and not all native threads are owner-qualified. Diagnostic
root CPU of 22.0–22.5 ms/frame is not a default bound or a removable amount.
Do not repeat this live all-call-clock method or infer savings from elapsed-time
windows with different camera progress. Identity-matched late-input removal
evidence remains necessary; no globally largest owner, FPS gain or promotion
is established.

[H455](present-pacing-completed-workload-visibility.455.md) breaks the equal-work
assumption at three late observed camera states: default native draw counts
are 1.60–1.75x D9VK's. A separate OFF/real-visibility ON pair shows lower late
submission counts and named Encode CPU (24.41 to 22.26–22.56 ms/frame;
29.74 to 25.03–25.15 in the last qualified CPU region). These are sparse samples
and diagnostic effects, not a full-window census, FPS gain or promotion.
Reversing execution order reproduces the last region's Encode CPU reduction
(30.96 to 24.85–24.92 ms/frame), with visible same-policy drift; the reverse
80–95 s CPU region is rejected for an unnamed handle's counter regression.
Visibility-dependent workload now precedes another small-copy candidate;
overall dxmt9 ownership and semantics-qualified removal remain open.

[H454](present-pacing-backend-cpu-camera.454.md) finds a substantial worker CPU
demand difference in a camera-qualified late scene: Replay 21.35 and Encode
22.26 ms/rendered frame, versus D9VK cs 4.24–4.27 and submit 2.75–2.76 ms.
These are differently divided responsibilities, not a removable-cost estimate.
Default visibility semantics still differ; count completed game submissions
before attributing the gap to per-draw overhead or another small copy.

[H453](present-pacing-api-residency.453.md) obtains matched native CPU and
reload-safe API residency. Late diagnostic API-body bounds are 0.270–0.289 core
against about 0.51 core per worker, but scope overhead and OFF/ON perturbation
remain unqualified. Do not generalize this to default-path PE CPU or removable
cost. The next useful discriminator is a matched backend CPU comparison, not
another unpriced small-copy candidate.

[H452](present-pacing-primary-clock-qualification.452.md) rules out using
`GetThreadTimes`/`QueryThreadCycleTime` to close Primary's CPU split on the actual
Wine runtime. Calibrated native CPU plus qualified non-overlapping API residency
can bound diagnostic API-body CPU as exercised in H453; boundary and observer
uncertainty still prevent a default-path owner verdict.

[H451](present-pacing-run-construction-removal.451.md) tests direct owned-run
construction on unchanged capture29: about 1 ms less body process CPU, or
1.53% ARM / 1.00% Rosetta median reduction. Within-architecture logical and route
checks pass; no new GPU/wild evidence or promotion. H450's copy-callsite sample
share is not a savings budget. The current tail's largest removable owner
remains unresolved, especially Primary's game/Wine/PE split.

Current STALKER default-path evidence: [H449](present-pacing-default-tail-cpu.449.md)
measures native thread CPU with product observers OFF;
[H450](present-pacing-default-tail-running.450.md) adds Running-only attribution.
Compatibility snapshot staging contains repeated large compiled copies and is
the next bounded removal candidate. Its 38.57% inclusive Replay sample share
is not an attainable saving or whole-pipeline budget. Largest observed GPU idle
gaps mostly precede submission; Primary's game/Wine/PE split remains unresolved.
Repaired-image [H448](present-pacing-l64-repaired-wild-abba.448.md) does not
establish a grouping benefit. Keep limit 0; measure removal on identical input
before another wild comparison or Encode-parallelization project.

[H429](present-pacing-query-proof-wild.429.md) measures H428 with observers OFF
on the default range-OFF Day route: +1.668% / -1.546% in reversed-order pairs,
against 2.484% same-image A/A variation. No FPS benefit is resolved. Five runs
pass; an earlier non-game capture is excluded. Keep the computation elision,
but do not promote ranges or assign H426's observed pass-entry time as savings.

[H428](present-pacing-query-proof-elision.428.md) implements the narrow H427
selector: omit store-proof walks only for query preservation with perf OFF.
Prepared-output fixtures pass 192 cases in each observer mode, and range OFF/ON
GPU query/pixel oracles pass. No wild/FPS result yet; perf ON retains the scans
and tape-only preservation remains unchanged.

[H427](present-pacing-pass-proof-audit.427.md) identifies a narrower code candidate:
query-preserved pass actions still compute store lookahead, while perf ON also
re-traverses color0/depth for a summary. Their costs are not yet separated.
Only the query-preservation predicate fixes both DontCare and late-store choices;
tape-only preservation must not inherit the shortcut. No runtime change.

[H426](present-pacing-drawrun-preparation.426.md) subdivides large-source DrawRun
residual: pass entry 22.466 ms and per-draw loop residual 17.018 ms dominate
47.538 ms across five bodies. Command binding-plan setup is 1.429 ms. These are
observer-inclusive, changed-input diagnostics; next separate pass continuation
from transitions and startRenderPass preparation. No policy/FPS promotion.

[H425](present-pacing-range-encode-body.425.md) splits five large range-source
bodies: 54.5% encodeDraw and 39.8% enclosing DrawRun preparation/pass residual;
per-record split and dispatch are small in that sample. The earlier trace
window does not reproduce H424's long waits, and costly sub-128-command sources
remain outside the detailed observer. Next decompose DrawRun residual and
select wait-linked sources without assuming fixed Present ordinals.

[H424](present-pacing-range-reuse-activation.424.md) confirms wild reuse (226/534
retained requests). Remaining same-prefix round trips total 5.385 ms in five
Presents; 15 other releases account for 110.226 ms. The five largest waits
overlap 89.968 ms of same-sequence CPU-active Encode bodies. Prioritize those
bodies over more ack tuning. Global Arena/Direct exclusion is confirmed, but
full query/polling tape route-cost isolation is unsupported; default stays OFF.

[H423](present-pacing-close-reuse-wild.423.md) completes a Day visual smoke and
observer-OFF reversed-order pairs on H422. Ranges ON remains 9.66% / 8.49%
slower; default OFF is retained. This is a whole-policy comparison, not isolated
reuse cost. Next count wild reuse/fallback activation and isolate the global
Arena/session exclusion from range-local costs on identical input.

[H422](present-pacing-close-reuse-integration.422.md) connects actual producer
ack reuse under queue + clean-dirty locks in the default-OFF range lane. Clean
requests return while Encode is paused; dirty requests still wake/wait. Explicit
reuse trace events and GPU query/pixel checks qualify activation. Wild visual
and observer-OFF performance validation remain the next gates, not a default
promotion or measured FPS recovery.

[H421](present-pacing-close-consumer-refinement.421.md) adds the candidate receipt
consumer and 46,656 bounded schedules. A concrete integration constraint remains:
`makeEncodeContext` consumes pending uniform-dirty state before a no-op close.
The consumer requires a side-effect-free-context guard, still not production
bound. No runtime wake elision or new performance result is claimed.

[H420](present-pacing-close-coordinator-owner.420.md) connects receipt
qualification to the actual queue-owned coordinator. Production-loop tests
observe issuance after real acknowledgement, then rejection at younger
publication and invalidation at work start. Every wake/ack remains intact;
next compose the consumer admission and temporal progress proof before elision.

[H419](present-pacing-close-witness-provenance.419.md) closes the candidate's
snapshot provenance gap: receipt issuance now performs the actual exact-head
FIFO ack, rather than accepting a historical ordinal. Real-helper fake-backend
tests cover close/reopen invalidation. Runtime coordinator ownership and wake
elision remain unimplemented; no new FPS claim or policy promotion.

[H418](present-pacing-close-outcome-oracle.418.md) reproduces actual redundant
pass closure on identical serialized input: retained mode closes once, then
returns `NoActivePass` 33 times, with exact samples/pixels in all transport and
replay modes. This justifies a reuse candidate, not its production safety or
FPS gain. Next bind the conditional admission value to trusted coordinator
epochs/work generations and prove invalidation before eliding any wake/ack.

[H417](present-pacing-session-release-cost.417.md) observes 535 consecutive
same-prefix retained ClosePass round trips in five active STALKER Presents,
7.621 ms total diagnostic elapsed. This confirms repeated transport, not a
safe early-return predicate or the full FPS deficit. OFF at the same Present
numbers has a different query population, so timing subtraction is invalid.
Next qualify pass-close witness reuse on identical input; policy stays OFF.

[H416](present-pacing-query-range-wild.416.md) confirms H415 activation in
STALKER, but observer-OFF pairs remain slower ON in both orders:
23.63/21.35 and 23.45/21.15 FPS (OFF/ON, about −9.7%). Separate diagnostics
reduce CBs/Present 21.03→6.51 while increasing replay idle and compatibility
batch calls. Six runs pass; sampled scene output is normal, not wild exact
proof. Default stays OFF. Next separate repeated published-prefix ClosePass
acknowledgements from Arena/session exclusion and range-consumer costs.

[H415](present-pacing-query-range-retained-release.415.md) connects serialized
GPU occlusion membership to retained writers while preserving published-prefix
ClosePass acknowledgement. The real contiguous/segmented GPU fixture now
publishes zero sources before polling and one at FLUSH, with exact query/pixel
results. Native bounded and regression checks pass; H416 supplies negative
wild performance qualification. Full concurrency composition remains open.
Default stays OFF.

[H414](present-pacing-query-range-replay-boundary.414.md) reproduces the range
slowdown in reverse order and identifies a production integration gap: ordered
Query replay publishes the writer and waits for ClosePass acknowledgement
before membership/END reach the range owner. Deferred END and marker counts
are zero, while Arena/session packing is lost. H415 addresses that release
boundary; these historical FPS values do not measure the new implementation.

[H413](present-pacing-query-range-routing.413.md) removes intentional Arena
fallback warnings and bypasses capacity planning for validated non-capture
draw/state raws. A clean single STALKER pair is 24.12 FPS OFF / 21.30 ON with
zero admission warnings. Both exit normally; no speedup or default promotion
is established, and remaining compatibility/range costs are not yet attributed.

[H412](present-pacing-query-range-integration.412.md) connects atomic DrawRun
append, real queue publication/FLUSH and command-range Metal membership in an
OFF-by-default candidate. Native reference/candidate exact GPU checks pass,
including immutable receipts and forced tile. Same-input native burst timings
are recorded separately from STALKER FPS; composed concurrency/wild promotion
remain open.

[H411](present-pacing-query-range-clear-transaction.411.md) joins metadata
admission to the real ChunkSlot Clear appender with staged payload/reservation
and rollback (four focused tests pass). That historical step was Clear-only;
H412 advances the queue/DrawRun/Metal integration boundary.

[H410](present-pacing-query-range-owner.410.md) implements a move-only bounded
metadata owner with frozen scope refs, range/END offsets and cancellation-aware
receipts. Native tests pass 3/3, including 15,625 lifecycle schedules. Queue
payload atomicity, real FLUSH/publication and Metal consumption remain open;
this is ownership groundwork, not production integration or measured speedup.

[H409](present-pacing-query-range-prototype.409.md) adds a candidate range
contract/reducer and native fixture (3/3 focused tests). Scope cuts cannot
be removed profitably while END remains eager: deferred writer-qualified END
binding and FLUSH progress must be composed with the range owner. Production
storage/consumer, GPU exact and CPU cost comparison remain open; no FPS claim.

[H408](present-pacing-stalker-occlusion-ab.408.md) tests visibility OFF/ON:
28.18 / 28.40 FPS (+0.78%, one exploratory pair, no demonstrated gain).
Separate diagnostics reduce draws/Present 9.51% but double CBs with nearly
unchanged pass counts; scope changes account for all 32,748 extra semantic
publications. ON alone is not the large-gap fix. Next examine membership
ranges within a source, not another CB-only join; benefit remains unproved.
H407's 27.41 FPS is user-accepted as the working baseline. Capture failures
remain a visual qualification limit, not a reason to repeat baseline loops.

[H407](present-pacing-stalker-baseline-recheck.407.md) repeats observer-OFF
after user-reported interference in the 23.93 FPS run: 27.41 FPS over 119.43 s,
still 8.37% below H403 OFF with matching binary/config receipts. The repeat's
window capture fails and falls back to an Orca screenshot, so foreground and
visual qualification remain open. Resolve this before renderer attribution.

[H406](present-pacing-stalker-cross-renderer-waits.406.md) finds the same
engine/Secondary/OpenAL dependency classes on D9VK, so their presence alone
does not explain the renderer gap. Both diagnostic runs drift far below prior
FPS and use buffered scene signals; comparative timing is unqualified.
Restore an observer-OFF control and stronger scene alignment before pricing
the differential; retain the snapshots only as common-owner evidence.

[H405](present-pacing-stalker-controlled-thread-owners.405.md) rechecks the
16-voice lane with native and guest snapshots: late encode/replay input waits
dominate their sampled stacks; guest engine coordination and one OpenAL wait
remain. The yielding helper starts in xrCPU_Pipe. Sequential snapshots do not
close a wake timeline or map H404's native IDs, and common game/audio costs
do not yet explain the D9VK differential. Cross-renderer late-window comparison
is the next gate, not another unqualified renderer policy change.

[H404](present-pacing-stalker-gpu-supply-gaps.404.md) records two short current
STALKER Metal System Trace windows. App GPU Active unions are 46.31% middle
and 15.89% late; the longest late gap is 176.05 ms, with the following CB
committed only after 175.62 ms. This favors pre-submit supply/progress over
continuous GPU saturation. Secondary/unnamed-thread CPU samples dominate
that gap; symbolicated export failed, so exact CPU owner and wait/waker remain.

[H403](present-pacing-stalker-covered-pending-validation.403.md) tests
covered-Pending OFF/ON and reversed repeats with observers OFF and default
occlusion fixed: 29.91 / 29.75 FPS (-0.53%), no demonstrated gain. A separate
census confirms 99.53% of backend calls use the fast path. This weakens mutex
polling as the dominant gap owner; it does not establish zero CPU savings or
qualify actual visibility. Runtime defaults remain unchanged.

[H402](present-pacing-stalker-query-split-census.402.md) accounts for all
35,922 real-visibility SemanticBoundary publications as scope replacement;
END-only deferral is not their owner. Backend polling reaches 1.88M/2.60M
calls, over 99.94% of Pending needing no new submission request, but the
current covered-Pending fast path remains default OFF. Counts do not price
CPU/lock cost. Existing CB joining has historical no-gain evidence; current
observer-OFF policy comparison and D9VK work/critical-path attribution remain.

[H401](present-pacing-stalker-visibility-gap.401.md) confirms that current
default occlusion results are primitive counts, not GPU visibility. A separate
current-binary diagnostic sees 78/78 positive retained results by default,
versus 90/93 zero results with real visibility. Scene-like draw averages fall
but CB averages double; unmatched camera windows and intrusive observers
prevent causal FPS attribution. A short native sample finds both replay and
encode waiting for input in most stacks. Cross-renderer workload counts,
visibility qualification and submission attribution remain open.

[H400](present-pacing-gt2-renderer-comparison.400.md) obtains a current
observer-OFF 3DMark05 GT2 dxmt9 baseline of 31.68 FPS across two accepted
runs. D9VK/MoltenVK 1.4.1 emits results but fails the black-screen gate in
both attempts; its values are excluded. Resolve rendering versus fullscreen
capture failure before using GT2 to generalize the STALKER comparison.

[H399](present-pacing-stalker-renderer-comparison.399.md) compares current
observer-OFF STALKER Day against D9VK/MoltenVK 1.4.1 with identical game
settings and the same Wine root/prefix. Two accepted runs per route average
29.54 versus 53.92 FPS (1.825x); the gap spans the middle as well as the tail.
A D9VK timeout is excluded and retained. D9VK launch preparation is much
longer, but excluded from game-owned scene FPS. The old README reference is
not a current matched baseline; next isolate mid-scene CPU/supply ownership.

[H398](present-pacing-ingress-wild.398.md) runs promoted commit `694427f3`
through STALKER Day/high: normal completion, zero GPU errors and sampled
middle/late visuals without obvious corruption. Engine average is 27.84 FPS;
selected early/middle/late windows are 32.56/47.65/16.70 FPS. This single run
is operational evidence, not matched-input speedup or all-family/Reset proof.

[H397](present-pacing-ingress-promotion.397.md) promotes capture-only ingress at
the user's request and removes the OFF selector. Final-binary focused tests
pass 13/13; unset versus retired `0` passes eight GPU iterations and 16 exact
readbacks on both routes. All-family/Reset and STALKER qualification remains
open. Default-off statements in H394–H396 describe their historical builds,
not current policy; no FPS gain is claimed.

[H396](present-pacing-ingress-gpu-exact.396.md) qualifies the same GT2 captured
input with ingress OFF/ON on Compatibility and ProductionSlot: 8/8 GPU
iterations pass, all 16 readbacks match original bytes, and loaded images/input
match. Ticket rechecks fall by 102 per process on both routes, confirming the
branch executes. This synchronous provider oracle is not an async performance
measurement; family/reset composition and STALKER wild remain open.

[H395](present-pacing-ingress-handoff-validation.395.md) binds ingress ownership
to actual parked-worker execution: six direct/compatibility success/failure
schedules and two pre-adoption refusals settle references correctly. Extended
rename tests cover capture-only residency followed by final-use sequence
protection. x86_64 focused tests pass 3/3; all-family/GPU/wild promotion remains
open and the candidate stays default off.

[H394](present-pacing-ingress-deferred-marks.394.md) adds a default-off
capture-only ordinary ingress candidate independent of CPU-ready policy.
Arm64 routing 2/2, x86_64 focused 9/9 and existing TLA regression pass; the
bounded handoff checker detects three deliberate lifetime/visibility faults.
All-family composition, GPU exact/wild qualification and performance remain
open. No default change or FPS gain is claimed.

[H393](present-pacing-ingress-lifetime-audit.393.md) clarifies the remaining
ingress coupling: Unix already owns accepted raw work, and backing capture is
already outside the queue mutex. The residual acquisition reconciles a
provisional use-stamp ticket. Capture-only ingress independent of execution
policy is a candidate, not yet qualified: mutation visibility, final-use
coverage and the model's stronger producer reservation premise must be closed
before removing the repair lock. No runtime or performance change is claimed.

[H392](present-pacing-stalker-cpu-recheck.392.md) rechecks `fd2fafa5`: baseline
early/middle/late selected windows are 32.43/47.81/15.35 FPS; late pass count
rises to 80.85/frame. Valid diagnostics show Replay input empty 67.8% before
the tail and 33.5% in the tail, with final app native CPU occupancy .888 core
versus Replay .676 / Encode .648. Compatibility lock residence remains real
but is not the whole limiter. Prioritize observer-OFF late app stack ownership;
do not treat same Present IDs across runs as identical scene inputs. The wide
tail trace overflow is retained/rejected; a short retry qualifies.

> Current, compact view for this performance domain. Historical detail from the former
> top-level `present-pacing.md` overview is preserved in [log](log.md). Domain landing: [index](index.md).

[H391](present-pacing-fixed-projection-validation.391.md) closes fresh GPU/wild
integration: both GT2 routes pass two cold-device exact readbacks each;
STALKER Day passes with 3,251 Presents, zero chunk rejects/GPU errors and
sampled normal scenes. Loading/focus delay excludes this run from performance
ranking. Order-reversed baseline/candidate CPU performance remains open.

[H390](present-pacing-fixed-projection-integration.390.md) connects direct field
comparison to production lookup. Native edge/collision tests and 18+18 GT2
CPU semantic iterations pass with identical work counts. Verify-mode lookup
elapsed falls 0.9791→0.9116ms/input; controlled performance remains open, while
H391 supplies fresh GPU/wild evidence. No hash-only authority or cache was introduced.

[H389](present-pacing-fixed-projection-probe.389.md) confirms an isolated x86
comparison still constructs the 2,568-byte fixed projection. Direct field
comparison passes 398 bounded edge cases and saves 65–68ns/call in a synthetic
equal-input probe. Production call-site inspection and same-Tape validation
remain open; this is a small candidate, not a replay/FPS improvement claim.

[H388](present-pacing-uniform-lookup-owners.388.md) reuses existing H387 counters:
full uniform lookup is 0.934–0.950ms/input, fixed-component find 0.299–0.303ms,
vertex append 0.178–0.185ms. There are 919 last hits, only 168 bucket probes
and no equal-hash collisions per input. Prioritize content/projection and
fixed-component search, not hash-table collision tuning. These elapsed totals
include warmups and observers; no new runtime change or STALKER transfer claim.

[H387](present-pacing-compat-append-phases.387.md) splits compatibility append
on identical GT2 Tape input: uniform lookup/intern/storage is ~2.20ms of the
~3.03ms split-observed append; state is ~0.245ms, final command/payload writes
~0.171ms, batch reserve ~0.067ms. Split clocks add ~0.525ms to parent-only
append observation. This is GT2 attribution, not STALKER cost equivalence or
removable CPU/FPS. Qualify the workload transfer before an off-lock rewrite.

[H386](present-pacing-mark-ticket-coverage.386.md) finds missing Replay holder
coverage: compatible Slot snapshot/reserve/mark/append overlaps 20.602ms of
31.133ms mark-ticket acquisition in 16 intervals; append alone overlaps
12.865ms (included in its parent). All measured holders cover 24.026ms.
Uncovered time is 7.107ms, not proved scheduler cost. This is observer-inclusive
attribution, not FPS or a cross-run improvement: inspect compatibility append
materialization/ownership before removing the producer's lifetime lock.

[H385](present-pacing-streaming-summary.385.md) implements the streaming
summary fold: no temporary observation vector or second traversal, with
3,906 exhaustive observation sequences and actual Slot ordering tests.
Compatibility/hash derivation and resource resolution remain unchanged.
The controlled wild run passes, but summary wall is 4.135ms versus H384's
4.163ms with different source counts (32 versus 38): no runtime gain is
established. Split remaining view/flags/resolver/hash work before caching it.

[H384](present-pacing-submit-summary-family.384.md) resolves the combined phase:
38/38 members use legacy ChunkSlot payloads; summary reconstruction costs
4.163ms versus validation 0.330ms in 16 intervals. Prioritize streaming summary
fold equivalence, not removal of safety checks or an Arena-specific fix. This
is 0.260ms/Present of measured summary service, not an FPS gain guarantee;
mark-ticket acquisition remains mostly unattributed.

[H383](present-pacing-submit-phase-owner.383.md) splits 32 submit sources:
validation/summary 4.297ms versus driver commit 0.667ms in 16 intervals;
pending-completion acquisition is 0.035ms. Mark-ticket acquisition remains
mostly unattributed (0.775ms holder coverage out of 17.095ms). Split safety
validation from repeated summary traversal before moving driver commit.

[H382](present-pacing-app-wait-holders.382.md) records same-mutex holders:
1,372 mark-ticket attempts total 17.113ms in 16 intervals; measured holder
overlap is only 1.739ms, mainly encode submit (1.604ms). Remaining acquisition
time is unattributed. No StateBlock queue-space burst recurred in this narrow
window; replay is still empty 51.42% of it. Split submit and uncovered lock
scopes before proposing lock removal or queue-capacity changes.

[H381](present-pacing-controlled-app-owner.381.md) follows the Primary: 179/320
guest PCs are in game modules, while most bridge PCs are Unix-call return
sites, not exclusive bridge CPU. Native samples expose StateBlock queue-space
backpressure and mark-ticket mutex acquisition waits. Average input starvation
coexists with full-queue bursts; trace these owners before changing capacity.

[H380](present-pacing-audio-controlled-supply.380.md) recaptures the capped lane:
app/replay/encode use 0.866/0.463/0.479 cores, while replay input wait and encode
unpublished-input gap remain about 25ms/Present. Fixed-arrival replay sensitivity
is near zero in this window, not an unconditional ceiling. Prioritize game/Wine/
PE/synchronous-Unix attribution on the primary before encode parallelism.

[H379](present-pacing-audio-cap-abba.379.md) implements an explicit 16-voice
control lane, verifies actual allocation, and completes observer-OFF ABBA.
Mean FPS rises 27.675→29.745 (+7.48%); whole-run p99 falls from 143–154ms to
87–88ms. The lane changes audio quality and stays opt-in; original 32-voice
results remain the baseline. Human audio/temporal validation remains open.

[H378](present-pacing-audio-mix-coefficients.378.md) reuses saved stacks to decode
the four applied mixing coefficients. In three late captures, 71–76% of
qualified samples have all coefficients below 0.01; none are all zero.
This is low-gain work, not proof of inaudibility or safe voice culling. No new
run or runtime change; causal audio-quality/provider A/B remains separate.

[H377](present-pacing-audio-source-progress.377.md) finds all 32 late source
slots advancing through 2–3 buffers with OpenAL looping disabled. The source
address set stays fixed across the two sampled windows; this is consistent
with active streaming, not frozen cursors or a growing observed list. Game-side
requeue/duplication and inaudible work remain unresolved; address reuse is not
generation-qualified. Do not reduce sound quality as an implicit optimization.

[H376](present-pacing-audio-workload-growth.376.md) reads the mixer source list
in early/late windows: two qualified early observations show two playing
sources, while 62 late observations show 22–32 (median 32), with list size 32
throughout. Frame bounds grow far less. Source workload growth is now concrete,
but early sample coverage is sparse and no CPU/FPS ratio follows from it.

[H375](present-pacing-audio-owner-execution.375.md) samples the WinMM owner:
118 of 124 lock-owned PCs execute the wrapper's scalar interpolation/mixing
routine; six execute other wrapper code. Another 36 samples show timer waiting
without owning the global lock. Installed-binary disassembly binds the x87
accumulation loop and recursive lock scope. Prioritize audio workload and
per-sample cost, without interpreting intrusive PC shares as CPU time or FPS.

[H374](present-pacing-openal-wake-edge.374.md) binds 1,724 OpenAL address waits
to WinMM-owner wake requests and waiter returns using existing Wine sync
traces. Median entry-to-return is 10ms; 1,720 returns share the alert's 1ms
timestamp bucket. Typical delay precedes the wake request, strengthening the
callback-work lead rather than a large post-alert scheduling-delay hypothesis.
Logging is perturbing; outermost lock hold time and audible causality stay open.

[H373](present-pacing-openal-lock-sampling.373.md) follows with a five-second
non-suspending poll: 1,382/1,663 samples observe the OpenAL lock owned, almost
all by the WinMM owner. The run passes with unchanged binaries. Sampled
same-owner spans are not continuous hold durations; guest CPU-time deltas are
unusable zeros. Exact unlock/wake/resume tracing remains open.

[H372](present-pacing-guest-wait-ownership.372.md) connects Primary/Secondary
guest engine locks to the bundled OpenAL global critical section, owned in
repeated snapshots by a WinMM thread executing `wrap_oal.dll`. Two bounded
guest-snapshot runs pass with identical staged binaries and zero reported GPU
errors. Sequential snapshots prove observed owner dependencies, not an atomic
wait graph or actual wake timing. Follow the audio lock's hold/release/resume
timeline before another encode-policy change; audible causality remains open.

[H371](present-pacing-tuned-tail-wait.371.md) records the user-selected
STALKER-specific tuning recipe and recaptures the tuned tail. Replay queue is
empty 77.84% of the bounded window; replay/encode use 0.241/0.217 cores.
Native stacks corroborate both waiting for input while primary and secondary
game threads enter NtWaitForAlertByThreadId. Guest caller/wait-object/waker
binding, not another encode parallelism change, is the next attribution gate.

[H370](present-pacing-xray-worker-abba.370.md) completes observer-OFF A–B–B–A:
default 25.425/22.000 FPS versus `-max-threads 2` 28.337/27.617 FPS, all PASS
with identical staged binaries. Both orderings favor the candidate; mean
difference is +17.98%, with substantial baseline drift and only two runs each.
Tail p99 and >250ms counts improve, but stalls remain. Screenshots show no
gross corruption; temporal visual/audio confirmation is open. No default or
README baseline is changed.

[H369](present-pacing-xray-worker-yield.369.md) identifies an installed X-Ray
helper idle protocol with up to one million SwitchToThread attempts before
sleep backoff. The existing `-max-threads 2` engine option reduces seven
yield-heavy helpers to one; a diagnostic run passes without runtime patches.
Single-run throughput improves, but unmatched scenes and diagnostic overhead
leave observer-OFF reversed-order performance and visual/audio qualification
open. The primary's native wait resolves to NtWaitForAlertByThreadId/kevent;
its guest waiter/waker is not yet identified.

[H368](present-pacing-tail-yield-owner.368.md) resolves H367's repeated native
PC to Wine `NtYieldExecution` → `cthread_yield`. Seven threads show that chain
in 89.7–94.9% of recorded stacks and consume about 3.13 cores in a separate
following CPU-delta window with the invasive PE sampler disabled. The primary's
late stopped-PC share shifts toward ntdll; guest callers and exact wait sites
remain open. Prioritize these owners, not a global Wine yield replacement or
encode parallelism; audio causality and FPS benefit are unproved.

[H367](present-pacing-tail-supply-audio.367.md) samples two late STALKER windows:
app CPU is 72.8–83.1ms/Present with substantial replay input and encode
unpublished-input gaps, and no selected instrumented queue-wait events.
Prioritize app/guest execution ownership, including seven unresolved repeated-PC
threads. System Trace coverage holes and teardown-only CoreAudio errors leave
in-game audio correlation open; no new runtime policy or FPS gain is claimed.

[H366](present-pacing-coalesce-query-reuse.366.md) removes repeated graph
reachability work without changing coalescing decisions. On the same four
STALKER inputs, OFF optimizer CPU falls from 7.8–27.6ms to 0.071–0.183ms;
full outputs/statistics match the frozen reference and bounded refinement
passes 102,414 cases. Return diagnostics are now explicit opt-in. A counters-OFF
wild run passes at 25.169 FPS, but this single historical comparison does not
prove an FPS gain; the late slowdown remains. Reattribute the residual slow
source before choosing the next optimization.

[H365](present-pacing-optimizer-fixture.365.md) established same-input attribution:
four real STALKER graphs match exactly under diagnostics OFF/ON and whole/split
optimizer execution. Large inputs still cost 26–28ms native CPU with diagnostics
OFF, almost entirely pass coalescing, **with zero merges**. Return diagnostics
add 0.85–3.39ms, not the majority. Prioritize reusable dependency/reachability
queries and safe early rejection on the unchanged graph; H366 implements
these without changing graph decisions. No paired FPS improvement is claimed.

[H364](present-pacing-encode-residual.364.md) records a counters-OFF STALKER
Day baseline of 24.822 FPS, with the late slowdown still present. A bounded
ordinary-source trace reproduces a 35.7ms planning CPU spike: 35.5ms is in
the optimizer, versus 0.185ms graph construction. Perf-enabled return-candidate
diagnostics are inside that phase; H365 separates these from core coalescing
on identical graphs. Residual encodeDraw budgets favor
index/stream binding over another sampler-plan rewrite. Full STALKER Tape
capture remains blocked by volume/query coverage; no fixed-input spike or
FPS gain is claimed.

[H363](present-pacing-binding-plan-default.363.md) makes serial binding-plan
reuse unconditional and retires its OFF selector at the user's request.
Safety/unsupported-context fallback remains; promotion adds no FPS claim.

[H362](present-pacing-binding-plan.362.md) (historical OFF/ON; selector removed)
removes disabled draw diagnostics
and implements opt-in command-local sampler-plan reuse. Same-image GT2 GPU
exact passes with nonzero reuse; short encode elapsed is 40.36→39.31ms, not
an FPS verdict. STALKER planning splits now work, but the 40–45ms spike did not
reproduce in the smaller selected sources; matched large-source evidence and
observer-OFF performance qualification remain open. Subsequent explicit user
promotion makes serial reuse unconditional and removes its OFF selector;
this policy change is not an additional performance verdict.

[H361](present-pacing-serial-encode-owners.361.md) remeasures serial STALKER
Encode on H360: encodeDraw owns 72% of observed encodeChunk elapsed scopes;
binding-packet preparation owns 16%, despite 90% packet-cache hits. The packet
is built before lookup on every draw. Cold diagnostic clocks are another
concrete candidate. These are elapsed service budgets, not native CPU or FPS
gains; intrusive stack sampling is excluded from the performance verdict.
The bounded native-CPU retry finds only 27.6% Encode-core occupancy and long
unpublished-input gaps in its selected late window. Three sources also expose
40–45ms planning/setup spikes; neither continuous Encode saturation nor a
whole-pipeline FPS gain is established.

[H360](present-pacing-prepared-default.360.md) records the explicit user decision
to promote prepared serial reads and remove the OFF policy. Eligible ordinary
Slots always use fused preparation; the old full-plan consumer is test-only.
Required failure/unsupported-mode fallback remains. This supersedes historical
default-OFF statements below without claiming a newly proven FPS improvement.

[H359](present-pacing-prepared-fused.359.md) fuses serial metadata append into
the existing publication resource-mark traversal and removes the second final
command-view walk. Both same-input campaigns are pixel-exact (216 iterations),
but block timing drift prevents an Encode/FPS claim. Preparation still adds
about 0.12–0.17ms in the repeated OFF/ON comparison, similar to H358. The lane
stays OFF; this is publication fusion, not final-Slot emission fusion or pooling.

[H358](present-pacing-prepared-minimal.358.md) replaces the serial consumer's
general canonical plan with raw read metadata only. On the same GT2 input,
preparation falls from H357's 2.2ms to 0.17ms (separate-build comparison), but
new-binary OFF/ON still demonstrates no Encode saving. All 108 iterations are
pixel-exact with 646 consumptions per ON iteration. Keep default OFF; the
remaining publication walk is not yet fused into Replay emission.

[H357](present-pacing-prepared-cost.357.md) prices the serial consumer on the
same GT2 input: preparation adds 2.2–2.3ms per captured-frame iteration while
Encode remains near 42ms with no demonstrated saving. Six sequential blocks
include A/A, reversed comparison and nested-timer OFF; all pixels match with
646 actual prepared consumptions per ON iteration. Keep the lane OFF and
target fused/reused preparation next. Cold-device wall scopes are not FPS or
pipeline-critical-path measurements.

[H356](present-pacing-prepared-serial.356.md) connects the first default-OFF
ordinary-Slot serial consumer: raw-equivalent DrawRun read-hazard membership
and Bloom bits. The same GT2 capture produces byte-exact source/output images
across OFF/ON, with 1292 actual prepared consumptions. This is not a general
Encode plan or a performance promotion; fresh publication allocation and a
second source walk are priced by H357.

[H355](present-pacing-prepared-comparison.355.md) requalifies the same runtime
with an in-scene capture, zero GPU errors and 21.617464 FPS (single run, not a
speedup claim). All 21 wide sources fit, at 415–459 identities. Equal synthetic
420-identity inputs cost 61 versus 231 microseconds for indexed versus linear
construction, with A/A and reversed-order controls. Actual captured-input
comparison and Encode consumer/GPU proof remain open.

[H354](present-pacing-prepared-index.354.md) implements the 512-entry dictionary
and 1024-slot builder-only lookup. Native collision/boundary/reuse tests pass;
real input seals 21 wide sources with no capacity rejection. Its game-window
capture fails; H355 supplies the integration retry and synthetic cost comparison.
Pass/certificate capacity and original Encode execution are unchanged.

[H353](present-pacing-prepared-cardinality.353.md) closes full rejected-source
cardinality: all 21 sources require 412–429 canonical identities, with zero
unresolved rows and 82.59% repeated within-source access occurrences. Select a
512-entry dense dictionary plus 1024 builder-only indexed lookup slots for the
implementation (now H354), preserving other capacity limits and fail-closed overflow.
H353 itself is cardinality evidence, not a limit increase or FPS claim.

[H352](present-pacing-prepared-reuse.352.md) reuses one private diagnostic region,
with native fresh/reused differential and OFF/ON publication checks passing.
The first real-input run was interfered with by a macOS launch dialog and timed
out. The same-binary clean retry completes normally with an in-scene capture,
zero GPU errors, and 1 allocation / 86 reuses across 87 candidates. Total
diagnostic CPU is 11.828 ms and native average 20.982122 FPS; changed live input
precludes a speedup claim. Exact GPU equivalence and async consumption remain open.

[H351](present-pacing-prepared-screen.351.md) implements diagnostic pre-allocation
screening. Actual STALKER input skips 1,084/1,126 sources, leaving 42 allocations
(5.991 MiB cumulative), 21 structural successes and 21 capacity rejections.
Total diagnostic CPU is 11.282 ms; live inputs differ from H350, so this is
allocation-suppression evidence, not an FPS speedup. Storage policy and wide-source
cardinality remain the next gates; Encode consumption is still disconnected.

[H350](present-pacing-stalker-prepared-shadow.350.md) measures the actual STALKER
publication shadow: 1,209 attempts cost 30.605 ms in 21 encoded Presents;
82.26% is spent on 1,146 query-scoped single-command sources, while 21 larger
sources exhaust the 256-identity bound. Cumulative requested plan storage is
172.46 MiB, not RSS. Prioritize pre-allocation screening and storage policy;
no fixed-input speedup or prepared-consumer qualification is established.

[H349](present-pacing-prepared-publication-shadow.349.md) connects an opt-in
prepared-plan shadow to ordinary publication. An x86_64 synthetic 64-command,
resource-empty source costs 19–20 us median including observation, with a
149,576-byte allocation. This is neither real-workload pricing nor an Encode
saving: fused construction, async ownership and differential comparison remain open.

[H348](present-pacing-shared-pass-indices.348.md) implements shared pass-table
membership for child coverage and certificate union. Native child read metadata
shrinks 2056→48 bytes; the production Replay provider tests pass. Construction
remains in Encode, lookup remains linear, and the 256-identity admission bound
is unchanged. This is storage refinement, not FPS or wide-resource GPU proof.

[H347](present-pacing-stalker-access-cardinality.347.md) closes rejected-pass
cardinality: all 21 overflows require 433–458 distinct canonical reads, with
69.95% repeated touches. All 1528 diagnostic pass transcripts are exact.
This supports investigating shared pass-owned identity tables and child index
views; production bounds and serial policy remain unchanged. No FPS claim.

[H346](present-pacing-stalker-pass-access.346.md) implements separate pass-read
metadata without changing draw/state SoA. R694 observes 54 accepted passes
(maximum 25 read identities), with 21 pass unions still exceeding 256. The next
gates were large-pass cardinality (now measured by H347), proof-storage cost and wide-resource GPU exact
validation; this is not parallel execution promotion or an FPS A/B.

[H345](present-pacing-stalker-resource-closure.345.md) isolates the resource
gate: R693's 61 coarse failures are all pass-read-union overflow at the shared
12-identity bound. Separate bounded pass closure from small draw-entry summaries
before considering capacity expansion; this is not yet parallel speedup evidence.

[H344](present-pacing-stalker-parallel-observer.344.md) finds 40 structural
parallel candidates / 24975 draws in 20 of 1470 late sources while retaining
serial execution. Candidate-bearing sources contain at most 26.36% of observed
encode CPU, including rejected work and observation. Incomplete resource-set
causes and child imbalance are the next admission targets; parallel speedup
and the late blocking-head reproduction remain unproved.

[H343](present-pacing-stalker-capture-origin.343.md) identifies volume locking
as the first STALKER capture blocker. Complete volume capture/replay is a real
grammar extension, not an admission-flag fix. Existing shadow counters run
only with ExplicitParallel; serial-only cost-weighted eligibility remains open.

[H342](present-pacing-stalker-tape-admission.342.md) blocks fixed-input
STALKER parallel qualification at capture admission. A generic pre-arm
`child_abort` invalidates the registry; later output-admission errors do not
identify the original caller. Query/volume provider grammar also needs work.
Repair and qualify capture before parallel policy timing; no speedup is claimed.

[H341](present-pacing-stalker-draw-phases.341.md) measures encodeDraw at
71.90% of selected DrawRun CPU (27.178/37.800ms), with 10.622ms residual
including preparation/pass/post-draw work and observer overhead. Both windows
lack blocking CV entries, so this does not extend the earlier blocking-head
chain. Align scene/semantic boundaries before attributing the late hitch;
do not derive pure product costs or FPS gains from these intrusive scopes.

[H340](present-pacing-stalker-command-phases.340.md) finds DrawRun accounts
for 93.85% of the selected observed body (18.545/19.760ms CPU); split and
dispatch residual include timer overhead. Internal DrawRun pass/binding/draw
work remains open. Same-image OFF/ON runs pass but differ in source population,
so the intrusive costs are not a pure product or FPS verdict.

[H339](present-pacing-stalker-chunk-phases.339.md) finds command execution
owns 98.60% of the selected head's encodeChunk CPU (16.719/16.957ms), with
0.076ms setup and 0.162ms finalization. The body includes draw, non-draw/pass
and split work; these internal owners, not source setup, are the next split.

[H338](present-pacing-stalker-framegraph-phases.338.md) attributes 97.51% of
the selected blocking head's backend CPU to encodeChunk (13.407ms), versus
0.337ms planning/setup and 0.005ms cleanup. The source-qualified partition is
complete, but encodeChunk's internal owners and observer-OFF gains remain open.

[H337](present-pacing-stalker-encode-phases.337.md) narrows the late STALKER
blocking-head CPU owner: 13.965ms backend, 0.892ms prefetch and 0.033ms other
work within a 14.890ms encode CPU scope. The 55-source pattern still fills the
inflight cap. Backend planning versus encoding and scope/END source batching
are separate next gates; no observer-OFF FPS improvement is established.

## Scope

This domain owns the **CPU/sync side** of GT1 wallclock: why
`completion_wait_ms` (the time the completion handler thread spends in
`MTLCommandBuffer.waitUntilCompleted()`) reached 28-31 s while
`gpu_command_buffer_time_ms` was only ~4 s, where that gap actually lives,
and which production-safe knobs can recover the slack without breaking
visual sync.

This domain investigates wallclock throughput and CPU work; an attribution
result or code reduction is not itself a measured FPS improvement. The
GPU frame-time story is owned by [hidden-backend-storage](../hidden-backend-storage/index.md) /
[index-cache-locality](../index-cache-locality/index.md); the per-CB encode story by [state-churn-encode](../state-churn-encode/index.md).

## Latest Conclusions

[H336 publication progress](present-pacing-stalker-publication-progress.336.md)
reproduces 55 sources from 54 BEGIN/END pairs: first BEGIN drains a 284-command
writer, then each END drains a one-command scope. All are SemanticBoundary.
Inflight=31 is released by the large head after 14.35ms encode plus 5.99ms
completion observation wait; post-observation retirement is 0.045ms. This
supports investigating scope-versus-source granularity and head encode CPU,
not removing query guards or treating observed waits as GPU arithmetic time.

[H335 replay ownership](present-pacing-stalker-replay-ownership.335.md): late
receipt waits inherit publication inflight-cap backpressure (153 waits at
count 31; no observed writer-slot CV wait). All 51 targets join to raw replay.
Separate pre-split diagnosis measures app CPU 50.44ms/P and nested import
CPU 2.16ms/P; guest-PC shares are game 47.29%, Wine-system PE 28.89%, dxmt9
PE 8.38%, bridge 11.96%. These are different, intrusive populations, not an
exclusive CPU budget or a reason to remove completion/receipt guards.

[H334 bounded wait trace](present-pacing-identity-wait-trace.334.md) adds
identity-qualified CV-entry and mutex-acquisition evidence without polling
subsampling. Native identity/closure tests pass. R671 ends before the selected
window closes and captures Orca instead of the game; its empty CSV is rejected,
not counted as zero waits. Final-image R672 captures the game and closes
1400–1800 with no lost/unknown rows: receipt CV/reacquisition totals 2.817ms,
recorded query-queue acquisition 1.723ms. The larger slowdown lies after
that window; its CPU helper rejects an oversized log before sampling.
R673 closes 1800–2600 with 194 CV entries and 1.931s receipt CV/reacquisition,
94.1% belonging to query 54; recorded query-queue acquisitions total 31.695ms.
Its early CPU helper validates envelopes: CPU/frame grows 28.94 → 41.47ms
before large CB fragmentation, then 59.62ms in 2100–2499. This identifies
late receipt blocking, not its replay-worker owner or the whole slowdown.

[H333 pre-split wait ownership](present-pacing-stalker-pre-split-wait-owner.333.md)
separates app-thread CPU growth from the later CB explosion. The pre-split
bracket reaches 45.18ms CPU/frame at 0.787 cores with only 3.799 tail CBs/P;
the later bracket reaches 61.34ms at 0.581 cores and 48.301 tail CBs/P.
These are partial diagnostic populations, not a whole-frame cost budget.
Periodic query reports stop before the target region, leaving exclusive
replay/queue/game/Wine wait ownership unresolved.

[H332 scoped CB composition](present-pacing-query-scope-cb-composition.332.md)
preserves query visibility passes while sharing session CBs. Exact native
sample/readback and bounded refinement checks pass. Same-image Tape-ON
scoped-join OFF/ON/OFF STALKER runs yield 14.832 / 14.801 / 14.941 FPS;
CBs per Present decrease about 46%, without demonstrated FPS improvement.
Late stalls remain. The prerequisite pre-effect physical-capacity abort is
fixed with a reproducing native test; a separate scheduling-sensitive broad
session test remains unresolved. Scoped join stays default OFF. This result
does not qualify enabling Tape itself or identify pre-fragmentation wait owners.

[H328 covered-Pending ON wild run](present-pacing-query-covered-pending-wild.328.md):
`92ac354c` passes STALKER Day High 1280x720 with observers OFF, eight zero health
counters, a plausible scene capture and subsequent user confirmation of normal
operation. Single-run average is 19.601 FPS; the
late slowdown remains. No matched OFF delta, continuous visual proof or default
promotion is claimed.

[H327 queue mutex owner audit](present-pacing-query-mutex-owners.327.md)
reviews publication, encode submission and finish reclamation holders separately
from polling waiters. Coverage is partial and no waiter/holder join exists.
The read-mostly follow-up is Pending-only with durable FLUSH coverage and locked
success/initializer refresh. The [covered-Pending candidate](../../../specs/d3d9/queries/read-mostly-polling.md)
is now implemented default OFF with native/TLA and gated GPU evidence;
H328 supplies observer-OFF wild integration and user qualitative confirmation;
matched performance evidence and default promotion remain open.

[H326 reference tails/backend poll split](present-pacing-query-backend-poll-cost.326.md)
confirms late slowdown in retained WineD3D and DXVK/MoltenVK results; these are
historical, not scene-aligned current A/B runs. In dxmt9 R650, 99.81% of pending
submission requests already deduplicate, while queue-mutex acquisition accounts
for 87.73% of sampled backend-poll elapsed time (not whole-frame time). Wake
work contributes only 0.03%. Prioritize queue-lock-owner/read-mostly progress
analysis, not redundant-wake removal. R650 has a failed app-window capture and
is not visual/FPS qualification or proof of the long-frame cause.

[H325 benchmark FPS windows](present-pacing-stkcop-fps-windows.325.md) confirm
the late STALKER tail without new instrumentation: R648/R649 start at roughly
28.58/27.43 FPS over 0–60 reconstructed seconds, but fall to 7.60/5.10 FPS in
90–100 seconds. R649 has 41 frames over 100ms in that window. R648 already has
the tail with receipt diagnostics OFF; this is not a matched optimization
comparison or a causal join to replay waits. Focus the next attribution on
late stalls rather than whole-run averages.

[H323](present-pacing-query-poll-receipts.323.md) promotes immutable receipt
polling unconditionally in `029cef81`. [H324](present-pacing-query-receipt-cost.324.md)
adds the missing production-route census: R649 handles 488,900 of 488,901 polls,
with 94.70% S_FALSE and 99.48% occlusion. Among 7,640 timing samples, captured-raw
replay wait contributes 59.95% and backend polling 36.37%; snapshot acquisition
and copy contribute 1.59%. These are sampled attempt-time shares, not whole-run
CPU or removable frame time. Next: source-qualified wait tails and backend
queue-lock/classification/submission-request attribution. Diagnostic FPS is not
a performance comparison; fallback expansion is not supported as the next target.

Historical motivation for that promotion:

[H322 Ready/replay frontier](present-pacing-query-ready-replay-frontier.322.md)
finds 1,773 stable-END polls with later raw work already queued; their drain
intervals total 5.751 s, not a proven saving. Busy-entry calls contain 99.79%
of inner drain elapsed time; empty checks are only 22 ms over 163,160 calls.
The selected 126 occlusion rows have median prior Ready depth 9; immediate
predecessor encoding covers only 4.12% of their summed residence. Prioritize
immutable polling-receipt ownership and prefix authority, not unchecked drain
removal. R647 has a mid-run window restore and diagnostic observers: its FPS
cannot qualify performance; one restored scene spot is not whole-run equality.

[H321 observer-off control / queue-drain attribution](present-pacing-query-queue-drain-attribution.321.md)
uses identical staged images with no scheduled build/test/agent work during
the two STALKER runtimes. The observer-OFF visual spot completes at 21.457 FPS
with scene geometry and zero required errors. Separate diagnostic evidence
places the selected 56 occlusion rows' median 14.340 ms before dequeue versus
0.268 ms in dequeue-to-relock encoding. Matching 309,583 polling calls spend
10.071 s in the replay-drain prologue out of 11.071 s in the outer PE bridge.
These are bounded elapsed intervals, not additive critical-path/FPS costs.
H322 above follows up older-source/frontier and query-qualified drain scope.
No provider promotion, whole-sequence equality or performance gain is claimed.

[H320 actual occlusion / timeline / StateBlock](present-pacing-occlusion-timeline-stateblock.320.md)
adds a default-OFF Metal visibility provider and identity-qualified polling
evidence. Query cuts exposed attachment loss: source-local depth death did
not justify discarding data read by a later query. Provider-wide preserving
stores restore the inspected STALKER scenes and mixed zero/nonzero results.
Native Metal/core and the 214-configuration TLC gate pass; the lifecycle model
does not prove attachment bytes. R644 overlaps worker verification and cannot
qualify FPS or clean timing. Its StateBlock census finds only 2 same-canonical
snapshots among 169,252 rebuilds, so broad snapshot bypass is not justified.
Next gates are quiet observer-off visual/performance repeats and separate
query submission/polling attribution, not default promotion.

[H319 committed-image wild checks](present-pacing-policy-consolidation-wild.319.md)
complete 05 GT2, 06 GT2 and STALKER Day with normal exit, qualified staged
images, scene captures and zero required error counters. Scene FPS is
26.385/16.417 for 05/06; STALKER's own average is 13.764. Both GT2 spots are
below README. Background activity and the absence of a same-session parent
control prevent causal attribution or a non-regression claim. Keep README
unchanged and use matched parent/current repeat controls for the next gate.

[H318](present-pacing-policy-consolidation.318.md) consolidates two qualified
redundant-work reductions: known-setter uniform invalidation and bounded
lock-scope refinement. Their runtime opt-outs and legacy alternatives are gone;
synchronous unpublished prefetch and secondary uploaded-cbuf hashing are
retired entirely. Test-only conservative/scalar references remain. This is an
explicit structural policy decision, not a new CPU/FPS non-regression result;
broader MT, live pacing and workload economics remain open. H317 END grouping
and independent experimental providers are unchanged.

[H317](present-pacing-query-end-coalescing.317.md) implements default-OFF END
grouping with actual-publication identity, independent receipt lifetime and
single-FLUSH progress. Bounded TLA/native, exact Metal bytes and x64/x86 Wine
checks pass. Split observers distinguish END/FLUSH source cuts from actual
CB counts. The separate old/current ABBA comparison suffers severe same-image
drift; no absolute code delta, FPS gain or default promotion is established.

[H316](present-pacing-query-default-promotion.316.md) promotes asynchronous
query completion to default ON after bounded native/model/GPU/Wine and visible
STALKER evidence. Explicit OFF preserves the synchronous reference. END still
publishes eagerly, but an actually submitted query no longer pressures an
unrelated suffix or repeats encode/finish wakes. Failure injection uses the
production upload reaper; it does not prove real driver recovery. Full-frame
wild equivalence, real Metal occlusion/timestamp providers and deferred END
coalescing are outside H316 (see H317 for its candidate). Clean same-image runs are kept separate from the
observer-enabled locality diagnostic; README is unchanged.

Historical [H315](present-pacing-query-publication-fences.315.md) separates actual-prefix
Query END publication from completion waiting behind a default-off candidate.
The receipt joins ordinary queue and initializer progress; persistent FLUSH
requests wake encode without inventing completion. Native/model checks and
Wine x64/x86 delayed-S_FALSE → S_OK plus exact pixel probes pass. Initializer
fault coverage, visible wild/locality and real query-data providers remain
distinct gates; no default or README promotion.
One same-image STALKER diagnostic OFF/ON pair observes 11.976 → 17.338 FPS,
core query elapsed 63.254 → 1.052 s and queue sequence waits 62.521 → 0 s.
Polling and CB count grow; both onscreen captures fail. These are not clean
FPS-promotion measurements, and a later failure-only hardening revision is
qualified by separate native/Wine tests, not by those performance spots.

[H314](present-pacing-stalker-query-waits.314.md) moves to STALKER Day on
unchanged H312 images. The retry is 12.298 FPS; a separate diagnostic measures
64.465 s in GetData, 62.378 s of queue completion waits and only 1.318 s of
query-entry replay drain at its last periodic endpoint. These scopes overlap.
Partial app CPU is 0.449 cores; query submission/completion identity is the
next priority, not a proven removable wait budget. Both runs miss the onscreen
capture, so visual/focus qualification and README promotion remain closed.

[H313](present-pacing-app-thread-attribution.313.md) samples unchanged H312
images. Interior guest PCs are 70.28%/81.06% game, 10.26%/5.83% dxmt9 PE
and 11.18%/7.30% bridge in 05/06 GT2; these are **not CPU-time shares**.
Rosetta/Wine stack unwinding is unreliable and Time Profiler fails to save,
so exclusive game/Wine/PE/Unix CPU remains open. Recurring synchronous
contexts point to commit/import and buffer Lock/Unlock for direct calibrated
boundary accounting. No additive CPU budget, FPS ceiling or default change.

[H312](present-pacing-cpu-supply-critical-path.312.md) measures four calibrated
05/06 GT2 CPU-supply windows with matched-image controls. The app thread uses
0.94–0.96 cores, replay 0.39–0.54 and encode 0.41–0.56; input waits are mostly
empty FIFO, not reserved-head blocking. The first constraint points upstream,
not to saturated replay. App CPU is not PE-exclusive. Fixed-arrival half/zero
replay sensitivity mostly advances latency; producer/lock feedback and the
unconditional end-to-end speedup ceiling remain open. No FPS/default promotion.

Historical [H311](present-pacing-lock-scope-attribution.311.md) finds that the exact
historical H309 provider also recovers to about 18 FPS in 06 GT2, near H310.
The earlier 5.95–9.31 FPS population is not a stable binary-specific deficit;
run environment/execution state confounds its attribution. No isolated FPS
gain is claimed for the payload fix. Measure actual batch construction,
size, duplicates and flush causes next; retain semantic fences. H311 kept
default OFF; H318 later retires that selector without closing the economic gaps.

[H310](present-pacing-payload-count-batch-cost.310.md) replaces the extra
post-capture payload-count walk with checked constant-time settlement from
newly appended snapshots. Actual mixed-role queue tests and bounded arithmetic
oracles pass. This removes duplicate work, not a measured amount of GT2 CPU
time; actual batch population remains open, with the later FPS attribution
follow-up in H311 above.

[H309](present-pacing-lock-scope-wild.309.md) confirms fewer range-upload and
non-app arena acquisitions, but withholds default promotion. 06 ON/OFF perf
pairs are -27.2% and -9.2%, while OFF itself drifts markedly; neither a clean
regression attribution nor non-regression is established. Diagnostic FPS is
not a substitute for low-observer economics. FPS gain is unnecessary, but
CPU/latency/FPS non-regression remains required. README is unchanged.

[H308](present-pacing-lock-scope-refinement.308.md) implements range-upload
queue lock elision, bounded concrete-backing stamps and pre-lock immutable
replay preparation. Native ON/OFF checks pass, including three captured scalar
acquisitions becoming one batch acquisition in the queue fixture. This is a
default-off synchronization candidate, not measured CPU/FPS improvement.
Live capture, exact Slot reserve/append and publication retain their locks.

[H307](present-pacing-lock-contention-audit.307.md) prioritizes synchronization
independently of FPS. Range upload consumes no queue watermark but holds the
queue mutex around its arena-protected write. Replay ingress and publication
still perform per-binding arena stamps outside the logical-handle batch path;
H306 counts 20.7k/22.6k exclusive updates/P on a non-app TID. Existing samples
confirm replay publication marking, not the exact source of every H306 update.
Narrow unnecessary upload locking and investigate qualified stamp batching
before changing mutex primitives. Full queue-holder coverage, exact duplicate
rates and causal waiter/holder overlap are not yet measured.

[H306](present-pacing-upload-arena-locks.306.md) closes the named lock timing
gap with a default-off observer and four same-candidate diagnostic spots.
Application upload acquisition is 1.123/0.905 ms/P and hold 0.418/0.382 ms/P
for 05/06 GT2. Full-upload acquisition averages 114/118 microseconds per call;
arena update averages under one microsecond acquisition but occurs 720/821
times/P. Queue hold contains nested arena work: these are not additive savings.
Native/TLA and evidence audits pass; no lock policy changes or FPS promotion.
The next contract question is whether uploads can use the existing conservative
completion-watermark protocol without the broad queue hold. This is unproven.

[H305](present-pacing-app-thread-waits.305.md) narrows the supply question:
native application/commit CPU is 33.511/55.889 ms per frame, or 0.924/0.954
cores, in partial 05/06 GT2 CPU-only diagnostics. This includes game/Wine/PE
and synchronous Unix, not just PE. Non-CPU remainders are 2.770/2.698 ms/frame,
not removable wait budgets. Separate intrusive stack runs identify upload
queue-mutex and buffer-arena contention, including upload acquisitions outside
the existing queue-mutex split observer. H306 supplies their hold/acquisition split;
no lock-removal or FPS promotion follows. 06 health tails remain uncovered.

[H303](present-pacing-replay-baseline.303.md) moves attribution to replay on
unchanged products. 05/06 GT2 contained counter windows measure 22.33/23.74 ms
replay service and 15.25/36.31 ms input wait per Present, not native thread CPU.
Writer/commit backpressure waits are zero. Separate diagnostic planning costs
0.48/0.40 ms; compatibility state/materialization and final-Slot append deserve
the next decomposition. Whole-scene perf spots are 25.480/16.448 FPS; lower
results than H301 on identical products are not isolated code regressions.
06 perf leaves 48 final frames without full health counters. README is unchanged.

[H302](present-pacing-pe-completion-window.302.md) completes the fixture-only
bounded producer-window lane. Phase-final completion, state/pixel/order checks,
legacy schemas and all five production images are preserved. Thirty actual
PE processes pass; window 32 improves same-image draw-mean repeat spreads to
median 0.653%/0.561% (PE32/PE64), with worst cells still 8.755%/6.970%.
Whole-process CPU and drain time fall within the fixture, not the product.
Use this opt-in recipe for shape-matched controls; universal tiny-delta
economics, exclusive PE CPU and game FPS savings remain unproven.

[H301](present-pacing-pe-qualified-pin-output.301.md) closes the unused
qualified-pin result with five explicit arguments instead of six, preserving
wire output timing and every identity/retain/rollback guard. Five PE64 helper
spans shrink 528 to 496 bytes; both-width caller setup is smaller. Native/TLA
and 162 actual PE correctness cells pass. Higher raw candidate QPC/process
totals remain warnings in the uncalibrated drain-heavy fixture. This local
code removal does not prove CPU savings or performance neutrality.
Fresh 05/06 GT2 spots are 30.200/18.357 FPS, near README 30.1/18.1. Three
final 06 frames lack the full health snapshot; no FPS promotion follows.

[H300](present-pacing-pe-draw-parent-tail.300.md) splits the three tail calls:
GetData(FLUSH) takes 97.83–98.26% of outer-tail wall in four actual PE timing
processes, completing in one call per group. All 270 cells and 56,322 tail
supplements across ten processes qualify. This locates the expensive public
boundary, not its internal waits or PE-exclusive CPU. Fewer fixture-level
completion drains are a calibration candidate, not a production query change;
unused pin-output removal still needs its own correctness/economics gates.

[H299](present-pacing-pe-draw-parent-groups.299.md) separates every group from
its EndScene/query tail without changing production images. Ten PE processes
pass 270 cells and conserve 84,402 samples. Tails occupy 97.54–98.46% of the
two measured wall scopes, yet GetData completes in one call per group: the
fixture's retry sleep is not the cause. Central populations shift across
repeats, so outlier trimming or a median cannot qualify small helper savings.
Subdivide the three tail calls before altering fixture pacing; no new product
CPU/FPS or GPU-bottleneck claim follows.

[H298](present-pacing-pe-draw-parent-harness.298.md) extends actual public-parent
coverage to indexed draws and VS-F counts 3/4 without a production change.
The 27-cell fixture and strict ordered trace qualification pass in both PE
widths, but longer timing accumulation fails repeatability: draw-row ranges
are 14–273% on PE32 and 20–230% on PE64 across three same-image processes.
Small private-helper savings cannot be judged from this calibration. Group
interval distributions and drain/scheduling attribution precede the next
economics decision; the last wild FPS evidence remains H297.

[H297](present-pacing-pe-identity-call-abi.297.md) closes H295's dummy private
identity-call arguments: PE32 explicit arguments shrink 6 to 4 and PE64 caller
stack reservation shrinks 72 to 56 bytes. Generic live instruction counts and
the shared transition are preserved. Native/TLA and actual PE pixels pass.
Four-repeat public-COM costs remain mixed, including PE32 viewport/scissor
draw median +4.83% with overlapping ranges. Local removal does not establish
exclusive CPU savings, general neutrality or FPS promotion.
Fresh 05/06 GT2 spots are 30.132/18.242 FPS against README 30.1/18.1;
28 final 06 frames remain outside the full health snapshot.

[H296](present-pacing-pe-warm-pin-identity.296.md) retains a bounded warm-pin
identity refinement with cold pre-retain snapshot semantics unchanged. Real
PE32 warm spills disappear; PE64 stack reservation shrinks. Native/TLA and
actual pixel parity pass. Four-repeat public-COM ranges overlap but medians
remain mixed, including PE64 constant +9.96%; neither savings nor neutrality
is established. GT2 spots are 29.940/18.266 FPS against README 30.1/18.1,
with 57 final 06 frames missing full health counters. Whole-parent economics
and exclusive CPU reduction remain open.

[H295](present-pacing-pe-compact-identity-insertion.295.md) retains a compact
always-wire identity specialization sharing the generic transition. The actual
PE32/PE64 helper drops unused promotion stores and three branches, with no
parent frame growth. Native/TLA and actual PE pixels pass. Four-repeat public
COM draw costs overlap; selected 05/06 GT2 spots are 30.136/18.205 FPS, near
README, not an attributed gain. The final four 06 frames lack the full health
snapshot. Two focus-affected 05 attempts are explicitly excluded. Whole-parent
CPU savings remain unproven; H297 subsequently closes the narrower signature.

[H294](present-pacing-pe-public-com-parent.294.md) measures actual public COM
draw/state windows against preserved H293 images. Four traces prove no timed
recorder flush; eight logging-OFF processes pass bounded getter/FFP-pixel checks.
Actual parent costs are mixed, not the surrogate's uniform small-input penalty:
PE32 six-constant+draw grows 12.81%, while scalar rows improve with overlapping
ranges. H293 remains withdrawn; two final traces restore all baseline images.
Production cardinality/weighting, exclusive CPU and new FPS gains remain open.

[H293](present-pacing-pe-descriptor-inline.293.md) rejects a call-free,
fixed-granule compact descriptor loop. Dense-table matched-header append
improves, but small-table PE64 append grows 11–15%; PE32 small-input results
are much closer. Eight actual PE processes pass independent byte oracles.
The source change is fully withdrawn after native/TLA regression checks;
zero-section append increases are not attributed to copy execution, and no
candidate pixel or wild-FPS promotion follows. Whole production-parent cost
and a qualified dispatch policy remain separate, unproven questions.

[H292](present-pacing-pe-descriptor-table.292.md) rejects general variable-size
compact descriptor-table emission. PE32 one/two-section matched-header append
grows 8.87%/8.27%; PE64 grows 18.33%/17.01%. Dense indexed/UP cases improve
slightly, but neither fewer copies nor smaller helpers qualifies the general
replacement. Native byte/rollback tests are retained; the whole product-header
edit is withdrawn and no candidate GPU/wild promotion is claimed. H293's
fixed-granule follow-up also fails the general-replacement economic gate.

[H291](present-pacing-pe-current-hotpath.291.md) refreshes the current
`627d9840` PE32 profiles: owner admission/emission and constants remain
investigation targets, with cross-symbol samples unassigned and no CPU-time
claim. A separate, fresh 06 GT2 OFF spot is 18.217 FPS (README +0.64%), with
34 unreported full-health tail frames. Its descriptor-table candidate led to
H292's true multi-section economics; the earlier multi-texture fixture had only
one descriptor. Production is unchanged.

[H290](present-pacing-pe-private-admission-proof.290.md) keeps a narrowly
qualified duplicate source-proof removal. Seven warm matched-header append
shapes improve by 1.26–3.42% in PE32 and 2.09–8.68% in PE64, with native/TLA
and actual PE pixel parity. GT2 spots are 29.809/18.264 FPS (README -0.97%/+0.90%);
these do not establish a whole-game CPU/FPS gain. The final 34 06 frames are
outside the full eight-counter health snapshot. README remains unchanged.

[H289](present-pacing-pe-wire-publication-fusion.289.md) rejects the measured
compact first-use handle fusion. Sixteen PE32/PE64 writer/append processes
pass their oracles, as do native/TLA and actual PE pixel comparisons. Yet
the twenty-texture matched-header append grows 4.56%/8.69%; the loaded writer
also grows. These are distinct fixture costs, not actual production-parent
or exclusive-CPU timing. Runtime is withdrawn, tests retained, no new FPS claim.

[H288](present-pacing-pe-retainer-component-cost.288.md) separates controlled
index, expiry and non-final release costs using the current DLL's actual PE
bodies. Six processes pass with compiler-qualified real retainer storage and
real resources. Warm PE32 100-entry rebuild averages 1.705µs; interleaved
124→100 expiry averages 5.583µs. A hypothetical rebuild at every H287 epoch
is only 27/32µs per Present, not workload weighting, a bound or savings.
The larger in-situ diagnostic epoch cost remains independently unattributed.
No index rewrite, new FPS run or runtime promotion follows; whole-owner work
and repeatable CPU economy remain the larger investigation gate.

[H287](present-pacing-pe-retainer-epoch-shapes.287.md) separates sampled
retainer expiry from rollback tombstones without changing the default path.
About 94% of the 05/06 GT2 epoch samples rebuild after expiry; average sets
are about 124 entries, with 2048 index slots. No sampled tombstones or
invalid/overflow rows occur. Native 6+6, 384 exact candidate pixel samples
(OFF/ON combined), and switch-table-aware default code comparison pass.
OFF spots are 30.036/18.228 FPS; 06 has 53 unreported full-health tail rows.
Epoch timing still includes unseparated resource release, so neither these
counts nor single FPS spots establish a CPU saving or README recovery.

[H286](present-pacing-pe-binding-wire-authority.286.md) removes the binding
locator's duplicate wire ordinal: private witness authority and actual source
checks remain. Locator size falls 16→12 bytes, PE32 compact stack shrinks
184 bytes, and complete linked prepare/writer families shrink. Native 6+6
and 192 baseline-matched candidate PE pixels pass. Sixteen actual-DLL helper
processes price small savings, not whole-PE CPU economy: long PE32 binding
means fall 3.2–4.0 ns, empty costs 0.88 ns, and PE64 results are mixed.
Sampler-OFF GT2 spots are 29.607/18.176 FPS, with 16 unreported health-tail
frames in 06; no repeatable FPS recovery is established.

[H285](present-pacing-pe-stream-binding-reuse.285.md) removes repeated wrapper
qualification for the same strongly retained stream VB. Offset/stride changes
still perform the typed dirty transition; null/Recording/replacement retain
full validation. Native 6+4, TLA regression and 8+8 actual stream COM tests
pass, as do 20 exact baseline/candidate PE pixel observations. Eight actual-PE
micro processes save 5.7–18.8 ns per targeted PE32 call, with code growth and a
0.19 ns PE64 null-zero tax disclosed. Fresh sampler-OFF
05/06 GT2 spots are 29.735/18.181 FPS; no whole-frame economy or README recovery
claim follows. Separate full-self-PC profiles still identify owner admission
and constant handling, not stream setters, as the larger PE-local groups.

[H284](present-pacing-pe-layout-dynamic-economy.284.md) answers H283's
bounded warm-helper economics gate. Sixteen actual-image PE32/PE64 processes
pass; fresh GT2 observers retain 99% count-shape pricing coverage without
overflow. PE32 weighted QPC cost is 30.459→21.436 ns for 05 and
30.772→21.710 ns for 06, projecting only 0.0153/0.0171 ms per Present.
These are helper-loop measurements and dimensional projections, not in-situ
PE CPU or FPS savings. Native 9+4, 192 default-path pixels and scoped codegen
checks qualify the cold observer. Whole-workload CPU economy and README
recovery remain open; further layout-only work has a small measured budget.

[H283](present-pacing-pe-layout-category-rebase.283.md) replaces compact
layout's fixed category-count prepass with checked active-descriptor rebasing.
Native 7/7 + x86_64 4/4 and 192 actual-PE pixel samples pass. Linked call sites
and stack state shrink. Eight GT2 spots give FPS +0.342%/−0.253% and process
CPU/frame −0.121%/−0.030% for 05/06: descriptive, not attributed savings or
statistical neutrality. Retain the local work-removal implementation;
H284 subsequently prices bounded dynamic shapes, while README recovery remains open.

[H282](present-pacing-pe-scalar-writer-authority.282.md) qualifies local scalar
writer proof reuse but withdraws the runtime candidate: linked writer code
and PE32 stack grow, while eight GT2 spots show FPS +0.584%/−0.545% and
process CPU/frame −0.434%/+0.279% for 05/06. These small mixed deltas establish
neither savings nor regression. Native subset/failure pins and 192 exact
PE32/PE64 pixel samples remain; README and production runtime are unchanged.

[H281](present-pacing-pe-three-register-single-pass.281.md) removes the mismatch
rescan in a measured one-pass candidate. Eight actual-PE processes pass 528
cells and PE32/PE64 pixels match 192 baseline samples. Count-three changed
VS-F saves 1.0–2.5 ns on PE32, but count-one equal float/int costs 0.5–0.9 ns
more; PE64 also regresses companion lengths and adds stack state. Single-pair
05/06 spots show +1.31%/+0.55% FPS and -0.80%/-0.41% process CPU/frame,
not robust attribution or an all-family net-economy proof. Runtime is withdrawn;
additive source-boundary/dirty-settlement pins remain. README is unchanged.

[H280](present-pacing-pe-constant-economy.280.md) closes the bounded actual-PE
setter-economy harness gap. Eight matched-image processes pass 528 cells.
Count-three VS-F equal calls save 1.4 ns in both PE widths, while changed calls
cost 1.0–2.1 ns more on PE32 and 1.6–1.7 ns more on PE64. Wine's current-thread
timing fallback is process-scoped; the final harness uses explicit process CPU
and never claims PE/thread exclusivity. The runtime remains unchanged. H281
subsequently tests a one-pass replacement; it is not a demonstrated whole-frame
saving or README recovery.

[H279](present-pacing-pe-three-register-equality.279.md) tests then withdraws
a bounded three-register equality shortcut. Native dirty/byte refinement and
PE32/PE64 pixel differentials pass. Eight fresh GT2 runs show mean FPS changes
of -0.139%/+0.434% (05/06) and small process-CPU reductions, insufficient to
robustly price mismatch precomparison and PE32 reloads. H278 runtime is
restored; additive tests remain. The then-missing controlled actual-PE setter
economy gate is now implemented by H280; whole-workload savings remain open.

[H278](present-pacing-pe-vs-constant-call-shape.278.md) separates current VS-F
call equality from accumulated flush dirty spans. The bounded optional
observer and strict CSV parser retain exact lengths even when the old
shader-key table overflows, with Recording calls separate. 05/06 GT2 have
85.05%/79.73% three-register calls, 87.89%/93.05% of those equal. Default-OFF
public setters and
constant kernels are linked-identical after address normalization. These are
population facts, not a measured benefit; a three-register shortcut needs its
own PE32/PE64 differential/codegen and sampler-OFF economy gates.

[H277](present-pacing-pe-pin-probe-cursor.277.md) removes repeated remainder
arithmetic from owner pin-index probes without changing table/identity
authority. Production-bound native cursor/modulo differentials and existing
TLA pass. Linked compact retention bodies shrink from 743→598 instructions
(PE32) and 753→510 (PE64); this is work-removal evidence, not a CPU/FPS saving.
The initial miss-prefix reuse candidate was compiled away already and rejected.
Fresh Warn spots are 29.642/18.097 FPS and 69.688/95.388 process CPU ms/frame;
they do not demonstrate a material end-to-end saving. Separate exact-image PE
profiles still show constant and sparse-state work. README recovery and
repeatable CPU savings remain open.

[H276](present-pacing-pe-post-lifetime-profile.276.md) refreshes current-image
05/06 GT2 PE profiles and separate CPU envelopes. The commit-entry thread
occupies 0.950/0.966 cores, while `d3d9.dll` owns only 9.96%/5.97% of separate
stopped-PC prefixes. Constant handling and owner admission/materialization
remain visible; Present lookup is unpriced and an alias-only shortcut is
invalidated by ordinary RT changes. No runtime/default or README change is
made. A late launcher log append is detected, preserved and byte-prefix
qualified; the separate CPU runs disable that lingering focus helper.

[H275](present-pacing-pe-cached-com-lifetime.275.md) separates canonical cache
allocation credits from public COM refs. The Release-2 cycle is reproduced
with ordinary capture-OFF Present, then eliminated in twelve PE32/PE64
lifecycle runs. Four fresh OFF/ON captures now pass both teardown and exact
provider source/output oracles. Bounded native/ASan and existing TLA regression
pass. Post-commit default-mode spots are 29.648/18.115 FPS and
69.806/95.914 process CPU ms/frame for 05/06 GT2. They show no clear throughput
regression, but are not a paired neutrality/CPU-saving proof or exhaustive COM
concurrency evidence; inline remains default-off.

[H274](present-pacing-inline-consumers.274.md) completes eight same-image
Warn inline OFF/ON GT2 runs. Sample-weighted 05 FPS changes 29.413→30.124
(+2.42%) while process CPU/frame changes −0.49%; 06 changes 18.131→18.174
(+0.24%) with CPU +0.15%. Small-N counterbalancing is not a CPU-economy or
README-recovery proof. A new PE capture oracle exposes a variable-section
origin-lookup defect: the cold fix passes canonical negatives and four fresh
PE32/PE64 bundle source/output oracles. The known backbuffer wrapper cycle
left two device refs in that frozen evidence; H275 supplies the later scoped
fix. The inline candidate remains default-off.

[H273](present-pacing-pe-selected-masks.273.md) finds a real duplicate
PendingDelta mask read but rejects all three reuse forms on linked PE32
spill/address cost. Production is restored exactly, with stronger native
selection/failure tests. Fresh default-path Info GT2 baselines are 29.549/18.070
FPS and 31.284/52.825 ms inclusive commit-thread CPU per frame for 05/06.
These are diagnostic baselines, not PE-exclusive costs or README promotion.

[H272](present-pacing-segmented-preflight.272.md) implements the segmented
geometry/owned-semantics split. The eight-cell native acceptance checker,
post-copy rejection counterexample, protected spans, existing TLC, and 1,152
fresh PE pixel samples qualify the bounded change. Single Info GT2 pairs show
thread CPU −1.14%/−0.38%, process CPU −0.79%/−0.56%, and FPS +0.29%/+0.87%
for 05/06. This is directional, not noise-qualified saving or transport
promotion. Full error-counter coverage of 06's 26/16-frame tails and the
existing public OOM HRESULT contract remain open.

[H271](present-pacing-pe-transport-inline-boundaries.271.md) finds and fixes
the segmented empty-role token → unmaterialized ExactFixed fallback defect
before timing the lane. Native counterexample, 12 fresh PE32/PE64 pixel
processes, and existing TLC pass. High-register/INDEX16 UP samples strengthen
bounded coverage, not all consumers or default promotion. The transport
comparison must price synchronous Unix admission as well as removed PE gather.
The same-image single pairs show no savings: segmented inclusive thread CPU
is +0.81%/+0.42% for 05/06 and Info FPS −0.69%/−0.87%. This is not a
noise-qualified regression or promotion result. Its duplicate semantic-walk
candidate is implemented with bounded evidence in H272.

[H270](present-pacing-inline-constant-boundaries.270.md) closes the bounded
recorded-StateBlock/full-snapshot pixel matrix: 16 PE processes, 1,152 exact
samples. External cumulative CPU envelopes avoid dropping unstable interior
log observations. Eight retained Info GT2 diagnostics show only a small
inclusive CPU signal (about 1–1.5%); the user-contaminated 06 OFF run is excluded
and replaced. This does not promote inline constants or change runtime code.

[H269](present-pacing-pe-held-constant-value.269.md) rejects three held-value
constant-loop shapes. The measured candidate removes a changed-only reload
but adds a common PE32 source-argument read; 05/06 spots show no benefit.
Production code is restored with byte-identical PE `.text`. Stronger native
boundary tests and the rejection evidence remain; no CPU/FPS gain is claimed.

[H268](present-pacing-pe-native-thread-cpu.268.md) resolves an external
native commit-thread CPU view without a new PE probe or thread suspension.
05/06 GT2 consume 33.826/52.550 ms per joined frame and 0.944/0.965 cores;
the populations cover 65.4%/90.8% of scene frames. Game/Wine/synchronous Unix
are included, so PE-exclusive attribution remains open. These are Info
diagnostics, not FPS promotion or a product CPU saving.

[H267](present-pacing-pe-constant-loop.267.md) removes repeated backing-pointer
lookup and a redundant changed flag from the measured constant Live kernel.
The pointer-only prototype was rejected for worse PE32 code; the final
sentinel version shrinks both PE widths with native mask equivalence.
Actual 05/06 GT2 spots are 29.989/18.270 FPS (06 rerun; desktop-use run excluded);
partial process CPU/frame is 69.468/95.017 ms. Small FPS changes do not establish
gain or regression. Full profiles retain 1,322/825 PE samples; producer-thread CPU
attribution remains the next measurement gap, not a proven saving.

[H266](present-pacing-inline-constant-economy.266.md) repeats the current
inline OFF/ON mechanism with matching images: FPS changes +1.49%/+0.88%
for 05/06 GT2, but partial all-thread CPU/frame changes only +0.24%/-0.39%.
Both fixed-order single pairs retain similar CB/pass rates and zero reported
errors; they do not establish CPU savings, repeatable recovery or promotion.
Default stays OFF.

[H265](present-pacing-inline-constant-pixels.265.md) binds the real PE selector
to executable SM3 pixels and exact append receipts across both PE widths and
replay modes. All eight processes pass 576 sampled pixels. Omitting the
production inline VS-bool update fails the two predicted B pixels, while
standalone OFF and UP controls remain correct. No CPU/FPS gain or default
promotion is inferred; wider consumer/layout and economy gates remain open.

[H264](present-pacing-inline-constant-replay.264.md) adds a bounded real
producer→replay→Slot oracle: 256 route/mask executions, 1,024 ordered draws,
independent six-family bytes/counts and a failing production BOOL mutant.
Restored arm64 7/7 and x86_64 8/8 pass. This closes the stated native domain,
not PE env-selector/worker/GPU coverage or the policy's CPU/FPS economy.
Inline constants remain default-off.

[H263](present-pacing-pe-retain-fault.263.md) fixes a pre-existing loss of
injected retain HRESULT at the semantic-owner adapter. Twelve real PE fault
cases pass across both widths and inline modes. The final-draw/pixel and
repeatable-economy gates remain open; this cold correctness fix is not a
performance gain or authority to promote inline constants.

Current refinement [H262](present-pacing-pe-compact-binding-validation.262.md)
reuses admission identity validity without weakening full source equality or
physical pin validation. Native checks and the deliberately failing equality
mutant pass their expected verdicts. Final linked inspection and sampler-OFF
05/06 GT2 collection give 29.960/18.323 FPS and approximate partial process
CPU of 69.648/94.621 ms/frame. 06 uses the same-binary user-requested repeat,
excluding the original after reported computer use. Separate full profiles
retain 1,366/851 PE samples with zero sampler errors or omissions. Small spot deltas do
not prove gain, neutrality or regression; repeatable recovery and reduced
CPU remain open. Inline-constant effective-state/fault equivalence is a
useful next policy prerequisite, not authority to enable that lane.

The latest diagnostic [H261](present-pacing-pe-binding-duplicates.261.md)
separates null/unbound rows from duplicate pinned bindings. Actual 05/06 GT2
samples contain 39,570 compact preparations and 103,369 pinned occurrences,
with zero within-record duplicates in every family. Do not add the proposed
duplicate-validation cache. This does not prove unsampled records or other
workloads are duplicate-free, nor remove first-occurrence owner checks. No
FPS or CPU saving follows from these observer-enabled runs.

The latest implementation [H260](present-pacing-pe-validated-constant-range.260.md)
reuses public constant-range authority in the Live kernel without adding a
setter call or persistent state. Native negative controls, final linked PE
inspection and actual x86/x64 getter/StateBlock checks pass. Sampler-OFF
05/06 GT2 gives 29.810/18.277 FPS and approximate partial process CPU of
69.857/95.278 ms/frame, with zero renderer failures. The requested same-binary
06 repeat replaces its original perf run after reported computer use. Separate
full profiles retain 1,420/866 PE samples without sampler failure or omission. Small deltas
and imperfect partial joins do not establish gain or CPU reduction; repeatable
README recovery remains open. Current owner retention/source checks remain
authoritative, not removal candidates.

The preceding implementation [H259](present-pacing-pe-settlement-refinement.259.md)
reuses immutable constant presence during compact settlement and consolidates
duplicate current-ticket checks in a non-invalidating lexical interval.
Native bounded combinations, deliberate failing mutants and linked PE checks
pass without persistent storage growth; PE64's local stack grows 16 bytes.
Actual x86/x64 getter/StateBlock tests pass. Sampler-OFF 05/06 GT2 spots give
29.682/18.220 FPS and approximate partial process CPU of 70.153/95.472
ms/frame, with zero recorded renderer failures. The 06 value uses the
requested same-binary repeat after reported computer use; the original
06 perf run is excluded from performance decisions. Separate full profiles retain
all 1,430/854 PE samples without sampler failures or omitted buckets; symbol
ambiguity and incomplete scene edges remain. These spots do not establish
gain or CPU reduction. Both-target repeatable README recovery remains open.

The latest mechanism check [H258](present-pacing-inline-constant-current.258.md)
reopens inline constants on the current compact semantic owner: about 1,003
standalone constant records per Present become 0.9. Same-binary 05/06 GT2
OFF/ON spots give 29.631→30.000 and 18.188→18.354 FPS (+1.25%/+0.91%),
without demonstrated process CPU reduction. Diagnostic parent reductions are
not product savings: they include child clocks/reconstruction and a different
observed constant wrapper. Keep default-off; current effective-state, fault,
pixel and repeatable economy gates remain open. No runtime code or README
change follows.

The preceding optimization [H257](present-pacing-pe-constant-copy.257.md)
expresses changed 16-byte constants as one bounded copy; both linked PE images
use two SIMD instructions instead of eight scalar copy instructions. Native,
ABI and real x86/x64 setter/StateBlock checks pass. Sampler-OFF 05/06 GT2
spot checks are 29.052/18.083 FPS with zero recorded renderer failures.
The first 06 run was user-interfered and is excluded; its replacement rounds
to README 18.1, while 05 remains below 30.1. Separate full profiles now retain
all 1,378/872 PE samples with zero omission/overflow or sampler failures.
They locate remaining constant-loop and semantic-owner/settlement work, not
measured savings. CPU attribution and repeatable recovery remain open.
README is unchanged.

The preceding optimization [H256](present-pacing-pe-autogen-bound-slots.256.md)
elides immutable non-AUTOGEN slots before draw-time arrays/RTTI, with native
bounded refinement, x86/x64 PE pixels and a detected missing-Apply mutant.
Sampler-OFF 05/06 GT2 spot checks are 29.121/17.428 FPS versus README
30.1/18.1; both retain zero renderer failures and close CB/pass ratios.
06 has observed Finder/host contention. Neither attributed CPU/FPS gain nor
noise-qualified neutrality is demonstrated; the README is unchanged.

The preceding diagnostic [H255](present-pacing-pe-full-selfpc.255.md) closes
top-32 self-PC output truncation: full 05/06 GT2 reports conserve all
1,502/902 PE samples with zero omitted/overflow samples or sampler failures.
64-byte symbol ambiguity and elapsed CPU attribution remain open. Separate
sampler-OFF spot checks give 29.316/18.065 FPS, with zero recorded renderer
errors and close CB/pass ratios. 06 rounds to README 18.1; 05 remains below
30.1. Neither CPU savings nor repeatable recovery is demonstrated. Current
single-index retention lookup is necessary; remove only proven duplicate
work, not safety checks merely because their PC samples are visible.

The preceding optimization [H254](present-pacing-pe-constant-bitcompare.254.md)
vectorizes exact 16-byte constant equality in both linked PE images, with
native and ABI checks passing. Fresh 05/06 GT2 spot checks give 29.500/17.958
FPS, below README 30.1/18.1, with zero chunk/GPU/pipeline failures. Actual
250-Hz PE samples show no demonstrated compare-cost reduction: the loop moved
to another RVA, and neither partial bucket counts nor module shares establish
a runtime win. The leaf preserves partial process CPU/RSS observations;
matched baseline intervals and repeated performance attribution remain open.

The previous implementation [H253](present-pacing-pe-canonical-authority.253.md)
removes OFF constant-range queries, duplicates in source borrow authority and
paired owner pin indexes, and reuses strongly retained same-object Live
bindings. Four builds, native 39/39, x86_64 14/14 and unchanged TLA pass.
Cadence, ABI and queue policy stay unchanged. Storage/DLL savings and local
code/stack growth are recorded in the leaf. Post-commit GT2 is 29.009 sampled
FPS versus H251 29.034 (-0.084%), with zero chunk/GPU/pipeline failures and
close CB/pass ratios. The comparison includes H252 and H253; one run with
background CPU contention establishes no attributed gain or qualified
neutrality. Whole-scene correctness and current CPU attribution remain open.

The previous runtime checkpoint [H251](present-pacing-pe-append-effects.251.md)
skips absent compact emitters, closes constant settlement effects before
narrowing rollback, removes dynamic ledger TLS initialization checks and
reads fresh owner frontiers without reconstructing unused wire sizes. It adds
no persistent cache and changes no cadence, ABI or queue policy. Linked
constant inner frames shrink, while active-section dispatch bodies grow;
these are static work-shape findings, not a measured FPS gain. Post-commit
GT2 is 29.034 sampled FPS versus H250 28.846 (+0.650%), with zero
chunk/GPU/pipeline failures and close CB/pass ratios. One non-interleaved run
with differing background load does not establish gain or qualified
neutrality. Whole-scene correctness and current CPU attribution remain open.

The prior runtime checkpoint [H250](present-pacing-pe-sparse-work.250.md)
uses active-bit compact binding scans, pending-interval constant clearing and
existing-section quota traversal. Four builds, native 29/29, x86_64 8/8,
unchanged TLA and constant sanitizers pass. Linked PE32/PE64 eliminate the
initial iterator outlining without new mutable state; exports/owners match
and provider/bridge binaries are unchanged. Post-commit GT2 is 28.846 FPS
versus H249 28.835 (+0.038%), with zero chunk/GPU/pipeline failures and about
4 CB/Present. This single run, with background CPU load observed, establishes
neither an FPS gain nor qualified neutrality. Whole-scene correctness and
current PE bottleneck attribution remain open.

The prior runtime checkpoint [H249](present-pacing-pe-flush-layout.249.md)
removes disabled constant-flush work and repeated compact mask counting. Four
builds, native 29/29, x86_64 8/8 and the unchanged TLA suite pass; linked
PE32/PE64 writers lose six software-popcount expansions each. A hash-qualified
post-commit GT2 run gives 28.835 FPS versus prior H248 29.048 (-0.73%), with
zero chunk rejects, GPU errors or no-pipeline skips. CB/Present stays near 4
and passes/Present near 15.77. This previous-day single-run comparison does
not establish a speedup or an attributable regression; whole-scene pixel
evidence remains open.

The latest [identity diagnostic](present-pacing-pe-identity-distribution.248.md)
uses the verified `706866a4` binaries: 21,755 sampled compact preparations have
mean 2.588 unique identities / 4.494 comparisons, sampled maximum 17 / 136.
Keep the linear lookup; this is a distribution decision, not an FPS claim.
The historical PE CPU percentage below predates the semantic-owner changes
and must not be used to close current PE optimization work.

The earlier bounded refinement is
[compact transaction / live borrow authority](present-pacing-pe-compact-refinement.247.md).
Generation-qualified indexes, clean warm-index reuse, compact receipts, fused
binding pin/emission, and lexical source access pass native 21/21 and x86_64
8/8. PE32/PE64 compact append frames no longer probe. Observer-off GT2 pair
harmonic means are 27.884 baseline / 27.427 candidate (-1.64%), with zero
errors and stable CB/pass shape. The slower candidate is retained; substantial
same-build variation prevents a gain or neutrality claim. The physical/API
refinement is implemented, but FPS promotion remains open.

The preceding checkpoint is
[prepared emission / compact producer integration](present-pacing-pe-prepared-emission.244.md).
Prepared layouts, narrow constants, and direct canonical bindings pass native
and existing TLA checks. GT2 OFF is neutral (26.872 → 26.919 FPS), ON is
25.564 → 26.748, with zero errors and about 4 CB/P. Five sampled admission
children fall about 12.2%, but PendingDelta settlement grows and the inclusive
parent is neutral. These single runs do not establish FPS recovery or close
the broader post-v0.1.1 investigation. The earlier admission-reuse checkpoint
is preserved below for attribution.

For the earlier semantic-owner PE implementation, see the
[2026-09-05 admission-reuse evidence](present-pacing-pe-admission-reuse.243.md),
following [direct canonical payload](present-pacing-pe-direct-canonical.242.md).
Prepared pin/wire ordinals, hot family staging, and active-prefix settlement
pass native/TLA and GT2 OFF/ON checks. Sampled fixed/pin, sparse, and settlement
costs decrease; canonical construction is nearly unchanged. OFF FPS moves
27.304 to 27.527 (+0.82%) and ON 25.929 to 26.463 (+2.06%), with about 4 CB/P
and zero errors. These single runs do not establish a stable FPS gain.
Bounded dedup/mapping scans, warm expiry scanning, generic cold controls,
and contiguous gather remain. H247 supersedes the twelve ordinary pin-index
clears and redundant clean warm-index rebuilds. The earlier 1.96 MiB value-arena
removal is retained. Older PE cost rankings below describe earlier code, not
a current PE symbol profile or attribution of the post-v0.1.1 regression.

| # | Hypothesis | Verdict | Evidence |
|---|---|---|---|
| H211 | Readonly cache stacks with the promoted pair | accepted FPS promotion | [present-pacing-readonly-cache-stacking.198](present-pacing-readonly-cache-stacking.198.md) corrects H197's config-confounded reading (the probe wrapper pinned the promoted pair off, so the cache alone was already an offload-class `+11.2%`), then stacks all three mechanisms in one run: **`2,271` presents** (`+26.2%` cumulative vs `1,800`), zero GPU errors, `54.4` locks/present, offload replay `8.961ms`, `169.5` reordered hits/present. Worker idle drops `44.3 -> 39.0ms/present` — still producer-bound. |
| H212 | Post-cache producer re-sample: the wall is now the guest blob | accepted attribution | [present-pacing-postcache-resample.199](present-pacing-postcache-resample.199.md) re-samples promoted HEAD (`2,220` presents, trio live): guest blob `73.5%` (`~36ms/present`), wow64 layer `19.8%` (`~9.8ms` — the remainder is the game's own win32/syscall traffic, not dxmt9 calls), winemetal unix `1.4%`. dxmt9-named producer cost is `~3ms/present` of a `~49ms` wall; the only remaining dxmt9 CPU question is the PE `d3d9.dll` share inside the guest blob. |
| H213 | Decimated PE stats size the recorder core at ~8.5ms/present | accepted attribution | [present-pacing-decimated-pe-stats.200](present-pacing-decimated-pe-stats.200.md) verifies the perturbation-free `DXMT9_PE_STATS_DECIMATION` instrument: recorder core `8.5-8.9ms/present` = `16-18%` of the producer wall, with the const chain at `~5.1-5.6ms/present` giving an inline-const-delta wire change a `~+10%` FPS ceiling. |
| H214 | Inline const delta proves mechanism but lands inside the noise band | accepted mechanism confirm; FPS below gate; kept opt-in | [present-pacing-inline-const-delta.201](present-pacing-inline-const-delta.201.md) lands R-BACK-2.52 (`DXMT9_PE_INLINE_CONST_DELTA`, ABI v2): mechanism exact (append events `1,377 -> 743`/present, flush `4,456 -> 56`, recorder core `-2.8ms` measured), off-path tax nil, but FPS `+1.6%` — inside the noise band. Second instance of the H193 pattern: Rosetta-measured PE CPU-ms over-credits wall value by `~3x`. Kept opt-in default-off. |
| H215 | Archive prewarm hardening closes the startup-flake class | accepted reliability fix | [present-pacing-archive-prewarm-hardening.202](present-pacing-archive-prewarm-hardening.202.md) implements R-BACK-3.9..3.11 (`30bee79b`): async full prewarm with compile fallback, `DXMT9_ARCHIVE_MAX_PREWARM_MB` size guard, locked milestone save. The preserved `125MB` archive that deterministically self-aborted 3DMark05 now runs `status=pass`; probe FPS baselines are freed from hidden archive state. |
| H216 | The promoted trio becomes the engine default | accepted default promotion | [present-pacing-engine-default-trio.203](present-pacing-engine-default-trio.203.md) flips `DXMT9_OFFLOAD_COMMIT_REPLAY` to engine-default ON with the index-cache default following it (`d45af067`), after all three blockers fell (per-present boundary suppression + ordinal latency cap `72132513`, R-BACK-2.51(g)/(h); the native-spec harness drain gap that masqueraded as heap corruption `cad446ce`). Pure engine-default proof: pair env set nowhere, `2,220` presents, trio counters live, `present_boundary_skipped` exactly `1.0/present`, zero GPU errors. |
| H217-H220 | Rejected experiment lanes removed from the tree | accepted cleanup waves | Four removal waves delete every rejected carrier whose reopen premise died with the engine-default offload (H195) and the H212 game-CPU attribution: open-CB/tail-present/split-present family + `DXMT9_FS_HALF_PRECISION` (`6379d5c8`, orphan sweep `a083bc8f`), draw-run preflush merge/mixed-carrier (`92047c4e`), chunk-end carry + the `AndRun`/`WithResourceMarking` carrier family + chunk-end flush probe (`570a5cde`, `04c9a827`), compact uniform submission carrier, canonical draw-run fast path, legacy publish-time PSO prefetch pair, and PE flush-after-clear/draw pacing probes (`bb1bec1d`, `c33d250a`, `8d16f290`, `f1224bdf`). Cooled GT1 smokes stay at `2,280` presents / zero GPU errors with the trio live. Every opt-in lane left is a live-default diagnostic A/B switch or an open frontier. See the [log](log.md) H217-H220 rows for per-wave detail. |
| H221 | Probe-wrapper defaults aligned with the promoted engine defaults | accepted policy change | `run_3dmark05_perf_probe.sh` now pins `DXMT9_OFFLOAD_COMMIT_REPLAY` / `DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE` to `1` by default like the engine (`e5129346`); env `0` is the off switch. Probe `result.json` baselines after 2026-07-12 are trio-on by default; reproduce historical default-off recipes by passing `0` explicitly. |
| H236 | Current GT2 first ceiling is the saturated producer thread, but PE `d3d9.dll` is only 10.6% | accepted current attribution | [present-pacing-current-bottleneck-pe-symbol.236](present-pacing-current-bottleneck-pe-symbol.236.md) joins a clean current-cap run, a ten-second Time Profiler interval, a Metal stage sidecar, and the Tier 2 PE sampler. Producer utilization is one full core versus encode `60%` and replay `57%`; PE module shares are game `64.3%`, `winemetal.dll` `13.9%`, and `d3d9.dll` `10.6%`. The PE module has no single dominant remaining leaf. |
| H237 | CPU-ready Tape now completes but still misses strict locality | promotion rejected; implementation remains default off | [present-pacing-cpu-ready-tape-promotion-gate.237](present-pacing-cpu-ready-tape-promotion-gate.237.md) records a clean native/formal/build/triangle gate and a completed same-build GT2 pair. Ready depth moves `1.000 -> 2.200`, but CB/pass/tile rise `0.31%`/`0.73%`/`2.93%`; the next owner is incomplete retained cross-source suffix attribution, not capacity or another FPS-only run. |
| H238 | Active-session lookahead closes every locally provable hold, but not the locality gate | implementation and proof complete; promotion rejected | [present-pacing-cpu-ready-active-head-locality.238](present-pacing-cpu-ready-active-head-locality.238.md) adds active-seed-preserving whole-head retention, carried terminal-suffix ownership, native tests, and a bounded TLA model. The final GT2 pair still raises CB/pass/tile by `0.25%`/`1.21%`/`4.70%`. Exact rejection conservation attributes all non-held heads to Present (`1,245`) or absence of a future Writing identity (`227`); the next owner is an atomic next-source intent contract, not another local wait policy. |
| H239 | Exact replay-FIFO next-source intent closes the identity contract, but not locality | implementation and proof complete; promotion rejected | [present-pacing-cpu-ready-next-source-intent.239](present-pacing-cpu-ready-next-source-intent.239.md) binds adopted raw, Direct disposition, cancellation, generation-stamped publication, and wake progress. GT2 selects the intent zero times and still raises CB/pass/tile by `0.34%`/`1.15%`/`4.40%`. Exact late-action accounting resolves `10,770/11,038` unknown actions to Store, moving the remaining owner to bounded replay/store-action equivalence. |

## Current Frontier

The engine-default trio is promoted and the dead lanes are gone; the honest
remaining branches are:

- **Producer thread:** current-image H276 05/06 GT2 commit-thread envelopes use
  0.950/0.966 cores. Separate stopped-PC prefixes are mostly game code
  (69.14%/78.96%); PE owns 9.96%/5.97%. These are different measurements, not
  additive PE-exclusive CPU budgets. Constant/owner work remains a bounded
  target; no new single large PE leaf or complete optimization ceiling is proved.
- **Bridge/resource updates:** current H276 bridge PCs own 11.53%/8.76% of
  the respective stopped-PC prefixes. Remove measured work or crossings,
  preserving mutation/lifetime ordering; a PC at the bridge does not price
  bridge-local work versus synchronous Unix/wait time.
- **Replay and encode second ceilings:** H312's current diagnostic 05/06 GT2
  windows use 0.39–0.54 replay cores and 0.41–0.56 encode cores, with substantial
  input waits. App-thread supply (0.94–0.96 cores) is the first attribution
  target; this is not PE-exclusive. Replay snapshot/materialization elimination
  remains a CPU/latency target, but fixed-arrival sensitivity does not close
  its arrival-changing FPS ceiling. Parallel render encoding stays non-default.
- **CPU-ready Tape:** the publication, admission, session, active-head, and
  next-source-intent mechanisms are implemented and bounded by native/TLA
  evidence, but H239 still fails CB/pass/tile locality. The remaining design
  question is frame-wide sealing versus an incremental store-action
  equivalence proof; the provider remains default off.

## Current Navigation

- [Domain index](index.md)
- [Historical log](log.md)
- [Root 3DMark05 GT1 map](../overview-3dmark05-gt1.md)

## Recent Leaf Documents

- [present-pacing-pe-admission-reuse.243 - Qualified Pin/Wire Reuse and Active-Prefix Staging](present-pacing-pe-admission-reuse.243.md)
- [present-pacing-pe-prepared-emission.244 - Prepared Layouts, Narrow Constants, and Compact Source Admission](present-pacing-pe-prepared-emission.244.md)
- [present-pacing-pe-direct-canonical.242 - Direct Canonical Value Payload](present-pacing-pe-direct-canonical.242.md)
- [present-pacing-cpu-ready-next-source-intent.239 - Replay-FIFO Intent Is Safe but Does Not Recover Tape Locality](present-pacing-cpu-ready-next-source-intent.239.md)
- [present-pacing-cpu-ready-active-head-locality.238 - Active-Session Lookahead Closes the Local Hold Gap but Not Tape Locality](present-pacing-cpu-ready-active-head-locality.238.md)
- [present-pacing-cpu-ready-tape-promotion-gate.237 - CPU-Ready Tape Restores Progress but Misses the Locality Gate](present-pacing-cpu-ready-tape-promotion-gate.237.md)
- [present-pacing-current-bottleneck-pe-symbol.236 - Current GT2 Ceiling Is The Producer Thread; PE d3d9.dll Is 10.6%](present-pacing-current-bottleneck-pe-symbol.236.md)
- [present-pacing-engine-default-trio.203 - The Promoted Trio Becomes The Engine Default](present-pacing-engine-default-trio.203.md)
- [present-pacing-archive-prewarm-hardening.202 - Archive Prewarm Hardening Closes The Startup-Flake Class](present-pacing-archive-prewarm-hardening.202.md)
- [present-pacing-inline-const-delta.201 - Inline Const Delta Proves Mechanism But Lands Inside The Noise Band](present-pacing-inline-const-delta.201.md)
- [present-pacing-decimated-pe-stats.200 - Decimated PE Stats Size The Recorder Core At ~8.5ms/present](present-pacing-decimated-pe-stats.200.md)
- [present-pacing-postcache-resample.199 - Post-Cache Producer Re-Sample - The Wall Is Now The Guest Blob](present-pacing-postcache-resample.199.md)
- [present-pacing-readonly-cache-stacking.198 - Readonly Cache Stacks With The Promoted Pair For +26% Cumulative](present-pacing-readonly-cache-stacking.198.md)
- [present-pacing-readonly-managed-buffer-cache.197 - Readonly Managed Buffer Cache Collapses The Lock Bridge Storm](present-pacing-readonly-managed-buffer-cache.197.md)
- [present-pacing-producer-sampling-attribution.196 - Non-Perturbing Producer Sampling Finds The Buffer-Lock Bridge Storm](present-pacing-producer-sampling-attribution.196.md)
