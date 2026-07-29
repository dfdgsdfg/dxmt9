# PE Legacy Record Removal Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the PE recorder's legacy intermediate record format and the fat `D9CDrawPrimitivePacket`, so `peState_` emits `SparseStateV2Input` directly into the V2 chunk builder.

**Architecture:** Two phases. Phase 1 rehosts the existing producer, unchanged, into a natively-buildable translation unit behind explicit POD parameters — no behavior change. Phase 2 rewrites it there, one record family at a time, gated by a native differential test that runs the real old and new producers over a shared corpus and compares emitted V2 bytes plus builder side effects.

**Tech Stack:** C++20, Meson/Ninja, macOS host build. No new dependencies.

**Spec:** `docs/superpowers/specs/2026-07-29-pe-legacy-record-removal-design.md`

## Global Constraints

- 2-space indentation, spaces not tabs, `#pragma once` in headers, local project includes before standard headers. Match surrounding file style; do not mass-format.
- No per-draw or per-state heap allocation on rendering paths. Scratch storage is reused, device-owned, fixed-capacity.
- Do not store borrowed spans, stack pointers, or PE COM pointers past the call that received them.
- PE-side code must not call Metal, Objective-C, or macOS framework APIs.
- New TUs added to native test targets must not include `windows.h` or `d3d9.h`, transitively included.
- New meson tests use stable names beginning with `dxmt9-`.
- Emitted V2 sections require strictly ascending slot order per section (`orderedSlot`, `src/d3d9/d3d9_pe_chunk_v2_draw.cpp:190`), spans alive across the `appendSparseRecordV2` call, and `valid <= 1`.
- The V2 encoders (`appendSparseRecordV2`, `appendApplyStateV2`, `appendSetConstantsV2`, `appendClearV2`, `appendPresentV2`, `appendStretchRectV2`, `appendColorFillV2`) do not change in this plan.
- Chunk seal cadence must not change. `appendCommandRecordDirect`'s capacity precheck (`src/d3d9/d3d9_pe_device.cpp:9182-9184`) keeps consuming a size value on the same scale as today's legacy record size.
- `git diff --check` must be clean before every commit.
- Do not create `specs/**/plan.md` (gitignored).

---

## File Structure

**New files:**

| File | Responsibility |
|---|---|
| `src/d3d9/d3d9_pe_producer_views.hpp` | POD input views: `PeStreamBinding`, `PeBindingView`, `PeChunkContext`, `PeDrawPayloads`, `PeSparseScratch`. No logic. |
| `src/d3d9/d3d9_pe_producer.hpp` | Declarations of `buildDrawPacketFromViews` (Phase 1) and `buildSparseStateV2` (Phase 2). |
| `src/d3d9/d3d9_pe_producer.cpp` | Both producer implementations. Natively buildable. |
| `tests/native/bridge/pe_shadow_native_spec.cpp` | Proves the shadow headers compile and behave without `windows.h`. |
| `tests/native/bridge/pe_producer_views_spec.cpp` | Pins the view types' POD-ness and default state. |
| `tests/native/bridge/pe_producer_differential_spec.cpp` | The corpus + old-vs-new differential. Grows across Tasks 6-8. |

**Modified files:**

| File | Change |
|---|---|
| `src/d3d9/d3d9_pe_state_shadow.hpp` | Windows types replaced with `std::uint32_t` and mirrored constants; `d3d9_pe.hpp` include dropped. |
| `src/d3d9/d3d9_pe_device.cpp` | Producer body removed; 6 call sites populate views; per-family call sites switch to direct V2 emitters. |
| `src/d3d9/d3d9_pe_chunk_v2_draw.cpp` | Legacy shim functions deleted in Task 9. |
| `src/d3d9/meson.build` | New TU added to `dxmt9_pe_core_srcs`. |
| `tests/native/bridge/meson.build` | Three new test executables. |
| `include/dxmt9/device_c.h` | Legacy record structs and fat-packet bridge ops deleted in Task 9. |

---

## Task 1: Make the PE state shadow Windows-free

**Files:**
- Modify: `src/d3d9/d3d9_pe_state_shadow.hpp` (lines 194-228, 309-344, 380-411, 456)
- Create: `tests/native/bridge/pe_shadow_native_spec.cpp`
- Modify: `tests/native/bridge/meson.build`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `PeHotStateShadow` (the existing struct, declared at global scope, unchanged in shape) compiles on a native host. `d3d9_pe_const_shadow.hpp` is already Windows-free and needs no change.

**Background:** `d3d9_pe_state_shadow.hpp:8` includes `d3d9_pe.hpp`, which is `windows.h` + `d3d9.h` + `dxmt9/device_c.h`. That include is the only thing keeping the shadow off the native build. The Windows surface it actually uses is 27 sites of `DWORD` and five D3D constant families.

The replacement constant values, read from `/Users/dididi/workspaces/wine/include/d3d9types.h`:

| Constant | Value | Source |
|---|---:|---|
| `D3DTS_VIEW` | `2` | `d3d9types.h:1201` |
| `D3DTS_PROJECTION` | `3` | `d3d9types.h:1202` |
| `D3DTS_TEXTURE0` | `16` | `d3d9types.h:1203` |
| `D3DTS_TEXTURE7` | `23` | `d3d9types.h:1210` |
| `D3DTS_WORLD` | `256` | `d3d9.h:98` → `D3DTS_WORLDMATRIX(0)` = `0 + 256` |
| `D3DDMAPSAMPLER` | `256` | `d3d9types.h:201` |
| `D3DVERTEXTEXTURESAMPLER0` | `257` | `d3d9types.h:202` (`D3DDMAPSAMPLER+1`) |
| `D3DVERTEXTEXTURESAMPLER3` | `260` | `d3d9types.h:205` (`D3DDMAPSAMPLER+4`) |

- [ ] **Step 1: Write the failing test**

Create `tests/native/bridge/pe_shadow_native_spec.cpp`:

```cpp
// pe_shadow_native_spec
//
// Proves d3d9_pe_state_shadow.hpp compiles and behaves on a native host with
// no windows.h / d3d9.h in its transitive include set. The transform-slot and
// sampler-slot mappings are pinned against the literal D3D9 constant values
// they replaced, so a wrong mirror constant fails here rather than in Wine.

#include "d3d9_pe_state_shadow.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

struct TestFailure : std::runtime_error {
  explicit TestFailure(std::string message)
      : std::runtime_error(std::move(message)) {}
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

// d3d9_pe_state_shadow.hpp declares everything at global scope -- there is no
// enclosing namespace to alias here. The V2 builder's types (dxmt9::d3d9::pe)
// are not used by this spec.

void transformSlotsMatchD3DConstants() {
  std::uint32_t slot = 0;

  // D3DTS_VIEW == 2, D3DTS_PROJECTION == 3 (d3d9types.h:1201-1202).
  check(FixedTransformTable::slotForState(2u, slot), "D3DTS_VIEW must map");
  const std::uint32_t viewSlot = slot;
  check(FixedTransformTable::slotForState(3u, slot), "D3DTS_PROJECTION must map");
  check(slot != viewSlot, "view and projection must occupy distinct slots");

  // D3DTS_TEXTURE0 == 16 .. D3DTS_TEXTURE7 == 23 (d3d9types.h:1203-1210).
  check(FixedTransformTable::slotForState(16u, slot), "D3DTS_TEXTURE0 must map");
  const std::uint32_t tex0 = slot;
  check(FixedTransformTable::slotForState(23u, slot), "D3DTS_TEXTURE7 must map");
  check(slot == tex0 + 7u, "texture transform slots must be contiguous");

  // D3DTS_WORLD == D3DTS_WORLDMATRIX(0) == 256 (d3d9.h:98, d3d9types.h:102).
  check(FixedTransformTable::slotForState(256u, slot), "D3DTS_WORLD must map");
  const std::uint32_t world0 = slot;
  check(FixedTransformTable::slotForState(259u, slot), "D3DTS_WORLDMATRIX(3) must map");
  check(slot == world0 + 3u, "world matrix slots must be contiguous");

  // Round-trip: every mapped slot must recover its original state id.
  for (const std::uint32_t state : {2u, 3u, 16u, 23u, 256u, 259u}) {
    check(FixedTransformTable::slotForState(state, slot), "state must map");
    check(FixedTransformTable::stateForSlot(slot) == state,
          "transform slot must round-trip to its state id");
  }
}

void vertexTextureSamplerSlotsMatchD3DConstants() {
  std::uint32_t slot = 0;

  // D3DVERTEXTEXTURESAMPLER0 == D3DDMAPSAMPLER + 1 == 257 (d3d9types.h:202).
  check(!vertexTextureSamplerSlot(256u, slot),
        "D3DDMAPSAMPLER (256) is not a vertex texture sampler");
  check(vertexTextureSamplerSlot(257u, slot),
        "D3DVERTEXTEXTURESAMPLER0 must map");
  check(slot == 0u, "D3DVERTEXTEXTURESAMPLER0 must be slot 0");
  check(vertexTextureSamplerSlot(260u, slot),
        "D3DVERTEXTEXTURESAMPLER3 must map");
  check(slot == 3u, "D3DVERTEXTEXTURESAMPLER3 must be slot 3");
  check(!vertexTextureSamplerSlot(261u, slot),
        "261 is past D3DVERTEXTEXTURESAMPLER3");
}

void pendingMasksAreThirtyTwoBitUnsigned() {
  PeHotStateShadow shadow{};
  check(!shadow.hasPendingHotState(), "a fresh shadow has nothing pending");

  // The pending masks were DWORD. After the change they must still hold all
  // 32 bits without sign extension.
  shadow.pendingTextureMask = 0xFFFFFFFFu;
  check(shadow.pendingTextureMask == 0xFFFFFFFFu,
        "pendingTextureMask must hold 32 bits unsigned");
  check(shadow.hasPendingHotState(), "a set mask must be pending");

  shadow.clearPendingHotState();
  check(!shadow.hasPendingHotState(), "clearPendingState must clear the mask");
  check(shadow.pendingTextureMask == 0u, "mask must be zero after clear");
}

int main() {
  try {
    transformSlotsMatchD3DConstants();
    vertexTextureSamplerSlotsMatchD3DConstants();
    pendingMasksAreThirtyTwoBitUnsigned();
  } catch (const TestFailure& failure) {
    std::cerr << "pe_shadow_native_spec FAILED: " << failure.what() << "\n";
    return 1;
  }
  std::cout << "pe_shadow_native_spec OK\n";
  return 0;
}
```

Append to `tests/native/bridge/meson.build`, following the existing
`pe_chunk_record_v2_value_spec` pattern at `:130-145`:

```meson
pe_shadow_native_spec = executable(
  'dxmt9-pe-shadow-native-spec',
  ['pe_shadow_native_spec.cpp'],
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src/d3d9'),
  ],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-pe-shadow-native-spec',
     pe_shadow_native_spec,
     env: dxmt9_native_test_env)
```

- [ ] **Step 2: Run the test to verify it fails**

```sh
meson compile -C build dxmt9-pe-shadow-native-spec
```

Expected: FAIL at compile time with `windows.h` (or `d3d9.h`) not found,
originating from `d3d9_pe_state_shadow.hpp:8`'s include of `d3d9_pe.hpp`.

If the exact helper names in the test (`FixedTransformTable::slotForState`,
`FixedTransformTable::stateForSlot`, `vertexTextureSamplerSlot`, `hasPendingHotState`,
`clearPendingHotState`) do not match the header, correct the test to the header's
real names before proceeding — the header is the source of truth, not this
plan. Read `src/d3d9/d3d9_pe_state_shadow.hpp:190-230` and `:309-350` and
`:414-440` for the actual names.

- [ ] **Step 3: Replace the Windows types**

In `src/d3d9/d3d9_pe_state_shadow.hpp`:

Replace the `d3d9_pe.hpp` include with `dxmt9/device_c.h` (which
`d3d9_pe_const_shadow.hpp` already pulls in, but state it explicitly).

Add a mirrored-constant block near the top of the `dxmt9::d3d9::pe` namespace:

