---
type: "Spec Requirements"
title: "Backend Requirements"
description: "Backend requirements and compatibility contracts."
tags: [specs, backend, requirements]
---

# Backend Requirements

The backend receives committed command chunks from the core and is responsible for
translating their canonical draw state/run payloads (`CanonicalDrawState` plus
`DrawRunDesc` and `DrawUniformPayload`, or imported `FlatDrawStateView`
equivalents) and `ClearDesc` / `SwapDesc` payloads into correct Metal commands.
It knows nothing about D3D9 COM objects. `fixture::DrawDesc` is a tests/offline
helper only; it is not a production backend input contract.

**Render-provider policy requirements `R-BACK-42.1`–`R-BACK-42.7` are owned by
[`render-provider/requirements.md`](render-provider/requirements.md).** They
classify stable provider modes, experimental candidates, diagnostics, and
retired selectors independently from implementation and default state.

---

## 1. Correctness of Translation

**R-BACK-1.1** Every production draw submitted by the core as
`CanonicalDrawState` plus `DrawRunDesc`, or as an imported `FlatDrawStateView`
equivalent, must produce rendering results that are equivalent to what a
conformant D3D9 implementation on the same geometry and state would produce,
within the precision limits of the Metal backend GPU.

**R-BACK-1.2** The results of a recorded draw command must be visible in the render
target before the next present command on the same swap chain commits.

**R-BACK-1.3** Draw calls must execute in submission order with respect to the same
render target. A draw call must not read stale color or depth data written by a prior
draw call targeting the same surface.

**R-BACK-1.4** The backend must not reorder operations that produce observable
side-effects on render targets, except where such reordering is invisible to the
application (e.g., merging render passes with identical attachments).

---

## 2. Command Encoding

**R-BACK-2.1** The Wine/application thread must not block on Metal API calls during
draw submission. Metal command encoding must occur on a dedicated encode thread.

**R-BACK-2.2** The application thread and the encode thread must synchronize only
through a bounded queue. The maximum number of frames the application thread can
outrun the encode thread is a configurable limit (default: 3).

**R-BACK-2.3** The encode thread must not allocate from the system heap during normal
encoding. All per-frame memory (argument buffers, imported replay storage, temporary
staging) must be drawn from pre-allocated ring allocators.

**R-BACK-2.4** The encode thread must group draw calls sharing the same render
targets into a single `MTLRenderCommandEncoder` where exact retained-resource
hazard tracking shows no read-after-write, write-after-write, or write-after-read
conflict between them. Splitting into multiple encoders is correct but unnecessary
when exact tracking is clean; the requirement is that merging must be considered.

**R-BACK-2.5** A `Clear()` issued before any draw call on a render target must be
expressed as `MTLLoadActionClear` on the render pass descriptor, not as a separate
blit or draw. This is a performance requirement; the clear results must be identical
to a mid-scene fill-rect clear.

**R-BACK-2.6** A render target change (`SetRenderTarget`) during a scene must
terminate the current `MTLRenderCommandEncoder` and begin a new one. The previous
render target's store action defaults to `MTLStoreActionStore` and may be relaxed
to `MTLStoreActionDontCare` only when the live-out proofs in
`R-BACK-15.7` / `R-BACK-15.8` apply, the safety invariants in
`specs/backend/render-pass-actions/requirements.md` section 5 are satisfied, and
the application contract for the resource (lock, present source, MSAA resolve)
does not require preservation. The encoder-split clause is unchanged: a render
target change always terminates the current encoder and begins a new one.

**R-BACK-2.7** The default Wine runtime path must submit work to the backend as
committed command chunks. Per-draw or per-state backend entry points may exist for
tests or bootstrap, but they must not be the hot path.

**R-BACK-2.8** A committed command chunk must cross the Wine PE/unix boundary with
one bridge operation. The backend must not require one `WINE_UNIX_CALL` per D3D9
`Set*`, `Draw*`, or `Clear` call.

**R-BACK-2.9** The unix-side importer must validate every command record before it
is queued for encoding. Invalid record kinds, invalid payload sizes, stale handles,
and malformed offsets must fail the chunk without dereferencing untrusted pointers.

**R-BACK-2.10** The command queue must enforce bounded chunk capacity by command
count and byte size. If the PE side submits a chunk larger than the negotiated
limits, the backend must reject it rather than allowing unbounded encode latency.

**R-BACK-2.11** The bridge ABI for committed chunks must remain POD and versioned.
The unix-side importer may translate validated records into queue-local replay
actions or direct encoder operations, but C++ lambdas and process-local pointers
must never cross the PE/unix boundary.

**R-BACK-2.12** Present-bearing chunks must carry explicit present metadata through
import, encoding, command-buffer commit, and completion. Non-present chunks advance
the normal sequence timeline only; they must not allocate frame-latency tokens.

**R-BACK-2.13** The command queue must own execution chunk lifecycle after import:
queue-slot assignment, encode-thread dispatch, command-buffer commit tracking,
finish-thread completion, allocator reclamation, sequence signaling, and
present-frame-token signaling.

**R-BACK-2.14** The importer must be the only backend stage that accepts raw
committed chunk bytes from the bridge. It must translate them into compact imported
records and retained backend handle references before the command queue may execute
the chunk.

**R-BACK-2.15** Backend replay and encode code must consume imported records and
explicit production state views (`FlatDrawStateView`, encoder state, resource
binding state, allocator cursors, cache keys, etc.). It must not consume PE COM
objects, PE `DeviceState` pointers, Objective-C objects from the bridge payload,
or ad-hoc per-call mutable state that is not owned by the execution chunk or
queue.

**R-BACK-2.15.1** Large draw-uniform payloads (shader constants, fixed-function
matrices, texture transforms, and clip planes) must be stored outside hot draw
state records and referenced by stable queue-local handles or spans. Hot
PSO/resource decisions must use hashes and compact flat records, not full constant
arrays.

**R-BACK-2.16** Replay transforms that derive encoder decisions from imported
records (pass merge/split, deferred clear application, hazard classification, PSO
key construction, argument-buffer layout) must be deterministic for identical
imported input records, retained resource metadata, and explicit queue-local state.
Backend caches may affect latency, but must not change the encoded command
semantics.

**R-BACK-2.17** The backend ownership split must match the DXMT target shape:
`CommandQueue` owns chunk execution, encode and finish threads, Metal command-buffer
lifetime, sequence fences, and frame tokens; `Presenter` owns drawable acquisition
and `presentDrawable` encoding; the importer owns validation and retention of POD
records and backend handles.

