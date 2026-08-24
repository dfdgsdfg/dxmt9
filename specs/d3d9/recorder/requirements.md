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
`R-ARCH-3.*`, and `R-ARCH-6.*`; the producer-ownership discipline in
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
An exact cross-projection witness remains an explicit gap until such an emitter
projection is production-used. These projections may prove finite transition/
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
use of the same COM identity. `D3D9DeviceImpl` must reach the recorder owner
through `recorderState_` directly and must not retain reference aliases to its
fields, duplicate transaction flags/storage, or add allocation/virtual
dispatch to draws. The allocation-free TU-local RTTI membership check is the
explicit exception for non-null app-supplied COM operands at setter/copy
boundaries; it must not enter internal draw emission. Flat fixed state tables must keep their value
arrays, occupancy words, and counters private behind bounded
`set`/`get`/`erase`/iteration operations. `LiveShadow`, `PendingDelta`, the
StateBlock candidate, and wrapper snapshots must expose mutation only through
explicit writer/consume capabilities and read-only snapshot views. This is
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

The same generated binding must cover at least two category-qualified tracked
values and repeated Capture/Apply cycles. Capture success refreshes values for
exactly the frozen tracked set, capture failure preserves the prior snapshot,
untracked live values remain isolated, each successful Apply publishes the
latest captured values, and post-entry Apply failure is fail-stop. The bounded
model owns value/ordinal refinement; native production truth tables and
fake-COM witnesses own exact per-slot reference multiplicity, including one
identity occupying multiple qualified slots across repeated cycles.

**R-CORE-REC-5.2** The ordinary Set/Draw/append path and its flat value types
must remain in the hot recorder owner. Render Tape, call-history diagnostics,
failure reporting, and state-block orchestration are cold owners even when
hot-reachable. Moving virtual definitions must preserve
`D3D9DeviceImpl::FlushPeRecorderForChild` as the deliberate key function unless
an independently measured vtable/code-placement change is part of the task.

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
