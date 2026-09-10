---
domain: mini-replay-bisection
workload: Render Tape native replay and 3DMark05 GT2 capture
title: "Fixed-input replay policy experiment: bounded CPU and Metal evidence"
type: experiment
status: partial
updated: 2026-09-10
source: experiments/output/fixed-input-harness-20260909/small-compatibility-r3/result.json; experiments/output/fixed-input-harness-20260909/small-direct-r3/result.json; experiments/output/fixed-input-harness-20260909/parallel-direct-r2/result.json; experiments/output/fixed-input-harness-20260909/cpu-{compatibility,production-slot}-{after-command,after-destroy}-r4/result.json; experiments/output/fixed-input-harness-20260909/cpu-{fault-submit,fault-release,budget-zero}-r4/result.json; experiments/output/fixed-input-harness-20260909/tla-final.log; experiments/output/app-d3d9-3dmark05-gt2-frame-tape-v2-20260909-r2/dxmt9.log; experiments/output/app-d3d9-3dmark05-gt2-frame-tape-v2-20260909-r3/dxmt9.log; experiments/output/app-d3d9-3dmark05-gt2-frame-tape-v2-20260909-r4-capture64/result.json; experiments/render-tapes/gt2-v2-20260909-r4-capture64/frame-1292677716873100-1/manifest.json; experiments/output/fixed-input-harness-20260909/gt2-capture64-{compatibility,direct}-r5/result.json; experiments/output/fixed-input-harness-20260909/gt2-capture64-x64-compatibility-r6/result.json; experiments/output/fixed-input-harness-20260909/gt2-capture64-arena-r9/result.json; experiments/output/app-d3d9-3dmark05-gt2-frame-tape-v2-20260909-r5-cold-capture64/result.json; experiments/output/fixed-input-harness-20260909/gt2-cold-capture64-compatibility-r10/result.json; experiments/output/fixed-input-harness-20260909/cpu-paired-{capacity,dynamic}-{after-command,after-destroy}-r30/result.json; experiments/output/fixed-input-harness-20260909/cpu-general-gt2-paired-r36/result.json; experiments/output/fixed-input-harness-20260909/cpu-general-gt2-{compatibility,production-slot}-r35/result.json; experiments/output/fixed-input-harness-20260909/cpu-general-gt2-before-next-work-r44/result.json; experiments/output/fixed-input-harness-20260909/cpu-general-gt2-source-projection-r65/result.json; experiments/output/fixed-input-harness-20260909/cpu-general-gt2-source-mutation-r66/result.json; experiments/output/fixed-input-harness-20260909/source-oracle-compat-r64/result.json; experiments/output/fixed-input-harness-20260909/cpu-general-gt2-source-projection-r75/result.json; experiments/output/fixed-input-harness-20260909/cpu-general-gt2-source-mutation-r75/result.json; experiments/output/fixed-input-harness-20260909/cpu-general-gt2-source-projection-r81/result.json; experiments/output/fixed-input-harness-20260909/cpu-general-gt2-source-mutation-r81/result.json; build/meson-logs/cpu-source-owner-r80.txt; build/meson-logs/cpu-two-source-model-r79.txt; experiments/output/fixed-input-harness-20260909/cpu-general-gt2-window-r88/result.json; experiments/output/fixed-input-harness-20260909/cpu-general-gt2-window-r88/immediate-control-check.json; experiments/output/fixed-input-harness-20260909/cpu-general-gt2-immediate-r88/result.json; build/meson-logs/cpu-window-retained-eligibility-r88.txt; build/meson-logs/cpu-window-model-r85.txt; experiments/output/fixed-input-harness-20260909/cpu-general-gt2-source-projection-r88/result.json; experiments/output/fixed-input-harness-20260909/cpu-general-gt2-source-mutation-r88/result.json; build/meson-logs/cpu-stream-integrated-r94.txt; build/meson-logs/cpu-stream-cli-r96.txt; build/meson-logs/cpu-stream-tla-r96.txt; experiments/output/fixed-input-harness-20260909/cpu-general-gt2-window-r95/result.json; experiments/output/fixed-input-harness-20260909/cpu-general-mipmap-r100/mipmap-{after-command,before-next-work}/result.json; build/meson-logs/cpu-mipmap-r98.txt; build/meson-logs/cpu-mipmap-regression-r98.txt; build/meson-logs/cpu-mipmap-cli-r100.txt; build/meson-logs/cpu-mipmap-tla-r98.txt; experiments/output/fixed-input-harness-20260909/cpu-general-gt2-{window,immediate}-r102/result.json; experiments/output/fixed-input-harness-20260909/cpu-general-gt2-window-r102/immediate-control-check.json; experiments/output/fixed-input-harness-20260909/cpu-general-gt2-source-projection-r102/result.json; experiments/output/fixed-input-harness-20260909/cpu-general-gt2-source-mutation-{r102,r103}/artifact-failure.json; build/meson-logs/cpu-explicit-mip-guard-r101.txt; build/meson-logs/cpu-owned-staging-r101.txt; build/meson-logs/cpu-compact-receipt-r103.txt; build/meson-logs/cpu-retain-exact-event-r105.txt; experiments/output/fixed-input-harness-20260909/cpu-general-gt2-source-projection-r105/result.json; experiments/output/fixed-input-harness-20260909/cpu-general-gt2-source-mutation-r106/result.json; experiments/output/fixed-input-harness-20260909/.cpu-general-gt2-source-mutation-compat-r105.rejected-ooj_btbr/rejected.json; experiments/output/fixed-input-harness-20260909/.cpu-general-gt2-source-mutation-compat-r105.rejected-ooj_btbr/provider.stdout; build/meson-logs/cpu-receipt-acceptance-r107.txt; build/meson-logs/cpu-exact-event-tla-r110.txt; build/meson-logs/cpu-explicit-mip-integrated-{r111,r112,r114}.txt; build/meson-logs/cpu-explicit-mip-routing-r113.txt; build/meson-logs/cpu-explicit-mip-regression-r115.txt; build/meson-logs/cpu-exact-event-progress-r116.txt; experiments/output/fixed-input-harness-20260909/cpu-general-gt2-source-projection-r117/result.json; experiments/output/fixed-input-harness-20260909/cpu-general-gt2-source-mutation-r118/result.json; build/meson-logs/cpu-harness-lifecycle-r119.txt; build/meson-logs/cpu-exact-event-thread-{r120,r121,r122,r123}.txt; build/meson-logs/cpu-exact-event-repeat-r124.txt; experiments/output/fixed-input-harness-20260909/cpu-general-gt2-source-projection-r125/result.json; experiments/output/fixed-input-harness-20260909/cpu-general-gt2-source-mutation-r126/result.json; build/meson-logs/cpu-singleton-transfer-r127.txt; build/meson-logs/cpu-singleton-transfer-tla-r128.txt; experiments/output/fixed-input-harness-20260909/cpu-general-gt2-source-projection-r129/result.json; experiments/output/fixed-input-harness-20260909/cpu-general-gt2-source-mutation-r130/result.json; build/meson-logs/cpu-harness-final-r131.txt; build-x86_64-builtin/meson-logs/cpu-singleton-transfer-x86-r132.txt; build/meson-logs/cpu-phase-ledger-r136.txt; experiments/output/fixed-input-harness-20260909/cpu-capacity-{compatibility,production-slot}-external-r137/result.json; experiments/output/fixed-input-harness-20260909/cpu-general-gt2-source-projection-r138/result.json; experiments/output/fixed-input-harness-20260909/cpu-capacity-production-slot-external-failure-r139/result.json; experiments/output/fixed-input-harness-20260909/cpu-capacity-production-slot-external-failure-replay-r140/result.json; experiments/output/fixed-input-harness-20260909/cpu-general-gt2-source-mutation-r141/result.json; build/meson-logs/cpu-harness-final-r142.txt; build-x86_64-builtin/meson-logs/cpu-phase-x86-r143.txt; experiments/output/fixed-input-harness-20260909/cpu-general-gt2-source-mutation-r144/result.json
related: specs/experiments/harness/replay/fixed-input-experiment.md; specs/experiments/gap.md
---

# Fixed-input replay policy experiment

## Question and boundary

Can a captured PE canonical stream replace repeated whole-game launches while
comparing production replay/Slot/encode policies? Yes for the bounded captured
inputs below, but this is not an FPS model or general resource-lifetime proof.
The PE/API timing and original source-arrival spacing are deliberately absent.
Captured full snapshots and chunk boundaries remain part of the fixed workload.

The working tree is based on `fa9bab58`; artifacts authenticate the actual
executable and loaded renderer images. Do not label it a clean-HEAD measurement.
No renderer default was changed.

## Observed execution

The small real capture
`experiments/output/pe-inline-consumers-20260908/analysis/capture-x64-off-r2/bundles/frame-1165361678863600-1`
was replayed with warm device caches, one warm-up and five measured iterations.
Both explicit compatibility and ProductionSlot passed strict source/output
oracles and selected-path non-vacuity. Each iteration executed 18 selected-path
draws; ProductionSlot used one committed direct lease.

ProductionSlot inclusive replay service was **3.622, 2.583, 2.527, 2.383 and
2.039 ms**. These scopes include resource setup/mutation, chunk replay,
readback/drain and cleanup; reset and preparation are separately reported.
They are not isolated thread CPU cost, GPU time or game frame time, and this
small sample does not establish policy superiority.

The generated 128-draw parallel fixture supplied a useful negative control:
ProductionSlot requested, **zero Direct draws / 128 compatibility draws**,
with `BeginLegacyPreEffectFailure`. Pixels matched, but `--require-path`
rejected the candidate. A selected enum plus good pixels is not execution
evidence.

## Controlled CPU evidence

