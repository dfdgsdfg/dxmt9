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
  const D9CCommandChunkWireSetConst fixed{240u, 2u};
  std::vector<std::uint8_t> payload;
  appendObject(payload, fixed);
  appendBytes(payload, kConstPayload.data(), sizeof(kConstPayload));
  return payload;
}

std::vector<std::uint8_t> makeDrawRecordPayload() {
  const D9CCommandChunkWireDrawHeader draw{
      .primitiveType = 4u,
      .startVertex = 0x1234u,
      .primitiveCount = 0x56u,
      .sectionCount = 1u,
      .sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader),
      .sectionPayloadOffset = sizeof(D9CCommandChunkWireDrawHeader) +
                              sizeof(D9CCommandChunkWireSectionDesc),
  };
  const D9CCommandChunkWireTextureBinding texture{
      .slot = 3u,
      .valid = 1u,
      .handleIndex = 0u,
  };
  const D9CCommandChunkWireSectionDesc section{
      .kind = D9C_COMMAND_CHUNK_SECTION_TEXTURE,
      .elementSize = sizeof(texture),
      .count = 1u,
      .payloadOffset = draw.sectionPayloadOffset,
      .byteSize = sizeof(texture),
  };
  std::vector<std::uint8_t> payload;
  appendObject(payload, draw);
  appendObject(payload, section);
  appendObject(payload, texture);
  return payload;
}

