---
type: "Spec Requirements"
title: "Query Requirements"
description: "D3D9 / Queries requirements and compatibility contracts."
tags: [specs, d3d9, queries, requirements]
---

# Query Requirements

D3D9 queries expose asynchronous GPU progress and query data through
`IDirect3DQuery9`. dxmt9 implements them on top of chunk sequence IDs and backend
query records, while preserving the Windows-visible COM and `HRESULT` contracts.

Traceability: R-CORE-8.1-R-CORE-8.3, R-CORE-11.4-R-CORE-11.5.

---

## 1. Supported Query Types

### 1.1 EVENT Query (`R-CORE-8.1`)

`D3DQUERYTYPE_EVENT` must be supported. `Issue(D3DISSUE_END)` must make the
query resolve only after the GPU has completed all commands ordered before the
END marker record.

### 1.2 OCCLUSION Query (`R-CORE-8.2`)

`D3DQUERYTYPE_OCCLUSION` must be supported when the Metal device supports
visibility result buffers. The returned sample count may be an approximation and
may be clamped to boolean visibility where the backend cannot provide an exact
count.

### 1.3 Validation and Support Probes (`R-CORE-8.3`)

Query validation must follow Windows D3D9-visible behavior. Unsupported or
invalid query types must return `D3DERR_NOTAVAILABLE`, and
`CreateQuery(type, NULL)` must act as a support probe without returning an object.

Optional timestamp query types may be exposed only when the backend can provide
deterministic public data:

- `D3DQUERYTYPE_TIMESTAMP`
- `D3DQUERYTYPE_TIMESTAMPDISJOINT`
- `D3DQUERYTYPE_TIMESTAMPFREQ`

Driver-statistics query types that Metal cannot expose may return deterministic
zero-compatible data or `D3DERR_NOTAVAILABLE`, matching the Wine behavioural oracle
for the type.

---

## 2. Public Data Size and Bytes

Public data size is part of `R-CORE-8.3`. `GetDataSize()` must return the
Windows-compatible public byte size for the query type, including:

| Query type | Required public size |
|---|---:|
| `D3DQUERYTYPE_EVENT` | `sizeof(BOOL)` |
| `D3DQUERYTYPE_OCCLUSION` | `sizeof(DWORD)` |
| `D3DQUERYTYPE_TIMESTAMP` | `sizeof(UINT64)` |
| `D3DQUERYTYPE_TIMESTAMPDISJOINT` | `sizeof(BOOL)` |
| `D3DQUERYTYPE_TIMESTAMPFREQ` | `sizeof(UINT64)` |

`GetData(pData, dwSize, flags)` must not write beyond `dwSize`, must not leak
uninitialized memory, and must write deterministic Windows-compatible bytes for
pre-issue, unsupported-backend, and zero-stat cases.

---

## 3. Issue and Resolution

Command recording follows `R-CORE-11.4`. Query commands recorded into a
`CommandChunk` must be POD command records, marker records, or replay operations
with opaque backend handles. They must not contain D3D9 COM pointers,
Objective-C object pointers, unix-side C++ object pointers, or executable
payloads.

Each query END must record the chunk sequence ID that orders the END marker.
A query is resolved only when `completedSeqId >= issuedSeqId`.

Multiple queries may share an `issuedSeqId` when their END records are in the
same chunk. Resolution remains chunk-granular: if the chunk completed, every END
record ordered within it has completed.

---

## 4. `GetData`, `HRESULT`, and Flush

`R-CORE-8.1` requires `GetData(NULL, 0, flags)` for an EVENT query to return
`S_OK` only after the issuing chunk is GPU-complete. Before completion it must
return `S_FALSE`.

`R-CORE-8.3` requires `GetData()` to preserve Windows-compatible `HRESULT`
behavior:

- `S_OK` means the result bytes, if any, are valid for the requested public range.
- `S_FALSE` means the query is not yet complete.
- `D3DERR_INVALIDCALL` is used for invalid parameters where the Wine-oracle
  oracle requires it.
- `D3DERR_NOTAVAILABLE` is used when a query type or backend data path is not
  available.

Flush behavior follows `R-CORE-11.5`. If `flags & D3DGETDATA_FLUSH` is set while
a query is unresolved, the command recorder must commit any current chunk needed
to submit the query END marker to the backend. The busy-wait pattern
`while (GetData(..., D3DGETDATA_FLUSH) == S_FALSE) {}` must not deadlock because
the END marker remained in an uncommitted chunk.

Flush commits are ordering operations only. They must not make the query appear
resolved before GPU completion advances `completedSeqId` past `issuedSeqId`.

### 4.1 Published-prefix completion (`R-CORE-11.5(a)`)

The asynchronous query-fence path must bind END to an actual
published queue prefix, qualified by a queue incarnation. A core API ordinal
or a predicted future writer sequence is not a completion identity. An empty
prefix is valid only with a live queue incarnation.
Separate initializer uploads preceding END must also be submitted and complete
before success; an empty ordinary prefix must not bypass that dependency.

`GetData(FLUSH)` may request submission progress for that prefix, but must not
wait for GPU completion or advance the core completion cursor. A pending query
returns `S_FALSE`; actual completion permits `S_OK`. A terminal queue, stale
device epoch, or invalid receipt must not report successful completion. Reissue
replaces the old receipt, and later unrelated submissions must not extend it.
Progress requests must survive return from the poll and wake a parked encoder.
Their submission pressure retires at actual submission; the query's completion
obligation persists until both completion frontiers pass or the queue terminates.

This path may publish the current writer at END, retaining existing
bounded admission backpressure at Issue. Blocking readback/reset/teardown
flushes remain separate. Default promotion requires native/model binding,
Wine behavior, visible wild correctness and locality/non-regression evidence.
Query result-data implementation is not proved by a fence.

### 4.2 Initializer completion evidence (`R-CORE-11.5(b)`)

The initializer completion frontier must be the minimum of the observed event
signal and the contiguous prefix of successfully completed upload command
buffers. Signaling alone, completion of a later upload, or ordinary queue
completion must not authorize an earlier pending or failed upload.

The retained upload tracker must reject capacity exhaustion, stale publication
ordinals, missing events, counter exhaustion and allocation failure before
claiming a new receipt. Failure is terminal for query success; tracker entries
must not be overwritten to admit newer work. Fault tests must exercise every
tracker position and reuse across wrap. A hardware oracle must independently
gate an initializer-only stream, observe pending polls without Present/readback,
then verify all uploaded bytes after successful query completion.

### 4.3 Submission locality (`R-CORE-11.5(c)`)

A pending query's submission obligation and completion obligation are distinct.
An actual submitted prefix may discharge the former, never the latter. An
already submitted query prefix should not force publication of unrelated later
sources while waiting for GPU completion. Blocking resource/readback/teardown
wait targets retain their existing semantics.

Deferring END publication or coalescing ENDs across a live writer requires a
separate admitted marker-to-publication identity; a predicted future queue
sequence remains forbidden. Such a candidate must preserve ordered-control
cuts and may not claim the eager-publication evidence as proof of the new path.

---

## 5. Lifetime

Query COM lifetime is independent from backend result lifetime. Releasing a query
while its END record is in flight must not free backend result storage or opaque
handles until the owning chunk has completed.

A query object may be reused after it resolves. Reissuing a query before its
previous result resolves follows D3D9 undefined behavior and is not required to
provide additional safety beyond preserving process integrity.
