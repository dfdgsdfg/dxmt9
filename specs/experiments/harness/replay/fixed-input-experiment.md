---
type: "Spec Design"
title: "Fixed-input Render Tape policy experiments"
description: "Fast repeated production replay and explicit completion schedules with independent semantic and Metal oracles."
tags: [specs, experiments, replay, lifetime, verification]
---

# Fixed-input Render Tape policy experiments

This design targets `requirements.md` R-HARN-REPLAY-8.1–8.12. It is a new
experiment operation, not a change to the existing `provider-replay`,
`parallel-verify`, capture, projection, or default renderer policies.

## Ownership and execution boundaries

### Shared pass identity metadata

The production encode coordinator owns the first shared-table representation:
`ParallelPassReadIndices` projects independently resolved child accesses into
the sealed pass's canonical table during certificate validation. The table is
reconstructed by production code for each replay, not serialized as trusted
capture evidence. `parallel-verify` reaches this same provider/encode path;
CPU-only policy timing does not by itself demonstrate certificate or worker
execution. Nonzero selected/children/worker counters and exact output remain
required for a non-vacuous parallel GPU verdict.

Native `parallel_render_pass_spec.cpp` compares indexed membership, union and
alias decisions with independent exact sets over exhaustive short traces and
feeds source-derived coverage into the production certificate validator.
Internal index numbers are not replay identity; canonical resource identities
and read/write semantics are. Unknown identities, foreign table domains and
omitted child reads reject. The synchronous index borrow is not retained by
the certificate, workers or a future iteration. The selected next target is
Replay-built `PreparedEncodePlan` publication alongside the final Slot, not a
migration of execution coordination. This remains unimplemented. It must keep
alias stability, failure-atomic publication and source-generation qualification.
Encode retains final session/pass admission, child dispatch/join and parent
ownership. Plans must be reconstructed from authenticated raw input on each
fresh replay; precomputed plans must not bypass measured construction or serve
as their own semantic oracle. The harness
must count metadata preparation in the stage that executes it and include it
in end-to-end comparisons; a lower Encode time alone does not qualify moving
work onto a supply-limiting Replay thread.

Use identical input and source boundaries for the existing Encode-prepared
reference and the Replay-prepared candidate. Compare source-derived canonical
accesses and ordered ranges, not internal index numbering. Exercise metadata
allocation/capacity failures, stale identities, malformed offsets and partial
publication before GPU comparison. Timing includes preparation, final admission,
encoding, queue starvation/backpressure and retained plan capacity. Prebuilt-plan
Encode-only timing is a separate microbenchmark, not end-to-end evidence.

### Iteration ownership

The bundle loader owns authenticated, immutable event/identity/blob storage
for the whole experiment. An iteration owns fresh logical replay state,
resource identities, temporary pins, Slot transactions and submitted work.
The backend owns execution and completion acknowledgment. The result owner
collects cold diagnostic evidence; production draws do not gain a fat return
type, allocation, per-draw clock or always-on observer to serve this tool.

Private CPU input staging may request filesystem copy-on-write clones, but
must preserve independent file ownership: neither input nor staged mutations
may modify the other. Hardlinks and retained symlinks are not owned copies.
On macOS the wrapper requests cloning with `/bin/cp -c -R -L`; unsupported
filesystems may fall back to physical copies. Other platforms use `copytree`.
An existing destination is rejected, copy failures propagate through the
private artifact transaction, and normal digest/identity validation still
runs on the staged bytes. Clone requests do not certify physical space usage.

Pre-publication failures of populated CPU artifact transactions must retain raw evidence outside
the normal publication path. The wrapper attempts `.OUTPUT.rejected-*` with
an `accepted=false` marker; if quarantine allocation or rename fails, the
original `.OUTPUT.staging-*` remains and its path is reported. Marker-writing
failure must neither mask the original exception nor delete evidence. Even a
complete root receipt inside this unpublished directory is not a successfully
published aggregate; completed child receipts remain byte-preserved evidence.
Empty pre-evidence staging may be removed. Existing output paths must never be
overwritten. CPU receipt-write failures record attempted payload bytes and
pre-write free space where available; free-space reports are observations,
not admission guarantees. GPU/publisher transactions retain their existing
separate policy.

The atomic rename is the publication commit point. Cancellation during the
subsequent parent-directory sync propagates without treating the committed
output as rejected staging. CPU comparisons must independently require each
child's `execution_valid=true` and integer `provider_exit_code=0`; an
`accepted=true` label or valid nested evidence cannot override execution failure.

The input boundary remains PE canonical wire before Unix scheduling. It is
early enough to vary direct/compatibility construction and encode policy,
but cannot reconstruct PE setters already coalesced or original Wine timing.
Capture-specific full snapshots and chunk sizing are part of the recorded
workload; measurements cannot silently be extrapolated to a differently
segmented live workload.

The fast CPU path executes the same importer, replay/state transitions,
planner and final-Slot construction as production. It substitutes only the
backend execution/completion boundary and exposes the substitution explicitly.
Its semantic oracle compares an ordered projection of execution content,
not allocation addresses or a requirement that two policies use identical
physical Slot/run layouts. Input and output resource identities remain fully
qualified even when process-local handles differ between iterations.

Capture provenance and replay execution identity are distinct. Canonical Tape
chunks do not carry the original PE semantic-owner producer interval. When it
is absent, the existing production admission rule uses a provider-local replay
ordinal and an explicitly unqualified producer interval. The experiment must
report this identity domain, not manufacture a captured PE interval from a
queue sequence ID. Authenticated event/object identity still determines the
immutable input and semantic oracle. A supplied qualified producer interval is
preserved and validated through the existing production rules.

The historical provider route is not a forced compatibility baseline:
`allowDirectArena=false` still permitted ordinary direct Slot construction
when CPU-ready Arena planning was disabled. Existing `provider-replay` keeps
that historical behavior. The new experiment has explicit `compatibility`
(both direct paths suppressed) and `production-slot` (existing production
eligibility, including fallback) selectors. Qualification requires executed
path counters; a compatibility receipt with committed direct leases is an
error even if its pixels match. Opt-in candidate non-vacuity additionally
requires nonzero selected-path draws.

The prepared API reports the requested policy only. Metal receipts derive
`observed_replay_path` from committed direct draws and executed compatibility
draws (including unbatched UP/fan fallback): `direct-slot`, `compatibility`,
`mixed`, or `no-qualified-draw-path`. A successful pixel replay without a
qualified candidate draw is not policy-execution evidence. `--require-path`
is mandatory for candidate qualification, but clear-only reset fixtures may
run without it. Metal `pixel_deterministic` describes readback equality only,
not a proof of ordered semantic equality. Physical batching/fallback may vary
without violating semantics; each iteration preserves its actual counters.

The real Metal path executes actual encode/submission and waits for real GPU
completion before readback or reclaim. Its cache policy states whether device
and compilation caches are cold or deliberately retained. Existing strict
source/output oracles and any already-declared capture envelope remain
authoritative; experiments do not widen tolerances to make a candidate pass.

The wrapper freezes the inherited provider environment before execution and
records only renderer knobs plus the allowlisted Metal validation/capture/HUD
settings. `MTL_DEBUG_LAYER` or `MTL_SHADER_VALIDATION` values other than unset,
empty or `0` set `metal_validation_diagnostic=true` and prevent experiment
acceptance. Validation diagnostics may use either reset mode; only seed audits
require cold-device reset. This records instrumentation, not a new rendering
policy or permission to weaken the pixel oracle.

Bounded diagnostic attachment readbacks may select an encoded-Present interval
and finite readback count. Such local frame ordinals are selectors only, never
capture identity or completion authority. Original/replay pass comparisons
must independently join the retained source/encoder metadata to authenticated
source rows and reject missing/truncated coverage. Raw and summary-only
readbacks remain diagnostic even when final pixels match; they cannot authorize
low-observer policy ranking.

The capture owner must not equate a complete device-shadow bootstrap with all
resource-owned state. Unsupported managed-texture LOD mutations use the
existing persistent capture-registry rejection path, without altering normal
D3D behavior or extending the bridge ABI. Pre-arm rejection deliberately
remains invalid even if a later call restores LOD zero. Executed-state evidence
is distinct from `identity.bin`: logical pass/source provenance does not attest
actual sampler values or physical render-pass boundaries.

## Reset is an operation, not a loop counter

Each iteration starts from the same effective bootstrap and resource bytes.
Mutable render targets, dynamic buffers, texture writes and live object
registry entries must not leak across repetitions. Teardown/reset time is
reported separately. Resource conservation, no pending completion, exact
bootstrap/state and output equality witness reset. The experiment includes
an A-to-B-to-A input/reset control so constant-output fixtures cannot make a
broken reset appear correct. Warm-up uses the same checks as measured work.

Preparation and device/resource setup can be excluded from a replay-only
timing only when separately reported. Retaining PSO caches does not justify
retaining every mutable resource or CPU pin. Native CPU time, elapsed replay
service, encode, waits and GPU time are distinct quantities and are not
added when their scopes overlap.

Bounded CPU fixtures report provider input read/validation/plan preparation as
`import_ns` and any shared CPU-visible Metal device acquisition as
`shared_device_setup_ns`, both outside the iteration. Their per-iteration
`phase_ns` contains `setup`, `replay`, `observe`, `completion`, and `cleanup`.
Nested observation or completion pauses the caller's phase, so these are
exclusive wall components (`phase_accounting: exclusive-wall`,
`clock: steady_clock`). The inclusive `elapsed_ns` includes fixture/sink
teardown; the phase sum must not exceed it. Failed prefixes retain the same
available timing fields without becoming successful samples. Both CPU grammars
also sample per-iteration `native_cpu_ns` using the process CPU clock where
available (`native_cpu_clock: process`); unavailable clocks are explicitly
labelled `unavailable` with null samples. Process CPU includes helper threads
in that process and may exceed wall time; it is neither replay-thread CPU nor
a component to add to wall time. Backend encoding
is substituted in this CPU lane; no field claims Metal encode/GPU duration.
The general-frame executor retains its separately scoped phase observations;
the bounded exclusive accounting label must not be projected onto other modes.
Missing, malformed, negative, boolean-valued or double-counted bounded phase
evidence is ineligible for current CPU acceptance.
The bounded receipt declares `reset_policy: fresh-runtime`; both CPU grammars
declare `cache_policy: fresh-runtime` and whether a native resource device is
shared across iterations. Core/queue/runtime cache owners are recreated, even
when the CPU-visible Metal device lease is shared. This is not warm PSO reuse
or a claim that opaque driver caches are reset.

## Qualified low-observer measurement and CPU policies

R-HARN-REPLAY-8.7–8.9 add a separate `cpu-policy-compare` operation. It owns
one unchanged authenticated Tape and two versioned policy values. It first
executes detailed qualification for both policies and compares every ordered
draw observation, ignoring physical source boundaries. Only successful
qualification permits low-observer measurement. Each measured child is bound
to its own qualification receipt, input components, native/validator/loaded
image identities, policy, environment and reset/backend settings. Rebuilding
a provider invalidates the binding. Missing or changed evidence is a named
failure, never an implicit baseline run.

