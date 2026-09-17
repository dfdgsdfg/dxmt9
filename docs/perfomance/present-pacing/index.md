---
domain: present-pacing
workload: 3DMark05 GT1
title: "Present-Pacing — display sync, frame latency, and the wallclock cap"
type: domain-index
status: current
updated: 2026-09-17
source: docs/perfomance/present-pacing/present-pacing-pe-constant-bitcompare.254.md; docs/perfomance/present-pacing/present-pacing-pe-canonical-authority.253.md; docs/perfomance/present-pacing/present-pacing-pe-append-effects.251.md; docs/perfomance/present-pacing/present-pacing-pe-sparse-work.250.md; docs/perfomance/present-pacing/present-pacing-pe-flush-layout.249.md; docs/perfomance/present-pacing/present-pacing-pe-identity-distribution.248.md
related: docs/perfomance/present-pacing/overview.md; docs/perfomance/present-pacing/log.md
---

# Present-Pacing — display sync, frame latency, and the wallclock cap

[H318](present-pacing-policy-consolidation.318.md) makes known-setter uniform
invalidation and bounded lock-scope refinement unconditional, removing their
legacy runtime alternatives. Unpublished-slot prefetch and secondary cbuf
content hashing are retired with exclusive metadata/counters. Independent
test references remain; structural acceptance is not a new FPS claim.

[H317](present-pacing-query-end-coalescing.317.md) adds default-OFF deferred
END groups, exact writer-to-publication binding and split attribution. Bounded
native/TLA/GPU and x64/x86 Wine gates pass; matched wild locality and stable
non-regression remain promotion gates. The old/current absolute FPS comparison
has severe same-image temporal drift and does not isolate a code delta.

[H316](present-pacing-query-default-promotion.316.md) promotes asynchronous
query completion to default ON, with explicit OFF as the synchronous reference.
Production upload-reaper fault tests, initializer-only exact GPU bytes,
default-mode Wine x64/x86 and visible same-image STALKER repetitions qualify
the bounded path. Actual submission now retires query splitting pressure;
END itself remains eager. Real Metal query-data providers and deferred END
coalescing are outside H316; H317 tracks the latter candidate.

Historical [H315](present-pacing-query-publication-fences.315.md) implements a default-off
actual-prefix query fence with persistent FLUSH submission and separate GPU
completion. Native/model binding and x64/x86 delayed-completion pixel probes
pass; visible wild/locality and initializer fault coverage remain promotion
gates. Core query result-data providers are not upgraded by this fence.

[H314](present-pacing-stalker-query-waits.314.md) establishes an unchanged-image
STALKER Day retry and separate query-wait diagnostic. Query crossing and queue
completion waits dominate replay-drain time; partial app CPU is 0.449 cores.
Actual END-to-queue identity and public nonblocking behavior need investigation.
Missing onscreen captures prevent visual/focus qualification and FPS promotion.

[H313](present-pacing-app-thread-attribution.313.md) records current-image
game/Wine-system/PE/bridge guest-PC populations. Game PCs dominate, but saved
bridge contexts and unreliable Rosetta unwinds leave exclusive CPU attribution
unclosed. Commit/import and buffer Lock/Unlock are the next synchronous
boundary targets; failed Time Profiler evidence is preserved and excluded.

[H312](present-pacing-cpu-supply-critical-path.312.md) finds upstream app-thread
supply near one core while replay/encode are not saturated in four 05/06 GT2
diagnostic windows. Empty-input waits dominate reservation/dispatch delay.
Fixed-arrival replay sensitivity mostly reduces latency; PE-exclusive cost,
feedback and the unconditional FPS ceiling remain open. Runtime defaults and
README are unchanged.

Historical [H311](present-pacing-lock-scope-attribution.311.md) reruns the exact H309
provider and H310 on 06 GT2. Both recover to about 18 FPS, so the earlier large
decline cannot be independently attributed to the lock candidate or its payload
recount. Actual batch size/duplicate/flush-cause economics remain open. H311
kept default OFF; H318 subsequently removes the opt-out by explicit structural
acceptance, without claiming those economic gates closed. README is unchanged.