```cpp
// D3D9 constant mirrors. This header is compiled natively (no windows.h /
// d3d9.h), so the values are inlined from the D3D9 SDK headers rather than
// included. Values verified against wine/include/d3d9types.h and d3d9.h;
// see the table in docs/superpowers/plans/2026-07-29-pe-legacy-record-removal.md.
// core_constants.hpp:665 sets the precedent for mirroring the WORLDMATRIX
// range this way.
inline constexpr std::uint32_t kD3dTsView = 2u;         // D3DTS_VIEW
inline constexpr std::uint32_t kD3dTsProjection = 3u;   // D3DTS_PROJECTION
inline constexpr std::uint32_t kD3dTsTexture0 = 16u;    // D3DTS_TEXTURE0
inline constexpr std::uint32_t kD3dTsTexture7 = 23u;    // D3DTS_TEXTURE7
inline constexpr std::uint32_t kD3dTsWorld = 256u;      // D3DTS_WORLDMATRIX(0)
inline constexpr std::uint32_t kD3dDmapSampler = 256u;  // D3DDMAPSAMPLER
inline constexpr std::uint32_t kD3dVertexTextureSampler0 = kD3dDmapSampler + 1u;
inline constexpr std::uint32_t kD3dVertexTextureSampler3 = kD3dDmapSampler + 4u;
```

Then, mechanically:

- `static_cast<std::uint32_t>(D3DTS_VIEW)` → `kD3dTsView`, and the same for
  `D3DTS_PROJECTION`, `D3DTS_TEXTURE0`, `D3DTS_TEXTURE7`, `D3DTS_WORLD`
  (lines 194, 198, 202-205, 208-212, 219, 222, 225, 228).
- `D3DVERTEXTEXTURESAMPLER0` → `kD3dVertexTextureSampler0` and
  `D3DVERTEXTEXTURESAMPLER3` → `kD3dVertexTextureSampler3` (lines 314, 318).
- `DWORD` → `std::uint32_t` at lines 309, 313, 322, 336, 380-381, 387, 403,
  407, 409-411, 456 (both parameters).
- `D3DTEXTURESTAGESTATETYPE` (line 331) and `D3DSAMPLERSTATETYPE` (line 344)
  → `std::uint32_t`. Both parameters are already `static_cast` to
  `std::uint32_t` inside the bodies; check and simplify the now-redundant
  casts only if they become no-ops.

- [ ] **Step 4: Run the test to verify it passes**

```sh
meson compile -C build dxmt9-pe-shadow-native-spec
meson test -C build dxmt9-pe-shadow-native-spec
```

Expected: PASS.

- [ ] **Step 5: Verify nothing else broke**

```sh
meson test -C build
```

Expected: all tests pass (667 registered at the time of writing, plus the one
added here). The shadow's callers in `d3d9_pe_device.cpp` pass `DWORD`
arguments, which convert to `std::uint32_t` implicitly — but that file only
builds on Windows, so a native run cannot prove it. Confirm by inspection that
no call site relies on a `DWORD`-specific overload:

```sh
grep -n "textureStageSlot\|vertexTextureSamplerSlot\|textureBindingSlot\|samplerSlot\|samplerStateSlot\|renderStateEquals" src/d3d9/d3d9_pe_device.cpp
```

- [ ] **Step 6: Commit**

```sh
git add src/d3d9/d3d9_pe_state_shadow.hpp tests/native/bridge/pe_shadow_native_spec.cpp tests/native/bridge/meson.build
git commit -m "refactor(pe): make the PE state shadow natively buildable

Replace windows.h-dependent types in d3d9_pe_state_shadow.hpp with
std::uint32_t and mirrored D3D9 constants, and drop the d3d9_pe.hpp
include. Values verified against wine/include/d3d9types.h.

Adds dxmt9-pe-shadow-native-spec, which both proves the header compiles
without windows.h and pins the mirrored constants against the literal
D3D9 values they replaced."
```

---

## Task 2: POD input views for the producer

**Files:**
- Create: `src/d3d9/d3d9_pe_producer_views.hpp`
- Create: `tests/native/bridge/pe_producer_views_spec.cpp`
- Modify: `tests/native/bridge/meson.build`

**Interfaces:**
- Consumes: `PeWireObjectRef` from `d3d9_pe_chunk_v2_builder.hpp:145-151`; `D9C_DRAW_PACKET_MAX_*` caps from `include/dxmt9/device_c.h`.
- Produces: `dxmt9::d3d9::pe::PeStreamBinding`, `PeBindingView`, `PeChunkContext`, `PeDrawPayloads`, `PeDrawParams`, `PeSparseScratch`. Tasks 3 and 6-8 take these as parameters.

**Background:** The design's §3 establishes that the producer's inputs fall into four classes — COM-derived bindings, the constant shadow, draw-call payloads, and destination-chunk state. This task defines the three that are not already a struct. `PeChunkContext` exists specifically so the differential test in Task 6 can *drive* chunk-retention state; a producer that called `CommandChunkV2Builder::referencesObject` directly could not be tested that way.

- [ ] **Step 1: Write the failing test**

Create `tests/native/bridge/pe_producer_views_spec.cpp`:

```cpp
// pe_producer_views_spec
//
// The producer's input views must be trivially copyable PODs with no owning
// members, because the differential harness constructs them directly and the
// producer must not retain anything from them past the call. Scratch capacity
// must match the V2 section caps, or a full-width delta silently truncates.

#include "d3d9_pe_producer_views.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

struct TestFailure : std::runtime_error {
  explicit TestFailure(std::string message)
      : std::runtime_error(std::move(message)) {}
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

namespace pe = dxmt9::d3d9::pe;

void viewsAreTriviallyCopyable() {
  static_assert(std::is_trivially_copyable_v<pe::PeStreamBinding>);
  static_assert(std::is_trivially_copyable_v<pe::PeBindingView>);
  static_assert(std::is_trivially_copyable_v<pe::PeChunkContext>);
  check(true, "compile-time only");
}

void payloadsHoldBorrowedSpans() {
  // PeDrawPayloads carries spans, so it is copyable but NOT trivially
  // constructible with owning semantics. It must be empty by default.
  pe::PeDrawPayloads payloads{};
  check(payloads.upIndex.empty(), "default upIndex must be empty");
  check(payloads.upVertex.empty(), "default upVertex must be empty");
}

void defaultBindingViewIsAllNull() {
  pe::PeBindingView bindings{};
  for (const auto& texture : bindings.textures) {
    check(texture.object == nullptr, "default texture ref must be null");
  }
  for (const auto& stream : bindings.streams) {
    check(stream.buffer.object == nullptr, "default stream buffer must be null");
    check(stream.offset == 0u, "default stream offset must be zero");
    check(stream.stride == 0u, "default stream stride must be zero");
  }
  check(bindings.vs.object == nullptr, "default vs must be null");
  check(bindings.ps.object == nullptr, "default ps must be null");
  check(bindings.vdecl.object == nullptr, "default vdecl must be null");
  check(bindings.indexBuffer.object == nullptr, "default ib must be null");
  check(bindings.depthStencil.object == nullptr, "default ds must be null");
  check(bindings.rtExplicitMask == 0u, "default rt mask must be zero");
  check(bindings.fvf == 0u, "default fvf must be zero");
}

void defaultChunkContextClaimsNothingRetained() {
  pe::PeChunkContext chunk{};
  check(chunk.retainedStreamMask == 0u,
        "a fresh chunk retains no streams, so every bound stream re-emits");
  check(!chunk.indexBufferKnown,
        "a fresh chunk has no known index buffer");
  check(chunk.submittedIndexBufferWire == 0u,
        "a fresh chunk has no submitted index buffer handle");
}

void scratchCapacityMatchesSectionCaps() {
  pe::PeSparseScratch scratch{};
  check(scratch.renderStates.size() == D9C_DRAW_PACKET_MAX_RENDER_STATES,
        "render state scratch must match the section cap");
  check(scratch.textures.size() == D9C_DRAW_PACKET_MAX_TEXTURES,
        "texture scratch must match the section cap");
  check(scratch.streams.size() == D9C_DRAW_PACKET_MAX_STREAMS,
        "stream scratch must match the section cap");
  check(scratch.renderTargets.size() == D9C_DRAW_PACKET_MAX_RENDER_TARGETS,
        "render target scratch must match the section cap");
  check(scratch.textureStageStates.size() == D9C_DRAW_PACKET_MAX_TSS,
        "TSS scratch must match the section cap");
  check(scratch.samplerStates.size() == D9C_DRAW_PACKET_MAX_SAMPLER,
        "sampler scratch must match the section cap");
  check(scratch.transforms.size() == D9C_DRAW_PACKET_MAX_TRANSFORMS,
        "transform scratch must match the section cap");
  check(scratch.lights.size() == D9C_DRAW_PACKET_MAX_LIGHTS,
        "light scratch must match the section cap");
}

int main() {
  try {
    viewsAreTriviallyCopyable();
    payloadsHoldBorrowedSpans();
    defaultBindingViewIsAllNull();
    defaultChunkContextClaimsNothingRetained();
    scratchCapacityMatchesSectionCaps();
  } catch (const TestFailure& failure) {
    std::cerr << "pe_producer_views_spec FAILED: " << failure.what() << "\n";
    return 1;
  }
  std::cout << "pe_producer_views_spec OK\n";
  return 0;
}
```

Append to `tests/native/bridge/meson.build`:

```meson
pe_producer_views_spec = executable(
  'dxmt9-pe-producer-views-spec',
  ['pe_producer_views_spec.cpp'],
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src/d3d9'),
  ],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-pe-producer-views-spec',
     pe_producer_views_spec,
     env: dxmt9_native_test_env)
```

- [ ] **Step 2: Run the test to verify it fails**

```sh
meson compile -C build dxmt9-pe-producer-views-spec
```

Expected: FAIL at compile time — `d3d9_pe_producer_views.hpp` does not exist.

- [ ] **Step 3: Create the header**

Create `src/d3d9/d3d9_pe_producer_views.hpp`:

```cpp
#pragma once

// POD input views for the PE sparse-state producer.
//
// The producer reads four classes of input (see
// docs/superpowers/specs/2026-07-29-pe-legacy-record-removal-design.md §3):
// COM-derived bindings, the constant shadow, draw-call payloads, and the
// destination chunk's retention history. Only the constant shadow is already
// a struct (PeConstShadowBlock); this header defines the other three, plus
// the reusable output scratch.
//
// PeChunkContext exists as an explicit parameter rather than a callback into
// CommandChunkV2Builder so that a native differential test can drive
// chunk-retention state directly. Do not replace it with a builder reference.

#include "d3d9_pe_chunk_v2_builder.hpp"
#include "dxmt9/device_c.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace dxmt9::d3d9::pe {

// The wire form of the device's streamSrc_ / streamOff_ / streamStr_ triple.
struct PeStreamBinding {
  PeWireObjectRef buffer{};
  std::uint32_t offset = 0u;
  std::uint32_t stride = 0u;
};

// (a) COM-derived bindings, already translated to wire refs by the device.
// The producer must not dereference `object`; it only forwards the ref to
// CommandChunkV2Builder::appendHandle, which owns retention.
struct PeBindingView {
  std::array<PeWireObjectRef, D9C_DRAW_PACKET_MAX_TEXTURES> textures{};
  std::array<PeStreamBinding, D9C_DRAW_PACKET_MAX_STREAMS> streams{};
  PeWireObjectRef vs{};
  PeWireObjectRef ps{};
  PeWireObjectRef vdecl{};
  PeWireObjectRef indexBuffer{};
  PeWireObjectRef depthStencil{};
  std::array<PeWireObjectRef, D9C_DRAW_PACKET_MAX_RENDER_TARGETS>
      renderTargets{};
  std::uint32_t rtExplicitMask = 0u;
  std::uint32_t fvf = 0u;
};

// (d) Destination-chunk history. Mirrors what
// d3d9_pe_device.cpp:9462-9477 computes today from
// CommandChunkV2Builder::referencesObject, and the index-buffer tracking at
// :9404-9412. A stream absent from retainedStreamMask must be re-emitted even
// when it is not dirty, or the chunk loses its retention record
// (R-CORE-11.17).
struct PeChunkContext {
  std::uint32_t retainedStreamMask = 0u;
  bool indexBufferKnown = false;
  std::uint64_t submittedIndexBufferWire = 0u;
};

// (c) Draw-call payloads. Borrowed for the duration of the producer call and
// the appendSparseRecordV2 that consumes its output. Never stored.
struct PeDrawPayloads {
  std::span<const std::byte> upIndex{};
  std::span<const std::byte> upVertex{};
};

// Per-draw scalars. Task 6 uses only the defaults (APPLY_STATE carries no
// draw header); Task 7 populates them into D9CCommandChunkWireDrawHeaderV2.
struct PeDrawParams {
  std::uint32_t recordType = 0u;
  std::uint32_t primitiveType = 0u;
  std::int32_t baseVertex = 0;
  std::uint32_t minVertex = 0u;
  std::uint32_t numVertices = 0u;
  std::uint32_t startVertex = 0u;
  std::uint32_t startIndex = 0u;
  std::uint32_t primitiveCount = 0u;
  std::uint32_t stride = 0u;
  std::uint32_t indexFormat = 0u;
};

// Device-owned, reused output storage. The SparseStateV2Input spans the
// producer fills point into these arrays, so the scratch must outlive the
// appendSparseRecordV2 call. Capacities match the V2 section caps exactly;
// the producer returns false rather than truncating.
struct PeSparseScratch {
  std::array<D9CCommandChunkWireRenderStateV2,
             D9C_DRAW_PACKET_MAX_RENDER_STATES> renderStates{};
  std::array<SparseBindingV2Input<D9CCommandChunkWireTextureBindingV2>,
             D9C_DRAW_PACKET_MAX_TEXTURES> textures{};
  std::array<SparseBindingV2Input<D9CCommandChunkWireStreamBindingV2>,
             D9C_DRAW_PACKET_MAX_STREAMS> streams{};
  std::array<SparseBindingV2Input<D9CCommandChunkWireShaderBindingV2>, 2>
      shaders{};
  std::array<SparseBindingV2Input<D9CCommandChunkWireVertexInputV2>, 2>
      vertexInputs{};
  std::array<SparseBindingV2Input<D9CCommandChunkWireIndexBindingV2>, 1>
      indexBuffers{};
  std::array<SparseBindingV2Input<D9CCommandChunkWireRenderTargetBindingV2>,
             D9C_DRAW_PACKET_MAX_RENDER_TARGETS> renderTargets{};
  std::array<SparseBindingV2Input<D9CCommandChunkWireDepthStencilBindingV2>, 1>
      depthStencils{};
  std::array<D9CViewport, 1> viewports{};
  std::array<D9CRect, 1> scissors{};
  std::array<D9CMaterial, 1> materials{};
  std::array<D9CCommandChunkWireClipPlaneV2, 6> clipPlanes{};
  std::array<D9CDrawPacketTextureStageState, D9C_DRAW_PACKET_MAX_TSS>
      textureStageStates{};
  std::array<D9CDrawPacketSamplerState, D9C_DRAW_PACKET_MAX_SAMPLER>
      samplerStates{};
  std::array<D9CDrawPacketTransform, D9C_DRAW_PACKET_MAX_TRANSFORMS>
      transforms{};
  std::array<D9CCommandChunkWireLightV2, D9C_DRAW_PACKET_MAX_LIGHTS> lights{};
  std::array<D9CCommandChunkWireLightEnableV2, D9C_DRAW_PACKET_MAX_LIGHTS>
      lightEnables{};
};

}  // namespace dxmt9::d3d9::pe
```