The CPU adapter runs real importer, replay, Slot, Queue completion and Pool
retirement on a no-GPU device. The small canonical fixture is one full Plain
buffer seed, draw, unbind, destroy and implicit Present. Four combinations of
Direct/compatibility and after-command/after-destroy acknowledgment compare
flattened effective draw state, uniforms and qualified resource identity.
The delayed case observes a destroy-pending live Pool record before completion
and its absence after completed-waterline reclaim.

The four `cpu-*-r4` artifacts pass **16 repetitions each**, with the same
semantic digest across both routes and both acknowledgment schedules. For
ProductionSlot/after-destroy, fixture-create through replay/cleanup wall time
is 0.142–0.959 ms (upper median 0.146 ms). This excludes process startup,
Tape preparation, image hashing and fixture destruction. It is a single-draw
fixture, not an estimate for a GT2 frame.

The real `before-submit`, `before-release` and zero-pending-budget negative
runs all terminate with named `unresolved` failures and preserved artifacts;
no partial result is accepted. Full `dxmt9-verify-tla` passes (156.95 seconds),
including the new model and its two required counterexamples.

The subsequent integrated check also passes eight focused native tests and
the full TLA suite (171.29 seconds):
[native log](../../../experiments/output/fixed-input-harness-20260909/native-final-r3.log),
[TLA log](../../../experiments/output/fixed-input-harness-20260909/tla-final-r3.log).
The new Pool pin keeps the completion frontier monotonic and demonstrates
that capture residency still prevents backing reuse after completion. Its
synthetic handle is never passed to Objective-C ownership operations; this
proves ring selection and captured identity, not physical Metal storage.
The Managed mutation mode-on pin uses the actual queue-allocation fault hook
before effects and counts one real apply on retry. These remain native
composition tests, not an expanded accepted CPU Tape grammar.

The final small-capture rerun also passes both policies, one warm-up plus five
repeats each, with authenticated per-iteration RGBA files:
[compatibility](../../../experiments/output/fixed-input-harness-20260909/small-compatibility-r11/result.json),
[ProductionSlot](../../../experiments/output/fixed-input-harness-20260909/small-direct-r11/result.json).

The original runs force publication per CommandChunk event; Present pacing is disabled.
The original `cpu-no-gpu` fixture proves CPU shadow bytes, not physical Metal
upload/rename. It does not qualify general GT2 CPU replay, populated-Slot
capacity coverage or concurrent queue progress. The later grouped case below
adds one exact populated-Slot capacity shape.

The added `cpu-metal-resources` fixture uses real Shared Metal buffers without
submitting GPU work. Its sequence is Plain A, draw A, DISCARD B, draw B, unbind,
destroy and Present. The four `cpu-dynamic-*-r5` artifacts each pass 16 repeats;
`cpu-dynamic-external-r5` also passes 16 under an authenticated external plan
releasing draw events 5 and 7 after destroy event 9, and Present event 10 after
closure event 11. All have semantic digest `1083658293988629013` and a real
backing-byte oracle. Retaining A is required only while its source remains
outstanding; the immediate schedule permits legal physical reuse.

External-plan iteration wall time is **0.208–0.965 ms**, upper median
**0.216 ms**, over fixture construction/replay/cleanup and excluding process
startup/preparation/destruction. These are two-draw lifecycle experiments, not
GT2 frame service estimates. The external binder reports actual source and
storage generations, not fabricated queue sequence IDs. Its bounded model
visits 15 states and detects three injected violations; the integrated seven
native tests and full TLA suite pass:
[native log](../../../experiments/output/fixed-input-harness-20260909/native-integrated-r3.log),
[TLA log](../../../experiments/output/fixed-input-harness-20260909/tla-event-binding-r2.log).
The model's pending-owner abstraction does not prove physical Pool or GPU
semantics; those byte/reuse checks are separate native composition evidence.

The Managed-buffer fixture extends the fault evidence through an actual Tape
event and the production replay queue allocator hook. All four
`cpu-managed-fault-*-r7` route/completion combinations pass 16 repetitions:
exact `D3DERR_INVALIDCALL`, consumed allocation hook, unchanged backend
ownership, still-open lock, then public Unlock retry with exactly one backend
mutation apply. These use actual Metal-backed resources but submit no GPU work.
The no-fault fixture is compared in the CLI test; both outcomes have identical
draw semantics. The wrapper keeps `accepted=false` and records the separate
`fault_oracle_passed=true` diagnostic verdict, never a performance result.
See [native fault log](../../../experiments/output/fixed-input-harness-20260909/native-managed-fault-r3.log).
An initial null-backed Managed attempt (`cpu-managed-baseline-r6`) failed
before draw capture because production versioned snapshots require a physical
handle; that backend is now rejected before runtime effects.

### Grouped publication and populated capacity

The authenticated `cpu-capacity-bundle-r9` fixture contains seven then two
plain-buffer draws in events 5–6, unbind 7, destroy 8, Present 9 and closure 10.
Events SHA-256 is
`f6855090014de780cb956f8d687790fce350e2d7fca303c14cb18a4fe8967d17`.
`--publication draw-island` holds the actual writing Slot across events 5–6.
Optional direct and aggregate headroom are fixed to zero, and the production
no-rotation admission policy is unchanged.

All six `cpu-capacity-{compatibility,production-slot}-{after-command,after-destroy,external}-r10`
runs pass 16 repetitions. Every iteration has semantic digest
`7614069032485648982`, one nine-draw source attributed to events 5–6, and a
separate Present source at event 9. Reference execution is nine compatibility
draws. Candidate execution is **seven Direct + two compatibility draws**, with
**one actual capacity rejection, one populated fallback and zero rotations**.
No manual compatibility prefix or synthetic capacity-rejection counter is used.
Immediate releases are events 6/9; both delayed policies release at 8/10.

ProductionSlot/external iteration wall is **0.326–0.957 ms**, upper median
**0.334 ms**. This scope includes fixture creation/replay/cleanup and excludes
process startup, authentication/preparation and fixture destruction. It is a
nine-draw CPU experiment, not a GT2 frame/FPS estimate or physical GPU oracle.
The first `...compatibility-after-command-r9` attempt remains preserved as a
grammar failure; only r10 qualifies the current grouped fixture.

The shared range predicate checks all 3072 bounded tuples and six A/B/publish
orders. The focused TLA composition visits 14 states and detects early publish,
missing draw and wrong-range counterexamples. Full TLA passes in
[tla-grouped-r1.log](../../../experiments/output/fixed-input-harness-20260909/tla-grouped-r1.log).
Ten integrated native tests pass in
[native-grouped-integrated-r6.log](../../../experiments/output/fixed-input-harness-20260909/native-grouped-integrated-r6.log).
The CLI also executes early planned-submit and vacuous-group failures instead
of relying only on edited result JSON. Preserved `cpu-capacity-early-submit-r10`
fails with `completion-plan-event-missing-submit` and zero submissions;
`cpu-dynamic-pending-two-r10` fails before its third submit with two unresolved
sources and zero completed. The external pending count now remains a count,
not a boolean; no synthetic completion is used to make either result pass.
Independent review found weaker preset receipt checks than external plans.
The validator now supplies input event types/dynamic-buffer metadata and the
wrapper checks storage generations/ranges, exact preset release points and
the null plan digest in every preset run. All six combinations repeat 16 times
again in `cpu-capacity-*-r12`, still with the same digest and seven/two capacity
outcome. ProductionSlot/external is **0.325–0.952 ms**, upper median **0.334 ms**;
the same timing exclusions apply. r10 remains historical evidence under the
older receipt verifier, not rewritten as an r12 receipt.
All ten integrated native tests pass again in
[native-final-integrated-r12.log](../../../experiments/output/fixed-input-harness-20260909/native-final-integrated-r12.log).
General capacity dimensions, rotation,
multiple resource owners and concurrent worker interleavings remain separate.

## GT2 capture gate

The new SegmentSerial attempt `...-r2` reached capture arm and Present output
reservation, but **first** fails `legacy-or-inline` with planner reason 9
(`ReplayReason::Oversize`, token 1 events 260/284/306 and later events).
`failCaptureIdentity` poisons and clears the ledger; subsequent
`snapshot-or-ledger-append` failures are secondary rejections of that failed
ledger, not evidence of malformed metadata at those later events.
No authenticated bundle was published. EventSerial `...-r3` fails the same
Oversize path. Both finish with `first_abort reason=identity_query`.
Neither is a publisher-path or demonstrated blob-capacity failure. The app
runner's ordinary `pass` does not establish capture success.

The source-group capacity explains the default-cadence failure: 256 draws
already require 2,105,344 bytes for the VS/PS derived payloads alone, exceeding
eight 64-page sources (2,097,152 bytes) before other regions. No admission bound
was raised. An explicit 64-record/262144-byte PE capture cadence in
`...-r4-capture64` publishes the complete next frame after one aborted token:

- Bundle: `experiments/render-tapes/gt2-v2-20260909-r4-capture64/frame-1292677716873100-1`.
- Events SHA-256: `4f33d50a4d001cc2cefc8ad8bdd0535b3b3371248c59d782e1afbaa3c0fba38a`.
- 965 events, 39 command chunks, 1395 indexed plus 25 primitive draws,
  135 authenticated source segments, 39 event settlements and one Present.
- Both original source and output RGBA oracles and 725 immutable blobs survive.

The recipe uses `frame-tape`, skip-Presents 200, segment identity and identity
partition mode, with the perf profile and info logging. It is a full frame,
not a selected draw subset, but its chunk boundaries differ from the default
256-record workload. Capture retry success is not universal capture closure.
The run's staged-build hashes, rather than the bundle's generic `runtime`
revision field, identify the actual dirty-tree binaries.