`dxmt9.render_tape.cpu_policy.v1` has exactly these fields:

| Field | Supported values / execution owner |
|---|---|
| `schema` | `dxmt9.render_tape.cpu_policy.v1` |
| `replay_route` | `compatibility` or `production-slot` |
| `draw_command_limit` | `0` or `3..64`; nonzero only for compatibility. Applies the existing queue DrawLimit policy, not a new wire grammar. |
| `slot_headroom_bytes` | `0` or `1..64 MiB`; positive only for production-slot. Applies actual empty-payload provisioning. |
| `aggregate_headroom_bytes` | Both capacity fields zero, or at least the per-slot value and at most `512 MiB`. Applies the actual aggregate retained-capacity lease. |

These are independent axes where the existing production route supports them;
ignored combinations are rejected. Production-slot does not honor the ordinary
compatibility DrawLimit, so a nonzero limit with that route is invalid rather
than falsely labelled a batching experiment. This interface does not yet add
deferred Direct publication across original raw events. All original event and
record order is retained, including coordinator commands.

Policy experiments use the admitted general-frame grammar, immediate
after-command completion, one pending submitted source and no payload-byte
auto-split, source oracle, delayed mutation or external completion schedule.
The controller may drain multiple Ready sources after replaying one raw. A
compatibility raw contains at most 64 records; nonzero limits below three are
rejected because the no-worker producer could exhaust its 31 inflight credits
before the controller regains control. A bounded exhaustive append/split model
pins this admission rule. The original richer lifetime qualification modes
remain available separately, unchanged.

Both run modes recreate the same runtime owners per iteration and share the
same CPU-visible native resource device. Measurement omits semantic hashing,
per-draw observations, resource closure byte proofs and observer phase clocks;
it keeps actual production imports, copies, locks, resource ownership,
submission identity, controlled completion and reset checks. Source command/
draw summaries and existing runtime perf counters remain enabled and reported:
this is low-observer service timing, not a claim of zero instrumentation.
Measurement phase fields are unmeasured, not zero-cost phases; only iteration
wall and process CPU time are cost observations. Semantic/pixel oracle flags
are false, and qualification provenance is separate from measured execution.

`cpu-experiment --run-mode diagnose --policy-lane` uses the same constrained
execution as measurement, but enables a cold controller-only exclusive wall
and process-CPU ledger. Its phases are fixture setup, admission, harness setup,
object setup, mutation, replay, publication, controlled completion, cleanup,
fixture teardown and remaining bookkeeping. Nested completion/publication
scopes pause replay; phase sums must equal ledger totals and fit within the
outer iteration clocks. Unavailable/regressing clocks reject the diagnostic
receipt. All failure artifacts are retained. This mode has `diagnostic=true`,
`accepted=false` and no semantic qualification; neither its timings nor a
receipt relabelled as `measure` may enter `cpu-policy-compare`. The ordinary
measurement lane keeps its phase clocks disabled. No runtime rendering policy
or source boundary is changed by diagnostics.

The resource-backed CPU fixture substitutes encode/completion and does not
provide GPU readback. `AttachmentContentOwner` separately selects CPU-shadow
or backend-command authority. The real provider uses backend ownership; old
recording fixtures default to CPU shadows. After successful general grammar
admission the CPU executor selects backend ownership before tape effects.
Core Clear/StretchRect shadow decisions use this authority, never readback
availability. The CPU grammar still rejects StretchRect, readback, CPU read
locks and ordered controls; seed mutations are complete, write-only uploads,
not requests for previous attachment bytes. The resource oracle hashes seed
receipts and ordered commands, not stale attachment shadows. No command,
snapshot, retention, publication or completion is elided.

General CPU receipts require `attachment_contents=backend-command-owned-v1`
and `gpu_readback=false` in verify, measure and diagnose. Missing or old
receipts fail both the CLI and policy comparison gates. This corrects the
software depth-Clear timing mismatch without pretending to render/read pixels.
Core readback of a backend-owned attachment must not fall back to its stale
CPU seed when real readback is unavailable or fails. Low-level seed locks
remain storage APIs, not a general post-render readback implementation.

Warm-up iterations follow the same reset policy, are individually validated
and retained, but are excluded from measured statistics. Failed/slow samples
remain in raw receipts. Actual direct/compatibility counts, source counts,
DrawLimit publications, provisioning/reuse/failure and aggregate denials show
which path executed. A policy difference alone is not evidence of an exercised
mechanism or a performance gain. Neither this CPU result nor the bounded
model proves real encoder/GPU execution, Wine arrival timing or concurrent
worker scheduling.

Run from the repository root, using a new output directory:

```sh
bash scripts/run_python.sh scripts/tools/run_dxmt9_render_tape.py cpu-policy-compare \
  experiments/render-tapes/gt2-v2-20260909-r4-capture64/frame-1292677716873100-1 \
  --reference-policy experiments/render-tape-policies/compatibility-exact.json \
  --candidate-policy experiments/render-tape-policies/compatibility-batch-4.json \
  --repeat 16 --warmup 2 --output-dir experiments/output/cpu-policy-batch
```

For capacity, use `production-exact.json` and `production-headroom-5m.json`.
Passing the same policy twice is the A/A control. Each policy uses a fresh
child process because the production knobs are cached at startup. The wrapper
retains two verification iterations and all `warmup + repeat` measurement
iterations per policy. `verify/{reference,candidate}/result.json` owns detailed
semantic evidence; `measure/{reference,candidate}/result.json` reports
`execution_valid=true`, `accepted=false`, and `semantic_qualified=false` for a
successful timing-only child. Only the parent comparison can qualify its cost.
Its receipt names and hashes each measurement and its own verification receipt,
records the warm-up split, and re-authenticates input/provider/loaded images
before publication. The two canonical policy files and raw child streams remain
in the output. Failed comparison aggregates expose no successful timing totals.

`dxmt9-render-tape-cpu-policy-spec` exhaustively checks bounded admission and
tiny-limit queue-exhaustion counterexamples. Compile-time assertions bind its
record/ring/headroom bounds to the actual grammar and production constants.
`dxmt9-render-tape-cpu-measurement-spec` pins verify/measure/verify execution;
`dxmt9-render-tape-cpu-policy-python-spec` and
`dxmt9-render-tape-cpu-policy-cli-spec` pin acceptance, stale/missing evidence,
warm-up failures and the public native-backed comparison workflow. Existing
`cpu-compare` lifetime/schedule modes remain separate and unchanged.

## Controlled completion

A bounded schedule identifies submissions and release points by iteration,
source/submission identity and action ordinal. Production resource-use,
retire and reclaim operations execute against this controlled frontier in the
CPU backend. The controller rejects completion before submission, duplicate
acknowledgment and frontier advancement over unresolved work. Small capacity
limits and explicit fault points exercise failed admission and rollback.

The Metal backend cannot use a synthetic frontier. A delayed acknowledgment
must be bounded by hardware-completed work; teardown cannot force-free an
in-flight allocation to satisfy a test deadline. Script exhaustion or a
blocked dependency has an explicit result. No hidden drain is inserted in a
lifetime experiment merely to reproduce the old conservative provider.

An external CPU completion plan names immutable Tape event ordinals and
event-local submission ordinals, never guessed queue sequence IDs. Its JSON
schema is `dxmt9.render_tape.completion_plan.v1`: `events_sha256`,
`events_count`, `repeat`, `replay_route`, `submits` (`event`, `submission`) and
`releases` (`at_event`, `event`, `submission`). The wrapper preserves that
request and emits a fixed 720-byte, versioned POD plan. Digest, route, repeat,
reserved fields, bounds and exact ordered submit/release coverage are checked
before runtime effects. Each event's local submission numbering starts at one.
No completion is invented for an event that publishes no physical source.

The runtime binds each planned locator to the real qualified completion source
immediately before submission. `RenderTapeCompletionPlanController` in
`src/d3d9/device_c_render_tape_completion_plan.hpp` receives a
`RenderTapeQualifiedCompletionKey` from the production queue owner; it does not
invent a queue sequence or source/storage generation. A release point is after
the named event's effects. Missing or extra submissions, stale identities, a
skipped release frontier, callback failure and unresolved end-of-script work
are failures, not implicit drains. The receipt authenticates the binary
schedule and records the planned event/local ordinal alongside the actual queue
identity and release event. `finishIteration()` succeeds only after the last
Tape event and every planned submit/release frontier are closed.

The focused `RenderTapeEventCompletionBinding` model and its native companion
cover the event-qualified binder around that controller: nine ordered
operations, two actual submissions, delayed ordered releases, a recycled
source index with new source/storage generations, terminal callback failure,
and finish only after all Tape events. The model's `pendingOwners` is adapter
pending-completion ownership, not an independent Pool residency pin. Pool
reclaim, GPU completion and Metal backing safety remain production/native
obligations; this bounded model is not a Pool/liveness proof and does not make
source-index reuse universally illegal before a callback. The older
`RenderTapeCompletionSchedule` model remains the smaller two-submission
controller model.

The native Pool composition case keeps completion monotonic while independently
releasing capture residency: advancing the completed stamp cannot recycle a
still-pinned old backing. Null-device tests qualify ring slot, content revision
and captured identity, not physical Metal bytes. The Managed mutation fault
case injects failure into the production queue reservation before backing
rotation; unchanged state and one actual apply on retry are separate checks.
Neither native case substitutes for admitting and executing those events in
the bounded Tape grammar.

### Multiple pending CPU sources

Increasing the harness pending budget is not permission to enter a production
wait with no encode/completion worker. Before constructing or publishing the
next source, the adapter must obtain a non-wait admission witness from the
actual queue control state, Tape reservation predicate and applicable inflight
limit. An exhausted dimension produces a named pre-effect rejection, not a
sleep, forced completion or a larger hidden capacity. A witness sampled under
the queue mutex is valid outside that lock only in the explicitly single-owner,
worker-disabled CPU fixture; it is not a production concurrency capability.
The admitted command range must also exclude operations that may wait for an
older resource use. Queue headroom alone is not such a resource witness.
Inspecting the emitted DrawRun after replay is too late to establish this
precondition: replay may already have entered a managed upload, rename or
resource wait. The adapter must establish both raw-command eligibility and
the live resource preconditions before entering the production replay call.

Each submitted source must own its completion identity, schedule, resource
closure and captured/live backing proofs as one unit. Completing A must not
clear or invalidate B's proofs. Full sequence, source generation and storage
identity must match the owner before invoking acknowledgment; a source index
alone is insufficient. A live-capacity qualification must also fill the actual
queue to denial without acknowledgment, show that denial has no side effects,
and admit one replacement only after one actual FIFO completion/reclaim.

