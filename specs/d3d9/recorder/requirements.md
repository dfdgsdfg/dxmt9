---
type: "Spec Requirements"
title: "PE Recorder Requirements"
description: "State-domain, transaction, identity, observer, and verification contracts for the D3D9 PE command recorder."
tags: [specs, d3d9, recorder, pe, command-chunk]
---

# PE Recorder Requirements

This topic refines the parent D3D9 hot-path contract
(`R-CORE-3.1`–`3.8`, `R-CORE-11.1`–`11.18`) for the PE recorder. It preserves
the ownership and POD boundaries in `R-ARCH-1.*`, `R-ARCH-2.*`,
`R-ARCH-3.*`, `R-ARCH-6.*`, and the immutable-source lifecycle in
`R-ARCH-7.11`–`7.24`; the producer-ownership discipline in
`R-BACK-43.4`–`43.7`; and the evidence ladder in `R-VERIF-1.5`–`1.8`,
`R-VERIF-6.4`–`6.6`, and `R-VERIF-7.1`–`7.4`.

## 1. State domains

**R-CORE-REC-1.1** The PE recorder must represent ordinary D3D9 mutable state
in three distinct domains: `LiveShadow`, the authoritative app-visible value;
`PendingDelta`, the partial set not yet durably represented by an accepted
command chunk; and `StateBlockRecorded`, the candidate tracked-key set and
captured values owned by an active state-block recording, fixed when End
succeeds. A dirty bit, backend shadow, or state-block value must not become the
source used by ordinary D3D9 getters.

**R-CORE-REC-1.2** Every recordable state category must have one declared key
space, value shape, live-shadow owner, pending representation, state-block
capture policy, and wire section or explicit cold-call disposition. Categories
must not be conflated merely because their indices share an integer
representation; render-state, texture-stage, sampler, transform, binding, and
constant keys remain kind-qualified. The StateBlock recorded-category enum,
fixed storage, Apply preparation role, Apply commit visitor, clear visitor, and
candidate COM lifetime visitor must derive from one authoritative 26-row Apply
physical inventory: four keyed stores, sixteen fixed stores, and the VS/PS
float/integer/boolean constant stores. Adding a physical row must not compile
while its Prepare, Commit, clear, or applicable lifetime behavior is omitted.
Every integer entering a keyed recorder boundary must first produce a bounded,
category-specific value type. Raw construction is closed; an out-of-domain
factory result is invalid and the whole operation must fail before mutation,
retention, or partial pending consumption.

**R-CORE-REC-1.3** `BeginStateBlock`, `EndStateBlock`, `Capture`, and `Apply`
must preserve the category table in [spec.md](spec.md): Begin establishes an
empty recorded domain only after its ordering flush succeeds; repeated writes
record the last value for each tracked key without mutating primary live state;
End fixes the tracked set; Capture refreshes values without enlarging that set;
and Apply replays exactly the fixed set through the normal state-transition
owners. Enumerated prior-value operations such as `MultiplyTransform` instead
update primary state while recording and do not enter the tracked set. Invalid
or failed Begin/End/Capture/Apply calls must not partially publish a new
recorded domain. An End failure before backend entry preserves Recording; once
backend End has been entered, backend or PE-wrapper publication failure must
leave Recording, discard the unpublished candidate, and poison the recorder so
PE cannot retry against a unix domain that may already have ended.
The common record-append envelope must test poison before negotiation,
capacity accounting, or either flush edge, so child-originated writers such as
`Query::Issue` cannot bypass the setter gates. Reset/ResetEx recovery remains a
non-append operation and must still reach the successful-reset transition.

## 2. State algebra

**R-CORE-REC-2.1** Outside state-block recording, an ordinary successful write
of the current effective value must be idempotent: it must not create a second
semantic transition or bridge operation. If a key is already pending, a
same-value write may retain that pending obligation but must not duplicate it.
Inside state-block recording, the first explicit Set must establish the tracked
key even when its value equals `LiveShadow`; later writes to that key remain
last-write-wins.

**R-CORE-REC-2.2** Successful writes to the same key must be last-write-wins
before the next semantic boundary. Successful writes to independent keys must
commute in effective state and may be serialized in canonical key order. The
only exceptions are explicitly ordered commands, binding/alias lifetime
transitions, state-block Begin/End rules, barriers, and API operations whose
result depends on the prior value, including `MultiplyTransform`.

**R-CORE-REC-2.3** A failed validation, normalization, allocation, append,
seal, bridge commit, capture preparation, or observer action must not silently
consume `PendingDelta`, alter `LiveShadow`, enlarge `StateBlockRecorded`, retire
an identity, or report success unless that effect is explicitly the D3D9
contract for the failed call. Retry after a pre-effect failure must observe the
same semantic input as the first attempt.

## 3. Record and chunk transactions

**R-CORE-REC-3.1** Appending one record must be atomic across its header,
payload, handle entries, local handle identities, retainer acquisitions, and
the exact `PendingDelta` entries represented by the record. The pending entries
may be marked as candidates during preparation, but they must be consumed only
after `commitRecord` succeeds. Any append failure must roll all components back
to their pre-record checkpoints. Capacity settlement is a separate edge: a
failed pre-capacity flush leaves the new record unattempted, an emitter or
`commitRecord` failure rolls the active record back, and a failed post-capacity
flush occurs after the new record and its represented pending tokens have been
accepted into the builder. The last case must not reintroduce or append the
same pending token a second time.

**R-CORE-REC-3.1.1** Raw byte append and overwrite primitives are builder
implementation details and must not be callable from production record
producers. Every variable-size producer callsite must instead use a closed
typed adapter for its exact POD section/tail, or a category-qualified byte-tail
adapter for schema-declared constant/UP bytes. The adapter must validate kind,
element size/count, alignment, and bounded byte arithmetic before copying;
pointer-bearing or unregistered types must be rejected at compile time.
Every public typed section/tail/table adapter must roll the active record back
on any rejected precondition, even when its caller ignores the returned
`false`. SetConst and Clear tails must match their already-written fixed
headers; final sparse table overwrite must match the active draw header and
validate canonical descriptor order, schema, alignment, sequential byte
ranges, and embedded constant ranges before it can leave a committable record.

**R-CORE-REC-3.2** Sealing must be deterministic and retry-stable. Repeated
`seal` calls without a successful commit or explicit discard must expose the
same bytes, record count, handle count, and retained-object obligations. A
failed pre-effect seal must preserve the builder, pending capture lifetime, and
retry inputs; it must not reset or advance the warm-retainer epoch. Once the
PE/unix commit call is entered, a failure whose pre-effect disposition is not
proved by the unchanged ABI is effect-unknown and must poison/fail-stop the PE
recorder while retaining the sealed projection for Reset/teardown cleanup. It
must not be retried as though the unix side were known not to have replayed it.

**R-CORE-REC-3.3** A successful bridge commit must settle in this order:
accepted canonical command; capture materialization/admission when enabled;
pending command-chunk lifetime release and each admitted `ObjectDestroy` at
most once, with child aliases before their parent; then builder reset and warm
retainer epoch advance. Capture rejection may disable or abort the diagnostic,
but must not change an already accepted application command or its HRESULT.

**R-CORE-REC-3.4** Record rollback, chunk discard, device Reset/ResetEx, and
device teardown must use explicit dispositions. Record rollback restores only
the active record checkpoints. A healthy Reset/ResetEx first submits its
queued command chunk at the ordering boundary; a poisoned Reset/ResetEx uses
discard recovery because the earlier entered bridge effect is unknown. A
successful chunk reset preserves bounded warm pins. Discard drains logical
pending capture references without journaling a command-dependent destroy and
releases all physical pins. No disposition may rewind an older accepted record
or leave a logical or physical reference owned by a discarded builder.

