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

    D3D9PePendingCommandRetainer() {
        entries_.reserve(64);
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
            release(entries_.back());
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
    }

    void clear() {
        for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
            release(*it);
        }
        entries_.clear();
    }

private:
    struct Entry {
        std::uint32_t kind = 0;
        void* ptr = nullptr;
        std::uint64_t lastTouchedEpoch = 0;
    };

    template<typename T, typename AddRefFn>
    void retain(std::uint32_t kind, T* ptr, AddRefFn&& addRef) {
        if (!ptr) {
            return;
        }
        const auto duplicate = std::find_if(
            entries_.begin(), entries_.end(),
            [kind, ptr](const Entry& entry) {
                return entry.kind == kind && entry.ptr == ptr;
            });
        if (duplicate != entries_.end()) {
            // Already pinned — by this chunk or by a recent one. Refresh the
            // warmth stamp and cross nothing.
            duplicate->lastTouchedEpoch = epoch_;
            return;
        }
        entries_.push_back(Entry{kind, ptr, epoch_});
        // Publish the entry before taking ownership so a vector-growth
        // failure cannot leak an AddRef that rollback/clear cannot see.
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
    std::uint64_t epoch_ = 0;
};
