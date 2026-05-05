#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include "dxmt9/core.hpp"
#include "dxmt9/device_c.h"

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

template <typename A, typename B>
void checkEq(const A& left, const B& right, std::string_view message) {
  if (!(left == right)) {
    std::ostringstream out;
    out << message << " (" << left << " vs " << right << ")";
    fail(out.str());
  }
}

template <typename T>
void checkTriviallyCopyable(std::string_view name) {
  if constexpr (!std::is_trivially_copyable_v<T>) {
    fail(std::string(name) + " must remain trivially copyable");
  }
}

template <typename T>
void checkStandardLayout(std::string_view name) {
  if constexpr (!std::is_standard_layout_v<T>) {
    fail(std::string(name) + " must remain standard layout");
  }
}

template <typename T>
void checkPodWireShape(std::string_view name) {
  checkTriviallyCopyable<T>(name);
  checkStandardLayout<T>(name);
}

std::size_t expectedSetConstBytes(std::size_t elemSize, std::uint32_t count) {
  return sizeof(D9CCommandRecordSetConst) + elemSize * count;
}

std::size_t expectedClearBytes(std::uint32_t rectCount) {
  return sizeof(D9CCommandRecordClear) + sizeof(D9CRect) * rectCount;
}

void testWireRecordsStayPod() {
  checkTriviallyCopyable<D9CDrawPrimitivePacket>("D9CDrawPrimitivePacket");
  checkTriviallyCopyable<D9CDrawIndexedPrimitivePacket>("D9CDrawIndexedPrimitivePacket");
  checkPodWireShape<D9CCommandChunkWireHeader>("D9CCommandChunkWireHeader");
  checkPodWireShape<D9CCommandChunkWireRecordHeader>("D9CCommandChunkWireRecordHeader");
  checkPodWireShape<D9CCommandChunkWireHandleEntry>("D9CCommandChunkWireHandleEntry");
  checkTriviallyCopyable<D9CCommandRecordHeader>("D9CCommandRecordHeader");
  checkTriviallyCopyable<D9CCommandRecordDrawPrimitive>("D9CCommandRecordDrawPrimitive");
  checkTriviallyCopyable<D9CCommandRecordDrawIndexedPrimitive>("D9CCommandRecordDrawIndexedPrimitive");
  checkTriviallyCopyable<D9CCommandRecordSetConst>("D9CCommandRecordSetConst");
  checkTriviallyCopyable<D9CCommandRecordClear>("D9CCommandRecordClear");
  checkTriviallyCopyable<D9CCommandRecordPresent>("D9CCommandRecordPresent");
  checkTriviallyCopyable<D9CCommandRecordApplyState>("D9CCommandRecordApplyState");
  checkTriviallyCopyable<D9CCommandChunk>("D9CCommandChunk");
  checkTriviallyCopyable<D9CChunkHandleEntry>("D9CChunkHandleEntry");
}

void testRecordHeaderLayout() {
  checkEq(D9C_COMMAND_CHUNK_VERSION, 1u, "command chunk ABI version");
  checkEq(D9C_COMMAND_CHUNK_WIRE_VERSION, 1u, "DOD command chunk ABI version");
  checkEq(offsetof(D9CCommandRecordDrawPrimitive, header), std::size_t{0}, "draw primitive header offset");
  checkEq(offsetof(D9CCommandRecordDrawIndexedPrimitive, header), std::size_t{0}, "draw indexed header offset");
  checkEq(offsetof(D9CCommandRecordClear, header), std::size_t{0}, "clear header offset");
  checkEq(offsetof(D9CCommandRecordPresent, header), std::size_t{0}, "present header offset");
  checkEq(offsetof(D9CCommandRecordApplyState, header), std::size_t{0}, "apply state header offset");
  check(sizeof(D9CCommandRecordHeader) <= sizeof(D9CCommandRecordDrawPrimitive),
        "fixed records contain the common header");
}

