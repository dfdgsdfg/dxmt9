#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace dxmt9::diagnostics {

inline constexpr std::array<std::string_view, 23> vertexSkinningProbeFields{
    "position", "vin0", "vin1", "vin2", "vin3", "a0",
    "bone_x0", "bone_y0", "bone_x1", "bone_y1", "bone_x2", "bone_y2",
    "projection192", "projection193", "projection194", "projection195",
    "half_pixel", "geometry_address", "skinned_position",
    "blended_row0", "blended_row1", "blended_row2", "pre_half_pixel_position"};

constexpr unsigned vertexProbeValueWords(bool skinningInputs) noexcept {
  return skinningInputs ? vertexSkinningProbeFields.size() * 4 : 4;
}

constexpr unsigned vertexProbeRecordWords(bool skinningInputs) noexcept {
  return vertexProbeValueWords(skinningInputs) * 2 + 1;
}

inline std::string vertexSkinningProbeOperand(char axis, unsigned row) {
  const auto index = std::string("clamp(a0.") + axis + " + " + std::to_string(row) + ", 0, 255)";
  return "((" + index + ") == 196 ? dxmt9_cdef196 : cFloat[" + index + "])";
}

// Closed generated two-bone skinning shape only. Capture an operand once and
// feed that SAME local value to the original expression, not a later reread of
// mutable GPU memory. This changes compilation and must be pixel-calibrated.
inline std::optional<std::string> stageVertexSkinningProbeInputs(std::string_view input) {
  if (input.find("dxmt9_input_") != std::string_view::npos) return std::nullopt;
  std::string result(input);
  const auto replaceOnce = [&](std::string_view from, std::string_view to) {
    const auto where = result.find(from);
    if (where == std::string::npos || result.find(from, where + from.size()) != std::string::npos)
      return false;
    result.replace(where, from.size(), to);
    return true;
  };
  std::string declarations;
  for (unsigned row = 0; row < 3; ++row) {
    for (char axis : {'x', 'y'}) {
      const auto operand = vertexSkinningProbeOperand(axis, row);
      const auto name = std::string("dxmt9_input_bone_") + axis + std::to_string(row);
      if (!replaceOnce(operand, name)) return std::nullopt;
      declarations += "  const float4 " + name + " = " + operand + ";\n";
    }
  }
  for (unsigned index = 192; index < 196; ++index) {
    const auto operand = "cFloat[" + std::to_string(index) + "]";
    const auto name = "dxmt9_input_projection" + std::to_string(index);
    if (!replaceOnce(operand, name)) return std::nullopt;
    declarations += "  const float4 " + name + " = " + operand + ";\n";
  }
  if (!replaceOnce("  out.position.xy += ffpVs.halfPixelFixup * out.position.w;",
                   "  out.position.xy += dxmt9_input_half_pixel.xy * out.position.w;")) return std::nullopt;
  declarations += "  const float4 dxmt9_input_half_pixel = float4(ffpVs.halfPixelFixup, 0.0f, 0.0f);\n";
  constexpr std::string_view first = "  // mul r0, c0, v3\n";
  if (!replaceOnce(first, declarations + std::string(first)) ||
      !replaceOnce("  // dp4 r0, v0, r0\n", "  const float4 dxmt9_input_blend0 = r[0];\n  // dp4 r0, v0, r0\n") ||
      !replaceOnce("  // dp4 r0, v0, r1\n", "  const float4 dxmt9_input_blend2 = r[1];\n  // dp4 r0, v0, r1\n"))
    return std::nullopt;
  return result;
}

inline std::string vertexSkinningProbeValues() {
  return "as_type<uint4>(out.position), as_type<uint4>(vin0), as_type<uint4>(vin1), "
      "as_type<uint4>(vin2), as_type<uint4>(vin3), as_type<uint4>(a0), "
      "as_type<uint4>(dxmt9_input_bone_x0), as_type<uint4>(dxmt9_input_bone_y0), "
      "as_type<uint4>(dxmt9_input_bone_x1), as_type<uint4>(dxmt9_input_bone_y1), "
      "as_type<uint4>(dxmt9_input_bone_x2), as_type<uint4>(dxmt9_input_bone_y2), "
      "as_type<uint4>(dxmt9_input_projection192), as_type<uint4>(dxmt9_input_projection193), "
      "as_type<uint4>(dxmt9_input_projection194), as_type<uint4>(dxmt9_input_projection195), "
      "as_type<uint4>(dxmt9_input_half_pixel), uint4(base, stride, uint(vertexIndex), vid), "
      "as_type<uint4>(r[0]), as_type<uint4>(dxmt9_input_blend0), as_type<uint4>(r[2]), "
      "as_type<uint4>(dxmt9_input_blend2), as_type<uint4>(outPosition)";
}

}  // namespace dxmt9::diagnostics