**R-CORE-REC-3.5** Production-used, host-buildable bounded projections must
compose semantic pending/write acceptance with append/capacity disposition,
the concrete builder record ordinal and counts, immutable seal projection,
bridge/capture disposition, pending-reference drain,
alias-before-parent order, builder reset, warm-epoch advance, explicit discard,
and successful Reset recovery. The state-write projection's exact-value witness
must qualify a pending token by semantic category, key, value, and ordinal.
The generic append/commit envelope may bind only acceptance, ordinal, counts,
and disposition when an emitter does not expose that semantic tuple; it must
not manufacture category/key/value from a wire record type or byte-size hint.
The render-state, texture-stage-state, and sampler-state scalar emitters must
always validate canonical order and exact PendingDelta
`(category,key,index,value)` before settlement. The optional, default-off
`DXMT9_PE_SCALAR_SEMANTIC_OBSERVER` may strengthen this transition with a cold
category-aligned ordinal ledger and an ephemeral
`(category,key,index,value,sourceOrdinal,recordOrdinal)` tuple, but that ledger
must not reside in `PeRecorderState`, allocate per setter, or add virtual
dispatch. Its bounded proof applies only while the observer is enabled; the
default path does not retain or claim a source ordinal.
An effective `FULL_SNAPSHOT` disposition is carried with the prepared state
through emission and settlement: snapshot rows projected from clean
`LiveShadow` do not require a pending token, while every represented
`PendingDelta` row is still value-validated and consumed exactly once, and an
observer-backed row is additionally source/record-ordinal validated. Pending
rows not represented by a bounded projection remain
pending for retry. If an accepted append cannot settle its prepared state, the
recorder enters the existing fail-stop/device-lost state; it must not return
`S_OK` with stale pending state.
The same default-off cold owner must issue a producer-qualified source ordinal
at the common append envelope for matrices, COM bindings, constants, and every
heterogeneous record family. After semantic-owner admission it must bind that source
ordinal to the exact record ordinal, payload-arena-relative committed byte range,
synchronous exact payload bytes, and every qualified wire
`(kind,generation,objectId)` named by the record. Pre-capacity and admission
rejection preserve the source disposition without manufacturing an accepted
token; an effect-unknown bridge failure retains accepted tokens until explicit
Reset/teardown discard; and materialized, rejected, and skipped capture paths
settle them only after command acceptance. The default path retains no ledger
and pays only the cached nullable-owner branch. These projections may prove finite transition/
refinement properties only; they must not claim C++ object layout, allocator
internals, ABI bytes, or unbounded execution.
Capacity-pre success must include settlement of the already-full builder across
the bridge, every materialized/rejected/skipped capture disposition, drain,
reset, and warm ordering before the proposed emitter may consume its token.
Progress claims require fairness only for enabled internal settlement actions;
retryable or effect-unknown environment outcomes do not imply liveness.

**R-CORE-REC-3.6** Every public `noexcept` COM/C ABI path must contain
allocation, container growth, thread creation, and bridge failures. Output
pointers must be cleared before fallible work except where the D3D9 contract
explicitly preserves a caller sentinel; recoverable pre-effect
allocation failures return `E_OUTOFMEMORY`; and wrapper/backend references
remain exactly balanced. Queue byte accounting changes only after successful
adoption. A pre-adoption failure preserves caller ownership, while a failure
after adoption is effect-unknown and must poison/fail-stop without releasing
the adopted references a second time. Worker startup must publish its owner
only after successful thread creation and roll back completely on failure.
Host evidence for container growth must throw from the allocator used by the
real container where feasible. A synthetic before/after disposition control
must be labeled as such and cannot substitute for allocator-failure evidence.

## 4. Object identity and lifetime

**R-CORE-REC-4.1** PE-local object identity used for lookup or deduplication
must be the pair `(kind, wrapperPointer)`. A raw wrapper pointer may exist in a
PE-local `PeWireObjectRef` only to retain or call the wrapper; it is not a wire
identity, must not cross the PE/unix ABI, and must not be used as an
unqualified global cache key. In particular, chunk presence, buffer-hazard,
and pending-destroy queries must preserve `kind` through their O(1) lookup and
any bounded linear fallback so same-address wrappers from different kinds do
not match.

**R-CORE-REC-4.2** Serialized object identity must be exactly
`(kind, generation, objectId)`, with every component valid and nonzero where
the schema requires it. Resolution and retention must validate the complete
batch before the first retain, preserve entry order, and reject wrong-kind,
stale-generation, cross-registry, or zero identity before any caller-visible
effect.

**R-CORE-REC-4.3** Wrapper aliases, texture-derived surface aliases, and slot
reuse must have explicit lifetime relations without weakening identity.
Generation-qualified identity remains stable while any wrapper or pending
command reference exists; a pending alias must settle or force one bounded
flush-and-restart before logical-slot replacement; and generation exhaustion
must retire a slot rather than wrap. Retain callbacks made while registry
mutation is excluded must be `noexcept`, direct, and non-reentrant, or the
implementation must move them outside the registry lock with an equivalent
pin protocol.

**R-CORE-REC-4.4** Every non-null PE COM operand accepted by a public device
method must first prove membership in the exact final wrapper type by a safe
RTTI `dynamic_cast` in the translation unit that owns that wrapper. After that
proof and before raw-member extraction, the boundary must validate the owning
device, exact public-interface address, concrete kind, nonzero wire
`(kind, generation, objectId)`, and surface-alias qualification, then return a
kind-qualified POD raw/wire reference to device code. Wrappers must not expose
a private authentication `IUnknown`, store a deterministic seal/token, or pay
a second instance vptr. This boundary validates the wrapper's cached identity
shape; only `WireObjectRegistry` resolution may claim that a generation is
currently live. Setter nulls retain their D3D9 contract.

## 5. Ownership and observer policy

**R-CORE-REC-5.1** `LiveShadow`, `PendingDelta`, the command builder, its
retainer, and recorder transaction state are `producer-owned` under
`R-BACK-43.4`. Calls without `D3DCREATE_MULTITHREADED` must satisfy the shared
debug thread-affinity predicate; calls admitted by that flag or the explicit
rollback lane must hold the recorder-lock witness. No diagnostic may create a
second writer.

The same conditional recorder-lock witness applies to the complete PE
StateBlock interval (`CreateStateBlock`, `BeginStateBlock`, `EndStateBlock`,
`Capture`, and `Apply`), every PE shadow/recording setter, and every Render
Tape child destruction, mutation, or ordered-control callback that mutates
`PeCaptureState` or its registry. The unlocked lane remains one branch plus
the existing debug thread-affinity assertion. Callback re-entry is bounded by
the existing recursive lock contract and must fail closed at its validation
boundary; no lock may be held across a user/external callback unless that
contract explicitly covers the callback.
The same guard serializes default-pool child ownership increments/decrements
with `Reset`'s legality read under `D3DCREATE_MULTITHREADED`; the unlocked lane
retains its ordinary non-atomic counter and one disabled guard branch.

**R-CORE-REC-5.1.3** StateBlock recorder poison is a fail-stop lifetime, not a
per-call flag. A post-entry End failure, wrapper publication failure after an
accepted End, or backend Capture/Apply failure that may have partially mutated
the unix device poisons the PE recorder, and every subsequent PE recording
write, including the specialized `SetRenderState` entry, returns
`D3DERR_DEVICELOST` before command or shadow mutation. A failed Reset/ResetEx
preserves poison and any pre-effect Apply staging; only a backend Reset/ResetEx
that returns success
discards staged Apply retains and clears poison so recording can recover.

**R-CORE-REC-5.1.1** `PeRecorderState` owns the producer-owned shadow,
reusable binding/build scratch, command builder, retainer lock witness, and
recorder transaction counters. Its closed `PeStateBlockTransactionState`
sub-owner exclusively owns StateBlock Recording/inside-End/poison lifecycle,
the `StateBlockRecorded` domain and constants, and occupied Apply staging.
Staged COM values must remain category-qualified and lifecycle operations must
release or transfer one retain per occupied category/slot, including repeated
use of the same COM identity in distinct qualified cells. A second stage into
an already occupied qualified cell must fail before retaining and must preserve
the first staged value; one-bit occupancy must never hide an overwritten
retain. Each successful Begin must advance a non-wrapping recording epoch, and
`RecordingCapability` must carry that epoch as well as the Recording phase.
End, Reset, and a later Begin must not revive an older capability; epoch
exhaustion is fail-stop rather than wraparound. Candidate and staged ownership
traversal must receive typed category references; `void*`
AddRef/Release/transfer callbacks
and convention-only casts from admitted public interfaces are forbidden. The
TU-local membership result is the validated capability carrying the exact
public-interface address, raw handle, wire reference, and concrete kind.
`D3D9DeviceImpl` must reach the recorder owner
through `recorderState_` directly and must not retain reference aliases to its
fields, duplicate transaction flags/storage, or add allocation/virtual
dispatch to draws. The allocation-free TU-local RTTI membership check is the
explicit exception for non-null app-supplied COM operands at setter/copy
boundaries; it must not enter internal draw emission. Flat fixed state tables must keep their value
arrays, occupancy words, and counters private behind bounded
`set`/`get`/`erase`/iteration operations. Ordinary live-plus-pending setter
mutation must be one operation on a category-scoped, allocation-free
transition capability; raw maintenance and bounded consume capabilities are
private to semantic settlement and reset owners. `LiveShadow`, `PendingDelta`,
the StateBlock candidate, and wrapper snapshots expose mutation only through
explicit transition/maintenance/consume capabilities and read-only snapshot
views. This is
capability-based mutation closure, not a claim that the owner is globally
immutable; no caller may obtain a mutable raw table/category enumeration. This closure must not add
per-state allocation or change pinned hot footprints.
The explicit vertex-declaration row is the exception to Apply staging: its
candidate/snapshot owner holds the AddRef for the complete synchronous Apply
call, while the live `vdecl_` binding remains a D3D9-required borrowed pointer.
Prepare must neither retain nor transfer that pointer; Commit may borrow it only
before the StateBlock wrapper can be released, matching ordinary
`SetVertexDeclaration` refcount semantics.