**R-BACK-2.18** A committed `CommandChunk` wire image must be composed of contiguous
POD storage: one fixed `ChunkHeader`, one fixed-width command record header array,
one payload arena, and one opaque handle table. Records must address variable data by
offset and size into the payload arena, not by process-local pointers.

**R-BACK-2.19** Wire headers and imported record headers must use fixed-size integer
fields with explicit ABI packing, alignment, and byte-order rules. The PE builder and
unix importer must enforce static `sizeof`/`alignof` checks for every wire header and
runtime checks for negotiated header sizes before any record payload is decoded.

**R-BACK-2.20** The importer must validate every payload range with overflow-safe
arithmetic. `payloadOffset + payloadSize` and all nested payload-relative ranges must
remain within the chunk payload arena and satisfy the alignment required by that
record schema. Whole-chunk preflight, including nested UP index/vertex/constant
sections, must complete before replay applies any state or dispatches any command.

**R-BACK-2.21** Command record schemas must be POD and fixed-layout. Wire records and
payloads must not contain COM pointers, Objective-C object pointers, unix-side object
pointers, vtables, polymorphic objects, lambdas, `std::function`, allocator-owned
containers, or any pointer that is meaningful only inside one process.

**R-BACK-2.22** Resource references inside a committed chunk must use indices into
the chunk handle table plus schema-defined kind information. The importer must reject
out-of-range indices, kind mismatches, stale handles, and records whose declared
handle ranges exceed the handle table before retaining any resource for execution.
For each record, the non-null payload reference set and declared handle range must
match in both directions; missing, extra, duplicate-substitution, and orphan table
entries must reject the whole chunk.

**R-BACK-2.23** Imported records must be stored in cache-friendly contiguous arrays or
arena-backed POD storage owned by the execution chunk. The encode thread must be able
to replay records by linear iteration without performing bridge-handle lookups or
allocating one heap object per command.

**R-BACK-2.24** The command chunk ABI must be versioned at the chunk header level.
The importer must reject chunks with unsupported ABI versions. Unknown opcodes in a
compatible version must be rejected unless the record is explicitly marked ignorable
and its full payload and handle ranges validate without executing it.

**R-BACK-2.25** Reserved fields in chunk headers, record headers, and fixed payload
headers must be validated as zero on import. Non-zero reserved fields must reject the
chunk so future ABI extensions are not silently misinterpreted by older importers.

**R-BACK-2.26** PE-side recording must append ordinary draw/state commands into
pre-reserved contiguous chunk storage. Exhausting command or payload capacity must
seal/submit the current chunk or use a bounded slow path outside the steady-state draw
hot path; it must not introduce per-command system heap allocation in normal
recording.

**R-BACK-2.27** Import, replay, and encode hot paths must use execution-slot storage
or preallocated ring allocators for command records, retained handle references,
argument buffers, staging, copy-temp, and imported replay storage. Cache misses may
allocate cache objects, but steady-state CPU/GPU command replay must not allocate
from the system heap.

**R-BACK-2.28** Encoder split decisions for resource hazards must be based on exact
read/write handle-set overlap. Probabilistic Bloom overlap checks may remain only as
diagnostic counters for false-positive detection; Bloom false positives must not
force a render-pass split in the default path.

**R-BACK-2.29** A single committed `CommandChunk` may produce one or more
`MTLCommandBuffer` instances chained in submission order on the same
`MTLCommandQueue`. The chunk's `seqId` covers the entire chain, and
`completedSeqId` advances only after the **final** `MTLCommandBuffer` in the
chain reaches the Metal Completed state. Sub-`MTLCommandBuffer` boundaries are
encode-thread implementation detail and must not be visible to the PE side, the
importer, the resource pool, or the present pacing path.

**R-BACK-2.30** When a present-bearing chunk is split into a chain of
`MTLCommandBuffer` instances under `R-BACK-2.29`, present metadata (drawable
acquisition, `presentDrawable` encoding, frame-token signaling) must attach to
the **last** `MTLCommandBuffer` of the chain only. Earlier sub-buffers must not
acquire a drawable, must not encode `presentDrawable`, and must not advance the
present-frame-token. Their completion contributes to chunk progress only
through the chain's final fence.

**R-BACK-2.31** Mid-chunk `MTLCommandBuffer` split decisions must be
deterministic with respect to imported record content, retained handle
metadata, and explicit queue-local state. Split policies based on wallclock
time, GPU progress feedback, or other non-deterministic signals are forbidden.
The encode thread must produce the same chain shape for identical chunk inputs
(R-BACK-2.16 extension).

**R-BACK-2.32** Resource reclaim, transient slab rotation, deferred destruction
gating, and any operation keyed off `completedSeqId` must continue to fire only
when a chunk's `seqId` reaches the completed state per `R-BACK-2.29`. No
backend stage may reclaim or invalidate resources based on the completion of
an intermediate sub-`MTLCommandBuffer`. Sub-buffer completion order on the GPU
is guaranteed by Metal's same-queue in-order submission and must not be
re-derived by the queue tracker.

**R-BACK-2.33** When a mid-chunk commit policy under `R-BACK-2.29` is active,
the encode thread must enforce a configurable per-chunk chain-length cap.
The default cap is 4 sub-`MTLCommandBuffer` instances per chunk (chain tail
counted toward the cap). Once the cap is reached the encode thread continues
encoding into the current sub-buffer until `flushRender(Final)` at chunk exit;
no further mid-chunk commit may fire. The cap is necessary because
unbounded splitting at every render-pass boundary on a heavy frame
(`docs/research/g-axis-tuning.md`) accumulates Apple-Silicon tile-flush
overhead that exceeds the pipelining win past ≈4 sub-buffers. A cap value of
0 disables the cap and is reserved for diagnostic A/B comparison; production
runs must use a positive cap.

**R-BACK-2.34** The production default mid-chunk commit policy must be
`per-render-pass` paired with the `R-BACK-2.33` default cap (4). The opt-out
`off` mode must remain reachable through the `DXMT9_MID_CHUNK_COMMIT_POLICY`
env knob so workloads that prefer the legacy 1-CB-per-chunk shape can disable
sub-CB chaining without recompilation. Justification: the X1 chain-parametric
measurement (`docs/boundary-baseline-measurements.md`) shows wall-clock `-5%`,
encode CPU `-63%`, drawable-acquire wait `-20%` under the new default; the U1
SFIV heavy-scene measurement (`docs/sfiv-benchmark-measurement.md`) is neutral
on fps but `-44%` on `gpu_command_buffer_time_ms` p99. The default trades a
small worst-case tile-flush overhead (≈2.1 ms / frame on SFIV-class envelopes
per `docs/research/g-axis-tuning.md`) for a measurable encode-thread +
drawable-acquire win on chain-rich frames, while the worst case stays bounded
by R-BACK-2.33's cap.

