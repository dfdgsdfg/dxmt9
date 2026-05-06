#pragma once

#include "d3d9_pe.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unordered_set>
#include <utility>
#include <vector>

using D3D9PeWireHandleEntryList =
    std::vector<D9CCommandChunkWireHandleEntry>;

enum class PeRecorderFlushReason : std::uint32_t {
    Explicit = 0,
    CapacityPre,
    CapacityPost,
    Barrier,
    Present,
    Readback,
    Reset,
    StateBlock,
    Child,
    Destructor,
    StateMutation,
    Count,
};

static constexpr std::size_t kPeRecorderFlushReasonCount =
    static_cast<std::size_t>(PeRecorderFlushReason::Count);

struct PeRecorderStats {
    std::uint64_t commitCount = 0;
    std::uint64_t recordCountTotal = 0;
    std::uint64_t recordCountMax = 0;
    std::uint64_t payloadBytesTotal = 0;
    std::uint64_t payloadBytesMax = 0;
    std::uint64_t handleCountTotal = 0;
    std::uint64_t handleCountMax = 0;
    std::array<std::uint64_t, kPeRecorderFlushReasonCount> flushReasons{};
    std::uint64_t drawPrimitiveUPCalls = 0;
    std::uint64_t drawIndexedPrimitiveUPCalls = 0;
    std::uint64_t upVertexBytes = 0;
    std::uint64_t upIndexBytes = 0;
};

inline const char* peRecorderFlushReasonName(PeRecorderFlushReason reason) {
    switch (reason) {
    case PeRecorderFlushReason::Explicit: return "explicit";
    case PeRecorderFlushReason::CapacityPre: return "capacity_pre";
    case PeRecorderFlushReason::CapacityPost: return "capacity_post";
    case PeRecorderFlushReason::Barrier: return "barrier";
    case PeRecorderFlushReason::Present: return "present";
    case PeRecorderFlushReason::Readback: return "readback";
    case PeRecorderFlushReason::Reset: return "reset";
    case PeRecorderFlushReason::StateBlock: return "stateblock";
    case PeRecorderFlushReason::Child: return "child";
    case PeRecorderFlushReason::Destructor: return "destructor";
    case PeRecorderFlushReason::StateMutation: return "state_mutation";
    case PeRecorderFlushReason::Count: break;
    }
    return "unknown";
}

inline D9CWireHandle toWireHandle(const void* handle) {
    const auto value = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(handle));
    return D9CWireHandle{
        static_cast<std::uint32_t>(value & 0xffffffffull),
        static_cast<std::uint32_t>(value >> 32),
    };
}

inline std::uint64_t d9cWireHandleValue(const D9CWireHandle& handle) {
    return static_cast<std::uint64_t>(handle.lo) |
           (static_cast<std::uint64_t>(handle.hi) << 32);
}

struct D3D9PePendingCommandRetainer {
    struct Acquired {
        std::vector<D9CSurface*> surfaces;
        std::vector<D9CTexture*> textures;
        std::vector<D9CBuffer*> buffers;
        std::vector<D9CShader*> shaders;
        std::vector<D9CVertexDecl*> vdecls;
    };

    void retainSurface(D9CSurface* surface, Acquired& acquired) {
        if (!surface) {
            return;
        }
        if (surfaces_.insert(surface).second) {
            dxmt9c_surface_addref(surface);
            surfaceList_.push_back(surface);
            acquired.surfaces.push_back(surface);
        }
    }

    void retainTexture(D9CTexture* texture, Acquired& acquired) {
        if (!texture) {
            return;
        }
        if (textures_.insert(texture).second) {
            dxmt9c_texture_addref(texture);
            textureList_.push_back(texture);
            acquired.textures.push_back(texture);
        }
    }

    void retainBuffer(D9CBuffer* buffer, Acquired& acquired) {
        if (!buffer) {
            return;
        }
        if (buffers_.insert(buffer).second) {
            dxmt9c_buffer_addref(buffer);
            bufferList_.push_back(buffer);
            acquired.buffers.push_back(buffer);
        }
    }

    void retainShader(D9CShader* shader, Acquired& acquired) {
        if (!shader) {
            return;
        }
        if (shaders_.insert(shader).second) {
            dxmt9c_shader_addref(shader);
            shaderList_.push_back(shader);
            acquired.shaders.push_back(shader);
        }
    }

