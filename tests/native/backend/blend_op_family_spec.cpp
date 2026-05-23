// P1-1 regression guard for D3DRS_BLENDOP / D3DRS_BLENDOPALPHA /
// D3DRS_SEPARATEALPHABLENDENABLE / D3DRS_SRCBLENDALPHA / D3DRS_DESTBLENDALPHA
// → MTLBlendOperation + separate-alpha lane wiring.
//
// The W site lives in `dxmt9_pipeline_cache.cpp::makeBlendAttachmentKeys`
// (descriptor build) + `dxmt9_format_convert.cpp::toBlendOperation`
// (D3D9 BlendOp → WMT enum). The flat blend-attachment key is hashed
// into the pipeline-cache key (see `ShaderVariantKeyHash`), so divergent
// blend ops/factors land in distinct cache entries — the per-RT key
// derivation covered here is what guarantees that.

#include <array>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "dxmt9/core.hpp"
#include "../../../src/dxmt9/dxmt9_format_convert.hpp"
#include "../../../src/dxmt9/dxmt9_pipeline_cache.hpp"

using namespace dxmt9::core;
using namespace dxmt9::core::fixture;

namespace {

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string message) {
  throw TestFailure(std::move(message));
}

void check(bool condition, std::string_view message) {
  if (!condition) {
    fail(std::string(message));
  }
}

template <typename A, typename B>
void checkEq(const A& left, const B& right, std::string_view message) {
  if (!(left == right)) {
    std::ostringstream out;
    out << message << " (left != right)";
    fail(out.str());
  }
}

struct FlatDrawFixture {
  FlatDrawStateRecord hot{};

  FlatDrawStateView view() const {
    return FlatDrawStateView{.hot = &hot};
  }
};

FlatDrawFixture makeFixture(const DrawDesc& desc) {
  return FlatDrawFixture{.hot = makeFlatDrawStateRecord(desc)};
}

auto makeBlend(const FlatDrawStateRecord& hot) {
  return dxmt9::pipeline::detail::makeBlendAttachmentKeys(
      FlatDrawStateView{.hot = &hot});
}

void testDefaultBlendDescriptor() {
  DrawDesc desc{};
  const auto keys = makeBlend(makeFixture(desc).hot);

  checkEq(keys[0].rgbBlendOperation,
          static_cast<u32>(BlendOp::Add),
          "default RGB blend op is Add");
  checkEq(keys[0].alphaBlendOperation,
          static_cast<u32>(BlendOp::Add),
          "default alpha blend op mirrors RGB (Add)");
  checkEq(keys[0].sourceRGBBlendFactor,
          static_cast<u32>(BlendFactor::One),
          "default source RGB blend factor is One");
  checkEq(keys[0].destinationRGBBlendFactor,
          static_cast<u32>(BlendFactor::Zero),
          "default destination RGB blend factor is Zero");
  checkEq(keys[0].sourceAlphaBlendFactor,
          static_cast<u32>(BlendFactor::One),
          "default source alpha blend factor mirrors RGB (One)");
  checkEq(keys[0].destinationAlphaBlendFactor,
          static_cast<u32>(BlendFactor::Zero),
          "default destination alpha blend factor mirrors RGB (Zero)");
}

void testBlendOpSubtractAppliesToRgb() {
  DrawDesc desc{};
  desc.rs.values[RS_BLEND_OP] = static_cast<u32>(BlendOp::Subtract);

  const auto keys = makeBlend(makeFixture(desc).hot);
  checkEq(keys[0].rgbBlendOperation,
          static_cast<u32>(BlendOp::Subtract),
          "RS_BLEND_OP=Subtract sets RGB lane to Subtract");
  // Without RS_SEPARATE_ALPHA_BLEND_ENABLE the alpha lane mirrors RGB.
  checkEq(keys[0].alphaBlendOperation,
          static_cast<u32>(BlendOp::Subtract),
          "alpha lane mirrors RGB when separate alpha is disabled");
}

