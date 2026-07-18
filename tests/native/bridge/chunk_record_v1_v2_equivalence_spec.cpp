#include "d3d9_pe_chunk_v2_builder.hpp"
#include "device_c_chunk_v2_registry.hpp"
#include "device_c_chunk_v2_replay.hpp"
#include "device_c_replay_offload.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

struct FakeObject {
  std::uint32_t refs = 1u;
  std::uint32_t tag = 0u;
};
struct D9CSurface : FakeObject {};
struct D9CTexture : FakeObject {};
struct D9CBuffer : FakeObject {};
struct D9CShader : FakeObject {};
struct D9CVertexDecl : FakeObject {};
struct D9CQuery : FakeObject {};

template <typename T>
void addRef(T* value) { ++value->refs; }
template <typename T>
std::uint32_t release(T* value) { return --value->refs; }

extern "C" void dxmt9c_surface_addref(D9CSurface* value) { addRef(value); }
extern "C" std::uint32_t dxmt9c_surface_release(D9CSurface* value) {
  return release(value);
}
extern "C" void dxmt9c_texture_addref(D9CTexture* value) { addRef(value); }
extern "C" std::uint32_t dxmt9c_texture_release(D9CTexture* value) {
  return release(value);
}
extern "C" void dxmt9c_buffer_addref(D9CBuffer* value) { addRef(value); }
extern "C" std::uint32_t dxmt9c_buffer_release(D9CBuffer* value) {
  return release(value);
}
extern "C" void dxmt9c_shader_addref(D9CShader* value) { addRef(value); }
extern "C" std::uint32_t dxmt9c_shader_release(D9CShader* value) {
  return release(value);
}
extern "C" void dxmt9c_vdecl_addref(D9CVertexDecl* value) { addRef(value); }
extern "C" std::uint32_t dxmt9c_vdecl_release(D9CVertexDecl* value) {
  return release(value);
}
extern "C" void dxmt9c_query_addref(D9CQuery* value) { addRef(value); }
extern "C" std::uint32_t dxmt9c_query_release(D9CQuery* value) {
  return release(value);
}

namespace {

using dxmt9::d3d9::ImportedChunkV2View;
using dxmt9::d3d9::ResolvedChunkV2View;
using dxmt9::d3d9::ResolvedRecordV2View;
using dxmt9::d3d9::SparseDrawCallV2;
using dxmt9::d3d9::V2ChunkEnvelope;
using dxmt9::d3d9::WireObjectRegistry;
using dxmt9::d3d9::pe::CommandChunkV2Builder;
using dxmt9::d3d9::pe::PeWireObjectRef;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) throw TestFailure(std::string(message));
}

enum class Op : std::uint32_t {
  RenderStates,
  Texture,
  Stream,
  Shader,
  VertexInput,
  IndexBuffer,
  RenderTarget,
  DepthStencil,
  Viewport,
  Scissor,
  Material,
  ClipPlane,
  TextureStageStates,
  SamplerStates,
  Transforms,
  Lights,
  LightEnables,
  Constants,
  Apply,
  Draw,
  Clear,
  Present,
  StretchRect,
  ColorFill,
  UpdateTexture,
  UpdateSurface,
  QueryIssue,
  Readback,
  Resz,
};

struct Event {
  Op op{};
  std::array<std::uint64_t, 12u> values{};
  std::uint32_t valueCount = 0u;
  std::uint64_t payloadHash = 0u;

  bool operator==(const Event&) const = default;
};

std::uint64_t hashBytes(std::span<const std::byte> bytes) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const auto value : bytes) {
    hash ^= std::to_integer<std::uint8_t>(value);
    hash *= 1099511628211ull;
  }
  return hash;
}

template <typename T, std::size_t Extent>
std::uint64_t hashValues(std::span<T, Extent> values) {
  return hashBytes(std::as_bytes(values));
}

std::uint64_t tag(void* object) {
  return object ? static_cast<FakeObject*>(object)->tag : 0u;
}

void appendEvent(std::vector<Event>& trace, Op op,
                 std::initializer_list<std::uint64_t> values = {},
                 std::uint64_t payloadHash = 0u) {
  Event event{.op = op, .payloadHash = payloadHash};
  check(values.size() <= event.values.size(), "event value capacity");
  for (const auto value : values) event.values[event.valueCount++] = value;
  trace.push_back(event);
}

bool tracesEqual(std::span<const Event> lhs, std::span<const Event> rhs,
                 const char* label) {
  if (lhs.size() != rhs.size()) {
    std::cerr << label << ": trace size " << lhs.size() << " != "
              << rhs.size() << '\n';
    return false;
  }
  for (std::size_t i = 0u; i < lhs.size(); ++i) {
    if (lhs[i] == rhs[i]) continue;
    std::cerr << label << ": event " << i << " differs (op "
              << static_cast<unsigned>(lhs[i].op) << " vs "
              << static_cast<unsigned>(rhs[i].op) << ", count "
              << lhs[i].valueCount << " vs " << rhs[i].valueCount
              << ", hash " << lhs[i].payloadHash << " vs "
              << rhs[i].payloadHash << ")\n";
    for (std::size_t j = 0u;
         j < std::max(lhs[i].valueCount, rhs[i].valueCount); ++j)
      std::cerr << "  value[" << j << "] " << lhs[i].values[j] << " vs "
                << rhs[i].values[j] << '\n';
    return false;
  }
  return true;
}

D9CWireHandle wire(void* object) {
  const auto value = static_cast<std::uint64_t>(
      reinterpret_cast<std::uintptr_t>(object));
  return {.lo = static_cast<std::uint32_t>(value),
          .hi = static_cast<std::uint32_t>(value >> 32u)};
}