- [ ] **Step 4: Run the test to verify it passes**

```sh
meson compile -C build dxmt9-pe-producer-views-spec
meson test -C build dxmt9-pe-producer-views-spec
```

Expected: PASS.

If the `static_assert`s on trivial copyability fail, the cause is almost
certainly a member with a non-trivial default — check that `PeWireObjectRef`
and the `D9C*` wire structs are aggregates. Do not "fix" it by relaxing the
assert; the differential harness depends on these being memcpy-able.

- [ ] **Step 5: Commit**

```sh
git add src/d3d9/d3d9_pe_producer_views.hpp tests/native/bridge/pe_producer_views_spec.cpp tests/native/bridge/meson.build
git commit -m "feat(pe): add POD input views for the sparse-state producer

PeBindingView, PeChunkContext, PeDrawPayloads, and PeSparseScratch are the
producer's explicit inputs and output storage. PeChunkContext is a parameter
rather than a CommandChunkV2Builder callback so a native differential test
can drive chunk-retention state directly."
```

---

## Task 3: Rehost the producer against the views

**Files:**
- Create: `src/d3d9/d3d9_pe_producer.hpp`
- Create: `src/d3d9/d3d9_pe_producer.cpp`
- Modify: `src/d3d9/d3d9_pe_device.cpp:3867-4103` (remove), and call sites at `:9329`, `:9389`, `:9527`, `:9627`, `:10000`, `:10139`
- Modify: `src/d3d9/meson.build:47-55` (add the new TU to `dxmt9_pe_core_srcs`)

**Interfaces:**
- Consumes: `PeHotStateShadow` (Task 1), `PeBindingView` / `PeChunkContext` / `PeDrawPayloads` (Task 2).
- Produces:
  ```cpp
  bool buildDrawPacketFromViews(const PeHotStateShadow& shadow,
                                const PeBindingView& bindings,
                                std::uint32_t primitiveType,
                                std::uint32_t startVertex,
                                std::uint32_t primitiveCount,
                                bool forceFullSnapshot,
                                PeDecimatedScopeStats& stats,
                                D9CDrawPrimitivePacket& packet) noexcept;
  ```
  Task 6 adds `buildSparseStateV2` to the same header.

**This task has no native test coverage, and that is a known, accepted gap.**
No meson test compiles `d3d9_pe_device.cpp` — `src/d3d9/meson.build:65` gates
`dxmt9_pe_core_srcs` on `host_machine.system() == 'windows'`.
`tests/native/bridge/pe_full_snapshot_equivalence_spec.cpp` cannot substitute:
its own header (`:44-53`) states it *mirrors* `buildDrawPrimitivePacket` at
test scope rather than calling it. Citing it here would be a false oracle.

The two substitute gates are Step 5 (mechanically reviewable diff) and Step 6
(Wine runtime evidence). Do not skip either. If the diff in Step 5 cannot be
kept mechanical, stop and split the task rather than proceeding.

- [ ] **Step 1: Create the producer header**

Create `src/d3d9/d3d9_pe_producer.hpp`:

```cpp
#pragma once

// The PE sparse-state producer.
//
// buildDrawPacketFromViews is d3d9_pe_device.cpp's former
// buildDrawPrimitivePacket, rehosted here against explicit POD inputs so it
// compiles and runs on a native host. Its body is unchanged; only member
// accesses became parameter accesses.
//
// It is a pure read of {shadow, bindings} apart from `stats`, which the
// DXMT9_PE_STATS_DECIMATION instrumentation accumulates into. It does NOT
// clear pending bits — callers do that on success.

#include "d3d9_pe_producer_views.hpp"
#include "d3d9_pe_state_shadow.hpp"
#include "d3d9_pe_stats_decimation.hpp"
#include "dxmt9/device_c.h"

#include <cstdint>

namespace dxmt9::d3d9::pe {

bool buildDrawPacketFromViews(const PeHotStateShadow& shadow,
                              const PeBindingView& bindings,
                              std::uint32_t primitiveType,
                              std::uint32_t startVertex,
                              std::uint32_t primitiveCount,
                              bool forceFullSnapshot,
                              PeDecimatedScopeStats& stats,
                              D9CDrawPrimitivePacket& packet) noexcept;

}  // namespace dxmt9::d3d9::pe
```

- [ ] **Step 2: Move the body verbatim**

Create `src/d3d9/d3d9_pe_producer.cpp` and move the body of
`d3d9_pe_device.cpp:3867-4103` into `buildDrawPacketFromViews`.

The substitution table — apply exactly these, and nothing else:

| Was | Becomes |
|---|---|
| `peState_.X` | `shadow.X` |
| `textures_[i]` (via `toWireHandle(rawTex(...))`) | `bindings.textures[i]` |
| `streamSrc_[i]` / `streamOff_[i]` / `streamStr_[i]` | `bindings.streams[i].buffer` / `.offset` / `.stride` |
| `vs_` / `ps_` / `vdecl_` / `dsSurface_` | `bindings.vs` / `.ps` / `.vdecl` / `.depthStencil` |
| `fvf_` | `bindings.fvf` |
| `currentRtWireHandles()` | `bindings.renderTargets` |
| `currentRtExplicitMask()` | `bindings.rtExplicitMask` |
| `peDrawPacketDecimatedStats_` | `stats` |
| `type` / `startVertex` / `count` (D3D types) | `primitiveType` / `startVertex` / `primitiveCount` (`std::uint32_t`) |

The `toWireHandle(raw*(...))` calls disappear entirely — the device performs
that translation when it fills `PeBindingView`. Where the old code wrote
`packet.textures[stage] = toWireHandle(rawTex(textures_[stage]))`, the new
code writes `packet.textures[stage] = bindings.textures[stage].identity` (or
whichever field the packet's `D9CWireHandle` expects — read
`PeWireObjectRef` at `d3d9_pe_chunk_v2_builder.hpp:145-151` and match it).

Keep `dxmt9PeFullSnapshotEnabled()`'s effect by leaving the `forceFullSnapshot
|| dxmt9PeFullSnapshotEnabled()` branch intact; `dxmt9PeFullSnapshotEnabled`
reads an env var through `util/config`, which is already native-safe.

Add the TU to `src/d3d9/meson.build`'s `dxmt9_pe_core_srcs` list (alongside
`'d3d9_pe_device.cpp'` at `:51`), and to the two native test executables that
will need it in Task 6 — add it now to
`tests/native/bridge/pe_producer_views_spec.cpp`'s target so a native
compile failure surfaces immediately:

```meson
pe_producer_views_spec = executable(
  'dxmt9-pe-producer-views-spec',
  [
    'pe_producer_views_spec.cpp',
    '../../../src/d3d9/d3d9_pe_producer.cpp',
  ],
  # ... rest unchanged
)
```

- [ ] **Step 3: Rewrite the 6 call sites**

At each of `d3d9_pe_device.cpp:9329`, `:9389`, `:9527`, `:9627`, `:10000`,
`:10139`, replace the `buildDrawPrimitivePacket(...)` call with:

```cpp
populateBindingView(peBindingView_);   // new private helper, see below
if (!dxmt9::d3d9::pe::buildDrawPacketFromViews(
        peState_, peBindingView_,
        static_cast<std::uint32_t>(type), startVertex, count,
        /*forceFullSnapshot=*/false,
        peDrawPacketDecimatedStats_, record.packet)) {
  return D3DERR_INVALIDCALL;   // preserve each site's existing failure return
}
```

Add one private helper to the device class that fills `PeBindingView` from the
COM members — this is the only new logic in this task:

```cpp
void populateBindingView(dxmt9::d3d9::pe::PeBindingView& view) const {
    for (DWORD stage = 0; stage < D9C_DRAW_PACKET_MAX_TEXTURES; ++stage) {
        view.textures[stage] = wireRef(rawTex(textures_[stage]));
    }
    for (DWORD stream = 0; stream < D9C_DRAW_PACKET_MAX_STREAMS; ++stream) {
        view.streams[stream].buffer = wireRef(rawVBuf(streamSrc_[stream]));
        view.streams[stream].offset = streamOff_[stream];
        view.streams[stream].stride = streamStr_[stream];
    }
    view.vs = wireRef(rawVS(vs_));
    view.ps = wireRef(rawPS(ps_));
    view.vdecl = wireRef(rawVDecl(vdecl_));
    view.indexBuffer = wireRef(rawIBuf(ib_));
    view.depthStencil = wireRef(rawSurf(dsSurface_));
    const auto rts = currentRtWireHandles();
    for (DWORD slot = 0; slot < D9C_DRAW_PACKET_MAX_RENDER_TARGETS; ++slot) {
        view.renderTargets[slot] = rts[slot];
    }
    view.rtExplicitMask = currentRtExplicitMask();
    view.fvf = fvf_;
}
```

`wireRef` is whatever the device already uses to produce a `PeWireObjectRef`
from a raw object pointer — find it by reading how `PeWireObjectRef` values
are constructed in `d3d9_pe_device.cpp` (search for `cacheWireObjectRef` and
`lookupCachedWireObjectRef`) and use the existing spelling. Do not invent a
new one. `peBindingView_` is a new device member so the fill reuses storage
rather than allocating per draw.

**Preserve the UP save/restore.** The call site at `:9527` (and `:9627`)
temporarily mutates `fvf_` / `vdecl_` / `vs_` and three pending bits around
the call and restores them afterwards (`:9511-9526`). That dance must survive:
because `populateBindingView` now snapshots those members into the view, the
fill must happen *inside* the mutated window, not before it. Read
`:9505-9535` carefully and place the `populateBindingView` call so the view
sees the temporarily-mutated values, exactly as the old inline read did.

- [ ] **Step 4: Build both PE lanes**

```sh
meson compile -C build
meson compile -C build-win32-x64-builtin
meson compile -C build-win32-x86-builtin
```

Expected: all three succeed. The native build proves `d3d9_pe_producer.cpp`
compiles without Windows; the two PE builds prove the call sites are correct.

Per `agents/rules/build.rules.md`, never build `winemetal.so` with a bare
`ninja` target — always `meson compile -C <builddir>`.

- [ ] **Step 5: Prove the diff is mechanical**

```sh
git diff src/d3d9/d3d9_pe_device.cpp > /tmp/task3-device.diff
git diff --stat
```

Read `/tmp/task3-device.diff` and confirm every removed line from the
producer body reappears in `d3d9_pe_producer.cpp` differing only by the
substitution table in Step 2. Anything else in the diff is a defect. Write the
list of lines that are NOT pure substitutions (there should be exactly two
categories: the new `populateBindingView` helper, and the six call-site
rewrites) into the task report.

- [ ] **Step 6: Wine runtime evidence**

```sh
python3 scripts/run_apps/run_experiment.py run app-d3d9-3dmark05
python3 scripts/run_apps/run_experiment.py run app-d3d9-sfiv-benchmark
```

Per `agents/rules/test_wild.rules.md`, this is the one invocation path — do
not use a per-app wrapper. Confirm for each run:

- `result.json` status is `pass` (a timeout-finalized 3DMark05 run with the
  expected artifacts is acceptable; see `agents/rules/metal_debugging.rules.md`).
- `gpu_command_buffer_errors` is `0`.
- The rendered scene matches the `v0.0.3` visual anchor — no missing geometry,
  no black characters, no texture drift.

If a run regresses, do not proceed. Return to Step 5's diff and find the
non-mechanical change.

- [ ] **Step 7: Run the full suite and commit**

```sh
meson test -C build
git diff --check
git add src/d3d9/d3d9_pe_producer.hpp src/d3d9/d3d9_pe_producer.cpp src/d3d9/d3d9_pe_device.cpp src/d3d9/meson.build tests/native/bridge/meson.build
git commit -m "refactor(pe): rehost the draw-packet producer against POD views

