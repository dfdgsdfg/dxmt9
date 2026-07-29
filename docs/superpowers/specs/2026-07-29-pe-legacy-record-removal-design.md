# PE Legacy Record Removal — Design

**Date:** 2026-07-29
**Status:** design, approved for planning
**Scope:** `src/d3d9/` PE recorder only. No unix-side behavior change.

## 1. Goal, and what "done" means

Reduce the number of representations the PE recorder uses to turn one D3D9
call sequence into V2 chunk bytes, from four to two.

**Success is structural, not performance.** Stated honestly:

- The `build` and `encode` phase figures in the census are per-call means over
  **all** `2,720` appends, not over the sparse family alone — its own table
  back-solves to that (`1,464 ns → 3.98 ms/present` requires `×2,719`). So the
  gross prize is `(158 + 1,174) ns × 2,720 = 3.62 ms` of a `53.2 ms` frame,
  **`6.8%`**. Section encoding itself remains, so a fraction of that survives.
- **The sparse family's own share of build+encode is unmeasured.** No per-type
  phase split exists. Do not attribute the all-append mean to the `1,704` sparse
  appends; that arithmetic prorates const-record and clear-record cost onto
  draws and measures nothing. An earlier draft of this document did exactly that
  and reported a fictitious `4.3%`.
- `6.8%` is a GT2 figure, and GT2 is the workload where dxmt9's PE share is
  largest. Other applications see less.
- Whatever the number, **it is not claimed as an FPS win.** The same
  discipline that
  [state-churn-encode-append-decomposition.01](../../perfomance/state-churn-encode/state-churn-encode-append-decomposition.01.md)
  applied to its own `0.93%` result applies here: below this workload's FPS
  resolution unless a paired A/B says otherwise.

If FPS moves, that is a bonus. If it does not, the work still succeeded: the
deliverable is the retirement of a duplicate representation of 21 record types
and of the surface where defects like the five-struct zeroing could hide
unnoticed.

## 2. Current shape and target shape

The unix side is already data-oriented: V2 sparse sections apply directly to
`DeviceState` through `SparseReplaySinkV2`'s per-section handlers, with no fat
packet reconstruction. Only the PE side changes.

```
now:    peState_ ──► D9CDrawPrimitivePacket ──► legacy record (scratch) ──► SparseStateV2Input ──► V2 sections
                     4.9 KB fat struct          memcpy in                   loadLegacy memcpy out   (spans only)

target: peState_ ────────────────────────────────────────────────────────► SparseStateV2Input ──► V2 sections
```

**The V2 encoders do not change.** `appendSparseRecordV2`,
`appendSetConstantsV2`, `appendClearV2`, and the rest keep their signatures.
`SparseStateV2Input` (`d3d9_pe_chunk_v2_builder.hpp:161`) is already a pure
span-only description, so it is the seam. Only who fills it changes.

`peState_` does not change either. It already carries per-category dirty
tracking (`FixedStateTable` — a slot-indexed value array plus an occupancy
bitmap — with `pendingRenderStates`, `pendingTextureMask`, `pendingStreamMask`,
and so on). The producer walks those bitmaps, writes compact pairs into a
reusable per-device scratch arena, and points the spans at it. No per-frame
heap allocation.

### Cost distribution that sets the migration order

GT2, per present, from the zero-timing per-type census:

| family | types | appends | record size |
|---|---:|---:|---:|
| draw / drawidx / drawUP / drawidxUP / applystate | 5 | `1,704` | `4,888–4,924 B` |
| vs/ps constants | 6 | `1,008` | `50–938 B` |
| clear, present, stretchrect, colorfill, updatetexture, updatesurface, readback, … | ~10 | `9` | `32 B`+ |

Only the first row does the fat-struct round trip: `appendLegacySparseRecord`
`loadLegacy`s `4.9 KB` back out of scratch into stack aggregates. That is
`~16.7 MB/present` of pure memcpy against `~0.5 MB` for everything else.

## 3. Ownership boundary — what the producer actually depends on

This is the load-bearing part of the design, and the naive version of it is
wrong. The current record is **not** a function of `{shadow, bindings}` alone.
Four distinct input classes feed it.

