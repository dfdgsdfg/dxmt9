#include "d3d9_pe_retainer.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace {
std::atomic<bool> gCountAllocations{false};
std::atomic<std::size_t> gAllocationCount{0u};

struct ReleaseEvent {
  std::uint8_t id = 0xffu;
  std::uint32_t kind = 0u;
};

struct ReleaseTrace {
  static constexpr std::size_t kMaxEvents = 64u;
  static constexpr std::size_t kMaxBindings = 3u;

  struct Binding {
    void* object = nullptr;
    std::uint8_t id = 0xffu;
    std::uint32_t kind = 0u;
  };

  std::array<ReleaseEvent, kMaxEvents> events{};
  std::array<Binding, kMaxBindings> bindings{};
  std::size_t count = 0u;
  bool overflow = false;

  void reset() noexcept {
    count = 0u;
    overflow = false;
  }

  void bind(std::size_t index, std::uint32_t kind, void* object) noexcept {
    bindings[index] = Binding{object, static_cast<std::uint8_t>(index), kind};
  }

  void recordId(std::uint8_t id, std::uint32_t kind) noexcept {
    if (count == events.size()) {
      overflow = true;
      return;
    }
    events[count++] = ReleaseEvent{id, kind};
  }

  void recordActual(std::uint32_t kind, void* object) noexcept {
    for (const auto& binding : bindings) {
      if (binding.object == object && binding.kind == kind) {
        recordId(binding.id, kind);
        return;
      }
    }
    overflow = true;
  }
};

ReleaseTrace* gReleaseTrace = nullptr;

void recordRelease(std::uint32_t kind, void* object) noexcept {
  if (gReleaseTrace) gReleaseTrace->recordActual(kind, object);
}
}

void* operator new(std::size_t size) {
  if (void* value = std::malloc(size ? size : 1u)) {
    if (gCountAllocations.load(std::memory_order_relaxed)) {
      gAllocationCount.fetch_add(1u, std::memory_order_relaxed);
    }
    return value;
  }
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

struct RefCounter {
  std::uint32_t refs = 1;
};

struct D9CSurface : RefCounter {};
struct D9CTexture : RefCounter {};
struct D9CBuffer : RefCounter {};
struct D9CShader : RefCounter {};
struct D9CVertexDecl : RefCounter {};
struct D9CQuery : RefCounter {};

// The cross-kind address test treats its pointer as opaque at the C stub
// boundary. Never dereference a Buffer object through a fabricated Query type.
static RefCounter* aliasedCounter = nullptr;

template<typename T>
RefCounter& counter(T* value) {
  if (static_cast<void*>(value) == static_cast<void*>(aliasedCounter))
    return *aliasedCounter;
  return *value;
}

template<typename T>
void addRef(T* value) {
  ++counter(value).refs;
}

template<typename T>
std::uint32_t release(T* value) {
  return --counter(value).refs;
}

extern "C" void dxmt9c_surface_addref(D9CSurface* value) { addRef(value); }
extern "C" std::uint32_t dxmt9c_surface_release(D9CSurface* value) {
  recordRelease(D9C_CHUNK_HANDLE_KIND_SURFACE, value);
  return release(value);
}
extern "C" void dxmt9c_texture_addref(D9CTexture* value) { addRef(value); }
extern "C" std::uint32_t dxmt9c_texture_release(D9CTexture* value) {
  recordRelease(D9C_CHUNK_HANDLE_KIND_TEXTURE, value);
  return release(value);
}
extern "C" void dxmt9c_buffer_addref(D9CBuffer* value) { addRef(value); }
extern "C" std::uint32_t dxmt9c_buffer_release(D9CBuffer* value) {
  recordRelease(D9C_CHUNK_HANDLE_KIND_BUFFER, value);
  return release(value);
}
extern "C" void dxmt9c_shader_addref(D9CShader* value) { addRef(value); }
extern "C" std::uint32_t dxmt9c_shader_release(D9CShader* value) {
  recordRelease(D9C_CHUNK_HANDLE_KIND_SHADER, value);
  return release(value);
}
extern "C" void dxmt9c_vdecl_addref(D9CVertexDecl* value) { addRef(value); }
extern "C" std::uint32_t dxmt9c_vdecl_release(D9CVertexDecl* value) {
  recordRelease(D9C_CHUNK_HANDLE_KIND_VERTEX_DECL, value);
  return release(value);
}
extern "C" void dxmt9c_query_addref(D9CQuery* value) { addRef(value); }
extern "C" std::uint32_t dxmt9c_query_release(D9CQuery* value) {
  recordRelease(D9C_CHUNK_HANDLE_KIND_QUERY, value);
  return release(value);
}

namespace {

bool check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "pe_retainer_spec failed: " << message << '\n';
  }
  return condition;
}

struct SerialReferenceRetainer {
  static constexpr std::size_t kResourceCount = 3u;
  static constexpr std::uint64_t kWarmEpochs = 1u;

  struct Entry {
    std::uint8_t id = 0xffu;
    std::uint64_t touched = 0u;
  };

  std::array<Entry, kResourceCount> entries{};
  std::array<std::uint32_t, kResourceCount> refs{1u, 1u, 1u};
  std::size_t capacity = 0u;
  std::size_t size = 0u;
  std::uint64_t epoch = 0u;

  SerialReferenceRetainer(std::size_t capacity_, std::uint64_t epoch_)
      : capacity(capacity_), epoch(epoch_) {}

  std::size_t beginAcquire() const noexcept { return size; }

  bool contains(std::uint8_t id) const noexcept {
    for (std::size_t i = 0u; i < size; ++i) {
      if (entries[i].id == id) return true;
    }
    return false;
  }

  bool retain(std::uint8_t id) noexcept {
    for (std::size_t i = 0u; i < size; ++i) {
      if (entries[i].id == id) {
        entries[i].touched = epoch;
        return true;
      }
    }
    if (size == capacity) return false;
    entries[size++] = Entry{id, epoch};
    ++refs[id];
    return true;
  }

  void rollback(std::size_t checkpoint, ReleaseTrace& releases) noexcept {
    while (size > checkpoint) {
      const auto id = entries[--size].id;
      releases.recordId(id, D9C_CHUNK_HANDLE_KIND_BUFFER);
      --refs[id];
    }
  }

