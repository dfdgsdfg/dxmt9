---
type: "Spec Requirements"
title: "Render Provider Policy Requirements"
description: "Lifecycle, composition, and compatibility requirements for selectable rendering-provider modes."
tags: [specs, backend, render-provider, requirements]
---

# Render Provider Policy Requirements

These requirements classify runtime choices that change how valid D3D9 work is
represented, scheduled, encoded, submitted, or presented. They do not turn
diagnostic probes into supported user features.

**R-BACK-42.1** Every runtime selector that can mutate production rendering must
have one policy class independent of its implementation and default state:

- `StableProvider` is a durable rendering mode. Once implemented, it must remain
  selectable until an explicit requirement amendment supplies migration and
  replacement regression coverage.
- `ExperimentalCandidate` is production-shaped but carries no compatibility
  promise. It must be default off and may be promoted, redesigned, or removed.
- `DiagnosticProbe` exists only for measurement, bisection, validation, or a
  deliberately correctness-invalid experiment. It must not become a dependency
  of a production mode.
- `Retired` is not honored by the current runtime. Its name may remain only in
  historical experiment documentation or a retired-variable note.

Implementation state (`Planned`, `Partial`, or `Implemented`) and activation
state (`Default`, `Automatic`, `OptIn`, `Fallback`, or `Unavailable`) must be
recorded separately from the policy class.

**R-BACK-42.2** A `StableProvider` axis must have a typed resolver, one immutable
owner, a canonical selector or capability rule, a declared default, deterministic
fallback, requested/resolved observability, and a native mode-matrix test. It
must state whether selection is process-, device-, queue-, pass-, or draw-scoped.
Unknown selector values must fail closed with one bounded warning.

**R-BACK-42.3** Stable axes must compose without undocumented implications.
Renderer backend, semantic optimization, producer replay, exclusive encode
execution, submission grain, binding representation, FFP execution, and
presentation policy may constrain a
combination only through an explicit dependency or capability rule. A selector
must not enable an optimizer unless that dependency is named by the owning
requirement and registry; no composition may change D3D9-visible semantics.

**R-BACK-42.4** `ExperimentalCandidate` lanes must identify their stable
fallback and promotion gate. Promotion requires deterministic correctness,
requested/resolved observability, workload evidence, and the owning locality,
completion, or resource-lifetime gates. A candidate must not be selected by an
unset environment variable, a compatibility profile, or another provider mode
unless a requirement first promotes it.

**R-BACK-42.5** `DiagnosticProbe` selectors must be explicitly documented as
diagnostic; names should use an unambiguous `PROBE`, `MEASURE`, `DUMP`, `FORCE`,
or debug spelling where compatibility permits. They may alter correctness for
controlled experiments, but must remain default off, must not be emitted by
normal launcher profiles, and may be removed without provider-mode migration.
Production code must not branch on a diagnostic selector to satisfy correctness
or resource lifetime. A supported rollback is not diagnostic merely because a
legacy selector uses a `DISABLE` spelling.

**R-BACK-42.6** The authoritative provider registry is
[`spec.md`](spec.md). Owning domain requirements remain authoritative for mode
semantics: renderer and optimizer policy under `R-BACK-31.*`/`R-BACK-40.*`,
submission and producer replay under `R-BACK-2.29`–`R-BACK-2.34` and
`R-BACK-2.51`, scheduling providers under `R-BACK-2.66`, present behavior under
`R-BACK-6.*`, binding under `R-BACK-12.*`, and tile FFP under `R-BACK-13.*`.
The registry classifies and composes those contracts; it does not override them.

**R-BACK-42.7** Environment-variable rule files are descriptive mirrors of the
runtime. A planned canonical selector must remain absent from those files until
the runtime honors it. A removed selector must leave the active tables and move
to a clearly marked retired section. Experiment profiles may pin values for
reproducibility but must not redefine engine defaults or policy class.