The first distinguishing qualification publishes two nonempty real draw
sources A and B through each replay route, observes both actual pending
completions, and binds their full sequence/source-generation/storage identities.
Completion must remain FIFO: reject B-before-A, stale generation and duplicate
release before invoking a production acknowledgment. After releasing A, only
A's completion/reclaim obligations may close; B's source and referenced
backing must remain valid until B is released. The final drain must conserve
both sources and their resources. The existing completion-controller models
cover the ordered callback frontier; actual Queue/Pool and backing observations
are separate required evidence for this composition.

This native qualification is a prerequisite to general-frame CLI admission,
not an implicit change to `before-next-work` or its one-source mutation
contract. Exposing a larger general-frame window additionally requires
per-source retained proofs, command/resource admission, explicit release rows
and receipt/comparator validation on an authenticated Tape input.

## Qualification and failure

A cold general-provider diagnostic may omit the extra provider-wide drain
before a newly defined independent Texture/Buffer's complete initial seed,
only when the exact generation was defined after the last executed command
chunk and no command has executed since. Sparse carried state cannot refer to
that previously undefined independent owner. Surface aliases, unknown owners,
partial/repeated subresource seeds and any intervening command retain the
conservative drain. Distinct mip/subresource seeds each need their own complete
first-seed witness. Normal bridge lock/unlock, Initializer ordering and Pool
retention are unchanged. The bounded provider that pre-creates all resources
does not infer this event-order witness.

This is a diagnostic policy, not a renderer default or promotion receipt.
The shared decision predicate requires a bounded exhaustive event/native pin
before a GT2 execution. Receipts count retained and omitted drains and preserve
the unchanged source/output oracle, including failures. A changed image can
identify boundary sensitivity; it cannot by itself establish correctness.
Audit `complete` means every planned mutation was successfully inspected and
applied; `mutation_count` and `inspected_mutations` expose that coverage. A
later pixel rejection must remain rejected without erasing completed mutation
coverage. A mutation failure leaves the coverage incomplete.

A separate cold texture-prestage discriminator may physically initialize a
bounded, immutable candidate set before replaying commands. Capture can journal
an already-resident texture immediately before its first command reference;
provider-side creation at that journal position is not evidence that the live
renderer performed an upload there. This diagnostic tests that asymmetry only.
Candidates must be independent CompleteSeed textures, with exact full once-only
coverage of every subresource, no intervening CommandChunk between definition
and the final seed, no Surface alias anywhere, and no other generation using
the same wire object slot. Buffers, later ResourceMutation writes,
missing/partial/repeated seeds and unknown descriptors are excluded. Candidate
planning finishes before replay
resource creation/mutation or command effects; factory/device setup remains a
separately reported earlier phase.

Physical staged objects remain unresolvable until their original ObjectDefine
events transfer ownership into the active table. Original seed events consume
their exact planned identity/subresource/digest once without reapplying bytes;
all other events remain ordered. A checked Initializer flush/wait before the
first command establishes deferred upload completion, never synthetic GPU
completion. Immediate `replaceRegion` uploads to Shared backings require no
initializer submission. Receipts classify successful seed applications using
the actual Pool record's staging decision, not the D3D pool enum alone; their
immediate/deferred subresource sum must cover all consumed seeds. A deferred
path must wait on the published upload event even if an earlier owner flush
submitted that work already.
Both hidden and active objects must be released exactly once on every exit.
Object conservation counts provider-owned wrapper acquisitions/releases, not
all underlying Metal allocations or implicit device/upload resources.
Receipts distinguish staged objects/subresources/bytes, immediate uploads versus
deferred flush/wait evidence, logical activations/seed consumptions and leftover
staged owners. A zero-flush Shared-only experiment does not test deferred-blit
boundary removal. Native
exhaustive event-order checks and a real pixel fixture must precede the GT2
diagnostic. The operation remains explicitly unqualified regardless of pixels;
unchanged GT2 residuals close this discriminator, not justify wider admission.

Every result authenticates the fixed input, candidate binary, requested and
effective policy, backend/cache/reset modes and schedule. Each iteration
records executed work, fallback, oracle and conservation outcomes. Invalid
input/policy is rejected before effects; post-effect failure terminates with
the executed prefix and failure disposition, never a successful partial mean.

The completion gate includes a small non-degenerate draw tape, a lifecycle
sequence, an actual GT2 capture, direct/compatibility non-vacuity, reset/fault
negative controls, a model-code binding and a real provider readback. The
report states measured turnaround time rather than promising a universal
latency bound. Full wild tests remain final integration/promotion evidence;
they are not required for every local policy candidate.

CPU publication has two explicit policies: `per-command` preserves the original
after-each-CommandChunk adapter; `draw-island` holds consecutive draw-only Tape
events in the writing Slot and publishes after the last event of that range.
An immutable range plan is constructed after grammar admission, before runtime
effects. Non-command events and non-draw commands end the grouping opportunity;
the mode initially admits only bounded plain-buffer draws. It does not join
across mutation, destruction, state-only, Present or ordered-control events.

Every multi-event group must bind exactly one actual completion source to its
executed first/last event, event count and projected payload draw count. An
early Ready source or unexpected split is a named failure, not an invented
range attribution. The shared `renderTapeGroupedPublicationMatches` predicate
in `device_c_render_tape_publication.hpp` checks the observed range against its
immutable plan. Submission locators in the external completion plan name the
closing event; all earlier events still advance the controller's event frontier
without a synthetic submit. Receipts preserve `publication`, first/last event
provenance and the qualified queue source independently of PE producer identity.

The production
source-wide path deliberately rejects populated Direct capacity admission
before effects and retains the prefix for carrier-free compatibility fallback;
`allowRotation=false` is not permission for the harness to rotate implicitly.
Capacity evidence must first exercise that existing fallback and conserve
ordered semantics. A rotation candidate is a separate policy change with its
own publication/locality and lifetime proof, not a prerequisite harness fix.

The initial grouped experiment fixes both `DXMT9_DIRECT_SLOT_HEADROOM_BYTES`
and `DXMT9_DIRECT_SLOT_AGGREGATE_HEADROOM_BYTES` to zero and records those
overrides. `--write-capacity-continuation-fixture` emits seven then two draws
in consecutive command events; each draw has a distinct start vertex, so the
ordered semantic projection detects reordering. The reference executes all
nine through compatibility; ProductionSlot must show seven Direct draws,
two compatibility draws, one actual continuation capacity rejection, one
populated fallback and zero rotations. Counter values come from the queue's
existing exact counters, not general rejection reasons or fallback draw counts.
Both sources' provenance and actual projected draw coverage are verified;
no pixel or GPU evidence is implied by the null-backed fixture.

An external plan for this fixture submits at closing draw event 6 and Present
event 9, with delayed releases at destroy event 8 and closure event 10.
A plan expecting submission at the unfinished event 5 fails without a synthetic
submit/drain. A grouped request with no multi-event range rejects before effects.
The pending budget counts actual outstanding submissions, not a boolean
nonempty state; a separate three-source/two-credit negative pin checks this.

Receipt qualification independently inspects the authenticated Tape event
types and dynamic-buffer descriptors through the validator CLI. All completion
modes require positive source/storage generations, non-overlapping qualified
storage, command-only event ranges and in-range release coordinates. Preset
releases are checked against those input events: after-command at publication,
plain after-destroy at the next destruction or frame closure, and the dynamic
backing oracle at frame closure. An absent external plan requires an explicit
null plan digest. Candidate-reported coordinates alone are not authority.

## Bounded execution surfaces

The same-process Metal operation is exposed as `experiment` on
`scripts/tools/run_dxmt9_render_tape.py`. It stages and authenticates a private
input copy, retains failed stdout/stderr/results, and records the candidate
binary and loaded renderer images. `cold-device` creates a new factory/device
per iteration; `warm-device` retains compilation caches but performs the real
device reset and recreates mutable replay resources. An inclusive replay wall
duration is not thread CPU time: resource setup, encode waits and readback are
reported as part of that scope until narrower phase evidence is present.

The wrapper also reserves a new private `readback` directory. Each executed
iteration writes its actual source/output RGBA there, including oracle-failed
iterations. Relative filenames, sizes and hashes are checked against both the
files and the provider's readback evidence. Artifact I/O occurs outside the
reported replay service scopes. Missing or corrupt files reject qualification;
preserved pixels never turn a failed provider result into success. The native
experiment's optional `--readback-dir` rejects an existing destination before
device effects; historical `replay` output options are unchanged.

`cpu-experiment` is a separate, explicitly no-GPU operation. Its initial
closure is a single Plain-buffer canonical lifecycle fixture with production
import/replay/Slot/Pool execution, fresh fixtures per iteration, and bounded
after-command or after-destroy acknowledgments. One implicit output Surface is
metadata only, to preserve the validated Present oracle grammar; it is not a
CPU-rendered attachment. Unsupported resource families, partial mutations and
unproved draw shapes must be rejected before effects.

The CPU operation has two deliberately different resource backends. With
`--resource-backend cpu-no-gpu`, the test device is null-backed: the receipt
reports `shadow-only-no-gpu`, no MTLBuffer is allocated, and no GPU command is
submitted. This lane admits one non-dynamic buffer, one complete zero-offset
`Plain` mutation, one supported draw and the canonical unbind/Present/destroy
closure. Dynamic resources are rejected before effects. With
`--resource-backend cpu-metal-resources`, the fixture acquires a real test
Metal device and production `Pool::createBuffer` allocates the dynamic
`D3DPOOL_DEFAULT` backing as `WMTResourceStorageModeShared` (an actual
CPU-visible MTLBuffer), while the adapter still submits no command buffer and
performs no GPU completion. The receipt reports
`cpu-visible-metal-no-gpu-submit`.

The admitted dynamic two-mutation grammar is intentionally small: one dynamic
buffer descriptor, full-range mutation A with `Plain`, one draw that observes
A, full-range mutation B with `Discard`, one draw that observes B, then the
canonical unbind/Present and destroy/PresentComplete closure. `Plain` and
`Discard` are the recorded lock dispositions, not labels inferred from the
bytes. At mutation B, captured A requires retention only when A's qualified
submission is still beyond the completed waterline. If A is already complete,
reuse may be legal and the oracle must not demand a distinct backing. When
retention is required, `--require-backing-oracle` checks the production
revision advance, distinct backing and byte preservation; it is accepted only
with `cpu-metal-resources`. The grammar remains bounded to two mutations and
two draws, not arbitrary dynamic-buffer or GT2 replay.

The Managed allocation-fault diagnostic is a separate bounded closure over a
single non-dynamic `D3DPOOL_MANAGED` buffer. It requires
`--resource-backend cpu-metal-resources` even for the no-fault baseline: the
null handle cannot exercise the production Managed capture path. The provider
preflights this fixture as a Managed-only grammar and rejects a Default or
dynamic buffer with `managed-allocation-fault-requires-managed-buffer` before
effects. The fixture is generated by
`--write-managed-allocation-failure-fixture` and is otherwise an authenticated
Tape input.