    void retainVdecl(D9CVertexDecl* vdecl, Acquired& acquired) {
        if (!vdecl) {
            return;
        }
        if (vdecls_.insert(vdecl).second) {
            dxmt9c_vdecl_addref(vdecl);
            vdeclList_.push_back(vdecl);
            acquired.vdecls.push_back(vdecl);
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
        default:
            break;
        }
    }

    void retainWireHandles(const D3D9PeWireHandleEntryList& handles,
                           Acquired& acquired) {
        for (const auto& handle : handles) {
            retainWireHandle(handle, acquired);
        }
    }

    void rollback(const Acquired& acquired) {
        for (auto* surface : acquired.surfaces) {
            surfaces_.erase(surface);
            eraseOne(surfaceList_, surface);
            dxmt9c_surface_release(surface);
        }
        for (auto* texture : acquired.textures) {
            textures_.erase(texture);
            eraseOne(textureList_, texture);
            dxmt9c_texture_release(texture);
        }
        for (auto* buffer : acquired.buffers) {
            buffers_.erase(buffer);
            eraseOne(bufferList_, buffer);
            dxmt9c_buffer_release(buffer);
        }
        for (auto* shader : acquired.shaders) {
            shaders_.erase(shader);
            eraseOne(shaderList_, shader);
            dxmt9c_shader_release(shader);
        }
        for (auto* vdecl : acquired.vdecls) {
            vdecls_.erase(vdecl);
            eraseOne(vdeclList_, vdecl);
            dxmt9c_vdecl_release(vdecl);
        }
    }

    void clear() {
        for (auto* surface : surfaceList_) {
            dxmt9c_surface_release(surface);
        }
        surfaceList_.clear();
        surfaces_.clear();
        for (auto* texture : textureList_) {
            dxmt9c_texture_release(texture);
        }
        textureList_.clear();
        textures_.clear();
        for (auto* buffer : bufferList_) {
            dxmt9c_buffer_release(buffer);
        }
        bufferList_.clear();
        buffers_.clear();
        for (auto* shader : shaderList_) {
            dxmt9c_shader_release(shader);
        }
        shaderList_.clear();
        shaders_.clear();
        for (auto* vdecl : vdeclList_) {
            dxmt9c_vdecl_release(vdecl);
        }
        vdeclList_.clear();
        vdecls_.clear();
    }

private:
    template<typename T>
    static void eraseOne(std::vector<T*>& values, T* value) {
        const auto it = std::find(values.begin(), values.end(), value);
        if (it != values.end()) {
            values.erase(it);
        }
    }

    std::unordered_set<D9CSurface*> surfaces_{};
    std::vector<D9CSurface*> surfaceList_{};
    std::unordered_set<D9CTexture*> textures_{};
    std::vector<D9CTexture*> textureList_{};
    std::unordered_set<D9CBuffer*> buffers_{};
    std::vector<D9CBuffer*> bufferList_{};
    std::unordered_set<D9CShader*> shaders_{};
    std::vector<D9CShader*> shaderList_{};
    std::unordered_set<D9CVertexDecl*> vdecls_{};
    std::vector<D9CVertexDecl*> vdeclList_{};
};

struct PeCommandChunkCommitInfo {
    std::uint32_t recordCount = 0;
    std::uint32_t payloadBytes = 0;
    std::uint32_t handleCount = 0;
    std::uint32_t wireBytes = 0;
};

struct PendingCommandChunk {
    std::vector<std::uint8_t> payloadArena{};
    std::vector<D9CCommandChunkWireRecordHeader> wireRecords{};
    std::uint32_t recordCount = 0;

    std::vector<std::uint8_t> wireBlob{};
    D3D9PeWireHandleEntryList chunkHandleScratch{};
    D3D9PePendingCommandRetainer retainer{};

    bool empty() const noexcept {
        return recordCount == 0;
    }

    void clear() {
        payloadArena.clear();
        wireRecords.clear();
        wireBlob.clear();
        recordCount = 0;
        chunkHandleScratch.clear();
        retainer.clear();
    }
};

class PeCommandChunkBuilder {
public:
    using WireHandleEntryList = D3D9PeWireHandleEntryList;

    bool empty() const noexcept {
        return chunk_.empty();
    }

    std::uint32_t recordCount() const noexcept {
        return chunk_.recordCount;
    }

    std::size_t payloadSize() const noexcept {
        return chunk_.payloadArena.size();
    }