void testSeparateAlphaLaneFullyOverrides() {
  DrawDesc desc{};
  desc.rs.values[RS_SEPARATE_ALPHA_BLEND_ENABLE] = 1u;
  desc.rs.values[RS_BLEND_OP] = static_cast<u32>(BlendOp::Add);
  desc.rs.values[RS_BLEND_OP_ALPHA] = static_cast<u32>(BlendOp::Max);
  desc.rs.values[RS_SRC_BLEND] = static_cast<u32>(BlendFactor::SrcAlpha);
  desc.rs.values[RS_DEST_BLEND] = static_cast<u32>(BlendFactor::InvSrcAlpha);
  desc.rs.values[RS_SRC_BLEND_ALPHA] = static_cast<u32>(BlendFactor::One);
  desc.rs.values[RS_DEST_BLEND_ALPHA] = static_cast<u32>(BlendFactor::One);

  const auto keys = makeBlend(makeFixture(desc).hot);
  checkEq(keys[0].rgbBlendOperation,
          static_cast<u32>(BlendOp::Add),
          "RGB lane retains Add when separate alpha overrides only alpha");
  checkEq(keys[0].sourceRGBBlendFactor,
          static_cast<u32>(BlendFactor::SrcAlpha),
          "RGB source factor unchanged by separate alpha");
  checkEq(keys[0].destinationRGBBlendFactor,
          static_cast<u32>(BlendFactor::InvSrcAlpha),
          "RGB destination factor unchanged by separate alpha");
  checkEq(keys[0].alphaBlendOperation,
          static_cast<u32>(BlendOp::Max),
          "alpha lane uses RS_BLEND_OP_ALPHA (Max) under separate alpha");
  checkEq(keys[0].sourceAlphaBlendFactor,
          static_cast<u32>(BlendFactor::One),
          "alpha lane uses RS_SRC_BLEND_ALPHA (One) under separate alpha");
  checkEq(keys[0].destinationAlphaBlendFactor,
          static_cast<u32>(BlendFactor::One),
          "alpha lane uses RS_DEST_BLEND_ALPHA (One) under separate alpha");
}

void testSeparateAlphaGateHonored() {
  DrawDesc desc{};
  // Alpha-family states set but the gate is off — they must be ignored.
  desc.rs.values[RS_SEPARATE_ALPHA_BLEND_ENABLE] = 0u;
  desc.rs.values[RS_BLEND_OP] = static_cast<u32>(BlendOp::RevSubtract);
  desc.rs.values[RS_BLEND_OP_ALPHA] = static_cast<u32>(BlendOp::Max);
  desc.rs.values[RS_SRC_BLEND] = static_cast<u32>(BlendFactor::SrcColor);
  desc.rs.values[RS_DEST_BLEND] = static_cast<u32>(BlendFactor::InvSrcColor);
  desc.rs.values[RS_SRC_BLEND_ALPHA] = static_cast<u32>(BlendFactor::One);
  desc.rs.values[RS_DEST_BLEND_ALPHA] = static_cast<u32>(BlendFactor::Zero);

  const auto keys = makeBlend(makeFixture(desc).hot);
  checkEq(keys[0].alphaBlendOperation,
          static_cast<u32>(BlendOp::RevSubtract),
          "alpha lane mirrors RGB blend op when separate gate is off");
  checkEq(keys[0].sourceAlphaBlendFactor,
          static_cast<u32>(BlendFactor::SrcColor),
          "alpha source factor mirrors RGB when separate gate is off");
  checkEq(keys[0].destinationAlphaBlendFactor,
          static_cast<u32>(BlendFactor::InvSrcColor),
          "alpha destination factor mirrors RGB when separate gate is off");
}