`--fault managed-allocation` arms the actual
`ReplayOffloadQueue::failNextAllocationForTest()` seam. It then enters the
public `dxmt9c_buffer_unlock` path with the application lock still open; it
does not set `mutationOffloadPlanned` by hand or invoke a private mutation
helper as the test operation. The expected first result is
`D3DERR_INVALIDCALL` with receipt
`managed-mutation-allocation-rejected-pre-effect-public-unlock`, and all
pre-effect ownership remains unchanged: Pool content revision, rename-ring
size/active index, shadow bytes, queued depth/bytes, replay-drain queued
sequence and the open lock. A retry through the same public Unlock recomputes
admission, starts the real `ReplayOffloadQueue` worker and drains it; the
actual mutation applies exactly once.

This diagnostic is intentionally not a passing policy sample: the wrapper
records `diagnostic=true`, `accepted=false`, `fault_oracle_passed=true` and
returns exit code zero only when the fault oracle passes. The four fault
route/completion combinations are repeated 16 times. The no-fault baseline
uses the same Managed fixture, runs the four combinations at four repetitions,
and has one identical semantic digest across them. Receipt-field corruption
and an incompatible non-Managed fixture are rejected. This closes the bounded
Tape allocation-fault case only; it does not claim every allocator/capacity
dimension, arbitrary CPU grammar, or performance/promotion evidence.

By default the CPU adapter seals after each CommandChunk event and disables
Present pacing (`DXMT9_DISABLE_PRESENT_BOUNDARY=1`,
`DXMT9_SYNC_PRESENT_FLUSH=0`): there is no presenter worker or GPU. This is a
controlled publication schedule, not evidence of production Present pacing.
The opt-in `draw-island` mode alone covers cross-event Slot continuation;
its executed range witness and capacity outcome are mandatory evidence.
Core buffer lock/unlock
executes against CPU shadow bytes; physical Metal upload, DISCARD renaming
and backing generations are not established by a null backend. The semantic
projection flattens executed draws and resolves effective binding/uniform
content, mapping runtime handles to the original qualified wire identities.
Neither input bytes alone nor raw Slot hashes are semantic oracles.

`--fault before-submit|before-release` exercises cold adapter boundary failure;
`--max-pending 0` is a pre-submit budget negative control. The Managed fault
above is the only allocator-failure seam currently qualified, and it remains a
diagnostic rather than a performance result. Failure artifacts remain failed
after cleanup. The restricted CPU grammar does not claim arbitrary GT2 replay
or pixel correctness; a GT2 Metal replay and a small controlled CPU lifetime
experiment establish different evidence layers.

## Paired CPU qualification and general-input boundary

`cpu-compare` owns one authenticated input and two child artifacts:
compatibility reference and production-slot candidate. It applies the existing
child validity rules, requires actual Direct work in the candidate, and
compares each iteration's logical draw count, qualified resource conservation
and ordered semantic digest. It also compares the cumulative digest after each
actual final-Slot draw, reporting the first differing logical draw when
available. These cold observations do not add production draw timers or alter
Slot construction. `publication_event` names the observer's physical source
publication event, which may close a multi-event draw island; it is not an
authoritative original draw-event locator.

Both children retain failures and their own queue-identity traces. The paired
operation does not compare physical DrawRun/source counts or pretend a queue
identity is portable across providers. Matching repeats within one route do
not by themselves prove route equivalence. Preset completion policies are the
initial paired scope; external route-bound schedules and injected faults must
not be silently rewritten into a different experiment.

Extending to real GT2 CPU replay requires the shared general-provider object
factory/identity lookup/release and actual Pool-backed texture/surface records,
not increased limits around fabricated handles. GPU-aware mutation helpers
are a separate boundary: private attachment seed, initializer flush, offscreen
Present and readback must not leak GPU submission into controlled CPU
completion. The CPU consumer may substitute the Device upload/execute boundary
while retaining normal core lock/unlock transitions and canonical commands.
It must distinguish CPU shadow/seed-digest evidence from physical texture
contents. Canonical float-depth seed storage is not packed depth shadow or a
physical depth readback. Existing real-buffer backing/rename checks retain
their stronger scope; no CPU semantic digest may populate a pixel-oracle field.

Destruction before completion remains an essential retirement case, not a
reason to insert an implicit drain. A mutation that could overwrite an
unfinished source's backing needs a proved version/rename or an explicit
resource-qualified failure; unrelated pending resources must not be mistaken
for a whole-device dependency. General grammar admission, seed attribution and
semantic projection must be completed before this design claims GT2 CPU
execution. The existing bounded CLI does not gain that coverage by sharing
its factory alone.

The current native resource-backed component fixture uses actual Pool texture
and surface records, including texture-subresource aliases. A bounded upload
observer receives ordinary core texture lock/unlock callbacks without invoking
the initializer or GPU submission. Its limit is 16 observations of at most
64 KiB each; rejection is explicit and never silent truncation. This component
checks exact descriptor/layout association, unchanged queue state, A/B content
and read-only content preservation. It does not yet execute texture mutations
from a general CPU Tape, attest physical texture bytes, or qualify GT2.

### General-frame CPU execution

`--grammar general-frame` is a separate narrowing of the shared general
provider preflight; the default `bounded` grammar and its delayed lifetime
oracles are unchanged. It admits at most 2048 events, 512 definitions, 1024
mutations, 128 command/bootstrap chunks, 4096 command records and 256 MiB of
blob storage. The 64-record per-chunk bound is a capture-cadence limit, **not**
a queue-capacity proof. Bootstrap is state-only, with absent or identity gamma;
ordinary commands are DrawPrimitive, DrawIndexedPrimitive, float constants,
ApplyState, Clear and one Present. OrderedControl remains rejected.

This lane requires `cpu-metal-resources` and `per-command` publication.
Completion is `after-command` or explicit `before-next-work`. Its fixture pins internal draw-count/payload
splitting, early-prefix publication and CPU-ready Arena streaming off before
cached policy reads. Compatibility dynamically reserves one source; actual
Direct admission must have one exact whole-raw lease or use the production
whole-raw compatibility fallback. Immediate and one-source modes require a
quiescent queue before each replay; the qualified two-source mode below instead
requires an exact pending-owner FIFO and fresh non-wait writer admission.
After each command chunk, the controller publishes, observes the real final
Slot, submits the test-only null-CB completion record, acknowledges and
reclaims that source in immediate mode. No missing encode worker is expected
to make progress.

`before-next-work --max-pending 1` holds at most one actual submitted source across subsequent
`ObjectDestroy` events. It releases that source immediately before the next
definition, mutation, bootstrap, command or frame closure, using the shared
completion reducer and real FIFO acknowledgment/finish/Pool reclaim. The
adapter binds the actual source and storage generations before submission;
it must not manufacture a queue identity from the Tape event number. Work
entry remains quiescent. A second source while one is outstanding or an
unreleased terminal source is a named failure, not an implicit successful
drain. Failed cleanup cannot turn an unresolved schedule into acceptance.

The receipt records each actual release event, pending peak, deferred releases
and destroys with pending work. The comparator derives the expected release
boundary from authenticated input event types. Submitted buffer backing views
must remain alive and byte-identical through a wrapper destruction; their
retained-byte checks are reported separately. This is a one-source window,
not delay across mutation/rename, general overlapping execution or a physical
texture oracle. Those require additional resource-scoped admission evidence.

`before-next-work --max-pending 2` permits overlap only across an independently
inspected ordinary draw/state command chunk. The shared cold
`planRenderTapeCpuWindowSource` predicate requires 1–64 records, only ordinary
Draw/DrawIndexed/ApplyState/float-constant records, a lease-eligible production
plan with no coordinator/control/compatibility records, and exactly one owned
lease covering the entire raw. UP, TriangleFan and internal rotation shapes
are not overlap evidence. They remain ordinary draining boundaries rather
than disappearing from the Tape. Bootstrap must be state-only and emit no
source. Actual queue workers and replay offload must be absent; CPU-ready
Arena remains disabled, page size is 4096, and auto-publication limits are
pinned before the first device. Without mutations, draw/state execution uses
CPU shadow and captured bindings; no implicit map/upload/rename is performed.
The native adapter checks real writer/control/Tape/inflight state immediately
before replay. Capacity denial fails before that call; no worker is assumed
to rescue a blocked reserve.

The two-source release policy is explicit and FIFO. Every retained owner must
also be eligible before another eligible source may overlap it. A noneligible
source remains alone until the next work boundary, even when that next source
is eligible. An eligible third source releases only the oldest of two eligible
pending owners before replay; other work
(including Clear/Present, resource mutation and definition) releases all
pending owners first. ObjectDestroy retains pending owners and checks each
owner's physical buffer proofs. Frame closure drains in order. Each source
owns its own closure, schedule, qualified sequence/source/storage identity and
backing proofs; completing A never clears B's evidence. Mutation holds and
external completion plans cannot be combined with this mode.

The independent inspector emits `dxmt9.render_tape.cpu_window_plan.v1` with page size,
bootstrap-state-only evidence and one row per authenticated command event:
record/allowed-record counts, partition/lease eligibility, coordinator/control/
compatibility counts and exact lease spans. The wrapper recomputes eligibility
from those fields, derives the FIFO release events and pending peak from the
input event stream, and matches every source trace. It does not accept a
candidate-provided eligibility boolean. The execution receipt reports actual
`window_runtime` and per-destroy `destroy_windows` with pending submission IDs,
checked-owner counts and successful retention checks. Missing/mismatched
witnesses, stale identities, incorrect release order or incomplete cleanup
reject the run. This remains a controlled CPU lifetime experiment, not a GPU
pixel or workload-FPS proof.

The mutation-window qualification is a separate, explicit controller
option, not a change to either CLI completion preset without an oracle. It may retain exactly
one submitted source through one buffer mutation when the shared pure
`reduceCpuTapeMutationAdmission` witness authorizes either:

- An unrelated DEFAULT/non-versioned buffer, Plain write, and actual
  `Pool::mapWaitSeqId == 0`.
- A referenced DEFAULT/versioned buffer, DISCARD write, zero map wait, and
  valid captured backing membership for every pending binding of that handle.

Target lookup must use the exact Tape kind/generation/object identity. A
declined witness releases the prior source before invoking the real lock;
it must not enter a self-wait while withholding that completion. Another
mutation or any command remains a release boundary. NoOverwrite, managed
unlock rotation, texture/surface mutation and multi-source overlap are not
authorized by these two cases. Post-mutation checks compare the uploaded
active bytes to core shadow, require a new active backing for referenced
DISCARD, and verify every pending backing remains byte-identical until its
acknowledgment. The next draw uses the ordinary production snapshot resolver.

The 512-input native admission truth table and bounded action-sequence checker
bind this cold controller to its ordering contract; real Pool/map/upload
fixtures test physical bytes and both replay routes. They do not prove GPU
completion or turn model-only timing into a benchmark.

`cpu-source-project` creates a buffer-closure observation by replaying the
immutable input through the production compatibility route at least twice.
Each command's closure comes from `visitSourceCommandResources` over the real
final source, including inherited bindings, and maps back to exact live Tape
kind/generation/object identities. All repetitions must agree. The artifact
binds input component digests, inspection, provider/validator hashes, loaded
images and the reference receipt bytes. Setup or validation rejection retains
the copied input and a rejected receipt rather than erasing the experiment.

