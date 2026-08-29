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
Renderer backend, semantic optimization, producer replay, source delivery,
partition execution, command-buffer segmentation, submission grain, binding
representation, FFP execution, and presentation policy may constrain a
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