  void endEpoch(ReleaseTrace& releases) noexcept {
    std::size_t write = 0u;
    for (std::size_t read = 0u; read < size; ++read) {
      const Entry entry = entries[read];
      if (epoch - entry.touched <= kWarmEpochs) {
        entries[write++] = entry;
      } else {
        releases.recordId(entry.id, D9C_CHUNK_HANDLE_KIND_BUFFER);
        --refs[entry.id];
      }
    }
    size = write;
    ++epoch;
  }

  void clear(ReleaseTrace& releases) noexcept {
    while (size != 0u) {
      const auto id = entries[--size].id;
      releases.recordId(id, D9C_CHUNK_HANDLE_KIND_BUFFER);
      --refs[id];
    }
  }
};

enum class RetainAction : std::uint8_t {
  RetainA,
  RetainB,
  RetainC,
  Begin,
  Commit,
  Rollback,
  EndEpoch,
  Clear,
};

struct ActionResult {
  bool actual = true;
  bool expected = true;
};

struct RetainerTraceCase {
  D9CBuffer a{}, b{}, c{};
  std::array<D9CBuffer*, 3u> objects{&a, &b, &c};
  D3D9PePendingCommandRetainer actual;
  SerialReferenceRetainer expected;
  D3D9PePendingCommandRetainer::Acquired actualCheckpoint{};
  std::size_t expectedCheckpoint = 0u;
  std::size_t capacity = 0u;
  bool active = false;
  bool epochProjectionMatches = true;

  RetainerTraceCase(std::size_t capacity_, std::uint64_t epoch)
      : actual(capacity_, D3D9PePendingCommandRetainer::EpochForTesting{epoch}),
        expected(capacity_, epoch), capacity(capacity_) {}

  void begin() noexcept {
    actualCheckpoint = actual.beginAcquire();
    expectedCheckpoint = expected.beginAcquire();
    active = true;
  }

  ActionResult step(RetainAction action, ReleaseTrace& actualReleases,
                    ReleaseTrace& expectedReleases) {
    actualReleases.reset();
    expectedReleases.reset();
    gReleaseTrace = &actualReleases;
    ActionResult result{};
    switch (action) {
    case RetainAction::RetainA:
    case RetainAction::RetainB:
    case RetainAction::RetainC: {
      if (!active) begin();
      const auto id = static_cast<std::uint8_t>(action);
      result.actual = actual.retainBuffer(objects[id], actualCheckpoint,
                                          capacity);
      result.expected = expected.retain(id);
      break;
    }
    case RetainAction::Begin:
      active = false;
      begin();
      break;
    case RetainAction::Commit:
      active = false;
      break;
    case RetainAction::Rollback:
      if (active) {
        actual.rollback(actualCheckpoint);
        expected.rollback(expectedCheckpoint, expectedReleases);
      }
      active = false;
      break;
    case RetainAction::EndEpoch:
    {
      active = false;
      const auto actualBefore = actual.epochSnapshot();
      const auto expectedBeforeSize = expected.size;
      const auto expectedBeforeEpoch = expected.epoch;
      actual.endEpoch();
      expected.endEpoch(expectedReleases);
      const auto actualAfter = actual.epochSnapshot();
      std::size_t expectedCurrentTouched = 0u;
      std::size_t expectedPreviousTouched = 0u;
      for (std::size_t i = 0u; i < expected.size; ++i) {
        if (expected.entries[i].touched == expectedBeforeEpoch) {
          ++expectedCurrentTouched;
        } else if (expectedBeforeEpoch - expected.entries[i].touched == 1u) {
          ++expectedPreviousTouched;
        }
      }
      const auto projection = projectRetainerEpoch(actualBefore, actualAfter);
      const auto expectedExpired = expectedBeforeSize - expected.size;
      const auto expectedDisposition =
          expectedExpired != 0u
              ? RetainerEpochDisposition::ExpiryRebuild
              : actualBefore.indexUsed != actualBefore.indexOccupied
                    ? RetainerEpochDisposition::FastTombstoneRebuild
                    : RetainerEpochDisposition::FastClean;
      epochProjectionMatches =
          epochProjectionMatches && projection.valid &&
          projection.entries == expectedBeforeSize &&
          projection.survivors == expected.size &&
          projection.expired == expectedExpired &&
          projection.disposition == expectedDisposition &&
          actualBefore.currentTouched == expectedCurrentTouched &&
          actualBefore.previousTouched == expectedPreviousTouched;
      break;
    }
    case RetainAction::Clear:
      active = false;
      actual.clear();
      expected.clear(expectedReleases);
      break;
    }
    gReleaseTrace = nullptr;
    return result;
  }

  bool sameState() const noexcept {
    if (actual.size() != expected.size || !epochProjectionMatches) return false;
    const auto snapshot = actual.epochSnapshot();
    std::size_t currentTouched = 0u;
    std::size_t previousTouched = 0u;
    for (std::size_t i = 0u; i < expected.size; ++i) {
      if (expected.entries[i].touched == expected.epoch) {
        ++currentTouched;
      } else if (expected.epoch - expected.entries[i].touched == 1u) {
        ++previousTouched;
      }
    }
    if (snapshot.entryCount != expected.size ||
        snapshot.indexOccupied != expected.size ||
        snapshot.currentTouched != currentTouched ||
        snapshot.previousTouched != previousTouched ||
        snapshot.epoch != expected.epoch)
      return false;
    for (std::size_t i = 0u; i < objects.size(); ++i) {
      if (actual.containsBuffer(objects[i]) != expected.contains(i) ||
          objects[i]->refs != expected.refs[i])
        return false;
    }
    return true;
  }
};

bool sameReleaseTrace(const ReleaseTrace& actual,
                      const ReleaseTrace& expected) noexcept {
  if (actual.overflow || expected.overflow || actual.count != expected.count)
    return false;
  for (std::size_t i = 0u; i < actual.count; ++i) {
    if (actual.events[i].id != expected.events[i].id ||
        actual.events[i].kind != expected.events[i].kind)
      return false;
  }
  return true;
}

