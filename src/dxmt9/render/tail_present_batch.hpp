#pragma once

#include "dag_observer.hpp"

#include "../dxmt9_draw_encoder.hpp"
#include "../dxmt9_queue.hpp"

#include <cstddef>
#include <deque>
#include <optional>
#include <span>

namespace dxmt9::render {

bool slotIsPresentOnlyTail(const core::ChunkSlot& slot) noexcept;

bool slotIsOpenCbPreencodeHead(const core::ChunkSlot& slot) noexcept;

bool canCoalesceTailPresentBatch(
    std::span<const core::metalqueue::ReadySlotSnapshot> sources) noexcept;

std::size_t selectTailPresentBatchPrefix(
    const std::deque<std::size_t>& readySlots,
    std::span<const core::ChunkSlot> slots,
    std::size_t maxCount) noexcept;

std::optional<core::metalqueue::QueueSubmissionRecord> encodeTailPresentBatch(
    encoders::EncodeContext& ctx,
    std::span<core::metalqueue::ReadySlotSnapshot> sources,
    DagObserver& observer);

}  // namespace dxmt9::render