void* pointer(D9CWireHandle value) {
  const auto bits = static_cast<std::uint64_t>(value.lo) |
                    (static_cast<std::uint64_t>(value.hi) << 32u);
  return reinterpret_cast<void*>(static_cast<std::uintptr_t>(bits));
}

template <typename T>
std::vector<std::byte> recordBytes(const T& record) {
  std::vector<std::byte> bytes(sizeof(record));
  std::memcpy(bytes.data(), &record, sizeof(record));
  return bytes;
}

template <typename T>
T load(std::span<const std::byte> bytes) {
  check(bytes.size() >= sizeof(T), "legacy record prefix fits");
  T value{};
  std::memcpy(&value, bytes.data(), sizeof(value));
  return value;
}

class TraceSink final : public dxmt9::d3d9::NonDrawReplaySinkV2,
                        public dxmt9::d3d9::SparseReplaySinkV2 {
public:
  explicit TraceSink(std::vector<Event>& trace) : trace_(trace) {}

  std::int32_t setConstants(
      std::uint32_t type, const D9CCommandChunkWireSetConstV2& fixed,
      std::span<const std::byte> bytes) override {
    appendEvent(trace_, Op::Constants,
                {type, fixed.startRegister, fixed.registerCount},
                hashBytes(bytes));
    return 0;
  }
  std::int32_t clear(const D9CCommandChunkWireClearV2& fixed,
                     std::span<const D9CRect> rects) override {
    appendEvent(trace_, Op::Clear,
                {fixed.flags, fixed.colorARGB, fixed.stencil, fixed.rectCount},
                hashValues(rects));
    return 0;
  }
  std::int32_t present(
      const D9CCommandChunkWirePresentV2& fixed) override {
    appendEvent(trace_, Op::Present,
                {fixed.hwnd, fixed.flags, fixed.hasSrc, fixed.hasDst},
                hashValues(std::span(&fixed.src, 2u)));
    return 0;
  }
  std::int32_t stretchRect(
      const D9CCommandChunkWireStretchRectV2& fixed, void* src,
      void* dst) override {
    appendEvent(trace_, Op::StretchRect,
                {tag(src), tag(dst), fixed.hasSrcRect, fixed.hasDstRect,
                 fixed.filter},
                hashValues(std::span(&fixed.srcRect, 2u)));
    return 0;
  }
  std::int32_t colorFill(
      const D9CCommandChunkWireColorFillV2& fixed, void* surface) override {
    appendEvent(trace_, Op::ColorFill,
                {tag(surface), fixed.colorARGB, fixed.hasRect},
                hashValues(std::span(&fixed.rect, 1u)));
    return 0;
  }
  std::int32_t updateTexture(
      const D9CCommandChunkWireUpdateTextureV2&, void* src,
      void* dst) override {
    appendEvent(trace_, Op::UpdateTexture, {tag(src), tag(dst)});
    return 0;
  }
  std::int32_t updateSurface(
      const D9CCommandChunkWireUpdateSurfaceV2& fixed, void* src,
      void* dst) override {
    appendEvent(trace_, Op::UpdateSurface,
                {tag(src), tag(dst), fixed.hasSrcRect, fixed.hasDstPoint},
                hashValues(std::span(&fixed.srcRect, 2u)));
    return 0;
  }
  std::int32_t queryIssue(
      const D9CCommandChunkWireQueryIssueV2& fixed, void* query) override {
    appendEvent(trace_, Op::QueryIssue, {tag(query), fixed.flags});
    return 0;
  }
  std::int32_t readback(
      const D9CCommandChunkWireReadbackV2&, void* src, void* dst) override {
    appendEvent(trace_, Op::Readback, {tag(src), tag(dst)});
    return 0;
  }
  std::int32_t reszDepthResolve(
      const D9CCommandChunkWireReszDepthResolveV2&, void* src,
      void* dst) override {
    appendEvent(trace_, Op::Resz, {tag(src), tag(dst)});
    return 0;
  }
  std::int32_t applyState(const ResolvedRecordV2View& record) override {
    return dxmt9::d3d9::replaySparseRecordV2(record, *this);
  }
  std::int32_t setRenderStates(
      std::span<const D9CCommandChunkWireRenderStateV2> values) override {
    appendEvent(trace_, Op::RenderStates, {values.size()}, hashValues(values));
    return 0;
  }
  std::int32_t setTexture(std::uint32_t slot, void* texture) override {
    appendEvent(trace_, Op::Texture, {slot, tag(texture)});
    return 0;
  }
  std::int32_t setStream(
      const D9CCommandChunkWireStreamBindingV2& value,
      void* buffer) override {
    appendEvent(trace_, Op::Stream,
                {value.slot, tag(buffer), value.offset, value.stride,
                 value.frequency});
    return 0;
  }
  std::int32_t setShader(std::uint32_t stage, void* shader) override {
    appendEvent(trace_, Op::Shader, {stage, tag(shader)});
    return 0;
  }
  std::int32_t setVertexInput(std::uint32_t kind, std::uint32_t value,
                              void* declaration) override {
    appendEvent(trace_, Op::VertexInput, {kind, value, tag(declaration)});
    return 0;
  }
  std::int32_t setIndexBuffer(void* buffer) override {
    appendEvent(trace_, Op::IndexBuffer, {tag(buffer)});
    return 0;
  }
  std::int32_t setRenderTarget(std::uint32_t slot, void* surface) override {
    appendEvent(trace_, Op::RenderTarget, {slot, tag(surface)});
    return 0;
  }
  std::int32_t setDepthStencil(void* surface) override {
    appendEvent(trace_, Op::DepthStencil, {tag(surface)});
    return 0;
  }
  std::int32_t setViewport(const D9CViewport& value) override {
    appendEvent(trace_, Op::Viewport, {}, hashValues(std::span(&value, 1u)));
    return 0;
  }
  std::int32_t setScissor(const D9CRect& value) override {
    appendEvent(trace_, Op::Scissor, {}, hashValues(std::span(&value, 1u)));
    return 0;
  }
  std::int32_t setMaterial(const D9CMaterial& value) override {
    appendEvent(trace_, Op::Material, {}, hashValues(std::span(&value, 1u)));
    return 0;
  }
  std::int32_t setClipPlane(
      const D9CCommandChunkWireClipPlaneV2& value) override {
    appendEvent(trace_, Op::ClipPlane, {value.slot},
                hashValues(std::span(value.values)));
    return 0;
  }
  std::int32_t setTextureStageStates(
      std::span<const D9CDrawPacketTextureStageState> values) override {
    appendEvent(trace_, Op::TextureStageStates, {values.size()},
                hashValues(values));
    return 0;
  }
  std::int32_t setSamplerStates(
      std::span<const D9CDrawPacketSamplerState> values) override {
    appendEvent(trace_, Op::SamplerStates, {values.size()}, hashValues(values));
    return 0;
  }
  std::int32_t setTransforms(
      std::span<const D9CDrawPacketTransform> values) override {
    appendEvent(trace_, Op::Transforms, {values.size()}, hashValues(values));
    return 0;
  }
  std::int32_t setLights(
      std::span<const D9CCommandChunkWireLightV2> values) override {
    appendEvent(trace_, Op::Lights, {values.size()}, hashValues(values));
    return 0;
  }
  std::int32_t setLightEnables(
      std::span<const D9CCommandChunkWireLightEnableV2> values) override {
    appendEvent(trace_, Op::LightEnables, {values.size()}, hashValues(values));
    return 0;
  }
  std::int32_t setConstants(
      std::uint16_t kind, const D9CCommandChunkWireConstantRangeV2& range,
      std::span<const std::byte> bytes) override {
    appendEvent(trace_, Op::Constants,
                {kind, range.startRegister, range.registerCount},
                hashBytes(bytes));
    return 0;
  }
  std::int32_t finishApplyState(std::uint32_t flags) override {
    appendEvent(trace_, Op::Apply, {flags});
    return 0;
  }
  std::int32_t draw(const SparseDrawCallV2& call) override {
    const auto payloadHash = call.payload.userIndexData.empty() &&
                                     call.payload.userVertexData.empty()
                                 ? 0u
                                 : hashBytes(std::as_bytes(
                                       call.payload.userIndexData)) ^
                                       (hashBytes(std::as_bytes(
                                            call.payload.userVertexData))
                                        << 1u);
    appendEvent(trace_, Op::Draw,
                {static_cast<std::uint32_t>(call.param.primitiveType) + 1u,
                 call.param.indexed, call.param.primitiveCount,
                 call.param.startVertex,
                 static_cast<std::uint32_t>(call.param.baseVertexIndex),
                 call.param.startIndex, call.minVertex, call.numVertices,
                 call.stride, call.indexFormat, call.flags},
                payloadHash);
    return 0;
  }

private:
  std::vector<Event>& trace_;
};

