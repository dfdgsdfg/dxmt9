#pragma once

#include "dxmt9/device_c.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

// Flat wrapper-retention set. Entries are kept in one capacity-preserving
// arena instead of one unordered_set + vector pair per wrapper kind. An
// acquisition is a checkpoint into that arena, so rollback never allocates and
// releases only entries appended by the failed record.
//
// The pin an entry holds exists for one reason: a recorded-but-not-yet-imported
// chunk names the unix object by raw pointer, so the object must survive the
// app releasing its D3D9 wrapper before the chunk is committed. Retention was
// already deduplicated per entry, so the wire cost was one addref/release pair
// per unique object per chunk — measured on GT2 as 663+663 buffer crossings and
// ~293x2 shader/texture crossings per present (~0.7 ms/present,
// `state-churn-encode-append-decomposition.24`). Those pairs are pure churn
// whenever the same object is named by the next chunk too, which for a scene's
// working set is nearly always.
//
// So entries now survive a chunk boundary. `endEpoch()` closes a chunk: an
// entry named during the epoch that just ended, or during the one before it,
// keeps its pin and costs no crossing when the next chunk names it again; a
// colder entry is released there. `clear()` still drops everything and is what
// Reset / ResetEx / device teardown call, so no pin outlives the device or
// crosses a `dxmt9c_device_reset*` call. The only behavioural difference is
// that a unix object the app has dropped can stay alive up to two chunk
// periods longer than before (GT2: ~5 ms) — a strictly longer real reference,
// never a shorter one, so no pointer can dangle that did not dangle before.
//
// R-BACK-43.4 `producer-owned` (PE game thread). `entries_` and `epoch_` are
// written and read only from the thread that drives the D3D9 recorder. This
// class holds NO token of its own: it is a private member of
// `CommandChunkBuilder`, itself private to `D3D9DeviceImpl`, and every path
// that reaches it passes `D3D9DeviceImpl::assertRecorderThreadConfined()`
// first (18 recorder-guarded entry points, one per `PeRecorderGuard` site).
// A construction-bound token here would be WRONG, not merely redundant: under
// `D3DCREATE_MULTITHREADED` the device legitimately serves other threads with
// `recorderMutex_` held, and only the device knows that — its assert is the
// R-BACK-43.5 shape-(c) form that admits it, with `recorderLockRequired_` as
// the lock witness.
//
// The pins this class holds are also what discharges the PRODUCER SIDE of the
// pool's arena-stamp pin-ordering obligation (`dxmt9_resource_pool.hpp`): a
// retained wrapper strictly contains the same chunk's marking window, so a
// record being stamped cannot become `destroyPending`. `endEpoch()` — the
// release point — must therefore stay after a successful commit_chunk return.
class D3D9PePendingCommandRetainer {
public:
    // An entry survives this many fully idle epochs before its pin is dropped.
    // 1 means "named in the epoch that just closed, or the one before it".
    static constexpr std::uint64_t kWarmEpochs = 1u;

    static constexpr std::size_t kDefaultCapacity = 256u;

    explicit D3D9PePendingCommandRetainer(
        std::size_t capacity = kDefaultCapacity) {
        entries_.reserve(capacity);
        index_.reserveForEntries(capacity);
    }

    ~D3D9PePendingCommandRetainer() {
        clear();
    }

    D3D9PePendingCommandRetainer(
        const D3D9PePendingCommandRetainer&) = delete;
    D3D9PePendingCommandRetainer& operator=(
        const D3D9PePendingCommandRetainer&) = delete;

    struct Acquired {
        std::size_t checkpoint = 0;
    };

    Acquired beginAcquire() const noexcept {
        return Acquired{entries_.size()};
    }

    std::size_t size() const noexcept {
        return entries_.size();
    }

    void retainSurface(D9CSurface* surface, Acquired&) {
        retain(D9C_CHUNK_HANDLE_KIND_SURFACE, surface,
               [](D9CSurface* value) { dxmt9c_surface_addref(value); });
    }