[H310](present-pacing-payload-count-batch-cost.310.md) removes the candidate's
second payload-count walk using a checked source-local snapshot byte delta.
Native/runtime tests and TLC pass; batch bookkeeping is investigated separately
from GT2's unknown batch distribution. Default OFF and README are unchanged.

[H309](present-pacing-lock-scope-wild.309.md) runs matched-image 05/06 GT2
OFF/ON repeats and 06 lock diagnostics. Range queue acquisitions disappear
and non-app arena update acquisitions fall, but both 06 perf pairs are slower
amid large repeat drift. Non-regression is not established; keep default OFF.

[H308](present-pacing-lock-scope-refinement.308.md) implements the three bounded
lock-scope candidates behind default-off `DXMT9_LOCK_SCOPE_REFINEMENT`.
Native tests pin real range bytes under queue contention, exact captured
stamps and final-Slot parity; H309 supplies the later runtime/economy evidence.

[H307](present-pacing-lock-contention-audit.307.md) identifies the next lock
targets without a new run: range upload holds the queue mutex despite using
no queue watermark; snapshot/override/publication walks take individual arena
locks beyond the logical-handle batch path. H306 has 20.7k/22.6k non-app arena
updates/P. Exact duplicate fraction and waiter-to-holder causal attribution
remain open; preserve captured-backing identity and final-sequence stamping.

[H306](present-pacing-upload-arena-locks.306.md) separates upload/arena
acquisition from hold with a default-off observer. In 05/06 GT2, app upload
acquisition is 1.123/0.905 ms/P and hold 0.418/0.382 ms/P. Arena operations
are much more frequent but individually short; nested times are not additive.
Native/TLA and four diagnostic spots pass their reported gates; no lock-removal
or FPS promotion is established, and 06 health tails remain explicit.

[H305](present-pacing-app-thread-waits.305.md) measures native application CPU:
05/06 GT2 use 0.924/0.954 cores in partial CPU-only diagnostic envelopes.
Upload queue-mutex and buffer-arena waits recur in separate stack samples;
H306 subsequently splits their durations; PE-exclusive CPU remains unmeasured. Replay input wait is
not application GPU wait. Products and README are unchanged.

[H303](present-pacing-replay-baseline.303.md) establishes 05/06 GT2 replay
service baselines: 22.33/23.74 ms per Present in explicit scene subwindows,
not native CPU. Input waits remain large; diagnostic planning is 0.48/0.40 ms.
Investigate compatibility state/materialization and final-Slot append next.
Products are unchanged; lower FPS spots are not a code-regression attribution.

[H302](present-pacing-pe-completion-window.302.md) adds bounded fixture-only
completion windows. Thirty PE processes pass 810 correctness cells; all five
product images are unchanged. Window 32 reduces median draw-mean repeat
spread to 0.653%/0.561% on PE32/PE64, but worst cells still reach 8.755%/6.970%.
Use explicit matched-shape controls, not a universal small-delta or FPS claim.

[H301](present-pacing-pe-qualified-pin-output.301.md) removes the discarded
qualified-pin output and optional-pointer branch. Linked PE32/PE64 work shrinks;
native/TLA and public-COM correctness checks pass. Raw parent/process totals
are higher in the uncalibrated completion-heavy fixture; reduced CPU cost and
performance neutrality are not established.
Fresh 05/06 GT2 spots are 30.200/18.357 FPS; the final three 06 frames lack
full-health coverage. README is unchanged.

[H300](present-pacing-pe-draw-parent-tail.300.md) attributes 97.83–98.26% of
the fixture's tail to its single GetData(FLUSH) call, not EndScene or retry
sleep. Internal wait/CPU attribution and calibrated small-helper economics
remain open; production images and README are unchanged.

[H299](present-pacing-pe-draw-parent-groups.299.md) retains 84,402 exact group
samples. EndScene/query tails dominate this fixture's measured scopes, with
one GetData call per group; broad timing-population shifts remain. This is
diagnostic evidence, not PE/FPS improvement or calibrated helper economics.

[H298](present-pacing-pe-draw-parent-harness.298.md) checks in the public COM
draw-parent harness with indexed and count-3/4 constant coverage. State/pixels,
ordered no-flush traces and strict parsing pass. Three same-image repeats per
PE width remain highly variable: this is functional evidence, not calibrated
small-delta economics. Production images and README remain unchanged.

