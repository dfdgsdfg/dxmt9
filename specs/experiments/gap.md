---
type: "Spec Gap"
title: "Experiments Gap"
description: "Implementation and evidence gaps for wild integration experiments."
tags: [specs, gap, experiments]
---

# Experiments Gap

## Detailed performance tracing — partial

[Tracing gaps](harness/tracing/gap.md) own R-HARN-TRACE-1.1–11.8.
CPU supply/wait traces, PE sampling and Metal joins provide existing inputs;
the required unix-owned Perfetto SDK integration, native trace bundle and
calibrated cross-source timeline remain unevidenced as an integrated system.
REA-guided target probes and a separate QEMU structural lane have explicit
acceptance contracts in the
[tracing spec](harness/tracing/spec.md).

The [external probe contract](harness/tracing/external-probes.md) adds
platform-qualified address/register and CPU-sampling evidence. Linux perf
sources remain optional reference lanes, not macOS collector dependencies.

## Fixed-input replay experiment

### External one-shot capture selection — partial

R-HARN-REPLAY-9.24 has a capture-only PE file reader, process/nonce/expiry gate,
request-to-token/epoch/queue logging and a same-prefix writer/transport fixture.
The original skip selector remains unchanged when no request is configured.
Native refinement covers 117,649 six-action histories and malformed-field/time
counterexamples; capture/snapshot/install-name regression tests pass 4/4 on
x86_64. Both PE builds and Sikarugir x86/x64 controlled file transport tests pass,
including cross-process submission and refusal of a mismatched creation time.
Existing TLA regression passes (209.69 s); the new selector's model/code binding
is the exhaustive native checker, not that regression suite. Evidence:
[H501](../../docs/perfomance/present-pacing/present-pacing-arm-request.501.md).
The new selector does not read game memory: a supervised camera/frame bracket,
published source join and original-image oracle are still required. H500
excludes the older H481 input from its intended relative tail window; it must
not be used to declare the largest default-tail CPU owner. See
[capture-arm-request.md](harness/replay/capture-arm-request.md).

[H502](../../docs/perfomance/present-pacing/present-pacing-scene-request-preflight.502.md)
now demonstrates the external late-camera request and real arm in two bounded
game runs. Both reject final identity attachment; the second isolates
`RenderTapeIdentityV4Status::InvalidCoordinator` before topology construction.
No authenticated bundle or original GPU oracle exists for this input. Retain
the exact failing coordinator/attempt witness and add a native counterexample
before another retry; do not relax provenance or change scene selection to
obtain a passing frame.

[H503](../../docs/perfomance/present-pacing/present-pacing-coordinator-witness.503.md)
adds a bounded first-failure witness to the existing cold validator, retained
through capture rollback. Native tests prove witness/no-witness admission parity
and rearm clearing. It is not a serialized success artifact.

[H504](../../docs/perfomance/present-pacing/present-pacing-closed-query-ordinal.504.md)
uses that witness in the unlocked late-camera run: the V2 closed occlusion
descriptor loses pre-cut absolute Issue history (canonical ordinal 1 versus
runtime 1017, both cycle 509). A native reduced counterexample reproduces the
rejection. [H505](../../docs/perfomance/present-pacing/present-pacing-closed-query-history.505.md)
implements R-HARN-REPLAY-9.9's versioned closed-history preservation and
fresh-local V4 execution mapping without old-result authority or weaker checks.
The native V2 rejection remains, V4 positives/negatives and 4,096 bounded query
histories pass, and H502/r5 publishes an authenticated late-camera bundle with
7 sources and 3,982 draws. Request/token/epoch/queue and component hashes join;
the external camera envelope is not per-draw/default-work qualification.

Original replay now stops at a separate contract gap: the captured production
occlusion-counter backend is not admitted by the real-Metal provider, which
requires hardware occlusion for every occlusion definition. Forcing hardware
occlusion changes source publication and fails the exact comparator at event
4075. R-HARN-REPLAY-9.3–9.5's GPU-query oracle must not be weakened. A separately
scoped, input-bound same-production-query diagnostic is implemented by
R-HARN-REPLAY-9.25 / [H506](../../docs/perfomance/present-pacing/present-pacing-production-query-diagnostic.506.md).
It owns caller-attested source/receipt digests, requires Normal exact-source
mode, and checks actual Metal/counter/asynchronous construction before effects.
Four H502/r5 replays preserve 7 sources, 9 Issues, 5 polls, 4 defined query
results and both original images bit-exactly. Hardware and synchronous negative
processes reject before setup. Six focused tests and 237 existing TLA
configurations pass. `gpuResultOracle` remains false: no public policy or
hardware-result promotion occurs. External receipt contents remain the caller's
proof obligation. Initial-content authority, warm/low-perturbation cost,
default-tail CPU ranking and removal budget remain open; first traces include
V4 qualification and cold prefetch, not exclusive product cost.

### Normal provider diagnostic — partial qualification

R-HARN-REPLAY-9.23 is implemented in immutable V4 preparation and the private
provider. Original source-bound action/history companions, actual Normal queue
construction, retained alias transfer and local checkpoint-before-restore-before-
arm are enforced. Stable-Exact launchers and all independent content/timing
qualification flags remain unchanged.

H485 executes the original H481 input in two fresh native Metal processes.
Both restore all 16 required surfaces, prepare/activate 12 identical retained
aliases, create/release 779 wrappers, and map all three original source cuts.
Both source and output readbacks match the original 3,686,400 bytes exactly;
GPU command-buffer errors are zero. The initial attempt without the explicit
PE Present-reservation producer contract correctly fails publication matching;
the successful attempts supply the existing contract, not a relaxed comparator.

Native and x86_64 focused suites pass 5/5 each; all 237 TLA configurations meet
their expected outcomes. This qualifies the bounded H481 execution/readback
diagnostic, not every resource's first-read content authority, full-tail
representativeness, Rosetta/Wine replay parity, policy timing or CPU removal.
The private runner is retained as evidence, not a general Normal CLI.
See [H485](../../docs/perfomance/present-pacing/present-pacing-normal-provider.485.md).

H486 additionally reproduces both original readbacks in nine fresh x86_64
processes under Rosetta and authenticates the loaded provider images in a
separate diagnostic. Three original source cuts are joined to native Encode
CPU; setup sources are excluded. This closes bounded x86 replay/readback
parity, not Wine execution parity. Fresh-cache prefetch builds and intrusive
draw clocks prevent steady-state/removable-cost claims. Content authority,
representative tail input and global CPU ownership remain open. See
[H486](../../docs/perfomance/present-pacing/present-pacing-normal-source-cost.486.md).

### Normal produced-parent setup metadata — partial

R-HARN-REPLAY-9.22 is implemented as an explicit bounded identity list in
`buildRenderTapeInitialSetupV4`. Whole-source authentication, exact late
generation selection, zero-content allocation, independent capacity and
transactional failure are covered by native color/DF24 fixtures. The empty
request leaves historical setup unchanged. H484's resource-free builder gains one DF24
parent and one alias (647 owners, 12 aliases) while preserving all 5,044 seed
operations and original semantic actions. H485's private Normal provider now
supplies this selection and retains the actual wrappers; production rendering
defaults and timing qualification are unchanged. See
[H484](../../docs/perfomance/present-pacing/present-pacing-normal-produced-setup.484.md).

### Normal attachment-history restoration — partial

R-HARN-REPLAY-9.21 now has a move-only bounded plan and a cold queue settlement
primitive. Preparation closes required generation coverage and the supported
exact-handle alias relation. Settlement requires an unchanged Normal policy,
no capture, a matching local queue/published prefix, completed queue/initializer
work, no borrowed writer and live pool generations. Touch changes are staged
before an atomic swap; original discard is restored independently of seed writes.
The primitive does not publish work, wait or import captured completion labels.

Native tests exercise the real inert queue and NULL-marker surface arena, not
GPU contents. They cover alias/generation/capacity shape, local prefix and
terminal/build rejection, one-use consumption and allocation rollback. The
bounded `RenderTapeNormalHistoryRestore` model distinguishes stale-prefix and
partial-mutation bugs. These layers do not prove physical GPU aliasing, load
actions or pixels. H485 connects the private Normal provider and supplies
bounded original-versus-replay GPU evidence; the stable-Exact launcher remains
unchanged. First-read content authority, default-tail CPU ownership and
removable cost are still unqualified.

Focused tests pass 3/3 native and 4/4 x86_64 including install-name audit;
Unix and both PE builds complete. All 233 TLA configurations meet expected
outcomes, including the new model and its two negatives. See
[H482](../../docs/perfomance/present-pacing/present-pacing-normal-restore.482.md)
for exact evidence scope and the remaining provider boundary.

H483 reuses the original H481 bytes through production General/V4/initial-setup
preflight without creating a device. All admissions pass. Of 16 required
surfaces, four are prefix-resident and 12 wrappers remain lazy: ten seeded
texture aliases, one NULL RT alias and one produced DF24 alias. The produced
parent is also absent from hidden initial setup. The NULL alias is originally
touched despite having no seed. Normal integration must retain the exact late
wrappers through logical activation, prepare the missing owner without granting
contents, and handle backbuffer seeding without the Exact-only completion call.
First-read content and GPU equality remain open; see
[H483](../../docs/perfomance/present-pacing/present-pacing-normal-setup-closure.483.md).

### Normal initial attachment history — partial

R-HARN-REPLAY-9.20 specifies a bounded retained census and quiescent queue
observation. Production Normal arm now freezes generation-qualified logical
surface/storage rows, color-touched/current membership, discard and completed
watermarks. D9RTSNV4 transports this observation without changing V2/V3 layouts;
PE joins it atomically and the publisher emits a digest-bound
`attachment_history.bin`. Inspection authenticates it without granting replay
or content authority. Exact and replay arms do not mint this original history.

Native evidence covers bounded retain/no-partial-allocation, stale generation,
non-reentrant census, missing-current and nonquiescent arm rejection, reference
release, immutable arm bytes, format truth tables, transfer truncation/nesting
and allocation failures, PE ownership and publication. CLI negatives cover
component digests, companion bindings, baseline, malformed rows and unknown
content authority. The focused native/CLI suite passes (10 targets); the
x86_64 suite plus Unix install-name audit passes (11 targets). Both PE
architectures and the Unix provider build. All 230 existing TLA configurations
meet their expected safety/progress/counterexample outcomes; they remain
regression evidence, not a new content-validity or Normal replay proof.

H479 still inspects successfully as history-absent/unknown; it predates this
component and cannot retrospectively supply it. H480 attempts fresh capture
but publishes no bundle: 20 uncaptured-cycle aborts, 6 final query-transcript
failures, and 16 unlocalized identity-finish failures across 43 completed arms
(the last is interrupted by timeout). Cold stage-qualified finish diagnostics
and native wrong-history binding negatives were added afterward; they are not
retroactive cause evidence. Shared images are restored. See
[H480](../../docs/perfomance/present-pacing/present-pacing-normal-history.480.md).
H481 publishes an authenticated Normal bundle on the first bounded arm, with
21 original history rows and exact history for all 16 defined surface
generations. Shared images are restored; the owned engine is intentionally
stopped after publication. H480 failures remain unexplained, not repaired.
Offline first-access inspection exposed constant-range header and explicit-null
binding parsing defects; both are corrected with native regression coverage.
Follow-up native counterexamples also close carried texture-binding tracking
across sparse draws, including unbind, generation replacement and read/write
conflict. The same H481 input retains all 21 prior locator classifications;
this is not new content authority or GPU evidence.
The input includes draw-first/read-first cases and two current-backbuffer
wrappers, so first-access labels do not establish defined contents or physical
equivalence. See
[H481](../../docs/perfomance/present-pacing/present-pacing-normal-history-input.481.md).
Content validity, history restoration, future-alias and
physical-storage closure, and original-versus-replay GPU evidence remain
missing; Normal replay qualification remains closed. This diagnostic change
does not establish default-tail CPU ownership or a removable-cost estimate.

### Normal-action capture diagnostic — partial

R-HARN-REPLAY-9.19 adds explicit capture-only `normal` action selection using a
shared PE/Unix pure predicate; default capture remains `tape-exact`. Existing
Normal witness transfer, publication and inspection are reused and stable-Exact
qualification remains closed. No new command/identity ABI or generic GPU
snapshot format is introduced.

The selector/PE gate truth tables, real queue-construction environment cases,
action-history, transfer, binding, capture and publisher tests pass on native
ARM64 and x86_64. The CPU/GPU launcher suites retain rejection-before-child for
Normal policy qualification. Both PE architectures and the x86_64 Unix provider
build; its install-name audit passes. All 230 existing TLA configurations pass
their expected safety/progress/counterexample outcomes. These are regression
and selection/publication safety checks, not a successful game capture or a
Normal GPU replay oracle.

The H479 STALKER Day diagnostic now publishes one Normal-action frame interval:
construction/arm/freeze are Normal with revisions zero, and native event/V4
identity/topology inspection passes. The bundle has 85 PE chunks, 3,640 wire
records and three published queue sources. Source/output readbacks are retained
and digest-equal; this is original Present evidence, not replay equivalence.
The actual Normal bundle remains rejected by the stable-Exact CPU authenticator
before child execution. Both diagnostic game runs exit successfully and shared
Wine images are restored; the first run selected an unreached capture boundary.
See [H479](../../docs/perfomance/present-pacing/present-pacing-normal-capture.479.md).

Normal replay and replay-versus-original GPU validation remain unqualified.
Normal replay still needs the selected workload's initial touched/discard and
content-validity evidence; no provider replay gate has been relaxed. Same-scene
game/source identity, passive timing, and largest/removable CPU-owner attribution
are not supplied by this capture mode. Capture drains and mirror reservations
remain observable perturbations. Inspect the selected Normal workload before
expanding checkpoint coverage; use `.gputrace` for GPU-side localization, not as
a dxmt9 CPU-policy replay substitute.

### Qualification scope: cold conditional policy experiments

**Production promotion exclusion:** H447 repairs the depth/stencil publication-cut
bug exposed by H446, with reduced ordinary-policy native/Metal regression tests
and recovered observer-OFF L64 sampled geometry. Existing V4 qualification uses
the authenticated exact attachment-action profile; it is not full normal-policy
qualification. Retain the historical cold evidence, but refresh image-bound
CPU/GPU gates and performance measurements on the repaired binary before L64
promotion or equivalent-rendering FPS claims.
See [H447](../../docs/perfomance/present-pacing/present-pacing-l64-depth-publication-fix.447.md).

H448 completes one repaired-image observer-OFF wild ABBA: all image/config
receipts agree and sampled scenes are normal, but +0.80% mean FPS is smaller
than baseline drift, paired signs disagree, and p95 worsens in both pairs.
No L64 benefit or default promotion is established. Current-image fixed-input
CPU/GPU requalification remains distinct; see
[H448](../../docs/perfomance/present-pacing/present-pacing-l64-repaired-wild-abba.448.md).

R-HARN-REPLAY-8.3/8.5/8.7/8.9 admit the declared fresh-device domain with
paired original-keyed effective-field equality, original Metal query/pixel
qualification and image-bound observer-light CPU measurements. They do not
require a second raw-state/shader interpreter or same-device warm reuse.
The explicit candidate-legal-NOOVERWRITE/closed-writers assumption remains
part of that domain, not a proven property of arbitrary callers.

The stronger missing authorities below must remain false but are not blockers
for this domain: warm reset, independent complete original scalar/uniform
reconstruction, numeric occlusion on the EVENT-only input, and unconditional
future backing preservation. Observer-light CPU costs must not be relabelled
as exclusive Metal encoding or game FPS. H431--H434 already exercised paired
CPU experiments; their historical image-bound receipts do not authorize
changed-image measurements. H441 supplies current exact-fit qualification.
The subsequent final-image exact/1 MiB/5 MiB CPU/GPU paired gates pass with
identical input/plan/endpoint/runtime/launcher hashes, and exact-fit CPU A/A
passes 36 fresh executions (two warm-ups and 16 measured samples per arm).
The four final capacity comparisons also pass, totaling 180 accepted fresh
children with A/A, forward and reversed arms. Main revalidated every native
receipt and final image binding. A prior forward run with concurrent analysis
is retained but excluded; its clean replacement is used. No material capacity
advantage or retained-capacity reuse is established. Evidence:
`docs/perfomance/present-pacing/present-pacing-v4-final-capacity.442.md`.