**(a) COM pointer members.** `buildDrawPrimitivePacket`
(`d3d9_pe_device.cpp:3867-4103`, 237 lines, 6 call sites at `:9329`, `:9389`,
`:9527`, `:9627`, `:10000`, `:10139`) reads `textures_`, `streamSrc_`,
`streamOff_`, `streamStr_`, `vs_`, `ps_`, `vdecl_`, `dsSurface_`, `fvf_`, and
`currentRtWireHandles()` / `currentRtExplicitMask()`, translating each through
`toWireHandle(raw*(…))`.

**(b) The constant shadow, folded after the fact.** Constants never pass through
`buildDrawPrimitivePacket`. The call site calls `foldPendingConstsIntoDrawPacket`
*afterwards* (`:9334-9345`), draining `peConsts_` into `constDeltaSections` plus
a trailing payload from `constDeltaPayloadScratch_`. `SparseStateV2Input`'s
`vsFloatConstants … psBoolConstants` fields are fed from there, not from
`peState_`.

**(c) Draw-call arguments.** `upIndexData` / `upVertexData` come from the UP
draw's parameters (`:9486-9530`), not from any shadow.

**(d) Destination-chunk state.** This is the one that breaks purity outright.
`populatePendingChunkDrawStreamDependencies` (`:9462-9477`) runs **inside the
writer lambda, after any CapacityPre flush has resealed the chunk**, and
re-emits stream bindings for streams the *actual destination chunk* has not yet
retained:

```cpp
if (pendingChunkReferencesBuffer(streamSources[slot].buffer))
    retainedStreamMask |= 1u << slot;
```

Indexed draws carry the same shape for the index buffer: `ibValid` derives from
`peState_.pendingIb || !submittedIndexBufferKnown_ ||
submittedIndexBufferWireValue_ != ibWireValue` (`:9404-9412`), with the tracking
updated only on success (`:9443-9449`). The emitted section is therefore a
function of chunk-boundary history, not of the shadow. Its comment states the
contract explicitly: "The serialized packet remains the sole source of retention
semantics (R-CORE-11.17)."

**Resolution: keep the transform pure by making every one of these an explicit
POD input.** COM-to-wire translation stays on the device; chunk context is
passed in rather than reached for.

```cpp
// New TU. No windows.h / d3d9.h.
struct PeBindingView {   // (a) — all POD; PeWireObjectRef is {identity, void*}
  std::array<PeWireObjectRef,  D9C_DRAW_PACKET_MAX_TEXTURES>       textures;
  std::array<PeStreamBinding,  D9C_DRAW_PACKET_MAX_STREAMS>        streams;
  PeWireObjectRef vs, ps, vdecl, indexBuffer, depthStencil;
  std::array<PeWireObjectRef, D9C_DRAW_PACKET_MAX_RENDER_TARGETS>  renderTargets;
  std::uint32_t rtExplicitMask;
  std::uint32_t fvf;
};

struct PeChunkContext {  // (d) — the destination chunk's history, as data
  std::uint32_t retainedStreamMask;      // from CommandChunkV2Builder::referencesObject
  bool          indexBufferKnown;        // submittedIndexBufferKnown_
  std::uint64_t submittedIndexBufferWire; // submittedIndexBufferWireValue_
};

struct PeDrawPayloads { std::span<const std::byte> upIndex, upVertex; };  // (c)

bool buildSparseStateV2(const PeStateShadow&   shadow,
                        const PeConstShadow&   constants,   // (b)
                        const PeBindingView&   bindings,    // (a)
                        const PeChunkContext&  chunk,       // (d)
                        const PeDrawPayloads&  payloads,    // (c)
                        PeSparseScratch&       scratch,
                        SparseStateV2Input&    out) noexcept;
```

`PeStreamBinding` is `{PeWireObjectRef buffer; std::uint32_t offset, stride;}`.
`PeSparseScratch` is a device-owned, reused set of fixed-capacity arrays (one
per `SparseStateV2Input` category, each sized to its existing
`D9C_DRAW_PACKET_MAX_*` cap) into which the producer writes compact entries and
which the output spans point at. It is an output parameter rather than a local
so that no per-draw allocation occurs and the spans stay alive exactly until
`appendSparseRecordV2` has consumed them.

With `chunk` as an explicit parameter the function is a pure transform again —
and, importantly, the differential test can now *drive* chunk context instead of
being unable to reproduce it. That is the whole reason to surface it rather than
let the producer call back into the builder.

