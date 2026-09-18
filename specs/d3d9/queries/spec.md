---
type: "Spec"
title: "Query Spec"
description: "D3D9 / Queries spec, ownership, ordering, and evidence mapping."
tags: [specs, d3d9, queries, spec]
---

# Query Spec

D3D9 queries let the application probe GPU state asynchronously. Because dxmt9
uses a deferred command queue, query results are not available until the GPU has
processed the commands that surround the query. The implementation uses POD
query operations plus CPU-visible ordinary and initializer completion fences.
Section 7 describes the implemented completion path. Section 4 describes the
opt-in Metal occlusion result provider; section 5 describes the still-incomplete
timestamp target.

The default-off deferred-END candidate (`R-CORE-11.5(d–e)`) retains that
completion predicate and initializer authority. A queue-local writer owns one
shared marker, independent of public Query lifetime. END stores that marker
and its own fixed initializer prefix. The marker carries admitted source and
storage generations, not a future sequence. The actual `sealAndPublish` seam
binds it once before making the source available to encode. A later writer
has a distinct marker even if physical Slot storage is reused.

Non-FLUSH polls never publish. FLUSH may publish only the marker's exact live
writer, and does not enter a full admission wait: END defers only below the
publication cap, otherwise it uses the eager Issue path. No later publication
may bypass that writer and consume its admission credit. Existing boundaries
also publish and bind the group. Marker storage is Unix/core ownership only;
PE query records and the PE/Unix ABI remain pointer-free and unchanged.
The eager path is the reference and remains default until candidate evidence
is complete. Missing gates are tracked in the query gap row.

---

## 1. Query Types in Scope

| D3DQUERYTYPE | Priority | Notes |
|---|---|---|
| D3DQUERYTYPE_EVENT | Required | GPU fence; most common synchronization primitive |
| D3DQUERYTYPE_OCCLUSION | Required | Sample count from visibility result |
| D3DQUERYTYPE_TIMESTAMP | Optional | GPU timestamp; requires Metal counter support |
| D3DQUERYTYPE_TIMESTAMPDISJOINT | Optional | Always non-disjoint on Metal when timestamps are exposed |
| D3DQUERYTYPE_TIMESTAMPFREQ | Optional | Nanoseconds per tick |
| D3DQUERYTYPE_VCACHE | Omit | Driver cache stats; deterministic zero-compatible data or unavailable |
| D3DQUERYTYPE_RESOURCEMANAGER | Omit | Memory stats; deterministic zero-compatible data or unavailable |
| D3DQUERYTYPE_VERTEXSTATS | Omit | VS invocations; not exposed by Metal |

---

## 2. Sequence ID Fence Mechanism

The command queue assigns a monotonically increasing sequence ID to each
committed `CommandChunk`. The finish thread increments a CPU-visible
`completedSeqId` counter after the GPU signals completion for each chunk.

```
CommandQueue state:
  currentSeqId:    uint64
  completedSeqId:  atomic<uint64>
```

A query requires `completedSeqId >= fence.sequence` as well as successful
initializer completion, a live queue incarnation and the current issue epoch.
An unbound deferred marker never satisfies this condition. The following
diagram describes the default eager-END path.

```mermaid
sequenceDiagram
    participant App
    participant Core as IDirect3DQuery9
    participant CQ as CommandQueue
    participant GPU

    App->>Core: Issue(D3DISSUE_END)
    Core->>CQ: publish ordinary writer and initializer uploads
    CQ-->>Core: actual queue incarnation, prefix N, initializer U

    App->>Core: GetData(FLUSH)
    Core->>CQ: request submission of N (no completion wait)
    Core-->>App: S_FALSE while pending

    CQ->>GPU: MTLCommandBuffer.commit() (chunk N)
    GPU-->>CQ: completion signal
    CQ->>CQ: completedSeqId = N

    App->>Core: GetData(&result, size, flags)
    Core->>CQ: ordinary N and initializer U successfully complete?
    alt resolved
        Core-->>App: S_OK, result filled
    else not yet
        alt D3DGETDATA_FLUSH
            Core->>CQ: retain submission obligation for N
        end
        Core-->>App: S_FALSE
    end
```

---

## 3. D3DQUERYTYPE_EVENT

An event query is a GPU fence with a `BOOL` public result. A caller may poll
with `GetData(NULL, 0, flags)` without requesting result bytes.

