---
domain: present-pacing
workload: 3DMark05 GT1
title: "Present-Pacing — display sync, frame latency, and the wallclock cap - Current Overview"
type: domain-overview
status: current
updated: 2026-09-17
source: docs/perfomance/present-pacing/log.md; docs/perfomance/overview-3dmark05-gt1.md; docs/perfomance/present-pacing/present-pacing-current-bottleneck-pe-symbol.236.md; docs/perfomance/present-pacing/present-pacing-cpu-ready-next-source-intent.239.md; docs/perfomance/present-pacing/present-pacing-pe-direct-canonical.242.md
related: docs/perfomance/present-pacing/index.md; docs/perfomance/present-pacing/log.md
---

# Present-Pacing — display sync, frame latency, and the wallclock cap - Current Overview

> Current, compact view for this performance domain. Historical detail from the former
> top-level `present-pacing.md` overview is preserved in [log](log.md). Domain landing: [index](index.md).

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
