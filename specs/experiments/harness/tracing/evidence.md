---
type: "Spec Evidence"
title: "Detailed Performance Tracing Evidence Matrix"
description: "Requirement-to-code, deterministic tests, and platform qualification evidence."
tags: [specs, experiments, harness, tracing, evidence]
---

# Detailed Performance Tracing Evidence Matrix

This matrix and the migration sections below cover the active requirements. Retired 7.1/7.2 remain reserved.
Implementation and qualification are separate: a deterministic fixture does not
qualify an actual platform collector or establish negligible observer cost.
Paths in the code column are repository-relative or unambiguous source basenames.
Test basenames live under `tests/scripts` or `tests/native/{backend,bridge}`;
formal models live under `specs/verification/tla`.

| Requirement | Contract | Code | Tests/model | Evidence and limits |
|---|---|---|---|---|
| R-HARN-TRACE-1.1 | Catalogue profiles, dry-run and bounded request | `scripts/run_apps/tracing_harness.py; scripts/run_apps/run_experiment.py` | `test_tracing_harness.py` | Profile fixtures/dry-run and live `conf-d3d9-triangle` x64 wrapper timeline runs pass; see the [CPU tracing](../../../../agents/skills/cputrace/SKILL.md) and [GPU tracing](../../../../agents/skills/gputrace/SKILL.md) and the captured attempt artifacts below. |
| R-HARN-TRACE-1.2 | Unique attempts and content provenance | `scripts/run_apps/tracing_harness.py: source_identity, attempt_paths` | `test_tracing_harness.py` | Hash-bound request and live wrapper artifact graph/manifest verified for the qualified 500 ms x64 timeline attempt. |
| R-HARN-TRACE-1.3 | Separate workload/collector outcomes | `scripts/run_apps/run_experiment.py; tracing_harness.py: verify_engine_receipt` | `test_tracing_harness.py` | Missing/failed required collector rejects; optional outcomes retained. |
| R-HARN-TRACE-2.1 | Incarnations and source records | `src/dxmt9/dxmt9_trace.cpp; dxmt9_trace_bindings.cpp; tracing_normalize.py` | `tracing_concurrency_spec.cpp; test_tracing_normalize.py` | Generation/admission fixtures pass; native CPU round-trip qualifies exact original timestamps and thread tracks. |
| R-HARN-TRACE-2.2 | Typed producer-consumer bindings | `device_c_chunk_replay.cpp; dxmt9_queue.cpp; tracing/perfetto_export.cpp` | `test_tracing_consumer.py` | Exact typed annotations and `submitted_in` endpoints pass on qualified native and Wine x64 command-buffer/stage captures. |
| R-HARN-TRACE-2.3 | Raw/queue/CB identity separation | `dxmt9_trace_gpu_bridge.mm; device_c_chunk_replay.cpp; dxmt9_queue.cpp` | `tracing_gpu_spec.cpp; test_tracing_normalize.py` | Native M1 smoke binds the observed submit to its GPU command buffer; broad many-source/shared-CB fan-out qualification remains pending. |
| R-HARN-TRACE-3.1 | Clock declarations and rational mapping | `scripts/tools/tracing_normalize.py; dxmt9_trace_gpu.mm` | `test_tracing_normalize.py; tracing_gpu_spec.cpp` | Native M1 and Wine x64 stage captures preserve qualified Metal clock calibration; Wine x86/x64 PE point brackets now qualify at the unchanged 1000 ns policy; see [PE clock repair](pe-clock.md#qualification). |
| R-HARN-TRACE-3.2 | Measurement-kind separation | `scripts/tools/tracing_normalize.py` | `test_tracing_normalize.py` | Legacy wall/CPU/sample meanings preserved; no exclusive-cost inference. |
| R-HARN-TRACE-3.3 | GPU vs CPU completion semantics | `dxmt9_trace_gpu_bridge.mm; dxmt9_queue.cpp` | `tracing_gpu_spec.cpp; tracing_gpu_smoke.mm` | GPU API timestamps separate from CPU dequeue/completion; actual M1 command-buffer and stage samples pass. |
| R-HARN-TRACE-3.4 | Sample population and unknown coverage | `scripts/tools/tracing_external.py; tracing_normalize.py` | `test_tracing_external.py; test_tracing_normalize.py` | Population/unknown retained; no scheduler or exclusive guest CPU claim. |
| R-HARN-TRACE-4.1 | Disabled hot-path gate | `dxmt9_trace.hpp; d3d9_pe_trace.hpp; production observation sites` | `tracing_runtime_spec.cpp` | Runtime-off fixtures pass; ARM64 false path has cached atomic load/branch without clock/payload call. |
| R-HARN-TRACE-4.2 | Bounded POD emission | `dxmt9_trace.cpp; tracing/session_state.hpp` | `tracing_concurrency_spec.cpp; TracingSessionLifecycle.tla` | Packed admission and bounded buffers pass; native emitter OFF/ON controls record 54 ns/event CPU delta in one bounded fixture, not a general overhead claim. |
| R-HARN-TRACE-4.3 | Bounded PE transport | `include/dxmt9/trace_batch.h; d3d9_pe_trace.cpp; device_c_trace.cpp` | `tracing_batch_spec.cpp` | ABI v2 and all four canonical builds pass. Three Wine captures per architecture each retain six independently calibrated PE endpoints, zero loss, and exact pinned TP agreement; bounded probe overhead is not a throughput qualification. |
| R-HARN-TRACE-5.1 | Raw/derived artifact graph | `tracing_harness.py; tracing_normalize.py; run_experiment.py` | `test_tracing_harness.py` | Input/artifact hashes, versioned envelopes, and live sealed manifests pass on the x64 Wine wrapper attempt. |
| R-HARN-TRACE-5.2 | Native protobuf and compatibility adapter | `tracing/perfetto_export.cpp; tracing_normalize.py; tracing_consumer.py` | `test_tracing_consumer.py` | Pinned native TP CPU round-trip and x64 Wine wrapper, command-buffer GPU, and stage GPU captures pass exact timeline/identity checks. |
| R-HARN-TRACE-5.3 | Fail-closed validity | `tracing_harness.py; tracing_normalize.py; tracing_consumer.py` | `test_tracing_harness.py; test_tracing_normalize.py` | Loss, pairing, identity, closure and clock negative fixtures pass. Historical uncovered/high-error PE captures stay rejected; new point-bracket captures qualify without changing the 1000 ns policy. |
| R-HARN-TRACE-6.1 | REA provenance validation | `scripts/tools/tracing_external.py: validate_manifest` | `test_tracing_external.py` | Module digest/architecture/provider/address proof required; static hypotheses retained. |
| R-HARN-TRACE-6.2 | Loaded-site validation | `scripts/tools/tracing_external.py: resolve_site, capability_preflight` | `test_tracing_external.py` | Hash/RVA/instruction validation; live code patching unsupported. |
| R-HARN-TRACE-6.3 | ABI and invocation exclusions | `scripts/tools/tracing_external.py` | `test_tracing_external.py` | Unsupported ABI/state preservation claims rejected; no general injector admitted. |
| R-HARN-TRACE-6.4 | Actual activation/hit distinction | `scripts/tools/tracing_external.py` | `test_tracing_external.py` | Static validation never establishes runtime hits; live target probes unavailable. |
| R-HARN-TRACE-7.3 | Separate emulated lane | `tracing_harness.py: TRACE_PROFILES; tracing_external.py` | `test_tracing_external.py` | QEMU optional, not launched or claimed equivalent to Wine/Metal. |
| R-HARN-TRACE-8.1 | Matched observer controls | `tools/tracing_gpu_smoke.mm; tracing_runtime_spec.cpp` | `tracing_gpu_smoke.mm` | Three matched 256-event native controls and three actual Metal OFF/ON pairs pass their declared fixture budgets and output hashes. |
| R-HARN-TRACE-8.2 | Performance claim limits | [Perfetto rules](../../../../agents/rules/trace_perfetto.rules.md); `tracing_harness.py` | `Acceptance artifacts below` | Three matched baseline/timeline pairs per x64 and x86 wrapper (12 runs) pass with identical `actual.bmp` hash; screenshot/catalogue-launch timing is not throughput and establishes no speedup or negligible-overhead claim. |
| R-HARN-TRACE-8.3 | Fixtures and model/code evidence | `tests/scripts/test_tracing_*.py; scripts/check/check_tracing_*.py` | `CPU/GPU TLA models and native specs` | CPU/GPU model safety/progress plus 11 deliberate tracing mutants pass; final runtime acceptance below. |
| R-HARN-TRACE-9.1 | In-process SDK producer | `dxmt9_trace.cpp; tracing/perfetto_export.cpp` | `tracing_runtime_spec.cpp` | Actual native producer export passed pinned consumer; concurrent independent SDK instance test passes. |
| R-HARN-TRACE-9.2 | Opt-in private SDK pin | `meson.options; src/dxmt9/meson.build; subprojects/perfetto/PINNED.md` | `Native/PE build matrix` | Default off; PE option rejected; license/digests retained. |
| R-HARN-TRACE-9.3 | Deferred original-time serialization | `dxmt9_trace.cpp; tracing/perfetto_export.cpp` | `tracing_runtime_spec.cpp; test_tracing_consumer.py` | SDK-free emitter and exact native TP round-trip pass original times/threads; explicit relation fixtures pass. |
| R-HARN-TRACE-9.4 | Generation/admission/drain/seal | `tracing/session_state.hpp; dxmt9_trace.cpp` | `TracingSessionLifecycle.tla; tracing_concurrency_spec.cpp` | Safety/progress and five deliberate mutants pass; production session tests cover delayed close, rearm, closed admission, loss and stale scopes. |
| R-HARN-TRACE-9.5 | Engine coverage and detail opt-in | `device_c_*.cpp; dxmt9_queue.cpp; dxmt9_draw_encoder_*.mm` | `Runtime/Wine acceptance` | Final x64 Wine captures resolve import/replay/queue/encode/wait/submit/completion; the CB+detail capture observes one detail event. Query applicability remains unobserved on these workloads. |
| R-HARN-TRACE-9.6 | Guest batch identity and clocks | `d3d9_pe_trace.cpp; device_c_trace.cpp` | `tracing_batch_spec.cpp` | Three captures per architecture qualify all 18 PE endpoints: x64 247–810 ns, x86 476–956 ns. Original QPC/API, same-session native/guest identities, first/last endpoint bounds and native TP annotations agree. See [PE clock repair](pe-clock.md#qualification). |
| R-HARN-TRACE-9.7 | Frozen request and receipt | `tracing/config.cpp; dxmt9_trace.cpp; tracing_harness.py` | `tracing_config_spec.cpp; test_tracing_harness.py` | Digest/type/path/category checks and actual atomic receipt pass; timeout/failure fixtures retain incomplete outcomes. |
| R-HARN-TRACE-9.8 | Tracks/relations/loss/precision | `tracing/perfetto_export.cpp; tracing_consumer.py` | `test_tracing_consumer.py` | Pinned TP checks exact endpoint annotations, tracks and zero-loss qualification on active native and x64 Wine wrapper captures. |
| R-HARN-TRACE-9.9 | Build/runtime matrix | `meson.build; src/dxmt9/meson.build` | `Canonical builds; tracing_runtime_spec.cpp` | PE x86/x64 and unix builds pass; SDK-off tests pass 17/17 and SDK-on tests pass 20/20. Wine x64 CPU, command-buffer+detail, and encoder-stage traces qualify. The PE repair adds qualified x86/x64 endpoint captures and SDK-on 19/19, SDK-off 18/18 focused checks including the install-name audit. |
| R-HARN-TRACE-10.1 | Independent Metal collector | `dxmt9_trace_gpu.hpp/.cpp/.mm; dxmt9_trace_gpu_bridge.mm` | `tracing_gpu_spec.cpp` | Diagnostic-owned samples; production lifetime authority unchanged. |
| R-HARN-TRACE-10.2 | Actual capability and level selection | `dxmt9_trace_gpu.mm; tracing/config.cpp` | `tracing_gpu_spec.cpp; tracing_gpu_smoke.mm` | Command-buffer vs stage levels explicit; unsupported level never silently downgrades. |
| R-HARN-TRACE-10.3 | Generation-qualified retirement | `tracing/gpu_retirement_state.hpp; dxmt9_trace_gpu*.mm` | `TracingGpuRetirement.tla; tracing_gpu_retirement_spec.cpp` | 936 predicate tuples; safety/progress and six intended counterexamples pass. |
| R-HARN-TRACE-10.4 | Asynchronous bounded collection | `dxmt9_trace_gpu_bridge.mm` | `tracing_gpu_spec.cpp; check_tracing_gpu_retirement.py` | Native and Wine x64 command-buffer/stage captures complete with zero GPU loss and successful retirement; dedicated live timeout injection remains pending. |
| R-HARN-TRACE-10.5 | Source-specific GPU evidence | `dxmt9_trace_gpu.mm; dxmt9_trace.cpp; tracing_normalize.py` | `tracing_gpu_spec.cpp; test_tracing_normalize.py` | Native M1 stage capture and qualified Wine x64 encoder-stage trace preserve Metal source timestamps and calibration metadata through pinned TP checks. |
| R-HARN-TRACE-10.6 | GPU tracks and CB relations | `tracing/perfetto_export.cpp; tracing_consumer.py; dxmt9_queue.cpp` | `test_tracing_consumer.py` | Wine x64 CB+detail and stage captures each pass pinned TP with four complete submission flows; the stage capture reports six GPU events and CB+detail reports four GPU plus one detail event. |
| R-HARN-TRACE-10.7 | Device and matched controls | `tools/tracing_gpu_smoke.mm` | `GPU TLA/native fixtures; native Metal smoke` | Apple M1: three matched 64x64 triangle/clear pairs, identical full-surface hash 0746f2c4157f2383, actual stage/CB timestamps, zero loss. |
| R-HARN-TRACE-11.1 | Platform-qualified external lanes | `scripts/tools/tracing_external.py` | `test_tracing_external.py` | Explicit capabilities; Linux/macOS live adapters unqualified/unavailable. |
| R-HARN-TRACE-11.2 | Address/register/ABI manifest | `scripts/tools/tracing_external.py` | `test_tracing_external.py` | Loaded-generation/address/register masks remain distinct from named arguments. |
| R-HARN-TRACE-11.3 | Thread and installation coverage | `scripts/tools/tracing_external.py` | `test_tracing_external.py` | Policies and coverage validated; no live all-thread completeness claim. |
| R-HARN-TRACE-11.4 | Perf ring transport | `scripts/tools/tracing_external.py: perf record parsing` | `test_tracing_external.py` | Portable wrap/size/loss/throttle fixtures; Linux execution not available. |
| R-HARN-TRACE-11.5 | Observation kinds and source identity | `scripts/tools/tracing_external.py; tracing_normalize.py` | `test_tracing_external.py` | Samples/hits/stops/scopes kept distinct; addresses never imply object identity. |
| R-HARN-TRACE-11.6 | External observer limits | `scripts/tools/tracing_external.py` | `test_tracing_external.py` | No exact duration from unmatched hit pairs; live observer controls unqualified. |
| R-HARN-TRACE-11.7 | Owned bounded lifecycle | `scripts/tools/tracing_external.py: ProbeSession` | `test_tracing_external.py` | Portable owned detach/failure protocol fixtures; no target mutation adapter admitted. |
| R-HARN-TRACE-11.8 | Platform adapter admission | `scripts/tools/tracing_external.py: capability_preflight` | `test_tracing_external.py` | Fail-closed unavailable status; no perftrace dependency or unlicensed copied code. |

## Acceptance artifacts

Raw local validation artifacts are kept under ignored
`experiments/output/tracing-acceptance/`. The current qualified captures are:

| Lane | Artifact directory | Result |
|---|---|---|
| Native CPU / SDK round-trip | `experiments/output/tracing-acceptance/dxmt9-tracing-runtime-77042/` | 50 engine events; closed window; zero loss; normalized bundle and pinned TP receipt qualify. |
| Native Metal command buffer | `experiments/output/tracing-acceptance/integrated-metal-smoke-270/` | 8 total events, including 4 GPU events; zero loss; 2 `submitted_in` flows; normalized bundle and pinned TP receipt qualify. |
| Native Metal encoder stage | `experiments/output/tracing-acceptance/integrated-metal-stage-smoke-270/` | 6 total events, including 4 GPU events; zero loss; 1 `submitted_in` flow; two-anchor calibration and pinned TP receipt qualify. |
| Wine x64 CPU timeline | `experiments/output/conf-d3d9-triangle-tracing-window-x64-trace-df36a79cffdd404da85d30de1f06b912/` | `wine_triangle.cpp` wrapper; 500 ms closed window; workload and trace qualification pass with pinned TP. |
| Wine x64 encoder-stage | `experiments/output/conf-d3d9-triangle-tracing-stage-final-x64-trace-d962bb867fed4406849ff7a68068e186/` | 43 total events, including 6 GPU events; zero loss; GPU stage request attached; four complete TP flows; strict trace qualification passes. |
| Wine x64 command-buffer + detail | `experiments/output/conf-d3d9-triangle-tracing-cb-detail-final-x64-trace-dadab305ece94d46b89c4300c3271c64/` | 42 total events, including 4 GPU events; zero loss; one detail event; four complete TP flows; strict trace qualification passes. |
| Wine x86/x64 PE observations | `experiments/output/conf-d3d9-triangle-tracing-pe-final-x86-trace-d31398a450a7432488b2338f84366106/` and `experiments/output/conf-d3d9-triangle-tracing-pe-final-x64-trace-a03b2809d50f463da078aef884831403/` | Each has 45 total events including 8 PE events; zero loss and complete engine receipt. Trace qualification correctly fails: QPC mapping/calibration is uncovered or missing and uncertainty exceeds the frozen 1000 ns policy. |
| SDK configuration builds and tests | `experiments/output/tracing-acceptance/{sdk-off-final-build.log,native-sdk-off-settled-tests.log,sdk-on-final-build.log,final-20-tests.log,pe32-final-build.log,pe64-final-build.log,unix-sdk-off-final-build.log,unix-sdk-on-restored-build.log}` | SDK-off tests 17/17; SDK-on tests 20/20; x86/x64 PE and unix builds complete. |
| Matched Wine wrapper controls | `experiments/output/tracing-acceptance/matched-wine-evidence.json` and `matched-wine-results.json` | Three baseline/timeline pairs each for x64 and x86; all 12 runs pass with identical `actual.bmp` hash `e1a0b87d3e0ad47602a8dbc2983e5105aeeb9ccca3501ad1974c678545b97567`. |

The x86 Wine PE capture is deliberately retained as a failed strict-qualification
case, as is the corresponding x64 PE capture: observed event presence and zero
loss do not substitute for a calibrated QPC mapping. Qualified x64 CB/stage
traces establish those GPU collector lanes for this runtime/workload pair only.
The matched wrapper runs use screenshot and catalogue-launch timing, not
throughput measurements. Live Linux and macOS external probe adapters remain
unavailable. Portable parser tests do not qualify those platform executions.

The finalized artifact inventory contains 418 files and has SHA-256
`fced3c997afc279c40efb2abb708388765c1ec9eb0745c475bce662e15a7b851` in
`experiments/output/tracing-acceptance/artifact-inventory.json`.


## PE endpoint clock repair

The follow-up [point-bracket protocol and qualification](pe-clock.md) replace
previous-batch clock reuse. Raw historical failures above remain unchanged.
New evidence lives under `experiments/output/tracing-pe-clock-fix/`, including
`wine-evidence.json`, `matched-results.json`, `abi-lockstep.json`, SDK-on/off
build and test logs, and the native point serialization round-trip. Three
baseline/timeline pairs per architecture (12 runs) pass with the same image
hash recorded above. All six PE captures pass strict normalization and pinned
Trace Processor; each retains six endpoints and zero loss. This is clock,
identity, serialization and output qualification for the fixture, not a
negligible-overhead or general guest-function attribution claim.

## Perfetto-first observation migration

R-HARN-TRACE-12.1–12.6, R-HARN-8.1 and R-TEST-12.22 are implemented by
`tracing_store.py`, `tracing_harness.py`, `run_experiment.py`,
`run_d3d9_conformance.py` and the native metric exporter. The default `overview`
requests optional command-buffer timing; strict GPU detail remains explicit.
SQLite stores normalized observations, source-line diagnostic fields, test
verdicts, exact values, source clocks, artifact bindings and provenance.

Deterministic evidence: `test_tracing_store.py`, `test_tracing_harness.py`,
`test_run_d3d9_conformance.py`, `test_tracing_consumer.py` and
`tracing_runtime_spec.cpp`. Cases include unknown clocks, clipped scopes, large
unsigned values, hashed capture directories, malformed/missing supplemental
inputs, collisions/symlinks, digest mismatch, unknown event links, original
workload/test failures and native integer-counter CSV decoding.

Local validation artifacts live under
`experiments/output/perfetto-unification/validation/`. The retained
`dxmt9-tracing-runtime-13110` capture under the same parent contains 54 native
records, four counter samples and three typed flows. Its `consumer.json` passes
pinned Trace Processor v58.2 exact expectations; its companion SQLite retains
all source records. SDK-off tracing tests pass 18/18; the SDK-on tracing/legacy/conformance set
passes 22/22; TLA verification passes.
These are native/fixture and dry-run checks, not fresh Wine workload or observer
cost qualification. Complete replacement of legacy producers and guest attribution beyond the
qualified PE point-bracket fixture remain open as recorded in gap.md. Shared
GPU acquisition was subsequently implemented and verified below.

## Shared acquisition

R-HARN-TRACE-13.1–13.5 are implemented by
`src/dxmt9/observations/{bounded_records.hpp,once_sample.hpp,gpu_source.mm,trace_sink.hpp}`,
`StageSampleBuffer::reserve/resolve`, the render-pass attachment selection,
queue diagnostics, native counter exporter, normalizer and pinned consumer.
The [shared acquisition spec](shared-acquisition.md) owns the lifetime and
measurement semantics.

| Contract | Code/test evidence | Limits |
|---|---|---|
| 13.1: one physical acquisition | Counting-source concurrency checks require exactly one shared endpoint/pair read; actual Metal render-pass timestamps agree between legacy and trace consumers. | Shared capture may cost more CPU than legacy bulk resolve; no overhead reduction claim. |
| 13.2: independent ownership | `shared_observation_spec.mm` checks close-before-completion, explicit retained-object destruction, wrong device/ticket generation, failed/missing timestamps, bounds rejection and fresh object lifetimes. | Existing `TracingGpuRetirement`/`TracingSessionLifecycle` safety and progress abstractions remain unchanged; Metal ownership and source caching are additionally tested in C++. |
| 13.3: CPU/wait semantics | `BoundedRecords`, existing CPU/wait fixtures and `test_shared_observation_capture.py` compare each native counter to the original CSV value. | Detailed gates/windows remain explicit; wait projections are result-time counters, not aligned wait slices. |
| 13.4: provenance | Raw/receipt acquisition contract, source keys, per-consumer counts, empty-source metadata and companion SQL round trip; pinned TP compares all 16 source/version/timestamp-kind counter annotations. | Join keys do not qualify unknown process incarnations or legacy clocks; legacy files remain acquisition evidence. |
| 13.5: matrix | SDK-off tracing/legacy/shared set 21/21; SDK-on 23/23; full TLC inventory passes (71 safety, 8 progress, 178 intended counterexamples). The final shared bounds/ownership test also passes SDK-off. | No fresh Wine workload, Rosetta overhead comparison or live timeout injection was performed for this migration. |

Retained evidence is under `experiments/output/shared-observation-samplers/`:
`validation/` contains SDK-off/on, bounded-source and TLC logs.
`dxmt9-tracing-runtime-64302/` contains 60 native events, 8 native counter samples,
3 flows, zero loss, a qualified pinned v58.2 `consumer.json`, legacy CPU/wait
files, and `observations.sqlite`. SQL integrity/foreign keys pass; ten SQL
counter rows include the two additional legacy CPU projections. SQL SHA-256:
`bfa70b4fb60e00aa4b5f20bcfc8526723bedd64b1c08394a4aef5f77662585f5`.
The earlier `dxmt9-tracing-runtime-60097` diagnostic capture retains the consumer
mismatch that exposed equal-timestamp counter ordering and missing native counter
annotations; it is not qualified evidence. Both defects have regression tests.
