#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace dxmt9::encoders {

using DrawTextureDumpPolicyU64 = std::uint64_t;

inline constexpr DrawTextureDumpPolicyU64 kMaxDrawTextureDumpCount = 256u;

struct DrawTextureDumpPassIdentity {
  DrawTextureDumpPolicyU64 handle = 0;
  bool srgb = false;
  bool vertexStage = false;
  DrawTextureDumpPolicyU64 seq = 0;
  DrawTextureDumpPolicyU64 enc = 0;

  friend constexpr bool operator==(const DrawTextureDumpPassIdentity&,
                                   const DrawTextureDumpPassIdentity&) = default;
};

constexpr DrawTextureDumpPassIdentity makeDrawTextureDumpPassIdentity(
    DrawTextureDumpPolicyU64 handle, bool srgb, bool vertexStage,
    DrawTextureDumpPolicyU64 seq, DrawTextureDumpPolicyU64 enc,
    bool repeatPerPass) noexcept {
  return {
      .handle = handle,
      .srgb = srgb,
      .vertexStage = vertexStage,
      .seq = repeatPerPass ? seq : 0u,
      .enc = repeatPerPass ? enc : 0u,
  };
}

constexpr bool drawTextureDumpSlotValid(
    DrawTextureDumpPolicyU64 slot,
    DrawTextureDumpPolicyU64 textureStageCount) noexcept {
  return slot < textureStageCount;
}

constexpr bool drawTextureDumpRepeatPolicyValid(
    bool repeatPerPass, bool hasSeqMin, DrawTextureDumpPolicyU64 seqMin,
    bool hasSeqMax, DrawTextureDumpPolicyU64 seqMax, bool hasMaxDumps,
    DrawTextureDumpPolicyU64 maxDumps) noexcept {
  if (!repeatPerPass) return true;
  return hasSeqMin && hasSeqMax && hasMaxDumps && seqMin <= seqMax &&
         maxDumps != 0u && maxDumps <= kMaxDrawTextureDumpCount;
}

// Strict unsigned parsing shared by the diagnostics environment reader and
// pure policy tests. Scalar values reject signs, whitespace, and suffixes;
// lists admit only comma/semicolon/colon/whitespace-separated scalar values.
inline bool parseDrawTextureDumpU64(std::string_view text,
                                    DrawTextureDumpPolicyU64& value) noexcept {
  if (text.empty() || text.front() == '+' || text.front() == '-') return false;
  int base = 10;
  std::size_t offset = 0u;
  if (text.size() >= 2u && text[0] == '0' &&
      (text[1] == 'x' || text[1] == 'X')) {
    base = 16;
    offset = 2u;
  }
  if (offset == text.size()) return false;
  const auto first = text[offset];
  const bool firstDigit = (first >= '0' && first <= '9') ||
      (base == 16 && ((first >= 'a' && first <= 'f') ||
                      (first >= 'A' && first <= 'F')));
  if (!firstDigit) return false;
  DrawTextureDumpPolicyU64 parsed = 0u;
  const auto result = std::from_chars(text.data() + offset,
                                      text.data() + text.size(), parsed, base);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    return false;
  }
  value = parsed;
  return true;
}

inline bool parseDrawTextureDumpU64List(
    std::string_view text, std::span<DrawTextureDumpPolicyU64> values,
    std::size_t& count) noexcept {
  count = 0u;
  if (text.empty()) return false;
  std::size_t cursor = 0u;
  bool sawValue = false;
  while (cursor < text.size()) {
    while (cursor < text.size() &&
           (text[cursor] == ',' || text[cursor] == ';' ||
            text[cursor] == ':' || text[cursor] == ' ' ||
            text[cursor] == '\t' || text[cursor] == '\n' ||
            text[cursor] == '\r')) {
      ++cursor;
    }
    if (cursor == text.size()) break;
    const auto begin = cursor;
    while (cursor < text.size() && text[cursor] != ',' &&
           text[cursor] != ';' && text[cursor] != ':' &&
           text[cursor] != ' ' && text[cursor] != '\t' &&
           text[cursor] != '\n' && text[cursor] != '\r') {
      ++cursor;
    }
    DrawTextureDumpPolicyU64 value = 0u;
    if (!parseDrawTextureDumpU64(text.substr(begin, cursor - begin), value) ||
        count == values.size()) {
      return false;
    }
    values[count++] = value;
    sawValue = true;
  }
  return sawValue;
}

}  // namespace dxmt9::encoders
