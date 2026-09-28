#pragma once

#include "query_scope_range_owner.hpp"
#include "../dxmt9_backend_types.hpp"
#include <exception>

namespace dxmt9::render {

struct QueryDrawPrepare {
  void operator()(const char*) const noexcept {}
};

// Draft-only adapter. Physical growth may survive a failed prepare; logical
// records, lookup links, retained scopes and END receipts must not change.
// Inputs must not alias destination vectors (the destination is privately owned
// by the source builder). No borrowed input survives this call.
template<class Owner, class Prepare = QueryDrawPrepare>
QueryRangeStatus appendQueryRangeDraw(Owner& owner, query::WriterIdentity id,
    core::ChunkSlot& slot, const query::OcclusionScope& scope,
    core::CanonicalDrawState state, const core::DrawUniformPayload& uniform,
    std::span<const core::DrawParam> draws,
    std::span<const core::DrawParamPayloadView> payloads,
    Prepare prepare = {}) noexcept {
  using namespace core;
  if (slot.prefetchedPipelinesSealed() ||
      slot.commandCount() != owner.commands()) return QueryRangeStatus::Invalid;
  if (draws.empty()) return owner.append(id, scope, 0);
  std::size_t bytes = 0;
  for (std::size_t i = 0; i < draws.size(); ++i) {
    const auto view = core::detail::chunkSlotPayloadAt(payloads, i);
    for (auto part : {view.userVertexData, view.userIndexData,
                      view.bindingOverrideData, view.bindingSnapshotData}) {
      if (!core::detail::chunkSlotCanAppendU32Range(bytes, part.size()))
        return QueryRangeStatus::Capacity;
      bytes += part.size();
    }
  }
  CommandPayloadIndex index{};
  if (!core::detail::chunkSlotTryMakeCommandPayloadIndex(
          slot.commandCount(), slot.drawRunRecords.size(), index) ||
      !slot.canAppendDrawRun(draws.size(), bytes, true) ||
      !core::detail::chunkSlotCanAppendU32IndexedElement(slot.drawPsoSubviews.size()))
    return QueryRangeStatus::Capacity;
  return owner.appendWithPayload(id, scope, 1, [&] {
    const auto reserve = [&](auto& vector, std::size_t count, const char* name) {
      if (count > vector.capacity()) {
        prepare(name);
        core::detail::chunkSlotReserveAtLeast(vector, count);
      }
    };
#define DXMT9_QUERY_RESERVE(member, extra) \
    reserve(slot.member, slot.member.size() + (extra), #member)
    DXMT9_QUERY_RESERVE(commandHeaders, 1);
    DXMT9_QUERY_RESERVE(drawRunRecords, 1);
    DXMT9_QUERY_RESERVE(drawHotStates, 1);
    DXMT9_QUERY_RESERVE(drawShaderLayouts, 1);
    DXMT9_QUERY_RESERVE(drawDebugSnapshots, 1);
    DXMT9_QUERY_RESERVE(drawPsoSubviews, 1);
    DXMT9_QUERY_RESERVE(drawParams, draws.size());
    DXMT9_QUERY_RESERVE(drawPayloadArena, bytes);
    DXMT9_QUERY_RESERVE(drawUniformPayloads, 1);
    DXMT9_QUERY_RESERVE(drawUniformFixedPayloads, 1);
    DXMT9_QUERY_RESERVE(drawUniformVertexConstants, 1);
    DXMT9_QUERY_RESERVE(drawUniformPixelConstants, 1);
    DXMT9_QUERY_RESERVE(drawUniformVertexConstantBytes, sizeof(VertexShaderConstants));
    DXMT9_QUERY_RESERVE(drawUniformPixelConstantBytes, sizeof(PixelShaderConstants));
#undef DXMT9_QUERY_RESERVE
    // Even with dedup disabled, stage appenders maintain their lookup links.
    // Reserve EVERY lookup allocation before any assign/rebuild can mutate a
    // prior chain. The ordinary batch reservation below then cannot allocate.
    const auto lookup = [&](auto& records, auto& heads, auto& tails, auto& next,
                            const char* h, const char* t, const char* n) {
      const auto buckets = std::max(heads.size(),
          core::detail::chunkSlotUniformLookupBucketCount(records.size() + 1));
      reserve(heads, buckets, h);
      reserve(tails, buckets, t);
      reserve(next, records.size() + 1, n);
    };
#define DXMT9_QUERY_LOOKUP(records, prefix) \
    lookup(slot.records, slot.prefix##Heads, slot.prefix##Tails, \
           slot.prefix##Next, #prefix "Heads", #prefix "Tails", #prefix "Next")
    DXMT9_QUERY_LOOKUP(drawUniformPayloads, drawUniformPayloadLookup);
    DXMT9_QUERY_LOOKUP(drawUniformVertexConstants, drawUniformVertexConstantsLookup);
    DXMT9_QUERY_LOOKUP(drawUniformPixelConstants, drawUniformPixelConstantsLookup);
#undef DXMT9_QUERY_LOOKUP
  }, [&]() noexcept {
    // Existing interning and SoA appender, not a second implementation. Every
    // allocation was admitted above; lookup rebuilds happen only at commit.
    if (!slot.reserveDrawRunBatchStorage(draws.size(), bytes, 1, 1) ||
        !slot.appendDrawRun(std::move(state), uniform, draws, payloads,
                            {}, true, true, bytes)) std::terminate();
  });
}

} // namespace dxmt9::render