Final-image grouping refresh is complete: new L3/L64 paired CPU/GPU gates and
L0 A/A plus both orders of L0/L3 and L0/L64 pass 180 fresh observer-light CPU
executions. Native receipt/qualification/input/image revalidation passes for
every row. H434 remains older-image history; it was not combined with H441
GPU-only diagnostics to fabricate current CPU qualification. Together with
capacity this closes the bounded cold matrix on the current image, without
promoting stronger authorities or runtime defaults. Evidence:
`docs/perfomance/present-pacing/present-pacing-v4-final-grouping.443.md`.

### Immutable V4 preparation reuse

Implemented and boundedly qualified: the eager V4 factory owns authenticated
input and stationary source/setup/execution templates; named replay creates
fresh query/cursor/completion state. Owner-move, template-isolation, malformed
input and intercepted pre-effect allocation-failure tests pass. Native 8/8,
x86 9/9, 230 existing TLA configurations, both-route paired CPU/GPU checks and
54 fresh-device L0/L3/L64 executions pass original pixels/EVENT readiness.
Every execution records one immutable preparation and one consumed fresh
admission. Intercepted allocation tests do not cover every aligned/runtime
allocation or post-arm OOM interleaving.

About 280 ms of immutable work is now one-off; fresh initialization is about
0.015 ms. Whole preparation and final owner destruction must not be mistaken
for free work: final destruction is outside the per-call clock. These are
harness service costs, not a production FPS claim. Warm reset, full semantic
oracle, future backing and numeric occlusion remain open. Evidence:
`docs/perfomance/present-pacing/present-pacing-v4-immutable-preparation.441.md`.

### Prepared V4 single-admission ownership

Implemented for one invocation: stable caller-owned result and stack executor
replace two pre-effect admissions with one bound pre-arm claim. The production
predicate's 1,024-state domain and actual-owner/capacity negatives pass, as do
five native/six x86 tests and all 230 existing TLA configurations. Final-image
Compatibility/ProductionSlot paired qualification and 54 Compatibility
L0/L3/L64 GPU repetitions preserve original pixels/EVENT readiness. Every GPU
row records admission_count=1 and consumed handoff. The bounded claim proof
does not establish concurrent public input mutation or warm reset; the paired
oracle still does not close complete scalar/uniform semantics, future backing
preservation or numeric occlusion. In that slice outer V4 planning remained uncached;
changed owner-destruction timing prevents inferring a full-provider speedup
from replay wall alone. Evidence:
`docs/perfomance/present-pacing/present-pacing-v4-single-admission.439.md`.

### V4 in-process reset boundary

H541's private direct same-device reuse probe violated the named V4 API's
fresh-device precondition: first invocation matched original pixels, second
returned `output-mismatch` despite matching query results and wrapper counts.
No warm timings were collected. This is caller misuse, not qualified warm
support or a product regression; retain the reset boundary below. Evidence:
`docs/perfomance/present-pacing/present-pacing-encode-source-selection.541.md`.

Shared-prepared-input fresh-device repetitions are implemented and bounded.
Capture29 L0/L3/L64 each pass two warm-ups plus 16 actual-Metal executions with
original pixels and EVENT readiness; omitted-control single execution also
passes. This closes repeated input preparation, not warm-device qualification
or exclusive cost attribution. See
`docs/perfomance/present-pacing/present-pacing-v4-inprocess-repetitions.436.md`.

Prepared V4 execution explicitly rejects `WarmDevice` before effects. Retaining
the immutable prepared input across fresh-device iterations does not relax
that gate. Existing `resetPreparedDevice` checks deferred-replay health and an
empty wire-object registry around Reset; `resetVerified` alone is not a
complete mutable-state or Pool-pin oracle. Core Reset retains the upper
queue/cache identity and some effective controls, including gamma and maximum
latency. Bootstrap gamma restoration is conditional on captured bytes.
Warm admission therefore needs preserved-control closure and a genuine V4
changed-state/seeded-resource A-to-B-to-A test with original pixels, queries,
query epoch and retirement evidence. Existing ordinary Chunk-mode warm Metal
tests do not discharge that requirement. Terminal queue/offload failure must
reject the entire experiment, not trigger warm recovery. Policy environment
selectors are process-cached; mixed-policy repetitions need an explicit tested
per-queue policy interface, not `setenv` between iterations.

### Actual-Metal diagnostic timing (2026-10-04)

The diagnostic caller now clocks the whole prepared-provider return/assignment
expression, including provider-local destruction. Native five/x86 six tests
pass; all 54 fresh-device L0/L3/L64 executions preserve original pixels/EVENT
readiness with zero GPU errors. Full-call medians are 569/544/538 ms, versus
about 280 ms outer preparation. This establishes a current inclusive baseline,
not a prior-version speedup or exclusive CPU attribution. The optional schema
is strict, nullable and per-execution, with no repeat-root aggregate. Evidence:
`docs/perfomance/present-pacing/present-pacing-v4-provider-call-timing.440.md`.

Prepared-only outer/inner admission, pre-arm checkpoint and finish scopes are
implemented with strict optional diagnostic export. On capture29, outer
plan/admission is about 282 ms and inner admission about 69 ms across all three
policies; checkpoint/finish are roughly 1.4/0.5--0.9 ms. All 54 actual executions
preserve original pixels/EVENT readiness. Duplicate admission is a measured
harness cost but does not explain the grouping gap. The single-admission
follow-up above removes the inner pass; H441 subsequently implements immutable
derived-plan reuse. Pre-change attribution evidence:
`docs/perfomance/present-pacing/present-pacing-v4-admission-timing.438.md`.

Existing query setup/transcript/oracle clocks now have strict optional native
and wrapper export, including individually validated repeat rows. Capture29
L0/L3/L64 pass 54 actual executions with original pixels/EVENT readiness; query
setup medians of 7.3--7.7 ms and captured GetData near 0.0024 ms do not explain
the grouping replay-wall gap. Admission, pre-arm checkpoint and final semantic
verification lacked separate attribution in that slice; the subsequent phase
measurements above address those scopes. Evidence:
`docs/perfomance/present-pacing/present-pacing-v4-query-timing.437.md`.

R-HARN-REPLAY-9.5 is partial: the private diagnostic exports existing prepared
elapsed phases and inclusive runtime snapshot deltas without adding hot-path
timers. Missing exclusive planning/encoder/GPU/wait dimensions remain null;
diagnostic timing does not grant policy or timing eligibility. Native and
wrapper schema validation preserve original pixel/query gates. Four unchanged
capture29 L0/L3/L64/L0 executions pass original pixels and EVENT readiness on
identical images. Identical L0 encodeChunk changes from 683.707 to 11.532 ms;
first-run pipeline costs prevent policy ranking. Prepared-input setup takes
4.25--4.30 seconds per invocation. Safe in-process reset/warm-cache qualification
under R-HARN-REPLAY-8.3, repeated balanced comparisons, exclusive cost attribution
and live/numeric-occlusion coverage remain open. Evidence:
`docs/perfomance/present-pacing/present-pacing-v4-metal-timing.435.md`.

### Independent bounded grouping integration (2026-10-03)

R-HARN-REPLAY-8.8 now has a versioned native route/grouping/capacity identity
bound through CPU/GPU wrappers and both aggregate paths. Native arm64 tests
pass 5/5, x86_64 tests 6/6, launcher/aggregate targets 6/6 and the full TLA
inventory 230 configurations. The real grouping-plus-EVENT-END fixture changes
L0/L64 three sources to L3 four sources while preserving logical/query digests,
four pre-END records and real retirement under Observe and ExecutionOnly.
Unchanged capture29 L3 CPU smoke consumes 172 sources and 512 commands while
preserving 2,799 draws and 2,806 logical items. Fresh final-image original
CPU/GPU paired qualification passes L0 and L64, with original pixels and EVENT
readiness in both routes and two fresh GPU executions per route. L3 fails
original exact pixels twice with the same output digest: 257 of 921,600 pixels
differ (maximum channel delta 14), despite valid logical mapping, EVENT
readiness and zero GPU errors. L3 is excluded from performance comparison;
diagnosis remains open. Qualified L0/L64 comparisons in both orders plus L0
A/A pass 108 executions, with 2.292%/2.356% lower cold CPU-body time for L64.
This does not measure Metal encoding or game FPS. See
`docs/perfomance/present-pacing/present-pacing-v4-grouping-comparison.433.md`.
The completed cold capacity matrix below does not discharge those gates.
Evidence: `experiments/output/stkcop-v4-grouping-15/verification.md`.

Natural-pass RT0 diagnostics retain the L3 failure and localize the first
differing common endpoint to original draw 2741 (event5570/record2), after an
equal endpoint at draw2597 (event5297/record1). The 144-draw interval spans
one L0 pass versus 19 L3 passes; RT0 is A8R8G8B8, not RGBA16Float. Earlier
matching RT0 endpoints do not certify unobserved depth/stencil or MRT content.
A standalone immutable color-roundtrip fixture passes native arm64 and
x86_64 with exact RGBA8/RGBA16Float additive/source-over split outputs.
It uses safe shader math and is not a production-scene proof. L3 remains
rejected; natural-boundary/state-content diagnosis is open. Evidence:
`experiments/output/stkcop-v4-grouping-16/verification.md`.

Actual issued shader/PSO lineage identifies three live fog-binding mismatches
inside that interval. The direct binding predicate lacks canonical hot-state
invalidation despite FFPPS/FFPVS consuming RS/TSS/viewport inputs outside the
fixed payload hash. Source/segment/row-qualified invalidation now passes four
focused native/Metal tests and fresh paired L0/L3/L64 original CPU/query/GPU
qualification (`experiments/output/stkcop-v4-hot-row-17/verification.md`).
Original pixels are unchanged. x86 focused tests/audit pass 5/5 and all 230 TLA
configurations pass. Corrected-image A/A and forward/reverse L3/L64 comparisons
pass 180 CPU executions. L3 costs 9.207%/10.543% more CPU than L0; L64 costs
2.820%/2.522% less, versus -0.039% A/A drift. This qualifies only the bounded
CPU experiment: actual encode-cost impact, live promotion, numeric occlusion
and broader capture coverage remain open.
Previous pixel failures and measurements are historical
evidence, retained unchanged rather than reused as current qualification.

### Capacity identity integration (2026-10-03)

The V4 CPU and GPU native CLIs now share bounded capacity admission and pin
both existing headroom selectors before device creation. Exact-fit is explicit
zero/zero; positive capacity is ProductionSlot-only. Native arm64 and x86_64
builds pass, with admission/policy/ownership checks on arm64 and
admission/ownership/install-name checks on x86_64 (3 tests each). All five Python
CPU/GPU/paired/experiment launcher targets pass with route-bound policy and
actual-stage validation. A capture29 execution-only 5 MiB/64 MiB aggregate smoke
passes with 648 Direct draws/57 leases plus 2,151 compatibility draws/23 batches,
three consumed sources and 512 commands. Evidence:
`experiments/output/stkcop-v4-capacity-14/verification.md`. Fresh same-image
exact-fit, 1 MiB and 5 MiB paired CPU/original-GPU qualifications now pass,
including final readback authentication. Direct draws are respectively
534, 781 and 648 with unchanged 2,799 total draws and zero GPU errors. This
does not establish a performance ranking. Process-wide reports show one
provision per positive candidate and no reuse/aggregate denial. The standalone
cross-qualification comparator passes 11 tests and all five cold comparisons
(A/A plus exact-fit versus 1/5 MiB in both orders), totaling 180 accepted
executions with final reauthentication. No material CPU advantage is established.
Independent bounded grouping identity and changed-partition query/pixel
qualification remain open under R-HARN-REPLAY-8.8; see the performance record
`docs/perfomance/present-pacing/present-pacing-v4-capacity-comparison.432.md`.
No production default
or generic partition policy has changed. Prior fixed-input measurements below
remain historical evidence for their recorded binaries, not authority for the
new native images.

### Public bounded V4 qualification and execution-only lane (2026-10-03)

`v4-paired-qualify` now owns one unchanged V4 input and couples CPU
Compatibility/ProductionSlot logical comparison with two fresh original
query/source/output GPU checks per route. Capture29 passes all six execution
children and final input/script/executable/runtime/readback authentication;
2,806 ordered logical items match, covering 2,799 draws. Exact original query
readiness passes, but this capture has no numeric occlusion comparisons.
Native and wrapper artifacts keep their separate authority domains.

The same native V4 driver now supports `ExecutionOnly`, omitting diagnostic
contexts/images/canonical copies and per-draw byte/hash observation while
preserving actual uploads, provenance, query, ACK/join/reset and route coverage.
Its original capture29 smoke passes with 534 Direct plus 2,265 compatibility
draws. Native regression passes 192 tests; x86_64 execute/route/executor and
install-name checks pass 4 tests. Evidence:
`experiments/output/stkcop-v4-controlled-cpu-13/verification.md`.

The paired command qualifies only its declared fixed-input route comparison,
not arbitrary policy correctness or timing. `v4-experiment` binds original-work
counts/provenance and timeout evidence to the qualified pair and reauthenticates
on completion. Its final 24 mocked regressions pass. Capture29 A/A and
forward/reverse comparisons accept 108 fresh executions: two warmups and
16 measured samples per arm per comparison. This closes the current fixed-input
route measurement gate, not Metal encode timing or game FPS. Actual alternate
grouping/capacity policy qualification remains outstanding; capacity identity
must bind both CPU and GPU qualification before sampling. Historical private
diagnostic gates below remain unchanged for their original evidence scope.

### Private semantic correspondence evidence (2026-10-03)

R-HARN-REPLAY-9.16 now has private SemanticMapping execution and an immutable
owned many-to-many origin map. All four canonical builds, 154/154 native
selected tests, 18/18 x86 focused tests, and the existing full TLA inventory
pass. The production mapper passes 7,381 legal partition pairs and 768 END
prefix perturbations; these finite checks do not prove arbitrary scheduling.

On unchanged capture29, fresh-process Metal-validated exact ProductionSlot,
semantic ProductionSlot and semantic compatibility diagnostics pass. Both
semantic runs match all 3,686,400 bytes of each original source/output oracle,
independently checked with `cmp`. ProductionSlot executes 534 Direct / 2,265
compatibility draws; compatibility executes all 2,799 draws in 80 batches,
with no Direct leases. Both map all 3,513 records, 87 raw ACKs and three source
obligations, release all 738 objects, and report zero GPU errors. EVENT
readiness passes; no numeric occlusion comparison is implied.

The actual routes differ, but both produce three sources and 512 commands;
neither changes command grouping on this input. Actual alternate-partition
GPU evidence remains missing. Capture28 semantic execution correctly rejects
before native execution because its historical action witness is absent.
Public CPU V4 execution, paired repetitions and timing acceptance remain
closed. Evidence: `experiments/output/stkcop-v4-semantic-mapping-01/verification.md`
and `experiments/output/stkcop-v4-capture29-mapping-{exact,production,compatibility}-01/`.

### Current V4 original-input evidence (2026-10-03)

Fresh capture29 now closes the production action-witness evidence gate.
The authenticated 200-byte component reports queue-observed exact
construction/arm/freeze and zero arm/freeze revisions. The unchanged original
input also passes one fresh-process private ProductionSlot exact diagnostic
with Metal validation: source/output each match all 3,686,400 original bytes,
SHA-256 `2c616e5aa12d5a0d03f4fb2229809ee3f4b360b243f7aae05a5016d4f1a67113`.
Its 5,730 events / 3,513 records yield 87 raw ACKs, three actual sources,
512 commands, 534 Direct and 2,265 compatibility draws; all 738 objects are
released and GPU errors are zero. EVENT readiness passes 1/1; there are no
occlusion queries or numeric result comparisons. This is not a timing run or
a policy-qualified candidate, nor an A/B comparison with different capture28.
Evidence: `experiments/output/stkcop-v4-snapshot-budget-01/verification.md`,
`experiments/output/stkcop-v4-capture29-snapshot-budget-diagnostic-01/`, and
`traces/app-d3d9-stkcop-bench-tape-h430-query-volume-29/tape/frame-1158897097572700-1/`.