In the default path, `Issue(D3DISSUE_END)` publishes the current writer
and captures its actual prefix, rather than inserting a new Metal query
command. No Metal query object is required. Section 7 adds initializer success,
queue incarnation and failure to the ordinary `completedSeqId` condition.

`GetData(NULL, 0, flags)` polls that receipt. If the query is unresolved and
`D3DGETDATA_FLUSH` is set, it requests submission of the published prefix.

```cpp
while (pQuery->GetData(NULL, 0, D3DGETDATA_FLUSH) == S_FALSE) {
  /* spin */
}
```

The sticky submission request is the deadlock breaker for this loop. The
finish thread advances `completedSeqId` only after actual GPU completion;
initializer completion is observed independently.

---

## 4. D3DQUERYTYPE_OCCLUSION

An occlusion query counts the number of samples that pass the depth-stencil test
between `BEGIN` and `END`.

The bounded `DXMT9_METAL_OCCLUSION` candidate uses Metal visibility buffers;
the existing default primitive-count approximation is not GPU visibility.
The candidate is opt-in until its remaining integration evidence is qualified.

The PE QueryIssue wire record is unchanged. Unix core BEGIN creates an
`OcclusionResult` qualified by object identity, issue generation and reset epoch.
Core constructs an immutable active `OcclusionScope` (at most 64 simultaneous
queries); replacing membership first publishes the previous ordinary writer.
Publication attaches that scope to its final ChunkSlot. Repeated BEGIN replaces
only that query's result generation; repeated END preserves its result. Old
published scopes retain old generations independently of public Query lifetime.
Released active owners are pruned at the next query boundary, and reset drains
before clearing all membership. No strong device Query registry delays release.

Each actual render pass allocates a separate, zeroed CPU-readable eight-byte
contribution. Counting is enabled once at pass creation and stays enabled for
all user draws in that encoder: Metal permits setting a particular visibility
offset only once per pass. Clear/copy/helper work closes the user encoder and
does not contribute.
At successful completion the retained contribution is added once to every
member of that pass's immutable scope; callbacks precede the queue completion
waterline. Each contribution's command-buffer status must be checked, including
mid-chunk command buffers. Reusing one slot across encoders must never be assumed
to accumulate. Saturating 64-bit totals are clamped to DWORD on public reads.

`GetData` requires the ordinary/initializer END fence, current reset epoch, no
failure and zero outstanding contributions. Zero outstanding contributions
before encoding is not itself completion. Pending and failed reads preserve
caller bytes. Allocation, encoder and command-buffer failure cannot report a
successful zero. Even the synchronous query-policy override does not bypass
the candidate's actual result fence.

The initial provider intentionally preserves membership boundaries: Arena
admission returns a pre-effect compatibility-required status; ordinary/direct
ChunkSlots carry the scope. Scoped sources cannot join cross-source encode
sessions, frame-graph reorder/DCE or parallel passes. A scope change closes an
active render pass. Queried FFP draws use portable FFP because tile-FFP alpha
rejection occurs after rasterization. Production queries take precedence over
the diagnostic visibility scout's single Metal visibility attachment. EVENT
END coalescing policy is unchanged; occlusion membership changes are eager
boundaries and must not be advertised as a batching optimization.

Early-prefix publication is suppressed whenever this provider is enabled,
including before the first BEGIN, so an unscoped prefix cannot park waiting
for a scope-incompatible successor.

While the provider is enabled, pass stores conservatively preserve color,
depth and stencil across every source boundary, including unqueried
predecessors. Neither source-local DontCare proof nor deferred Unknown store
selection is admitted. MSAA color attachments use StoreAndMultisampleResolve
so the resolved image and the multisample contents both survive a later Load.
Ordinary Load/Clear selection is unchanged. Query cuts are not live-out death;
re-admitting store elision requires a separate cross-boundary proof.

Evidence owners: `QueryOcclusionResults.tla`, the shared predicates in
`include/dxmt9/query_occlusion.hpp`, core lifecycle tests, and the production
Metal occlusion oracle. Model safety does not prove Metal sample bytes or wild
performance; remaining gates belong to the D3D9 and verification gap rows.

---

## 5. D3DQUERYTYPE_TIMESTAMP

GPU timestamps report the GPU clock at a point in the command stream.

When Metal counter sampling is available, `Issue(D3DISSUE_END)` appends a
timestamp sample record carrying the opaque counter sample buffer handle and
reserved sample index. Queue replay samples the active command encoder and the
frontend records `query.issuedSeqId`.

