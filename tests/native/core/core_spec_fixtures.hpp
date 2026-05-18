#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "dxmt9/com.hpp"
#include "dxmt9/core.hpp"
#include "dxmt9/winemetal.h"
#include "device_c_common.hpp"
#include "../../../src/dxmt9/dxmt9_format_convert.hpp"
#include "../../../src/dxmt9/dxmt9_draw_shader.hpp"
#include "../../../src/dxmt9/dxmt9_draw_state.hpp"
#include "../../../src/dxmt9/dxmt9_ring_arena.hpp"
#include "../../../src/dxmt9/dxmt9_shader_translator.hpp"

// All helpers are placed in dxmt9::core::spec; nested name lookup resolves
// types from dxmt9::core automatically. Spec files should bring fixture
// symbols in with `using namespace dxmt9::core::fixture;` after including.
namespace dxmt9::core::spec {

using namespace ::dxmt9::core::fixture;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[noreturn]] inline void fail(std::string message) {
  throw TestFailure(std::move(message));
}

inline void check(bool condition, std::string_view message) {
  if (!condition) {
    fail(std::string(message));
  }
}

template <typename A, typename B>
void checkEq(const A& left, const B& right, std::string_view message) {
  if (!(left == right)) {
    std::ostringstream out;
    out << message;
    out << " (left != right)";
    fail(out.str());
  }
}

inline void checkNear(float left, float right, float epsilon, std::string_view message) {
  if (std::fabs(left - right) > epsilon) {
    std::ostringstream out;
    out << message << " (" << left << " vs " << right << ")";
    fail(out.str());
  }
}

inline void checkBytes(std::span<const u8> actual, std::span<const u8> expected, std::string_view message) {
  if (actual.size() < expected.size() || !std::equal(expected.begin(), expected.end(), actual.begin())) {
    std::ostringstream out;
    out << message;
    fail(out.str());
  }
}

inline void checkContains(std::string_view haystack, std::string_view needle, std::string_view message) {
  if (haystack.find(needle) == std::string_view::npos) {
    std::ostringstream out;
    out << message << " (missing '" << needle << "')";
    fail(out.str());
  }
}

inline void checkNotContains(std::string_view haystack, std::string_view needle, std::string_view message) {
  if (haystack.find(needle) != std::string_view::npos) {
    std::ostringstream out;
    out << message << " (unexpected '" << needle << "')";
    fail(out.str());
  }
}

