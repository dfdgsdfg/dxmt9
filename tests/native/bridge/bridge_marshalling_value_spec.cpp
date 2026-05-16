#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "dxmt9/wineunixlib.h"
#include "dxmt9_bridge_ops.generated.h"
#include "util/unixcall_marshal.hpp"

namespace {

// The generated PE client wrapper constructs these Args_* blocks before
// calling dxmt9_winemetal_unix_call, but that generated client TU includes
// windows.h and belongs to the PE forwarder. The generated unix dispatch TU is
// monolithic and references every dxmt9c_* implementation. This native test
// therefore observes the stable boundary shape at the generated argument block
// and marshal-helper level without linking either production generated TU.

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

template <typename A, typename B>
void checkEq(const A& left, const B& right, std::string_view message) {
  if (!(left == right)) {
    std::ostringstream out;
    out << message << " (" << left << " vs " << right << ")";
    fail(out.str());
  }
}

void checkSamePtr(const void* left, const void* right, std::string_view message) {
  if (left != right) {
    std::ostringstream out;
    out << message << " (" << left << " vs " << right << ")";
    fail(out.str());
  }
}

constexpr std::size_t alignUp(std::size_t value, std::size_t alignment) {
  return ((value + alignment - 1u) / alignment) * alignment;
}

template <typename T>
void checkPodArgShape(std::string_view name,
                      std::size_t expectedSize,
                      std::size_t expectedAlign) {
  check(std::is_standard_layout_v<T>, std::string(name) + " is standard layout");
  check(std::is_trivially_copyable_v<T>,
        std::string(name) + " is trivially copyable");
  checkEq(sizeof(T), expectedSize, std::string(name) + " byte size");
  checkEq(alignof(T), expectedAlign, std::string(name) + " alignment");
}

template <typename T>
void appendObject(std::vector<std::uint8_t>& bytes, const T& value) {
  const auto offset = bytes.size();
  bytes.resize(offset + sizeof(T));
  std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

void appendBytes(std::vector<std::uint8_t>& bytes,
                 const void* data,
                 std::size_t size) {
  const auto offset = bytes.size();
  bytes.resize(offset + size);
  std::memcpy(bytes.data() + offset, data, size);
}

template <typename T>
void writeObject(std::vector<std::uint8_t>& bytes,
                 std::size_t offset,
                 const T& value) {
  check(offset <= bytes.size() && sizeof(T) <= bytes.size() - offset,
        "writeObject range is inside byte vector");
  std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

template <typename T>
T readObject(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  check(offset <= bytes.size() && sizeof(T) <= bytes.size() - offset,
        "readObject range is inside byte vector");
  T value{};
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return value;
}

D9CWireHandle toWireHandle(const void* pointer) {
  const auto value =
      static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer));
  return D9CWireHandle{
      static_cast<std::uint32_t>(value & 0xffffffffull),
      static_cast<std::uint32_t>(value >> 32),
  };
}

std::uint64_t wireHandleValue(const D9CWireHandle& handle) {
  return static_cast<std::uint64_t>(handle.lo) |
         (static_cast<std::uint64_t>(handle.hi) << 32);
}

const void* wireHandlePtr(const D9CWireHandle& handle) {
  return reinterpret_cast<const void*>(
      static_cast<std::uintptr_t>(wireHandleValue(handle)));
}

D9CDevice* fakeDevice() {
  return reinterpret_cast<D9CDevice*>(static_cast<std::uintptr_t>(0x13579000u));
}

D9CFactory* fakeFactory() {
  return reinterpret_cast<D9CFactory*>(static_cast<std::uintptr_t>(0x2468a000u));
}

constexpr std::array<float, 8> kConstPayload{
    -0.0f, 1.0f, -2.5f, 3.75f, 65504.0f, -8192.5f, 0.125f, 42.0f};

struct WireBlobFixture {
  std::vector<std::uint8_t> blob;
  std::uint32_t recordTableOffset = 0;
  std::uint32_t handleTableOffset = 0;
  std::uint32_t payloadArenaOffset = 0;
  std::uint32_t constPayloadOffset = 0;
  std::uint32_t drawPayloadOffset = 0;
  std::uint32_t presentPayloadOffset = 0;
  std::uint32_t constPayloadSize = 0;
};

std::vector<std::uint8_t> makeConstRecordPayload() {
  D9CCommandRecordSetConst record{};
  record.header.type = D9C_COMMAND_RECORD_SET_VS_CONST_F;
  record.header.size =
      static_cast<std::uint32_t>(sizeof(record) + sizeof(float) * kConstPayload.size());
  record.start = 240u;
  record.count = 2u;

  std::vector<std::uint8_t> payload;
  appendObject(payload, record);
  appendBytes(payload, kConstPayload.data(), sizeof(float) * kConstPayload.size());
  return payload;
}

D9CCommandRecordDrawPrimitive makeDrawRecordPayload() {
  D9CCommandRecordDrawPrimitive record{};
  record.header.type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE;
  record.header.size = static_cast<std::uint32_t>(sizeof(record));
  record.packet.primitiveType = 4u;
  record.packet.startVertex = 0x1234u;
  record.packet.primitiveCount = 0x56u;
  record.packet.textureMask = 1u << 3u;
  record.packet.textures[3] =
      D9CWireHandle{0x00c0ffeeu, 0x00000001u};
  record.packet.tssCount = 1u;
  record.packet.tss[0] = D9CDrawPacketTextureStageState{
      7u,
      24u,
      0xaabbccddu,
  };
  record.packet.samplerStateCount = 1u;
  record.packet.samplerStates[0] = D9CDrawPacketSamplerState{
      15u,
      6u,
      0x10203040u,
  };
  return record;
}

D9CCommandRecordPresent makePresentRecordPayload() {
  D9CCommandRecordPresent record{};
  record.header.type = D9C_COMMAND_RECORD_PRESENT;
  record.header.size = static_cast<std::uint32_t>(sizeof(record));
  record.hwnd = 0x1122334455667788ull;
  record.flags = 0x01020304u;
  record.hasSrc = 1u;
  record.hasDst = 1u;
  record.src = D9CRect{1, 2, 640, 480};
  record.dst = D9CRect{10, 20, 650, 500};
  return record;
}

WireBlobFixture makeWireBlobFixture() {
  WireBlobFixture fixture;

  const auto constPayload = makeConstRecordPayload();
  const auto drawPayload = makeDrawRecordPayload();
  const auto presentPayload = makePresentRecordPayload();

  std::vector<std::uint8_t> arena;
  fixture.constPayloadOffset = 0u;
  appendBytes(arena, constPayload.data(), constPayload.size());
  fixture.constPayloadSize = static_cast<std::uint32_t>(constPayload.size());
  arena.resize(arena.size() + 8u, 0xcdu);

  fixture.drawPayloadOffset = static_cast<std::uint32_t>(arena.size());
  appendObject(arena, drawPayload);
  arena.resize(arena.size() + 8u, 0xefu);

  fixture.presentPayloadOffset = static_cast<std::uint32_t>(arena.size());
  appendObject(arena, presentPayload);

  std::vector<D9CCommandChunkWireRecordHeader> records{
      D9CCommandChunkWireRecordHeader{
          D9C_COMMAND_RECORD_PRESENT,
          D9C_COMMAND_CHUNK_WIRE_RECORD_FLAG_NONE,
          fixture.presentPayloadOffset,
          static_cast<std::uint32_t>(sizeof(presentPayload)),
          2u,
          1u,
          0u,
          0u,
      },
      D9CCommandChunkWireRecordHeader{
          D9C_COMMAND_RECORD_SET_VS_CONST_F,
          D9C_COMMAND_CHUNK_WIRE_RECORD_FLAG_NONE,
          fixture.constPayloadOffset,
          fixture.constPayloadSize,
          0u,
          0u,
          0u,
          0u,
      },
      D9CCommandChunkWireRecordHeader{
          D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
          D9C_COMMAND_CHUNK_WIRE_RECORD_FLAG_NONE,
          fixture.drawPayloadOffset,
          static_cast<std::uint32_t>(sizeof(drawPayload)),
          0u,
          2u,
          0u,
          0u,
      },
  };

  std::vector<D9CCommandChunkWireHandleEntry> handles{
      D9CCommandChunkWireHandleEntry{
          D9C_CHUNK_HANDLE_KIND_BUFFER,
          0x10u,
          0x000000010000beefull,
          0u,
          0u,
      },
      D9CCommandChunkWireHandleEntry{
          D9C_CHUNK_HANDLE_KIND_TEXTURE,
          0x11u,
          0x000000020000cafeull,
          0u,
          0u,
      },
      D9CCommandChunkWireHandleEntry{
          D9C_CHUNK_HANDLE_KIND_SURFACE,
          0x12u,
          0x000000030000d00dull,
          0u,
          0u,
      },
  };

  fixture.recordTableOffset =
      static_cast<std::uint32_t>(sizeof(D9CCommandChunkWireHeader));
  const auto recordTableBytes =
      static_cast<std::uint32_t>(records.size() * sizeof(records[0]));
  fixture.handleTableOffset = fixture.recordTableOffset + recordTableBytes;
  const auto handleTableBytes =
      static_cast<std::uint32_t>(handles.size() * sizeof(handles[0]));
  fixture.payloadArenaOffset = fixture.handleTableOffset + handleTableBytes + 16u;

  D9CCommandChunkWireHeader header{};
  header.version = D9C_COMMAND_CHUNK_WIRE_VERSION;
  header.headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE;
  header.recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE;
  header.handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE;
  header.recordTableOffset = fixture.recordTableOffset;
  header.recordCount = static_cast<std::uint32_t>(records.size());
  header.handleTableOffset = fixture.handleTableOffset;
  header.handleCount = static_cast<std::uint32_t>(handles.size());
  header.payloadArenaOffset = fixture.payloadArenaOffset;
  header.payloadArenaSize = static_cast<std::uint32_t>(arena.size());

  fixture.blob.resize(fixture.payloadArenaOffset + arena.size());
  writeObject(fixture.blob, 0u, header);
  std::memcpy(fixture.blob.data() + fixture.recordTableOffset, records.data(),
              recordTableBytes);
  std::memcpy(fixture.blob.data() + fixture.handleTableOffset, handles.data(),
              handleTableBytes);
  std::memcpy(fixture.blob.data() + fixture.payloadArenaOffset, arena.data(),
              arena.size());
  return fixture;
}

struct CommitChunkCapture {
  bool seen = false;
  D9CDevice* device = nullptr;
  const D9CCommandChunk* chunk = nullptr;
};

NTSTATUS captureCommitChunk(void* opaque,
                            std::int32_t returnedStatus,
                            CommitChunkCapture& capture) {
  auto* args =
      dxmt9::util::marshal::decodeOpaque<
          dxmt9::bridge::Args_dxmt9c_device_commit_chunk>(opaque);
  if (!args) {
    return DXMT9_STATUS_INVALID_PARAMETER;
  }

  capture.seen = true;
  capture.device = args->arg0;
  capture.chunk = args->arg1;
  args->ret = returnedStatus;
  return DXMT9_STATUS_SUCCESS;
}

void testGeneratedArgumentStructLayouts() {
  constexpr std::size_t kPtr = sizeof(void*);
  constexpr std::size_t kPtrAlign = alignof(void*);

  checkPodArgShape<dxmt9::bridge::Args_dxmt9c_device_commit_chunk>(
      "Args_dxmt9c_device_commit_chunk", alignUp(kPtr * 2u + 4u, kPtrAlign),
      kPtrAlign);
  checkEq(offsetof(dxmt9::bridge::Args_dxmt9c_device_commit_chunk, arg0),
          std::size_t{0}, "commit chunk device pointer offset");
  checkEq(offsetof(dxmt9::bridge::Args_dxmt9c_device_commit_chunk, arg1), kPtr,
          "commit chunk payload pointer offset");
  checkEq(offsetof(dxmt9::bridge::Args_dxmt9c_device_commit_chunk, ret),
          kPtr * 2u, "commit chunk HRESULT/status offset");

  checkPodArgShape<dxmt9::bridge::Args32_dxmt9c_device_commit_chunk>(
      "Args32_dxmt9c_device_commit_chunk", 12u, 4u);
  checkEq(offsetof(dxmt9::bridge::Args32_dxmt9c_device_commit_chunk, arg0),
          std::size_t{0}, "wow64 commit chunk device token offset");
  checkEq(offsetof(dxmt9::bridge::Args32_dxmt9c_device_commit_chunk, arg1),
          std::size_t{4}, "wow64 commit chunk client pointer offset");
  checkEq(offsetof(dxmt9::bridge::Args32_dxmt9c_device_commit_chunk, ret),
          std::size_t{8}, "wow64 commit chunk HRESULT/status offset");

  checkPodArgShape<dxmt9::bridge::Args_dxmt9c_device_set_sampler_state>(
      "Args_dxmt9c_device_set_sampler_state", alignUp(kPtr + 16u, kPtrAlign),
      kPtrAlign);
  checkEq(offsetof(dxmt9::bridge::Args_dxmt9c_device_set_sampler_state, sampler),
          kPtr, "sampler state sampler offset");
  checkEq(offsetof(dxmt9::bridge::Args_dxmt9c_device_set_sampler_state, ret),
          kPtr + 12u, "sampler state HRESULT/status offset");

  checkPodArgShape<dxmt9::bridge::Args_dxmt9c_device_set_texture_stage_state>(
      "Args_dxmt9c_device_set_texture_stage_state",
      alignUp(kPtr + 16u, kPtrAlign), kPtrAlign);
  checkEq(offsetof(dxmt9::bridge::Args_dxmt9c_device_set_texture_stage_state,
                   stage),
          kPtr, "TSS stage offset");
  checkEq(offsetof(dxmt9::bridge::Args_dxmt9c_device_set_texture_stage_state,
                   ret),
          kPtr + 12u, "TSS HRESULT/status offset");

  constexpr std::size_t kConstDataOffset = alignUp(kPtr + 4u, kPtrAlign);
  constexpr std::size_t kConstRetOffset = kConstDataOffset + kPtr + 4u;
  checkPodArgShape<dxmt9::bridge::Args_dxmt9c_device_set_vs_const_f>(
      "Args_dxmt9c_device_set_vs_const_f",
      alignUp(kConstRetOffset + 4u, kPtrAlign), kPtrAlign);
  checkEq(offsetof(dxmt9::bridge::Args_dxmt9c_device_set_vs_const_f, data),
          kConstDataOffset, "VS const pointer offset");
  checkEq(offsetof(dxmt9::bridge::Args_dxmt9c_device_set_vs_const_f, ret),
          kConstRetOffset, "VS const HRESULT/status offset");

  checkPodArgShape<dxmt9::bridge::Args_dxmt9c_factory_check_device_type>(
      "Args_dxmt9c_factory_check_device_type", alignUp(kPtr + 24u, kPtrAlign),
      kPtrAlign);
  checkEq(offsetof(dxmt9::bridge::Args_dxmt9c_factory_check_device_type,
                   adapter),
          kPtr, "factory check_device_type adapter offset");
  checkEq(offsetof(dxmt9::bridge::Args_dxmt9c_factory_check_device_type, ret),
          kPtr + 20u, "factory check_device_type HRESULT/status offset");

  checkPodArgShape<dxmt9::bridge::Args32_dxmt9c_factory_check_device_format2>(
      "Args32_dxmt9c_factory_check_device_format2", 24u, 4u);
  checkEq(offsetof(dxmt9::bridge::Args32_dxmt9c_factory_check_device_format2,
                   resourceType),
          std::size_t{16}, "wow64 factory resource type offset");
  checkEq(offsetof(dxmt9::bridge::Args32_dxmt9c_factory_check_device_format2,
                   ret),
          std::size_t{20}, "wow64 factory HRESULT/status offset");
}

void testCommitChunkArgsPreservePointerAndWireValues() {
  auto fixture = makeWireBlobFixture();
  const D9CDevice* device = fakeDevice();

  D9CCommandChunk chunk{};
  chunk.version = D9C_COMMAND_CHUNK_VERSION;
  chunk.recordCount = 3u;
  chunk.recordBytes = static_cast<std::uint32_t>(fixture.blob.size());
  chunk.records = toWireHandle(fixture.blob.data());
  chunk.handleCount = 3u;
  chunk.handles = D9CWireHandle{};

  dxmt9::bridge::Args_dxmt9c_device_commit_chunk args{};
  args.arg0 = const_cast<D9CDevice*>(device);
  args.arg1 = &chunk;

  constexpr std::int32_t kReturnedStatus = 0x13572468;
  CommitChunkCapture capture;
  const NTSTATUS unixStatus = captureCommitChunk(&args, kReturnedStatus, capture);

  checkEq(unixStatus, DXMT9_STATUS_SUCCESS,
          "commit chunk generated-style sink returns unix-call success");
  check(capture.seen, "commit chunk generated-style sink decoded args");
  checkSamePtr(capture.device, device, "commit chunk preserves device pointer");
  checkSamePtr(capture.chunk, &chunk, "commit chunk preserves chunk pointer");
  checkEq(args.ret, kReturnedStatus,
          "commit chunk HRESULT/status returns through generated args");

  const auto* decodedChunk = capture.chunk;
  checkEq(decodedChunk->version, D9C_COMMAND_CHUNK_VERSION,
          "commit chunk preserves D9CCommandChunk version");
  checkEq(decodedChunk->recordCount, 3u,
          "commit chunk preserves record count");
  checkEq(decodedChunk->recordBytes,
          static_cast<std::uint32_t>(fixture.blob.size()),
          "commit chunk preserves record byte length");
  checkSamePtr(wireHandlePtr(decodedChunk->records), fixture.blob.data(),
               "commit chunk preserves record blob pointer");
  checkEq(decodedChunk->handleCount, 3u,
          "commit chunk preserves handle count");
  checkEq(wireHandleValue(decodedChunk->handles), 0ull,
          "commit chunk preserves empty legacy handle pointer");

  const auto header =
      readObject<D9CCommandChunkWireHeader>(fixture.blob, 0u);
  checkEq(header.recordTableOffset, fixture.recordTableOffset,
          "wire header preserves record table offset");
  checkEq(header.recordCount, 3u,
          "wire header preserves generated record table count");
  checkEq(header.handleTableOffset, fixture.handleTableOffset,
          "wire header preserves handle table offset");
  checkEq(header.handleCount, 3u,
          "wire header preserves handle table count");
  checkEq(header.payloadArenaOffset, fixture.payloadArenaOffset,
          "wire header preserves payload arena offset");

  const auto firstRecord = readObject<D9CCommandChunkWireRecordHeader>(
      fixture.blob, fixture.recordTableOffset);
  const auto secondRecord = readObject<D9CCommandChunkWireRecordHeader>(
      fixture.blob,
      fixture.recordTableOffset + sizeof(D9CCommandChunkWireRecordHeader));
  const auto thirdRecord = readObject<D9CCommandChunkWireRecordHeader>(
      fixture.blob,
      fixture.recordTableOffset + sizeof(D9CCommandChunkWireRecordHeader) * 2u);

  checkEq(firstRecord.type, static_cast<std::uint32_t>(D9C_COMMAND_RECORD_PRESENT),
          "wire record table keeps first ordering record");
  checkEq(firstRecord.payloadOffset, fixture.presentPayloadOffset,
          "wire record table keeps first payload offset");
  checkEq(firstRecord.firstHandle, 2u,
          "wire first record keeps handle range start");
  checkEq(firstRecord.handleCount, 1u,
          "wire first record keeps handle range count");
  checkEq(secondRecord.type,
          static_cast<std::uint32_t>(D9C_COMMAND_RECORD_SET_VS_CONST_F),
          "wire record table keeps second constant record");
  checkEq(secondRecord.payloadOffset, fixture.constPayloadOffset,
          "wire second record may point before first payload");
  checkEq(thirdRecord.type,
          static_cast<std::uint32_t>(D9C_COMMAND_RECORD_DRAW_PRIMITIVE),
          "wire record table keeps third draw record");
  checkEq(thirdRecord.payloadOffset, fixture.drawPayloadOffset,
          "wire third record preserves draw payload offset");
  checkEq(thirdRecord.firstHandle, 0u,
          "wire third record keeps handle range start");
  checkEq(thirdRecord.handleCount, 2u,
          "wire third record keeps handle range count");

  const auto firstHandle = readObject<D9CCommandChunkWireHandleEntry>(
      fixture.blob, fixture.handleTableOffset);
  const auto secondHandle = readObject<D9CCommandChunkWireHandleEntry>(
      fixture.blob,
      fixture.handleTableOffset + sizeof(D9CCommandChunkWireHandleEntry));
  const auto thirdHandle = readObject<D9CCommandChunkWireHandleEntry>(
      fixture.blob,
      fixture.handleTableOffset + sizeof(D9CCommandChunkWireHandleEntry) * 2u);

  checkEq(firstHandle.kind, static_cast<std::uint32_t>(D9C_CHUNK_HANDLE_KIND_BUFFER),
          "wire handle table preserves first handle kind");
  checkEq(firstHandle.generation, 0x10u,
          "wire handle table preserves first handle generation");
  checkEq(firstHandle.opaqueHandle, 0x000000010000beefull,
          "wire handle table preserves first opaque handle");
  checkEq(secondHandle.kind,
          static_cast<std::uint32_t>(D9C_CHUNK_HANDLE_KIND_TEXTURE),
          "wire handle table preserves second handle kind");
  checkEq(secondHandle.opaqueHandle, 0x000000020000cafeull,
          "wire handle table preserves second opaque handle");
  checkEq(thirdHandle.kind, static_cast<std::uint32_t>(D9C_CHUNK_HANDLE_KIND_SURFACE),
          "wire handle table preserves third handle kind");
  checkEq(thirdHandle.opaqueHandle, 0x000000030000d00dull,
          "wire handle table preserves third opaque handle");

  const auto decodedDraw = readObject<D9CCommandRecordDrawPrimitive>(
      fixture.blob, fixture.payloadArenaOffset + fixture.drawPayloadOffset);
  checkEq(decodedDraw.packet.startVertex, 0x1234u,
          "commit chunk draw payload preserves start vertex");
  checkEq(decodedDraw.packet.primitiveCount, 0x56u,
          "commit chunk draw payload preserves primitive count");
  checkEq(decodedDraw.packet.tssCount, 1u,
          "commit chunk draw payload preserves TSS count");
  checkEq(decodedDraw.packet.tss[0].stage, 7u,
          "commit chunk draw payload preserves TSS stage");
  checkEq(decodedDraw.packet.tss[0].type, 24u,
          "commit chunk draw payload preserves TSS type");
  checkEq(decodedDraw.packet.tss[0].value, 0xaabbccddu,
          "commit chunk draw payload preserves TSS value");
  checkEq(decodedDraw.packet.samplerStateCount, 1u,
          "commit chunk draw payload preserves sampler count");
  checkEq(decodedDraw.packet.samplerStates[0].sampler, 15u,
          "commit chunk draw payload preserves sampler slot");
  checkEq(decodedDraw.packet.samplerStates[0].type, 6u,
          "commit chunk draw payload preserves sampler type");
  checkEq(decodedDraw.packet.samplerStates[0].value, 0x10203040u,
          "commit chunk draw payload preserves sampler value");

  const auto decodedConst = readObject<D9CCommandRecordSetConst>(
      fixture.blob, fixture.payloadArenaOffset + fixture.constPayloadOffset);
  checkEq(decodedConst.start, 240u,
          "commit chunk const payload preserves start register");
  checkEq(decodedConst.count, 2u,
          "commit chunk const payload preserves vector count");
  const auto constBytesOffset = fixture.payloadArenaOffset +
                                fixture.constPayloadOffset +
                                sizeof(D9CCommandRecordSetConst);
  check(std::memcmp(fixture.blob.data() + constBytesOffset,
                    kConstPayload.data(), sizeof(float) * kConstPayload.size()) == 0,
        "commit chunk const payload preserves exact float bytes");
}

void testRepresentativeGeneratedStateArgsPreserveValues() {
  dxmt9::bridge::Args_dxmt9c_device_set_sampler_state sampler{};
  sampler.arg0 = fakeDevice();
  sampler.sampler = 15u;
  sampler.type = 6u;
  sampler.value = 0x10203040u;
  auto* decodedSampler =
      dxmt9::util::marshal::decodeOpaque<
          dxmt9::bridge::Args_dxmt9c_device_set_sampler_state>(&sampler);
  checkSamePtr(decodedSampler->arg0, fakeDevice(),
               "generated sampler args preserve device pointer");
  checkEq(decodedSampler->sampler, 15u,
          "generated sampler args preserve sampler slot");
  checkEq(decodedSampler->type, 6u,
          "generated sampler args preserve state type");
  checkEq(decodedSampler->value, 0x10203040u,
          "generated sampler args preserve state value");
  decodedSampler->ret = 0x01020304;
  checkEq(sampler.ret, 0x01020304,
          "generated sampler args preserve returned status");

  dxmt9::bridge::Args_dxmt9c_device_set_texture_stage_state tss{};
  tss.arg0 = fakeDevice();
  tss.stage = 7u;
  tss.type = 24u;
  tss.value = 0xaabbccddu;
  auto* decodedTss =
      dxmt9::util::marshal::decodeOpaque<
          dxmt9::bridge::Args_dxmt9c_device_set_texture_stage_state>(&tss);
  checkEq(decodedTss->stage, 7u,
          "generated TSS args preserve texture stage");
  checkEq(decodedTss->type, 24u,
          "generated TSS args preserve state type");
  checkEq(decodedTss->value, 0xaabbccddu,
          "generated TSS args preserve state value");
  decodedTss->ret = 0x02030405;
  checkEq(tss.ret, 0x02030405,
          "generated TSS args preserve returned status");

  dxmt9::bridge::Args_dxmt9c_device_set_vs_const_f constants{};
  constants.arg0 = fakeDevice();
  constants.start = 240u;
  constants.data = kConstPayload.data();
  constants.count = 2u;
  auto* decodedConstants =
      dxmt9::util::marshal::decodeOpaque<
          dxmt9::bridge::Args_dxmt9c_device_set_vs_const_f>(&constants);
  checkEq(decodedConstants->start, 240u,
          "generated const args preserve start register");
  checkSamePtr(decodedConstants->data, kConstPayload.data(),
               "generated const args preserve data pointer");
  checkEq(decodedConstants->count, 2u,
          "generated const args preserve vector count");
  check(std::memcmp(decodedConstants->data, kConstPayload.data(),
                    sizeof(float) * kConstPayload.size()) == 0,
        "generated const args preserve exact pointed float bytes");
  decodedConstants->ret = 0x03040506;
  checkEq(constants.ret, 0x03040506,
          "generated const args preserve returned status");
}

void testRepresentativeGeneratedFactoryArgsPreserveValues() {
  dxmt9::bridge::Args_dxmt9c_factory_check_device_type checkType{};
  checkType.arg0 = fakeFactory();
  checkType.adapter = 3u;
  checkType.devType = 1u;
  checkType.adapterFmt = 21u;
  checkType.backFmt = 22u;
  checkType.windowed = 1u;
  auto* decodedType =
      dxmt9::util::marshal::decodeOpaque<
          dxmt9::bridge::Args_dxmt9c_factory_check_device_type>(&checkType);
  checkSamePtr(decodedType->arg0, fakeFactory(),
               "factory check_device_type preserves factory pointer");
  checkEq(decodedType->adapter, 3u,
          "factory check_device_type preserves adapter");
  checkEq(decodedType->devType, 1u,
          "factory check_device_type preserves device type");
  checkEq(decodedType->adapterFmt, 21u,
          "factory check_device_type preserves adapter format");
  checkEq(decodedType->backFmt, 22u,
          "factory check_device_type preserves back-buffer format");
  checkEq(decodedType->windowed, 1u,
          "factory check_device_type preserves windowed flag");
  decodedType->ret = 0x04050607;
  checkEq(checkType.ret, 0x04050607,
          "factory check_device_type preserves returned status");

  dxmt9::bridge::Args_dxmt9c_factory_check_device_format2 checkFormat{};
  checkFormat.arg0 = fakeFactory();
  checkFormat.adapter = 4u;
  checkFormat.fmt = 23u;
  checkFormat.usage = 0x00000401u;
  checkFormat.resourceType = 3u;
  auto* decodedFormat =
      dxmt9::util::marshal::decodeOpaque<
          dxmt9::bridge::Args_dxmt9c_factory_check_device_format2>(&checkFormat);
  checkEq(decodedFormat->adapter, 4u,
          "factory check_device_format2 preserves adapter");
  checkEq(decodedFormat->fmt, 23u,
          "factory check_device_format2 preserves format");
  checkEq(decodedFormat->usage, 0x00000401u,
          "factory check_device_format2 preserves usage bits");
  checkEq(decodedFormat->resourceType, 3u,
          "factory check_device_format2 preserves resource type");
  decodedFormat->ret = 0x05060708;
  checkEq(checkFormat.ret, 0x05060708,
          "factory check_device_format2 preserves returned status");
}

void testWow64GeneratedArgsUseHandleAndPointerHelpers() {
  D9CDevice* device = fakeDevice();
  const auto deviceToken = dxmt9::util::marshal::wow64::encodeHandle(device);
  check(deviceToken != 0u, "wow64 test device token allocated");

  dxmt9::bridge::Args32_dxmt9c_device_commit_chunk commit{};
  commit.arg0 = deviceToken;
  commit.arg1 = 0x00c0ffeeu;
  auto* decodedCommit =
      dxmt9::util::marshal::decodeOpaque<
          dxmt9::bridge::Args32_dxmt9c_device_commit_chunk>(&commit);
  checkSamePtr(dxmt9::util::marshal::wow64::decodeHandle<D9CDevice*>(
                   decodedCommit->arg0),
               device, "wow64 commit args decode device token");
  checkSamePtr(dxmt9::util::marshal::wow64::decodePtr<const D9CCommandChunk*>(
                   decodedCommit->arg1),
               reinterpret_cast<const D9CCommandChunk*>(
                   static_cast<std::uintptr_t>(0x00c0ffeeu)),
               "wow64 commit args decode client chunk pointer value");
  decodedCommit->ret = 0x06070809;
  checkEq(commit.ret, 0x06070809,
          "wow64 commit args preserve returned status field");

  dxmt9::bridge::Args32_dxmt9c_device_set_vs_const_f constants{};
  constants.arg0 = deviceToken;
  constants.start = 240u;
  constants.data = 0x00abc000u;
  constants.count = 2u;
  auto* decodedConstants =
      dxmt9::util::marshal::decodeOpaque<
          dxmt9::bridge::Args32_dxmt9c_device_set_vs_const_f>(&constants);
  checkEq(decodedConstants->start, 240u,
          "wow64 const args preserve start register");
  checkSamePtr(dxmt9::util::marshal::wow64::decodePtr<const float*>(
                   decodedConstants->data),
               reinterpret_cast<const float*>(
                   static_cast<std::uintptr_t>(0x00abc000u)),
               "wow64 const args decode data pointer value");
  checkEq(decodedConstants->count, 2u,
          "wow64 const args preserve vector count");

  check(dxmt9::util::marshal::wow64::releaseHandle(deviceToken),
        "wow64 test device token released");
  checkSamePtr(dxmt9::util::marshal::wow64::decodeHandle<D9CDevice*>(
                   deviceToken),
               nullptr, "wow64 released device token no longer decodes");
}

void testDecodeOpaqueRejectsMissingArgumentBlock() {
  CommitChunkCapture capture;
  checkEq(captureCommitChunk(nullptr, 0, capture),
          DXMT9_STATUS_INVALID_PARAMETER,
          "generated-style commit sink rejects null opaque args");
  check(!capture.seen, "null opaque args do not produce a capture");
}

}  // namespace

