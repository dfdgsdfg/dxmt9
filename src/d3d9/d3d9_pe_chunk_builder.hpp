#pragma once

#include "d3d9_pe_retainer.hpp"
#include "device_c_chunk_schema.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

namespace dxmt9::d3d9::pe {

struct PeWireObjectRef {
  D9CWireObjectIdentity identity{};
  void* object = nullptr;

  bool valid(std::uint32_t expectedKind) const noexcept {
    return object && identity.kind == expectedKind &&
           identity.kind <= D9C_CHUNK_HANDLE_KIND_QUERY &&
           identity.generation != 0u && identity.objectId != 0u;
  }
};

void publishCachedWireObjectRef(const PeWireObjectRef& object) noexcept;
void unpublishCachedWireObjectRef(const PeWireObjectRef& object) noexcept;
void noteWireIdentityGetterCall() noexcept;
std::uint64_t wireIdentityGetterCallCount() noexcept;

template <typename Object, typename Getter>
bool cacheWireObjectRef(Object* object, std::uint32_t expectedKind,
                        Getter&& getter, PeWireObjectRef& out) {
  out = {};
  if (!object) {
    return false;
  }
  noteWireIdentityGetterCall();
  D9CWireObjectIdentity identity{};
  if (getter(object, &identity) < 0 || identity.kind != expectedKind ||
      identity.generation == 0u || identity.objectId == 0u) {
    return false;
  }
  out = PeWireObjectRef{
      .identity = identity,
      .object = object,
  };
  publishCachedWireObjectRef(out);
  return true;
}

struct CommandChunkBuilderCapacities {
  std::size_t records = 64u;
  std::size_t handles = 256u;
  std::size_t payloadBytes = 256u * 1024u;
  std::size_t sealedBytes = 272u * 1024u;
};

struct SealedCommandChunk {
  std::span<const std::byte> blob{};
  std::uint32_t recordCount = 0u;
  std::uint32_t handleCount = 0u;

  bool valid() const noexcept { return !blob.empty(); }
};

// R-BACK-43.4 `producer-owned` (PE game thread). Every member below —
// `records_`, `handles_`, `handleObjects_`, `payload_`, `sealedBlob_`,
// `retainer_`, `active_`, `sealed_` — is written and read only on the thread
// driving the D3D9 recorder, and none of it is reachable from the replay
// worker, encode thread, or completion path: the builder's output crosses to
// unix as the sealed POD blob, never as live state.
//
// Enforcement is at the `D3D9DeviceImpl` call boundary
// (`assertRecorderThreadConfined()`, R-BACK-43.5 shape (c)), not with a token
// here — see the same note on `D3D9PePendingCommandRetainer` for why a
// builder-local construction-bound token would be incorrect under
// `D3DCREATE_MULTITHREADED` rather than merely duplicated.
class CommandChunkBuilder {
 public:
  explicit CommandChunkBuilder(
      const CommandChunkBuilderCapacities& capacities = {});
  ~CommandChunkBuilder() = default;

  CommandChunkBuilder(const CommandChunkBuilder&) = delete;
  CommandChunkBuilder& operator=(const CommandChunkBuilder&) = delete;

  bool beginRecord(std::uint32_t type) noexcept;
  bool appendPayload(std::span<const std::byte> bytes,
                     std::uint32_t alignment = 1u,
                     std::uint32_t* recordRelativeOffset = nullptr) noexcept;

  template <typename T>
  bool appendPayloadValue(const T& value,
                          std::uint32_t* recordRelativeOffset = nullptr) {
    return appendPayload(
        std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(&value), sizeof(value)),
        alignof(T), recordRelativeOffset);
  }

  bool overwritePayload(std::uint32_t recordRelativeOffset,
                        std::span<const std::byte> bytes) noexcept;
  bool appendHandle(const PeWireObjectRef& object,
                    std::uint32_t expectedKind,
                    std::uint32_t& absoluteIndex) noexcept;
  bool commitRecord() noexcept;
  void rollbackRecord() noexcept;