Capture28 now passes original-input private V4 qualification twice in fresh
processes with Metal API validation enabled. Both source and output readbacks
are byte-identical to their respective captured 3,686,400-byte oracles, SHA-256
`7a1c7f21f430728954c12100390fd3289e726a7017aa4370bcbdf74f1cb091d3`.
The fixed PE producer explicitly tags bootstrap/final RT0
`(surface, generation1, objectId4294967297)` and Present source
`(surface, generation4, objectId4294968987)` as primary-backbuffer aliases.
Their arm seed is different from the terminal oracle; replay no longer returns
the untouched arm image. Capture27 is preserved as the missing-alias
counterexample, not rewritten.

The genuine Sikarugir Day/high/1280x720 run uses skip 1200, no CPU-ready/session
or route override, no worker/audio tuning, and passes with no runner failures.
The staged PE32/PE64/Unix hashes and successful ABI handshake are recorded.
Capture28 has 5,261 events, 4,260 blobs, 81 raw attempts/acknowledgments,
3,426 records, 472 physical commands and three actual sources with extents
1/470/1. Both diagnostics pass canonical traversal, raw coverage, actual
lifecycle/topology and the exact original publication cuts. Each executes
584 Direct lease draws and 2,165 compatibility draws, releases all 686 created
objects and reports zero GPU command-buffer errors. EVENT readiness passes
without substituted results or supplemental polls/drains. The input has two
EVENT definitions, one END and one poll; it proves no occlusion sample count.

The private `pe-present-reservation-v1` producer contract is explicitly selected
in both receipts. This is not policy-neutral public execution: `qualified`,
`source_exact`, `output_exact` and `query_qualified` are true, while
`policy_qualified` and `timing_eligible` remain false. Same-input policy
qualification and warmup/repetition gates still require implementation; two
fresh-process diagnostic passes are not the requested timing experiment.

Capture evidence:
`experiments/output/app-d3d9-stkcop-bench-tape-h430-query-volume-28/` and
`traces/app-d3d9-stkcop-bench-tape-h430-query-volume-28/tape/frame-1153494769614600-1/`.
Qualified readbacks and receipts:
`experiments/output/stkcop-v4-capture28-exact-diagnostic-01/` and
`experiments/output/stkcop-v4-capture28-exact-diagnostic-02/`.
Fifty-one five-second process samples observed peak RSS 2,659,104 KiB;
first-arm scratch highwater is 754,337,942 bytes. Sampling is not a hard RSS
bound and scratch/RSS are distinct accounting domains, not additive costs.

The next blocking policy obligations are concrete, not promotion flags:

- R-HARN-REPLAY-9.15 GPU arm payload budgeting is implemented and locally
  verified: retained credit precedes PE allocation, separate conservative 2N
  staging credit precedes the bridge, and exact-allocation receipts follow
  consuming bootstrap/JIT adoption into immutable storage. Move-only arm
  snapshots and checked capture-input copies prevent implicit leased deep
  copies; populated assignment retires bytes before returning credit. The
  provider independently enforces the per-resource ceiling before its own
  materialization/readback. Four canonical builds, 153/153 native selected
  tests, 15/15 x86 focused tests and the existing full TLA inventory pass.
  Exhaustive native reducers and allocation/lifetime fault tests bind the new
  budget contract. Evidence:
  `experiments/output/stkcop-v4-snapshot-budget-01/verification.md`.
  The unchanged capture28 source/output remain byte-exact with zero GPU errors
  in `experiments/output/stkcop-v4-capture28-snapshot-budget-diagnostic-01/`.
  This is not a total RSS bound: CPU scratch quotas, late CPU temporaries,
  Present mirror/oracle storage, allocator overhead and Metal storage remain
  separate domains. Fresh capture29 also publishes successfully under the new
  admission contract; successful admission is not a measured peak/RSS bound.
- R-HARN-REPLAY-9.14 capture-action provenance is implemented and locally
  verified: the queue owns construction/current profile and monotonic actual
  change history; the keyed arm/freeze binding freezes one witness and Present
  ordinal; explicit snapshot transport V3 and the authenticated 200-byte
  `action_profile.bin` publish it atomically with the V4 pair. ABA, overflow,
  revision parity, stale keys, frozen retries, allocation rollback and manifest
  contradictions have native/CLI coverage. All four canonical builds, 149/149
  native selected tests, 11/11 x86 focused tests and the complete existing TLA
  inventory pass. Evidence is in
  `experiments/output/stkcop-v4-action-provenance-01/verification.md`.
  Fresh production capture29 now carries and authenticates this witness;
  capture28 has no historical witness and cannot acquire one from a post-hoc
  manifest edit. Its unchanged original source/output readbacks remain exact
  after this change in
  `experiments/output/stkcop-v4-capture28-action-provenance-diagnostic-01/`:
  81 ACKs, 472 commands, 584 Direct and 2,165 compatibility draws, no GPU errors.
  Loader evidence is explicitly `legacy-unknown`, and policy/timing gates
  remain closed.
- Private V4 SemanticMapping now has an owned canonical origin mapper and
  deferred publication obligations (R-HARN-REPLAY-9.16). It accepts source and
  DrawRun splits/merges without forcing original cuts, validates complete real
  candidate lifecycle/ACK evidence, and checks the whole pre-END semantic
  prefix. Admission rejects unsupported reference prefixes before effects.
  Interval initializers and differing coordinator-command expansion remain
  unsupported. Native integration and private original-input Metal validation
  pass as recorded above; public policy/timing acceptance remains closed.
- CPU General grammar and executor now share a value-owned input budget with
  separate publication/progress capacities (R-HARN-REPLAY-9.17). Explicit
  extended admission allows 8,192 events, 1,024 objects, 8,192 mutations and
  512 MiB blob bytes; it does not enable V4 execution or raise source capacity.
  Defaults preserve historical body/bootstrap record and aggregate chunk
  bounds, but the new 2,048-blob metadata cap is an explicit additional limit.
  Large-input fixture acceptance is admission evidence only, not execution.
- Public V4 CPU/Metal paired qualification and low-observer repetition are not
  connected. Two warmups plus 32 measured iterations exceed the current CPU
  wrapper's total-32 bound, even though the Metal total-64 bound admits them.
  Phase attribution and the independent EVENT/occlusion evidence limits remain.
- CPU V4 candidate qualification requires the distinct controlled-queue domain
  in R-HARN-REPLAY-9.18. The existing CPU consumer submits null-command-buffer
  test records through the queue lifecycle but performs no Encode invocation;
  it cannot pass the production V4 topology-complete finish. Typed CPU-only
  finalization and the bounded consumer now have native domain/reducer tests;
  both targets pass 32 repeated runs. The dedicated wake/exit/join model passes
  2,316 states and a missing-publication-wake temporal counterexample, within
  the complete 230-configuration TLC gate. See
  `experiments/output/stkcop-v4-controlled-cpu-01/verification.md`.
  Initial setup now passes 496 host ownership/seed checks. The real-queue
  adapter passes natural publication beyond resident capacity, exact receipt
  identity, unlocked metadata observation, empty wake/join and failure-phase
  quarantine tests. A separate authenticated General integration now passes
  actual setup, V4 arm, observed Compatibility replay, natural Present
  publication, join and CPU-only finalization (32/32 repetitions). Its two
  reference source obligations map to one genuine candidate source without
  forcing a captured cut; missing raw ACK or join acknowledgment rejects.
  Evidence: `experiments/output/stkcop-v4-controlled-cpu-02/verification.md`.
  ImmediateFifo explicitly rejects strict SegmentSerial groups requiring joint
  retirement. Original-input setup and whole-input execution remain open; no unchanged
  capture29 CPU acceptance or policy timing is claimed. Fabricated topology
  is not an acceptable bridge to public policy qualification.
  The dedicated consumer must also obtain immutable state/uniform/resource
  observations: the legacy CPU digest reads mutable executor tables and cannot
  simply move onto a concurrent consumer thread. Source identity, command-kind
  and draw counts alone are insufficient for full CPU semantic qualification.
  Serial resource normalization now produces an owned identity/alias/receipt/
  D24S8 observation and a shared exact-field-stream emitter (57 focused checks;
  selected native regression 159/159). Wrapper and retired-parent qualification
  remain producer-owned. Canonical record/subdraw context binding, immutable
  expected buffer versions and publication-qualified origin pulls now pass
  focused native tests. The unchanged no-later-mutation fixture passes the
  independent serial digest comparison, actual backing proof after wrapper
  destruction, five semantic negatives and 32 repeated integration runs. These
  facts do not qualify concurrent in-place writes or original capture29.
  Evidence: `experiments/output/stkcop-v4-controlled-cpu-03/verification.md`.
  Fresh unchanged capture29 inspection in
  `experiments/output/stkcop-v4-controlled-cpu-03/capture29-admission-summary.json`
  finds 15 body raws above the existing 64-record CPU profile limit, with a
  maximum of 256, and 169 NOOVERWRITE buffer unlocks. Original-input admission
  now passes the distinct V4 raw256 input gate, including its original gamma:
  `experiments/output/stkcop-v4-controlled-cpu-05/capture29-input-r1.json`
  records 5,730 events, 3,513 body records, 2,799 draws and 4,644 blobs with the
  unchanged events hash. Legacy extended64 still rejects event 1695/chunk 2.
  This is input evidence only; separate source-progress/capacity and actual
  execution admission remain unproven. The production range upload
  mutates NOOVERWRITE backing in place; a retained source/backing token does
  not freeze its entire byte extent. A record-time whole-buffer expected image
  cannot simply be compared to a later whole backing after legal disjoint
  writes. The immutable-context hookup must preserve logical stream evidence
  while separately establishing a sound physical-byte proof for these epochs.
  The arena-locked per-buffer footprint primitive and shared encoder count/
  index-extent predicates now pass focused native tests and 32 repetitions
  each (`experiments/output/stkcop-v4-controlled-cpu-04/verification.md`).
  They do not derive the actual draw read set or establish cross-buffer atomic
  observation. Full qualification still requires those contracts and checked
  fetch addresses; snapshot revision alone cannot cache physical proof.
  Neither overlapping upload ranges nor a full final-backing timeline alone
  establish preservation of historical draw reads. Original-input semantic
  and timing qualification remain open.
  A cold arena-locked current revision/backing-value API now covers nonversioned
  buffers. Aggregate immutable expected-byte ownership has a distinct budgeted
  factory with shared-owner release and allocation rollback tests. Original V4
  executor now binds those facts with one exact session budget for every
  version; legacy factories remain per-version only. Neither
  helper grants provenance, source progress, physical byte or GPU authority.
  The distinct controlled V4 inventory executor and cold CLI now compile.
  A grammar-valid 256-record native stress raw, with an explicit natural
  command-limit policy, passes real-root origin observation/retirement before
  raw ACK, resident queue pressure, and consumer-failure poison/wake (32/32).
  This is a progress-domain test, not original-input acceptance or cut parity.
  Unchanged capture29 prepares all 608 hidden owners and 4,729 seeds.
  Distinct captured primary-backbuffer identities sharing one native surface
  are now preserved by an explicit metadata-only inventory purpose, with
  ambiguous native lookup rejected and strict semantic authority unchanged.
  Context/ledger tests passed 64 repeated executions; focused integration
  passed 7/7. The authenticated PE Present preparation now uses the genuine
  queue flush under a live controlled capability and dedicated consumer;
  selected/unselected executor cases passed 32 repetitions.
  Unchanged capture29's fourth attempt completes metadata inventory: 5,730
  events, 2,799 draws, 87 raw acknowledgements, 3 sources / 512 commands,
  3,513 immutable record contexts, and all 738 created wrappers released.
  All sources submit/acknowledge/retire, query transcript traversal completes,
  and final cleanup/reset succeeds. Expected-buffer high water is 189,950,852
  bytes under its explicit 512 MiB budget, not process RSS. This is controlled
  CPU metadata evidence only: physical fetch semantics, actual Encode route
  coverage, coupled A/B mutation oracle and GPU/policy qualification remain open.
  Preparation-publication failure now has a targeted native poison/wake case:
  capacity rejection prevents the original Present, joins the consumer, and
  reports the unclaimed Ready source as quarantined rather than retired.
  Immutable versions now own their original optional backing observations.
  A bounded multi-buffer primitive compares all qualified IB/VB ranges under
  one arena shared lock; a cold source-row planner shares production scalar
  count/address formulas. Focused tests pass 9/9 and context/ledger/proof/
  executor repetition passes 128/128. The source observer now joins original
  record contexts and actual payload bindings under its genuine synchronous
  borrow, before submit/ACK. Unchanged capture29 qualifies scoped IB/VB row
  equality for all 2,799 draws with zero rejection and 26,623,246 aggregate
  compared bytes; original metadata traversal and cleanup/reset still pass.
  New helper/executor tests pass 64 repeated executions and focused tests 6/6.
  Conservative whole-stride rows are not exact shader demand, and instantaneous
  equality does not prove future Encode/GPU preservation. Actual native draw
  location must still join successful expansion/staging storage and Metal issue
  before coupled GPU/policy/timing qualification. Independent current native
  Metal ProductionSlot and compatibility runs both retain exact original
  source/output pixels and query results with zero GPU errors; this does not
  connect their executed transient geometry to CPU row evidence. Evidence:
  `experiments/output/stkcop-v4-controlled-cpu-08/verification.md`.
  The optional serial Encode recorder now joins all 2,799 actual draw issues
  to authenticated original records on both natural ProductionSlot and
  compatibility executions of unchanged capture29. Original query and exact
  source/output pixels pass; observer OFF/ON production route counters agree.
  Actual upload/reuse observations are zero in both routes, so this workload
  does not exercise the new accepted-Shared-destination comparison. That helper
  instead has real-buffer mutation/bounds/storage tests. Direct native stream
  bindings were not covered by the upload-only selection receipts. Serial-only
  admission rejects cached
  explicit-parallel policy before recorder attachment. No coupled semantic,
  policy, timing or future-preservation gate is promoted. Evidence:
  `experiments/output/stkcop-v4-controlled-cpu-09/verification.md`.
  The actual serial Encode observer now derives canonical BUFFER images from
  the authenticated original blobs in that same Metal execution, binds 3,513
  immutable original-record contexts, and joins selected backing snapshots,
  actual native VB/IB bindings and slot-5 address constants. Both unchanged
  capture29 natural routes qualify all 2,799 draw source-row footprints under
  the existing coherent arena-locked proof: 26,623,246 compared bytes each,
  zero unsupported/rejected draws. Original query/source/output gates still
  pass; independently audited original-keyed versions/addresses/statuses agree
  between routes, and ProductionSlot observation OFF/ON route counters agree.
  Setup fragments are explicitly outside the armed body; out-of-body draws
  and missing armed-source origins still reject. Native actual-Shared-buffer
  wrong-byte/address/stale-backing negatives pass. This closes the selected
  direct-native instantaneous BUFFER evidence gap, not full shader fetch,
  future GPU preservation or coupled CPU-A/GPU-B mutation qualification.
  Public semantic/policy/timing qualification remains open: the controlled
  session still needs an original-keyed state/uniform/resource logical stream,
  exact RT/DS/Present alias-role provenance, paired candidate execution and an
  observer-light measurement path. A bounded cold-device domain may explicitly
  rely on legal NOOVERWRITE preserving each candidate's unretired readers and
  exclude external mapped/GPU buffer writers, as permitted by the replay spec.
  Exact minimal shader-demand extraction and unconditional future preservation
  are stronger research authorities, not prerequisites for that explicitly
  conditional domain. Neither conditional admission nor current byte receipts
  alone qualifies a policy pair or timing result.
  Evidence: `experiments/output/stkcop-v4-controlled-cpu-10/verification.md`.
  A separate controlled logical-stream diagnostic now serializes the actual
  represented state/uniform/resource fields with original record keys, exact
  canonical RT/DS/Present role attribution and logical BUFFER epochs. It
  validates observed order without sorting away reorders; metadata inventory
  authority is unchanged. The versioned projection excludes redundant uniform
  cache hashes and first-run alpha state while preserving effective values.
  Native rebatching/value-change/alias/bounds/negative tests and 187 regression
  tests pass. Two authenticated unchanged capture29 executions produce identical
  complete receipts: 2,806 original items, 2,799 qualified draw byte checks,
  3 sources and 512 physical commands. This accepts diagnostic stream coverage
  only, conditional on explicit candidate-legal-NOOVERWRITE/closed writers;
  original/cross-policy semantic equality and all GPU/policy/timing authorities
  remain false in individual collection receipts. Explicit controlled
  Compatibility and ProductionSlot executions now independently match all
  2,806 ordered original item fingerprints on the same authenticated capture29
  and loaded images. Actual body coverage is Compatibility 2,799 draws/80
  batches versus ProductionSlot 534 Direct draws/54 leases plus 2,265
  compatibility draws/26 batches. An all-fallback candidate is rejected by
  the CPU launcher. Native regression passes 188 tests; x86_64 focused and
  install-name checks pass 8 tests. Public coupled CPU/GPU qualification and
  low-observer measurement remain unimplemented; metadata mode still performs
  context and byte observation and is not a timing substitute. Evidence:
  `experiments/output/stkcop-v4-controlled-cpu-11/verification.md` and
  `experiments/output/stkcop-v4-controlled-cpu-12/verification.md`.
  The observed semantic-mapping GPU launcher now also rejects an all-fallback
  ProductionSlot candidate (22 mocked tests). Fresh real-Metal executions of
  both routes reproduce exact original source/output bytes and the recorded
  query readiness obligation, with zero GPU errors. Main independently
  compared all four readback files; public aggregation still needs automated
  readback and final input/executable/loaded-image authentication. This capture
  requires no numeric query result comparisons and does not establish that
  broader coverage.