inline bool getenvFlag(const char* name) {
  const char* value = std::getenv(name);
  return value && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

inline std::string shaderSourceToString(dxmt9_u64 shaderHandle) {
  const char* source = dxmt9_winemetal_shader_source(shaderHandle);
  check(source != nullptr, "shader source");
  const dxmt9_u64 length = dxmt9_winemetal_shader_source_size(shaderHandle);
  check(length != 0, "shader source size");
  return std::string(source, static_cast<size_t>(length));
}

inline std::array<u8, 4> bgra(u8 b, u8 g, u8 r, u8 a) {
  return {b, g, r, a};
}

inline std::array<u8, 4> readPixel(const LockedRegion& region, u32 x, u32 y) {
  const auto* bytes = static_cast<const u8*>(region.data);
  const size_t offset = static_cast<size_t>(y) * region.pitch + static_cast<size_t>(x) * 4u;
  return {bytes[offset + 0], bytes[offset + 1], bytes[offset + 2], bytes[offset + 3]};
}

struct ScreenSpaceTexturedVertex {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float rhw = 1.0f;
  float u = 0.0f;
  float v = 0.0f;
};

struct TextureUploadRecord {
  TextureHandle handle{};
  u32 level = 0;
  u32 width = 0;
  u32 height = 0;
  u32 depth = 1;
  u32 pitch = 0;
  u32 slicePitch = 0;
  std::vector<u8> bytes;
};

struct TextureSurfaceRecord {
  TextureHandle texture{};
  u32 subresource = 0;
  SurfaceDesc desc{};
};

struct RecordedDraw {
  CanonicalDrawState state{};
  DrawUniformPayload uniforms{};
  FlatDrawStateRecord hot{};
  DrawParam param{};
  std::vector<u8> payloadArena;
};

struct RecordedDrawRun {
  CanonicalDrawState state{};
  DrawUniformPayload uniforms{};
  FlatDrawStateRecord hot{};
  std::vector<DrawParam> draws;
  std::vector<u8> payloadArena;
};

inline std::span<const u8> payloadSlice(const RecordedDraw& draw, DrawPayloadRange range,
                                        std::string_view message) {
  if (range.empty()) {
    return {};
  }
  const auto offset = static_cast<size_t>(range.offset);
  const auto size = static_cast<size_t>(range.size);
  if (offset > draw.payloadArena.size() || size > draw.payloadArena.size() - offset) {
    fail(std::string(message));
  }
  return std::span<const u8>(draw.payloadArena.data() + offset, size);
}

constexpr u32 kD3DSIO_MOV = 1u;
constexpr u32 kD3DSIO_ADD = 2u;
constexpr u32 kD3DSIO_SLT = 12u;
constexpr u32 kD3DSIO_SGE = 13u;
constexpr u32 kD3DSIO_EXP = 14u;
constexpr u32 kD3DSIO_LOG = 15u;
constexpr u32 kD3DSIO_M4x4 = 20u;
constexpr u32 kD3DSIO_M4x3 = 21u;
constexpr u32 kD3DSIO_M3x2 = 24u;
constexpr u32 kD3DSIO_CALL = 25u;
constexpr u32 kD3DSIO_LOOP = 27u;
constexpr u32 kD3DSIO_RET = 28u;
constexpr u32 kD3DSIO_ENDLOOP = 29u;
constexpr u32 kD3DSIO_LABEL = 30u;
constexpr u32 kD3DSIO_DCL = 31u;
constexpr u32 kD3DSIO_DEF = 81u;
constexpr u32 kD3DSIO_SINCOS = 37u;
constexpr u32 kD3DSIO_REP = 38u;
constexpr u32 kD3DSIO_ENDREP = 39u;
constexpr u32 kD3DSIO_IF = 40u;
constexpr u32 kD3DSIO_ELSE = 42u;
constexpr u32 kD3DSIO_ENDIF = 43u;
constexpr u32 kD3DSIO_MOVA = 46u;
constexpr u32 kD3DSIO_POW = 32u;
constexpr u32 kD3DSIO_SETP = 94u;
constexpr u32 kD3DSIO_END = 0xffffu;

constexpr u32 kD3DSPR_TEMP = 0u;
constexpr u32 kD3DSPR_CONST = 2u;
constexpr u32 kD3DSPR_ADDR = 3u;
constexpr u32 kD3DSPR_RASTOUT = 4u;
constexpr u32 kD3DSPR_TEXCRDOUT = 6u;
constexpr u32 kD3DSPR_COLOROUT = 8u;
constexpr u32 kD3DSPR_PREDICATE = 19u;
constexpr u32 kFvfXyzrhw = 0x0004u;
constexpr u32 kFvfTex1 = 0x0100u;
constexpr u32 kTextureAddressClamp = 3u;
constexpr u32 kTextureAddressBorder = 4u;
constexpr u32 kD3DDeclUsagePosition = 0u;
constexpr u32 kD3DDeclUsageTexcoord = 5u;
constexpr u32 kD3DDeclUsageColor = 10u;

inline u32 encodeRegisterType(u32 regType) {
  return ((regType & 0x7u) << 28) | (((regType >> 3) & 0x3u) << 11);
}

inline u32 makeVersionToken(bool vertex, u32 major, u32 minor) {
  return ((vertex ? 0xfffeu : 0xffffu) << 16) | ((major & 0xffu) << 8) | (minor & 0xffu);
}

inline u32 makeInstructionToken(u32 opcode, u32 operandCount) {
  return (opcode & 0xffffu) | ((operandCount & 0xfu) << 24);
}

inline u32 makeDstToken(u32 regType, u32 regIndex, u32 writeMask = 0xfu, u32 modifier = 0u) {
  return (1u << 31) | encodeRegisterType(regType) | ((modifier & 0xfu) << 20) | ((writeMask & 0xfu) << 16) |
         (regIndex & 0x7ffu);
}

inline u32 makeSrcToken(u32 regType, u32 regIndex, u32 swizzle = 0xe4u, u32 modifier = 0u) {
  return (1u << 31) | encodeRegisterType(regType) | ((modifier & 0xfu) << 24) | ((swizzle & 0xffu) << 16) |
         (regIndex & 0x7ffu);
}

inline u32 makeDclSemanticToken(u32 usage, u32 usageIndex = 0u) {
  return (1u << 31) | (usage & 0xfu) | ((usageIndex & 0xfu) << 16);
}

inline u32 makeLabelToken(u32 label) {
  return (1u << 31) | (label & 0x7ffu);
}

inline std::vector<u32> makeVertexBytecode() {
  std::vector<u32> words;
  words.push_back(makeVersionToken(true, 2, 0));
  words.push_back(makeInstructionToken(kD3DSIO_DEF, 5));
  words.push_back(makeDstToken(kD3DSPR_CONST, 0));
  words.push_back(std::bit_cast<u32>(1.0f));
  words.push_back(std::bit_cast<u32>(2.0f));
  words.push_back(std::bit_cast<u32>(3.0f));
  words.push_back(std::bit_cast<u32>(4.0f));
  words.push_back(makeInstructionToken(kD3DSIO_MOV, 2));
  words.push_back(makeDstToken(kD3DSPR_RASTOUT, 0));
  words.push_back(makeSrcToken(kD3DSPR_CONST, 0));
  words.push_back(kD3DSIO_END);
  return words;
}

inline std::vector<u32> makeVertexTexcoordBytecode() {
  std::vector<u32> words;
  words.push_back(makeVersionToken(true, 2, 0));
  words.push_back(makeInstructionToken(kD3DSIO_DEF, 5));
  words.push_back(makeDstToken(kD3DSPR_CONST, 0));
  words.push_back(std::bit_cast<u32>(1.0f));
  words.push_back(std::bit_cast<u32>(2.0f));
  words.push_back(std::bit_cast<u32>(3.0f));
  words.push_back(std::bit_cast<u32>(4.0f));
  words.push_back(makeInstructionToken(kD3DSIO_MOV, 2));
  words.push_back(makeDstToken(kD3DSPR_RASTOUT, 0));
  words.push_back(makeSrcToken(kD3DSPR_CONST, 0));
  words.push_back(makeInstructionToken(kD3DSIO_MOV, 2));
  words.push_back(makeDstToken(kD3DSPR_TEXCRDOUT, 1));
  words.push_back(makeSrcToken(kD3DSPR_CONST, 0));
  words.push_back(kD3DSIO_END);
  return words;
}

inline std::vector<u32> makePixelBytecode() {
  std::vector<u32> words;
  words.push_back(makeVersionToken(false, 2, 0));
  words.push_back(makeInstructionToken(kD3DSIO_DEF, 5));
  words.push_back(makeDstToken(kD3DSPR_CONST, 0));
  words.push_back(std::bit_cast<u32>(0.25f));
  words.push_back(std::bit_cast<u32>(0.5f));
  words.push_back(std::bit_cast<u32>(0.75f));
  words.push_back(std::bit_cast<u32>(1.0f));
  words.push_back(makeInstructionToken(kD3DSIO_ADD, 3));
  words.push_back(makeDstToken(kD3DSPR_TEMP, 0));
  words.push_back(makeSrcToken(kD3DSPR_CONST, 0));
  words.push_back(makeSrcToken(kD3DSPR_CONST, 0));
  words.push_back(makeInstructionToken(kD3DSIO_MOV, 2));
  words.push_back(makeDstToken(kD3DSPR_COLOROUT, 0));
  words.push_back(makeSrcToken(kD3DSPR_TEMP, 0));
  words.push_back(kD3DSIO_END);
  return words;
}

inline std::vector<u32> makePixelMrtBytecode() {
  std::vector<u32> words;
  words.push_back(makeVersionToken(false, 2, 0));
  words.push_back(makeInstructionToken(kD3DSIO_DEF, 5));
  words.push_back(makeDstToken(kD3DSPR_CONST, 0));
  words.push_back(std::bit_cast<u32>(1.0f));
  words.push_back(std::bit_cast<u32>(0.0f));
  words.push_back(std::bit_cast<u32>(0.0f));
  words.push_back(std::bit_cast<u32>(1.0f));
  words.push_back(makeInstructionToken(kD3DSIO_DEF, 5));
  words.push_back(makeDstToken(kD3DSPR_CONST, 1));
  words.push_back(std::bit_cast<u32>(0.0f));
  words.push_back(std::bit_cast<u32>(1.0f));
  words.push_back(std::bit_cast<u32>(0.0f));
  words.push_back(std::bit_cast<u32>(1.0f));
  words.push_back(makeInstructionToken(kD3DSIO_MOV, 2));
  words.push_back(makeDstToken(kD3DSPR_COLOROUT, 0));
  words.push_back(makeSrcToken(kD3DSPR_CONST, 0));
  words.push_back(makeInstructionToken(kD3DSIO_MOV, 2));
  words.push_back(makeDstToken(kD3DSPR_COLOROUT, 1));
  words.push_back(makeSrcToken(kD3DSPR_CONST, 1));
  words.push_back(kD3DSIO_END);
  return words;
}

inline std::vector<u32> makeTexturedPixelShaderBytecode(u32 samplerIndex = 0) {
  // ps_2_0:
  //   dcl t0.xy
  //   dcl_2d sN
  //   texld r0, t0, sN
  //   mov oC0, r0
  return {
      0xffff0200u,
      0x0200001fu, 0x80000000u, 0xb0030000u,
      0x0200001fu, 0x90000000u, 0xa00f0800u | (samplerIndex & 0x7ffu),
      0x03000042u, 0x800f0000u, 0xb0e40000u, 0xa0e40800u | (samplerIndex & 0x7ffu),
      0x02000001u, 0x800f0800u, 0x80e40000u,
      0x0000ffffu,
  };
}

inline std::vector<u32> makeControlFlowBytecode() {
  std::vector<u32> words;
  words.push_back(makeVersionToken(true, 3, 0));

  auto appendDef = [&](u32 index, float x, float y, float z, float w) {
    words.push_back(makeInstructionToken(kD3DSIO_DEF, 5));
    words.push_back(makeDstToken(kD3DSPR_CONST, index));
    words.push_back(std::bit_cast<u32>(x));
    words.push_back(std::bit_cast<u32>(y));
    words.push_back(std::bit_cast<u32>(z));
    words.push_back(std::bit_cast<u32>(w));
  };

  appendDef(0, 1.0f, 2.0f, 3.0f, 4.0f);
  appendDef(1, 2.0f, 2.0f, 2.0f, 2.0f);
  appendDef(2, 3.0f, 3.0f, 3.0f, 3.0f);
  appendDef(3, 0.5f, 0.25f, 0.75f, 1.0f);
  appendDef(10, 1.0f, 0.0f, 0.0f, 0.0f);
  appendDef(11, 0.0f, 1.0f, 0.0f, 0.0f);
  appendDef(12, 0.0f, 0.0f, 1.0f, 0.0f);
  appendDef(13, 0.0f, 0.0f, 0.0f, 1.0f);
  appendDef(14, 4.0f, 3.0f, 2.0f, 1.0f);
  appendDef(15, 1.0f, 0.0f, 1.0f, 0.0f);
  appendDef(16, 0.0f, 1.0f, 0.0f, 1.0f);
  appendDef(17, 2.0f, 2.0f, 2.0f, 2.0f);
  appendDef(18, 3.0f, 3.0f, 3.0f, 3.0f);
  appendDef(19, 4.0f, 4.0f, 4.0f, 4.0f);

  words.push_back(makeInstructionToken(kD3DSIO_MOVA, 2));
  words.push_back(makeDstToken(kD3DSPR_ADDR, 0));
  words.push_back(makeSrcToken(kD3DSPR_CONST, 0));

  words.push_back(makeInstructionToken(kD3DSIO_IF, 1));
  words.push_back(makeSrcToken(kD3DSPR_CONST, 0));
  words.push_back(makeInstructionToken(kD3DSIO_MOV, 2));
  words.push_back(makeDstToken(kD3DSPR_TEMP, 0));
  words.push_back(makeSrcToken(kD3DSPR_CONST, 0));
  words.push_back(makeInstructionToken(kD3DSIO_ELSE, 0));
  words.push_back(makeInstructionToken(kD3DSIO_MOV, 2));
  words.push_back(makeDstToken(kD3DSPR_TEMP, 0));
  words.push_back(makeSrcToken(kD3DSPR_CONST, 1));
  words.push_back(makeInstructionToken(kD3DSIO_ENDIF, 0));

  words.push_back(makeInstructionToken(kD3DSIO_LOOP, 1));
  words.push_back(makeSrcToken(kD3DSPR_CONST, 1));
  words.push_back(makeInstructionToken(kD3DSIO_MOV, 2));
  words.push_back(makeDstToken(kD3DSPR_TEMP, 1));
  words.push_back(makeSrcToken(kD3DSPR_CONST, 2));
  words.push_back(makeInstructionToken(kD3DSIO_ENDLOOP, 0));

  words.push_back(makeInstructionToken(kD3DSIO_REP, 1));
  words.push_back(makeSrcToken(kD3DSPR_CONST, 2));
  words.push_back(makeInstructionToken(kD3DSIO_MOV, 2));
  words.push_back(makeDstToken(kD3DSPR_TEMP, 2));
  words.push_back(makeSrcToken(kD3DSPR_CONST, 3));
  words.push_back(makeInstructionToken(kD3DSIO_ENDREP, 0));

  words.push_back(makeInstructionToken(kD3DSIO_CALL, 1));
  words.push_back(makeLabelToken(7));
  words.push_back(makeInstructionToken(kD3DSIO_SETP, 2));
  words.push_back(makeDstToken(kD3DSPR_PREDICATE, 0));
  words.push_back(makeSrcToken(kD3DSPR_CONST, 1));
  words.push_back(makeInstructionToken(kD3DSIO_LABEL, 1));
  words.push_back(makeLabelToken(7));
  words.push_back(makeInstructionToken(kD3DSIO_SINCOS, 2));
  words.push_back(makeDstToken(kD3DSPR_TEMP, 3));
  words.push_back(makeSrcToken(kD3DSPR_CONST, 3));
  words.push_back(makeInstructionToken(kD3DSIO_LOG, 2));
  words.push_back(makeDstToken(kD3DSPR_TEMP, 4));
  words.push_back(makeSrcToken(kD3DSPR_CONST, 3));
  words.push_back(makeInstructionToken(kD3DSIO_EXP, 2));
  words.push_back(makeDstToken(kD3DSPR_TEMP, 5));
  words.push_back(makeSrcToken(kD3DSPR_CONST, 3));
  words.push_back(makeInstructionToken(kD3DSIO_POW, 3));
  words.push_back(makeDstToken(kD3DSPR_TEMP, 5));
  words.push_back(makeSrcToken(kD3DSPR_CONST, 0));
  words.push_back(makeSrcToken(kD3DSPR_CONST, 1));
  words.push_back(makeInstructionToken(kD3DSIO_SLT, 3));
  words.push_back(makeDstToken(kD3DSPR_TEMP, 6));
  words.push_back(makeSrcToken(kD3DSPR_CONST, 3));
  words.push_back(makeSrcToken(kD3DSPR_CONST, 1));
  words.push_back(makeInstructionToken(kD3DSIO_SGE, 3));
  words.push_back(makeDstToken(kD3DSPR_TEMP, 7));
  words.push_back(makeSrcToken(kD3DSPR_CONST, 0));
  words.push_back(makeSrcToken(kD3DSPR_CONST, 1));
  words.push_back(makeInstructionToken(kD3DSIO_M4x4, 3));
  words.push_back(makeDstToken(kD3DSPR_TEMP, 8));
  words.push_back(makeSrcToken(kD3DSPR_CONST, 0));
  words.push_back(makeSrcToken(kD3DSPR_CONST, 10));
  words.push_back(makeInstructionToken(kD3DSIO_M4x3, 3));
  words.push_back(makeDstToken(kD3DSPR_TEMP, 9));
  words.push_back(makeSrcToken(kD3DSPR_CONST, 0));
  words.push_back(makeSrcToken(kD3DSPR_CONST, 14));
  words.push_back(makeInstructionToken(kD3DSIO_M3x2, 3));
  words.push_back(makeDstToken(kD3DSPR_TEMP, 10));
  words.push_back(makeSrcToken(kD3DSPR_CONST, 0));
  words.push_back(makeSrcToken(kD3DSPR_CONST, 18));
  words.push_back(makeInstructionToken(kD3DSIO_RET, 0));

  words.push_back(makeInstructionToken(kD3DSIO_MOV, 2));
  words.push_back(makeDstToken(kD3DSPR_RASTOUT, 0));
  words.push_back(makeSrcToken(kD3DSPR_TEMP, 0));
  words.push_back(kD3DSIO_END);
  return words;
}

struct RecordingBackend final : BackendDevice {
  void setDeviceLostObserver(DeviceLostObserver observer) override {
    deviceLostObserver = std::move(observer);
  }

  void setPresentationStatusObserver(PresentationStatusObserver observer) override {
    presentationStatusObserver = std::move(observer);
  }

  void setMaxFrameLatency(u32 latency) override {
    maxFrameLatency = latency;
    maxFrameLatencyCalls.push_back(latency);
  }

  HResult waitForVBlank(const SwapDesc& desc) override {
    waitForVBlankCalls.push_back(desc);
    return D3D_OK;
  }

  BufferHandle createBuffer(const BufferDesc& desc) override {
    createdBuffers.push_back(desc);
    return Handle{nextHandle++};
  }

  TextureHandle createTexture(const TextureDesc& desc) override {
    createdTextures.push_back(desc);
    return Handle{nextHandle++};
  }

  SurfaceHandle createSurface(const SurfaceDesc& desc) override {
    createdSurfaces.push_back(desc);
    return Handle{nextHandle++};
  }

  SurfaceHandle createSurfaceForTexture(TextureHandle texture, u32 subresource,
                                        const SurfaceDesc& desc) override {
    textureSurfaces.push_back({texture, subresource, desc});
    return Handle{nextHandle++};
  }

  void destroyBuffer(BufferHandle handle) override {
    destroyedBuffers.push_back(handle);
  }

  void destroyTexture(TextureHandle handle) override {
    destroyedTextures.push_back(handle);
  }

  void destroySurface(SurfaceHandle handle) override {
    destroyedSurfaces.push_back(handle);
  }

  void* mapBuffer(BufferHandle handle, u32 flags) override {
    mappedBuffers.push_back({handle, flags});
    return nullptr;
  }

  void unmapBuffer(BufferHandle handle) override {
    unmappedBuffers.push_back(handle);
  }

  void uploadTextureLevel(TextureHandle handle, u32 level, u32 width, u32 height,
                          u32 depth, u32 pitch, u32 slicePitch,
                          std::span<const u8> bytes) override {
    textureUploads.push_back({handle, level, width, height, depth, pitch,
                              slicePitch,
                              std::vector<u8>(bytes.begin(), bytes.end())});
  }

  void submitDrawRun(CanonicalDrawState state, const DrawUniformPayload& uniforms,
                     std::span<const DrawParam> submittedDraws,
                     std::span<const DrawParamPayloadView> payloads) override {
    RecordedDrawRun run{};
    run.state = std::move(state);
    run.uniforms = uniforms;
    run.hot = run.state.hot;
    run.draws.reserve(submittedDraws.size());

    auto appendPayload = [&](std::span<const u8> bytes) -> DrawPayloadRange {
      if (bytes.empty()) {
        return {};
      }
      const auto offset = static_cast<u32>(run.payloadArena.size());
      run.payloadArena.insert(run.payloadArena.end(), bytes.begin(), bytes.end());
      return DrawPayloadRange{
          .offset = offset,
          .size = static_cast<u32>(bytes.size()),
      };
    };
    for (std::size_t i = 0; i < submittedDraws.size(); ++i) {
      DrawParam param = submittedDraws[i];
      const DrawParamPayloadView payload = i < payloads.size() ? payloads[i] : DrawParamPayloadView{};
      param.userVertexRange = appendPayload(payload.userVertexData);
      param.userIndexRange = appendPayload(payload.userIndexData);
      run.draws.push_back(param);
    }

    for (const auto& param : run.draws) {
      RecordedDraw draw{};
      draw.state = run.state;
      draw.uniforms = run.uniforms;
      draw.hot = run.hot;
      draw.param = param;
      draw.payloadArena = run.payloadArena;
      draws.push_back(std::move(draw));
    }

    drawRuns.push_back(std::move(run));
  }

  void submitClear(const ClearDesc& desc) override {
    clears.push_back(desc);
  }

  void submitSurfaceCopy(const SurfaceCopyDesc& desc) override {
    surfaceCopies.push_back(desc);
  }

  void submitStretchRect(const StretchRectDesc& desc) override {
    stretchRects.push_back(desc);
  }

  void submitReadback(const ReadbackDesc& desc) override {
    readbacks.push_back(desc);
  }

  void submitColorFill(const ColorFillDesc& desc) override {
    colorFills.push_back(desc);
  }

  void present(const SwapDesc& desc) override {
    presents.push_back(desc);
  }

  void flush() override {
    ++flushCount;
  }

  u64 nextHandle = 1;
  std::vector<BufferDesc> createdBuffers;
  std::vector<TextureDesc> createdTextures;
  std::vector<SurfaceDesc> createdSurfaces;
  std::vector<TextureSurfaceRecord> textureSurfaces;
  std::vector<BufferHandle> destroyedBuffers;
  std::vector<TextureHandle> destroyedTextures;
  std::vector<SurfaceHandle> destroyedSurfaces;
  std::vector<std::pair<BufferHandle, u32>> mappedBuffers;
  std::vector<BufferHandle> unmappedBuffers;
  std::vector<TextureUploadRecord> textureUploads;
  std::vector<RecordedDrawRun> drawRuns;
  std::vector<RecordedDraw> draws;
  std::vector<ClearDesc> clears;
  std::vector<SurfaceCopyDesc> surfaceCopies;
  std::vector<StretchRectDesc> stretchRects;
  std::vector<ReadbackDesc> readbacks;
  std::vector<ColorFillDesc> colorFills;
  std::vector<SwapDesc> presents;
  u32 flushCount = 0;
  DeviceLostObserver deviceLostObserver;
  PresentationStatusObserver presentationStatusObserver;
  u32 maxFrameLatency = kDefaultFrameLatency;
  std::vector<u32> maxFrameLatencyCalls;
  std::vector<SwapDesc> waitForVBlankCalls;

  void triggerDeviceLost(bool lost = true) {
    if (deviceLostObserver) {
      deviceLostObserver(lost);
    }
  }

  void triggerPresentationOccluded(bool occluded = true) {
    if (presentationStatusObserver) {
      presentationStatusObserver(occluded);
    }
  }
};

}  // namespace dxmt9::core::spec