  SealedCommandChunk seal() noexcept;
  // Chunk boundary after a successful commit: keeps recently-named wrapper
  // pins warm across the boundary (see D3D9PePendingCommandRetainer).
  void reset() noexcept;
  // Discard: same as reset() but also releases every warm pin. Use at device
  // teardown / Reset / ResetEx.
  void resetAndReleaseRetained() noexcept;

  bool recordActive() const noexcept { return active_.active; }
  bool sealed() const noexcept { return sealed_; }
  std::size_t recordCount() const noexcept { return records_.size(); }
  std::size_t handleCount() const noexcept { return handles_.size(); }
  std::size_t payloadBytes() const noexcept { return payload_.size(); }
  std::size_t retainedObjectCount() const noexcept { return retainer_.size(); }
  bool referencesObject(void* object) const noexcept;

  const std::vector<D9CCommandChunkWireRecordHeader>& recordsForTest()
      const noexcept {
    return records_;
  }
  const std::vector<D9CCommandChunkWireHandleEntry>& handlesForTest()
      const noexcept {
    return handles_;
  }
  const std::vector<D9CCommandChunkWireHandleEntry>& handles() const noexcept {
    return handles_;
  }
  const std::vector<std::byte>& payloadForTest() const noexcept {
    return payload_;
  }

 private:
  struct ActiveRecord {
    bool active = false;
    std::uint32_t type = 0u;
    std::size_t recordCheckpoint = 0u;
    std::size_t handleCheckpoint = 0u;
    std::size_t payloadCheckpoint = 0u;
    std::size_t payloadStart = 0u;
    // Monotonically increasing, assigned once per beginRecord() regardless
    // of whether the record eventually commits or rolls back, and never
    // reused (see RecordLocalDedupTable below). 0 is reserved as "no active
    // record" and is never handed out by beginRecord().
    std::uint64_t recordOrdinal = 0u;
    D3D9PePendingCommandRetainer::Acquired retainedCheckpoint{};
  };

  // R-BACK-43.7: `referencesObject()` used to be a `std::find` over the
  // whole builder-lifetime `handleObjects_` array, called per qualifying
  // buffer Lock — the full-arena O(n) shape this spec's process rule was
  // written to catch. This is a chunk-lifetime pointer -> multiplicity
  // accelerator: `handleObjects_` stays the source of truth (an object can
  // be named by more than one handle across different records in the same
  // chunk, so the table stores a count, not a presence bit). On overflow it
  // stops answering and the caller falls back to the original linear scan,
  // which stays correct because every pushed handle object is still
  // appended to `handleObjects_` regardless of table state. `reset()` /
  // `resetAndReleaseRetained()` clear it in full (handleObjects_ is
  // cleared too); `rollbackRecord()` decrements counts for exactly the
  // range of handles a failed record added, using the same handleCheckpoint
  // bound the surrounding rollback already computes.
  //
  // This does NOT replace `appendHandle`'s own record-local dedup scan
  // (finding a handle already appended by the *current* record): that scan
  // is bounded by one record's handle count (tens, per R-BACK-43.7's own
  // review), and this table answers a different question — "does this
  // pointer appear anywhere in the chunk" — that would need to carry
  // per-record membership to serve the narrower query, which the dedup
  // scan does not need.
  struct HandlePresenceTable {
    struct Slot {
      void* key = nullptr;
      std::uint32_t count = 0u;
    };

    std::vector<Slot> slots;
    std::size_t occupied = 0u;
    bool overflowed = false;

    void init(std::size_t handleCapacityHint) noexcept {
      std::size_t capacity = 64u;
      const std::size_t target =
          std::max<std::size_t>(handleCapacityHint * 2u, 64u);
      while (capacity < target) {
        capacity <<= 1u;
      }
      slots.assign(capacity, Slot{});
      occupied = 0u;
      overflowed = false;
    }