- The CPU-A/GPU-B authority split and 64 KiB scratch-window transactions are
  implemented under R-HARN-REPLAY-9.12, but a broader peak-memory bound is not
  established. GPU-B retained and transient staging reservations now precede
  allocation and follow immutable ownership through release, as recorded above.
  This does not bound total process RSS, driver memory, CPU replay temporaries
  or Present mirror/oracle storage. Equality deduplication grants no advance
  allocation credit.
  Resource-sized staging is allowed by 9.12, so streaming GPU readback is not
  itself mandatory. A coupled production A / alias-written GPU B / partial CPU
  unlock / A-plus-patch test remains stronger missing evidence beyond the
  existing compositional tests. Capture28's `unique_complete=0` also precludes
  claiming a complete unique-backing memory ledger.

### Capture27 counterexample and earlier diagnostic history

Capture27 publishes a genuine STALKER Day/high/1280x720 V4 identity and
execution-topology pair without a route or CPU-ready session override.
Independent bundle validation passes: 5,850 events, one Present, 84 actual
attempts and settlements, 560 commands, and three completed sources
(3103/3104/3105, command counts 1/558/1). One ordinary-domain query END retains
its actual issue cycle 586 and source prefix 3103. The DF24 Produced proof
passes final publication under R-HARN-REPLAY-7.37.

Evidence is retained in
`experiments/output/app-d3d9-stkcop-bench-tape-h430-query-volume-27/` and
`traces/app-d3d9-stkcop-bench-tape-h430-query-volume-27/tape/frame-1136429092790700-1/`.
The runner reports pass with no failures; this is capture success, not replay
qualification. The public V4 gate remains inspection-only. Original-input
CPU traversal, raw acknowledgments, lifecycle/topology and EVENT readiness now
pass in diagnostic09 below. Replayed pixels fail: missing captured primary
backbuffer alias authority leaves the output at its arm seed. Occlusion numeric
results and same-input policy timings remain unqualified. The alias correction
requires a genuine recapture, not mutation of capture27. Historical failed
capture evidence below is not the current publication status.

The private single-run diagnostic and authenticated launcher now build on ARM
and x86_64. Its six CPU-only launcher tests and four V4 admission/execution/
public-gate regressions pass. The first x86_64/Metal-validation replay of the
unchanged capture27 passes canonical, General and paired-plan admission with
plan digest `f3b4f5efdc09f4dbe6c9a889677e655d125c33a8ea8a89fdd0f615f2b0144a30`.
It rejects at event 11 with `v4-local-arm-failed`, after eleven setup events,
two completed EVENT-query checkpoints and before any raw/draw execution.
Evidence: `experiments/output/stkcop-v4-capture27-exact-diagnostic-01/`.
There are two EVENT definitions, one END and one polling call in this input,
not an occlusion-sample workload oracle. Zero GPU errors before this rejection
does not establish original-input execution or pixels. The exact pre-arm
initializer/queue rejection predicate remains under investigation; the
diagnostic must not weaken admission or force publication cuts to pass.
The second unchanged-input x86_64 run independently identifies a pending
initializer as a rejecting arm condition: ordinary published/completed 1/1,
inflight zero, quiescent builders, empty writer and nonterminal queue, but
`initializer_pending=true`. Arm returns `D3DERR_NOTAVAILABLE`/FAILED. The
read-only inspection intentionally reports unavailable initializer progress as
null, not zero completion authority. Evidence:
`experiments/output/stkcop-v4-capture27-exact-diagnostic-02/`.
The setup prefix contains a 7,372,800-byte texture seed after EVENT checkpoint
completion. V4 setup must use the existing checked checkpoint before arm;
original-input execution and pixel gates remain open until rerun.
V4 now uses that setup-only checkpoint and releases its lock before arm; V3
and interval replay paths are unchanged. The third identical-input x86_64 run
confirms `initializer_pending=false` and successful V4 arm. It reaches event
index 23 (ordinal 24, query END) and rejects with
`v4-observation-qualification-rejected`: 23 events traversed, one raw/three
records acknowledged, no draws or pixel readbacks. Actual partial evidence has
one source, initializer, command and accepted placement, but no coordinator;
these partial counts are not full lifecycle/query qualification. The retained
first-failure trace identifies `RenderTapeRuntimeIdentityCore::queryIssue` at
event ordinal 24/raw 3/attempt 2/record 0/type 26. The no-explicit-cycle branch
rejects its local ordinal continuity check, not source-cut/topology equality.
The replay's real pre-arm EVENT checkpoint and the first in-arm query need an
authenticated baseline/cycle join; captured cycle labels cannot substitute for
local query completion authority. Evidence:
`experiments/output/stkcop-v4-capture27-exact-diagnostic-03/`.
ARM and x86_64 full builds pass; all 90 serialized native Tape/query/routing
regressions pass, including the six launcher cases and new no-publication/
failed-arm/setup-boundary tests. Existing TLA regression passes; it is not a
proof of original-input query results or pixels. No public policy/timing gate
is promoted.

The replay-only authenticated query-baseline join is implemented. Captured
query labels and local core identity/epoch/ordinal/fence remain separate;
actual rows are validated against production-derived local record facts,
without weakening exact publication/topology checks. Both full architecture
builds pass, as do 90 serialized native Tape/query/routing regressions and six
x86_64 execution/binding/provider/launcher/install-name checks. Native tests
cover 1,536 bounded seed/Poll/Issue combinations plus atomic rejection,
promotion, stale-generation, BEGIN/END, retirement and rollback cases. The
production comparator accepts captured cycle/ordinal 32/48 mapped to actual
2/2, and rejects substituted labels and changed source/prefix/command extents.
These finite tests do not prove unbounded histories, all concurrent retirement
interleavings or actual GPU completion. Original capture27 query readiness,
strict source/output pixels and policy/timing qualification remain open; the
subsequent unchanged-input results are recorded below.

Unchanged-input x86_64 diagnostics 04/05 now admit both authenticated query
baselines (2/2) and record the first actual query END coordinator with two
acknowledged raws/five records. The earlier ordinal-continuity rejection is
resolved; complete query-transcript qualification is still absent. The next
rejection is `v4-exact-source-publication`, reported while beginning event index
24 and closing the preceding query command event (ordinal 24). Diagnostic05's
bounded, reporting-only publication summaries identify the exact mismatch:

| Row | Original capture | Actual replay |
|---|---|---|
| Source 0 command count | 1 | 1 |
| Source 0 publication cut | ordinal 24, AfterRecord(1) | ordinal 19, BeforeEvent(0) |
| Initializer publications | none | ordinal 19 BeforeEvent(0), ordinal 24 AfterRecord(1) |

This is not a command-count mismatch or evidence of a pixel failure: execution
stops before draws/readback. Ordinal 19 is the 7,372,800-byte full initial Upload
for texture `{kind=0,generation=1,id=4294967304}`, defined immediately before it
as a DEFAULT-pool, RT-usage, 1280x720 A16B16G16R16F 2D `CompleteSeed` resource.
The PE lazy-definition publisher emits this Upload to materialize initial
snapshot content; it is not an application mutation. In `replayGeneralPlan`,
the ordinary ResourceMutation branch treats it as an interval write after prior
command work and calls `upper->flush()` before `applyGeneralMutation`, publishing
the held Clear at BeforeEvent(19). Initial snapshot restoration therefore
perturbs the observed source/initializer timeline. The missing implementation
is authenticated pre-arm seed preparation with retained ownership and ordered
logical activation, preserving real interval-write synchronization. Removing
all mutation drains, enabling an unrelated prestage experiment, or relaxing
exact source comparison does not close this gap. Evidence:
`experiments/output/stkcop-v4-capture27-exact-diagnostic-04/` and
`experiments/output/stkcop-v4-capture27-exact-diagnostic-05/`. Existing TLA
regression passes after the join; the final reporting-only diagnostic addition
also passes both builds and launcher/install-name checks. Policy and timing
gates remain closed.

Capture27's late initial closure is not texture-only: 627 owners, 4,807
subresources and 412,055,888 promised bytes comprise 494 2D textures, four cube
textures, one volume texture, 86 vertex buffers and 42 index buffers. Every
late initial owner has an exact contiguous promised Upload block. The 195
subsequent real buffer mutations (191 vertex, four index) must remain interval
writes. Two standalone surfaces and one backbuffer are seeded within the
existing pre-arm prefix; 13 later texture aliases promise no independent
content. Setup admission must cover this whole authenticated initial
closure with explicit memory/ownership bounds, not bypass drains for one
resource ID or only enable the existing texture-prestage audit.

The typed initial-setup owner is now implemented in
`device_c_render_tape_initial_setup_v4.*` and the private V4 provider. Admission
authenticates the complete immutable input before reset and bounds late setup
to 1,024 owners, 8,192 seeds and 512 MiB of promised bytes. Generation-qualified
metadata, contiguous complete seed blocks and original alias activation are
retained; no blob payload is copied into the plan. Shared ownership transitions
bind preparation, activation, ordered consumption and failure release. Actual
buffer backing/bytes and texture backing are checked. The initializer now
distinguishes successful immediate writes from failed staging allocation and
retains failure even before a capture sink exists; an empty pending stream
cannot qualify a silently dropped seed.

Both full ARM and x86_64 builds pass. All 96 serialized native Tape/query/
routing/upload regressions and eight x86_64 focused/launcher/install-name checks
pass, including 6,451 initial-setup checks and actual missing-destination/
missing-backing upload and zero-init counterexamples. Existing TLA regression
passes (201.19 seconds). These checks do not prove actual allocator-success
timing, all concurrent retirement histories or original-input pixels.

Unchanged capture27 diagnostic06 confirms all 627 owners prepared and activated,
all 4,807 seeds consumed, and 412,055,888 setup bytes. Source 0 now exactly
matches ordinal 24 / AfterRecord(1), including one Clear and its actual END
coordinator. Physical conservation is 757 created / 757 released. Replay reaches
5,847 events, 83 acknowledged raws and 3,642 dispatched records before rejecting
at event index 5,848 with `v4-exact-source-publication`. The next mismatch is
source fragmentation: expected three sources, actual partial evidence has 26;
the first extra cut is ordinal 4,929 / BeforeEvent with 307 commands, whereas
the original second source has 558 commands and publishes at ordinal 5,848 /
AfterEvent. One extra initializer appears at ordinal 5,162 / BeforeEvent.
Actual route counters include 893 Direct-lease draws, 2,046 compatibility draws
and zero reported GPU errors, but execution stops before output readback.
There is no pixel-failure verdict and no performance comparison. Complete
query/source/pixel/policy qualification remains closed; the next task is to
join genuine interval mutations with their original production submission
semantics, not weaken the comparator or globally remove mutation drains.
Evidence, build and regression logs:
`experiments/output/stkcop-v4-capture27-exact-diagnostic-06/`.

The immutable first extra-cut event is now identified: ordinal 4,929 is a
genuine `CpuUnlock/NoOverwrite` of buffer `{kind=BUFFER,generation=1,
objectId=4294967300}`, offset 30,408, size 73,728. Its descriptor (ordinal 4,918)
is a 1,572,864-byte DEFAULT-pool vertex buffer with usage `0x208`
(DYNAMIC|WRITEONLY); its full initial seed at ordinal 4,919 has already been
prepared and consumed. All 195 genuine buffer writes in this input have the
same DEFAULT+DYNAMIC/NoOverwrite classification. `applyGeneralMutation`
preserves the lock flags, but the provider's generic pre-mutation flush runs
before production's no-wait/range-upload classification. A future narrow V4
admission may reuse that production path for exact active generation and
validated bounds; it must not authorize Managed/Plain/Discard, texture writes,
destruction or an unrestricted mutation-drain bypass. Ordinal 5,162 is another
NoOverwrite on the same buffer (offset 39,760, size 112), not a texture upload;
the initializer published there must be attributed to preceding work.
The preceding NULLRT definition at ordinal 5,096 is the concrete code-supported
candidate: DEFAULT-pool RT creation calls `initializeTextureZero`, and
`NullRt` has four bytes per pixel, so Private backing stages a zero fill.
The DF24 texture defined at ordinal 5,098 has depth-stencil usage and does not
take that branch; its alias also does not upload. Bypassing the extra buffer
flush may relocate this initializer publication but cannot eliminate that
separate late physical-definition effect. Allocation-only setup must preserve
original logical activation and produced-content semantics; skipping arbitrary
zero initialization is not an admitted fix.
Its immutable identity is texture `{kind=0,generation=1,objectId=4294967320}`:
2048x2048x1, one mip/subresource, `NullRtDescriptorOnly`, zero promised bytes
and zero seeds. Alias ordinal 5,097 is surface
`{kind=1,generation=1,objectId=4294967321}` with that exact parent/subresource
zero and no independent content. This is descriptor-only, not
`ProducedByCapturedPass`; a future allocation-only setup owner must explicitly
support zero-seed lifecycle/accounting without inventing an Upload or moving
the separate DF24 produced-content operation.

Both narrow refinements are now implemented. The shared V4 mutation predicate
requires an exact active DEFAULT+DYNAMIC buffer, the original bounded
NoOverwrite write, and a complete original Upload backed by either the checked
prefix checkpoint or a consumed checked setup owner. Only the provider's extra
preflush is omitted; production Lock/Unlock and range upload remain unchanged.
AllocationOnly NULLRT setup has a separate 64 MiB footprint bound, no seeds or
content authority, and original definition/alias activation. Its normal physical
creation and zero initialization complete before arm.

Diagnostic07 on unchanged capture27 confirms 628 prepared/activated owners,
including one 16,777,216-byte allocation-only owner; 4,807 consumed seeds and
412,055,888 seed bytes are unchanged. All 195 genuine NoOverwrite writes use
the proven no-extra-preflush path. There are zero interval mutation preflushes
and zero captured initializer publications. Physical conservation remains
757 created / 757 released. However, exact execution still rejects at event
index 5,848: only the first one-command source has published, whereas the
second original source must publish 558 commands at ordinal 5,848 / AfterEvent.
The earlier fragmentation is removed; the missing original publication boundary
is the next unresolved contract. Readback was not reached, so this is not a
pixel mismatch or performance verdict. Query readiness locally matches, but
full query/source/pixel/policy qualification remains closed.

Both full native and x86_64 builds pass. After correcting one malformed-alias
fixture by retiring its first alias before its replacement, all 97 native
regressions and all three focused setup/mutation/upload tests pass.
Nine x86_64 focused/launcher/install-name checks and the existing TLA
regression (200.22 seconds) also pass. The allocation-only lifecycle and mutation
admission are checked through shared production predicates; existing TLA checks
are regression evidence, not proof of original-input pixels.
Evidence and build/test logs:
`experiments/output/stkcop-v4-capture27-exact-diagnostic-07/`.

