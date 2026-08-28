// Bounded publication contract for the append-only draw-PSO handle table.
// The production cache serializes append() with Cache::mutex; this spec
// exercises the table's release/acquire publication independently, including
// readers racing a single writer.

#include "../../../src/dxmt9/dxmt9_segmented_slot_table.hpp"
#include "dxmt9/core_constants.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

struct Value {
  std::uint32_t generation = 0;
  std::uint32_t value = 0;
  bool occupied = false;
};

using Table = dxmt9::detail::SegmentedImmutableSlotTable<Value>;

const Value* lookupHandle(const Table& table, dxmt9::core::PsoHandle handle) {
  if (!handle.valid()) {
    return nullptr;
  }
  const Value* value = table.lookup(handle.slot);
  if (!value || !value->occupied || value->generation != handle.generation) {
    return nullptr;
  }
  return value;
}

void testEmptyAndBoundaries() {
  Table table;
  check(table.size() == 0u, "new table is empty");
  check(table.lookup(0u) == nullptr, "empty table rejects slot zero");
  check(table.lookup(Table::kInvalidIndex) == nullptr,
        "invalid index is rejected");

  for (std::uint32_t i = 0; i < 65u; ++i) {
    auto index = table.append(Value{.generation = 1u, .value = i, .occupied = true});
    check(index.has_value() && *index == i, "append returns monotonic index");
  }
  check(table.size() == 65u, "boundary appends are visible");
  check(table.lookup(63u)->value == 63u, "slot 63 survives boundary");
  check(table.lookup(64u)->value == 64u, "slot 64 starts a new block");
}

void testInvalidGenerationAndImmutability() {
  Table table;
  const auto index = table.append(Value{.generation = 7u, .value = 42u, .occupied = true});
  check(index.has_value(), "first append succeeds");
  const dxmt9::core::PsoHandle valid{.slot = *index, .generation = 7u};
  const dxmt9::core::PsoHandle stale{.slot = *index, .generation = 8u};
  const dxmt9::core::PsoHandle invalid{};
  check(lookupHandle(table, valid)->value == 42u, "matching generation resolves");
  check(lookupHandle(table, stale) == nullptr, "generation mismatch fails closed");
  check(lookupHandle(table, invalid) == nullptr, "invalid handle fails closed");

  check(table.append(Value{.generation = 1u, .value = 99u, .occupied = true}).value() == 1u,
        "second append succeeds");
  check(lookupHandle(table, valid)->value == 42u,
        "published old slot remains immutable");
}

void testExhaustion() {
  Table table;
  for (std::size_t i = 0; i < Table::kCapacity; ++i) {
    const auto index = table.append(Value{.generation = 1u,
                                          .value = static_cast<std::uint32_t>(i),
                                          .occupied = true});
    check(index.has_value() && *index == i, "all valid slots append");
  }
  check(table.size() == Table::kCapacity, "capacity reaches invalid-slot boundary");
  check(table.lookup(static_cast<std::uint16_t>(Table::kCapacity - 1u))->value ==
            Table::kCapacity - 1u,
        "last valid slot resolves");
  check(!table.append(Value{.generation = 1u, .value = 0u, .occupied = true}),
        "invalid slot is reserved for exhaustion");
}

void testConcurrentReaders() {
  Table table;
  constexpr std::uint32_t kCount = 4096u;
  std::atomic<bool> writerDone{false};
  std::atomic<bool> failed{false};

  std::thread writer([&] {
    for (std::uint32_t i = 0; i < kCount; ++i) {
      if (!table.append(Value{.generation = 1u, .value = i, .occupied = true})) {
        failed.store(true, std::memory_order_relaxed);
        return;
      }
    }
    writerDone.store(true, std::memory_order_release);
  });
  std::vector<std::thread> readers;
  for (unsigned readerIndex = 0; readerIndex < 4u; ++readerIndex) {
    readers.emplace_back([&] {
      std::size_t lastSize = 0u;
      while (!writerDone.load(std::memory_order_acquire) ||
             lastSize < table.size()) {
        const std::size_t size = table.size();
        for (std::size_t i = 0; i < size; ++i) {
          const Value* value = table.lookup(static_cast<std::uint16_t>(i));
          if (!value || !value->occupied || value->generation != 1u ||
              value->value != i) {
            failed.store(true, std::memory_order_relaxed);
            return;
          }
        }
        lastSize = size;
      }
    });
  }
  writer.join();
  for (auto& reader : readers) {
    reader.join();
  }
  check(!failed.load(std::memory_order_relaxed),
        "concurrent readers see only fully published immutable values");
  check(table.size() == kCount, "concurrent writer publishes complete table");
}

}  // namespace

int main() {
  try {
    static_assert(sizeof(dxmt9::core::PsoHandle) == sizeof(std::uint32_t));
    testEmptyAndBoundaries();
    testInvalidGenerationAndImmutability();
    testExhaustion();
    testConcurrentReaders();
  } catch (const TestFailure& failure) {
    std::cerr << "pso_slot_publication_spec failed: " << failure.what() << '\n';
    return 1;
  } catch (const std::exception& exception) {
    std::cerr << "pso_slot_publication_spec unexpected exception: "
              << exception.what() << '\n';
    return 1;
  }
  std::cout << "pso_slot_publication_spec passed\n";
  return 0;
}