bool boundedAgeSummaryTracesMatchSerialReference() {
  // Five-step traces over {retain A, retain B, retain C, begin, commit,
  // rollback, endEpoch, clear}: every shorter trace is a prefix. This is
  // 8^5 sequences per capacity/epoch configuration, 393,216 total. The
  // model intentionally excludes allocator failure, recorder fault injection,
  // invalid handle kinds, and cross-kind pointer aliases; those have focused
  // fixtures below or elsewhere in this spec.
  constexpr std::array<std::size_t, 3u> kCapacities{1u, 2u, 3u};
  constexpr std::array<std::uint64_t, 4u> kStartingEpochs{
      0u, std::numeric_limits<std::uint64_t>::max() - 2u,
      std::numeric_limits<std::uint64_t>::max() - 1u,
      std::numeric_limits<std::uint64_t>::max()};
  constexpr unsigned kActionCount = 8u;
  constexpr unsigned kTraceLength = 5u;
  constexpr std::size_t kSequenceCount = [] {
    std::size_t count = 1u;
    for (unsigned depth = 0u; depth < kTraceLength; ++depth)
      count *= kActionCount;
    return count;
  }();
  constexpr std::size_t kTraceCount = kCapacities.size() *
      kStartingEpochs.size() * kSequenceCount;
  static_assert(kTraceCount == 393216u);

  ReleaseTrace actualReleases;
  ReleaseTrace expectedReleases;
  for (const auto capacity : kCapacities) {
    for (const auto epoch : kStartingEpochs) {
      for (std::size_t sequence = 0u; sequence < kSequenceCount; ++sequence) {
        RetainerTraceCase trace(capacity, epoch);
        actualReleases = ReleaseTrace{};
        actualReleases.bind(0u, D9C_CHUNK_HANDLE_KIND_BUFFER, trace.objects[0]);
        actualReleases.bind(1u, D9C_CHUNK_HANDLE_KIND_BUFFER, trace.objects[1]);
        actualReleases.bind(2u, D9C_CHUNK_HANDLE_KIND_BUFFER, trace.objects[2]);
        auto remaining = sequence;
        bool matches = true;
        for (unsigned depth = 0u; depth < kTraceLength; ++depth) {
          const auto action = static_cast<RetainAction>(remaining % kActionCount);
          remaining /= kActionCount;
          const auto result =
              trace.step(action, actualReleases, expectedReleases);
          matches = result.actual == result.expected &&
              trace.sameState() &&
              sameReleaseTrace(actualReleases, expectedReleases);
          if (!matches) {
            std::cerr << "serial retainer mismatch: capacity=" << capacity
                      << " epoch=" << epoch << " sequence=" << sequence
                      << " depth=" << depth << '\n';
            gReleaseTrace = nullptr;
            return false;
          }
        }
        const auto cleanup =
            trace.step(RetainAction::Clear, actualReleases, expectedReleases);
        if (cleanup.actual != cleanup.expected || !trace.sameState() ||
            !sameReleaseTrace(actualReleases, expectedReleases)) {
          std::cerr << "serial retainer cleanup mismatch: capacity="
                    << capacity << " epoch=" << epoch
                    << " sequence=" << sequence << '\n';
          gReleaseTrace = nullptr;
          return false;
        }
      }
    }
  }
  return true;
}

bool sameAddressDifferentKindsAreIndependent() {
  D9CBuffer object{};
  aliasedCounter = &object;
  D3D9PePendingCommandRetainer retainer(2u);
  auto acquired = retainer.beginAcquire();
  const bool okay = retainer.retainBuffer(&object, acquired, 2u) &&
      retainer.retainQuery(reinterpret_cast<D9CQuery*>(&object), acquired, 2u) &&
      object.refs == 3u && retainer.size() == 2u;
  retainer.clear();
  aliasedCounter = nullptr;
  return okay && object.refs == 1u;
}

