---
type: "Spec Requirements"
title: "Detailed Performance Tracing Requirements"
description: "Contracts for correlated application, PE, replay, CPU, wait, and Metal performance evidence."
tags: [specs, experiments, harness, tracing, requirements]
---

# Detailed Performance Tracing Requirements

## Perfetto-first observation policy

**R-HARN-TRACE-12.1** The default tracing entry point must select a bounded
Perfetto overview of engine activity. Overview means all available selected
domains plus explicit coverage/absence, not a claim of whole-process attribution.
Uninstrumented baseline runs must remain available and must not silently enable
tracing. Detailed or expensive observers remain explicit profile selections.

**R-HARN-TRACE-12.2** Structured observations not represented by the native
timeline must have a versioned SQLite analysis surface with Perfetto-style
track, slice, counter and argument tables where their semantics permit it.
Native `.pftrace` remains protobuf; a companion SQLite database must not be
advertised as a native Perfetto trace or as a Trace Processor database dump.
Unqualified clocks must remain separate and unknown values must remain null.

**R-HARN-TRACE-12.3** Supplemental observations must retain source record,
artifact digest, collector, run/attempt, identity, clock, unit and qualification.
Connections to Perfetto must use explicit artifact/event/work identities;
unproven joins must be recorded as unavailable, never inferred from proximity.
Opaque captures and bulk payloads may remain files referenced by digest.
Control requests, immutable acquisition evidence and failure breadcrumbs may
remain simple files when their lifecycle or external format requires it.

**R-HARN-TRACE-12.4** Migration must preserve counter aggregates, native thread
CPU versus elapsed time, wait acquisition/state information, sampling
denominators, capture bounds, loss and existing result consumers. A generic
counter track must preserve metric identity, unit and exact raw values. Heavy
observers must not become always-on per-call instrumentation.

**R-HARN-TRACE-12.5** Experiment and test evidence must use the same observation
store contract. It must retain source/build/runtime/configuration provenance,
test or workload outcome separately from collector qualification, and hashed
raw/derived artifact relationships. Publication must be atomic, refuse
overwrite, and bind the finalized database digest in the parent artifact graph.
Failed and partial attempts must remain queryable without acquiring a success
verdict merely because a database was written.

**R-HARN-TRACE-12.6** Acceptance must include SQLite integrity and foreign-key
checks, exact large-identity preservation, partial scope/unknown clock fixtures,
missing/malformed supplemental input, collision and tamper rejection, and
native counter round trips through the pinned Trace Processor. Observer-on/off
and live runtime performance qualification remain separate evidence gates.

This is a cross-domain tracing contract under the existing
[harness requirements](../requirements.md), not an eighth harness domain.
It adds measurement interpretation contracts to the parent's artifact
contracts. Scope is dxmt9 under the qualified Wine runtime on macOS, including
guest x86/x64 and host CPU/GPU observations. Requirement IDs use
`R-HARN-TRACE-*`; [spec](spec.md) owns design and [gap](gap.md) owns status.
Optional external reference lanes under section 11 retain their own platform
identity and cannot qualify the macOS runtime's performance.

## 1. Run and Collection Contract

**R-HARN-TRACE-1.1** Application collection must use the existing catalogue
runner and resolved runtime/staging contracts. A tracing profile must declare
its question, required and optional collectors, capture window, resource
bounds, and qualification policy before collection. Dry-run must expose the
resolved configuration and output locations without launching the workload.

**R-HARN-TRACE-1.2** Each attempt must have a unique run ID and manifest binding
the target module hashes/architectures, staged PE/unix hashes and bridge ABI,
source revision and dirty-state identity, Wine identity, machine/tool versions,
lane, workload settings, and resolved observer configuration. Retries must not
overwrite an earlier attempt or inherit its success status.

**R-HARN-TRACE-1.3** Workload success and tracing qualification must be separate
results. An explicitly requested tracing operation must exit nonzero when a
required collector or qualification fails, even if the workload passed.
Optional-source absence must be recorded and must invalidate claims requiring
that source; it must not silently select another collection method.

## 2. Event Identity and Causality

**R-HARN-TRACE-2.1** Every normalized event must identify its run, collector,
process/thread incarnation, clock, event kind and source record. Object/work
IDs must be scoped by owner and lifetime generation. Reused PIDs, TIDs, object
addresses and sequence values must not alias older observations.