## Default Activation Policy

**R-BACK-42.8** A production optimization that performs workload-dependent
search, candidate construction, command reordering, draw merge or split, pass
elision, attachment-lifetime relaxation, or parallel dispatch must default off
until its expensive work is preceded by a bounded pre-eligibility gate and the
complete lane proves positive end-to-end frame or throughput benefit on every
representative workload exercised by its intended default scope. A local CPU,
GPU-counter, pass-count, or encoder-time reduction alone is insufficient.

**R-BACK-42.9** A GPU-targeted optimization must not become the general default
when the measured first ceiling is the producer, replay, encode, or presentation
CPU path. Promotion requires evidence that GPU execution is on the target
workload's critical path, that the selected GPU mechanism moves whole-frame
wall time, and that selection or candidate-building CPU does not consume the
gain. Workload-specific opt-in remains valid when those conditions hold only
for a bounded workload class.

**R-BACK-42.10** A behavior-preserving hot-path implementation detail may be
automatic without becoming a provider axis when it only removes redundant
copies, hashes, bridge crossings, materialization, or cache lookup; has bounded
storage and work; introduces no new command, pass, completion, resource-lifetime,
or D3D9-visible ordering; and has deterministic correctness plus local-cost
evidence. Such an implementation does not need an average-FPS claim, but it
must retain a focused regression seam while its equivalence proof is incomplete.

**R-BACK-42.11** A default-on optimization must be demoted to opt-in or removed
when a supported workload demonstrates that its selection, construction,
validation, storage, or synchronization cost materially exceeds its end-to-end
benefit. Prior success on another workload and an unchanged GPU counter do not
override that regression. Demotion must preserve the stable fallback and update
the provider registry, environment-variable mirror, and owning performance
evidence together.

**R-BACK-42.12** Do not create a provider selector solely to retain a tiny
memo, handle carry, copy elision, or other behavior-equivalent cache. Keep the
implementation unconditional when its invariants and maintenance cost are
small; otherwise remove it. A rollback selector is justified only by unresolved
correctness, compatibility, or measurable regression risk, not by the existence
of a micro-optimization itself.

## Encode Execution Provider

**R-BACK-42.13** Encode execution must resolve exactly one queue-immutable
provider:

- `SerialFinalSlotThread`;
- `SerialDirectCursor`;
- `LongSessionDirectCursor`; or
- `ExplicitParallelCompactSoA`.

These values are alternatives, not independently composable feature axes. A
queue must not combine long-session ownership with explicit parallel children,
switch providers per source or pass, or enable one provider as a side effect of
another selector. The planned canonical selector is
`DXMT9_RENDER_EXECUTION_MODE=serial-final-slot-thread|serial-direct|long-session-direct|parallel-compact-soa`.
Under R-BACK-42.7 it must remain absent from the environment-variable mirror
until the runtime implements the resolver. Existing source-delivery, partition,
and segmentation selectors are migration/implementation controls, not the
future provider algebra.

**R-BACK-42.14** `SerialFinalSlotThread` is the stable/current serial topology.
It must consume the immutable Unix-owned semantic source through a bounded
transactional replay projection, construct one queue-owned final `ChunkSlot`
with `TransactionalChunkSlotAssembler`, and publish that complete source
before the dedicated encode thread performs Metal effects. The build may retain
bounded working replay state, exact layout metadata, source-qualified locators,
and compact sidecars, but it must not create a per-draw
`DrawRunSubmission`-equivalent carrier or a second semantic serialization.
Reservation must precede effects and the final destination must be constructed
at most once. The Replay worker remains separate from the PE/game thread.

The dedicated encode thread is an execution-placement policy, not a semantic
owner: it consumes only the immutable final slot, preserves source order and
completion identity, and cannot mutate PE state or source storage. Until the
optional fused provider is implemented and promoted, this topology is the
runtime serial baseline.