    // Finds `key`'s slot, inserting a fresh zero-count slot if absent.
    // Returns nullptr (and sets `overflowed`) once the table has no more
    // room; callers must fall back to a linear scan for the rest of the
    // chunk's lifetime once that happens.
    Slot* findOrInsert(void* key) noexcept {
      if (overflowed || slots.empty()) {
        return nullptr;
      }
      const auto mask = slots.size() - 1u;
      auto idx = (reinterpret_cast<std::uintptr_t>(key) >> 4u) & mask;
      for (std::size_t probes = 0; probes < slots.size(); ++probes) {
        Slot& s = slots[idx];
        if (s.key == key) {
          return &s;
        }
        if (s.key == nullptr) {
          if (occupied * 4u >= slots.size() * 3u) {
            overflowed = true;
            return nullptr;
          }
          s.key = key;
          s.count = 0u;
          ++occupied;
          return &s;
        }
        idx = (idx + 1u) & mask;
      }
      overflowed = true;
      return nullptr;
    }

    // Mutable lookup: never inserts, never sets `overflowed`, but returns a
    // writable slot so a caller that already knows `key` is present (such as
    // rollbackRecord() undoing its own earlier increment) can adjust its
    // count without a second, insert-capable probe.
    Slot* find(void* key) noexcept {
      if (overflowed || slots.empty()) {
        return nullptr;
      }
      const auto mask = slots.size() - 1u;
      auto idx = (reinterpret_cast<std::uintptr_t>(key) >> 4u) & mask;
      for (std::size_t probes = 0; probes < slots.size(); ++probes) {
        Slot& s = slots[idx];
        if (s.key == key) {
          return &s;
        }
        if (s.key == nullptr) {
          return nullptr;
        }
        idx = (idx + 1u) & mask;
      }
      return nullptr;
    }

    // Non-mutating lookup for const query contexts (referencesObject()).
    const Slot* find(void* key) const noexcept {
      if (overflowed || slots.empty()) {
        return nullptr;
      }
      const auto mask = slots.size() - 1u;
      auto idx = (reinterpret_cast<std::uintptr_t>(key) >> 4u) & mask;
      for (std::size_t probes = 0; probes < slots.size(); ++probes) {
        const Slot& s = slots[idx];
        if (s.key == key) {
          return &s;
        }
        if (s.key == nullptr) {
          return nullptr;
        }
        idx = (idx + 1u) & mask;
      }
      return nullptr;
    }

    void clear() noexcept {
      std::fill(slots.begin(), slots.end(), Slot{});
      occupied = 0u;
      overflowed = false;
    }
  };

