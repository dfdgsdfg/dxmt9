#include "../../../src/dxmt9/dxmt9_publication_mark_cache.hpp"
#include "../../../src/dxmt9/dxmt9_mark_reclaim_predicates.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

using namespace dxmt9;
using Kind = core::ChunkHandleKind;
void check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
template<std::size_t Capacity>
void exhaustive() {
  // Colliding indices, equal handles across kinds, and distinct generations.
  constexpr std::array<core::ChunkHandleEntry, 6> entries{{
      {Kind::Texture, {1}}, {Kind::Texture, {3}},
      {Kind::Surface, {1}}, {Kind::Texture, {(1ull << 32) | 1}},
      {Kind::Buffer, {1}}, {Kind::Texture, {0}}}};
  constexpr unsigned length = 7;
  unsigned cases = 1;
  for (unsigned i = 0; i < length; ++i) cases *= entries.size();
  for (unsigned encoded = 0; encoded < cases; ++encoded) {
    resources::PublicationMarkCache<Capacity> cache;
    std::array<unsigned, entries.size()> reference{}, candidate{};
    auto digits = encoded;
    for (unsigned step = 0; step < length; ++step) {
      const auto index = digits % entries.size();
      digits /= entries.size();
      const auto entry = entries[index];
      const bool mark = cache.shouldMark(entry);
      if (entry.kind == Kind::Buffer || !entry.handle) {
        check(mark, "buffer and null effects must pass through");
        continue;
      }
      reference[index] = resources::markStampUpper(reference[index], 9u);
      if (mark) candidate[index] = resources::markStampUpper(candidate[index], 9u);
      check(reference == candidate, "every prefix preserves lifetime stamps");
    }
  }
}
int main() try {
  exhaustive<1>();
  exhaustive<2>();
  exhaustive<64>();
  resources::PublicationMarkCache<> first, next;
  core::ChunkHandleEntry texture{Kind::Texture, {42}};
  check(first.shouldMark(texture) && !first.shouldMark(texture), "duplicate removed");
  check(next.shouldMark(texture), "new publication must stamp again");
  for (unsigned prior = 0; prior <= 12; ++prior)
    for (unsigned stamp = 0; stamp <= 12; ++stamp)
      for (unsigned completed = 0; completed <= 12; ++completed) {
        const auto once = resources::markStampUpper(prior, stamp);
        const auto twice = resources::markStampUpper(once, stamp);
        check(once == twice &&
              resources::canReclaimRecord(true, once, completed) ==
              resources::canReclaimRecord(true, twice, completed),
              "stamp elision preserves every reclaim decision");
      }
  std::cout << "publication mark refinement passed\n";
  return 0;
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
