# PE Legacy Record Removal Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the PE recorder's legacy intermediate record format and the fat `D9CDrawPrimitivePacket`, so `peState_` emits `SparseStateV2Input` directly into the V2 chunk builder.

**Architecture:** `appendCommandRecordDirect` already owns every gate that matters — the recorder mutex, the negotiation check, the CapacityPre and CapacityPost flushes, and the append telemetry — and the legacy round trip is only its `write` and `encode` phases. This plan replaces those two phases with a caller-supplied V2 emitter, one record family at a time, so every gate survives by construction. The producer feeding those emitters is first rehosted unchanged into a natively-buildable TU, then rewritten there behind a differential test that runs the real old and new code over a shared corpus.

**Tech Stack:** C++20, Meson/Ninja, macOS host build. No new dependencies.

**Spec:** `docs/superpowers/specs/2026-07-29-pe-legacy-record-removal-design.md` (as amended at `455f1c56`)

## Global Constraints

- 2-space indentation, spaces not tabs, `#pragma once` in headers, local project includes before standard headers. Match surrounding file style; do not mass-format.
- No per-draw or per-state heap allocation on rendering paths. Scratch storage is reused, device-owned, fixed-capacity.
- Do not store borrowed spans, stack pointers, or PE COM pointers past the call that received them.
- New TUs added to native test targets must not include `windows.h` or `d3d9.h`, transitively.
- New meson tests use stable names beginning with `dxmt9-`.
- Emitted V2 sections require strictly ascending slot order per section (`orderedSlot`, `src/d3d9/d3d9_pe_chunk_v2_draw.cpp:190`), spans alive across the append that consumes them, and `valid <= 1`.
- The V2 emitters (`appendSparseRecordV2`, `appendApplyStateV2`, `appendSetConstantsV2`, `appendClearV2`, `appendPresentV2`, `appendStretchRectV2`, `appendColorFillV2`) do not change.
- **Every append keeps going through the `appendCommandRecordDirect` envelope.** Its mutex, `commandChunkNegotiated_` gate, CapacityPre and CapacityPost flushes, and telemetry (`recordPeChunkInterAppendGap`, `peV2AppendDecimatedStats_`, `peAppendTypeCounts_`, `recordPeAppendCpu`, `notePeChunkAppendBoundary`, `logPeRecordMilestoneAfterPresent`) must survive every task. Chunk seal cadence therefore cannot drift.
- `DXMT9_PE_INLINE_CONST_DELTA` is **off by default** (`d3d9_pe_device.cpp:112-115`). The default draw path emits standalone SET_CONST records via `flushPendingConsts()` before the draw. Do not move constant folding into the draw record on the default path.
- `git diff --check` clean before every commit.
- Do not create `specs/**/plan.md` (gitignored).

---

## File Structure

**New files:**

| File | Responsibility |
|---|---|
| `src/d3d9/d3d9_pe_producer_views.hpp` | POD types: `PeStreamBinding`, `PeBindingView`, `PeChunkContext`, `PeDrawPayloads`, `PeDrawParams`, `PeSparseScratch`. No logic. |
| `src/d3d9/d3d9_pe_producer.hpp` / `.cpp` | `buildDrawPacketFromViews` (Task 4), then `buildSparseStateV2` and `addChunkContextSections` (Tasks 7-8). Natively buildable. |
| `tests/native/bridge/pe_shadow_native_spec.cpp` | Shadow compiles and behaves without `windows.h`. |
| `tests/native/bridge/pe_producer_views_spec.cpp` | View types are POD; scratch capacities match section caps. |
| `tests/native/bridge/pe_producer_differential_spec.cpp` | Old-vs-new corpus differential. Grows across Tasks 7-9. |

**Key modified files:**

| File | Change |
|---|---|
| `src/d3d9/d3d9_pe_state_shadow.hpp` | Windows types removed (Task 1). |
| `src/d3d9/d3d9_pe_device.cpp` | `appendRecordV2` seam (Task 3); producer body removed (Task 4); per-family emitters (Tasks 5-9). |
| `src/d3d9/d3d9_pe_chunk_v2_draw.cpp` | Legacy shim shrinks per family, deleted in Task 10. |
| `include/dxmt9/device_c.h` | Legacy records and fat-packet bridge ops deleted in Task 10. |

---

## Task 1: Make the PE state shadow Windows-free

**Files:**
- Modify: `src/d3d9/d3d9_pe_state_shadow.hpp` — include at `:4`; constant uses at `:194-228` and `:314-318`; `DWORD` at `:309`, `:313`, `:322`, `:331`, `:336`, `:344`, `:380-381`, `:387`, `:403`, `:407`, `:409-411`, `:456` (twice)
- Create: `tests/native/bridge/pe_shadow_native_spec.cpp`
- Modify: `tests/native/bridge/meson.build`

**Interfaces:**
- Consumes: nothing.
- Produces: `PeHotStateShadow` and the `FixedStateTable` / `FixedTransformTable` helpers compile natively. **All of it is at global scope — `d3d9_pe_state_shadow.hpp` has no enclosing namespace.** `d3d9_pe_const_shadow.hpp` is already Windows-free.

Mirrored constant values, read from `/Users/dididi/workspaces/wine/include/d3d9types.h` and `d3d9.h`:

| Constant | Value | Source |
|---|---:|---|
| `D3DTS_VIEW` | `2` | `d3d9types.h:1201` |
| `D3DTS_PROJECTION` | `3` | `d3d9types.h:1202` |
| `D3DTS_TEXTURE0` | `16` | `d3d9types.h:1203` |
| `D3DTS_TEXTURE7` | `23` | `d3d9types.h:1210` |
| `D3DTS_WORLD` | `256` | `d3d9types.h:102` — `D3DTS_WORLDMATRIX(0)` |
| `D3DDMAPSAMPLER` | `256` | `d3d9types.h:201` |
| `D3DVERTEXTEXTURESAMPLER0` | `257` | `d3d9types.h:202` |
| `D3DVERTEXTEXTURESAMPLER3` | `260` | `d3d9types.h:205` |

- [ ] **Step 1: Write the failing test**

Create `tests/native/bridge/pe_shadow_native_spec.cpp`:

```cpp
// pe_shadow_native_spec
//
// Proves d3d9_pe_state_shadow.hpp compiles and behaves natively, with no
// windows.h / d3d9.h in its transitive include set, and pins the mirrored
// D3D9 constants against the literal values they replaced.
//
// Everything in that header is at global scope -- there is no namespace to
// alias here.

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
  if (!condition) throw TestFailure(std::string(message));
}

void transformSlotsMatchD3DConstants() {
  std::uint32_t slot = 0;

  check(FixedTransformTable::slotForState(2u, slot), "D3DTS_VIEW must map");
  check(slot == 0u, "D3DTS_VIEW is slot 0");
  check(FixedTransformTable::slotForState(3u, slot),
        "D3DTS_PROJECTION must map");
  check(slot == 1u, "D3DTS_PROJECTION is slot 1");

  check(FixedTransformTable::slotForState(16u, slot),
        "D3DTS_TEXTURE0 must map");
  check(slot == kPeTransformTextureBaseSlot, "D3DTS_TEXTURE0 is the tex base");
  check(FixedTransformTable::slotForState(23u, slot),
        "D3DTS_TEXTURE7 must map");
  check(slot == kPeTransformTextureBaseSlot + 7u,
        "texture transform slots are contiguous");

  check(FixedTransformTable::slotForState(256u, slot), "D3DTS_WORLD must map");
  check(slot == kPeTransformWorldBaseSlot, "D3DTS_WORLD is the world base");
  check(FixedTransformTable::slotForState(259u, slot),
        "D3DTS_WORLDMATRIX(3) must map");
  check(slot == kPeTransformWorldBaseSlot + 3u,
        "world matrix slots are contiguous");

  for (const std::uint32_t state : {2u, 3u, 16u, 23u, 256u, 259u}) {
    check(FixedTransformTable::slotForState(state, slot), "state must map");
    check(FixedTransformTable::stateForSlot(slot) == state,
          "transform slot must round-trip to its state id");
  }
}

void vertexTextureSamplerSlotsMatchD3DConstants() {
  std::uint32_t slot = 0;

  // Vertex texture samplers land ABOVE the fragment sampler block:
  // slot = kPeFragmentSamplerSlots + (sampler - D3DVERTEXTEXTURESAMPLER0).
  // See d3d9_pe_state_shadow.hpp:313-320. They are NOT slots 0..3.
  check(!vertexTextureSamplerSlot(256u, slot),
        "D3DDMAPSAMPLER (256) is not a vertex texture sampler");
  check(vertexTextureSamplerSlot(257u, slot),
        "D3DVERTEXTEXTURESAMPLER0 must map");
  check(slot == kPeFragmentSamplerSlots,
        "D3DVERTEXTEXTURESAMPLER0 sits just above the fragment sampler block");
  check(vertexTextureSamplerSlot(260u, slot),
        "D3DVERTEXTEXTURESAMPLER3 must map");
  check(slot == kPeFragmentSamplerSlots + 3u,
        "D3DVERTEXTEXTURESAMPLER3 is three above it");
  check(!vertexTextureSamplerSlot(261u, slot),
        "261 is past D3DVERTEXTEXTURESAMPLER3");
}

void pendingMasksAreThirtyTwoBitUnsigned() {
  PeHotStateShadow shadow{};
  check(!shadow.hasPendingHotState(), "a fresh shadow has nothing pending");

  shadow.pendingTextureMask = 0xFFFFFFFFu;
  check(shadow.pendingTextureMask == 0xFFFFFFFFu,
        "pendingTextureMask must hold 32 bits unsigned");
  check(shadow.hasPendingHotState(), "a set mask must be pending");

  shadow.clearPendingHotState();
  check(!shadow.hasPendingHotState(),
        "clearPendingHotState must clear the mask");
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

Append to `tests/native/bridge/meson.build`, following the
`pe_chunk_record_v2_value_spec` pattern at `:130-147`:

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

Expected: FAIL at compile time with `windows.h` not found, from
`d3d9_pe_state_shadow.hpp:4`'s include of `d3d9_pe.hpp`.

- [ ] **Step 3: Replace the Windows types**

Replace the `d3d9_pe.hpp` include at `:4` with `"dxmt9/device_c.h"`.

Add the mirrored constants **at global scope**, beside the existing `kPe*Slots`
constants at `:14-31` — this header has no namespace, and putting them in one
would break every unqualified use below:

```cpp
// D3D9 constant mirrors. This header compiles natively (no windows.h /
// d3d9.h), so the values are inlined from the D3D9 SDK headers. Verified
// against wine/include/d3d9types.h and d3d9.h; see the table in
// docs/superpowers/plans/2026-07-29-pe-legacy-record-removal.md.
// core_constants.hpp:665 sets the precedent for mirroring the WORLDMATRIX
// range this way.
static constexpr std::uint32_t kD3dTsView = 2u;
static constexpr std::uint32_t kD3dTsProjection = 3u;
static constexpr std::uint32_t kD3dTsTexture0 = 16u;
static constexpr std::uint32_t kD3dTsTexture7 = 23u;
static constexpr std::uint32_t kD3dTsWorld = 256u;
static constexpr std::uint32_t kD3dDmapSampler = 256u;
static constexpr std::uint32_t kD3dVertexTextureSampler0 = kD3dDmapSampler + 1u;
static constexpr std::uint32_t kD3dVertexTextureSampler3 = kD3dDmapSampler + 4u;
```

Then mechanically: `static_cast<std::uint32_t>(D3DTS_*)` → the matching
`kD3dTs*` at `:194`, `:198`, `:202-205`, `:208-212`, `:219`, `:222`, `:225`,
`:228`; `D3DVERTEXTEXTURESAMPLER0/3` → `kD3dVertexTextureSampler0/3` at `:314`,
`:318`; `DWORD` → `std::uint32_t` at the 13 lines listed under **Files**
(`:456` has two); `D3DTEXTURESTAGESTATETYPE` (`:331`) and `D3DSAMPLERSTATETYPE`
(`:344`) → `std::uint32_t`, dropping the now-redundant casts inside those two
bodies.

- [ ] **Step 4: Run the test to verify it passes**

```sh
meson compile -C build dxmt9-pe-shadow-native-spec
meson test -C build dxmt9-pe-shadow-native-spec
```

Expected: PASS.

If an assertion fails, the header is the source of truth — read the function
and fix the test's expected value, then note the correction in the task report.
Do not change the header to match the test.

- [ ] **Step 5: Run the full suite and commit**

```sh
meson test -C build
grep -n "textureStageSlot\|vertexTextureSamplerSlot\|textureBindingSlot\|samplerSlot\|samplerStateSlot\|renderStateEquals" src/d3d9/d3d9_pe_device.cpp
```

The grep confirms no call site depends on a `DWORD`-specific overload —
`d3d9_pe_device.cpp` only builds on Windows, so the native run cannot prove it.

```sh
git add src/d3d9/d3d9_pe_state_shadow.hpp tests/native/bridge/pe_shadow_native_spec.cpp tests/native/bridge/meson.build
git commit -m "refactor(pe): make the PE state shadow natively buildable