    D3D9PePendingCommandRetainer& retainer() noexcept {
        return chunk_.retainer;
    }

    bool shouldFlushBeforeAppend(std::size_t bytes,
                                 std::uint32_t maxRecords,
                                 std::size_t maxBytes) const noexcept {
        return chunk_.recordCount != 0 &&
               (chunk_.recordCount >= maxRecords ||
                chunk_.payloadArena.size() + bytes > maxBytes);
    }

    void clear() {
        chunk_.clear();
    }

    bool payload(const D9CCommandChunkWireRecordHeader& wireRecord,
                 const std::uint8_t*& out) const {
        if (chunk_.payloadArena.size() > 0xffffffffull) {
            return false;
        }
        const auto payloadArenaSize =
            static_cast<std::uint32_t>(chunk_.payloadArena.size());
        if (!d9c_command_chunk_wire_payload_range_valid(
                payloadArenaSize, wireRecord.payloadOffset,
                wireRecord.payloadSize)) {
            return false;
        }
        out = wireRecord.payloadSize == 0
            ? nullptr
            : chunk_.payloadArena.data() + wireRecord.payloadOffset;
        return true;
    }

    static bool appendRecordWireHandle(WireHandleEntryList& handles,
                                       std::uint32_t kind,
                                       std::uint64_t handle) {
        if (handle == 0) {
            return true;
        }
        if (kind > D9C_CHUNK_HANDLE_KIND_VERTEX_DECL) {
            return false;
        }
        for (const auto& existing : handles) {
            if (existing.kind == kind && existing.opaqueHandle == handle) {
                return true;
            }
        }
        handles.push_back(D9CCommandChunkWireHandleEntry{
            .kind = kind,
            .generation = D9C_COMMAND_CHUNK_WIRE_HANDLE_GENERATION_NONE,
            .opaqueHandle = handle,
            .reserved0 = 0,
            .reserved1 = 0,
        });
        return true;
    }

    static bool appendDrawPacketWireHandles(
        const D9CDrawPrimitivePacket& packet,
        WireHandleEntryList& handles) {
        for (std::uint32_t stage = 0; stage < D9C_DRAW_PACKET_MAX_TEXTURES; ++stage) {
            if ((packet.textureMask & (1u << stage)) != 0 &&
                !appendRecordWireHandle(handles, D9C_CHUNK_HANDLE_KIND_TEXTURE,
                                        d9cWireHandleValue(packet.textures[stage]))) {
                return false;
            }
        }
        for (std::uint32_t stream = 0; stream < D9C_DRAW_PACKET_MAX_STREAMS; ++stream) {
            if ((packet.streamSourceMask & (1u << stream)) != 0 &&
                !appendRecordWireHandle(
                    handles, D9C_CHUNK_HANDLE_KIND_BUFFER,
                    d9cWireHandleValue(packet.streamSources[stream].buffer))) {
                return false;
            }
        }
        for (std::uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_RENDER_TARGETS; ++slot) {
            if ((packet.rtMask & (1u << slot)) != 0 &&
                !appendRecordWireHandle(handles, D9C_CHUNK_HANDLE_KIND_SURFACE,
                                        d9cWireHandleValue(packet.rtHandles[slot]))) {
                return false;
            }
        }
        if (packet.dsValid != 0 &&
            !appendRecordWireHandle(handles, D9C_CHUNK_HANDLE_KIND_SURFACE,
                                    d9cWireHandleValue(packet.dsHandle))) {
            return false;
        }
        return true;
    }

    static bool appendIndexedDrawPacketWireHandles(
        const D9CDrawIndexedPrimitivePacket& packet,
        WireHandleEntryList& handles) {
        if (!appendDrawPacketWireHandles(packet.state, handles)) {
            return false;
        }
        if (packet.ibValid != 0 &&
            !appendRecordWireHandle(handles, D9C_CHUNK_HANDLE_KIND_BUFFER,
                                    d9cWireHandleValue(packet.ibHandle))) {
            return false;
        }
        return true;
    }

