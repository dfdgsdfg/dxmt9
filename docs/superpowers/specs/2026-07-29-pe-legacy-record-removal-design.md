# PE Legacy Record Removal — Design

**Date:** 2026-07-29
**Status:** design, approved for planning
**Scope:** `src/d3d9/` PE recorder only. No unix-side behavior change.

## 1. Goal, and what "done" means

Reduce the number of representations the PE recorder uses to turn one D3D9
call sequence into V2 chunk bytes, from four to two.

**Success is structural, not performance.** Stated honestly:

- The measured cost this removes is GT2's `build 158 ns + encode 1,174 ns`
  per sparse append over `1,704` sparse appends per present — `2.27 ms` of a
  `53.2 ms` frame, **`4.3%`**. Section encoding itself remains, so realistically
  about half that survives.
- `4.3%` is a GT2 figure, and GT2 is the workload where dxmt9's PE share is
  largest. Other applications see less.
- Whether it is `4.3%` or `2%`, **it is not claimed as an FPS win.** The same
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

## 3. Ownership boundary — the producer is not yet a pure transform

This is the load-bearing part of the design.

`buildDrawPrimitivePacket` (`d3d9_pe_device.cpp:3867-4103`, 237 lines, 6 call
sites) does not read only `peState_`:

```cpp
packet.textures[stage] = toWireHandle(rawTex(textures_[stage]));
s.buffer               = toWireHandle(rawVBuf(streamSrc_[stream]));
packet.vsHandle        = toWireHandle(rawVS(vs_));
// ... currentRtWireHandles(), currentRtExplicitMask(), dsSurface_
```

`textures_`, `streamSrc_`, `vs_`, `ps_`, `dsSurface_` are **COM pointer members
of the device class**. They are the only obstacle to extracting a pure
transform.

**Resolution: COM-to-wire translation stays on the device; the producer takes a
POD view.**

```cpp
// New TU. No windows.h / d3d9.h.
struct PeBindingView {   // all POD; PeWireObjectRef is {identity, void*}
  std::array<PeWireObjectRef,  D9C_DRAW_PACKET_MAX_TEXTURES>       textures;
  std::array<PeStreamBinding,  D9C_DRAW_PACKET_MAX_STREAMS>        streams;
  PeWireObjectRef vs, ps, vdecl, indexBuffer, depthStencil;
  std::array<PeWireObjectRef, D9C_DRAW_PACKET_MAX_RENDER_TARGETS>  renderTargets;
  std::uint32_t rtExplicitMask;
  std::uint32_t fvf;
};

bool buildSparseStateV2(const PeStateShadow& shadow,
                        const PeBindingView& bindings,
                        PeSparseScratch& scratch,
                        SparseStateV2Input& out) noexcept;
```

`PeStreamBinding` is `{PeWireObjectRef buffer; std::uint32_t offset, stride;}` —
the wire-handle form of the device's `streamSrc_` / `streamOff_` / `streamStr_`
triple. `PeSparseScratch` is a device-owned, reused set of fixed-capacity arrays
(one per `SparseStateV2Input` category, each sized to its existing
`D9C_DRAW_PACKET_MAX_*` cap) into which the producer writes compact entries and
which the output spans point at. It is an output parameter rather than a local
so that no per-draw allocation occurs and the spans outlive the call, exactly
until `appendSparseRecordV2` has consumed them.

`buildSparseStateV2` is then a pure transform of `{shadow, bindings}` into
`SparseStateV2Input`. COM ownership does not move, and a native test can call
the real function. This is what `agents/rules/codebase_conventions.rules.md`
asks for: "Prefer pure value transforms … unit-testable without Wine, Metal, or
GPU timing."

Two facts from the survey make the extraction cheap:

- `d3d9_pe_chunk_v2_builder.cpp` and `d3d9_pe_chunk_v2_draw.cpp` **already do
  not include `windows.h`**. They are merely placed in the Windows-only
  `dxmt9_pe_core_srcs` meson target.
- `d3d9_pe_state_shadow.hpp`'s Windows dependency is shallow: `DWORD` (14
  sites), `D3DTS_*` (12), `D3DVERTEXTEXTURESAMPLER*` (3), plus two enum types.
  All map to `std::uint32_t` and mirrored constants, for which
  `include/dxmt9/core_constants.hpp:665` already sets precedent by handling
  `D3DTS_WORLDMATRIX`.

## 4. Phase 1 — mechanical extraction, zero behavior change

Each step is proven by the existing meson suite (667 registered tests at the
time of writing). No new test is required to catch a regression in this phase.

| Step | Change | Proof |
|---|---|---|
| 1a | Move `d3d9_pe_chunk_v2_builder.cpp` / `_draw.cpp` into a natively-buildable target. **No code change.** | Native target compiles; existing suite green |
| 1b | Remove Windows types from `d3d9_pe_state_shadow.hpp` (`DWORD` → `std::uint32_t`, `D3DTS_*` / `D3DSAMP_*` → mirrored constants) | Existing suite |
| 1c | Introduce `PeBindingView`; change `buildDrawPrimitivePacket` to `(shadow, bindings, packet)` and populate the view at the 6 call sites; move the function **verbatim** into the new TU | Existing suite + `pe_full_snapshot_equivalence_spec` |

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
| 2b | Non-draw (clear, present, stretchrect, colorfill, updatetexture, updatesurface, readback, …) | ~10 | `9` | Call `appendClearV2` and friends directly at the call site. No shadow involvement; most mechanical |
| 2c | Constants (vs/ps × float/int/bool) | `6` | `1,008` | Call `appendSetConstantsV2(type, start, count, bytes)` directly |
| 2d | `applystate` | `1` | `10` | **First appearance of `buildSparseStateV2`** — sparse sections with no draw |
| 2e | `draw` + `drawidx` | `2` | `1,694` | 2d's producer plus the draw header. Most of the cost |
| 2f | `drawUP` + `drawidxUP` | `2` | `~0` | Adds UP payload spans |