**R-CORE-REC-5.1.2** Ordinary PE COM references are non-atomic under the D3D9
device/COM ownership contract. `D3D9StateBlockImpl`'s atomic reference count is
an explicitly documented exception because state-block snapshots can be held
by the device and child wrappers across their independent ownership paths; it
must not be generalized to every PE child. Backend chunk pins remain private
retains and never change the public COM count observed by applications.

**R-CORE-REC-5.1.4** StateBlock Begin/End/Capture/Apply/reset/teardown serial
and reference effects must be selected by one production transition matrix
shared with the bounded TLA+ model. Every phase/event row identifies the next
serial phase plus candidate, staged-reference, and capture-publication effects.
Generator freshness must reject new enum phases, events, or actions without a
mapped row, and native tests must exhaustively compare the production planner
with every shared row. The bounded model proves only abstract staged-reference
cardinality; exact COM AddRef/Release/transfer multiplicity remains concrete
native fake-object evidence.

The lifecycle model must retain one issued Recording capability across an
End/Reset/Begin cycle. The guarded model rejects a write whose captured epoch
does not equal the active epoch, while an explicit phase-only capability
mutation must violate `NoStaleCapabilityWrite`. Epoch wraparound is outside the
bounded state space; production fails closed before wrap as required by
`R-CORE-REC-5.1.1`.

The same generated binding must cover at least two category-qualified tracked
values and repeated Capture/Apply cycles. Capture success refreshes values for
exactly the frozen tracked set, capture failure preserves the prior snapshot,
untracked live values remain isolated, each successful Apply publishes the
latest captured values, and post-entry Apply failure is fail-stop. The bounded
model owns value/ordinal refinement; native production truth tables and
fake-COM witnesses own exact per-slot reference multiplicity, including one
identity occupying multiple qualified slots across repeated cycles.

The matrix includes an explicit `PoisonRequested` event for every non-terminal
phase (`Idle`, `Recording`, `EndPublication`, `ApplyPrepared`, and `Poisoned`).
Each row enters or remains in `Poisoned`, discards the candidate, releases every
occupied staged retain, and preserves the last published capture. `Terminal`
has no poison or write row and rejects every recording write. Setting only the
phase while retaining candidate or staged ownership is non-conforming; the
bounded model must contain an expected-failure mutation for that leak.

**R-CORE-REC-5.2** The ordinary Set/Draw/append path and its flat value types
must remain in the hot recorder owner. Render Tape, call-history diagnostics,
failure reporting, and state-block orchestration are cold owners even when
hot-reachable. Moving virtual definitions must preserve
the deliberate out-of-line `D3D9DeviceImpl::QueryInterface` key function
unless an independently measured vtable/code-placement change is part of the task.
The device remains one COM object with direct `PeRecorderState` ownership;
the declaration shell and staged source owners must not introduce capability
bases or reorder COM virtual declarations. Child wrappers
use one nullable pointer to a device-owned, kind-qualified plain context;
context operations are `noexcept` and allocation-free at dispatch. Fallible
implementations translate failure to `HRESULT` or the recorder fail-stop
transition.

Every generic callable invoked by a `noexcept` recorder boundary must itself be
constrained as nothrow-invocable for the exact typed arguments. This includes
StateBlock recording writers, live/pending binding transitions, and enabled or
disabled child call-scope bodies. A merely conventional non-throwing lambda is
not sufficient; throwing callables must be rejected during overload resolution.

**R-CORE-REC-5.2.1** The broad recorder facade is absent from public child
headers. Six independent plain context families — StateBlock, Buffer,
SurfaceTexture, Query, Presentation, and ShaderDeclaration — each contain
only one non-owning `D3D9DeviceImpl*` and expose only the operations consumed by
that family. A wrapper has exactly one nullable pointer to its family context;
it must not gain a second vptr, capability base, allocation, or additional
per-child pointer. Context entry points are `noexcept`, allocation-free, and
consume synchronous spans before returning. Object-definition and device-only
state-shadow invalidation remain private device operations.

The device implementation header must converge on a declaration and ownership
shell: DOD state aggregates own data, while hot recorder, cold COM, Render
Tape, diagnostics, and SWVP behavior is defined out of line in its owning
translation unit. Header decomposition must not reorder Windows COM virtuals,
grow child wrapper storage, or move the deliberate key-function/vtable owner.

**R-CORE-REC-5.2.2** The PE decomposition audit is a checked, architecture-
stable manifest at `scripts/check/pe_device_abi_manifest.toml`, driven by
`scripts/check/audit_d3d9_pe_abi_codegen.py`. It records the exact x64/x86
builtin configuration, export ordinal/name allowlist, QueryInterface/vtable/
RTTI ownership, `PeRecorderState` size pins, critical device member order, cold
symbol owners, and normalized representative hot code metrics (bytes,
instructions, and direct calls). The audit rejects app-local
`wine_builtin_dll=false` artifacts and mismatched toolchain/configuration.
Addresses, RVAs, timestamps, relocation order, archive order, whole-assembly
hashes, and total-text comparisons are not evidence and must not enter the
manifest. Native-only test lanes run its `--source-only` mode and never require
PE artifacts.

**R-CORE-REC-5.2.3** State implementation fragments and diagnostic/tape helper
fragments are not always-included substitutes for ownership. Pure value types
belong in narrow owner headers; cold definitions belong to recorder,
diagnostic, SWVP, COM-cold, tape, tape-registry, or tape-child translation
units. Any residual hot declaration/body mass above the target is recorded with
the exact measured codegen reason in the recorder gap rather than hidden by a
renamed include or an unstable size claim.

**R-CORE-REC-5.3** A disabled observer must cost at most one cached boolean or
nullable-sink branch and perform no callback, virtual dispatch, scope-object
construction, sample-slot mutation, allocation, locking, timestamp read,
formatting, object-cache publication, or capture-lifetime work. An enabled
observer must receive an exact kind-qualified, synchronous, call-local view;
it must not retain borrowed spans, call back into the recorder or registry, or
change command boundaries, application results, or retry/discard semantics.

**R-CORE-REC-5.3.1** Render Tape storage is owned by a nullable heap-owned cold
`PeCaptureState`; the owner is null when capture is disabled. PE
diagnostic-only counters, scopes, sampler handles, and observer scratch belong
to `PeDiagnosticsState` (or an equivalent cold owner). Disabled observer and
diagnostic paths may perform at most a cached boolean or nullable-sink branch.

**R-CORE-REC-5.3.2** The recorder's prepare/accept protocol is non-reentrant:
`prepare` only projects state into caller-owned scratch, while `accept` is the
only operation allowed to settle the represented pending entries and is
reachable only through an explicit consume capability. Acceptance validates
the complete represented key set before the first erase; one malformed key
rejects the entire batch without consumption. The producer
thread/recorder lock serializes the interval; a failed append cannot erase a
mutation made after preparation, and a reentrant setter during preparation is
not a supported interleaving.

## 6. Verification and promotion

**R-CORE-REC-6.1** Every production decision that can consume pending state,
settle a record/chunk, qualify identity, replace an alias, or activate observer
work must be expressed through a shared pure predicate or transition function
where practical. The bounded model and native tests must execute or translate
that same owner; a test-only reimplementation is not model-to-code binding.