The missing boundary is the production capture-Present reservation, not a
state-only replay operation. `D3D9DeviceImpl::commitPendingCommandChunk` reserves the
selected mirror before preparing/stamping the Present raw;
`dxmt9c_device_reserve_render_tape_present_capture` joins software replay and
flushes the preceding writer. Original edges ACK 335, publication 336,
completion 339 and next raw attempt 340 establish its location. Present ordinal
5,849 begins only after this capture-only effect. Generic Tape does not encode
the reservation reason, so a publication expectation alone cannot authorize a
flush. The explicit `pe-present-reservation-v1` diagnostic producer contract is
specified separately; its implementation and original-input qualification must
preserve this distinction from ordinary D3D9 Present behavior and policy timing.

The explicit adapter is implemented through shared production
`prepareRenderTapePresentReservation`, an exactly-once digest/event-bound
executor transition and the private launcher's authenticated producer/scope
checks. Default NONE performs no preparation. Native tests retain generic
destroy-to-Present as a negative (no preceding raw ACK), test a genuine
state-raw-to-Present positive and preserve immutable source actions. Full native
and x86_64 builds, five focused native tests, nine x86_64 checks, 12 launcher
tests and existing TLA regression (200.49 seconds) pass.

Diagnostic08 explicitly selects `pe-present-reservation-v1` on the unchanged
bundle. Preparation completes once at event index 5,848 with HRESULT zero.
Actual source publications now have the original 1/558/1 command extents and
ordinal/cut positions 24/AfterRecord(1), 5,848/AfterEvent and
5,849/AfterRecord(2). All 84 raws and 3,644 records execute; all 560 physical
commands and zero initializers are observed. Readback finally executes and
reveals an actual pixel failure: both local source and output contain
3,686,400 bytes with digest
`ccc8aa1103a1f9410a77bce7f4f4470935307e6b56fcd4231aec09b88a66a26d`,
not the captured
`82f2323f61515b48040f395bfa3b99acc594002620a1186f805db3d60d5b35f9`.
Output is non-degenerate, reported GPU errors are zero and physical ownership
remains 757/757. `OutputMismatch` stops PresentComplete before final
execution/query qualification; matching publication rows are not a complete
CPU/topology/query certificate. Pixel and policy/timing gates remain closed.
Evidence: `experiments/output/stkcop-v4-capture27-exact-diagnostic-08/`.

Diagnostic09 separates the final pixel rejection from independent finalization.
All 5,850 events, 84 raw acknowledgments and 3,644 records complete; canonical
traversal, raw coverage, actual lifecycle/topology and exact-source diagnostic
checks are all true. Actual EVENT readiness qualification passes. This input
contains no occlusion-result oracle, so it provides no sample-count proof.
Overall provider status remains OutputMismatch and all pixel/policy/timing
gates remain closed. The local source/output digest exactly repeats diagnostic08.

Both actual 3,686,400-byte readbacks are preserved with exclusive creation in
the private diagnostic directory. Independent byte/image analysis finds 907,785
different RGB pixels out of 921,600 (98.50098%); alpha is identical. Mean
absolute channel differences are R 8.0765, G 9.2564 and B 10.9951 (8-bit values),
with maxima 112/125/128. This is a broad image discrepancy, not only undefined
alpha or a few low-bit errors. Side-by-side inspection shows the same scene,
but does not identify the causal transform/content error. No tolerance change
or rendering-path promotion is justified.

Final native regression is 97/97; x86_64 focused checks are 9/9; the private
launcher tests pass. Evidence, readbacks, build/test logs and reproducible image
comparison script/results:
`experiments/output/stkcop-v4-capture27-exact-diagnostic-09/`, including
`pixel-analysis/comparison.json` and
`pixel-analysis/expected-left-actual-right.png`.

Follow-up diagnosis identifies missing physical-backbuffer alias authority.
Diagnostic09's source readback is byte-identical to the arm seed blob
`ccc8aa1103a1f9410a77bce7f4f4470935307e6b56fcd4231aec09b88a66a26d`.
The bootstrap/final RT0 is surface `(kind=1,generation=1,objectId=4294967297)`,
serialized as Standalone; the terminal Present source is
`(kind=1,generation=4,objectId=4294968987)`, serialized as SwapchainBackbuffer.
Eight final draws target the first identity; no recorded copy or RT binding
updates the second. Replay therefore correctly follows those descriptors into
two different physical backings, leaving the Present source at its arm seed.
`device_c_draw.cpp` / `dxmt9c_device_get_render_target` can create a fresh
facade around the original primary backbuffer, while PE Present uses a separate
cached facade; the current capture classification does not preserve that
relationship for every wrapper. Equal seeds do not authenticate the original
alias. Capture27 remains immutable counterexample evidence, not a repairable
pixel-qualified policy input. Provider-authenticated primary-backing
classification and multiple-wrapper replay admission are implemented. Arm-time
classification preserves logical identities, uses no content/shape heuristic
and performs no interval drain or snapshot; late new aliases reject. General
admission chooses the actual explicit Present source independently of definition
order and permits only unambiguous handle-less Present. Native regression passes
100/100; an additional actual-provider ABI test confirms two live facade IDs
share the primary backing, equal-shaped standalone storage does not, stale and
retired identities fail with zero output, and releasing one facade preserves
the other's classification. The PE source-routing audit passes five checks.
Full native, x86_64 Unix, PE x64 and PE x86 builds pass; the focused x86_64
regression passes 12/12, including actual provider, V4 setup/execution,
classification and install-name checks. The first PE x64 regeneration lacked
`llvm-ar` on PATH; the canonical retry with the installed llvm-mingw bin path
passes. Evidence logs are preserved in
`experiments/output/stkcop-v4-primary-backbuffer-alias-01/`.
At this implementation checkpoint a genuine corrected recapture was still
required. The subsequent capture28 evidence above closes original-input private
pixel qualification, not public policy or timing eligibility.

### Query-aware captured policy loop (2026-10-02)

R-HARN-REPLAY-9.1–9.5 are not yet implemented end-to-end. The current capture
now has V2 query descriptors and polling payloads preserving FLUSH, output
shape, defined result bytes and derived issue cycles; the native Render Tape
validator passes stale/reissued/uncaptured-cycle and malformed payload cases.
General CPU and Metal query execution and receipt qualification are under
integration. Synthetic cold/warm Metal and controlled CPU CLI runs now accept
both compatibility and production-slot routes, with original query transcript
and pixel checks in `traces/query-tape-integration-h430/cli-*`. The real Metal
profile returns 192 samples and zero after reissue; the default primitive-count
backend is not a GPU query oracle and is rejected before execution. Capture
and replay must use the same explicitly qualified backend profile.
A retained old-query GPU snapshot assertion subsequently reproduced with named
PollBudget exhaustion (262,144 polls in about 56 ms, actual contribution still
pending, no backend failure). A controlled completion delay reproduces that
bounded-oracle outcome. The fixture must distinguish bounded polling from
retained-result correctness after real completion; the original uninstrumented
failure remains unclassified. General provider settlement follows its actual
Present drain, unlike the original isolated retained-query fixture.
A complete device-state bootstrap still does not close an open query at the capture cut; capture
explicitly rejects that cut. Synthetic query fixtures are not evidence of full
captured STALKER support.

An explicit published-only query snapshot capability preserves the marker and
cancellation through reissue/release. The initial core snapshot, END-marker and
device-lifecycle tests pass, along with the native-free query receipt gate.
The snapshot target also passes cancellation-after-classification and stale
epoch cases. The existing full TLA regression exits successfully; it does not
prove the new captured end-to-end path. This does not change default snapshot
eligibility or permit unbound-writer polling.

Required evidence remains query-aware capture/validation, production CPU
controlled-completion execution, actual Metal query and original-pixel
qualification, and repeated unchanged-input policy comparisons with separate
setup/replay/Encode/completion costs. No query stripping, eligibility relaxation,
recorded-duration sleeps, or synthetic results may substitute for those gates.

Measurement qualification now binds logical release schedules to independently
verified query identity/cycle evidence. Count-preserving wrong-cycle and foreign
Ready releases reject in paired results; standalone `execution_valid` remains
structural-only. The completion-mode adapter and zero-obligation transcript
contracts are corrected, with native-free negative tests and the x86 CPU CLI
regression passing. No actual-workload repeated timing result is qualified yet.

R-HARN-REPLAY-9.6 is also open. PE volume LockBox capture, canonical mip
content/layout helpers, General provider creation/upload and the CPU full-slice
writer are implemented. PE32/PE64 and the canonical x86 Unix builds pass,
including the Unix install-name audit. Seven focused x86/Rosetta schema,
query snapshot/transcript, Volume writer/layout and shadow-layout tests pass;
the real Metal query, Volume mip/z-slice and seed-reference rejection tests also
pass independently on x86/Rosetta. Actual STALKER capture remains open.
The first new run (`app-d3d9-stkcop-bench-tape-h430-query-volume-02`) passes the
bridge handshake but rejects format 116, 64x64, pitch 1024, at startup with
`unsupported_lock_format`. It is intentionally stopped after permanent registry
invalidation, not a successful wild/FPS or Tape result. Explicit format-116
16-byte canonical layout support and raw-bit/padded-row, partial/mip/Volume and
overflow tests now pass on native and x86. Capture retry
`app-d3d9-stkcop-bench-tape-h430-query-volume-03` passes startup registration,
reaches the selected arm boundary and completes color seed snapshots, but
repeatedly rejects bootstrap construction with `snapshot_built=0`,
`snapshot_appended=0` (`arm_status=4`, `interval_status=3`). The app exits zero;
its runner PASS and desktop-fallback screenshot are not Tape or visual oracle
qualification. Bootstrap-state rejection attribution is the next actual-input
gate; no captured state may be dropped to bypass it.
Failure-only bootstrap diagnostics now report all four shadow table counts and
capacities plus all six constant-family byte/alignment/register bounds. Both
PE builds pass. Before retry 04, the larger full-shadow extents made overflow
of the single-section capacities (render-state/TSS/sampler/transform:
64/64/64/16) a candidate without an observed rejecting dimension.
Capture retry `app-d3d9-stkcop-bench-tape-h430-query-volume-04` now identifies
the rejection: sampler state has 83 entries against capacity 64; render states
43/64, TSS 12/64 and transforms 3/16 fit. All six constant ranges are aligned
and within their register limits (VS float 256/256, PS float 8/224, others
empty). The run was intentionally terminated after repeated identical
bootstrap rejection; its process failure is not a rendering/FPS verdict. No
Tape was published. A lossless paged bootstrap is required: simply emitting
multiple FULL_SNAPSHOT records is unsafe because the CPU state fold resets its
baseline at each such record. The normal recorder section capacities and
capture-OFF scratch footprint must not grow to accommodate this cold checkpoint.

R-HARN-REPLAY-9.7 now has a dedicated cold PE bootstrap emitter, baseline-profile
3 validation, cumulative paged fold output and ordered provider RT0
classification. Ordinary producer source/header and section capacities are
unchanged. Native producer tests compare real sparse replay with the entire
source state, including complete constant bytes/dirty cursors, explicit nulls,
index binding and unchanged live shadow; bounded emission and malformed
constant failures also pass. PE32/PE64 and the full x86 Unix build pass. Native
tape/projection/provider plus query/Volume/budget/seed regressions pass 7/7;
Main additionally reran x86 tape/projection, emitter and install-name checks.
The shape layer covers 1554 independently specified v2/v3 trace/partition
cases, and page coverage checks 85 small partitions. Maximum legal-domain
folding stays inside the conservative 35-record producer and 64-record CPU
admission bounds. The actual PE helper now feeds an 83-sampler CPU fixture:
compatibility/production-slot verify and measure runs pass with three bootstrap
records, three ordinary/source-summary commands, two draws, three conserved
submissions and five conserved resources. A tail-page mutation changes the
actual CPU semantic digest; both correct verification routes agree. Main reran
the producer and CPU paged-route tests and independently inspected the exported
fixture. Independent verify/execution/measure receipt checks and cross-route
verification also accept that same input without counting bootstrap pages as
ordinary sources. A real Metal fixture now uses 64+14 sampler rows with sampled
Volume s5 ADDRESSW deliberately in the final page. Both routes pass fresh
correct / warm wrong-tail / warm correct recovery: correct pixels match the
unchanged original oracle, wrong wrap produces independently predicted swapped
stripes and OutputMismatch, and real query results and wrapper conservation
hold. The final seven-test native matrix and Main's separate Volume Metal
rerun pass. These fixtures do not replace actual STALKER evidence. Retry 05
passes paged snapshot construction but rejects resource closure with
`bootstrap_referenced_incomplete_seed`: kind 1, generation 1, object
4294967298, format 75 (D24S8), 1280x720, usage 2, no multisampling;
descriptor admission succeeds but expected seed layout is `unsupported_format`.
The verified task process was terminated after repeated identical rejection.
No Tape was published and no FPS/visual qualification is claimed. Lossless
depth/stencil checkpoint capture and restoration, or an independently proven
first-access initialization contract, remain to be established; omitting the
bound resource or fabricating its initial contents is not a valid workaround.