void replayLegacyState(const D9CDrawPrimitivePacket& packet,
                       const D9CDrawIndexedPrimitivePacket* indexed,
                       std::span<const std::byte> bytes,
                       std::uint32_t constBase,
                       std::vector<Event>& trace) {
  if (packet.renderStateCount) {
    appendEvent(trace, Op::RenderStates, {packet.renderStateCount},
                hashValues(std::span(packet.renderStates)
                               .first(packet.renderStateCount)));
  }
  for (std::uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_TEXTURES; ++slot) {
    if (packet.textureMask & (1u << slot))
      appendEvent(trace, Op::Texture,
                  {slot, tag(pointer(packet.textures[slot]))});
  }
  for (std::uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_STREAMS; ++slot) {
    if (!(packet.streamSourceMask & (1u << slot))) continue;
    const auto& value = packet.streamSources[slot];
    appendEvent(trace, Op::Stream,
                {slot, tag(pointer(value.buffer)), value.offset, value.stride,
                 0u});
  }
  if (packet.vsValid)
    appendEvent(trace, Op::Shader,
                {D9C_COMMAND_CHUNK_V2_SHADER_STAGE_VERTEX,
                 tag(pointer(packet.vsHandle))});
  if (packet.psValid)
    appendEvent(trace, Op::Shader,
                {D9C_COMMAND_CHUNK_V2_SHADER_STAGE_PIXEL,
                 tag(pointer(packet.psHandle))});
  if (packet.vdeclValid)
    appendEvent(trace, Op::VertexInput,
                {D9C_COMMAND_CHUNK_V2_VERTEX_INPUT_DECLARATION, packet.fvf,
                 tag(pointer(packet.vdeclHandle))});
  else if (packet.fvfValid)
    appendEvent(trace, Op::VertexInput,
                {D9C_COMMAND_CHUNK_V2_VERTEX_INPUT_FVF, packet.fvf, 0u});
  if (indexed && indexed->ibValid)
    appendEvent(trace, Op::IndexBuffer,
                {tag(pointer(indexed->ibHandle))});
  for (std::uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_RENDER_TARGETS;
       ++slot) {
    if (packet.rtMask & (1u << slot))
      appendEvent(trace, Op::RenderTarget,
                  {slot, tag(pointer(packet.rtHandles[slot]))});
  }
  if (packet.dsValid)
    appendEvent(trace, Op::DepthStencil, {tag(pointer(packet.dsHandle))});
  if (packet.viewportValid)
    appendEvent(trace, Op::Viewport, {},
                hashValues(std::span(&packet.viewport, 1u)));
  if (packet.scissorValid)
    appendEvent(trace, Op::Scissor, {},
                hashValues(std::span(&packet.scissor, 1u)));
  if (packet.materialValid)
    appendEvent(trace, Op::Material, {},
                hashValues(std::span(&packet.material, 1u)));
  for (std::uint32_t slot = 0; slot < 6u; ++slot) {
    if (packet.clipPlaneMask & (1u << slot))
      appendEvent(trace, Op::ClipPlane, {slot},
                  hashValues(std::span(&packet.clipPlanes[slot * 4u], 4u)));
  }
  if (packet.tssCount)
    appendEvent(trace, Op::TextureStageStates, {packet.tssCount},
                hashValues(std::span(packet.tss).first(packet.tssCount)));
  if (packet.samplerStateCount)
    appendEvent(trace, Op::SamplerStates, {packet.samplerStateCount},
                hashValues(std::span(packet.samplerStates)
                               .first(packet.samplerStateCount)));
  if (packet.transformCount)
    appendEvent(trace, Op::Transforms, {packet.transformCount},
                hashValues(std::span(packet.transforms)
                               .first(packet.transformCount)));
  std::array<D9CCommandChunkWireLightV2, D9C_DRAW_PACKET_MAX_LIGHTS> lights{};
  std::uint32_t lightCount = 0u;
  std::array<D9CCommandChunkWireLightEnableV2, D9C_DRAW_PACKET_MAX_LIGHTS>
      enables{};
  std::uint32_t enableCount = 0u;
  for (std::uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_LIGHTS; ++slot) {
    if (packet.lightSlotMask & (1u << slot))
      lights[lightCount++] = {.slot = slot, .light = packet.lights[slot]};
    if (packet.lightEnableValidMask & (1u << slot))
      enables[enableCount++] = {
          .slot = slot,
          .enabled = (packet.lightEnableMask & (1u << slot)) != 0u};
  }
  if (lightCount)
    appendEvent(trace, Op::Lights, {lightCount},
                hashValues(std::span(lights).first(lightCount)));
  if (enableCount)
    appendEvent(trace, Op::LightEnables, {enableCount},
                hashValues(std::span(enables).first(enableCount)));
  static constexpr std::array sectionKinds = {
      D9C_COMMAND_CHUNK_V2_SECTION_VS_CONST_F,
      D9C_COMMAND_CHUNK_V2_SECTION_VS_CONST_I,
      D9C_COMMAND_CHUNK_V2_SECTION_VS_CONST_B,
      D9C_COMMAND_CHUNK_V2_SECTION_PS_CONST_F,
      D9C_COMMAND_CHUNK_V2_SECTION_PS_CONST_I,
      D9C_COMMAND_CHUNK_V2_SECTION_PS_CONST_B,
  };
  for (std::uint32_t kind = 0u; kind < sectionKinds.size(); ++kind) {
    const auto& range = packet.constDeltaSections[kind];
    if (!range.valid) continue;
    const auto slice = d9c_draw_packet_const_delta_section_slice(
        &packet, constBase, kind);
    appendEvent(trace, Op::Constants,
                {sectionKinds[kind], range.startRegister,
                 range.registerCount},
                hashBytes(bytes.subspan(slice.payloadOffset,
                                        slice.payloadSize)));
  }
}