bool warmBoundaryAndTombstoneTrace() {
  struct TinyReference {
    enum : std::size_t { kCapacity = 4u };
    std::array<D9CBuffer*, kCapacity> objects{};
    std::array<std::uint64_t, kCapacity> touched{};
    std::size_t size = 0u;
    std::uint64_t epoch = 0u;

    std::size_t beginAcquire() const noexcept { return size; }

    bool retain(D9CBuffer* object) noexcept {
      if (!object) return true;
      for (std::size_t i = 0u; i < size; ++i) {
        if (objects[i] == object) {
          touched[i] = epoch;
          return true;
        }
      }
      if (size == kCapacity) return false;
      objects[size] = object;
      touched[size] = epoch;
      ++size;
      ++object->refs;
      return true;
    }

    void rollback(std::size_t checkpoint) noexcept {
      while (size > checkpoint) {
        --objects[size - 1u]->refs;
        --size;
      }
    }

    void endEpoch() noexcept {
      std::size_t write = 0u;
      for (std::size_t read = 0u; read < size; ++read) {
        if (epoch - touched[read] <= D3D9PePendingCommandRetainer::kWarmEpochs) {
          objects[write] = objects[read];
          touched[write] = touched[read];
          ++write;
        } else {
          --objects[read]->refs;
        }
      }
      size = write;
      ++epoch;
    }

    void clear() noexcept {
      while (size != 0u) {
        --objects[size - 1u]->refs;
        --size;
      }
    }
  };

  D9CBuffer actualA{};
  D9CBuffer actualB{};
  D9CBuffer actualC{};
  D9CBuffer actualD{};
  D9CBuffer referenceA{};
  D9CBuffer referenceB{};
  D9CBuffer referenceC{};
  D9CBuffer referenceD{};
  D3D9PePendingCommandRetainer actual(4u);
  TinyReference reference;
  auto equivalent = [&]() noexcept {
    return actual.size() == reference.size &&
           actualA.refs == referenceA.refs && actualB.refs == referenceB.refs &&
           actualC.refs == referenceC.refs && actualD.refs == referenceD.refs;
  };
  auto acquire = [&]() noexcept {
    return std::pair{actual.beginAcquire(), reference.beginAcquire()};
  };
  auto retain = [&](D9CBuffer* actualObject, D9CBuffer* referenceObject,
                    auto& checkpoints) noexcept {
    return actual.retainBuffer(actualObject, checkpoints.first) &&
           reference.retain(referenceObject);
  };
  auto settle = [&]() noexcept {
    actual.endEpoch();
    reference.endEpoch();
    return equivalent();
  };

  auto checkpoints = acquire();
  if (!retain(&actualA, &referenceA, checkpoints) ||
      !retain(&actualB, &referenceB, checkpoints) ||
      !retain(&actualC, &referenceC, checkpoints) || !equivalent())
    return false;
  // The production condition structurally skips rebuild here: no entry
  // moved or expired and used == occupied. Allocation counting is separate
  // evidence that this clean-boundary trace also stays within reserved space.
  gAllocationCount.store(0u, std::memory_order_relaxed);
  gCountAllocations.store(true, std::memory_order_release);
  const bool firstSettled = settle();
  gCountAllocations.store(false, std::memory_order_release);
  if (!firstSettled ||
      gAllocationCount.load(std::memory_order_relaxed) != 0u)
    return false;
  checkpoints = acquire();
  if (!retain(&actualA, &referenceA, checkpoints) ||
      !retain(&actualC, &referenceC, checkpoints) || !settle())
    return false;
  // The middle entry expires after its second idle boundary and forces
  // survivor movement; <= 1 keeps it alive through the preceding boundary.
  checkpoints = acquire();
  if (!retain(&actualA, &referenceA, checkpoints) ||
      !retain(&actualC, &referenceC, checkpoints) || !settle() ||
      actualB.refs != 1u || actual.size() != 2u)
    return false;

  checkpoints = acquire();
  if (!retain(&actualD, &referenceD, checkpoints) || !settle()) return false;
  checkpoints = acquire();
  if (!retain(&actualA, &referenceA, checkpoints) ||
      !retain(&actualC, &referenceC, checkpoints) || !settle())
    return false;
  // D is a tail entry, so its expiry does not move a survivor.
  checkpoints = acquire();
  if (!retain(&actualA, &referenceA, checkpoints) ||
      !retain(&actualC, &referenceC, checkpoints) || !settle() ||
      actualD.refs != 1u || actual.size() != 2u)
    return false;

  checkpoints = acquire();
  if (!retain(&actualD, &referenceD, checkpoints) ||
      !retain(&actualD, &referenceD, checkpoints) ||
      !actual.retainBuffer(nullptr, checkpoints.first) ||
      !reference.retain(nullptr) || actual.size() != reference.size ||
      actualD.refs != referenceD.refs)
    return false;
  actual.rollback(checkpoints.first);
  reference.rollback(checkpoints.second);
  if (!equivalent() || actualD.refs != 1u) return false;
  checkpoints = acquire();
  if (!retain(&actualD, &referenceD, checkpoints) || !equivalent())
    return false;
  actual.clear();
  reference.clear();
  return equivalent() && actualA.refs == 1u && actualB.refs == 1u &&
         actualC.refs == 1u && actualD.refs == 1u;
}

bool pureHeadroomAndMembershipTrace() {
  D3D9PePendingCommandRetainer unavailable(
      4u, D3D9PePendingCommandRetainer::ConstructionMode::FailForTesting);
  D9CBuffer unavailableObject{};
  if (unavailable.hasHeadroom(0u, 4u) ||
      unavailable.containsWireObject(D9C_CHUNK_HANDLE_KIND_BUFFER,
                                     &unavailableObject) ||
      unavailable.containsWireObject(0u, &unavailableObject) ||
      unavailable.containsWireObject(D9C_CHUNK_HANDLE_KIND_BUFFER, nullptr))
    return false;

  constexpr std::size_t kCapacity = 4u;
  D3D9PePendingCommandRetainer retainer(kCapacity);
  std::array<D9CBuffer, kCapacity> objects{};
  std::array<D9CBuffer, kCapacity> unknown{};
  for (std::size_t count = 0u; count <= kCapacity; ++count) {
    if (count != 0u) {
      auto acquired = retainer.beginAcquire();
      if (!retainer.retainBuffer(&objects[count - 1u], acquired))
        return false;
    }
    for (std::size_t capacity = 0u; capacity <= kCapacity + 1u;
         ++capacity) {
      for (std::size_t add = 0u; add <= kCapacity + 1u; ++add) {
        const bool expected = retainer.constructionSucceeded() &&
            capacity <= kCapacity && retainer.size() <= capacity &&
            add <= capacity - retainer.size();
        if (retainer.hasHeadroom(add, capacity) != expected) return false;
      }
    }
    for (std::size_t i = 0u; i < count; ++i) {
      if (!retainer.containsWireObject(D9C_CHUNK_HANDLE_KIND_BUFFER,
                                       &objects[i]) ||
          retainer.containsWireObject(D9C_CHUNK_HANDLE_KIND_TEXTURE,
                                      &unknown[i]))
        return false;
    }
  }

  // The pure probes remain allocation-free at a full aggregate ceiling. This
  // is the mixed-kind/warm counterexample plan for main: each per-kind
  // MaxPins quota may fit while one novel kind cannot fit the warm arena.
  gAllocationCount.store(0u, std::memory_order_relaxed);
  gCountAllocations.store(true, std::memory_order_release);
  const bool fullWarm = retainer.hasHeadroom(0u, kCapacity) &&
      !retainer.hasHeadroom(1u, kCapacity) &&
      retainer.containsWireObject(D9C_CHUNK_HANDLE_KIND_BUFFER, &objects[0]);
  gCountAllocations.store(false, std::memory_order_release);
  if (!fullWarm || gAllocationCount.load(std::memory_order_relaxed) != 0u)
    return false;

  retainer.endEpoch();
  if (!retainer.containsWireObject(D9C_CHUNK_HANDLE_KIND_BUFFER, &objects[0]) ||
      !retainer.hasHeadroom(0u, kCapacity))
    return false;
  retainer.endEpoch();
  retainer.endEpoch();
  return !retainer.containsWireObject(D9C_CHUNK_HANDLE_KIND_BUFFER,
                                      &objects[0]);
}