void testDodWireChunkLayout() {
  checkEq(sizeof(D9CCommandChunkWireHeader),
          static_cast<std::size_t>(D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE),
          "DOD chunk header byte size");
  checkEq(sizeof(D9CCommandChunkWireRecordHeader),
          static_cast<std::size_t>(D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE),
          "DOD record header byte size");
  checkEq(sizeof(D9CCommandChunkWireHandleEntry),
          static_cast<std::size_t>(D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE),
          "DOD handle entry byte size");

  checkEq(offsetof(D9CCommandChunkWireHeader, version), std::size_t{0},
          "DOD chunk header version offset");
  checkEq(offsetof(D9CCommandChunkWireHeader, recordTableOffset), std::size_t{16},
          "DOD chunk header record table offset");
  checkEq(offsetof(D9CCommandChunkWireHeader, handleTableOffset), std::size_t{24},
          "DOD chunk header handle table offset");
  checkEq(offsetof(D9CCommandChunkWireHeader, payloadArenaOffset), std::size_t{32},
          "DOD chunk header payload arena offset");

  checkEq(offsetof(D9CCommandChunkWireRecordHeader, payloadOffset), std::size_t{8},
          "DOD record payload offset field offset");
  checkEq(offsetof(D9CCommandChunkWireRecordHeader, payloadSize), std::size_t{12},
          "DOD record payload size field offset");
  checkEq(offsetof(D9CCommandChunkWireRecordHeader, firstHandle), std::size_t{16},
          "DOD record first handle field offset");
  checkEq(offsetof(D9CCommandChunkWireRecordHeader, handleCount), std::size_t{20},
          "DOD record handle count field offset");

  checkEq(offsetof(D9CCommandChunkWireHandleEntry, kind), std::size_t{0},
          "DOD handle kind field offset");
  checkEq(offsetof(D9CCommandChunkWireHandleEntry, generation), std::size_t{4},
          "DOD handle generation field offset");
  checkEq(offsetof(D9CCommandChunkWireHandleEntry, opaqueHandle), std::size_t{8},
          "DOD opaque handle field offset");
  checkEq(offsetof(D9CCommandChunkWireHandleEntry, reserved0), std::size_t{16},
          "DOD handle reserved field offset");
}

void testDodWireDefaultsAndPayloadRanges() {
  D9CCommandChunkWireHeader chunk{};
  checkEq(chunk.reserved0, 0u, "DOD chunk reserved0 defaults to zero");
  checkEq(chunk.reserved1, 0u, "DOD chunk reserved1 defaults to zero");

  D9CCommandChunkWireRecordHeader record{};
  checkEq(record.flags, D9C_COMMAND_CHUNK_WIRE_RECORD_FLAG_NONE,
          "DOD record flags default to none");
  checkEq(record.reserved0, 0u, "DOD record reserved0 defaults to zero");
  checkEq(record.reserved1, 0u, "DOD record reserved1 defaults to zero");

  D9CCommandChunkWireHandleEntry handle{};
  checkEq(handle.generation, D9C_COMMAND_CHUNK_WIRE_HANDLE_GENERATION_NONE,
          "DOD handle generation defaults to none");
  checkEq(handle.reserved0, 0u, "DOD handle reserved0 defaults to zero");
  checkEq(handle.reserved1, 0u, "DOD handle reserved1 defaults to zero");

  check(d9c_command_chunk_wire_payload_range_valid(16u, 0u, 16u),
        "DOD payload helper accepts full arena");
  check(d9c_command_chunk_wire_payload_range_valid(16u, 8u, 8u),
        "DOD payload helper accepts tail range");
  check(!d9c_command_chunk_wire_payload_range_valid(16u, 17u, 0u),
        "DOD payload helper rejects offset past arena");
  check(!d9c_command_chunk_wire_payload_range_valid(16u, 12u, 5u),
        "DOD payload helper rejects range past arena");
  check(!d9c_command_chunk_wire_payload_range_valid(0xffffffffu, 0xfffffff0u, 0x20u),
        "DOD payload helper rejects overflowing range");
}