**Encode scheduling requirements `R-BACK-2.35`–`R-BACK-2.50` are owned by
[`encode-scheduling/requirements.md`](encode-scheduling/requirements.md).**
They distinguish CPU-ready source publication from partition, physical encoder,
logical render-pass, and submission boundaries, and define overlap,
`EncodeSession`, completion, and promotion contracts.

**R-BACK-2.51** *(Commit-replay offload contract.)* The commit-replay
offload path (`DXMT9_OFFLOAD_COMMIT_REPLAY`, **engine default ON since
2026-07-10** — explicit `0` opts out; `DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE`
unset follows the offload state because the pair is coupled) must (a) keep wire
header/range validation and import synchronous on the app thread before any
record is handed off. The default and Legacy lanes also keep the historical
combined resource-mark/backing-capture step synchronous before handoff. An
explicitly admitted CPU-ready Direct lane may instead persist canonical
resource identities, backing snapshots, wrapper retention, and raw-residency
tokens on the app thread, then apply the exact owning `seqId` resource mark
after strict admission and before `Sealed -> Ready` visibility as required by
`R-BACK-2.38` and `R-BACK-2.60`. StateOnly performs no resource mark; an
opt-in planned Legacy lane marks before semantic replay. Disabling the
CPU-ready gate must preserve the historical combined synchronous path and skip
worker planning; (b) preserve record order
by draining the raw-chunk queue through a single FIFO replay worker, never
reordering or parallelizing replay across chunks; (c) pace present-bearing
commits with a present-ordinal frame-latency boundary
(`CommandQueue::waitPresentOrdinalBoundary`) that honors the resolved
`BoundaryPolicy` and stays order-isomorphic to the inline present boundary;
(d) before a direct (non-chunk) device call observes or mutates unix-side
state, it must choose one of three ordered fences: (i) when the call has one
ledger-classified target resource, canonicalized by underlying core-buffer
identity rather than by one D3D wrapper, wait only while that resource's
synchronously published `lastQueuedSeq` is greater than the replay worker's
`lastReplayedSeq`; (ii) a valid `D3DLOCK_DISCARD` buffer lock may bypass replay
waiting only when runtime dynamic renaming is enabled and can select a fresh
or idle non-in-flight backing, and every chunk queued before the rename must
hold raw-entry residency on, retain, and consume its synchronously captured
pre-rename backing; or (iii) when the call is not completely
classifiable to one target resource, is device-wide, spans multiple resources,
performs readback, present, reset, shutdown, query, or state-block work, or
observes a stopped or poisoned replay worker, it must use the existing global
drain and propagate its fail-stop outcome before entering the provider as the
conservative fallback; or (iv) a Managed plain writable buffer unlock admitted
by the R-BACK-44 mutation-offload mode (`DXMT9_MANAGED_MUTATION_OFFLOAD`,
`specs/backend/buffer-mutation-offload/requirements.md`) may substitute
ordered FIFO-mutation admission for the pre-mutation wait: the unlock's
reserve/rotate/commit transaction (R-BACK-44.2) fixes the mutation's FIFO
ordinal before the logical rotation becomes visible, publishes it against the
buffer's resource-scoped drain target, and the replay worker applies the byte
materialization at exactly that position (R-BACK-44.3), so every consumer
outside that order remains covered by fence (i) unchanged. A provider-facing
call that is intentionally exempt
from draining, including `BeginScene` and `EndScene`, must still acquire-check
the device terminal replay state and must not enter the provider after stop or
poison. Wrapper lifetime-only addref/release calls are the sole exception and
must remain reachable after terminal publication so teardown can complete.
This single-core-buffer allowlist is the conservative implemented special case;
the generalized multi-resource conflict-prefix optimization is owned by
`R-BACK-2.61`. A call outside the allowlist may use that optimization only when
its complete canonical access summary and FIFO-prefix proof satisfy
`R-BACK-2.61`; a missing, stale, global, or unknown summary selects the full
drain.
Replay failure must publish its terminal state, poison the ledger, and
stop admission before publishing queue completion. A resource-scoped
wait must not return while pending unreplayed work references its target, and
the DISCARD lane must not weaken ordering for any non-DISCARD conflicting
access; (e) fail-stop on a deferred replay
failure — poisoning later commits and aborting pending present-ordinal
waits — without synthesizing a per-record HRESULT for a chunk that failed
after its synchronous validation phase already returned success; and (f)
remain byte-identical to the inline (non-offload) replay path when the flag
is explicitly set to `0` (the opt-out — unset resolves to ON since the
2026-07-10 engine-default flip). (g) *(Per-present-context boundary suppression.)* Enabling the
offload flag must not globally suppress `CommandQueue::submitPresent`'s
inline seqId-based present boundary for every present in the process — only
the specific present whose `core::SwapDesc::pacedByPresentOrdinal` was
stamped by the D3D9 chunk-replay path that already paced that present through
`waitPresentOrdinalBoundary` may skip the inline boundary
(`dxmt9::resolvePresentBoundaryAction`). A present that does not flow through
that path — a direct (non-chunk) COM caller in the same process, or a present
replayed by the synchronous non-offload chunk-replay path — must keep the
inline boundary and its own frame-latency pacing even while
`DXMT9_OFFLOAD_COMMIT_REPLAY` is globally enabled for other, paced presents.
(h) *(Cap honoring.)* The present-ordinal boundary's effective frame latency
must honor `DXMT9_CAP_FRAME_LATENCY_TO_BACKBUFFERS` the same way the inline
boundary's `presentBoundaryLatency()` does, using the committing chunk's
swapchain `backBufferCount` (`dxmt9::cappedFrameLatency`).
(i) *(Raw-queue entry immutability.)* A raw-chunk queue entry — the unix-owned
wire copy plus its retained wrapper references — must be immutable from push
until the replay worker consumes it, and must be consumed exactly once in FIFO
order. No producer thread, direct device call, or diagnostic path may mutate a
queued entry in place; any future scoped-drain or rename optimization that
inspects queued entries must treat them as read-only. See
`specs/backend/spec.md` §Commit-Replay Offload for the architecture. Verified
by `dxmt9-replay-offload-queue-spec` (raw-queue FIFO/bound/drain-fence
rules), `dxmt9-present-ordinal-boundary-spec` (ordinal target math, planner
policy mapping, capped-latency math, and the `PresentOrdinalGate` wait/abort
mechanics), `dxmt9-present-boundary-policy-spec`
(`resolvePresentBoundaryAction` per-present truth table), and the
`PresentFrameLatency.tla` ordinal-variant invariants
(`PresentOrdinalWaitIsomorphism`) checked by `dxmt9-verify-tla`. Clauses (g)
and (h) close the two boundary-pacing gaps that previously blocked promoting
this flag to an engine default; the third blocker — offload-forced crashes in
the former imported-record harnesses that looked like a replay-worker
resource-retention defect — was root-caused as a test-harness drain gap, not a
production race (`cad446ce`). Those runtime harnesses were retired with the
pointer-bearing producer/importer; current offload coverage uses the canonical
format only.
With all three
resolved, the engine default
flipped to ON on 2026-07-10 (`d45af067`) — see the `specs/backend/gap.md`
"Commit-replay offload" row.