bool epochWrapTrace() {
  struct Reference {
    std::array<D9CBuffer*, 2u> objects{};
    std::array<std::uint64_t, 2u> touched{};
    std::size_t size = 0u;
    std::uint64_t epoch = 0u;

    explicit Reference(std::uint64_t start) : epoch(start) {}

    void retain(D9CBuffer* object) noexcept {
      for (std::size_t i = 0u; i < size; ++i) {
        if (objects[i] == object) {
          touched[i] = epoch;
          return;
        }
      }
      objects[size] = object;
      touched[size++] = epoch;
    }

    void endEpoch() noexcept {
      std::size_t write = 0u;
      for (std::size_t read = 0u; read < size; ++read) {
        if (epoch - touched[read] <=
            D3D9PePendingCommandRetainer::kWarmEpochs) {
          objects[write] = objects[read];
          touched[write++] = touched[read];
        }
      }
      size = write;
      ++epoch;
    }
  };

  D3D9PePendingCommandRetainer retainer(
      4u, D3D9PePendingCommandRetainer::EpochForTesting{
               std::numeric_limits<std::uint64_t>::max() - 1u});
  Reference reference(std::numeric_limits<std::uint64_t>::max() - 1u);
  D9CBuffer hot{};
  D9CBuffer second{};
  D9CBuffer referenceHot{};
  D9CBuffer referenceSecond{};

  auto acquire = retainer.beginAcquire();
  if (!retainer.retainBuffer(&hot, acquire)) return false;
  reference.retain(&referenceHot);
  retainer.endEpoch();  // max - 1 -> max; hot is one epoch old.
  reference.endEpoch();

  acquire = retainer.beginAcquire();
  if (!retainer.retainBuffer(&hot, acquire) ||
      !retainer.retainBuffer(&second, acquire))
    return false;
  reference.retain(&referenceHot);
  reference.retain(&referenceSecond);
  retainer.endEpoch();  // max -> 0; both last touches cross the wrap.
  reference.endEpoch();
  if (hot.refs != 2u || second.refs != 2u || retainer.size() != reference.size)
    return false;

  retainer.endEpoch();  // 0: max-touch entries have unsigned age one.
  reference.endEpoch();
  if (hot.refs != 2u || second.refs != 2u || retainer.size() != reference.size)
    return false;
  retainer.endEpoch();  // 1: unsigned age two expires both, in arena order.
  reference.endEpoch();
  return hot.refs == 1u && second.refs == 1u &&
         retainer.size() == reference.size && reference.size == 0u;
}

bool epochSnapshotProjectionTrace() {
  using Retainer = D3D9PePendingCommandRetainer;
  using Snapshot = Retainer::EpochSnapshot;

  {
    Retainer empty(4u);
    const auto before = empty.epochSnapshot();
    empty.endEpoch();
    const auto after = empty.epochSnapshot();
    const auto projection = projectRetainerEpoch(before, after);
    if (!check(projection.valid && projection.entries == 0u &&
                   projection.survivors == 0u && projection.expired == 0u &&
                   projection.disposition == RetainerEpochDisposition::FastClean,
               "empty epoch snapshot projects as a clean zero-entry boundary"))
      return false;
  }

  {
    D9CBuffer first{};
    D9CBuffer second{};
    Retainer clean(4u);
    auto acquire = clean.beginAcquire();
    clean.retainBuffer(&first, acquire);
    clean.retainBuffer(&second, acquire);
    const auto before = clean.epochSnapshot();
    clean.endEpoch();
    const auto after = clean.epochSnapshot();
    const auto projection = projectRetainerEpoch(before, after);
    if (!check(projection.valid && projection.entries == 2u &&
                   projection.survivors == 2u && projection.expired == 0u &&
                   projection.disposition == RetainerEpochDisposition::FastClean,
               "no-expiry epoch snapshot preserves entries and refs"))
      return false;
    clean.clear();
  }

  {
    D9CBuffer first{};
    D9CBuffer rolledBack{};
    Retainer tombstones(4u);
    auto acquire = tombstones.beginAcquire();
    tombstones.retainBuffer(&first, acquire);
    acquire = tombstones.beginAcquire();
    tombstones.retainBuffer(&rolledBack, acquire);
    tombstones.rollback(acquire);
    const auto before = tombstones.epochSnapshot();
    tombstones.endEpoch();
    const auto after = tombstones.epochSnapshot();
    const auto projection = projectRetainerEpoch(before, after);
    if (!check(projection.valid && projection.expired == 0u &&
                   projection.tombstonesBefore != 0u &&
                   projection.disposition ==
                       RetainerEpochDisposition::FastTombstoneRebuild,
               "rollback tombstones classify as a tombstone-only rebuild"))
      return false;
    tombstones.clear();
  }

  {
    D9CBuffer hot{};
    D9CBuffer cold{};
    Retainer expiry(4u);
    auto acquire = expiry.beginAcquire();
    expiry.retainBuffer(&hot, acquire);
    expiry.retainBuffer(&cold, acquire);
    expiry.endEpoch();
    acquire = expiry.beginAcquire();
    expiry.retainBuffer(&hot, acquire);
    expiry.endEpoch();
    const auto before = expiry.epochSnapshot();
    expiry.endEpoch();
    const auto after = expiry.epochSnapshot();
    const auto projection = projectRetainerEpoch(before, after);
    if (!check(projection.valid && projection.entries == 2u &&
                   projection.survivors == 1u && projection.expired == 1u &&
                   projection.disposition == RetainerEpochDisposition::ExpiryRebuild &&
                   hot.refs == 2u && cold.refs == 1u,
               "expiry compaction reports one survivor and one expired entry"))
      return false;
    expiry.clear();
  }

  {
    Retainer wrapped(4u, Retainer::EpochForTesting{
                                 std::numeric_limits<std::uint64_t>::max()});
    const Snapshot before = wrapped.epochSnapshot();
    wrapped.endEpoch();
    const Snapshot after = wrapped.epochSnapshot();
    const auto projection = projectRetainerEpoch(before, after);
    if (!check(projection.valid && after.epoch == 0u,
               "epoch projection accepts unsigned uint64 wrap"))
      return false;
  }

  const Snapshot validBefore{2u, 1u, 1u, 2u, 2u, 4u, 7u};
  Snapshot malformedAfter{2u, 0u, 1u, 2u, 2u, 4u, 7u};
  if (!check(!projectRetainerEpoch(validBefore, malformedAfter).valid,
             "projection rejects a non-advancing epoch"))
    return false;
  malformedAfter = Snapshot{3u, 0u, 1u, 3u, 3u, 4u, 8u};
  if (!check(!projectRetainerEpoch(validBefore, malformedAfter).valid,
             "projection rejects impossible post-index conservation"))
    return false;
  malformedAfter = Snapshot{2u, 0u, 1u, 2u, 2u, 3u, 8u};
  if (!check(!projectRetainerEpoch(validBefore, malformedAfter).valid,
             "projection rejects an index-slot capacity change"))
    return false;
  const Snapshot missingBefore{2u, 1u, 1u, 1u, 1u, 4u, 7u};
  const Snapshot missingAfter{2u, 0u, 1u, 2u, 2u, 4u, 8u};
  if (!check(!projectRetainerEpoch(missingBefore, missingAfter).valid,
             "projection rejects missing pre-epoch index entries"))
    return false;
  const Snapshot overflowingBefore{
      std::numeric_limits<std::uint64_t>::max(),
      std::numeric_limits<std::uint64_t>::max(), 1u,
      std::numeric_limits<std::uint64_t>::max(),
      std::numeric_limits<std::uint64_t>::max(),
      std::numeric_limits<std::uint64_t>::max(), 9u};
  if (!check(!projectRetainerEpoch(overflowingBefore, malformedAfter).valid,
             "projection rejects overflowing touched-count conservation"))
    return false;
  return true;
}

}  // namespace