  // R-BACK-43.7 follow-up (fresh binary-matched sampler evidence,
  // docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.32.md):
  // appendHandle()'s *record-local* dedup scan — "did the record currently
  // being built already append this exact wire identity, and if so at which
  // absolute handle index" — was left as an O(record window) linear scan by
  // R-BACK-43.7's own review, on the argument that a record's handle window
  // is "tens" of entries. Sampler evidence refutes that: 9.5% of d3d9.dll
  // self-PC time (~0.35ms/present on GT2) lands in this loop, so call volume
  // (many appendHandle calls across many records, not any single record's
  // width) makes the O(n) shape hot.
  //
  // This is deliberately a SEPARATE structure from HandlePresenceTable
  // above, not an extension of it, because the two answer different
  // questions with different correctness requirements:
  //   - HandlePresenceTable (referencesObject()) asks "does this *pointer*
  //     occur anywhere in the chunk" and is correctly keyed by pointer alone
  //     — it does not need to know whether the identity recorded against
  //     that pointer is internally consistent.
  //   - This table must reproduce the original scan's *identity*-keyed
  //     semantics exactly: two different pointers that present the same
  //     generation-qualified identity within one record is a genuine
  //     integrity fault (a stale/duplicate wire-cache entry) that the
  //     original scan detects and fails the record for
  //     (`handleObjects_[i] != object.object` -> failActiveRecord()). A
  //     pointer-keyed lookup cannot see that fault at all: the second,
  //     differently-pointered append would simply miss on its own pointer
  //     and be treated as a brand-new handle, silently losing the check.
  //     So this table is keyed by the (kind, generation, objectId) identity
  //     tuple, matching what the original loop actually compared first.
  //
  // Record-locality is achieved without a per-record clear or per-record
  // capacity: every slot is stamped with the record ordinal that last wrote
  // it. `ActiveRecord::recordOrdinal` is assigned once per beginRecord(),
  // monotonically increasing and never reused — including for a record that
  // is later rolled back, since it is handed out before the record's outcome
  // is known — so a slot stamped by a rolled-back or already-committed
  // record can never alias a later record's ordinal. A slot whose stamp does
  // not match the *current* record's ordinal is treated as absent for this
  // record's dedup query (a "miss") and is unconditionally overwritten by
  // the new entry in place, exactly like inserting into an empty slot, so
  // `occupied` — and therefore overflow risk — is bounded by the number of
  // *chunk-lifetime distinct identities*, not by records seen. Overflow
  // (fixed capacity, same 3/4 load-factor policy as HandlePresenceTable)
  // permanently falls callers back to the original per-record linear scan
  // for the remainder of the chunk's lifetime — identical fallback policy to
  // HandlePresenceTable, and correct for the same reason: every appended
  // handle is still recorded in `handles_`/`handleObjects_` regardless of
  // this table's state.
  struct RecordLocalDedupTable {
    struct Slot {
      bool used = false;
      std::uint32_t kind = 0u;
      std::uint32_t generation = 0u;
      std::uint64_t objectId = 0u;
      std::uint64_t recordOrdinal = 0u;
      std::uint32_t handleIndex = 0u;
      void* object = nullptr;
    };

    enum class Lookup { kMiss, kHit, kOverflowed };

    std::vector<Slot> slots;
    std::size_t occupied = 0u;
    bool overflowed = false;

    static std::size_t hashIdentity(
        const D9CWireObjectIdentity& identity) noexcept {
      // Plain multiplicative mix over the three identity fields; this table
      // is chunk-local and never persisted, so no cross-process stability
      // requirement applies, only in-memory distribution.
      std::uint64_t h = identity.objectId;
      h ^= static_cast<std::uint64_t>(identity.kind) * 0x9e3779b97f4a7c15ull;
      h ^= static_cast<std::uint64_t>(identity.generation) *
           0xc2b2ae3d27d4eb4full;
      h ^= h >> 33u;
      h *= 0xff51afd7ed558ccdull;
      h ^= h >> 33u;
      return static_cast<std::size_t>(h);
    }

    void init(std::size_t handleCapacityHint) noexcept {
      std::size_t capacity = 64u;
      const std::size_t target =
          std::max<std::size_t>(handleCapacityHint * 2u, 64u);
      while (capacity < target) {
        capacity <<= 1u;
      }
      slots.assign(capacity, Slot{});
      occupied = 0u;
      overflowed = false;
    }

    void clear() noexcept {
      std::fill(slots.begin(), slots.end(), Slot{});
      occupied = 0u;
      overflowed = false;
    }