Constraints `appendSparseRecordV2` imposes on any producer, verified against
`d3d9_pe_chunk_v2_draw.cpp`: strictly ascending slot order per section
(`orderedSlot`, `:190`), spans alive across the call, `valid ≤ 1`. An ascending
bitmask walk satisfies all three naturally.

**Not pure, and staying that way:** `buildDrawPrimitivePacket` is declared
`const` but mutates `peDrawPacketDecimatedStats_` (`mutable`, see the comment at
`:3385-3386`; guard at `:3876-3890`) for `DXMT9_PE_STATS_DECIMATION`. The moved
function keeps that instrumentation by taking the stats block as a `mutable`
reference parameter. It does **not** clear pending bits — callers do that on
success — so the shadow itself is read-only to the producer.

Two facts from the survey make the extraction cheaper than expected:

- `d3d9_pe_chunk_v2_builder.cpp`, `_draw.cpp`, and `_nondraw.cpp` are
  **already compiled into native test executables**
  (`tests/native/bridge/meson.build:134-136`,
  `tests/native/backend/meson.build:469-471`). Their Windows-freedom holds
  transitively: `builder.hpp → retainer.hpp → dxmt9/device_c.h` (pure C) and
  `device_c_chunk_v2_schema.hpp`, and retention is C-ABI
  `dxmt9c_*_addref/release`, not COM.
- `d3d9_pe_state_shadow.hpp`'s Windows dependency is shallow: `DWORD` (14
  sites), `D3DTS_*` (12), `D3DVERTEXTEXTURESAMPLER*` (3), plus two enum types.
  All map to `std::uint32_t` and mirrored constants, for which
  `include/dxmt9/core_constants.hpp:665` already sets precedent by handling
  `D3DTS_WORLDMATRIX`.

## 4. Phase 1 — mechanical extraction, zero behavior change

| Step | Change | Proof |
|---|---|---|
| 1a | Add `d3d9_pe_state_shadow.hpp`'s TU (and any new producer TU) to the native test targets. The three `d3d9_pe_chunk_v2_*.cpp` files are **already there** — this step is smaller than first assumed | Native targets compile |
| 1b | Remove Windows types from `d3d9_pe_state_shadow.hpp` (`DWORD` → `std::uint32_t`, `D3DTS_*` / `D3DSAMP_*` → mirrored constants) | Existing meson suite (667 registered tests) |
| 1c | Introduce `PeBindingView` / `PeChunkContext` / `PeDrawPayloads`; rehost `buildDrawPrimitivePacket` in the new TU against them; populate them at the 6 call sites | **See below — the existing suite does not cover this** |

**Step 1c has no native coverage, and the design does not pretend otherwise.**
No meson test compiles or executes `d3d9_pe_device.cpp`: it lives in
`dxmt9_pe_core_srcs`, which `src/d3d9/meson.build:65` gates on
`host_machine.system() == 'windows'`. `pe_full_snapshot_equivalence_spec.cpp`
cannot help either — its own header states it **mirrors**
`buildDrawPrimitivePacket` at test scope precisely because "Native bridge tests
cannot instantiate src/d3d9/d3d9_pe_device.cpp" (`:44-53`). Citing it as a gate
for a change to that function would be the mirror trap this design warns about
in §6.

So 1c is gated by two things instead, both stated as real cost:

1. **A mechanically reviewable diff.** The moved body must differ only by
   member-to-parameter substitution (`textures_[i]` → `bindings.textures[i]`,
   and so on). Anything else in the diff is a defect by construction. The UP
   call site's save/restore of `fvf_` / `vdecl_` / `vs_` and three pending bits
   around the call (`:9511-9526`) must be preserved explicitly; it is the one
   place where the substitution is not purely textual.
2. **Wine runtime evidence.** GT1, GT2, GT3, and SFIV runs per
   `agents/rules/test_wild.rules.md`, compared against the `v0.0.3` visual
   anchor, with `gpu_command_buffer_errors = 0`.

After Phase 1 the producer still builds the fat packet and still goes through
the legacy round trip — but it is a pure transform that runs natively. That is
the precondition for Phase 2's differential test.