**R-HARN-TRACE-2.2** Cross-layer edges must carry explicit producer/consumer
bindings, including their relation type and evidence. Temporal overlap or
equal sequence numbers alone must not establish ownership, dependency or
causality. Missing bindings must remain unknown.

**R-HARN-TRACE-2.3** Present IDs, raw replay IDs, queue sequences, command-buffer
IDs and encoder IDs must remain distinct. Correlation must represent zero,
one or many children and shared submissions. A caller-context edge must not
claim exclusive ownership of all work published inside that call.

## 3. Clocks and Cost Semantics

**R-HARN-TRACE-3.1** Each clock must declare its API/source, units, domain,
resolution and applicable process/thread/device. Cross-clock alignment must
record calibration observations, transform, validity interval and uncertainty.
Equal units or wall-clock launch timestamps must not imply a common epoch.
Unsupported or out-of-range mappings must not produce aligned durations.

**R-HARN-TRACE-3.2** Wall duration, thread CPU, scheduler state residency, sampled
PC population and GPU execution must be separate measurement kinds. Nested or
overlapping scopes must not be summed as exclusive cost. CPU residuals require
a proven same-thread, same-clock partition; missing coverage remains unknown.

**R-HARN-TRACE-3.3** GPU execution timestamps must identify the GPU/driver timing
source. Commit, completion-worker dequeue, CPU observation of completion,
retirement and display presentation must remain distinct events. Neither
completion callback time nor Xcode replay duration may substitute for the
original workload's GPU execution interval.

**R-HARN-TRACE-3.4** Guest-PC samples must retain sample population and unknown
coverage, and must not be multiplied by native thread CPU to infer exclusive
game/Wine/PE cost. Scheduler attribution must retain interrupted, unknown,
missing and clipped intervals rather than treating unclassified time as zero.

## 4. Observer Boundary

**R-HARN-TRACE-4.1** Disabled instrumentation must satisfy the existing observer
boundary: at most one cached-bool test, no clock read, allocation, formatting,
diagnostic payload construction or unconditional diagnostic write. Trace-only
fields and aggregate return types must not enter production hot-path contracts.

**R-HARN-TRACE-4.2** Enabled event emission must use bounded diagnostic-owned
storage. Measured paths must not allocate per event, format text, perform file
I/O, acquire a sink-wide mutex or block for exporter progress. Capacity loss
must be observable. Engine export and expensive parsing must run outside the
selected measurement corridor. An external collector may drain kernel buffers
and persist raw records concurrently under a bounded, costed collection policy;
that exception must not move I/O or blocking work into the target's hot path.

**R-HARN-TRACE-4.3** PE collection must preserve the three-binary boundary.
It must not call Metal/macOS APIs or cross the bridge once per traced D3D9
operation. Any diagnostic transport must use versioned, bounded, pointer-free
records and the generated bridge contract. Observer failures must not alter
HRESULTs, query predicates, command ordering or resource lifetime.

## 5. Artifact and Perfetto Contract

**R-HARN-TRACE-5.1** A trace bundle must retain immutable raw artifacts, parent
harness envelopes, normalized identities/clocks/events/relations, qualification
results and a derived timeline. Every derived artifact must identify its exact
inputs by digest and its converter version. A viewer file alone is insufficient
evidence; existing counters and `result.json` meanings must remain compatible.

**R-HARN-TRACE-5.2** Perfetto is the primary integrated timeline consumer.
The engine-integrated collector must produce native Perfetto protobuf under
section 9; offline conversion remains a compatibility and validation surface.
Both paths must preserve declared units, exact identities, scope hierarchy,
relation semantics, uncertainty and missing-coverage annotations. Conversion
must reject malformed or unsupported required input instead of emitting a
plausible empty timeline. Native Perfetto protobuf and Chrome Trace Event JSON
are distinct versioned adapters, not interchangeable raw evidence formats.

**R-HARN-TRACE-5.3** Qualification must check window closure, event loss,
schema compatibility, identity uniqueness, endpoint pairing, clock validity
and declared join coverage. Missing, unsupported, uncollected and measured-zero
values must be distinguishable. A partial timeline may be retained for triage
but must not satisfy a complete-attribution gate.

## 6. REA and Target Probes

**R-HARN-TRACE-6.1** REA evidence must bind the original module hash, architecture,
provider/version, address convention, RVA and supporting instructions/references.
Static reachability and decompiler naming must remain hypotheses until linked
to runtime observations. Decompiled target code must not be imported as project
implementation source.