    void retainTexture(D9CTexture* texture, Acquired&) {
        retain(D9C_CHUNK_HANDLE_KIND_TEXTURE, texture,
               [](D9CTexture* value) { dxmt9c_texture_addref(value); });
    }

    void retainBuffer(D9CBuffer* buffer, Acquired&) {
        retain(D9C_CHUNK_HANDLE_KIND_BUFFER, buffer,
               [](D9CBuffer* value) { dxmt9c_buffer_addref(value); });
    }

    void retainShader(D9CShader* shader, Acquired&) {
        retain(D9C_CHUNK_HANDLE_KIND_SHADER, shader,
               [](D9CShader* value) { dxmt9c_shader_addref(value); });
    }

    void retainVdecl(D9CVertexDecl* vdecl, Acquired&) {
        retain(D9C_CHUNK_HANDLE_KIND_VERTEX_DECL, vdecl,
               [](D9CVertexDecl* value) { dxmt9c_vdecl_addref(value); });
    }

    void retainQuery(D9CQuery* query, Acquired&) {
        retain(D9C_CHUNK_HANDLE_KIND_QUERY, query,
               [](D9CQuery* value) { dxmt9c_query_addref(value); });
    }

    void retainWireObject(std::uint32_t kind, void* object,
                          Acquired& acquired) {
        switch (kind) {
        case D9C_CHUNK_HANDLE_KIND_TEXTURE:
            retainTexture(static_cast<D9CTexture*>(object), acquired);
            break;
        case D9C_CHUNK_HANDLE_KIND_SURFACE:
            retainSurface(static_cast<D9CSurface*>(object), acquired);
            break;
        case D9C_CHUNK_HANDLE_KIND_BUFFER:
            retainBuffer(static_cast<D9CBuffer*>(object), acquired);
            break;
        case D9C_CHUNK_HANDLE_KIND_SHADER:
            retainShader(static_cast<D9CShader*>(object), acquired);
            break;
        case D9C_CHUNK_HANDLE_KIND_VERTEX_DECL:
            retainVdecl(static_cast<D9CVertexDecl*>(object), acquired);
            break;
        case D9C_CHUNK_HANDLE_KIND_QUERY:
            retainQuery(static_cast<D9CQuery*>(object), acquired);
            break;
        default:
            break;
        }
    }

    void rollback(const Acquired& acquired) {
        while (entries_.size() > acquired.checkpoint) {
            const Entry& entry = entries_.back();
            // Only the tail is ever popped here, so surviving entries below
            // the checkpoint keep their indices — erase just the popped key
            // instead of rebuilding the whole table.
            index_.erase(entry.kind, entry.ptr);
            release(entry);
            entries_.pop_back();
        }
    }

    // Closes the current chunk epoch: entries not named within the last
    // kWarmEpochs + 1 epochs release their pin and leave the arena; everything
    // else stays pinned for the next chunk at zero wire cost. Must not be
    // called with a record in flight — the caller rolls back first, so no
    // outstanding Acquired checkpoint can survive the compaction below.
    void endEpoch() noexcept {
        std::size_t write = 0;
        for (std::size_t read = 0; read < entries_.size(); ++read) {
            const Entry& entry = entries_[read];
            if (epoch_ - entry.lastTouchedEpoch <= kWarmEpochs) {
                if (write != read) {
                    entries_[write] = entry;
                }
                ++write;
                continue;
            }
            release(entry);
        }
        entries_.resize(write);
        ++epoch_;
        // The compaction above can move any surviving entry to a new index,
        // so the accelerator must be rebuilt in full. `write <= entries_`'s
        // pre-compaction size (no entries were added), so this never needs a
        // slot array larger than what already exists — see RetentionIndex's
        // comment for why that keeps this call allocation-free and endEpoch()
        // genuinely noexcept.
        index_.rebuildFrom(entries_);
    }

    void clear() {
        for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
            release(*it);
        }
        entries_.clear();
        index_.clear();
    }