Replace windows.h-dependent types in d3d9_pe_state_shadow.hpp with
std::uint32_t and global-scope mirrored D3D9 constants, and drop the
d3d9_pe.hpp include. Values verified against wine/include/d3d9types.h.

dxmt9-pe-shadow-native-spec proves the header compiles without windows.h
and pins the mirrors, including that vertex texture samplers map above the
fragment sampler block rather than to slots 0..3."
```

---

## Task 2: POD input views

**Files:**
- Create: `src/d3d9/d3d9_pe_producer_views.hpp`
- Create: `tests/native/bridge/pe_producer_views_spec.cpp`
- Modify: `tests/native/bridge/meson.build`

**Interfaces:**
- Consumes: `PeWireObjectRef` (`d3d9_pe_chunk_v2_builder.hpp:14-23`), `SparseBindingV2Input` (`:145-151`), `D9C_DRAW_PACKET_MAX_*` from `include/dxmt9/device_c.h`.
- Produces, all in `namespace dxmt9::d3d9::pe`: `PeStreamBinding`, `PeBindingView`, `PeChunkContext`, `PeDrawPayloads`, `PeDrawParams`, `PeSparseScratch`.

- [ ] **Step 1: Write the failing test**

Create `tests/native/bridge/pe_producer_views_spec.cpp`:

```cpp
// pe_producer_views_spec
//
// The producer's input views must be trivially copyable PODs, because the
// differential harness constructs them directly and the producer must retain
// nothing from them past the call. Scratch capacity must match the V2 section
// caps, or a full-width delta silently truncates.

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
  if (!condition) throw TestFailure(std::string(message));
}

namespace pe = dxmt9::d3d9::pe;

void viewsAreTriviallyCopyable() {
  static_assert(std::is_trivially_copyable_v<pe::PeStreamBinding>);
  static_assert(std::is_trivially_copyable_v<pe::PeBindingView>);
  static_assert(std::is_trivially_copyable_v<pe::PeChunkContext>);
  static_assert(std::is_trivially_copyable_v<pe::PeDrawParams>);
  check(true, "compile-time only");
}