Move buildDrawPrimitivePacket out of d3d9_pe_device.cpp into
d3d9_pe_producer.cpp, taking PeStateShadow, PeBindingView, and the
decimation stats as explicit parameters. The body is unchanged; member
accesses became parameter accesses.

No native test covers this step -- no meson test compiles
d3d9_pe_device.cpp, and pe_full_snapshot_equivalence_spec mirrors the
producer rather than executing it. Gated instead by a mechanically
reviewable diff and by GT1/SFIV runs against the v0.0.3 visual anchor."
```

---

## Task 4: Emit non-draw records directly as V2

**Files:**
- Modify: `src/d3d9/d3d9_pe_device.cpp` — the non-draw record call sites (`appendCommandRecord(&rec, sizeof(rec))` at `:10037`, `:10056`, `:10076`, `:10150`, `:10200`, and the retained variants)
- Modify: `src/d3d9/d3d9_pe_chunk_v2_draw.cpp:914-1010` — remove the migrated `case` labels from `appendLegacyCommandRecordAsV2`

**Interfaces:**
- Consumes: nothing new.
- Produces: no new symbols. The non-draw call sites now call `appendClearV2`, `appendPresentV2`, `appendStretchRectV2`, `appendColorFillV2`, and their siblings directly from `d3d9_pe_chunk_v2_builder.hpp:208-219`.

**Background:** These records never touched the fat packet — the call site built a small legacy struct and `appendLegacyCommandRecordAsV2` converted it. Removing the middle step means calling the same V2 emitter with the same values.

**Two things this step is not.** (i) It does not remove the legacy path from
barrier sites: `chunkBarrierFlush()` still prepends a legacy `APPLY_STATE`
until Task 6. (ii) Object-ref acquisition moves from `lookupCachedWireObjectRef`
(`d3d9_pe_chunk_v2_draw.cpp:468-476`) to wrapper-side refs, which changes both
the failure path and the `noteWireIdentityGetterCall` counter. Neither blocks
the step; both belong in the task report.

- [ ] **Step 1: Extend the existing V2 value spec with a Clear case**

Add to `tests/native/bridge/pe_chunk_record_v2_value_spec.cpp`, before `main`:

```cpp
// Task 4 regression: a Clear emitted directly must be byte-identical to one
// emitted through the legacy shim, including its rect payload.
void directClearMatchesLegacyClear() {
  dxmt9::d3d9::pe::CommandChunkV2Builder direct;
  dxmt9::d3d9::pe::CommandChunkV2Builder viaLegacy;

  const D9CRect rects[2] = {{0, 0, 64, 64}, {64, 64, 128, 128}};
  const D9CCommandChunkWireClearV2 fixed{
      .flags = 0x7u,
      .colorARGB = 0xFF204080u,
      .z = 1.0f,
      .stencil = 0u,
      .rectCount = 0u,
      .rectOffset = 0u,
  };

  check(dxmt9::d3d9::pe::appendClearV2(direct, fixed,
                                   std::span<const D9CRect>(rects, 2)),
        "direct Clear must append");

  D9CCommandRecordClear legacy{};
  legacy.header.type = D9C_COMMAND_RECORD_CLEAR;
  legacy.flags = fixed.flags;
  legacy.colorARGB = fixed.colorARGB;
  legacy.z = fixed.z;
  legacy.stencil = fixed.stencil;
  legacy.rectCount = 2u;
  legacy.rectOffset = sizeof(D9CCommandRecordClear);
  legacy.header.size = sizeof(legacy) + sizeof(rects);
  std::vector<std::byte> legacyBytes(legacy.header.size);
  std::memcpy(legacyBytes.data(), &legacy, sizeof(legacy));
  std::memcpy(legacyBytes.data() + sizeof(legacy), rects, sizeof(rects));
  check(dxmt9::d3d9::pe::appendLegacyCommandRecordAsV2(viaLegacy, legacyBytes),
        "legacy Clear must append");

  check(direct.payloadBytes() == viaLegacy.payloadBytes(),
        "direct and legacy Clear must produce the same payload size");
  check(direct.recordCount() == viaLegacy.recordCount(),
        "direct and legacy Clear must produce the same record count");
  check(direct.handleCount() == viaLegacy.handleCount(),
        "direct and legacy Clear must produce the same handle count");
}
```

Call it from `main()` alongside the existing cases.

- [ ] **Step 2: Run the test to verify it passes**

```sh
meson compile -C build dxmt9-pe-chunk-record-v2-value-spec
meson test -C build dxmt9-pe-chunk-record-v2-value-spec
```

Expected: PASS immediately — both paths already exist and this pins their
equivalence *before* the call sites move. This is the baseline the migration
must not break, not a red-then-green cycle.

If it fails, the two paths already differ and that is a pre-existing defect to
report before touching anything.

- [ ] **Step 3: Migrate the non-draw call sites**

For each non-draw record type — Clear, Present, StretchRect, ColorFill,
UpdateTexture, UpdateSurface, Readback, QueryIssue, and any sibling in
`appendLegacyCommandRecordAsV2`'s switch past `:914` — replace the
"build legacy struct, call `appendCommandRecord`" pair with a direct call to
the matching `append*V2` function.

Find them with:

```sh
grep -n "appendCommandRecord(\|appendCommandRecordRetained(" src/d3d9/d3d9_pe_device.cpp src/d3d9/d3d9_pe_device_child_misc.cpp
```

Each becomes, for example:

```cpp
// Before:
D9CCommandRecordClear rec{};
rec.header.type = D9C_COMMAND_RECORD_CLEAR;
rec.flags = flags; /* ... */
const HRESULT hr = appendCommandRecord(&rec, sizeof(rec));

// After:
const bool ok = dxmt9::d3d9::pe::appendClearV2(
    commandChunkV2_,
    D9CCommandChunkWireClearV2{
        .flags = flags,
        .colorARGB = colorARGB,
        .z = z,
        .stencil = stencil,
        .rectCount = 0u,
        .rectOffset = 0u,
    },
    std::span<const D9CRect>(rects, rectCount));
const HRESULT hr = ok ? S_OK : D3DERR_INVALIDCALL;
```

Keep the chunk-capacity precheck working: `appendCommandRecordDirect`'s
`bytes` argument fed the seal decision (`:9182-9184`). Where a call site no
longer goes through `appendCommandRecordDirect`, it must still consult the
same precheck with the record's V2 size before appending, so seal cadence does
not change. Extract that precheck into a small helper
(`bool shouldFlushBeforeAppend(std::size_t bytes) const`) and call it from
both the remaining legacy path and the new direct paths.

Then delete the migrated `case` labels from `appendLegacyCommandRecordAsV2`
in `d3d9_pe_chunk_v2_draw.cpp`.

- [ ] **Step 4: Build and test**

```sh
meson compile -C build && meson compile -C build-win32-x64-builtin && meson compile -C build-win32-x86-builtin
meson test -C build
```

Expected: all pass.

- [ ] **Step 5: Wine runtime check**

```sh
python3 scripts/run_apps/run_experiment.py run app-d3d9-3dmark05
```

Expected: `status: pass`, `gpu_command_buffer_errors = 0`, scene matches the
`v0.0.3` anchor. Clear and Present are on every frame, so a defect here is
immediately visible.

- [ ] **Step 6: Commit**

```sh
git add -A src/d3d9 tests/native/bridge
git commit -m "refactor(pe): emit non-draw records directly as V2

Clear, Present, StretchRect, ColorFill, UpdateTexture, UpdateSurface,
Readback, and QueryIssue call their V2 emitters directly instead of
building a legacy struct for appendLegacyCommandRecordAsV2 to convert.

Chunk seal cadence is preserved: the capacity precheck moves into
shouldFlushBeforeAppend() and every append path consults it.

Note: barrier sites still prepend a legacy APPLY_STATE until the
applystate migration, and object-ref acquisition moved from the
wire-handle cache to wrapper-side refs, changing the failure path and
the noteWireIdentityGetterCall counter."
```

---

## Task 5: Emit constant records directly as V2

**Files:**
- Modify: `src/d3d9/d3d9_pe_device.cpp` — the six VS/PS constant flush sites
- Modify: `src/d3d9/d3d9_pe_chunk_v2_draw.cpp:894-913` — remove the six constant `case` labels

**Interfaces:**
- Consumes: `shouldFlushBeforeAppend` (Task 4).
- Produces: no new symbols. Constant flushes call `appendSetConstantsV2(builder, type, startRegister, registerCount, registerBytes)` from `d3d9_pe_chunk_v2_builder.hpp:204-207`.

**Background:** GT2 issues `1,008` constant appends per present — the second
largest family by count. Each currently builds a `D9CCommandRecordSetConst`
plus a trailing payload, which the shim re-parses with `legacyRange` before
calling `appendSetConstantsV2` with the same four values.

- [ ] **Step 1: Write the failing test**

Add to `tests/native/bridge/pe_chunk_record_v2_value_spec.cpp`:

```cpp
// Task 5 regression: a constant range emitted directly must be byte-identical
// to one emitted through the legacy shim, for every one of the six kinds.
void directConstantsMatchLegacyConstants() {
  struct Case {
    std::uint32_t type;
    std::size_t elementSize;
    const char* name;
  };
  const Case cases[] = {
      {D9C_COMMAND_RECORD_SET_VS_CONST_F, 16u, "vs float"},
      {D9C_COMMAND_RECORD_SET_VS_CONST_I, 16u, "vs int"},
      {D9C_COMMAND_RECORD_SET_VS_CONST_B, 4u, "vs bool"},
      {D9C_COMMAND_RECORD_SET_PS_CONST_F, 16u, "ps float"},
      {D9C_COMMAND_RECORD_SET_PS_CONST_I, 16u, "ps int"},
      {D9C_COMMAND_RECORD_SET_PS_CONST_B, 4u, "ps bool"},
  };

  for (const Case& c : cases) {
    const std::uint32_t start = 3u;
    const std::uint32_t count = 5u;
    std::vector<std::byte> values(count * c.elementSize);
    for (std::size_t i = 0; i < values.size(); ++i) {
      values[i] = static_cast<std::byte>(i & 0xFFu);
    }

    dxmt9::d3d9::pe::CommandChunkV2Builder direct;
    check(dxmt9::d3d9::pe::appendSetConstantsV2(direct, c.type, start, count,
                                            values),
          std::string("direct constants must append: ") + c.name);

    D9CCommandRecordSetConst legacy{};
    legacy.header.type = c.type;
    legacy.start = start;
    legacy.count = count;
    legacy.header.size =
        static_cast<std::uint32_t>(sizeof(legacy) + values.size());
    std::vector<std::byte> legacyBytes(legacy.header.size);
    std::memcpy(legacyBytes.data(), &legacy, sizeof(legacy));
    std::memcpy(legacyBytes.data() + sizeof(legacy), values.data(),
                values.size());

    dxmt9::d3d9::pe::CommandChunkV2Builder viaLegacy;
    check(dxmt9::d3d9::pe::appendLegacyCommandRecordAsV2(viaLegacy, legacyBytes),
          std::string("legacy constants must append: ") + c.name);

    check(direct.payloadBytes() == viaLegacy.payloadBytes(),
          std::string("payload size must match: ") + c.name);
    check(direct.recordCount() == viaLegacy.recordCount(),
          std::string("record count must match: ") + c.name);
  }
}
```

Call it from `main()`.

- [ ] **Step 2: Run the test**

```sh
meson compile -C build dxmt9-pe-chunk-record-v2-value-spec
meson test -C build dxmt9-pe-chunk-record-v2-value-spec
```

Expected: PASS. As in Task 4 this pins existing equivalence before the move.

- [ ] **Step 3: Migrate the constant flush sites**

Find them:

```sh
grep -n "D9C_COMMAND_RECORD_SET_VS_CONST\|D9C_COMMAND_RECORD_SET_PS_CONST" src/d3d9/d3d9_pe_device.cpp
```

Each currently builds a `D9CCommandRecordSetConst` header plus payload and
calls `appendCommandRecordDirect`. Replace with:

```cpp
if (shouldFlushBeforeAppend(recordV2SizeForConstants(count, elementSize))) {
    const HRESULT flushHr = flushChunk();
    if (FAILED(flushHr)) return flushHr;
}
const bool ok = dxmt9::d3d9::pe::appendSetConstantsV2(
    commandChunkV2_, type, shadow.dirtyStart, count,
    std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(shadow.values.data()) +
            static_cast<std::size_t>(shadow.dirtyStart) * elementSize,
        static_cast<std::size_t>(count) * elementSize));