void replayLegacy(std::span<const std::byte> bytes,
                  std::vector<Event>& trace) {
  const auto header = load<D9CCommandRecordHeader>(bytes);
  switch (header.type) {
  case D9C_COMMAND_RECORD_DRAW_PRIMITIVE: {
    const auto value = load<D9CCommandRecordDrawPrimitive>(bytes);
    replayLegacyState(value.packet, nullptr, bytes,
                      d9c_command_record_draw_primitive_const_delta_offset(),
                      trace);
    appendEvent(trace, Op::Draw,
                {value.packet.primitiveType, 0u, value.packet.primitiveCount,
                 value.packet.startVertex, 0u, 0u, 0u, 0u, 0u, 0u,
                 (value.packet.textureMask == 0xfffffu &&
                  value.packet.streamSourceMask == 0xffffu)});
    break;
  }
  case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE: {
    const auto value = load<D9CCommandRecordDrawIndexedPrimitive>(bytes);
    replayLegacyState(value.packet.state, &value.packet, bytes,
                      d9c_command_record_draw_indexed_primitive_const_delta_offset(),
                      trace);
    appendEvent(trace, Op::Draw,
                {value.packet.state.primitiveType, 1u,
                 value.packet.primitiveCount, 0u,
                 static_cast<std::uint32_t>(value.packet.baseVertex),
                 value.packet.startIndex, value.packet.minVertex,
                 value.packet.numVertices, 0u, 0u, 0u});
    break;
  }
  case D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP: {
    const auto value = load<D9CCommandRecordDrawPrimitiveUP>(bytes);
    replayLegacyState(value.packet.state, nullptr, bytes,
                      d9c_command_record_draw_primitive_up_const_delta_offset(
                          &value.packet), trace);
    const auto vertices = bytes.subspan(value.packet.vertexDataOffset,
                                        value.packet.vertexDataSize);
    appendEvent(trace, Op::Draw,
                {value.packet.state.primitiveType, 0u,
                 value.packet.primitiveCount, 0u, 0u, 0u, 0u, 0u,
                 value.packet.stride, 0u, 0u},
                hashBytes({}) ^ (hashBytes(vertices) << 1u));
    break;
  }
  case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP: {
    const auto value = load<D9CCommandRecordDrawIndexedPrimitiveUP>(bytes);
    replayLegacyState(
        value.packet.state, nullptr, bytes,
        d9c_command_record_draw_indexed_primitive_up_const_delta_offset(
            &value.packet), trace);
    const auto indices = bytes.subspan(value.packet.indexDataOffset,
                                       value.packet.indexDataSize);
    const auto vertices = bytes.subspan(value.packet.vertexDataOffset,
                                        value.packet.vertexDataSize);
    appendEvent(trace, Op::Draw,
                {value.packet.state.primitiveType, 1u,
                 value.packet.primitiveCount, 0u, 0u, 0u,
                 value.packet.minVertex, value.packet.numVertices,
                 value.packet.stride, value.packet.indexFormat, 0u},
                hashBytes(indices) ^ (hashBytes(vertices) << 1u));
    break;
  }
  case D9C_COMMAND_RECORD_APPLY_STATE: {
    const auto value = load<D9CCommandRecordApplyState>(bytes);
    replayLegacyState(value.packet, nullptr, bytes, sizeof(value), trace);
    appendEvent(trace, Op::Apply, {0u});
    break;
  }
  case D9C_COMMAND_RECORD_SET_VS_CONST_F:
  case D9C_COMMAND_RECORD_SET_VS_CONST_I:
  case D9C_COMMAND_RECORD_SET_VS_CONST_B:
  case D9C_COMMAND_RECORD_SET_PS_CONST_F:
  case D9C_COMMAND_RECORD_SET_PS_CONST_I:
  case D9C_COMMAND_RECORD_SET_PS_CONST_B: {
    const auto value = load<D9CCommandRecordSetConst>(bytes);
    appendEvent(trace, Op::Constants,
                {header.type, value.start, value.count},
                hashBytes(bytes.subspan(sizeof(value))));
    break;
  }
  case D9C_COMMAND_RECORD_CLEAR: {
    const auto value = load<D9CCommandRecordClear>(bytes);
    appendEvent(trace, Op::Clear,
                {value.flags, value.colorARGB, value.stencil, value.rectCount},
                hashBytes(bytes.subspan(value.rectOffset,
                                        value.rectCount * sizeof(D9CRect))));
    break;
  }
  case D9C_COMMAND_RECORD_PRESENT: {
    const auto value = load<D9CCommandRecordPresent>(bytes);
    appendEvent(trace, Op::Present,
                {value.hwnd, value.flags, value.hasSrc, value.hasDst},
                hashValues(std::span(&value.src, 2u)));
    break;
  }
  case D9C_COMMAND_RECORD_STRETCH_RECT: {
    const auto value = load<D9CCommandRecordStretchRect>(bytes);
    appendEvent(trace, Op::StretchRect,
                {tag(reinterpret_cast<void*>(value.srcWire)),
                 tag(reinterpret_cast<void*>(value.dstWire)), value.hasSrcRect,
                 value.hasDstRect, value.filter},
                hashValues(std::span(&value.srcRect, 2u)));
    break;
  }
  case D9C_COMMAND_RECORD_COLOR_FILL: {
    const auto value = load<D9CCommandRecordColorFill>(bytes);
    appendEvent(trace, Op::ColorFill,
                {tag(reinterpret_cast<void*>(value.surfaceWire)),
                 value.colorARGB, value.hasRect},
                hashValues(std::span(&value.rect, 1u)));
    break;
  }
  case D9C_COMMAND_RECORD_UPDATE_TEXTURE: {
    const auto value = load<D9CCommandRecordUpdateTexture>(bytes);
    appendEvent(trace, Op::UpdateTexture,
                {tag(reinterpret_cast<void*>(value.srcWire)),
                 tag(reinterpret_cast<void*>(value.dstWire))});
    break;
  }
  case D9C_COMMAND_RECORD_UPDATE_SURFACE: {
    const auto value = load<D9CCommandRecordUpdateSurface>(bytes);
    appendEvent(trace, Op::UpdateSurface,
                {tag(reinterpret_cast<void*>(value.srcWire)),
                 tag(reinterpret_cast<void*>(value.dstWire)),
                 value.hasSrcRect, value.hasDstPoint},
                hashValues(std::span(&value.srcRect, 2u)));
    break;
  }
  case D9C_COMMAND_RECORD_QUERY_ISSUE: {
    const auto value = load<D9CCommandRecordQueryIssue>(bytes);
    appendEvent(trace, Op::QueryIssue,
                {tag(reinterpret_cast<void*>(value.queryWire)), value.flags});
    break;
  }
  case D9C_COMMAND_RECORD_READBACK: {
    const auto value = load<D9CCommandRecordReadback>(bytes);
    appendEvent(trace, Op::Readback,
                {tag(reinterpret_cast<void*>(value.srcWire)),
                 tag(reinterpret_cast<void*>(value.dstWire))});
    break;
  }
  case D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE: {
    const auto value = load<D9CCommandRecordReszDepthResolve>(bytes);
    appendEvent(trace, Op::Resz,
                {tag(reinterpret_cast<void*>(value.msaaDepthHandle)),
                 tag(reinterpret_cast<void*>(value.intzDestHandle))});
    break;
  }
  default:
    throw TestFailure("unhandled V1 opcode");
  }
}

