#include "chunk_record_import_spec_fixtures.hpp"
#include "d3d9_pe_draw_packet.hpp"
#include "d3d9_pe_const_shadow.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <vector>

namespace {

using namespace dxmt9::d3d9::devicec::spec;
using dxmt9::d3d9::devicec::ImportedRecordView;

// Native bridge tests cannot instantiate src/d3d9/d3d9_pe_device.cpp:
// the PE device is built only for Windows targets and pulls in windows.h/d3d9.h.
// Attachment packet production is factored into a native-safe helper so this
// spec can cover the PE live-shadow -> packet boundary for RT/DS deltas.

constexpr std::uint32_t kD3dptTriangleList = 4u;

constexpr std::uint64_t kTexture0 = 0x0102030405060708ull;
constexpr std::uint64_t kTexture15 = 0x1112131415161718ull;
constexpr std::uint64_t kStream0 = 0x2122232425262728ull;
constexpr std::uint64_t kStream15 = 0x3132333435363738ull;
constexpr std::uint64_t kVs = 0x4142434445464748ull;
constexpr std::uint64_t kPs = 0x5152535455565758ull;
constexpr std::uint64_t kVdecl = 0x6162636465666768ull;
constexpr std::uint64_t kRt0 = 0x7172737475767778ull;
constexpr std::uint64_t kRt3 = 0x8182838485868788ull;
constexpr std::uint64_t kDs = 0x9192939495969798ull;

std::uint64_t wireValue(const D9CWireHandle& handle) {
  return static_cast<std::uint64_t>(handle.lo) |
         (static_cast<std::uint64_t>(handle.hi) << 32);
}

void checkWire(const D9CWireHandle& handle,
               std::uint64_t expected,
               std::string_view message) {
  checkEq(wireValue(handle), expected, message);
}

D9CMatrix makeMatrix(float base) {
  D9CMatrix matrix{};
  for (std::uint32_t i = 0; i < 16u; ++i) {
    matrix.m[i] = base + static_cast<float>(i) * 0.25f;
  }
  return matrix;
}

D9CLight makeLight(std::uint32_t type, float base) {
  D9CLight light{};
  light.type = type;
  light.diffuse = D9CColorRGBA{base + 0.1f, base + 0.2f, base + 0.3f, base + 0.4f};
  light.specular = D9CColorRGBA{base + 1.1f, base + 1.2f, base + 1.3f, base + 1.4f};
  light.ambient = D9CColorRGBA{base + 2.1f, base + 2.2f, base + 2.3f, base + 2.4f};
  light.position[0] = base + 3.1f;
  light.position[1] = base + 3.2f;
  light.position[2] = base + 3.3f;
  light.direction[0] = -(base + 4.1f);
  light.direction[1] = -(base + 4.2f);
  light.direction[2] = -(base + 4.3f);
  light.range = base + 5.1f;
  light.falloff = base + 5.2f;
  light.attenuation0 = base + 5.3f;
  light.attenuation1 = base + 5.4f;
  light.attenuation2 = base + 5.5f;
  light.theta = base + 5.6f;
  light.phi = base + 5.7f;
  return light;
}

D9CCommandRecordDrawPrimitive makeRichDrawRecord() {
  D9CCommandRecordDrawPrimitive draw{};
  draw.header.type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE;
  draw.header.size = sizeof(draw);
  auto& packet = draw.packet;

  packet.renderStateCount = 3u;
  packet.renderStates[0] = D9CDrawPacketRenderState{7u, 0xffffffffu};
  packet.renderStates[1] = D9CDrawPacketRenderState{22u, 0u};
  packet.renderStates[2] = D9CDrawPacketRenderState{206u, 0x13579bdfu};

  packet.textureMask = (1u << 0u) | (1u << 15u);
  packet.textures[0] = wireHandle(kTexture0);
  packet.textures[15] = wireHandle(kTexture15);

  packet.streamSourceMask = (1u << 0u) | (1u << 15u);
  packet.streamSources[0].buffer = wireHandle(kStream0);
  packet.streamSources[0].offset = 0u;
  packet.streamSources[0].stride = 12u;
  packet.streamSources[15].buffer = wireHandle(kStream15);
  packet.streamSources[15].offset = 0xfffffff0u;
  packet.streamSources[15].stride = 0x400u;

  packet.fvfValid = 1u;
  packet.fvf = 0x11223344u;
  packet.vsValid = 1u;
  packet.vsHandle = wireHandle(kVs);
  packet.psValid = 1u;
  packet.psHandle = wireHandle(kPs);
  packet.vdeclValid = 1u;
  packet.vdeclHandle = wireHandle(kVdecl);

  packet.rtMask = (1u << 0u) | (1u << 3u);
  packet.rtHandles[0] = wireHandle(kRt0);
  packet.rtHandles[3] = wireHandle(kRt3);
  packet.dsValid = 1u;
  packet.dsHandle = wireHandle(kDs);

  packet.viewportValid = 1u;
  packet.viewport = D9CViewport{11u, 13u, 1920u, 1080u, 0.125f, 0.875f};
  packet.scissorValid = 1u;
  packet.scissor = D9CRect{-17, 19, 2047, 4095};

  packet.tssCount = 2u;
  packet.tss[0] = D9CDrawPacketTextureStageState{0u, 1u, 0x2468ace0u};
  packet.tss[1] = D9CDrawPacketTextureStageState{7u, 32u, 0x10203040u};

  packet.samplerStateCount = 2u;
  packet.samplerStates[0] = D9CDrawPacketSamplerState{0u, 5u, 2u};
  packet.samplerStates[1] = D9CDrawPacketSamplerState{15u, 13u, 0xffffffffu};

  packet.materialValid = 1u;
  packet.material.diffuse = D9CColorRGBA{0.1f, 0.2f, 0.3f, 0.4f};
  packet.material.ambient = D9CColorRGBA{1.1f, 1.2f, 1.3f, 1.4f};
  packet.material.specular = D9CColorRGBA{2.1f, 2.2f, 2.3f, 2.4f};
  packet.material.emissive = D9CColorRGBA{3.1f, 3.2f, 3.3f, 3.4f};
  packet.material.power = 128.5f;

  packet.clipPlaneMask = (1u << 0u) | (1u << 5u);
  const std::array<float, 4> clip0{1.0f, -2.0f, 3.5f, -4.5f};
  const std::array<float, 4> clip5{-5.0f, 6.0f, -7.0f, 8.0f};
  std::memcpy(&packet.clipPlanes[0], clip0.data(), sizeof(float) * clip0.size());
  std::memcpy(&packet.clipPlanes[5u * 4u], clip5.data(), sizeof(float) * clip5.size());

  packet.transformCount = 2u;
  packet.transforms[0].state = 2u;
  packet.transforms[0].reserved = 0u;
  packet.transforms[0].matrix = makeMatrix(10.0f);
  packet.transforms[1].state = 263u;
  packet.transforms[1].reserved = 0u;
  packet.transforms[1].matrix = makeMatrix(-20.0f);

  packet.lightSlotMask = (1u << 0u) | (1u << 7u);
  packet.lights[0] = makeLight(1u, 30.0f);
  packet.lights[7] = makeLight(3u, 40.0f);
  packet.lightEnableValidMask = (1u << 0u) | (1u << 7u);
  packet.lightEnableMask = (1u << 7u);

  packet.primitiveType = kD3dptTriangleList;
  packet.startVertex = std::numeric_limits<std::uint32_t>::max() - 1u;
  packet.primitiveCount = std::numeric_limits<std::uint32_t>::max();
  return draw;
}

template <typename T>
std::vector<std::uint8_t> makeSetConstRecordBytes(
    std::uint32_t type,
    std::uint32_t start,
    const std::vector<T>& values,
    std::uint32_t count) {
  D9CCommandRecordSetConst record{};
  record.header.type = type;
  record.header.size = static_cast<std::uint32_t>(
      sizeof(record) + sizeof(T) * values.size());
  record.start = start;
  record.count = count;

  std::vector<std::uint8_t> bytes(record.header.size);
  std::memcpy(bytes.data(), &record, sizeof(record));
  std::memcpy(bytes.data() + sizeof(record), values.data(),
              sizeof(T) * values.size());
  return bytes;
}

std::uint32_t appendPayload(std::vector<std::uint8_t>& arena,
                            const std::vector<std::uint8_t>& payload) {
  const auto offset = static_cast<std::uint32_t>(arena.size());
  arena.insert(arena.end(), payload.begin(), payload.end());
  return offset;
}

template <typename T>
std::uint32_t appendPayload(std::vector<std::uint8_t>& arena, const T& record) {
  return appendPayload(arena, recordBytes(record));
}

void checkSetConstRecord(const ImportedRecordView& record,
                         std::uint32_t expectedType,
                         std::uint32_t expectedStart,
                         std::uint32_t expectedCount,
                         const std::vector<std::uint8_t>& expectedTail,
                         std::string_view message) {
  check(record.valid(), std::string(message) + " record validates");
  check(record.header.size >= sizeof(D9CCommandRecordSetConst),
        std::string(message) + " has fixed header");
  D9CCommandRecordSetConst header{};
  std::memcpy(&header, record.record, sizeof(header));
  checkEq(header.header.type, expectedType, std::string(message) + " type");
  checkEq(header.header.size, record.header.size,
          std::string(message) + " header size");
  checkEq(header.start, expectedStart, std::string(message) + " start");
  checkEq(header.count, expectedCount, std::string(message) + " count");
  const auto tailBytes = record.header.size -
                         static_cast<std::uint32_t>(sizeof(D9CCommandRecordSetConst));
  checkEq(tailBytes, static_cast<std::uint32_t>(expectedTail.size()),
          std::string(message) + " tail byte count");
  check(std::memcmp(record.record + sizeof(D9CCommandRecordSetConst),
                    expectedTail.data(), expectedTail.size()) == 0,
        std::string(message) + " tail bytes");
}

void checkRichDrawPacketValues(const D9CDrawPrimitivePacket& packet) {
  check(!packetHasNoStateDelta(packet), "rich draw packet carries state delta");

  checkEq(packet.renderStateCount, 3u, "render-state count");
  checkEq(packet.renderStates[0].state, 7u, "render-state 0 key");
  checkEq(packet.renderStates[0].value, 0xffffffffu, "render-state 0 value");
  checkEq(packet.renderStates[2].state, 206u, "render-state high key");
  checkEq(packet.renderStates[2].value, 0x13579bdfu, "render-state high value");

  checkEq(packet.textureMask, (1u << 0u) | (1u << 15u), "texture mask");
  checkWire(packet.textures[0], kTexture0, "texture stage 0 handle");
  checkWire(packet.textures[15], kTexture15, "texture stage 15 handle");

  checkEq(packet.streamSourceMask, (1u << 0u) | (1u << 15u),
          "stream-source mask");
  checkWire(packet.streamSources[0].buffer, kStream0, "stream 0 handle");
  checkEq(packet.streamSources[0].offset, 0u, "stream 0 offset");
  checkEq(packet.streamSources[0].stride, 12u, "stream 0 stride");
  checkWire(packet.streamSources[15].buffer, kStream15, "stream 15 handle");
  checkEq(packet.streamSources[15].offset, 0xfffffff0u, "stream 15 offset");
  checkEq(packet.streamSources[15].stride, 0x400u, "stream 15 stride");

  checkEq(packet.fvfValid, 1u, "FVF valid");
  checkEq(packet.fvf, 0x11223344u, "FVF value");
  checkEq(packet.vsValid, 1u, "VS valid");
  checkWire(packet.vsHandle, kVs, "VS handle");
  checkEq(packet.psValid, 1u, "PS valid");
  checkWire(packet.psHandle, kPs, "PS handle");
  checkEq(packet.vdeclValid, 1u, "vdecl valid");
  checkWire(packet.vdeclHandle, kVdecl, "vdecl handle");

  checkEq(packet.rtMask, (1u << 0u) | (1u << 3u), "RT mask");
  checkWire(packet.rtHandles[0], kRt0, "RT0 handle");
  checkWire(packet.rtHandles[3], kRt3, "RT3 handle");
  checkEq(packet.dsValid, 1u, "DS valid");
  checkWire(packet.dsHandle, kDs, "DS handle");

  checkEq(packet.viewportValid, 1u, "viewport valid");
  checkEq(packet.viewport.x, 11u, "viewport X");
  checkEq(packet.viewport.y, 13u, "viewport Y");
  checkEq(packet.viewport.width, 1920u, "viewport width");
  checkEq(packet.viewport.height, 1080u, "viewport height");
  checkEq(packet.viewport.minZ, 0.125f, "viewport minZ");
  checkEq(packet.viewport.maxZ, 0.875f, "viewport maxZ");
  checkEq(packet.scissorValid, 1u, "scissor valid");
  checkEq(packet.scissor.left, -17, "scissor left");
  checkEq(packet.scissor.top, 19, "scissor top");
  checkEq(packet.scissor.right, 2047, "scissor right");
  checkEq(packet.scissor.bottom, 4095, "scissor bottom");

  checkEq(packet.tssCount, 2u, "TSS count");
  checkEq(packet.tss[0].stage, 0u, "TSS stage 0");
  checkEq(packet.tss[0].type, 1u, "TSS type 0");
  checkEq(packet.tss[0].value, 0x2468ace0u, "TSS value 0");
  checkEq(packet.tss[1].stage, 7u, "TSS stage 7");
  checkEq(packet.tss[1].type, 32u, "TSS type 7");
  checkEq(packet.tss[1].value, 0x10203040u, "TSS value 7");

  checkEq(packet.samplerStateCount, 2u, "sampler-state count");
  checkEq(packet.samplerStates[0].sampler, 0u, "sampler 0 slot");
  checkEq(packet.samplerStates[0].type, 5u, "sampler 0 type");
  checkEq(packet.samplerStates[0].value, 2u, "sampler 0 value");
  checkEq(packet.samplerStates[1].sampler, 15u, "sampler 15 slot");
  checkEq(packet.samplerStates[1].type, 13u, "sampler 15 type");
  checkEq(packet.samplerStates[1].value, 0xffffffffu, "sampler 15 value");

  checkEq(packet.materialValid, 1u, "material valid");
  checkEq(packet.material.diffuse.r, 0.1f, "material diffuse.r");
  checkEq(packet.material.ambient.g, 1.2f, "material ambient.g");
  checkEq(packet.material.specular.b, 2.3f, "material specular.b");
  checkEq(packet.material.emissive.a, 3.4f, "material emissive.a");
  checkEq(packet.material.power, 128.5f, "material power");

  checkEq(packet.clipPlaneMask, (1u << 0u) | (1u << 5u), "clip-plane mask");
  checkEq(packet.clipPlanes[0], 1.0f, "clip plane 0 x");
  checkEq(packet.clipPlanes[3], -4.5f, "clip plane 0 w");
  checkEq(packet.clipPlanes[20], -5.0f, "clip plane 5 x");
  checkEq(packet.clipPlanes[23], 8.0f, "clip plane 5 w");

  checkEq(packet.transformCount, 2u, "transform count");
  checkEq(packet.transforms[0].state, 2u, "transform 0 state");
  checkEq(packet.transforms[0].reserved, 0u, "transform 0 reserved");
  checkEq(packet.transforms[0].matrix.m[0], 10.0f, "transform 0 first value");
  checkEq(packet.transforms[0].matrix.m[15], 13.75f, "transform 0 last value");
  checkEq(packet.transforms[1].state, 263u, "transform 1 state");
  checkEq(packet.transforms[1].reserved, 0u, "transform 1 reserved");
  checkEq(packet.transforms[1].matrix.m[0], -20.0f, "transform 1 first value");
  checkEq(packet.transforms[1].matrix.m[15], -16.25f, "transform 1 last value");

  checkEq(packet.lightSlotMask, (1u << 0u) | (1u << 7u), "light slot mask");
  checkEq(packet.lights[0].type, 1u, "light 0 type");
  checkEq(packet.lights[0].diffuse.r, 30.1f, "light 0 diffuse.r");
  checkEq(packet.lights[0].direction[2], -34.3f, "light 0 direction.z");
  checkEq(packet.lights[7].type, 3u, "light 7 type");
  checkEq(packet.lights[7].range, 45.1f, "light 7 range");
  checkEq(packet.lights[7].phi, 45.7f, "light 7 phi");
  checkEq(packet.lightEnableValidMask, (1u << 0u) | (1u << 7u),
          "light enable valid mask");
  checkEq(packet.lightEnableMask, (1u << 7u), "light enable value mask");

  checkEq(packet.primitiveType, kD3dptTriangleList, "primitive type");
  checkEq(packet.startVertex, std::numeric_limits<std::uint32_t>::max() - 1u,
          "draw start vertex");
  checkEq(packet.primitiveCount, std::numeric_limits<std::uint32_t>::max(),
          "draw primitive count");

  const auto param = makeRunParam(packet);
  check(!param.indexed, "draw run param is non-indexed");
  checkEq(param.startVertex, packet.startVertex, "run param start vertex");
  checkEq(param.primitiveCount, packet.primitiveCount,
          "run param primitive count");
}

void testPeAttachmentDeltaBuilderPreservesExplicitNullsAndSlots() {
  D9CDrawPrimitivePacket packet{};
  packet.rtMask = 0xffffffffu;
  for (auto& handle : packet.rtHandles) {
    handle = wireHandle(0xffffffffffffffffull);
  }
  packet.dsValid = 0u;
  packet.dsHandle = wireHandle(0xffffffffffffffffull);

  dxmt9::d3d9::pe::PeRtWireHandles rtHandles{};
  rtHandles[0] = wireHandle(kRt0);
  rtHandles[1] = D9CWireHandle{};
  rtHandles[3] = wireHandle(kRt3);

  dxmt9::d3d9::pe::populateDrawPacketAttachmentDelta(
      packet, (1u << 0u) | (1u << 1u) | (1u << 3u) | (1u << 7u),
      rtHandles, true, D9CWireHandle{});

  checkEq(packet.rtMask, (1u << 0u) | (1u << 1u) | (1u << 3u),
          "PE delta builder masks legal RT slots");
  checkWire(packet.rtHandles[0], kRt0, "PE delta builder RT0 handle");
  checkWire(packet.rtHandles[1], 0u, "PE delta builder explicit null RT1");
  checkWire(packet.rtHandles[2], 0u, "PE delta builder clears inactive RT2");
  checkWire(packet.rtHandles[3], kRt3, "PE delta builder RT3 handle");
  checkEq(packet.dsValid, 1u, "PE delta builder explicit null DS valid");
  checkWire(packet.dsHandle, 0u, "PE delta builder explicit null DS handle");
}

void testPeAttachmentSnapshotBuilderPreservesExplicitNulls() {
  D9CDrawPrimitivePacket packet{};
  dxmt9::d3d9::pe::PeRtWireHandles rtHandles{};
  rtHandles[0] = wireHandle(kRt0);
  rtHandles[2] = D9CWireHandle{};
  rtHandles[3] = wireHandle(kRt3);

  dxmt9::d3d9::pe::PeRtExplicitMask rtExplicit{};
  rtExplicit[1] = false;
  rtExplicit[2] = true;

  dxmt9::d3d9::pe::populateDrawPacketAttachmentSnapshot(
      packet, rtHandles, rtExplicit, true, wireHandle(kDs));

  checkEq(packet.rtMask, (1u << 0u) | (1u << 2u) | (1u << 3u),
          "PE snapshot builder includes populated and explicit-null RTs");
  checkWire(packet.rtHandles[0], kRt0, "PE snapshot builder RT0 handle");
  checkWire(packet.rtHandles[1], 0u, "PE snapshot builder omits implicit null RT1");
  checkWire(packet.rtHandles[2], 0u, "PE snapshot builder keeps explicit null RT2");
  checkWire(packet.rtHandles[3], kRt3, "PE snapshot builder RT3 handle");
  checkEq(packet.dsValid, 1u, "PE snapshot builder DS valid");
  checkWire(packet.dsHandle, kDs, "PE snapshot builder DS handle");
}

void testPeDrawStreamDependencyCheckpointPreservesCoalescing() {
  using dxmt9::d3d9::pe::PeStreamSources;
  using dxmt9::d3d9::pe::populateDrawPacketStreamDependencies;

  PeStreamSources bound{};
  bound[0] = D9CDrawPacketStreamSource{
      .buffer = wireHandle(kStream0), .offset = 4u, .stride = 24u};
  bound[1] = D9CDrawPacketStreamSource{
      .buffer = wireHandle(kStream15), .offset = 8u, .stride = 32u};

  D9CDrawPrimitivePacket firstDraw{};
  populateDrawPacketStreamDependencies(firstDraw, bound, 0u);
  checkEq(firstDraw.streamSourceMask, (1u << 0u) | (1u << 1u),
          "first chunk draw checkpoints every unretained bound stream");
  checkWire(firstDraw.streamSources[0].buffer, kStream0,
            "stream checkpoint carries slot 0 buffer");
  checkEq(firstDraw.streamSources[0].offset, 4u,
          "stream checkpoint carries slot 0 offset");
  checkEq(firstDraw.streamSources[1].stride, 32u,
          "stream checkpoint carries slot 1 stride");

  D9CDrawPrimitivePacket repeatedDraw{};
  populateDrawPacketStreamDependencies(
      repeatedDraw, bound, (1u << 0u) | (1u << 1u));
  checkEq(repeatedDraw.streamSourceMask, 0u,
          "later draw omits streams already retained by serialized packets");
  check(packetHasNoStateDelta(repeatedDraw),
        "retained stream dependencies do not break later draw coalescing");

  D9CDrawPrimitivePacket pendingDetach{};
  pendingDetach.streamSourceMask = 1u << 0u;
  populateDrawPacketStreamDependencies(
      pendingDetach, PeStreamSources{}, 1u << 0u);
  checkEq(pendingDetach.streamSourceMask, 1u << 0u,
          "dependency checkpoint preserves an explicit null detach delta");
  checkWire(pendingDetach.streamSources[0].buffer, 0u,
            "explicit null detach remains null");
}

void testPeDrawIndexDependencyCheckpointPreservesCoalescing() {
  using dxmt9::d3d9::pe::populateDrawPacketIndexDependency;

  D9CDrawIndexedPrimitivePacket firstDraw{};
  firstDraw.ibHandle = wireHandle(kStream0);
  populateDrawPacketIndexDependency(firstDraw, false);
  checkEq(firstDraw.ibValid, 1u,
          "first indexed draw checkpoints an unretained index buffer");

  D9CDrawIndexedPrimitivePacket repeatedDraw{};
  repeatedDraw.ibHandle = wireHandle(kStream0);
  populateDrawPacketIndexDependency(repeatedDraw, true);
  checkEq(repeatedDraw.ibValid, 0u,
          "later indexed draw omits an already retained index buffer");

  D9CDrawIndexedPrimitivePacket pendingDetach{};
  pendingDetach.ibValid = 1u;
  populateDrawPacketIndexDependency(pendingDetach, false);
  checkEq(pendingDetach.ibValid, 1u,
          "dependency checkpoint preserves an explicit null IB detach");
}

void testRichDrawRecordPreservesPacketValuesAndHandles() {
  const auto draw = makeRichDrawRecord();
  checkValidRecordBytes(recordBytes(draw), D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
                        sizeof(draw), sizeof(draw),
                        "rich draw record validates");
  checkRichDrawPacketValues(draw.packet);

  ImportedChunkHandleSet payloadHandles;
  collectDrawPacketResourceHandles(draw.packet, payloadHandles);
  const auto payloadHandleEntries = makeImportedChunkHandleEntries(payloadHandles);
  check(containsHandle(payloadHandleEntries, D9C_CHUNK_HANDLE_KIND_TEXTURE, kTexture0),
        "draw payload collects texture 0");
  check(containsHandle(payloadHandleEntries, D9C_CHUNK_HANDLE_KIND_TEXTURE, kTexture15),
        "draw payload collects texture 15");
  check(containsHandle(payloadHandleEntries, D9C_CHUNK_HANDLE_KIND_BUFFER, kStream0),
        "draw payload collects stream 0");
  check(containsHandle(payloadHandleEntries, D9C_CHUNK_HANDLE_KIND_BUFFER, kStream15),
        "draw payload collects stream 15");
  check(containsHandle(payloadHandleEntries, D9C_CHUNK_HANDLE_KIND_SURFACE, kRt0),
        "draw payload collects RT0");
  check(containsHandle(payloadHandleEntries, D9C_CHUNK_HANDLE_KIND_SURFACE, kRt3),
        "draw payload collects RT3");
  check(containsHandle(payloadHandleEntries, D9C_CHUNK_HANDLE_KIND_SURFACE, kDs),
        "draw payload collects DS");
  check(containsHandle(payloadHandleEntries, D9C_CHUNK_HANDLE_KIND_SHADER, kVs),
        "draw payload collects VS");
  check(containsHandle(payloadHandleEntries, D9C_CHUNK_HANDLE_KIND_SHADER, kPs),
        "draw payload collects PS");
  check(containsHandle(payloadHandleEntries, D9C_CHUNK_HANDLE_KIND_VERTEX_DECL,
                       kVdecl),
        "draw payload collects vertex declaration");
}

void testMaxTextureStageAndSamplerDeltaPacketBoundaries() {
  constexpr std::uint32_t kMaxTss = D9C_DRAW_PACKET_MAX_TSS;
  constexpr std::uint32_t kMaxSampler = D9C_DRAW_PACKET_MAX_SAMPLER;

  D9CCommandRecordDrawPrimitive draw{};
  draw.header.type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE;
  draw.header.size = sizeof(draw);
  draw.packet.primitiveType = kD3dptTriangleList;
  draw.packet.primitiveCount = 1u;
  draw.packet.tssCount = kMaxTss;
  draw.packet.samplerStateCount = kMaxSampler;

  for (std::uint32_t i = 0; i < kMaxTss; ++i) {
    draw.packet.tss[i] = D9CDrawPacketTextureStageState{
        i % 8u,
        1u + (i / 8u),
        0x81000000u | i,
    };
  }
  for (std::uint32_t i = 0; i < kMaxSampler; ++i) {
    draw.packet.samplerStates[i] = D9CDrawPacketSamplerState{
        i % 16u,
        1u + (i / 16u),
        0x52000000u | (i * 3u),
    };
  }

  checkValidRecordBytes(recordBytes(draw), D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
                        sizeof(draw), sizeof(draw),
                        "max TSS/sampler draw record validates");

  std::vector<std::uint8_t> arena;
  const auto drawOffset = appendPayload(arena, draw);
  const auto record = wireRecordHeader(D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
                                       drawOffset,
                                       static_cast<std::uint32_t>(sizeof(draw)));
  const auto wire = makeImportedWireChunkView(
      &record, 1u, arena.data(), static_cast<std::uint32_t>(arena.size()),
      nullptr, 0u);
  const auto validation = validateImportedWireChunk(wire);
  check(validation.valid(), "max TSS/sampler wire chunk validates");

  const auto imported = nextImportedRecord(wire, 0u);
  check(imported.has_value(), "max TSS/sampler draw record imports");
  D9CCommandRecordDrawPrimitive decoded{};
  std::memcpy(&decoded, imported->record, sizeof(decoded));
  checkEq(decoded.packet.tssCount, kMaxTss,
          "max TSS count survives wire import");
  checkEq(decoded.packet.samplerStateCount, kMaxSampler,
          "max sampler-state count survives wire import");

  for (std::uint32_t i = 0; i < kMaxTss; ++i) {
    checkEq(decoded.packet.tss[i].stage, i % 8u,
            "TSS max-boundary stage");
    checkEq(decoded.packet.tss[i].type, 1u + (i / 8u),
            "TSS max-boundary type");
    checkEq(decoded.packet.tss[i].value, 0x81000000u | i,
            "TSS max-boundary value");
  }
  for (std::uint32_t i = 0; i < kMaxSampler; ++i) {
    checkEq(decoded.packet.samplerStates[i].sampler, i % 16u,
            "sampler max-boundary slot");
    checkEq(decoded.packet.samplerStates[i].type, 1u + (i / 16u),
            "sampler max-boundary type");
    checkEq(decoded.packet.samplerStates[i].value, 0x52000000u | (i * 3u),
            "sampler max-boundary value");
  }
}

void testSetConstTailBytesRecordOrderAndWireHandleRange() {
  const std::vector<float> vsF{
      1.0f, -2.0f, 3.25f, -4.5f,
      5.5f, -6.75f, 7.875f, -8.125f,
  };
  const std::vector<std::int32_t> vsI{
      std::numeric_limits<std::int32_t>::min(), -3, -2, -1,
      0, 1, 2, std::numeric_limits<std::int32_t>::max(),
  };
  const std::vector<std::uint32_t> vsB{0u, 1u, 0xffffffffu};
  const std::vector<float> psF{9.0f, 10.0f, -11.0f, 12.5f};
  const std::vector<std::int32_t> psI{-9, 8, -7, 6};
  const std::vector<std::uint32_t> psB{1u, 0u};

  const std::array<std::vector<std::uint8_t>, 6> setConstPayloads{
      makeSetConstRecordBytes(D9C_COMMAND_RECORD_SET_VS_CONST_F, 2u, vsF, 2u),
      makeSetConstRecordBytes(D9C_COMMAND_RECORD_SET_VS_CONST_I, 4u, vsI, 2u),
      makeSetConstRecordBytes(D9C_COMMAND_RECORD_SET_VS_CONST_B, 1u, vsB, 3u),
      makeSetConstRecordBytes(D9C_COMMAND_RECORD_SET_PS_CONST_F, 6u, psF, 1u),
      makeSetConstRecordBytes(D9C_COMMAND_RECORD_SET_PS_CONST_I, 8u, psI, 1u),
      makeSetConstRecordBytes(D9C_COMMAND_RECORD_SET_PS_CONST_B, 5u, psB, 2u),
  };
  const std::array<std::uint32_t, 6> setConstTypes{
      D9C_COMMAND_RECORD_SET_VS_CONST_F,
      D9C_COMMAND_RECORD_SET_VS_CONST_I,
      D9C_COMMAND_RECORD_SET_VS_CONST_B,
      D9C_COMMAND_RECORD_SET_PS_CONST_F,
      D9C_COMMAND_RECORD_SET_PS_CONST_I,
      D9C_COMMAND_RECORD_SET_PS_CONST_B,
  };
  const std::array<std::uint32_t, 6> setConstStarts{2u, 4u, 1u, 6u, 8u, 5u};
  const std::array<std::uint32_t, 6> setConstCounts{2u, 2u, 3u, 1u, 1u, 2u};

  std::vector<std::uint8_t> arena;
  std::vector<D9CCommandChunkWireRecordHeader> records;
  records.reserve(setConstPayloads.size() + 1u);

  for (std::size_t i = 0; i < setConstPayloads.size(); ++i) {
    const auto offset = appendPayload(arena, setConstPayloads[i]);
    records.push_back(wireRecordHeader(
        setConstTypes[i], offset,
        static_cast<std::uint32_t>(setConstPayloads[i].size())));
  }

  const auto draw = makeRichDrawRecord();
  const auto drawOffset = appendPayload(arena, draw);
  std::vector<D9CCommandChunkWireHandleEntry> handles{
      wireHandleEntry(D9C_CHUNK_HANDLE_KIND_TEXTURE, kTexture0),
      wireHandleEntry(D9C_CHUNK_HANDLE_KIND_TEXTURE, kTexture15),
      wireHandleEntry(D9C_CHUNK_HANDLE_KIND_BUFFER, kStream0),
      wireHandleEntry(D9C_CHUNK_HANDLE_KIND_BUFFER, kStream15),
      wireHandleEntry(D9C_CHUNK_HANDLE_KIND_SURFACE, kRt0),
      wireHandleEntry(D9C_CHUNK_HANDLE_KIND_SURFACE, kRt3),
      wireHandleEntry(D9C_CHUNK_HANDLE_KIND_SURFACE, kDs),
      wireHandleEntry(D9C_CHUNK_HANDLE_KIND_SHADER, kVs),
      wireHandleEntry(D9C_CHUNK_HANDLE_KIND_SHADER, kPs),
      wireHandleEntry(D9C_CHUNK_HANDLE_KIND_VERTEX_DECL, kVdecl),
  };
  records.push_back(wireRecordHeader(
      D9C_COMMAND_RECORD_DRAW_PRIMITIVE, drawOffset,
      static_cast<std::uint32_t>(sizeof(draw)), 0u,
      static_cast<std::uint32_t>(handles.size())));

  const auto wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()), arena.data(),
      static_cast<std::uint32_t>(arena.size()), handles.data(),
      static_cast<std::uint32_t>(handles.size()));
  const auto validation = validateImportedWireChunk(wire);
  check(validation.valid(), "consts plus draw wire chunk validates");
  checkEq(validation.parsedRecordCount, 7u, "wire record count");

  std::vector<std::uint8_t> expectedVsFTail(sizeof(float) * vsF.size());
  std::memcpy(expectedVsFTail.data(), vsF.data(), expectedVsFTail.size());
  std::vector<std::uint8_t> expectedVsITail(sizeof(std::int32_t) * vsI.size());
  std::memcpy(expectedVsITail.data(), vsI.data(), expectedVsITail.size());
  std::vector<std::uint8_t> expectedVsBTail(sizeof(std::uint32_t) * vsB.size());
  std::memcpy(expectedVsBTail.data(), vsB.data(), expectedVsBTail.size());
  std::vector<std::uint8_t> expectedPsFTail(sizeof(float) * psF.size());
  std::memcpy(expectedPsFTail.data(), psF.data(), expectedPsFTail.size());
  std::vector<std::uint8_t> expectedPsITail(sizeof(std::int32_t) * psI.size());
  std::memcpy(expectedPsITail.data(), psI.data(), expectedPsITail.size());
  std::vector<std::uint8_t> expectedPsBTail(sizeof(std::uint32_t) * psB.size());
  std::memcpy(expectedPsBTail.data(), psB.data(), expectedPsBTail.size());
  const std::array<std::vector<std::uint8_t>, 6> expectedTails{
      expectedVsFTail,
      expectedVsITail,
      expectedVsBTail,
      expectedPsFTail,
      expectedPsITail,
      expectedPsBTail,
  };

  for (std::uint32_t i = 0; i < 6u; ++i) {
    const auto record = nextImportedRecord(wire, i);
    check(record.has_value(), "set-const record imports");
    checkEq(record->header.type, setConstTypes[i], "set-const record order");
    checkSetConstRecord(*record, setConstTypes[i], setConstStarts[i],
                        setConstCounts[i], expectedTails[i],
                        "set-const payload");
  }

  const auto importedDraw = nextImportedRecord(wire, 6u);
  check(importedDraw.has_value(), "draw follows constant uploads");
  checkEq(importedDraw->header.type,
          static_cast<std::uint32_t>(D9C_COMMAND_RECORD_DRAW_PRIMITIVE),
          "draw record is last");
  D9CCommandRecordDrawPrimitive decodedDraw{};
  std::memcpy(&decodedDraw, importedDraw->record, sizeof(decodedDraw));
  checkRichDrawPacketValues(decodedDraw.packet);

  ImportedChunkHandleSet chunkHandles;
  check(collectImportedWireChunkHandles(wire, chunkHandles),
        "wire chunk handle collection succeeds");
  const auto entries = makeImportedChunkHandleEntries(chunkHandles);
  check(containsHandle(entries, D9C_CHUNK_HANDLE_KIND_TEXTURE, kTexture0),
        "wire range retains texture 0");
  check(containsHandle(entries, D9C_CHUNK_HANDLE_KIND_TEXTURE, kTexture15),
        "wire range retains texture 15");
  check(containsHandle(entries, D9C_CHUNK_HANDLE_KIND_BUFFER, kStream0),
        "wire range retains stream 0");
  check(containsHandle(entries, D9C_CHUNK_HANDLE_KIND_BUFFER, kStream15),
        "wire range retains stream 15");
  check(containsHandle(entries, D9C_CHUNK_HANDLE_KIND_SURFACE, kRt0),
        "wire range retains RT0");
  check(containsHandle(entries, D9C_CHUNK_HANDLE_KIND_SURFACE, kRt3),
        "wire range retains RT3");
  check(containsHandle(entries, D9C_CHUNK_HANDLE_KIND_SURFACE, kDs),
        "wire range retains DS");
}