`cpu-experiment` and `cpu-compare --source-projection DIR` may consume that
artifact only with `general-frame`, `before-next-work`, `per-command`,
`cpu-metal-resources`, `max-pending=1`, repeat at least two and no fault or
external completion plan. A bounded binary oracle contains each one-based Tape
command event and its sorted buffer identities (128 sources, 512 identities
per source). Native replay must compare every actual source closure to this
oracle on every repetition and route before a later mutation can be held.
Matching is necessary, not sufficient: only the actual Pool/map/backing
witness above permits a hold. The receipt names the held mutation event and
identity; independent input metadata fixes the next permissible release and
the mutation counters. Each candidate's final and per-draw cumulative semantic
digests must also match the immediate-completion reference recorded by the
projection producer; agreement between two delayed routes alone is insufficient.
Rejected oracle bindings must not start native replay.

This is input-digest and qualified-object provenance, not a claim of complete
frame/pass identity-sidecar attestation. An existing `identity.bin` is validated
and bound with the other components; its absence does not invalidate resource
identities defined by the event stream. No raw handle table or caller-provided
"safe" boolean substitutes for the production resource visitor. The oracle
does not authorize multiple outstanding sources or change renderer defaults.

Objects use the shared provider factory and real Pool allocations. Core
texture lock/unlock reaches a cold, layout-checked upload callback, which
compares its bytes against the authenticated mutation blob without issuing a
GPU upload. Canonical float-depth seeds retain explicit logical receipts;
they are not packed-depth shadow. Buffer binding observations read actual
CPU-visible backing bytes and compare them against the core byte shadow.
Repeated byte proofs are cached only within a mutation-free object-lifetime
epoch; any mutation, definition or destruction invalidates that cache.

`final-slot-general-v2` selects each effective stream/index binding using the
encoder's UP precedence. Inline vertex/index bytes are observed from the draw
payload, not looked up as Pool buffers. Versioned bindings require a valid
captured snapshot belonging to the buffer's rename ring. Non-versioned
bindings use a separate cold live Pool view, never a fabricated snapshot.
Both routes compare physical CPU-visible bytes against the logical buffer
shadow. The live view is valid only inside this serialized observer epoch;
it grants no general concurrent raw-Pool access capability.

The receipt separates `buffer_snapshot_observations` and
`buffer_live_binding_observations`; `buffer_binding_observations` must equal
their sum, be positive and not exceed 17 per draw. Snapshot count may be zero.
The historical `retained_snapshots_after_destroy` counter now counts retained
byte proofs for either backing representation. Projection v1 receipts remain
historical artifacts; the current comparator rejects them rather than mixing
digest definitions. Unused inherited snapshot entries do not override UP
bytes or become additional effective bindings.

The versioned final-Slot projection compares resolved per-draw state,
uniforms, shader/declaration content identities, buffer bytes and qualified
resource seed receipts. It excludes allocator addresses, PSO cache handles
and physical source grouping. The receipt reports observation time and
completion/finish/reclaim time separately from replay service time.
`physical_texture_oracle` must remain false. Each
iteration destroys its core/cache owners while retaining the backend for
post-destruction Pool inspection; this is fresh-device cleanup, not warm reset.

Reset qualification uses the actual general executor on A, distinct B buffer
contents, then A again, sharing one Metal device across fresh core/queue
instances. Both replay routes and immediate/deferred presets must preserve
every A draw and final digest while distinguishing B. A native-only skipped
wrapper-release fault must leave observable Tape-owned objects at the owner
release boundary and fail the reset oracle. Recovery releases those objects
before core teardown but must not relabel the failed sample; a subsequent
fresh A must still match. Core/cache-held Pool resources are inspected only
after core teardown, not conflated with the earlier Tape-owner boundary.
`dxmt9-render-tape-cpu-reset-qualification-spec` owns this bounded control;
ordinary CLI repetitions still report their own actual cleanup oracle.

Texture-derived surface observations resolve seed receipts through the exact
immutable `parentTexture` identity and subresource in the surface definition.
Destroying the Tape's parent wrapper entry must not erase the surviving
surface's content attribution. The actual retained parent backing must match
either the live registry entry or the generation-qualified retirement
tombstone; an object-ID-only lookup is insufficient. This remains a logical
seed/command oracle, not physical texture readback.
`dxmt9-render-tape-cpu-alias-after-parent-destroy-spec` validates an actual
seeded texture and surviving render-target surface through both production
replay routes and completion presets. Moving only parent destruction preserves
every draw; changing the parent seed is distinguishable. A native-only
live-registry attribution fault drops the retired parent's receipt and must
first diverge on the draw after destruction, with resource cleanup still
valid. This bounded test does not claim same-object-ID redefinition coverage.

CPU receipts preserve the same allowlisted renderer/Metal environment as the
Metal experiment. The wrapper snapshots that environment and passes it to
the native subprocess. Nonempty `MTL_DEBUG_LAYER` / `MTL_SHADER_VALIDATION`
values other than `0` set `metal_validation_diagnostic`; native semantic
`execution_valid` may remain true, but policy `accepted` must be false.
Diagnostics retain stdout, stderr and receipts. A paired comparison or source
projection must not promote a diagnostic child. Environment-gate tests using
a recorded native receipt/provider double prove wrapper behavior only, not
the driver's behavior under validation.

The unchanged GT2 r4 bundle now executes through both CPU routes, including
all 1420 draws and 277 object definitions. Its paired CPU result does not
certify missing PE capture state or close the independent strict Metal pixel
residual. General delayed/multi-object schedules require a separate progress
and lifetime admission contract; an implicit whole-device drain is not an
acceptable substitute.

```sh
bash scripts/run_python.sh scripts/tools/run_dxmt9_render_tape.py cpu-compare \
  experiments/render-tapes/gt2-v2-20260909-r4-capture64/frame-1292677716873100-1 \
  --grammar general-frame --resource-backend cpu-metal-resources \
  --completion after-command --publication per-command --repeat 16 \
  --output-dir experiments/output/cpu-gt2-policy-comparison
```

### Stream-frequency capture and fold contract

The PE binding owner must retain stream frequency independently of each
buffer/offset/stride tuple. Fresh-device and Reset values are one; a tuple
rebind must not reset the frequency. StateBlock recording/capture/apply and
SWVP reads use that same authoritative frequency owner. Accepted frequency
setters enter the pending canonical stream category in call order rather than
bypassing the recorded stream through an immediate Unix setter. All full,
compact and semantic-owner wire emitters must serialize the exact value.

For existing D9C wire records, `frequency == 0` means omitted/preserve, not
reset-to-one. Production replay applies a nonzero frequency in the same
transaction as the stream tuple, with both covered by rollback. Snapshot
and batch caches must treat a frequency change as stable hot-state
invalidation: frequencies are not carried by `DrawBindingOverride`. Updating
only the live Device or a cached key without changing its reuse generation
does not prove per-draw equivalence. Both batch compatibility and encoder
key-only reuse must compare frequencies. Sparse replay must derive instance
count from the effective stream-zero frequency before choosing a direct or
compatibility submission route, using the same reducer as ordinary draws.
A folded snapshot preserves the previous known frequency when an incoming
stream row omits it, including an incoming FULL_SNAPSHOT. With no known frequency, the
fold must retain zero/unknown rather than invent fresh-device provenance.
New capture producers emit explicit values; this does not retroactively prove
that old tapes captured historical out-of-band frequency calls.

Explicit child mip generation belongs to the canonical GENERATE_MIPMAPS
command stream, not a duplicate ResourceMutation blob. Recorder-backed child
calls must preserve AUTOGEN/no-op rules, the existing explicit-call completion
fence and recorder-unavailable behavior. Record acceptance alone must not
settle an explicit call clean or replace its synchronous completion boundary.
A CPU-only replay can prove command
order and qualified texture lifetime, but cannot claim generated physical
texture bytes without an actual GPU oracle.

An explicit-call capture transaction must bind its canonical command event to
an authenticated Unix-issued source/range and the executed operation's
completion receipt. A PE-created synthetic source/sequence number is not a
substitute: identity validation requires complete source coverage for every
CommandChunk. The transaction must not execute mip generation a second time
merely to obtain queue identity. Flush/direct-call failure must not emit a
successful command receipt; capture failure after successful runtime execution
must abort only capture and preserve the application's completion and clean
state. Tracking outside active capture, or without a negotiated recorder,
must reject an unrepresented explicit AUTOGEN operation with a hidden mip
chain using `unsupported_explicit_mip_generation`, including before arm.
Disabled tracking, non-AUTOGEN no-ops and the direct path for a one-level
physical texture are unaffected.

The completion-preserving recorder route uses an isolated real canonical
chunk and its actual committed `{captureToken, eventOrdinal}` ticket. A
completion-only bridge must drain replay, retrieve that exact event's accepted
settlement, issue an ordered queue flush to release any parked open session,
and wait the existing queue tail, including earlier prerequisites. Merely
waiting on the completion CV cannot release a non-Present source's session.
Invalid tickets must not issue that flush or acknowledge any event.
The ledger's `expectedSettlement` query must not substitute a later capture
tail, acknowledge completion or expose stale output on lookup failure.
Capture-off/recorder-unavailable execution retains the direct path. A rejected
attempt may fall back only before canonical acceptance; after acceptance,
direct generation would risk double execution. The device-owned cold receipt
collector spans append and final flush, including automatic CapacityPost
commits. It records bridge acceptance before later PE bookkeeping can fail;
an empty final flush cannot erase it and a second acceptance is a conflict,
not permission to substitute the newest event. The preceding prefix flush
is outside this scope. Capture state owns the temporary collector pointer;
neither TLS nor an unconditional production collector is permitted.

The exact-event progress model checks two already accepted valid requests
under fair enabled bridge/queue steps. Its abstract parked completion prefix
cannot advance until the release fence is issued; each request must then
return success or explicit failure. Removing the release action must produce
a temporal counterexample even though the safety invariants still hold.
Malformed-request injection is checked by the separate safety configuration,
not assumed to stop starving a valid request. This conditional model does not
assert driver progress or replace a native production encode-loop wake test.

Exact settlement identity must survive post-encode physical payload retirement.
A valid strict singleton group transfers its value identity into the existing
generation-qualified queue receipt before detachment. Physical source/page
reuse does not settle the group. Only successful receipt completion may move
that identity once into the ordered settlement FIFO; finish publishes it after
the completed tail. Receipt-backed and resident group tails must enter that
FIFO in the same source order. Cancellation clears the obligation, stale
receipt generations cannot extract it, and finish must reject an untransferred
obligation. Multi-member groups retain their existing resident-tail policy.
This transfer changes no PE wire field or per-draw observer and requires no
per-source heap allocation; bounded receipt-entry storage owns the metadata.
An independent bounded transfer model must distinguish sequence completion
from exact-identity settlement: omitting identity transfer must prevent eventual
exact settlement even when the sequence waterline advances.