The new fixed input is not yet oracle-qualified. ARM64 compatibility and
ProductionSlot experiments (`gt2-capture64-*-r5`) both stop at warm-up zero
with the same source/output SHA-256
`91e03763888abb05871733a7bef432f14264838ae846bd6542960a83606711d0`.
They differ from capture at **2378 pixels**, maximum RGB delta **16**, total
RGB delta **3704**, zero alpha differences; the existing envelope permits
only 96 differing pixels. ProductionSlot actually executes 691 Direct draws
and 729 compatibility draws; compatibility executes all 1420 without a Direct
lease. Both release all 277 created resources and report zero GPU errors.
The x86_64 compatibility control (`...-x64-compatibility-r6`) reproduces exactly
the same digest and difference metrics. Thus this observed mismatch is shared
by the two policies and both provider architectures, not evidence against
only Direct or ARM64. No measured-repeat performance claim follows from a
failed warm-up.

The explicit CPU-ready Arena diagnostic `...-arena-r9` also reproduces that
same image. Its ordinary Direct-lease counter is zero; this is not ordinary
Direct qualification or a complete Arena path-attribution receipt. Both actual
RGBA files are retained and their hashes agree with the provider receipt.
The earlier `...-arena-r8` never executed: a newly edited wrapper supplied a
new option to an old native CLI and received usage/exit 2. Keep that artifact
as a version-mismatch failure, not an Arena behavioral result.

One concrete configuration difference was tested next: the first capture
loaded the existing shader archive with full prewarm, while the hermetic
provider disables both. The `...-r5-cold-capture64` run explicitly disables
archive/prewarm during capture and publishes a new complete 1198-event v2
frame after multiple rejected capture tokens. Compatibility replay
(`...-compatibility-r10`) still fails: **1839 pixels**, maximum RGB delta **18**,
total RGB delta **3962**, zero alpha differences. Its failed warm-up replay
service is **646.231 ms**, including resource setup, replay/encode, wait,
readback and cleanup; output artifacts are written afterward. This is a useful
sub-second fixed-input counterexample, not a measured-repeat performance result.
The two captures contain different frames, so their difference counts cannot
be used as a cache A/B delta; the new result establishes only that disabling
the archive is insufficient to close capture/replay equality.

Do not widen identity validation, discard the sidecar, replace the captured
oracle with replay output or loosen pixel tolerances to obtain a passing gate.
The surviving historical GT2 bundles in this checkout carry retired
`identity.v1`; the documented `identity.v2` r17 path is absent. They cannot be
silently repacked or stripped of identity to claim current v2 qualification.

### Initial resource diagnostic

`gt2-initial-seed-audit-r14` initially read one X8 render-target texture as
opaque black despite an image-valued seed. This was a diagnostic ordering
false negative, not a demonstrated product upload failure: `DeviceImpl::flush`
does not flush the deferred `Initializer` upload queue. r15 then correctly
reported an incomplete audit because a copied timeout assumed milliseconds
although the bridge takes nanoseconds. Both failed artifacts remain retained.

The corrected audit flushes Initializer and checks completion with the proper
timeout unit. `gt2-initial-seed-audit-r16` matches **5/5 supported** initial
readbacks, reports **87 unsupported**, and still produces the same final
2378-pixel mismatch. It is deliberately perturbative and always unqualified
for performance. A private render-target native seed/readback pin passes in
`native-seed-audit-r4.log`.

The questioned texture `0:13:4294967525` is also fully overwritten before its
first sample: alias `1:293:4294967651` becomes RT0 at event 128 record 0 and is
fully target/depth cleared at record 1; parent sampling first appears at event
941 record 0. This seed is therefore not a causal explanation of the final
image residual. Of the 2378 different pixels, 2139 have maximum channel delta
1, 179 delta 2, and 60 greater than 2. That distribution is diagnostic evidence,
not permission to enlarge the source/output envelope.

Two tempting explanations remain unproved or contradicted. Exact attachment
preservation is not provider-only: `CommandQueue` also enables it when
`DXMT9_RENDER_TAPE_CAPTURE` is set, as it was for capture. SegmentSerial edges
are required non-pass boundaries by R-BACK-32.1; differing identity partitions
could only explain pixels through a violation of that contract, not as an
expected fidelity difference. The narrower next discriminator is the
provider's extra global drain before a freshly defined independent resource's
initial seed, tested without changing captured bytes or defaults below.

### Fresh-resource drain discriminator

`gt2-fresh-seed-drain-audit-r17` exercises 530 omitted and 23 retained
post-command mutation drains. Initial mutations before the first command do
not count as omissions. The result still fails: **2398 pixels**, maximum RGB
delta **16**, total delta **3756**, no alpha differences or GPU errors; actual
source/output SHA-256 is `02d194e7920b4d8c79fea53d3bc5c8b1ac584fba945555d25330461388f973ec`.
All 1420 draws execute in compatibility, with 277 objects created/released.
The bypass is non-vacuous but does not explain away the capture/replay residual.
The changed image establishes boundary sensitivity only, not a safe promotion.

The first r17 receipt incorrectly ties audit `complete` to reaching the end
of the replay function: the final pixel rejection returns earlier even though
all mutation events were visited. Preserve that receipt; the coverage fix
counts successfully inspected mutations independently of the pixel oracle.
It does not turn the failed render into a passing result. The admission proof
has an independent 64-decision chronological native checker plus targeted
alias, generation, partial/repeated seed and subresource negatives.

`gt2-fresh-seed-drain-audit-r18` verifies the coverage repair: requested,
attempted and complete are true, with **645/645** inspected mutations and the
same **530/23** bypass/required counts. The actual SHA and every pixel-delta
metric equal r17. Inclusive replay service is **138.102 ms**, excluding input
preparation, factory/device creation and artifact I/O; this remains a failed
single diagnostic iteration, not a performance qualification. Native
`native-integrated-r6.log` passes all nine focused tests; the preceding r5
failure was a CLI assertion incorrectly expecting a chronological audit in
the bounded pre-created fixture, now explicitly pinned as not attempted.

The independent-texture prestage discriminator also leaves the residual
unchanged. `gt2-texture-prestage-audit-r19` physically creates 40 hidden
textures and applies 500 complete seeds (41,111,057 bytes) before commands,
then activates the original definitions and consumes each seed event once.
All 277 provider-owned wrappers are released, with zero remaining staged
owners/seeds/bytes and zero GPU errors. Its upload-flush count is zero; that
alone did not distinguish an immediate upload from deferred work.

The corrected storage-route receipt in
`gt2-texture-prestage-audit-r20` reports **500 immediate uploads / zero deferred
uploads**, classified from the actual Pool backing under the queue mutex.
`Pool::stageTextureUpload` uses immediate `replaceRegion` for these Shared
backings; it does not postpone their upload until first draw. This is a
non-vacuous physical prestage experiment, but not evidence of deferred-blit
boundary removal. It again produces SHA `91e03763888abb05871733a7bef432f14264838ae846bd6542960a83606711d0`
and **2378 / 16 / 3704** differing-pixel / maximum-RGB / total-RGB metrics,
identical to conservative r16. Inclusive service is **138.787 ms**, excluding
preparation, factory/device setup and artifact I/O; the diagnostic remains
`accepted=false`, not a measured policy-speed result.

The prerequisite native indexed-texture fixture tests both DEFAULT and MANAGED
pools: ordinary and hidden-prestaged output bytes match exactly, while changing
the texture seed changes pixels. The deferred variant requires a real upload
event/wait; the immediate variant must not claim an initializer submission.
All five fixture wrappers are conserved, and the independent event-order
checker exhausts six one-mip and 24 two-mip permutations.

### Validation and capture-state completeness audit

`gt2-metal-validation-r21` enables both `MTL_DEBUG_LAYER=1` and
`MTL_SHADER_VALIDATION=1` on the original r4 bundle, compatibility/identity,
warm-device, zero warm-ups and one iteration. It completes 1420 compatibility
draws with zero reported GPU errors and no API/shader error in provider stderr,
but fails the original oracle: **2403 pixels / maximum RGB 16 / total RGB 3729**,
zero alpha differences. Actual source/output SHA is
`951cfb23fc86634e1659f5b68909d210714b0d6473b100432ae5d944948cc373`.
This is instrumentation-sensitive output, not a passing correctness gate or a
policy timing. Its 4.243-second inclusive service includes validation overhead.

The r21 wrapper predates the environment receipt repair: it records only DXMT
knobs, so `renderer_environment={}` and `diagnostic=false` do **not** mean an
uninstrumented run. The preserved stderr explicitly announces both validation
layers. New receipts include the allowlisted Metal settings and prohibit
policy acceptance for validation diagnostics; old evidence is not rewritten.

The small synthetic cold-device validation check also exposed a separate
unresolved failure: `validation-fixture-r22` terminates its provider with signal
11 before a JSON receipt, with both validation layers announced in stderr.
The wrapper preserves `provider_exit_code=-11`, `diagnostic=true`, both actual
validation settings and `accepted=false`. It must not be counted as a passed
Metal validation oracle. The CLI acceptance-gate test uses an explicitly named
provider double with an already-validated native receipt/readback set, so it
tests instrumentation rejection independently of this runtime failure.

At the r21 audit, two direct state paths were missing from the Tape's claimed
starting-state coverage. The stream-frequency implementation below supersedes
that code observation, but cannot repair historical input bytes:

- `d3d9_pe_device_child_surface.cpp::setTextureLod` changes resource-owned LOD;
  `core_draw.cpp` reads that LOD into the executed sampler key, but
  `RenderTapeTextureDescriptorV2` and the provider do not restore it.
- `d3d9_pe_device.cpp::SetStreamSourceFreq` retains independent `streamFreq_`
  and calls the backend directly. `d3d9_pe_producer.cpp` emits stream frequency
  zero and `device_c_chunk_replay.cpp` restores only buffer/offset/stride;
  fresh backend frequency defaults to one. StateBlock replay is another path
  that a complete frequency solution must include.

