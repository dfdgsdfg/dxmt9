---
description: Evidence-first correctness workflow for stateful rendering performance changes
paths:
  - "src/dxmt9/**"
  - "src/d3d9/device_c_*replay*"
  - "tests/native/backend/**"
  - "tests/shader_runner/**"
  - "specs/backend/**"
  - "specs/verification/**"
globs: "{src/dxmt9/**,src/d3d9/device_c_*replay*,tests/native/backend/**,tests/shader_runner/**,specs/backend/**,specs/verification/**}"
alwaysApply: false
---

# Rendering Correctness for Performance Lanes

Stateful rendering optimizations can pass API validation and short native tests
while emitting wrong pixels only after a rare state sequence. Treat formal or
exhaustive refinement as the first correctness layer, and use long wild runs as
discovery/final integration evidence. The normative contract is
`specs/verification/requirements.md` R-VERIF-1.5–1.8 and R-VERIF-6.4.

## When the Formal Layer Is Required

Use a bounded TLA+ refinement model or equivalent exhaustive state checker when
the lane changes any of these:

- draw, source, partition, pass, encoder, submission, or completion order;
- mutable state-shadow/cache reuse, invalidation, snapshot, or ownership;
- load/store/clear/action elision, delayed resolution, or pass coalescing;
- worker/coordinator interleavings, wakeups, joins, or failure boundaries;
- resource residency, generation, retirement, or GPU-visible lifetime.

Pure value transforms such as format tables or shader math normally use
unit/property tests instead. Record the reason when formal refinement is not
applicable; do not silently omit it because the wild test is convenient.

## Required Evidence Order

1. State the serial/reference semantics and observable invariants.
2. Model the smallest distinguishing trace and check safety; add liveness only
   for progress, wake, join, or drain obligations.
3. Bind model transitions to C++ through shared pure predicates and native
   truth-table/property/fake-backend tests. Do not duplicate the policy in an
   unrelated test-only implementation.
4. Add a deterministic GPU oracle when correctness depends on concrete shader
   layout, buffer bytes, attachments, floating point, or pixels.
5. Run supervised wild correctness tests, then measure performance/locality.

Prefer small counterexample-rich domains: two workers, two resources, and an
`A -> B -> A` state generation often cover more than a large app trace. Split
ownership/progress and binding/pixel concerns into separate models when a
single composed model would explode.

## Promotion and Failure Rules

- Keep the lane default-off while an applicable evidence layer is missing.
- A reproducible wrong pixel blocks promotion even when Metal validation and
  `gpu_command_buffer_errors` are clean.
- Reduce every wild counterexample into the earliest deterministic layer that
  can express it before repeating long runs.
- State model exclusions explicitly. TLA+ does not prove Metal driver behavior,
  shader ABI bytes, resource contents, floating-point output, or final pixels.
- Runtime assertions, mismatch counters, and liveness watchdogs are cheap
  production monitors, not substitutes for refinement or GPU readback.
- Update the owning `specs/<topic>/gap.md` when any required model, code-binding
  pin, GPU oracle, or wild promotion evidence is missing.

## Review Checklist

- Can the optimization be described as a refinement of the serial stream?
- Does every fallback happen before visible side effects, or fail-stop after?
- Are cached/bound generations correct at every emitted draw, not only at the
  first draw of a pass or child?
- Are model actions tied to production predicates and code owners?
- Does the GPU fixture prove the changed path executed and compare a meaningful
  output, rather than only checking for non-black output or API errors?
- Is the wild run the final evidence layer rather than the only oracle?

See `agents/rules/metal_debugging.rules.md` for capture/readback operations and
`agents/rules/test_wild.rules.md` for supervised integration runs.