**R-HARN-TRACE-6.2** A target probe must use a versioned manifest specifying
module identity, RVA, expected instruction bytes, ABI, capture semantics and
installation method. Installation must validate the loaded image and relocation
context, reject mismatches before modifying code, and preserve original files.
Concurrent installation/removal, instruction-cache publication and rollback
must have explicit qualified protocols.

**R-HARN-TRACE-6.3** Probe qualification must cover register/flag, stack,
callee-visible return address, LastError and relevant floating-point/vector
state preservation for the selected ABI. Recursion, tail calls, failed calls,
exceptions and nonlocal exits must be handled or explicitly excluded with
unclosed scopes rejected. A before-call marker must not be labelled an exact
function entry/exit duration.

**R-HARN-TRACE-6.4** Prefer existing observations and bounded D3D9 caller-PC
attribution before app-internal patching. Installed probes must report actual
activation, hit counts, thread identity and loss. A valid static address or
successful installation alone must not establish that the intended path ran.

## 7. Emulated Structural Observation

Requirement IDs `R-HARN-TRACE-7.1` and `R-HARN-TRACE-7.2` are retired and
reserved; remaining IDs retain their original meaning.

**R-HARN-TRACE-7.3** QEMU-based instruction, branch or memory observation must
be a separately identified execution lane recording emulator version, machine,
guest environment and acceleration/TCG configuration. Such observations must
not qualify the current Wine/Rosetta/Metal runtime's CPU, GPU or FPS performance.
QEMU must not be assumed to attach to an existing Rosetta process.

## 8. Qualification and Performance Claims

**R-HARN-TRACE-8.1** Observer cost must be measured with matched workload/build
controls: collection disabled and enabled; target patches additionally require
unmodified, inert-marker and armed controls. Reports must include affected-stage
CPU/wall cost, event volume/loss and run-level timing. Small whole-frame impact
alone must not qualify a hot-stage observer as negligible.

**R-HARN-TRACE-8.2** Performance decisions require repeated comparable runs,
declared noise/overhead budgets and the workload's correctness gates. Budgets
must be fixed before collection; a trace must not subtract its measured observer
cost as if it reconstructed an unobserved run. Attribution does not establish
removable CPU, critical-path savings or FPS improvement.

**R-HARN-TRACE-8.3** Acceptance must bind each supported adapter to deterministic
positive and negative fixtures, producer/consumer round-trip checks, and a
bounded qualified-runtime integration run. Read-only conversion is a pure
transform; new concurrent collector/hook lifecycles require model/code evidence
under R-VERIF-1.1–1.4. Rendering/scheduling changes require the applicable
R-VERIF-1.5–1.8 layers independently of successful tracing.

## 9. Engine-Integrated Perfetto

**R-HARN-TRACE-9.1** The unix provider must own an opt-in Perfetto SDK integration
that captures engine events and produces a native `.pftrace` plus the bundle
manifest in the target process. An offline JSON exporter alone does not satisfy
this contract. The initial backend is in-process; collection must not require
a system tracing daemon, privileged service, browser connection or network.

**R-HARN-TRACE-9.2** SDK headers, initialization, data-source registration and
serialization must remain inside a private unix tracing module. Neither PE DLL
may link the SDK. The build must expose explicit enabled/disabled selection,
default disabled, with pinned SDK revision/digest/license and notices. Disabled
builds must contain no SDK dependency, startup thread or tracing allocation;
enabled builds must remain runtime-disabled unless explicitly requested.

**R-HARN-TRACE-9.3** Measured engine paths must emit bounded POD records through
an SDK-free internal facade. The initial implementation must serialize those
records through the SDK after closing the capture window, using the original
timestamps and producer tracks. SDK callbacks, allocation, interning, flush,
file writes and locks must not enter replay/encode/wait critical sections.

**R-HARN-TRACE-9.4** A process-wide session controller must enforce explicit
preparation, collection, closure, writer quiescence, SDK finalization and seal.
Only one session may collect per provider process at a time. Each session must
have a fresh generation, bounded storage/deadlines and an explicit completion
receipt; stale writers must not enter a later session or write freed storage.

**R-HARN-TRACE-9.5** The initial engine coverage must include import, replay,
queue publication, encode, queue/query waits, submission and CPU completion
observation, with applicability and unsupported paths recorded. Flow bindings
must use actual work ownership. Per-draw/command-detail events must require a
separately selected bounded category; no mandatory duplicate legacy trace stream
may be introduced to supply the same events.