If capture fails after runtime acceptance, completion-only
`dxmt9c_device_wait_for_pending_work` drains replay and flushes the existing
queue without re-executing mip generation or acknowledging a capture row.
Success requires an unstopped, unpoisoned queue, no active construction or
unpublished commands, and a completed source waterline through the accepted
tail. This fallback preserves the application fence but cannot validate an
aborted capture. Genuine recorder/bridge errors remain errors. Failure before
the isolated mip append may retain the direct route only after the original
prefix flush succeeded.

The CPU executor must count actual final-Slot GENERATE_MIPMAPS commands and
compare that count with the authenticated inspector histogram. Delayed
completion tests must attempt Pool reclaim at the actual completion waterline
after wrapper destruction and verify that the qualified texture remains alive
until its source completes. Route and completion-policy changes must preserve
the ordered semantic digest; this is not a physical mip-texel oracle.

## Commands

Use the existing authenticated capture bundle without repacking its identities:

```sh
bash scripts/run_python.sh scripts/tools/run_dxmt9_render_tape.py experiment BUNDLE \
  --output-dir OUTPUT --replay-route production-slot --partition-mode identity \
  --reset warm-device --warmup 1 --repeat 16 --require-path
```

Run `compatibility` with the same input as the reference. `serial` and
`parallel` are explicit partition selectors; selection is not eligibility or
parallel-execution proof. Existing parallel-provider counters and oracles remain
necessary to qualify those candidates.

For the deliberately narrow no-GPU grammar:

```sh
bash scripts/run_python.sh scripts/tools/run_dxmt9_render_tape.py cpu-experiment BUNDLE \
  --output-dir OUTPUT --replay-route production-slot --completion after-destroy \
  --repeat 16 --require-path
```

Compare the same admitted CPU input across both actual replay routes:

```sh
bash scripts/run_python.sh scripts/tools/run_dxmt9_render_tape.py cpu-compare BUNDLE \
  --output-dir OUTPUT --completion after-destroy --repeat 16
```

Use `--publication draw-island` for the grouped capacity fixture. Each child
retains its own native result, provider streams and input identity. The parent
reports the first differing iteration/draw, rather than replacing a failed
child with a passing retry. Physical source layouts may differ legitimately;
the semantic comparison does not claim pixel correctness or FPS equivalence.

Generate and run the grouped seven-plus-two capacity fixture:

```sh
build/tests/native/backend/dxmt9-cpu-ready-production-routing-spec \
  --write-capacity-continuation-fixture CAPACITY_SOURCE
bash scripts/run_python.sh scripts/tools/run_dxmt9_render_tape.py pack \
  --events CAPACITY_SOURCE/events.bin --blob CAPACITY_SOURCE/blob.bin \
  --output-dir CAPACITY_BUNDLE
bash scripts/run_python.sh scripts/tools/run_dxmt9_render_tape.py cpu-experiment \
  CAPACITY_BUNDLE --output-dir OUTPUT --replay-route production-slot \
  --publication draw-island --completion after-destroy --repeat 16 --require-path
```

Use a fresh output path for each run; repeat with `--replay-route compatibility`
for its fixed-input semantic reference. An external plan may replace the
completion preset, with the closing-event locators described above.

To require the dynamic backing-byte and conditional-retention oracle while
using an authenticated external completion schedule:

```sh
bash scripts/run_python.sh scripts/tools/run_dxmt9_render_tape.py cpu-experiment BUNDLE \
  --output-dir OUTPUT --replay-route production-slot \
  --resource-backend cpu-metal-resources --require-backing-oracle \
  --completion-plan PLAN.json --repeat 16 --require-path
```

Generate and run the bounded Managed allocation-fault fixture:

```sh
build/tests/native/backend/dxmt9-cpu-ready-production-routing-spec \
  --write-managed-allocation-failure-fixture MANAGED_SOURCE
bash scripts/run_python.sh scripts/tools/run_dxmt9_render_tape.py pack \
  --events MANAGED_SOURCE/events.bin --blob MANAGED_SOURCE/blob.bin \
  --output-dir MANAGED_BUNDLE
bash scripts/run_python.sh scripts/tools/run_dxmt9_render_tape.py cpu-experiment \
  MANAGED_BUNDLE --output-dir OUTPUT --replay-route production-slot \
  --resource-backend cpu-metal-resources --fault managed-allocation \
  --completion after-destroy --repeat 16 --require-path
```

The preserved qualification artifacts are the four route/completion results
under `experiments/output/fixed-input-harness-20260909/`
(`cpu-managed-fault-{compatibility,production-slot}-{after-command,after-destroy}-r7/result.json`),
with the focused native log at
`native-managed-fault-r3.log`.

CPU repeats are bounded to 1–32, pending completions to 0–32. Metal warm-up plus
measured repetitions are bounded to 64. Each command writes an authenticated
private input copy, raw provider stdout/stderr and `result.json`, including
failed/timeout results. Compare only matching input, binary, reset and policy
receipts; these measurements exclude original PE/Wine supply timing.

CPU and GPU experiment staging share `copy_input_owned` in
`scripts/tools/run_dxmt9_render_tape.py`. On macOS it requests independent
APFS copy-on-write files rather than duplicating every immutable blob for each
policy child; other platforms retain a deep copy. Hard links and borrowed
mutable input files are not ownership substitutes. The destination must be
absent, source links are dereferenced, and clone/copy failures propagate before
validation or provider effects. The staged manifest, blobs and oracles still
undergo the same hash reauthentication; storage sharing never relaxes pixel or
policy acceptance.

## GPU qualification coupled to CPU policy comparison

The external completion-plan options described above apply only to the CPU
lane. GPU qualification uses real GPU completion, never synthetic schedules.

`cpu-policy-compare --gpu-qualify` inserts a strict GPU qualification gate
after both CPU detailed verification receipts and the cross-policy identity
pair binding, and before any `measure/` child is started. It launches one fresh
`experiment` subprocess per canonical `dxmt9.render_tape.cpu_policy.v1` value,
with `--warmup 0 --repeat 2 --partition-mode identity --require-path` and an
explicit `--gpu-reset cold-device|warm-device`. The wrapper sets all three
cached knobs exactly from the policy before process startup:
`DXMT9_DRAW_CHUNK_COMMAND_LIMIT`, `DXMT9_DIRECT_SLOT_HEADROOM_BYTES`, and
`DXMT9_DIRECT_SLOT_AGGREGATE_HEADROOM_BYTES`. The CPU verification provider and
Metal experiment provider are independently fingerprinted; shared runtime
modules (excluding each provider executable itself), validator, input and
non-policy renderer environment must still match.

Qualification requires an authenticated original `output.rgba` and
`source.rgba` in the unchanged staged bundle. A real capture must declare
`scope.production_capture=true`; a fixture may instead declare
`scope.synthetic=true` or `scope.test_input=true`, and is reported as
`synthetic-test-input`, never as a production capture. `pack --test-input`
sets the latter marker without creating production capture authority. The
coupled gate admits single-interval frame tapes only. Each provider result
must carry top-level `oracle_mode=strict`, `expected_digest_matched=true`, and
`source_oracle_exact_matched=true`; every interval must carry output/source
readback and SHA-256 values equal to the original captured bytes. The
provider-owned per-iteration RGBA artifacts are authenticated by path, size,
hash and exact bytes. Pixel-envelope diagnostics,
Metal validation diagnostics, missing/tampered readbacks, zero selected-route
draws, GPU command-buffer errors, failed completion/reset/conservation, policy
or environment mismatches all reject the gate.

The coupled parent receipt records `gpu_qualification` child paths and hashes,
`qualification_scope`, and re-authenticates GPU provider/validator/image
identities at commit. A failed GPU or pre-measurement identity gate starts no
measurement child. Final reauthentication failure invalidates the aggregate
while retaining already-produced measurements as unqualified evidence.
CPU-only invocations retain their existing
CPU-only receipt semantics and `gpu_evidence=false`.

The standalone acceptance predicates live in
`scripts/tools/render_tape_gpu_policy.py`; they are also the predicates used by
the coupled wrapper and its native-free negative tests. This lane is evidence
of strict readback qualification only; it is not a GT2 promotion claim until
the real captured GT2 run passes.

## Capture source-boundary discriminator

Compilation has a separate, explicit equality boundary (R-HARN-REPLAY-8.15).
The production unix WMT factory pins the same MSL2.4/fast profile with explicit
invariant vertex-position source attributes for Wine and
native replay, independently of executable SDK metadata. Compiler receipts
record the actual option fields and carry them through the library/function/PSO
chain. Historical receipts with nil options do not prove effective-language
equality; their immutable original pixels remain the strict oracle. A
same-text executable-SDK control can identify this missing input, but promotion
requires the normal provider using the fixed profile, unchanged original tapes,
zero pixel mismatches, and the existing public CPU/GPU qualification join.

Original intermediate readbacks must be joined to the published Tape's actual
source identities, not a guessed encoded-Present interval. Capture retries may
make an otherwise valid encoded-frame selector disjoint from the final bundle.
Such readbacks remain diagnostic artifacts and must not be used as paired
original/replay evidence. Capture-local frame IDs are not global frame numbers.

SegmentSerial's batch FrameGraph assigns event-local pass IDs. The capture
ledger must namespace each nonzero ID by CommandChunk event before emitting
frame-wide `identity.bin` IDs. Same-event segments carrying the same local ID
must retain continuity and identical DAG index/kind; different events must
never alias solely because their local ID and metadata match. Event-local zero,
overflow, changed same-ID membership, non-exact record coverage, or mixed ID
scopes within an event must fail closed. The trusted frame-ID append API keeps
its existing semantics and must not silently reinterpret caller authority.
`RenderTapeProductionIdentityLedger::appendEventLocalPasses` owns this cold
conversion under its existing mutex; no queue/wire payload or renderer order
changes are required. `dxmt9-render-tape-capture-spec` exhausts a small namespace
and checks the serialized, settled ledger plus failure boundaries. This is an
identity-value refinement, not a new concurrent transition or pixel proof;
existing lifecycle TLA and concrete GPU oracle evidence remain separate gates.

The bounded color hash observer records only completed, unconverted tight GPU
bytes, with size/format/row-pitch metadata and SHA-256. Hash-only output retains
no raw image and requires an explicit finite count; it bounds disk use, not
GPU work or timing effects. Capture-only selection registers each validated
Arena source while the publication lock still excludes Ready consumption,
then matches owner, seq and the full generation-qualified source/storage ref
at the encoder. It adds no fields to PE wire, source payloads or queue tickets.
The diagnostic registry has fixed capacity and is cleared for a destroyed
queue owner. Lost coverage, rejected identity, unsupported route or table
collision must never count as equal intermediate output. Successful source
registration is not successful Tape publication: failed-attempt observations
must remain outside the final sidecar join. Hash equality can localize matching
pass endpoints; it cannot recover raw bytes or exclude transient differences
within a pass. A divergent endpoint requires narrower byte evidence before
attributing a draw/shader cause. All these inline observers invalidate policy
qualification independently of pixel equality.

