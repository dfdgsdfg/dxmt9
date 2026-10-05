#include "dxmt9_render_tape_emission_observer.hpp"

#include <algorithm>
#include <limits>

namespace dxmt9::render {

void TapeEmissionObserver::fail() noexcept {
  if (failed_) return;
  failed_ = true;
  if (sink_.fail) sink_.fail(sink_.context);
}

void TapeEmissionObserver::record(TapeEmissionRecordLocator locator) noexcept {
  if (!locator.eventOrdinal) { fail(); return; }
  current_ = locator;
}

void TapeEmissionObserver::acceptCompatibilityDraw() noexcept {
  if (failed_) return;
  if (!current_.eventOrdinal || pendingDraws_.size() >= maxLocators_) { fail(); return; }
  try { pendingDraws_.push_back(current_); }
  catch (...) { fail(); }
}

void TapeEmissionObserver::stagePresent(query::WriterIdentity writer) noexcept {
  if (failed_) return;
  if (!writer.valid() || !current_.eventOrdinal || presents_.size() >= maxLocators_ ||
      std::any_of(presents_.begin(), presents_.end(),
          [&](const auto& value) { return value.writer == writer; })) { fail(); return; }
  try { presents_.push_back({writer, current_}); }
  catch (...) { fail(); }
}

void TapeEmissionObserver::takePresent(query::WriterIdentity writer) noexcept {
  if (failed_) return;
  const auto found = std::find_if(presents_.begin(), presents_.end(),
      [&](const auto& value) { return value.writer == writer; });
  if (found == presents_.end()) { fail(); return; }
  current_ = found->record;
  presents_.erase(found);
}

void TapeEmissionObserver::discardTentative() noexcept {
  // Called only after actual destination restoration. Root transaction
  // rollback separately restores every tentative physical binding.
  pendingDraws_.clear();
  presents_.clear();
  current_ = {};
}

void TapeEmissionObserver::append(query::WriterIdentity writer,
    std::size_t physical, core::MetalCommandKind kind,
    std::uint32_t firstSubdraw, std::uint32_t drawCount) noexcept {
  if (failed_) return;
  if (!writer.valid() || physical > UINT32_MAX || !sink_.bind) { fail(); return; }
  std::span<const TapeEmissionRecordLocator> records(&current_, 1u);
  if (kind == core::MetalCommandKind::DrawRun) {
    if (!drawCount || std::uint64_t{firstSubdraw} + drawCount > UINT32_MAX) { fail(); return; }
    if (batchActive_) {
      if (batchCursor_ > batch_.size() || drawCount > batch_.size() - batchCursor_) {
        fail(); return;
      }
      records = batch_.subspan(batchCursor_, drawCount);
    } else if (drawCount != 1u) { fail(); return; }
  } else if (drawCount || firstSubdraw) { fail(); return; }
  if (records.empty() || !records.front().eventOrdinal ||
      !sink_.bind(sink_.context, writer, static_cast<std::uint32_t>(physical),
          kind, records, firstSubdraw)) { fail(); return; }
  if (kind == core::MetalCommandKind::DrawRun && batchActive_) batchCursor_ += drawCount;
}

void TapeEmissionObserver::appendLocal(std::size_t physical,
    core::MetalCommandKind kind, std::uint32_t firstSubdraw,
    std::uint32_t drawCount) noexcept {
  if (commandOffset_ > std::numeric_limits<std::size_t>::max() - physical) {
    fail(); return;
  }
  append(writer_, commandOffset_ + physical, kind, firstSubdraw, drawCount);
}

} // namespace dxmt9::render
