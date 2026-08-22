// pe_typed_slot_spec
//
// Pins R-PE-TYPED-SLOTS: the typed slot-key layer added on top of the PE
// state shadow (d3d9_pe_state_shadow.hpp). Two things are proven:
//
//   1. Runtime equivalence -- every typed accessor reads/writes the exact
//      same storage the pre-existing untyped accessor does, so migrating a
//      caller to the typed surface cannot change behavior.
//   2. Compile-time safety -- the specific bug class the audit called out
//      (a sampler index used where a render-state slot was expected, a TSS
//      type used where a texture-stage index was expected, etc.) is
//      unrepresentable: static_assert on std::is_invocable_v shows the
//      typed accessors reject a foreign key type at compile time. These are
//      static_asserts, not a "must fail to compile" test file, because the
//      thing being proven is a property of the type system checked at this
//      TU's compile time, not a runtime behavior.
//
// Header-only: d3d9_pe_state_shadow.hpp has no windows.h / d3d9.h in its
// transitive include set (see pe_shadow_native_spec.cpp), so this spec
// builds the same way.

#include "d3d9_pe_state_shadow.hpp"

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

// ---------------------------------------------------------------------------
// Zero-overhead evidence for the key types themselves.
//
// Each key is `enum class Tag : std::uint32_t {}` with NO enumerators and NO
// user-provided constructors -- not a wrapper struct/class. The standard
// guarantees a scoped enum's object representation is exactly its fixed
// underlying type's (basic.compound / dcl.enum), so there is no wrapper-ABI
// question to hand-wave: these five checks are true by the language rule,
// and pinned here so a future edit that turns one of these into something
// class-like (a constructor, a base class, extra members) fails the build
// immediately instead of silently changing parameter-passing ABI.
#define DXMT9_PE_STATIC_ASSERT_ZERO_OVERHEAD_KEY(Key)                        \
  static_assert(sizeof(Key) == sizeof(std::uint32_t),                       \
                #Key " must be exactly one uint32_t wide");                 \
  static_assert(alignof(Key) == alignof(std::uint32_t),                     \
                #Key " must have uint32_t's alignment");                    \
  static_assert(std::is_trivially_copyable_v<Key>,                          \
                #Key " must be trivially copyable (register/stack "         \
                     "passable, no special member functions)");             \
  static_assert(std::is_standard_layout_v<Key>,                             \
                #Key " must be standard-layout (no hidden base/vtable "     \
                     "state)");                                             \
  static_assert(std::is_trivially_default_constructible_v<Key>,             \
                #Key " must have no user-provided constructor")

DXMT9_PE_STATIC_ASSERT_ZERO_OVERHEAD_KEY(RenderStateSlot);
DXMT9_PE_STATIC_ASSERT_ZERO_OVERHEAD_KEY(TextureStageIndex);
DXMT9_PE_STATIC_ASSERT_ZERO_OVERHEAD_KEY(TextureStageStateType);
DXMT9_PE_STATIC_ASSERT_ZERO_OVERHEAD_KEY(SamplerIndex);
DXMT9_PE_STATIC_ASSERT_ZERO_OVERHEAD_KEY(SamplerStateType);
DXMT9_PE_STATIC_ASSERT_ZERO_OVERHEAD_KEY(TransformState);

#undef DXMT9_PE_STATIC_ASSERT_ZERO_OVERHEAD_KEY

// Storage size must be byte-for-byte unchanged from before this change.
// FixedStateTable/FixedStateMatrix/FixedTransformTable/PeHotStateShadow
// themselves were NOT touched (no member added/removed/reordered) -- only
// new, separate view types were added alongside them -- so these literals
// are the exact sizes measured on this same compiler/ABI immediately before
// R-PE-TYPED-SLOTS landed (native arm64 host build,
// -std=c++20 -O2, this TU's compiler). A change here means the hot DOD
// tables grew or shrank, which this task must not do.
static_assert(sizeof(FixedStateTable<kPeRenderStateSlots>) == 1064,
              "FixedStateTable<kPeRenderStateSlots> storage size changed "
              "(was 1064 bytes before typed keys)");
static_assert(
    sizeof(FixedStateMatrix<kPeTextureStageSlots, kPeTextureStageStateSlots>) ==
        2184,
    "TSS FixedStateMatrix storage size changed (was 2184 bytes before "
    "typed keys)");
static_assert(
    sizeof(FixedStateMatrix<kPeSamplerSlots, kPeSamplerStateSlots>) == 1608,
    "sampler FixedStateMatrix storage size changed (was 1608 bytes before "
    "typed keys)");
static_assert(sizeof(FixedTransformTable) == 17072,
              "FixedTransformTable storage size changed (was 17072 bytes "
              "before typed keys)");
static_assert(sizeof(PeHotStateShadow) == 80160,
              "PeHotStateShadow storage size changed (was 80160 bytes "
              "before typed keys)");

// The views themselves are single-reference façades, not extra state: each
// must be exactly pointer-sized (a reference is implemented as a pointer),
// confirming they add no per-instance storage beyond "which table".
static_assert(sizeof(RenderStateTableView) == sizeof(void*),
              "RenderStateTableView must be a single reference, no added "
              "state");
static_assert(sizeof(TssTableView) == sizeof(void*),
              "TssTableView must be a single reference, no added state");
static_assert(sizeof(SamplerStateTableView) == sizeof(void*),
              "SamplerStateTableView must be a single reference, no added "
              "state");
static_assert(sizeof(TypedTransformTableView) == sizeof(void*),
              "TypedTransformTableView must be a single reference, no "
              "added state");

// ---------------------------------------------------------------------------
// Compile-time evidence: cross-index-space mixing is unrepresentable.
//
// std::is_invocable_v<MemberFnPtr, Object&, Args...> asks "would this
// expression compile", without actually compiling a failing call anywhere
// (that would abort the whole TU). A `false` result here is not a guess: if
// the language allowed the mismatched key through, this trait would report
// `true` and the assertion below it would fail the build.

// RenderStateTableView::get must accept its own key (RenderStateSlot) and
// must NOT accept SamplerIndex, TextureStageIndex, TextureStageStateType,
// SamplerStateType, or TransformState -- the exact "sampler index used as a
// render-state slot" class of bug the audit flagged.
static_assert(std::is_invocable_v<decltype(&RenderStateTableView::get),
                                   RenderStateTableView&, RenderStateSlot,
                                   std::uint32_t&>,
              "RenderStateTableView::get must accept RenderStateSlot");
static_assert(!std::is_invocable_v<decltype(&RenderStateTableView::get),
                                    RenderStateTableView&, SamplerIndex,
                                    std::uint32_t&>,
              "RenderStateTableView::get must reject SamplerIndex");
static_assert(!std::is_invocable_v<decltype(&RenderStateTableView::get),
                                    RenderStateTableView&, TransformState,
                                    std::uint32_t&>,
              "RenderStateTableView::get must reject TransformState");
static_assert(!std::is_invocable_v<decltype(&RenderStateTableView::set),
                                    RenderStateTableView&,
                                    TextureStageStateType, std::uint32_t>,
              "RenderStateTableView::set must reject TextureStageStateType");

// TssTableView::get(row, col) must accept (TextureStageIndex,
// TextureStageStateType) in that order and must NOT accept the axes
// swapped, and must NOT accept the unrelated sampler-matrix key pair --
// the "TSS type index used as a transform index" / axis-swap class of bug.
static_assert(std::is_invocable_v<decltype(&TssTableView::get), TssTableView&,
                                   TextureStageIndex, TextureStageStateType,
                                   std::uint32_t&>,
              "TssTableView::get must accept (TextureStageIndex, "
              "TextureStageStateType)");
static_assert(!std::is_invocable_v<decltype(&TssTableView::get), TssTableView&,
                                    TextureStageStateType, TextureStageIndex,
                                    std::uint32_t&>,
              "TssTableView::get must reject the axes swapped");
static_assert(!std::is_invocable_v<decltype(&TssTableView::get), TssTableView&,
                                    SamplerIndex, SamplerStateType,
                                    std::uint32_t&>,
              "TssTableView::get must reject the sampler-matrix key pair");

// SamplerStateTableView::get(row, col) must accept (SamplerIndex,
// SamplerStateType) and must NOT accept the TSS matrix's key pair.
static_assert(std::is_invocable_v<decltype(&SamplerStateTableView::get),
                                   SamplerStateTableView&, SamplerIndex,
                                   SamplerStateType, std::uint32_t&>,
              "SamplerStateTableView::get must accept (SamplerIndex, "
              "SamplerStateType)");
static_assert(!std::is_invocable_v<decltype(&SamplerStateTableView::get),
                                    SamplerStateTableView&, TextureStageIndex,
                                    TextureStageStateType, std::uint32_t&>,
              "SamplerStateTableView::get must reject the TSS-matrix key "
              "pair");

// TypedTransformTableView::get must accept TransformState and must NOT
// accept RenderStateSlot (D3DTS_* vs D3DRS_* are numerically overlapping
// small integers, exactly the kind of value that a stray uint32_t would let
// through silently).
static_assert(std::is_invocable_v<decltype(&TypedTransformTableView::get),
                                   TypedTransformTableView&, TransformState,
                                   D9CMatrix&>,
              "TypedTransformTableView::get must accept TransformState");
static_assert(!std::is_invocable_v<decltype(&TypedTransformTableView::get),
                                    TypedTransformTableView&, RenderStateSlot,
                                    D9CMatrix&>,
              "TypedTransformTableView::get must reject RenderStateSlot");

// A bare std::uint32_t (or int, or any other integral type) must not
// satisfy rawSlot() -- the overload set is closed over the six tag types,
// so nothing can "launder" a raw index into a typed accessor without going
// through one of the typed constructors (renderStateSlotKey, ...), each of
// which documents which untyped clamp/lookup function it wraps.
template<typename T, typename = void>
struct HasRawSlot : std::false_type {};
template<typename T>
struct HasRawSlot<T, std::void_t<decltype(rawSlot(std::declval<T>()))>>
    : std::true_type {};

static_assert(HasRawSlot<RenderStateSlot>::value,
              "rawSlot must accept RenderStateSlot");
static_assert(HasRawSlot<TextureStageIndex>::value,
              "rawSlot must accept TextureStageIndex");
static_assert(HasRawSlot<TextureStageStateType>::value,
              "rawSlot must accept TextureStageStateType");
static_assert(HasRawSlot<SamplerIndex>::value,
              "rawSlot must accept SamplerIndex");
static_assert(HasRawSlot<SamplerStateType>::value,
              "rawSlot must accept SamplerStateType");
static_assert(HasRawSlot<TransformState>::value,
              "rawSlot must accept TransformState");
static_assert(!HasRawSlot<std::uint32_t>::value,
              "rawSlot must reject a bare uint32_t");
static_assert(!HasRawSlot<int>::value, "rawSlot must reject a bare int");

// ---------------------------------------------------------------------------
// Runtime evidence: typed and untyped accessors observe the same storage.

void renderStateTypedMatchesUntyped() {
  PeHotStateShadow shadow{};
  const RenderStateSlot lighting = renderStateSlotKey(2u);  // D3DRS_ZENABLE-ish
  shadow.renderStateShadowTyped().set(lighting, 7u);

  std::uint32_t untypedValue = 0;
  check(shadow.renderStateShadow.get(2u, untypedValue),
        "value set through the typed view must be visible to the untyped "
        "get");
  check(untypedValue == 7u, "value must round-trip unchanged");

  std::uint32_t typedValue = 0;
  check(shadow.renderStateShadowTyped().get(lighting, typedValue),
        "typed get must see the value");
  check(typedValue == 7u, "typed get must match untyped get");

  // And the reverse direction: set through the untyped surface (as
  // d3d9_pe_device.cpp does), read through the typed one.
  shadow.pendingRenderStates.set(9u, 42u);
  std::uint32_t viaTyped = 0;
  check(shadow.pendingRenderStatesTyped().get(renderStateSlotKey(9u),
                                              viaTyped),
        "typed get must see a value set through the untyped surface");
  check(viaTyped == 42u, "value must be identical either way");

  // const-qualified access (the read path a `const PeHotStateShadow&`
  // caller, e.g. process-vertices, actually has).
  const PeHotStateShadow& constShadow = shadow;
  std::uint32_t constValue = 0;
  check(constShadow.renderStateShadowTyped().get(lighting, constValue),
        "const typed accessor must work");
  check(constValue == 7u, "const typed accessor must match");
}

void tssTypedMatchesUntyped() {
  PeHotStateShadow shadow{};
  const std::uint32_t stage = 3u;
  const std::uint32_t type = 5u;
  const auto stageKey = textureStageIndexKey(stage);
  const auto typeKey = textureStageStateTypeKey(type);
  check(rawSlot(stageKey) == textureStageSlot(stage),
        "textureStageIndexKey must match the untyped clamp function");
  check(rawSlot(typeKey) == textureStageStateSlot(type),
        "textureStageStateTypeKey must match the untyped clamp function");

  shadow.tssShadowTyped().set(stageKey, typeKey, 123u);
  std::uint32_t untypedValue = 0;
  check(shadow.tssShadow.get(rawSlot(stageKey), rawSlot(typeKey),
                             untypedValue),
        "value set through the typed matrix view must be visible untyped");
  check(untypedValue == 123u, "value must round-trip unchanged");

  // Clamping behavior must be preserved: an out-of-range stage/type clamps
  // to the last valid slot, exactly like the untyped clamp functions.
  const auto clampedStage = textureStageIndexKey(999u);
  check(rawSlot(clampedStage) == kPeTextureStageSlots - 1u,
        "out-of-range stage must clamp like textureStageSlot() does");
}

void samplerTypedMatchesUntyped() {
  PeHotStateShadow shadow{};
  SamplerIndex samplerIndex{};
  check(samplerIndexKey(2u, samplerIndex),
        "fragment sampler 2 must resolve to a SamplerIndex");
  check(rawSlot(samplerIndex) == 2u, "fragment samplers map identity-wise");

  SamplerIndex vertexSamplerIndex{};
  check(samplerIndexKey(257u, vertexSamplerIndex),
        "D3DVERTEXTEXTURESAMPLER0 (257) must resolve to a SamplerIndex");
  check(rawSlot(vertexSamplerIndex) == kPeFragmentSamplerSlots,
        "vertex texture sampler 0 sits just above the fragment block");

  SamplerIndex rejected{};
  check(!samplerIndexKey(999u, rejected),
        "an out-of-range sampler ordinal must be rejected, not clamped");

  SamplerStateType stateType{};
  check(samplerStateTypeKey(1u, stateType),  // D3DSAMP_ADDRESSU
        "D3DSAMP_ADDRESSU must resolve to a SamplerStateType");

  shadow.samplerStateShadowTyped().set(samplerIndex, stateType, 55u);
  std::uint32_t untypedValue = 0;
  check(shadow.samplerStateShadow.get(rawSlot(samplerIndex),
                                      rawSlot(stateType), untypedValue),
        "value set through the typed sampler matrix must be visible "
        "untyped");
  check(untypedValue == 55u, "value must round-trip unchanged");
}

void transformTypedMatchesUntyped() {
  PeHotStateShadow shadow{};
  const D9CMatrix view = identityTransformMatrix();
  const auto viewState = transformStateKey(2u);  // D3DTS_VIEW
  shadow.transformShadowTyped().set(viewState, view);

  D9CMatrix untypedValue{};
  check(shadow.transformShadow.get(2u, untypedValue),
        "value set through the typed transform view must be visible "
        "untyped");
  check(matrixEquals(untypedValue, view), "matrix must round-trip unchanged");

  D9CMatrix typedValue{};
  check(shadow.transformShadowTyped().get(viewState, typedValue),
        "typed get must see the value");
  check(matrixEquals(typedValue, view), "typed get must match untyped get");
}

int main() {
  try {
    renderStateTypedMatchesUntyped();
    tssTypedMatchesUntyped();
    samplerTypedMatchesUntyped();
    transformTypedMatchesUntyped();
  } catch (const TestFailure& failure) {
    std::cerr << "pe_typed_slot_spec FAILED: " << failure.what() << "\n";
    return 1;
  }
  std::cout << "pe_typed_slot_spec OK\n";
  return 0;
}