The original r4 `events.bin` has **47 texture definitions: 40 MANAGED and 7
DEFAULT; 36 MANAGED textures have multiple mips; none has AUTOGENMIPMAP usage**.
This removes explicit autogen generation of those defined textures as the
leading explanation, but their actual LOD is absent, not proven zero. Likewise
zero frequency fields do not attest the live PE frequency. Neither omission
is yet independently attributed as the cause of this small pixel residual.
All 18 captured perf rows report
`d3d9_draw_state_cache_miss_after_texture_lod=0`; that counter covers cache
invalidation after a change to an already-bound texture, not LOD set while
unbound. It is therefore not a substitute for an actual LOD-state receipt.

The bounded safety fix rejects a tracked effective managed-texture LOD change
with `unsupported_texture_lod_mutation`, including before arm, without changing
the application's return/state. It does not add LOD replay support or certify
older captures. Frequency/bootstrap and automatic mip-generation coverage
remain separate gaps. Before further policy-speed experiments, compare a new
capture's executed sampler/constant/raster values and physical pass boundaries
against replay; the authenticated logical-pass sidecar alone cannot establish
those values. Capture/replay intermediate attachment comparisons must observe
natural pass closures rather than introduce new pass cuts.

After the guard and receipt changes, native and x86_64 Unix full builds and
both 32/64-bit PE builds pass. Four focused capture/provider/CLI/CPU-experiment
tests pass in
`build/meson-logs/fixed-input-lod-validation-r23.txt`; `git diff --check` and the
performance-document artifact audit pass. This is build/native evidence only:
no new Wine capture qualifies the LOD guard, and no passing GT2 pixel result
or resolution of the validation-fixture signal 11 is claimed.
See [native log](../../../experiments/output/fixed-input-harness-20260909/native-prestage-r3.log).

This closes the bounded independent-texture prestage discriminator. Do not
widen its admission or the pixel envelope. The remaining shared mismatch needs
command/state-projection or publication-boundary localization; it is not yet
attributed to either. Resource import, Direct eligibility and product defaults
are unchanged by these diagnostics.

Final integrated verification passes all nine focused native tests in
`native-integrated-r7.log`, plus the full native, x86_64 Unix and both PE
cross-builds. The latest full TLA suite passes in `tla-event-binding-r3.log`
(155.98 seconds). These checks do not close the failed GT2 pixel gate.
The later grouped-publication section closes one populated-Slot fallback
shape, not arbitrary capacity dimensions or resource families.

## Paired CPU capacity experiment (r24)

The new `cpu-compare` entry point runs the same authenticated Tape through
compatibility and production-slot children. Each child must independently
pass its path, reset, conservation, image-identity and completion checks before
the parent compares logical evidence. Cold observations read each executed
final-Slot draw and record its cumulative semantic digest, enabling the parent
to identify the first differing draw even when a corrupted final digest is
unchanged. Physical source counts and publication events are not required to
match across routes.

Both `after-command` and `after-destroy` pass 16 repetitions per route for the
existing nine-draw capacity bundle. The reference uses nine compatibility
draws; the candidate uses seven Direct draws and two compatibility draws after
one populated-capacity rejection. All nine ordered draw observations agree,
and the final digest remains `7614069032485648982`. No new capture, GPU output
oracle, performance acceptance or arbitrary GT2 grammar is claimed.

Artifacts:

- [Immediate completion comparison](../../../experiments/output/fixed-input-harness-20260909/cpu-paired-capacity-after-command-r24/result.json)
- [Delayed completion comparison](../../../experiments/output/fixed-input-harness-20260909/cpu-paired-capacity-after-destroy-r24/result.json)

The current shared object factory/lookup/release refactor and paired CLI pass
the three focused native provider, CPU routing and CPU CLI tests. A failed
handle resolution publishes no partial pointer list; stale/missing identities
and insufficient scratch capacity leave the resolved count zero. Broader CPU
resource creation, mutation projection and actual GT2 input execution remain
separate integration gates. Existing GT2 pixel failures above remain failures.

## Final paired integration (r30)

Both capacity and dynamic-buffer inputs pass immediate and delayed completion,
16 iterations per route per case: 128 child iterations pass. Capacity still
selects seven Direct plus two compatibility draws with final digest
`7614069032485648982`. Dynamic input selects two Direct draws with digest
`1083658293988629013`; delayed completion requires and observes preservation
of captured A after DISCARD B, while immediate completion correctly does not
require distinct retained backing. These are CPU-visible Metal buffer bytes,
not GPU texture readbacks.

The final comparator excludes physical command/source counts from cross-route
equality while retaining per-child submitted/completed/reclaimed conservation.
It rejects extra fabricated Direct draws, corrupted per-draw observations,
malformed ordinals/digests, unqualified publication events, oversized forged
trace ranges and nonzero/timeout wrapper status. A final-digest match cannot
override an earlier per-draw mismatch.

Artifacts are `cpu-paired-{capacity,dynamic}-{after-command,after-destroy}-r30`
under the same fixed-input output root. Final native/provider/CLI tests pass
in `build/meson-logs/cpu-paired-final-r30.txt`; all four canonical build trees
compile. The full TLA suite passes in
`build/meson-logs/cpu-paired-tla-r26.txt` (160.65 seconds). Shared preflight
remains the existing provider parser, and the new resource-backed component
tests cover real Pool texture/alias/surface retirement and bounded logical
uploads through ordinary core lock/unlock without queue publication. These
component tests do not widen CPU Tape grammar or close the failed GT2 pixel
gate.

## Whole GT2 CPU execution (r35–r41)

The unchanged r4 capture now executes on the real CPU replay/Slot/Pool path.
`cpu-general-gt2-paired-r41` passes 16 fresh-device iterations per route, with
identical per-draw and final semantic digests (`16710489906774813854`).

| Quantity per iteration | Compatibility | Production Slot |
|---|---:|---:|
| Input events / definitions / mutations | 965 / 277 / 645 | same |
| Input chunks / records / draws | 39 / 2085 / 1420 | same |
| Direct / compatibility draws | 0 / 1420 | 1410 / 10 |
| Submitted / completed / reclaimed sources | 39 / 39 / 39 | same |
| Object acquisitions / releases | 277 / 277 | same |
| Actual buffer snapshot observations | 4235 | 4235 |

The native executed record count is 2086 because it additionally consumes the
single bootstrap record. Every iteration checks fresh-device cleanup after
dropping core/cache owners; the backend remains alive for actual Pool
inspection. Texture upload callbacks validate layout and exact mutation bytes.
Canonical depth seeds remain logical float-depth receipts. No GPU submission
or physical texture readback is claimed.

Single diagnostic r35 iterations took 680.5 ms compatibility and 670.7 ms
production-slot; observation accounted for 569.7 / 570.7 ms and actual replay
service for 71.5 / 68.0 ms. These are instrumented native wall scopes, not FPS,
thread CPU time, or an optimization verdict. Import/authentication is outside
those iteration scopes. An earlier full-byte-per-draw observer spent roughly
8.3 seconds in observation; byte proofs now reuse a bounded mutation-free
epoch cache and are invalidated on any mutation, definition or destruction.
The first observation compares actual snapshot bytes against core shadow,
rather than trusting a resource handle or a candidate-provided success flag.

The initial integration failures are preserved: r31 rejected identity gamma,
r32 used an incorrect wire-to-core format cast, and r33 exposed default fixture
Depth24Stencil8 support on Apple M1. The fixture now uses the production format
conversion and queries the actual device's Depth24Stencil8 support. r34 native
execution completed, but the wrapper rejected an incorrect record cap and
draw-count metadata lookup; r35–r36 include those schema corrections.

The final r41 rerun also authenticates the effective no-internal-split CPU
policy and requires a positive, bounded actual snapshot-observation count.
Five focused tests pass in `build/meson-logs/cpu-general-final-r41.txt`:
general grammar, snapshot proof, production routing, provider and CLI. The
snapshot proof tests pin A-to-B-to-A invalidation, reused address/revision with
stale bytes, invalid extents and exhausted cache capacity. All four canonical
builds pass; full TLA passes in
`build/meson-logs/cpu-general-tla-r36.txt` (162.88 seconds). TLA covers its
modeled transitions, not the physical byte comparison.

Final artifact:
[GT2 paired CPU r41](../../../experiments/output/fixed-input-harness-20260909/cpu-general-gt2-paired-r41/result.json).

This is the fast CPU policy inner loop, not closure of the independent GT2
Metal pixel residual. General delayed/multi-object schedules, wider command
grammar and missing capture-state attestation remain separate gates. The
existing bounded delayed/dynamic/fault fixtures retain their stricter lifetime
scope and are not silently replaced by immediate completion.

## Bounded deferred CPU acknowledgment (r44–r49)

The explicit `before-next-work` mode preserves one submitted source across
intervening ObjectDestroy events and acknowledges it before the next command,
mutation, definition or PresentComplete. It never enters production work with
an unserviced pending source; no queue-capacity limit was enlarged.

[GT2 delayed comparison r44](../../../experiments/output/fixed-input-harness-20260909/cpu-general-gt2-before-next-work-r44/result.json)
passes 16 iterations per route. Each iteration records 39 deferred releases,
two destruction events with a pending source, 16 retained buffer snapshot
checks, and the same `16710489906774813854` digest as immediate completion.
Direct/compatibility remains 1410/10 in the candidate. This is actual
production Queue/Pool acknowledgment and byte observation with no GPU submit.