[H297](present-pacing-pe-identity-call-abi.297.md) removes two unused compact
identity-call arguments while preserving the shared generic transition.
Native/TLA and actual PE32/PE64 pixel parity pass; four-repeat public-COM costs
remain mixed. Smaller argument/frame setup is not a whole-parent CPU saving
or a performance-neutrality proof.
05/06 GT2 spots are 30.132/18.242 FPS, with 28 uncovered full-health tail
frames in 06; no FPS promotion follows.

[H296](present-pacing-pe-warm-pin-identity.296.md) removes warm-pin identity
materialization while preserving the cold pre-retain snapshot. Native/TLA,
exact bytes/refcounts and both-width PE pixels pass. Public-COM economics are
mixed; 05/06 GT2 spots are 29.940/18.266 FPS, with 57 uncovered full-health
tail frames in 06. Retained local code reduction is not CPU/FPS promotion.

[H295](present-pacing-pe-compact-identity-insertion.295.md) removes compact
identity insertion's unused wire-promotion output. Native/TLA and both-width
PE pixels pass; four-repeat public-COM costs overlap. Clean GT2 spots are
30.136/18.205 FPS, with four unreported full-health tail frames in 06.
The local change stays; CPU/FPS savings and performance neutrality are unproven.

[H294](present-pacing-pe-public-com-parent.294.md) adds actual loaded public
COM draw-parent economics. Eight logging-OFF processes and four boundary traces
pass, but costs are mixed; H293's surrogate percentages are not production
draw overhead. The candidate stays withdrawn, all baseline images are restored,
and no new CPU/FPS gain is claimed.

[H293](present-pacing-pe-descriptor-inline.293.md) rejects the fixed-granule
follow-up as a general replacement. Inline copies remove H292's external-copy
call, but PE64 small-table append still costs 11–15% more; dense cases improve.
Four builds, native/TLA regression and eight actual PE processes pass their
correctness gates. Product changes are withdrawn; no new FPS claim follows.

[H292](present-pacing-pe-descriptor-table.292.md) rejects an unconditional
variable-size compact descriptor-table copy. Eight PE32/PE64 processes pass
byte oracles, but one/two-section matched-header append regresses 8–18%; dense-table
gains do not qualify it. Product changes are withdrawn; stronger native
coverage remains. H293 supplies the fixed-granule follow-up's separate economics.

[H291](present-pacing-pe-current-hotpath.291.md) refreshes exact-image PE32
profiles at `627d9840` and separately remeasures 06 GT2 at 18.217 FPS.
Owner/constant work remains visible; its descriptor-table candidate is
subsequently priced and rejected by H292. Production is unchanged.

[H290](present-pacing-pe-private-admission-proof.290.md) removes a duplicate
private compact source qualification while keeping envelope and writer checks.
Seven warm append shapes improve in actual PE32/PE64 fixtures; native/TLA/pixels
pass. GT2 spots are 29.809/18.264 FPS, not a repeatable FPS-gain claim.

[H289](present-pacing-pe-wire-publication-fusion.289.md) withdraws compact
first-use handle publication. Native/TLA and PE pixels pass, but 16 bounded
PE processes show higher binding costs, including the separate matched-header
append fixture. Production and README stay unchanged; stronger tests remain.

[H288](present-pacing-pe-retainer-component-cost.288.md) prices the actual
loaded retainer bodies without changing production: warm PE32 100-entry
rebuild is 1.705µs and interleaved 24-entry expiry is 5.583µs. Six PE32/PE64
processes pass. This prices controlled fixtures, not GT2 CPU or savings;
index/release rewrites and repeatable whole-PE economy remain unproven.

[H287](present-pacing-pe-retainer-epoch-shapes.287.md) supplies conserved
retainer epoch samples: expiry, not rollback tombstones, dominates the
observed GT2 rebuilding. Native, actual PE pixels and unchanged default-code
qualification pass. OFF FPS spots are 30.036/18.228; isolated index economy
and repeatable whole-PE savings remain open.

