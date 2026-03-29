#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
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

using namespace dxmt9::core;

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
    out << message;
    out << " (left != right)";
    fail(out.str());
  }
}

void checkNear(float left, float right, float epsilon, std::string_view message) {
  if (std::fabs(left - right) > epsilon) {
    std::ostringstream out;
    out << message << " (" << left << " vs " << right << ")";
    fail(out.str());
  }
}

void checkBytes(std::span<const u8> actual, std::span<const u8> expected, std::string_view message) {
  if (actual.size() < expected.size() || !std::equal(expected.begin(), expected.end(), actual.begin())) {
    std::ostringstream out;
    out << message;
    fail(out.str());
  }
}

void checkContains(std::string_view haystack, std::string_view needle, std::string_view message) {
  if (haystack.find(needle) == std::string_view::npos) {
    std::ostringstream out;
    out << message << " (missing '" << needle << "')";
    fail(out.str());
  }
}

std::string shaderSourceToString(dxmt9_u64 shaderHandle) {
  const char* source = dxmt9_winemetal_shader_source(shaderHandle);
  check(source != nullptr, "shader source");
  const dxmt9_u64 length = dxmt9_winemetal_shader_source_size(shaderHandle);
  check(length != 0, "shader source size");
  return std::string(source, static_cast<size_t>(length));
}

