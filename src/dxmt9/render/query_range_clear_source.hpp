#pragma once

#include "query_scope_range_owner.hpp"
#include "../dxmt9_backend_types.hpp"
#include <optional>

namespace dxmt9::render {

enum class QueryClearPreparePoint : std::uint8_t { Copy, Headers, Records };
struct QueryClearPrepare {
  void operator()(QueryClearPreparePoint) const noexcept {}
};

// Integration fixture component, NOT a general replay source. Only Clear is
// admitted until each additional family has atomic storage and consumer proof.
template<std::size_t Scopes, std::size_t Ranges, std::size_t Ends>
class QueryRangeClearSource {
 public:
  using Owner = QueryScopeRangeOwner<Scopes, Ranges, Ends>;
  QueryRangeClearSource(query::WriterIdentity id, std::uint32_t capacity)
      : id_(id), metadata_(id, capacity) {}
  QueryRangeClearSource(const QueryRangeClearSource&) = delete;
  QueryRangeClearSource& operator=(const QueryRangeClearSource&) = delete;
  QueryRangeClearSource(QueryRangeClearSource&&) = delete;
  QueryRangeClearSource& operator=(QueryRangeClearSource&&) = delete;

  template<class Prepare = QueryClearPrepare>
  QueryRangeStatus appendClear(const query::OcclusionScope& scope,
                               const core::ClearDesc& clear,
                               Prepare prepare = {}) noexcept {
    core::CommandPayloadIndex index{};
    if (payload_.prefetchedPipelinesSealed() ||
        !core::detail::chunkSlotTryMakeCommandPayloadIndex(
            payload_.commandCount(), payload_.clearRecords.size(), index))
      return QueryRangeStatus::Invalid;
    std::optional<core::ClearDesc> staged;
    return metadata_.appendWithPayload(id_, scope, 1, [&] {
      prepare(QueryClearPreparePoint::Copy);
      staged.emplace(clear);
      prepare(QueryClearPreparePoint::Headers);
      payload_.commandHeaders.reserve(payload_.commandCount() + 1);
      prepare(QueryClearPreparePoint::Records);
      payload_.clearRecords.reserve(payload_.clearRecords.size() + 1);
    }, [&]() noexcept {
      static_assert(std::is_nothrow_move_constructible_v<core::ClearDesc>);
      // Actual production appender, with both vectors reserved and nested
      // rect storage already owned. No payload/header allocation can occur.
      payload_.appendClear(std::move(*staged));
    });
  }
  QueryRangeStatus end(std::uint64_t initializer, QueryRangeEndReceipt& out) noexcept {
    return metadata_.end(id_, initializer, out);
  }
  bool seal() noexcept {
    return payload_.commandCount() == metadata_.commands() && metadata_.seal(id_);
  }
  bool bindPublished(std::uint64_t sequence) noexcept {
    if (!metadata_.bindPublished(id_, sequence)) return false;
    payload_.seqId = sequence;
    return true;
  }
  const core::ChunkSlot* payload() const noexcept {
    return metadata_.phase() == Owner::Phase::Sealed ||
        metadata_.phase() == Owner::Phase::Published ? &payload_ : nullptr;
  }
  const Owner& metadata() const noexcept { return metadata_; }
 private:
  query::WriterIdentity id_;
  core::ChunkSlot payload_;
  Owner metadata_;
};

} // namespace dxmt9::render
