#include "../../../src/dxmt9/render/query_range_clear_source.hpp"
#include <iostream>
#include <stdexcept>

namespace {
void check(bool condition) {
  if (!condition) throw std::runtime_error("query range clear transaction");
}
}

int main() {
  using namespace dxmt9;
  using namespace render;
  using Source = QueryRangeClearSource<2, 4, 2>;
  const query::WriterIdentity id{7, 0, 1, 2};
  try {
    auto result = std::make_shared<query::OcclusionResult>(1, 1, 1);
    query::OcclusionScope scoped;
    scoped.count = 1;
    scoped.results[0] = result;
    core::ClearDesc clear;
    clear.clearDepth = true;
    clear.depth = 0.25f;
    clear.rects.push_back({1, 2, 30, 40});
    for (bool extend : {false, true}) {
      for (auto fault : {QueryClearPreparePoint::Copy,
                         QueryClearPreparePoint::Headers,
                         QueryClearPreparePoint::Records}) {
        Source source(id, 4);
        check(source.appendClear(extend ? scoped : query::OcclusionScope{}, clear)
              == QueryRangeStatus::Accepted);
        QueryRangeEndReceipt receipt;
        check(source.end(0, receipt) == QueryRangeStatus::Accepted);
        const auto refs = result.use_count();
        check(source.appendClear(scoped, clear, [&](auto point) {
          if (point == fault) throw std::bad_alloc();
        }) == QueryRangeStatus::AllocationFailed);
        check(result.use_count() == refs);
        check(source.metadata().commands() == 1);
        check(source.metadata().rangeCount() == 1);
        check(source.metadata().scopeCount() == (extend ? 1u : 0u));
        check(source.metadata().endCount() == 1 && receipt.commandEnd() == 1);
        check(!source.payload());
        check(source.seal());
        const auto* payload = source.payload();
        check(payload && payload->commandCount() == 1 && payload->clearRecords.size() == 1);
        check(source.metadata().ranges()[0].end == 1);
        check(payload->commandAt(0).kind == core::MetalCommandKind::Clear);
        check(payload->commandAt(0).clear->depth == 0.25f);
        check(payload->commandAt(0).clear->rects.size() == 1);
        check(source.bindPublished(9));
        check(payload->seqId == 9 && receipt.sequence() == 9);
        check(receipt.poll({7, 9, 0}) == query::Poll::Pending);
        check(receipt.poll({7, 9, 9}) == query::Poll::Complete);
      }
    }
    {
      Source source(id, 3);
      for (unsigned i = 0; i < 3; ++i) {
        clear.depth = float(i) / 4;
        check(source.appendClear(i == 1 ? scoped : query::OcclusionScope{}, clear)
              == QueryRangeStatus::Accepted);
      }
      bool prepareCalled = false;
      check(source.appendClear(scoped, clear, [&](auto) { prepareCalled = true; })
            == QueryRangeStatus::Capacity);
      check(!prepareCalled);
      clear.rects.clear();
      check(source.seal());
      const auto* payload = source.payload();
      unsigned visited = 0;
      for (const auto& range : source.metadata().ranges()) {
        check(range.begin == visited);
        for (unsigned i = range.begin; i < range.end; ++i) {
          const auto command = payload->commandAt(i);
          check(command.clear && command.clear->depth == float(i) / 4);
          check(command.clear->rects.size() == 1);
          check(bool(range.scope.slot) == (i == 1));
          ++visited;
        }
      }
      check(visited == 3 && payload->commandCount() == 3);
    }
    std::cout << "real ChunkSlot clear payload/range atomicity and six preparation fault cases passed\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