void payloadsAreEmptyByDefault() {
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
  check(chunk.retainedStreamMask == 0u, "a fresh chunk retains no streams");
  check(!chunk.indexBufferKnown, "a fresh chunk has no known index buffer");
  check(chunk.submittedIndexBufferWire == 0u,
        "a fresh chunk has no submitted index buffer wire value");
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
    payloadsAreEmptyByDefault();
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

Expected: FAIL — `d3d9_pe_producer_views.hpp` does not exist.

- [ ] **Step 3: Create the header**

Create `src/d3d9/d3d9_pe_producer_views.hpp`:

```cpp
#pragma once

// POD inputs for the PE sparse-state producer.
//
// Per the design's §3 (as amended), the producer is pure over (a) bindings,
// (b) the constant shadow, and (c) draw payloads. Destination-chunk state (d)
// is NOT a producer input: production applies it in the draw call sites'
// writer lambdas, after the producer runs, and never for APPLY_STATE. It is
// therefore an input to addChunkContextSections instead.
//
// PeChunkContext is passed as data rather than reached for through
// CommandChunkV2Builder so a native differential test can drive it.

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

// (a) COM-derived bindings, already translated by the device. The producer
// forwards `object` to CommandChunkV2Builder::appendHandle, which owns
// retention; it never dereferences it.
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

// (d) Destination-chunk history, consumed by addChunkContextSections only.
// Mirrors what d3d9_pe_device.cpp:9462-9484 computes today.
// submittedIndexBufferWire holds the POINTER-valued wire, as
// d9cWireHandleValue(toWireHandle(rawIBuf(indexBuf_))) does at :9401-9406 --
// not an objectId. Comparing the wrong one makes every indexed draw re-emit
// its index binding.
struct PeChunkContext {
  std::uint32_t retainedStreamMask = 0u;
  bool indexBufferKnown = false;
  std::uint64_t submittedIndexBufferWire = 0u;
};

// (c) Draw payloads. Borrowed for the producer call and the append that
// consumes its output. Never stored.
struct PeDrawPayloads {
  std::span<const std::byte> upIndex{};
  std::span<const std::byte> upVertex{};
};

// Per-draw scalars. APPLY_STATE passes this default-constructed.
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

// Device-owned, reused output storage. The SparseStateV2Input spans point
// into these arrays, so the scratch must outlive the append that consumes
// them. Capacities match the V2 section caps; the producer returns false
// rather than truncating.
struct PeSparseScratch {
  std::array<D9CCommandChunkWireRenderStateV2,
             D9C_DRAW_PACKET_MAX_RENDER_STATES> renderStates{};
  std::array<SparseBindingV2Input<D9CCommandChunkWireTextureBindingV2>,
             D9C_DRAW_PACKET_MAX_TEXTURES> textures{};
  std::array<SparseBindingV2Input<D9CCommandChunkWireStreamBindingV2>,
             D9C_DRAW_PACKET_MAX_STREAMS> streams{};
  std::array<SparseBindingV2Input<D9CCommandChunkWireShaderBindingV2>, 2>
      shaders{};
  // ONE vertex input, not two: the section is V2SectionRuleSingle with
  // maxCount 1, and FVF versus declaration is the entry's `kind` field.
  std::array<SparseBindingV2Input<D9CCommandChunkWireVertexInputV2>, 1>
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

Expected: PASS. If a `static_assert` fails, find the member with a non-trivial
default rather than relaxing the assert — the differential depends on these
being memcpy-able.

- [ ] **Step 5: Commit**

```sh
git add src/d3d9/d3d9_pe_producer_views.hpp tests/native/bridge/pe_producer_views_spec.cpp tests/native/bridge/meson.build
git commit -m "feat(pe): add POD input views for the sparse-state producer

PeBindingView, PeDrawPayloads, and PeDrawParams are producer inputs;
PeChunkContext feeds the separate chunk-context step the draw sites call
afterwards, matching production. PeSparseScratch is reused device-owned
output storage sized to the V2 section caps."
```

---

## Task 3: The `appendRecordV2` seam

**Files:**
- Modify: `src/d3d9/d3d9_pe_device.cpp:9160-9275` (`appendCommandRecordDirect`)

**Interfaces:**
- Consumes: nothing.
- Produces:
  ```cpp
  // Emit is invoked as:
  //   HRESULT(dxmt9::d3d9::pe::CommandChunkV2Builder&, const AppendPhaseTimer&)
  template <typename Emit>
  HRESULT appendRecordV2(std::uint32_t type, std::size_t sizeHint, Emit&& emit);
  ```
  **The emitter returns HRESULT, not bool, and this is enforced by a
  `static_assert` inside `appendRecordV2`.** `HRESULT` is `long`, so a lambda
  with no trailing return type wrapping a `bool`-returning V2 emitter would
  deduce `bool` and convert `false` to `0 == S_OK`: a failed append reported as
  success, CapacityPost run, and `notePeChunkAppendBoundary` counting a record
  that never landed. Every emitter below spells out
  `? S_OK : D3DERR_INVALIDCALL`. The `AppendPhaseTimer` second parameter exists
  because the envelope owns the decimation sampling decision; an emitter records
  `peAppendPhaseEncode_` around its own emission so `encode` keeps one meaning
  across the migration.
  Tasks 5-9 call this instead of `appendCommandRecordDirect`.

**Why this task exists.** `appendCommandRecordDirect` owns the recorder mutex,
the `commandChunkNegotiated_` gate (`:9174-9177`), the CapacityPre decision
(`:9182-9190`), the CapacityPost flush (`:9255-9261`), and seven telemetry
sites. The legacy round trip is only its `write` and `encode` phases
(`:9243-9254`). Extracting an emit seam preserves everything else by
construction — which is what makes "chunk seal cadence must not change" true
without inventing a separate mechanism.

- [ ] **Step 1: Add the seam**

Split the body so the envelope becomes a template taking an emitter, and
`appendCommandRecordDirect` becomes its first adapter:

```cpp
// Envelope: mutex, negotiation gate, CapacityPre, telemetry, CapacityPost.
// `sizeHint` feeds the capacity precheck. During migration it stays the legacy
// record size so seal cadence is bit-identical to before; a family that no
// longer builds a legacy record passes its own estimate on the same scale.
template <typename Emit>
HRESULT appendRecordV2(std::uint32_t type, std::size_t sizeHint, Emit&& emit) {
    // ... everything from :9160 through the CapacityPre flush, unchanged,
    // with `bytes` renamed to `sizeHint` ...
    if (SUCCEEDED(hr)) {
        const auto t1 = phaseNow();
        if (!emit(commandChunkV2_)) {
            hr = D3DERR_INVALIDCALL;
        }
        phaseRecord(peAppendPhaseEncode_, t1);
    }
    // ... CapacityPost and the telemetry tail from :9255 onward, unchanged ...
}

template <typename Write>
HRESULT appendCommandRecordDirect(std::uint32_t type, std::size_t bytes,
                                  Write&& write) {
    return appendRecordV2(type, bytes,
        [&](auto& builder, const auto& phase) -> HRESULT {
        const auto t0 = phaseNow();
        if (legacyV2RecordScratch_.size() < bytes) {
            legacyV2RecordScratch_.resize(bytes);
        }
        phaseRecord(peAppendPhaseResize_, t0);
        const auto t1 = phaseNow();
        write(reinterpret_cast<std::uint8_t*>(legacyV2RecordScratch_.data()));
        phaseRecord(peAppendPhaseWrite_, t1);
        return dxmt9::d3d9::pe::appendLegacyCommandRecordAsV2(
            builder,
            std::span<const std::byte>(legacyV2RecordScratch_.data(), bytes));
    });
}
```

The `resize` and `write` phase timers move into the adapter, being
legacy-specific. `encode` stays in the envelope and now times whatever the
emitter does — which is the measurement this whole plan is about.

Read `:9160-9275` in full before editing; the sketch names the moving parts but
is not a substitute for the actual body.

- [ ] **Step 2: Build all lanes**

```sh
meson compile -C build
meson compile -C build-win32-x64-builtin
meson compile -C build-win32-x86-builtin
```

Expected: all succeed with no call-site changes anywhere — every existing caller
still calls `appendCommandRecordDirect`.

- [ ] **Step 3: Prove the diff is a pure split**

```sh
git diff src/d3d9/d3d9_pe_device.cpp
```

Every line of the old body must appear exactly once in the new pair, with no
reordering across the CapacityPre / encode / CapacityPost boundaries. List any
line that is not a pure move in the task report.

- [ ] **Step 4: Wine runtime check and commit**

```sh
meson test -C build
python3 scripts/run_apps/run_experiment.py run app-d3d9-3dmark05
```

Expected: `status: pass`, `gpu_command_buffer_errors = 0`, scene matches the
`v0.0.3` visual anchor. This step changes nothing observable; a regression here
means the split was not pure.

```sh
git add src/d3d9/d3d9_pe_device.cpp
git commit -m "refactor(pe): extract an emit seam from appendCommandRecordDirect

appendRecordV2 keeps the recorder mutex, the negotiation gate, the
CapacityPre and CapacityPost flushes, and the append telemetry, and takes
record emission as a callable. appendCommandRecordDirect becomes its
legacy adapter.

Migrating a record family is now a change of emitter at one call site,
with every gate and counter preserved by construction -- which is what
keeps chunk seal cadence fixed."
```

---

## Task 4: Rehost the producer against the views

**Files:**
- Create: `src/d3d9/d3d9_pe_producer.hpp` / `.cpp`
- Modify: `src/d3d9/d3d9_pe_device.cpp:3867-4103` (remove) and the 6 call sites at `:9329`, `:9389`, `:9527`, `:9627`, `:10000`, `:10139`
- Modify: `src/d3d9/meson.build` (`dxmt9_pe_core_srcs`, near `:51`); `tests/native/bridge/meson.build`

**Interfaces:**
- Consumes: Tasks 1-2.
- Produces:
  ```cpp
  namespace dxmt9::d3d9::pe {
  bool buildDrawPacketFromViews(const PeHotStateShadow& shadow,
                                const PeBindingView& bindings,
                                std::uint32_t primitiveType,
                                std::uint32_t startVertex,
                                std::uint32_t primitiveCount,
                                bool forceFullSnapshot,
                                PeDecimatedScopeStats& stats,
                                D9CDrawPrimitivePacket& packet) noexcept;
  }
  ```
  `PeDecimatedScopeStats` is at **global scope** (`d3d9_pe_stats_decimation.hpp:17`);
  `peDrawPacketDecimatedStats_` is that type (`d3d9_pe_device.cpp:3428`).

**This task has no native test coverage, and that is a known, accepted gap.**
No meson test compiles `d3d9_pe_device.cpp` — `src/d3d9/meson.build:66` gates
`dxmt9_pe_core_srcs` on `host_machine.system() == 'windows'`.
`pe_full_snapshot_equivalence_spec.cpp` cannot substitute: its header
(`:41-53`) states it *mirrors* the producer rather than calling it. The two
substitute gates are Step 4 and Step 5. Do not skip either.

- [ ] **Step 1: Move the body, resolving five things renaming does not cover**

Create the header and TU, and move `d3d9_pe_device.cpp:3867-4103` into
`buildDrawPacketFromViews`. The straightforward renames are `peState_.X` →
`shadow.X` and `peDrawPacketDecimatedStats_` → `stats`.

Five things need real work:

1. **File-static helpers.** The body calls `dxmt9PeStatsDecimationN()`
   (`:131`), `DxmtPeDecimatedScopeGuard` (`:146`), and
   `dxmt9PeFullSnapshotEnabled()` (`:376`) — all `static` in
   `d3d9_pe_device.cpp` and unreachable from the new TU. Move the first two
   into `d3d9_pe_stats_decimation.hpp`, which already hosts the decimation
   machinery, and `dxmt9PeFullSnapshotEnabled` into `d3d9_pe_producer.hpp` as a
   non-static inline. Its `getenvFlag` dependency is native-safe.
2. **`DWORD` loop counters** inside the body at `:3903`, `:3910`, `:4031`,
   `:4035` — change to `std::uint32_t`.
3. **Handle type conversion.** `packet.textures[]`, `vsHandle`, `psHandle`, and
   the stream `buffer` fields are `D9CWireHandle`; the view carries
   `PeWireObjectRef`. Convert with the encoding production uses —
   `toWireHandle` (`d3d9_pe_recorder.hpp:340`) applied to `ref.object`. Writing
   `ref.identity` instead produces a handle the V2 builder's cache cannot
   resolve.
4. **`populateDrawPacketAttachmentDelta` / `...Snapshot`** take
   `PeRtWireHandles` = `std::array<D9CWireHandle, 4>`
   (`d3d9_pe_draw_packet.hpp:10-11`; callers at `:3933-3935`, `:4041-4043`).
   The view holds `PeWireObjectRef`s, so build a local `PeRtWireHandles` from
   `bindings.renderTargets` before the call.
5. **`D3DPRIMITIVETYPE`** in the old signature becomes `std::uint32_t`; call
   sites cast.

Add `d3d9_pe_producer.cpp` to `dxmt9_pe_core_srcs` and to the
`dxmt9-pe-producer-views-spec` target's source list so native compilation is
proven immediately.

- [ ] **Step 2: Rewrite the 6 call sites — each differently**

There is no uniform template. Read each site and preserve its own shape:

| Site | Packet argument | Notes |
|---|---|---|
| `:9329` drawidx | `record.packet` | |
| `:9389` draw | `record.packet.state` | |
| `:9527` drawUP | `header.packet.state` | passes a live `forceFullSnapshot` from `appendDrawPrimitiveUPRecordWithFvf` (`:9501`) — do **not** hardcode `false` |
| `:9627` drawidxUP | `header.packet.state` | same live `forceFullSnapshot` (`:9601`) |
| `:10000` applystate | `record.packet` | on failure falls through to `drainOversizedPendingStateAsApplyStateRecords()` (`:10008-10014`) — do **not** `return D3DERR_INVALIDCALL` |
| `:10139` present tail | `tail.packet` | |

Add one private helper that fills the view from the COM members. Verified
accessor names: `rawTex`, `rawVBuf`, `rawVS`, `rawPS`, **`rawVD`** (not
`rawVDecl`, `:3195`), `rawSurf`; the index-buffer member is **`indexBuf_`**
(not `ib_`, `:9401`). There is **no `wireRef` function in the tree** —
construct `PeWireObjectRef` the way the wrapper accessors do (see the
`wireObject()` pattern at `d3d9_pe_device_child_misc.cpp:61-80`) and report
which spelling you used.

**Preserve the UP save/restore window.** `:9511-9526` temporarily mutates
`fvf_` / `vdecl_` / `vs_` and three pending bits around the call, then restores
them. The view fill must run *inside* that window so it snapshots the temporary
values, exactly as the old inline reads did. Read `:9505-9535`.

- [ ] **Step 3: Build all lanes**

```sh
meson compile -C build
meson compile -C build-win32-x64-builtin
meson compile -C build-win32-x86-builtin
```

- [ ] **Step 4: Prove the diff is mechanical**

```sh
git diff src/d3d9/d3d9_pe_device.cpp > /tmp/task4-device.diff
```

Read it and confirm every removed producer line reappears in
`d3d9_pe_producer.cpp` differing only by the renames and the five resolutions
from Step 1. Write the list of non-mechanical changes into the task report;
there should be exactly the view-fill helper and the six call-site rewrites.

- [ ] **Step 5: Wine runtime evidence — all four workloads**

```sh
python3 scripts/run_apps/run_experiment.py run app-d3d9-3dmark05
DXMT_3DMARK05_ARGS="-gt2 -nosplash -nosysteminfo -noscreens" \
  python3 scripts/run_apps/run_experiment.py run app-d3d9-3dmark05
DXMT_3DMARK05_ARGS="-gt3 -nosplash -nosysteminfo -noscreens" \
  python3 scripts/run_apps/run_experiment.py run app-d3d9-3dmark05
python3 scripts/run_apps/run_experiment.py run app-d3d9-sfiv-benchmark
```

`DXMT_3DMARK05_ARGS` replaces the launcher's **whole** argument list, not just
its test selection: `default_3dmark05_args` is
`"$default_3dmark05_selection_args $default_3dmark05_runner_args"`, so passing
`-gt2` alone drops `-nosplash -nosysteminfo -noscreens` and the run sits on the
splash / system-info screens, never reaches the scene, and fails with
`missing_capture`. That failure looks exactly like a render regression and is
not one. Always carry the runner args. The design's §4 gate names all four
workloads.

For each: `status: pass` (a timeout-finalized 3DMark05 run with the expected
artifacts is acceptable), `gpu_command_buffer_errors = 0`, scene matches the
`v0.0.3` visual anchor.

If a run regresses, return to Step 4 and find the non-mechanical change.

- [ ] **Step 6: Run the suite and commit**

```sh
meson test -C build
git diff --check
git add src/d3d9 tests/native/bridge/meson.build
git commit -m "refactor(pe): rehost the draw-packet producer against POD views

Move buildDrawPrimitivePacket into d3d9_pe_producer.cpp against
PeHotStateShadow and PeBindingView. Five things needed more than renaming:
three file-static helpers moved to shared headers, DWORD loop counters,
PeWireObjectRef-to-D9CWireHandle conversion via toWireHandle, a local
PeRtWireHandles for the attachment helpers, and the D3DPRIMITIVETYPE
parameter.

No native test covers this -- no meson test compiles d3d9_pe_device.cpp,
and pe_full_snapshot_equivalence_spec mirrors the producer rather than
executing it. Gated by a mechanically reviewable diff and by
GT1/GT2/GT3/SFIV runs against the v0.0.3 anchor."
```

---

## Task 5: Emit non-draw records directly as V2

**Files:**
- Modify: `src/d3d9/d3d9_pe_device.cpp` — Present `:10740-10748`, and the retained non-draw sites at `:11258`, `:11294`, `:11358`, `:11475`, `:11510`, `:12271`
- Modify: `src/d3d9/d3d9_pe_device_child_misc.cpp:219-225` — QueryIssue
- Modify: `src/d3d9/d3d9_pe_chunk_v2_draw.cpp` — remove the migrated cases

**Interfaces:**
- Consumes: `appendRecordV2` (Task 3).
- Produces: no new symbols.

**Two things this is not.** (i) It does not remove the legacy path from barrier
sites — `chunkBarrierFlush()` still prepends a legacy APPLY_STATE until Task 7.
(ii) Object-ref acquisition moves from `lookupCachedWireObjectRef`
(`d3d9_pe_chunk_v2_draw.cpp:468-476`) to wrapper-side refs, changing the failure
path and the `noteWireIdentityGetterCall` counter. Report both.

**QueryIssue is the awkward one.** It reaches the recorder through the
`AppendRecordForChild` interface, not a direct device call, so migrating it
needs a V2-shaped `ForChild` entry point. If that proves larger than the rest of
this task combined, **leave QueryIssue on the legacy path**, say so in the
report, and migrate it in Task 10 — it is one record per query, not a hot path.

- [ ] **Step 1: Pin the existing equivalence before moving anything**

Add to `tests/native/bridge/pe_chunk_record_v2_value_spec.cpp`, before `main`,
and call it from `main`:

```cpp
// Task 5 baseline: a Clear emitted directly must match one emitted through the
// legacy shim, including its rect payload. This pins today's behavior before
// the call sites move; it is not a red-then-green cycle.
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
  legacy.header.size =
      static_cast<std::uint32_t>(sizeof(legacy) + sizeof(rects));
  std::vector<std::byte> legacyBytes(legacy.header.size);
  std::memcpy(legacyBytes.data(), &legacy, sizeof(legacy));
  std::memcpy(legacyBytes.data() + sizeof(legacy), rects, sizeof(rects));
  check(dxmt9::d3d9::pe::appendLegacyCommandRecordAsV2(viaLegacy, legacyBytes),
        "legacy Clear must append");

  check(direct.recordCount() == viaLegacy.recordCount(),
        "record count must match");
  check(direct.handleCount() == viaLegacy.handleCount(),
        "handle count must match");
  check(direct.payloadBytes() == viaLegacy.payloadBytes(),
        "payload size must match");
}
```

- [ ] **Step 2: Run it**

```sh
meson compile -C build dxmt9-pe-chunk-record-v2-value-spec
meson test -C build dxmt9-pe-chunk-record-v2-value-spec
```

Expected: PASS. A failure means the two paths already differ — a pre-existing
defect to report before touching anything.

- [ ] **Step 3: Migrate each site to `appendRecordV2`**

For each non-draw record type, replace the legacy-struct-plus-append pair with
an `appendRecordV2` call whose emitter invokes the matching V2 function.
Present, at `:10740-10748`, becomes:

```cpp
const HRESULT appendHr = appendRecordV2(
    D9C_COMMAND_RECORD_PRESENT, sizeof(D9CCommandRecordPresent),
    [&](auto& builder, const auto& phase) -> HRESULT {
        const auto t0 = decltype(phase)::now();
        const bool ok = dxmt9::d3d9::pe::appendPresentV2(
            builder,
            D9CCommandChunkWirePresentV2{
                .hwnd = hwnd, .flags = flags,
                .hasSrc = hasSrc, .hasDst = hasDst,
                .reserved0 = 0u, .src = src, .dst = dst,
            });
        phase.record(peAppendPhaseEncode_, t0);
        return ok ? S_OK : D3DERR_INVALIDCALL;
    });