void testBlendOpWmtMapping() {
  // Every D3D9 BlendOp must round-trip to its WMT enum equivalent.
  // Out-of-range values fall back to Add (mirrors the existing
  // fall-through in `toBlendOperation`).
  checkEq(static_cast<int>(dxmt9::convert::toBlendOperation(static_cast<u32>(BlendOp::Add))),
          static_cast<int>(WMTBlendOperationAdd),
          "BlendOp::Add → WMTBlendOperationAdd");
  checkEq(static_cast<int>(dxmt9::convert::toBlendOperation(static_cast<u32>(BlendOp::Subtract))),
          static_cast<int>(WMTBlendOperationSubtract),
          "BlendOp::Subtract → WMTBlendOperationSubtract");
  checkEq(static_cast<int>(dxmt9::convert::toBlendOperation(static_cast<u32>(BlendOp::RevSubtract))),
          static_cast<int>(WMTBlendOperationReverseSubtract),
          "BlendOp::RevSubtract → WMTBlendOperationReverseSubtract");
  checkEq(static_cast<int>(dxmt9::convert::toBlendOperation(static_cast<u32>(BlendOp::Min))),
          static_cast<int>(WMTBlendOperationMin),
          "BlendOp::Min → WMTBlendOperationMin");
  checkEq(static_cast<int>(dxmt9::convert::toBlendOperation(static_cast<u32>(BlendOp::Max))),
          static_cast<int>(WMTBlendOperationMax),
          "BlendOp::Max → WMTBlendOperationMax");
  checkEq(static_cast<int>(dxmt9::convert::toBlendOperation(0u)),
          static_cast<int>(WMTBlendOperationAdd),
          "out-of-range BlendOp falls back to Add");
  checkEq(static_cast<int>(dxmt9::convert::toBlendOperation(0xffffu)),
          static_cast<int>(WMTBlendOperationAdd),
          "out-of-range BlendOp falls back to Add (high value)");
}

void testPipelineKeyDifferentiatesBlendOps() {
  // Distinct blend ops must land on distinct keys so the pipeline-cache
  // does not alias different blend semantics into one PSO.
  DrawDesc baseDesc{};
  baseDesc.rs.values[RS_BLEND_OP] = static_cast<u32>(BlendOp::Add);
  DrawDesc subDesc = baseDesc;
  subDesc.rs.values[RS_BLEND_OP] = static_cast<u32>(BlendOp::Subtract);

  const auto baseKeys = makeBlend(makeFixture(baseDesc).hot);
  const auto subKeys = makeBlend(makeFixture(subDesc).hot);
  check(!(baseKeys[0] == subKeys[0]),
        "different RS_BLEND_OP yields different BlendAttachmentKey");
  check(dxmt9::pipeline::BlendAttachmentKeyHash{}(baseKeys[0]) !=
            dxmt9::pipeline::BlendAttachmentKeyHash{}(subKeys[0]),
        "different RS_BLEND_OP yields different BlendAttachmentKey hash");

  DrawDesc alphaDesc = baseDesc;
  alphaDesc.rs.values[RS_SEPARATE_ALPHA_BLEND_ENABLE] = 1u;
  alphaDesc.rs.values[RS_BLEND_OP_ALPHA] = static_cast<u32>(BlendOp::Max);
  const auto alphaKeys = makeBlend(makeFixture(alphaDesc).hot);
  check(!(baseKeys[0] == alphaKeys[0]),
        "separate alpha lane override yields different BlendAttachmentKey");
  check(dxmt9::pipeline::BlendAttachmentKeyHash{}(baseKeys[0]) !=
            dxmt9::pipeline::BlendAttachmentKeyHash{}(alphaKeys[0]),
        "separate alpha lane override yields different BlendAttachmentKey hash");
}

}  // namespace

int main() {
  try {
    testDefaultBlendDescriptor();
    testBlendOpSubtractAppliesToRgb();
    testSeparateAlphaLaneFullyOverrides();
    testSeparateAlphaGateHonored();
    testBlendOpWmtMapping();
    testPipelineKeyDifferentiatesBlendOps();
  } catch (const TestFailure& failure) {
    std::cerr << "blend_op_family_spec failed: " << failure.what() << '\n';
    return 1;
  } catch (const std::exception& ex) {
    std::cerr << "blend_op_family_spec unexpected exception: " << ex.what() << '\n';
    return 1;
  }

  std::cout << "blend_op_family_spec passed\n";
  return 0;
}
