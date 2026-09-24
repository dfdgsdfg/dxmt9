#pragma once

#include "dxmt9_prepared_encode_plan.hpp"
#include <algorithm>
#include <vector>

namespace dxmt9::encoders {

class PreparedSerialReadsBuilder;

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
      Fault fault = Fault::None, std::size_t handleLimit = 8192) noexcept;

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
  friend class PreparedSerialReadsBuilder;
  PreparedSourceKey key_{};
  std::vector<Row> rows_;
  std::vector<std::uint64_t> handles_;
};

// Private mutable prefix: no immutable owner escapes until complete coverage.
// Failed appends do not stop the caller's mandatory resource-mark traversal.
class PreparedSerialReadsBuilder {
 public:
  using Fault = PreparedSerialReads::Fault;
  explicit PreparedSerialReadsBuilder(PreparedSourceKey key,
      Fault fault = Fault::None, std::size_t handleLimit = 8192) noexcept
      : handleLimit_(handleLimit) {
    if (!key.valid() || key.commandCount > 1024 || handleLimit > 8192) return;
    try {
      if (fault == Fault::Owner) return;
      auto result = std::make_shared<PreparedSerialReads>(PreparedSerialReads::BuildToken{});
      result->key_ = key;
      if (fault == Fault::Rows) return;
      result->rows_.reserve(key.commandCount);
      if (fault == Fault::Handles) return;
      result->handles_.reserve(std::min(handleLimit, key.commandCount * kMaxReads));
      storage_ = std::move(result);
    } catch (const std::bad_alloc&) {
      // The caller still marks every resource; only optional metadata failed.
    }
  }
  PreparedSerialReadsBuilder(const PreparedSerialReadsBuilder&) = delete;
  PreparedSerialReadsBuilder& operator=(const PreparedSerialReadsBuilder&) = delete;
  PreparedSerialReadsBuilder(PreparedSerialReadsBuilder&&) noexcept = default;
  PreparedSerialReadsBuilder& operator=(PreparedSerialReadsBuilder&&) noexcept = default;

  bool append(std::size_t ordinal, const core::MetalCommandView& command) noexcept {
    if (!storage_) return false;
    auto& s = *storage_;
    if (s.rows_.size() == s.key_.commandCount ||
        ordinal != s.key_.firstCommand + s.rows_.size()) return fail();
    PreparedSerialReads::Row row{};
    row.offset = static_cast<std::uint32_t>(s.handles_.size());
    if (command.kind == core::MetalCommandKind::DrawRun &&
        command.drawState.hot && command.drawState.shaderLayout &&
        command.drawState.debug && command.drawRunRecord &&
        !command.drawParams.empty()) {
      const auto reads = core::makeDrawEntryReadSet(command.drawState);
      if (!reads.complete() || reads.count > kMaxReads ||
          reads.count > handleLimit_ - s.handles_.size()) return fail();
      row.count = reads.count;
      row.eligible = true;
      for (std::uint32_t j = 0; j < reads.count; ++j) {
        s.handles_.push_back(reads.handles[j]);
        addHazardBloom(row.bloom, reads.handles[j]);
      }
    }
    // Constructor reserved the complete bound; these POD appends cannot grow.
    s.rows_.push_back(row);
    return true;
  }
  std::shared_ptr<const PreparedSerialReads> seal() noexcept {
    if (!storage_) return {};
    if (storage_->rows_.size() != storage_->key_.commandCount) {
      fail();
      return {};
    }
    return std::move(storage_);
  }
 private:
  bool fail() noexcept { storage_.reset(); return false; }
  static constexpr std::size_t kMaxReads = 1 + core::kMaxStreams + core::kMaxTextures;
  std::shared_ptr<PreparedSerialReads> storage_;
  std::size_t handleLimit_;
};

// Standalone reference/test adapter. Production appends already-projected
// command views from its existing resource-mark traversal instead.
inline std::shared_ptr<const PreparedSerialReads> PreparedSerialReads::build(
    core::SourcePayloadView source, PreparedSourceKey key, Fault fault,
    std::size_t handleLimit) noexcept {
  if (!source.valid() || key.firstCommand > source.commandCount() ||
      key.commandCount > source.commandCount() - key.firstCommand) return {};
  PreparedSerialReadsBuilder builder(key, fault, handleLimit);
  for (std::uint32_t i = 0; i < key.commandCount; ++i)
    if (!builder.append(key.firstCommand + i,
                        source.commandAt(key.firstCommand + i).command)) return {};
  return builder.seal();
}

} // namespace dxmt9::encoders