Median completion/finish/reclaim wall is 0.385 ms reference and 0.362 ms
candidate; replay is 67.27 / 65.46 ms and observation is 612.00 / 612.34 ms.
These are instrumented native scopes, not policy-speed or FPS evidence.
The full TLA suite passes in `build/meson-logs/cpu-work-boundary-tla-r43.txt`
(166.36 seconds). The boundary predicate has a separate bounded native
enumeration; neither proves GPU texture contents or delay across mutation.

Final r49 repeats both modes, 16 iterations per route (64 child iterations):
[immediate](../../../experiments/output/fixed-input-harness-20260909/cpu-general-gt2-immediate-r49/result.json),
[before-next-work](../../../experiments/output/fixed-input-harness-20260909/cpu-general-gt2-before-next-work-r49/result.json).
All ordered draw and final digests match within and across completion modes.
Seven focused tests pass in `build/meson-logs/cpu-work-boundary-final-r49.txt`.
The isolated indexed fixture observes dynamic VB/IB bytes through both wrapper
destroys, and deliberate physical-byte corruption fails its oracle while
cleanup still succeeds. This pins a destroy window, not mutation overlap.

Failed small-fixture attempts remain recorded: r42 used the narrower old
non-indexed grammar, r45–r46 had non-versioned buffers without captured
snapshots, and r47 mixed the general executor with previously cached routing
test policies and timed out. The accepted fixture uses explicit dynamic
VB/IB snapshots, draw-local resource closure, and a separate process with the
same pre-construction policy pins as the CPU CLI. Production admission was
not relaxed. Non-versioned live observation was still open at r49; the next
section records its separately verified closure for this one-source window.

## Sparse captured/live binding projection (r51–r53)

General projection v2 observes actual effective bindings, separating captured
versioned backing from non-versioned live Pool backing. Both compare physical
buffer bytes with the logical shadow; no snapshot is fabricated. The native
destroy-window matrix exercises all four static/dynamic VB/IB combinations,
both replay routes and immediate/delayed completion. Eight deliberate physical
byte-corruption cases reject and clean up. Pure tests cover the eight route
classifications, live A→B→A invalidation, and resource-versus-UP selection.

The first GT2 attempt, `cpu-general-gt2-immediate-r51`, rejected both routes
at event 940 after 1410 observed draws: the observer incorrectly looked up a
live buffer for UP stream zero. This was an observer error, not a renderer
regression. The fix mirrors production's inline-byte precedence and does not
reject unused inherited snapshots. The failed artifact remains preserved.

[Final immediate](../../../experiments/output/fixed-input-harness-20260909/cpu-general-gt2-immediate-r53/result.json)
and [delayed comparisons](../../../experiments/output/fixed-input-harness-20260909/cpu-general-gt2-before-next-work-r53/result.json)
pass 16 iterations per route each (64 total); every ordered draw digest and
final digest agrees across routes and completion modes. Each executes 1420 draws (candidate 1410 Direct,
10 compatibility), conserves all 39 sources, and checks 4235 captured bindings,
with delayed mode observing two pending destroys and 16 retained byte proofs. The v2 final digest is
`11633069703464745659`; it must not be compared numerically to v1 digests.
GT2 has zero live-only bindings, so the static/mixed coverage comes from the
native matrix. Seven focused tests pass in
`build/meson-logs/cpu-live-binding-r53.txt`; the full TLA suite passes in
`build/meson-logs/cpu-live-binding-tla-r53.txt` (169.10 s). Native and x86_64
Unix builds pass; both canonical PE builds are up to date. TLA evidence does
not certify physical bytes or broaden the admitted delay policy.
These are CPU semantic/lifetime
receipts, not FPS measurements or physical texture/pixel qualification.

## Native resource-scoped mutation qualification (r54–r56)

The general CPU executor now has a native-only option that retains one actual
submitted source across one proved buffer mutation. Its closure comes from
the production `visitSourceCommandResources`, not raw chunk handle tables
(which can omit inherited bindings). The controller uses the actual
`Pool::mapWaitSeqId` before invoking the normal core lock/upload path.

The matrix covers unrelated static-buffer writes with static or dynamic draw
bindings, same-buffer Plain writes for both backing flavors, and referenced
dynamic DISCARD. Each immutable sequence has a second draw after mutation.
Two routes and hold-off/on yield 20 accepted cases with matching per-draw and
final semantics. Six deliberate pending-byte corruptions fail and clean up.
Same-buffer Plain explicitly releases before the potentially blocking map;
DISCARD must select a different active backing while old bytes remain stable.

Nine focused tests pass in `build/meson-logs/cpu-mutation-r56.txt`.
`render_tape_cpu_mutation_boundary_spec.cpp` exhausts 512 witness valuations
and all four/five-action words over publish, two mutation classes and complete;
counterexamples cover self-wait and captured-backing overwrite. The first
r54 integration caught an incorrect model expectation that safe mutation had
not entered the lock while pending; the corrected model executes no-wait
mutation before acknowledgment. The failed test log is preserved.
The existing full TLA suite passes in
`build/meson-logs/cpu-mutation-tla-r55.txt` (167.01 s); it is complementary
evidence, not a claim that this new byte policy has a separate TLA module.

Native and all three canonical cross build directories pass. The unchanged
[GT2 delayed CLI mode](../../../experiments/output/fixed-input-harness-20260909/cpu-general-gt2-before-next-work-r56/result.json)
and [immediate mode](../../../experiments/output/fixed-input-harness-20260909/cpu-general-gt2-immediate-r56/result.json)
also pass 16 iterations per route each (64 total), with identical ordered
observations and v2 digest `11633069703464745659`. Neither selects the new native-only
mutation option. CLI publication of that policy remains gated on qualified
input mutation metadata and an authenticated source-closure projection;
candidate-provided `safe` flags are not sufficient.

## Qualified source projection and public mutation policy (r65–r66)

The [r65 projection](../../../experiments/output/fixed-input-harness-20260909/cpu-general-gt2-source-projection-r65/source-projection.json)
records the real production resource visitor's buffer closure for all 39
commands: 913 references in total, maximum 43 in one source. Two immediate
compatibility replays agree. The artifact binds the input components,
inspection, provider/validator executables, loaded images and reference
receipt. It is an observation, not an admission decision. Optional identity
sidecars are validated when present; event-defined resource identity does not
claim independent frame/pass-sidecar completeness.

`--source-projection` now stages and validates this evidence, then supplies a
bounded source oracle to either replay route. The actual native visitor must
match every row on every repetition. Only the existing no-wait/backing reducer
can then retain one source through one Plain/unrelated or DISCARD/referenced
buffer mutation. The wrapper derives release boundaries from input metadata,
checks mutation-counter conservation, and compares every cumulative draw and
final digest with the immediate-completion reference, not merely with the
other delayed route.

The final [r66 comparison](../../../experiments/output/fixed-input-harness-20260909/cpu-general-gt2-source-mutation-r66/result.json)
passes 16 repetitions on each route:

| Per replay | Compatibility | Production Slot |
|---|---:|---:|
| Draws | 1420 | 1420 |
| Direct / compatibility draws | 0 / 1420 | 1410 / 10 |
| Source closures checked | 39 | 39 |
| Submitted / completed / reclaimed | 39 / 39 / 39 | 39 / 39 / 39 |
| Deferred buffer mutations, all DISCARD | 3 | 3 |
| Retained-buffer byte checks after mutation | 20 | 20 |
| Release-before-mutation boundaries | 3 | 3 |

All repetitions retain semantic digest `11633069703464745659` and match the
immediate reference's draw-by-draw observations. This is non-vacuous actual
Pool/map/upload/snapshot evidence without GPU command submission, not an FPS
measurement or a Metal pixel-equivalence result.

Reproduce with the unchanged bundle and matching binaries:

```sh
bash scripts/run_python.sh scripts/tools/run_dxmt9_render_tape.py cpu-source-project \
  experiments/render-tapes/gt2-v2-20260909-r4-capture64/frame-1292677716873100-1 \
  --output-dir experiments/output/gt2-source-projection --repeat 2
bash scripts/run_python.sh scripts/tools/run_dxmt9_render_tape.py cpu-compare \
  experiments/render-tapes/gt2-v2-20260909-r4-capture64/frame-1292677716873100-1 \
  --output-dir experiments/output/gt2-source-mutation \
  --grammar general-frame --resource-backend cpu-metal-resources \
  --completion before-next-work --max-pending 1 --repeat 16 \
  --source-projection experiments/output/gt2-source-projection \
  --timeout-seconds 60
```

Use fresh output paths; a provider rebuild requires a new projection. Twelve
focused native/CLI tests pass (`cpu-source-binding-r65.txt`), followed by the
three final Python/CLI tests (`cpu-source-binding-r66.txt`). The CLI fixture
tests both routes and paired comparison, malformed input/validator failure,
inspection mismatch, altered artifact bytes, held-event/identity/release
tampering and divergence from the immediate reference. Native tests pin
missing/extra/stale rows, the empty Present closure and malformed binary input.
Full TLA passes in 168.34 seconds (`cpu-source-binding-tla-r63.txt`); the shared
mutation reducer still uses its separate exhaustive native checker. Native
and all three canonical cross builds pass.

The [r64 rejection](../../../experiments/output/fixed-input-harness-20260909/source-oracle-compat-r64/result.json)
is preserved: execution completed, but four mutation counters were absent
from JSON, so acceptance correctly failed. r65 adds those fields; r66 further
binds all candidate observations to the immediate reference. Earlier failed
evidence is not relabelled. No renderer default or Wine runtime was changed.

## Reset, retired surface-alias attribution and environment qualification (r74–r75)

The general CPU executor now runs A -> B -> A with actual changed buffer
contents on both replay routes and immediate/deferred completion. One Metal
device is shared, while every iteration receives fresh core/queue/resource
owners. Every A draw and final digest match; B differs. A native skipped
wrapper-release fault fails the real reset oracle before recovery cleanup,
and the next fresh A still matches. This is cold-device reset qualification,
not warm-core reuse.

