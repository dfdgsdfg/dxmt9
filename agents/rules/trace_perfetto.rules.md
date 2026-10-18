---
description: Perfetto capture, provenance, and interpretation constraints for dxmt9 performance evidence
paths:
  - "scripts/run_apps/tracing_harness.py"
  - "scripts/tools/tracing*"
  - "src/dxmt9/*trace*"
  - "src/dxmt9/tracing/**/*"
  - "src/dxmt9/observations/**/*"
  - "specs/experiments/harness/tracing/**/*.md"
  - "docs/perfomance/**/*.md"
  - "agents/skills/cputrace/**/*"
  - "agents/skills/gputrace/**/*"
globs: "scripts/tools/tracing*"
alwaysApply: false
---

# Perfetto Performance Evidence

Use the integrated Perfetto harness as the default trace view. `--trace` selects
a bounded CPU/wait/submission overview with optional command-buffer GPU timing;
heavy detail and PE observations remain explicit. Use [cputrace](../skills/cputrace/SKILL.md) for engine CPU timelines and
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

PE clock qualification is attempt-specific. The original Wine captures fail
strict QPC qualification; subsequent [point-bracket captures](../../specs/experiments/harness/tracing/pe-clock.md#qualification)
qualify both x86 and x64 at the unchanged 1000 ns policy. Treat each run by its
own receipt. Chunk endpoint timing does not establish arbitrary guest-function
attribution or negligible observer overhead.

Live Linux perf/ptrace and macOS target-probe adapters are currently unavailable.
Portable perf-ring, manifest and lifecycle fixtures do not qualify live
collection. `scheduler`, `target-probe`, and `emulated-structure` profiles require
an admitted adapter. Linux references and QEMU execution cannot establish
macOS/Wine/Metal timing. No perftrace code or mandatory dependency is bundled.

Use [REA rules](debug_rea.rules.md) for target discovery and address provenance.
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

## Shared SQL evidence

- Prefer the versioned companion SQLite store for structured supplemental
  observations and experiment/test provenance. Native `.pftrace` remains
  protobuf; the companion is not a Trace Processor dump or a UI input.
- Keep source clocks, exact IDs/values, loss, closure and sampling populations.
  Logs without time remain source-line-indexed records; never fabricate slices.
- Bind supplemental evidence by digest and explicit source event or owner/work
  IDs. A native slice's `observation_store` and `source_record` are lookup keys,
  not automatic clock alignment or evidence of exclusive ownership.
- Seal SQL before the final artifact graph. Failed imports must retain a
  queryable failure envelope and invalidate the requested evidence product.
- Preserve legacy acquisition formats, control/receipt envelopes, crash
  breadcrumbs and opaque captures where their format/lifecycle requires files.
  Existing aggregates remain usable with the SDK disabled.

For cross-run analytical queries, apply the
[optional analytics boundary](../../specs/experiments/harness/tracing/spec.md#optional-cross-run-analytics).
DuckDB belongs to offline analysis when measured query cost justifies it;
keep runtime collection and ordinary qualification independent of it. Preserve
sealed per-attempt SQLite and native traces. Derived Parquet or DuckDB outputs
retain source digests, run/attempt identities, exact values and qualification;
they cannot establish clock alignment or comparable sample populations.
Keep reusable study SQL in `experiments/analysis/<study>/` and generated
products in ignored `experiments/output/analysis/<analysis-id>/`; follow the
[analysis layout](../../experiments/analysis/README.md). The Brewfile supplies
the optional CLI without making it a capture prerequisite.
Use the [cross-run analysis workflow](../skills/cputrace/references/perfetto.md#optional-cross-run-analysis)
for adoption decisions.

## Artifact retention

- Apply the [retention contract](../../specs/experiments/harness/tracing/artifact-retention.md)
  to explicitly scoped trace/analysis roots. Age and disk pressure identify
  candidates; neither overrides live owners, pins or evidence dependencies.
- Treat raw spool, receipts and completion proofs as evidence when the artifact
  graph references them. Do not delete them as temporary files by extension.
- Preserve sealed bytes and manifests. Compression uses an external archive
  mapping and verified restoration; do not gzip/replace files or VACUUM sealed
  SQLite in place. Do not remove live WAL or unresolved crash-recovery data.
- Inventory first; recheck paths, digests, ownership and transitive references
  before applying selected actions. Include current uncommitted evidence docs.
  The existing 3DMark05 name-based inventory is not a general deletion gate.
- Keep cited paths materialized unless a qualified reader contract and source
  audit can recover their exact bytes; other readers require explicit restore.
  Record deletion/eviction explicitly; unavailable evidence cannot support a
  fresh qualified claim. Use the [offline retention tool](../../experiments/analysis/retention.md)
  for explicit plans and verified recovery; automatic cleanup remains disabled.

## Shared acquisition

- The SDK-independent observation source owns physical acquisition. Trace and
  legacy consumers may share only equivalent object-lifetime/sample-index pairs.
  Do not reset or reuse timestamp indices within a shared counter buffer.
- Trace stage tickets retain supplied legacy buffers independently. Close/timeout
  cannot release an in-flight source or replace production retirement authority.
  Cache missing/invalid results as invalid; never retry them into plausible data.
- CPU supply and wait keep their existing gates/windows. Native projections use
  already-acquired values. CPU counter timestamps mean query start; wait counter
  timestamps mean result observation, not wait endpoint alignment.
- Retain acquisition version and per-consumer attempted/accepted counts. Matching
  observation_source/observation_record requires a complete source, proven same
  process/attempt and immutable artifact; row numbers alone do not prove a join.
- SDK-off legacy acquisition must work without allocating trace source caches.
  Shared per-pair resolve has a different CPU cost from legacy bulk resolve;
  require matched controls before asserting an overhead reduction.

- Query/frame/append snapshot timestamps mean `snapshot-observed`; they do not
  locate original phase intervals. Preserve mode, stride, populations, unit and
  aggregation. Snapshot `observation_record` identifies a source-local projection,
  not a legacy CSV/log row. Detailed collector gates remain explicit.
- Admit all fields of a snapshot in one bounded generation-qualified batch.
  Closure, filtering or insufficient capacity must not retain a partial snapshot.
  Keep latest unsigned GPU identities as exact annotations rather than signed
  or floating-point counters. See [snapshot contract](../../specs/experiments/harness/tracing/shared-snapshots.md).