**R-HARN-TRACE-9.6** PE events, when requested, must reach the unix tracing owner
in validated batches preserving guest timestamps, process/thread mapping and
source ordering. Native-only capture must label PE/application coverage absent.
PE/native and CPU/GPU clock qualification remains mandatory; engine SDK
integration alone must not imply kernel scheduling or Metal GPU visibility.

**R-HARN-TRACE-9.7** Engine trace configuration must freeze run/session identity,
categories, capture trigger/window, memory/event limits, output destination and
finalization deadline before arming. The catalogue runner must request and
verify the engine receipt and artifact digest. Unsupported builds, failed
startup, lost records or failed flush must invalidate the requested trace
without changing production rendering semantics or silently switching exporters.

**R-HARN-TRACE-9.8** Native output must preserve original clock IDs, timestamps,
track identities, descriptors and explicit work relations, including deferred
serialization across multiple producer threads. SDK/consumer loss diagnostics
and engine loss counts must be retained. Pinned Trace Processor queries must
verify counts, durations, relations and precision; UI import alone is insufficient.

**R-HARN-TRACE-9.9** Acceptance must cover SDK-disabled, SDK-enabled/runtime-off
and active configurations on the native host and x86_64 Wine unix provider,
with both x86 and x64 PE runtime lanes. It must include bounded concurrent
session model/code tests, finalization failure tests, native Perfetto consumer
checks and matched observer-cost controls. Internal SDK state must not escape
through the public bridge ABI or collide with another module's SDK instance.

## 10. Built-In GPU Timing

**R-HARN-TRACE-10.1** A unix-side GPU timing collector must normalize Metal
observations independently of the Perfetto SDK. It must own diagnostic sample
storage and result validation while production queue/encoder owners retain
submission, completion and resource-lifetime authority. PE code must not own
Metal sampling objects or invoke GPU measurement APIs.

**R-HARN-TRACE-10.2** Collection must distinguish command-buffer timing from
optional encoder/pass-stage sampling. Capability checks must record supported
counter sets, sampling points and selected boundaries for the actual device.
Per-draw sampling requires separate enablement and a bounded budget. Unsupported
detail must not silently become command-buffer timing or zero duration.

**R-HARN-TRACE-10.3** Every sample allocation must bind session generation,
device/queue, command buffer, scope and slot generation. Storage must not be
reused until all GPU writes/resolves and asynchronous CPU readers are complete
and the result has been consumed or explicitly discarded. Cancellation, timeout,
queue reset and delayed completion must not attach old results to new work.

**R-HARN-TRACE-10.4** Collection must not wait for GPU progress on render,
submission or completion-critical paths, or force a queue/device idle solely
for profiling. Results must be retired asynchronously after normal completion.
Exhausted storage, unavailable results and deadline expiry must produce explicit
loss/incomplete outcomes. SDK serialization, allocating resolve operations and
file I/O must remain outside measured critical paths.

**R-HARN-TRACE-10.5** Normalized GPU observations must retain source API,
scope/boundary semantics, reported endpoint values and units, clock domain,
calibration reference/uncertainty, work identity and validity. Device ticks and
command-buffer API time must not be assumed interchangeable. Unsupported,
unavailable, invalid, lost and measured-zero outcomes must be distinguishable.

**R-HARN-TRACE-10.6** Perfetto GPU tracks and CPU submission flows must use actual
command-buffer/encoder bindings and qualified clock mappings. GPU execution,
CPU completion observation and display presentation must remain distinct.
Overlapping GPU intervals must not be summed as exclusive busy time, and
timestamp intervals must not imply shader occupancy, bandwidth or cache cost.

**R-HARN-TRACE-10.7** Acceptance must include deterministic sample-lifetime and
clock/identity counterexamples, applicable model/code evidence, native consumer
checks, and device-qualified Metal runs. Matched sampling-OFF/ON controls must
measure CPU/GPU perturbation and verify rendering correctness. No timing claim
may subtract observer overhead as a reconstruction of an unobserved execution.

## 11. External Address, Register and CPU Observations

**R-HARN-TRACE-11.1** External collectors must declare OS/kernel, architecture,
native/translated/emulated execution, collection API, tool version and required
capabilities. Linux `ptrace`, `perf_event_open`, `perf record` and the identified
`trishume/perftrace` implementation must not be presented as macOS collectors.
A Linux guest/reference run must not qualify host Wine/Rosetta/Metal timing.

