---
type: "Spec Gap"
title: "Experiments Gap"
description: "Implementation and evidence gaps for wild integration experiments."
tags: [specs, gap, experiments]
---

# Experiments Gap

## Fixed-input replay experiment

### Current GPU qualification status (2026-09-14)

R-HARN-REPLAY-8.15 and R-BACK-4.10 now have normal-provider GT2 evidence:
explicit invariant vertex-position outputs plus the shared runtime compile
profile repair all ten retained final-pixel failures, with two cold replays
per original, exact source/output bytes and zero GPU errors (r585). The
fresh public compatibility-exact/production-exact comparison completes both
CPU verifications, both two-iteration cold GPU qualifications, and one warm-up
plus three measured CPU iterations per policy. It atomically publishes an
accepted production-capture result (r593). Post-publication r598 reauthenticates
original/staged input, source plan, policies, every child, runtime images and
readbacks. R-HARN-REPLAY-8.10–8.15 are qualified for this bounded frame-tape
pair; r597 separately reauthenticates all 20 retained-cohort iterations.
The prior ENOSPC transaction r588 remains unpublished and must not be called
accepted. The 5MiB headroom candidate fails capacity-receipt admission in
r586 and remains rejected. See
[current evidence and remaining gates](../../docs/perfomance/mini-replay-bisection/gpu-position-invariance.04.md).
The fresh focused native 14/14, x86 2/2, expanded regression 28/28 and complete
190-configuration TLA gates pass. The expanded set also fixes the capture CLI's
stale exact metadata expectation without weakening its dictionary comparison.
This is not full-intermediate, arbitrary-policy, sequence-tape or FPS
qualification. Pre-r585 failure statements in the historical notes below
are superseded only for the named ten final-pixel originals.

### Earlier CPU and GPU qualification evidence

R-HARN-REPLAY-8.7–8.9 are qualified for the bounded immediate-completion CPU
policy lane. Separate full-oracle and low-observer modes execute the same
production replay/Slot/Pool operations. `cpu-policy-compare` binds measured
execution to its own detailed qualification, immutable input, policies and
image identities; warm-ups/failures remain preserved. GT2 A/A r159,
compatibility DrawLimit 0/4 r156 and production exact/headroom r157 each pass
two verification plus two warm-up and 16 measured iterations per policy.
All 1,420 ordered draws match; batch source counts change 39→130, while
headroom records two provisions and 34 reuses. Native r151/r154, final CLI
r160 and full TLA r148 pass. These are CPU-service experiments, not Direct
cross-raw batching, concurrent scheduling, GPU pixels or FPS promotion.
See the [measurement leaf](../../docs/perfomance/mini-replay-bisection/cpu-policy-measurement.02.md)
and `harness/replay/fixed-input-experiment.md` for evidence and exact bounds.

R-HARN-REPLAY-8.1–8.6 are specified in
[fixed-input-experiment.md](harness/replay/fixed-input-experiment.md).
Status: **partial qualification, usable bounded experiment surfaces**. The
`experiment` operation prepares authenticated input once, runs the actual Metal
provider repeatedly in one process, reports resource/replay/readback/cleanup
wall scopes, and enforces warm reset plus strict output/source oracles.
`compatibility` suppresses both direct paths; `production-slot` retains actual
production eligibility and reports the executed path, not just the request.
The old provider command and renderer defaults are unchanged.

The real small capture in
`experiments/output/fixed-input-harness-20260909/small-{compatibility,direct}-r3`
passes five measured warm repetitions each; actual selected-path draws are 18
per repetition. The 128-draw fixture in `parallel-direct-r2` instead falls back
to compatibility and fails `--require-path`, even though its pixels match.
That is a negative execution control, not Direct evidence.

`cpu-experiment` executes production replay/Slot/Queue/Pool operations for one
Plain full-buffer seed, draw, unbind, destroy and implicit Present sequence.
It uses controlled per-command publication and no GPU/Present pacing; its
four Direct/compatibility × immediate/delayed-completion combinations compare
flattened executed-draw semantics. The adapter TLA model passes its bounded
two-submission safety case and detects deliberately premature/duplicate
completion. This does not prove all queue interleavings or physical backing
rename. Tests: `dxmt9-render-tape-provider-spec`,
`dxmt9-render-tape-provider-cli-spec`,
`dxmt9-cpu-ready-production-routing-spec`, and
`dxmt9-render-tape-cpu-experiment-cli-spec`.

Additional native composition pins bind the completion reducer to real Pool
DISCARD/backing selection and distinguish a completed use stamp from a still
held capture-residency pin (`dxmt9-dynamic-rename-ring-spec`). The Managed
mutation transaction's mode-on test now injects the existing queue allocation
failure before effects, verifies unchanged ownership/state, and counts exactly
one successful application after retry. Its test-configured fault owner
outlives the worker; default worker construction still supplies no fault hook.
These are production-predicate/transaction tests, not a Tape-driven mutation
grammar or physical Metal backing-byte oracle.

The subsequent bounded `cpu-metal-resources` Tape lane now supplies that byte
oracle for one DEFAULT+DYNAMIC buffer: Plain A, draw A, DISCARD B, draw B,
unbind, destroy, Present. Actual Shared Metal buffers are allocated but no GPU
work is submitted. Delayed completion proves A remains unchanged while B has
new bytes and an advanced revision; immediate completion permits legal backing
reuse. Both replay routes pass 16 repetitions under both presets. The external
event/submission completion plan also passes 16 repetitions with real qualified
Queue identities. These results do not qualify arbitrary resource families.

`RenderTapeEventCompletionBinding` and its shared-controller native test bind
the external plan to ordered event publication/release, terminal failure and
generation-qualified identity. Focused TLC visits 15 normal states and detects
three injected violations. The full suite passes in `tla-event-binding-r2.log`
under `experiments/output/fixed-input-harness-20260909/`; seven integrated native
tests pass in `native-integrated-r3.log`. The model abstracts adapter owners,
not physical Pool allocation, all queue interleavings or GPU completion.

The bounded cross-event capacity case now executes through authenticated Tape:
`--publication draw-island` accumulates seven then two plain-buffer draws in
one writing Slot. With optional headroom disabled, ProductionSlot executes
seven Direct draws, rejects the next capacity admission once, and appends two
compatibility draws without rotation or an intermediate publication. Both
routes and all three completion policies pass 16 repetitions in
`cpu-capacity-*-r10`, sharing one ordered semantic digest. Receipts bind the
actual nine-draw source to events 5–6 and preserve exact queue identity.
`RenderTapeGroupedPublication` checks the bounded composition (14 normal states,
three required counterexamples); the shared native range predicate checks
3072 tuples and six execution orders. This closes one populated-Slot fallback
shape, not every capacity dimension, rotation or arbitrary resource grammar.

The paired `cpu-compare` command now owns an authenticated input and separate
compatibility/reference and production-slot/candidate receipts. It validates
each child first and compares logical draws/resources plus cumulative semantic
observations after every actual final-Slot draw. The grouped capacity input
passes both preset completion schedules, 16 repetitions per route, in
`cpu-paired-capacity-{after-command,after-destroy}-r24`. Native negatives reject
corrupt per-draw evidence even when the final digest is unchanged. Publication
events qualify the actual physical source, not an original per-draw Tape
locator; physical source counts are not the cross-route equality contract.
The shared general-provider object factory, exact identity lookup, release and
pure preflight plan are implemented. The resource-backed native fixture uses real
Pool texture/surface records and observes normal core texture uploads at the
Device boundary without GPU submission. Its 16-entry/64-KiB observation bounds
and component watermark tests are not general Tape-driven texture lifetime or
physical-content evidence.

Final `cpu-paired-{capacity,dynamic}-{after-command,after-destroy}-r30` runs
pass 16 iterations per route per case. The comparator no longer equates
physical command/source counts across policies, but each child must conserve
submissions/completions/reclaims and Direct plus compatibility draw totals.
The delayed dynamic case additionally observes real A backing-byte retention
across DISCARD B. All four canonical builds, four focused native/CLI tests and
the full TLA suite pass for that bounded evidence set.

`cpu-general-gt2-paired-r41` additionally passes 16 fresh-device iterations per
route on the unchanged r4 capture: 965 events, 277 objects, 645 mutations,
39 chunks, 2085 records and 1420 draws. Compatibility executes 1420 ordinary
draws; production-slot executes 1410 Direct plus 10 compatibility draws. Every
iteration has the same ordered per-draw/final digest and conserves all 39
submitted/completed/reclaimed sources and all 277 wrapper acquisitions/releases.
Each iteration verifies 4235 actual buffer snapshot observations against core
shadow bytes. Actual Pool texture/surface records and checked logical upload
receipts replace fabricated resource handles. This closes bounded **GT2 CPU
execution**, with explicit immediate completion and no internal split policy;
it does not close physical texture/pixel correctness or general delayed
multi-object lifetime schedules. See the versioned artifact and
[evidence](../../docs/perfomance/mini-replay-bisection/fixed-input-replay.01.md).
Five focused native/CLI tests and all four canonical builds pass. A native
snapshot-proof pin rejects stale bytes after mutation even when address and
revision are reused; cache overflow rejects instead of truncating. Full TLA
passes in `build/meson-logs/cpu-general-tla-r36.txt`; that model evidence does
not prove the physical byte oracle or expand the admitted completion policy.

The explicit `before-next-work` CPU mode additionally holds one submitted
source through wrapper destruction, releasing before the next work event.
`cpu-general-gt2-before-next-work-r44` passes 16 iterations per route with the
same draw digest: 39 deferred releases, two destroys while pending and 16
retained buffer snapshot checks per iteration. This does not authorize delay
across mutation/rename or multiple outstanding sources. The existing
chunk-entry quiescence guard remains active, and release boundaries are
checked against authenticated event types rather than trusted receipt flags.
The isolated native destroy-window fixture covers all four static/dynamic
VB/IB combinations, both replay routes and actual-byte corruption with
failed-iteration cleanup. General projection v2 now observes non-versioned
live Pool backing separately from required versioned snapshots, with explicit
UP precedence and strict observation-count conservation. Final r53 immediate
and before-next-work GT2 comparisons pass 16 iterations per route per mode; GT2 itself
has zero live-binding observations, so non-versioned coverage comes from the
native matrix, not that artifact. This does not prove overlapping static
buffer mutation, general texture-alias lifetime or GPU pixel equivalence.

Native mutation-window qualification now passes 20 route/schedule cases and
six concrete byte-corruption negatives. It uses the production
`visitSourceCommandResources` closure, one pending source, one mutation, and
the actual zero-wait map predicate. Unrelated DEFAULT/plain and referenced
DEFAULT/dynamic DISCARD mutate before acknowledgment; same-buffer Plain
completes before locking. The second draw consumes the new bytes through the
normal snapshot resolver. Nine focused tests pass in
`build/meson-logs/cpu-mutation-r56.txt`; the new pure admission checker covers
512 witness valuations and action words of length four/five. Existing full
TLA checks pass separately in `cpu-mutation-tla-r55.txt`.
The CLI now exposes this bounded policy only through an explicit
`cpu-source-project` artifact and `--source-projection` on `cpu-experiment` /
`cpu-compare`. The compatibility producer records the actual source visitor's
buffer closure, qualified by input digests and live object identity; native
candidate replay rechecks all source rows before any mutation hold. The
wrapper independently checks held events, mutation targets, release boundaries
and counter conservation, plus final/per-draw digests against the immediate
reference. Raw chunk handles are not a closure substitute. Identity sidecars
remain optional; this is not frame/pass-sidecar completeness evidence.

GT2 projection r65 contains 39 sources and 913 buffer references (peak 43).
The final r66 paired CLI run passes 16 repeats per route, 1420 draws each,
with 39 submitted/completed/reclaimed sources, three deferred DISCARDs,
20 retained-buffer byte checks and three release-before-mutation boundaries
per replay. All ordered/final digests match the immediate reference; final
digest is `11633069703464745659`. Twelve focused tests pass in
`cpu-source-binding-r65.txt`, the final three CLI/pure tests in
`cpu-source-binding-r66.txt`, and full TLA in
`cpu-source-binding-tla-r63.txt`. The r63/r64 missing-counter rejection is
preserved, not relabelled as accepted evidence. See the
[experiment leaf](../../docs/perfomance/mini-replay-bisection/fixed-input-replay.01.md)
for artifacts and commands. Neither completion preset changes without the
explicit oracle, and no renderer default changes.

The general executor now has a maintained A-to-B-to-A reset qualification on
both routes and completion presets, sharing a Metal device but recreating the
actual core/queue/resources. A skipped wrapper release fails the real reset
boundary; recovery cleanup does not relabel that sample. The next A matches
every original A draw. An actual seeded-texture/surviving-surface fixture also
closes parent-destroy receipt attribution: immutable parent identity plus
live/tombstone backing evidence replaces the live-wrapper-only lookup. A
native fault reproducing the old lookup first diverges on the post-destroy
draw. Same-object-ID texture redefinition is not covered by this fixture.