int main() {
  if (!check(boundedAgeSummaryTracesMatchSerialReference(),
             "bounded age-summary traces match an independent serial reference"))
    return 1;
  if (!check(sameAddressDifferentKindsAreIndependent(),
             "one opaque address retains two independent kind-qualified pins"))
    return 1;
  if (!check(warmBoundaryAndTombstoneTrace(),
             "warm/tombstone reference preserves release and allocation bounds"))
    return 1;
  if (!check(pureHeadroomAndMembershipTrace(),
             "pure aggregate headroom and qualified membership probes"))
    return 1;
  if (!check(epochWrapTrace(),
             "warm expiry preserves unsigned uint64 epoch wrap semantics"))
    return 1;
  if (!check(epochSnapshotProjectionTrace(),
             "cold retainer snapshots classify clean, tombstone, expiry, and malformed epochs"))
    return 1;

  D3D9PePendingCommandRetainer retainer;
  D9CQuery first{};
  D9CQuery second{};

  auto firstAcquire = retainer.beginAcquire();
  retainer.retainQuery(&first, firstAcquire);
  retainer.retainQuery(&first, firstAcquire);
  if (!check(first.refs == 2u, "query is addref'd exactly once") ||
      !check(retainer.size() == 1u, "query occupies one flat-set entry")) {
    return 1;
  }

  auto rollbackAcquire = retainer.beginAcquire();
  retainer.retainQuery(&first, rollbackAcquire);
  retainer.retainQuery(&second, rollbackAcquire);
  if (!check(first.refs == 2u, "existing query is not re-retained") ||
      !check(second.refs == 2u, "new query is retained")) {
    return 1;
  }
  retainer.rollback(rollbackAcquire);
  if (!check(first.refs == 2u, "rollback preserves pre-checkpoint query") ||
      !check(second.refs == 1u, "rollback releases new query") ||
      !check(retainer.size() == 1u, "rollback restores flat arena checkpoint")) {
    return 1;
  }

  auto objectAcquire = retainer.beginAcquire();
  retainer.retainWireObject(D9C_CHUNK_HANDLE_KIND_QUERY, &first,
                            objectAcquire);
  if (!check(first.refs == 2u, "typed canonical retain de-duplicates query")) {
    return 1;
  }

  retainer.clear();
  if (!check(first.refs == 1u, "clear releases retained query") ||
      !check(retainer.size() == 0u, "clear preserves an empty flat set")) {
    return 1;
  }

  // --- Cross-epoch pin aggregation -------------------------------------
  // A chunk boundary is endEpoch(); a discard is clear(). The point of the
  // former is that an object named by consecutive chunks is addref'd once, not
  // once per chunk, so the wire pair never happens.
  {
    D3D9PePendingCommandRetainer warm;
    D9CBuffer hot{};
    D9CBuffer cold{};

    for (int chunk = 0; chunk < 5; ++chunk) {
      auto acquire = warm.beginAcquire();
      warm.retainBuffer(&hot, acquire);
      if (chunk == 0) {
        warm.retainBuffer(&cold, acquire);
      }
      warm.endEpoch();
    }
    if (!check(hot.refs == 2u,
               "an object named every chunk is addref'd exactly once") ||
        !check(warm.size() == 1u,
               "the cold entry is evicted and the hot one is kept")) {
      return 1;
    }
    // cold was named only in epoch 0; kWarmEpochs == 1 keeps it through the
    // end of epoch 1 and releases it when epoch 2 closes.
    if (!check(cold.refs == 1u, "a cold entry releases its pin on eviction")) {
      return 1;
    }

    // Naming it again after eviction takes a fresh pin.
    auto reacquire = warm.beginAcquire();
    warm.retainBuffer(&cold, reacquire);
    if (!check(cold.refs == 2u, "an evicted object is re-pinned when named") ||
        !check(warm.size() == 2u, "re-pinning appends a new arena entry")) {
      return 1;
    }

    // A record that only dedupe-hits a warm entry must not release it on
    // rollback: the entry belongs to an earlier epoch, below the checkpoint.
    auto rollbackWarm = warm.beginAcquire();
    warm.retainBuffer(&hot, rollbackWarm);
    warm.rollback(rollbackWarm);
    if (!check(hot.refs == 2u,
               "rollback does not release a warm entry it only re-touched") ||
        !check(warm.size() == 2u, "rollback leaves the warm arena intact")) {
      return 1;
    }

    // Exact eviction boundary: touching an entry inside the warm window keeps
    // it, and one epoch past the window drops it.
    D9CBuffer boundary{};
    auto boundaryAcquire = warm.beginAcquire();
    warm.retainBuffer(&boundary, boundaryAcquire);
    warm.endEpoch();  // closes the epoch it was named in
    if (!check(boundary.refs == 2u, "an entry survives its own epoch close")) {
      return 1;
    }
    warm.endEpoch();  // one fully idle epoch, still inside kWarmEpochs
    if (!check(boundary.refs == 2u, "an entry survives one idle epoch")) {
      return 1;
    }
    warm.endEpoch();  // second idle epoch, now cold
    if (!check(boundary.refs == 1u,
               "an entry is released after kWarmEpochs + 1 idle epochs")) {
      return 1;
    }

    // Interleaved kinds share one arena and one epoch clock.
    D9CTexture texture{};
    D9CShader shader{};
    auto mixedAcquire = warm.beginAcquire();
    warm.retainWireObject(D9C_CHUNK_HANDLE_KIND_TEXTURE, &texture,
                          mixedAcquire);
    warm.retainWireObject(D9C_CHUNK_HANDLE_KIND_SHADER, &shader, mixedAcquire);
    warm.endEpoch();
    auto mixedAgain = warm.beginAcquire();
    warm.retainWireObject(D9C_CHUNK_HANDLE_KIND_TEXTURE, &texture, mixedAgain);
    warm.retainWireObject(D9C_CHUNK_HANDLE_KIND_SHADER, &shader, mixedAgain);
    warm.endEpoch();
    if (!check(texture.refs == 2u, "texture pin survives a chunk boundary") ||
        !check(shader.refs == 2u, "shader pin survives a chunk boundary")) {
      return 1;
    }

    // The three idle epochs above evicted `hot` and `cold` as well; re-pin
    // them so the discard assertions below are not vacuous.
    auto finalAcquire = warm.beginAcquire();
    warm.retainBuffer(&hot, finalAcquire);
    warm.retainBuffer(&cold, finalAcquire);
    if (!check(hot.refs == 2u && cold.refs == 2u,
               "both buffers are pinned again before the discard check")) {
      return 1;
    }

    // The discard path must release every warm pin, whatever its epoch — this
    // is what device teardown / Reset / ResetEx rely on so nothing is still
    // pinned when dxmt9c_device_reset* runs.
    warm.clear();
    if (!check(hot.refs == 1u, "clear releases a warm pin") ||
        !check(cold.refs == 1u, "clear releases a re-pinned entry") ||
        !check(texture.refs == 1u, "clear releases a warm texture pin") ||
        !check(shader.refs == 1u, "clear releases a warm shader pin") ||
        !check(warm.size() == 0u, "clear empties the warm arena")) {
      return 1;
    }
  }

  // --- R-BACK-43.7 RetentionIndex consistency pins ---------------------
  // retain()'s duplicate check moved from an O(n) std::find_if scan to a
  // (kind, ptr) -> index accelerator that must stay in lockstep with
  // entries_ across rollback, endEpoch compaction, and growth.

  // retain-after-rollback: the index must not resolve a tombstoned/removed
  // slot to a stale entries_ index, and a fresh retain of the same pointer
  // must dedupe correctly afterward.
  {
    D3D9PePendingCommandRetainer idx;
    D9CBuffer obj{};

    auto acquire = idx.beginAcquire();
    idx.retainBuffer(&obj, acquire);
    if (!check(obj.refs == 2u,
               "retain-after-rollback: initial retain addrefs once")) {
      return 1;
    }
    idx.rollback(acquire);
    if (!check(obj.refs == 1u,
               "retain-after-rollback: rollback releases the pin") ||
        !check(idx.size() == 0u,
               "retain-after-rollback: rollback empties the arena")) {
      return 1;
    }
    auto reacquire = idx.beginAcquire();
    idx.retainBuffer(&obj, reacquire);
    if (!check(obj.refs == 2u,
               "retain-after-rollback: re-retain addrefs fresh") ||
        !check(idx.size() == 1u,
               "retain-after-rollback: re-retain occupies one slot")) {
      return 1;
    }
    // A second retain in the same acquire must dedupe through the rebuilt
    // index, not double-addref via a stale erased entry.
    idx.retainBuffer(&obj, reacquire);
    if (!check(obj.refs == 2u,
               "retain-after-rollback: repeat retain dedupes through the "
               "reused slot") ||
        !check(idx.size() == 1u,
               "retain-after-rollback: dedupe does not grow the arena")) {
      return 1;
    }
    idx.clear();
    if (!check(obj.refs == 1u, "retain-after-rollback: clear releases the pin")) {
      return 1;
    }
  }

  // endEpoch compaction rebuild: when a middle entry is evicted, every
  // surviving entry's index shifts. The accelerator must be rebuilt so a
  // dedupe after compaction resolves to the correct (shifted) entry and
  // never falls back to a stale index into entries_.
  {
    D3D9PePendingCommandRetainer idx;
    D9CBuffer keepFirst{};
    D9CBuffer dropMiddle{};
    D9CBuffer keepLast{};

    auto a = idx.beginAcquire();
    idx.retainBuffer(&keepFirst, a);
    idx.retainBuffer(&dropMiddle, a);
    idx.retainBuffer(&keepLast, a);
    idx.endEpoch();  // closes the epoch all three were named in: all survive.

    auto b = idx.beginAcquire();
    idx.retainBuffer(&keepFirst, b);
    idx.retainBuffer(&keepLast, b);
    idx.endEpoch();  // dropMiddle idle 1 epoch: still inside kWarmEpochs.

    auto c = idx.beginAcquire();
    idx.retainBuffer(&keepFirst, c);
    idx.retainBuffer(&keepLast, c);
    idx.endEpoch();  // dropMiddle idle 2 epochs: evicted. keepLast's
                      // position in entries_ shifts down by one.

    if (!check(dropMiddle.refs == 1u,
               "endEpoch compaction: the cold middle entry is evicted") ||
        !check(keepFirst.refs == 2u && keepLast.refs == 2u,
               "endEpoch compaction: the warm entries around it survive") ||
        !check(idx.size() == 2u,
               "endEpoch compaction: the arena shrinks to the survivors")) {
      return 1;
    }

    // Dedupe against the shifted entry must hit the rebuilt index, not a
    // stale position (which after compaction could alias dropMiddle's old
    // slot or an out-of-range index).
    auto d = idx.beginAcquire();
    idx.retainBuffer(&keepLast, d);
    if (!check(keepLast.refs == 2u,
               "endEpoch compaction: dedupe after rebuild does not "
               "re-addref the shifted entry") ||
        !check(idx.size() == 2u,
               "endEpoch compaction: dedupe after rebuild does not grow "
               "the arena")) {
      return 1;
    }

    // A brand-new object retained right after the rebuild must still index
    // correctly (appended at the next free position, not colliding with a
    // stale slot the rebuild left behind).
    D9CBuffer freshAfterCompaction{};
    idx.retainBuffer(&freshAfterCompaction, d);
    if (!check(freshAfterCompaction.refs == 2u,
               "endEpoch compaction: a fresh retain after rebuild addrefs "
               "once") ||
        !check(idx.size() == 3u,
               "endEpoch compaction: a fresh retain after rebuild appends "
               "one entry")) {
      return 1;
    }

    idx.clear();
    if (!check(keepFirst.refs == 1u && keepLast.refs == 1u &&
                  freshAfterCompaction.refs == 1u,
               "endEpoch compaction: clear releases every surviving pin")) {
      return 1;
    }
  }

  // Overflow/growth: the accelerator grows (never falls back to a scan) once
  // the working set exceeds its initial capacity; dedupe must stay correct
  // at scale, both for the initial retain pass and for a second pass that
  // re-retains every object.
  {
    D3D9PePendingCommandRetainer idx;
    constexpr int kCount = 200;
    std::vector<std::unique_ptr<D9CBuffer>> buffers;
    buffers.reserve(kCount);

    auto acquire = idx.beginAcquire();
    for (int i = 0; i < kCount; ++i) {
      buffers.push_back(std::make_unique<D9CBuffer>());
      idx.retainBuffer(buffers.back().get(), acquire);
    }
    bool allAddreffedOnce = true;
    for (auto& b : buffers) {
      allAddreffedOnce = allAddreffedOnce && (b->refs == 2u);
    }
    if (!check(allAddreffedOnce,
               "index growth: every distinct buffer is addref'd exactly "
               "once") ||
        !check(idx.size() == static_cast<std::size_t>(kCount),
               "index growth: the arena holds every distinct entry")) {
      return 1;
    }

    // Re-retaining the whole set must dedupe every one of them through the
    // grown table, not just the entries present before the last grow.
    for (auto& b : buffers) {
      idx.retainBuffer(b.get(), acquire);
    }
    allAddreffedOnce = true;
    for (auto& b : buffers) {
      allAddreffedOnce = allAddreffedOnce && (b->refs == 2u);
    }
    if (!check(allAddreffedOnce,
               "index growth: re-retaining after growth still dedupes "
               "every entry") ||
        !check(idx.size() == static_cast<std::size_t>(kCount),
               "index growth: re-retaining after growth does not grow the "
               "arena")) {
      return 1;
    }

    idx.clear();
  }

  // A builder-admitted 256-handle working set must remain allocation-free on
  // the retry after Reset/discard.  In particular, RetentionIndex::clear()
  // must preserve its preallocated slots rather than rebuilding from zero.
  {
    constexpr std::size_t kCapacity = 256u;
    D3D9PePendingCommandRetainer idx(kCapacity);
    std::vector<std::unique_ptr<D9CBuffer>> buffers;
    buffers.reserve(kCapacity);
    for (std::size_t i = 0; i < kCapacity; ++i) {
      buffers.push_back(std::make_unique<D9CBuffer>());
    }
    auto first = idx.beginAcquire();
    for (auto& buffer : buffers) idx.retainBuffer(buffer.get(), first);
    idx.clear();

    gAllocationCount.store(0u, std::memory_order_relaxed);
    gCountAllocations.store(true, std::memory_order_release);
    auto retry = idx.beginAcquire();
    for (auto& buffer : buffers) idx.retainBuffer(buffer.get(), retry);
    gCountAllocations.store(false, std::memory_order_release);
    if (!check(gAllocationCount.load(std::memory_order_relaxed) == 0u,
               "max-capacity retry after clear performs no allocations") ||
        !check(idx.size() == kCapacity,
               "max-capacity retry retains every admitted handle")) {
      return 1;
    }
    idx.clear();
  }

  // The bounded owner path is allocation-free through its reserved ceiling,
  // including a warm duplicate and a novel denial at that ceiling.
  {
    constexpr std::size_t kCapacity = 3u;
    D3D9PePendingCommandRetainer idx(kCapacity);
    D9CBuffer first{}, second{}, third{}, denied{};
    auto acquire = idx.beginAcquire();
    idx.retainBuffer(&first, acquire, kCapacity);
    idx.retainBuffer(&second, acquire, kCapacity);
    idx.retainBuffer(&third, acquire, kCapacity);
    idx.clear();
    gAllocationCount.store(0u, std::memory_order_relaxed);
    gCountAllocations.store(true, std::memory_order_release);
    auto retry = idx.beginAcquire();
    const bool firstResult = idx.retainBuffer(&first, retry, kCapacity);
    const bool duplicateResult = idx.retainBuffer(&first, retry, kCapacity);
    const bool secondResult = idx.retainBuffer(&second, retry, kCapacity);
    const bool thirdResult = idx.retainBuffer(&third, retry, kCapacity);
    const bool deniedResult = idx.retainBuffer(&denied, retry, kCapacity);
    gCountAllocations.store(false, std::memory_order_release);
    if (!check(firstResult && duplicateResult && secondResult && thirdResult &&
                   !deniedResult,
               "bounded reserve accepts warm/novel entries and denies over-capacity") ||
        !check(gAllocationCount.load(std::memory_order_relaxed) == 0u,
               "bounded warm-set retry and cap denial perform no allocations") ||
        !check(first.refs == 2u && second.refs == 2u && third.refs == 2u &&
                   denied.refs == 1u,
               "bounded cap denial preserves exact AddRef multiplicity")) {
      return 1;
    }
    idx.rollback(retry);
    idx.clear();
  }

  return 0;
}
