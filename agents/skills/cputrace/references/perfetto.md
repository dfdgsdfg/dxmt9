# Engine timeline and shared Perfetto qualification

Read [Perfetto rules](../../../rules/trace_perfetto.rules.md) before capture or analysis.
Run commands from the repository root using `scripts/run_python.sh`.
For existing artifacts, skip capture and use normalization or native consumer
validation below. Do not rebuild or launch a workload merely to inspect a trace.

## Capture a bounded catalogue run

1. Select the workload and the question: engine timeline or bounded per-draw
   detail. For GPU capture, use [the GPU recipe](../../gputrace/references/perfetto.md).
   Read the workload's catalogue entry and the applicable
   [wild-run rules](../../../rules/test_wild.rules.md).
2. Follow [build and staging instructions](../../../../docs/build.md#optional-native-perfetto-tracing).
   For the example below, first build the self-closing wrapper using
   [the fixture instructions](../../../../tests/integration/tracing/README.md).
   The example binary is generated locally, not included in the repository.
3. List profiles and inspect a dry run before capture. Verify the chosen binary,
   Wine runtime, collector capabilities, duration, capacity and output location.
   If preflight rejects a required collector, retain the failure and resolve the
   stated prerequisite before retrying; do not silently change the profile.

Build the unix provider with `-Ddxmt9_perfetto=true` and stage the x86/x64 PE
binaries from the same bridge schema. The runner resolves the existing Wine
manifest and workload settings; tracing does not change the runtime selection.

```sh
scripts/run_python.sh scripts/run_apps/run_experiment.py trace-profiles
scripts/run_python.sh scripts/run_apps/run_experiment.py run conf-d3d9-triangle \
  --binary experiments/output/tracing-acceptance/wine-triangle-x64.exe \
  --trace-profile timeline --trace-duration-ms 500 --timeout 60 --trace-dry-run
scripts/run_python.sh scripts/run_apps/run_experiment.py run conf-d3d9-triangle \
  --binary experiments/output/tracing-acceptance/wine-triangle-x64.exe \
  --trace-profile timeline --trace-duration-ms 500 --timeout 60
```

The workload timeout must cover delay, capture, and the frozen finalization
deadline (30 seconds for these profiles); use `--timeout 60` for short fixtures.
Each attempt has a unique output directory, frozen request and provenance
manifest. The process captures bounded POD events, closes admission, waits for
admitted writers and asynchronous GPU retirement within the frozen deadline,
then serializes the original events using its private SDK. The runner verifies
the atomically published engine receipt, exact artifact digests, normalized
schema/clock/identity/coverage checks, and pinned Trace Processor queries.
Workload exit status remains separate from tracing qualification. Required
collector failure makes an explicitly requested trace command fail.

`--trace-detail` selects bounded per-draw observations. `--trace-include-pe`
selects PE chunk batches; raw QPC ticks remain separate when a covering clock
mapping is unavailable, and such captures cannot claim complete attribution.
`--trace-include-metal` requests optional Metal timing with an explicit outcome.
`gpu-detail` requires Metal timing; unsupported stage sampling never silently
falls back to another level. These profiles do not collect scheduler residency,
shader occupancy or display presentation timestamps.

For a short D3D9 app, use the self-closing `wine_triangle.cpp` wrapper described
in [`tests/integration/tracing/README.md`](../../../../tests/integration/tracing/README.md).
It runs the triangle fixture, then keeps the process alive for a bounded 1.5 s
dwell after the app window closes. The trace window must fit inside that dwell;
the qualified x64 CPU timeline used 500 ms. The original 1000 ms examples
against the ordinary short-lived catalogue binary could outlive the process and
fail final receipt publication. Keeping the process alive supports capture
closure only; it is not a workload-throughput or overhead measurement.

`tracing_external.py` validates REA/probe manifests, loaded-image sites, bounded
Linux perf ring snapshots and owned lifecycle protocols. Live Linux and macOS
external probe adapters are unavailable; portable parser fixtures do not
qualify either platform. The `scheduler`, `target-probe` and
`emulated-structure` profiles reject execution without an admitted platform
adapter. Linux reference observations cannot qualify macOS/Wine timing. No
code from trishume/perftrace or Wine implementations is bundled.

## Normalize existing observations

`tracing_normalize.py` validates dxmt9 event bundles and adapts the existing
`cpu-supply-v2`, `wait-trace-v1`, and PE sampler log formats into
`dxmt9-trace-bundle-v1`. It records exact input SHA-256 digests. Legacy rows
retain their original limits: CPU supply rows keep wall and native thread CPU
separate; wait rows are elapsed hooked-site intervals; PE sampler reports are
cumulative guest-PC population snapshots. The adapters do not infer process or
thread incarnations, cross-clock alignment, or causal edges. Supply
`--process-incarnation` only when an external manifest proves it.

The bundle stores identities, clock declarations, events and relations as
separate tables. Opaque IDs and integer timestamps are decimal strings. Each
scope has a process/thread incarnation, clock, and explicit `scope_id`; an
unpaired endpoint is retained and fails complete attribution. Relations require
typed event endpoints and evidence. Coverage uses explicit observed/population
counts. Unknown schemas, duplicate IDs, missing references, invalid timestamps,
loss, open windows, and incomplete coverage never produce a qualified complete
claim.

```sh
scripts/run_python.sh scripts/tools/tracing_normalize.py trace.raw.jsonl \
  --output trace.normalized.json --format auto --run-id run-123
scripts/run_python.sh scripts/tools/tracing_normalize.py wait.csv \
  --output wait.normalized.json --format wait-trace-v1 --run-id run-123
```

`--chrome-output` is an explicit Chrome Trace Event compatibility export. It
requires exactly one qualified monotonic clock and numeric process/thread IDs;
it does not align independent clocks. The output is a view artifact, not raw
evidence.

## Validate a native Perfetto trace

`tracing_consumer.py` reads the engine-produced native `.pftrace` using the
platform-specific executable and SHA-256 in
[`tracing_toolchain.lock.json`](../../../../scripts/tools/tracing_toolchain.lock.json). The pin is
Perfetto v58.2, revision `add693d8b338ba9599dbcbc3e300b1ab8c000897`, under
Apache-2.0. First use downloads the exact pinned binary into the user cache and
checks both size and digest. An explicit `--tp` binary must match the same pin.
The consumer validates its executable version, parses the protobuf, runs native
Trace Processor queries for slice/counter/flow counts, endpoints, durations,
track counts and serialized `debug.*` binding annotations, and retains nonzero
parser/loss diagnostics in a receipt. Empty or rejected traces fail. Complete
qualification requires a normalized bundle; its mapped timestamps, typed
relations and source digests become the query expectations.

```sh
scripts/run_python.sh scripts/tools/tracing_consumer.py capture.pftrace \
  --bundle capture.normalized.json \
  --receipt capture.trace-processor.json
```

Use `--expected` with fixture query-result values for round-trip tests. The
bundle must declare capture-window bounds and its clock, and complete
qualification rejects event timestamps outside that window. A consumer receipt
qualifies only the file it hashes and the checks it records; it does not
establish clock alignment, source coverage, identities, or causal joins absent
from the bundle.

## Inspect outcomes and hand off evidence

Read the attempt manifest, engine receipt, normalization outcome and Trace
Processor receipt before interpreting a timeline. Keep workload success and
collector qualification separate. If qualification fails, preserve the raw
artifacts and exact reason; do not remove loss records or relax clock checks to
obtain a passing result. Fix the cause and create a new attempt.

Report the workload/run ID, runtime and build identities, profile and window,
artifact paths and hashes, collector outcomes, qualified observations, and
remaining clock/coverage/capability limits. Use the
[requirement evidence matrix](../../../../specs/experiments/harness/tracing/evidence.md)
for the scope of existing qualification. Put durable findings in the owning
`docs/perfomance/` domain with artifact links; keep bulk captures under ignored
experiment output directories.

## Default overview and supplemental SQLite

`run_experiment.py run <app-id> --trace` selects `overview`: the engine CPU/wait/
submission timeline plus optional command-buffer GPU timing. Use `--trace-dry-run`
to inspect capabilities and paths. `--trace-profile timeline` keeps the CPU-only
profile; `--trace-profile baseline` disables the integrated trace. Legacy observer
environment settings remain explicit and must also be disabled for observer-off
controls. Expensive detail, PE events and encoder-stage GPU timing remain explicit.

Each traced attempt seals `observations.sqlite` before its final manifest. Open
`engine.pftrace` in Perfetto for the overview. Slice arguments contain
`observation_store=observations.sqlite` and original `source_record` identities
(`begin_`/`end_` on paired slices). Resolve the relative database path beside the
trace, verify its digest in the final manifest, and use the source record to
query detailed evidence. An absent database is an unresolved link, not empty data.

```sql
SELECT e.source_record, e.collector, e.record_json
FROM source_event e JOIN bundle b ON b.id=e.bundle_id
JOIN artifact a ON a.id=b.artifact_id
WHERE a.role='engine' AND e.source_record='42';

SELECT t.name, c.ts, c.value_exact, t.unit, k.alignment
FROM counter c JOIN track t ON t.id=c.track_id
JOIN clock k ON k.id=c.clock_id;

SELECT r.line_number, r.category, v.key, v.value_exact
FROM diagnostic_row r JOIN diagnostic_value v ON v.row_id=r.id;

SELECT key, value_json FROM provenance;
```

This is a companion analysis schema, not a Trace Processor dump or a file that
Perfetto UI loads directly. `slice.ts`/`counter.ts` retain their declared clock;
never join timestamps across `clock_id` without validated calibration. A null
slice duration means a missing endpoint. `value_exact` and source JSON retain
large integers that SQL floating-point counters cannot represent exactly.
`qualification` describes normalized source coverage; workload/test outcomes and
native consumer receipts remain separate provenance. Log metrics without a
clock stay in `diagnostic_row`/`diagnostic_value`, not synthetic timed counters.

Set `DXMT9_CPU_SUPPLY_TRACE=perfetto` or `DXMT9_WAIT_TRACE=perfetto` for
explicit detail acquisition without CSV output. A path selects the legacy file
plus the active native consumer; an unwritable file does not disable native
records. Keep the existing frame/window gates. Trace-only selection requires an
active detail-enabled session. Typed rows and endpoint brackets appear in
`observation_report`/`observation_field`; PE PC samples retain their QPC domain,
target incarnation, failures and omission counts without inventing alignment.
Renderer structural rows can link directly through `structural_value.report_id`.

The runner automatically imports selected `DXMT9_CPU_SUPPLY_TRACE` and
`DXMT9_WAIT_TRACE` files and indexes its workload log. Additional legacy PE logs
or normalized bundles can be supplied with `--trace-bundle ROLE=PATH`. For opaque
captures, use `--trace-artifacts PATH` with a JSON list of `role`, `path`, `sha256`
and optional `collector`. Explicit links additionally name `bundle_role`,
`event_id` and `evidence`, or owner-scoped `work_id`, `owner_id` and `evidence`.
Unknown endpoints and digest mismatches reject the evidence product. Directory
captures use `tracing_store.artifact_identity()`'s `tree-sha256-v1` digest.

For offline conversion or test producers, use the same store directly:

```sh
scripts/run_python.sh scripts/tools/tracing_store.py \
  --output observations.sqlite --run-id run-123 --attempt-id attempt-1 \
  --bundle engine=trace.normalized.json --provenance run-provenance.json
```

`--provenance` is a JSON object; test producers may include a `test_results`
name-to-verdict mapping. Archived D3D9 conformance runs already emit this store
and a hashed `observation-receipt.json`; `--no-archive` explicitly opts out.
Publication refuses existing outputs. Raw acquisition files, control/receipt
JSON and opaque capture bundles remain retained evidence; the SQLite file is
the structured analysis product. Preserve all artifacts needed by its digests.

## Optional asynchronous persistence

Use `--trace-drain-mode async` to persist bounded binary batches during capture.
`--trace-batch-events` defaults to 64, `--trace-service-interval-ms` to 10, and
`--trace-transport-events` to 16384. Freeze these settings before comparing runs;
small producer blocks can lose bursty reports even with spare total capacity.
Deferred mode remains the default. Native Perfetto serialization still occurs
after close in both modes; async is not live streaming to the Perfetto viewer.

Retain the async spool and its manifest digest. New engine receipts require a
matching `.completion.json` proof; a candidate receipt alone is insufficient.
Timeout or loss keeps a capture unqualified, and retained diagnostic ownership
can temporarily reject rearming. Run the normal harness qualification instead
of treating file existence or `stop_and_finalize()` alone as completion proof.

For overhead comparisons, supply matched repeated observer-off/deferred/async
evidence and a budget frozen before collection to
`scripts/tools/qualify_trace_observer.py`. This evaluator does not launch runs or
collect missing metrics. Native fixture success does not establish Wine/Metal
workload overhead; see the [transport contract](../../../../specs/experiments/harness/tracing/asynchronous-drain.md).
