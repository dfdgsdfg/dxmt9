#include "../../../src/dxmt9/dxmt9_resource_lifetime.hpp"
#include "../../../src/dxmt9/dxmt9_resource_pool.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string message) {
  throw TestFailure(std::move(message));
}

void check(bool condition, std::string_view message) {
  if (!condition) {
    fail(std::string(message));
  }
}

void resourceLifetimePredicateTruthTables() {
  std::size_t cases = 0;

  for (const bool destroyPending : {false, true}) {
    for (std::uint64_t lastUsed = 0; lastUsed <= 2; ++lastUsed) {
      for (std::uint64_t completed = 0; completed <= 2; ++completed) {
        const bool expected = destroyPending && lastUsed <= completed;
        check(dxmt9::resources::canReclaimRecord(
                  destroyPending, lastUsed, completed) == expected,
              "ResourceLifetime FreeResource guard matches production predicate");
        ++cases;
      }
    }
  }

  for (const bool pending : {false, true}) {
    for (const bool retainedDestination : {false, true}) {
      check(dxmt9::resources::lifetime::pendingInitializerReferenceSafe(
                pending, retainedDestination) ==
                (!pending || retainedDestination),
            "ResourceLifetime Initializer ownership guard matches production predicate");
      ++cases;
    }
  }

  check(cases == 22u, "resource lifetime truth-table domain remains exhaustive");
}

struct ArenaRecord {
  int identity = 0;
  bool destroyPending = false;
  std::uint64_t lastUsedSeqId = 0;
};

constexpr std::uint32_t handleSlot(std::uint64_t handle) noexcept {
  return static_cast<std::uint32_t>(handle & 0xffffffffull);
}

constexpr std::uint32_t handleGeneration(std::uint64_t handle) noexcept {
  return static_cast<std::uint32_t>((handle >> 32u) & 0x00ffffffull);
}

void handleArenaGenerationAndSlotAbaSafety() {
  using Arena = dxmt9::resources::detail::HandleArena<
      ArenaRecord, dxmt9::resources::detail::ResourceHandleKind::Buffer>;
  using WrongKindArena = dxmt9::resources::detail::HandleArena<
      ArenaRecord, dxmt9::resources::detail::ResourceHandleKind::Texture>;

  Arena arena;
  WrongKindArena wrongKindArena;
  const auto first = arena.insert(ArenaRecord{.identity = 11});
  const auto sibling = arena.insert(ArenaRecord{.identity = 22});
  check(first && sibling && first != sibling,
        "HandleArena issues distinct nonzero handles");
  check(arena.find(first.value) && arena.find(first.value)->identity == 11,
        "HandleArena resolves the exact first slot occupant");
  check(arena.find(sibling.value) && arena.find(sibling.value)->identity == 22,
        "HandleArena does not cross-alias sibling slots");
  check(wrongKindArena.find(first.value) == nullptr,
        "HandleArena rejects a handle encoded for another resource kind");

  check(arena.update(first.value, [](ArenaRecord& record) {
          record.destroyPending = true;
          record.lastUsedSeqId = 2;
        }),
        "HandleArena updates a live generation");
  std::size_t erased = 0;
  arena.reclaimCompleted(1, [&](const ArenaRecord&) { ++erased; });
  check(erased == 0u && arena.find(first.value) != nullptr,
        "HandleArena preserves a destroy-pending in-flight record");
  arena.reclaimCompleted(2, [&](const ArenaRecord& record) {
    check(record.identity == 11, "HandleArena reclaims the intended occupant");
    ++erased;
  });
  check(erased == 1u && arena.find(first.value) == nullptr,
        "HandleArena stale generation resolves null after reclaim");

  const auto replacement = arena.insert(ArenaRecord{.identity = 33});
  check(replacement && handleSlot(replacement.value) == handleSlot(first.value),
        "HandleArena deterministically reuses the released LIFO slot");
  check(handleGeneration(replacement.value) != handleGeneration(first.value),
        "HandleArena advances generation before slot reuse");
  check(arena.find(first.value) == nullptr,
        "HandleArena old generation never aliases the replacement");
  check(arena.find(replacement.value) &&
            arena.find(replacement.value)->identity == 33,
        "HandleArena new generation resolves the replacement");
  check(arena.find(sibling.value) && arena.find(sibling.value)->identity == 22,
        "HandleArena slot reuse leaves the sibling mapping unchanged");
}

}  // namespace

int main() {
  try {
    resourceLifetimePredicateTruthTables();
    handleArenaGenerationAndSlotAbaSafety();
  } catch (const TestFailure& failure) {
    std::cerr << "resource_lifetime_spec failed: " << failure.what() << '\n';
    return 1;
  } catch (const std::exception& ex) {
    std::cerr << "resource_lifetime_spec unexpected exception: " << ex.what()
              << '\n';
    return 1;
  }
  std::cout << "resource_lifetime_spec passed\n";
  return 0;
}