The alias test exposed a logical observation defect: after the Tape releases
parent texture T, surviving render-target surface S still owns T, but the
old live-wrapper lookup stopped finding T's seed receipt. The observer now
resolves S's immutable parent identity/subresource and checks the actual
retained backing against a live entry or retirement tombstone. The fixture
passes existing validation without widening grammar: two indexed draws target
S; only parent destruction moves across their boundary. Both schedules/routes
preserve all observations. Changing T's seed changes the result. Reinstating
the old attribution through a native fault leaves the first draw equal and
diverges on the post-destroy draw, while cleanup still succeeds. Same-object-ID
texture redefinition remains outside this fixture. Texture evidence is
logical seed/command evidence, not GPU readback.

CPU receipts now record the allowlisted renderer/Metal environment passed to
the native process. Enabled or unknown nonempty API/shader validation settings
make the run diagnostic, even when native execution passes; comparison and
source projection cannot promote it. The environment negative controls use a
provider double to test the wrapper, not driver-validation execution.

Twelve focused tests plus full TLA pass in
`build/meson-logs/cpu-alias-reset-r74.txt` (TLA 165.04 seconds). Native and all
three canonical cross builds pass. Earlier malformed alias fixture attempts
remain in the r69–r71 logs; r72/r73 and the integrated r74 suite pass.

The fresh [r75 projection](../../../experiments/output/fixed-input-harness-20260909/cpu-general-gt2-source-projection-r75/result.json)
binds the new executable to all 39 sources in the unchanged r4 GT2 bundle.
The [r75 paired comparison](../../../experiments/output/fixed-input-harness-20260909/cpu-general-gt2-source-mutation-r75/result.json)
passes 16 repetitions per route, without validation diagnostics. Each replay
executes 1420 draws, checks all 39 source closures and conserves 39
submissions/completions/reclaims. The candidate executes 1410 Direct and 10
compatibility draws; the reference executes 1420 compatibility draws. Both
retain three DISCARD mutations, perform 20 retained-buffer checks and release
before three unqualified mutation boundaries. All final/per-draw observations
match the immediate reference; final digest remains `11633069703464745659`.
No Wine launch, GPU submission, FPS claim or renderer-default change occurred.

## Source-owned proofs and actual capacity qualification (r79–r81)

`CpuGeneralTapeExecutor::PendingSourceProof` now owns the schedule, complete
sequence/source/storage identity, resource closure, backing proofs and mutation
state together. The owner is retained only after actual submission identity
matches, and released only after its trace identity, completion and reclaim
match. Six native faults perturb each identity dimension independently; all
24 route/preset combinations reject before normal acknowledgment, retain a
failed trace and clean up. Four non-fault controls complete.

The separate native two-source fixture replays B while A is already submitted
and pending. It seeds actual Metal CPU-visible buffers, retires both D9C
wrappers after unbinding and verifies A-only reclaim leaves B's backing bytes
intact. The existing event-qualified controller rejects B-before-A; repeated
A release does not acknowledge B. Direct/compatibility route counts prevent
silent fallback from masquerading as two-route evidence.

Actual capacity saturation reaches 31 pending sources on both routes without
any completion. Repeated denied admission probes leave the queue unchanged;
one real FIFO completion/reclaim admits exactly one replacement. All 32 actual
submissions then reclaim. This is a native admission/reclaim qualification,
not a larger public Tape pending window or a GPU progress result. The pure
admission table covers 1,600 valuations and every missing capacity dimension.

Fourteen focused native/CLI tests pass in `cpu-source-owner-r80.txt`. The two
completion controller tests and full TLA suite pass in
`cpu-two-source-model-r79.txt` (TLA 168.76 seconds). No production path or queue
capacity was changed by this qualification.

The fresh [r81 projection](../../../experiments/output/fixed-input-harness-20260909/cpu-general-gt2-source-projection-r81/result.json)
authenticates the rebuilt executable and all 39 GT2 source closures. The
[r81 comparison](../../../experiments/output/fixed-input-harness-20260909/cpu-general-gt2-source-mutation-r81/result.json)
passes 16 repetitions per route with `diagnostic=false`: 1420 draws, 39
submissions/completions/reclaims, three deferred DISCARDs and 20 retained-buffer
checks each. Candidate routing remains 1410 Direct / 10 compatibility draws;
the reference uses 1420 compatibility draws. Every ordered observation and
final digest `11633069703464745659` matches the immediate reference. The public
general-frame pending peak remains one. This supersedes r75 as the latest CPU
checkpoint, not as capture-fidelity or FPS evidence.

## Public two-source CPU window (r82–r88)

The general executor now retains a FIFO of independently qualified source
owners. The shared cold emission planner and independent inspector identify
eligible whole-source Draw/APPLY_STATE ranges; the Python validator derives
release order from authenticated input, not provider claims. Queue admission
uses a fresh actual-capacity snapshot with workers/offload/Arena absent. This
does not enable concurrent production replay or change renderer defaults.

Independent review found that incoming-only eligibility could preserve a
noneligible predecessor. R88 closes that edge: each retained owner carries its
eligibility, and Draw A / Clear B / Draw C / Draw D releases B before C while
C/D may overlap. Both real replay routes exercise this fixture, with and without
buffer mutations. Native admission checks exhaust 128 window valuations and
1600 capacity valuations. Forged input plans, release/destroy receipts, runtime
flags, sequence mappings and vacuous peak-one executions fail public acceptance.

The [r88 comparison](../../../experiments/output/fixed-input-harness-20260909/cpu-general-gt2-window-r88/result.json)
passes 16 repetitions per route on the unchanged full GT2 input. Of 39 inspected
command sources, 28 have eligible shapes. Every repetition actually reaches
pending peak two, conserves all 39 submitted/completed/reclaimed sources and
observes 1420 draws. The candidate uses 1410 Direct and 10 compatibility draws;
the reference uses 1420 compatibility draws. Final semantic digest remains
`11633069703464745659`.

Reproduce this CPU-only policy comparison with a fresh output directory:

```sh
bash scripts/run_python.sh scripts/tools/run_dxmt9_render_tape.py cpu-compare \
  experiments/render-tapes/gt2-v2-20260909-r4-capture64/frame-1292677716873100-1 \
  --output-dir experiments/output/cpu-gt2-window-new \
  --grammar general-frame --resource-backend cpu-metal-resources \
  --completion before-next-work --max-pending 2 --repeat 16 \
  --timeout-seconds 60
```

A separately executed [immediate control](../../../experiments/output/fixed-input-harness-20260909/cpu-general-gt2-immediate-r88/result.json)
also repeats 16 times. Every ordered draw observation and final digest agrees
with both window routes; the [control check](../../../experiments/output/fixed-input-harness-20260909/cpu-general-gt2-window-r88/immediate-control-check.json)
records the exact compared artifact hashes. These are CPU semantic/backing and
lifetime results, not pixel, throughput or FPS evidence. The runs may coexist
because their timing is not used for a performance verdict.

The separate max-one mutation policy also passes with a freshly generated
[r88 source projection](../../../experiments/output/fixed-input-harness-20260909/cpu-general-gt2-source-projection-r88/result.json)
and [r88 comparison](../../../experiments/output/fixed-input-harness-20260909/cpu-general-gt2-source-mutation-r88/result.json):
16 repetitions per route, 39 independently checked source closures, three
deferred DISCARDs and 20 retained-buffer checks each. The same final digest is
preserved; this does not combine mutation delay with the max-two window.

The native build and three focused native/CLI tests pass in
`build/meson-logs/cpu-window-retained-eligibility-r88.txt`. Completion plan,
event binding and the full TLA suite pass in
`build/meson-logs/cpu-window-model-r85.txt` (TLA 168.82 seconds). The release
policy's retained-eligibility refinement is pinned by the shared C++ reducer
table and actual mixed-source fixture, not claimed as an additional TLA model.
R87 artifacts remain historical; r88 supersedes their weaker eligibility gate.

## Stream-frequency production refinement (r89 onward)

The canonical PE binding owner now holds frequencies independently of stream
buffer/offset/stride tuples, defaults them to one, and supplies StateBlock,
Reset and SWVP. All four producer lanes emit the explicit value. Unix sparse
replay journals tuple and frequency together. Legacy wire zero remains
omitted/preserve; the state folder preserves previously known values through
sparse and FULL_SNAPSHOT updates without inventing unknown capture history.

The new actual-queue A/B/A fixture exposed two independent reuse omissions:
the frequency setter used binding-only cache invalidation, and the shared
batch/encoder key predicate omitted frequencies. Fixing only invalidation left
all three submitted draws at A (`[2,2,2]`, r92); frequency must participate in
both the stable generation and compatibility relation. The subsequent flagged
fixture exposed another bypass: sparse batched replay retained DrawParam's
default instance count instead of deriving it from effective stream-zero
frequency. It now calls the same pure reducer as ordinary draws before choosing
a submission route. These are production corrections discovered with native
CPU queue observations, not an attribution of the historical GT2 pixel error.

The tests cover every stream slot's key/record compatibility, Arena/ordinary
direct/compatibility A/B/A, indexed/instance-data flags, queued instance counts,
legacy omission, null bindings and rollback. Producer parity retains the old
zero-frequency golden corpus without regenerating its hashes. Twenty-eight
fold cases distinguish omission from explicit reset across sparse/full updates.
Historical zero-frequency bundles still require new capture-origin evidence;
native replay equivalence alone cannot supply it.