CPU wrapper receipts now snapshot allowlisted renderer/Metal environment and
pass that same environment to the provider. API/shader validation enabled or
unknown nonempty values mark a diagnostic run; execution may pass, but policy
acceptance, paired comparison and source-projection promotion may not. The
environment test uses a provider double for wrapper-gate coverage, not actual
driver-validation evidence. Fresh-device cleanup is not warm-device reset.
Twelve focused tests plus full TLA pass in `cpu-alias-reset-r74.txt`
(TLA 165.04 seconds); native and all three canonical cross builds pass.
Fresh GT2 projection/comparison r75 bind the updated binary: 16 repetitions
per route, 1420 draws, all 39 source checks and three deferred DISCARDs each;
all ordered observations and final digest `11633069703464745659` still match
the immediate reference. Receipts report `diagnostic=false`. This supersedes
r66 as the CPU execution checkpoint at that stage, not as Metal or FPS evidence.

The r80 native qualification now replays B while A is actually pending,
retires the actual wrappers and verifies A-only reclaim preserves B's physical
backing. Both routes also saturate the real queue at 31 pending sources;
denial changes no state and one completion/reclaim admits one replacement.
The general executor's `PendingSourceProof` unifies full qualified identity,
schedule, resource closure, backing proofs and mutation state. Six independent
identity faults reject before acknowledgment on both routes/presets (24
negatives), with failed-trace preservation and cleanup. Fourteen focused tests
pass; the two completion model/code tests and full TLA pass in r79 (168.76 s).
Fresh GT2 projection/comparison r81 supersede r75: 16 repeats per route, the
same 1420 draws, 39 conserved sources, three deferred DISCARDs and identical
per-draw/final semantics, `diagnostic=false`.

Public general-frame now admits a qualified two-source window. The shared
immutable emission-plan witness limits overlap to 1–64 ordinary draw/state
records in one whole-source lease. Actual offload/Arena/queue-worker absence,
4096-byte pages and fresh no-wait queue admission are checked. Every pending
owner retains its own eligibility and qualified identity; an ineligible owner
cannot overlap a later eligible source. A real Draw/Clear/Draw/Draw fixture
pins that distinguishing edge on both routes, alongside the 128-value window
reducer table and independent inspector/Python receipt checks. R88 focused
native/CLI tests pass; the completion model/code tests and full TLA passed in
r85 (168.82 s). GT2 r88 reaches pending peak two in all 16 repeats per route:
1420 draws, 39 submitted/completed/reclaimed sources and every ordered draw/
final digest equal to a separately executed immediate-completion control.
The candidate executes 1410 Direct and 10 compatibility draws. This supersedes
r87, whose incoming-only eligibility was corrected after independent review.
The full accepted receipts and control check are linked from
[the experiment leaf](../../docs/perfomance/mini-replay-bisection/fixed-input-replay.01.md).
Mutation holds remain a separate max-one policy; no general concurrent
production admission or GPU-execution guarantee is implied by this cold window.
Fresh r88 projection and max-one comparison also pass 16 repetitions per route,
including all 39 source checks, three deferred DISCARDs and 20 retained-buffer
checks per replay. Earlier projection artifacts are not reused with new binaries.

Remaining extensions beyond the bounded CPU R8 scope: actual GT2 strict Metal
replay qualification; broader CPU command and delayed multi-object lifetime
grammar; broader production continuation/capacity cuts. Adapter boundary-fault
injection and a pending-completion budget are not substitutes for other capacity
coverage. Existing native allocation/generation tests remain separate evidence.
The current bounded CPU completion checkpoint is recorded below; it does not
claim complete Metal/capture qualification. No FPS or renderer-promotion claim
follows from fixed-input service timings.