[H286](present-pacing-pe-binding-wire-authority.286.md) makes the prepared
identity witness the sole local wire-ordinal authority. Smaller locator/stack
and linked helper families retain native and real-PE pixel equivalence.
Actual-DLL micro measurements show small PE32 binding savings but an empty
path cost and mixed PE64 results; no whole-frame economy claim follows.

[H285](present-pacing-pe-stream-binding-reuse.285.md) reuses the strong Live
VB's membership qualification in `SetStreamSource`, preserving typed layout
updates. Eight actual-PE micro processes show lower same-VB setter cost;
the linked body grows and PE64 null-zero costs 0.19 ns more. Candidate 05/06
GT2 spots are 29.735/18.181 FPS, not a repeatable CPU/FPS gain. Fresh PE
profiles retain owner/constant work as larger remaining investigation targets.

[H284](present-pacing-pe-layout-dynamic-economy.284.md) measures the actual
H283 DLL helper and weights it by fresh GT2 compact-preparation shapes.
PE32 helper wall cost falls about 29.5% over the priced sample mass, but the
projected saving is only 15–17 microseconds/Present. A cold bounded observer,
native 9+4 and 192 default-path pixels pass; no whole-frame CPU/FPS claim.

[H283](present-pacing-pe-layout-category-rebase.283.md) removes compact
layout's initial category-count walk with alignment-proven descriptor rebasing.
Native layout/transaction differentials and actual PE32/PE64 pixels pass;
linked call sites and stack usage shrink. Whole-workload CPU economy is a
separate gate from this call-local work removal.

[H282](present-pacing-pe-scalar-writer-authority.282.md) tests scalar writer
admission-proof reuse. Eight GT2 spots give mixed economy while linked writer
code grows; runtime is withdrawn. Thirty-two native subset/rollback cases
and PE32/PE64 pixel evidence remain. The duplicate layout category walk is
the next unproven work-removal candidate.

[H281](present-pacing-pe-three-register-single-pass.281.md) prices a one-pass
three-register update: changed calls improve, but non-three float/int calls
regress. Native and PE pixels pass; favorable single-pair GT2 spots do not
establish net economy. Runtime is withdrawn; stronger boundary/dirty pins remain.

[H280](present-pacing-pe-constant-economy.280.md) adds a qualified actual-PE
setter economy harness. Eight PE32/PE64 processes price H279's equal-path
saving and changed-path tax. An audited Wine thread-time/process-time fallback
is corrected; the harness explicitly reports process CPU. No runtime promotion.

[H279](present-pacing-pe-three-register-equality.279.md) withdraws the exact
three-register equality candidate after eight fresh GT2 runs. Native and
PE32/PE64 pixels pass, but small process-CPU trends do not robustly price the
extra fallback work. H278 runtime is restored; tests and evidence remain.

[H278](present-pacing-pe-vs-constant-call-shape.278.md) adds a cold-only,
exact-length VS setter population. Default-disabled PE32/PE64 setter and
constant-kernel code stays unchanged. Both GT2 distributions favor testing
a three-register equal shortcut, not a general long-range scan; no runtime
shortcut or CPU/FPS promotion follows from diagnostic counts.

[H277](present-pacing-pe-pin-probe-cursor.277.md) replaces per-probe modulo
with an equivalent cyclic pin-index cursor. Bounded native and linked PE32/
PE64 evidence qualify the arithmetic change. Fresh 05/06 Warn spots are
29.642/18.097 FPS, without demonstrated end-to-end CPU savings; separate PE
profiles preserve the next constant/owner investigation targets.

[H276](present-pacing-pe-post-lifetime-profile.276.md) refreshes current-image
PE attribution and separate native-thread CPU: 05/06 GT2 commit threads use
0.950/0.966 cores. Constant/owner work remains visible; new Present-cache and
size-guard deletions are not approved by these samples. No runtime promotion
or README FPS update is made.

[H275](present-pacing-pe-cached-com-lifetime.275.md) fixes the canonical cache /
public COM-reference cycle, including ordinary capture-OFF Present. Bounded
PE32/PE64 lifecycle and capture/provider pixel checks pass with Release zero;
broader concurrency and failed-Reset proofs remain open.

