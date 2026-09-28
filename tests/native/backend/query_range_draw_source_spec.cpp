#include "../../../src/dxmt9/render/query_range_draw_append.hpp"
#include "../../../src/dxmt9/dxmt9_chunk_slot_capacity.hpp"
#include <iostream>
#include <stdexcept>

namespace {
void check(bool value) {
  if (!value) throw std::runtime_error("DrawRun query range atomicity");
}
}
int main() {
  using namespace dxmt9;
  using namespace core;
  using namespace render;
  try {
    const query::WriterIdentity id{7, 2, 3, 4};
    auto result = std::make_shared<query::OcclusionResult>(1, 1, 1);
    query::OcclusionScope scope{};
    scope.results[0] = result;
    scope.count = 1;
    DrawParam draw{};
    draw.primitiveCount = 2;
    const std::array<u8, 4> bytes{1, 2, 3, 4};
    DrawParamPayloadView view{};
    view.userVertexData = bytes;
    unsigned failures = 0;
    for (unsigned prefix : {0u, 1u, 8u, 16u, 32u}) {
      for (unsigned fault = 0;; ++fault) {
        ChunkSlot slot{};
        QueryScopeRangeOwner<2, 4, 2> owner(id, 100);
        DrawUniformPayload uniform{};
        uniform.vertexFloatConstantCount = uniform.vsConst.float4.size();
        uniform.vertexIntConstantCount = uniform.vsConst.int4.size();
        uniform.vertexBoolConstantCount = uniform.vsConst.bools.size();
        uniform.pixelFloatConstantCount = uniform.psConst.float4.size();
        uniform.pixelIntConstantCount = uniform.psConst.int4.size();
        uniform.pixelBoolConstantCount = uniform.psConst.bools.size();
        const auto append = [&](const query::OcclusionScope& membership, auto hook) {
          return appendQueryRangeDraw(owner, id, slot, membership,
              CanonicalDrawState{}, uniform, std::span(&draw, 1),
              std::span(&view, 1), hook);
        };
        for (unsigned i = 0; i < prefix; ++i) {
          uniform.hash = i + 1;
          uniform.vertexConstantsHash = i + 1;
          uniform.pixelConstantsHash = i + 1;
          uniform.fixedPayloadHash = i + 1;
          check(append({}, QueryDrawPrepare{}) == QueryRangeStatus::Accepted);
        }
        QueryRangeEndReceipt receipt;
        if (prefix) check(owner.end(id, 0, receipt) == QueryRangeStatus::Accepted);
        const auto refs = result.use_count();
        const auto retained = chunkSlotPhysicalRetainedBytes(slot);
        const auto heads = slot.drawUniformPayloadLookupHeads;
        const auto tails = slot.drawUniformPayloadLookupTails;
        const auto next = slot.drawUniformPayloadLookupNext;
        const auto vertexHeads = slot.drawUniformVertexConstantsLookupHeads;
        const auto vertexTails = slot.drawUniformVertexConstantsLookupTails;
        const auto vertexNext = slot.drawUniformVertexConstantsLookupNext;
        const auto pixelHeads = slot.drawUniformPixelConstantsLookupHeads;
        const auto pixelTails = slot.drawUniformPixelConstantsLookupTails;
        const auto pixelNext = slot.drawUniformPixelConstantsLookupNext;
        std::array<std::size_t, 40> sizes{};
        unsigned dimension = 0;
#define SAVE(region, plan, storage, element, physical, provision, allocation, lookup, ownerRole) \
        DXMT9_DIRECT_CHUNK_SLOT_EXPAND_PHYSICAL_##physical(sizes[dimension++] = slot.storage.size();)
        DXMT9_DIRECT_CHUNK_SLOT_DIMENSIONS(SAVE)
#undef SAVE
        uniform.hash = 100;
        uniform.vertexConstantsHash = 100;
        uniform.pixelConstantsHash = 100;
        uniform.fixedPayloadHash = 100;
        unsigned point = 0;
        const auto status = append(scope, [&](const char*) {
          if (point++ == fault) throw std::bad_alloc();
        });
        if (status == QueryRangeStatus::Accepted) {
          check(slot.commandCount() == prefix + 1 && owner.commands() == prefix + 1);
          check(slot.drawPayloadArena.size() == (prefix + 1) * 4);
          check(slot.commandAt(prefix).drawParams[0].primitiveCount == 2);
          break;
        }
        ++failures;
        check(status == QueryRangeStatus::AllocationFailed);
        check(owner.commands() == prefix && result.use_count() == refs);
        check(owner.endCount() == (prefix ? 1u : 0u));
        check(receipt.commandEnd() == prefix);
        dimension = 0;
#define CHECK(region, plan, storage, element, physical, provision, allocation, lookup, ownerRole) \
        DXMT9_DIRECT_CHUNK_SLOT_EXPAND_PHYSICAL_##physical(check(sizes[dimension++] == slot.storage.size());)
        DXMT9_DIRECT_CHUNK_SLOT_DIMENSIONS(CHECK)
#undef CHECK
        check(heads == slot.drawUniformPayloadLookupHeads &&
              tails == slot.drawUniformPayloadLookupTails && next == slot.drawUniformPayloadLookupNext);
        check(vertexHeads == slot.drawUniformVertexConstantsLookupHeads &&
              vertexTails == slot.drawUniformVertexConstantsLookupTails &&
              vertexNext == slot.drawUniformVertexConstantsLookupNext);
        check(pixelHeads == slot.drawUniformPixelConstantsLookupHeads &&
              pixelTails == slot.drawUniformPixelConstantsLookupTails &&
              pixelNext == slot.drawUniformPixelConstantsLookupNext);
        check(chunkSlotPhysicalRetainedBytes(slot) >= retained);
        check(append(scope, QueryDrawPrepare{}) == QueryRangeStatus::Accepted);
        check(owner.seal(id) && owner.bindPublished(id, 9));
        if (prefix) check(receipt.poll({7, 9, 9}) == query::Poll::Complete);
      }
    }
    check(failures >= 23);
    std::cout << "DrawRun allocation fault points=" << failures
              << "; all schema lengths, lookup chains and ENDs preserved; retry passed\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