Side effect: 237 lines plus the state shadow leave `d3d9_pe_device.cpp`
(currently 14,971 lines).

## 5. Phase 2 — rewrite, one family at a time

**Step 2a comes first.** The differential harness is written *before* the new
producer, and is confirmed failing against a stub. Attaching a passing test
afterwards is how a harness ends up proving nothing — the mini-replay defect
closed earlier this cycle was exactly that shape.

Order runs simplest to hottest. Each step deletes its legacy record types and
their `case` in `appendLegacyCommandRecordAsV2`.

| Step | Family | Types | GT2 appends/present | Character |
|---|---|---:|---:|---|
| 2b | Non-draw (clear, present, stretchrect, colorfill, updatetexture, updatesurface, readback, …) | ~10 | `9` | Call `appendClearV2` and friends directly at the call site. No shadow involvement; most mechanical — see the two caveats below |
| 2c | Constants (vs/ps × float/int/bool) | `6` | `1,008` | Call `appendSetConstantsV2(type, start, count, bytes)` directly |
| 2d | `applystate` | `1` | `10` | **First appearance of `buildSparseStateV2`** — sparse sections with no draw |
| 2e | `draw` + `drawidx` | `2` | `1,694` | 2d's producer plus the draw header. Most of the cost |
| 2f | `drawUP` + `drawidxUP` | `2` | `~0` | Adds UP payload spans |

**Two caveats on 2b**, which is otherwise the mechanical step it looks like.
Retention itself is fine — the non-draw records carry surfaces and textures, but
`appendClearV2` and its siblings already retain through `appendHandle`, which
does not change. What does change is (i) barrier sites keep prepending a legacy
`APPLY_STATE` via `chunkBarrierFlush()` until 2d lands, so 2b does not remove
the legacy path from those call sites, only their own record; and (ii) object-ref
acquisition moves from a wire-handle cache lookup to wrapper-side refs, which
changes the failure path and the `noteWireIdentityGetterCall` counter. Neither
blocks the step, but neither is a no-op either.

`DXMT9_PE_DRAW_FULL_SNAPSHOT` is **ported** in 2d, not retired. In the sparse
representation it is the same shadow-versus-pending switch: emit every section
from the full shadow instead of only the pending set. Its delta/snapshot
equivalence contract is a real requirement, and porting it lets
`pe_full_snapshot_equivalence_spec.cpp` be rewritten against the real code
rather than a test-scope mirror.

## 6. Equivalence proof — byte identity, and the three things it does not cover

V2 chunk bytes are the PE recorder's only *wire* output, so byte identity for
the same call sequence is a strong oracle: nothing downstream — unix side,
backend, GPU — can observe a difference in what a chunk *contains*. It is not,
however, a complete oracle, and an earlier draft of this document claimed it
was. Three things sit outside it and need their own gates:

1. **Where chunks are cut.** `appendCommandRecordDirect`'s capacity precheck
   compares the **legacy record size** against V2 payload bytes:
   `payloadBytesBefore + bytes > maxBytes`, where `bytes` is the ~4.9 KB legacy
   record and `payloadBytes()` is the V2 arena (`d3d9_pe_device.cpp:9182-9184`).
   Delete the legacy format and that input is gone, so chunk seal cadence
   changes — and a per-record byte diff cannot see it. **Decision: preserve
   current cadence.** The precheck keeps consuming a size estimate with the same
   scale as today's legacy record, computed from the sparse description, so
   `DXMT9_PE_CHUNK_MAX_BYTES` retains its meaning and chunk boundaries land
   where they land now. Whether the cadence *should* change is a separate
   question with its own downstream reach (offload queue depth, present pacing)
   and is explicitly out of scope here.
2. **Retention and tracking side effects.** Two producers can emit identical
   bytes while leaving different state behind: objects retained into the chunk,
   pending bits cleared, `submittedIndexBuffer*` updated. The differential
   therefore asserts on more than bytes — after each record it compares the
   builder's retained-object set, `recordCount`/`handleCount`/`payloadBytes`,
   the return value, and the post-call shadow and tracking state.
3. **Failure paths.** The legacy path can fail through
   `lookupCachedWireObjectRef` misses (`d3d9_pe_chunk_v2_draw.cpp:468-476`)
   that a direct producer holding wrapper refs never reaches. The corpus
   includes over-cap and unresolvable-handle cases and asserts that both paths
   return `false` together — not that they fail for the same reason, which they
   legitimately may not.