The final native build and both PE builds pass. The r94 focused suite passes
20 of 21 tests, including all actual-queue frequency cases; the CPU experiment
CLI fails while writing `provider.stdout` with `ENOSPC`. The concurrent r93 TLA
run reaches its 300-second timeout and is not a proof result. The x86_64 Unix
full rebuild is interrupted after storage exhaustion; it is not qualified as a
completed build. Existing r91 TLA and r88 GT2 results remain historical.

The fresh [r95 GT2 attempt](../../../experiments/output/fixed-input-harness-20260909/cpu-general-gt2-window-r95/result.json)
is rejected before valid child receipts because storage is exhausted. Its
114 MB duplicated input is removed, while failure receipts and the original
authenticated r4 capture remain; the [cleanup receipt](../../../experiments/output/fixed-input-harness-20260909/cpu-general-gt2-window-r95/cleanup-note.json)
records that disposition. Immediate-control staging is cleaned by the wrapper,
and source-projection staging cannot start. No new GT2 equivalence or performance
claim follows from these failed artifact operations.

After removing only that failed input copy, the CPU experiment CLI rerun passes
in `build/meson-logs/cpu-stream-cli-r96.txt` (30.16 seconds). Together with r94,
all 21 focused tests have passing post-fix evidence; this does not turn the
failed GT2 artifact operation into an accepted run.

The separate r96 full TLA rerun passes in
`build/meson-logs/cpu-stream-tla-r96.txt` (340.90 seconds, timeout multiplier
two): all 55 safety, two progress and 121 expected-counterexample
configurations pass their declared gates. This is the existing model suite;
the new stream-frequency cache/batch contract is pinned by native production
tests, not claimed as a newly added TLA model. Fresh GT2 CPU artifacts and the
interrupted x86_64 Unix full build remain open pending storage headroom.

## Canonical mipmap CPU replay (r98–r100)

The general CPU grammar now consumes GENERATE_MIPMAPS through production
replay and Queue/Pool. Its Slot observer records ordered texture identity and
counts executed generation commands. The inspector histogram includes command
30 (`generate_mipmaps`); the public receipt validator compares this authenticated
input count with every replay iteration and rejects missing or forged counts.

The small authenticated AUTOGEN fixture carries a level-zero seed, an indexed
draw, a generation command, wrapper destruction and Present. Both compatibility
and production-slot routes pass four fresh-device iterations each under
after-command and before-next-work completion in the retained r100 artifacts.
The delayed test actively attempts Pool reclaim after destroy and checks that
the pending texture remains alive, then proves final cleanup. Ordered digests
agree across both routes and both completion policies. This is CPU logical
command/lifetime evidence; `physical_texture_oracle` remains false.

The r98 native mipmap/destroy/grammar tests pass, as do eight focused regression
tests. The extended public CLI test passes at r100 (11.25 seconds). The r98
existing TLA suite passes (172.92 seconds); no mip-texel or new explicit-call
completion model is claimed.

Independent PE review rejected the provisional explicit GenerateMipSubLevels
append-only migration: the old backend calls `waitUntilCompleted`, while mere
record acceptance returns before execution and loses recorder-unavailable
behavior. That provisional migration was removed, preserving the existing
synchronous public call. Capturing this explicit call still requires a
completion-preserving bridge and failure contract; CPU canonical-command
support is not evidence that this producer gap has closed.

## Owned input staging and fresh GT2 CPU checks (r101–r103)

CPU-only staging now requests macOS filesystem clones instead of unconditionally
materializing the same bundle three times for a paired experiment. The files
remain independently owned, not hardlinks: local tests verify distinct inodes,
bidirectional mutation isolation, dereferenced input symlinks, existing-target
refusal, portable copy fallback and propagated copy errors. Normal digest and
identity validation still authenticates the copied bytes. A clone request is
not a guarantee of physical space savings on every filesystem. Large CPU
receipts also use compact JSON, preserving every field and observation.

The r102 two-source comparison and separate immediate-completion control pass
16 iterations per route on the unchanged authenticated GT2 capture. Each has
1420 draws; the candidate executes 1410 Direct and 10 compatibility draws.
All iterations submit, complete and reclaim 39 sources. The delayed lane reaches
pending peak two. The ordered digest remains `11633069703464745659`; the
`immediate-control-check.json` receipt binds matching input, provider and loaded
images across both runs. This refreshes CPU equivalence after the native
stream-frequency corrections, but cannot supply missing historical PE capture
provenance for legacy zero-frequency or texture LOD state.

The r102 source projection separately authenticates 39 production closures.
Its source-mutation comparisons at r102 and r103 both fail at parent receipt
writing with ENOSPC; compact formatting alone does not close that storage
gate. The transaction removes unpublished staging and child receipts on this
exception. Each attempt has an `artifact-failure.json` documenting the lost
raw evidence, so neither is claimed as an accepted child or aggregate run.
The last accepted source-mutation evidence remains r88. The compact-receipt
and projection CLI regression tests pass at r103 (11.36 and 2.42 seconds).

The explicit mip producer gap now fails closed with
`unsupported_explicit_mip_generation` for tracked AUTOGEN calls with hidden
levels, including before arm. Non-AUTOGEN/one-level no-ops and disabled tracking
remain unaffected. All three child variants retain their existing flush,
direct call, GPU wait and success-only clean settlement. Shared predicate
truth tables, capture tests and both PE builds pass. This is a capture safety
guard, not support for explicit-call replay: complete source-sidecar coverage
still needs an authenticated Unix direct-completion receipt.

## Failure-preserving CPU artifacts and exact event lookup (r104–r107)

Populated failed CPU transactions now retain raw output in a private rejected
directory instead of deleting it when the aggregate cannot be published.
Secondary ENOSPC during marker/quarantine creation or rename leaves the original
private staging intact and reports its location. Empty pre-evidence staging
and non-CPU publication retain their previous cleanup policy. Native-free
fault tests cover original-error preservation, interruption, marker/name/move
failure and existing-output protection. The full CLI and capture tests pass in
`cpu-retain-exact-event-r105.txt`.

This path was exercised by the actual r105 standalone GT2 compatibility run:
its 2.9 MB `provider.stdout` survives with 16 complete iterations, 1420 draws
and 39 submitted/completed/reclaimed sources each. The root receipt write
still reports ENOSPC for 3,320,281 requested bytes despite 125,743,104 free bytes
sampled just before the write. The normal target is absent and `rejected.json`
marks the unpublished transaction; these raw native results are not an
accepted aggregate. A later allocation probe succeeds for both a single 4 MiB
write and 64 KiB blocks after reported headroom increases. No write-granularity
cause is established and no block-write workaround is claimed.

With the current native image and a fresh two-repeat immediate source oracle
(r105), the r106 paired source-mutation comparison passes all 16 iterations
per route. Every iteration has three deferred DISCARDs, 20 retained-buffer
checks, 39 source closure checks and the same ordered digest
`11633069703464745659`. The candidate executes 1410 Direct and 10 compatibility
draws; both routes complete/reclaim all 39 sources. Every draw matches the
bound immediate reference. The complete compact parent receipt is 17,631,060
bytes. Failed r102/r103/r105 attempts remain distinct preserved evidence.

The production identity ledger additionally exposes exact token/event lookup
without completion side effects. Native pins distinguish an earlier event
from a newer tail, clear stale output on foreign/missing lookup, prevent lookup
from acknowledging completion and preserve an early acknowledgment through
final settlement. This is the lookup primitive for a completion-preserving
isolated explicit mip record; the PE committed-event ticket and completion-only
bridge are still unimplemented. The capture guard is not removed.

The r107 review additionally rejects inconsistent child execution metadata:
neither a successful nested payload nor an accepted label can override an
invalid execution flag or a nonzero/non-integer provider exit code. Both
routes have forged-receipt negative tests. A post-publication interruption
keeps the committed output and propagates cancellation without reporting
nonexistent rejected staging. The three capture/CPU CLI/source-projection CLI
tests pass in `cpu-receipt-acceptance-r107.txt`. Read-only revalidation of the
existing r106 comparison with the strengthened checker also passes all 16
iterations; no new workload execution or FPS measurement is claimed.

## Explicit mip capture completion bridge (r108–r114)

Active, negotiated PE explicit mip generation now emits an isolated canonical
GENERATE_MIPMAPS record. A cold device-scoped collector retains the actual
accepted token/event even when CapacityPost commits during append and the
final explicit flush is empty. A second accepted commit invalidates the
single-event receipt, including a repeated equal ticket. The exact-event
bridge drains replay, authenticates that event's settlement, releases a
potentially parked session through flush, waits completion and acknowledges
only the selected event. Capture-off, pre-arm, unavailable-recorder and
physical-one-level paths retain their prior behavior.

Capture metadata failure after acceptance does not replay the operation or
invent a capture acknowledgment. A separate completion-only bridge drains and
flushes application work, then checks the real queue waterline and terminal /
unpublished-work conditions. Genuine append/bridge errors remain errors. This
preserves application completion without upgrading a rejected capture.

The r111/r112 integrated attempts timed out in the new native test's teardown:
its flush hook unconditionally dequeued a source after the queue was empty.
The captured stack identified `RuntimeFixture::releaseCoreForInspection` /
`Device::~Device` / the fixture hook, not a production encode-thread wedge.
The hook now consumes only its ready-count snapshot. The corrected routing
test passes in 0.63 seconds (r113), and all seven capture, routing, arena,
ABI and CPU CLI tests pass together (r114). Thirteen additional provider,
projection, PE producer, lifecycle and install-name checks pass (r115).
Native, 32/64-bit PE and x86_64 Unix builds pass; no Wine staging is performed.
Failed logs are retained rather
than relabelled as passes.

