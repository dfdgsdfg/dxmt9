#pragma once

#include "device_c_render_tape.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace dxmt9::test {

inline constexpr std::size_t kCpuSourceOracleMaxRows = 128u;
inline constexpr std::size_t kCpuSourceOracleMaxIdentitiesPerRow = 512u;
inline constexpr std::size_t kCpuSourceOracleMaxBytes =
    12u + kCpuSourceOracleMaxRows *
              (8u + kCpuSourceOracleMaxIdentitiesPerRow * 16u);

struct CpuSourceOracleIdentity {
  std::uint32_t kind = 0u;
  std::uint32_t generation = 0u;
  std::uint64_t objectId = 0u;
};

struct CpuSourceOracleRow {
  std::uint32_t event = 0u;
  std::vector<CpuSourceOracleIdentity> identities{};
};

inline bool cpuSourceOracleSameIdentity(
    const CpuSourceOracleIdentity& a,
    const D9CWireObjectIdentity& b) noexcept {
  return a.kind == b.kind && a.generation == b.generation &&
         a.objectId == b.objectId;
}

inline bool cpuSourceOracleIdentityLess(const CpuSourceOracleIdentity& a,
                                        const CpuSourceOracleIdentity& b) noexcept {
  return std::tie(a.kind, a.generation, a.objectId) <
         std::tie(b.kind, b.generation, b.objectId);
}

inline bool cpuSourceOracleIdentityLess(const D9CWireObjectIdentity& a,
                                        const D9CWireObjectIdentity& b) noexcept {
  return std::tie(a.kind, a.generation, a.objectId) <
         std::tie(b.kind, b.generation, b.objectId);
}

inline std::uint32_t cpuSourceOracleReadU32(std::span<const std::byte> bytes,
                                            std::size_t offset) noexcept {
  return std::to_integer<std::uint32_t>(bytes[offset]) |
         (std::to_integer<std::uint32_t>(bytes[offset + 1u]) << 8u) |
         (std::to_integer<std::uint32_t>(bytes[offset + 2u]) << 16u) |
         (std::to_integer<std::uint32_t>(bytes[offset + 3u]) << 24u);
}

inline std::uint64_t cpuSourceOracleReadU64(std::span<const std::byte> bytes,
                                            std::size_t offset) noexcept {
  std::uint64_t value = 0u;
  for (std::size_t index = 0u; index < sizeof(value); ++index) {
    value |= static_cast<std::uint64_t>(std::to_integer<unsigned>(
                    bytes[offset + index])) << (index * 8u);
  }
  return value;
}

inline bool decodeCpuSourceOracle(std::span<const std::byte> bytes,
                                  std::vector<CpuSourceOracleRow>& rows) {
  constexpr std::array<std::byte, 8u> magic{
      std::byte{'D'}, std::byte{'9'}, std::byte{'C'}, std::byte{'S'},
      std::byte{'R'}, std::byte{'C'}, std::byte{'0'}, std::byte{'1'}};
  if (bytes.size() < magic.size() + sizeof(std::uint32_t) ||
      std::memcmp(bytes.data(), magic.data(), magic.size()) != 0) {
    return false;
  }
  const auto rowCount = cpuSourceOracleReadU32(bytes, magic.size());
  if (rowCount == 0u || rowCount > kCpuSourceOracleMaxRows) return false;
  rows.clear();
  std::vector<CpuSourceOracleRow> parsed;
  parsed.reserve(rowCount);
  std::size_t offset = magic.size() + sizeof(std::uint32_t);
  std::uint32_t priorEvent = 0u;
  for (std::uint32_t rowIndex = 0u; rowIndex < rowCount; ++rowIndex) {
    if (bytes.size() - offset < 2u * sizeof(std::uint32_t)) return false;
    const auto event = cpuSourceOracleReadU32(bytes, offset);
    const auto identityCount = cpuSourceOracleReadU32(
        bytes, offset + sizeof(std::uint32_t));
    offset += 2u * sizeof(std::uint32_t);
    if (event == 0u || event <= priorEvent ||
        identityCount > kCpuSourceOracleMaxIdentitiesPerRow ||
        identityCount > (bytes.size() - offset) / 16u) {
      return false;
    }
    CpuSourceOracleRow row{.event = event};
    row.identities.reserve(identityCount);
    for (std::uint32_t identityIndex = 0u; identityIndex < identityCount;
         ++identityIndex) {
      const CpuSourceOracleIdentity identity{
          .kind = cpuSourceOracleReadU32(bytes, offset),
          .generation = cpuSourceOracleReadU32(bytes,
                                               offset + sizeof(std::uint32_t)),
          .objectId = cpuSourceOracleReadU64(bytes, offset + 8u)};
      offset += 16u;
      if (identity.kind != D9C_CHUNK_HANDLE_KIND_BUFFER ||
          identity.generation == 0u || identity.objectId == 0u ||
          (!row.identities.empty() &&
           !cpuSourceOracleIdentityLess(row.identities.back(), identity))) {
        return false;
      }
      row.identities.push_back(identity);
    }
    priorEvent = event;
    parsed.push_back(std::move(row));
  }
  if (offset != bytes.size()) return false;
  rows = std::move(parsed);
  return true;
}

inline bool cpuSourceOracleMatches(
    std::span<const D9CWireObjectIdentity> actual,
    const CpuSourceOracleRow& expected) noexcept {
  if (actual.size() != expected.identities.size()) return false;
  for (std::size_t index = 0u; index < actual.size(); ++index) {
    if (!cpuSourceOracleSameIdentity(expected.identities[index], actual[index]))
      return false;
  }
  return true;
}

}  // namespace dxmt9::test