**R-BACK-2.52** *(Inline const delta contract.)* The experimental-candidate
inline-const-delta wire mode (`DXMT9_PE_INLINE_CONST_DELTA`, read once at first
use) must
(a) when the flag is unset, keep replay behavior identical, keep every
pre-existing packet field at its pre-change offset, and append zero
const-payload bytes (the fixed per-section `{valid,start,count}` header block
is an additive tail whose cross-build safety clause (g) gates);
(b) when set, let `Draw*` records carry per-shadow merged
constant-delta sections (VS/PS × float/int/bool, each `{valid, startRegister,
registerCount}` plus payload, mirroring the `D9CCommandRecordSetConst`
element-size rules) instead of emitting standalone
`D9C_COMMAND_RECORD_SET_*_CONST_*` records for constants consumed by that
draw; (c) validate every section at import — register range against the
D3D9 register-file limits and payload length against the record header —
before any state is applied or any handle is retained, rejecting the chunk on
violation exactly like other malformed packets; (d) apply const sections with
replay semantics observably equivalent to replaying the equivalent standalone
const records immediately before the same draw, preserving chunk order;
(e) keep standalone const records for every non-`Draw*` consumer of shader
constants (`ProcessVertices`, chunk-barrier and chunk-end const drains) — the
inline path may fold only constants that the carrying draw consumes; (f) not
introduce new draw-run break classes: a const-bearing packet may break a
coalesced run at most where the equivalent standalone const record breaks it
today; and (g) bump the PE/unix wire schema so the existing
`DXMT9_WINEMETAL_CALL_ABI_HASH` handshake refuses mixed builds. Verified by
the PE record byte-pinning specs (off-path unchanged plus new inline-section
rows) and an on/off replay-equivalence spec following the
`pe_full_snapshot_equivalence_spec` pattern.

**R-BACK-2.53** *(Retired wire grammar absent.)* Production PE and unix code
must not declare, advertise, negotiate, produce, import, or replay the retired
numeric wire version 1 envelope or records. Numeric wire version 1 and every
unsupported version must be rejected before validation, retention, state
mutation, or queue submission; no retired struct, conversion adapter, or replay
fixture may remain in active code.

**R-BACK-2.54** *(Canonical stable handle-index ABI.)* The canonical command-
chunk format has numeric wire version 2 and must use `uint32_t` handle-table
indices instead of payload-embedded server-wrapper addresses.
Each table entry must carry a schema kind, stable object identifier, and
generation. The generation stamp must be a nonzero full `uint32_t`, and the
device-local registry must match the exact `{kind, objectId, generation}` before
any retain, replay, dispatch, or state-mutation side effect. Releasing a slot at
`UINT32_MAX` generation must retire it instead of wrapping. Null must use the
schema-defined index sentinel and must not occupy a table entry. Every non-null
payload index must fall within its record's canonical handle slice and resolve
to the declared identity. A chunk must contain one wire version only, and
PE/unix negotiation plus the ABI-hash handshake must reject unsupported or
mixed-version builds.

**R-BACK-2.55** *(Canonical sparse draw packets.)* A canonical numeric-version-2
draw record must store fixed draw arguments plus a canonical, typed section-
descriptor array. Only changed state sections and required draw/UP data may ride
the payload; absent sections mean no delta, while explicit null handle indices
mean unbind. Section kinds must be unique and sorted, and every
`{offset, byteSize, elementSize, count}` must pass overflow-safe alignment,
non-overlap, and schema-size validation. The importer must preflight the complete
record/chunk, including UP index, vertex, and inline-constant sections, before
applying any state.

**R-BACK-2.56** *(Recorder/replay scratch ownership.)* Steady-state PE wrapper
retention and unix replay staging must reuse capacity-preserving chunk/device or
thread-local arenas. Per-record rollback must use checkpoints, and duplicate
retains must be eliminated by an exact flat set or equivalent bounded
structure. Draw-run parameters, binding overrides, payload views, pending
submissions, and resolved core-handle lists must not allocate fresh containers
for each replayed record or run after warm-up.

**Encode scheduling requirements `R-BACK-2.57`–`R-BACK-2.66` are owned by
[`encode-scheduling/requirements.md`](encode-scheduling/requirements.md).**
They define immutable published storage, serial partition consumption,
CPU-ready residency, scoped FIFO drains, production planning, parallel render
encoding, Metal 4 segmented logical passes, deterministic admission
headroom/session caps, and stable render scheduling provider modes.

---

## 3. Pipeline State Objects

**R-BACK-3.1** The backend must cache compiled `MTLRenderPipelineState` objects.
Two production draw states (`CanonicalDrawState` / `FlatDrawStateView`) that
produce the same PSO key must receive the same `MTLRenderPipelineState` object
without recompilation.

**R-BACK-3.2** PSO compilation must not block draw submission on the hot path after
the cache is warm. The first draw call that requires a new PSO may stall; subsequent
draws with the same PSO must not.

**R-BACK-3.3** The PSO key must include exactly the states that Metal bakes into
`MTLRenderPipelineState`: vertex and fragment function identity (including variant
specialization), vertex descriptor layout, render target pixel formats, blend state
per attachment, sample count, and alpha-to-coverage.

**R-BACK-3.4** States that Metal encodes dynamically on the command encoder (cull
mode, fill mode, viewport, scissor, depth bias, stencil reference) must not be part
of the PSO key.