    template<typename CommitFn>
    HRESULT flush(PeRecorderFlushReason reason,
                  CommitFn&& commit) {
        if (chunk_.recordCount == 0) {
            return S_OK;
        }
        if (chunk_.wireRecords.size() != chunk_.recordCount) {
            return D3DERR_INVALIDCALL;
        }
        const auto wireRecordCount =
            static_cast<std::uint32_t>(chunk_.wireRecords.size());
        const auto payloadArenaSize =
            static_cast<std::uint32_t>(chunk_.payloadArena.size());
        if (chunk_.chunkHandleScratch.size() > 0xffffffffull) {
            return D3DERR_INVALIDCALL;
        }
        const auto wireHandleCount =
            static_cast<std::uint32_t>(chunk_.chunkHandleScratch.size());

        const auto recordTableBytes =
            static_cast<std::uint64_t>(wireRecordCount) *
            D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE;
        const auto handleTableBytes =
            static_cast<std::uint64_t>(wireHandleCount) *
            D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE;
        const auto payloadArenaOffset =
            static_cast<std::uint64_t>(D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE) +
            recordTableBytes + handleTableBytes;
        const auto wireBlobSize =
            payloadArenaOffset + static_cast<std::uint64_t>(payloadArenaSize);
        if (recordTableBytes > 0xffffffffull ||
            handleTableBytes > 0xffffffffull ||
            payloadArenaOffset > 0xffffffffull ||
            wireBlobSize > 0xffffffffull) {
            return D3DERR_INVALIDCALL;
        }

        D9CCommandChunkWireHeader wireHeader{};
        wireHeader.version = D9C_COMMAND_CHUNK_WIRE_VERSION;
        wireHeader.headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE;
        wireHeader.recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE;
        wireHeader.handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE;
        wireHeader.recordTableOffset = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE;
        wireHeader.recordCount = wireRecordCount;
        wireHeader.handleTableOffset =
            wireHeader.recordTableOffset +
            static_cast<std::uint32_t>(recordTableBytes);
        wireHeader.handleCount = wireHandleCount;
        wireHeader.payloadArenaOffset =
            static_cast<std::uint32_t>(payloadArenaOffset);
        wireHeader.payloadArenaSize = payloadArenaSize;

        std::uint32_t nextHandle = 0;
        for (auto& wireRecord : chunk_.wireRecords) {
            if (wireRecord.firstHandle != nextHandle) {
                return D3DERR_INVALIDCALL;
            }
            nextHandle += wireRecord.handleCount;
        }
        if (nextHandle != wireHandleCount ||
            chunk_.chunkHandleScratch.size() != wireHandleCount) {
            return D3DERR_INVALIDCALL;
        }
        if (sizeof(wireHeader) != wireHeader.recordTableOffset ||
            wireHeader.recordTableOffset + recordTableBytes !=
                wireHeader.handleTableOffset ||
            wireHeader.handleTableOffset + handleTableBytes !=
                wireHeader.payloadArenaOffset ||
            wireHeader.payloadArenaOffset + payloadArenaSize != wireBlobSize) {
            return D3DERR_INVALIDCALL;
        }

        chunk_.wireBlob.clear();
        chunk_.wireBlob.resize(static_cast<std::size_t>(wireBlobSize));
        auto* const wireBlob = chunk_.wireBlob.data();
        std::memcpy(wireBlob, &wireHeader, sizeof(wireHeader));
        if (recordTableBytes != 0) {
            std::memcpy(
                wireBlob + wireHeader.recordTableOffset,
                chunk_.wireRecords.data(),
                static_cast<std::size_t>(recordTableBytes));
        }
        if (handleTableBytes != 0) {
            std::memcpy(
                wireBlob + wireHeader.handleTableOffset,
                chunk_.chunkHandleScratch.data(),
                static_cast<std::size_t>(handleTableBytes));
        }
        if (payloadArenaSize != 0) {
            std::memcpy(
                wireBlob + wireHeader.payloadArenaOffset,
                chunk_.payloadArena.data(),
                payloadArenaSize);
        }

        D9CCommandChunk chunk{};
        chunk.version = D9C_COMMAND_CHUNK_VERSION;
        chunk.recordCount = wireRecordCount;
        chunk.recordBytes = static_cast<std::uint32_t>(chunk_.wireBlob.size());
        chunk.records = toWireHandle(chunk_.wireBlob.data());
        chunk.handleCount = wireHandleCount;
        chunk.handles = D9CWireHandle{};

        const PeCommandChunkCommitInfo info{
            wireRecordCount,
            payloadArenaSize,
            wireHandleCount,
            static_cast<std::uint32_t>(chunk_.wireBlob.size()),
        };
        const HRESULT hr = commit(reason, chunk, info);
        if (SUCCEEDED(hr)) {
            clear();
        }
        return hr;
    }

