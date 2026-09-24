#pragma once

#include "dxmt9_prepared_encode_plan.hpp"
#include <algorithm>
#include <vector>

namespace dxmt9::encoders {

// Serial-reference metadata, deliberately not canonical identity or parallel
// scheduling authority. Only the counted rows/handles are initialized.
class PreparedSerialReads {
  struct BuildToken {};
 public:
  struct Row {
    std::array<std::uint64_t, 2> bloom{};
    std::uint32_t offset = 0, count = 0;
    bool eligible = false;
  };
  enum class Fault : std::uint8_t { None, Owner, Rows, Handles };
  explicit PreparedSerialReads(BuildToken) noexcept {}
  PreparedSerialReads(const PreparedSerialReads&) = delete;
  PreparedSerialReads& operator=(const PreparedSerialReads&) = delete;

  static std::shared_ptr<const PreparedSerialReads> build(
      core::SourcePayloadView source, PreparedSourceKey key,
      Fault fault = Fault::None, std::size_t handleLimit = 8192) noexcept {
    if (!key.valid() || !source.valid() || key.commandCount > 1024 ||
        key.firstCommand > source.commandCount() ||
        key.commandCount > source.commandCount() - key.firstCommand ||
        handleLimit > 8192) return {};
    try {
      if (fault == Fault::Owner) return {};
      auto result = std::make_shared<PreparedSerialReads>(BuildToken{});
      result->key_ = key;
      if (fault == Fault::Rows) return {};
      result->rows_.reserve(key.commandCount);
      if (fault == Fault::Handles) return {};
      constexpr std::size_t maxReads = 1 + core::kMaxStreams + core::kMaxTextures;
      result->handles_.reserve(std::min(handleLimit, key.commandCount * maxReads));
      for (std::uint32_t i = 0; i < key.commandCount; ++i) {
        const auto command = source.commandAt(key.firstCommand + i).command;
        Row row{};
        row.offset = static_cast<std::uint32_t>(result->handles_.size());
        if (command.kind == core::MetalCommandKind::DrawRun &&
            command.drawState.hot && command.drawState.shaderLayout &&
            command.drawState.debug && command.drawRunRecord &&
            !command.drawParams.empty()) {
          // Reuse Replay's raw read projection, including inactive bindings.
          // No alias resolution: makeDrawReadHazard consumes these exact keys.
          const auto reads = core::makeDrawEntryReadSet(command.drawState);
          if (!reads.complete() || reads.count > maxReads ||
              reads.count > handleLimit - result->handles_.size())
            return {};
          row.count = reads.count;
          row.eligible = true;
          for (std::uint32_t j = 0; j < reads.count; ++j) {
            result->handles_.push_back(reads.handles[j]);
            addHazardBloom(row.bloom, reads.handles[j]);
          }
        }
        result->rows_.push_back(row);
      }
      return result;
    } catch (const std::bad_alloc&) {
      return {}; // Unpublished prefix is discarded; reference remains available.
    }
  }

  const Row* row(PreparedSourceKey key, std::size_t commandIndex) const noexcept {
    if (key != key_ || commandIndex < key.firstCommand ||
        commandIndex - key.firstCommand >= rows_.size()) return nullptr;
    const auto& result = rows_[commandIndex - key.firstCommand];
    return result.eligible ? &result : nullptr;
  }
  std::span<const std::uint64_t> handles() const noexcept { return handles_; }
  std::size_t storageBytes() const noexcept {
    return sizeof(*this) + rows_.capacity() * sizeof(Row) +
        handles_.capacity() * sizeof(std::uint64_t);
  }
 private:
  PreparedSourceKey key_{};
  std::vector<Row> rows_;
  std::vector<std::uint64_t> handles_;
};

} // namespace dxmt9::encoders
