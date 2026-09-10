---
type: "Spec Gap"
title: "Experiments Gap"
description: "Implementation and evidence gaps for wild integration experiments."
tags: [specs, gap, experiments]
---

# Experiments Gap

## Fixed-input replay experiment

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