**R-BACK-3.5** The `MTLDepthStencilState` object must be cached separately from the
render PSO. Its key is the set of depth and stencil compare/write state (depth enable,
depth write, depth func, front/back stencil ops and masks).

**R-BACK-3.6** The backend must support async PSO compilation. A PSO may be compiled
on a background thread; the encode thread blocks on first use if compilation is not
yet complete.

**R-BACK-3.7** The PSO cache must be load-warmable from the on-disk
`MTLBinaryArchive` at device creation. Cache load must populate
`MTLRenderPipelineState` lookups for every (shader hash, variant) pair present
in the archive before the first draw is encoded. Load failures (missing file,
schema mismatch, unsupported GPU family) must fall back to fresh-compile
behavior without blocking device creation.

**R-BACK-3.8** Prewarm scope must be configurable: full archive load on device
init (default for shipping builds), lazy on first draw (default for dev), or
disabled (debug). The selected mode must be observable via a counter and the
chosen archive path/identity must be visible in present diagnostics.

**R-BACK-3.9** *(Non-blocking prewarm.)* A successful `Full` prewarm must not
block device creation on archive I/O or deserialization: the load must run
off the device-init critical path (asynchronously), with PSO lookups falling
back to fresh compilation until the load completes, and pipeline-cache
archive writes that race the load must be preserved (queued or merged), not
dropped. Additionally, an archive whose on-disk size exceeds a configurable
guard (`DXMT9_ARCHIVE_MAX_PREWARM_MB`, default generous) must demote to the
lazy path with a diagnostic instead of loading. Evidence class: 3DMark05
self-aborted deterministically when a `125MB` archive was full-prewarmed
synchronously inside `CreateDevice`
(`docs/perfomance/present-pacing/present-pacing-inline-const-delta.201.md`).

**R-BACK-3.10** *(Persistence without clean shutdown.)* Archive persistence
must not depend solely on clean device destruction: after the compile set
stabilizes (a bounded new-entry-quiescence or present-count milestone), the
runtime must serialize the archive once mid-session under the existing
`LOCK_EX` + atomic-rename contract, so a process later terminated without
clean teardown still leaves a warm archive. Repeated mid-session saves must
be bounded (no per-present serialization).

**R-BACK-3.11** *(Diagnostic-variant pollution guard.)* Shader variants whose
identity includes a non-default shader debug-env key (the
`DXMT_DISABLE_*` / `DXMT_FORCE_*` classifier family) must not be persisted
into the shared production archive: a session with a non-default shader
debug-env key must skip archive save (load may proceed). Rationale: probe
campaigns grew the shared archive `68KB -> 125MB`, which is what armed the
R-BACK-3.9 failure.

**R-BACK-3.12** *(Backend identity canonicalization.)* The backend PSO identity
must be a canonical representation of Metal-visible pipeline state, without
mutating the D3D9 semantic shadow. Portable fixed-function pixel identity must
exclude `alphaTestEnable` and `alphaTestFunc`, because the portable fragment
source evaluates those values from per-draw state. Tile-FFP identity must retain
both fields because its tile source consumes them. General draw PSO identity
must not vary with sampler min/mag filtering; the independent stretch/blit PSO
may retain its linear-filter bit.

**R-BACK-3.13** *(Inactive blend normalization.)* For an attached color
format, a blend-disabled attachment must canonicalize operations and factors to
`Add`, `One`, and `Zero` while preserving its color-write mask and pixel format.
An invalid or zero color format must canonicalize the complete attachment entry
to the no-attachment identity. An attached blend-enabled entry must preserve
all active blend fields.

**R-BACK-3.14** *(Bounded handle publication.)* Draw PSO handles must be
published through a bounded, append-only table with stable numeric slots and
release/acquire visibility. Publication must not clone the complete slot prefix
per new PSO, and stale generation/invalid handles must fail closed.

**R-BACK-3.15** *(Identity evidence.)* The canonicalization transforms in
R-BACK-3.12/3.13 must be pure, production-called functions covered by native
truth tables. Their model binding is value-level; Metal shader ABI and pixel
behavior remain subject to the separate shader-corpus and GPU-oracle gates.

**R-BACK-3.16** *(Cold PSO diagnostics.)* PSO fanout diagnosis must be an
explicit opt-in observer whose disabled path performs no clock read, key-payload
read, allocation, or lock acquisition after its cached gate. The observer must
separate probe lookup, source generation, source-library lookup, final lookup,
final insertion, and handle publication. Key cardinality must include source
tuple, pre-source backend identity, and every retained final-key axis; storage
must be bounded and report saturation rather than allocate without limit.
Observer-enabled timing is attribution evidence only and cannot promote a
performance change without a separate observer-off run.

---

## 4. Shader Translation

**R-BACK-4.1** The backend must accept D3D9 shader bytecode for SM 1.x, 2.0, and 3.0
vertex and pixel shaders and produce Metal shader functions that compute equivalent
results.

**R-BACK-4.2** The backend must accept `FFPKeyVS` and `FFPKeyPS` values from the core
and produce Metal vertex and fragment functions that implement D3D9's fixed-function
lighting, transform, texture combining, and fog behavior for those keys.

**R-BACK-4.3** Compiled shader functions (both from bytecode and from FFP keys) must
be cached persistently across process restarts, keyed by a stable hash of the input
(bytecode content hash or FFP key value plus variant parameters). Recompilation on
cache hit is not permitted.

**R-BACK-4.4** The half-pixel offset correction (as specified in core/spec §7) must
be applied to every vertex shader before translation. The translation output must
include this correction.

**R-BACK-4.5** Alpha test (as specified in core/spec §8) must be encoded in pixel
shader variants when required by the key. The pixel shader must execute
`discard_fragment()` for failing fragments before any color output.

**R-BACK-4.6** Programmable pixel-shader translation must not use a default
texture-coordinate V flip to compensate for D3D/Metal coordinate differences.
The generated MSL sampling path must preserve the D3D V coordinate unless the
diagnostic `DXMT_DEBUG_FORCE_PIXEL_V_FLIP` flag is enabled. This debug flag is
not part of normal rendering correctness and must stay independent from
`DXMT_DEBUG_FLIP_VERTEX_Y`.

**R-BACK-4.7** Clip planes enabled via `D3DRS_CLIPPLANEENABLE` must be emitted as
`[[clip_distance]]` outputs in the vertex shader. This is a Metal hardware feature
that must not be emulated with fragment shader discards.