R-HARN-REPLAY-9.8 has a native Metal capability proof, not production admission:
`dxmt9-render-tape-d24s8-checkpoint-metal-spec` passes with API validation,
including Main's independent rerun. Independent rendering produces nonuniform
17x19 Depth32Float_Stencil8 contents; two-plane capture and inverse blits
preserve all 1615 canonical bytes through A-to-B-to-A, all 256 stencil values,
and nine independent depth/stencil-tested pixel oracles. Single-aspect writes
also preserve the other aspect. No float quantization or oracle relaxation was
used. PE ABI/layout admission and production queue-owned capture/restore are
now being integrated; actual STALKER replay remains unqualified.
The additive capture ABI, checked dual-plane layout and arm-local overlay pass
the native capture suite, including Main's independent rerun, and both PE
builds. The new CPU fixture exposed and closed missing disposition-6
structural-validator branches. Six focused CPU query/bootstrap/volume/attachment/
alias/D24S8 tests pass, including Main's independent rerun; both replay routes
preserve the full logical seed, query obligations and resource conservation.
Depth-only and stencil-only changes alter the verification digest; measured
execution omits those semantic observations. Synthetic repeat-two receipt
checks accept both routes, not actual STALKER timings or GPU results. The full
x86 Unix build and install-name audit pass. Main's Rosetta production checkpoint
GPU test also passes with API validation: queued two-aspect clear drain, exact
bridge capture and inverse A-to-B-to-A restoration, nine independent aspect
pixel oracles, stale-generation/layout rejection and exclusive lease ownership.
It checks 31,104 shared completion-predicate states and all 32 build flags.
The bounded ordering checker additionally covers 20,480 traces and 62
missing-post-drain-recheck counterexamples. The actual provider passes 36
Compatibility/ProductionSlot replays covering depth-only, stencil-only and
combined tests, A-to-B-to-A warm reuse, authenticated wrong-plane OutputMismatch
and recovery, with six fresh exact-5N seed audits. Warm seed auditing remains
unsupported; warm cases qualify through original-pixel equality instead.
API validation initially exposed borrowed-device adoption at three existing
Presenter sites. Both the new fixture and the existing Volume oracle crashed
on PSO teardown on native and Rosetta. Correct retain-from-borrowed construction
closes that defect: the unchanged validation profiles now pass both fixtures,
the checkpoint test, and the ownership source contract, including Main's
independent five-test Rosetta matrix and install-name audit. No validation,
float precision or pixel oracle was relaxed. Existing
TLA regression passes all 227 configurations; it does not establish the new
exclusive checkpoint transfer protocol. Do not treat these intermediate build
and capability results as a published STALKER Tape or a performance result.
Actual retry `app-d3d9-stkcop-bench-tape-h430-query-volume-06` passes the new
ABI handshake, snapshots the bound 1280x720 D24S8 attachment as exactly
4,608,000 bytes on physical format 260, and completes six-record paged state
bootstrap plus resource closure. It then repeatedly rejects
`query_uncaptured_cycle` for query identities kind 5, generation 1, objects
4294968943 and 4294968944. The verified benchmark process was intentionally
terminated after repeated rejection; no Tape was published and no FPS or
visual qualification is claimed. Query issue history across the capture cut
is now the next actual-input gate. Query type and pre-cut phase must be
established before selecting a bootstrap or retained-history contract; dropping
the poll or inventing an issue/result is not a valid workaround.
Retry 07 adds failure-only PE query diagnostics without changing semantics.
It identifies both alternating queries as EVENT (type 8), closed, with absolute
cycles 589/590 in the sampled failures. The real call is GetData(FLUSH), S_OK,
four requested output bytes but zero defined bytes under the current EVENT
contract. No open BEGIN or missing occlusion samples are implicated by this
failure. The verified benchmark process was stopped after repeated identical
rejection. R-HARN-REPLAY-9.9 now requires an authenticated completed-EVENT
checkpoint and a separately accounted local production END on replay.
The implementation now uses explicit V3 descriptors, independent Unix
cycle/Issue-ordinal history, current device-epoch revalidation, and an arm-local
overlay without changing durable query state. CPU and Metal replay restore one
real local END before the transcript, then activate the same hidden owner at
ObjectDefine; sequence-zero empty prefixes remain valid. Setup counters and
timing are separate from original Issues, polls and measured transcript work.
Native schema/capture/authority/current-epoch tests pass, including 512 bounded
authority states. CPU tests and independently inspected, exported synthetic
receipts pass both Compatibility and ProductionSlot verify/measure routes.
The Metal oracle passes sixteen actual provider executions covering Pending
rejection, current/stale cycle/generation/epoch/terminal authority, reissue,
warm A/B/A pixels, returned creation failures before/after activation, and
combined texture-prestage physical ownership conservation. Ten serial native
oracle repetitions pass. A real prestage initializer may require setup polls:
only a witnessed empty sequence/initializer prefix requires zero waits. The
first transcript poll must consume the exact completed setup incarnation with
no supplemental wait or additional END; original Issue/poll counts and exact
pixels remain unchanged.
Main's final native matrix passes 11 tests; the Rosetta matrix passes five,
including the actual Metal oracle and Unix install-name audit. Both PE builds
and the full canonical x86_64 Unix build complete with the same bridge schema.
Ownership is also guarded across exceptions, but actual allocation-throw
injection remains unexecuted; returned creation failure is not that evidence.
The existing 227-configuration TLA regression passes, not a new proof of the
entire checkpoint protocol. No actual STALKER Tape is published yet; actual
capture, production query/pixel qualification and repeated policy measurements
remain open. These synthetic results establish no FPS benefit.
Actual retry `app-d3d9-stkcop-bench-tape-h430-query-volume-08` on `e795e2a5`
passes bridge ABI `0xb5182eeb924e5741` and authenticates both pre-cut EVENT
queries, including independent cycles/ordinals 585/585 and 586/586 in the first
attempt. Color and D24S8 checkpointing also complete. The next failure is now
the PE bootstrap producer: closure construction reports four objects, then
throws with one object/mutation/blob already staged. The catch currently
reports only `reason=exception`; the cause is not yet attributed. The verified
game process was deliberately terminated after repeated identical failures;
runner exit 1 is that termination, not a GPU failure or FPS result. No Tape
was published. The harness screenshot captured Orca rather than the game and
is not visual-correctness evidence.
Retry `app-d3d9-stkcop-bench-tape-h430-query-volume-09` on `4d9d177e`
does not reproduce the bootstrap exception: four objects, three mutations and
four blobs arm successfully. This is not evidence that the exception is fixed.
The next repeated rejection is `command_chunk_produced_pass_preflight`:
surface alias 4294967305 refers to texture 4294967304 (generation 1), an
1280x720 format-113 render target whose expected-content layout is unsupported
and which has no arm snapshot. Binding-only access supplies no terminal-write
proof, so ProducedByCapturedPass cannot replace its missing initial contents.
Exact RGBA16F checkpoint/readback/upload support remains open. The process was
deliberately terminated after repeated rejection; no Tape was published and
this diagnostic run provides no FPS or visual-correctness verdict.
R-HARN-REPLAY-9.10 now has an explicit raw eight-byte color encoding, a shared
checked snapshot layout/row writer, PE arm/result validation, CPU restoration
and Unix capture/upload support for the observed RGBA16F RT shape. Both PE
builds pass. Five native schema/capture/CPU alias/Volume/EVENT tests pass,
including both CPU routes with the new raw seed; these do not establish GPU
bit preservation. The canonical Unix build now passes, and the actual Metal
oracle preserves raw half bits (including NaN payloads) in odd 7x5 rows across
six provider restores and A/B/A receipts, with alias conservation and unchanged
pixels. D24S8, EVENT and install-name regression checks also pass. That fixture
explicitly drains its initial upload before capture; capture's own completion
and producer-exclusion guarantees require a separate audit before this proves
self-contained checkpoint admission. The audit confirms a real gap: ordinary
flush does not publish pending initializer uploads when there is no ordinary
source, and retained-texture readback does not exclude intervening producers.
The V2 path now holds the existing exclusive checkpoint lease through
initializer completion and raw transfer, bypassing the unprotected ordinary
flush. The rebuilt Metal oracle removes the external pre-drain, asserts an
initializer-only pending upload before each A/B/A capture and checks complete,
nonfailed initializer progress afterward. It and D24S8/EVENT/install-name
regressions pass (4/4). The earlier conditional test is superseded for this
initializer-only case. Targeted new-entry negatives also pass: real active
Direct admission leaves the initializer pending and caller bytes unchanged,
wrong physical backing rejects, and terminal state rejects completion.
the existing 227-configuration TLA regression passes, not a new protocol proof.
New workload capture and original
query/pixel qualification remain pending. Generic
LockRect/LockBox and produced-by-pass proof gates are unchanged.
Actual retry `app-d3d9-stkcop-bench-tape-h430-query-volume-10` completes V2
snapshots for six RGBA16F textures (four 1280x720, one 64x64 and one 8x8),
including the parent that rejected capture09. EVENT and D24S8 also complete.
The next repeated rejection is an exception in PE arm-snapshot collection,
after several managed texture snapshots, before bootstrap. Its exception type
and exact throwing operation are not yet identified; process RSS alone does
not prove allocation/address-space exhaustion. The process was deliberately
terminated after repetition, with no published Tape or FPS qualification.
Retry `app-d3d9-stkcop-bench-tape-h430-query-volume-11` on `516c3f25`
does not reproduce the arm-snapshot exception. Snapshots and bootstrap complete,
but session arming repeatedly returns `CapacityExceeded` (`arm_status=5`,
`produced=1`, four seed blobs). Both `armWithBlobs` and its nested `arm` map
allocation exceptions to that same status, so this evidence does not yet
distinguish a configured limit from an allocation failure. The runner was
deliberately terminated after repetition; no Tape was published. Cold failure
provenance must cover both layers before attributing or changing storage.
Both arm layers now report allocation-free failure provenance distinguishing
explicit limits from typed exceptions and identifying the failing phase.
Terminal abort releases capture-owned payloads and builder storage before
retry, preserving only value diagnostics. Native capture/tape tests pass (2/2),
including allocation-failure sweeps and abort deallocation checks; PE32, PE64,
and the canonical Unix build pass. This fixes an independent retry-retention
bug, not the unexplained first-attempt failure in capture 11. A new workload
capture remains required to identify that failure and qualify the original Tape.
Capture `app-d3d9-stkcop-bench-tape-h430-query-volume-12` on `f375803c`
identifies the first failing operation: `arm_with_blobs/blob_digest` throws
`bad_alloc` for blob 0 (3,686,400 bytes), before any unique blob has been
copied. The configured blob budget is 268,435,456 bytes, not the rejecting
condition. The failure repeats; the game was deliberately terminated and no
Tape was published. The SHA-256 whole-input temporary is the next removable
allocation; removing it does not yet prove that later ownership copies will
succeed or establish the underlying allocator/address-space exhaustion cause.
SHA-256 now processes complete input blocks directly with a fixed 128-byte
padding buffer. Native capture/tape tests pass (2/2), including independent
padding-boundary digests and a zero-allocation check at the observed failing
extent. Actual workload capture and downstream admission remain unqualified.
All three canonical runtime builds pass. Capture
`app-d3d9-stkcop-bench-tape-h430-query-volume-13` with the streaming hash
passes hashing but repeatedly fails at `arm_with_blobs/owned_blob_copy`:
`bad_alloc` for blob 1 (4,608,000 bytes), after one unique blob (3,686,400
bytes). The game was deliberately terminated without a published Tape.
This moves the evidenced failure to duplicate payload ownership; consuming
the already-owned bootstrap seed is the next change, not increasing limits.
The production bootstrap now uses consuming `armWithOwnedBlobs` admission;
the borrowed API still copies through shared validation. Native tests pass
(2/2), covering canonical event/digest equivalence, payload-pointer
conservation, duplicate-aware budgets, failed rearm, and allocation-failure
rollback. Arm snapshots remain intact for JIT materialization. Runtime builds
pass for PE32, PE64 and Unix. Capture
`app-d3d9-stkcop-bench-tape-h430-query-volume-14` now reaches `arm_complete`.
Its first command repeatedly rejects alias 4294967307 (generation 1) to
texture 4294967306 (generation 1): format 21, 1280x720, single mip, no MSAA,
render-target usage, absent arm snapshot and incomplete initial content.
The observed access is binding-only, not a terminal-write proof, so the
existing rejection remains correct. The run was deliberately terminated;
no Tape was published. Investigate this snapshot omission before claiming
original workload capture or policy qualification.
R-HARN-REPLAY-9.11 now declares exact DEFAULT A8R8G8B8 2D RT arm snapshots.
Both shared gates admit that shape; the provider reuses the exclusive raw
checkpoint lease with exact BGRA8 physical storage and meaningful alpha.
Native capture/tape tests pass (2/2); existing TLA regression passes all 227
configurations. PE32, PE64 and canonical Unix builds pass. Actual Metal
verification passes 5/5: initializer-only alpha-aware A8 A/B/A, RGBA16F,
D24S8, completed EVENT checkpoint, and the install-name audit. The A8 oracle
checks six restores across both routes, exact bytes/digests, and alias
ownership. Actual workload retry remains required; synthetic oracle success
does not establish production-capture qualification.
Capture `app-d3d9-stkcop-bench-tape-h430-query-volume-15` confirms the real
format-21 owner 4294967306 snapshot completes (1280x720, pitch 5120,
3,686,400 bytes) and arming succeeds. It then repeatedly aborts at
`jit_resource_mutation`; the current caller log does not distinguish blob
admission from mutation-event rejection. The game was deliberately terminated
without publishing a Tape. Add identity-qualified phase/status/exception
provenance before attributing this new failure or changing content ownership.
Mutation failures now retain identity-qualified phase/status/typed-exception
and pre-operation accounting through abort. Native capture/tape tests pass
(2/2), including catalogue/payload/event allocation failures, explicit budgets,
validation rejection and accounting retention. PE32, PE64 and canonical Unix
builds pass. Capture `app-d3d9-stkcop-bench-tape-h430-query-volume-16` fails
earlier than capture 15: bootstrap `content_blob_copy` throws `bad_alloc` for
surface 4294967298, generation 1, subresource 0, 4,608,000 bytes, after one
object/mutation/blob. Thus it does not attribute capture 15's JIT failure.
Snapshots complete and closure remains four objects; the process was
deliberately terminated after repeated failures without a published Tape.
The next ownership review must address selected arm-snapshot duplication in
bootstrap and JIT while preserving snapshots not yet materialized and EVENT
checkpoint authority; a larger configured budget is not the demonstrated fix.
Selected current-arm payloads now transfer to bootstrap/session storage after
checked identity/epoch/source selection. Bootstrap stages throwing work before
final swaps; JIT owned-mutation failures abort the interval. Descriptors,
unmaterialized snapshots and mutable registry bytes remain intact. Native
capture/tape tests pass (2/2), including 4,096 transfer-predicate cases, pointer
conservation, duplicate content, partial-transfer failure, fresh-epoch retry,
and EVENT authority. These are bounded ownership tests, not original workload
qualification. PE32, PE64 and canonical Unix builds pass. Capture
`app-d3d9-stkcop-bench-tape-h430-query-volume-17` reaches arm completion and
100 events, 73 blobs, 32,673,548 owned blob bytes. Its first JIT failure is
buffer 4294969099 generation 1, 2,097,150 bytes, at borrowed
`register_blob_bytes/blob_payload_copy` (`bad_alloc`), not an explicit budget.
A later retry fails on texture 4294969217 generation 21, 524,288 bytes,
after 132 events and 35,120,242 owned bytes. The selected snapshot transfer
does not remove this durable-registry fallback copy. The process was
deliberately terminated with no published Tape; host VM summary alone cannot
attribute Wine guest allocator exhaustion. Further ownership work must not
move mutable durable bytes away from later application mutations.
Durable capture shadows now freeze into shared immutable payload owners for
session admission and publication. Explicit partial writes detach only while
an old version is retained; full writes consume incoming contents. UpdateTexture
validates and stages every source version before committing destinations.
Borrowed admission still copies, and digest/extent/budget checks are unchanged.
Boundary accounting distinguishes logical payload bytes from unique retained
backings, with an explicit lower-bound marker on observer overflow. Native full
build and ten focused capture/layout/publication/provider/mutation/EVENT tests
pass, including allocation-fault rollback and overlapping UpdateTexture slots.
The new fault fixture initially used invalid mip extents; corrected descriptor
preconditions now ensure injection reaches the intended staging path. PE32,
PE64 and canonical Unix rebuilds pass, as do the four A8R8G8B8/RGBA16F/D24S8/
EVENT Metal oracle regressions and install-name audit (5/5).
Unchanged-budget capture `app-d3d9-stkcop-bench-tape-h430-query-volume-18`
on `67898161` reaches 2,996 events and 2,466 blobs before explicit
`blob_byte_limit`, not `bad_alloc`: 268,331,522 unique admitted bytes plus
131,072 incoming bytes exceeds 268,435,456. Byte-exact deduplication precedes
this admission check. The bounded ownership observer overflows its 2,048-entry
backing table, so its unique totals are lower bounds, not total process memory.
The workload was deliberately terminated after the rejection. No published
STALKER Tape, quantified memory reduction, original-input GPU qualification or
policy-performance result is claimed.
Separate capacity probe `app-d3d9-stkcop-bench-tape-h430-query-volume-19`
uses the existing capture-only 512 MiB override; the default remains 256 MiB.
It fails before admission, during `arm_snapshot/color_content_allocate`, with
`bad_alloc` for object 4294970213 generation 3, format 21, 1024x1024,
pool 1, 4,194,304 bytes. At failure 19 snapshots retain 57,610,752 bytes and
durable shadows account for 748,413,336 logical bytes. Repeated failures were
terminated; this run does not prove 512 MiB fit or resolve allocation pressure.
Capture 18 also exposes a separate source-identity gap: QUERY_ISSUE selects
whole-raw Legacy (`plan_reason=1`), whose replay rejects capture identity at
event 24. Later snapshot/settlement rejections follow the sticky failed token;
Oversize (`plan_reason=9`) also needs an authoritative fallback-source contract.
Payload capacity alone cannot qualify these captures. The next gates are exact
arm-snapshot ownership and real compatibility/control publication identity,
not synthetic source rows or relaxed original-byte validation.
The current identity grammar requires nonempty real source/pass coverage for
every CommandChunk record. QUERY_ISSUE includes coordinator-only effects and
empty ENDs with no new publication; these cannot be represented as fabricated
passes. Closing this gap requires explicit no-publication/coordinator evidence
and capture of real fallback publication/settlement. Deferred END sequence zero
is unbound, not completed: its generation-qualified marker must resolve only at
the actual successful queue publication edge, without capture-induced flushes.
Managed pool/usage and payload extent alone likewise cannot authorize skipping
an arm readback: GPU-side writes can leave PE durable contents stale. Any
snapshot storage reuse must preserve actual GPU-byte authentication and failure
atomicity, rather than substituting the CPU shadow for the checkpoint.
Destructive durable-backing reuse is not adopted: managed CPU A followed by
GPU StretchRect B and a partial unlock uploads the whole core CPU A-plus-patch,
not B-plus-patch. `core_surface.cpp` / StretchRect and `core_texture.cpp` /
unlock demonstrate why retaining both authority versions is necessary. Marking
the CPU seed unknown would instead lose existing partial-mutation coverage.
R-HARN-REPLAY-7.26–7.28 now specify coordinator-capable identity v3. Separate
`device_c_render_tape_identity_v3.{hpp,cpp}` implements bounded POD tables,
opcode-qualified exact record/command coverage, no-publication replay settlement,
source and independent initializer lifecycles, exact END cursor/prefix checks,
and a cold capture ledger. V2 bytes/readers are unchanged; the shared wire
version/header-size prefix is pinned. Native full build and nine focused
Tape/capture/source-plan/projection/provider/publication/EVENT gates pass.
The new native test executes 18,965 checks, including stale writer/issue,
cross-event writer, future-source empty END, independent upload readiness,
younger-upload rejection, bounded failure and cross-version wire round-trip.
This binds pure predicates and finite transitions, not live scheduling or GPU
bytes. Actual publication/query observers, every historical poll observation,
PE bridge/publication and CPU/Metal harness driver
integration remain incomplete; no v3 production capture or policy result is
claimed. In particular, the v2 replay driver's per-range forced publication
and CPU event-entry empty-writer assumption cannot simply be reused for v3.
The next native tranche separates unique physical command rows from canonical
record dispositions and ordered subdraw slices. Draw/constant/Draw may share
one DrawRun, but effectful interleaving, physical reordering and ordered-control
straddles reject. Generated coordinator commands are currently admitted only
for UPDATE_TEXTURE SurfaceCopy/GenerateMipmaps mappings. The owned v3 source
plan validates complete evidence, preserves original record/publication cuts,
and binds an explicit LE digest; failed admission invalidates previous output.
Its cursor proves traversal, not execution or GPU completion. Native/Python
inspection exposes six exact tables with lossless u64 values. V3 remains
inspection-only and rejects policy execution before staging/provider effects;
v2 execution is not silently adapted. The CLI fixture currently covers Present
with no nonempty initializer/coordinator tables. Runtime capture integration,
arm-snapshot memory pressure and original-input GPU/query qualification remain
open independently of these native contracts.
Native full build and all 15 focused identity/source-plan/inspection,
Tape/capture/projection/provider and CPU/GPU policy regression gates pass.
Integration corrected a malformed QueryGetData fixture payload length and a
stale v2 CLI expectation for the already-existing empty query transcript;
neither fix weakens validation or removes public evidence. No production
runtime, wild capture, GPU-pixel or concurrency qualification is inferred
from these native/CLI gates.