`DXMT9_PE_DRAW_FULL_SNAPSHOT` is **ported** in 2d, not retired. In the sparse
representation it is the same shadow-versus-pending switch: emit every section
from the full shadow instead of only the pending set. Its delta/snapshot
equivalence contract is a real requirement, and porting it lets
`pe_full_snapshot_equivalence_spec.cpp` be rewritten against the real code
rather than a test-scope mirror.

## 6. Equivalence proof — why byte identity is a complete oracle

V2 chunk bytes are the PE recorder's **only** output. If the bytes match for
the same call sequence, nothing downstream — unix side, backend, GPU — can
observe a difference. This is a complete oracle, not a partial one.

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

The corpus has two parts:

- **Deterministic sequences** aimed at what the 6 call sites can produce: empty
  delta, single category dirty, every category dirty, each array at its cap, and
  each array over its cap (which must return `false` on both paths).
- **Fixed-seed randomized sequences** interleaving state mutations with draws
  and barriers. The seed is pinned, so CI remains deterministic.

**What this harness does not prove:** state combinations absent from the corpus.
No exhaustiveness is claimed. Phase 2 therefore also takes the repository's
normal promotion evidence per `agents/rules/test_wild.rules.md` — GT1/GT2/GT3
and SFIV runs, screenshot comparison against the `v0.0.3` visual anchor, and
`gpu_command_buffer_errors = 0`.

## 7. Deletions, in one commit after Phase 2

- The 21 `D9CCommandRecord*` struct definitions in `include/dxmt9/device_c.h`
- `appendLegacyCommandRecordAsV2`, `appendLegacySparseRecord`,
  `populateLegacySparseState`, `loadLegacy`, `legacyRange`
- The `build` and `encode` phases of `appendCommandRecordDirect` (`resize` and
  `flush` remain)
- `D9CDrawPrimitivePacket` and the dead bridge ops that carry it —
  `dxmt9c_device_draw_primitive_packet` / `dxmt9c_device_draw_primitive_chunk`
  have **zero PE callers** — together with the unix-side `applyDrawPacketState*`
  family they feed
- Unix-side legacy symbols with zero external references:
  `validateCommandRecord`, `importedRecordIsDrawRunCandidate`,
  `drawPacketStateDeltaEquals`, `collectDrawPacketResourceHazards`. Each is
  re-verified immediately before deletion, not on the strength of this survey.

**Retained:** `D9CDrawPacketTextureStageState`, `D9CDrawPacketSamplerState`, and
`D9CDrawPacketTransform` are used directly by `SparseStateV2Input` and stay.
`packetHasNoStateDelta` has 9 external references and is judged separately.

Removing the bridge ops changes `DXMT9_WINEMETAL_CALL_ABI_HASH`, so both PE
build directories and the unix provider must be rebuilt together
(`agents/rules/build.rules.md` lockstep rule). This is routine, not a risk.

## 8. Risks

| Risk | Mitigation |
|---|---|
| The corpus misses a state combination that only real workloads produce | Wild-run evidence (§6) is required in addition to the differential, not instead of it |
| Phase 1c touches 6 call sites in a 14,971-line file | Verbatim move with an unchanged body; the existing suite plus the full-snapshot spec are the gate. No behavior change is permitted in this step |
| The `4.3%` does not survive because section encoding remains | Stated as an expectation, not a commitment (§1). The design is justified structurally |
| Porting `DXMT9_PE_DRAW_FULL_SNAPSHOT` to the sparse producer introduces a divergence the delta path does not exercise | The differential corpus runs both modes; the ported spec runs against the real producer instead of a mirror |
| An intermediate state ships to master where some families are migrated and others are not | Accepted deliberately. Each step is independently verified by the differential and the existing suite; a half-migrated tree is correct, just not yet minimal |

## 9. What this design does not do

- It does not change the V2 wire format, the unix-side replay path, or
  `SparseReplaySinkV2`.
- It does not restructure `peState_`.
- It does not address the other `appendRecordDirect` phases. `flush` is
  `21.9%` of append and is a separate question.
- It does not claim an FPS improvement.

## Related

- [attribution.04](../../perfomance/present-pacing/present-pacing-post-defselect-cpu-attribution.04.md)
  — `appendRecordDirect` at `14.5%` of GT2's critical thread, calibrated
- [append-decomposition.01](../../perfomance/state-churn-encode/state-churn-encode-append-decomposition.01.md)
  — the phase split that identified the round trip
- `agents/rules/codebase_conventions.rules.md` — pure value transforms, DOD hot paths
- `agents/rules/build.rules.md` — PE/unix ABI lockstep