[H274](present-pacing-inline-consumers.274.md) repeats inline OFF/ON economy
with eight same-image Warn GT2 runs: 05 FPS +2.42%, process CPU/frame −0.49%;
06 +0.24%/+0.15%, without a repeatable CPU saving. A cold capture-origin fix
passes native rejection pins and four exact PE32/PE64 fresh-provider oracles;
that frozen capture teardown failure is subsequently fixed by H275. The inline
constant default stays OFF.

[H273](present-pacing-pe-selected-masks.273.md) rejects three compact mask-reuse
forms that trade the removed source reread for added PE32 spills. Original runtime
remains; 108 native selection cases and fresh default-path GT2 CPU baselines
are retained. No candidate Wine run or economy claim is made.

[H272](present-pacing-segmented-preflight.272.md) removes the borrowed
segmented semantic walk while retaining bounded geometry and complete owned
validation. Native failure/conservation and 1,152 fresh PE pixel samples pass.
Initial thread CPU deltas are −1.14%/−0.38% for 05/06 GT2; repeatable economy
and default promotion remain open.

[H271](present-pacing-pe-transport-inline-boundaries.271.md) fixes a real
segmented empty-role/readback failure and adds high-register/indexed-UP pixel
evidence. Twelve fresh post-fix processes pass; both candidates remain off by
default and CPU economy is a separate gate.

[H270](present-pacing-inline-constant-boundaries.270.md) adds actual PE
StateBlock/full-snapshot inline-constant pixels and continuous external CPU
envelopes. The candidate remains default-off; records alone do not prove savings.

[H269](present-pacing-pe-held-constant-value.269.md) rejects held-value
constant fusion: smaller PE symbols add an equal-path argument reload; GT2
spots show no economy benefit. Original runtime is retained, with stronger
native bit/guard-page tests and separate equal/changed-path acceptance.

[H268](present-pacing-pe-native-thread-cpu.268.md) adds externally queried
native commit-thread CPU: 05/06 GT2 occupy 0.944/0.965 cores in qualified
partial intervals. This includes game/Wine/synchronous Unix, not PE-exclusive
time. Diagnostic-only; no runtime binary, README or provider-default change.

[H267](present-pacing-pe-constant-loop.267.md) reduces the fixed-width Live
constant loop to call-local backing borrows and one first-changed reduction.
Both linked PE widths shrink; native dirty/change-mask refinement passes.
Current GT2 spots are 29.989/18.270 FPS (06 rerun; desktop-use run excluded);
full stopped-PC reports preserve 1,322/825 PE samples. Small FPS changes and
partial all-thread CPU changes do not establish savings or repeatable recovery.

[H266](present-pacing-inline-constant-economy.266.md) measures current-image
OFF/ON spots after the pixel prerequisite: 05 GT2 29.787/30.230 FPS and
06 GT2 18.266/18.428. Partial all-thread CPU changes +0.24%/-0.39%, so reduced
CPU consumption and default promotion remain unproved despite fewer records.

[H265](present-pacing-inline-constant-pixels.265.md) adds real PE32/PE64
OFF/ON and synchronous/worker pixel checks: 576 exact sampled pixels,
qualified record counts and a deliberately failing production inline-BOOL
mutation with a passing OFF control. UP remains a standalone control.
Wider boundaries and repeatable CPU economy remain open; default stays OFF.

Native policy prerequisite [H264](present-pacing-inline-constant-replay.264.md)
connects inline/standalone owner emission to final draw bytes across 64
constant-family masks and both replay routes. A dropped production BOOL delta
fails deterministically; restored arm64/x86_64 tests pass. Its native scope
complements H265's bounded pixels; broader consumer boundaries and repeatable
CPU savings remain open.

Correctness prerequisite [H263](present-pacing-pe-retain-fault.263.md) restores
the injected retain HRESULT through owner rollback. Twelve PE32/PE64 Wine
fault/retry cases pass across inline OFF/ON. This proves public error/Live
state behavior, not final draw pixels, an inline default change or CPU savings.

Current refinement: [H262](present-pacing-pe-compact-binding-validation.262.md)
removes duplicate compact typed-ref validation and per-row range lookup,
retaining full source/pin/wire checks. Native counterexamples and the
deliberately failing equality mutant pass their expected verdicts. Scoped
linked inspection and sampler-OFF 05/06 GT2 collection are complete at
29.960/18.323 FPS; 06 uses the same-binary user-requested repeat, excluding
the original after reported computer use. Separate profiles retain all
1,366/851 reported PE samples. Small single-run deltas do not establish FPS
or CPU savings.