The next runtime integration adds bounded Unix-to-PE snapshot transfer,
queue-owned revocable observer endpoints, real replay-transaction/ledger
settlement, and a Compatibility/FreshDevice v3 execution session. A full native
build passes. Capture, transfer, publisher, pure admission and runtime identity
tests pass. Integration found and fixed failure-cleanup owner lifetime,
duplicate/empty DrawRun observations, exact cross-event writer admission, and
missing accepted Submit observation in the controlled CPU backend. The
cross-event execution, shared Draw/constant/Draw physical run, stale/foreign
writer rejection and teardown/re-execution negatives pass. Full native build
and 28 focused runtime/capture/transfer/publication/query/CLI regression targets
pass. Strict fixtures were corrected for actual Present ordinals/pass kind,
V2 query descriptors and canonical per-record handle slices; validation was
not relaxed. The existing TLA regression passes, but does not establish
new end-to-end or Metal evidence. ProductionSlot execution remains unsupported, and
the public v3 execution gate remains closed. PE arm/finish/cancel and original
external-event cuts are not yet connected to these new APIs.
The pure execution admission API now reparses exact tape bytes using only the
admitted owner's authenticated blob catalogue and compares the General preflight
value tables. The historical preflight and this binding share one grammar
parser. Explicit field comparisons include reserved values without native
padding dependence; 38 tampering cases cover mutable derived tables. Full
native build and all 67 Render Tape regression targets pass. A separate serial
rerun of the three native depth-snapshot/buffer-lifetime Metal specs passes
after toolchain validation. These synthetic gates do not enable public
execution or qualify the original STALKER workload.

The runtime's publication-time source-local FrameGraph reconstruction is not
evidence of the optimized graph selected by Encode, especially for multi-source
pass coalescing. Actual committed-plan observation and fallback disposition are
required before pass membership can qualify a captured policy. Initializer
completion must likewise come from actual completed command buffers, not solely
the shared-event counter. These remain integration gates, not reasons to relax
identity validation or force a different production route during capture.
R-HARN-REPLAY-7.31 selects identity v4 neutral physical extents plus a required
execution-topology v1 companion. The new pure schema and cold stage/accept/reject
observer are implemented. V3 and V4 now share the full canonical-fact,
coordinator, exact END-prefix and replay-settlement validator; the temporary
V4 query rejection/reduced parser is removed. Actual QUERY_ISSUE without its
required coordinator rejects, while valid checkpoint-only polls retain their
provenance meaning without inventing a captured END. External poll readiness
still requires a separate live-receipt execution join. The integrated native
build and all 71 then-existing Render Tape targets passed in a serialized regression run,
including V3/V4 differential diagnostics and transactional parser budget tests.
At that checkpoint, fully omitted source lifecycles and production acceptance
hooks remained unsupported; subsequent integration is described below. V3 is not silently
reinterpreted to encode natural execution or optimizer omissions. The cold TLS
observer scope requires a retained queue attachment for its entire backend call;
callback revocation alone is not evidence that an unlocked Encode scope ended.
The bounded observer now retains multiple per-invocation candidates and joins
actual registration acceptance with successful Encode observation in either
order. Only the chronological prefix with both facts appears as executed
topology. Repeated prefix refs do not duplicate persistent storage. Native
coverage includes 6,561 bounded dual-order lifecycle traces and 16 prefix/suffix
combinations, receipt negatives, no-growth budgets and revoke/fail-stop cases.
This does not prove production hook interleavings or connect DCE to the observer.

The production partial/direct route can use multiple real transactions per raw.
Existing capture rejects nonzero span starts, and its single generation per raw
settlement cannot authenticate that route. V4 needs explicit per-span attempt,
commit/destination evidence and separate raw-ledger acknowledgment; a synthetic
whole-raw transaction is not an acceptable replacement. The selected V4 schema
separates mandatory real-attempt rows from raw acknowledgment and preserves
publication metadata for receipt joins. Native integration now passes all 73
Render Tape targets before runtime-hook integration, including V4 split/rollback/receipt/query tests and the
paired nine-table transport. Direct spans join a stable source with delayed
publication; Arena joins its real first-ticket publication before commit.
The retained attachment/edge-clock endpoint passes 625 bounded lifecycle
traces, concurrent edge allocation, callback retirement/rearm and destructor
re-entry tests. These tests do not install production topology/partial-replay
hooks, publish an actual Tape, or qualify original-workload GPU/pixels.
Queue completion/routing native regressions and the existing TLA suite also
pass; the latter is not an end-to-end proof of the unconnected V4 runtime.
Actual STALKER capture and GPU/policy qualification remain unachieved.

Runtime integration is in progress: a shared lifecycle core with separate
V3/V4 policies, actual transaction transition observations, exact backend
source locators, and paired PE publication. The four V4 Unix ABI entrypoints
now implement independently returned arm queue identity, keyed event cuts,
frozen paired size/copy receipts and cancellation (R-HARN-REPLAY-7.34).
Retained replay scopes observe real effect/destination rollback boundaries
and the separate raw-ledger acknowledgment. Endpoint retirement joins admitted
callbacks outside the queue mutex. The inert-queue ABI spec covers empty-ledger
admission and receipt transport, not nonempty backend execution or GPU results.
The integrated native build and 77 Render Tape targets pass after these edits
(`/tmp/dxmt9-v4-runtime-final-build.log`,
`/tmp/dxmt9-v4-runtime-final-tape-tests.log`). Queue completion/routing, retained
binding and the existing TLA regression also pass (4/4,
`/tmp/dxmt9-v4-runtime-retirement-tla.log`); this is not an end-to-end runtime
refinement or original-workload proof. Allocation-fault coverage exposed and
fixed publisher string streams silently accepting truncated digest/manifest
output; serialization errors now enter whole-staging rollback, and recovered
success must preserve exact events plus the complete identity/topology pair.

Exact physical append hooks and selected-backend-plan integration are now
implemented and compile in the native build. Compatibility batching copies
per-accepted-draw locators through capacity flushes and intervening state
records; Direct Present retains its locator until the actual physical append.
Publication verifies these bindings rather than reconstructing them. Retained
topology scopes surround ordinary, DCE and session backend execution, observing
the actual selected plan, successful Encode, registration and carrier folds.
No backend Encode runs inside an admitted attachment callback.

A source whose commands are all ProvenOmitted is admitted only with genuine
publication, submission and completion plus complete physical placement
coverage. True inline/no-submit completion remains unsupported; no synthetic
GPU lifecycle is added. Paired V4 inspection/packing and the owned V4 source
plan/cursor are implemented. Cursor completion proves ordered traversal only,
not runtime query readiness or pixels. The initial integrated regression
passed 79/80 Tape targets; correcting the V3 CLI's expected rejection for a
forged manifest made both V3/V4 CLI reruns pass. Queue/source-payload/routing
and existing TLA regressions pass 7/7 (TLA 204.39 seconds). Evidence is in
`/tmp/dxmt9-v4-hooks-{build,tape-tests,cli-recheck,queue-tla}.log`.
The subsequent admission build and serialized 84/84 regression pass after the
route-neutral provider entry, partial-session range correction and lease-gated
retirement fix (`/tmp/dxmt9-v4-admission-build-recheck.log`,
`/tmp/dxmt9-v4-admission-tests.log`). This includes 81 Tape targets, production
routing and both inline/offloaded query receipt tests. Provider callback
self-cancellation preserves the actual renderer HRESULT and ledger while
rejecting capture; stale replay callbacks cannot mutate detached frozen rows.
The final native build and 86/86 serialized regressions pass after permanent
V4-selection legacy-route cleanup and the shared exact-diagnostic executor
(`device_c_render_tape_execution_v4.*`). This includes 82 Tape targets,
production routing, inline/offloaded query receipts and TLA (202.45 seconds).
Logs: `/tmp/dxmt9-v4-executor-final-build.log` and
`/tmp/dxmt9-v4-executor-final-tests.log`. They do not qualify an original workload.