**R-BACK-4.8** The shader compile path must persist every produced
`MTLLibrary` / `MTLFunction` to a process-shared on-disk
`MTLBinaryArchive`, keyed by SHA-1 of the input bytecode plus variant key.
Subsequent process starts must load from this archive before any compile
attempt; archive misses fall through to compile and write back. The archive
identity (file path, version) must be cross-process consistent so multiple
dxmt9 instances can warm each other's caches.

---

## 5. Resource Allocation

**R-BACK-5.1** `createBuffer()` must return a handle to a GPU-accessible `MTLBuffer`.
The storage mode must be appropriate for the pool and usage: `D3DPOOL_DEFAULT` with
no CPU writes maps to private storage; `D3DPOOL_DEFAULT` with `D3DUSAGE_DYNAMIC` maps
to shared or managed storage.

**R-BACK-5.2** `createTexture()` must return a handle to a `MTLTexture` with the
correct pixel format, dimensions, mip count, array count, and texture type (2D, cube,
3D, or array).

**R-BACK-5.3** `mapBuffer(handle, D3DLOCK_DISCARD)` must return a writable pointer to
a fresh, non-overlapping buffer region. The previous contents are undefined. The
implementation may return a new sub-allocation within a ring buffer.

**R-BACK-5.4** `mapBuffer(handle, D3DLOCK_NOOVERWRITE)` must return a pointer to the
current buffer allocation. The backend may assume the caller will not overwrite
in-flight regions. No synchronization against the GPU is required.

**R-BACK-5.5** `mapBuffer` with neither `DISCARD` nor `NOOVERWRITE` must wait until
the GPU has completed all commands that read from the buffer before returning,
except for `D3DPOOL_MANAGED` buffers covered by the CPU-shadow/versioned-backing
contract in R-BACK-5.11.

**R-BACK-5.6** `destroyBuffer()` and `destroyTexture()` must not free the underlying
Metal object until all in-flight GPU commands that reference it have completed.

**R-BACK-5.7** D3D9 pool / usage combinations must map to Metal storage modes
according to the following contract. The mapping must be selected at resource
create time, must not change for the resource's lifetime, and must be
identical between buffers and textures of the same pool/usage class.

| D3D9 pool | D3D9 usage | Apple Silicon (unified memory) | Intel/AMD (discrete-style) |
|---|---|---|---|
| `D3DPOOL_DEFAULT` | none / `RENDERTARGET` / `DEPTHSTENCIL` | `MTLStorageModePrivate` | `MTLStorageModePrivate` |
| `D3DPOOL_DEFAULT` | `D3DUSAGE_DYNAMIC` | `MTLStorageModeShared` (rename ring) | `MTLStorageModeShared` (rename ring) |
| `D3DPOOL_MANAGED` | any | `MTLStorageModeShared` (no staging copy) | `MTLStorageModeManaged` (with staging) |
| `D3DPOOL_SYSTEMMEM` | any | host malloc + `MTLStorageModeShared` upload buffer | host malloc + staging |
| `D3DPOOL_SCRATCH` | any | host malloc only (never reaches GPU) | host malloc only |

Unified-memory detection must use `MTLDevice.hasUnifiedMemory`. The intent is
that on Apple Silicon, `MANAGED` pool resources do **not** require a separate
private-storage copy and CPU↔GPU upload — both sides see the same backing.

**R-BACK-5.8** `D3DUSAGE_DYNAMIC` buffer rename (on `D3DLOCK_DISCARD`) must
draw from a per-frame ring of `MTLStorageModeShared` allocations. A rename
that finds no free allocation in the ring must fall back to a fresh
`newBufferWithLength:options:` rather than blocking on prior GPU completion.

**R-BACK-5.9** Many small textures (sub-`64 KB` allocation footprint, e.g.
D3D9 lightmaps, decals, glyph atlases, particle sprites) must be backed by a
shared `MTLHeap` of appropriate storage mode rather than individual
`newTextureWithDescriptor:`. Heap residency tracking is per-heap, not
per-texture; per-texture residency calls on heap-backed textures must be
elided. Textures larger than the heap-eligibility threshold or with usage
flags incompatible with heap allocation (e.g., specific `framebufferOnly`
combinations) fall through to direct allocation.

**R-BACK-5.10** `MTLHeap` capacity must grow geometrically with a configurable
ceiling. Heap exhaustion must trigger allocation of a new heap rather than
falling back to direct allocation, so subsequent residency cost stays bounded.
Heap reclamation is deferred until all heap-backed textures are freed (same
DXMT-style sequence-ID gate as direct allocations).

**R-BACK-5.11** A writable `D3DPOOL_MANAGED` buffer lock must return immediately
from the buffer's CPU-authoritative shadow without waiting for an in-flight Metal
backing. Writable unlock/upload must copy the complete CPU shadow, including
bytes outside a partial lock range, into the active backing when it is idle or
otherwise rotate to another idle `MTLStorageModeShared` backing. If no backing is
idle, upload must allocate a fresh backing rather than wait for GPU completion.
Every draw that references a versioned MANAGED buffer must snapshot the concrete
Metal buffer handle and CPU-visible contents associated with that draw. Each
backing has its own last-used sequence watermark and must not be overwritten or
reused until that sequence is at or below the completed watermark. The logical
buffer retains a monotonic aggregate last-used watermark so destruction cannot
release any version still referenced by queued or in-flight work. Read-only
locks do not rotate or upload a backing.

**R-BACK-5.12** A successful `D3DPOOL_DEFAULT | D3DUSAGE_DYNAMIC` lock carrying
`D3DLOCK_NOOVERWRITE` must publish only its successful `(offset, size)` byte
range at unlock. The CPU shadow remains complete, but the backend upload must
not copy unrelated shadow bytes into the active Metal backing: those bytes may
still be read by in-flight draws. Range arithmetic is overflow-safe and rejects
an out-of-bounds publication before touching either shadow or Metal contents.
`DISCARD`, plain writable locks, and `D3DPOOL_MANAGED` retain their existing
full-shadow upload contracts. The production path is implemented by the
`core::Buffer` lock metadata → `Device::uploadBufferDataRange` →
`Pool::uploadBufferDataRange` seam; the bounded `NoOverwriteByteRange.tla`
model and native range/sentinel specs are required evidence.

---

## 6. Presentation