if (!ok) return D3DERR_INVALIDCALL;
shadow.clear();
```

`shadow.clear()` must stay after a successful append, matching the existing
`flushConstShadow` contract — the dirty range is consumed, not merely read.
Read `src/d3d9/d3d9_pe_const_shadow.hpp:207-240` (`foldConstShadowIntoDeltaSection`)
for how the existing code sequences drain-then-clear, and preserve that order.

Then delete the six constant `case` labels from `appendLegacyCommandRecordAsV2`
(`d3d9_pe_chunk_v2_draw.cpp:894-913`).

- [ ] **Step 4: Build and test**

```sh
meson compile -C build && meson compile -C build-win32-x64-builtin && meson compile -C build-win32-x86-builtin
meson test -C build
```

- [ ] **Step 5: Wine runtime check**

```sh
python3 scripts/run_apps/run_experiment.py run app-d3d9-3dmark05
```

Expected: `status: pass`, `gpu_command_buffer_errors = 0`, `v0.0.3` anchor
match. A constant-path defect shows as wrong transforms or wrong colours, not
as a crash — inspect the screenshot, do not trust exit status alone.

- [ ] **Step 6: Commit**

```sh
git add -A src/d3d9 tests/native/bridge
git commit -m "refactor(pe): emit constant records directly as V2

The six VS/PS constant flush sites call appendSetConstantsV2 with the
shadow's dirty range instead of building a D9CCommandRecordSetConst for
appendLegacyCommandRecordAsV2 to re-parse. Drain-then-clear ordering on
the const shadow is preserved."
```

---

## Task 6: The differential harness, and the sparse producer for applystate

**Files:**
- Create: `tests/native/bridge/pe_producer_differential_spec.cpp`
- Modify: `tests/native/bridge/meson.build`
- Modify: `src/d3d9/d3d9_pe_producer.hpp` / `.cpp` — add `buildSparseStateV2`
- Modify: `src/d3d9/d3d9_pe_device.cpp` — the `APPLY_STATE` site at `:10000`
- Modify: `src/d3d9/d3d9_pe_chunk_v2_draw.cpp` — remove the `APPLY_STATE` case

**Interfaces:**
- Consumes: everything from Tasks 1-3.
- Produces:
  ```cpp
  bool buildSparseStateV2(const PeHotStateShadow& shadow,
                          PeConstShadowBlock& constants,
                          const PeBindingView& bindings,
                          const PeChunkContext& chunk,
                          const PeDrawPayloads& payloads,
                          const PeDrawParams& params,
                          PeSparseScratch& scratch,
                          D9CCommandChunkWireDrawHeaderV2& header,
                          SparseStateV2Input& out) noexcept;
  ```
  This is the final signature; Tasks 7 and 8 add no parameters. APPLY_STATE
  passes a default-constructed `PeDrawParams` and ignores `header`, matching
  `appendApplyStateV2`, which takes no header. Tasks 7 and 8 populate both.
  `constants` is non-const: like `foldConstShadowIntoDeltaSection`, the
  producer drains dirty ranges.

**The design's step 2a ("harness first") is honored inside this task rather
than as a separate one.** A harness committed alone would sit red across Tasks
4 and 5, breaking their gate. Here it is written first, confirmed failing
against a stub, then made to pass — same ordering, no red period.

- [ ] **Step 1: Write the failing differential test**

Create `tests/native/bridge/pe_producer_differential_spec.cpp`:

```cpp
// pe_producer_differential_spec
//
// Runs the real old and new producers over one corpus and requires that they
// agree on the emitted V2 chunk bytes AND on the builder side effects that
// bytes do not capture: retained objects, record/handle/payload counts, and
// return value.
//
// Both lanes call functions from src/. Nothing here mirrors production logic
// -- that is the whole reason Task 3 rehosted the producer natively. See
// docs/superpowers/specs/2026-07-29-pe-legacy-record-removal-design.md §6.

#include "d3d9_pe_chunk_v2_builder.hpp"
#include "d3d9_pe_producer.hpp"
#include "d3d9_pe_producer_views.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

struct RefCounter { std::uint32_t refs = 1u; };
struct D9CSurface : RefCounter {};
struct D9CTexture : RefCounter {};
struct D9CBuffer : RefCounter {};
struct D9CShader : RefCounter {};
struct D9CVertexDecl : RefCounter {};
struct D9CQuery : RefCounter {};

// The C-ABI retain/release stubs the builder links against. Copy the full set
// from tests/native/bridge/pe_chunk_record_v2_value_spec.cpp:17-60 -- that
// file already defines exactly these for the same builder.

struct TestFailure : std::runtime_error {
  explicit TestFailure(std::string message)
      : std::runtime_error(std::move(message)) {}
};

void check(bool condition, std::string_view message) {
  if (!condition) throw TestFailure(std::string(message));
}

namespace pe = dxmt9::d3d9::pe;

// One corpus entry: the complete input state for a single record.
struct Fixture {
  std::string name;
  PeHotStateShadow shadow{};
  PeConstShadowBlock constants{};
  pe::PeBindingView bindings{};
  pe::PeChunkContext chunk{};
  pe::PeDrawPayloads payloads{};
  pe::PeDrawParams params{};          // Task 7/8 fill this; Task 6 leaves it default
  bool forceFullSnapshot = false;
};

// What a lane produced, beyond the bytes.
struct LaneResult {
  bool ok = false;
  std::vector<std::byte> bytes;
  std::size_t recordCount = 0;
  std::size_t handleCount = 0;
  std::size_t retainedObjectCount = 0;
};

LaneResult runLegacyLane(const Fixture& fixture) {
  LaneResult result;
  dxmt9::d3d9::pe::CommandChunkV2Builder builder;
  PeConstShadowBlock constants = fixture.constants;  // lanes must not share
  D9CDrawPrimitivePacket packet;
  PeDecimatedScopeStats stats{};
  result.ok = pe::buildDrawPacketFromViews(
      fixture.shadow, fixture.bindings, fixture.params.primitiveType,
      fixture.params.startVertex, fixture.params.primitiveCount,
      fixture.forceFullSnapshot, stats, packet);
  if (!result.ok) return result;
  // Serialize the packet as a legacy APPLY_STATE record and convert it, which
  // is exactly what d3d9_pe_device.cpp does today.
  D9CCommandRecordApplyState record{};
  record.header.type = D9C_COMMAND_RECORD_APPLY_STATE;
  record.packet = packet;
  record.header.size = sizeof(record);
  std::vector<std::byte> recordBytes(sizeof(record));
  std::memcpy(recordBytes.data(), &record, sizeof(record));
  result.ok = dxmt9::d3d9::pe::appendLegacyCommandRecordAsV2(builder, recordBytes);
  result.recordCount = builder.recordCount();
  result.handleCount = builder.handleCount();
  result.retainedObjectCount = builder.retainedObjectCount();
  const auto sealed = builder.seal();
  result.bytes.assign(sealed.blob.begin(), sealed.blob.end());
  return result;
}

LaneResult runDirectLane(const Fixture& fixture) {
  LaneResult result;
  dxmt9::d3d9::pe::CommandChunkV2Builder builder;
  PeConstShadowBlock constants = fixture.constants;
  pe::PeSparseScratch scratch{};
  dxmt9::d3d9::pe::SparseStateV2Input state{};
  D9CCommandChunkWireDrawHeaderV2 header{};
  result.ok = pe::buildSparseStateV2(fixture.shadow, constants,
                                     fixture.bindings, fixture.chunk,
                                     fixture.payloads, fixture.params, scratch,
                                     header, state);
  if (!result.ok) return result;
  result.ok = dxmt9::d3d9::pe::appendApplyStateV2(builder, /*flags=*/0u, state);
  result.recordCount = builder.recordCount();
  result.handleCount = builder.handleCount();
  result.retainedObjectCount = builder.retainedObjectCount();
  const auto sealed = builder.seal();
  result.bytes.assign(sealed.blob.begin(), sealed.blob.end());
  return result;
}

void requireLanesAgree(const Fixture& fixture) {
  const LaneResult legacy = runLegacyLane(fixture);
  const LaneResult direct = runDirectLane(fixture);

  check(legacy.ok == direct.ok,
        fixture.name + ": lanes must agree on success/failure");
  if (!legacy.ok) return;   // both failed; failure reasons may differ

  check(legacy.bytes.size() == direct.bytes.size(),
        fixture.name + ": chunk byte length must match");
  check(std::memcmp(legacy.bytes.data(), direct.bytes.data(),
                    legacy.bytes.size()) == 0,
        fixture.name + ": chunk bytes must be identical");
  check(legacy.recordCount == direct.recordCount,
        fixture.name + ": record count must match");
  check(legacy.handleCount == direct.handleCount,
        fixture.name + ": handle count must match");
  check(legacy.retainedObjectCount == direct.retainedObjectCount,
        fixture.name + ": retained object count must match");
}

// --- Corpus part 1: deterministic sequences ---------------------------------

D9CBuffer vb0, vb1;
D9CTexture tex0;
D9CShader vsObj, psObj;

pe::PeWireObjectRef refTo(void* object, std::uint32_t kind,
                          std::uint64_t objectId) {
  pe::PeWireObjectRef ref{};
  ref.object = object;
  ref.identity.kind = kind;
  ref.identity.generation = 1u;
  ref.identity.objectId = objectId;
  return ref;
}

void emptyDelta() {
  Fixture f;
  f.name = "empty delta";
  requireLanesAgree(f);
}

void singleCategoryDirty() {
  Fixture f;
  f.name = "single category dirty: one render state";
  f.shadow.pendingRenderStates.set(/*slot=*/7u, /*value=*/1u);
  requireLanesAgree(f);
}

void everyCategoryDirty() {
  Fixture f;
  f.name = "every category dirty";
  f.shadow.pendingRenderStates.set(7u, 1u);
  f.shadow.pendingTextureMask = 0x1u;
  f.shadow.pendingStreamMask = 0x1u;
  f.shadow.pendingVs = true;
  f.shadow.pendingPs = true;
  f.shadow.pendingViewport = true;
  f.shadow.pendingScissor = true;
  f.shadow.pendingMaterial = true;
  f.bindings.textures[0] = refTo(&tex0, D9C_CHUNK_HANDLE_KIND_TEXTURE, 1u);
  f.bindings.streams[0].buffer =
      refTo(&vb0, D9C_CHUNK_HANDLE_KIND_BUFFER, 2u);
  f.bindings.vs = refTo(&vsObj, D9C_CHUNK_HANDLE_KIND_SHADER, 3u);
  f.bindings.ps = refTo(&psObj, D9C_CHUNK_HANDLE_KIND_SHADER, 4u);
  requireLanesAgree(f);
}