Provider execution has a tested route-neutral retained observation entry and
an exact authenticated General-plan matcher. The shared executor verifies
original event/record order, genuine raw ACK coverage and actual source/query
prefix receipts for exact-source diagnostics. It does not force captured
attempt segmentation or generate missing publication with an extra flush.
The General provider now has an explicit V4 exact-diagnostic mode, paired
pre-effect admission, normal-route observed replay, independent evidence and
cancellation before recovery. An admission-only spec covers both requested
routes, paired-input/digest errors, wrong exact owner, fresh-only reset and
competing-audit rejection. Nonempty GPU-backed successful finish, public CLI
execution, query-readiness and pixel qualification, and cross-policy semantic
mapping remain open; the latter is explicitly rejected before effects.
Legacy raw capture token fields must not be populated merely
to enable provider observation: they also select production routes. PE V4 arm
is now connected and fails closed instead of publishing a v2 fallback.
Permanent V4 selection survives cancellation, failure and rearm; legacy
capture planning/settlement cannot regain authority when its root is absent.
PE32, PE64 and canonical x86 Unix builds now pass, as does the install-name
audit. A final warning-only cleanup passes four focused binding/execution/
observation/routing tests. Actual capture20
(`experiments/output/app-d3d9-stkcop-bench-tape-h430-query-volume-20`)
uses Day/high/1280x720, 1,200 skipped Presents and a 1 GiB capture-only blob
budget. The previous color allocation failure is not reproduced: GPU snapshots
and V4 arm complete, but the first post-arm journal fails repeatedly at
`external_event_before_cut`. The game was deliberately terminated after
collecting that evidence; runner exit failure is not an unexplained crash.
No bundle was published. Its desktop-only screenshot is not a rendering oracle.
CPU scratch high-water is 748,576,168 bytes; a separate 5-second process
sampler reports peak RSS 2,396,348 KiB and VSZ 46,744,392 KiB. These are
different accounting domains, not additive ownership totals or proof of a
memory reduction. Precise cut rejection attribution is the next capture gate;
no actual STALKER V4 Tape, original-workload GPU/pixel oracle or repeated policy
timing is qualified.
Cold first-failure/cut-phase attribution now preserves the original rejection
site and qualified cursor through subsequent failures. The new provider and
diagnostic tranche passes a native full build and 86/86 serialized Tape,
production-routing and inline/offloaded query receipt regressions
(`/tmp/dxmt9-v4-cut-provider-native-rebuild.log`,
`/tmp/dxmt9-v4-cut-provider-tests.log`). This run does not rerun TLA or prove
nonempty V4 GPU execution. Existing CPU grammar tests needed explicitly
qualified `render_tape_runtime::detail` references after the new provider
header dependency; their tested predicates are unchanged.
Instrumented capture21 (`app-d3d9-stkcop-bench-tape-h430-query-volume-21`)
identifies the first rejection at `RenderTapeRuntimeIdentityCore::finishRecord`:
event 24, QUERY_ISSUE record 0, attempt 2. The subsequent BeforeEvent(25)
finds a revoked clock. Ordinary compatibility replay can dispatch the query
without calling the session-release API; that API itself returns without a
release when the session lane is disabled. The capture core nevertheless
requires a valid release for every coordinator. R-HARN-REPLAY-7.35 closes this
representation gap with explicit ordinary-serial versus session-release
authority. The implementation now records the immutable queue domain and a
positive same-lease ordinary admission before dispatch, without fabricating
release evidence or selecting a capture-only session route. The 264-byte V4
header, 208-byte coordinator and 448-byte paired-transfer header preserve this
distinction through validation, CLI inspection and source-plan authentication.
Exact diagnostics reject a known captured/local domain mismatch at arm, even
without coordinator rows. Native counterexamples cover missing, duplicated,
late and identity-mismatched admission, stale/replaced leases, session-only
release obligations, actual whole-raw and mixed Direct/coordinator replay,
query END source/baseline/initializer joins and topology-edge collisions.
The native full build and all 87 serialized Tape/routing/query-receipt tests
pass (`/tmp/dxmt9-v4-ordinary-native-final.log`,
`/tmp/dxmt9-v4-ordinary-tests-final.log`). Existing TLA models also pass
(`/tmp/dxmt9-v4-coordinator-tla.log`, 202.33 seconds); this is a production
model regression, not a new proof of the diagnostic authority sum. Canonical
PE32, PE64 and x86 Unix builds and the install-name audit pass
(`/tmp/dxmt9-v4-ordinary-{pe32,pe64,unix,install-audit}.log`). Actual capture22
no longer reproduces the event-24 query rejection, but does not
publish a bundle: the PE owner reaches its default 4,096-event limit while
materializing first-reference resources. The first arm records 643,308 event
bytes, 3,383 unique blobs / 312,137,476 blob bytes and 460 objects at rejection.
The original byte ceilings are not the rejecting predicate. Bounded capture-only
event/blob-entry count overrides now preserve the 4,096 defaults with 65,536
hard ceilings and unchanged byte budgets. Parser/default/clamp and real
one-past-boundary admission tests pass, including the full 65,536-event bound;
native build and all 87 regressions pass again
(`/tmp/dxmt9-v4-cardinality-native-final.log`,
`/tmp/dxmt9-v4-cardinality-tests-final.log`). Canonical PE32, PE64 and x86 Unix
builds and the install-name audit also pass
(`/tmp/dxmt9-v4-cardinality-{pe32,pe64,unix,install-audit}.log`). Capture23 uses
both 65,536 count overrides and reaches a different repeated admission failure:
`command_chunk_produced_pass_preflight`, NULL-format 2048x2048 RT texture and
its Surface alias, APPLY_STATE binding-only access. Alias/descriptor checks
pass but expected content is unsupported and produced proof lacks a terminal
access. R-HARN-REPLAY-7.36 now implements explicit descriptor-only authority for
the exact single-level NULL Texture2D RT and its generation-qualified Surface
alias. PE bootstrap/JIT and already-admitted references, canonical validation
and General provider preadmission check every use; samplers, mutations, copies,
readbacks and oracle uses reject. Standalone NULL and generic unavailable
content remain outside this authority. Original factories/routes are unchanged.
Native full build and all 87 serialized Tape/routing/query-receipt regressions
pass, including a matched Bootstrap RT-only positive and active-sampler negative
with exact rejection attribution (`/tmp/dxmt9-v4-null-native-final.log`,
`/tmp/dxmt9-v4-null-tests-final.log`). Canonical PE32/PE64/x86 Unix builds and
install-name audit pass (`/tmp/dxmt9-v4-null-{pe32,pe64,unix,install-audit}.log`).
This cold grammar extension changes no production scheduling/lifetime policy;
it is not a new concurrency or pixel proof. The original workload remains
unqualified pending a published bundle and independent GPU/pixel checks. Evidence:
`experiments/output/app-d3d9-stkcop-bench-tape-h430-query-volume-23/`.
Capture24 no longer reproduces that NULL binding failure, but stops at the next
initial-content gap: one-level 1x1 R32F Texture2D DEFAULT render targets first
referenced as shader inputs at slot 16. Their expected byte layout is accepted,
but content is incomplete and no arm snapshot exists; produced-pass authority
correctly rejects read-first access. The existing bounded snapshot shape must
not be bypassed by zero-fill, descriptor-only authority or an invented clear.
R-HARN-REPLAY-9.13 specifies exact R32Float arm capture/restoration through the
exclusive checkpoint lease. Descriptor/layout admission and physical raw
transfer support now use that existing lease; ordinary readback is unchanged.
Native shape/receipt/row-copy negatives and the real Metal 1x1/7x5 A/B/A oracle
pass, including signed zero, subnormal, infinity and NaN payload bits, pending
initializer-only capture without external flush, wrong physical storage,
active Direct and terminal rejection. The oracle runs 12 fresh raw-seed audits
and 12 separate same-device replays; warm results prove reset/route/pixels and
conservation, not unsupported warm raw-seed audit. Each capture source uses a
distinct Factory-owned backend so its deliberate terminal negative cannot
poison the next shape's queue. Native full build and all 88 serialized Tape/
routing/query-receipt tests pass (`/tmp/dxmt9-v4-r32f-native-final.log`,
`/tmp/dxmt9-v4-r32f-tests-final.log`, `/tmp/dxmt9-v4-r32f-testlog.txt`).
Canonical PE32/PE64/x86 Unix rebuilds and install-name audit also pass
(`/tmp/dxmt9-v4-r32f-{pe32,pe64,unix,install-audit}.log`). This synthetic Metal
evidence does not qualify the original STALKER input; original capture retry
and independent original-input GPU/pixel checks remain required.
Capture25 passes the prior resource-admission failures and reaches Present
completion, but final canonical validation rejects Texture object 4294967318,
generation 1, disposition `ProducedByCapturedPass`, expected content 0/0 as
`invalid-object-define` / `texture-descriptor-disposition` (first arm event
4132). Its schema/dimension/mip/subresource counts are accepted-looking, but
the logged detail does not identify the pixel format. Source review finds that
PE Produced admission accepts matching full-clear topology without checking
the parent RT usage bit; final validation requires that bit. The first-access
classifier can report a full depth/stencil clear as well as a color clear, so
the actual descriptor and accepted clear aspect need runtime attribution before
choosing a supported Produced policy. The rejection must not be bypassed by
removing the validator's usage check or weakening terminal-first write proof.
Capture-only diagnostics now retain the selected Produced proof and log its
parent/alias identity, actual level-zero descriptor, terminal record/attachment
section and actual Clear flags. Final rejection details expose level-zero
fields only after canonical descriptor validation. Native counterexamples
preserve depth-usage rejection, color-RT admission and the unresolved-write
obligation; full native build and all 88 regressions pass
(`/tmp/dxmt9-v4-produced-diag-{native,tests}.log`). Canonical PE32/PE64/x86 Unix
builds and install-name audit also pass
(`/tmp/dxmt9-v4-produced-diag-{pe32,pe64,unix,install-audit}.log`). These are
diagnostic changes only; the Produced contract is not widened.
Evidence:
`experiments/output/app-d3d9-stkcop-bench-tape-h430-query-volume-25/`.
No paired V4 bundle or original-input GPU/pixel qualification was produced.
Capture26's diagnostic-only retry identifies the exact rejected parent as
DF24 (875710020), DEFAULT Texture2D, DEPTHSTENCIL usage 2, 2048x2048, one mip/
subresource and no MSAA, with Surface alias 4294967319/generation 1/subresource
0. The selected first terminal record is a DEPTH_STENCIL Clear with ZBUFFER
only (`flags=0x2`, no explicit rectangles). PE's Produced witness is accepted;
final color-RT-only disposition validation rejects it at the corresponding
definition (first arm event 3969). Logical/physical DF24 aspects and clear
coverage must be qualified before any bounded Produced-depth extension.
R-HARN-REPLAY-7.37 now specifies that extension for exact DF24, usage 2,
depth-only flags 2 and an exact DEPTH_STENCIL alias witness. The shared policy
now connects PE bootstrap/JIT admission, canonical disposition and temporal
validation, and General-provider preflight. DF24 alias-parent comparison is
generation-qualified independently of the existing logical-slot helper;
ambiguous aliases, parent/alias mutations and pre-proof CPU reads reject.
Canonical and first-access native tests pass, including 5,184 descriptor
combinations, 75 topology combinations and temporal/identity counterexamples
(`/tmp/dxmt9-v4-df24-pure-fix1.log`). The initial provider oracle fixture exposed
the existing same-chunk direct-parent ambiguity restriction; it is being split
into alias-only production and later sampling chunks without weakening that
restriction; provider preflight now passes
(`/tmp/dxmt9-v4-df24-focused-fix2.log`). The first Metal oracle failed in its
constructed missing-RT0 draw case: all 35 depth texels retained the prior
0.125 value, with one submitted but zero encoded draws and zero draw passes
(`/tmp/dxmt9-v4-df24-gpu-diag.log`). Backend render-pass admission rejects a
missing RT0 before load/store setup; this is not a valid matching-merge
positive. Every admitted nonzero RT0, including NULL, differs from the Z-only
clear's zero color-handle key. The replacement oracle therefore uses actual
full-size/smaller color RT0 draws, requires encoded-draw evidence, and selects
the existing capture/provider exact-attachment-preservation contract for raw
readback. No ordinary renderer behavior is changed to satisfy the oracle, and
no valid matching Z-only merge or preservation-OFF proof is claimed. The valid
RT0 raw oracle reaches provider verification after its whole-image checks.
The route diagnostic then confirms exact output/source/digest and balanced
8-object creation/release for three Compatibility replays and the fresh
ProductionSlot request: the latter naturally takes one
`BeginLegacyPreEffectFailure`, with three compatibility draws and zero Direct
draws (`/tmp/dxmt9-v4-df24-gpu-route-diag.log`). No flush or boundary is invented
to manufacture Direct eligibility. The Metal oracle now passes with 18
whole-depth A/B/A overwrite checks, 12 actually encoded non-depth-writing
control draws, and six fresh/warm exact provider pixel replays. All three
ProductionSlot requests independently exhibit the named natural fallback,
strict output/source/digest equality, verified warm resets and 8/8 wrapper
conservation. Native full build and all 89 serialized Tape/routing/query
regressions pass (`/tmp/dxmt9-v4-df24-native-final2.log`,
`/tmp/dxmt9-v4-df24-tests-final.log`, `/tmp/dxmt9-v4-df24-testlog.txt`). Final
canonical PE32/PE64/x86 Unix builds and install-name audit pass
(`/tmp/dxmt9-v4-df24-{pe32-final,pe64-final,unix-final2,install-audit-final}.log`).
The raw observer explicitly selects the
existing capture/provider exact-attachment preservation policy; it does not
prove ordinary preservation-OFF behavior. Actual DF24 Direct execution and
original-input qualification remain unproved. Its
coverage authority is current canonical/backend stream semantics, not D3D9
viewport/scissor API Clear conformance. Source inspection confirms DF24 has
physical Depth32Float storage with no stencil aspect. The separate existing
DF24 sample-component documentation mismatch (prose versus replicated RGB
implementation) is outside this extension; the depth oracle must use raw
depth or sampled red only.
Evidence: `experiments/output/app-d3d9-stkcop-bench-tape-h430-query-volume-26/`.
The run was terminated after repeated evidence; no original bundle or pixel/
performance qualification was produced.
Evidence: `experiments/output/app-d3d9-stkcop-bench-tape-h430-query-volume-24/`.
The run was deliberately terminated after repeated failures; no V4 bundle or
original-workload GPU/pixel/performance qualification was produced.
No complete query/source/GPU/pixel qualification follows from
this partial progress. Evidence:
`experiments/output/app-d3d9-stkcop-bench-tape-h430-query-volume-22/`.
Both capture21 and capture22 were deliberately terminated after repeated
authentic failure evidence, with no published Tape or visual/performance
qualification.

The previous publisher profile was not route-faithful: capture plus a nonempty
output root enabled Arena/session replay and a 2,048-page queue before arm,
including startup raws. Capture18/19 report zero
DirectSlot raws and span leases, so they cannot qualify the normal default
DirectSlot route. Capture no longer selects streaming or enlarged queue storage;
the resolver truth table covers 105 configuration combinations. V4 selection
also suppresses legacy token-based DirectSlot/query-range demotion while
preserving snapshot drains, producer exclusion and real completion fences.
Partial transaction observation is therefore required for route-faithful
STALKER capture, not just future policy experiments. Exact-attachment
preservation is a separate rendering-action override and must be declared;
removing route overrides alone does not make all capture policy unchanged.
The publication bundle currently carries no authenticated queue-observed
attachment-preservation setting. A manifest declaration must come from the
capture binding's actual setting and interval-stability evidence, not a
publisher inference from environment variables. Legacy artifacts lacking this
declaration are unknown, not policy-neutral or preservation-disabled.

CPU shadow A now has owner-scoped immutable scratch extents with bounded
64 KiB I/O, atomic patch/rollback and leased retirement; GPU checkpoint B keeps
its independent bytes. This does not prove reduced live RSS or successful
STALKER capture, and it does not eliminate B's allocation pressure. Native
memory/fault coverage and actual capture evidence must be recorded separately.
Existing H343 artifacts do not identify the workload's volume format.
The existing mutation event carries complete volume mip blobs without a new
wire schema. Compressed PE/WoW64 slice pitch differs from packed native storage
and must not be inferred from logical height; the narrow shadow-layout fix and
native layout tests do not replace actual captured-content qualification.

### STALKER capture and parallel experiment admission (2026-09-20)

R-HARN-REPLAY-7.12/7.23 remain unqualified for STALKER. H342 R684 misses its
selected Present boundary; R685 reaches the boundary but publishes no Tape.
R686 info diagnostics identify pre-arm `registry_invalid reason=child_abort`
with no qualified resource/caller, followed by `present_output_admission`
rejection. The generic first-abort needs capture-only callsite attribution;
it does not prove a backbuffer failure or a volume-specific cause.
At this checkpoint, General provider query and volume execution were also
unsupported, independently of the capture failure. Those primitives now exist
and have synthetic Metal coverage in `render_tape_provider_spec.cpp`
(`nativeMetalCapturedQueryTapeOracle`, `nativeMetalVolumeMipSliceOracle`). The
remaining execution gap is their authenticated V4 source/coordinator/topology
join, not absence of the primitives. No STALKER serial/parallel oracle or
repeated performance sample is qualified. See
[H342](../../docs/perfomance/present-pacing/present-pacing-stalker-tape-admission.342.md).

H343 R688 closes first-callsite attribution: the pre-arm rejection originates
in `D3D9VolumeTextureImpl::LockBox`, whose successful volume lock deliberately
aborts capture. Failure-only caller literals survive the child facade; the
WoW64 stack-walk attempt was inconclusive and removed. Volume identity/layout
and complete capture/provider support remain open. Serial-run parallel shadow
zeros are unobserved (producer requires ExplicitParallel), not zero eligibility.
Both diagnostic runs were intentionally stopped after startup evidence and
are not successful wild or pixel tests. See
[H343](../../docs/perfomance/present-pacing/present-pacing-stalker-capture-origin.343.md).

### CPU timing representativeness (2026-09-16)

R-HARN-REPLAY-8.9 attachment authority is now separated from GPU readback.
General CPU execution skips only backend-owned shadow work, preserves Clear
publication/lifetime and seed writes, and rejects CPU read/control consumers
at admission. Unavailable/failed backend readback cannot return a stale seed.
The corrected ownership contract is mandatory in CLI/comparison receipts.
The 373,248-trace native refinement, production Clear/readback pins, fifteen
focused tests and all 196 TLA configurations pass. GT2 r621 reauthenticates
12 CPU verification, 96 measured CPU plus 12 warm-up, and 12 GPU exact
iterations: old ordered semantics and original pixels match; A/A shifts are
0.27–0.63%. This closes the bounded attachment-work mismatch, not application
FPS, asynchronous scheduling or the old software loop's speed mechanism.
See [authority evidence](../../docs/perfomance/mini-replay-bisection/attachment-authority.08.md).

The corrected r622 ten-pair matrix also reauthenticates 320 measured CPU and
40 GPU exact iterations. A/A brackets differ by -0.77%/+0.08%; Direct exact
is 3.48–3.69% cheaper than Compatibility, and 1MiB/64MiB is 1.10–2.97% cheaper
than exact in both orders. These are whole-iteration controlled-completion
observations, not isolated replay service time or default/FPS promotion.
1MiB has zero GPU denial/growth at 20.86MiB retained capacity; 5MiB retains
61.84MiB and has 56 optional denials per two GPU iterations without a CPU
benefit. Asynchronous owner pressure/pacing, repeated wild A/B and broader
tapes remain open. See
[corrected matrix](../../docs/perfomance/mini-replay-bisection/gt2-qualified-policy-matrix.09.md).

Corrected phase attribution r623 reauthenticates twelve fresh semantic
verification, 96 ordinary measured and 192 diagnostic measured iterations,
plus 36 warm-ups, using unchanged r622 images/input/policies and reauthenticated
GPU evidence. Direct exact saves 2.794ms in the replay envelope
(18.869→16.075ms mean); harness admission stays near 19.270ms. Ordinary
brackets repeat a 3.05–3.08% route benefit, but the 1MiB increment is not stable
(-0.52%/+0.16%). Diagnostic receipts remain excluded from policy ranking.
First-level setup/resource/replay/publication/completion separation is complete;
isolated production service attribution, warm-resource reset fidelity and
asynchronous/wild promotion remain open. See
[phase evidence](../../docs/perfomance/mini-replay-bisection/cpu-phase-attribution.10.md).

r624/r625 narrow source-driver ownership using unchanged images/input, six
external stack samples and existing counters; 434 total iterations (eight
semantic verification, 32 ordinary, 356 diagnostic, 38 warm-up) reauthenticate.
Snapshots and final Slot append dominate sampled production ancestry;
planning is smaller. Diagnostic clock overhead is visible, and the planner's
library-load environment gate requires an explicit pre-launch counter flag.
No optimization/default/FPS promotion follows. Complete exclusive wrapper/
production attribution, commit-closure timing, observer-off cost and live
asynchronous contention remain open under R-HARN-REPLAY-8.7–8.9. See
[cost owners](../../docs/perfomance/mini-replay-bisection/replay-cost-owners.11.md).

Pre-correction R-HARN-REPLAY-8.7–8.9 evidence: diagnostic-only exclusive
wall/process-CPU accounting separated fixture, admission, resource work,
replay, publication, controlled completion and reset. GT2 r620 reauthenticates
182 iterations; slow processes add about 88ms in replay, not completion/setup.
Unchanged-image r619 stacks concentrate in software attachment Clear. That
was a timing-fidelity gap, not a failed semantic/pixel oracle. The correction
above removes the mismatched work; the exact old process-speed cause remains
open. Sample percentages must not be subtracted to manufacture timings. See
[slow-mode evidence](../../docs/perfomance/mini-replay-bisection/cpu-shadow-slowmode.07.md).

The earlier 5MiB rejection below is historical: capacity ownership recovery
and the subsequent same-image matrix pass that bounded GPU policy. The
pre-correction matrix withheld CPU ranking; no headroom default is promoted.
See [policy matrix](../../docs/perfomance/mini-replay-bisection/gt2-policy-matrix.06.md).

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