Latest candidate decision: [H261](present-pacing-pe-binding-duplicates.261.md)
adds cold null/first/duplicate binding counts. Actual 05/06 GT2 samples contain
103,369 pinned occurrences and no within-record duplicates, so a duplicate
validation cache is rejected for the observed populations. No hot validation
is removed; this is not new FPS evidence.

Latest implementation: [H260](present-pacing-pe-validated-constant-range.260.md)
reuses public constant-range validation in the Live kernel. Native negative
controls, final linked PE32/PE64 inspection and actual Wine checks pass.
Sampler-OFF 05/06 GT2 gives 29.810/18.277 FPS and partial process CPU of
69.857/95.278 ms/frame; 06 uses the requested same-binary repeat after
reported computer use, excluding its original perf run. Separate full
profiles retain 1,420/866 PE samples.
No renderer or sampler failures are recorded. Measured gain and repeatable
README recovery remain unproven.

Preceding measured implementation: [H259](present-pacing-pe-settlement-refinement.259.md)
consolidates compact settlement checks and skips absent constant work with
producer-issued immutable presence. Native/refinement and linked-layout
checks and actual x86/x64 getter/StateBlock tests pass. Sampler-OFF 05/06
GT2 spots give 29.682/18.220 FPS, with zero recorded renderer failures;
06 uses the requested same-binary repeat, superseding the interfered run.
Separate full profiles preserve all 1,430/854 PE samples. Measured gain,
CPU reduction and repeatable README recovery remain unproven.

Latest mechanism evidence: [H258](present-pacing-inline-constant-current.258.md)
removes almost all standalone constant records through the current owner,
but 05/06 OFF/ON spots show only +1.25%/+0.91% FPS and no demonstrated CPU
reduction. The mode stays default-off. [H257](present-pacing-pe-constant-copy.257.md)
owns the preceding implementation's measurement and profile conclusions.

Earlier implementation: [H254](present-pacing-pe-constant-bitcompare.254.md)
replaces short-circuit 16-byte constant equality with a bitwise reduction;
both linked PE images vectorize it, with native/ABI checks passing. One
post-commit perf run per product gives 05 GT2 29.500 and 06 GT2 17.958 FPS
(prior 29.009/17.926), with zero chunk/GPU/pipeline failures. This is not
attributed gain, README recovery or CPU-consumption reduction. The leaf owns
actual PE sampling, instruction-layout joins and partial process CPU evidence.

Previous implementation: [H253](present-pacing-pe-canonical-authority.253.md)
gates OFF constant-range queries, groups source authority, reuses retained
Live bindings and replaces paired owner pin indexes with one checked relation.
Native/linked verification and code/stack tradeoffs are recorded there.
Post-commit GT2 is 29.009 sampled FPS versus H251 29.034 (-0.084%), with zero
chunk/GPU/pipeline failures and close CB/pass ratios. This spans H252 and H253;
one run with background CPU contention is not an attributed gain or qualified
neutrality. Whole-scene correctness and current CPU attribution remain open.

Previous runtime checkpoint: [H251](present-pacing-pe-append-effects.251.md) adds
active-section dispatch, concrete constant settlement with narrower rollback,
constinit ledger overrides and fresh counts-only recorder bookkeeping.
Post-commit GT2 is 29.034 sampled FPS versus H250 28.846 (+0.650%), with zero
chunk/GPU/pipeline failures and close CB/pass ratios. Native/linked checks
pass; one run with differing background load does not establish an FPS gain
or qualified neutrality. Whole-scene evidence and CPU attribution remain open.

Prior runtime checkpoint: [H250](present-pacing-pe-sparse-work.250.md) limits
compact binding traversal, quota processing and constant bitmap clearing to
active input. Native/TLA, sanitizer and linked PE checks pass. Post-commit GT2
is 28.846 FPS versus H249 28.835 (+0.038%), with zero chunk/GPU/pipeline
failures and about 4 CB/Present. One run with observed background CPU load
does not establish a gain or qualified neutrality; whole-scene evidence
remains open. No new mutable cache or transport change is introduced.

