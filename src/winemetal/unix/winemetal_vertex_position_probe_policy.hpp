#pragma once

#include "winemetal_vertex_skinning_probe_policy.hpp"

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace dxmt9::diagnostics {

inline std::optional<std::uint32_t> vertexProbeBound(std::string_view value,
                                                  std::uint32_t maximum) noexcept {
  std::uint32_t result = 0;
  if (value.empty()) return std::nullopt;
  const auto converted = std::from_chars(value.data(), value.data() + value.size(), result);
  if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size() ||
      result == 0 || result > maximum) return std::nullopt;
  return result;
}

inline bool vertexProbeDigestValid(std::string_view value) noexcept {
  if (value.size() != 64) return false;
  for (char c : value) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

inline bool vertexNoContractSelectionValid(std::string_view value) noexcept {
  if (value.empty() || value.size() > 8 * 65 - 1) return false;
  unsigned count = 0;
  std::string_view prior = value;
  while (!value.empty()) {
    const auto separator = value.find(',');
    const auto token = value.substr(0, separator);
    if (!vertexProbeDigestValid(token) || ++count > 8) return false;
    // A duplicate is an ambiguous request, not extra compilation authority.
    if (prior.substr(0, value.data() - prior.data()).find(token) != std::string_view::npos)
      return false;
    if (separator == std::string_view::npos) return true;
    value.remove_prefix(separator + 1);
  }
  return false;
}

inline bool vertexNoContractSelectionContains(std::string_view selection,
                                              std::string_view digest) noexcept {
  return vertexProbeDigestValid(digest) && vertexNoContractSelectionValid(selection) &&
      selection.find(digest) != std::string_view::npos;
}

inline bool vertexProbeDrawEligible(std::uint64_t indexCount, std::uint32_t requestedCount,
    std::uint32_t instances, std::int32_t baseVertex, std::uint32_t baseInstance,
    std::uint64_t maximumIndex, std::uint32_t capacity, bool sourceKnown,
    bool triangleList) noexcept {
  return sourceKnown && triangleList && requestedCount != 0 && requestedCount <= 24576 &&
      indexCount == requestedCount && indexCount % 3 == 0 &&
      instances == 1 && baseVertex == 0 && baseInstance == 0 && capacity != 0 &&
      capacity <= 8192 && maximumIndex < capacity;
}

// A cold, explicitly selected generated entry only. Never rewrite arbitrary MSL.
// Atomic min/max makes repeated indexed invocations defined and exposes conflicts.
inline std::optional<std::string> instrumentVertexPositionSource(
    std::string_view input, std::uint32_t capacity, bool skinningInputs = false) {
  if (capacity == 0 || capacity > 8192 || input.size() > 1024 * 1024 ||
      input.find("dxmt9_position_") != std::string_view::npos) return std::nullopt;
  std::string compact;
  compact.reserve(input.size());
  for (char c : input) {
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '\f' && c != '\v')
      compact += c;
  }
  if (compact.find("buffer(29)") != std::string::npos) return std::nullopt;
  std::optional<std::string> staged;
  if (skinningInputs) {
    if (capacity > 2048) return std::nullopt;
    staged = stageVertexSkinningProbeInputs(input);
    if (!staged) return std::nullopt;
    input = *staged;
  }
  constexpr std::string_view entry = "vertex VSOut dxmt9_vs(";
  const auto begin = input.find(entry);
  if (begin == std::string_view::npos || input.find(entry, begin + entry.size()) != std::string_view::npos)
    return std::nullopt;
  const auto signatureEnd = input.find(") {", begin);
  if (signatureEnd == std::string_view::npos ||
      input.substr(begin, signatureEnd - begin).find("uint vid [[vertex_id]]") == std::string_view::npos)
    return std::nullopt;
  constexpr std::string_view finish = "  return out;\n}";
  const auto end = input.find(finish, signatureEnd);
  if (end == std::string_view::npos || input.find(finish, end + finish.size()) != std::string_view::npos ||
      input.substr(signatureEnd + 3, end - signatureEnd - 3).find("return") != std::string_view::npos)
    return std::nullopt;
  auto suffix = input.substr(end + finish.size());
  const auto first = suffix.find_first_not_of(" \t\r\n");
  if (first != std::string_view::npos) {
    suffix.remove_prefix(first);
    constexpr std::string_view marker = "// decoded d3d hash ";
    if (!suffix.starts_with(marker)) return std::nullopt;
    suffix.remove_prefix(marker.size());
    if (suffix.empty()) return std::nullopt;
    std::uint64_t hash = 0;
    const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), hash);
    if (parsed.ec != std::errc{} || std::string_view(parsed.ptr,
        suffix.data() + suffix.size() - parsed.ptr).find_first_not_of(" \t\r\n") != std::string_view::npos)
      return std::nullopt;
  }
  std::string result(input);
  std::string observer =
      "  if (vid < " + std::to_string(capacity) + "u) {\n"
      "    uint4 dxmt9_position_bits = as_type<uint4>(out.position);\n"
      "    for (uint dxmt9_c = 0; dxmt9_c < 4u; ++dxmt9_c) {\n"
      "      atomic_fetch_min_explicit(dxmt9_position_probe + vid * 9u + dxmt9_c, dxmt9_position_bits[dxmt9_c], memory_order_relaxed);\n"
      "      atomic_fetch_max_explicit(dxmt9_position_probe + vid * 9u + 4u + dxmt9_c, dxmt9_position_bits[dxmt9_c], memory_order_relaxed);\n"
      "    }\n"
      "    atomic_fetch_add_explicit(dxmt9_position_probe + vid * 9u + 8u, 1u, memory_order_relaxed);\n"
      "  }\n";
  if (skinningInputs) {
    observer = "  if (vid < " + std::to_string(capacity) + "u) {\n"
        "    uint4 dxmt9_position_bits[23] = {" + vertexSkinningProbeValues() + "};\n"
        "    for (uint dxmt9_g = 0; dxmt9_g < 23u; ++dxmt9_g) {\n"
        "      for (uint dxmt9_c = 0; dxmt9_c < 4u; ++dxmt9_c) {\n"
        "        uint dxmt9_index = vid * 185u + dxmt9_g * 4u + dxmt9_c;\n"
        "        atomic_fetch_min_explicit(dxmt9_position_probe + dxmt9_index, dxmt9_position_bits[dxmt9_g][dxmt9_c], memory_order_relaxed);\n"
        "        atomic_fetch_max_explicit(dxmt9_position_probe + dxmt9_index + 92u, dxmt9_position_bits[dxmt9_g][dxmt9_c], memory_order_relaxed);\n"
        "      }\n"
        "    }\n"
        "    atomic_fetch_add_explicit(dxmt9_position_probe + vid * 185u + 184u, 1u, memory_order_relaxed);\n"
        "  }\n";
  }
  result.insert(end, observer);
  result.insert(signatureEnd, ", device atomic_uint* dxmt9_position_probe [[buffer(29)]]");
  return result;
}

// Compiler intervention for a digest-selected, closed generated VS entry.
// Reuse the entry/terminal-return guard, not its emitted observer. This changes
// arithmetic and must never be presented as passive probe calibration.
inline std::optional<std::string> vertexNoContractControlSource(std::string_view input) {
  if (input.find("#pragma") != std::string_view::npos ||
      !instrumentVertexPositionSource(input, 1)) return std::nullopt;
  return "#pragma clang fp contract(off)\n" + std::string(input);
}

}  // namespace dxmt9::diagnostics