    template<typename WriteFn, typename AppendExtraFn, typename FlushFn>
    HRESULT appendRecordDirect(std::uint32_t type,
                               std::size_t bytes,
                               std::uint32_t maxRecords,
                               std::size_t maxBytes,
                               WriteFn&& write,
                               AppendExtraFn&& appendExtraHandles,
                               FlushFn&& flushForCapacity) {
        if (bytes == 0 || bytes > 0xffffffffull) {
            return D3DERR_INVALIDCALL;
        }
        if (shouldFlushBeforeAppend(bytes, maxRecords, maxBytes)) {
            const HRESULT flushHr =
                flushForCapacity(PeRecorderFlushReason::CapacityPre);
            if (FAILED(flushHr)) return flushHr;
        }

        const auto payloadOffset = chunk_.payloadArena.size();
        if (payloadOffset > 0xffffffffull) {
            return D3DERR_INVALIDCALL;
        }
        chunk_.payloadArena.resize(payloadOffset + bytes);
        write(chunk_.payloadArena.data() + payloadOffset);

        D9CCommandChunkWireRecordHeader wireRecord{};
        wireRecord.type = type;
        wireRecord.flags = D9C_COMMAND_CHUNK_WIRE_RECORD_FLAG_NONE;
        wireRecord.payloadOffset = static_cast<std::uint32_t>(payloadOffset);
        wireRecord.payloadSize = static_cast<std::uint32_t>(bytes);
        wireRecord.firstHandle = 0;
        wireRecord.handleCount = 0;

        D3D9PePendingCommandRetainer::Acquired acquired{};
        WireHandleEntryList recordHandles{};
        const auto* payload = wireRecord.payloadSize == 0
            ? nullptr
            : chunk_.payloadArena.data() + wireRecord.payloadOffset;
        if (!retainRecordPayloadObjects(wireRecord, acquired) ||
            !collectRecordPayloadWireHandles(wireRecord, recordHandles) ||
            !appendExtraHandles(wireRecord, payload, recordHandles)) {
            chunk_.retainer.rollback(acquired);
            chunk_.payloadArena.resize(payloadOffset);
            return D3DERR_INVALIDCALL;
        }
        chunk_.retainer.retainWireHandles(recordHandles, acquired);
        if (chunk_.chunkHandleScratch.size() >
            0xffffffffull - recordHandles.size()) {
            chunk_.retainer.rollback(acquired);
            chunk_.payloadArena.resize(payloadOffset);
            return D3DERR_INVALIDCALL;
        }
        wireRecord.firstHandle =
            static_cast<std::uint32_t>(chunk_.chunkHandleScratch.size());
        wireRecord.handleCount =
            static_cast<std::uint32_t>(recordHandles.size());

        chunk_.wireRecords.push_back(wireRecord);
        chunk_.chunkHandleScratch.insert(chunk_.chunkHandleScratch.end(),
                                         recordHandles.begin(),
                                         recordHandles.end());
        ++chunk_.recordCount;

        if (chunk_.recordCount >= maxRecords ||
            chunk_.payloadArena.size() >= maxBytes) {
            return flushForCapacity(PeRecorderFlushReason::CapacityPost);
        }
        return S_OK;
    }

    template<typename AppendExtraFn, typename FlushFn>
    HRESULT appendRecord(const void* data,
                         std::size_t bytes,
                         std::uint32_t maxRecords,
                         std::size_t maxBytes,
                         AppendExtraFn&& appendExtraHandles,
                         FlushFn&& flushForCapacity) {
        if (!data) {
            return D3DERR_INVALIDCALL;
        }
        std::uint32_t type = 0;
        if (bytes >= sizeof(D9CCommandRecordHeader)) {
            D9CCommandRecordHeader legacyHeader{};
            std::memcpy(&legacyHeader, data, sizeof(legacyHeader));
            type = legacyHeader.type;
        }
        return appendRecordDirect(
            type, bytes, maxRecords, maxBytes,
            [data, bytes](std::uint8_t* dst) {
                std::memcpy(dst, data, bytes);
            },
            std::forward<AppendExtraFn>(appendExtraHandles),
            std::forward<FlushFn>(flushForCapacity));
    }

private:
    template<typename T>
    static T* wireHandleAsPtr(const D9CWireHandle& handle) {
        return reinterpret_cast<T*>(
            static_cast<std::uintptr_t>(d9cWireHandleValue(handle)));
    }