`RenderTapeExactEventWait` covers two ordered events with 182 generated / 87
distinct states. Wrong-latest-row selection and premature acknowledgment are
expected invariant failures. The full TLA gate passes in 211.48 seconds (r110).
The model is finite safety, not liveness or driver validation. Native pins use
the real ledger/C bridge/Queue/Pool but supply CPU completion through a test
hook; the pure receipt test does not execute PE COM CapacityPost itself.
Actual PE fault integration, parked production-thread progress and generated
GPU mip texels remain unqualified. No Wine, visual, FPS or promotion evidence
is claimed by these runs.

## Conditional progress and current-image CPU qualification (r116–r119)

The exact-event model now has a separate valid-request progress configuration.
For two accepted requests, fair enabled bridge/queue steps must eventually
return success or explicit failure. The abstract parked queue prefix can
advance only after a release fence. The normal progress case has 34 generated /
32 distinct states; removing the release produces a temporal counterexample
without violating the safety invariants. Full verification passes all 183
configurations in 167.27 seconds (r116). This is conditional protocol progress,
not an assertion about Metal driver scheduling.

A fresh current-image source projection (r117, two immediate reference
iterations) authenticates all 39 GT2 source buffer closures. The paired r118
source-mutation run passes 16 iterations per route, each with 1420 draws,
39 submitted/completed/reclaimed sources, three deferred DISCARDs, 20 retained
buffer checks and 277 released wrapper owners. The candidate uses 1410 Direct
and 10 compatibility draws; the reference uses 1420 compatibility draws.
Every iteration preserves digest `11633069703464745659`, matches the bound
immediate reference and passes reset. Executable images remain stable.
The first invocation omitted `--max-pending 1` and was rejected before output
creation; the executed r118 invocation explicitly supplies that required bound.
No rejected execution was relabelled or overwritten.

Nine lifecycle/CLI checks pass in r119: destroy and mip lifetime, deferred
mutation, A-to-B-to-A reset, alias after parent destruction, two pending real
sources, mutation admission, source projection and the CPU experiment CLI.
One-source mutation and two-source FIFO are separately qualified bounded
contracts; their concurrent composition is an extension, not a tested product
of those cases. The numbered R8 requirements do not demand every combination
of otherwise bounded policies. PE COM auto-commit fault integration and the
production-thread completion composition remain separately tracked.

## Singleton exact-event identity survives physical retirement (r120–r127)

A new native test drives the real captured raw replay, production CPU-ready
encode loop, ordered flush, C completion bridge and capture acknowledgment.
Its backend pauses deterministically and substitutes CPU-controlled completion;
it does not submit GPU work. The test requires a Submitted post-encode receipt,
so a resident-source fallback cannot silently satisfy the regression.

The first two attempts exposed test problems: r120 had a dangling flush callback
during failure cleanup, and r121 used an invented completion helper that bypassed
the actual retired receipt. After correcting teardown and using the existing
receipt-aware completion helper, r122 exposed the production bug: the queue
sequence completed, but the exact event never settled. Those logs remain
preserved and are not successful evidence.

Strict singleton retirement erased the group tuple along with the physical
source. The post-encode receipt previously retained only sequence/Present
information. It now owns the full group identity before payload detach. Only
successful completion transfers it once into the source-ordered settlement
FIFO; finish publishes the exact tuple. Receipt and resident tails share one
loop, preserving their order. Multi-member groups retain their existing
physical-residency policy. No receipt token, PE ABI, draw observer or per-source
heap allocation was added; the fixed queue receipt array gains inline metadata.

The distinguishing test passes in r123 (1.99 seconds) and all ten r124 repeats.
The four native tests in r127 pass: receipt transition/ABA, singleton source
reuse, mixed receipt/resident completion, and the integrated encode loop.
That invocation's separate install-name audit failed because it scanned the
x86 Unix image while its full build had linked but not yet applied the normal
fixup; this is not an all-green r127 gate. Final build/audit evidence is recorded
separately below. Independent review found no production blocker. The test
does not establish entry into the coordinator's idle CV wait, and a FIFO-full
failure immediately after identity extraction is not a new dedicated pin.

Current-image GT2 source projection r125 validates 39 source closures. The
paired r126 replay passes 16 iterations per route: all 1420 draws, all 39
source completions/reclaims, 3 deferred DISCARDs, 20 retained buffer checks,
4235 snapshot observations, and 277 wrapper acquisitions/releases per iteration.
Candidate Direct/compatibility draws are 1410/10 versus reference 0/1420.
All digests are `11633069703464745659`, with reset and image stability passing.
This CPU receipt does not validate physical texture pixels or live FPS.

Final verification uses the rebuilt native and x86_64 images. Full TLA r128
passes all 188 configurations in 214.21 seconds (57 safety, 4 progress and 127
expected failures). `RenderTapeSingletonArenaRetirement` has 18 distinct states
for both safety and progress. Dropping identity transfer reproduces an eventual
settlement failure even though sequence completion advances; stale extraction
and premature publication fail their named safety checks. This models two
sequential singleton groups; native mixed-source tests supply the separate
resident/receipt ordering pin, not an unbounded composition proof.

After final header/build reconciliation, fresh projection r129 and comparison
r130 reproduce all r126 counts/digests for 16 iterations per route, with stable
loaded images. Native r131 passes all 24 selected checks, including the now
completed install-name audit. The x86_64 r132 gate passes receipt, Tape, mixed
queue completion and production-loop tests. `git diff --check` and the perf
document source audit pass. No Wine launch, GPU submission, visual/FPS
qualification, commit or promotion was performed in this continuation.

## Bounded CPU harness closure: timing and retained schedules

The final receipt audit found a real difference between the two CPU grammars:
general-frame had phase observations, but bounded experiments reported only
elapsed time. R133 revalidated the external plans but did not close that timing
requirement. R134/r135 preserved and reproduced a release-callback failure;
they remain historical receipts, not evidence for the new timing contract.

The bounded runner now reports preparation and shared-device setup separately,
then exclusive setup/replay/observe/completion/cleanup wall phases. Nested
observation and completion pause the surrounding phase. Fixture/sink destruction
is inside elapsed/cleanup. Both grammars report per-iteration process CPU time,
explicit fresh-runtime cache policy and whether the native resource device is
shared. Process CPU can exceed wall time and is not replay-thread time. General
phase observations retain their existing semantics; they are not relabelled as
the bounded exclusive ledger. No encoder/GPU timing is fabricated.

Fake-clock tests pin nested scopes and exception unwinding; timestamp conversion
rejects invalid/overflowing values. CLI negatives reject missing or malformed
phase/CPU/cache/reset metadata, including a separately reviewed general-frame
top-level reset-policy omission. Failed iterations retain their timings. A
partially available process clock preserves raw samples and fails acceptance,
rather than silently dropping successful samples.

Current-image external plans in r137 pass 16 repetitions on both routes:
9 draws, two submitted/completed/reclaimed sources, reset true and digest
`7614069032485648982`. Compatibility executes 0/9 Direct/compatibility draws;
production-slot executes 7/2. R139 injects failure before release and preserves
the actual prefix: `completion-plan-release-callback-failure`, one submission,
zero completions/reclaims and failed reset. R140 consumes only the retained
input and schedule plus the same declared fault option and reproduces that
prefix and digest `14217217491593176700`. This is failure reproduction, not a
successful aggregate or an automatic schedule-reduction claim.

R138 authenticates 39 GT2 source closures against the rebuilt images. R141
passes 16 paired repetitions with phase/process-CPU data; r144 repeats after
the final Python reset-policy acceptance fix. Both retain 1420 draws, candidate
1410/10 versus reference 0/1420, 39 submitted/completed/reclaimed sources,
three deferred DISCARDs, 20 retained-buffer checks, 4235 snapshot observations
and 277 acquired/released wrappers. All draw observations and final digest
`11633069703464745659` match; reset and loaded-image stability pass.

| CPU requirement | Closing evidence in the declared bounded scope |
|---|---|
| R8.1 authenticated input and separated costs | Owned Tape/blobs and image-bound projection; bounded exclusive phase ledger; both grammars' process CPU and preparation receipts |
| R8.2 production path, non-vacuous candidate | Real replay/planner/Slot/Pool; r137 capacity fallback and r144's 1410 Direct/10 compatibility draws |
| R8.3 reset/cache policy and contamination detection | Explicit runtime/cache/device sharing; native changed-byte A-to-B-to-A and skipped-release negative; all repeated resets pass |
| R8.4 qualified controlled completion | Event/source/generation-bound controller, shared native predicates, TLA expected failures and external-plan r137/r139/r140 |
| R8.5 semantic/lifetime equivalence | Paired GT2 per-draw comparison, source closure, retained backing, destroy/rename/capacity/allocation-fault native pins |
| R8.6 receipts and reproducible failures | Strict metadata validation, preserved raw prefixes, actual plan-bound failure replay, GT2 plus small lifecycle input |

Native r142 passes 25 selected tests including CLI and install-name checks;
x86_64 r143 passes five routing/queue/receipt/phase tests. Both default builds
pass. The lifecycle model is unchanged by cold timing instrumentation: full
TLA r128 remains the 188-configuration result (57 safety, four progress, 127
expected failures). This closes the bounded CPU policy/lifetime harness, not
an unbounded concurrency proof, a Wine COM capture gate, GPU pixels or game FPS.

## Next gate

Use the [fixed-input commands](../../../specs/experiments/harness/replay/fixed-input-experiment.md)
for bounded policy experiments. Preserve the actual executed path, full failure
prefix and binary/input identity. Extend one unsupported lifecycle or draw shape
at a time with model/native/Metal evidence. Wild runs remain the final supply,
locality, visual and FPS gate rather than the inner-loop test.

The bounded public two-source path is available; wider windows, worker-backed
admission, delay across resource mutations and broader command families still
need their own no-wait/lifetime proofs. The queue capacity probe alone does not
authorize those paths. Capture state completeness and the strict full-frame
GT2 Metal residual remain independent open gates.