`GetData(&timestamp, sizeof(UINT64), flags)` returns `S_FALSE` until resolved.
After completion, the backend resolves the counter range when needed and returns
a nanosecond timestamp.

`D3DQUERYTYPE_TIMESTAMPFREQ` returns `1000000000`. `D3DQUERYTYPE_TIMESTAMPDISJOINT`
returns `Disjoint = FALSE` with the same frequency when the timestamp path is
enabled. If counter sampling is unavailable, timestamp query creation fails with
`D3DERR_NOTAVAILABLE`.

---

## 6. Object Lifecycle

```mermaid
stateDiagram-v2
    [*] --> Created : CreateQuery(type)
    Created --> Issued : Issue(D3DISSUE_END)
    Issued --> Pending : chunk committed to GPU
    Pending --> Resolved : completedSeqId >= issuedSeqId
    Resolved --> Created : Issue(D3DISSUE_BEGIN or END) resets query
    Created --> [*] : Release()
    Issued --> [*] : Release() while pending; backend storage retained by chunk
```

The COM object may be released while a backend record is in flight. Command chunk
retention keeps backend handles and result storage alive until GPU completion,
without depending on public COM references.

## 7. Published-prefix asynchronous completion

`DXMT9_ASYNC_QUERY_FENCES` controls the implementation of R-CORE-11.5(a).
It defaults ON; explicit `0` (or an empty value) selects the synchronous
diagnostic/reference path. This is a completion-policy change, not a new
Metal occlusion/timestamp result provider.
By default, END first settles any compatibility draw batch, then publishes the ordinary
queue writer and obtains `{queue incarnation, published sequence}` under the
queue mutex. With no writer it captures the existing published prefix, including
zero for a genuinely empty queue. It does not invent a Query GPU command or
use the core operation counter as a queue sequence. Active Arena/direct build
transactions reject the operation rather than returning a deferred receipt.

The initializer's separate upload stream is also part of the receipt. END
flushes pending initializer uploads without waiting and captures its submitted
shared-event value under the queue mutex. Poll requires both the ordinary
completion waterline and this initializer event value. A bounded FIFO of 64
retained upload command buffers additionally proves successful upload completion;
an error, missing event, exhausted counter or full tracker fails closed rather
than dropping an unobserved upload. Ordinary queue GPU errors also invalidate
query success independently of the resource-reclamation waterline. A failed initializer
flush yields no receipt and fail-stops; absence of ordinary commands must not
hide pending uploads. The native exhaustive product table covers both axes;
`QueryPublicationFence.tla` abstracts the initializer/Metal event axis out.
`QueryInitializerFence.tla` owns the independent two-frontier product, including
signal-before-success and bounded tracker capacity. Its expected failures
remove the upload dependency, trust the event without successful completion,
and remove capacity admission. This is an abstract driver-status model, not a
proof of actual Metal bytes or driver recovery after a device fault.

The fixed `query::UploadTracker` owns retained entries, contiguous successful
completion and terminal failure. Production supplies Metal status reads to the
same reaper that native fault tests drive with deterministic statuses. Admission
fails before allocation when capacity/event/ordinal constraints cannot be met;
failed entries and their unobserved suffix are never evicted for new work.
The initializer-only GPU oracle parks an actual command queue behind a separate
event, confirms pending query polls, releases the gate, and reads every byte
only after completion. It repeats the upload while preserving the old receipt.

The core query retains this value with its issue ordinal and device-reset
epoch. There is no callback retaining or mutating the public query object:
polling reads the queue-owned completion waterline, so destruction/reissue
cannot redirect a late callback. Reset invalidates earlier issue epochs.

FLUSH on a pending receipt raises a sticky queue submission target and wakes
the encode coordinator. It does not enter `waitForSequence`, nor does it call
`completeUpTo`. The request participates in the encoder's existing ordered
progress decisions and remains active after the producer returns until the
requested prefix has actually been submitted. Repeated polls of an already
requested/submitted prefix do not repeat encode/finish CV notifications.
Readback,
reset and teardown continue to use their blocking contracts. An already
complete query does not request submission of a newer suffix.