void testConstShadowTracksSparseDirtyElements() {
  ConstShadow shadow;
  const std::array<std::uint32_t, 4> first{10u, 20u, 30u, 40u};
  touchConstShadow(shadow, 2u, 4u, first.data(), sizeof(std::uint32_t));
  check(shadow.dirty(), "initial const write marks shadow dirty");
  checkEq(shadow.dirtyStart, 2u, "initial dirty start");
  checkEq(shadow.dirtyEnd, 6u, "initial dirty end");
  for (std::uint32_t reg = 2u; reg < 6u; ++reg) {
    check(shadow.dirtyElems[reg] != 0, "initial dirty element marked");
  }

  shadow.clear();
  const std::array<std::uint32_t, 4> sparse{10u, 200u, 30u, 400u};
  touchConstShadow(shadow, 2u, 4u, sparse.data(), sizeof(std::uint32_t));
  check(shadow.dirty(), "sparse const write marks shadow dirty");
  checkEq(shadow.dirtyStart, 3u, "sparse dirty start skips clean prefix");
  checkEq(shadow.dirtyEnd, 6u, "sparse dirty end skips clean suffix");
  check(shadow.dirtyElems[2u] == 0, "unchanged leading element stays clean");
  check(shadow.dirtyElems[3u] != 0, "changed middle element marked");
  check(shadow.dirtyElems[4u] == 0, "unchanged gap stays clean");
  check(shadow.dirtyElems[5u] != 0, "changed trailing element marked");

  shadow.clear();
  touchConstShadow(shadow, 2u, 4u, sparse.data(), sizeof(std::uint32_t));
  check(!shadow.dirty(), "identical const write stays clean");
}

