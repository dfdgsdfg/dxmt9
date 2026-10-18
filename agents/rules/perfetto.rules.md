---
description: Perfetto capture, provenance, and interpretation constraints for dxmt9 performance evidence
paths:
  - "scripts/run_apps/tracing_harness.py"
  - "scripts/tools/tracing*"
  - "src/dxmt9/*trace*"
  - "src/dxmt9/tracing/**/*"
  - "specs/experiments/harness/tracing/**/*.md"
  - "docs/perfomance/**/*.md"
  - "agents/skills/cputrace/**/*"
  - "agents/skills/gputrace/**/*"
globs: "scripts/tools/tracing*"
alwaysApply: false
---

# Perfetto Performance Evidence

Use the integrated Perfetto harness for bounded dxmt9 engine timelines and
explicitly requested Metal timing. Use [cputrace](../skills/cputrace/SKILL.md) for engine CPU timelines and
[gputrace](../skills/gputrace/SKILL.md) for GPU timing. They share the
[normalization and native consumer workflow](../skills/cputrace/references/perfetto.md).
The [tracing spec](../../specs/experiments/harness/tracing/spec.md) owns the
contract; the [evidence matrix](../../specs/experiments/harness/tracing/evidence.md)
records the qualified platform/workload scope and remaining gaps.

## Runtime and toolchain boundaries

- The pinned, private Perfetto SDK belongs to the unix provider. Enable it
  with `-Ddxmt9_perfetto=true`; PE builds must not link it. Stage ABI-matched
  PE/unix binaries using [build instructions](../../docs/build.md).
- Use the catalogue runner to freeze requests, capabilities, duration,
  capacity, and finalization bounds. Preserve its Wine runtime selection and
  [wild-run requirements](test_wild.rules.md).
- Keep hot-path observations bounded and POD-based. Serialize original event
  times after admission closes and admitted writers/GPU work retire within
  the deadline. Serialization time and thread are not observation time and
  thread. Diagnostic retirement must not replace production lifetime authority.
- Use the pinned [Trace Processor lock](../../scripts/tools/tracing_toolchain.lock.json).
  A viewer opening a trace is not qualification. An overridden executable
  must pass the same pin checks; preserve consumer diagnostics and receipts.

## Qualification and interpretation

- Retain the unique attempt, frozen request, runtime/build identities, raw
  artifacts, SHA-256 provenance, normalized bundle, and consumer receipts.
  A receipt qualifies only its hashed inputs and recorded checks.
- Treat workload success and collector success separately. Missing required
  collectors, loss, open windows, unpaired scopes, invalid identities/clocks,
  and incomplete coverage prevent complete attribution. Keep partial evidence
  and its rejection reason instead of silently dropping records.
- Join only proven process/thread incarnations and typed relation endpoints.
  Numeric PID/TID reuse, matching labels, temporal proximity, or a shared
  command buffer do not prove identity, causality, or exclusive contribution.
- Preserve declared clocks, capture bounds, calibration validity and error.
  Do not align PE QPC, CPU monotonic, and Metal timestamps by appearance.
  Zero loss cannot compensate for missing or excessive-error clock mappings.
- Keep wall time, native thread CPU, elapsed waits, guest-PC population,
  GPU command-buffer time and stage timing distinct. CPU completion callbacks
  indicate retirement, not GPU execution timestamps. Unknown population
  coverage remains unknown; samples are not exact invocation durations.
- Request the GPU timing level explicitly. Unsupported stage sampling must
  not silently become command-buffer timing. These captures do not establish
  scheduler residency, shader occupancy, or display presentation time.

## Platform and performance claim limits

The recorded Wine x86/x64 PE captures retain observations but fail strict QPC
clock qualification. Treat new PE runs according to their own receipts; do not
infer complete guest attribution from the existing CPU/GPU acceptance runs.

Live Linux perf/ptrace and macOS target-probe adapters are currently unavailable.
Portable perf-ring, manifest and lifecycle fixtures do not qualify live
collection. `scheduler`, `target-probe`, and `emulated-structure` profiles require
an admitted adapter. Linux references and QEMU execution cannot establish
macOS/Wine/Metal timing. No perftrace code or mandatory dependency is bundled.

Use [REA rules](rea.rules.md) for target discovery and address provenance.
Static reachability or a validated probe manifest does not prove activation,
runtime hits, invocation pairing, or CPU/GPU cost.

Trace closure smoke tests and short-fixture dwell time do not measure throughput
or observer overhead. Performance claims require matched observer-off/on runs,
the same workload/output correctness, declared measurement windows and budgets,
and source-appropriate metrics. Preserve rendering qualification under
[rendering correctness rules](rendering_correctness.rules.md).

Keep bulk captures in ignored experiment output directories. Record durable
conclusions with concrete evidence links in the owning `docs/perfomance/`
domain; follow [documentation rules](documentation.rules.md).