void everySectionAtCap() {
  Fixture f;
  f.name = "every section at its cap";
  for (std::uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_RENDER_STATES;
       ++slot) {
    f.shadow.pendingRenderStates.set(slot, slot + 1u);
  }
  requireLanesAgree(f);
}

void overCapMustFailOnBothLanes() {
  Fixture f;
  f.name = "render states over cap";
  for (std::uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_RENDER_STATES + 1u;
       ++slot) {
    f.shadow.pendingRenderStates.set(slot, slot + 1u);
  }
  const LaneResult legacy = runLegacyLane(f);
  const LaneResult direct = runDirectLane(f);
  check(!legacy.ok, "over-cap must fail on the legacy lane");
  check(!direct.ok, "over-cap must fail on the direct lane");
}

// The FULL_SNAPSHOT draw flag is derived from all-ones texture and stream
// masks in the shim being deleted (d3d9_pe_chunk_v2_draw.cpp:867-873). Only
// an all-dirty case exercises it, and the new producer must own the heuristic.
void allSlotsDirtyTriggersSnapshotFlag() {
  Fixture f;
  f.name = "all texture and stream slots dirty (snapshot flag heuristic)";
  f.shadow.pendingTextureMask = (1u << D9C_DRAW_PACKET_MAX_TEXTURES) - 1u;
  f.shadow.pendingStreamMask = (1u << D9C_DRAW_PACKET_MAX_STREAMS) - 1u;
  for (std::uint32_t i = 0; i < D9C_DRAW_PACKET_MAX_TEXTURES; ++i) {
    f.bindings.textures[i] =
        refTo(&tex0, D9C_CHUNK_HANDLE_KIND_TEXTURE, 100u + i);
  }
  for (std::uint32_t i = 0; i < D9C_DRAW_PACKET_MAX_STREAMS; ++i) {
    f.bindings.streams[i].buffer =
        refTo(&vb0, D9C_CHUNK_HANDLE_KIND_BUFFER, 200u + i);
  }
  requireLanesAgree(f);
}

// --- Corpus part 2: chunk-context sequences ---------------------------------
//
// These are the §3(d) cases. They exist only because PeChunkContext is a
// parameter; a producer that called CommandChunkV2Builder::referencesObject
// directly could not be driven this way.

void streamDirtyButAlreadyRetained() {
  Fixture f;
  f.name = "stream 0 dirty and already retained by the chunk";
  f.shadow.pendingStreamMask = 0x1u;
  f.bindings.streams[0].buffer =
      refTo(&vb0, D9C_CHUNK_HANDLE_KIND_BUFFER, 2u);
  f.chunk.retainedStreamMask = 0x1u;
  requireLanesAgree(f);
}

void streamRetainedButNotDirty() {
  Fixture f;
  f.name = "stream 0 retained but not dirty";
  f.bindings.streams[0].buffer =
      refTo(&vb0, D9C_CHUNK_HANDLE_KIND_BUFFER, 2u);
  f.chunk.retainedStreamMask = 0x1u;
  requireLanesAgree(f);
}

void streamNeitherDirtyNorRetained() {
  Fixture f;
  f.name = "stream 0 bound but neither dirty nor retained -- must re-emit";
  f.bindings.streams[0].buffer =
      refTo(&vb0, D9C_CHUNK_HANDLE_KIND_BUFFER, 2u);
  f.chunk.retainedStreamMask = 0u;
  requireLanesAgree(f);
}

void indexBufferKnownAndUnchanged() {
  Fixture f;
  f.name = "index buffer known and unchanged -- must not re-emit";
  f.bindings.indexBuffer = refTo(&vb1, D9C_CHUNK_HANDLE_KIND_BUFFER, 9u);
  f.chunk.indexBufferKnown = true;
  f.chunk.submittedIndexBufferWire = 9u;
  requireLanesAgree(f);
}

void indexBufferKnownButChanged() {
  Fixture f;
  f.name = "index buffer known but changed -- must re-emit";
  f.bindings.indexBuffer = refTo(&vb1, D9C_CHUNK_HANDLE_KIND_BUFFER, 9u);
  f.chunk.indexBufferKnown = true;
  f.chunk.submittedIndexBufferWire = 8u;
  requireLanesAgree(f);
}

void indexBufferNotKnown() {
  Fixture f;
  f.name = "index buffer not yet known to the chunk -- must emit";
  f.bindings.indexBuffer = refTo(&vb1, D9C_CHUNK_HANDLE_KIND_BUFFER, 9u);
  f.chunk.indexBufferKnown = false;
  requireLanesAgree(f);
}

// --- Corpus part 3: fixed-seed randomized sequences -------------------------

void randomizedSequences() {
  std::mt19937 rng(0xD9C0DEu);   // pinned seed: CI must be deterministic
  for (int iteration = 0; iteration < 256; ++iteration) {
    Fixture f;
    f.name = "randomized iteration " + std::to_string(iteration);
    const std::uint32_t stateCount = rng() % D9C_DRAW_PACKET_MAX_RENDER_STATES;
    for (std::uint32_t i = 0; i < stateCount; ++i) {
      f.shadow.pendingRenderStates.set(rng() % D9C_DRAW_PACKET_MAX_RENDER_STATES,
                                       rng());
    }
    f.shadow.pendingTextureMask =
        rng() & ((1u << D9C_DRAW_PACKET_MAX_TEXTURES) - 1u);
    f.shadow.pendingStreamMask =
        rng() & ((1u << D9C_DRAW_PACKET_MAX_STREAMS) - 1u);
    f.shadow.pendingVs = (rng() & 1u) != 0u;
    f.shadow.pendingPs = (rng() & 1u) != 0u;
    f.chunk.retainedStreamMask =
        rng() & ((1u << D9C_DRAW_PACKET_MAX_STREAMS) - 1u);
    f.chunk.indexBufferKnown = (rng() & 1u) != 0u;
    f.chunk.submittedIndexBufferWire = rng() % 4u;
    for (std::uint32_t i = 0; i < D9C_DRAW_PACKET_MAX_TEXTURES; ++i) {
      f.bindings.textures[i] =
          refTo(&tex0, D9C_CHUNK_HANDLE_KIND_TEXTURE, 300u + i);
    }
    for (std::uint32_t i = 0; i < D9C_DRAW_PACKET_MAX_STREAMS; ++i) {
      f.bindings.streams[i].buffer =
          refTo(&vb0, D9C_CHUNK_HANDLE_KIND_BUFFER, 400u + i);
      f.bindings.streams[i].offset = rng() % 1024u;
      f.bindings.streams[i].stride = 4u * (1u + rng() % 16u);
    }
    f.bindings.vs = refTo(&vsObj, D9C_CHUNK_HANDLE_KIND_SHADER, 3u);
    f.bindings.ps = refTo(&psObj, D9C_CHUNK_HANDLE_KIND_SHADER, 4u);
    requireLanesAgree(f);
  }
}

int main() {
  try {
    emptyDelta();
    singleCategoryDirty();
    everyCategoryDirty();
    everySectionAtCap();
    overCapMustFailOnBothLanes();
    allSlotsDirtyTriggersSnapshotFlag();
    streamDirtyButAlreadyRetained();
    streamRetainedButNotDirty();
    streamNeitherDirtyNorRetained();
    indexBufferKnownAndUnchanged();
    indexBufferKnownButChanged();
    indexBufferNotKnown();
    randomizedSequences();
  } catch (const TestFailure& failure) {
    std::cerr << "pe_producer_differential_spec FAILED: " << failure.what()
              << "\n";
    return 1;
  }
  std::cout << "pe_producer_differential_spec OK\n";
  return 0;
}
```

Append to `tests/native/bridge/meson.build`:

```meson
pe_producer_differential_spec = executable(
  'dxmt9-pe-producer-differential-spec',
  [
    'pe_producer_differential_spec.cpp',
    '../../../src/d3d9/d3d9_pe_producer.cpp',
    '../../../src/d3d9/d3d9_pe_chunk_v2_builder.cpp',
    '../../../src/d3d9/d3d9_pe_chunk_v2_draw.cpp',
    '../../../src/d3d9/d3d9_pe_chunk_v2_nondraw.cpp',
  ],
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src/d3d9'),
  ],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-pe-producer-differential-spec',
     pe_producer_differential_spec,
     env: dxmt9_native_test_env)
```

- [ ] **Step 2: Add a stub and confirm the test fails**

Add to `src/d3d9/d3d9_pe_producer.hpp` the `buildSparseStateV2` declaration
from this task's Interfaces block, and to `.cpp` a stub:

```cpp
bool buildSparseStateV2(const PeHotStateShadow&, PeConstShadowBlock&,
                        const PeBindingView&, const PeChunkContext&,
                        const PeDrawPayloads&, const PeDrawParams&,
                        PeSparseScratch&, D9CCommandChunkWireDrawHeaderV2&,
                        SparseStateV2Input&) noexcept {
  return false;   // Task 6 stub -- the differential must fail here.
}
```

```sh
meson compile -C build dxmt9-pe-producer-differential-spec
meson test -C build dxmt9-pe-producer-differential-spec
```

Expected: FAIL on the first fixture with "lanes must agree on
success/failure". **If it passes, stop** — the harness is not exercising the
new lane, which is the exact defect class this design exists to avoid.

Both lanes read `recordCount()` / `handleCount()` / `retainedObjectCount()`
*before* calling `seal()`, then copy `SealedCommandChunkV2::blob`
(`d3d9_pe_chunk_v2_builder.hpp:60-66`) into an owned vector — the span points
into the builder, which is a lane-local and dies at return.

- [ ] **Step 3: Implement `buildSparseStateV2`**

Fill each `SparseStateV2Input` category by walking the corresponding pending
mask or table in ascending order, writing compact entries into `scratch`, and
pointing the output span at the written prefix. For example:

```cpp
std::size_t renderStateCount = 0;
shadow.pendingRenderStates.forEach([&](std::uint32_t state,
                                       std::uint32_t value) {
  if (renderStateCount >= scratch.renderStates.size()) return;
  scratch.renderStates[renderStateCount++] =
      D9CCommandChunkWireRenderStateV2{.state = state, .value = value};
});
if (shadow.pendingRenderStates.size() > scratch.renderStates.size()) {
  return false;   // over cap: seal the chunk rather than truncate
}
out.renderStates = std::span<const D9CCommandChunkWireRenderStateV2>(
    scratch.renderStates.data(), renderStateCount);
```

`FixedStateTable::forEach` iterates slots ascending, which satisfies
`orderedSlot`'s strict-ascending requirement (`d3d9_pe_chunk_v2_draw.cpp:190`).
Every mask walk must iterate slot 0 upward for the same reason.

For streams, the emitted set is the union of dirty and not-yet-retained:

```cpp
const std::uint32_t emitStreamMask =
    shadow.pendingStreamMask |
    (boundStreamMask(bindings) & ~chunk.retainedStreamMask);
```

For the index buffer, reproduce `d3d9_pe_device.cpp:9404-9412`:

```cpp
const std::uint64_t ibWire = bindings.indexBuffer.identity.objectId;
const bool emitIndexBuffer = shadow.pendingIb || !chunk.indexBufferKnown ||
                             chunk.submittedIndexBufferWire != ibWire;