**R-HARN-TRACE-11.2** Address probes must extend the REA manifest with module
hash/load generation, address space, RVA/resolved address, probe type, exact
instruction semantics, selected registers and ABI. Host and translated guest
PCs/registers must remain separate unless a qualified mapping exists. Raw
register values must not become named arguments without instruction/ABI proof.

**R-HARN-TRACE-11.3** Collection must declare thread selection, attachment and
new-thread/fork/exec policy, hardware breakpoint capacity and scope. Per-thread
coverage intervals and failed installations must be retained. Missing slots or
unobserved threads must fail dependent completeness claims, not silently reduce
the requested address set or manufacture zero hits.

**R-HARN-TRACE-11.4** External event transport must validate record sizes,
ring wrap-around, publication ordering, schema/ABI and loss/throttle indicators.
Bounded concurrent draining may preserve raw records, but loss without a known
affected interval must disqualify all potentially affected claims. Sampling
period, frequency, PMU multiplexing and unwind quality must remain explicit.

**R-HARN-TRACE-11.5** Normalized observations must distinguish address hits,
sampled instruction pointers/stacks, debugger stops and exact scoped events.
Each must retain target/process/thread identity, source timestamp/clock,
requested/valid register masks, probe identity and source record. A register
address alone must not identify a reused D3D9 object or establish a causal flow.

**R-HARN-TRACE-11.6** Breakpoint exception cost, debugger suspension/resume,
kernel collection and external drain overhead must be reported under matched
OFF/armed/active controls where supported. Stop latency is not original workload
CPU time; hit pairs require complete same-invocation evidence before becoming
durations. Missing returns, recursion, tail calls and nonlocal exits must not
produce synthetic complete intervals.

**R-HARN-TRACE-11.7** The external probe owner must preserve target state and
owns bounded attach, arm, collect, disable, drain and detach operations. Failure
must restore only state it changed under a qualified protocol, preserve signal/
exception behavior, and report incomplete teardown. It must not silently kill,
leave stopped or resume a target from an unknown debugger state.

**R-HARN-TRACE-11.8** Platform adapters require pinned capability/permission
preflight, deterministic transport/address/lifecycle counterexamples, bounded
actual-platform evidence and Perfetto consumer checks before admission.
`trishume/perftrace` is a Linux reference candidate, not a bundled dependency or
qualified general tracer. macOS debugger/Mach or Wine marker adapters require
independent guest-state and overhead qualification. Retired IDs 7.1/7.2 remain
reserved; these contracts do not reinstate the removed generic tool adapters.

## 13. Shared Acquisition and Independent Consumers

**R-HARN-TRACE-13.1** A physical observation is acquired once when consumers
request the same object, lifetime, sampling point and interval. Perfetto, legacy
aggregates and SQL consume that result; equal labels or overlapping time alone
do not establish equivalence. The common source is SDK-independent.

**R-HARN-TRACE-13.2** Shared GPU source storage belongs to the retained Metal
object lifetime, not a Perfetto session. Encoder sample indices are single-use
within a buffer. Trace tickets additionally retain session/work generations.
Closing one consumer must not free resources needed by another or by the GPU.
Resolve only after completion; failures and missing/non-monotone timestamps
remain invalid. Production retirement authority must not change.

**R-HARN-TRACE-13.3** Existing CPU supply and wait sites keep their gates, frame
windows, detail selection and original records. Native thread CPU, elapsed wall
time, mutex acquisition and CV-plus-reacquisition time remain distinct. Output
adapters reuse acquired values; they must not repeat a thread-CPU query or wait.
Expensive observations do not become implicit in the overview profile.

**R-HARN-TRACE-13.4** Native records and companion SQL retain acquisition version,
consumer selection, source record keys, clocks and per-consumer loss/closure.
Compatibility files may remain acquisition artifacts; SQL remains the structured
analysis product for experiments and tests. A consumer's closure does not imply
that another consumer covered the same window.

**R-HARN-TRACE-13.5** Verification covers SDK-off, legacy-only, trace-only and
simultaneous consumers, concurrent reads, failed completion, retained storage
after close and fresh object generations. GPU lifecycle model/code checks and
source/consumer round trips are required. Live observer overhead and Wine
workload qualification remain separate evidence gates.