    // Looks up `identity` scoped to `recordOrdinal`.
    //  - kOverflowed: table has no room to answer; caller must fall back to
    //    the original linear scan (and keeps doing so for the rest of the
    //    chunk's lifetime, mirroring HandlePresenceTable's policy).
    //  - kHit: this exact identity was already appended by the record with
    //    ordinal `recordOrdinal`; `*outIndex`/`*outObject` are the stored
    //    handle index and object pointer from that append.
    //  - kMiss: no live entry for this record. `*insertAt` names the slot a
    //    fresh insert belongs in (either genuinely empty, or holding a
    //    stale-ordinal entry for the same identity that is safe to
    //    overwrite in place).
    Lookup findForRecord(const D9CWireObjectIdentity& identity,
                         std::uint64_t recordOrdinal, Slot** insertAt,
                         std::uint32_t* outIndex, void** outObject) noexcept {
      if (insertAt) {
        *insertAt = nullptr;
      }
      if (overflowed || slots.empty()) {
        return Lookup::kOverflowed;
      }
      const auto mask = slots.size() - 1u;
      auto idx = hashIdentity(identity) & mask;
      for (std::size_t probes = 0; probes < slots.size(); ++probes) {
        Slot& s = slots[idx];
        if (!s.used) {
          if (occupied * 4u >= slots.size() * 3u) {
            overflowed = true;
            return Lookup::kOverflowed;
          }
          if (insertAt) {
            *insertAt = &s;
          }
          return Lookup::kMiss;
        }
        if (s.kind == identity.kind && s.generation == identity.generation &&
            s.objectId == identity.objectId) {
          if (s.recordOrdinal == recordOrdinal) {
            if (outIndex) {
              *outIndex = s.handleIndex;
            }
            if (outObject) {
              *outObject = s.object;
            }
            return Lookup::kHit;
          }
          // Same identity, stamped by an earlier (committed or
          // rolled-back) record: stale for this record's query. Reuse the
          // slot in place rather than probing further, so `occupied` does
          // not grow on repeat identities across records.
          if (insertAt) {
            *insertAt = &s;
          }
          return Lookup::kMiss;
        }
        idx = (idx + 1u) & mask;
      }
      overflowed = true;
      return Lookup::kOverflowed;
    }

    void insert(Slot& slot, const D9CWireObjectIdentity& identity,
               std::uint64_t recordOrdinal, std::uint32_t handleIndex,
               void* object) noexcept {
      const bool wasUsed = slot.used;
      slot.used = true;
      slot.kind = identity.kind;
      slot.generation = identity.generation;
      slot.objectId = identity.objectId;
      slot.recordOrdinal = recordOrdinal;
      slot.handleIndex = handleIndex;
      slot.object = object;
      if (!wasUsed) {
        ++occupied;
      }
    }
  };

  bool failActiveRecord() noexcept;
  bool appendNewHandleEntry(const PeWireObjectRef& object,
                            std::uint32_t& absoluteIndex) noexcept;

  std::vector<D9CCommandChunkWireRecordHeader> records_;
  std::vector<D9CCommandChunkWireHandleEntry> handles_;
  std::vector<void*> handleObjects_;
  std::vector<std::byte> payload_;
  std::vector<std::byte> sealedBlob_;
  D3D9PePendingCommandRetainer retainer_;
  HandlePresenceTable handlePresence_;
  RecordLocalDedupTable recordLocalDedup_;
  ActiveRecord active_{};
  // Never reused across the builder's whole lifetime (spans many chunks via
  // reset()/resetAndReleaseRetained()), so a RecordLocalDedupTable slot's
  // stamp can never alias a later record. Starts at 1 so 0 stays a safe
  // "no record" sentinel, though nothing currently relies on that.
  std::uint64_t nextRecordOrdinal_ = 1u;
  bool sealed_ = false;
};

template <typename Wire>
struct SparseBindingInput {
  static_assert(std::is_trivially_copyable_v<Wire>);

  Wire wire{};
  PeWireObjectRef object{};
};

struct SparseConstantRangeInput {
  std::uint32_t startRegister = 0u;
  std::uint32_t registerCount = 0u;
  std::span<const std::byte> registerBytes{};

  bool present() const noexcept { return registerCount != 0u; }
};