```

For constants, drain each of the six shadows in `constants` the way
`foldConstShadowIntoDeltaSection` does (`d3d9_pe_const_shadow.hpp:207-240`):
read `dirtyStart` / `dirtyEnd`, point the span at
`values.data() + dirtyStart * elemSize`, then `clear()`. Because the span
points into the shadow's own storage, the shadow must outlive the
`appendSparseRecordV2` call — it does, being device-owned.

For UP payloads, forward `payloads.upIndex` and `payloads.upVertex` unchanged.

Port the snapshot-flag heuristic explicitly rather than letting it fall out of
mask arithmetic: set the draw header's `FULL_SNAPSHOT` flag when both the
texture and stream masks are all-ones, matching
`d3d9_pe_chunk_v2_draw.cpp:867-873`.

- [ ] **Step 4: Run the differential to verify it passes**

```sh
meson compile -C build dxmt9-pe-producer-differential-spec
meson test -C build dxmt9-pe-producer-differential-spec
```

Expected: PASS on all 12 named fixtures plus 256 randomized iterations.

When a fixture fails, the message names it — fix the producer, not the
fixture. Changing a fixture to match the new lane's output defeats the
differential. The one legitimate reason to change a fixture is if it encodes
a state the device cannot actually produce; record that reasoning in the task
report.

- [ ] **Step 5: Migrate the APPLY_STATE call site**

At `d3d9_pe_device.cpp:10000`, replace the
`buildDrawPacketFromViews` + `appendCommandRecord` pair with
`buildSparseStateV2` + `appendApplyStateV2`, filling `PeChunkContext` from the
builder:

```cpp
dxmt9::d3d9::pe::PeChunkContext chunk{};
chunk.retainedStreamMask = retainedStreamMaskForChunk();
chunk.indexBufferKnown = submittedIndexBufferKnown_;
chunk.submittedIndexBufferWire = submittedIndexBufferWireValue_;
```

`retainedStreamMaskForChunk()` is the loop currently inside
`populatePendingChunkDrawStreamDependencies` (`:9462-9477`), lifted to its own
method so both the draw and applystate sites use it. It must still be
evaluated **after** any capacity flush has resealed the chunk — read the
comment at `:9471-9475` and preserve that ordering.

Then delete the `D9C_COMMAND_RECORD_APPLY_STATE` case from
`appendLegacyCommandRecordAsV2`.

- [ ] **Step 6: Build, test, and run Wine**

```sh
meson compile -C build && meson compile -C build-win32-x64-builtin && meson compile -C build-win32-x86-builtin
meson test -C build
python3 scripts/run_apps/run_experiment.py run app-d3d9-3dmark05
```

Expected: all tests pass; `status: pass`, `gpu_command_buffer_errors = 0`,
`v0.0.3` anchor match. APPLY_STATE precedes every barrier, so a defect shows
as state leaking across Clear/StretchRect boundaries.

- [ ] **Step 7: Commit**

```sh
git add -A src/d3d9 tests/native/bridge
git commit -m "feat(pe): sparse-state producer, and applystate through it

buildSparseStateV2 fills SparseStateV2Input directly from the state and
const shadows, the binding view, and the destination chunk's retention
context, with no fat packet in between. APPLY_STATE is the first call
site to use it.

Gated by dxmt9-pe-producer-differential-spec, which runs the real old
and new producers over 12 named fixtures plus 256 fixed-seed randomized
ones and compares emitted chunk bytes, record/handle counts, retained
object count, and return value. Confirmed failing against a stub before
the producer existed."
```

---

## Task 7: Migrate `draw` and `drawidx`

**Files:**
- Modify: `src/d3d9/d3d9_pe_device.cpp:9329`, `:9389`
- Modify: `tests/native/bridge/pe_producer_differential_spec.cpp` — add draw-header fixtures
- Modify: `src/d3d9/d3d9_pe_chunk_v2_draw.cpp` — remove the two draw cases

**Interfaces:**
- Consumes: `buildSparseStateV2` (Task 6), `retainedStreamMaskForChunk()` (Task 6).
- Produces: no new symbols.

**Background:** `1,694` of GT2's `2,720` appends per present. This is where the round trip actually cost something.

- [ ] **Step 1: Extend the differential with draw-header fixtures**

Add to `pe_producer_differential_spec.cpp`, and generalize
`runLegacyLane`/`runDirectLane` to take a record type so they emit
`appendSparseRecordV2` with a draw header rather than `appendApplyStateV2`:

```cpp
void indexedDrawWithBaseVertex() {
  Fixture f;
  f.name = "indexed draw, base vertex and index range";
  f.params.recordType = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
  f.params.primitiveType = 4u;          // D3DPT_TRIANGLELIST
  f.params.primitiveCount = 128u;
  f.params.baseVertex = 64;
  f.params.minVertex = 8u;
  f.params.numVertices = 256u;
  f.params.startIndex = 12u;
  f.bindings.indexBuffer = refTo(&vb1, D9C_CHUNK_HANDLE_KIND_BUFFER, 9u);
  f.shadow.pendingIb = true;
  requireLanesAgree(f);
}

void nonIndexedDraw() {
  Fixture f;
  f.name = "non-indexed draw";
  f.params.recordType = D9C_COMMAND_RECORD_DRAW_PRIMITIVE;
  f.params.primitiveType = 5u;          // D3DPT_TRIANGLESTRIP
  f.params.startVertex = 32u;
  f.params.primitiveCount = 64u;
  f.shadow.pendingStreamMask = 0x1u;
  f.bindings.streams[0].buffer =
      refTo(&vb0, D9C_CHUNK_HANDLE_KIND_BUFFER, 2u);
  f.bindings.streams[0].stride = 32u;
  requireLanesAgree(f);
}

void indexedDrawNegativeBaseVertex() {
  Fixture f;
  f.name = "indexed draw, negative base vertex";
  f.params.recordType = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
  f.params.baseVertex = -32;            // int32_t in D9CCommandChunkWireDrawHeaderV2
  f.params.primitiveCount = 16u;
  f.bindings.indexBuffer = refTo(&vb1, D9C_CHUNK_HANDLE_KIND_BUFFER, 9u);
  f.shadow.pendingIb = true;
  requireLanesAgree(f);
}
```

`Fixture::params` already carries every one of these from Task 6, defaulting
to the applystate shape, so existing fixtures keep working unchanged. The
`randomizedSequences` loop must also pick `params.recordType` at random from
the three record types migrated so far.

- [ ] **Step 2: Run the differential to verify the new fixtures fail**

```sh
meson compile -C build dxmt9-pe-producer-differential-spec
meson test -C build dxmt9-pe-producer-differential-spec
```

Expected: FAIL on `indexedDrawWithBaseVertex` — `buildSparseStateV2` does not
yet fill a draw header. If it passes, the draw-header path is not being
exercised; fix the harness before touching the producer.

- [ ] **Step 3: Extend the producer with draw-header output**

`buildSparseStateV2` already takes `const PeDrawParams& params` and
`D9CCommandChunkWireDrawHeaderV2& header` from Task 6 — Task 6 left them
unread because APPLY_STATE carries no draw header. This step reads them. For
reference, the signature is unchanged:

```cpp
bool buildSparseStateV2(const PeHotStateShadow& shadow,
                        PeConstShadowBlock& constants,
                        const PeBindingView& bindings,
                        const PeChunkContext& chunk,
                        const PeDrawPayloads& payloads,
                        const PeDrawParams& params,
                        PeSparseScratch& scratch,
                        D9CCommandChunkWireDrawHeaderV2& header,
                        SparseStateV2Input& out) noexcept;
```

Populate the header from `params`:

```cpp
header.flags = snapshotFlag;   // from the all-ones mask heuristic
header.primitiveType = primitiveType;
header.baseVertex = baseVertex;
header.minVertex = minVertex;
header.numVertices = numVertices;
header.startVertex = startVertex;
header.startIndex = startIndex;
header.primitiveCount = primitiveCount;
header.stride = stride;
header.indexFormat = indexFormat;
// sectionCount / sectionTableOffset / sectionPayloadOffset are filled by
// appendSparseRecordV2, not here. Leave them zero.
```

Field names and types are from `include/dxmt9/device_c.h:801-816`.

- [ ] **Step 4: Run the differential to verify it passes**

```sh
meson test -C build dxmt9-pe-producer-differential-spec
```

Expected: PASS on all fixtures.

- [ ] **Step 5: Migrate the two call sites**

At `d3d9_pe_device.cpp:9329` (`drawidx`) and `:9389` (`draw`), replace the
`buildDrawPrimitivePacket` + `foldPendingConstsIntoDrawPacket` +
`appendCommandRecordDirect` sequence with `buildSparseStateV2` +
`appendSparseRecordV2`. The constant folding disappears — `buildSparseStateV2`
drains the const shadows itself.

Preserve two orderings that the old code got from where its calls sat:

1. `PeChunkContext` is filled **after** any capacity flush, exactly as
   `populatePendingChunkDrawStreamDependencies` ran inside the writer lambda
   (`:9453-9460`).
2. `submittedIndexBufferKnown_` / `submittedIndexBufferWireValue_` update only
   on success (`:9443-9449`), and pending bits clear only on success.

- [ ] **Step 6: Build, test, and run Wine**

```sh
meson compile -C build && meson compile -C build-win32-x64-builtin && meson compile -C build-win32-x86-builtin
meson test -C build
python3 scripts/run_apps/run_experiment.py run app-d3d9-3dmark05
python3 scripts/run_apps/run_experiment.py run app-d3d9-sfiv-benchmark
```

Expected: all tests pass; both runs `status: pass`,
`gpu_command_buffer_errors = 0`, `v0.0.3` anchor match. This is the highest-risk
migration — inspect the screenshots carefully for missing geometry and for
state leaking between draws.

- [ ] **Step 7: Commit**

```sh
git add -A src/d3d9 tests/native/bridge
git commit -m "refactor(pe): emit draw and drawidx through the sparse producer