**R-BACK-6.1** A present command must display the most recently rendered frame on
the associated window. When `D3DPRESENT_INTERVAL_ONE` (vsync) is requested, drawable
availability and vsync pacing are handled by the presenter/encode path. The
application-facing wait is governed by frame-latency tokens, not by synchronous
drawable acquisition in the device object.

**R-BACK-6.2** A present command with `D3DPRESENT_INTERVAL_IMMEDIATE` must not wait
for vsync. `CAMetalLayer.displaySyncEnabled` must be `NO` for this mode.

**R-BACK-6.3** After `present()` returns, the back buffer contents are undefined
(consistent with `D3DSWAPEFFECT_DISCARD`). The next render pass targeting the back
buffer must use `MTLLoadActionDontCare` or `MTLLoadActionClear`, not `MTLLoadActionLoad`.

**R-BACK-6.4** A present-bearing command chunk must be assigned a monotonically
increasing frame token when it is accepted by the backend queue.

**R-BACK-6.5** Frame-latency waits must target queue/presenter completion of frame
tokens. A frame token is complete only after the Metal command buffer carrying that
present has completed.

**R-BACK-6.6** Frame-latency waits must not be satisfied merely because the encode
thread dequeued, began encoding, or committed the chunk. Encode progress and present
completion are separate timelines.

**R-BACK-6.7** `setMaxFrameLatency(n)` must configure how many present-bearing frame
tokens may remain incomplete. The default is 4. The effective value must be clamped
to the range accepted by the core requirements.

**R-BACK-6.8** The presenter owns drawable acquisition, layer synchronization, and
`presentDrawable` encoding. The command queue owns frame-token allocation and
completion signaling. The API/device layer must not own presenter timing state.

**R-BACK-6.9** Present diagnostics must expose the selected source validity, source
handle/texture identity, source size, format, sample count, render-pass source size,
and destination size. These counters must make a valid 1280x720 SFIV source
distinguishable from missing source, missing texture, resolve, or invalid-size
present failures before present-policy tuning is considered.

**R-BACK-6.10** When an Immediate present still uses the engine-default maximum
frame latency, the backend scheduler must apply an effective one-frame
present-completion boundary. This stricter scheduling bound must not change the
public maximum-frame-latency value. Synchronized presents must retain the normal
default, and a non-default application or environment override must remain
authoritative. The inline queue-sequence boundary and commit-replay
present-ordinal boundary must resolve the same effective latency for the same
present.

**R-BACK-6.11** Presenter drawable acquisition must resolve once into one typed
policy: `Sync`, `PreAcquire`, `SyncOnSubmit`, or `Async`. `Sync` is the default.
Every alternative must preserve Presenter ownership, the frame-token timeline,
and identical Present source selection. Multiple legacy boolean selectors must
resolve deterministically with `Async > SyncOnSubmit > PreAcquire > Sync` until
a canonical single selector replaces them.

**R-BACK-6.12** Present-boundary timing must resolve once into one typed policy.
`PresentCompletion` is the production default; `Default`, `AfterAcquire`, and
`Completion` are stable alternatives. `DeferredPresentCompletion` remains an
experimental candidate, and `Disabled` remains a diagnostic override. Multiple
legacy selectors must use the documented deterministic precedence. No boundary
policy may move drawable ownership out of Presenter, signal completion before
the owning Metal command buffer completes, or weaken explicit resource/query
drains.

---

## 7. Thread Safety

**R-BACK-7.1** The backend interface (`commitChunk`, resource create/destroy,
map/unmap, shader compile, frame-token waits, etc.) must be safe to call from a
single thread (the Wine/application thread). No concurrent calls from multiple
threads are required.

**R-BACK-7.2** Internal backend threads (encode thread, completion thread) must not
be visible to or callable by the core.

**R-BACK-7.3** Resource destruction (`destroyBuffer`, `destroyTexture`) must be safe
to call while in-flight GPU work references the resource. The destruction must be
deferred until GPU completion.

---

## 8. Wine Bridge

**R-BACK-8.1** On Wine, the backend must use the `winemetal` unix-lib thunk mechanism
to cross the Win32/macOS boundary. Direct Objective-C or Metal API calls must not
appear in the PE-side code.

**R-BACK-8.2** The shader compilation path (D3DBC → SPIR-V → MSL, or direct
translation) must be callable from the Win32 side via the `airconv`-style thunk
interface.

**R-BACK-8.3** All Metal object handles passed across the boundary must be opaque
integer handles, not Objective-C object pointers.

**R-BACK-8.4** The thunk mechanism must be used at chunk/resource granularity, not
per D3D9 draw or state operation. The intended hot-path call shape is
`commitChunk()` plus coarse resource lifecycle/map operations.

**R-BACK-8.5** The Wine bridge must expose diagnostic counters for bridge calls and
time spent in each bridge class. At minimum this includes chunk commits, resource
create/destroy/map/unmap calls, frame-token waits, shader compiler calls, and any
compatibility per-call draw/state fallback.

---

## 9. Surface Operations

Detailed API contracts, validation rules, and bridge-recording policy are defined in
[`surface-ops/requirements.md`](surface-ops/requirements.md). The backend design for
Metal replay is defined in [`surface-ops/spec.md`](surface-ops/spec.md).

**R-BACK-9.1** `submitSurfaceCopy()` (used by `UpdateSurface` and `UpdateTexture`)
must correctly copy all specified mip levels and cube/array slices. Row pitch and
slice pitch from the source must be respected exactly. The operation must cross the
Wine PE/unix boundary as POD chunk records, not as per-copy bridge calls.

**R-BACK-9.2** `submitStretchRect()` must produce correct results for both same-size
(blit) and scaled (render pass) copies. The `MTLSamplerMinMagFilter` used for scaled
copies must match the requested `D3DTEXTUREFILTERTYPE`.

**R-BACK-9.3** `submitReadback()` (used by `GetRenderTargetData`) must block the
calling thread until the GPU has written the result to the staging buffer and all
prior writes to the source render target are visible. It must not return until the
data is CPU-readable and copied to the D3D9 destination allocation.

**R-BACK-9.4** `submitColorFill()` must produce a correctly filled surface region.
For full-surface fills, `MTLLoadActionClear` must be used. For partial fills, a
scissored render pass or fragment shader fill must be used.

**R-BACK-9.5** All surface operations must be recorded as command records in the
current chunk, except readback which must additionally commit the current chunk and
wait for completion before returning.

**R-BACK-9.6** Surface operation command records must be fixed-layout POD records
with handle-table references and payload offsets. They must not contain closures,
lambdas, process-local pointers, COM object pointers, or Objective-C object pointers.