static_assert(std::is_standard_layout_v<
              dxmt9::bridge::Args_dxmt9c_device_commit_chunk>);
static_assert(std::is_trivially_copyable_v<
              dxmt9::bridge::Args_dxmt9c_device_commit_chunk>);
static_assert(offsetof(dxmt9::bridge::Args_dxmt9c_device_commit_chunk, arg0) ==
              0u);
static_assert(offsetof(dxmt9::bridge::Args_dxmt9c_device_commit_chunk, arg1) ==
              sizeof(void*));
static_assert(offsetof(dxmt9::bridge::Args_dxmt9c_device_commit_chunk, ret) ==
              sizeof(void*) * 2u);
static_assert(offsetof(dxmt9::bridge::Args32_dxmt9c_device_commit_chunk, arg0) ==
              0u);
static_assert(offsetof(dxmt9::bridge::Args32_dxmt9c_device_commit_chunk, arg1) ==
              4u);
static_assert(offsetof(dxmt9::bridge::Args32_dxmt9c_device_commit_chunk, ret) ==
              8u);

int main() {
  try {
    testGeneratedArgumentStructLayouts();
    testDecodeOpaqueRejectsMissingArgumentBlock();
    testCommitChunkArgsPreservePointerAndWireValues();
    testRepresentativeGeneratedStateArgsPreserveValues();
    testRepresentativeGeneratedFactoryArgsPreserveValues();
    testWow64GeneratedArgsUseHandleAndPointerHelpers();
  } catch (const TestFailure& e) {
    std::cerr << "bridge_marshalling_value_spec failed: " << e.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& e) {
    std::cerr << "bridge_marshalling_value_spec unexpected exception: "
              << e.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "bridge_marshalling_value_spec passed\n";
  return EXIT_SUCCESS;
}
