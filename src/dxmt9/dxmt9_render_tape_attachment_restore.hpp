#pragma once

#include "dxmt9/render_tape_attachment_history.hpp"

#include <optional>

namespace dxmt9 {

// Unix-local cold setup mapping. These handles are never serialized. The
// caller retains every resolved surface until queue settlement completes.
struct TapeAttachmentRestoreBinding {
  D9CWireObjectIdentity original{};
  std::uint64_t localHandle = 0u;
};
struct TapeAttachmentRestoreBoundary {
  std::uint64_t queue = 0u;
  std::uint64_t published = 0u;
  std::uint64_t initializerPublished = 0u;
};
struct TapeAttachmentRestoreRow {
  std::uint64_t handle = 0u;
  bool touched = false;
  bool current = false;
};

// A one-use setup plan, NOT content or replay qualification authority. Its
// caller must authenticate the history against the original V4 bundle first.
// Construction closes required-identity coverage and bounded alias mapping;
// the queue independently checks local lifetime, progress and Normal policy.
class TapeAttachmentRestorePlan {
 public:
  TapeAttachmentRestorePlan(const TapeAttachmentRestorePlan&) = delete;
  TapeAttachmentRestorePlan& operator=(const TapeAttachmentRestorePlan&) = delete;
  TapeAttachmentRestorePlan(TapeAttachmentRestorePlan&& other) noexcept
      : rows_(std::move(other.rows_)), boundary_(other.boundary_),
        discard_(other.discard_), current_(other.current_),
        valid_(std::exchange(other.valid_, false)) {}
  TapeAttachmentRestorePlan& operator=(TapeAttachmentRestorePlan&&) = delete;

  bool valid() const noexcept { return valid_; }
  bool discard() const noexcept { return discard_; }
  std::uint64_t current() const noexcept { return current_; }
  TapeAttachmentRestoreBoundary boundary() const noexcept { return boundary_; }
  std::span<const TapeAttachmentRestoreRow> rows() const noexcept { return rows_; }

  static std::optional<TapeAttachmentRestorePlan> prepare(
      const d3d9::RenderTapeAttachmentHistoryHeader& header,
      std::span<const d3d9::RenderTapeAttachmentHistoryRow> history,
      std::span<const D9CWireObjectIdentity> required,
      std::span<const TapeAttachmentRestoreBinding> bindings,
      TapeAttachmentRestoreBoundary boundary) noexcept {
    using namespace d3d9;
    if (!renderTapeAttachmentHistoryValid(header, history) || !boundary.queue ||
        required.size() != bindings.size() || required.size() > history.size())
      return std::nullopt;
    const auto same = [](const auto& a, const auto& b) {
      return a.kind == b.kind && a.generation == b.generation && a.objectId == b.objectId;
    };
    try {
      TapeAttachmentRestorePlan plan;
      plan.boundary_ = boundary;
      plan.discard_ = (header.flags & 1u) != 0u;
      plan.rows_.reserve(required.size());
      std::vector<const RenderTapeAttachmentHistoryRow*> joined;
      joined.reserve(required.size());
      for (std::size_t i = 0; i < required.size(); ++i) {
        const auto& binding = bindings[i];
        if (!same(required[i], binding.original) || !binding.localHandle)
          return std::nullopt;
        const RenderTapeAttachmentHistoryRow* row = nullptr;
        for (const auto& candidate : history)
          if (same(candidate.surface, required[i])) { row = &candidate; break; }
        if (!row) return std::nullopt;
        for (std::size_t j = 0; j < i; ++j) {
          if (same(required[i], required[j])) return std::nullopt;
          const auto& previous = *joined[j];
          const bool sameStorage = (row->currentBackbuffer && previous.currentBackbuffer) ||
              (same(row->storage, previous.storage) && row->subresource == previous.subresource);
          const bool sameLocal = binding.localHandle == bindings[j].localHandle;
          // This bounded contract requires exact handle aliasing. Different
          // views of one allocation require a wider physical-alias proof.
          if (sameStorage != sameLocal ||
              (sameLocal && (row->touched != previous.touched ||
                            row->currentBackbuffer != previous.currentBackbuffer)))
            return std::nullopt;
        }
        joined.push_back(row);
        plan.rows_.push_back({binding.localHandle, bool(row->touched), bool(row->currentBackbuffer)});
        if (row->currentBackbuffer) plan.current_ = binding.localHandle;
      }
      if (bool(header.flags & 2u) != bool(plan.current_)) return std::nullopt;
      plan.valid_ = true;
      return plan;
    } catch (...) { return std::nullopt; }
  }

 private:
  TapeAttachmentRestorePlan() = default;
  std::vector<TapeAttachmentRestoreRow> rows_;
  TapeAttachmentRestoreBoundary boundary_{};
  bool discard_ = false;
  std::uint64_t current_ = 0u;
  bool valid_ = false;
};

} // namespace dxmt9