Prior runtime checkpoint: [H249](present-pacing-pe-flush-layout.249.md) gates
disabled constant-flush diagnostics, skips unobserved empty flushes, and reuses
prepared compact counts. Native/codegen checks pass. A post-commit GT2 spot
check gives 28.835 FPS versus prior H248 29.048 (-0.73%); run health passes,
but the single-run comparison establishes no attributed FPS gain.

Latest diagnostic: [H248](present-pacing-pe-identity-distribution.248.md) measures
2.588 unique identities / 4.494 comparisons per sampled compact GT2 preparation.
Keep the small linear lookup; no hash or FPS promotion follows from this run.

Prior refinement evidence: `247` - compact receipts, fused binding emission, index reuse,
and live borrow authority pass bounded native verification. Ordinary PE append
frames no longer probe. GT2 OFF pairs are 27.884/27.427 harmonic-mean FPS
(-1.64%, substantial same-build variation); no FPS promotion is established.
The CPU-ready Tape locality gap remains an independent workstream.

## Start Here

- [Current overview](overview.md) - latest conclusion and active gates only.
- [Historical log](log.md) - long-form chronology moved from the old domain root.
- [Root 3DMark05 GT1 map](../overview-3dmark05-gt1.md)
- [Root 3DMark05 GT2 map](../overview-3dmark05-gt2.md)

## Recent Leaf Documents

