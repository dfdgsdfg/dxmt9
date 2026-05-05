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
zero-compatible data or `D3DERR_NOTAVAILABLE`, matching the Wine-derived oracle
for the type.

---

## 2. Public Data Size and Bytes

Public data size is part of `R-CORE-8.3`. `GetDataSize()` must return the
Windows-compatible public byte size for the query type, including:

| Query type | Required public size |
|---|---:|
| `D3DQUERYTYPE_EVENT` | `0` |
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
- `D3DERR_INVALIDCALL` is used for invalid parameters where the Wine-derived
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

---

## 5. Lifetime

Query COM lifetime is independent from backend result lifetime. Releasing a query
while its END record is in flight must not free backend result storage or opaque
handles until the owning chunk has completed.

A query object may be reused after it resolves. Reissuing a query before its
previous result resolves follows D3D9 undefined behavior and is not required to
provide additional safety beyond preserving process integrity.