The two hottest record types (1,694 of GT2's 2,720 appends per present)
now go peState_ -> SparseStateV2Input -> V2 sections, with no fat packet
and no legacy scratch round trip. Constant folding moves into
buildSparseStateV2, which drains the const shadows directly.

Differential extended with indexed/non-indexed/negative-base-vertex
draw-header fixtures, confirmed failing before the header path existed."
```

---

## Task 8: Migrate `drawUP` and `drawidxUP`

**Files:**
- Modify: `src/d3d9/d3d9_pe_device.cpp:9527`, `:9627`
- Modify: `tests/native/bridge/pe_producer_differential_spec.cpp` — add UP payload fixtures
- Modify: `src/d3d9/d3d9_pe_chunk_v2_draw.cpp` — remove the two UP cases and the now-empty `appendLegacySparseRecord`

**Interfaces:**
- Consumes: `buildSparseStateV2` with draw-header output (Task 7).
- Produces: no new symbols. After this task `appendLegacySparseRecord` has no callers.

**Background:** Rare on GT2 (`~0` appends per present) but they carry the
inline vertex/index payloads and the save/restore dance at `:9511-9526` that
Task 3 had to preserve. Low frequency, highest per-site subtlety.

- [ ] **Step 1: Add UP payload fixtures**

```cpp
void nonIndexedUpDraw() {
  Fixture f;
  f.name = "draw primitive UP with inline vertices";
  f.params.recordType = D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP;
  f.params.primitiveType = 4u;
  f.params.primitiveCount = 2u;
  f.params.stride = 16u;
  static std::array<std::byte, 96> vertices{};
  for (std::size_t i = 0; i < vertices.size(); ++i) {
    vertices[i] = static_cast<std::byte>(i & 0xFFu);
  }
  f.payloads.upVertex = std::span<const std::byte>(vertices);
  requireLanesAgree(f);
}

void indexedUpDraw() {
  Fixture f;
  f.name = "draw indexed primitive UP with inline indices and vertices";
  f.params.recordType = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP;
  f.params.primitiveType = 4u;
  f.params.primitiveCount = 2u;
  f.params.stride = 16u;
  f.params.indexFormat = 101u;          // D3DFMT_INDEX16
  f.params.minVertex = 0u;
  f.params.numVertices = 6u;
  static std::array<std::byte, 12> indices{};
  static std::array<std::byte, 96> vertices{};
  for (std::size_t i = 0; i < indices.size(); ++i) {
    indices[i] = static_cast<std::byte>(i & 0xFFu);
  }
  for (std::size_t i = 0; i < vertices.size(); ++i) {
    vertices[i] = static_cast<std::byte>((i * 3u) & 0xFFu);
  }
  f.payloads.upIndex = std::span<const std::byte>(indices);
  f.payloads.upVertex = std::span<const std::byte>(vertices);
  requireLanesAgree(f);
}

void upDrawWithEmptyPayload() {
  Fixture f;
  f.name = "draw primitive UP with zero vertex bytes";
  f.params.recordType = D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP;
  f.params.primitiveCount = 0u;
  f.params.stride = 16u;
  requireLanesAgree(f);
}
```

Call all three from `main()` and add the two UP types to the randomized
loop's record-type pool.

- [ ] **Step 2: Run the differential to verify the new fixtures fail**

```sh
meson test -C build dxmt9-pe-producer-differential-spec
```

Expected: FAIL on `nonIndexedUpDraw` — the payload spans are not yet forwarded
into the emitted record.

- [ ] **Step 3: Forward the UP payloads**

In `buildSparseStateV2`, set `out.upIndexData = payloads.upIndex` and
`out.upVertexData = payloads.upVertex`. `appendSparseRecordV2` already knows
how to append them for the two UP record types — read
`d3d9_pe_chunk_v2_draw.cpp:202-442` to confirm the offsets it computes, and do
not duplicate that logic in the producer.

- [ ] **Step 4: Run the differential to verify it passes**

```sh
meson test -C build dxmt9-pe-producer-differential-spec
```

Expected: PASS.

- [ ] **Step 5: Migrate the two call sites**

At `:9527` and `:9627`, replace the packet-build-and-append sequence with
`buildSparseStateV2` + `appendSparseRecordV2`, passing the UP data through
`PeDrawPayloads`.

**The save/restore window at `:9511-9526` still applies.** The call site
temporarily mutates `fvf_` / `vdecl_` / `vs_` and three pending bits, calls
the producer, then restores them. `populateBindingView` must run inside that
window so the view snapshots the temporary values. Read `:9505-9535` and place
the fill accordingly — this is the same constraint Task 3 Step 3 flagged, and
it is easy to break when the surrounding code changes shape.

Then delete the two UP cases from `appendLegacyCommandRecordAsV2` and, since
its switch is now empty, delete `appendLegacySparseRecord` and
`populateLegacySparseState` entirely.

- [ ] **Step 6: Build, test, and run Wine**

```sh
meson compile -C build && meson compile -C build-win32-x64-builtin && meson compile -C build-win32-x86-builtin
meson test -C build
python3 scripts/run_apps/run_experiment.py run app-d3d9-3dmark05
python3 scripts/run_apps/run_experiment.py run app-d3d9-sfiv-benchmark
```

Expected: all pass. UP draws are rare on both workloads, so a green run is
weak evidence here — the differential fixtures are the real gate for this task.
Say so in the task report rather than implying the Wine runs proved the UP path.

- [ ] **Step 7: Commit**

```sh
git add -A src/d3d9 tests/native/bridge
git commit -m "refactor(pe): emit UP draws through the sparse producer

drawUP and drawidxUP forward their inline index/vertex payloads through
PeDrawPayloads. appendLegacySparseRecord and populateLegacySparseState
now have no callers and are deleted.

The fvf_/vdecl_/vs_ save-restore window around the UP call sites is
preserved: the binding view is filled inside it, so it snapshots the
temporarily-mutated values as the old inline read did.

UP draws are rare on GT1/SFIV, so the differential fixtures rather than
the Wine runs are this task's real gate."
```

---

## Task 9: Delete the legacy format and the dead fat-packet ABI

**Files:**
- Modify: `include/dxmt9/device_c.h` — remove the legacy record structs, `D9CDrawPrimitivePacket` and its relatives, and the two bridge op declarations
- Delete/trim: `src/d3d9/d3d9_pe_draw_packet.hpp`
- Modify: `src/d3d9/d3d9_pe_chunk_v2_draw.cpp` — remove `appendLegacyCommandRecordAsV2`, `loadLegacy`, `legacyRange`
- Modify: `src/d3d9/d3d9_pe_device.cpp` — remove `appendCommandRecord`, `appendCommandRecordRetained`, and the `build`/`encode` phases of `appendCommandRecordDirect`
- Modify: `src/d3d9/device_c_chunk_replay.cpp` — remove `applyDrawPacketState*`, `commitChunkDrawDeltaMask`, `validateDrawPacketStateDelta`, and the two bridge op implementations
- Modify: `src/d3d9/device_c_record_utils.hpp`, `device_c_record_replay.cpp`, `device_c_record_hazard.cpp`, `device_c_record_validate.cpp` — surgical symbol removal
- Modify: `src/d3d9/d3d9_pe_device_child_misc.cpp:220` — `D9CCommandRecordQueryIssue` use
- Modify: `tests/native/bridge/bridge_ops_spec.cpp:48`, `:103` — opcode count and adjacency assertions
- Delete: `tests/native/bridge/pe_full_snapshot_equivalence_spec.cpp` — or rewrite; see Step 4

**Interfaces:**
- Consumes: everything. This task runs only after Tasks 4-8 are all merged.
- Produces: nothing. It removes.

- [ ] **Step 1: Re-verify every "zero reference" claim**

The design's survey is a starting point, not authority. Re-run each check
immediately before deleting:

```sh
for sym in validateCommandRecord importedRecordIsDrawRunCandidate \
           drawPacketStateDeltaEquals collectDrawPacketResourceHazards \
           packetHasNoStateDelta makeRunParam applyDrawPacketState \
           dxmt9c_device_draw_primitive_packet dxmt9c_device_draw_primitive_chunk; do
  echo "=== $sym ==="
  grep -rn "\b$sym\b" src/ tests/ include/ scripts/ | grep -v "device_c_record_"
done
```

`replayInfoForCommandRecordType` (`device_c_record_replay.cpp:368`) is **live**
from `device_c_chunk_replay.cpp:1506`. `device_c_record_replay.cpp` must not
be deleted wholesale — remove symbols individually and keep that one.

Any symbol that turns out to still have a caller stays, and the reason goes in
the task report.

- [ ] **Step 2: Delete, in dependency order**

Work leaves-first so each intermediate state still compiles:

1. `appendLegacyCommandRecordAsV2`, `loadLegacy`, `legacyRange` from
   `d3d9_pe_chunk_v2_draw.cpp`.
2. `appendCommandRecord` / `appendCommandRecordRetained` from
   `d3d9_pe_device.cpp`, and the `build` / `encode` phases of
   `appendCommandRecordDirect`. `resize` and `flush` stay — `flush` now
   consults `shouldFlushBeforeAppend` (Task 4) with a V2-derived size, which
   is what preserves seal cadence.
3. The unix-side dead symbols confirmed in Step 1.
4. `applyDrawPacketState*`, `commitChunkDrawDeltaMask`,
   `validateDrawPacketStateDelta`, `packetHasNoStateDelta`, `makeRunParam`
   from `device_c_chunk_replay.cpp` and `device_c_record_utils.hpp`.
5. `d3d9_pe_draw_packet.hpp`'s `populateDrawPacket*` helpers.
6. `D9CDrawPrimitivePacket`, `D9CDrawIndexedPrimitivePacket`, the 16 legacy
   `D9CCommandRecord*` typedef structs, and the two bridge op declarations
   from `include/dxmt9/device_c.h`.

**Keep** `D9CDrawPacketTextureStageState`, `D9CDrawPacketSamplerState`, and
`D9CDrawPacketTransform` — `SparseStateV2Input` uses them directly
(`d3d9_pe_chunk_v2_builder.hpp:181-183`).

Removing the two bridge ops renumbers every later opcode. Update
`tests/native/bridge/bridge_ops_spec.cpp:48` (`kBridgeOpcodeCount == 158`, now
`156`) and `:103` (`drawChunk == drawPacket + 1`, now deleted along with both
opcodes).

- [ ] **Step 3: Rebuild all lanes in lockstep**

The ABI hash changes, so PE and unix must be rebuilt together
(`agents/rules/build.rules.md`):

```sh
meson compile -C build
meson compile -C build-x86_64-builtin
meson compile -C build-win32-x64-builtin
meson compile -C build-win32-x86-builtin
```

Never use a bare `ninja src/winemetal/unix/winemetal.so` target — it skips the
`winemetal_unix_install_name_fixup` stamp and produces bare-dep `.so` files
that fail the bridge handshake with `status=0xc0000003`.

- [ ] **Step 4: Rewrite or retire the full-snapshot spec**

`tests/native/bridge/pe_full_snapshot_equivalence_spec.cpp` mirrors a producer
that no longer exists. Two options, and the choice belongs to whoever runs
this task:

- **Rewrite** it against `buildSparseStateV2`, comparing delta-mode and
  snapshot-mode output for the same shadow. This is now possible because the
  producer is natively callable — the mirror existed only because it was not.
  This is the preferred option: it converts a self-consistent mirror into a
  real test.
- **Retire** it, if the differential's `allSlotsDirtyTriggersSnapshotFlag`
  fixture plus a snapshot-mode variant of each existing fixture already covers
  the contract.

Whichever is chosen, state it and the reasoning in the task report. Do not
leave a mirror spec compiling against deleted types.

- [ ] **Step 5: Run everything**

```sh
meson test -C build
bash scripts/check/verify_tla.sh
python3 scripts/check/audit_winemetal_install_names.py
git diff --check
```

Expected: all green. The install-name audit matters here specifically because
this task rebuilds the unix provider.

- [ ] **Step 6: Wine runtime evidence on all four workloads**

```sh
python3 scripts/run_apps/run_experiment.py run app-d3d9-3dmark05
python3 scripts/run_apps/run_experiment.py run app-d3d9-sfiv-benchmark
```

Run 3DMark05 for GT1, GT2, and GT3. For each: `status: pass`,
`gpu_command_buffer_errors = 0`, `v0.0.3` anchor match.

Per `agents/rules/metal_debugging.rules.md`, do **not** use GT2's
`result.json` present count as an FPS metric — GT2 is a fixed ~68 s timeline
that hangs post-scene until the timeout kill, and SIGKILL loses the final
counter flush. Enable `DXMT9_PERF_FRAME_SAMPLING=1` and compute scene fps from
the per-frame `wall_ms` samples if a number is wanted.

- [ ] **Step 7: Measure, and report honestly**

Take a paired GT2 A/B against the pre-Task-1 commit, both on the `perf`
profile, frame sampling on, no gputrace:

```sh
bash scripts/tools/run_3dmark05_perf_probe.sh --suffix legacy-removal-after \
  --frame 50 --no-gputrace --timeout 120 --keep-frontmost
```

The design's §1 expectation is at most `3.62 ms` of a `53.2 ms` frame
(`6.8%`), with a fraction of that surviving because section encoding remains,
and the sparse family's own share unmeasured. **Report what the A/B actually
shows.** If FPS does not move, say so — the design states plainly that this
work is justified structurally and that FPS was never the criterion. Do not
convert a null into a claim.

Record the result as a new leaf under
`docs/perfomance/state-churn-encode/`, following the frontmatter and `source:`
conventions of the existing leaves in that directory, with `source:` pointing
at the actual `experiments/output/...` directories the runs produced.

- [ ] **Step 8: Commit**

```sh
git add -A
git commit -m "refactor(pe): delete the legacy record format and dead fat-packet ABI

Removes the 16 legacy D9CCommandRecord* structs, D9CDrawPrimitivePacket
and everything typed on it (d3d9_pe_draw_packet.hpp's populate helpers,
packetHasNoStateDelta, makeRunParam, the unix applyDrawPacketState*
family), the appendLegacyCommandRecordAsV2 shim, and the two
zero-PE-caller bridge ops dxmt9c_device_draw_primitive_packet and
dxmt9c_device_draw_primitive_chunk.

Kept: D9CDrawPacketTextureStageState / SamplerState / Transform, which
SparseStateV2Input uses directly, and replayInfoForCommandRecordType,
which is live from device_c_chunk_replay.cpp.

The bridge op removal renumbers opcodes and changes
DXMT9_WINEMETAL_CALL_ABI_HASH; all four build lanes rebuilt in lockstep
and bridge_ops_spec updated."
```

---

## Self-Review

**Spec coverage.** Every design section maps to a task: §3's four input classes
→ Tasks 1-3 (shadow, views, rehost); §4's Phase 1 → Tasks 1-3; §5's family
order → Tasks 4-8 (2b→4, 2c→5, 2d→6, 2e→7, 2f→8); §6's differential and its
three side-channel gates → Task 6 Step 1 (retained-object and count
comparisons) and Task 4 Step 3 (`shouldFlushBeforeAppend`, preserving seal
cadence); §7's deletions → Task 9.

**One deliberate deviation.** The design numbers the differential harness as
step 2a, before the non-draw and constant migrations. This plan puts it inside
Task 6 instead. Reason: a harness committed as its own task would sit red
across Tasks 4 and 5, breaking their gate. The design's actual requirement —
harness written and confirmed failing *before* the producer it tests — is
preserved exactly within Task 6, Steps 1-3.

**Known-weak gates, stated rather than hidden.** Task 3 has no native
coverage; its substitute gates and their limits are written into the task.
Task 8's Wine runs are weak evidence because UP draws are rare on both
workloads; the task says so and names the differential as the real gate.

**Line numbers.** Every `file:line` in this plan was read from the tree at
`cc5e3a99`. They will drift as tasks land. Treat them as starting points and
re-locate by symbol name if a line does not match.