void testVariableRecordSizes() {
  checkEq(expectedSetConstBytes(sizeof(float) * 4u, 3u),
          sizeof(D9CCommandRecordSetConst) + sizeof(float) * 12u,
          "float constant record byte size");
  checkEq(expectedSetConstBytes(sizeof(std::int32_t) * 4u, 2u),
          sizeof(D9CCommandRecordSetConst) + sizeof(std::int32_t) * 8u,
          "int constant record byte size");
  checkEq(expectedSetConstBytes(sizeof(std::uint32_t), 5u),
          sizeof(D9CCommandRecordSetConst) + sizeof(std::uint32_t) * 5u,
          "bool constant record byte size");
  checkEq(expectedClearBytes(0u), sizeof(D9CCommandRecordClear), "full-target clear size");
  checkEq(expectedClearBytes(2u), sizeof(D9CCommandRecordClear) + sizeof(D9CRect) * 2u,
          "rect clear size");
}

void testHandleKindCompatibility() {
  using dxmt9::core::ChunkHandleKind;
  checkEq(D9C_CHUNK_HANDLE_KIND_TEXTURE, static_cast<std::uint32_t>(ChunkHandleKind::Texture),
          "texture handle kind wire value");
  checkEq(D9C_CHUNK_HANDLE_KIND_SURFACE, static_cast<std::uint32_t>(ChunkHandleKind::Surface),
          "surface handle kind wire value");
  checkEq(D9C_CHUNK_HANDLE_KIND_BUFFER, static_cast<std::uint32_t>(ChunkHandleKind::Buffer),
          "buffer handle kind wire value");
  checkEq(D9C_CHUNK_HANDLE_KIND_SHADER, static_cast<std::uint32_t>(ChunkHandleKind::Shader),
          "shader handle kind wire value");
  checkEq(D9C_CHUNK_HANDLE_KIND_VERTEX_DECL, static_cast<std::uint32_t>(ChunkHandleKind::VertexDecl),
          "vertex declaration handle kind wire value");

  D9CCommandChunkWireHandleEntry entry{};
  entry.kind = D9C_CHUNK_HANDLE_KIND_SHADER;
  checkEq(entry.kind, static_cast<std::uint32_t>(ChunkHandleKind::Shader),
          "DOD handle entry uses legacy chunk handle kind mapping");
}

void testDrawPacketDeltaDefaults() {
  D9CDrawPrimitivePacket packet{};
  checkEq(packet.renderStateCount, 0u, "default packet has no render-state delta");
  checkEq(packet.textureMask, 0u, "default packet has no texture delta");
  checkEq(packet.streamSourceMask, 0u, "default packet has no stream delta");
  checkEq(packet.fvfValid, 0u, "default packet has no FVF delta");
  checkEq(packet.vsValid, 0u, "default packet has no VS delta");
  checkEq(packet.psValid, 0u, "default packet has no PS delta");
  checkEq(packet.rtMask, 0u, "default packet has no RT delta");
  checkEq(packet.dsValid, 0u, "default packet has no DS delta");
  checkEq(packet.tssCount, 0u, "default packet has no TSS delta");
  checkEq(packet.samplerStateCount, 0u, "default packet has no sampler delta");
  checkEq(packet.transformCount, 0u, "default packet has no transform delta");
}

}  // namespace

int main() {
  try {
    testWireRecordsStayPod();
    testRecordHeaderLayout();
    testDodWireChunkLayout();
    testDodWireDefaultsAndPayloadRanges();
    testVariableRecordSizes();
    testHandleKindCompatibility();
    testDrawPacketDeltaDefaults();
  } catch (const TestFailure& e) {
    std::cerr << "chunk_record_spec failed: " << e.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