With those three added, the differential is a sound gate. Without them it would
pass while chunk boundaries moved underneath it.

```
corpus ──► buildDrawPrimitivePacket ──► appendLegacySparseRecord ──► V2 bytes ──┐
                                                                                ├── memcmp
corpus ──► buildSparseStateV2 ────────► appendSparseRecordV2 ─────► V2 bytes ──┘
```

**Why this avoids the mirror trap:** both sides are the real functions from
`src/`. The test reimplements nothing. Phase 1 exists precisely to make that
possible — the current `pe_full_snapshot_equivalence_spec.cpp` states in its own
header comment that it mirrors `buildDrawPrimitivePacket` at test scope because
native tests cannot instantiate `d3d9_pe_device.cpp`.

The corpus has three parts:

- **Deterministic sequences** aimed at what the 6 call sites can produce: empty
  delta, single category dirty, every category dirty, each array at its cap, and
  each array over its cap (which must return `false` on both paths).
- **Chunk-context sequences** driving `PeChunkContext` independently of the
  shadow: a stream dirty but already retained, retained but not dirty, neither,
  and the same three for the index buffer. These are the §3(d) cases, and they
  exist only because chunk context is an explicit parameter — a producer that
  called back into the builder could not be driven this way.
- **Fixed-seed randomized sequences** interleaving state mutations with draws
  and barriers. The seed is pinned, so CI remains deterministic.

One case the corpus must contain for a reason that is easy to miss: an
all-slots-dirty delta. The `FULL_SNAPSHOT` draw flag is derived inside the shim
being deleted, from all-ones texture and stream masks
(`d3d9_pe_chunk_v2_draw.cpp:867-873`), which also means it fires in delta mode
whenever both masks happen to be full. The new producer must own that heuristic
explicitly, and only an all-dirty case exercises it.

**What this harness does not prove:** state combinations absent from the corpus.
No exhaustiveness is claimed. Phase 2 therefore also takes the repository's
normal promotion evidence per `agents/rules/test_wild.rules.md` — GT1/GT2/GT3
and SFIV runs, screenshot comparison against the `v0.0.3` visual anchor, and
`gpu_command_buffer_errors = 0`.

## 7. Deletions, in one commit after Phase 2

- The `D9CCommandRecord*` struct definitions in `include/dxmt9/device_c.h` —
  **16 typedef structs**, covering the 21 record *types* (5 sparse + 6 constant
  + ~10 non-draw). An earlier draft said "21 struct definitions"; the two counts
  are different things
- `appendLegacyCommandRecordAsV2`, `appendLegacySparseRecord`,
  `populateLegacySparseState`, `loadLegacy`, `legacyRange`
- The `build` and `encode` phases of `appendCommandRecordDirect`. `resize` and
  `flush` remain, with `flush` fed by the size estimate from §6(1) rather than a
  legacy record size
- `D9CDrawPrimitivePacket`, and with it **everything typed on it**:
  `d3d9_pe_draw_packet.hpp`'s `populateDrawPacket*` helpers,
  `packetHasNoStateDelta`, `makeRunParam`, the unix-side `applyDrawPacketState*`
  family, and the dead bridge ops `dxmt9c_device_draw_primitive_packet` /
  `dxmt9c_device_draw_primitive_chunk` (**zero PE callers**, verified)
- `D9CCommandRecordQueryIssue`'s use in `d3d9_pe_device_child_misc.cpp:220`
- `tests/native/bridge/bridge_ops_spec.cpp`, which pins
  `kBridgeOpcodeCount == 158` (`:48`) and `drawChunk == drawPacket + 1` (`:103`)
- Unix-side legacy symbols with zero external references:
  `validateCommandRecord`, `importedRecordIsDrawRunCandidate`,
  `drawPacketStateDeltaEquals`, `collectDrawPacketResourceHazards`. Each is
  re-verified immediately before deletion, not on the strength of this survey.

**Retained:** `D9CDrawPacketTextureStageState`, `D9CDrawPacketSamplerState`, and
`D9CDrawPacketTransform` are used directly by `SparseStateV2Input` and stay.