struct SparseStateInput {
  std::span<const D9CCommandChunkWireRenderState> renderStates{};
  std::span<const SparseBindingInput<
      D9CCommandChunkWireTextureBinding>> textures{};
  std::span<const SparseBindingInput<
      D9CCommandChunkWireStreamBinding>> streams{};
  std::span<const SparseBindingInput<
      D9CCommandChunkWireShaderBinding>> shaders{};
  std::span<const SparseBindingInput<
      D9CCommandChunkWireVertexInput>> vertexInputs{};
  std::span<const SparseBindingInput<
      D9CCommandChunkWireIndexBinding>> indexBuffers{};
  std::span<const SparseBindingInput<
      D9CCommandChunkWireRenderTargetBinding>> renderTargets{};
  std::span<const SparseBindingInput<
      D9CCommandChunkWireDepthStencilBinding>> depthStencils{};
  std::span<const D9CViewport> viewports{};
  std::span<const D9CRect> scissors{};
  std::span<const D9CMaterial> materials{};
  std::span<const D9CCommandChunkWireClipPlane> clipPlanes{};
  std::span<const D9CDrawPacketTextureStageState> textureStageStates{};
  std::span<const D9CDrawPacketSamplerState> samplerStates{};
  std::span<const D9CDrawPacketTransform> transforms{};
  std::span<const D9CCommandChunkWireLight> lights{};
  std::span<const D9CCommandChunkWireLightEnable> lightEnables{};
  SparseConstantRangeInput vsFloatConstants{};
  SparseConstantRangeInput vsIntConstants{};
  SparseConstantRangeInput vsBoolConstants{};
  SparseConstantRangeInput psFloatConstants{};
  SparseConstantRangeInput psIntConstants{};
  SparseConstantRangeInput psBoolConstants{};
  std::span<const std::byte> upIndexData{};
  std::span<const std::byte> upVertexData{};
};

bool appendSparseRecord(CommandChunkBuilder& builder,
                          std::uint32_t type,
                          D9CCommandChunkWireDrawHeader draw,
                          const SparseStateInput& state) noexcept;
bool appendApplyState(CommandChunkBuilder& builder,
                        std::uint32_t flags,
                        const SparseStateInput& state) noexcept;

bool appendSetConstants(
    CommandChunkBuilder& builder, std::uint32_t type,
    std::uint32_t startRegister, std::uint32_t registerCount,
    std::span<const std::byte> registerBytes) noexcept;
bool appendClear(CommandChunkBuilder& builder,
                   D9CCommandChunkWireClear fixed,
                   std::span<const D9CRect> rects) noexcept;
bool appendPresent(CommandChunkBuilder& builder,
                     D9CCommandChunkWirePresent fixed,
                     const PeWireObjectRef& source) noexcept;
bool appendStretchRect(CommandChunkBuilder& builder,
                         D9CCommandChunkWireStretchRect fixed,
                         const PeWireObjectRef& src,
                         const PeWireObjectRef& dst) noexcept;
bool appendColorFill(CommandChunkBuilder& builder,
                       D9CCommandChunkWireColorFill fixed,
                       const PeWireObjectRef& surface) noexcept;
bool appendUpdateTexture(CommandChunkBuilder& builder,
                           const PeWireObjectRef& src,
                           const PeWireObjectRef& dst) noexcept;
bool appendUpdateSurface(CommandChunkBuilder& builder,
                           D9CCommandChunkWireUpdateSurface fixed,
                           const PeWireObjectRef& src,
                           const PeWireObjectRef& dst) noexcept;
bool appendQueryIssue(CommandChunkBuilder& builder,
                        std::uint32_t flags,
                        const PeWireObjectRef& query) noexcept;
bool appendReadback(CommandChunkBuilder& builder,
                      const PeWireObjectRef& src,
                      const PeWireObjectRef& dst) noexcept;
bool appendReszDepthResolve(CommandChunkBuilder& builder,
                              const PeWireObjectRef& msaaDepth,
                              const PeWireObjectRef& intzDest) noexcept;


}  // namespace dxmt9::d3d9::pe
