# Tracing normalizer and native consumer

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

`tracing_consumer.py` reads the engine-produced native `.pftrace` using the
platform-specific executable and SHA-256 in
[`tracing_toolchain.lock.json`](tracing_toolchain.lock.json). The pin is
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

## Catalogue capture

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
scripts/run_python.sh scripts/run_apps/run_experiment.py run conf-d3d9-triangle \
  --binary experiments/output/tracing-acceptance/wine-triangle-x64.exe \
  --trace-profile gpu-detail --trace-gpu-level encoder-stage \
  --trace-gpu-capacity 64 --trace-duration-ms 500 --timeout 60
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
in [`tests/integration/tracing/README.md`](../../tests/integration/tracing/README.md).
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