**Surgical, not wholesale:** `device_c_record_replay.cpp` cannot be deleted
outright — `replayInfoForCommandRecordType` (`:368`) is live from
`device_c_chunk_replay.cpp:1506`. The deletion is per-symbol.

An earlier draft deferred judgment on `packetHasNoStateDelta` while deleting
`D9CDrawPrimitivePacket`. That was incoherent: its signature is
`bool packetHasNoStateDelta(const D9CDrawPrimitivePacket&)`
(`device_c_record_utils.hpp:377`), so it cannot outlive the type. Its only
production caller sits inside `applyDrawPacketStateDirect`
(`device_c_chunk_replay.cpp:788`), which this list already deletes; the
remaining references are in the mirror spec.

Removing the bridge ops changes `DXMT9_WINEMETAL_CALL_ABI_HASH` and renumbers
subsequent opcodes, so both PE build directories and the unix provider must be
rebuilt together (`agents/rules/build.rules.md` lockstep rule). The renumbering
fails loudly — the ABI handshake plus `bridge_ops_spec.cpp`'s hardcoded
assertions catch it — so this is routine, but the spec edits above are part of
the change, not a follow-up.

## 8. Risks

| Risk | Mitigation |
|---|---|
| The corpus misses a state combination that only real workloads produce | Wild-run evidence (§6) is required in addition to the differential, not instead of it |
| **Phase 1c has no native test coverage** — no meson test compiles `d3d9_pe_device.cpp` | The largest real risk in this design, and it is not mitigated away. §4 states the two substitute gates (mechanically reviewable diff, Wine runtime evidence) and their limits. If 1c must be split further to stay reviewable, split it |
| Chunk seal cadence shifts once the legacy record size leaves the capacity precheck, invisibly to a byte diff | §6(1): the precheck is fed an equivalent-scale size estimate so cadence is preserved. Changing cadence is explicitly a separate question |
| The saving does not survive because section encoding remains | Stated as an expectation, not a commitment (§1). The design is justified structurally |
| Porting `DXMT9_PE_DRAW_FULL_SNAPSHOT` to the sparse producer introduces a divergence the delta path does not exercise | The differential corpus runs both modes and includes the all-slots-dirty case that triggers the flag's mask heuristic; the ported spec runs against the real producer instead of a mirror |
| An intermediate state ships to master where some families are migrated and others are not | Accepted deliberately. Each step is independently verified by the differential; a half-migrated tree is correct, just not yet minimal |

## 9. What this design does not do

- It does not change the V2 wire format, the unix-side replay path, or
  `SparseReplaySinkV2`.
- It does not restructure `peState_`.
- It does not address the other `appendRecordDirect` phases. `flush` is
  `21.9%` of append and is a separate question.
- It does not change chunk seal cadence (§6(1)), even though removing the legacy
  record size would otherwise change it as a side effect.
- It does not claim an FPS improvement.

## 10. Review history

Reviewed adversarially on 2026-07-29 against the source. Five claims in the
first draft were confirmed defective and are corrected above: the
`{shadow, bindings}` purity model (missed the constant shadow, UP payloads, and
the destination-chunk retention dependency at `d3d9_pe_device.cpp:9462-9477`);
the `4.3%` arithmetic (a category error — the census means are per-append over
all appends); the claim that byte identity is a complete oracle (chunk seal
cadence, retention side effects, failure paths sit outside it); the Phase 1c
proof (no native test executes the file being changed); and §7's
`packetHasNoStateDelta` contradiction plus an incomplete blast radius.

Claims that survived verification: the V2 encoders need no change; the V2
builder TUs are Windows-free transitively and are already in the native test
targets; the two fat-packet bridge ops have zero PE callers; and the unix side
is already section-direct with no fat-packet reconstruction.

## Related

- [attribution.04](../../perfomance/present-pacing/present-pacing-post-defselect-cpu-attribution.04.md)
  — `appendRecordDirect` at `14.5%` of GT2's critical thread, calibrated
- [append-decomposition.01](../../perfomance/state-churn-encode/state-churn-encode-append-decomposition.01.md)
  — the phase split that identified the round trip
- `agents/rules/codebase_conventions.rules.md` — pure value transforms, DOD hot paths
- `agents/rules/build.rules.md` — PE/unix ABI lockstep