Capture completeness (R-HARN-REPLAY-7.4) has a bounded LOD guard, not full
resource-owned-state replay: effective tracked managed-texture `SetLOD` changes
invalidate capture before or during arm; no-ops/non-managed calls preserve
capture. Native truth-table coverage and PE call-site/build checks do not
retroactively certify old bundles. Stream frequency now has one PE binding
owner shared by setters/getters, StateBlock, Reset and SWVP. Accepted setters
enter canonical pending stream state; all four producer lanes emit explicit
frequencies. Unix sparse replay journals source/frequency together and preserves
legacy wire zero; the state folder also preserves a known value across sparse
and FULL_SNAPSHOT zero rows. Producer parity, default/flagged values, rollback
and fold tests cover the value boundary. Production queue tests also pin A/B/A
and derived instance counts in Arena, ordinary direct and compatibility routes:
stable cache generation and batch/encoder key compatibility now include
frequency, and sparse replay uses the ordinary draw instance-count reducer.
New PE-origin capture evidence remains separate. The post-fix GT2 CPU recheck
now passes at r102 after independently owned copy-on-write input staging removes
the previous ENOSPC blocker. Both routes pass 16 repetitions with pending peak
two, 1420 draws and 39 submitted/completed/reclaimed sources, matching a
separate 16-repeat immediate-completion control with the same input and loaded
images. The r95 artifact failure remains preserved, not relabelled as passing.
The r102/r103 source-mutation rechecks fail while writing the parent receipt
with ENOSPC; their lost unpublished evidence remains documented. CPU artifact
transactions now preserve populated rejected staging even when quarantine or
marker creation fails. The r105 standalone receipt failure retains all 16
completed native iterations, but is not a published aggregate. The fresh r105
two-repeat immediate reference authenticates 39 closures; r106 then passes
the paired source-mutation comparison for 16 repetitions per route, with three
deferred DISCARDs, 20 retained-buffer checks and 39 source closure checks per
iteration. All draw digests match the bound immediate reference. This replaces
r88 as the accepted source-mutation evidence without relabelling prior failures.
The r129 two-repeat projection and r130 paired comparison
revalidate the same 39 closures and all 1420 draws for 16 repetitions per route;
three deferred DISCARDs, 20 retained-buffer checks and complete resource/source
reclaim remain unchanged. Loaded images are stable within both runs.
The bounded CPU R8 timing/receipt closure is now qualified separately from
Metal/capture completion. R137 passes 16 repetitions per route with external
completion plans, nine draws and two conserved sources. R139 retains a real
injected release-callback failure; r140 reproduces its authenticated Tape,
schedule and incomplete prefix. Bounded results now expose exclusive wall
phases including teardown, separate import/shared-device setup, and process
CPU measurements; both grammars declare reset/cache/shared-device policy.
Missing, malformed or inconsistent timing/reset declarations fail acceptance.
Final-image r138 projection and r144 paired GT2 comparison pass 16 repetitions
per route with the unchanged 1420 draws, 39 source closures, three deferred
DISCARDs, 20 retained-buffer checks and identical semantic digest. R142 passes
25 native tests; r143 passes five x86_64 tests. Together with the unchanged
188-config r128 TLA gate and native contamination/fault controls, this closes
R8.1–8.6 for the declared bounded CPU policy/lifetime scope. It does not close
arbitrary concurrent admission, broader grammars, Wine capture or GPU pixels.
The requirement-to-evidence table is in the
[experiment leaf](../../docs/perfomance/mini-replay-bisection/fixed-input-replay.01.md#bounded-cpu-harness-closure-timing-and-retained-schedules).

The CPU queued-operation oracle covers canonical GENERATE_MIPMAPS
through actual replay/Queue/Pool, authenticated command counts, immediate and
delayed completion, destroy-before-completion retention and final reclaim.
Active, negotiated PE explicit AUTOGEN calls now use an isolated canonical
record, a device-owned cold receipt collector (including CapacityPost commits),
and the exact token/event completion bridge. The bridge authenticates the row,
flushes a potentially parked session, waits the queue settlement, and
acknowledges only that event. Capture-only failures use an unqualified
completion fence without repeating generation or manufacturing capture proof.
Tracking before arm or without a recorder still rejects unrepresented calls
with `unsupported_explicit_mip_generation`; capture-off and physical-one-level
execution retain the old direct path. Native integrated r114 pins cover real
D9CDevice/Queue/Pool completion, missing/foreign lookup, independent event
acknowledgment, rejected-capture completion, scoped receipt retention and
second-commit conflicts. The subsequent r122 production-loop test exposed a
real composition failure: post-encode physical retirement lost a strict
singleton's group identity, so sequence completion did not settle the exact
event. The queue now transfers that identity into its receipt before retirement
and forwards completed receipt/resident group tails through one ordered FIFO.
The real replay/encode-loop/C API/capture-ledger regression passes in r123 and
ten repeats in r124; r127 adds singleton physical reuse, receipt ABA/one-shot
transfer and mixed resident-group ordering. The backend and completion are
CPU-controlled: this proves release of an unsubmitted production source, not
that the coordinator was observed asleep on its idle CV. Actual PE COM
CapacityPost fault integration and GPU mip texels remain outside these pins.
Explicit PE capture under
Wine remains unqualified; no promotion or complete capture claim is made.
The exact-event TLA safety model passes with 87 distinct states and both
expected counterexamples in the full r110 verification gate. Its r116
conditional progress extension passes with 32 states and detects a
missing-release temporal counterexample (183-configuration full gate passes).
Fair enabled steps and valid accepted requests are premises; queue completion
is abstracted, so this does not alone close production-thread progress.
The separate singleton-retirement model now distinguishes sequence completion
from exact-identity settlement (18 states for both safety and progress).
Missing transfer fails eventual settlement; stale receipt extraction and
premature publication fail their safety invariants. The complete 188-config
TLA gate passes in r128 (214.21 seconds). Final native r131 passes all 24
selected tests including install-name audit; x86_64 r132 passes the four core
receipt/Tape/queue/encode-loop tests. Model-code correspondence is bounded
scenario evidence, not an unbounded proof of arbitrary callback interleavings.
The small r100 fixture passes both routes and four repeats per completion
policy; it does not prove PE explicit-call capture or generated mip texels.
The r4 GT2 input
has no AUTOGEN texture definitions
but 36 managed multi-mip textures, with no actual LOD attestation.
Validation diagnostic r21 still fails (2403 pixels, maximum RGB delta 16,
zero reported GPU errors). Subsequent wrappers preserve Metal validation
settings and exclude those runs from policy-performance acceptance. See
[fixed-input evidence](../../docs/perfomance/mini-replay-bisection/fixed-input-replay.01.md).
The small synthetic cold-device validation fixture separately exits with
signal 11 (`validation-fixture-r22`); this remains unresolved and is not a
passing validation oracle. The environment/acceptance test deliberately uses
a provider double. Sticky pre-arm LOD invalidation/active abort use the existing
PE registry path but still lack a direct PE/COM runtime fixture; pure predicate
tests are not evidence of that whole owner lifecycle.

The bounded Managed Tape now binds the actual replay queue allocation-failure
hook (`--fault managed-allocation`, `cpu-metal-resources`). It verifies an exact
pre-effect rejection, unchanged backend ownership and still-open lock, then
retries through public Unlock and observes exactly one backend apply. Both
routes and both completion presets pass 16 repetitions in
`cpu-managed-fault-*-r7`; the receipt is explicitly diagnostic:
`fault_oracle_passed=true`, `accepted=false`. The CLI native test compares the
same Tape without the fault and rejects corrupted fault evidence. This closes
one Tape-driven allocation transaction, not all Slot capacity dimensions.

Current GT2 capture/replay gate: both SegmentSerial `...gt2-frame-tape-v2-20260909-r2`
and EventSerial `...-r3` at the default 256-record cadence abort identity publication. The first
`legacy-or-inline` rejection is `ReplayReason::Oversize`; it fails/clears the
ledger, so later `snapshot-or-ledger-append` messages are secondary. The explicit
64-record/262144-byte capture recipe `...-r4-capture64` now publishes an
authenticated complete frame with 965 events, 1420 draws, 135 identity-v2 source
segments and 39 event settlements. This does not qualify the default cadence;
the first capture token still aborts and the next token supplies this bundle.
ARM64 compatibility and ProductionSlot, and x86_64 compatibility replay all
produce the same image but fail the original source/output oracle at 2378
pixels (maximum RGB delta 16). Direct executes 691 draws plus 729 compatibility
draws, so this is a non-vacuous shared capture/replay mismatch, not a passing
policy gate. A second capture with archive/prewarm disabled also validates but
fails compatibility replay at 1839 pixels (maximum RGB delta 18); cache-policy
matching alone is insufficient. Failed source/output RGBA artifacts are now
preserved and authenticated outside replay timings. The historical r17 v2
input remains absent locally. See
[fixed-input evidence](../../docs/perfomance/mini-replay-bisection/fixed-input-replay.01.md).

The cold initial-seed audit is diagnostic only. GT2 `gt2-initial-seed-audit-r16`
matches all five supported seed readbacks (87 unsupported), with the same final
2378-pixel failure. The earlier black RT readback in r14 was an audit ordering
error: deferred Initializer uploads had not completed. The diagnostic now
flushes that owner and checks a nanosecond-unit wait before readback, with a
native private-RT seed regression pin. No source oracle was changed.

The cold fresh-owner preflush diagnostic (`gt2-fresh-seed-drain-audit-r17`)
omits 530 mutation drains and retains 23 after commands, but still rejects the
output at 2398 pixels (maximum RGB delta 16). This is non-vacuous boundary
sensitivity, not a capture/replay fix or a renderer promotion. Its first
receipt's audit-completion field was coupled to the final pixel result;
mutation coverage is now counted independently without changing acceptance.
The r18 rerun confirms complete **645/645** coverage and identical rejected
pixels; its replay service is 138.102 ms, excluding prepare/device/artifact
work. All nine focused tests pass in `native-integrated-r6.log`.

The independent-texture prestage diagnostic (`gt2-texture-prestage-audit-r20`)
activates all 40 candidates and consumes 500/500 seeds with zero leftovers,
277/277 provider-owned wrapper conservation, and unchanged 2378-pixel failure.
The actual backing receipt classifies all 500 uploads as immediate Shared
`replaceRegion`, not deferred Initializer work. Thus a zero upload-flush count
is expected, not an unperformed seed; this run does not exercise deferred-blit
boundary removal. DEFAULT/MANAGED native indexed fixtures pin exact ordinary
versus prestaged pixels, alternate-seed sensitivity, upload-route evidence and
conservation (`native-prestage-r3.log`). The bounded discriminator is closed;
command/state-projection and publication-boundary localization remain open.

The strict GPU/CPU policy join is implemented as an optional qualification
gate, not yet production-qualified. The 2026-09-10 GT2 A/A r12 executes CPU
verification, rejects both real-GPU original source/output comparisons, and
starts no measurement child. Fresh r17/r18 traces align 2,196 issued draws
and match render-state/viewport/transform/declaration fields. New r28/r29
additionally align 2,267 issued draws and match all selected programmable
uniform hashes, active counts and bytes; run-level hashes still differ without
payload corruption. Final pixels still differ at 1,453 pixels (max RGB delta
27) for that separate frame. An independent synthetic
positive fixture exposed partial Clear being encoded as a whole-target clear;
GT2 r13's nine Clears have no rectangles, so the defects are not equated.
The r22 actual positive CLI gate passes strict A/A and nonidentical A/B
comparisons on an independent nonuniform synthetic oracle. Alternating draw
states force the candidate command-limit policy to publish more encode chunks;
both policies still reproduce the original source and output bytes. Its
clipped geometry proves Clear/gate control flow, not visible draw/uniform
fidelity. The corrected partial-Clear attachment oracle additionally exposed
full-Clear resolve-only source loss, now fixed and pinned by a shared store
predicate. r27 passes actual MRT/mip/cube/MSAA and exact independent depth/
stencil readbacks; r30 passes the existing 188 TLA configurations, without
claiming those lifecycle models prove pixels.
Fresh r36/r37 additionally matches post-override fragment slot 5 bytes and
observed vertex slot 3 half-pixel/viewport/clip fields across 2,257 issued
draws without missing binding observations. Pixels still differ (1,749 for
this separate frame). Live has 230 physical render-pass starts, replay 38;
the replay starts are a strict subset. The provider currently validates and
reports the identity sidecar but does not consume its source record intervals
to reproduce capture cuts. `partition-mode=identity` is a scheduler selector,
not capture-boundary identity. The default-off source-boundary diagnostic
(`R-HARN-REPLAY-8.13`) now consumes an authenticated exact record cover and
requests ordinary queue publication per range without a source-local GPU wait.
State-only ranges are no-ops; missing/stale/overlapping ranges reject before
effects, and prefix failure is terminal. r44 independently observes all 230
physical pass starts matching the original and exact source/output RGBA.
Same-input/same-binary diagnostic-off r45 reproduces the original mismatch;
r46 repeats the exact diagnostic result twice with effect tracing off.
The older r28 input in r47 remains a strict counterexample: both cold
iterations have six differing pixels (maximum RGB delta 1, total 6), improved
from 1,453 but not eligible for tolerance-based promotion. r48 still matches
all selected programmable uniform bytes; its older live trace lacks physical
pass IDs and effective slot 3/5 bindings, so those comparisons remain unobserved.
Rebuilt x86_64 r50 reproduces the same six-pixel residual SHA. Native exact-cover,
sidecar rejection and 36-range Metal control/Clear tests pass within the 8/8
integrated r43 gate; existing queue TLA passes in r41. Source publications do
not generically promise physical encoder boundaries when sessions coalesce.
r53 additionally restores all 15 missing intra-source DrawRun starts: all 836
original command starts match, with unchanged 224 replay physical pass starts
and identical six-byte residual. This excludes the run partition/hash drift
under the source-cut regime. Same-build trace-off r54 repeats the residual twice;
r55 retains exact original r36 pixels. The pure authenticated source-plan
builder now owns provenance rows/digest and is consumed by the diagnostic.
Native exhaustive interior cuts and stale-plan failures, 31-flush/three-source
Metal execution, and integrated CPU/GPU gates pass 8/8 in r56. Normal CPU/GPU
policy execution did not yet consume this plan at r56; diagnostic success remains
excluded from ranking.
Remaining: close r28's residual, bind the explicit capture-boundary reference
plan into versioned CPU/GPU policy receipts, and qualify both ordinary routes with
original oracles and actual path evidence. Diagnostic receipts remain excluded
even when exact. GPU policy promotion and general GT2 fidelity remain open. See
[GPU qualification evidence](../../docs/perfomance/mini-replay-bisection/gpu-policy-qualification.03.md).

R-HARN-REPLAY-8.14 now specifies the normal capture-source reference contract.
The borrowed range planner preserves raw-absolute locators and full canonical
handle/payload storage. The normal CPU executor and GPU provider now use one
source-aware driver, while the public versioned policy join remains open.
The typed queue-publication seam now returns actual queue-issued receipts or
empty/rejected settlement, and the diagnostic consumes it. Native pins cover
stale identity, active leases, repeated private-raw cuts, completion/reclaim,
Clear, Present and post-effect failure. The normal driver now captures actual
Ready publications including implicit cuts and Present, binds range settlement
to the authenticated plan and fail-stops on a failed prefix. The CPU executor
rejects stale same-shape plans by exact tape/sidecar digest binding and matches
each Ready source/sequence to its receipt before controlled completion.
Tape's raw high-water
forbids repeated nonzero raw ordinals; capture/private-raw provenance remains
separate rather than relaxing that invariant. New trace-complete GT2 r65/r66
matches 136 source cuts, 149 physical pass starts and exact original bytes;
trace-off r68 repeats twice exactly, while cuts-off r67 differs at 6,943 pixels.
The typed implementation retains exact original bytes in both cold r75
iterations with 136/136 cut requests and zero GPU errors; it still exercises
compatibility only. Native integration passes 9/9 and the existing TLA suite
passes in r71, without claiming a new composed policy proof.
These diagnostics are not policy qualification and do not close r28's six-pixel
residual. r64's host-POSIX output path disabled the PE publisher and produced
no tape despite benchmark success; r65 corrected the invocation to a PE-visible
Windows absolute root. All failed evidence is retained.

Normal source execution now passes native 7/7 (r89), x86_64 6/6 (r93), both
full builds and the existing TLA suite (r90, 172.67 s). Public provider CLI,
strict GPU policy Python and synthetic GPU A/A + A/B CLI pass 3/3 in r97.
The no-worker Present
test timeout was a fixture pacing mismatch, localized by r82's stack and
fixed with the CPU harness's isolated pacing contract, not a queue wait bypass.
Real r65 input passes exact original source/output in two cold iterations
per route (r91 compatibility, r92 ProductionSlot); Direct executes 133 leases
and 1387 draws, plus 10 compatibility draws, with 136/136 source publications.
The older exact r36 input also passes ordinary Direct twice (r95: 214 leases,
2265 Direct draws, 10 compatibility draws, 217/217 publications). All have zero
GPU errors. Normal Direct on r28 still reproduces exactly the prior six-pixel
residual twice (r94: 207 leases, 2275 Direct draws); strict promotion is denied.
The r91/r92/r94/r95 receipts remain unqualified historical normal executions:
canonical v1 policies do not bind this boundary mode/plan. The explicit v2
policy now binds capture-source mode and an expected plan SHA before effects.
Full source rows/local Ready publications are retained in both executors;
Python reconstructs the native plan digest and joins CPU publications to their
actual completion traces. Final GPU reauthentication takes the caller's
expected name-to-policy mapping, not a policy chosen by a stored receipt.
GT2 r65 now passes the public CPU verification (r103) and strict GPU execution
(r104). Actual GPU-qualified Compatibility/Direct A/B (r106, two verification,
two GPU, two warm-up and sixteen measured iterations per policy) and Direct
A/A (r107) pass with original source/output bytes. Final-code reruns r116/r117
reconfirm A/A and A/B after the optional-digest CLI and expected-policy
reauthentication fixes; native/CLI/Metal plus existing TLA pass 9/9 in r113,
and the final x86_64 provider/source-policy/Clear/CLI set passes 6/6 in r123.
Stale expected plan digests reject before replay effects in GPU r119 and CPU
r120. This qualification is input/policy-bound, not general GT2 fidelity or
production/FPS promotion.
The r28 pass-end scene/bloom probe r110 retains exactly the same six-pixel
counterexample; it is diagnostic and rejected. Its initial r108 invocation
failed before effects because the CLI incorrectly marked an absent optional
digest as supplied; that flag is fixed. Non-diagnostic v2 Direct r121 still
fails exact original source/output equality in two cold iterations with the
same six one-unit RGB differences. Offline final-composite analysis is not an
acceptance oracle and does not establish original current-frame intermediate
or emitted shader/compiler identity. Actual compiler-input receipts now retain
matching translated source sets on ARM64/x86_64 r131/r134 with the unchanged
residual; bounded scene/bloom r145 is also invariant. Visible indexed A→B→A
color/geometry coverage now passes both actual routes with independent pixels,
and altered PS/stale VS inputs reject; provider/GPU-policy CLI/compiler-lineage
tests pass 3/3 on native r148 and x86_64 r150. Live r146 independently validates
222 source segments, but its runner was terminated during finalization for
low disk space and supplies no full-run qualification. Same-input r152/r157
confirms exact bloom bytes but a seven-byte/six-pixel scene mismatch before
the final composite and two differing final pixels. Its 236 physical encoder
identities and 2296 FS-volatile rows align. Non-indexed effect observation is
now covered by a one-UP-draw logger off/on invariance test (not an independent
pixel oracle); native r156 passes 3/3 and x86_64 r158 passes 4/4. Actual PSO
prefetch suppression in r159 restores the 12 differing VS fallback-literal
sources to their live variants, without changing scene/bloom/final bytes.
The r161 bounded pass-end ROI observer preserves that final failure and finds
only three RGB-changing endpoints among 123 observed scene passes. This is
replay-only candidate localization; it omits alpha and original per-pass
comparisons. Summary-only readback paths now reject GPU qualification even
when pixels match, covered by native Python and actual-GPU CLI tests.
Live r171 completes and independently validates a 174-source Tape, but its
135 fixed-frame pass dumps share zero sources with the published interval.
Observer-off r172 reproduces a new three-pixel strict failure twice. The
disjoint original observation is retained as rejected evidence. Bounded
hash-only readback and capture-source-only selection now have pure identity,
single/batch actual queue publication, teardown and actual GPU hash/raw pins
(r176/r178 and final r180/r181: 5/5 per architecture; existing TLA r177
passes). Registration is
under the publication lock before Ready consumption, and matches the actual
encoder's full source/storage ref. Diagnostic pass provenance adds no fields
to execution payloads or queue tickets. The bounded positive native
source-to-GPU hash join now passes EventSerial and SegmentSerial with
independent full-image BGRA constants, actual settled production-ledger
entries and uncaptured-source exclusion (`dxmt9-capture-color-observer-metal-spec`;
r183/r184 pass 2/2 per architecture including the existing Direct/compatibility
Metal oracle). This fixture has no PE publisher. After disk space was freed,
live r185 independently authenticates 211 original pass hashes against the
published 198-source Tape and excludes 345 outside observations. r186 matches
final pixels, reconfirmed observer-off twice in r187. Intermediate comparison
finds 45 raw hash differences, mostly R32F; canonical cross-process attachment
identity and raw-byte localization remain open. Capture-attempt membership
alone is not successful publication authority; prior evidence remains intact.
The r190 cold ledger repair namespaces SegmentSerial event-local pass IDs before
frame-sidecar publication. Its native counterexample failed under the prior
frame-global interpretation; source/settlement and actual Metal observer pins
pass after repair. Same-event continuation and trusted frame-ID APIs remain
distinct. This fixes a demonstrated identity-composition defect, not GPU pixels;
post-repair r191 successfully publishes 101 sources and authenticates 114 original
pass hashes with no outside-attempt rows. r192 has exact final scene/bloom and
output bytes, reconfirmed observer-off twice in r193. r194 independently repeats
each original r28/r146/r171 twice; all six retain the same 6/2/3-pixel strict
failures. The successful capture does not substitute for those counterexamples.
Fresh current-build r201, with per-pass hashes disabled and final inputs retained,
also fails by six final pixels; r202 localizes five scene-input pixels while
bloom is exact, r203 repeats observer-off twice, and r204's same-input pass
hash observer leaves the same failure SHA. This is not a historical-build-only
problem. r205 adds effective late-resolved Store witnesses to cold color hashes;
native/actual-GPU pins pass 5/5 per architecture and existing TLA passes (r206/207).
r208 finds 80 Store-qualified replay scene endpoints and seven RGB-changing
writer candidates, not the first original divergence. Defined-content and
paired original endpoint evidence remain open. Observer-off compatibility r209
retains the same six-pixel failure twice with zero Direct draws. Actual prefetch
suppression r211 restores all original candidate VS compilation variants but
leaves scene/bloom/final bytes unchanged. r212 authenticates identical input,
source cuts and provider images; Direct-only and these compiler-variant
explanations are excluded for this input, not the shared capture/replay path.
Tracked Shared Metal cbuf backing and actual vertex-slot-5 observations now
have A→B→A Direct/compatibility/argument-table pixel pins, capture source-scope
negatives and passing TLA (r218–r223). Live r225 authenticates 226 sources and
2,231 draw observations, yet replay r226 fails by 56 pixels with equal observed
VS/PS backing, volatile values and 2,213 indexed stream-0/index hashes. FFPVS
full backing and some FFP shader hashes differ; GPU-read and all-field equality
remain unproven. Observer-off r227 repeats the failure once (stops before the
requested second iteration). Exact r221 is positive evidence, not replacement.
r225's live texture dump budget expires before its composite. Exact-source
texture selection now precedes budget admission, with both-architecture Metal
negative/positive pins (r231) and TLA (r232). Published r233/r234 joins 195
sources, 2,127 draws and final-composite inputs: final pixels and bloom are
exact; scene differs only in one alpha byte. This does not replace failing
originals. Conservative actual-compilation audit r236 finds no material/light
field reads in the observed r225/r233 VS variants; ten changed FFP sources
differ only in their final hash comment, not executable text. This is not a
per-draw selected-PSO/GPU-read proof. Cold completed-readback per-channel hashes
have native/x86_64 Metal pins (r237) to separate RGB and alpha evidence; paired
failing source endpoints and first-divergence localization remain open.
r238/r239 pairs 179 stored endpoints: 131 raw-equal, two alpha-only and 46
R32F differences, with exact final scene/bloom/output and no BGRA/RGBA RGB
difference. Store does not prove defined/live contents. Observer-off r240
retains the original r225 56-pixel failure on the final build. Actual issued
index/bound-stream0 observation has A→B→A route and filter-negative Metal pins
on native/x86_64 and passing TLA (r243). On unchanged r225 all 2,213 indexed
ranges match both replay logical geometry and retained live source rows;
five index staging rebases preserve bytes. The 56-pixel failure is unchanged.
This is not a same-build live pair or proof of GPU reads; 18 non-indexed draws,
other streams, selected PSO/raster state and later mutation remain outside it.
See the GPU qualification leaf for preserved receipts, byte comparisons and exclusions.
Actual raster observation and strict scene-extent filters have native/x86_64
Metal pins (r249–r253); native TLA passes r249. Same-build failing r254/r255
now pairs 132 scene-sized Store endpoints. Source 22693 (event 775, records
23–33) first differs in observed RGB after an exact same-scene predecessor.
Actual indexed bytes/ranges, raster and VS/FS volatile match through qualified
draw keys. One replay-only FFP binding event remains explicitly uncovered.
Final scene differs at one pixel; observer-off r256 repeats its three-pixel
final failure twice. Wire audit r258 establishes logical complete seed,
full Clear and final-composite alias/read provenance. Actual runtime canonical
resource identity, selected PSO/view/sampler, candidate produced R32F contents
(371 preceding writer draws) and first within-pass divergence remain unproven
for r254. After disk recovery, r261/r262 has exact final pixels despite 209
changed R32F values. New r263/r264 instead fails by 72 pixels: the first observed
scene divergence is four draws at source 22237. Wire audit r265 connects the
retained R32F input (1,415 changed float values) across a checked no-writer
interval; the later selected family's full material/cube mip coverage does not
cover the four earlier draws' other PS families. Observer-off r266 retains the
identical failure once and exits before its requested second repeat. Remaining
work focuses on the earlier four draws' inputs and actual selected PSO/view/
sampler lineage. Controlled R32F intervention r274–r286 leaves the exact same
72-pixel final failure under replay-input and original-input replacement;
post-upload readbacks authenticate both inputs. Ten selected-view dumps retain
the original replacement. A zero-input sensitivity control changes 515,132
pixels, so the diagnostic has a real downstream effect; the observed 1–4 ULP
input differences alone are not a sufficient explanation at this cut. The
new cold intervention is permanently diagnostic, bounds D3D-format 114 input
and fails closed before effects on bad identity/extent/plan. Rejected populated
intervention artifacts are retained. These results do not prove every actual
Metal binding or resolve any original counterexample.
The native R32F pre/post-readback fixture, malformed identity/extent/format/
mode admission pins, CLI ownership/forwarding and post-render publication
failure retention pass in r295; the existing full TLA+ suite passes in r289.
This closes the scoped diagnostic, not original-pixel GPU qualification.
Actual extra-stream observation now covers Metal slots 6–20 with cold
encoder-local setBuffer/setBytes invalidation and expected-declaration stride
labelling. A real stream-1 POSITION fixture at nonzero offset changes pixels
when only that region changes, while stream-0/index hashes remain unchanged;
both routes and off/filter negatives pass on native/x86 (r296–r307).
Same-build r300/r303 pairs 1,902 draws, 1,884 indexed ranges and 107 extra-stream
ranges without a byte-hash mismatch or unreadable range, but two final pixels
still differ by one unit. Of 80 stored scene endpoints, 36 are raw-exact and
44 RGB-different; the first observed difference is source 22190's eleven draws
(event 843 records 24–34), after raw-exact source 22189. Observer-off r308
completes two cold iterations with identical wrong bytes and zero GPU errors.
r299 preserves r263's original 72-pixel failure under the extended observer.
Shared bytes at encode time and expected stride are not GPU-read or actual
selected compiled-PSO/view/sampler proof. No lifetime/interleaving policy or
pixel threshold changed; the lower diagnostic pixel envelope is not the strict
public GPU gate. Actual selected-PSO creation provenance r310–r327 now connects
actual bound pipelines to authenticated library source/function/creation
descriptors; unavailable publication, conflicting associations and unsupported
binary/specialized-function paths remain unknown. Native/x86 publication
failure negatives pass. New r315/r316 and r319/r320 are exact positive pairs,
not replacements for older counterexamples. A same-input r263 prefetch-on/off
intervention changes actual selected VS source on 872 of 1,868 draws while
preserving the identical 72-pixel failure; all other serialized creation
fields match, including unchanged first-bad four-draw receipts. Original r300
also retains its two-pixel failure with prefetch off. Older originals lack
new creation metadata, so this is causal replay/replay evidence, not original
PSO proof. Actual texture/view/sampler binding and GPU-time content equality
remain open. Actual fragment sampling descriptors r328–r335 now cover direct
Metal calls and bounded actual argument-table writes; native exact mip-view
pixels and null/reopen/offset/setBytes/raw-mutation/eviction negatives pass on
native and x86. Current replays retain the same r263 72-pixel and r300 two-pixel
failures, with respectively 7,617/7,519 actual direct texture/sampler stage
entries under the expected stage mask and no unknown descriptors. GT2 does not
provide argument-table coverage; the native fixture does. Original sampling
receipts, actual resource/content joins and selected shader ABI consumption
remain unproven. r323/r326/r334 r263 attempts reach readback but are rejected
by the source oracle, not successful completed iterations. The lower r300
diagnostic pixel envelope is still not the strict public gate. All original
failures remain open. Process-local actual texture tokens now join issued
binding to selected-view readback, with native/x86 root/view/null/off and
EventSerial/SegmentSerial production readback pins. Fresh r346 provides 7,573
paired actual texture/sampler descriptors each without differences, yet r347
retains 69 wrong pixels. Six generation-qualified cube input pairs differ by
the same 2,168 float texels; their complete faces and no-writer intervals are
checked. The first scene difference precedes cube sampling, so this is not its
causal attribution or a replacement for missing r300 input bytes. Prefetch-off
r349 makes all first-bad eight actual PSO construction receipts equal to the
original while retaining identical cube and final wrong bytes. Both attempts
are rejected through readback, not successful completed cold iterations.
Actual depth/stencil creation/binding/reference observation now has native/x86
exact depth/stencil rejection/update pixels, unknown/conflict/off negatives,
and production three-draw row joins. New r359/r364 original/replay pairs have
1,857/1,859 equal actual depth descriptors and exact final pixels; they are
diagnostic positives, not replacements for failures. r363 preserves r346's
identical 69-pixel mismatch on the new build. The failing older originals lack
actual depth witnesses, and GPU-consumption-time contents remain unproven.
Actual direct-buffer/cache coherence now observes successful Metal slots 0/3
and offset/setBytes transitions with non-reused process-local identity. Native
stale-rebind pixels, A/B/A and production three-draw/off/filter pins pass.
r373/r375/r376 each have 7,412 actual/cache matches and equal observed bound
bytes/84 replay scene hashes, but retain r346's identical 69-pixel failure.
API/GPU validation reports no access error; the existing dedicated transient
diagnostic changes actual allocation counts without changing pixels. These
same-build replay interventions use an older original: they do not prove its
binds, original-side lifetime or GPU-time contents. r370 identifies the shared
2048² R32F shadow sampled by the first divergent eight draws; its original
content evidence remains missing. The r377 audit preserves strict rejection.
The bounded shadow-input capture r379 times out before 500 Presents and emits
no Tape, despite the catalogue's timeout-permitting `pass`. A retained sample
places replay in Arena admission and encode in a CPU-ready session wait;
the retained PC now identifies the open-session/empty-Ready branch. r382
reauthenticates this as failed capture evidence, not pixels. r383–r389 reproduce
and fix a concrete low-water latch/session-cap composition: a deferred Clear
pins 1025 pages above capture's 1024-page reopen threshold. Native, x86, GPU
smoke and full TLA gates pass; the high-water-only model fails as expected.
One repaired-build live run (r390) now exits normally without timeout and
publishes 176 settled sources/1862 draws; this does not exclusively attribute
every sampled r379 wait. The same-build r391 replay has exact final RGBA.
The actual shadow-input join executes on six readbacks with matched object,
generation, source/pass/draw and no-writer witnesses: all six repeat one R32F
texture difference (934 float texels, maximum 60 ULP / 3.5763e-6) after writer
draw554, despite the exact final output. This is a newly authenticated
intermediate difference, not proof of sampled texel/branch divergence or a
replacement for any failing original. The r393–r396 follow-up identifies 395
shadow writers with equal observed PSO/raster/depth/geometry inputs and exact
replay A/A shadow bytes. All six selected VS and their reachable helpers only
read FFP fields outside the differing material/light region. This bounded
source-use audit does not prove driver instructions or GPU-time contents.
The r397/r398 capture/replay pair now narrows the first differing existing
shadow endpoint to source22267's eleven draws260–270, after eleven equal
stored endpoint hashes. Its final RGBA is exact; original per-pass raw bytes,
the first differing draw and GPU-consumption distinction remain open. Do not
attribute this intermediate difference to an old final-pixel failure yet.
The bounded paired-depth observer now passes native/x86 real-Metal byte and
negative-selection tests. It distinguishes native Depth32Float (252) from
D24X8's Depth32Float_Stencil8 (260), extracting only its depth plane. New
r405/r406 retains 32 source/Store/canonical-generation-qualified paired pass
endpoints: nine exact, then simultaneous color/depth differences in source22176,
draws245–255 (121 depth texels, maximum2 ULP). Final RGBA differs by one RGB
pixel. Removing earlier replay readbacks preserves the same wrong bytes in
r409. This is a new retained failure and localization evidence, not policy
qualification or proof of a first draw/GPU-consumption cause. Actual first-bad
PSO/raster/depth/geometry and VS/FS values match; FFP backing differences remain
an observed distinction, not a proven cause.
Observer-free normal replay r411 retains identical wrong source/output bytes;
r412 reauthenticates the original/source plan and confirms public GPU rejection
with `diagnostic=false`. Full TLA/lineage/install-name gates pass. Rendering
fidelity remains open independently of diagnostic implementation coverage.
Diagnostic prefix cuts r414-r418 localize the replay depth writer to global
draw251/replay ordinal252. Canonical VB/IB restoration matches the issued CPU
range hashes. The r420-r429 standalone discriminator uses unchanged captured
MSL and 1,501,552 bytes of input; three triangles reproduce all 121 replay depth
values, with 68/51/2 coordinates in their separate outputs. Repetition and
validation-off preserve reduced depth/color bytes. This is not a production
route qualification, full-attachment oracle or proof of original GPU reads.
The original-context buffer/vertex-output distinction remains open, as do all
ten retained final-pixel counterexamples.
Standalone r431/r438-r441 calibrates bounded GPU position min/max observations:
seven vertices/nine invocations, no conflicting duplicate values, unchanged
full depth/color and identical outputs under four caller rounding modes.
The warning/missing-schema r433-r436 receipts are excluded and preserved.
This standalone evidence is not a production fenv exclusion. The bounded
actual-draw collector is now implemented: supported generated source + unused
slot 29, actual-PSO clone/restore, once-only issue, duplicate-safe atomic
min/max/counts, and completion-pinned output. Native and x86 Metal fixtures
verify independent positions/pixels, preflight rejection, restoration, budget,
disabled/malformed settings and no files before completion. The public gate
rejects its directory independently, including literal `0`. r453/r455 observes
all 1872 vertices of r405's localized writer; validation-off preserves the
entire old depth/final output and its one-pixel failure. Validation-on retains
the same depth/positions but adds one final pixel; it is not baseline-equivalent.
Original r456 captures ten source/generation-qualified position receipts.
r458/r459/r460 pairs original source22174/draw787286 with replay27/draw260:
139 of 1872 actual GPU clip positions differ despite matching source/PSO
receipts and issued CPU geometry/VS/PS bytes. The first nine paired depth
endpoints are exact; the tenth differs at 807 texels, at most 2 ULP. Probing
does not change replay depth or final pixels. New r456 final pixels happen to
match; this does not close any of the ten old failures. Original GPU-time
input bytes versus arithmetic/compilation attribution remains open. FFP
material/light backing still differs; matching CPU half-pixel fields are not
independent GPU-consumption evidence. Detailed input/intermediate r465/r466
changes the 139 replay positions and the whole selected depth to match the
original, failing passive calibration. Thirty-four bounded standalone GPU
controls r469–r477/r480 reversibly reproduce this immutable-input difference
with skinning MAD fusion versus separate rounding. They do not prove exclusive
original-context input/arithmetic attribution. r478 repairs r405's selected
depth, not its final one-pixel error. A default-off digest-selected no-contract
compiler control, independently rejected by the public GPU gate, extends its
exact shadow prefix from 9 to 12 endpoints (r483/r486/r488). The next divergence
is source22179/replay30, eleven draws with three other vertex programs. A bounded
four-source control r492/r494 subsequently makes all 32 selected shadow color
endpoint hashes exact while retaining the identical final one-pixel error;
the remaining scene/other shader path and final-pixel cause are open. A bounded
exact-source scan and singleton controls r499–r522 localize a target-pixel
intervention to VS `bad49ade...`, source22283/replay134's first draw
(global1380, original ordinal788346; 4608 indices / 1480 vertices). Pass-end
and per-draw ROI observations preserve both full final images, but the
intervention makes only the target exact while introducing 790 other differing
pixels. This is not original intermediate evidence or a production arithmetic
fix. Two unmatched source digests and r518/r519's absent per-draw observations
are explicitly excluded. Same-build r346 controls r496–r498/r515 retain its
69-pixel failure under the four-shadow intervention; the two-scene-VS control
worsens it to 1751. The retained scene draw now has full-image calibrated
standalone replay under baseline and matched-intervention inputs (r524–r553).
The46-pixel inherited color-seed difference is disjoint from the14-pixel local
response; depth and all76 sampled texture subresources remain equal. Twenty-seven
bounded arithmetic controls reversibly localize the target response to the
last skinning MAD. API/shader-validation controls preserve the images after
correcting the fixture's PS allocation to the declared argument size; the prior
short-allocation validation failure is retained. This is a replay discriminator,
not original GPU-consumption equality, exclusive original-context attribution
or a final-image repair. All ten original failures remain open. This is
not all-frame attachment/depth or original GPU-read equivalence. Native/x86 source and
actual-Metal diagnostic fixtures plus public CLI/lineage/full TLC pass; no
production arithmetic default or old failure is closed. Explicit failed-GPU-completion fault
injection is still absent; failed status is rejected by the completion path.
The repaired build's r387 replay preserves r346's byte-identical 69-pixel
failure; the progress repair is not GPU-fidelity closure.
r340 produced no Tape and is excluded despite catalogue pass.
Remaining: localize the earlier scene-write divergence and close the
original r28, r146, r171, current-build r201, r225, r254, r263, r300, r346 and
r405 strict counterexamples. Actual Direct
growth counters remain evidence, not a zero-growth claim.

Domain-owned implementation and evidence gap tracker. Use the [root gap index](../gap.md) for cross-domain rollup.

## Experiments Layer

✅ Structurally complete for the two-lane contract (R-WILD-1.2 as revised
2026-08-06): the runner, launcher harness, output layout, verified native-lane
sample entries, and the Wine-lane wild catalogue with its manifest-governed
runtime (`R-RT-*`) all exist. The former "partial because Wine, not native
injection" framing described drift against the pre-catalogue clause and is
retired with that revision.

| Area | Status | Spec |
|---|---|---|
| Render Tape v2 segmented identity (`R-HARN-REPLAY-7.21`, `7.22`, `7.25`) | ✅ bounded production group admission/publication, cross-segment projection, and settlement evidence implemented | The current implementation has only the v2 reader/writer and fixed pointer-free v2 tables; v1 bytes are Retired and rejected by the retained negative pin. V2 defines one event as ordered authoritative source segments with exact record partition, pass-piece continuity, atomic publication, and bounded settlement. GT2 r17 authenticates 147 capture segments and all 43 settlement rows, then projects event 899 records 0..18 across two source rows into one derived two-segment bundle. | Broaden interval/provider grammar and cross-workload evidence; never infer identity from event order or revive v1. |
| Capture SegmentSerial source bound | ✅ bounded production selector, atomic admission, and multi-source GT2 mechanism evidence | The capture Arena remains 2,048 4-KiB pages (8 MiB), while queue admission remains 512 pages/source for EventSerial/default, startup/non-capture raws, and capture-off. Only an authenticated token+event-ordinal capture event selected for queue-immutable `DXMT9_RENDER_IDENTITY_MODE=segment` applies the call-local 64-page-per-source planner bound; events crossing it use the Arena batch lease and publish payload, descriptors, controls, and Ready entries all-or-nothing. GT2 r17 completes with 147/147 source segments, 43/43 settlement rows, zero watchdog/GPU errors, and a two-source strict projected oracle. | Promotion remains open: the r17 full-frame provider replay differs from capture at 15,193 pixels (maximum RGB delta 27, total delta 23,987), outside the pixel envelope. Keep SegmentSerial default-off pending paired visual/locality/no-gputrace and cross-workload gates. |
| PE Render Tape capture boundary | ⚠️ bounded frame/sequence production capture, one-shot standard-Present output digest, and transactional publisher | The default-off device registry owns exact descriptors/payloads and supported CPU-written contents from creation, then serializes the actual PE shadow plus generation-qualified object and initial-mutation seeds at the arm Present. `DXMT9_RENDER_TAPE_PROFILE=sequence-tape` explicitly keeps two consecutive standard-Present intervals in one journal, preserving one complete digest-backed mutation between them; interval 1 is not sealed or published. `ObjectDefine` expected extent/count and pre-effect unique-subresource summed-byte closure reject incomplete checkpoints. Texture and Surface definitions now have one production representation: canonical V2 headers with exact subresource descriptors and storage/disposition/parent metadata; the unversioned level-0/count texture payload and raw surface payload are Retired. Bounds-checked uncompressed and DXT1/DXT3/DXT5 2D/cube face+mip locks plus complete-buffer locks now close pre-command resource bytes; full locks establish row-tight seeds, while partial locks overlay only an existing exact-size seed and emit the resulting complete content. If an already-lockable 2D texture subresource is first touched by an unseeded partial writable lock, capture may take one exact-owner full CPU-visible lock after user unlock, strip pitch, and record one complete seed/current-content snapshot; it admits only the exact bytes returned by that successful full lock and never admits inferred or otherwise unproven allocation bytes or the partial seed. Full-lock/copy/unlock or identity/extent/generation proof failure rejects only the tape with a typed first diagnostic. Texture-derived 2D surface wrappers now take the same exact-owner fallback after their original surface unlock, after proving the owner texture identity and generation-qualified subresource; standalone, cube, volume, and user-memory surfaces remain surface-owned and do not use the fallback. This capture-only relock may repeat backend dirty/autogen work even though the bytes are read-only; that side-effect/performance debt is not a promotion claim. The exact Firefly Forest GT2 blocker is now the r25 generation-qualified Texture2D identity whose canonical V2 descriptor is 1024x768, format 22, `D3DUSAGE_RENDERTARGET`, and `D3DPOOL_DEFAULT`; its first captured consumer is an `APPLY_STATE` record and its required 3,145,728-byte subresource seed is absent. The separately observed 128x32 CPU-visible texture completed its full snapshot and is not this blocker. This bounded capture closure does not itself claim general GPU readback, indexed GT2 replay, identity-sidecar completion, or executable projection; the separate r57 bundle supplies provider-replay evidence. For each captured canonical `PRESENT`, the PE owner drains prior replay and renderer work before reserving a one-shot existing-Presenter offscreen mirror; the normal drawable remains intact and the mirror uses the same present PSO/source/gamma in that command buffer. Finish drains the captured replay, flushes its renderer work before checking the ticket, reads tightly packed bytes, hashes SHA-256, and validates fixed result metadata before `PresentComplete`; cancellation/failure aborts only the tape without changing Present HRESULT or publishing. Volume locks, missing initial bytes, prior-backbuffer, PresentEx, and broader provider grammar remain open. |
| `experiments/CATALOGUE.toml` + launcher tree scaffolded | ✅ | R-WILD-5.1 |
| Wine-lane launcher harness (`run_experiment.py`, launcher scripts, manifest-selected runtime staging) | ✅ | R-WILD-1.2 (Wine lane), R-RT-* |
| 3DMark05/06 canonical benchmark lanes | ✅ named invocation and bounded `.3dr` artifact capture; runtime evidence remains per lane | R-HARN-RUN-6.1–6.6. One shared preset table defines bounded scene, CPU diagnostic, score, feature, batch, and exhaustive suites for both products; both launchers reject unknown names, preserve raw arguments as an explicit `custom` lane, and `run_experiment.py` records the resolved identity in `result.json:benchmark_lane`. Catalogue runs now inject a unique result basename when absent, snapshot bounded source roots, atomically publish only created/modified regular `.3dr` files under `benchmark-results/`, and record digest/provenance or `not_emitted`. Native contracts cover presets, raw precedence, stale/symlink exclusion, overwrite detection, basename collision, digest identity, and missing output. This closes reproducible selection and artifact custody, not scene correctness or performance evidence. |
| 3DMark06 external experiment lane runtime evidence | ⚠️ GT1/GT2/HDR1/HDR2 render paths qualified; unattended result export remains open | The installed Advanced Edition runs the four graphics scenes under Sikarugir with perf-profile frame evidence and zero GPU command-buffer errors, command-chunk rejects, shader-library failures, or no-pipeline draw skips in the qualified runs; the HDR pair reports score 1888. The final child-current GT2 observer-off retry also reached the scene with 1,860 encoded/full/valid presents, 2,031,373 draws, and a non-black Firefly Forest screenshot (`experiments/output/app-d3d9-3dmark06-final-gt2-20260830-r1`), but timed out at the catalogue-allowed boundary (`returncode=143`) and emitted no `.3dr`; its overlay showed 19 FPS at frame 195, not an official benchmark FPS. Its Professional-only per-test switches are accepted as arguments but do not start the selected lane, so HDR required one UI selection. The positional `.3dr` request remains `not_emitted`; automatic discovery now proves absence without accidentally reusing an older file. CPU1/CPU2, screenshot oracles, Professional-edition unattended selection, and benchmark-owned result export remain unqualified. |
| Internal backbuffer frame dump + SSIM comparison + `result.json` output | ✅ | R-WILD-2.3, R-WILD-4.1 |
| Bootstrap verified entry: `conf-d3d9-wsi-present` on Heroic Wine 11.5 | ✅ | local workflow validation |
| Verified real application entries: `sample-d3d9-basic-hlsl`, `sample-d3d9-tutorial07`, `sample-d3d9-hdr-formats`, `sample-d3d9-dxut-simple`, `sample-d3d9-irrlicht-lights` | ✅ | Heroic Wine 11.5, direct capture, SSIM 1.0000 |
| Initial catalogue from R-WILD-3.1 staged and verified | ✅ | All five required feature groups covered |
| Reference screenshots for initial catalogue entries | ✅ | R-WILD-4.1 |
| Harness script evidence-production contracts (`scripts/tools/`, `scripts/run_apps/`, `scripts/check/`, `scripts/run_suites/`) | ⚠️ | `specs/experiments/harness/requirements.md`, `specs/experiments/harness/spec.md` |
| Harness `R-HARN-*` requirement enforcement | ❌ | Requirements in `specs/experiments/harness/requirements.md` are phrased as predicates but nothing evaluates them; no checker exists. Deliberate scope choice on 2026-07-27 for the docs-only specification round — means these documents can corrode exactly as the harness scripts they describe already have |
| `replay` domain harness (`scripts/tools/run_3dmark05_mini_replay.py`) | ✅ | Works as of 2026-07-28. All five defects `specs/experiments/harness/requirements.md` §2-§6 derive their rationale from are fixed, plus a sixth found while debugging them: defects 1 and 2 (direct-cbuf signature rewrite, sliced-stream-offset double-count) in `12348666`; defect 6 — every draw's PSO, depth state, and cull mode collapsed onto draw 0's state — in `07c39ecb`; defect 5 (`--force-fragment-color` returning a bare `float4` from a function declared `FSOut`/`FfpFsOut`) in `fe673fd5`; defect 3 (unrecognized `core::Format` silently becoming `RGBA8Unorm`) in `e2d3ed0e`; and defect 4 (black image, exit 0) in `36a41ad5`. Defect 4's root cause: the generated program bound nothing to fragment `buffer(5)`, while every dxmt9 fragment shader declares `constant FsVolatile& fsVolatile [[buffer(5)]]` and drives an alpha-test switch ending in `discard_fragment()` from its contents — the undefined read discarded every fragment. The vertex stage had always bound its own `DrawVolatile` at the same index; only the fragment volatile was omitted. Verified end-to-end: replaying the GT1 frame60 `enc1` manifest renders the recognisable "Return to Proxycon" interior, 12,231 distinct RGB values over 784,476 of 786,432 non-black pixels, untextured because the replay binds white dummy textures |
| Unified Render Tape `frame-tape` / `sequence-tape` profiles (`R-HARN-7.1`–`7.6`, `R-HARN-REPLAY-7.1`–`7.23`) | ⚠️ bounded refinement, production capture/provider replay, capture identity, reduction, executable projection, and parallel join | Production validation/provider replay retain unchanged canonical chunks, while texture/surface schema payloads use only the canonical V2 grammar; legacy texture and raw surface descriptors are Retired. The r57 Sikarugir GT2 bundle closes indexed production provider replay with an authenticated `output.rgba` sidecar and the bounded R-HARN-REPLAY-7.19 pixel envelope. The r65 capture closes original capture-authority source/pass identity, and r66 closes one `FULL_SNAPSHOT`-anchored executable GT2 projection through two candidate plus two strict fresh-provider runs. The default sparse-delta start-state seam is closed at the pure native and hermetic fixture boundary by folding the canonical BootstrapState and ordered D9C v2 sparse records; the 2026-08-22 r3 Sikarugir run additionally closes a production default-delta GT2 capture and sidecar-proven strict projection through two fresh identity-provider runs at the explicit 64-record chunk cap. Full-frame provider replay remains bounded by its typed pixel-envelope tolerance rather than strict equality. R-HARN-REPLAY-7.23 closes the bounded fresh-process identity/ExplicitParallel implementation join; its supervised GT2 recipe now executes non-vacuous worker work but remains rejected by the strict capture-to-provider pixel oracle. Strict SHA equality remains the default promotion gate. The default 256-record GT2 capture still fails the typed `identity_query` join. PresentEx, provider execution semantics for retained reducible controls/destruction, prior-output loads, volume resources, and broader interval counts/provider grammar remain open. |
| Render Tape authoritative identity (`R-HARN-REPLAY-7.21`) | ✅ bounded EventSerial and SegmentSerial capture joins with current v2 wild evidence | `device_c_render_tape_identity.*` owns the only implemented fixed pointer-free grammar, v2, with fail-closed exact command/event-record coverage, monotonic source/sequence, bounded token, pass range, and settlement validation. GT2 r17 publishes 147 actual source/sequence rows, 162 pass ranges, and 43 exact event settlements; the cross-segment projection preserves two source-qualified rows. The earlier r65/r66 bundles remain historical evidence only. Capture-off and ordinary streaming bounds are unchanged. | Compatibility/control sources continue to fail closed rather than receiving inferred identity; broader provider grammar and promotion evidence remain open. |
| Render Tape v2 to executable GT2 draw-slice (`R-HARN-REPLAY-7.16`/`7.17`/`7.22`) | ✅ bounded default-sparse and cross-segment GT2 executable lanes proven | `device_c_render_tape_projection.*` validates one sidecar-proven pass, folds canonical BootstrapState plus ordered sparse state/constant writes through the selected first Draw, emits one deterministic APPLY_STATE|FULL_SNAPSHOT bootstrap, rebuilds canonical Clear → contiguous Draw range → Present bytes, and conserves generation-qualified object/blob/mutation closure. GT2 r17 selects event 899 records 0..18, 19 indexed Draws crossing source rows 13908 and 13909. The published 88-event derived bundle has events SHA-256 `484adb56...`, output SHA-256 `f4760301...`, two provenance segments, deterministic fresh replay twice, strict non-degenerate oracle equality, and zero GPU command-buffer errors. Earlier r66 and r3 evidence remains valid historical coverage. | PresentEx, retained reducible controls/destruction, prior-output loads, volume resources, and broader interval/provider grammar remain open. Some r17 ranges correctly fail closed on unresolved produced-output proof; final-pass non-indexed ranges that materialize but return `D3DERR_INVALIDCALL` remain a typed provider-closure gap. |
| GT2 Render Tape parallel verification recipe | ⚠️ executed, promotion gate rejected | Capture one bounded GT2 `frame-tape` with `DXMT9_RENDER_TAPE_CAPTURE=1`, `DXMT9_RENDER_TAPE_PROFILE=frame-tape`, an explicit `DXMT9_RENDER_TAPE_OUTPUT_ROOT`, `DXMT9_PE_DRAW_FULL_SNAPSHOT=1`, and `DXMT9_PE_CHUNK_MAX_RECORDS=256`; keep capture itself on `DXMT9_RENDER_PARTITION_MODE=identity`. After structural validation, run `scripts/run_python.sh scripts/tools/run_dxmt9_render_tape.py parallel-verify <bundle>`; it launches at least two fresh processes per identity and ExplicitParallel mode and requires strict equality within each mode and across modes. A production-capture or output-oracle bundle must report `validity.output_non_degenerate=true`; every run must also report `selected>0`, `children>=2`, `draws>0`, worker batches/tasks/active peak all greater than zero, and zero GPU command-buffer errors. Structurally valid uniform/black bundles remain available for generic fixture `provider-replay`, conservation, and schema evidence, but are not promotion evidence for this join. The 2026-08-16 r67 black bundle now fails the explicit non-degeneracy gate. The skip-200 r71 bundle is non-degenerate and runs one 345-draw pass as five ExplicitParallel children in both fresh runs (`worker_tasks=5`, `worker_active_peak=5`, zero fallback/GPU errors); identity and parallel reproduce the same SHA-256 `2c00f0cb...`, but both differ from the capture-time oracle at 323 pixels, so the strict production oracle correctly rejects promotion. CPU/wall timing is informational and is not a non-vacuity gate. This evidence does not lower any threshold or change the default-off/identity policy. |
| `replay` domain output-validity self-assertion | ✅ | R-HARN-REPLAY-3.1, R-HARN-REPLAY-3.6, instantiating R-HARN-3.1/3.2/3.3. Closed 2026-07-29 (defect 7). `run_3dmark05_mini_replay.py --run` now reads the written color output back, counts distinct RGB triples and non-background pixels, records both in `mini-replay-summary.json` under `validity`, and exits non-zero when the image carries a single distinct value — the exact shape of defect 4's four all-black lanes that printed `draws=229 repeat=1` and exited 0. The threshold is the named constant `MIN_DISTINCT_RGB_VALUES = 2`; no percentage-coverage gate was added because the degenerate case is the only failure shape with evidence behind it. `validity` is present on every invocation, stating explicitly when the assertion did not run. Verified end-to-end: the GT1 frame60 `enc1` manifest passes (12,231 distinct values over 784,476 of 786,432 non-background pixels); the same manifest sliced to its 42 depth-only draws exits 1 naming the degeneracy. The generated program applies the same threshold to the same image as of 2026-07-29 (R-HARN-REPLAY-3.7, row below), so the guarantee no longer depends on which entry point was used |
| `replay` domain replay binary self-asserts when invoked directly | ✅ | R-HARN-REPLAY-3.7, instantiating R-HARN-3.1/3.3. Closed 2026-07-29. The generated Objective-C++ program now counts distinct RGB triples in the same readback buffer `writePpm` wrote from, prints `distinct_rgb=<n>` on the existing `mini replay draws=<N> repeat=<R>` line, and returns `REPLAY_DEGENERATE_EXIT_STATUS` (3) — naming the output path, the count, and the threshold in the wrapper's own wording — when the count is below the threshold. The threshold is interpolated into the generated source from the single `MIN_DISTINCT_RGB_VALUES` constant the wrapper assertion uses, so the two cannot drift. The wrapper assertion stays and is not redundant: it is the only side that records `validity` and diagnoses a missing, truncated, or unreadable image, so the binary's degenerate status is a dedicated value that `run_binary()` carries through to that assertion instead of surfacing as an opaque subprocess error. Verified end-to-end: the 42-depth-only-draw slice invoked directly as `DXMT9_MINI_REPLAY_COLOR_OUTPUT_PATH=... ./dxmt9-3dmark05-mini-replay` prints `distinct_rgb=1` and exits 3 where it previously printed `mini replay draws=42 repeat=1` and exited 0; the full `enc1` manifest prints `distinct_rgb=12231` and exits 0 |
| `replay` domain pixel results can be proven to execute the path under test | ✅ | R-HARN-REPLAY-3.4, R-HARN-REPLAY-3.5, R-HARN-REPLAY-3.8. Closed 2026-07-29 (defect 7). The two questions stay separate and are answered by separate mechanisms. **Containment** is `coverage`: `mini-replay-summary.json` names the manifest rows and encoders replayed, the draw and shader-variant counts, and the per-`vs_hash`/per-`ps_hash` draw counts (summing to the draw count), plus a `scope` string stating this is a single-encoder slice of one frame that does not establish any branch executed. **Execution** is the opt-in `--prove-executed 'REGEX=>REPLACEMENT'`: it prepares a second shader tree under `<output-dir>/execution-proof/` with `re.sub` applied to every generated `.metal` source, replays it, compares the two images, records `execution_proof` (pattern, files scanned/mutated, sites, differing pixels, verdict), and exits non-zero when they are identical, because an identical image proves the mutated construct was never executed. The two failure modes are separate named verdicts and separate messages: `not-present` (matched no site — the pattern is wrong or the construct is absent; **not** an execution verdict, and decided from the generated sources before anything is compiled) and `present-but-not-executed` (matched sites, byte-identical image — defect 7's shape). Verified against defect 7's own manifest: the literal `dxmt9_cdef<N> : ` pattern reports `not-present` (0 sites across 34 generated sources), because those dumps predate `d63f7a65`'s emission; reconstructing the DEF select over the inputs the dumps do carry (`cFloat[196] = ...` overlays plus `cFloat[clamp(a0.x + N, 0, 255)]` relative reads, in exactly eight vertex shaders) matches 48 sites in 8 of 34 sources and changes 0 of 786,432 pixels, while the unconditional-marker control over the same 48 sites changes 15,134 pixels — independently reproducing the original hand investigation's figure. A pixel-identical replay still must not be used as the correctness oracle for a shader-translator or codegen change unless `--prove-executed` (or equivalent instrumentation) establishes the changed path executes in the replayed window |
| `replay` domain resolved attachment formats recorded in `mini-replay-summary.json` | ✅ | R-HARN-REPLAY-2.3, instantiating R-HARN-2.4. Closed 2026-07-29. `prepare()` now writes an `attachment_formats` block carrying, for colour and depth, the resolved `MTLPixelFormat` name beside the `core::Format` ordinal it was resolved from, the attachment dimensions, whether depth carries stencil, and whether the value came from a declared manifest attachment or the legacy no-`attachments` default. `resolve_attachment_formats()` is the single resolution point: `render_source()` bakes the same record into the generated `.mm`, so the artifact cannot name a format the replay did not render with |
| `docs/perfomance/overview-sfiv.md` SFIV baseline figure | ✅ | Reproduced 2026-07-29. The `11.3` sampled fps that looked like a 4x regression was the `debug` experiment profile — validation layer on, debug logging, 1.0 GB of log. The same build under `perf` gives `43.02` sampled fps with a 22 MB log, inside the documented `44.668`/`45.416`/`42.684` family. The profile now defaults to `perf` (`d431dc1a`) and is recorded in the run output, so the trap that produced the false reading is closed |
| `scripts/tools/summarize_3dmark05_cleanup_candidates.py` citation counting | ❌ | Miscounts brace-expanded citations such as `...-r{1,2,3}-...`, classifying 84 referenced runs (4.5 GB) as unreferenced and eligible for cleanup when they are not |
| `docs/perfomance/` `source:` citation integrity | ❌ | 33 of the 55 distinct `.log` paths mentioned in `docs/perfomance/` are already missing from disk as of a 2026-07-27 scan (34 of 56 when the same scan also covers `docs/`, `agents/`, and `README.md` as a whole, not only `docs/perfomance/`); the citations are dangling |
| Harness domain map (`specs/experiments/harness/spec.md` §1) is a partial partition, not a complete one | ⚠️ | R-HARN-1.1. Of the 86 in-scope harness scripts (`scripts/tools/`, `scripts/run_apps/`, `scripts/check/`, `scripts/run_suites/`), mechanically applying the domain map's own `Owns` column (explicit names plus its `scripts/check/*` and `scripts/tools/compare_*` wildcard rows) assigns exactly 41. The remaining 45 are owned by no domain: 11 are named by `reduce/spec.md` §2.4 while it explicitly declines to assign them a domain (verified against that section's own listed 11 filenames); 5 more are named individually elsewhere without a domain assignment (`analyze_indexed_probe_classes.py`, `analyze_shader_dumps.py`, `analyze_xcode_dxmt_encoder_attribution.py` in `join/spec.md` and `join/requirements.md`; `run_with_timeout.py` in `probe/spec.md`; `shader_corpus_tool.py` in `audit/spec.md`); and 29 are not named in any of the sixteen tracked harness documents at all — including `run_dx9_present_policy_ab.py` (661 lines, a documented workflow in `agents/rules/metal_debugging.rules.md` §7), `run_d3d9_conformance.py` (521 lines, whose `:264` sets `DXMT9_PREWARM=disabled` into its launched subprocess with no domain to attach that contract-relevant variable to per parent `spec.md` §4 Rule 1), `analyze_pso_backend_churn.py` (585 lines), and 26 other `scripts/tools/` scripts (mostly `analyze_*`, plus `audit_backend_escape_surface.py`, `cleanup_dxmt9_temp_prefixes.py`, `gen_wine_d3d9_test_inventory.py`, `package_app_local.py`, `plan_backend_escape_reduced_ab.py`, `plan_effect_roi_forcewhite_probes.py`, `run_3dmark05_semantic_replay_gate.py`, `select_3dmark05_payload_window.py`, and `sync_corpus.sh`). No checker enforces domain assignment; this is a documentation-completeness gap, not a runtime one |
| `compare-gate → record` boundary (`specs/experiments/harness/spec.md` §2) cites no parent `R-HARN-*` requirement | ⚠️ | Extracting every `R-HARN-\d+\.\d+` token from each `###` boundary subsection in `spec.md` §2 returns a non-empty match set for seven of the eight boundaries and an empty set for exactly one, the terminal `compare-gate → record` section (`spec.md:254-274`). `specs/experiments/harness/audit/requirements.md` R-HARN-AUDIT-1.2 already discloses this accurately and extends parent principles to the `audit` domain's own `record`-stage requirements by stated analogy rather than inventing a citation; this row records the gap in the parent document without adding a parent requirement to close it, so as not to contradict what `audit`'s own documents already say about it |

### Render Tape bootstrap exact closure (frame-tape capture only)

The frame-tape arm-boundary producer now materializes only the exact
generation-qualified
closure reachable from the validated bootstrap overlay, required Present
output, and recursive descriptor dependencies such as a texture-derived
Surface's parent texture/subresource. Unreferenced incomplete live objects are
pruned; referenced missing, stale-generation, incomplete, or dependency seeds
fail closed with typed attribution. A complete omitted pre-arm object is
materialized just in time before its first command/control reference. Its exact
seed expectation is independent of unrelated events; a missing or incomplete
seed fails closed at that identity's first use. Mutations and destroys of
unadmitted objects remain registry-only. Arbitrary deferred closure for opaque
producer-side references remains out of scope. Sequence-tape retains the
complete all-live arm snapshot because its second interval cannot admit
`ObjectDefine` events.

### GT2 frame-tape retry lifecycle (capture-only)

`PresentOutput` is a capture-owned single-holder role. The admission that names
a new holder hands the role back first, and an arm attempt that does not reach
an active interval hands it back immediately rather than at the next attempt.
A surface holder the admission itself registered and that still carries only
the admission's own wrapper reference is retired into the tombstone set, so the
existing monotone-generation and alias-replacement rules keep applying to it
unchanged; any other holder is demoted back to its exact displaced
initial-content state and stays registered. Retirement is scoped to that proven
swap-chain output handoff — a generic standalone or texture-derived alias the
capture merely re-roled is only ever demoted, so the policy never removes an
entry the alias rules still own. This closes the two r6 GT2 failures in
`experiments/output/app-d3d9-3dmark05-gt2-frame-tape-exact-closure-r6-20260814`:
`present_output_count count=2..8` across retries, and the terminal
`prior_not_retained_alias` registry invalidation that a recycled wire object id
produced when it met a stale holder in the logical-slot replacement scan.

The **initial** r6 abort remains open. Its first arm reached an active interval
and then aborted with `first_abort reason=block_resource_mutation`, with no
attribution for which branch failed. Only diagnosis, not a fix, is claimed
here, and the fail-closed behaviour is unchanged. The append can be rejected by
exactly three things — the PE registry shape, blob registration, and the
mutation event's own validation — so the owner now drives the blob-register and
mutation steps separately (the same pair `resourceMutationBytes` performs) and
emits one bounded `mutation_reject` line naming the failing step
(`registry_entry_missing`, `subresource_out_of_range`, `blob_register`,
`mutation_event`), the status, the capture state, identity/subresource/bytes,
whether the identity is still live in the tape, and each counter against its
limit (`event_count`, `buffered_bytes`, `owned_blob_bytes`,
`owned_blob_entries`). That makes the mutation event's fused `InvalidInput`
decidable rather than inferred. At that point no capacity was raised and no
predicate relaxed: no correctness-preserving layout or registry-only treatment was
provable from the existing artifact, so none was applied, and the next GT2 run
must name the failing predicate before any further capacity change is considered.

The subsequent GT2 r7 exact-closure capture attempt log identifies the failing
predicate: line 63 records `detail=blob_register status=CapacityExceeded` with
`owned_blob_bytes=66847615/67108864`, `incoming=524288`, and
`overage=263039`, so that run established a required owned-byte lower bound of
67,371,903. GT2 r8 then proved that the former 68 MiB value was only a prefix
lower bound, not a sufficient total-bundle bound. The capture-only policy is
now a 256 MiB default, an optional decimal-byte
`DXMT9_RENDER_TAPE_MAX_BLOB_BYTES` override, and a hard 1 GiB ceiling;
unset/invalid/zero values use the default and valid over-ceiling values clamp
to 1 GiB. Digest, descriptor, generation, event, and replay validation remain
unchanged. A complete GT2 bundle fitting this policy is still unproven; the r7
capture attempt log is not a bundle artifact, and r8 is not evidence that the
default is sufficient for the whole bundle. PresentOutput retry ownership is a
separate fixed capture correctness issue and is not used to justify this
capacity policy.

### GT2 r9 surface identity closure (capture-only)

GT2 r9 removed the 256 MiB blob-capacity blocker: the capture attempt no longer
fails at `blob_register` capacity. Its next typed failure is
`unmaterialized_pre_arm_object kind=SURFACE`, which identifies a missing
generation-qualified Render Tape registry identity rather than another blob
capacity shortfall. Standalone wrappers from the cached swap-chain
`GetBackBuffer` path and device `GetRenderTarget`/
`GetDepthStencilSurface` now register their exact surface identities before
chunk admission; texture-derived aliases continue to register through their
parent texture dependency. `PresentOutput` is admitted against the same stable
cached backbuffer identity used by commands, with a raw wire-identity check
against the cached PE identity to detect cache drift. Exact-generation
fail-close remains unchanged, and duplicate wrapper references are not added
for explicit `Create*` calls or aliases. Native bridge coverage exercises
standalone registration routing, two wrapper references and balanced release,
alias non-regression, and stale-generation rejection.

### GT2 buffer partial-seed closure (capture-only)

The remaining GT2 seed gap for indexed buffer inputs is now narrowed to one
bounded PE fallback: when the generation-qualified registry reports that the
first writable buffer lock is partial and unseeded, the owner may relock the
same `D9CBuffer` over offset zero and exactly `D9CBufferDesc::size` bytes with
`D3DLOCK_READONLY` after the application unlock succeeds. Only the exact bytes
returned by that lock become the complete zero-offset seed. Identity,
descriptor-extent, generation, lock, copy, or unlock proof failures reject the
tape with a typed diagnostic while preserving the application HRESULT; the
capture-off path performs no relock or copy. The read-lock's possible backend
synchronization/dirty side effects remain explicit capture-only debt, and
`D3DLOCK_DISCARD` is not promoted. This does not widen provider indexed-draw
grammar or claim that GT2 replay is complete.

### GT2 r11 pending alias replacement (capture-only)

GT2 r11
(`experiments/output/app-d3d9-3dmark05-gt2-frame-tape-exact-closure-r11-20260814`)
retained surface identity `kind=1 generation=1 object_id=4294967602` for a
pending command (`object_destroy deferred ... pending=1`) but then rejected the
same exact handle as `unmaterialized_pre_arm_object`. The registry lifetime
truth table classified a texture-derived alias with zero wrapper references as
`RetainedAlias` before considering its pending chunk reference, so a later
wrapper for the same parent/subresource logical slot could replace the registry
entry before `captureCommittedRenderTapeChunk` materialized the old handle.

Pending chunk ownership now takes precedence over retained-alias ownership.
Logical-slot replacement classifies that state as
`PendingChunkRequiresFlush`; the capture-enabled registry synchronously commits,
captures, and drains the existing builder with the child flush reason, then
restarts registration once. This preserves the required order of old exact
generation materialization, old command, pending drain and old destroy before
the new generation definition. Flush failure or an unresolved second attempt
invalidates and aborts only capture, leaving the application call and the
bridge-failure retry contract unchanged. Native coverage binds the production
classification to the bounded old-command-to-new-definition sequence; GT2 r12
wild evidence and cross-build validation remain pending.

### GT2 r12 ObjectDefine validation attribution (capture-only)

GT2 r12
(`experiments/output/app-d3d9-3dmark05-gt2-frame-tape-exact-closure-r12-20260814`)
reached final validation with 2303 events and 144 command chunks, then reported
`invalid-object-define` at event index 2. The event is an early seed definition
immediately after `BootstrapState`, before the r11 pending-alias flush sequence;
the artifact did not identify which ObjectDefine predicate failed. The r11
flush ordering therefore remains unchanged and the r12 artifact does not prove
that the flush introduced the invalid definition.

Final validation now carries a bounded, value-owned ObjectDefine failure detail
through the capture-only result. Its typed subreason identifies the failing
intrinsic, descriptor-extent, versioned texture/surface metadata, parent
lifetime, or parent-extent predicate and retains the exact identity, descriptor
kind/declared and observed bytes, immutable-payload fields, expected-content
extent, and safely decoded V2 dimension/storage/disposition/parent/subresource
fields. `finishRenderTapeCaptureAtPresentBoundary` emits that detail only when
final validation fails; it does not change wire bytes, bridge ABI, validator
semantics, or capture-off behavior. Native coverage pins representative
subreasons and extends
`testPendingAliasFlushBeforeReplacementSequence` through Present/PresentComplete
to successful final canonical validation.

GT2 r13 remains open: a new wild retry is required to use the diagnostic and
establish the exact production ObjectDefine failure before any validator or
producer change is considered.

### Canonical V2 texture/surface descriptors

Production capture, registry/bootstrap/JIT materialization, validator,
publisher/inspector/projection, and provider consumers now use one canonical
texture/surface schema payload. Texture2D, DXT, cube, and volume definitions
carry `RenderTapeTextureDescriptorV2` plus one exact `D9CSurfaceDesc` per
subresource; standalone, texture-alias, and swapchain-output surfaces carry
the corresponding `RenderTapeSurfaceDescriptorV2` storage, disposition, and
generation-qualified ownership. Legacy level-0/count texture descriptors and
raw surface descriptors are Retired and rejected rather than accepted behind
a compatibility flag. The event/chunk/bridge wire ABI and hash are unchanged
because descriptor bytes remain `ObjectDefine` schema payload.

The production bootstrap and frame-tape JIT paths now preflight the same
pure V2-only expected-content contract before registering a capture blob,
`ObjectDefine`, or seed mutation. The contract derives each exact tight
subresource extent (linear and DXT1/DXT3/DXT5) and compares every actual seed
payload, not only the aggregate byte/count pair. Buffers require exactly one
`D9CBufferDesc::size` payload; standalone surfaces require one supported
CPU-seeded 2D payload; texture `Unavailable` and V2 volume `CompleteSeed`
are rejected with typed diagnostics because their closure is not proven.
Texture-derived aliases and swapchain outputs remain zero/zero dispositions.

GT2 r14
(`experiments/output/app-d3d9-3dmark05-gt2-frame-tape-exact-closure-r14-20260814`)
ran the staged canonical-V2 build under Sikarugir for 90 seconds without an
application failure, but published no Render Tape bundle. The former V2 cube
`ObjectDefine` zero/zero mismatch was no longer the first rejection. Capture
instead failed before arm with `incomplete_subresource_seed` for texture kind
0, generation 14, object id 4294967525, subresource 0. The registry retained 1021
objects at the first failed arm and later grew to 1073 while the same typed
failure remained latched. This is evidence that texture/surface descriptor
migration reached the next closure boundary; it is not evidence of a complete
GT2 capture. The earlier attribution of this identity to the documented
128x32 A8R8G8B8 user-memory texture was not proven: later diagnostics show the
successful user-memory snapshot on object 7526, while object 7525 remains
unclassified. GT2 r15 and r16 reproduced the same rejection without producing
a bundle. The UpdateTexture seam below is therefore a bounded code-audit
hypothesis for a possible future source-to-destination closure, not an
explanation of r14-r16 or GT2 capture evidence.

This closes the representation migration only. Actual GT2 bundle evidence,
provider replay of the captured indexed workload, and output-oracle
conservation remains open.

### UpdateTexture exact closure (capture-only)

The PE recorder now has a bounded value-only seam for a successful full-resource
`UpdateTexture`: it requires canonical V2 Texture2D or cube descriptors with
matching subresource extents and complete source bytes, then carries those
exact bytes into the destination registry shadow after the command append is
accepted. An active interval fails closed when the destination was not already
admitted, because defining it with post-copy bytes before the command would
move an unproven initial state across the command boundary. The successful
copy still updates the registry for a later retry. Autogen-mipmap, P8/A8P8,
volume, self-copy, unsupported-format, incomplete-source, and descriptor or
generation mismatch cases remain fail-closed; normal command bytes and the
capture-off path are unchanged. Native truth-table coverage proves empty and
pre-observed destinations, incomplete sources, extent mismatch, autogen, and
volume rejection. No GT2 bundle or provider-replay promotion is claimed yet.

### GT2 r24/r25 missing-seed provenance (capture-only)

The GT2 r24 retry confirmed that Render Tape capture was active and reached the
generation-qualified missing-seed materialization path. The r25 retry then
measured the bounded typed diagnostic: texture kind, generation 14, object id
4294967525, Texture2D, one mip and one subresource, 1024x768 format 22,
`D3DUSAGE_RENDERTARGET`, `D3DPOOL_DEFAULT`, no multisampling, and an exact
3,145,728-byte tight extent. The original consumer provenance is handle 0 of
record 0, type 28 (`APPLY_STATE`). The r26 locator run at
`experiments/output/app-d3d9-3dmark05-gt2-frame-tape-origin-locator-r26-20260815`
resolved the original reference as a generation-28 `SURFACE` in render-target
section 7, slot 0, aliased to the generation-14 `TEXTURE` storage above. Its
typed roles are `RenderTargetBinding` and `RenderTargetCandidate`; they prove
the binding/storage shape but deliberately do not claim an actual GPU write.
The next evidence layer must classify the capture interval's first access to
that generation-qualified storage and prove a full render-pass overwrite before
any read before a `ProducedByCapturedPass` seed disposition can replace the
missing bootstrap bytes. If that proof fails, a generation-qualified
pre-commit GPU snapshot remains the general fallback. Rejection semantics,
descriptor/generation validation,
event/chunk/bridge ABI, and capture-off cost are unchanged. No complete GT2
bundle or provider-replay promotion is claimed.

### GT2 r27 first-access locator (capture-only, observation-only)

The r27 increment arms one exact generation-qualified target at the r26
missing-seed diagnostic, then continues to inspect later validated command
chunks after capture has failed closed. Its bounded ledger carries RT/depth
attachment bindings across chunk boundaries and emits one terminal typed result:
unrestricted or partial clears, draw write with unknown coverage, shader-read
candidate, copy/readback source or conservative partial destination, present
read, and `Unknown` for malformed or same-draw read/write ambiguity. Command
chunks do not expose an ordered-control observation seam, so ordered boundaries
are intentionally outside this increment. An
`APPLY_STATE` binding is non-terminal; generation mismatches are ignored, and
the result is emitted exactly once. This is observation of command/binding
evidence only: it does not claim an actual GPU read, full draw overwrite, or
`ProducedByCapturedPass`.

The bounded GT2 r27 run at
`experiments/output/app-d3d9-3dmark05-gt2-frame-tape-first-access-r27-20260815`
observed the r26 alias-qualified RT as `full_clear_write`: the missing seed was
the generation-14 Texture2D storage behind the generation-28 RT surface, and
record 1 was a production-shaped unrestricted target `CLEAR` immediately after
the binding. This rules out a pre-commit GPU snapshot for this exact blocker.
The follow-on admission is now bounded to one generation-qualified
`Texture2D` with exactly one mip/subresource and one exact texture-derived
surface alias. Capture validates the current command chunk before defining the
produced texture, retains that same chunk after `ObjectDefine`, emits no seed
mutation, and the tape validator resolves the one obligation exactly once at
the first alias/full-clear chunk. Partial, draw-coverage-unknown, read-first,
malformed, identity-mismatch, multiple-target, later-unresolved, cube, and
multi-mip cases remain fail-closed. Native provider planning can construct the
texture without an initial mutation and construct the alias after its parent;
no wild visual or promotion evidence is claimed here.

### GT2 r28 produced-pass admission retry (capture-only, failed gate)

The bounded 32-second GT2 r28 run at
`experiments/output/app-d3d9-3dmark05-gt2-frame-tape-produced-pass-r28-20260815`
used the production PE/unix provider builds with frame-tape capture enabled.
The first capture interval reached the new admission seam but failed closed
once with `command_chunk_produced_pass_preflight`. No produced identity or
specific preflight rejection reason is currently emitted, so this run does not
prove that the r27 alias/full-clear pair itself was the rejected handle. After
that failure, 183 later arm attempts failed earlier with
`bootstrap_referenced_incomplete_seed`; each was reported as
`first_abort=arm_validation`. The output root
`experiments/render-tapes/gt2-produced-pass-r28-20260815` remained empty.

Therefore the native `ProducedByCapturedPass` value contract and provider path
remain implemented, but the GT2 production-capture gate is not satisfied. The
next bounded increment must first add typed, generation-qualified attribution
for each preflight rejection. If the rejected identity is already referenced
by `BootstrapState`, the start-state producer must carry an independently
validated produced obligation across the bootstrap-to-command boundary; it
must not infer completeness from the later clear or silently omit the live
reference. No complete GT2 bundle, replay result, visual oracle, or promotion
claim follows from r28.

### GT2 r43 production-capture milestone

The 2026-08-15 Sikarugir r43 run published the immutable `frame-tape` bundle
`experiments/render-tapes/gt2-attribution-r43-manual-live-20260815/`
`frame-138345988793600-1`. Its 1,453,416-byte `events.bin` has SHA-256
`d0f97424e3db3931dc4b52bc7cdcf6395e5ab543e604a926c94ebe08c1590e41`.
Repository `validate` reports `valid=true`, 1,199 events, and one Present.
Repository `inspect` reports 80 command chunks, 293 object definitions, two
destroys, 822 mutations carrying 139,495,661 bytes, 2,847 records, and 4,497
handles. The bundle contains 857 blobs carrying 132,900,457 bytes and exactly
one `PresentComplete`.

This closes the bounded GT2 production-capture milestone only. The bundle
manifest records `production_capture=true`,
`production_provider_replay=false`, and `output_oracle=false`. Production
provider preflight validates the structure and blob digests, then returns
`unsupported-grammar` at event 1 before consuming any definition or blob. That
event is the exact generation-qualified texture identity `(kind=Texture,
generation=1, objectId=4294967343)`: a schema-2 one-mip, one-subresource
`Texture2D` `CompleteSeed`, format 81 (`R32F`), resource type 3, usage 0,
`D3DPOOL_SYSTEMMEM`, single-sample, 1024×128×1, with one expected 262,144-byte
content item. The bounded provider deliberately rejects this storage contract;
normalizing its pool would not preserve D3D9 SYSTEMMEM seed/update semantics.
Supporting that exact grammar, then the later indexed GT2 grammar, replay
digest identity, and create/release conservation is the next phase rather than
part of this capture milestone.

### GT2 r57 production-provider replay milestone

The 2026-08-15 Sikarugir capture published
`experiments/render-tapes/gt2-output-oracle-r57-20260815/`
`frame-156258260414600-1`. Structural validation and inspection pass for 912
events, 32 command chunks, 269 definitions, 607 mutations, 687 blobs, 1,877
records, 1,231 indexed draws, 25 non-indexed draws, and one Present. The bundle
atomically carries the exact 3,145,728-byte capture output as `output.rgba`;
its manifest SHA-256 and `PresentComplete` SHA-256 are both
`6c4705e6a7fd302038a4deb6aab505f93d80be5e6f5de452d051806632b83d01`.

One warm-up and two measured provider runs in separate processes all produced
the same output SHA-256
`c82fc63f8c75dcf1453cfcf1251c96560b89389075f237c9d4b1442fb79fd052`,
referenced all 687 blobs, and conserved 269 created/released objects and the
single Present/completion ordinal. Exact digest equality remains false. The
authenticated byte comparison differs at 34 of 786,432 pixels, with maximum
RGB channel delta 2, total RGB delta 62, and no alpha differences; this is
inside the R-HARN-REPLAY-7.19 maximum of 64 pixels and total RGB delta 128.
The official runner reports `complete`, `oracle_mode=pixel-envelope`,
`deterministic=true`, and `production_provider_replay=true`. It rejects the
same envelope after only one process, and the C++ predicate rejects missing or
wrong-digest sidecars, structural/blob/object conservation failures, a 65th
changed pixel, RGB delta 3, or any alpha difference.

This closes general indexed GT2 frame-tape capture-to-provider replay. It does
not turn the separate projection artifact into an executable mini-replay, does
not support `PresentEx`, and does not weaken strict SHA equality for native
fixtures, sequence tapes, or promotion claims that require byte identity.

## Render Tape bounded wild evidence

The canonical `perf-d3d9-present-loop` experiment was run with
`PRESENT_LOOP_ITERATIONS=2`, the Sikarugir runtime, an existing manifest-owned
prefix, and a fresh PE-visible output root (`Z:\\...`). The app and runner
both exited successfully.

For this run, the runtime was
`/Users/dididi/workspaces/dxmt9/experiments/wine/sikarugir-cx-24.0.7` and the
manifest was `/Users/dididi/workspaces/dxmt9/experiments/wine/manifest.toml`.
The captured bundle was
`experiments/output/render-tape-oracle-final.0ZFP3y/frame-95919862787500-1`;
its
`events.bin` is 2992 bytes with SHA-256
`ed6bb63659a72c60066c0653d4934669dd7f7081e7389f6a110f13a94eb5c7be`.
It contains four events, one object definition, one bootstrap chunk, one
command chunk with `Clear` + `Present`, and zero blobs or mutations. The v2
CLI `validate` and `inspect` commands passed. The `provider-replay` command
returned status `complete` with exit code 0; the result was
`production_capture=true`, `production_provider_replay=true`, and
`output_oracle=true`.

The provider requirements were 256×256, format 21. Readback was 262144 bytes
with SHA-256
`49843e277c6ce8246d199c69c77aba0e7791c50522ab16c6a926f1528bd7474c`, and
object conservation was 1 created / 1 released. Capture and replay both
produced that digest, so
`expected_digest_captured=true` and `expected_digest_matched=true`. Its uniform
clear makes
`output_non_degenerate=false`; the bundle remains structurally valid and useful
for exact-equivalence evidence, but this output is not promotion evidence for a
production parallel join.

The staged binary SHA-256 values were x64 `d3d9.dll`
`f3b62e8c9d2886a99134f6c00d9027fa9230a2d949cba8ce04b9924e432abadc`, x64
`winemetal.dll`
`9ed9c128a5e21e60b6bc0c887fe860da8eef463940f70791564324ea6c069912`, x86
`d3d9.dll`
`d5160032acb1926988153835a31b8832da0316aa733ad80df20e13b3fd08552c`, x86
`winemetal.dll`
`98cdeacb700bfd159274d526aae0d3b9fc2ccdef84258eacae4be27f3c423d92`, and
Unix `winemetal.so`
`062191018118ba5b617614cb08ae0daa1bc050dd22b3111727bbadb4e5f549fb`.

The production implicit backbuffer is represented as a `PresentOutput` surface
with `initial-content-not-required`; proof for intervals that load prior
backbuffer contents remains open. Direct `PresentEx`/direct-control calls do
not currently emit the canonical `PRESENT` event and therefore fail closed at
`presentChunkSeen`; PresentEx event support is a separate documented gap.

---
