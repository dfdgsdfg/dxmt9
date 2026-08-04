#include "d3d9_pe_buffer_hazard.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

enum class EventKind {
  Draw,
  Seal,
  BridgeLock,
  LockRename,
  BridgeUnlock,
  UnlockUploadRename,
};

struct Event {
  EventKind kind;
  std::uint32_t generation;

  bool operator==(const Event&) const = default;
};

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

struct FakePeRecorderBridge {
  std::uint32_t generation = 0u;
  std::vector<Event> events;
  std::vector<std::vector<std::uint32_t>> rawChunks{{}};

  void draw() {
    events.push_back({EventKind::Draw, generation});
    rawChunks.back().push_back(generation);
  }

  int seal() {
    events.push_back({EventKind::Seal, generation});
    if (!rawChunks.back().empty()) {
      rawChunks.emplace_back();
    }
    return 0;
  }

  int lock(std::uint32_t flags, bool renameAtLock,
           bool renameAtUnlock) {
    const int sealStatus =
        dxmt9::d3d9::pe::sealBufferGenerationBeforeLock(
            true, flags, 0, [&] { return seal(); });
    if (sealStatus != 0) {
      return sealStatus;
    }
    events.push_back({EventKind::BridgeLock, generation});
    if (renameAtLock) {
      ++generation;
      events.push_back({EventKind::LockRename, generation});
    }
    events.push_back({EventKind::BridgeUnlock, generation});
    if (renameAtUnlock) {
      ++generation;
      events.push_back({EventKind::UnlockUploadRename, generation});
    }
    return 0;
  }
};

void testDynamicDiscardSealsG0BeforeLockRename() {
  FakePeRecorderBridge sink;
  sink.draw();
  check(sink.lock(0x00002000u, true, false) == 0,
        "DEFAULT+DYNAMIC DISCARD lock succeeds");
  sink.draw();

  const std::vector<Event> expected{
      {EventKind::Draw, 0u},
      {EventKind::Seal, 0u},
      {EventKind::BridgeLock, 0u},
      {EventKind::LockRename, 1u},
      {EventKind::BridgeUnlock, 1u},
      {EventKind::Draw, 1u},
  };
  check(sink.events == expected,
        "DISCARD seals G0 before bridge lock changes the backing to G1");
  check(sink.rawChunks ==
            std::vector<std::vector<std::uint32_t>>{{0u}, {1u}},
        "G0 and G1 draws occupy distinct PE raw chunks");
}

void testNoOverwriteKeepsOneGenerationAndChunk() {
  FakePeRecorderBridge sink;
  sink.draw();
  check(sink.lock(0x00001000u, false, false) == 0,
        "NOOVERWRITE lock succeeds");
  sink.draw();

  const std::vector<Event> expected{
      {EventKind::Draw, 0u},
      {EventKind::BridgeLock, 0u},
      {EventKind::BridgeUnlock, 0u},
      {EventKind::Draw, 0u},
  };
  check(sink.events == expected,
        "NOOVERWRITE neither seals nor rotates the backing");
  check(sink.rawChunks ==
            std::vector<std::vector<std::uint32_t>>{{0u, 0u}},
        "NOOVERWRITE draws retain G0 in one PE raw chunk");
}

void testManagedWritableSealsBeforeUnlockUploadRename() {
  FakePeRecorderBridge sink;
  sink.draw();
  check(sink.lock(0u, false, true) == 0,
        "MANAGED writable lock/unlock succeeds");
  sink.draw();

  const std::vector<Event> expected{
      {EventKind::Draw, 0u},
      {EventKind::Seal, 0u},
      {EventKind::BridgeLock, 0u},
      {EventKind::BridgeUnlock, 0u},
      {EventKind::UnlockUploadRename, 1u},
      {EventKind::Draw, 1u},
  };
  check(sink.events == expected,
        "MANAGED G0 seals before unlock publishes G1");
  check(sink.rawChunks ==
            std::vector<std::vector<std::uint32_t>>{{0u}, {1u}},
        "MANAGED upload rotation preserves one generation per raw chunk");
}

}  // namespace

int main() {
  try {
    testDynamicDiscardSealsG0BeforeLockRename();
    testNoOverwriteKeepsOneGenerationAndChunk();
    testManagedWritableSealsBeforeUnlockUploadRename();
  } catch (const TestFailure& error) {
    std::cerr << "pe_buffer_lock_order_spec failed: " << error.what() << '\n';
    return 1;
  }
  std::cout << "pe_buffer_lock_order_spec passed\n";
  return 0;
}