    void retainDrawPacketPayloadObjects(
        const D9CDrawPrimitivePacket& packet,
        D3D9PePendingCommandRetainer::Acquired& acquired) {
        for (std::uint32_t stage = 0; stage < D9C_DRAW_PACKET_MAX_TEXTURES; ++stage) {
            if ((packet.textureMask & (1u << stage)) != 0) {
                chunk_.retainer.retainTexture(
                    wireHandleAsPtr<D9CTexture>(packet.textures[stage]), acquired);
            }
        }
        for (std::uint32_t stream = 0; stream < D9C_DRAW_PACKET_MAX_STREAMS; ++stream) {
            if ((packet.streamSourceMask & (1u << stream)) != 0) {
                chunk_.retainer.retainBuffer(
                    wireHandleAsPtr<D9CBuffer>(packet.streamSources[stream].buffer),
                    acquired);
            }
        }
        for (std::uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_RENDER_TARGETS; ++slot) {
            if ((packet.rtMask & (1u << slot)) != 0) {
                chunk_.retainer.retainSurface(
                    wireHandleAsPtr<D9CSurface>(packet.rtHandles[slot]), acquired);
            }
        }
        if (packet.dsValid != 0) {
            chunk_.retainer.retainSurface(
                wireHandleAsPtr<D9CSurface>(packet.dsHandle), acquired);
        }
        if (packet.vsValid != 0) {
            chunk_.retainer.retainShader(
                wireHandleAsPtr<D9CShader>(packet.vsHandle), acquired);
        }
        if (packet.psValid != 0) {
            chunk_.retainer.retainShader(
                wireHandleAsPtr<D9CShader>(packet.psHandle), acquired);
        }
        if (packet.vdeclValid != 0) {
            chunk_.retainer.retainVdecl(
                wireHandleAsPtr<D9CVertexDecl>(packet.vdeclHandle), acquired);
        }
    }

    void retainIndexedDrawPacketPayloadObjects(
        const D9CDrawIndexedPrimitivePacket& packet,
        D3D9PePendingCommandRetainer::Acquired& acquired) {
        retainDrawPacketPayloadObjects(packet.state, acquired);
        if (packet.ibValid != 0) {
            chunk_.retainer.retainBuffer(
                wireHandleAsPtr<D9CBuffer>(packet.ibHandle), acquired);
        }
    }