void replayV2(const ImportedChunkV2View& imported,
              std::span<void* const> objects, std::vector<Event>& trace) {
  ResolvedChunkV2View resolved{.wire = imported, .objects = objects};
  TraceSink sink(trace);
  for (std::size_t i = 0; i < imported.records.size(); ++i) {
    const auto record = resolved.record(i);
    const auto hr = dxmt9::d3d9::isSparseRecordV2(record.wire.header.type)
                        ? dxmt9::d3d9::replaySparseRecordV2(record, sink)
                        : dxmt9::d3d9::replayNonDrawRecordV2(record, sink);
    check(hr == 0, "V2 normalized replay succeeds");
  }
}

std::vector<std::byte> setConstRecord(std::uint32_t type,
                                      std::uint32_t elementSize,
                                      std::uint8_t seed) {
  D9CCommandRecordSetConst record{};
  record.header.type = type;
  record.header.size = sizeof(record) + elementSize;
  record.start = 1u;
  record.count = 1u;
  std::vector<std::byte> bytes(record.header.size);
  std::memcpy(bytes.data(), &record, sizeof(record));
  for (std::uint32_t i = 0; i < elementSize; ++i)
    bytes[sizeof(record) + i] = std::byte(seed + i);
  return bytes;
}

void testCompleteParityCorpus() {
  WireObjectRegistry registry;
  D9CTexture srcTexture{{1u, 11u}};
  D9CTexture dstTexture{{1u, 12u}};
  D9CBuffer vertexBuffer{{1u, 21u}};
  D9CBuffer indexBuffer{{1u, 22u}};
  D9CShader vertexShader{{1u, 31u}};
  D9CShader pixelShader{{1u, 32u}};
  D9CVertexDecl declaration{{1u, 41u}};
  D9CSurface srcSurface{{1u, 51u}};
  D9CSurface dstSurface{{1u, 52u}};
  D9CQuery query{{1u, 61u}};

  const auto registerObject = [&](std::uint32_t kind, FakeObject* object) {
    const auto identity = registry.insert(kind, object);
    PeWireObjectRef ref{.identity = identity, .object = object};
    dxmt9::d3d9::pe::publishCachedWireObjectRef(ref);
    return ref;
  };
  const std::array refs = {
      registerObject(D9C_CHUNK_HANDLE_KIND_TEXTURE, &srcTexture),
      registerObject(D9C_CHUNK_HANDLE_KIND_TEXTURE, &dstTexture),
      registerObject(D9C_CHUNK_HANDLE_KIND_BUFFER, &vertexBuffer),
      registerObject(D9C_CHUNK_HANDLE_KIND_BUFFER, &indexBuffer),
      registerObject(D9C_CHUNK_HANDLE_KIND_SHADER, &vertexShader),
      registerObject(D9C_CHUNK_HANDLE_KIND_SHADER, &pixelShader),
      registerObject(D9C_CHUNK_HANDLE_KIND_VERTEX_DECL, &declaration),
      registerObject(D9C_CHUNK_HANDLE_KIND_SURFACE, &srcSurface),
      registerObject(D9C_CHUNK_HANDLE_KIND_SURFACE, &dstSurface),
      registerObject(D9C_CHUNK_HANDLE_KIND_QUERY, &query),
  };

  std::vector<std::vector<std::byte>> corpus;
  corpus.push_back(setConstRecord(D9C_COMMAND_RECORD_SET_VS_CONST_F, 16u, 1u));
  corpus.push_back(setConstRecord(D9C_COMMAND_RECORD_SET_VS_CONST_I, 16u, 2u));
  corpus.push_back(setConstRecord(D9C_COMMAND_RECORD_SET_VS_CONST_B, 4u, 3u));
  corpus.push_back(setConstRecord(D9C_COMMAND_RECORD_SET_PS_CONST_F, 16u, 4u));
  corpus.push_back(setConstRecord(D9C_COMMAND_RECORD_SET_PS_CONST_I, 16u, 5u));
  corpus.push_back(setConstRecord(D9C_COMMAND_RECORD_SET_PS_CONST_B, 4u, 6u));

  D9CCommandRecordDrawPrimitive draw{};
  draw.header = {D9C_COMMAND_RECORD_DRAW_PRIMITIVE, sizeof(draw)};
  draw.packet.renderStateCount = 1u;
  draw.packet.renderStates[0] = {7u, 9u};
  draw.packet.textureMask = 1u;
  draw.packet.textures[0] = wire(&srcTexture);
  draw.packet.streamSourceMask = 1u;
  draw.packet.streamSources[0] = {wire(&vertexBuffer), 4u, 12u};
  draw.packet.vsValid = draw.packet.psValid = 1u;
  draw.packet.vsHandle = wire(&vertexShader);
  draw.packet.psHandle = wire(&pixelShader);
  draw.packet.fvfValid = draw.packet.vdeclValid = 1u;
  draw.packet.fvf = 0x112u;
  draw.packet.vdeclHandle = wire(&declaration);
  draw.packet.rtMask = 1u;
  draw.packet.rtHandles[0] = wire(&dstSurface);
  draw.packet.dsValid = 1u;
  draw.packet.dsHandle = wire(&srcSurface);
  draw.packet.viewportValid = 1u;
  draw.packet.viewport = {1u, 2u, 640u, 480u, 0.0f, 1.0f};
  draw.packet.scissorValid = 1u;
  draw.packet.scissor = {1, 2, 20, 30};
  draw.packet.tssCount = 1u;
  draw.packet.tss[0] = {0u, 1u, 2u};
  draw.packet.samplerStateCount = 1u;
  draw.packet.samplerStates[0] = {0u, 2u, 3u};
  draw.packet.materialValid = 1u;
  draw.packet.material.diffuse.r = 0.5f;
  draw.packet.clipPlaneMask = 1u;
  draw.packet.clipPlanes[0] = 1.0f;
  draw.packet.transformCount = 1u;
  draw.packet.transforms[0].state = 2u;
  draw.packet.transforms[0].matrix.m[0] = 1.0f;
  draw.packet.lightSlotMask = 1u;
  draw.packet.lights[0].type = 1u;
  draw.packet.lightEnableValidMask = draw.packet.lightEnableMask = 1u;
  draw.packet.primitiveType = 4u;
  draw.packet.startVertex = 3u;
  draw.packet.primitiveCount = 1u;
  draw.packet.constDeltaSections[D9C_DRAW_PACKET_CONST_DELTA_VS_F] = {
      1u, 2u, 1u};
  draw.header.size = sizeof(draw) + 16u;
  std::vector<std::byte> drawBytes(draw.header.size);
  std::memcpy(drawBytes.data(), &draw, sizeof(draw));
  for (std::uint32_t i = 0u; i < 16u; ++i)
    drawBytes[sizeof(draw) + i] = std::byte(0x40u + i);
  corpus.push_back(std::move(drawBytes));

  D9CCommandRecordDrawPrimitive carry{};
  carry.header = {D9C_COMMAND_RECORD_DRAW_PRIMITIVE, sizeof(carry)};
  carry.packet.primitiveType = 4u;
  carry.packet.startVertex = 6u;
  carry.packet.primitiveCount = 1u;
  corpus.push_back(recordBytes(carry));

  D9CCommandRecordDrawIndexedPrimitive indexed{};
  indexed.header = {D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE, sizeof(indexed)};
  indexed.packet.state.primitiveType = 4u;
  indexed.packet.baseVertex = -2;
  indexed.packet.minVertex = 1u;
  indexed.packet.numVertices = 3u;
  indexed.packet.startIndex = 2u;
  indexed.packet.primitiveCount = 1u;
  indexed.packet.ibValid = 1u;
  indexed.packet.ibHandle = wire(&indexBuffer);
  corpus.push_back(recordBytes(indexed));

  D9CCommandRecordDrawPrimitiveUP up{};
  up.header = {D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP,
               sizeof(up) + 12u};
  up.packet.state.primitiveType = 4u;
  up.packet.primitiveCount = 1u;
  up.packet.stride = 4u;
  up.packet.vertexDataOffset = sizeof(up);
  up.packet.vertexDataSize = 12u;
  auto upBytes = recordBytes(up);
  upBytes.resize(up.header.size);
  for (std::uint32_t i = 0u; i < 12u; ++i)
    upBytes[sizeof(up) + i] = std::byte(0x20u + i);
  corpus.push_back(std::move(upBytes));

  D9CCommandRecordDrawIndexedPrimitiveUP indexedUp{};
  indexedUp.header = {D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP,
                      sizeof(indexedUp) + 18u};
  indexedUp.packet.state.primitiveType = 4u;
  indexedUp.packet.minVertex = 0u;
  indexedUp.packet.numVertices = 3u;
  indexedUp.packet.primitiveCount = 1u;
  indexedUp.packet.indexFormat = 101u;
  indexedUp.packet.stride = 4u;
  indexedUp.packet.indexDataOffset = sizeof(indexedUp);
  indexedUp.packet.indexDataSize = 6u;
  indexedUp.packet.vertexDataOffset = sizeof(indexedUp) + 6u;
  indexedUp.packet.vertexDataSize = 12u;
  auto indexedUpBytes = recordBytes(indexedUp);
  indexedUpBytes.resize(indexedUp.header.size);
  for (std::uint32_t i = 0u; i < 18u; ++i)
    indexedUpBytes[sizeof(indexedUp) + i] = std::byte(0x40u + i);
  corpus.push_back(std::move(indexedUpBytes));

  D9CCommandRecordApplyState apply{};
  apply.header = {D9C_COMMAND_RECORD_APPLY_STATE, sizeof(apply)};
  apply.packet.textureMask = 1u;
  apply.packet.streamSourceMask = 1u;
  apply.packet.fvfValid = 1u;
  apply.packet.fvf = 0x002u;
  apply.packet.rtMask = 1u;
  apply.packet.dsValid = 1u;
  corpus.push_back(recordBytes(apply));

  D9CCommandRecordDrawPrimitive snapshot{};
  snapshot.header = {D9C_COMMAND_RECORD_DRAW_PRIMITIVE, sizeof(snapshot)};
  snapshot.packet.textureMask = 0xfffffu;
  snapshot.packet.streamSourceMask = 0xffffu;
  snapshot.packet.fvfValid = snapshot.packet.vdeclValid = 1u;
  snapshot.packet.fvf = 0x112u;
  snapshot.packet.vdeclHandle = wire(&declaration);
  snapshot.packet.primitiveType = 4u;
  snapshot.packet.primitiveCount = 1u;
  corpus.push_back(recordBytes(snapshot));

  D9CCommandRecordClear clear{};
  clear.header = {D9C_COMMAND_RECORD_CLEAR,
                  sizeof(clear) + sizeof(D9CRect)};
  clear.flags = 3u;
  clear.colorARGB = 0xff123456u;
  clear.z = 0.75f;
  clear.stencil = 4u;
  clear.rectCount = 1u;
  clear.rectOffset = sizeof(clear);
  auto clearBytes = recordBytes(clear);
  clearBytes.resize(clear.header.size);
  const D9CRect clearRect{1, 2, 3, 4};
  std::memcpy(clearBytes.data() + sizeof(clear), &clearRect, sizeof(clearRect));
  corpus.push_back(std::move(clearBytes));

  D9CCommandRecordPresent present{};
  present.header = {D9C_COMMAND_RECORD_PRESENT, sizeof(present)};
  present.hwnd = 0x1234u;
  present.flags = 2u;
  present.hasSrc = present.hasDst = 1u;
  present.src = {1, 2, 3, 4};
  present.dst = {5, 6, 7, 8};
  corpus.push_back(recordBytes(present));

  D9CCommandRecordStretchRect stretch{};
  stretch.header = {D9C_COMMAND_RECORD_STRETCH_RECT, sizeof(stretch)};
  stretch.srcWire = reinterpret_cast<std::uintptr_t>(&srcSurface);
  stretch.dstWire = reinterpret_cast<std::uintptr_t>(&dstSurface);
  stretch.hasSrcRect = stretch.hasDstRect = 1u;
  stretch.filter = 2u;
  stretch.srcRect = {1, 2, 3, 4};
  stretch.dstRect = {5, 6, 7, 8};
  corpus.push_back(recordBytes(stretch));

  D9CCommandRecordColorFill fill{};
  fill.header = {D9C_COMMAND_RECORD_COLOR_FILL, sizeof(fill)};
  fill.surfaceWire = reinterpret_cast<std::uintptr_t>(&dstSurface);
  fill.colorARGB = 0xffabcdefu;
  fill.hasRect = 1u;
  fill.rect = {1, 2, 3, 4};
  corpus.push_back(recordBytes(fill));

  D9CCommandRecordUpdateTexture updateTexture{};
  updateTexture.header = {D9C_COMMAND_RECORD_UPDATE_TEXTURE,
                          sizeof(updateTexture)};
  updateTexture.srcWire = reinterpret_cast<std::uintptr_t>(&srcTexture);
  updateTexture.dstWire = reinterpret_cast<std::uintptr_t>(&dstTexture);
  corpus.push_back(recordBytes(updateTexture));

  D9CCommandRecordUpdateSurface updateSurface{};
  updateSurface.header = {D9C_COMMAND_RECORD_UPDATE_SURFACE,
                          sizeof(updateSurface)};
  updateSurface.srcWire = reinterpret_cast<std::uintptr_t>(&srcSurface);
  updateSurface.dstWire = reinterpret_cast<std::uintptr_t>(&dstSurface);
  updateSurface.hasSrcRect = updateSurface.hasDstPoint = 1u;
  updateSurface.srcRect = {1, 2, 3, 4};
  updateSurface.dstPoint = {5, 6, 5, 6};
  corpus.push_back(recordBytes(updateSurface));

  D9CCommandRecordQueryIssue issue{};
  issue.header = {D9C_COMMAND_RECORD_QUERY_ISSUE, sizeof(issue)};
  issue.queryWire = reinterpret_cast<std::uintptr_t>(&query);
  issue.flags = 1u;
  corpus.push_back(recordBytes(issue));

  D9CCommandRecordReadback readback{};
  readback.header = {D9C_COMMAND_RECORD_READBACK, sizeof(readback)};
  readback.srcWire = reinterpret_cast<std::uintptr_t>(&srcSurface);
  readback.dstWire = reinterpret_cast<std::uintptr_t>(&dstSurface);
  corpus.push_back(recordBytes(readback));

  D9CCommandRecordReszDepthResolve resz{};
  resz.header = {D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE, sizeof(resz)};
  resz.msaaDepthHandle = reinterpret_cast<std::uintptr_t>(&srcSurface);
  resz.intzDestHandle = reinterpret_cast<std::uintptr_t>(&dstTexture);
  corpus.push_back(recordBytes(resz));

  std::vector<Event> v1Trace;
  CommandChunkV2Builder builder;
  for (const auto& record : corpus) {
    replayLegacy(record, v1Trace);
    check(dxmt9::d3d9::pe::appendLegacyCommandRecordAsV2(builder, record),
          "complete V1 opcode corpus converts to V2");
  }
  const auto sealed = builder.seal();
  ImportedChunkV2View imported;
  const V2ChunkEnvelope envelope{
      .version = D9C_COMMAND_CHUNK_VERSION_V2,
      .recordCount = sealed.recordCount,
      .handleCount = sealed.handleCount,
  };
  check(dxmt9::d3d9::validateCommandChunkV2(sealed.blob, envelope,
                                             &imported).valid(),
        "complete converted V2 corpus validates");

  std::vector<void*> inlineObjects(imported.handles.size());
  std::uint32_t inlineRetains = 0u;
  const auto retain = +[](std::uint32_t, void* object) noexcept {
    ++static_cast<FakeObject*>(object)->refs;
  };
  check(registry.resolveAndRetain(imported.handles, inlineObjects, retain),
        "inline V2 corpus resolves transactionally");
  inlineRetains = static_cast<std::uint32_t>(inlineObjects.size());
  std::vector<Event> inlineTrace;
  replayV2(imported, inlineObjects, inlineTrace);
  for (auto* object : inlineObjects) --static_cast<FakeObject*>(object)->refs;
  check(inlineRetains == imported.handles.size() &&
            tracesEqual(v1Trace, inlineTrace, "inline"),
        "V1 and inline V2 produce identical normalized effects");

  dxmt9::d3d9::RawCommandChunk raw;
  check(dxmt9::d3d9::prepareV2OffloadChunk(
            sealed.blob, envelope, registry, retain, raw),
        "offload V2 corpus owns resolved objects before replay");
  ImportedChunkV2View ownedImported;
  const auto ownedBytes = std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(raw.recordBlob.data()),
      raw.recordBlob.size());
  check(dxmt9::d3d9::validateCommandChunkV2(
            ownedBytes, envelope, &ownedImported).valid(),
        "offload immutable V2 copy validates");
  std::vector<Event> offloadTrace;
  replayV2(ownedImported, raw.resolvedObjects, offloadTrace);
  for (const auto& retained : raw.retainedWrappers)
    --static_cast<FakeObject*>(retained.ptr)->refs;
  raw.retainedWrappers.clear();
  raw.resolvedObjects.clear();
  check(tracesEqual(v1Trace, offloadTrace, "offload"),
        "V1 and offloaded V2 produce identical normalized effects");

  builder.reset();
  for (const auto& ref : refs) {
    dxmt9::d3d9::pe::unpublishCachedWireObjectRef(ref);
    check(registry.erase(ref.identity, ref.object),
          "parity fixture unregisters every object");
  }
}

}  // namespace

int main() {
  try {
    testCompleteParityCorpus();
  } catch (const TestFailure& error) {
    std::cerr << "chunk_record_v1_v2_equivalence_spec failed: "
              << error.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "chunk_record_v1_v2_equivalence_spec passed\n";
  return EXIT_SUCCESS;
}