private:
    struct Entry {
        std::uint32_t kind = 0;
        void* ptr = nullptr;
        std::uint64_t lastTouchedEpoch = 0;
    };

    // R-BACK-43.7: `retain()`'s duplicate check used to be a
    // `std::find_if` scan over `entries_` (O(n) per call, O(n^2) per chunk
    // once the warm set fills up — the exact shape this spec's process rule
    // was written to catch). This table maps (kind, ptr) -> index into
    // `entries_` so the check is O(1) amortized. `entries_` stays the sole
    // storage of truth for retention/rollback/release; the index is a pure
    // accelerator that is rebuilt (never left dangling) at every point that
    // can move or drop entries:
    //   - `rollback()` only ever pops from the tail, so it erases exactly
    //     the popped keys (tombstones them) without touching the rest.
    //   - `endEpoch()` compacts `entries_` in place, which can change every
    //     surviving entry's index, so it does one full `rebuildFrom()` —
    //     itself O(n), no worse than the compaction loop it follows, and by
    //     construction never grows past the pre-compaction capacity because
    //     `write <= entries_.size()` (no entries are added), so the rebuild
    //     never reallocates and `endEpoch()` stays genuinely `noexcept`.
    //   - `clear()` drops everything, so the index is cleared too.
    // Deletion uses tombstones rather than backward-shift because the only
    // deletion path (`rollback()`) is a record-build-failure path, not the
    // steady-state hot path (every `rollbackRecord()` caller in
    // `d3d9_pe_chunk_draw.cpp` / `d3d9_pe_chunk_nondraw.cpp` is a validation
    // failure branch) — tombstone accumulation between rollbacks is bounded
    // by the (rare) failure rate, and `endEpoch()`'s full rebuild reclaims
    // them on every chunk boundary regardless.
    struct RetentionIndex {
        enum class SlotState : std::uint8_t { Empty, Occupied, Tombstone };
        struct Slot {
            SlotState state = SlotState::Empty;
            std::uint32_t kind = 0;
            void* ptr = nullptr;
            std::size_t index = 0;
        };

        std::vector<Slot> slots;
        std::size_t occupied = 0;  // Occupied only.
        std::size_t used = 0;      // Occupied + Tombstone (load-factor input).

        static std::size_t hashOf(std::uint32_t kind, void* ptr) noexcept {
            std::uint64_t h =
                static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(ptr)) *
                0x9E3779B97F4A7C15ull;
            h ^= static_cast<std::uint64_t>(kind) + 0x517CC1B727220A95ull;
            h ^= h >> 33;
            return static_cast<std::size_t>(h);
        }

        void reserveForEntries(std::size_t entryCapacity) {
            const std::size_t target =
                std::max<std::size_t>(entryCapacity * 2u, 64u);
            std::size_t capacity = 64u;
            while (capacity < target) {
                capacity <<= 1u;
            }
            slots.assign(capacity, Slot{});
            occupied = 0u;
            used = 0u;
        }

        void insertRaw(std::uint32_t kind, void* ptr, std::size_t index) noexcept {
            const auto mask = slots.size() - 1u;
            auto idx = hashOf(kind, ptr) & mask;
            for (;;) {
                Slot& s = slots[idx];
                if (s.state != SlotState::Occupied) {
                    s = Slot{SlotState::Occupied, kind, ptr, index};
                    ++occupied;
                    ++used;
                    return;
                }
                idx = (idx + 1u) & mask;
            }
        }

        const std::size_t* find(std::uint32_t kind, void* ptr) const noexcept {
            if (slots.empty()) {
                return nullptr;
            }
            const auto mask = slots.size() - 1u;
            auto idx = hashOf(kind, ptr) & mask;
            for (std::size_t probes = 0; probes < slots.size(); ++probes) {
                const Slot& s = slots[idx];
                if (s.state == SlotState::Empty) {
                    return nullptr;
                }
                if (s.state == SlotState::Occupied && s.kind == kind &&
                    s.ptr == ptr) {
                    return &s.index;
                }
                idx = (idx + 1u) & mask;
            }
            return nullptr;
        }

        void erase(std::uint32_t kind, void* ptr) noexcept {
            if (slots.empty()) {
                return;
            }
            const auto mask = slots.size() - 1u;
            auto idx = hashOf(kind, ptr) & mask;
            for (std::size_t probes = 0; probes < slots.size(); ++probes) {
                Slot& s = slots[idx];
                if (s.state == SlotState::Empty) {
                    return;
                }
                if (s.state == SlotState::Occupied && s.kind == kind &&
                    s.ptr == ptr) {
                    s.state = SlotState::Tombstone;
                    --occupied;
                    return;
                }
                idx = (idx + 1u) & mask;
            }
        }

        // Rebuilds the table from scratch against the authoritative
        // `entries` array, indexed by position. Grows (never shrinks) the
        // slot array only when `entries.size()` needs more room than the
        // current capacity provides; a same-size rebuild reuses the
        // existing allocation (`std::fill`, not `assign`), so callers whose
        // `entries` never grows (endEpoch's post-compaction call) never
        // reallocate.
        void rebuildFrom(const std::vector<Entry>& entries) {
            std::size_t capacity = slots.empty() ? 64u : slots.size();
            const std::size_t target =
                std::max<std::size_t>(entries.size() * 2u, 64u);
            while (capacity < target) {
                capacity <<= 1u;
            }
            if (capacity != slots.size()) {
                slots.assign(capacity, Slot{});
            } else {
                std::fill(slots.begin(), slots.end(), Slot{});
            }
            occupied = 0u;
            used = 0u;
            for (std::size_t i = 0; i < entries.size(); ++i) {
                insertRaw(entries[i].kind, entries[i].ptr, i);
            }
        }

        void clear() noexcept {
            // Preserve the builder-admitted capacity across Reset/discard so
            // the next chunk can retry at the same handle ceiling without a
            // fresh hash-table allocation.
            std::fill(slots.begin(), slots.end(), Slot{});
            occupied = 0u;
            used = 0u;
        }
    };

    template<typename T, typename AddRefFn>
    void retain(std::uint32_t kind, T* ptr, AddRefFn&& addRef) {
        if (!ptr) {
            return;
        }
        if (const std::size_t* found = index_.find(kind, ptr)) {
            // Already pinned — by this chunk or by a recent one. Refresh the
            // warmth stamp and cross nothing.
            entries_[*found].lastTouchedEpoch = epoch_;
            return;
        }
        entries_.push_back(Entry{kind, ptr, epoch_});
        try {
            if (index_.slots.empty() ||
                index_.used * 4u >= index_.slots.size() * 3u) {
                index_.rebuildFrom(entries_);
            } else {
                index_.insertRaw(kind, ptr, entries_.size() - 1u);
            }
        } catch (...) {
            // Keep entries_/index_ mutually consistent: an entry that is not
            // indexed must not exist, the same invariant the original
            // publish-before-addRef ordering protected.
            entries_.pop_back();
            throw;
        }
        // Publish the entry (and its index) before taking ownership so a
        // failure above cannot leak an AddRef that rollback/clear cannot see.
        addRef(ptr);
    }

    static void release(const Entry& entry) {
        switch (entry.kind) {
        case D9C_CHUNK_HANDLE_KIND_TEXTURE:
            dxmt9c_texture_release(static_cast<D9CTexture*>(entry.ptr));
            break;
        case D9C_CHUNK_HANDLE_KIND_SURFACE:
            dxmt9c_surface_release(static_cast<D9CSurface*>(entry.ptr));
            break;
        case D9C_CHUNK_HANDLE_KIND_BUFFER:
            dxmt9c_buffer_release(static_cast<D9CBuffer*>(entry.ptr));
            break;
        case D9C_CHUNK_HANDLE_KIND_SHADER:
            dxmt9c_shader_release(static_cast<D9CShader*>(entry.ptr));
            break;
        case D9C_CHUNK_HANDLE_KIND_VERTEX_DECL:
            dxmt9c_vdecl_release(static_cast<D9CVertexDecl*>(entry.ptr));
            break;
        case D9C_CHUNK_HANDLE_KIND_QUERY:
            dxmt9c_query_release(static_cast<D9CQuery*>(entry.ptr));
            break;
        default:
            break;
        }
    }

    std::vector<Entry> entries_{};
    RetentionIndex index_{};
    std::uint64_t epoch_ = 0;
};