```

`sizeHint` stays `sizeof(D9CCommandRecordPresent)` — the same value the capacity
precheck saw before, so seal cadence is unchanged. Keep that convention at every
migrated site: pass the legacy record's `sizeof` plus its payload, even though
no legacy record is built.

`appendCommandRecordRetained` sites additionally retained wrappers. The V2
builder's `appendHandle` performs retention itself, so the explicit retain
arguments disappear — verify by reading `appendStretchRectV2` /
`appendColorFillV2` (`d3d9_pe_chunk_v2_builder.hpp:213-219`) and confirming each
takes the `PeWireObjectRef`s it needs.

Then remove the migrated cases from `appendLegacyCommandRecordAsV2`.

- [ ] **Step 4: Build, test, run Wine, commit**

```sh
meson compile -C build && meson compile -C build-win32-x64-builtin && meson compile -C build-win32-x86-builtin
meson test -C build
python3 scripts/run_apps/run_experiment.py run app-d3d9-3dmark05
```

Expected: all pass; `status: pass`, `gpu_command_buffer_errors = 0`, `v0.0.3`
anchor match. Present and Clear happen every frame, so defects are immediate.

```sh
git add -A src/d3d9 tests/native/bridge
git commit -m "refactor(pe): emit non-draw records directly as V2

Present, StretchRect, ColorFill, UpdateTexture, UpdateSurface, Readback,
and RESZ call their V2 emitters through appendRecordV2 instead of building
a legacy struct for the shim to convert. sizeHint keeps the legacy sizeof
so the capacity precheck sees the same value and seal cadence is unchanged.

Barrier sites still prepend a legacy APPLY_STATE until the applystate
migration, and object-ref acquisition moved from the wire-handle cache to
wrapper-side refs, changing the failure path and the
noteWireIdentityGetterCall counter."
```

---

## Task 6: Emit constant records directly as V2

**Files:**
- Modify: `src/d3d9/d3d9_pe_device.cpp:9705-9730` — `appendSetConstRecord`
- Modify: `src/d3d9/d3d9_pe_chunk_v2_draw.cpp:894-913` — remove the six constant cases

**Interfaces:**
- Consumes: `appendRecordV2` (Task 3).
- Produces: no new symbols.

**There is one emitter, not six.** `appendSetConstRecord(recordType, start,
count, data, elemSize)` at `:9705` is called once, from `flushConstShadow` at
`:9850`, for all six kinds. Replacing its body migrates the whole family and
automatically preserves `flushConstShadow`'s surroundings: the
`DXMT9_SPLIT_SPARSE_CONST_RECORDS` multi-run diagnostic path (`:9864-9874`) and
the const-flush telemetry (`recordPeConstFlushCpu`, `recordVsConstSetterRange`).

- [ ] **Step 1: Pin the existing equivalence for all six kinds**

Add to `tests/native/bridge/pe_chunk_record_v2_value_spec.cpp` and call from
`main`:

```cpp
// Task 6 baseline: a constant range emitted directly must match one emitted
// through the legacy shim, for every one of the six kinds.
void directConstantsMatchLegacyConstants() {
  struct Case { std::uint32_t type; std::size_t elementSize; const char* name; };
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
    check(dxmt9::d3d9::pe::appendLegacyCommandRecordAsV2(viaLegacy,
                                                         legacyBytes),
          std::string("legacy constants must append: ") + c.name);

    check(direct.recordCount() == viaLegacy.recordCount(),
          std::string("record count must match: ") + c.name);
    check(direct.payloadBytes() == viaLegacy.payloadBytes(),
          std::string("payload size must match: ") + c.name);
  }
}
```

- [ ] **Step 2: Run it**

```sh
meson test -C build dxmt9-pe-chunk-record-v2-value-spec
```

Expected: PASS.

- [ ] **Step 3: Replace `appendSetConstRecord`'s body**

Keep the signature, the overflow guard, and the null-data guard exactly. Keep
`sizeHint` as `sizeof(D9CCommandRecordSetConst) + payloadBytes` so the capacity
precheck sees the value it saw before:

```cpp
HRESULT appendSetConstRecord(uint32_t recordType, UINT start, UINT count,
                             const void* data, std::size_t elemSize) {
    const std::uint64_t payload64 =
        static_cast<std::uint64_t>(count) * elemSize;
    if (payload64 > 0xffffffffull - sizeof(D9CCommandRecordSetConst)) {
        return D3DERR_INVALIDCALL;
    }
    const std::uint32_t payloadBytes = static_cast<std::uint32_t>(payload64);
    if (payloadBytes != 0 && !data) {
        return D3DERR_INVALIDCALL;
    }
    return appendRecordV2(
        recordType, sizeof(D9CCommandRecordSetConst) + payloadBytes,
        [&](auto& builder, const auto& phase) -> HRESULT {
            const auto t0 = decltype(phase)::now();
            const bool ok = dxmt9::d3d9::pe::appendSetConstantsV2(
                builder, recordType, start, count,
                std::span<const std::byte>(
                    reinterpret_cast<const std::byte*>(data), payloadBytes));
            phase.record(peAppendPhaseEncode_, t0);
            return ok ? S_OK : D3DERR_INVALIDCALL;
        });
}
```

Then remove the six constant cases from `appendLegacyCommandRecordAsV2`
(`d3d9_pe_chunk_v2_draw.cpp:894-913`).

- [ ] **Step 4: Build, test, run Wine, commit**

```sh
meson compile -C build && meson compile -C build-win32-x64-builtin && meson compile -C build-win32-x86-builtin
meson test -C build
python3 scripts/run_apps/run_experiment.py run app-d3d9-3dmark05
```

Expected: all pass. A constant-path defect shows as wrong transforms or wrong
colours, not a crash — inspect the screenshot, do not trust exit status alone.

```sh
git add -A src/d3d9 tests/native/bridge
git commit -m "refactor(pe): emit constant records directly as V2

appendSetConstRecord -- the single emitter behind all six VS/PS constant
kinds -- calls appendSetConstantsV2 through appendRecordV2 instead of
building a D9CCommandRecordSetConst for the shim to re-parse. Its guards
and sizeHint are unchanged, so flushConstShadow's split-sparse diagnostic
path, its telemetry, and seal cadence are all untouched."
```

---

## Task 7: The differential harness, and applystate through the new producer

**Files:**
- Create: `tests/native/bridge/pe_producer_differential_spec.cpp`
- Modify: `tests/native/bridge/meson.build`
- Modify: `src/d3d9/d3d9_pe_producer.hpp` / `.cpp` — add `buildSparseStateV2`
- Modify: `src/d3d9/d3d9_pe_device.cpp:10000` — the APPLY_STATE site
- Modify: `src/d3d9/d3d9_pe_chunk_v2_draw.cpp` — remove the APPLY_STATE case

**Interfaces:**
- Consumes: Tasks 1-4.
- Produces:
  ```cpp
  bool buildSparseStateV2(const PeHotStateShadow& shadow,
                          PeConstShadowBlock& constants,
                          const PeBindingView& bindings,
                          const PeDrawPayloads& payloads,
                          const PeDrawParams& params,
                          PeSparseScratch& scratch,
                          D9CCommandChunkWireDrawHeaderV2& header,
                          SparseStateV2Input& out) noexcept;
  ```
  Final signature; Tasks 8-9 add no parameters. **No `PeChunkContext`** — that
  belongs to `addChunkContextSections`, which Task 8 adds and only draw sites
  call. `constants` is non-const: like `foldConstShadowIntoDeltaSection`, the
  producer drains dirty ranges when the inline-delta path is active.

**The harness's legacy lane must be the real legacy path.** For APPLY_STATE that
is: `buildDrawPacketFromViews` → serialize as `D9CCommandRecordApplyState` →
`appendLegacyCommandRecordAsV2`. No chunk-context logic, no constant folding —
`chunkBarrierFlush` (`:9995-10006`) applies neither. Verify by reading the site
before writing the lane.

**Both lanes must publish wire object refs.** `appendLegacySparseRecord`
resolves packet handles through `lookupCachedWireObjectRef`
(`d3d9_pe_chunk_v2_draw.cpp:459-476`), keyed by the object pointer encoded in
the wire handle. A fixture that builds bare `PeWireObjectRef`s without calling
`publishCachedWireObjectRef` makes the legacy lane fail on every bound object.
`tests/native/bridge/pe_chunk_record_v2_value_spec.cpp:71,136-141` shows the
publication pattern — follow it.

- [ ] **Step 1: Write the failing differential**

Create `tests/native/bridge/pe_producer_differential_spec.cpp`. Copy the C-ABI
retain/release stubs and the publication pattern from
`pe_chunk_record_v2_value_spec.cpp:17-60` and `:71,136-141` — that file already
links the same builder.

```cpp
// pe_producer_differential_spec
//
// Runs the real old and new producers over one corpus and requires they agree
// on emitted V2 chunk bytes AND on the builder side effects bytes do not
// capture: retained objects, record/handle/payload counts, return value.
//
// Both lanes call functions from src/. Nothing here mirrors production logic
// -- that is why Task 4 rehosted the producer natively. Design §6.
//
// APPLY_STATE fixtures only in this task. Chunk-context and draw-header
// fixtures arrive with the code paths that use them, in Tasks 8 and 9.

#include "d3d9_pe_chunk_v2_builder.hpp"
#include "d3d9_pe_producer.hpp"
#include "d3d9_pe_producer_views.hpp"
// ... standard headers, TestFailure, check(), and the C-ABI object stubs ...

namespace pe = dxmt9::d3d9::pe;

struct Fixture {
  std::string name;
  PeHotStateShadow shadow{};
  PeConstShadowBlock constants{};
  pe::PeBindingView bindings{};
  pe::PeChunkContext chunk{};       // unused until Task 8
  pe::PeDrawPayloads payloads{};
  pe::PeDrawParams params{};
  bool forceFullSnapshot = false;
};

struct LaneResult {
  bool ok = false;
  std::vector<std::byte> bytes;
  std::size_t recordCount = 0, handleCount = 0, retainedObjectCount = 0;
};

LaneResult runLegacyLane(const Fixture& fixture) {
  LaneResult result;
  pe::CommandChunkV2Builder builder;
  D9CDrawPrimitivePacket packet;
  PeDecimatedScopeStats stats{};
  if (!pe::buildDrawPacketFromViews(
          fixture.shadow, fixture.bindings, fixture.params.primitiveType,
          fixture.params.startVertex, fixture.params.primitiveCount,
          fixture.forceFullSnapshot, stats, packet)) {
    return result;   // ok stays false
  }
  D9CCommandRecordApplyState record{};
  record.header.type = D9C_COMMAND_RECORD_APPLY_STATE;
  record.packet = packet;
  record.header.size = sizeof(record);
  std::vector<std::byte> recordBytes(sizeof(record));
  std::memcpy(recordBytes.data(), &record, sizeof(record));
  result.ok = pe::appendLegacyCommandRecordAsV2(builder, recordBytes);
  if (!result.ok) return result;
  result.recordCount = builder.recordCount();
  result.handleCount = builder.handleCount();
  result.retainedObjectCount = builder.retainedObjectCount();
  const auto sealed = builder.seal();
  result.bytes.assign(sealed.blob.begin(), sealed.blob.end());
  return result;
}