WireBlobFixture makeWireBlobFixture() {
  WireBlobFixture fixture;
  const auto constPayload = makeConstRecordPayload();
  const auto drawPayload = makeDrawRecordPayload();
  const D9CCommandChunkWirePresent present{
      .hwnd = 0x1122334455667788ull,
      .flags = 0x01020304u,
      .hasSrc = 1u,
      .hasDst = 1u,
      .src = D9CRect{1, 2, 640, 480},
      .dst = D9CRect{10, 20, 650, 500},
  };

  std::vector<std::uint8_t> arena;
  fixture.constPayloadOffset = static_cast<std::uint32_t>(arena.size());
  appendBytes(arena, constPayload.data(), constPayload.size());
  fixture.constPayloadSize = static_cast<std::uint32_t>(constPayload.size());
  fixture.drawPayloadOffset = static_cast<std::uint32_t>(arena.size());
  appendBytes(arena, drawPayload.data(), drawPayload.size());
  fixture.presentPayloadOffset = static_cast<std::uint32_t>(arena.size());
  appendObject(arena, present);

  const std::array records{
      D9CCommandChunkWireRecordHeader{
          D9C_COMMAND_RECORD_SET_VS_CONST_F,
          D9C_COMMAND_CHUNK_RECORD_FLAG_NONE,
          fixture.constPayloadOffset,
          fixture.constPayloadSize,
          0u, 0u, 0u, 0u,
      },
      D9CCommandChunkWireRecordHeader{
          D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
          D9C_COMMAND_CHUNK_RECORD_FLAG_NONE,
          fixture.drawPayloadOffset,
          static_cast<std::uint32_t>(drawPayload.size()),
          0u, 1u, 0u, 0u,
      },
      D9CCommandChunkWireRecordHeader{
          D9C_COMMAND_RECORD_PRESENT,
          D9C_COMMAND_CHUNK_RECORD_FLAG_NONE,
          fixture.presentPayloadOffset,
          static_cast<std::uint32_t>(sizeof(present)),
          1u, 0u, 0u, 0u,
      },
  };
  const std::array handles{
      D9CCommandChunkWireHandleEntry{
          D9C_CHUNK_HANDLE_KIND_TEXTURE,
          0x11u,
          0x000000020000cafeull,
      },
  };

  fixture.recordTableOffset = sizeof(D9CCommandChunkWireHeader);
  fixture.handleTableOffset = fixture.recordTableOffset + sizeof(records);
  fixture.payloadArenaOffset = static_cast<std::uint32_t>(
      alignUp(fixture.handleTableOffset + sizeof(handles), 8u));
  const D9CCommandChunkWireHeader header{
      D9C_COMMAND_CHUNK_WIRE_VERSION,
      D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE,
      D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE,
      fixture.recordTableOffset,
      static_cast<std::uint32_t>(records.size()),
      fixture.handleTableOffset,
      static_cast<std::uint32_t>(handles.size()),
      fixture.payloadArenaOffset,
      static_cast<std::uint32_t>(arena.size()),
      0u, 0u,
  };

  fixture.blob.resize(fixture.payloadArenaOffset + arena.size());
  writeObject(fixture.blob, 0u, header);
  std::memcpy(fixture.blob.data() + fixture.recordTableOffset,
              records.data(), sizeof(records));
  std::memcpy(fixture.blob.data() + fixture.handleTableOffset,
              handles.data(), sizeof(handles));
  std::memcpy(fixture.blob.data() + fixture.payloadArenaOffset,
              arena.data(), arena.size());
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
  auto* args = dxmt9::util::marshal::decodeOpaque<
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
  D9CCommandChunk chunk{};
  chunk.version = D9C_COMMAND_CHUNK_VERSION;
  chunk.recordCount = 3u;
  chunk.recordBytes = static_cast<std::uint32_t>(fixture.blob.size());
  chunk.records = toWireHandle(fixture.blob.data());
  chunk.handleCount = 1u;

  dxmt9::bridge::Args_dxmt9c_device_commit_chunk args{};
  args.arg0 = fakeDevice();
  args.arg1 = &chunk;
  CommitChunkCapture capture;
  checkEq(captureCommitChunk(&args, 0x13572468, capture),
          DXMT9_STATUS_SUCCESS, "commit chunk args decode");
  check(capture.seen, "commit chunk capture is populated");
  checkSamePtr(capture.device, fakeDevice(), "commit device pointer survives");
  checkSamePtr(capture.chunk, &chunk, "commit chunk pointer survives");
  checkEq(args.ret, 0x13572468, "commit status survives");
  checkEq(capture.chunk->version, D9C_COMMAND_CHUNK_VERSION,
          "commit advertises only canonical");
  checkSamePtr(wireHandlePtr(capture.chunk->records), fixture.blob.data(),
               "commit preserves canonical blob pointer");
  checkEq(wireHandleValue(capture.chunk->handles), 0ull,
          "retired external handle pointer stays empty");

  const auto header = readObject<D9CCommandChunkWireHeader>(fixture.blob, 0u);
  checkEq(header.version, D9C_COMMAND_CHUNK_WIRE_VERSION,
          "fixture uses the canonical wire version");
  checkEq(header.recordTableOffset, fixture.recordTableOffset,
          "canonical record table offset survives marshalling");
  const auto draw = readObject<D9CCommandChunkWireRecordHeader>(
      fixture.blob,
      fixture.recordTableOffset + sizeof(D9CCommandChunkWireRecordHeader));
  checkEq(draw.type, static_cast<std::uint32_t>(D9C_COMMAND_RECORD_DRAW_PRIMITIVE),
          "canonical draw record survives marshalling");
  checkEq(draw.handleCount, 1u, "canonical draw handle slice survives marshalling");
  const auto handle = readObject<D9CCommandChunkWireHandleEntry>(
      fixture.blob, fixture.handleTableOffset);
  checkEq(handle.objectId, 0x000000020000cafeull,
          "canonical stable object identity survives marshalling");
  const auto fixed = readObject<D9CCommandChunkWireSetConst>(
      fixture.blob, fixture.payloadArenaOffset + fixture.constPayloadOffset);
  checkEq(fixed.startRegister, 240u, "canonical const start survives marshalling");
  const auto dataOffset = fixture.payloadArenaOffset +
                          fixture.constPayloadOffset + sizeof(fixed);
  check(std::memcmp(fixture.blob.data() + dataOffset, kConstPayload.data(),
                    sizeof(kConstPayload)) == 0,
        "canonical const bytes survive marshalling");
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

void testCommandChunkGeneratedArgumentLayouts() {
  checkPodArgShape<
      dxmt9::bridge::Args_dxmt9c_device_negotiate_command_chunk>(
      "native canonical negotiation args", 24u, 8u);
  checkPodArgShape<
      dxmt9::bridge::Args32_dxmt9c_device_negotiate_command_chunk>(
      "wow64 canonical negotiation args", 12u, 4u);
  checkPodArgShape<
      dxmt9::bridge::Args_dxmt9c_texture_get_wire_identity>(
      "native canonical texture identity args", 24u, 8u);
  checkPodArgShape<
      dxmt9::bridge::Args32_dxmt9c_texture_get_wire_identity>(
      "wow64 canonical texture identity args", 12u, 4u);
  check(std::is_standard_layout_v<dxmt9::bridge::
            Args_dxmt9c_device_capture_render_tape_d24x8_snapshot> &&
            std::is_trivially_copyable_v<dxmt9::bridge::
                Args_dxmt9c_device_capture_render_tape_d24x8_snapshot>,
        "native D24X8 snapshot arguments are pointer-only POD");
  check(std::is_standard_layout_v<dxmt9::bridge::
            Args32_dxmt9c_device_capture_render_tape_d24x8_snapshot> &&
            std::is_trivially_copyable_v<dxmt9::bridge::
                Args32_dxmt9c_device_capture_render_tape_d24x8_snapshot>,
        "wow64 D24X8 snapshot arguments are pointer-token POD");

  D9CCommandChunkNegotiation negotiation{
      .peSupportedVersions = D9C_COMMAND_CHUNK_CAP_CURRENT,
      .pePreferredVersion = D9C_COMMAND_CHUNK_VERSION,
  };
  dxmt9::bridge::Args_dxmt9c_device_negotiate_command_chunk args{};
  args.arg0 = reinterpret_cast<D9CDevice*>(std::uintptr_t{0x12340000u});
  args.arg1 = &negotiation;
  checkSamePtr(args.arg1, &negotiation,
               "native canonical negotiation preserves POD pointer");

  dxmt9::bridge::Args32_dxmt9c_texture_get_wire_identity wow64{};
  wow64.arg0 = 0x01020304u;
  wow64.out = 0x00abc000u;
  checkSamePtr(dxmt9::util::marshal::wow64::decodePtr<
                   D9CWireObjectIdentity*>(wow64.out),
               reinterpret_cast<D9CWireObjectIdentity*>(
                   std::uintptr_t{0x00abc000u}),
               "wow64 canonical identity decodes output pointer value");

  dxmt9::bridge::Args32_dxmt9c_device_capture_render_tape_d24x8_snapshot
      snapshot{};
  snapshot.arg0 = 0x11110000u;
  snapshot.request = 0x22220000u;
  snapshot.out = 0x33330000u;
  snapshot.bytes = 0x44440000u;
  snapshot.capacity = 0x0000000100001000ull;
  checkSamePtr(dxmt9::util::marshal::wow64::decodePtr<
                   const D9CRenderTapeD24X8SnapshotRequest*>(snapshot.request),
               reinterpret_cast<const D9CRenderTapeD24X8SnapshotRequest*>(
                   std::uintptr_t{0x22220000u}),
               "wow64 D24X8 snapshot decodes top-level request pointer");
  checkSamePtr(dxmt9::util::marshal::wow64::decodePtr<void*>(snapshot.bytes),
               reinterpret_cast<void*>(std::uintptr_t{0x44440000u}),
               "wow64 D24X8 snapshot decodes top-level byte buffer pointer");
  checkEq(snapshot.capacity, 0x0000000100001000ull,
          "wow64 D24X8 snapshot preserves 64-bit capacity");
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
    testCommandChunkGeneratedArgumentLayouts();
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