For tightly packed BGRA8/RGBA8 Metal readbacks, color endpoint metadata may
add separate raw channel SHA-256 digests and their explicit storage-channel
order. They must hash the same completed bytes without sRGB conversion, alpha
normalization or tolerance. Unsupported formats/extents must report no channel
witness. This diagnostic distinguishes RGB divergence from an unused X8 alpha
channel; it does not certify channel liveness and must not replace the original
full-byte oracle or qualify a policy run.

Color endpoint metadata must distinguish a matched effective Store action from
an unknown, discarded, resolve-only or unsupported multisample source. The
observer pulls the final action from the real attachment ledger after late
resolution; it must not force Store or split a pass to manufacture evidence.
Stored-content qualification is necessary, not sufficient: it does not prove
initialization of every pixel/channel, canonical cross-process attachment
identity or live-out use. Missing historical store metadata is unknown, not
implicitly Store. Raw bytes may remain diagnostic artifacts, but such unknown
or discarded endpoints must not be treated as semantic equality/divergence.

Uniform-byte localization must distinguish the immutable expected payload from
the actual tracked binding backing. Tracked upload/cache state alone does not
prove the actual encoder bind. A cold direct-buffer witness must separately
observe successful Metal vertex/fragment buffer and offset calls at slots 0/3,
invalidate on setBytes, and compare process-local non-reused object identity
plus offset to the tracked binding at the selected draw. Fresh, null, inline,
missing-origin and mismatched bindings must not become a match. This is a
direct-binding receipt, not argument-buffer, GPU-time contents or residency
proof. Native tests must include a deliberately skipped rebind with stale
pixels as well as A/B/A, distinct-buffer and offset-only cases. The existing
three-draw production oracle must require matches on its direct routes.
The cold binding observer may inspect only
bounds-checked shared Metal slices after successful upload/bind; it must not
rebuild expected constants and call them bound bytes. A direct-binding shadow
must be reset on actual encoder creation before carrying clean bindings across
draws. Missing first binding, unsupported storage and unknown extent are missing
evidence. Vertex-slot-5 observations must record the bytes passed to Metal, not
reconstructed draw parameters. This CPU-visible witness does not prove GPU
execution, private storage or parallel-child coverage. The A→B→A independent
Metal oracle must additionally validate pixels and actual route coverage.
No execution payload or disabled-path state may be added solely for the observer.
Capture-only effect observations must reuse the registered full source identity
at the actual encoder entry, without requiring GPU color readbacks. Missing,
stale or colliding provenance must suppress observations, never widen to all
draws. The published sidecar, not attempt-local registration, owns the final
cross-process join.

Issued indexed-geometry diagnostics must distinguish source bytes from the
actual Metal-call buffer/offset and the last successful stream-0/DrawVolatile
bindings on that encoder. Track only cold diagnostic state, reset it on real
encoder creation, and clear selected draw identity before filtering each draw.
Suppressed/fake Metal calls, absent bindings, unreadable/private storage,
overflow and bounded-read limits must not produce a known byte witness.
Compare exact index and referenced stream-0 backing ranges under the issued
base-vertex/volatile parameters, not the live logical resource's newer bytes.
For declared programmable extra streams, observe actual Metal slots 6–20 and
invalidate a buffer witness on setBytes; never reuse stale buffer identity.
Use issued indices/base-vertex or instance IDs/divisors for bounded ranges and
label declaration-derived stride as expected metadata, not selected compiled
PSO reflection. A visible fixture must read the extra stream, use a nonzero
binding offset and change pixels when only those bytes change. Empty or unknown
hashes cannot count as equality. This is CPU-visible binding evidence, not proof
of GPU reads, expanded/non-indexed draws or parallel children. It must not add
fields/arguments to queue, draw or PE-wire execution payloads.

Actual raster diagnostics must observe successful viewport, scissor,
rasterizer, pipeline and blend/stencil-reference calls on the real encoder,
not reconstruct those values from the next draw's logical shadow. Cold
encoder-local storage must reset at encoder creation and distinguish absent
bindings from zero-valued bindings. Issue rows must share the selected draw's
source/ordinal, including non-indexed draws, and preserve exact floating-point
bits. A pipeline label identifies an observed binding only; it does not prove
the selected compiled shader source or descriptor. Suppressed calls, rejected
draw filters and missing capture provenance must not create issue evidence.
These diagnostics must not mutate renderer state or qualify timing/pixels.

Selected-PSO provenance must bind the actual successful Metal source-library
creation to the actual named function and conventional render-pipeline creation,
then join that immutable receipt through the PSO used by the real draw call.
The receipt must serialize concrete descriptor fields and full source SHA-256,
not desired cache keys or a later mutable label. Object-scoped diagnostic
associations must not retain Metal resources or survive object destruction;
conflicting associations are permanently unknown. Bound diagnostic storage and
reject missing/unpublished/unsupported lineage, including binary-only libraries,
specialized functions and tile/mesh pipelines. Tests must authenticate the
receipt file and source bytes, pin used function names/descriptor fields and
verify that real draws without creation receipts remain unknown. Driver-internal
binary selection and later GPU reads are outside this construction-input proof.
Keep the existing strict original-pixel gate and all retained counterexamples.

Depth/stencil diagnostics must bind successful Metal state creation to the
actual depth compare/write and front/back stencil descriptors, then snapshot
that creation evidence only after the real encoder state bind. Record actual
stencil-reference calls separately. A new encoder is unobserved; an explicit
null bind is not a carried prior state. Missing or conflicting creation remains
unknown. No renderer cache key or desired state may stand in for the actual
descriptor. Keep one bounded value-only snapshot per object/encoder, without
resource retains, and use the existing cold binding/geometry gate. Join emitted
rows to the same selected source/draw identity. Native Metal pixels must cover
depth rejection, stencil rejection and distinct front/back descriptor values,
plus null/off/missing-creation negatives. This is construction/binding evidence,
not depth/stencil attachment contents or proof of the driver's later tests.

Sampling descriptor observers must track actual Metal fragment texture/sampler
calls and actual argument-encoder writes, not reconstruct a binding from Pool
or shader expectations. A direct null bind must invalidate the preceding value.
Argument-table evidence must name the actually bound buffer slice, reject
unreadable/out-of-bounds or unobserved changed bytes, and invalidate old entries
before a later observed write can bless an unobserved raw mutation. Reopening
a table or replacing slot 30 with bytes must invalidate its old witness.
Snapshots must not retain Metal resources. Keep descriptor storage bounded;
eviction, missing creation and observation failure yield unknown, not defaults.
Serialize exact sampler LOD float bits and actual texture view descriptors.
This proves issued descriptor inputs only, not cross-process resource identity,
texture contents, shader ABI selection, residency or eventual GPU reads.

A same-process texture-content join may use a nonzero monotonic observation
token attached to the actual Metal texture object. Bind observation and the
texture selected for readback must obtain the same token while the objects are
live, then preserve the token in cold output. Tokens must not retain Metal
objects or be reused after destruction; zero means unavailable. A token is not
a Tape identity or comparable across processes. Pair it with authenticated
source/pass/draw provenance and an explicit no-writer interval before attributing
readback bytes to a sampled input. This still does not prove sampled texels or
GPU reads, and cannot repair missing original evidence.

Color endpoint extent selectors must intersect source, attachment and pass
selectors before admitting a GPU readback or charging its bounded budget.
Width/height are positive exact subresource extents, not resource identity.
Malformed, zero or overflowing selectors must disable observation instead of
widening the scope. Same-input pixel and empty-output negative tests must pin
this diagnostic-only selection.

Capture-only draw-texture observations must apply the same full source witness
before deduplication/budget admission, so uncaptured startup frames cannot
exhaust the budget needed by the published interval. This scope requires
bounded per-pass collection; malformed or unbounded selectors are rejected.
The cold completion metadata must preserve owner, source/raw ordinal and
source/storage generations independently of later registry reuse. A missing
witness cannot be repaired with a nearby frame's texture. Effect logging and
color endpoint readbacks are not prerequisites for this texture-only scope.
Independent native pixel cuts and a two-dump budget with earlier uncaptured
draws must verify noninterference and exact source selection.

The normal reference contract is distinct from this diagnostic
(`R-HARN-REPLAY-8.14`). Its versioned policy must explicitly bind capture-source
cuts and route to a shared authenticated `CaptureSourceReplayPlan`. The pure
range planner borrows the full raw's handle table and payload arena, adjusts
only its local planning header/record span, and returns raw-absolute record
locators. Lease/segment ordinals describe that local range, not a new imported
raw. Exact capacity is recomputed for each range; non-additive derived capacity
dimensions must not be summed from unrelated whole-raw plans.

At execution, a Direct lease settles before its source boundary is published.
An ordinary non-Present publication returns its actual local source metadata;
a terminal Present owns its own publication; state-only completion does not
manufacture a source. The expected Direct source/storage/sequence must match
the current writer before it can be flushed. Capture provenance and private
raw ownership are bound separately from queue-local publication metadata:
`CpuReadyTape` rejects repeated nonzero raw ordinals at publication, even when
each new writer has an empty span witness. No generic high-water relaxation is
permitted to make capture cuts pass. One raw owner, wrapper/snapshot retention,
and drain journal survive all ranges. Source-local pre-effect fallback must
never invoke the whole-raw compatibility fallback after an executed prefix.
Sequential bounded admission may fail-stop; this is not a guarantee of
completion under allocation failure. CPU execution must drain through its
controlled completion seam and must not wait for an absent encoder worker.

The shared execution entry is
`device_c_chunk_replay.cpp` / `replayPrevalidatedResolvedCommandChunkBySources`.
Its cold publication scope observes actual Ready publications (including
implicit command-limit cuts and Present) under the queue mutex, copies bounded
receipts, and invokes the source consumer only after releasing that mutex.
Receipt storage admits at most 4096 records per raw and 8193 receipts; it is not
queue capacity. Bootstrap retains its whole-chunk route. The CPU executor
reauthenticates the plan against the exact tape/sidecar before object effects,
checks each actual Ready identity against the receipts, and drains the
no-worker queue outside the publication callback's locked observer. Its replay
timing excludes nested controlled completion/observation, but still includes
bootstrap replay. A normal GPU callback does not add source-local waits.

### Controlled sampled-input intervention

A cold general-provider causal diagnostic may replace one complete, single-level
2D R32F texture after one authenticated captured-source settlement. It must
require an exact source-plan digest, kind/generation/object identity, source
locator and immutable input SHA-256; validate all of these and the bounded full
extent before replay effects. The source must exist and precede another source.
The CLI is `--audit-texture-input GEN:ID:AFTERSEQ:SHA256:PATH`, with positive
decimal identities, lowercase SHA-256, an owned nonempty regular-file payload
bounded to 64 MiB, and 2D dimensions bounded to 4096 each. Descriptor format
uses the D3D wire domain (`R32F=114`), not the backend `core::Format` ordinal.
The wrapper stages a separate `texture-input-audit.r32f`, records the request
and provider command, and retains populated rejected transactions if artifact
publication fails. Observer directories must remain outside the final output
directory so they cannot pre-create the atomic publication destination.
The intervention may neither modify the Tape/original oracle nor become a CPU
or GPU policy candidate. Every receipt layer must retain its diagnostic status;
forged wrapper flags must not hide provider-level intervention evidence.