The shared pure predicates in `include/dxmt9/query_fence.hpp` distinguish
invalid/failed/pending/complete. `submissionWorkTarget` retires submission
pressure only at an actual submitted prefix (or completed prefix), while
`classify` still requires successful completion. A submitted but pending query
must not force unrelated later sources to split. The request value remains
sticky; no completion or reclamation cursor advances with it.
Formal/native evidence must cover delayed completion,
queue-incarnation mismatch, old/new issue receipts, empty prefix, terminal
state, reset, and persistent wakeup. END publication may change batching;
performance and command-buffer locality are explicit promotion gates.

`DXMT9_QUERY_OBSERVER` is separate, bounded, default-off evidence. It reports
query kind, identity/issue, FLUSH, result, core ordinal, published fence and
queue waterlines. Legacy observations must label the absence of an authoritative
query fence; matching aggregate counters are not identity binding. Observer-off
must not construct reports, read clocks, or acquire diagnostic locks.
Elapsed poll scopes cover core `Device::getQueryData`, not PE recorder flush
or bridge replay drain. Issue events report core/replay disposition; a recorded
PE Issue may already have returned before replay encounters a failure.
Logged queue waterlines are sampled after the call;
they may advance after an `S_FALSE` decision and are not its linearization
snapshot. Event quotas reserve 128 Issue rows independently from up to 384
poll rows; omitted rows are counted. These bounded diagnostics do not claim a
complete per-query lifecycle trace for a long game run.

Its separate recent-evidence join keeps up to 128 END summaries keyed by
queue incarnation, query identity, issue ordinal and epoch. A recent-128
sequence/marker ring joins publication, actual submission and successful
completion, including stages observed before END returns. Shared sequence
evidence applies to every matching query. `DXMT9_QUERY_OBSERVER_SKIP_ENDS`
selects a later window by skipping a fixed number of END observations; skipped,
dropped and evicted counts remain explicit. Missing stages are not inferred
from S_OK. The END timestamp is core END-return observation, not API entry;
publication may precede it. Arena publication and independent initializer
stage timestamps are not covered by this observer.

### Replay-drain and Ready-residence attribution

The PE `Query::GetData` flushes its child recorder before crossing the bridge.
The Unix `dxmt9c_query_get_data` then drains the replay FIFO before entering
the core provider. `ReplayOffloadQueue::waitDrained` waits for both an empty
deque and no in-flight replay item, or terminal stop. This is a device-wide
CPU replay condition, not the query's GPU completion predicate. Later raw
chunks and records after END within its raw may be covered by that drain.

Cold diagnostics may associate core END with the currently executing raw
ordinal and snapshot queued ordinals around the unchanged drain. The END
association must be copied on the owning core/replay thread and qualified by
immutable query identity, issue ordinal and reset epoch. Before draining,
the bridge must not read mutable Query issue/fence state; it may look up
copied diagnostic evidence using only immutable object identity. Missing,
evicted, failed, reissued and inline/non-offloaded associations remain explicit.
A post-drain END association is retrospective evidence, not authority for a
prefix wait before that END has replayed.

Publication-time Ready/Encoding/frontier snapshots and preceding ordinary
encode intervals may refine Ready-residence attribution. Queue-qualified
sequence identity and exact interval intersection are required; a sampled
completion gap is not proof that GPU execution blocks the encoder. Missing
multi-source or evicted intervals must not be synthesized. These observations
must not change admission, queue predicates, wakeups, HRESULTs or completion.

A future narrower drain requires admission-time END identity, successful
FIFO replay-completion authority, same-query concurrency ordering, reset and
terminal handling, and resource lifetime coverage. A raw-prefix receipt alone
cannot omit records after END inside the same raw chunk. No such optimization
is authorized by the diagnostic mapping.
It must also separate polling from mutable core ownership: current
`Device::getQueryData` reads device/query state and may resolve the public
Query value, while the COM adapter adds no independent serialization.
Reaching an END ordinal alone is not permission to run that provider call
concurrently with later replay. An immutable polling receipt or an explicitly
proved synchronization boundary is required.

`DXMT9_QUERY_SPLIT_OBSERVER` observes ordinary source publications by reason
and completion-bearing tail command buffers handed to `QueueLifecycleController`.
Its `command_buffer` category excludes mid-chunk sub-CB commits and initializer
CBs. Compare it with the separate `sub_command_buffers` commit counter and
`command_buffers` creation counter; none alone is a complete causal split
attribution. Final group totals and the first 128 identity events have distinct
populations. The observer does not turn the candidate on.

Completion fencing alone does not prove query data. Section 4's separately
gated occlusion provider supplies real visibility values; GPU timestamp data
remains a separate gap.