LaneResult runDirectLane(const Fixture& fixture) {
  LaneResult result;
  pe::CommandChunkV2Builder builder;
  PeConstShadowBlock constants = fixture.constants;   // lanes must not share
  pe::PeSparseScratch scratch{};
  pe::SparseStateV2Input state{};
  D9CCommandChunkWireDrawHeaderV2 header{};
  if (!pe::buildSparseStateV2(fixture.shadow, constants, fixture.bindings,
                              fixture.payloads, fixture.params, scratch,
                              header, state)) {
    return result;
  }
  result.ok = pe::appendApplyStateV2(builder, header.flags, state);
  if (!result.ok) return result;
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
  check(legacy.ok == direct.ok, fixture.name + ": lanes must agree on ok");
  if (!legacy.ok) return;
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
```

Fixtures for this task — APPLY_STATE shapes only:

```cpp
void emptyDelta() {
  Fixture f; f.name = "empty delta";
  requireLanesAgree(f);
}

void singleRenderStateDirty() {
  Fixture f; f.name = "one render state dirty";
  f.shadow.pendingRenderStates.set(7u, 1u);
  requireLanesAgree(f);
}

void everyCategoryDirty() {
  Fixture f; f.name = "every category dirty";
  f.shadow.pendingRenderStates.set(7u, 1u);
  f.shadow.pendingTextureMask = 0x1u;
  f.shadow.pendingStreamMask = 0x1u;
  f.shadow.pendingVs = true;
  f.shadow.pendingPs = true;
  f.shadow.pendingViewport = true;
  f.shadow.pendingScissor = true;
  f.shadow.pendingMaterial = true;
  f.bindings.textures[0] = publishedRef(&tex0, D9C_CHUNK_HANDLE_KIND_TEXTURE);
  f.bindings.streams[0].buffer =
      publishedRef(&vb0, D9C_CHUNK_HANDLE_KIND_BUFFER);
  f.bindings.vs = publishedRef(&vsObj, D9C_CHUNK_HANDLE_KIND_SHADER);
  f.bindings.ps = publishedRef(&psObj, D9C_CHUNK_HANDLE_KIND_SHADER);
  requireLanesAgree(f);
}

void renderStatesAtCap() {
  Fixture f; f.name = "render states at the section cap";
  for (std::uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_RENDER_STATES;
       ++slot) {
    f.shadow.pendingRenderStates.set(slot, slot + 1u);
  }
  requireLanesAgree(f);
}

void renderStatesOverCapFailBothLanes() {
  Fixture f; f.name = "render states over cap";
  // The table holds kPeRenderStateSlots (256) slots and the section cap is
  // D9C_DRAW_PACKET_MAX_RENDER_STATES (64), so 65 distinct sets do over-fill.
  for (std::uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_RENDER_STATES + 1u;
       ++slot) {
    f.shadow.pendingRenderStates.set(slot, slot + 1u);
  }
  check(!runLegacyLane(f).ok, "over-cap must fail on the legacy lane");
  check(!runDirectLane(f).ok, "over-cap must fail on the direct lane");
}

void unpublishedHandleBehavior() {
  Fixture f; f.name = "texture bound but never published";
  f.shadow.pendingTextureMask = 0x1u;
  f.bindings.textures[0] =
      unpublishedRef(&tex0, D9C_CHUNK_HANDLE_KIND_TEXTURE);
  // The legacy lane resolves through lookupCachedWireObjectRef and must fail.
  check(!runLegacyLane(f).ok, "unpublished handle must fail the legacy lane");
  // The direct lane holds the wrapper ref and may legitimately succeed. The
  // design's §6(3) requires agreement on failure only where both lanes can
  // observe the same condition. Record which behavior occurs in the task
  // report and assert that, rather than assuming symmetry.
}

void randomizedApplyStateSequences() {
  std::mt19937 rng(0xD9C0DEu);   // pinned seed: CI must be deterministic
  for (int i = 0; i < 256; ++i) {
    Fixture f;
    f.name = "randomized applystate " + std::to_string(i);
    const std::uint32_t n = rng() % D9C_DRAW_PACKET_MAX_RENDER_STATES;
    for (std::uint32_t k = 0; k < n; ++k) {
      f.shadow.pendingRenderStates.set(
          rng() % D9C_DRAW_PACKET_MAX_RENDER_STATES, rng());
    }
    f.shadow.pendingTextureMask =
        rng() & ((1u << D9C_DRAW_PACKET_MAX_TEXTURES) - 1u);
    f.shadow.pendingStreamMask =
        rng() & ((1u << D9C_DRAW_PACKET_MAX_STREAMS) - 1u);
    f.shadow.pendingVs = (rng() & 1u) != 0u;
    f.shadow.pendingPs = (rng() & 1u) != 0u;
    for (std::uint32_t k = 0; k < D9C_DRAW_PACKET_MAX_TEXTURES; ++k) {
      f.bindings.textures[k] =
          publishedRef(&tex0, D9C_CHUNK_HANDLE_KIND_TEXTURE);
    }
    for (std::uint32_t k = 0; k < D9C_DRAW_PACKET_MAX_STREAMS; ++k) {
      f.bindings.streams[k].buffer =
          publishedRef(&vb0, D9C_CHUNK_HANDLE_KIND_BUFFER);
      f.bindings.streams[k].offset = rng() % 1024u;
      f.bindings.streams[k].stride = 4u * (1u + rng() % 16u);
    }
    f.bindings.vs = publishedRef(&vsObj, D9C_CHUNK_HANDLE_KIND_SHADER);
    f.bindings.ps = publishedRef(&psObj, D9C_CHUNK_HANDLE_KIND_SHADER);
    requireLanesAgree(f);
  }
}
```

`publishedRef(object, kind)` calls `cacheWireObjectRef` /
`publishCachedWireObjectRef` and returns the resulting `PeWireObjectRef`;
`unpublishedRef` builds one without publishing. Write both against the existing
spec's pattern.

Meson target:

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

- [ ] **Step 2: Add a stub and confirm the differential fails**

```cpp
bool buildSparseStateV2(const PeHotStateShadow&, PeConstShadowBlock&,
                        const PeBindingView&, const PeDrawPayloads&,
                        const PeDrawParams&, PeSparseScratch&,
                        D9CCommandChunkWireDrawHeaderV2&,
                        SparseStateV2Input&) noexcept {
  return false;   // Task 7 stub -- the differential must fail here.
}
```

```sh
meson compile -C build dxmt9-pe-producer-differential-spec
meson test -C build dxmt9-pe-producer-differential-spec
```

Expected: FAIL on `emptyDelta` with "lanes must agree on ok". **If it passes,
stop** — the harness is not exercising the new lane, which is the exact defect
class this design exists to avoid.

- [ ] **Step 3: Implement `buildSparseStateV2`**

Fill each `SparseStateV2Input` category by walking the corresponding pending
mask or table in **ascending slot order**, writing compact entries into
`scratch`, and pointing the output span at the written prefix:

```cpp
std::size_t renderStateCount = 0;
if (shadow.pendingRenderStates.size() > scratch.renderStates.size()) {
  return false;   // over cap: seal the chunk rather than truncate
}
shadow.pendingRenderStates.forEach(
    [&](std::uint32_t state, std::uint32_t value) {
      scratch.renderStates[renderStateCount++] =
          D9CCommandChunkWireRenderStateV2{.state = state, .value = value};
    });
out.renderStates = std::span<const D9CCommandChunkWireRenderStateV2>(
    scratch.renderStates.data(), renderStateCount);
```

`FixedStateTable::forEach` iterates ascending, satisfying `orderedSlot`
(`d3d9_pe_chunk_v2_draw.cpp:190`). Every mask walk must go slot 0 upward.

**Streams and the index buffer here use the pending masks only** — no chunk
context. `out.streams` covers `shadow.pendingStreamMask`; the index binding
covers `shadow.pendingIb`. Task 8 adds the re-emission.

**Constants:** drain `constants` only when the inline-delta path is active,
mirroring `foldConstShadowIntoDeltaSection`
(`d3d9_pe_const_shadow.hpp:207-240`) — read `dirtyStart` / `dirtyEnd`, point the
span at `values.data() + dirtyStart * elemSize`, then `clear()`. On the default
path (`DXMT9_PE_INLINE_CONST_DELTA` unset) constants are **not** drained here;
`flushPendingConsts()` has already emitted standalone SET_CONST records.

**The snapshot flag:** set `header.flags`'s `FULL_SNAPSHOT` bit when both the
texture and stream masks are all-ones, matching the shim at
`d3d9_pe_chunk_v2_draw.cpp:867-873`. `runDirectLane` passes `header.flags` to
`appendApplyStateV2`, so the fixtures compare it.

- [ ] **Step 4: Run the differential to verify it passes**

```sh
meson test -C build dxmt9-pe-producer-differential-spec
```

Expected: PASS on all named fixtures plus 256 randomized iterations.

When a fixture fails, fix the producer, not the fixture. Changing a fixture to
match the new lane defeats the differential. The one legitimate reason is a
fixture encoding a state the device cannot produce — record that reasoning.

- [ ] **Step 5: Migrate the APPLY_STATE site**

At `:10000`, replace `buildDrawPrimitivePacket` + `appendCommandRecord` with
`buildSparseStateV2` + an `appendRecordV2` call whose emitter invokes
`appendApplyStateV2`. `sizeHint` stays `sizeof(D9CCommandRecordApplyState)`.

**Preserve the failure path**: on producer failure the site falls through to
`drainOversizedPendingStateAsApplyStateRecords()` (`:10008-10014`). It does not
return an error. That drain still builds legacy APPLY_STATE records and stays on
the legacy path until Task 10 — it is the over-cap regression path, not a hot
path.

Remove the `D9C_COMMAND_RECORD_APPLY_STATE` case from
`appendLegacyCommandRecordAsV2` **only if** the drain path no longer needs it.
If it does, leave the case and note that in the report.

- [ ] **Step 6: Build, test, run Wine, commit**

```sh
meson compile -C build && meson compile -C build-win32-x64-builtin && meson compile -C build-win32-x86-builtin
meson test -C build
python3 scripts/run_apps/run_experiment.py run app-d3d9-3dmark05
```

Expected: all pass. APPLY_STATE precedes every barrier, so a defect shows as
state leaking across Clear/StretchRect boundaries.

```sh
git add -A src/d3d9 tests/native/bridge
git commit -m "feat(pe): sparse-state producer, and applystate through it

buildSparseStateV2 fills SparseStateV2Input directly from the state and
const shadows, the binding view, and the draw params -- no fat packet in
between, and no chunk context, which production applies only at draw sites.

Gated by dxmt9-pe-producer-differential-spec: real old and new producers
over named plus 256 fixed-seed randomized APPLY_STATE fixtures, compared
on chunk bytes, record/handle counts, retained object count, and return
value. Confirmed failing against a stub first."
```

---

## Task 8: Migrate `draw` and `drawidx`, and add the chunk-context step

**Files:**
- Modify: `src/d3d9/d3d9_pe_producer.hpp` / `.cpp` — add `addChunkContextSections`
- Modify: `src/d3d9/d3d9_pe_device.cpp:9329`, `:9389`, and the helpers at `:9462-9484`
- Modify: `tests/native/bridge/pe_producer_differential_spec.cpp`
- Modify: `src/d3d9/d3d9_pe_chunk_v2_draw.cpp` — remove the two draw cases

**Interfaces:**
- Consumes: `buildSparseStateV2` (Task 7).
- Produces:
  ```cpp
  bool addChunkContextSections(const PeChunkContext& chunk,
                               const PeBindingView& bindings,
                               PeSparseScratch& scratch,
                               SparseStateV2Input& out) noexcept;
  ```

**This is the task the first draft of this plan got wrong.** Chunk context is a
draw-site step applied after the producer. The differential grows a second lane
pair mirroring that composition — producer, then context — on both sides.

### H5. Task 10's deletion list conflicts with keeping the differential alive

Tasks 8 and 9 deliberately did NOT delete the shim's draw/UP cases,
`appendLegacySparseRecord`, or `populateLegacySparseState`, even though both
tasks' step 5 called for it. They cannot be deleted while the differential
exists: its legacy lanes feed real draw and UP records through
`appendLegacyCommandRecordAsV2`, which is the whole reason the comparison means
anything. Task 9's plan text claiming "after this task appendLegacySparseRecord
has no callers" is now false -- the tests are the callers.

So Task 10 has to choose, explicitly:

1. **Keep the shim as test-only code**, moved or clearly marked as such, and keep
   the differential as a standing regression gate. The cost is that the legacy
   record structs and the fat packet cannot be deleted either, since the shim
   parses them -- which is most of what Task 10 set out to remove.
2. **Delete the shim and retire the differential**, replacing it with pins that
   assert the sparse output directly rather than against a legacy oracle. The
   cost is losing the oracle exactly when the last of the migration lands, and
   every fixture becomes a snapshot of current behaviour rather than a
   comparison.

Option 1 defeats the purpose; option 2 removes the safety net at the riskiest
moment. A plausible middle path is to delete the production shim call sites and
the fat packet from the PE record path while keeping the legacy structs plus
`appendLegacyCommandRecordAsV2` compiled only into the test target, and to say so
in one place so the next reader does not think it is dead code awaiting deletion.

Decide before writing Task 10, not during it.

### H4. RESOLVED -- the indexed draw site regressed GT1 on a by-value stamp

**Resolved in `7b198453`.** Both draw sites are wired and GT1/GT2/GT3/SFIV are
visually verified.

**Cause.** `buildSparseStateForRecord` took a separate `recordType` argument and
stamped it onto `params` -- but `params` came in BY VALUE, so the stamp landed on
the local copy and the call sites' own `params` still carried `recordType == 0`.
`addChunkContextSections` reads exactly that one field, and on a non-indexed
verdict it does not merely skip the index section: it rebuilds the span as
`first(0)`, wiping the section `buildSparseStateV2` had already emitted for
`pendingIb`. `SetIndices` records nothing standalone in chunk mode, so every
indexed draw replayed against a stale index buffer.

**Fix.** `params.recordType` is now the only record-type input -- two sources of
truth for one value, one of them silently write-only, collapsed to one. Guards at
both ends refuse `0`: the producer with a release-safe log-once, the device
forwarder as the choke point covering `chunkBarrierFlush`'s APPLY_STATE, which
never reaches the producer guard. Pinned in `pe_producer_views_spec`.

**Two corrections to what this section originally claimed.** The "ruled out by
experiment" list below was partly wrong. The probe that supposedly cleared the
emit decision set `emitIndex = indexedDraw && ibBound` -- still ANDed with the
broken predicate, so it tested nothing, and the 27.0 -> 43.0 luma move read as
"still corrupt" was frame-phase noise. A probe that cannot fail for the reason
under test is not evidence. And the divergence was NOT invisible to the
differential in principle: forcing `indexedDraw = false` in the producer fails 20
fixtures immediately. It was invisible because the defect lived in the device
forwarder's by-value seam, which no fixture threaded. Blind spot 3 below
(cross-record sequencing) was never implicated; blind spot 2 (the derivation of
`PeChunkContext`) was the right neighbourhood but not the bug.

The original record follows, kept because the bisection was sound even where the
conclusions drawn from it were not:

| state | GT1 |
|---|---|
| mechanism only, no wiring (`321f67dc`) | correct, luma 61-85 |
| non-indexed site migrated, indexed legacy | **correct** |
| both sites migrated | **CORRUPT** — luma 27.0, geometry smeared into long stretched triangles, HUD digits garbled |

So the defect is isolated to `appendDrawIndexedPrimitiveRecord`, and it is NOT:

- **the index-section emit decision.** Forcing `emitIndex` unconditionally true
  for indexed draws left the corruption unchanged (luma 43.0, same smearing).
- **the tracking update.** With always-emit, `indexSectionEmitted` is always
  true, so `submittedIndexBuffer*` is always refreshed — still corrupt.
- **a missing index format.** `D9CCommandChunkWireIndexBindingV2` is
  `{valid, handleIndex}` only; format rides on the buffer object.
- **an unusable wire ref.** The release-safe log-once added in Task 7 never
  fired, so every emitted ref had a valid identity.
- **reentrancy into the shared scratch.** `flushPendingCommandChunk` does not
  reach `buildSparseStateForRecord` or `chunkBarrierFlush`, and the recorder lock
  is held throughout.
- **record content.** The differential compares emitted chunk BYTES for indexed
  draws, across 20 named fixtures including every chunk-context leg in isolation,
  and passes. Whatever differs is at the call site, not in the producer.

`status: pass` reported nothing again, for the third time in this migration. Only
`mean_luma` (27.0 against a 61-85 baseline) and the screenshot showed it.

**What the differential structurally cannot see, and so where to look next:**

1. `sizeHint` is the one call-site input with no test coverage. The indexed site
   passes `sizeof(D9CCommandRecordDrawIndexedPrimitive) + sparseConstPayloadBytes()`.
   If that moves a chunk boundary the record content stays identical while the
   chunk *stream* changes — invisible to a per-record byte diff, and a wrong
   boundary is exactly the kind of thing that makes a later draw replay against
   state a previous chunk owned.
2. The differential drives `PeChunkContext` from fixture values;
   `currentChunkContext()` derives it from `commandChunkV2_.referencesObject`.
   A wrong *derivation* passes every fixture.
3. Cross-record sequencing. Each fixture builds ONE record into a fresh builder.
   Nothing exercises "draw N's decisions given what draws 1..N-1 put in this
   chunk", which is precisely what the retention legs are about.

A runtime differential — build both records at the live call site and compare
bytes, behind an env flag — is the tool that would settle it, and is the
recommended next step rather than more inspection.

### Three hazards found before starting, from the Tasks 5-7 review

All three were verified against source. Resolve each explicitly; none is a
detail that can be worked out while editing.

**H1. `PeChunkContext` cannot express the index-buffer retention leg.**
Production derives the emitted `ibValid` from *two* independent conditions:

- at the call site, `pendingIb || !submittedIndexBufferKnown_ ||
  submittedIndexBufferWireValue_ != ibWireValue`; and
- afterwards, in the writer lambda, `populateDrawPacketIndexDependency`
  (`d3d9_pe_draw_packet.hpp:41-47`) sets `ibValid` when the handle is non-null
  **and the destination chunk does not already reference it** —
  `if (!retained && !wireHandleIsNull(packet.ibHandle)) packet.ibValid = 1u;`

`PeChunkContext` carries `retainedStreamMask` for streams but has no
counterpart for the index buffer, so the predicate this plan sketched
(`!chunk.indexBufferKnown || submittedIndexBufferWire != wireValueOf(...)`)
reproduces only the first condition. Dropping the second means a fresh chunk's
first indexed draw with a known, unchanged IB emits no index section, so that
chunk never retains the buffer — the in-flight-free hazard the re-emit exists
to prevent. **Add an `indexBufferRetained` flag to `PeChunkContext`** and give
it its own differential fixtures (retained / not-retained x known / unknown).

**H2. `PeBindingView.streams` is authoritative only for *pending* slots.**
`populateBindingView` deliberately fills a stream slot only when its pending
bit is set (that masking is what kept the Task 4 cost regression out). But
stream re-emission needs *every currently bound* stream: production reads a
separately captured `currentDrawStreamSources()` (`d3d9_pe_device.cpp:9292`,
`:9352`), not the view. A non-pending view slot holds whatever the last build
that touched it left there, which coincides with the current binding in steady
state but not after `clearPeStateTracking()` / `Reset`, which clears
`streamOff_` / `streamStr_` and the shadow but not `peBindingView_`.

So `addChunkContextSections(chunk, bindings, ...)` as sketched is wrong: the
`bindings` it receives cannot answer "what is bound in slot N". Either pass the
stream sources explicitly, or make the view fill authoritative for all slots in
the draw path and re-measure the `raw*` cost that masking was protecting. Decide
which before writing the function; do not let it read stale view slots.

**H3. The constant decision this plan deferred is now due.**
`buildSparseStateV2` emits no constant sections at all — its body is
`(void)constants;`. That is correct for APPLY_STATE because
`chunkBarrierFlush` always calls `flushPendingConsts()` first. But the draw
sites **skip** that flush under `DXMT9_PE_INLINE_CONST_DELTA=1`
(`d3d9_pe_device.cpp:9280`, `:9342`) and fold the dirty ranges into the record
instead — and the fold writes into `packet.constDeltaSections` of a fat packet
that will not exist once these sites migrate. Wiring a draw site to the current
producer under that env drops the constants silently.

Pick one and say so in the task report:
1. implement the drain in `buildSparseStateV2` (it already takes `constants`
   non-const for exactly this), with differential fixtures covering all six
   ranges under the env; or
2. make the migrated draw sites always `flushPendingConsts()`, retiring the
   inline-delta fold — a behaviour change to a documented env knob, so it needs
   saying out loud rather than happening by omission.

**Also carried forward:** the producer writes into shared members
(`peSparseScratch_` / `peSparseState_`) that now live across the CapacityPre
flush window inside `appendRecordV2`, where the old code used stack locals.
`recorderMutex_` is recursive, so any reentrant path that rebuilds between build
and emit would corrupt the in-flight record. The Task 7 review found one real
bug of this shape already (a stale vertex-input ref in reused scratch); assume
more, and keep the differential's scratch shared so it can see them.

- [ ] **Step 1: Extend the differential with draw lanes**

Generalize `runLegacyLane` / `runDirectLane` to switch on
`fixture.params.recordType`:

- For `D9C_COMMAND_RECORD_APPLY_STATE`, keep Task 7's behavior exactly.
- For the two draw types, the **legacy** lane must reproduce the call site:
  after `buildDrawPacketFromViews`, apply
  `populatePendingChunkDrawStreamDependencies` and, for indexed,
  `populatePendingChunkDrawIndexDependency` to the packet, then serialize and
  convert. Those two helpers are currently private methods of the device class
  — move them into `d3d9_pe_producer.cpp` as free functions taking
  `(const PeChunkContext&, const PeBindingView&, D9CDrawPrimitivePacket&)` as
  part of this step, so the lane calls production code rather than a copy.
  **If they cannot be moved without dragging device state along, stop and
  report.** A hand-written copy in the test is the mirror trap, and the task
  needs rethinking rather than a workaround.
- The **direct** lane calls `buildSparseStateV2`, then
  `addChunkContextSections`, then `appendSparseRecordV2` with the header.

New fixtures:

```cpp
void indexedDrawWithBaseVertex() {
  Fixture f; f.name = "indexed draw, base vertex and index range";
  f.params.recordType = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
  f.params.primitiveType = 4u;   // D3DPT_TRIANGLELIST
  f.params.primitiveCount = 128u;
  f.params.baseVertex = 64;
  f.params.minVertex = 8u;
  f.params.numVertices = 256u;
  f.params.startIndex = 12u;
  f.bindings.indexBuffer = publishedRef(&ib0, D9C_CHUNK_HANDLE_KIND_BUFFER);
  f.shadow.pendingIb = true;
  requireLanesAgree(f);
}

void nonIndexedDraw() {
  Fixture f; f.name = "non-indexed draw";
  f.params.recordType = D9C_COMMAND_RECORD_DRAW_PRIMITIVE;
  f.params.primitiveType = 5u;   // D3DPT_TRIANGLESTRIP
  f.params.startVertex = 32u;
  f.params.primitiveCount = 64u;
  f.params.stride = 32u;
  f.shadow.pendingStreamMask = 0x1u;
  f.bindings.streams[0].buffer =
      publishedRef(&vb0, D9C_CHUNK_HANDLE_KIND_BUFFER);
  f.bindings.streams[0].stride = 32u;
  requireLanesAgree(f);
}

void indexedDrawNegativeBaseVertex() {
  Fixture f; f.name = "indexed draw, negative base vertex";
  f.params.recordType = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
  f.params.baseVertex = -32;     // int32_t in the wire draw header
  f.params.primitiveCount = 16u;
  f.bindings.indexBuffer = publishedRef(&ib0, D9C_CHUNK_HANDLE_KIND_BUFFER);
  f.shadow.pendingIb = true;
  requireLanesAgree(f);
}

// The §3(d) cases. They exist only because PeChunkContext is a parameter.
void streamDirtyAndAlreadyRetained() {
  Fixture f; f.name = "stream 0 dirty and already retained";
  f.params.recordType = D9C_COMMAND_RECORD_DRAW_PRIMITIVE;
  f.shadow.pendingStreamMask = 0x1u;
  f.bindings.streams[0].buffer =
      publishedRef(&vb0, D9C_CHUNK_HANDLE_KIND_BUFFER);
  f.chunk.retainedStreamMask = 0x1u;
  requireLanesAgree(f);
}

void streamRetainedNotDirty() {
  Fixture f; f.name = "stream 0 retained but not dirty";
  f.params.recordType = D9C_COMMAND_RECORD_DRAW_PRIMITIVE;
  f.bindings.streams[0].buffer =
      publishedRef(&vb0, D9C_CHUNK_HANDLE_KIND_BUFFER);
  f.chunk.retainedStreamMask = 0x1u;
  requireLanesAgree(f);
}

void streamNeitherDirtyNorRetained() {
  Fixture f;
  f.name = "stream 0 bound, neither dirty nor retained: must re-emit";
  f.params.recordType = D9C_COMMAND_RECORD_DRAW_PRIMITIVE;
  f.bindings.streams[0].buffer =
      publishedRef(&vb0, D9C_CHUNK_HANDLE_KIND_BUFFER);
  f.chunk.retainedStreamMask = 0u;
  requireLanesAgree(f);
}

void indexBufferKnownUnchanged() {
  Fixture f; f.name = "index buffer known and unchanged: must not re-emit";
  f.params.recordType = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
  f.bindings.indexBuffer = publishedRef(&ib0, D9C_CHUNK_HANDLE_KIND_BUFFER);
  f.chunk.indexBufferKnown = true;
  // POINTER-valued wire, matching d3d9_pe_device.cpp:9401-9406. Using an
  // objectId here would let a producer that compares the wrong field pass.
  f.chunk.submittedIndexBufferWire = wireValueOf(f.bindings.indexBuffer);
  requireLanesAgree(f);
}

void indexBufferKnownChanged() {
  Fixture f; f.name = "index buffer known but changed: must re-emit";
  f.params.recordType = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
  f.bindings.indexBuffer = publishedRef(&ib0, D9C_CHUNK_HANDLE_KIND_BUFFER);
  f.chunk.indexBufferKnown = true;
  f.chunk.submittedIndexBufferWire =
      wireValueOf(publishedRef(&ib1, D9C_CHUNK_HANDLE_KIND_BUFFER));
  requireLanesAgree(f);
}

void indexBufferNotKnown() {
  Fixture f; f.name = "index buffer not yet known: must emit";
  f.params.recordType = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
  f.bindings.indexBuffer = publishedRef(&ib0, D9C_CHUNK_HANDLE_KIND_BUFFER);
  f.chunk.indexBufferKnown = false;
  requireLanesAgree(f);
}
```

Extend `randomizedApplyStateSequences` into a variant that also picks a draw
record type and randomizes `chunk`.

`wireValueOf(ref)` returns `d9cWireHandleValue(toWireHandle(ref.object))` — the
same encoding `:9401-9406` stores.

- [ ] **Step 2: Run the differential to verify the new fixtures fail**

```sh
meson test -C build dxmt9-pe-producer-differential-spec
```

Expected: FAIL on `indexedDrawWithBaseVertex` — neither the draw header nor
`addChunkContextSections` exists yet. The APPLY_STATE fixtures must still pass.

- [ ] **Step 3: Implement the draw header and `addChunkContextSections`**

Populate `header` from `params`, using the field names at
`include/dxmt9/device_c.h:801-816`:

```cpp
header.primitiveType = params.primitiveType;
header.baseVertex = params.baseVertex;
header.minVertex = params.minVertex;
header.numVertices = params.numVertices;
header.startVertex = params.startVertex;
header.startIndex = params.startIndex;
header.primitiveCount = params.primitiveCount;
header.stride = params.stride;
header.indexFormat = params.indexFormat;
// sectionCount / sectionTableOffset / sectionPayloadOffset are filled by
// appendSparseRecordV2. Leave them zero.
```

`addChunkContextSections` adds stream bindings for the bound-but-not-retained
set that `buildSparseStateV2` did not already emit, and an index binding when
either leg of H1 fires:

```
!chunk.indexBufferKnown ||
chunk.submittedIndexBufferWire != wireValueOf(bindings.indexBuffer) ||
(!chunk.indexBufferRetained && bindings.indexBuffer.object != nullptr)
```

The third clause is the retention leg H1 describes; omitting it is silent and
only shows up as a use-after-free under load. Which source answers "what is
bound in slot N" for the stream set depends on the H2 decision. Both must keep
sections in ascending slot order after merging, or `orderedSlot` rejects the
record.

- [ ] **Step 4: Run the differential to verify it passes**

```sh
meson test -C build dxmt9-pe-producer-differential-spec
```

Expected: PASS on every fixture.

- [ ] **Step 5: Migrate the two call sites**

At `:9329` (drawidx) and `:9389` (draw), replace the packet build and
`appendCommandRecordDirect` with `buildSparseStateV2` +
`addChunkContextSections` + an `appendRecordV2` emitter calling
`appendSparseRecordV2`. `sizeHint` stays the legacy record `sizeof`.

Preserve three orderings:

1. `PeChunkContext` is filled **inside the emitter**, after any CapacityPre
   flush has resealed the chunk — matching where
   `populatePendingChunkDrawStreamDependencies` runs today (`:9349`, `:9422`,
   inside the writer lambda).
2. `submittedIndexBufferKnown_` / `submittedIndexBufferWireValue_` update only
   on success (`:9443-9449`); pending bits clear only on success.
3. `flushPendingConsts()` still runs before the draw (`:9321-9326`) on the
   default path. Do **not** move constants into the draw record — the
   inline-delta fold at `:9334-9358` stays behind its env flag.

Then remove the two draw cases from `appendLegacyCommandRecordAsV2`.

- [ ] **Step 6: Build, test, run Wine, commit**

```sh
meson compile -C build && meson compile -C build-win32-x64-builtin && meson compile -C build-win32-x86-builtin
meson test -C build
python3 scripts/run_apps/run_experiment.py run app-d3d9-3dmark05
DXMT_3DMARK05_ARGS=-gt2 python3 scripts/run_apps/run_experiment.py run app-d3d9-3dmark05
python3 scripts/run_apps/run_experiment.py run app-d3d9-sfiv-benchmark
```

Expected: all pass; `v0.0.3` anchor match on each. This is the highest-risk
migration — inspect screenshots for missing geometry and state leaking between
draws, not just exit status.

```sh
git add -A src/d3d9 tests/native/bridge
git commit -m "refactor(pe): emit draw and drawidx through the sparse producer

The two hottest record types now go peState_ -> SparseStateV2Input -> V2
sections. Chunk-context re-emission moves into addChunkContextSections, a
separate step the draw sites call after the producer -- mirroring
production, where populatePendingChunkDrawStreamDependencies runs in the
writer lambda and never for APPLY_STATE.

Constants stay on the default standalone-record path; the inline-delta fold
remains behind DXMT9_PE_INLINE_CONST_DELTA.

Differential extended with draw-header and chunk-context fixtures,
including the pointer-valued index-buffer wire comparison."
```

---

## Task 9: Migrate `drawUP` and `drawidxUP`

**Files:**
- Modify: `src/d3d9/d3d9_pe_device.cpp:9527`, `:9627`
- Modify: `tests/native/bridge/pe_producer_differential_spec.cpp`
- Modify: `src/d3d9/d3d9_pe_chunk_v2_draw.cpp` — remove the two UP cases and `appendLegacySparseRecord` / `populateLegacySparseState`

**Interfaces:**
- Consumes: Tasks 7-8.
- Produces: no new symbols. After this task `appendLegacySparseRecord` has no callers.

Rare on GT2 (`~0` appends/present) but carrying the inline payloads and the
save/restore dance. Low frequency, highest per-site subtlety.

- [ ] **Step 1: Add UP payload fixtures**

```cpp
void nonIndexedUpDraw() {
  Fixture f; f.name = "draw primitive UP with inline vertices";
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
  f.params.indexFormat = 101u;   // D3DFMT_INDEX16
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
  Fixture f; f.name = "draw primitive UP with zero vertex bytes";
  f.params.recordType = D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP;
  f.params.primitiveCount = 0u;
  f.params.stride = 16u;
  requireLanesAgree(f);
}
```

Add the two UP types to the randomized loop's record-type pool.

- [ ] **Step 2: Run the differential to verify the new fixtures fail**

```sh
meson test -C build dxmt9-pe-producer-differential-spec
```

Expected: FAIL on `nonIndexedUpDraw` — payload spans are not yet forwarded.

- [ ] **Step 3: Forward the payloads**

In `buildSparseStateV2`, set `out.upIndexData = payloads.upIndex` and
`out.upVertexData = payloads.upVertex`. `appendSparseRecordV2` computes the
offsets for the two UP record types (`d3d9_pe_chunk_v2_draw.cpp:202-442`) — do
not duplicate that logic in the producer.

- [ ] **Step 4: Run the differential to verify it passes**

```sh
meson test -C build dxmt9-pe-producer-differential-spec
```

- [ ] **Step 5: Migrate the two call sites**

At `:9527` and `:9627`, switch to `buildSparseStateV2` +
`addChunkContextSections` + `appendRecordV2`, passing the UP data through
`PeDrawPayloads`.

**The save/restore window at `:9511-9526` still applies.** The site temporarily
mutates `fvf_` / `vdecl_` / `vs_` and three pending bits and restores them
afterwards; the binding-view fill must run inside that window. Both sites also
pass a live `forceFullSnapshot` from
`appendDraw{,Indexed}PrimitiveUPRecordWithFvf` (`:9501`, `:9601`) — thread it
through, do not hardcode.

Then delete the two UP cases and, since the switch is now empty for sparse
types, `appendLegacySparseRecord` and `populateLegacySparseState`.

- [ ] **Step 6: Build, test, run Wine, commit**

```sh
meson compile -C build && meson compile -C build-win32-x64-builtin && meson compile -C build-win32-x86-builtin
meson test -C build
python3 scripts/run_apps/run_experiment.py run app-d3d9-3dmark05
python3 scripts/run_apps/run_experiment.py run app-d3d9-sfiv-benchmark
```

UP draws are rare on both workloads, so a green run is **weak evidence** here —
the differential fixtures are the real gate. Say that in the task report rather
than implying the Wine runs proved the UP path.

```sh
git add -A src/d3d9 tests/native/bridge
git commit -m "refactor(pe): emit UP draws through the sparse producer

drawUP and drawidxUP forward their inline payloads through PeDrawPayloads.
appendLegacySparseRecord and populateLegacySparseState now have no callers
and are deleted. The fvf_/vdecl_/vs_ save-restore window and the live
forceFullSnapshot argument are both preserved.

UP draws are rare on GT1/SFIV, so the differential fixtures rather than the
Wine runs are this task's real gate."
```

---

## Task 10: Delete the legacy format and the dead fat-packet ABI

**Files** — far wider than a first reading suggests:

- `src/d3d9/d3d9_pe_device.cpp` — `appendCommandRecord`, `appendCommandRecordRetained`, `appendCommandRecordDirect` and `legacyV2RecordScratch_`, `drainOversizedPendingStateAsApplyStateRecords` (migrate or delete)
- `src/d3d9/d3d9_pe_producer.{hpp,cpp}` — `buildDrawPacketFromViews` and the legacy chunk-context helpers moved there in Task 8
- `src/d3d9/d3d9_pe_chunk_v2_draw.cpp` — `appendLegacyCommandRecordAsV2`, `loadLegacy`, `legacyRange`; and its declaration at `d3d9_pe_chunk_v2_builder.hpp:237-239`
- `src/d3d9/d3d9_pe_draw_packet.hpp` — the whole `populateDrawPacket*` family
- `src/d3d9/device_c_chunk_replay.cpp` — `applyDrawPacketState*`, `commitChunkDrawDeltaMask`, `validateDrawPacketStateDelta`, the two bridge op implementations at `:1666-1681`
- `src/d3d9/device_c_record_utils.hpp` — `packetHasNoStateDelta`, `makeRunParam`
- `src/d3d9/device_c_record_{replay,hazard,validate}.cpp` — **surgically**: `replayInfoForCommandRecordType` (`device_c_record_replay.cpp:368`) is live from `device_c_chunk_replay.cpp:1506` and stays
- `src/d3d9/device_c_provider_macros.hpp:73-74`, `src/d3d9/device_c_provider_undefs.hpp:73-74`
- `src/d3d9/device_c_bridge_device_state_draw.cpp:272-280` — PE-side stubs typed on `D9CDrawPrimitivePacket*`
- `src/winemetal/winemetal_bridge.cpp:204-205`, `:319-320`, `:458`, `:461` — names and marshal switch
- `include/dxmt9/device_c.h` — the 16 legacy `D9CCommandRecord*` typedef structs, `D9CDrawPrimitivePacket`, `D9CDrawIndexedPrimitivePacket`, the two bridge op declarations at `:1643-1646`
- `tests/native/bridge/bridge_ops_spec.cpp:48` (`158u` → `156u`), `:93`, `:95`, `:103`
- `tests/native/bridge/pe_chunk_record_v2_value_spec.cpp` — the Task 5/6 equivalence cases and the `d3d9_pe_draw_packet.hpp` include
- `tests/native/bridge/pe_producer_differential_spec.cpp` — `runLegacyLane` and every fixture comparing against it
- `tests/native/bridge/pe_full_snapshot_equivalence_spec.cpp` — rewrite or retire (Step 4)

**Keep:** `D9CDrawPacketTextureStageState`, `D9CDrawPacketSamplerState`,
`D9CDrawPacketTransform` — `SparseStateV2Input` uses them
(`d3d9_pe_chunk_v2_builder.hpp:181-183`).

**The differential loses its legacy lane here.** That is expected and must be
handled deliberately, not by deletion: convert the corpus into a golden test
pinning the *new* producer's output per fixture, capturing the goldens from the
last commit before this one. State in the report that the old-vs-new property
ends here and what replaced it.

- [ ] **Step 1: Re-verify every zero-reference claim**

```sh
for sym in validateCommandRecord importedRecordIsDrawRunCandidate \
           drawPacketStateDeltaEquals collectDrawPacketResourceHazards \
           packetHasNoStateDelta makeRunParam applyDrawPacketState \
           replayInfoForCommandRecordType \
           dxmt9c_device_draw_primitive_packet dxmt9c_device_draw_primitive_chunk; do
  echo "=== $sym ==="; grep -rn "\b$sym\b" src/ tests/ include/ scripts/
done
```

Any symbol with a surviving caller stays, and the reason goes in the report.

- [ ] **Step 2: Determine whether the bridge layer is generated**

```sh
grep -rn "device_c_provider_macros\|winemetal_bridge\|gen_.*bridge" src/*/meson.build scripts/
```

If these are `custom_target` outputs, regenerate rather than hand-editing —
`agents/rules/codebase_conventions.rules.md` forbids editing generated files. If
they are checked in and hand-maintained (both are tracked in git), edit them and
say so. Resolve this before Step 3; it changes what Step 3 does.

- [ ] **Step 3: Delete callers before callees**

The order matters and the obvious one is backwards —
`appendLegacyCommandRecordAsV2` is still called by `appendCommandRecordDirect`
after Task 9:

1. `appendCommandRecord` / `appendCommandRecordRetained` /
   `appendCommandRecordDirect` and `legacyV2RecordScratch_` (callers).
2. `appendLegacyCommandRecordAsV2`, `loadLegacy`, `legacyRange`, and the
   builder-header declaration (callees).
3. The test files that use them — the differential's legacy lane, the
   value-spec equivalence cases.
4. `buildDrawPacketFromViews` and the legacy chunk-context helpers.
5. Unix-side dead symbols confirmed in Step 1, plus `applyDrawPacketState*`,
   `packetHasNoStateDelta`, `makeRunParam`.
6. `d3d9_pe_draw_packet.hpp`.
7. The bridge layer (per Step 2's finding) and `include/dxmt9/device_c.h`.
8. `bridge_ops_spec.cpp`'s opcode count and adjacency assertions.

Build after each numbered step, not only at the end.

- [ ] **Step 4: Rewrite or retire the full-snapshot spec**

`pe_full_snapshot_equivalence_spec.cpp` mirrors a producer that no longer
exists. Prefer **rewriting** it against `buildSparseStateV2`, comparing
delta-mode and snapshot-mode output for the same shadow — now possible because
the producer is natively callable, which is exactly why the mirror existed.
Retiring it is acceptable only if the differential's snapshot-flag coverage
already subsumes the contract. State the choice and the reasoning.

- [ ] **Step 5: Rebuild all lanes in lockstep and run every check**

```sh
meson compile -C build
meson compile -C build-x86_64-builtin
meson compile -C build-win32-x64-builtin
meson compile -C build-win32-x86-builtin
meson test -C build
bash scripts/check/verify_tla.sh
python3 scripts/check/audit_winemetal_install_names.py
git diff --check
```

Never use a bare `ninja src/winemetal/unix/winemetal.so` — it skips the
`winemetal_unix_install_name_fixup` stamp and produces bare-dep `.so` files that
fail the bridge handshake with `status=0xc0000003`. The install-name audit
matters here specifically because this task rebuilds the unix provider.

- [ ] **Step 6: Wine runtime evidence on all four workloads**

```sh
python3 scripts/run_apps/run_experiment.py run app-d3d9-3dmark05
DXMT_3DMARK05_ARGS="-gt2 -nosplash -nosysteminfo -noscreens" \
  python3 scripts/run_apps/run_experiment.py run app-d3d9-3dmark05
DXMT_3DMARK05_ARGS="-gt3 -nosplash -nosysteminfo -noscreens" \
  python3 scripts/run_apps/run_experiment.py run app-d3d9-3dmark05
python3 scripts/run_apps/run_experiment.py run app-d3d9-sfiv-benchmark
```

For each: `status: pass`, `gpu_command_buffer_errors = 0`, `v0.0.3` anchor
match.

Do **not** use GT2's `result.json` present count as an FPS metric — GT2 is a
fixed ~68 s timeline that hangs post-scene until the timeout kill, and SIGKILL
loses the final counter flush. Set `DXMT9_PERF_FRAME_SAMPLING=1` and compute
scene fps from the per-frame `wall_ms` samples.

- [ ] **Step 7: Measure, and report honestly**

Paired GT2 A/B against the pre-Task-1 commit, both on the `perf` profile, frame
sampling on, no gputrace. Capture the baseline first:

```sh
git stash && git checkout <pre-task-1-sha>
bash scripts/tools/run_3dmark05_perf_probe.sh --suffix legacy-removal-before \
  --frame 50 --no-gputrace --timeout 120 --keep-frontmost
git checkout - && git stash pop
bash scripts/tools/run_3dmark05_perf_probe.sh --suffix legacy-removal-after \
  --frame 50 --no-gputrace --timeout 120 --keep-frontmost
```

The design's §1 expectation is at most `3.62 ms` of a `53.2 ms` frame (`6.8%`),
with a fraction surviving because section encoding remains, and the sparse
family's own share unmeasured. **Report what the A/B actually shows.** If FPS
does not move, say so — the design states plainly that this work is justified
structurally and FPS was never the criterion. Do not convert a null into a claim.

Record the result as a new leaf under `docs/perfomance/state-churn-encode/`,
following the frontmatter and `source:` conventions of the existing leaves, with
`source:` pointing at the actual `experiments/output/...` directories the runs
produced.

- [ ] **Step 8: Commit**

```sh
git add -A
git commit -m "refactor(pe): delete the legacy record format and dead fat-packet ABI

Removes the 16 legacy D9CCommandRecord* structs, D9CDrawPrimitivePacket and
everything typed on it (d3d9_pe_draw_packet.hpp's populate helpers,
packetHasNoStateDelta, makeRunParam, the unix applyDrawPacketState* family),
the appendLegacyCommandRecordAsV2 shim and the appendCommandRecord* envelope
adapters, and the two zero-PE-caller bridge ops.

Blast radius includes the provider macro/undef headers, the PE-side bridge
stubs, winemetal_bridge.cpp's names and marshal switch, and bridge_ops_spec's
opcode count and adjacency assertions.

Kept: D9CDrawPacketTextureStageState / SamplerState / Transform, used by
SparseStateV2Input, and replayInfoForCommandRecordType, live from
device_c_chunk_replay.cpp.

The differential's old-vs-new property ends here; its corpus becomes a
golden test pinning the new producer's output."
```

---

## Self-Review

**Spec coverage.** Design §3's two-function split → Tasks 2, 7, 8. §4's Phase 1
→ Tasks 1-4. §5's family order → Tasks 5-9 (2b→5, 2c→6, 2d→7, 2e→8, 2f→9).
§6's differential → Task 7, with its three side-channel gates: seal cadence via
the `appendRecordV2` envelope (Task 3), retention/count comparison in
`LaneResult`, failure paths in `renderStatesOverCapFailBothLanes` and
`unpublishedHandleBehavior`. §7's deletions → Task 10.

**Corrections from the Tasks 5-7 review**, recorded so the remaining tasks are
read against what the code now does:

- `buildSparseStateV2` gained a `forceFullSnapshot` parameter. The signature in
  this plan omitted it, and snapshot mode silently emitted a delta (2548 bytes
  against 136) until the differential caught it.
- `populateBindingView` fills the FULL `PeWireObjectRef` via the `D3D9PeWire*`
  accessors, identity included. Filling only `object` made every record with a
  bound object fail through `failActiveRecord()` with no log line — GT1 died with
  "IDirect3DDevice9::Clear failed: Invalid call" while the harness still reported
  `status=pass`, because it does not gate on the rendered image. Task 4's review
  predicted this exactly; treat "the harness passed" as evidence of nothing on
  its own.
- The differential's scratch is SHARED, mirroring the reused device member. A
  fresh per-lane scratch hid a real stale-entry defect.
- `DXMT_ASSERT` is not a usable guard for anything that only manifests under
  Wine: all three PE/unix lanes are `buildtype=release` with
  `b_ndebug=if-release`, so it compiles to nothing there. Use a release-safe
  log-once.
- Task 8's three open hazards are written up in its own section. Read those
  before starting it.

**Corrections applied during execution, from the Tasks 1-3 implementation
review.** Recorded here so the remaining tasks are read against the corrected
text, not the original:

- The emitter contract is `HRESULT(builder, AppendPhaseTimer)`, enforced by a
  `static_assert`, not `bool(builder)`. The bool form silently converts a failed
  append to `S_OK`. Every emitter snippet in Tasks 5-9 was rewritten.
- Each emitter records `peAppendPhaseEncode_` around its own emission. Timing it
  centrally in the envelope would redefine `encode` to include the legacy
  adapter's resize and write, breaking the Task 10 A/B's comparability.
- `PeSparseScratch::vertexInputs` is **1**, not 2 — the section is
  `V2SectionRuleSingle` with `maxCount 1`, and FVF-versus-declaration is the
  entry's `kind` field. A 2-entry span is rejected by `validSectionCount` and
  the draw is dropped. Every scratch capacity is now `static_assert`ed against
  `v2SectionRule(kind)->maxCount` instead of a copied macro.
- Line-number citations in the new headers were replaced with symbol names. The
  numbers in *this plan* drift as tasks land; re-locate by symbol.

**Changes from this plan's first draft, all from its review:**

- Task 3 is new. Extracting the emit seam from `appendCommandRecordDirect`
  preserves the mutex, negotiation gate, both flushes, and seven telemetry sites
  by construction. The draft's `shouldFlushBeforeAppend` extraction would have
  dropped the CapacityPost flush and the append census.
- Chunk context is a separate function called only by draw sites. The draft
  merged it into the producer, which would have made APPLY_STATE emit sections
  production never emits and made the differential unsatisfiable.
- Constants stay on the standalone-record path. `DXMT9_PE_INLINE_CONST_DELTA`
  is off by default, so the draft's "constant folding disappears" would have
  changed the default wire stream.
- Task 6 replaces one emitter, `appendSetConstRecord`, not six sites.
- Task 5's site list is corrected: `:10037`-`:10150` are the over-cap
  APPLY_STATE drain, not non-draw records.
- Fixtures publish wire object refs, and the index-buffer comparison uses the
  pointer-valued wire, not `objectId`.
- Task 1's vertex-texture-sampler expectations are corrected: those slots sit
  above the fragment sampler block, not at 0..3.
- Task 4's substitution list gained the five things renaming does not cover, and
  its call-site table is per-site rather than one template.
- Task 10's file list roughly doubled and its deletion order inverted — callers
  before callees.

**Known-weak gates, stated rather than hidden.** Task 4 has no native coverage;
its substitute gates and their limits are in the task. Task 9's Wine runs are
weak evidence because UP draws are rare; the task says so.

**Two places where the plan tells the implementer to stop rather than
improvise.** Task 5's QueryIssue, if the `ForChild` V2 surface proves large, and
Task 8 Step 1 if the chunk-context helpers cannot move out of the device class.
Both would otherwise be resolved by writing production logic into a test — the
failure this design exists to prevent.

**Line numbers** were read from the tree at `455f1c56` and will drift. Treat them
as starting points; re-locate by symbol name when one does not match.