---

## 10. Clip Planes

**R-BACK-10.1** When the production draw state has `clipPlaneMask != 0`, the
vertex shader must output one Metal `[[clip_distance]]` value equal to the
minimum signed distance across the enabled D3D9 clip planes (maximum 6 source
planes). This preserves D3D9's rule that a vertex outside any enabled plane is
clipped while respecting the Apple GPU single-distance limitation.

**R-BACK-10.2** With a fixed-function vertex pipeline, the core must transform
world-space clip planes into clip space using the inverse of `View * Projection`;
the world transform must not be included. With a programmable vertex shader,
the core must preserve the app-provided clip-space coefficients unchanged. The
resulting planes must be passed through the fixed-function uniform buffer or a
dedicated small constant buffer.

**R-BACK-10.3** `clipPlaneMask` must be part of the vertex shader variant key. Draws
with different `clipPlaneMask` values must not share a compiled vertex shader.

---

## 11. Multisampling

**R-BACK-11.1** When a render target has `sampleCount > 1`, the `MTLRenderPassDescriptor`
must use a multisample texture as the color attachment with
`storeAction = MTLStoreActionMultisampleResolve` and a single-sample resolve texture.

**R-BACK-11.2** The `PSO.rasterSampleCount` must match the render target's sample
count. A draw call to a 4× MSAA render target must use a PSO compiled with
`rasterSampleCount = 4`.

**R-BACK-11.3** `GetRenderTargetData` on a multisample render target must resolve to
the single-sample texture first, then read back from the resolve texture.

---

## 13. Tile-Shader FFP (Apple Silicon Candidate)

These requirements define an Apple-Silicon-specific candidate path for D3D9
fixed-function fragment effects. Portable is the stable FFP provider. Tile-auto
must resolve to portable until the implementation proves fragment-coverage and
prior-attachment preservation in addition to workload benefit; selection occurs
per render pass from queue-cached GPU capability and render-pass shape.

**R-BACK-13.1** On GPU families that support programmable blending and tile
shaders (`MTLGPUFamilyApple3` and later), the backend may emit FFP fog,
alpha-test, and alpha-to-coverage as tile-stage code instead of executing
them as fragment-stage `discard_fragment()` / fragment-stage scalar ops. The
choice between portable and tile-shader paths must be made per render-pass
descriptor, not per draw, so a pass cannot mix the two.

**R-BACK-13.2** Tile-shader FFP must produce results bit-identical to the
portable fragment path within the precision limits already permitted by
`R-BACK-1.1`. Where Metal's tile stage cannot reproduce a D3D9 corner case
(extreme alpha-test reference values, specific fog non-linear modes), the
selector must fall back to the portable path for that pass, not produce a
different result.

**R-BACK-13.3** The PSO key must include a tile-FFP-mode bit. Two draws with
the same FFP key but different tile-mode selection must compile separate
pipeline states. Tile-mode flips inside a pass are not permitted; they
require a render-pass split.

**R-BACK-13.4** Pass and draw routing must be observable via
`tile_ffp_pass_count`, `portable_ffp_pass_count`,
`tile_ffp_routed_{tile,portable}_{draws,primitives,vertices}`, and fallback
reason counters for precision, unsupported state, GPU family, and mid-pass
ineligibility. While `R-BACK-13.7` is open, a non-diagnostic `auto` request
must keep `tile_ffp_pass_count` at zero; diagnostic `force` may populate tile
counters.

**R-BACK-13.5** Tile-shader FFP must remain disabled on GPU families without
programmable blending support and on any non-Apple-Silicon configuration. The
spec does not require tile-shader FFP to be available; conformance evidence
is for the portable path only.

**R-BACK-13.6** Tile-shader FFP source must be a separate generator, not a
post-hoc transform of the portable FFP MSL. The two paths share the
`FFPKeyPS` value but produce distinct MSL, distinct `MTLFunction` handles,
and distinct cache entries.

**R-BACK-13.7** A tile-stage implementation must modify exactly the samples
covered by the owning D3D9 draw and must preserve the pre-draw attachment value
for every uncovered or alpha-test-rejected sample. An attachment-wide tile
dispatch after a base-colour draw is not sufficient evidence: it must not fog
clear pixels, reprocess earlier draws, or retain a rejected draw's base colour.
Until a coverage/prior-colour mechanism passes partial-draw, overlap, and
multi-draw GPU readback equality, non-diagnostic `tile-auto` requests must fail
closed to the portable provider.

**R-BACK-13.8** Tile-stage fog and alpha-test inputs must have the same
per-fragment provenance as the portable path: the fog blend factor comes from
the owning draw's interpolated fog input, and the alpha-test operand is the
fragment's shaded alpha before any destination blend. Neither value may be
inferred from the destination attachment; a tile candidate that reads
attachment channels as fog distance or as the alpha-test operand is
correctness-invalid regardless of coverage handling, and the carrier that
transports these per-fragment values into the tile stage is part of the
promotable design (`R-BACK-13.7` evidence matrix).

---

## 14. Resource Heap Pooling

Beyond the per-resource `MTLHeap` rule in `R-BACK-5.9`, this section pins the
contract for D3D9 small-resource heap allocation policy.

**R-BACK-14.1** The backend must maintain at least three independent heap
families: private-storage textures (`D3DPOOL_DEFAULT` non-RT non-DS small
textures), shared-storage textures (`MANAGED` / `SYSTEMMEM` small textures on
unified memory), and shared-storage buffers (vertex/index buffers below the
direct-allocation threshold). Heap families must not be cross-allocated;
storage mode and usage flags must be compatible across all heap members.

**R-BACK-14.2** Heap eligibility thresholds must be configurable but must
default to values that capture D3D9's typical small-texture working set:
allocation footprint ≤ 64 KB and a usage class compatible with heap
allocation. Render targets, depth buffers, and dynamic-rename buffers must
always allocate directly, regardless of size.

**R-BACK-14.3** Heap capacity, allocation count, and eviction count must be
exposed as counters per family. A per-workload baseline must show heap
allocation absorbs the expected fraction of total `MTLTexture` /
`MTLBuffer` creates; a regression to direct allocation is observable.

**R-BACK-14.4** Heap-backed resource destruction must observe the same
deferred-destroy sequence-ID gate as direct allocations
(`R-BACK-5.6` / `R-BACK-7.3`). Heap reclamation cannot occur while any
heap-backed resource is referenced by an in-flight chunk.