- [present-pacing-pe-settlement-refinement.259 - Qualified Compact Settlement Work Removal](present-pacing-pe-settlement-refinement.259.md)
- [present-pacing-inline-constant-current.258 - Current Semantic-Owner Inline-Constant Economics](present-pacing-inline-constant-current.258.md)
- [present-pacing-pe-constant-copy.257 - Bounded Changed-Constant Copy](present-pacing-pe-constant-copy.257.md)
- [present-pacing-pe-autogen-bound-slots.256 - Immutable AUTOGEN Eligibility](present-pacing-pe-autogen-bound-slots.256.md)
- [present-pacing-pe-full-selfpc.255 - Complete Bounded PE Self-PC Reporting](present-pacing-pe-full-selfpc.255.md)
- [present-pacing-pe-constant-bitcompare.254 - Actual PE Attribution and Branch-Reduced Constant Equality](present-pacing-pe-constant-bitcompare.254.md)
- [present-pacing-pe-canonical-authority.253 - Grouped Borrow and Single Pin Authority](present-pacing-pe-canonical-authority.253.md)
- [present-pacing-pe-append-boundary.252 - Family Preparation and Production Ledger Routing](present-pacing-pe-append-boundary.252.md)
- [present-pacing-pe-append-effects.251 - Active Dispatch and Closed Append Effects](present-pacing-pe-append-effects.251.md)
- [present-pacing-pe-sparse-work.250 - Active Sparse Work and Bounded Dirty Clearing](present-pacing-pe-sparse-work.250.md)
- [present-pacing-pe-flush-layout.249 - Constant Flush and Prepared Count Refinement](present-pacing-pe-flush-layout.249.md)
- [present-pacing-pe-identity-distribution.248 - Measured Small-Set Identity Decision](present-pacing-pe-identity-distribution.248.md)
- [present-pacing-pe-compact-refinement.247 - Compact Transaction and Live Borrow Refinement](present-pacing-pe-compact-refinement.247.md)
- [present-pacing-pe-plan-authority.246 - Private Plan Authority and Fused Retention](present-pacing-pe-plan-authority.246.md)
- [present-pacing-pe-compact-reuse.245 - Compact Destination and Identity Reuse](present-pacing-pe-compact-reuse.245.md)
- [present-pacing-pe-prepared-emission.244 - Prepared Emission and Compact Producer Integration](present-pacing-pe-prepared-emission.244.md)
- [present-pacing-pe-admission-reuse.243 - Qualified Pin/Wire Reuse and Active-Prefix Staging](present-pacing-pe-admission-reuse.243.md)
- [present-pacing-pe-direct-canonical.242 - Direct Canonical Value Payload](present-pacing-pe-direct-canonical.242.md)
- [present-pacing-pe-prepared-reset.241 - Prepared-Witness Reset](present-pacing-pe-prepared-reset.241.md)
- [present-pacing-cpu-ready-next-source-intent.239 - Replay-FIFO Intent Is Safe but Does Not Recover Tape Locality](present-pacing-cpu-ready-next-source-intent.239.md)
- [present-pacing-cpu-ready-active-head-locality.238 - Active-Session Lookahead Closes the Local Hold Gap but Not Tape Locality](present-pacing-cpu-ready-active-head-locality.238.md)
- [present-pacing-cpu-ready-tape-promotion-gate.237 - CPU-Ready Tape Restores Progress but Misses the Locality Gate](present-pacing-cpu-ready-tape-promotion-gate.237.md)
- [present-pacing-current-bottleneck-pe-symbol.236 - Current GT2 Ceiling Is The Producer Thread; PE d3d9.dll Is 10.6%](present-pacing-current-bottleneck-pe-symbol.236.md)
- [present-pacing-sfiv-current-tail-attribution.235 - Current SFIV Tail Is CPU/Producer Cadence, Not A GPU Hot Frame](present-pacing-sfiv-current-tail-attribution.235.md)
- [present-pacing-arena-admission-regression-boundary.233 - Segmented Arena Admission Turns Capacity Pressure Into Session Fragmentation](present-pacing-arena-admission-regression-boundary.233.md)
- [present-pacing-post-defselect-cpu-attribution.05 - The Producer Thread Cannot Be Split By Image](present-pacing-post-defselect-cpu-attribution.05.md)
- [present-pacing-drain-fence-attribution.207 - The Blocked Locks Are Mostly MANAGED](present-pacing-drain-fence-attribution.207.md)
- [present-pacing-drain-fence-attribution.206 - One Of 84 Bridge Entry Points Owns 99.8% Of The Drain Fence](present-pacing-drain-fence-attribution.206.md)
- [present-pacing-sfiv-shader-cost-attribution.205 - The SFIV 88ms Instances Are Real Shader Work, Not A Wait](present-pacing-sfiv-shader-cost-attribution.205.md)
- [present-pacing-sfiv-scene-pass-stall.204 - SFIV Scene-Pass Frame-Period Stall Owns The Frame Wall](present-pacing-sfiv-scene-pass-stall.204.md)
- [present-pacing-engine-default-trio.203 - The Promoted Trio Becomes The Engine Default](present-pacing-engine-default-trio.203.md)
- [present-pacing-archive-prewarm-hardening.202 - Archive Prewarm Hardening Closes The Startup-Flake Class](present-pacing-archive-prewarm-hardening.202.md)
- [present-pacing-inline-const-delta.201 - Inline Const Delta Proves Mechanism But Lands Inside The Noise Band](present-pacing-inline-const-delta.201.md)
- [present-pacing-decimated-pe-stats.200 - Decimated PE Stats Size The Recorder Core At ~8.5ms/present](present-pacing-decimated-pe-stats.200.md)
- [present-pacing-postcache-resample.199 - Post-Cache Producer Re-Sample - The Wall Is Now The Guest Blob](present-pacing-postcache-resample.199.md)
- [present-pacing-readonly-cache-stacking.198 - Readonly Cache Stacks With The Promoted Pair For +26% Cumulative](present-pacing-readonly-cache-stacking.198.md)
- [present-pacing-readonly-managed-buffer-cache.197 - Readonly Managed Buffer Cache Collapses The Lock Bridge Storm](present-pacing-readonly-managed-buffer-cache.197.md)
- [present-pacing-producer-sampling-attribution.196 - Non-Perturbing Producer Sampling Finds The Buffer-Lock Bridge Storm](present-pacing-producer-sampling-attribution.196.md)
- [present-pacing-consolidation-long-confirm.194 - Consolidation Long Confirm - Offload+IndexCache +10% Over The Full Demo](present-pacing-consolidation-long-confirm.194.md)
- [present-pacing-pe-const-overhead-cut.193 - PE Const-Chain Overhead Cuts Land Clean But Move No FPS](present-pacing-pe-const-overhead-cut.193.md)
- [present-pacing-pe-cost-verification.192 - PE Recording Cost Is Real (~10ms/present, Overhead-Corrected)](present-pacing-pe-cost-verification.192.md)