// R-BACK-2.52 (Inline Const Delta, DXMT9_PE_INLINE_CONST_DELTA=1): covers
// foldConstShadowIntoDeltaSection (d3d9_pe_const_shadow.hpp), the
// natively-testable unit backing D3D9DeviceImpl::foldPendingConstsIntoDrawPacket
// (d3d9_pe_device.cpp). D3D9DeviceImpl itself cannot be instantiated from a
// native bridge test (Windows-only PE build pulling in windows.h/d3d9.h — see
// the file header comment above), so this is the deepest level this suite can
// pin the fold mechanics at; it is complementary to
// tests/native/bridge/chunk_record_spec.cpp's testInlineConstDeltaSections(),
// which pins the T1 wire schema (section layout, caps, offsets) that this
// fold path writes into.
void testInlineConstDeltaFoldRangesAndPayloadBytes() {
  // (a) A Set*ConstantF-then-Draw sequence: SetVertexShaderConstantF(4, ..., 3)
  // then SetPixelShaderConstantB(0, ..., 2). The fold must reproduce EXACTLY
  // the merged [dirtyStart, dirtyEnd) range and element size that
  // flushConstShadow's default (non-split) path would emit as a standalone
  // record (R-BACK-2.52(d)) — this is what preserves replay equivalence.
  const std::array<float, 12> vsFValues{
      1.0f, -2.0f, 3.25f, -4.5f, 5.5f, -6.75f,
      7.875f, -8.125f, 9.0f, -9.5f, 10.25f, -10.75f,
  };
  ConstShadow vsF;
  touchConstShadow(vsF, 4u, 3u, vsFValues.data(), sizeof(float) * 4);
  check(vsF.dirty(), "VS float shadow is dirty after SetVertexShaderConstantF");
  checkEq(vsF.dirtyStart, 4u, "VS float shadow dirty start");
  checkEq(vsF.dirtyEnd, 7u, "VS float shadow dirty end");

  // Both values must differ from the shadow's zero-initialized default so
  // touchConstShadow marks both registers dirty (a value that happens to
  // match the existing shadow contents is, by design, not re-marked dirty).
  const std::array<std::uint32_t, 2> psBValues{1u, 1u};
  ConstShadow psB;
  touchConstShadow(psB, 0u, 2u, psBValues.data(), sizeof(std::uint32_t));
  check(psB.dirty(), "PS bool shadow is dirty after SetPixelShaderConstantB");

  ConstShadow vsI, vsB, psF, psI;  // untouched -> stay clean, fold to no-ops

  D9CDrawPrimitivePacket packet{};
  std::array<std::uint8_t, 512> scratch{};
  std::uint32_t payloadBytes = 0;

  auto fold = [&](ConstShadow& shadow, std::uint32_t kind, std::size_t elemSize) {
    return foldConstShadowIntoDeltaSection(
        shadow, kind, elemSize, packet.constDeltaSections[kind],
        scratch.data(), scratch.size(), payloadBytes);
  };

  check(fold(vsF, D9C_DRAW_PACKET_CONST_DELTA_VS_F, sizeof(float) * 4),
        "VS F fold succeeds (in-cap range)");
  check(fold(vsI, D9C_DRAW_PACKET_CONST_DELTA_VS_I, sizeof(std::int32_t) * 4),
        "VS I fold succeeds (no-op, shadow clean)");
  check(fold(vsB, D9C_DRAW_PACKET_CONST_DELTA_VS_B, sizeof(std::uint32_t)),
        "VS B fold succeeds (no-op, shadow clean)");
  check(fold(psF, D9C_DRAW_PACKET_CONST_DELTA_PS_F, sizeof(float) * 4),
        "PS F fold succeeds (no-op, shadow clean)");
  check(fold(psI, D9C_DRAW_PACKET_CONST_DELTA_PS_I, sizeof(std::int32_t) * 4),
        "PS I fold succeeds (no-op, shadow clean)");
  check(fold(psB, D9C_DRAW_PACKET_CONST_DELTA_PS_B, sizeof(std::uint32_t)),
        "PS B fold succeeds (in-cap range)");

  check(!vsF.dirty(), "folded VS float shadow is cleared like flushConstShadow");
  check(!psB.dirty(), "folded PS bool shadow is cleared like flushConstShadow");

  // "No standalone const record" for a touched shadow means a populated
  // section carries the fold instead. For an untouched shadow it means no
  // section at all (valid=0) — matching flushPendingConsts() today, which
  // simply skips a clean shadow rather than emitting an empty record.
  checkEq(packet.constDeltaSections[D9C_DRAW_PACKET_CONST_DELTA_VS_F].valid, 1u,
          "VS F section is populated");
  checkEq(packet.constDeltaSections[D9C_DRAW_PACKET_CONST_DELTA_VS_F].startRegister,
          4u, "VS F section start matches the merged dirty range");
  checkEq(packet.constDeltaSections[D9C_DRAW_PACKET_CONST_DELTA_VS_F].registerCount,
          3u, "VS F section count matches the merged dirty range");
  checkEq(packet.constDeltaSections[D9C_DRAW_PACKET_CONST_DELTA_VS_I].valid, 0u,
          "untouched VS I shadow folds to a no-op section");
  checkEq(packet.constDeltaSections[D9C_DRAW_PACKET_CONST_DELTA_VS_B].valid, 0u,
          "untouched VS B shadow folds to a no-op section");
  checkEq(packet.constDeltaSections[D9C_DRAW_PACKET_CONST_DELTA_PS_F].valid, 0u,
          "untouched PS F shadow folds to a no-op section");
  checkEq(packet.constDeltaSections[D9C_DRAW_PACKET_CONST_DELTA_PS_I].valid, 0u,
          "untouched PS I shadow folds to a no-op section");
  checkEq(packet.constDeltaSections[D9C_DRAW_PACKET_CONST_DELTA_PS_B].valid, 1u,
          "PS B section is populated");
  checkEq(packet.constDeltaSections[D9C_DRAW_PACKET_CONST_DELTA_PS_B].startRegister,
          0u, "PS B section start matches the merged dirty range");
  checkEq(packet.constDeltaSections[D9C_DRAW_PACKET_CONST_DELTA_PS_B].registerCount,
          2u, "PS B section count matches the merged dirty range");

  check(d9c_draw_packet_const_delta_sections_valid(&packet),
        "folded packet passes the same section-range validator the importer runs");
  checkEq(payloadBytes, d9c_draw_packet_const_delta_payload_bytes(&packet),
          "accumulated fold payload bytes match the schema's declared total");
  checkEq(payloadBytes,
          static_cast<std::uint32_t>(sizeof(float) * 12u + sizeof(std::uint32_t) * 2u),
          "payload is exactly VS F(3 regs * 16B) + PS B(2 regs * 4B)");

  // Payload packing: canonical order means VS F's bytes land first and PS
  // B's bytes pack immediately after (every other section contributes 0
  // bytes) — exactly what d9c_draw_packet_const_delta_section_local_offset
  // computes from the schema.
  const auto vsFLocalOffset = d9c_draw_packet_const_delta_section_local_offset(
      &packet, D9C_DRAW_PACKET_CONST_DELTA_VS_F);
  const auto psBLocalOffset = d9c_draw_packet_const_delta_section_local_offset(
      &packet, D9C_DRAW_PACKET_CONST_DELTA_PS_B);
  checkEq(vsFLocalOffset, 0u, "VS F is the first populated section");
  checkEq(psBLocalOffset, static_cast<std::uint32_t>(sizeof(float) * 12u),
          "PS B packs immediately after VS F's payload");
  check(std::memcmp(scratch.data() + vsFLocalOffset, vsFValues.data(),
                    sizeof(float) * vsFValues.size()) == 0,
        "VS F payload bytes round-trip exactly through the fold");
  check(std::memcmp(scratch.data() + psBLocalOffset, psBValues.data(),
                    sizeof(std::uint32_t) * psBValues.size()) == 0,
        "PS B payload bytes round-trip exactly through the fold");

  // (b) A merged range that fails the register-file cap check (should be
  // unreachable in production — the Set* fast paths already bound ranges to
  // the D3D9 register-file limit before touchConstShadow runs) is a safe
  // no-op: the section stays invalid and the shadow is left dirty so the
  // caller's fallback to a standalone flushConstShadow does not silently
  // drop bytes.
  ConstShadow overCapVsI;
  const std::array<std::int32_t, 8> overCapValues{0, 1, 2, 3, 4, 5, 6, 7};
  touchConstShadow(overCapVsI, 15u, 2u, overCapValues.data(), sizeof(std::int32_t) * 4);
  check(overCapVsI.dirty(), "over-cap shadow is dirty before folding");
  D9CDrawPacketConstDeltaSection overCapSection{};
  std::uint32_t overCapPayloadBytes = 0;
  check(!foldConstShadowIntoDeltaSection(
            overCapVsI, D9C_DRAW_PACKET_CONST_DELTA_VS_I,
            sizeof(std::int32_t) * 4, overCapSection, scratch.data(),
            scratch.size(), overCapPayloadBytes),
        "fold rejects a range that exceeds the VS I register-file cap");
  checkEq(overCapSection.valid, 0u, "rejected section stays invalid");
  checkEq(overCapPayloadBytes, 0u, "rejected fold does not consume payload bytes");
  check(overCapVsI.dirty(),
        "rejected fold leaves the shadow dirty for a standalone fallback flush");

  // (c) Capacity guard: a payload buffer too small for the section's bytes
  // is also rejected rather than overflowing, again leaving the shadow
  // dirty for a fallback flush.
  ConstShadow tooBigForBuffer;
  const std::array<float, 4> smallValues{1.0f, 2.0f, 3.0f, 4.0f};
  touchConstShadow(tooBigForBuffer, 0u, 1u, smallValues.data(), sizeof(float) * 4);
  D9CDrawPacketConstDeltaSection tinyBufferSection{};
  std::uint32_t tinyBufferPayloadBytes = 0;
  std::array<std::uint8_t, 4> tooSmallScratch{};
  check(!foldConstShadowIntoDeltaSection(
            tooBigForBuffer, D9C_DRAW_PACKET_CONST_DELTA_VS_F,
            sizeof(float) * 4, tinyBufferSection, tooSmallScratch.data(),
            tooSmallScratch.size(), tinyBufferPayloadBytes),
        "fold rejects a section that would overflow the payload buffer");
  checkEq(tinyBufferPayloadBytes, 0u,
          "capacity-rejected fold does not consume payload bytes");
  check(tooBigForBuffer.dirty(),
        "capacity-rejected fold leaves the shadow dirty for a standalone fallback flush");

  // (d) A clean shadow folds to a true no-op: valid stays 0, no payload
  // bytes consumed, shadow stays clean.
  ConstShadow clean;
  D9CDrawPacketConstDeltaSection cleanSection{};
  std::uint32_t cleanPayloadBytes = 5u;  // sentinel proving it's untouched
  check(foldConstShadowIntoDeltaSection(
            clean, D9C_DRAW_PACKET_CONST_DELTA_PS_F, sizeof(float) * 4,
            cleanSection, scratch.data(), scratch.size(), cleanPayloadBytes),
        "folding a clean shadow succeeds trivially");
  checkEq(cleanSection.valid, 0u, "clean shadow's section stays invalid");
  checkEq(cleanPayloadBytes, 5u,
          "clean shadow's fold does not touch payloadBytesInOut");
  check(!clean.dirty(), "clean shadow stays clean");
}

}  // namespace

int main() {
  try {
    testPeAttachmentDeltaBuilderPreservesExplicitNullsAndSlots();
    testPeAttachmentSnapshotBuilderPreservesExplicitNulls();
    testPeDrawStreamDependencyCheckpointPreservesCoalescing();
    testPeDrawIndexDependencyCheckpointPreservesCoalescing();
    testRichDrawRecordPreservesPacketValuesAndHandles();
    testMaxTextureStageAndSamplerDeltaPacketBoundaries();
    testSetConstTailBytesRecordOrderAndWireHandleRange();
    testConstShadowTracksSparseDirtyElements();
    testInlineConstDeltaFoldRangesAndPayloadBytes();
  } catch (const TestFailure& e) {
    std::cerr << "pe_chunk_record_value_spec failed: " << e.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& e) {
    std::cerr << "pe_chunk_record_value_spec unexpected exception: "
              << e.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "pe_chunk_record_value_spec passed\n";
  return EXIT_SUCCESS;
}