    bool retainRecordPayloadObjects(
        const D9CCommandChunkWireRecordHeader& wireRecord,
        D3D9PePendingCommandRetainer::Acquired& acquired) {
        const std::uint8_t* recordPayload = nullptr;
        if (!payload(wireRecord, recordPayload)) {
            return false;
        }

        switch (wireRecord.type) {
        case D9C_COMMAND_RECORD_DRAW_PRIMITIVE: {
            if (wireRecord.payloadSize < sizeof(D9CCommandRecordDrawPrimitive)) {
                return false;
            }
            D9CCommandRecordDrawPrimitive decoded{};
            std::memcpy(&decoded, recordPayload, sizeof(decoded));
            retainDrawPacketPayloadObjects(decoded.packet, acquired);
            return true;
        }
        case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE: {
            if (wireRecord.payloadSize < sizeof(D9CCommandRecordDrawIndexedPrimitive)) {
                return false;
            }
            D9CCommandRecordDrawIndexedPrimitive decoded{};
            std::memcpy(&decoded, recordPayload, sizeof(decoded));
            retainIndexedDrawPacketPayloadObjects(decoded.packet, acquired);
            return true;
        }
        case D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP: {
            if (wireRecord.payloadSize < sizeof(D9CCommandRecordDrawPrimitiveUP)) {
                return false;
            }
            D9CCommandRecordDrawPrimitiveUP decoded{};
            std::memcpy(&decoded, recordPayload, sizeof(decoded));
            retainDrawPacketPayloadObjects(decoded.packet.state, acquired);
            return true;
        }
        case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP: {
            if (wireRecord.payloadSize < sizeof(D9CCommandRecordDrawIndexedPrimitiveUP)) {
                return false;
            }
            D9CCommandRecordDrawIndexedPrimitiveUP decoded{};
            std::memcpy(&decoded, recordPayload, sizeof(decoded));
            retainDrawPacketPayloadObjects(decoded.packet.state, acquired);
            return true;
        }
        case D9C_COMMAND_RECORD_APPLY_STATE: {
            if (wireRecord.payloadSize < sizeof(D9CCommandRecordApplyState)) {
                return false;
            }
            D9CCommandRecordApplyState decoded{};
            std::memcpy(&decoded, recordPayload, sizeof(decoded));
            retainDrawPacketPayloadObjects(decoded.packet, acquired);
            return true;
        }
        case D9C_COMMAND_RECORD_STRETCH_RECT: {
            if (wireRecord.payloadSize < sizeof(D9CCommandRecordStretchRect)) {
                return false;
            }
            D9CCommandRecordStretchRect decoded{};
            std::memcpy(&decoded, recordPayload, sizeof(decoded));
            chunk_.retainer.retainSurface(
                reinterpret_cast<D9CSurface*>(
                    static_cast<std::uintptr_t>(decoded.srcWire)), acquired);
            chunk_.retainer.retainSurface(
                reinterpret_cast<D9CSurface*>(
                    static_cast<std::uintptr_t>(decoded.dstWire)), acquired);
            return true;
        }
        case D9C_COMMAND_RECORD_COLOR_FILL: {
            if (wireRecord.payloadSize < sizeof(D9CCommandRecordColorFill)) {
                return false;
            }
            D9CCommandRecordColorFill decoded{};
            std::memcpy(&decoded, recordPayload, sizeof(decoded));
            chunk_.retainer.retainSurface(
                reinterpret_cast<D9CSurface*>(
                    static_cast<std::uintptr_t>(decoded.surfaceWire)), acquired);
            return true;
        }
        case D9C_COMMAND_RECORD_UPDATE_TEXTURE: {
            if (wireRecord.payloadSize < sizeof(D9CCommandRecordUpdateTexture)) {
                return false;
            }
            D9CCommandRecordUpdateTexture decoded{};
            std::memcpy(&decoded, recordPayload, sizeof(decoded));
            chunk_.retainer.retainTexture(
                reinterpret_cast<D9CTexture*>(
                    static_cast<std::uintptr_t>(decoded.srcWire)), acquired);
            chunk_.retainer.retainTexture(
                reinterpret_cast<D9CTexture*>(
                    static_cast<std::uintptr_t>(decoded.dstWire)), acquired);
            return true;
        }
        case D9C_COMMAND_RECORD_UPDATE_SURFACE: {
            if (wireRecord.payloadSize < sizeof(D9CCommandRecordUpdateSurface)) {
                return false;
            }
            D9CCommandRecordUpdateSurface decoded{};
            std::memcpy(&decoded, recordPayload, sizeof(decoded));
            chunk_.retainer.retainSurface(
                reinterpret_cast<D9CSurface*>(
                    static_cast<std::uintptr_t>(decoded.srcWire)), acquired);
            chunk_.retainer.retainSurface(
                reinterpret_cast<D9CSurface*>(
                    static_cast<std::uintptr_t>(decoded.dstWire)), acquired);
            return true;
        }
        case D9C_COMMAND_RECORD_READBACK: {
            if (wireRecord.payloadSize < sizeof(D9CCommandRecordReadback)) {
                return false;
            }
            D9CCommandRecordReadback decoded{};
            std::memcpy(&decoded, recordPayload, sizeof(decoded));
            chunk_.retainer.retainSurface(
                reinterpret_cast<D9CSurface*>(
                    static_cast<std::uintptr_t>(decoded.srcWire)), acquired);
            chunk_.retainer.retainSurface(
                reinterpret_cast<D9CSurface*>(
                    static_cast<std::uintptr_t>(decoded.dstWire)), acquired);
            return true;
        }
        default:
            return true;
        }
    }