**R-CORE-REC-6.2** The recorder verification bundle must include bounded
counterexamples for lost pending state, partial append, retry drift,
wrong-kind/stale/aliased identity, pending-alias replacement, disabled-observer
work, and reentrant retain. Pending/durable evidence must use a qualified token
containing key, value, and a bounded epoch/ordinal; key presence alone cannot
settle a newer value after an older durable record. Each guarded premise must
have an expected-failure configuration or mutation control that demonstrates
the defect when removed, including dropped-value/token, non-Accepted
consumption, inexact Accepted representation, and live prior-value pending-
replacement mutations.
It must also include independent controls for treating an effect-unknown bridge
failure as retryable, consuming an unattempted pre-capacity record, retracting
capture after command acceptance, resetting before alias/parent drain, changing
a StateBlock tracked set during Capture, publishing a failed Capture candidate,
or applying a stale captured category value.

**R-CORE-REC-6.3** Acceptance must map every requirement to an exact production
owner, deterministic native or PE differential evidence, and any applicable
TLA+/exhaustive model. Missing failure injection, model/code binding, Wine
state-block oracle, disabled-path codegen evidence, or wild capture evidence
must remain in [gap.md](gap.md). A recorder optimization that changes state or
lifetime transitions stays default-off until the applicable
`R-VERIF-1.5`–`1.8` and `R-VERIF-6.4` layers are complete.

The finite state/append transition rows are maintained in one canonical table
shared by the production C++ algebra and generated TLA module. The verifier
must reject a stale generated module before invoking TLC; symbol-name
similarity is not model/code binding evidence.

---

## 7. Direct wire construction and typed capabilities

**R-CORE-REC-7.1** The target PE builder must reserve one final contiguous wire
blob and construct the header, record table, handle table, and payload arena in
their final offset-addressed locations. Its build transaction must checkpoint
every visible length, handle retain, pending-delta settlement witness, and
capture identity; `commit` publishes the complete pointer-free blob, while
`rollback` restores the exact checkpoint and releases newly acquired owners.
All fallible reserve and validation work must precede an infallible publish.
The C ABI version, alignment, record ordinals, handle ordinals, padding bytes,
and offset/length validation rules must not change merely to enable direct
construction.

**R-CORE-REC-7.2** The direct builder must run beside the legacy
`CommandChunkBuilder` seal path until a differential harness proves, for the
same canonical input and injected failure, exact sealed-blob byte identity,
record/command order and count, handle-table identity, retained-owner
multiplicity, pending-delta settlement, retry result, and capture disposition.
The harness must include zero/maximum-alignment tails, duplicate handles, all
record families, CapacityPre/CapacityPost boundaries, rollback after each
fallible step, repeated seal, Reset, and full-snapshot cases. Equivalence is a
test oracle, not permission to keep both materializations in production after
promotion.

**R-CORE-REC-7.2.1** Normal command-chunk promotion requires a replayable
producer transaction that owns one complete immutable batch across both
passes. Its first pass must compute exact record, unique-handle, and payload
counts without retaining objects, consuming pending state, journaling capture,
or publishing recorder effects; its second pass must visit the same owned
inputs and emit into the exact final layout. The transaction must not retain a
call-local borrow or callback-derived span between passes, and installing it
must preserve the existing CapacityPre/CapacityPost flush cadence. A per-call
append owner whose future chunk contents and final counts are unknown does not
satisfy this production boundary.

The production recorder uses one mandatory bounded `PeSemanticBatchOwner` as
its sole final-wire transaction. All 21 producer rows have typed record slots,
kind-qualified physical pin arenas, source/record checkpoints, and bounded
canonical payload ownership. Logical constant/UP, rectangle, and sparse quotas
remain admission constraints even when canonical payload bytes replace their
intermediate typed storage under R-CORE-REC-7.11. Admission is atomic and fail-closed for generation
collisions and capacity overflow; successful settlement preserves the bounded
warm retainer epoch and clears used ranges, while Reset/discard releases every
retain. Its typed `appendOwnedRecord` adapter and const emission visitors
perform checked record-local handle dedup and exact payload alignment into
either segmented fixed roles or one contiguous ExactFixed extent, with no
allocation or recorder side effect during emission. The persistent recorder
transaction binds PendingDelta, capture, resource-side-effect, ordering, and
bridge retry/poison boundaries without mixing a compatibility builder graph.
Its exact typed pins are also the authority for buffer-hazard queries and
Render Tape pending-chunk leases. The
256-record/256-pin production alias matches the promoted default cadence;
larger debug record-cap overrides fail closed. Owner construction failure is a
device-creation failure and no append, seal, or transport failure may select a
second production builder. The older
`PePrewireChunkTransaction` and `PeOwnedRecordCandidate` remain call-local
test oracles, not all-family owners or production fallbacks. A
residual cannot be smuggled into the owner through a raw or type-erased sink.

**R-CORE-REC-7.3** Recorder reads and writes that depend on the recorder mutex
must use typed, epoch-qualified, non-copyable and non-movable call-scope
capabilities: `RecorderBorrow<T>` for immutable source access and
`RecorderLockCapability` for destination mutation and settlement. A producer
callback receives only the minimum capability it needs, must be synchronously
and nothrow invocable, and must not return, store, capture, enqueue, or otherwise
retain the capability or a span/reference derived from it. Reset, poison,
rollback, or recorder-epoch advance invalidates every prior capability. Raw
mutable builder spans and an unqualified `recorderLockRequired_` boolean are
not substitutes for the typed witness.

**R-CORE-REC-7.4** The recorder's semantic projection must cover the complete
heterogeneous append envelope with exact qualified tokens: render, texture
stage, sampler, transform, texture/buffer/shader/declaration bindings, all six
constant families, stream/index/frequency, viewport/scissor/material/light,
draw/UP payload, StateBlock, query/readback/update/copy, Present, and capture
settlement. A token binds source category/key/value-or-identity, source ordinal,
wire record ordinal, and represented byte range. Record type, byte size, count,
or a hash without collision proof must not stand in for semantic identity.
The all-family producer inventory must be one production C++ table that also
generates the bounded formal family table; freshness checking and exhaustive
native enumeration must reject a newly added producer without a semantic row.

**R-CORE-REC-7.5** PE promotion requires bounded x64 and x86 Wine fault
evidence in addition to native projection tests. At minimum, the fixture must
exercise reserve failure, producer failure after partial construction,
handle-retain failure, pre-entry bridge rejection, entered/effect-unknown bridge
failure, capture reject/throw, Reset recovery, and retry. It must assert exact
HRESULT/poison disposition, no leaked retain, no partial blob publication, and
no duplicate accepted command. An unavailable fault seam remains an explicit
gap; ordinary successful Wine execution cannot replace it.
An injected unique-retain failure must preserve its configured HRESULT through
owner rollback to the failed public append. Its one-shot receipt must not leak
into the next admission. Successful production append must not consume or
query a diagnostic failure result.

**R-CORE-REC-7.6** A future multi-record CPU-ready Tape producer must own one
complete immutable semantic batch before transport begins. The batch must be
pointer-free at the PE/unix boundary, while a trusted in-process recorder
pointer may be used only synchronously inside the producer owner to read
validated source values; no pointer, borrow, span, COM identity, or callback
capture may cross a role boundary or survive the call. A pure first pass must
compute exact record count, unique qualified-handle count, and payload bytes
without retaining, consuming `PendingDelta`, journaling capture, publishing a
source, or changing application-visible state. The total final wire extent,
including fixed header, record/handle tables, payload, alignment, and padding,
must be bounded by 32 MiB; canonical D9C V2 sizes are a 48-byte header,
32-byte record row, 16-byte handle row, and 16-byte section descriptor. The
fixed `D9CCommandChunkSegmentedTransportV1` descriptor is 144 bytes, with its
last 48 bytes carrying the capture token/event ordinal and the immutable
producer event/source interval; role byte counts
exclude inter-table alignment, which is reconstructed from canonical offsets.
An overflow or invalid checked arithmetic result rejects before retention or
publication.
Each borrowed transport role must carry a zero address token if and only if
its byte extent is zero, including empty spans backed by a nonempty arena.
An invalid selected emission must not select a different, unmaterialized
transport representation or enter the bridge with an empty outer chunk.

**R-CORE-REC-7.7** The transport handoff must expose exactly three ordered
roles: `ReserveRole`, `AdoptRole`, and `EmitRole`. `ReserveAll` reserves every
final region and ownership/capacity credit from the pure counts. `AdoptAll`
must adopt the complete batch atomically, including every retained identity and
semantic-ledger entry; partial adoption is invalid. `ExactFixed` emission may
run only after complete adoption and must visit the same immutable batch to
write one contiguous final CPU Tape extent. A later role may not bypass or
re-enter an earlier role.