std::array<u8, 4> bgra(u8 b, u8 g, u8 r, u8 a) {
  return {b, g, r, a};
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
constexpr u32 kD3DSIO_DEF = 66u;
constexpr u32 kD3DSIO_SINCOS = 37u;
constexpr u32 kD3DSIO_REP = 38u;
constexpr u32 kD3DSIO_ENDREP = 39u;
constexpr u32 kD3DSIO_IF = 40u;
constexpr u32 kD3DSIO_ELSE = 42u;
constexpr u32 kD3DSIO_ENDIF = 43u;
constexpr u32 kD3DSIO_MOVA = 46u;
constexpr u32 kD3DSIO_POW = 32u;
constexpr u32 kD3DSIO_SETP = 79u;
constexpr u32 kD3DSIO_END = 0xffffu;

constexpr u32 kD3DSPR_TEMP = 0u;
constexpr u32 kD3DSPR_CONST = 2u;
constexpr u32 kD3DSPR_ADDR = 3u;
constexpr u32 kD3DSPR_RASTOUT = 4u;
constexpr u32 kD3DSPR_COLOROUT = 8u;
constexpr u32 kD3DSPR_PREDICATE = 19u;

u32 encodeRegisterType(u32 regType) {
  return ((regType & 0x7u) << 28) | (((regType >> 3) & 0x3u) << 11);
}

u32 makeVersionToken(bool vertex, u32 major, u32 minor) {
  return ((vertex ? 0xfffeu : 0xffffu) << 16) | ((major & 0xffu) << 8) | (minor & 0xffu);
}

u32 makeInstructionToken(u32 opcode, u32 operandCount) {
  return (opcode & 0xffffu) | ((operandCount & 0xfu) << 24);
}

u32 makeDstToken(u32 regType, u32 regIndex, u32 writeMask = 0xfu, u32 modifier = 0u) {
  return (1u << 31) | encodeRegisterType(regType) | ((modifier & 0xfu) << 20) | ((writeMask & 0xfu) << 16) |
         (regIndex & 0x7ffu);
}

u32 makeSrcToken(u32 regType, u32 regIndex, u32 swizzle = 0xe4u, u32 modifier = 0u) {
  return (1u << 31) | encodeRegisterType(regType) | ((modifier & 0xfu) << 24) | ((swizzle & 0xffu) << 16) |
         (regIndex & 0x7ffu);
}

u32 makeLabelToken(u32 label) {
  return (1u << 31) | (label & 0x7ffu);
}

std::vector<u32> makeVertexBytecode() {
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

std::vector<u32> makePixelBytecode() {
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

std::vector<u32> makeControlFlowBytecode() {
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

  void submitDraw(const DrawDesc& desc) override {
    draws.push_back(desc);
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

  u64 nextHandle = 1;
  std::vector<BufferDesc> createdBuffers;
  std::vector<TextureDesc> createdTextures;
  std::vector<SurfaceDesc> createdSurfaces;
  std::vector<BufferHandle> destroyedBuffers;
  std::vector<TextureHandle> destroyedTextures;
  std::vector<SurfaceHandle> destroyedSurfaces;
  std::vector<std::pair<BufferHandle, u32>> mappedBuffers;
  std::vector<BufferHandle> unmappedBuffers;
  std::vector<DrawDesc> draws;
  std::vector<ClearDesc> clears;
  std::vector<SurfaceCopyDesc> surfaceCopies;
  std::vector<StretchRectDesc> stretchRects;
  std::vector<ReadbackDesc> readbacks;
  std::vector<ColorFillDesc> colorFills;
  std::vector<SwapDesc> presents;
  DeviceLostObserver deviceLostObserver;
  PresentationStatusObserver presentationStatusObserver;
  u32 maxFrameLatency = 3;
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

void testFormatAndCaps() {
  const auto* a8r8g8b8 = findFormatInfo(Format::A8R8G8B8);
  check(a8r8g8b8 != nullptr, "A8R8G8B8 format info missing");
  checkEq(a8r8g8b8->backendFormat, BackendPixelFormat::BGRA8Unorm, "A8R8G8B8 backend format");
  checkEq(a8r8g8b8->support, FormatClass::Required, "A8R8G8B8 support class");
  checkEq(a8r8g8b8->bytesPerPixel, 4u, "A8R8G8B8 bytes per pixel");
  check(a8r8g8b8->renderTarget, "A8R8G8B8 should be renderable");

  const auto* l8 = findFormatInfo(Format::L8);
  check(l8 != nullptr, "L8 format info missing");
  check(!l8->renderTarget, "L8 must not be reported as a render target");

  const auto* r8g8b8 = findFormatInfo(Format::R8G8B8);
  check(r8g8b8 != nullptr, "R8G8B8 format info missing");
  checkEq(r8g8b8->support, FormatClass::Unsupported, "R8G8B8 support class");

  BackendLimits limits{};
  limits.maxTextureSize = 4096;
  limits.maxColorAttachments = 2;
  limits.maxAnisotropy = 8;
  limits.supportsBgr10A2 = false;
  limits.supportsDepth32FloatStencil8 = false;

  Factory factory(limits);
  checkEq(factory.adapterCount(), size_t{1}, "adapter count");
  checkEq(factory.caps(0).maxTextureWidth, 4096u, "max texture width");
  checkEq(factory.caps(0).maxTextureHeight, 4096u, "max texture height");
  checkEq(factory.caps(0).numSimultaneousRTs, 2u, "simultaneous RT count");
  checkEq(factory.caps(0).maxAnisotropy, 8u, "max anisotropy");

  checkEq(factory.checkDeviceType(0, DeviceType::Hal, Format::A8R8G8B8, Format::A8R8G8B8, true), D3D_OK,
          "HAL windowed device type");
  checkEq(factory.checkDeviceType(0, DeviceType::Hal, Format::A8R8G8B8, Format::A8R8G8B8, false), D3D_OK,
          "HAL fullscreen device type");
  checkEq(factory.checkDeviceType(0, DeviceType::Ref, Format::A8R8G8B8, Format::A8R8G8B8, true),
          D3DERR_NOTAVAILABLE, "non-HAL device type");
  const auto modes = factory.enumAdapterModes(0, Format::A8R8G8B8);
  check(!modes.empty(), "adapter modes");
  checkEq(modes.front().width, 640u, "first adapter mode width");
  checkEq(modes.front().height, 480u, "first adapter mode height");
  checkEq(modes.front().format, Format::A8R8G8B8, "first adapter mode format");
  const auto displayMode = factory.getAdapterDisplayMode(0);
  checkEq(displayMode.width, 1920u, "adapter display width");
  checkEq(displayMode.height, 1080u, "adapter display height");
  checkEq(displayMode.format, Format::A8R8G8B8, "adapter display format");
  const auto identifier = factory.getAdapterIdentifier(0);
  checkEq(identifier.description, std::string("Adapter 0"), "adapter description");
  checkEq(identifier.monitor, 1u, "adapter monitor");
  checkEq(factory.getAdapterMonitor(0), 1u, "adapter monitor lookup");

  checkEq(factory.checkDeviceFormat(0, Format::A8R8G8B8, UsageTexture), D3D_OK, "A8R8G8B8 texture support");
  checkEq(factory.checkDeviceFormat(0, Format::L8, UsageRenderTarget), D3DERR_NOTAVAILABLE,
          "L8 render-target support");
  checkEq(factory.checkDeviceFormat(0, Format::A2B10G10R10, UsageTexture), D3DERR_NOTAVAILABLE,
          "A2B10G10R10 support gate");
  checkEq(factory.checkDeviceFormat(1, Format::A8R8G8B8, UsageTexture), D3DERR_INVALIDCALL,
          "invalid adapter index");
  checkEq(factory.checkDeviceMultiSampleType(0, Format::A8R8G8B8, MultiSampleType::Four), D3D_OK,
          "4x MSAA support");
  checkEq(factory.checkDeviceMultiSampleType(0, Format::A8R8G8B8, MultiSampleType::Eight), D3DERR_NOTAVAILABLE,
          "8x MSAA support");
}

void testHelpers() {
  const Viewport viewport{0, 0, 800, 600, 0.0f, 1.0f};
  const auto fixup = halfPixelFixup(viewport);
  checkNear(fixup[0], 1.0f / 800.0f, 1.0e-6f, "half-pixel X fixup");
  checkNear(fixup[1], 1.0f / 600.0f, 1.0e-6f, "half-pixel Y fixup");

  const auto zeroFixup = halfPixelFixup(Viewport{});
  checkEq(zeroFixup[0], 0.0f, "zero viewport X fixup");
  checkEq(zeroFixup[1], 0.0f, "zero viewport Y fixup");

  const std::vector<u32> fanIndices{0, 1, 2, 3};
  const auto triangles = decomposeTriangleFanIndices(fanIndices);
  const std::vector<u32> expected{0, 1, 2, 0, 2, 3};
  checkEq(triangles, expected, "triangle fan decomposition");

  const std::vector<u8> upload{0x22, 0x33, 0x44, 0x55};
  const auto converted = convertTextureUpload(Format::A8R8G8B8, 1, 1, upload);
  checkBytes(std::span<const u8>(converted.data(), converted.size()),
             std::span<const u8>(upload.data(), upload.size()), "texture upload conversion");

  check(hashString("dxmt9") != 0, "hashString should not be zero");
}

void testFfpKeys() {
  DeviceState state;
  state.reset();

  state.renderStates[RS_LIGHTING] = 1;
  state.renderStates[RS_SPECULAR_ENABLE] = 1;
  state.renderStates[RS_ALPHA_TEST_ENABLE] = 1;
  state.renderStates[RS_ALPHA_FUNC] = static_cast<u32>(CompareFunc::GreaterEqual);
  state.renderStates[RS_FOG_TABLE_MODE] = static_cast<u32>(FogMode::Exp2);
  state.renderStates[RS_EMISSIVE_MATERIAL_SOURCE] = 3;
  state.renderStates[RS_AMBIENT_MATERIAL_SOURCE] = 2;
  state.renderStates[RS_DIFFUSE_MATERIAL_SOURCE] = 1;
  state.renderStates[RS_SPECULAR_MATERIAL_SOURCE] = 0;
  state.renderStates[RS_VERTEX_BLEND] = 2;

  state.textureStageStates[0][TSS_COLOR_OP] = static_cast<u32>(TextureOp::SelectArg1);
  state.textureStageStates[0][TSS_ALPHA_OP] = static_cast<u32>(TextureOp::Modulate);
  state.textureStageStates[0][TSS_TEXCOORD_INDEX] = 4;
  state.textureStageStates[0][TSS_TEXTURE_TRANSFORM_FLAGS] = 7;

  state.lightEnabled[0] = true;
  state.lights[0].type = LightType::Point;

  const auto vertexKey = makeFfpVertexKey(state);
  check(vertexKey.lightingEnabled, "vertex key lighting");
  check(vertexKey.specularEnabled, "vertex key specular");
  check(vertexKey.lightEnabled[0], "vertex key light enable");
  checkEq(vertexKey.lightType[0], static_cast<u32>(LightType::Point), "vertex key light type");
  checkEq(vertexKey.texCoordGen[0], 4u, "vertex key texcoord");
  checkEq(vertexKey.texTransformFlags[0], 7u, "vertex key transform flags");
  check(vertexKey.hash != 0, "vertex key hash");

  const auto pixelKey = makeFfpPixelKey(state);
  check(pixelKey.alphaTestEnable, "pixel key alpha test");
  checkEq(pixelKey.alphaTestFunc, static_cast<u32>(CompareFunc::GreaterEqual), "pixel key alpha func");
  checkEq(pixelKey.stages[0].colorOp, static_cast<u32>(TextureOp::SelectArg1), "pixel key color op");
  checkEq(pixelKey.stages[0].alphaOp, static_cast<u32>(TextureOp::Modulate), "pixel key alpha op");
  checkEq(pixelKey.stages[0].texCoordIndex, 4u, "pixel key texcoord");
  check(pixelKey.hash != 0, "pixel key hash");
}

void testShaderThunk() {
  const auto vertexWords = makeVertexBytecode();
  const auto pixelWords = makePixelBytecode();

  WinemetalShaderCompileRequest vertexRequest{};
  vertexRequest.kind = WinemetalShaderKind_D3DBytecodeVertex;
  vertexRequest.bytecode = vertexWords.data();
  vertexRequest.bytecodeSize = static_cast<dxmt9_u64>(vertexWords.size() * sizeof(u32));
  vertexRequest.bytecodeHash = hashBytes(std::as_bytes(std::span(vertexWords)));
  vertexRequest.clipPlaneMask = 1u;
  vertexRequest.sampleCount = 4u;
  const auto vertexHandle = dxmt9_winemetal_compile_shader(&vertexRequest);
  check(vertexHandle != 0, "vertex shader thunk");
  const auto vertexSource = shaderSourceToString(vertexHandle);
  checkEq(dxmt9_winemetal_shader_source_size(vertexHandle), static_cast<dxmt9_u64>(vertexSource.size()),
          "vertex shader source size");
  checkContains(vertexSource, "vertex VSOut dxmt9_vs", "vertex shader source");
  checkContains(vertexSource, "def c0", "vertex shader decode comment");
  checkContains(vertexSource, "cFloat[0] = float4(1.0f, 2.0f, 3.0f, 4.0f)", "vertex shader constant define");
  checkContains(vertexSource, "outPosition = cFloat[0]", "vertex shader mov translation");
  checkContains(vertexSource, "clip_distance", "vertex shader clip distance");
  dxmt9_winemetal_destroy_shader(vertexHandle);

  WinemetalShaderCompileRequest pixelRequest{};
  pixelRequest.kind = WinemetalShaderKind_D3DBytecodePixel;
  pixelRequest.bytecode = pixelWords.data();
  pixelRequest.bytecodeSize = static_cast<dxmt9_u64>(pixelWords.size() * sizeof(u32));
  pixelRequest.bytecodeHash = hashBytes(std::as_bytes(std::span(pixelWords)));
  pixelRequest.alphaTestEnable = 1u;
  pixelRequest.alphaTestFunc = static_cast<u32>(CompareFunc::GreaterEqual);
  pixelRequest.alphaRef = 0.5f;
  const auto pixelHandle = dxmt9_winemetal_compile_shader(&pixelRequest);
  check(pixelHandle != 0, "pixel shader thunk");
  const auto pixelSource = shaderSourceToString(pixelHandle);
  checkContains(pixelSource, "fragment float4 dxmt9_fs", "pixel shader source");
  checkContains(pixelSource, "def c0", "pixel shader decode comment");
  checkContains(pixelSource, "add r0, c0, c0", "pixel shader arithmetic comment");
  checkContains(pixelSource, "r[0] = (cFloat[0] + cFloat[0])", "pixel shader arithmetic translation");
  checkContains(pixelSource, "discard_fragment()", "pixel shader alpha test");
  dxmt9_winemetal_destroy_shader(pixelHandle);

  DeviceState state;
  state.reset();
  state.renderStates[RS_LIGHTING] = 1;
  state.renderStates[RS_SPECULAR_ENABLE] = 1;
  state.renderStates[RS_ALPHA_TEST_ENABLE] = 1;
  state.renderStates[RS_ALPHA_FUNC] = static_cast<u32>(CompareFunc::GreaterEqual);
  state.renderStates[RS_ALPHA_REF] = 128;
  state.renderStates[RS_FOG_TABLE_MODE] = static_cast<u32>(FogMode::Exp2);
  state.renderStates[RS_CLIP_PLANE_ENABLE] = 1;
  state.lightEnabled[0] = true;
  state.lights[0].type = LightType::Point;
  state.textureStageStates[0][TSS_COLOR_OP] = static_cast<u32>(TextureOp::SelectArg1);
  state.textureStageStates[0][TSS_ALPHA_OP] = static_cast<u32>(TextureOp::Modulate);
  state.textureStageStates[0][TSS_TEXCOORD_INDEX] = 4;
  state.textureStageStates[0][TSS_TEXTURE_TRANSFORM_FLAGS] = 7;

  const auto vertexKey = makeFfpVertexKey(state);
  WinemetalShaderCompileRequest ffpVertexRequest{};
  ffpVertexRequest.kind = WinemetalShaderKind_FfpVertex;
  ffpVertexRequest.variantKey = &vertexKey;
  ffpVertexRequest.textured = true;
  ffpVertexRequest.clipPlaneMask = vertexKey.clipPlaneMask;
  const auto ffpVertexHandle = dxmt9_winemetal_compile_shader(&ffpVertexRequest);
  check(ffpVertexHandle != 0, "ffp vertex shader thunk");
  const auto ffpVertexSource = shaderSourceToString(ffpVertexHandle);
  checkContains(ffpVertexSource, "ffp vertex hash", "ffp vertex source");
  checkContains(ffpVertexSource, "halfPixelFixup", "ffp vertex half-pixel");
  dxmt9_winemetal_destroy_shader(ffpVertexHandle);

  const auto pixelKey = makeFfpPixelKey(state);
  WinemetalShaderCompileRequest ffpPixelRequest{};
  ffpPixelRequest.kind = WinemetalShaderKind_FfpPixel;
  ffpPixelRequest.variantKey = &pixelKey;
  ffpPixelRequest.textured = true;
  ffpPixelRequest.alphaTestEnable = pixelKey.alphaTestEnable ? 1u : 0u;
  ffpPixelRequest.alphaTestFunc = pixelKey.alphaTestFunc;
  ffpPixelRequest.alphaRef = 0.5f;
  const auto ffpPixelHandle = dxmt9_winemetal_compile_shader(&ffpPixelRequest);
  check(ffpPixelHandle != 0, "ffp pixel shader thunk");
  const auto ffpPixelSource = shaderSourceToString(ffpPixelHandle);
  checkContains(ffpPixelSource, "ffp pixel hash", "ffp pixel source");
  checkContains(ffpPixelSource, "discard_fragment()", "ffp pixel alpha test");
  dxmt9_winemetal_destroy_shader(ffpPixelHandle);

  const auto controlWords = makeControlFlowBytecode();
  WinemetalShaderCompileRequest controlRequest{};
  controlRequest.kind = WinemetalShaderKind_D3DBytecodeVertex;
  controlRequest.bytecode = controlWords.data();
  controlRequest.bytecodeSize = static_cast<dxmt9_u64>(controlWords.size() * sizeof(u32));
  controlRequest.bytecodeHash = hashBytes(std::as_bytes(std::span(controlWords)));
  controlRequest.clipPlaneMask = 1u;
  const auto controlHandle = dxmt9_winemetal_compile_shader(&controlRequest);
  check(controlHandle != 0, "control flow shader thunk");
  const auto controlSource = shaderSourceToString(controlHandle);
  checkContains(controlSource, "vertex VSOut dxmt9_vs", "control flow vertex shader source");
  checkContains(controlSource, "call label 7", "control flow call comment");
  checkContains(controlSource, "label 7", "control flow label comment");
  checkContains(controlSource, "do {", "control flow call wrapper");
  checkContains(controlSource, "break;", "control flow return break");
  checkContains(controlSource, "} while (false);", "control flow call wrapper close");
  checkContains(controlSource, "if ((cFloat[0]).x != 0.0f)", "control flow if translation");
  checkContains(controlSource, "} else {", "control flow else translation");
  checkContains(controlSource, "for (int dxmt9_loop_", "control flow loop translation");
  checkContains(controlSource, "for (int dxmt9_rep_", "control flow rep translation");
  checkContains(controlSource, "a0 = int(round(", "control flow mova translation");
  checkContains(controlSource, "float4(sin(", "control flow sin translation");
  checkContains(controlSource, "cos(", "control flow cos translation");
  checkContains(controlSource, "float4(log2(", "control flow log translation");
  checkContains(controlSource, "float4(exp2(", "control flow exp translation");
  checkContains(controlSource, "pow(", "control flow pow translation");
  checkContains(controlSource, "select(float4(0.0f), float4(1.0f),", "control flow compare translation");
  checkContains(controlSource, "p[0] =", "control flow setp translation");
  checkContains(controlSource, "m4x4", "control flow matrix opcode");
  checkContains(controlSource, "m4x3", "control flow matrix opcode");
  checkContains(controlSource, "m3x2", "control flow matrix opcode");
  dxmt9_winemetal_destroy_shader(controlHandle);
}

void testVisualDerivedFfpCoverage() {
  // derived from Wine: visual.c:lighting_test
  DeviceState lightingState;
  lightingState.reset();
  lightingState.renderStates[RS_LIGHTING] = 1;
  lightingState.renderStates[RS_SPECULAR_ENABLE] = 1;
  lightingState.lightEnabled[0] = true;
  lightingState.lights[0].type = LightType::Directional;
  lightingState.lights[0].diffuse = {1.0f, 0.5f, 0.25f, 1.0f};
  const auto lightingVertexKey = makeFfpVertexKey(lightingState);
  check(lightingVertexKey.lightingEnabled, "lighting visual key");
  check(lightingVertexKey.specularEnabled, "lighting specular visual key");
  checkEq(lightingVertexKey.lightType[0], static_cast<u32>(LightType::Directional),
          "lighting visual light type");
  WinemetalShaderCompileRequest lightingRequest{};
  lightingRequest.kind = WinemetalShaderKind_FfpVertex;
  lightingRequest.variantKey = &lightingVertexKey;
  const auto lightingHandle = dxmt9_winemetal_compile_shader(&lightingRequest);
  check(lightingHandle != 0, "lighting ffp shader");
  const auto lightingSource = shaderSourceToString(lightingHandle);
  checkContains(lightingSource, "out.color.rgb *= 1.0", "lighting visual source");
  dxmt9_winemetal_destroy_shader(lightingHandle);

  // derived from Wine: visual.c:fog_test
  DeviceState fogState;
  fogState.reset();
  fogState.renderStates[RS_FOG_TABLE_MODE] = static_cast<u32>(FogMode::Exp2);
  fogState.renderStates[RS_ALPHA_TEST_ENABLE] = 1;
  fogState.renderStates[RS_ALPHA_FUNC] = static_cast<u32>(CompareFunc::GreaterEqual);
  fogState.textureStageStates[0][TSS_COLOR_OP] = static_cast<u32>(TextureOp::Modulate);
  const auto fogPixelKey = makeFfpPixelKey(fogState);
  checkEq(fogPixelKey.fogMode, FogMode::Exp2, "fog visual key");
  WinemetalShaderCompileRequest fogRequest{};
  fogRequest.kind = WinemetalShaderKind_FfpPixel;
  fogRequest.variantKey = &fogPixelKey;
  fogRequest.alphaTestEnable = fogPixelKey.alphaTestEnable ? 1u : 0u;
  fogRequest.alphaTestFunc = fogPixelKey.alphaTestFunc;
  fogRequest.alphaRef = 0.5f;
  const auto fogHandle = dxmt9_winemetal_compile_shader(&fogRequest);
  check(fogHandle != 0, "fog ffp shader");
  const auto fogSource = shaderSourceToString(fogHandle);
  checkContains(fogSource, "fogFactor", "fog visual source");
  checkContains(fogSource, "mix(fogColor, color, fog)", "fog visual blend");
  dxmt9_winemetal_destroy_shader(fogHandle);

  // derived from Wine: visual.c:texture_transform_test
  DeviceState transformState;
  transformState.reset();
  transformState.textureStageStates[0][TSS_TEXCOORD_INDEX] = 4;
  transformState.textureStageStates[0][TSS_TEXTURE_TRANSFORM_FLAGS] = 7;
  const auto transformVertexKey = makeFfpVertexKey(transformState);
  checkEq(transformVertexKey.texCoordGen[0], 4u, "texture transform texcoord");
  checkEq(transformVertexKey.texTransformFlags[0], 7u, "texture transform flags");

  // derived from Wine: visual.c:texop_test
  DeviceState texopState;
  texopState.reset();
  texopState.textureStageStates[0][TSS_COLOR_OP] = static_cast<u32>(TextureOp::Add);
  texopState.textureStageStates[0][TSS_ALPHA_OP] = static_cast<u32>(TextureOp::Modulate);
  texopState.textureStageStates[0][TSS_TEXCOORD_INDEX] = 4;
  texopState.textureStageStates[0][TSS_TEXTURE_TRANSFORM_FLAGS] = 7;
  const auto texopPixelKey = makeFfpPixelKey(texopState);
  checkEq(texopPixelKey.stages[0].colorOp, static_cast<u32>(TextureOp::Add), "texop visual color op");
  checkEq(texopPixelKey.stages[0].alphaOp, static_cast<u32>(TextureOp::Modulate), "texop visual alpha op");
  checkEq(texopPixelKey.stages[0].texCoordIndex, 4u, "texop visual texcoord");
  WinemetalShaderCompileRequest texopRequest{};
  texopRequest.kind = WinemetalShaderKind_FfpPixel;
  texopRequest.variantKey = &texopPixelKey;
  texopRequest.textured = true;
  const auto texopHandle = dxmt9_winemetal_compile_shader(&texopRequest);
  check(texopHandle != 0, "texop ffp shader");
  const auto texopSource = shaderSourceToString(texopHandle);
  checkContains(texopSource, "color += texColor", "texop add source");
  dxmt9_winemetal_destroy_shader(texopHandle);

  // derived from Wine: visual.c:fixed_function_varying_test
  DeviceState varyingState;
  varyingState.reset();
  varyingState.renderStates[RS_LIGHTING] = 1;
  varyingState.renderStates[RS_SPECULAR_ENABLE] = 1;
  varyingState.lightEnabled[0] = true;
  varyingState.lights[0].type = LightType::Point;
  varyingState.textureStageStates[0][TSS_COLOR_OP] = static_cast<u32>(TextureOp::SelectArg1);
  varyingState.textureStageStates[0][TSS_ALPHA_OP] = static_cast<u32>(TextureOp::Modulate);
  const auto varyingVertexKey = makeFfpVertexKey(varyingState);
  const auto varyingPixelKey = makeFfpPixelKey(varyingState);
  check(varyingVertexKey.hash != 0, "varying vertex hash");
  check(varyingPixelKey.hash != 0, "varying pixel hash");
  check(varyingVertexKey != lightingVertexKey, "varying vertex key differs");
  check(varyingPixelKey.hash != fogPixelKey.hash, "varying pixel key differs");
}

void testVisualPortCoverage() {
  // derived from Wine: visual.c:test_sanity
  auto backend = std::make_shared<RecordingBackend>();
  BackendLimits limits{};
  limits.maxTextureSize = 4096;
  limits.maxColorAttachments = 4;
  limits.maxAnisotropy = 16;
  limits.supportsBgr10A2 = true;
  limits.supportsDepth32FloatStencil8 = true;

  Factory factory(limits, backend);
  PresentParameters params{};
  params.backBufferWidth = 32;
  params.backBufferHeight = 32;
  params.backBufferFormat = Format::A8R8G8B8;
  params.windowed = true;
  params.presentationInterval = PresentInterval::Immediate;
  params.deviceWindow = Handle{11};
  auto device = factory.createDevice(0, params);
  check(device != nullptr, "visual sanity device");
  auto backBuffer = device->swapChain()->backBuffer();
  auto probeSurface = device->createSurface({32, 32, Format::A8R8G8B8, Pool::Scratch, 0, false, false});
  check(backBuffer != nullptr && probeSurface != nullptr, "visual sanity surfaces");
  ClearDesc clear{};
  clear.clearColor = true;
  clear.color = {0.0f, 0.0f, 0.0f, 1.0f};
  clear.colorAttachments[0] = {backBuffer->handle(), backBuffer->level(), backBuffer->multiSampleCount()};
  checkEq(device->clear(clear), D3D_OK, "visual sanity clear");
  checkEq(device->getRenderTargetData(backBuffer, probeSurface), D3D_OK, "visual sanity readback");
  auto region = probeSurface->lockRect(nullptr, 0);
  check(region.data != nullptr, "visual sanity lock");
  const auto sanityPixel = bgra(0x00, 0x00, 0x00, 0xff);
  checkBytes(std::span<const u8>(static_cast<const u8*>(region.data), 4),
             std::span<const u8>(sanityPixel.data(), sanityPixel.size()), "visual sanity pixel");
  probeSurface->unlockRect();

  // derived from Wine: visual.c:alpha_test
  DeviceState alphaState;
  alphaState.reset();
  alphaState.renderStates[RS_ALPHA_TEST_ENABLE] = 1;
  const std::array<CompareFunc, 8> alphaFuncs{
      CompareFunc::Never,        CompareFunc::Less,      CompareFunc::Equal,       CompareFunc::LessEqual,
      CompareFunc::Greater,      CompareFunc::NotEqual,  CompareFunc::GreaterEqual, CompareFunc::Always};
  u64 alphaHash = 0;
  for (size_t i = 0; i < alphaFuncs.size(); ++i) {
    alphaState.renderStates[RS_ALPHA_FUNC] = static_cast<u32>(alphaFuncs[i]);
    alphaState.renderStates[RS_ALPHA_REF] = 128;
    const auto pixelKey = makeFfpPixelKey(alphaState);
    check(pixelKey.alphaTestEnable, "alpha test pixel key enabled");
    checkEq(pixelKey.alphaTestFunc, static_cast<u32>(alphaFuncs[i]), "alpha test function key");
    check(pixelKey.hash != 0, "alpha test hash");
    if (i == 0) {
      alphaHash = pixelKey.hash;
    } else {
      check(pixelKey.hash != alphaHash, "alpha test hash varies");
    }
  }
  const auto alphaPixelKey = makeFfpPixelKey(alphaState);
  WinemetalShaderCompileRequest alphaRequest{};
  alphaRequest.kind = WinemetalShaderKind_FfpPixel;
  alphaRequest.variantKey = &alphaPixelKey;
  alphaRequest.textured = true;
  alphaRequest.alphaTestEnable = 1u;
  alphaRequest.alphaTestFunc = alphaPixelKey.alphaTestFunc;
  alphaRequest.alphaRef = 0.5f;
  const auto alphaHandle = dxmt9_winemetal_compile_shader(&alphaRequest);
  check(alphaHandle != 0, "alpha test visual shader");
  const auto alphaSource = shaderSourceToString(alphaHandle);
  checkContains(alphaSource, "discard_fragment()", "alpha test visual source");
  dxmt9_winemetal_destroy_shader(alphaHandle);

  // derived from Wine: visual.c:texbem_test
  DeviceState texbemState;
  texbemState.reset();
  texbemState.textureStageStates[0][TSS_COLOR_OP] = static_cast<u32>(TextureOp::BumpEnvMap);
  texbemState.textureStageStates[0][TSS_ALPHA_OP] = static_cast<u32>(TextureOp::BumpEnvMapLuminance);
  texbemState.textureStageStates[0][TSS_TEXCOORD_INDEX] = 1;
  const auto texbemKey = makeFfpPixelKey(texbemState);
  checkEq(texbemKey.stages[0].colorOp, static_cast<u32>(TextureOp::BumpEnvMap), "texbem color op");
  checkEq(texbemKey.stages[0].alphaOp, static_cast<u32>(TextureOp::BumpEnvMapLuminance), "texbem alpha op");
  checkEq(texbemKey.stages[0].texCoordIndex, 1u, "texbem texcoord");
  check(texbemKey.hash != 0, "texbem hash");

  // derived from Wine: visual.c:ps_1_4_test
  DeviceState ps14State;
  ps14State.reset();
  ps14State.textureStageStates[0][TSS_COLOR_OP] = static_cast<u32>(TextureOp::AddSigned);
  ps14State.textureStageStates[0][TSS_ALPHA_OP] = static_cast<u32>(TextureOp::Modulate);
  ps14State.textureStageStates[1][TSS_COLOR_OP] = static_cast<u32>(TextureOp::DotProduct3);
  const auto ps14Key = makeFfpPixelKey(ps14State);
  check(ps14Key.hash != 0, "ps_1_4 hash");
  check(ps14Key != texbemKey, "ps_1_4 key differs from texbem");

  // derived from Wine: visual.c:vshader_version_varying_test
  DeviceState baselineVertexState;
  baselineVertexState.reset();
  const auto baselineVertexKey = makeFfpVertexKey(baselineVertexState);
  DeviceState varyingState;
  varyingState.reset();
  varyingState.renderStates[RS_LIGHTING] = 1;
  varyingState.renderStates[RS_SPECULAR_ENABLE] = 1;
  varyingState.renderStates[RS_VERTEX_BLEND] = 2;
  varyingState.lightEnabled[0] = true;
  varyingState.lights[0].type = LightType::Point;
  const auto varyingVertexKey = makeFfpVertexKey(varyingState);
  check(varyingVertexKey.hash != 0, "vshader varying hash");
  check(varyingVertexKey.vertexBlend == 2u, "vshader varying vertex blend");
  check(varyingVertexKey.indexedVertexBlend, "vshader varying indexed blend");
  check(varyingVertexKey != baselineVertexKey, "vshader varying key differs");
}

void testRasterStateCoverage() {
  auto backend = std::make_shared<RecordingBackend>();
  BackendLimits limits{};
  limits.maxTextureSize = 4096;
  limits.maxColorAttachments = 4;
  limits.maxAnisotropy = 16;
  limits.supportsBgr10A2 = true;
  limits.supportsDepth32FloatStencil8 = true;

  Factory factory(limits, backend);
  PresentParameters params{};
  params.backBufferWidth = 16;
  params.backBufferHeight = 16;
  params.backBufferFormat = Format::A8R8G8B8;
  params.windowed = true;
  params.presentationInterval = PresentInterval::Immediate;
  params.deviceWindow = Handle{12};
  params.enableAutoDepthStencil = true;
  params.autoDepthStencilFormat = Format::D24S8;
  auto device = factory.createDevice(0, params);
  check(device != nullptr, "raster coverage device");
  auto depthStencil = device->swapChain()->depthStencilSurface();
  check(depthStencil != nullptr, "raster coverage depth stencil");

  const auto fixup = halfPixelFixup(Viewport{0, 0, 16, 16, 0.0f, 1.0f});
  checkNear(fixup[0], 1.0f / 16.0f, 1.0e-6f, "raster half-pixel x");
  checkNear(fixup[1], 1.0f / 16.0f, 1.0e-6f, "raster half-pixel y");

  checkEq(device->setViewport({0, 0, 16, 16, 0.0f, 1.0f}), D3D_OK, "raster viewport");
  checkEq(device->setRenderState(RS_CULL_MODE, static_cast<u32>(CullMode::Ccw)), D3D_OK, "raster cull ccw");
  checkEq(device->setRenderState(RS_Z_ENABLE, 1), D3D_OK, "raster depth enable");
  checkEq(device->setRenderState(RS_Z_WRITE_ENABLE, 1), D3D_OK, "raster depth write");
  checkEq(device->setRenderState(RS_Z_FUNC, static_cast<u32>(CompareFunc::LessEqual)), D3D_OK,
          "raster depth func");
  checkEq(device->drawPrimitive(PrimitiveType::TriangleList, 1), D3D_OK, "raster draw ccw");
  check(!backend->draws.empty(), "raster draw recorded");
  const auto& firstDraw = backend->draws.back();
  checkEq(firstDraw.viewport.viewport.width, 16u, "raster draw viewport width");
  checkEq(firstDraw.viewport.viewport.height, 16u, "raster draw viewport height");
  checkEq(firstDraw.rs.values.at(RS_CULL_MODE), static_cast<u32>(CullMode::Ccw), "raster cull state ccw");
  checkEq(firstDraw.rs.values.at(RS_Z_ENABLE), 1u, "raster depth enable state");
  checkEq(firstDraw.rs.values.at(RS_Z_WRITE_ENABLE), 1u, "raster depth write state");
  checkEq(firstDraw.rs.values.at(RS_Z_FUNC), static_cast<u32>(CompareFunc::LessEqual), "raster depth func state");
  checkEq(device->setRenderState(RS_CULL_MODE, static_cast<u32>(CullMode::Cw)), D3D_OK, "raster cull cw");
  checkEq(device->drawPrimitive(PrimitiveType::TriangleList, 1), D3D_OK, "raster draw cw");
  const auto& secondDraw = backend->draws.back();
  checkEq(secondDraw.rs.values.at(RS_CULL_MODE), static_cast<u32>(CullMode::Cw), "raster cull state cw");
}

void testDeviceCoreFlow() {
  auto backend = std::make_shared<RecordingBackend>();
  BackendLimits limits{};
  limits.maxTextureSize = 8192;
  limits.maxColorAttachments = 4;
  limits.maxAnisotropy = 16;
  limits.supportsBgr10A2 = true;
  limits.supportsDepth32FloatStencil8 = true;

  Factory factory(limits, backend);
  PresentParameters params{};
  params.backBufferWidth = 640;
  params.backBufferHeight = 480;
  params.backBufferFormat = Format::A8R8G8B8;
  params.windowed = true;
  params.presentationInterval = PresentInterval::Default;
  params.deviceWindow = Handle{99};
  params.enableAutoDepthStencil = true;
  params.autoDepthStencilFormat = Format::D24S8;
  params.multiSampleType = MultiSampleType::Four;

  auto device = factory.createDevice(0, params);
  check(device != nullptr, "device creation");
  check(device->swapChain() != nullptr, "default swap chain");
  const auto primaryChain = device->swapChain();
  const auto backBuffer = primaryChain->backBuffer();
  const auto depthStencil = primaryChain->depthStencilSurface();
  check(backBuffer != nullptr, "back buffer");
  check(depthStencil != nullptr, "depth stencil");
  check(primaryChain->displaySyncEnabled(), "display sync");
  checkEq(backBuffer->desc().width, 640u, "back buffer width");
  checkEq(backBuffer->desc().height, 480u, "back buffer height");
  checkEq(backBuffer->desc().format, Format::A8R8G8B8, "back buffer format");
  checkEq(backBuffer->multiSampleCount(), 4u, "back buffer sample count");
  checkEq(device->state().renderTargets[0].handle, backBuffer->handle(), "primary render target binding");
  checkEq(device->state().depthStencil.handle, depthStencil->handle(), "depth stencil binding");

  auto dynamicBuffer = device->createBuffer({16, Pool::Default, UsageVertexBuffer | UsageDynamic});
  auto managedBuffer = device->createBuffer({16, Pool::Managed, UsageVertexBuffer});
  auto texture = device->createTexture({4, 4, 1, 2, Format::A8R8G8B8, TextureType::TwoD, Pool::Managed, UsageTexture});
  auto srcTexture = device->createTexture({2, 2, 1, 2, Format::A8R8G8B8, TextureType::TwoD, Pool::Managed, UsageTexture});
  auto dstTexture = device->createTexture({2, 2, 1, 2, Format::A8R8G8B8, TextureType::TwoD, Pool::Managed, UsageTexture});
  auto systemSurface = device->createSurface({2, 2, Format::A8R8G8B8, Pool::SystemMem, 0, false, false});
  auto scratchSurface = device->createSurface({2, 2, Format::A8R8G8B8, Pool::Scratch, 0, false, false});
  auto copySurface = device->createSurface({2, 2, Format::A8R8G8B8, Pool::Default, 0, false, false});
  auto stretchSurface = device->createSurface({4, 4, Format::A8R8G8B8, Pool::Default, 0, false, false});
  auto readbackSurface = device->createSurface({2, 2, Format::A8R8G8B8, Pool::Scratch, 0, false, false});

  check(dynamicBuffer != nullptr, "default buffer");
  check(managedBuffer != nullptr, "managed buffer");
  check(texture != nullptr, "managed texture");
  check(srcTexture != nullptr, "source texture");
  check(dstTexture != nullptr, "destination texture");
  check(systemSurface != nullptr, "systemmem surface");
  check(scratchSurface != nullptr, "scratch surface");
  check(copySurface != nullptr, "copy surface");
  check(stretchSurface != nullptr, "stretch surface");
  check(readbackSurface != nullptr, "readback surface");

  auto bufferRegion = dynamicBuffer->lock(0, 16, UsageDiscard);
  check(bufferRegion.data != nullptr, "buffer lock");
  const std::array<u8, 16> bufferBytes{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  std::memcpy(bufferRegion.data, bufferBytes.data(), bufferBytes.size());
  dynamicBuffer->unlock();
  checkBytes(dynamicBuffer->bytes(), std::span<const u8>(bufferBytes.data(), bufferBytes.size()),
             "buffer upload");

  auto managedRegion = managedBuffer->lock(0, 16, 0);
  check(managedRegion.data != nullptr, "managed buffer lock");
  const std::array<u8, 16> managedBytes{16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
  std::memcpy(managedRegion.data, managedBytes.data(), managedBytes.size());
  managedBuffer->unlock();
  checkEq(backend->mappedBuffers.size(), size_t{2}, "backend buffer map count");
  checkEq(backend->unmappedBuffers.size(), size_t{2}, "backend buffer unmap count");

  const auto textureLevel1 = texture->surfaceLevel(1);
  check(textureLevel1 != nullptr, "texture level surface");
  textureLevel1->fillColor(nullptr, {0.0f, 1.0f, 0.0f, 1.0f});
  const std::array<u8, 4> greenPixel = bgra(0x00, 0xff, 0x00, 0xff);
  checkBytes(std::span<const u8>(texture->levelBytes(1).data(), 4),
             std::span<const u8>(greenPixel.data(), greenPixel.size()),
             "texture-backed surface fill");

  auto srcLevel0 = srcTexture->surfaceLevel(0);
  auto srcLevel1 = srcTexture->surfaceLevel(1);
  auto dstLevel0 = dstTexture->surfaceLevel(0);
  auto dstLevel1 = dstTexture->surfaceLevel(1);
  check(srcLevel0 != nullptr && srcLevel1 != nullptr && dstLevel0 != nullptr && dstLevel1 != nullptr,
        "texture levels");
  srcLevel0->fillColor(nullptr, {1.0f, 0.0f, 0.0f, 1.0f});
  srcLevel1->fillColor(nullptr, {0.0f, 0.0f, 1.0f, 1.0f});
  checkEq(device->updateTexture(srcTexture, dstTexture), D3D_OK, "update texture");
  checkBytes(dstTexture->levelBytes(0), srcTexture->levelBytes(0), "texture level 0 copy");
  checkBytes(dstTexture->levelBytes(1), srcTexture->levelBytes(1), "texture level 1 copy");

  checkEq(device->fillSurface(systemSurface, nullptr, {0.0f, 0.0f, 1.0f, 1.0f}), D3D_OK, "systemmem fill");
  checkEq(device->fillSurface(scratchSurface, nullptr, {1.0f, 1.0f, 0.0f, 1.0f}), D3D_OK, "scratch fill");
  checkEq(device->updateSurface(systemSurface, copySurface), D3D_OK, "update surface");
  checkEq(device->stretchRect(systemSurface, nullptr, stretchSurface, nullptr, true), D3D_OK,
          "stretch rect");
  checkEq(device->getRenderTargetData(copySurface, readbackSurface), D3D_OK, "readback");
  auto copyRegion = copySurface->lockRect(nullptr, 0);
  check(copyRegion.data != nullptr, "copy surface lock");
  const std::array<u8, 4> bluePixel = bgra(0xff, 0x00, 0x00, 0xff);
  checkBytes(std::span<const u8>(static_cast<const u8*>(copyRegion.data), 4),
             std::span<const u8>(bluePixel.data(), bluePixel.size()), "surface copy");
  copySurface->unlockRect();

  auto stretchRegion = stretchSurface->lockRect(nullptr, 0);
  check(stretchRegion.data != nullptr, "stretch surface lock");
  checkBytes(std::span<const u8>(static_cast<const u8*>(stretchRegion.data), 4),
             std::span<const u8>(bluePixel.data(), bluePixel.size()), "stretched surface copy");
  stretchSurface->unlockRect();

  auto readbackRegion = readbackSurface->lockRect(nullptr, 0);
  check(readbackRegion.data != nullptr, "readback surface lock");
  checkBytes(std::span<const u8>(static_cast<const u8*>(readbackRegion.data), 4),
             std::span<const u8>(bluePixel.data(), bluePixel.size()), "readback surface copy");
  readbackSurface->unlockRect();

  auto systemRegion = systemSurface->lockRect(nullptr, 0);
  check(systemRegion.data != nullptr, "system surface lock");
  checkBytes(std::span<const u8>(static_cast<const u8*>(systemRegion.data), 4),
             std::span<const u8>(bluePixel.data(), bluePixel.size()),
             "system surface fill");
  systemSurface->unlockRect();

  auto scratchRegion = scratchSurface->lockRect(nullptr, 0);
  check(scratchRegion.data != nullptr, "scratch surface lock");
  const std::array<u8, 4> yellowPixel = bgra(0x00, 0xff, 0xff, 0xff);
  checkBytes(std::span<const u8>(static_cast<const u8*>(scratchRegion.data), 4),
             std::span<const u8>(yellowPixel.data(), yellowPixel.size()),
             "scratch surface fill");
  scratchSurface->unlockRect();

  checkEq(backend->colorFills.size(), size_t{2}, "backend color fill count");
  checkEq(backend->surfaceCopies.size(), size_t{4}, "backend surface copy count");
  checkEq(backend->stretchRects.size(), size_t{1}, "backend stretch rect count");
  checkEq(backend->readbacks.size(), size_t{1}, "backend readback count");

  Light light{};
  light.type = LightType::Point;
  light.diffuse = {1.0f, 1.0f, 1.0f, 1.0f};
  light.position = {1.0f, 2.0f, 3.0f};
  checkEq(device->setLight(0, light), D3D_OK, "set light");
  checkEq(device->lightEnable(0, true), D3D_OK, "enable light");
  checkEq(device->setRenderState(RS_LIGHTING, 1), D3D_OK, "lighting state");
  checkEq(device->setRenderState(RS_ALPHA_TEST_ENABLE, 1), D3D_OK, "alpha test state");
  checkEq(device->setRenderState(RS_ALPHA_FUNC, static_cast<u32>(CompareFunc::GreaterEqual)), D3D_OK,
          "alpha function state");
  checkEq(device->setRenderState(RS_CLIP_PLANE_ENABLE, 1), D3D_OK, "clip plane enable");
  const ClipPlane clipPlane{1.0f, 0.0f, 0.0f, -1.0f};
  checkEq(device->setClipPlane(0, clipPlane), D3D_OK, "clip plane state");
  checkEq(device->setTextureStageState(0, TSS_COLOR_OP, static_cast<u32>(TextureOp::SelectArg1)), D3D_OK,
          "texture stage color op");
  checkEq(device->setTextureStageState(0, TSS_ALPHA_OP, static_cast<u32>(TextureOp::Modulate)), D3D_OK,
          "texture stage alpha op");
  checkEq(device->setSamplerState(0, SAMP_MAX_ANISOTROPY, 4), D3D_OK, "sampler state");
  checkEq(device->setSamplerState(0, SAMP_MIN_FILTER, 2), D3D_OK, "sampler min filter");
  checkEq(device->setTexture(0, texture), D3D_OK, "texture binding");
  checkEq(device->setStreamSource(0, dynamicBuffer, 8, 16), D3D_OK, "stream source");
  checkEq(device->setIndices(dynamicBuffer, IndexType::UInt32), D3D_OK, "index buffer");
  checkEq(device->setFVF(0x1122u), D3D_OK, "fvf");
  checkEq(device->setVertexDeclaration({VertexElement{0, 0, 0, 0, 0, 0}}), D3D_OK, "vertex decl");

  Matrix4x4 view{};
  view.m = {2.0f, 0.0f, 0.0f, 0.0f, 0.0f, 3.0f, 0.0f, 0.0f, 0.0f, 0.0f, 4.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f};
  checkEq(device->setTransform(XFORM_VIEW, view), D3D_OK, "view transform");
  checkEq(device->setViewport({0, 0, 640, 480, 0.0f, 1.0f}), D3D_OK, "viewport");
  checkEq(device->setScissorRect({16, 24, 320, 240}), D3D_OK, "scissor");

  auto stateBlock = device->createStateBlock();
  check(stateBlock != nullptr, "state block");
  check(stateBlock->snapshot().textures[0] == texture, "state block texture snapshot");
  checkEq(stateBlock->snapshot().vertexDecl.fvf, 0x1122u, "state block fvf snapshot");

  checkEq(device->setRenderState(RS_LIGHTING, 0), D3D_OK, "mutate lighting");
  checkEq(device->setTexture(0, nullptr), D3D_OK, "unbind texture");
  checkEq(device->setStreamSource(0, nullptr, 0, 0), D3D_OK, "unbind stream");
  checkEq(device->setIndices(nullptr), D3D_OK, "unbind indices");
  checkEq(device->setViewport({0, 0, 320, 240, 0.0f, 1.0f}), D3D_OK, "mutate viewport");
  checkEq(device->setScissorRect({0, 0, 160, 120}), D3D_OK, "mutate scissor");

  checkEq(device->applyStateBlock(*stateBlock), D3D_OK, "apply state block");
  checkEq(device->getRenderState(RS_LIGHTING), 1u, "restored lighting");
  checkEq(device->state().textures[0], texture, "restored texture binding");
  checkEq(device->state().streamBuffers[0], dynamicBuffer, "restored stream source");
  checkEq(device->state().indexBuffer, dynamicBuffer, "restored index buffer");
  checkEq(device->state().viewport.width, 640u, "restored viewport width");
  checkEq(device->state().scissorEnabled, true, "restored scissor enabled");
  checkEq(device->state().scissorRect.left, 16, "restored scissor left");
  checkEq(device->state().transforms.at(XFORM_VIEW).m[0], 2.0f, "restored transform");

  std::array<u8, 4> vertexPayload{1, 2, 3, 4};
  checkEq(device->drawPrimitiveUP(PrimitiveType::TriangleList, 1,
                                  std::span<const u8>(vertexPayload.data(), vertexPayload.size())),
          D3D_OK, "draw primitive up");
  checkEq(backend->draws.size(), size_t{1}, "first draw count");

  const auto& draw0 = backend->draws[0];
  checkEq(draw0.primitiveType, PrimitiveType::TriangleList, "draw0 primitive type");
  checkEq(draw0.indexBuffer, dynamicBuffer->handle(), "draw0 index buffer");
  checkEq(draw0.indexType, IndexType::UInt32, "draw0 index type");
  checkEq(draw0.vertexDecl.fvf, 0x1122u, "draw0 fvf");
  checkEq(draw0.vertexDecl.elements.size(), size_t{1}, "draw0 vertex decl size");
  checkEq(draw0.vertexDecl.streams[0].buffer, dynamicBuffer, "draw0 stream buffer");
  checkEq(draw0.vertexDecl.streams[0].offset, 8u, "draw0 stream offset");
  checkEq(draw0.vertexDecl.streams[0].stride, 16u, "draw0 stream stride");
  checkEq(draw0.textures[0].handle, texture->handle(), "draw0 texture handle");
  checkEq(draw0.textures[0].stageStates.at(TSS_COLOR_OP), static_cast<u32>(TextureOp::SelectArg1),
          "draw0 color op");
  checkEq(draw0.textures[0].stageStates.at(TSS_ALPHA_OP), static_cast<u32>(TextureOp::Modulate),
          "draw0 alpha op");
  checkEq(draw0.samplers[0].states.at(SAMP_MAX_ANISOTROPY), 4u, "draw0 max anisotropy");
  checkEq(draw0.samplers[0].states.at(SAMP_MIN_FILTER), 2u, "draw0 min filter");
  checkEq(draw0.rs.values.at(RS_LIGHTING), 1u, "draw0 lighting state");
  checkEq(draw0.rs.values.at(RS_ALPHA_TEST_ENABLE), 1u, "draw0 alpha test state");
  checkEq(draw0.clipPlaneMask, 1u, "draw0 clip plane mask");
  checkNear(draw0.clipPlanes[0][0], 0.5f, 1.0e-6f, "draw0 clip plane x");
  checkNear(draw0.clipPlanes[0][1], 0.0f, 1.0e-6f, "draw0 clip plane y");
  checkNear(draw0.clipPlanes[0][2], 0.0f, 1.0e-6f, "draw0 clip plane z");
  checkNear(draw0.clipPlanes[0][3], -1.0f, 1.0e-6f, "draw0 clip plane w");
  checkEq(draw0.rts.color[0].handle, primaryChain->backBuffer()->handle(), "draw0 render target");
  checkEq(draw0.rts.color[0].sampleCount, 4u, "draw0 render target sample count");
  checkEq(draw0.rts.depthStencil.handle, primaryChain->depthStencilSurface()->handle(),
          "draw0 depth stencil target");
  checkEq(draw0.viewport.viewport.width, 640u, "draw0 viewport width");
  checkEq(draw0.viewport.scissorEnabled, true, "draw0 scissor enabled");
  checkEq(draw0.viewport.scissor.left, 16, "draw0 scissor left");
  check(draw0.vertexShader.kind == ShaderRef::Kind::FixedFunctionVertex, "draw0 vertex shader kind");
  check(draw0.pixelShader.kind == ShaderRef::Kind::FixedFunctionPixel, "draw0 pixel shader kind");
  check(draw0.vertexShader.vertexKey.has_value(), "draw0 vertex shader key");
  check(draw0.pixelShader.pixelKey.has_value(), "draw0 pixel shader key");
  check(draw0.vertexShader.vertexKey->lightingEnabled, "draw0 vertex key lighting");
  check(draw0.pixelShader.pixelKey->alphaTestEnable, "draw0 pixel key alpha test");
  checkEq(draw0.userVertexData.size(), vertexPayload.size(), "draw0 user vertex payload");

  std::array<u32, 4> fanIndices{0, 1, 2, 3};
  std::array<u8, sizeof(fanIndices)> fanIndexBytes{};
  std::memcpy(fanIndexBytes.data(), fanIndices.data(), fanIndexBytes.size());
  checkEq(device->drawIndexedPrimitiveUP(PrimitiveType::TriangleFan, 2,
                                          std::span<const u8>(vertexPayload.data(), vertexPayload.size()),
                                          std::span<const u8>(fanIndexBytes.data(), fanIndexBytes.size()),
                                          IndexType::UInt32),
          D3D_OK, "draw indexed primitive up");
  checkEq(backend->draws.size(), size_t{2}, "second draw count");
  const auto& draw1 = backend->draws[1];
  checkEq(draw1.primitiveType, PrimitiveType::TriangleList, "fan decomposed to triangle list");
  checkEq(draw1.primitiveCount, 2u, "fan primitive count");
  checkEq(draw1.indexType, IndexType::UInt32, "fan draw index type");
  checkEq(draw1.userIndexData.size(), sizeof(u32) * 6u, "fan user index payload");

  auto occlusion = device->createQuery(QueryType::Occlusion);
  auto timestamp = device->createQuery(QueryType::Timestamp);
  check(occlusion != nullptr, "occlusion query");
  check(timestamp != nullptr, "timestamp query");
  checkEq(device->issueQuery(occlusion, true), D3D_OK, "occlusion begin");
  checkEq(device->drawPrimitive(PrimitiveType::TriangleList, 2), D3D_OK, "occlusion draw");
  checkEq(device->issueQuery(occlusion, false), D3D_OK, "occlusion end");
  checkEq(device->issueQuery(timestamp, false), D3D_OK, "timestamp issue");

  u32 occlusionCount = 0;
  checkEq(device->getQueryData(occlusion, &occlusionCount, sizeof(occlusionCount), 0), S_FALSE,
          "occlusion not ready before present");
  checkEq(device->present(), D3D_OK, "present");
  checkEq(backend->presents.size(), size_t{1}, "present count");
  checkEq(backend->presents[0].window, params.deviceWindow, "present window");
  checkEq(backend->presents[0].width, 640u, "present width");
  checkEq(backend->presents[0].height, 480u, "present height");
  checkEq(backend->presents[0].format, Format::A8R8G8B8, "present format");
  checkEq(backend->presents[0].multiSampleType, MultiSampleType::Four, "present sample type");
  check(backend->presents[0].displaySyncEnabled, "present sync");

  checkEq(device->getQueryData(occlusion, &occlusionCount, sizeof(occlusionCount), QUERY_GETDATA_FLUSH), S_OK,
          "occlusion ready after present");
  checkEq(occlusionCount, 2u, "occlusion count");

  u64 timestampValue = 0;
  checkEq(device->getQueryData(timestamp, &timestampValue, sizeof(timestampValue), 0), S_OK,
          "timestamp query");
  check(timestampValue != 0, "timestamp should be non-zero");

  auto freq = device->createQuery(QueryType::TimestampFreq);
  auto disjoint = device->createQuery(QueryType::TimestampDisjoint);
  u64 freqValue = 0;
  u64 disjointValue = 1234;
  checkEq(device->getQueryData(freq, &freqValue, sizeof(freqValue), 0), S_OK, "timestamp freq");
  checkEq(device->getQueryData(disjoint, &disjointValue, sizeof(disjointValue), 0), S_OK,
          "timestamp disjoint");
  checkEq(freqValue, 1000000000ull, "timestamp frequency");
  checkEq(disjointValue, 0ull, "timestamp disjoint value");

  const auto oldBackBuffer = primaryChain->backBuffer();
  const auto oldDepthStencil = primaryChain->depthStencilSurface();
  checkEq(device->reset(PresentParameters{
                                .backBufferWidth = 800,
                                .backBufferHeight = 600,
                                .backBufferFormat = Format::A8R8G8B8,
                                .backBufferCount = 1,
                                .windowed = true,
                                .presentationInterval = PresentInterval::Immediate,
                                .deviceWindow = Handle{77},
                                .enableAutoDepthStencil = true,
                                .autoDepthStencilFormat = Format::D24S8,
                                .discardSwapEffect = true,
                                .multiSampleType = MultiSampleType::Two,
                            }),
          D3D_OK, "device reset");

  check(!oldBackBuffer->valid(), "default pool back buffer invalidated by reset");
  check(!oldDepthStencil->valid(), "default pool depth stencil invalidated by reset");
  check(primaryChain->backBuffer() != oldBackBuffer, "swap chain back buffer recreated");
  check(primaryChain->depthStencilSurface() != oldDepthStencil, "swap chain depth recreated");
  check(primaryChain->backBuffer()->valid(), "replacement back buffer valid");
  check(primaryChain->depthStencilSurface()->valid(), "replacement depth valid");
  checkEq(primaryChain->backBuffer()->desc().width, 800u, "resized back buffer width");
  checkEq(primaryChain->backBuffer()->desc().height, 600u, "resized back buffer height");
  checkEq(primaryChain->backBuffer()->multiSampleCount(), 2u, "resized back buffer sample count");
  checkEq(device->state().viewport.width, 800u, "reset viewport width");
  checkEq(device->state().viewport.height, 600u, "reset viewport height");
  checkEq(device->state().renderTargets[0].handle, primaryChain->backBuffer()->handle(),
          "reset render target rebound");
  checkEq(device->state().depthStencil.handle, primaryChain->depthStencilSurface()->handle(),
          "reset depth stencil rebound");
  check(!dynamicBuffer->valid(), "default pool buffer invalidated");
  check(managedBuffer->valid(), "managed buffer survives reset");
  check(!copySurface->valid(), "default pool surface invalidated");
  check(backend->destroyedBuffers.size() >= size_t{1}, "backend buffers destroyed on reset");
  check(backend->destroyedSurfaces.size() >= size_t{4}, "backend surfaces destroyed on reset");
  check(systemSurface->valid(), "systemmem surface survives reset");
  check(scratchSurface->valid(), "scratch surface survives reset");
  check(texture->valid(), "managed texture survives reset");
  check(dstTexture->valid(), "managed texture copy survives reset");

  auto managedAfter = managedBuffer->bytes();
  checkBytes(managedAfter, std::span<const u8>(managedBytes.data(), managedBytes.size()),
             "managed buffer contents survive reset");
  checkBytes(texture->levelBytes(1), std::span<const u8>(greenPixel.data(), greenPixel.size()),
             "managed texture contents survive reset");

  auto systemAfter = systemSurface->lockRect(nullptr, 0);
  check(systemAfter.data != nullptr, "system surface lock after reset");
  checkBytes(std::span<const u8>(static_cast<const u8*>(systemAfter.data), 4),
             std::span<const u8>(bluePixel.data(), bluePixel.size()),
             "system surface contents survive reset");
  systemSurface->unlockRect();

  auto scratchAfter = scratchSurface->lockRect(nullptr, 0);
  check(scratchAfter.data != nullptr, "scratch surface lock after reset");
  checkBytes(std::span<const u8>(static_cast<const u8*>(scratchAfter.data), 4),
             std::span<const u8>(yellowPixel.data(), yellowPixel.size()),
             "scratch surface contents survive reset");
  scratchSurface->unlockRect();

  checkEq(device->present(), D3D_OK, "present after reset");
  checkEq(backend->presents.size(), size_t{2}, "present count after reset");
  checkEq(backend->presents[1].width, 800u, "present width after reset");
  checkEq(backend->presents[1].height, 600u, "present height after reset");
  check(!backend->presents[1].displaySyncEnabled, "immediate present after reset");
  checkEq(device->swapChain()->backBuffer()->desc().width, 800u, "swapchain width after reset");
  checkEq(device->swapChain()->backBuffer()->desc().height, 600u, "swapchain height after reset");
}

void testComWrappers() {
  using namespace dxmt9::com;

  auto* d3d = Direct3DCreate9(D3D_SDK_VERSION);
  check(d3d != nullptr, "Direct3DCreate9");
  checkEq(d3d->AddRef(), 2u, "factory addref");
  checkEq(d3d->Release(), 1u, "factory release after addref");
  checkEq(d3d->GetAdapterCount(), size_t{1}, "factory adapter count");
  checkEq(d3d->CheckDeviceType(0, DeviceType::Hal, Format::A8R8G8B8, Format::A8R8G8B8, true), D3D_OK,
          "factory device type");
  checkEq(d3d->CheckDeviceType(0, DeviceType::Hal, Format::A8R8G8B8, Format::A8R8G8B8, false), D3D_OK,
          "factory fullscreen device type");
  check(!d3d->EnumAdapterModes(0, Format::R8G8B8).size(), "unsupported adapter modes");
  checkEq(d3d->GetAdapterDisplayMode(0).width, 1920u, "factory adapter mode width");

  void* unknown = nullptr;
  check(d3d->QueryInterface(InterfaceId::IUnknown, &unknown), "factory query interface");
  auto* queriedFactory = static_cast<IDirect3D9*>(unknown);
  check(queriedFactory != nullptr, "factory query result");
  checkEq(queriedFactory->Release(), 1u, "factory qi release");
  void* exUnknown = nullptr;
  check(!d3d->QueryInterface(InterfaceId::Direct3D9Ex, &exUnknown), "base factory must not expose ex");

  PresentParameters params{};
  params.backBufferWidth = 320;
  params.backBufferHeight = 240;
  params.windowed = true;

  auto* device = d3d->CreateDevice(0, params);
  check(device != nullptr, "wrapper device create");
  check(device->coreDevice().swapChain() != nullptr, "wrapper core device swap chain");
  checkEq(device->GetDeviceCaps().maxTextureWidth, 16384u, "wrapper caps");
  checkEq(device->TestCooperativeLevel(), D3D_OK, "wrapper cooperative level");
  checkEq(device->GetSwapChainCount(), size_t{1}, "wrapper swap chain count");
  auto* primarySwapChain = device->GetSwapChain();
  check(primarySwapChain != nullptr, "wrapper primary swap chain");
  check(primarySwapChain->backBuffer() != nullptr, "wrapper primary back buffer");
  checkEq(primarySwapChain->Present(), D3D_OK, "wrapper primary swap present");
  checkEq(primarySwapChain->Release(), 0u, "wrapper primary swap release");
  auto* extraSwapChain = device->CreateAdditionalSwapChain(params);
  check(extraSwapChain != nullptr, "wrapper additional swap chain");
  check(extraSwapChain->backBuffer() != nullptr, "wrapper additional back buffer");
  checkEq(extraSwapChain->presentParameters().backBufferWidth, 320u, "wrapper additional params");
  checkEq(extraSwapChain->Present(), D3D_OK, "wrapper additional swap present");
  checkEq(device->GetSwapChainCount(), size_t{2}, "wrapper swap chain count after create");
  checkEq(extraSwapChain->Release(), 0u, "wrapper additional swap release");
  checkEq(device->AddRef(), 2u, "device addref");
  checkEq(device->Release(), 1u, "device release after addref");

  void* deviceUnknown = nullptr;
  check(device->QueryInterface(InterfaceId::Direct3DDevice9, &deviceUnknown), "device query interface");
  auto* queriedDevice = static_cast<IDirect3DDevice9*>(deviceUnknown);
  check(queriedDevice != nullptr, "device query result");
  checkEq(queriedDevice->Release(), 1u, "device qi release");
  check(!device->QueryInterface(InterfaceId::Direct3DDevice9Ex, &deviceUnknown),
        "base device must not expose ex");
  checkEq(device->Release(), 0u, "device release");
  checkEq(d3d->Release(), 0u, "factory release");
}

void testComWrappersEx() {
  using namespace dxmt9::com;

  auto backend = std::make_shared<RecordingBackend>();
  BackendLimits limits{};
  limits.maxTextureSize = 8192;
  limits.maxColorAttachments = 4;
  limits.maxAnisotropy = 16;
  limits.supportsBgr10A2 = true;
  limits.supportsDepth32FloatStencil8 = true;

  auto* d3d = Direct3DCreate9Ex(D3D_SDK_VERSION, backend);
  check(d3d != nullptr, "Direct3DCreate9Ex");
  checkEq(d3d->AddRef(), 2u, "factory ex addref");
  checkEq(d3d->Release(), 1u, "factory ex release after addref");

  void* exUnknown = nullptr;
  check(d3d->QueryInterface(InterfaceId::Direct3D9Ex, &exUnknown), "factory ex query interface");
  auto* queriedFactory = static_cast<IDirect3D9Ex*>(exUnknown);
  check(queriedFactory != nullptr, "factory ex query result");
  checkEq(queriedFactory->Release(), 1u, "factory ex qi release");

  checkEq(d3d->GetAdapterModeCountEx(0, nullptr), d3d->EnumAdapterModes(0, Format::A8R8G8B8).size(),
          "ex adapter mode count");
  DisplayModeFilter filter{};
  filter.format = Format::A8R8G8B8;
  DisplayModeEx mode{};
  check(d3d->EnumAdapterModesEx(0, &filter, 0, &mode), "ex enum adapter modes");
  checkEq(mode.scanLineOrdering, DisplayScanLineOrdering::Progressive, "ex scanline ordering");
  DisplayModeEx currentMode{};
  DisplayRotation rotation = DisplayRotation::Rotate90;
  check(d3d->GetAdapterDisplayModeEx(0, &currentMode, &rotation), "ex adapter display mode");
  checkEq(rotation, DisplayRotation::Identity, "ex rotation");
  Luid luid0{};
  Luid luid1{};
  check(d3d->GetAdapterLUID(0, &luid0), "ex adapter luid");
  check(d3d->GetAdapterLUID(0, &luid1), "ex adapter luid stable");
  checkEq(luid0.lowPart, luid1.lowPart, "luid low stable");
  checkEq(luid0.highPart, luid1.highPart, "luid high stable");
  check(luid0.lowPart != 0 || luid0.highPart != 0, "luid non-zero");

  PresentParameters params{};
  params.windowed = false;
  params.backBufferWidth = 0;
  params.backBufferHeight = 0;
  params.backBufferFormat = Format::Unknown;
  params.presentationInterval = PresentInterval::Default;
  params.deviceWindow = Handle{202};
  DisplayModeEx fullscreenMode{};
  fullscreenMode.width = 1024;
  fullscreenMode.height = 768;
  fullscreenMode.format = Format::A8R8G8B8;

  auto* device = d3d->CreateDeviceEx(0, params, &fullscreenMode);
  check(device != nullptr, "wrapper ex device create");
  check(device->coreDevice().swapChain() != nullptr, "wrapper ex core device swap chain");
  void* deviceUnknown = nullptr;
  check(device->QueryInterface(InterfaceId::Direct3DDevice9Ex, &deviceUnknown), "device ex query interface");
  auto* queriedDevice = static_cast<IDirect3DDevice9Ex*>(deviceUnknown);
  check(queriedDevice != nullptr, "device ex query result");
  checkEq(queriedDevice->Release(), 1u, "device ex qi release");
  checkEq(device->GetMaximumFrameLatency(), 3u, "default max frame latency");
  checkEq(device->SetMaximumFrameLatency(0), D3D_OK, "set frame latency clamp low");
  checkEq(device->GetMaximumFrameLatency(), 1u, "clamped low frame latency");
  checkEq(backend->maxFrameLatencyCalls.back(), 1u, "backend received low frame latency");
  checkEq(device->SetMaximumFrameLatency(99), D3D_OK, "set frame latency clamp high");
  checkEq(device->GetMaximumFrameLatency(), 3u, "clamped high frame latency");
  checkEq(backend->maxFrameLatencyCalls.back(), 3u, "backend received high frame latency");

  checkEq(device->CheckDeviceState(params.deviceWindow), D3D_OK, "initial device state");
  backend->triggerPresentationOccluded(true);
  checkEq(device->CheckDeviceState(params.deviceWindow), S_PRESENT_OCCLUDED, "occluded device state");
  backend->triggerDeviceLost(true);
  checkEq(device->CheckDeviceState(params.deviceWindow), D3DERR_DEVICELOST, "lost beats occluded");
  checkEq(device->ResetEx(params, &fullscreenMode), D3D_OK, "device ex reset");
  checkEq(device->CheckDeviceState(params.deviceWindow), D3D_OK, "device recovered after reset");
  checkEq(device->coreDevice().presentParameters().backBufferWidth, 1024u, "reset ex width");
  checkEq(device->coreDevice().presentParameters().backBufferHeight, 768u, "reset ex height");
  const auto displayModeEx = device->GetDisplayModeEx();
  checkEq(displayModeEx.width, 1024u, "device ex display mode width");
  checkEq(displayModeEx.height, 768u, "device ex display mode height");
  checkEq(displayModeEx.scanLineOrdering, DisplayScanLineOrdering::Progressive,
          "device ex display mode scanline");

  checkEq(device->WaitForVBlank(0), D3D_OK, "wait for vblank");
  checkEq(backend->waitForVBlankCalls.size(), size_t{1}, "backend wait for vblank call");
  checkEq(device->CheckResourceResidency(), S_OK, "check resource residency");
  i32 priority = 123;
  checkEq(device->GetGPUThreadPriority(&priority), D3D_OK, "get gpu priority");
  checkEq(priority, 0, "gpu priority zero");
  checkEq(device->SetGPUThreadPriority(7), D3D_OK, "set gpu priority");
  checkEq(device->SetConvolutionMonoKernel(), E_NOTIMPL, "mono kernel not impl");
  checkEq(device->ComposeRects(), E_NOTIMPL, "compose rects not impl");

  Handle sharedHandle{123};
  auto rt = device->CreateRenderTargetEx({128, 64, Format::A8R8G8B8, Pool::Default, UsageRenderTarget, true,
                                          false, MultiSampleType::Four},
                                         &sharedHandle);
  check(rt != nullptr, "render target ex");
  checkEq(sharedHandle.value, 0u, "render target shared handle cleared");
  check(rt->desc().renderTarget, "render target flagged");

  sharedHandle = Handle{123};
  auto offscreen = device->CreateOffscreenPlainSurfaceEx({32, 32, Format::A8R8G8B8, Pool::SystemMem, 0, false,
                                                          false, MultiSampleType::None},
                                                         &sharedHandle);
  check(offscreen != nullptr, "offscreen ex");
  checkEq(sharedHandle.value, 0u, "offscreen shared handle cleared");

  sharedHandle = Handle{123};
  auto depth = device->CreateDepthStencilSurfaceEx({64, 64, Format::D24S8, Pool::Default, UsageDepthStencil, false,
                                                    true, MultiSampleType::Two},
                                                   &sharedHandle);
  check(depth != nullptr, "depth ex");
  checkEq(sharedHandle.value, 0u, "depth shared handle cleared");
  check(depth->desc().depthStencil, "depth flagged");

  checkEq(device->Release(), 0u, "device ex release");
  checkEq(d3d->Release(), 0u, "factory ex release");
}

void testFullscreenAndDeviceLost() {
  auto backend = std::make_shared<RecordingBackend>();
  Factory factory({}, backend);

  PresentParameters params{};
  params.windowed = false;
  params.backBufferWidth = 0;
  params.backBufferHeight = 0;
  params.backBufferFormat = Format::Unknown;
  params.presentationInterval = PresentInterval::Immediate;
  params.deviceWindow = Handle{101};

  auto device = factory.createDevice(0, params);
  check(device != nullptr, "fullscreen device create");
  checkEq(device->presentParameters().windowed, false, "fullscreen mode stored");
  checkEq(device->presentParameters().backBufferWidth, 1920u, "fullscreen default width stored");
  checkEq(device->presentParameters().backBufferHeight, 1080u, "fullscreen default height stored");
  checkEq(device->swapChain()->backBuffer()->desc().width, 1920u, "fullscreen back buffer width");
  checkEq(device->swapChain()->backBuffer()->desc().height, 1080u, "fullscreen back buffer height");
  checkEq(device->swapChain()->backBuffer()->desc().format, Format::A8R8G8B8, "fullscreen back buffer format");
  checkEq(device->testCooperativeLevel(), D3D_OK, "fullscreen cooperative level");

  backend->triggerDeviceLost(true);
  checkEq(device->testCooperativeLevel(), D3DERR_DEVICELOST, "triggered lost state");
  checkEq(device->present(), D3DERR_DEVICELOST, "present while lost");
  checkEq(device->reset(params), D3D_OK, "reset after device lost");
  checkEq(device->testCooperativeLevel(), D3D_OK, "recovered cooperative level");
  checkEq(device->swapChain()->backBuffer()->desc().width, 1920u, "fullscreen reset width");
  checkEq(device->swapChain()->backBuffer()->desc().height, 1080u, "fullscreen reset height");
}

}  // namespace

int main() {
  try {
    testFormatAndCaps();
    testHelpers();
    testFfpKeys();
    testShaderThunk();
    testVisualDerivedFfpCoverage();
    testVisualPortCoverage();
    testRasterStateCoverage();
    testDeviceCoreFlow();
    testComWrappers();
    testComWrappersEx();
    testFullscreenAndDeviceLost();
  } catch (const TestFailure& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "unexpected exception: " << error.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