    bool collectRecordPayloadWireHandles(
        const D9CCommandChunkWireRecordHeader& wireRecord,
        WireHandleEntryList& handles) const {
        const std::uint8_t* recordPayload = nullptr;
        if (!payload(wireRecord, recordPayload)) {
            return false;
        }

        switch (wireRecord.type) {
        case D9C_COMMAND_RECORD_DRAW_PRIMITIVE: {
            if (wireRecord.payloadSize < sizeof(D9CCommandRecordDrawPrimitive)) {
                return false;
            }
            D9CCommandRecordDrawPrimitive decoded{};
            std::memcpy(&decoded, recordPayload, sizeof(decoded));
            return appendDrawPacketWireHandles(decoded.packet, handles);
        }
        case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE: {
            if (wireRecord.payloadSize < sizeof(D9CCommandRecordDrawIndexedPrimitive)) {
                return false;
            }
            D9CCommandRecordDrawIndexedPrimitive decoded{};
            std::memcpy(&decoded, recordPayload, sizeof(decoded));
            return appendIndexedDrawPacketWireHandles(decoded.packet, handles);
        }
        case D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP: {
            if (wireRecord.payloadSize < sizeof(D9CCommandRecordDrawPrimitiveUP)) {
                return false;
            }
            D9CCommandRecordDrawPrimitiveUP decoded{};
            std::memcpy(&decoded, recordPayload, sizeof(decoded));
            return appendDrawPacketWireHandles(decoded.packet.state, handles);
        }
        case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP: {
            if (wireRecord.payloadSize < sizeof(D9CCommandRecordDrawIndexedPrimitiveUP)) {
                return false;
            }
            D9CCommandRecordDrawIndexedPrimitiveUP decoded{};
            std::memcpy(&decoded, recordPayload, sizeof(decoded));
            return appendDrawPacketWireHandles(decoded.packet.state, handles);
        }
        case D9C_COMMAND_RECORD_APPLY_STATE: {
            if (wireRecord.payloadSize < sizeof(D9CCommandRecordApplyState)) {
                return false;
            }
            D9CCommandRecordApplyState decoded{};
            std::memcpy(&decoded, recordPayload, sizeof(decoded));
            return appendDrawPacketWireHandles(decoded.packet, handles);
        }
        case D9C_COMMAND_RECORD_STRETCH_RECT: {
            if (wireRecord.payloadSize < sizeof(D9CCommandRecordStretchRect)) {
                return false;
            }
            D9CCommandRecordStretchRect decoded{};
            std::memcpy(&decoded, recordPayload, sizeof(decoded));
            return appendRecordWireHandle(handles, D9C_CHUNK_HANDLE_KIND_SURFACE,
                                          decoded.srcWire) &&
                   appendRecordWireHandle(handles, D9C_CHUNK_HANDLE_KIND_SURFACE,
                                          decoded.dstWire);
        }
        case D9C_COMMAND_RECORD_COLOR_FILL: {
            if (wireRecord.payloadSize < sizeof(D9CCommandRecordColorFill)) {
                return false;
            }
            D9CCommandRecordColorFill decoded{};
            std::memcpy(&decoded, recordPayload, sizeof(decoded));
            return appendRecordWireHandle(handles, D9C_CHUNK_HANDLE_KIND_SURFACE,
                                          decoded.surfaceWire);
        }
        case D9C_COMMAND_RECORD_UPDATE_TEXTURE: {
            if (wireRecord.payloadSize < sizeof(D9CCommandRecordUpdateTexture)) {
                return false;
            }
            D9CCommandRecordUpdateTexture decoded{};
            std::memcpy(&decoded, recordPayload, sizeof(decoded));
            return appendRecordWireHandle(handles, D9C_CHUNK_HANDLE_KIND_TEXTURE,
                                          decoded.srcWire) &&
                   appendRecordWireHandle(handles, D9C_CHUNK_HANDLE_KIND_TEXTURE,
                                          decoded.dstWire);
        }
        case D9C_COMMAND_RECORD_UPDATE_SURFACE: {
            if (wireRecord.payloadSize < sizeof(D9CCommandRecordUpdateSurface)) {
                return false;
            }
            D9CCommandRecordUpdateSurface decoded{};
            std::memcpy(&decoded, recordPayload, sizeof(decoded));
            return appendRecordWireHandle(handles, D9C_CHUNK_HANDLE_KIND_SURFACE,
                                          decoded.srcWire) &&
                   appendRecordWireHandle(handles, D9C_CHUNK_HANDLE_KIND_SURFACE,
                                          decoded.dstWire);
        }
        case D9C_COMMAND_RECORD_READBACK: {
            if (wireRecord.payloadSize < sizeof(D9CCommandRecordReadback)) {
                return false;
            }
            D9CCommandRecordReadback decoded{};
            std::memcpy(&decoded, recordPayload, sizeof(decoded));
            return appendRecordWireHandle(handles, D9C_CHUNK_HANDLE_KIND_SURFACE,
                                          decoded.srcWire) &&
                   appendRecordWireHandle(handles, D9C_CHUNK_HANDLE_KIND_SURFACE,
                                          decoded.dstWire);
        }
        default:
            return true;
        }
    }

    PendingCommandChunk chunk_{};
};