**R-CORE-REC-7.8** A failure before adoption and before any visible effect may
restore the exact transaction checkpoint and use at most one contiguous,
at-most-once legacy fallback. A failure after adoption, receipt activation, or
the first ExactFixed effect is effect-unknown and must poison/fail-stop; it
must not retry, duplicate the batch, or route through legacy fallback. Retain,
capacity, ledger, source order, and FIFO settlement counts must conserve at
every role transition, and reclaim that releases blocked capacity must publish
the wake/progress signal used by the waiting producer.

**R-CORE-REC-7.9** The committed `PeSemanticBatchOwner` output is the PE
specialization of `ImmutableSemanticSource` in R-ARCH-7.11 through
R-ARCH-7.14. It must assign the closed producer event interval and complete
semantic-source interval needed to derive one `EndToEndSourceIdentity`, seal the same immutable
record/payload/resource/control values for contiguous and segmented emission,
and carry only pointer-free offsets, counts, qualified identities, capture
token, and event ordinal across the PE/unix boundary. PE settlement must not
claim Unix `SourceLease`, queue sequence, storage generation, or completion
ownership; those fields are bound atomically by the importer/queue contract in
R-BACK-2.90 through R-BACK-2.95. The all-family projection and fault matrix
must reject a producer family that cannot preserve this mapping.

**R-CORE-REC-7.10** PE must expose the complete committed semantic batch as one
synchronous borrowed source during the bridge call. The batch may use
pointer-free record/handle tables and payload arenas, but no PE address, span,
allocator block, or recorder capability may become an asynchronous Replay
owner merely because PE and unixlib share a process address space. Before the
call returns, the importer must establish complete Unix ownership through the
named `copy.bridge.raw-owned` operation or a separately specified atomic
shared-lease adoption protocol. PE must not prebuild backend-private
`ChunkSlot`/Arena SoA, Unix resource resolutions, or Metal binding state.
For segmented admission, Unix must freeze the descriptor and validate source
identity, canonical metadata, checked total size, and all live role extents
before allocating or copying. This geometry check must not inspect role bodies.
Full semantic validation must run on the owned copy before any object retain
or publication; successful validation of borrowed bytes is not authority for
the copied source. Geometrically valid but semantically invalid input may incur
bounded speculative storage/copy work, but rejection must leave the destination
unchanged and return all tentative storage without retained ledger credit.

**R-CORE-REC-7.11** Pointer-free scalar sparse rows and variable payload tails
may be constructed directly in the owner's canonical payload region during
synchronous admission. No duplicate typed value arena is required after those
bytes are committed. The source remains a call-local borrow under the recorder
guard; no input pointer or span may enter a record slot, survive admission, or
be consulted by seal, capture, retry, or segmented emission. Binding rows and
kind-qualified pins retain their existing ownership contract.

This representation change must preserve the logical semantic-byte, rectangle,
and per-category sparse quotas, all record/handle ordinals, exact canonical
bytes including padding, and CapacityPre/CapacityPost cadence. Physical
materialization accounting must exclude removed staging rather than equating
logical quota consumption with copied bytes. Commit exposes one complete
record only after payload construction and PendingDelta acceptance; failure
restores the previous committed prefix, logical frontiers, ordinals, and pin
multiplicity. Unpublished tail bytes confer no authority and must be fully
initialized before a subsequent successful admission exposes them.

Bounded native differential refinement must compare the actual owner with the
canonical builder oracle across all affected families, mixed scalar/binding
records, A/B/A source mutation, malformed and capacity rejection, partial pin
acquisition, settlement failure/retry, reset/reuse, and both emission forms.
Existing recorder transition models retain authority over append/settlement
ordering; they do not prove payload bytes or Wine/Metal behavior. Transport,
queue policy, and chunk-size promotion require independent evidence and must
not be coupled to removal of intermediate PE value storage.

**R-CORE-REC-7.12** Admission may resolve each qualified object and its local
wire-handle mapping once and carry a bounded, call-local prepared witness into
append. Reuse must remain bound to the same destination frontiers and input;
it must preserve kind, generation, pointer collision rejection, first-seen wire
order, null unbinding, unique retain multiplicity, and all-or-nothing rollback.
The witness is not a new asynchronous owner or bridge ABI. Reuse must not make
uncommitted pins or handles visible to hazard, capture, or emission consumers.

**R-CORE-REC-7.13** Family-specific input staging and active-prefix storage reuse
must initialize every field before any consumer reads it. Inactive array tails,
old family fields, and cleared index entries confer no authority. A/B/A family
changes, reset/reuse, failed partial admission, and settlement failure/retry must
produce the same canonical bytes and retain/release effects as fresh storage.
Clearing may be reduced only when validity/frontier discipline replaces it;
warm-retainer settlement, logical quotas, failure disposition, and source
ordering must remain unchanged. Native byte/ownership differential checks and
release PE callsite inspection are required; lower clearing/copy counts alone
do not establish lower runtime cost or an FPS gain.

**R-CORE-REC-7.14** A synchronous prepared emission witness must preserve the
exact active section order, counts, offsets, alignment, payload extent, logical
quota deltas, and destination identity proved during admission. Later writers
must consume that witness rather than reconstruct a second layout from the
same source. A compact shadow plan may feed the mandatory semantic owner under
one recorder capability; this does not re-enable a production compatibility
builder or permit a borrow to survive append. CapacityPre must requalify the
destination-dependent stream/index selection without consuming pending state.
Capability qualification includes the issuing recorder identity, not merely an
equal numeric epoch or access lane from another recorder.
Malformed, stale, incomplete, or over-capacity plans must reject atomically.

**R-CORE-REC-7.15** Producer-family specialization may omit irrelevant resource
and sparse work only when the typed family input establishes its absence.
Standalone constants must retain the same range/size validation, canonical
bytes, logical quotas, ordinals, failure disposition, and exact pending-range
settlement as generic admission. Generic/oracle inputs with foreign fields
must not silently lose represented data. Family changes and failed retries
must not revive stale input authority or choose a second transaction owner.

**R-CORE-REC-7.16** Binding occurrences may write canonical wire rows directly
using admission-qualified pin and wire locators. A locator binds the exact
input occurrence, kind, generation, object identity, null disposition, and
destination frontier; an unqualified index is insufficient. Removing a typed
binding arena must preserve committed resource visits, pending-chunk leases,
buffer hazards, first wire-visible order, and unique retain multiplicity.
Only committed canonical metadata or equivalent qualified ownership metadata
may serve later visitors. Native differential tests must cover all seven
binding families, duplicate/promotion/null cases, source mutation, partial
acquisition and settlement rollback, reset/reuse, and both emission forms.

**R-CORE-REC-7.17** Calibrated PE attribution must include compact source
preparation and destination qualification before owner admission, and distinguish
settlement populations by producer family. Aggregate admission timings must not
be presented as measurements of work outside their scope. Disabled observation
must perform no clock reads, allocation, or diagnostic payload construction;
enabled rows must expose event/sample counts and empty-clock calibration.
Nested sampling must not silently charge child instrumentation as caller work.
Independently sampled nested scopes must expose collision counts or prevent
overlap; a null-pair subtraction alone does not remove a child's instrument.

**R-CORE-REC-7.18** Compact source selection and destination qualification may
be reused only under their issuing recorder/source authority and declared
PendingDelta, binding, and destination dependencies. Required pending-state
consumption remains after durable record construction. Skipping observer-only
validation must preserve observer-enabled validation and atomic rejection.
Selection reuse must not trust a caller-mutable flag, preserve a borrow beyond
append, or accept stale authority after CapacityPre, failure, reset, or source
mutation. Unchanged-input reuse and changed-dependency rejection must be bound
to production predicates by bounded exhaustive/native refinement.

**R-CORE-REC-7.19** Record-local identity deduplication, predicted physical pin
assignment, and first wire-visible ordering may share one qualified active-prefix
authority. Retained-only order and wire-visible order remain distinct. Every
occurrence must retain its null/inactive/qualified disposition, and deduplication
must reject kind/generation/object collisions without publishing partial pins
or bytes. Consolidation must preserve canonical byte order, logical quotas,
rollback/retry, committed visitors, and both transport forms. Native ownership
and byte differentials plus PE32/PE64 release frame/code inspection must qualify
the change; a smaller source type alone is not evidence of lower runtime cost.