**R-BACK-42.14a** `SerialDirectCursor` is an optional future topology. It
consumes the same immutable source and replay relation without constructing a
complete final draw SoA, and may perform Replay projection and serial Metal
encoding on one Unix worker. Its absence must not block the stable
`SerialFinalSlotThread` DOD closure or carrier retirement.

**R-BACK-42.15** `LongSessionDirectCursor` is an `ExperimentalCandidate` whose
fallback is `SerialFinalSlotThread`. It must use the same direct cursor and may
extend only queue/command-buffer, active encoder, attachment, hazard, binding-
shadow, action-ledger, and completion ownership across admitted source
boundaries. It must not require or implicitly construct a final SoA. It uses
the same single replay/encode worker as `SerialDirectCursor`; session
lifetime extension must not introduce a dedicated downstream encode thread.
Promotion requires the EncodeSession correctness, progress, completion,
resource-lifetime, and locality gates plus positive end-to-end workload
evidence.

**R-BACK-42.16** `ExplicitParallelCompactSoA` is an
`ExperimentalCandidate` whose fallback is `SerialFinalSlotThread`. Only after a
complete sealed logical pass passes semantic, attachment, hazard, resource,
first-draw, child-capacity, and economic proof may it materialize one bounded
pass-local compact indexed SoA. That representation must store unique state,
uniform, and resource-set values once and index them from draw columns; it must
not expand full state or binding payload per draw. Coordinator commands,
ordered control, Present, query/readback, and unsealed or carried-incomplete
passes remain outside child ranges. Rejection before Metal effects consumes the
same pass through the provider's direct serial fallback. This fail-closed branch
is internal to the selected provider and does not permit provider composition
or a mid-queue mode switch.

This is the only provider permitted to split replay/materialization from Metal
encoding through a dedicated encode coordinator or child-worker pool. The
Replay worker may publish only an accepted pass-local compact indexed SoA plus
its source-qualified certificate and sidecar. Worker creation, the handoff, and
all associated synchronization/materialization cost belong to this provider's
economy gate. Its ineligible-pass serial fallback executes on the provider's
encode coordinator without changing the resolved provider.

**R-BACK-42.17** Provider equivalence must be established from the same
immutable source and initial replay state. Direct serial execution is the
semantic oracle. Long-session execution must preserve command order, next
replay state, logical-pass actions, completion identity, and GPU output while
changing only encoder lifetime. Parallel compact-SoA execution must additionally
preserve exactly-once draw coverage, child binding isolation, parent/join order,
and source-qualified attribution. Candidate materialization and proof cost is
part of the promotion economy; an encoder-local speedup is insufficient.

**R-BACK-42.18** Provider thread topology is part of the resolved policy and
must be observable and regression tested. `SerialFinalSlotThread` resolves to
one Replay/materialization worker and one dedicated encode thread.
`SerialDirectCursor` and `LongSessionDirectCursor` resolve to one Unix
replay/encode execution worker and zero additional encode workers.
`ExplicitParallelCompactSoA` resolves to one Replay/materialization worker, one
encode coordinator, and a bounded child pool
whose capacity is fixed at queue creation. PE production remains a distinct
application-side stage in every mode. Finish, GPU completion, Presenter
acquisition, shader compilation, and diagnostic sampler workers are outside
this encode-execution count and must not be used to claim render parallelism.
Queue lifecycle construction and teardown must therefore run finish and
completion workers independently of whether the selected topology has a
dedicated encode thread.

Requested/resolved observability must include execution-worker topology,
direct-cursor sources, compact-SoA publications, serial-fallback passes, and
child tasks. Native topology pins must prove that direct modes invoke Replay
projection and encode on the same owner with zero complete-draw Ready
publications, that the parallel mode publishes exactly one compact pass only
after acceptance, and that stop, poison, device loss, and completion release do
not depend on the optional fused mode's lack of a dedicated encode thread.
