---
domain: present-pacing
workload: 3DMark05 GT1
title: "Present-Pacing — display sync, frame latency, and the wallclock cap - Current Overview"
type: domain-overview
status: current
updated: 2026-09-30
source: docs/perfomance/present-pacing/log.md; docs/perfomance/overview-3dmark05-gt1.md; docs/perfomance/present-pacing/present-pacing-current-bottleneck-pe-symbol.236.md; docs/perfomance/present-pacing/present-pacing-cpu-ready-next-source-intent.239.md; docs/perfomance/present-pacing/present-pacing-pe-direct-canonical.242.md
related: docs/perfomance/present-pacing/index.md; docs/perfomance/present-pacing/log.md
---

# Present-Pacing — display sync, frame latency, and the wallclock cap - Current Overview

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