**R-CORE-REC-7.20** Reusable compact selection facts must be issued by the
recorder producer and immutable to consumers. Read-only views must not expose
a mutable plan, a public trusted setter, or a caller-supplied validity bit.
Admission, emission, and settlement may reuse category counts, masks, and range
metadata only while the exact source authority and each declared dependency
remain valid. A recorder access epoch alone does not prove that borrowed
constant or binding payloads are unchanged. Checks for independently mutable
dependencies remain mandatory unless an equivalent mutation-qualified witness
replaces them. CapacityPre must requalify only the destination-dependent
selection without admitting stale source facts or consuming PendingDelta.

**R-CORE-REC-7.21** A fused retainer lookup/acquisition must preserve the
kind-qualified warm-object result, epoch refresh, capacity denial, unique
AddRef fault point, and exact rollback/release multiplicity of the reference
contains-then-retain sequence. Lookup reuse must be synchronous and private to
the retainer operation; it must not expose an index that can survive mutation
or table rebuild. Warm hits at the capacity ceiling remain admissible; a novel
acquisition at that ceiling must fail before AddRef or index publication.

**R-CORE-REC-7.22** Chunk-boundary pin-index invalidation may avoid clearing
inactive storage, and warm-retainer settlement may avoid rebuilding an unchanged
index. Neither refinement may change the warm epoch, release multiplicity,
generation/object collision rejection, capacity, or rollback outcome. Every
lookup after reset must reject old entries. Reuse, collision, partial acquisition,
rollback, and any generation-wrap or tombstone-reclamation path must have bounded
native differential evidence. Ordinary admission and settlement must not allocate.

**R-CORE-REC-7.23** Compact producer admission may use schema-bounded witness
storage and a minimal committed record receipt instead of a generic all-family
record product. Removing metadata must preserve logical quotas, source/record
identity, canonical bytes, committed resource visits, capture, and retry.
Generic/UP/control inputs must retain their complete semantics; compatibility
inspection may reconstruct metadata from committed canonical roles but must not
require the production compact path to maintain a second value representation.
Release PE32 and PE64 code generation, not source type size alone, qualifies the
stack-footprint result.

**R-CORE-REC-7.24** After non-mutating capacity and identity preparation, compact
binding acquisition and canonical row emission may share one traversal under the
existing append checkpoint. A failed pin, malformed row, or failed settlement
must publish neither partial bytes nor resource authority, and must restore the
previous committed prefix and retain multiplicity. First-seen wire order, null
unbinding, all seven binding families, and unique-acquisition fault order remain
unchanged. Borrowed input must remain qualified throughout the traversal.

**R-CORE-REC-7.25** Access to a recorder borrow and value-bearing mutation or
settlement of a compact plan must require current matching recorder access, not
merely the epoch and lane recorded when the borrow was issued. A public
no-capability visitor, source-facts accessor, or finalizer must not bypass that
requirement. Metadata-only access may return copied scalar facts or tickets,
but not live shadow views or borrowed payload spans. The capability-issuing API
must keep access lexically inside its live guard; non-copyability alone is insufficient
when a factory can return a capability beyond the guard. Reacquisition may reuse
an unchanged source borrow only under fresh matching access and all existing
source/dependency checks. No new allocation, atomic, or mutex operation is
permitted on the ordinary single-producer lane solely to enforce this contract.
Revoke-only invalidation may remain access-free: it must only remove plan/borrow
authority, never read borrowed storage, rebind a source, alter source values, or
publish/settle a record. Its caller still obeys recorder confinement. This permits
failure cleanup after epoch poison without manufacturing fresh mutation authority.

**R-CORE-REC-7.26** A synchronous compact operation may qualify its source
bundle once and reuse that proof within a closed lexical body. The bundle must
bind all sources to one live recorder owner/epoch/lane and the pending ticket;
it must not expose an unchecked public accessor or escaping capability. Public
operation entry, CapacityPre rebase, and any invalidating callback boundary
must acquire fresh qualification. Independently mutable binding, constant,
and destination dependencies are not proved by access qualification alone.
Native stale/wrong-owner/failure and byte-differential evidence, together with
release callsite inspection, must demonstrate both safety and actual reuse.

**R-CORE-REC-7.27** Reusing compact plan storage must explicitly initialize all
readable facts before publishing prepared authority. Absent constant/UP spans,
selection masks, destination facts, and failed partial preparation must not
inherit a previous plan. Reducing whole-object clearing must preserve A/B/A,
family changes, failed retry, and exact canonical emission without allocation.

**R-CORE-REC-7.28** A warm-retainer no-expiry fast path must be justified by
retainer-owned age facts and preserve the reference epoch arithmetic, release
order/multiplicity, warm-hit refresh, and rollback behavior. Uncertain age or
index facts must take the bounded reference scan/rebuild. No diagnostic-only
counter, clock, or allocation may be added to disabled production execution.
Exhaustive native traces must compare the optimized operation with the serial
reference, including wrap, rollback touches, and full-capacity warm hits.

**R-CORE-REC-7.29** PE stream offset/stride metadata must have one live value
authority shared by setters, getters, StateBlock, and draw preparation. SWVP
temporary overrides must be scoped and restore the prior values on success,
rejection, and exceptional exits. Removing a duplicate view refresh must not
change binding identity, pending-state consumption, UP reset semantics, or COM
layout ownership. Native bounded override/reference checks must exercise the
same helper used by production; Wine/SWVP pixel evidence remains separate.

**R-CORE-REC-7.30** Capacity preparation must account for the bounded aggregate
warm retainer as well as each kind's current-chunk pin quota. A novel pin that
cannot fit must reject as capacity before AddRef, canonical writes, or pending
settlement; an existing warm pin at the ceiling remains admissible. The proof
must distinguish current pins, warm membership, duplicate occurrences, and
null identities without consuming fault injection or refreshing warmth.
CapacityPre may retry under the existing bounded commit/rebase protocol; an
empty destination or still-insufficient warm headroom must not spin, evict
pins early, grow storage, or manufacture an empty epoch to admit the record.
Mixed-kind/warm saturation, committed-prefix preservation, and retry after a
legitimate epoch must have native counterexample and refinement evidence.

**R-CORE-REC-7.31** Seal-time emission must consume the authoritative canonical
roles without materializing unused per-record layout metadata. Removing a
private layout projection must preserve final record-header validation,
aggregate bounds, padding, exact contiguous bytes, segmented role spans,
producer identity, and capture/retry behavior. PE32/PE64 release inspection
must distinguish eliminated storage, initialization, and stack probes from
unmeasured wall-time benefit.

**R-CORE-REC-7.32** A prepared append may consume a private destination-bound
capacity capability instead of repeating an unchanged quota/frontier proof.
The capability must be issued only after complete admission validation, remain
inside one synchronous append, and preserve atomic quota publication and
rollback. No public trusted constructor or unchecked append API may expose it.
Source/dependency requalification, payload/storage bounds, identity collisions,
and checks after invalidating callbacks remain mandatory. Stale-frontier,
capacity-dimension, partial-retain, and failed-settlement counterexamples must
reject without changing the committed prefix or exact retry bytes.

**R-CORE-REC-7.33** Compact PendingDelta settlement may skip empty categories
and consume represented masks in bulk only as a refinement of the existing
per-category consumer. It must preserve unrelated pending bits and constant
ranges, full-snapshot behavior, source/ticket rejection, and observer-enabled
validation. Production-used pure mask operations and bounded native
differentials must cover empty, partial, full, repeated-value, and failed/retried
settlement. The refinement must not add a second pending-state authority,
allocation, atomic, mutex, or disabled diagnostic work.

**R-CORE-REC-7.34** Compact append staging may reuse source qualification only
inside a private synchronous region with no invalidating callback. Logical
quota publication must not independently requalify an unchanged source, and
fixed draw-header construction may share the qualified payload writer. Public
entry, destination rebase, actual borrowed-value consumption, and settlement
must retain their required fresh checks. No capability may escape the append
or make a stale owner, epoch, ticket, or destination admissible. Generic and
ordered-control failure guards remain unchanged.

**R-CORE-REC-7.35** Production admission must not maintain derived metadata
solely for a diagnostic or differential oracle. Compact quota preparation and
publication must consume one authoritative category-delta representation;
record-local unique-pin summaries may be projected for inspection without
being accumulated by production. Removing those fields must preserve exact
capacity decisions, identity deduplication, warm-retention deltas, wire order,
and failed-append rollback. Release PE32/PE64 inspection must verify that the
removed work is absent at production callsites, not merely in source types.

