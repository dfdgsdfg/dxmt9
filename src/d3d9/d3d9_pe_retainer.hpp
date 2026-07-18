#pragma once

#include "dxmt9/device_c.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

// Flat, chunk-lifetime wrapper-retention set. Entries are kept in one
// capacity-preserving arena instead of one unordered_set + vector pair per
// wrapper kind. An acquisition is a checkpoint into that arena, so rollback
// never allocates and releases only entries appended by the failed record.
class D3D9PePendingCommandRetainer {
public:
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

    void retainWireHandle(const D9CCommandChunkWireHandleEntry& handle,
                          Acquired& acquired) {
        const auto ptr = static_cast<std::uintptr_t>(handle.opaqueHandle);
        switch (handle.kind) {
        case D9C_CHUNK_HANDLE_KIND_TEXTURE:
            retainTexture(reinterpret_cast<D9CTexture*>(ptr), acquired);
            break;
        case D9C_CHUNK_HANDLE_KIND_SURFACE:
            retainSurface(reinterpret_cast<D9CSurface*>(ptr), acquired);
            break;
        case D9C_CHUNK_HANDLE_KIND_BUFFER:
            retainBuffer(reinterpret_cast<D9CBuffer*>(ptr), acquired);
            break;
        case D9C_CHUNK_HANDLE_KIND_SHADER:
            retainShader(reinterpret_cast<D9CShader*>(ptr), acquired);
            break;
        case D9C_CHUNK_HANDLE_KIND_VERTEX_DECL:
            retainVdecl(reinterpret_cast<D9CVertexDecl*>(ptr), acquired);
            break;
        case D9C_CHUNK_HANDLE_KIND_QUERY:
            retainQuery(reinterpret_cast<D9CQuery*>(ptr), acquired);
            break;
        default:
            break;
        }
    }

    void retainWireHandles(
        const D9CCommandChunkWireHandleEntry* handles,
        std::size_t handleCount,
        Acquired& acquired) {
        for (std::size_t i = 0; i < handleCount; ++i) {
            retainWireHandle(handles[i], acquired);
        }
    }

    void rollback(const Acquired& acquired) {
        while (entries_.size() > acquired.checkpoint) {
            release(entries_.back());
            entries_.pop_back();
        }
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
            return;
        }
        entries_.push_back(Entry{kind, ptr});
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
};