The callback runs outside the queue lock, with the raw owner still retained.
It drains preceding work, snapshots the target's actual bytes, uploads the
replacement through the existing resource path and reads it back before the
next source is admitted. One-shot or upload/readback failure is fail-stop; it
must not continue with an unknown partial intervention. The unchanged default
has no new renderer observer, queue state or PE/unix ABI. Pure admission
negatives and actual Metal readbacks bind this diagnostic to existing source
settlement/completion semantics; no new interleaving policy is introduced.

Every causal experiment must pair replay-produced bytes (same-path control)
with original-produced bytes at the identical locator. A changed image under
both inputs can instead be caused by the added drain/upload. A successful
original-input intervention is localization evidence, never proof that the
unmodified renderer has been fixed or permission to replace its oracle.
A large-contrast input may test sensitivity, but neither that response nor a
pool-selected view readback proves every actual Metal binding or sample address.

`experiment --source-boundary-mode capture-source --reset cold-device` exposes
this normal route for either `compatibility` or `production-slot`, using the
bundle's authenticated identity. It is exclusive with the seed/source-cut
diagnostic modes; the controlled sampled-input intervention may consume this
same source contract but remains unqualified. Normal policy execution rejects
unbound v1 policy knobs. The exact five-key `dxmt9.render_tape.cpu_policy.v1`
schema keeps its historical chunk-boundary meaning. The seven-key
`dxmt9.render_tape.cpu_policy.v2` schema adds `source_boundary_mode`, which must
be `capture-source`, and a nonzero lowercase `source_plan_sha256`. This digest
binds the native plan's domain/version, original tape and identity digests,
and ordered capture-owned event/source/sequence/chunk/record-range rows. It is
not a queue authority. Both native CLIs must reject a mismatching expected
digest before replay effects. The public wrapper forwards it through
`--source-plan-sha256`; omitted expected digests permit only unqualified normal
GPU investigation, never policy ranking.

`source_replay` records full source settlements and actual local Ready
publications, as well as the plan digest and completed, published, empty and
Present-bearing counts. The gate must reconstruct the plan digest from the
rows and authenticated input components, validate exact range coverage and
fresh local source/storage/sequence identities, and reject missing, duplicate,
reordered or truncated receipts. Counts alone are insufficient. CPU and GPU
queue-local identities need not equal one another; the shared capture plan
must equal. Verification, GPU qualification and low-observer measurement each
retain their own policy-bound receipt. Source evidence under v1 is rejected,
including nonempty evidence hidden behind a false request flag. Exact original
GPU readbacks and final input/binary/environment reauthentication remain
mandatory; diagnostic receipts cannot be converted into normal evidence.

`experiment --audit-source-boundaries --replay-route compatibility
--reset cold-device` is a cold general-provider diagnostic. It requires the
original authenticated `identity.bin` with Capture authority and completed
event settlements. The provider revalidates the sidecar against its owned
tape/catalogue, plans all event-local source record intervals before replay,
and executes them through the existing record-range replay transaction and
production sink. Every successful source range requests queue publication; state
continues across ranges and no source-local GPU wait is added. Bootstrap and
resource/control event ordering remain unchanged. Fresh provider-local queue
identities are never relabelled with capture seq IDs.

The optional repeated `--audit-batch-cut EVENT:RECORD` adds a CPU batch flush
before a zero-based record within that event (at most 4096 locators). It requires
the source-boundary diagnostic. Locators must be sorted and unique, target a
non-UP draw, and lie strictly inside an authenticated source. All locators
validate before any tape effect. Subranges share the source transaction;
the sink flushes pending DrawRun storage at each subrange end, while source
publication remains outside that loop. No explicit encoder end or GPU wait
is added. Requested/completed batch cuts are distinct from publication counts;
observed physical pass starts must be compared to exclude an indirect change.

The trace is `unqualified input -> validated exact cover -> ordered range
effects -> publication request -> next range`. Missing/overlapping/reordered/zero
ranges or an altered tape digest reject before effects. A failure after the
first effect is terminal and does not fall back to whole-chunk replay. A
state-only range with no pending queue commands completes without manufacturing
empty GPU work. Allocation exceptions close the active transaction journal and
preserve the completed-range prefix. Native
exhaustive coverage checks and malformed/stale sidecar tests bind this bounded
ordering contract; existing queue lifecycle TLA models cover the reused publication
mechanism, not equivalence of pixels across different physical cuts.

`source_boundary_audit` reports requested/validated/complete and requested/
completed source counts independently of the pixel oracle. `complete` means
all source ranges and publication requests executed; it does not certify the GPU
pass count. The existing effect trace joins actual physical pass starts by
issued draw order to test whether the requested cuts reproduced the original.
Source count is not physical pass count. The option is mutually exclusive with
other audits and is excluded from GPU policy qualification even when its
readback matches. It must neither replace original RGBA nor widen tolerance.

### Paired pass-end depth diagnostic

`DXMT9_DUMP_COLOR_ATTACHMENT_DEPTH_PAIR=1` adds one cold depth copy only when
the existing bounded hash-only color observer selects a pass end. It requires
`AFTER_DRAW=0`, a path/directory and `MAX_DUMPS` in 1–256. The same selector and
budget own both outputs; there is no second scan, forced Store, pass split or
publication. Missing depth, unresolved/discarded Store, logical stencil, MSAA,
non-2D/mipmapped depth or mismatching extents produce no depth witness.

The supported copy is the complete tightly packed native float32 depth image:
plain Depth32Float (Metal format 252), or only the depth plane of logical
D24X8 backed by Depth32Float_Stencil8 (Metal format 260), using
`MTLBlitOptionDepthFromDepthStencil`. D24S8 and Depth24Unorm_Stencil8 remain
unsupported; there is no D24 packing, shader conversion or numeric conversion.
Metadata records the actual native format and whether depth-plane extraction
was used. The sibling `.depth.bin` and
`.depth.json` retain byte digest/extent, source and storage generations, depth
handle, and the paired color metadata filename. Captured source provenance
must be the same frozen pass witness used by the color observation. Native
GPU tests must compare independently expected depth bytes and unchanged color
pixels, and cover disabled, disjoint-selector and malformed-policy absence.
No new TLA model is required for this diagnostic: it changes neither ordering
nor lifetime policy. It does not prove depth contents at a particular draw,
the first differing draw, or final-pixel fidelity, and cannot qualify a policy.

### Bounded actual-draw vertex position probe

An explicitly enabled, diagnostic-only probe may clone the actual selected
conventional PSO to observe final clip-space positions. It requires an exact
vertex-source SHA-256, output directory, 1-32 draw budget, 1-8192 vertex bound
and a finite indexed triangle-list count. Initial eligibility is one instance,
zero base vertex/instance and known bounded CPU-readable indices. The source
must be the supported generated `dxmt9_vs` entry, with an unused slot29 and one
terminal return; unsupported/ambiguous shapes reject before changing bindings.

The probe instruments only the selected draw, never globally replaces its
shader family. It restores the original PSO before the next draw, does not
split a pass, and invokes the original draw exactly once even when preflight
fails. No fallback/retry is permitted after issuing Metal work. Readback storage
is immutable to the CPU after admission and pinned to the actual command-buffer
completion. Unknown/failed completion must not produce a successful witness.
Atomic per-vertex position min/max bits and invocation counts handle duplicate
indices without racing ordinary writes and expose conflicting observations.

Receipts retain original/instrumented source and PSO identities, actual source/
ordinal and capture owner/source/storage generations, geometry bounds, output
digest and completion disposition. Replay-local identities require a separate
validated capture-source mapping. A directory is diagnostic even when named
`0`; forged non-diagnostic metadata cannot make the run eligible for GPU policy
ranking. The disabled path adds no draw-payload fields or per-draw allocation.

This is a shader/PSO perturbation, not a passive proof of historical GPU reads.
Pure eligibility/source-rewrite tests, actual-Metal pixel calibration, rejection
and restore/exactly-once/completion tests precede a bounded wild capture. The
existing completion/lifetime model still applies; no new scheduling policy or
general shader-invariance proof is claimed. Calibration of a synthetic or
standalone fixture cannot qualify an original GT2 pixel failure.

The optional `skinning-inputs` detail is a closed generated two-bone skinning
shape, additionally bounded to 16 attempts and 2048 vertices. It materializes
each of the six selected bone rows and four projection rows once in shader
locals, then uses those exact locals in both the original expressions and the
observation. It must not label a later reread as the operand previously used.
It records 23 four-word fields: final position, four decoded vertex inputs,
address registers, six bone rows, four projection rows, half-pixel correction,
geometry address, skinned position, three blended rows and pre-half-pixel
position. The separate input-probe schema specifies 92 min words, 92 max words
and one invocation counter per vertex. Labels, extent, identities and source
digests are mandatory; malformed or ambiguous generated shapes reject before
effects. This is compiler perturbation with earlier operand evaluation, valid
as equivalent only for immutable inputs; retain a baseline calibration and
explicitly exclude retrospective original GPU-memory proof. Independent Metal
input/value and pixel fixtures precede a new original-context capture. No new
queue/scheduling policy or numerical invariance proof is introduced.

### Digest-selected arithmetic intervention

`DXMT9_PROBE_NO_CONTRACT_SOURCE_SHA256` is a diagnostic-only compiler control,
not a renderer policy. A comma-separated selection of one to eight distinct
original lowercase source digests selects only bounded generated vertex entries
accepted by the position probe's unique-entry, unused-slot and terminal-return
guard; the guard's observer is not emitted. Before library creation the unix
owner may prepend `#pragma clang fp contract(off)` without changing
shader inputs, geometry, binding ABI or submission order. Unknown shapes,
existing pragmas and preparation failures retain the original source before
compilation; compiler failure must propagate normally. Actual compiled source
and PSO lineage must describe the transformed bytes, and the diagnostic log
must bind the original and transformed digests. No source file replacement or
shader substitution is allowed after compilation.

Any nonempty request, including malformed `0` or a digest that never matches,
taints the public experiment/qualification receipt independently of other
observers. Duplicate, malformed or over-bound selections disable transformation
without losing diagnostic classification. This control is an explicit arithmetic
intervention, not a claim that all generated MAD instructions should change
semantics in production.
Pure source-transform negatives and an actual selected-library Metal fixture
precede retained-Tape trials. TLA is not the arithmetic oracle: this control
changes no queue ownership or ordering. Historical input/output bytes remain
mandatory, and an intermediate depth repair does not qualify a final pixel.