**R-CORE-REC-7.36** A compact binding writer may consume a private typed
qualified mapping rather than revalidate the same source identity through a
generic pin API. Qualification must bind kind, generation, object, mapping,
and wire ordinal before consumption; callers must not manufacture or retain
unchecked tokens. Current physical-pin validation, sequential new-pin
acquisition, retain/fault order, canonical bounds, and post-callback validity
remain mandatory. Null and duplicate occurrences, cross-family aliases,
stale identity, partial acquisition, and failed settlement must refine the
checked reference path in bytes, ownership, and retry behavior.

Repeated raw-to-typed conversion may be omitted only when admission has
already proved the expected kind and nonzero identity of that exact mapping,
and emission still compares every current source identity field and object
pointer against it. A checked section range may supply per-row bounds within
that section; row ordinals and null/pinned disposition remain checked. Neither
reuse permits a stale source after a retain callback or a changed physical pin.

**R-CORE-REC-7.37** Ordinary constant setters must consume comparison and state
transition results in the shadow update instead of discarding per-register
facts and independently deriving them again. The production-used helper must
preserve bitwise values, materialized extent, existing pending obligations,
exact dirty registers and merged range, and the separate StateBlock recording
domain. First writes of zero into an absent extent must retain the established
zero-extension semantics. Warm bounded updates must not add allocation or a
large per-call staging product. Native reference traces must exercise the same
helper as production across all six constant families, equality, partial writes,
A/B/A, extent growth, and explicit recording; timing instrumentation must stay
gated and report its actual event population.

**R-CORE-REC-7.38** Immutable state-write table consistency must be checked at
compile time, not revalidated for each dynamic state write. The production
reducer and model must continue to derive from one transition table; generated
lookup or branch forms may replace the interpreter only when native exhaustive
comparison preserves row precedence, normalized facts, effect bits, and invalid
input behavior. Compile-time negative fixtures must reject inconsistent rows.
This does not waive validation of mutable state or introduce an independent
hand-maintained policy. Release PE32/PE64 evidence must identify eliminated
runtime checks rather than infer zero cost from `constexpr` declarations.

**R-CORE-REC-7.39** Record-local identity strategy changes require evidence of
actual occurrence, unique-identity, and comparison-count distributions. The
existing cold semantic-owner observer may collect bounded admission-attempt
distributions with explicit family, sample population, outcome, and retry
semantics. Nulls, duplicates, collisions, and capacity rejection must not change
production identity order, retention, canonical bytes, or rollback. Disabled
production witnesses and calls must not acquire diagnostic counters, storage,
clock reads, or identity walks. Diagnostic reconstruction, if used, must be
labelled and tested against the actual operation; its cost and counts are not
product CPU time or permission to replace the linear lookup.

Binding-validation reuse additionally requires separating null/unbound rows
from pinned occurrences. Compact sampled preparations must identify first
record-unique identities and repeated pinned occurrences across all seven
binding families in emission order. A cross-family repeat belongs to the
later family; per-family uniqueness must not erase that repeat. Counts must
conserve `rows = nulls + pinned` and `pinned = first + duplicates`. Report
sampled per-record duplicate buckets and family sums with their preparation
denominator, not as actual committed pin-helper calls. Failed reconstruction
must remain unmeasured and must never change the production append outcome.

**R-CORE-REC-7.40** Fixed-width constant setters may specialize the canonical
transition for their known element width and recording phase. The ordinary
4-byte and 16-byte Live/Apply paths must preserve the checked reference's
bitwise values, grow-only materialized extent, zero-extension behavior, dirty
membership and pending range, without independently maintained transition
policy. Unused aggregate permission results must not require per-register
production work. Native differential traces must cover equal, changed,
partially changed and A/B/A updates; release PE32/PE64 evidence must inspect
the linked production callers for per-element comparison/copy calls. Generic
validation and the separate StateBlock recording domain remain authoritative
for their inputs. Diagnostic code must remain cold when disabled.
The Live/Apply byte-write projection must be checked at compile time against
all old-extent/equality/pending-membership combinations of the canonical
plan. A policy change that invalidates that projection must fail compilation;
fixed-width specialization must not reintroduce runtime table lookup per
register.

**R-CORE-REC-7.41** Compact source qualification may be shared within one
synchronous, non-invalidating operation rather than repeated by adjacent
helpers. The authority must remain call-local and bound to the issuing
recorder, capability epoch, PendingDelta ticket and current destination.
Retention callbacks, CapacityPre retry, and other invalidating boundaries
must still be followed by fresh qualification before dependent facts are
accepted. Fusion must not expose unchecked borrowed getters, extend a borrow
across a boundary, or weaken malformed, rollback or fail-stop behavior.
Native counterexamples must include wrong-recorder, stale epoch/ticket,
callback invalidation and destination A/B/A, alongside canonical-byte and
failed-retry equivalence.

**R-CORE-REC-7.42** Disabled constant-flush range diagnostics must not scan
dirty membership or resolve shader hashes. Enabled diagnostics must count
dirty membership before append can settle it, and report the range only after
successful append. Gating must preserve canonical bytes, pending state,
failure disposition and the enabled observer's event population.

**R-CORE-REC-7.43** An unobserved empty constant family may bypass the outlined
flush operation using its existing dirty predicate. Dispatch must preserve
VS float/int/bool then PS float/int/bool order, stop at the first failure, and
evaluate each family's current state after preceding appends. It must not
introduce a second mutable dirty authority. Decimated observation must retain
the reference population, including empty calls. Bounded native refinement
must compare emitted records, residual dirty state and failure prefixes for
all six-family dirty combinations, including callback changes and retries.

**R-CORE-REC-7.44** Compact canonical emission may reuse exact section counts
from its prepared sparse-layout witness instead of recomputing mask counts.
The witness must remain bound to the same qualified source and append; source
freshness, section shape, bounds, retention, capacity and rollback checks remain
authoritative. Empty, sparse and maximum masks, destination reuse, A/B/A and
failed retry must preserve generic-reference canonical bytes. Linked PE32/PE64
inspection must identify eliminated work separately from runtime speedup.

**R-CORE-REC-7.45** Sparse plan, admission and canonical emission may traverse
only set binding bits, in ascending source-index order. Empty masks must do no
element work. Mask bounds, first-failure ordering, null/duplicate identities,
source qualification and callback invalidation must remain unchanged. Bounded
native comparison must preserve the full-scan reference's byte order and
failure prefix, including empty, sparse and maximum masks.

**R-CORE-REC-7.46** Successful constant settlement may clear dirty membership
only within the pending interval. All dirty membership must be contained in
that interval before settlement; every production writer must preserve this
invariant. Clearing must preserve materialized values and extents, and failed
append must preserve pending membership for retry. Native refinement against
full-bitmap clearing must cover high-to-low register reuse, disjoint updates,
equal writes, A/B/A, restored state, reset and failed retry. No second mutable
dirty authority or per-clear full-bitmap validation is permitted.

**R-CORE-REC-7.47** Compact quota proof and publication may visit only active
categories derived from the same prepared sparse-layout witness. Every
nonzero delta must participate exactly once; inactive categories must remain
unchanged. Generic admission, aggregate limits, source freshness and rollback
remain authoritative. Native counterexamples must deny each sparse capacity
dimension independently and preserve reference bytes and counters across
empty/sparse/full reuse and retry. Active metadata must not outlive or drift
from its prepared append.

**R-CORE-REC-7.48** Compact layout and canonical writing may omit calls for
absent sections and consume the prepared active-section inventory. Empty work
must not require an outlined emitter call. This must preserve category order,
padding, exact size, first-failure behavior and malformed absent-range
rejection; absence must not suppress source validation. No independent mutable
active mask or source lifetime extension is permitted. Native differential
tests and linked PE32/PE64 inspection must distinguish semantic equivalence
from eliminated calls and from unmeasured runtime benefit.

**R-CORE-REC-7.49** A constant-only append may use a checkpoint limited to its
actual mutation set only when a closed settlement capability cannot mutate
other owner domains or reenter the owner. An arbitrary callback must retain
the full transaction checkpoint. Failure before publication must preserve the
previous canonical prefix, pending constants, ordinals, retention and retry
behavior. Native comparison must exercise successful and failed settlement,
generic retained-ref callback failure, and retry against the full-checkpoint reference.
The optimization must not add per-register proofs or mutable effect metadata.

**R-CORE-REC-7.50** A disabled copy/materialization ledger must not require a
dynamic thread-local initializer check at each PE record admission. Production
configuration remains process-cached; deterministic scoped overrides must
preserve owner qualification, nested restoration and thread isolation. No
unconditional clock, allocation, registry mutation or per-record environment
lookup may replace the removed check. Enabled and disabled native processes
and actual linked production callers must be inspected separately.

**R-CORE-REC-7.51** Recorder transaction checkpoints and settlement may reuse
an owner-issued emission frontier rather than recompute an unchanged layout.
The projection must describe the issuing owner's current committed prefix;
copied counts are not reusable admission authority. A fresh synchronous read
after every invalidating flush, reset, retry or callback may replace a
retained receipt and its invalidation machinery. Failed admission must settle
against the restored frontier. Empty-owner handling,
capacity fault disposition and the established cadence-byte policy must
remain unchanged; cadence bytes are not interchangeable with exact wire size.
Native A/B/A, callback failure and capacity-retry traces must compare the
production-used projection to the checked reference. Any exposed reusable
receipt additionally requires stale-owner/source rejection.

**R-CORE-REC-7.52** Disabled recorder fault injection must not compute its
capacity-only prediction or query its limits. Owner availability and frontier
validity failures remain unconditional. Enabled injection must preserve its
existing payload-based threshold, pre-effect failure, and retry identity;
that threshold must not be replaced with the distinct cadence-byte policy.

**R-CORE-REC-7.53** Append family selection may precede construction of
family-specific views, ordinals and callback environments. Constant appends
must not prepare sparse/generic inputs. One shared transaction choreography
must retain capacity pre/post behavior, retry, source ordinals, owner admission,
pending settlement, observer events and HRESULT/fail-stop disposition. Native
differential traces must exercise all families and failure edges; PE32/PE64
linked inspection must prove that unused preparation actually disappears.

**R-CORE-REC-7.54** Compact preparation must have one owner for sparse-layout
initialization. Invalid or partially prepared witnesses must remain unusable
across malformed, unavailable, capacity and retry outcomes. Adjacent logical
quota publications may share one private authority check only while no
callback or qualifying-frontier mutation intervenes. Source requalification
and post-materialization checks remain authoritative. No persistent validity
cache or unchecked public capability may replace these checks.

**R-CORE-REC-7.55** Production semantic-owner ledger lookup may bypass native
test-only TLS overrides through a statically selected access policy. The
admission/emission transaction implementation must remain shared. Production
configuration is process-cached and owner-qualified; disabled operation must
not construct ledger events or perform TLS lookup. Override-aware native
access must retain nested restoration and thread isolation. Both access
policies must exercise real owner operations in fresh enabled/disabled tests;
test-only dispatch must not silently replace the production path in evidence.

**R-CORE-REC-7.56** Ordinary compact draws with inline constant deltas disabled
must not qualify a source solely to sum the six necessarily empty constant
ranges. Enabled inline deltas must retain exact payload accounting. Plan build,
destination rebase, admission and settlement must retain their required fresh
qualification. Native reuse/failure traces must preserve record bytes and
historical cadence for both modes without a second cached payload count.

**R-CORE-REC-7.57** Sources issued together from one recorder capability may
share one private borrow-domain witness. The aggregate must retain all typed
source pointers, owner identity, epoch identity/value and access lane, and must
reject missing sources, wrong owners, stale epochs, denied/mismatched lanes and
invalid PendingDelta tickets before exposing any source. Construction and
invalidation must be atomic across the aggregate. Source exposure remains
noncopyable, callback-scoped and freshly qualified at invalidating boundaries;
no unchecked getter or cross-callback validity cache is permitted.

**R-CORE-REC-7.58** Live setters may bypass membership reconstruction for an
exactly identical, already-qualified and strongly retained binding. Poison,
argument/slot validation and StateBlock Recording semantics must precede or
exclude this bypass. The retained binding must prove stable owner/kind/identity
until replacement; pointer equality alone is not authority for arbitrary or
borrowed objects. Null, A/B/A, replacement/release, Recording, Reset and foreign
object traces must preserve HRESULT, pending state, diagnostics and retain
multiplicity. Borrowed vertex declarations are outside this optimization.
For a stream source, identical retained buffer identity may reuse membership
qualification even when offset or stride changes; those layout changes must
still execute the normal typed stream transition. Only equality of the entire
buffer/offset/stride tuple permits a no-op. Null `(0, 0)` deactivation must
preserve the cached layout, and Recording must retain its explicit write.

**R-CORE-REC-7.59** A single kind-qualified object-ID-to-pin index may replace
paired object-ID and exact-identity indexes only when it is authoritative for
the live typed pin prefix. Every hit must validate its range, full identity
(including generation) and object pointer against that prefix. Missing keys
alone may acquire new pins; stale generation, pointer aliases and malformed
hits must fail closed. Admission, dependency queries, rollback, rebuild, reset
and stamp wrap must use the same relation. Native bounded refinement must
compare exact bytes, queries, failure prefixes and retain/release counts with
an independent reference over collision, partial acquisition, retry and wrap
traces. Removing redundant index-consistency checks must not be presented as
preserving detection of arbitrary memory corruption.

**R-CORE-REC-7.60** Draw-time automatic-mipmap candidate discovery may skip
texture slots proved ineligible by immutable creation metadata captured at
the concrete membership gate. Outside terminal failure, a derived bounded
slot mask must agree with every Live texture binding; normal publication,
StateBlock Apply and reset must update it with binding ownership. Recording
and failed setter validation must not publish candidate Live state. An
unexpected Apply validation failure must
clear the replaced slot's eligibility and follow the existing terminal policy.
The mask must not cache Dirty, raw objects, wire identity or completion state.
Eligible slots still read current Dirty state, deduplicate in first-slot order,
use the exact bound wire identity and settle Clean only after accepted record
append. Native exhaustive refinement and PE draw/readback traces must bind
these publication and failure boundaries before performance qualification.

**R-CORE-REC-7.61** A qualified compact plan may carry a producer-issued
immutable constant-presence fact to omit settlement work for absent ranges.
It must describe the represented record, not current Live dirty state; plan
reuse and failed preparation must not revive earlier presence. Every represented
constant range still requires current dirty-range validation before any clear,
because constants do not share PendingDelta's generation. Current source,
recorder owner/epoch/lane, durable append and transaction-ticket qualification
remain required before settlement effects. Repeated current-ticket checks may
be consolidated only within the private synchronous interval that cannot
mutate PendingDelta or invoke an invalidating callback before consumption.
Native bounded refinement must cover all six-family presence combinations,
OFF/ON reuse, stale range/ticket, token rejection and retry with exact bytes
and pending-state effects. PE32/PE64 layout and linked-code evidence must
establish the absence of new storage and actual removed work; source-level
array declarations or fewer checks alone do not prove lower CPU cost.

During compact source preparation, the full-snapshot flag may reuse the
producer-selected texture and stream masks instead of rereading PendingDelta.
This reuse is call-local and precedes destination-dependent stream expansion;
it must not infer a full snapshot from the later destination mask. Constant
range preparation must remain free of PendingDelta mutation and callbacks in
this interval. A failed range preparation must not publish the flag or a usable
plan; retry must recompute selection from the new source. No persistent mask
cache or weakened source/retention qualification is permitted.
Linked inspection must price added live ranges, spills and common-path work
on both PE widths. Fewer source reads or a smaller PE64 body alone do not
justify replacing a cheaper PE32 path.

**R-CORE-REC-7.62** Public constant setters may pass call-local immutable
nonempty range authority to the fixed-width Live consumer instead of repeating
null and extent validation. The authority must bind the exact source pointer,
start, end and element width, be issued only after overflow-safe register-cap
validation, and neither escape the synchronous call nor add persistent storage.
Empty writes must take their original no-op route, including arbitrary start
and null input; failed nonempty validation must precede effects. Range authority
must not bypass poison, phase write permission or StateBlock Recording routing.
The generic checked entry and canonical transition remain authoritative for
unqualified inputs. Bounded native tests must cover all six family caps,
invalid/empty dispatch, bitwise values, zero extension, dirty membership,
alias/reuse and boundary overflow. Linked PE32/PE64 evidence must qualify both
caller and consumer costs, and actual PE conformance plus separate observer-OFF
CPU/FPS and PE profiles must precede performance claims.
