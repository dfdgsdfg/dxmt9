#include <algorithm>
#include <bit>
#include <array>
#include <cstdio>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <unordered_map>
#include <optional>
#include <span>
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

struct ShaderSection {
  enum class Kind { Vertex, Pixel };

  Kind kind = Kind::Pixel;
  std::vector<u8> bytecode;
};

struct SourceExpectation {
  ShaderSection::Kind kind = ShaderSection::Kind::Pixel;
  std::string needle;
};

struct ProbeExpectation {
  u32 target = 0;
  u32 x = 0;
  u32 y = 0;
  std::array<u8, 4> expected{};
  u32 tolerance = 0;
};

struct TextureLevelSetup {
  u32 width = 0;
  u32 height = 0;
  std::vector<std::array<u8, 4>> texels;
  std::vector<u8> rawBytes;
};

struct TextureSetup {
  u32 id = 0;
  Format format = Format::A8R8G8B8;
  std::vector<TextureLevelSetup> levels;
};

struct SamplerSetup {
  u32 stage = 0;
  std::unordered_map<u32, u32> states;
};

struct TextureStageSetup {
  u32 stage = 0;
  std::unordered_map<u32, u32> states;
};

struct TextureTransformSetup {
  u32 stage = 0;
  Matrix4x4 matrix{};
};

struct TextureBind {
  u32 stage = 0;
  u32 textureId = 0;
};

struct RenderTargetSetup {
  u32 slot = 0;
  Format format = Format::A8R8G8B8;
};

struct ClipPlaneSetup {
  u32 index = 0;
  ClipPlane plane{};
};

struct RenderStateSetup {
  u32 state = 0;
  u32 value = 0;
};

struct SolidRectDraw {
  float left = 0.0f;
  float top = 0.0f;
  float right = 0.0f;
  float bottom = 0.0f;
};

struct SolidQuadXyzDepthDraw {
  float z = 0.0f;
};

struct CorpusTest {
  std::string path;
  std::optional<ShaderSection> vertexShader;
  std::optional<ShaderSection> pixelShader;
  std::vector<SourceExpectation> sourceExpectations;
  std::vector<ProbeExpectation> probes;
  std::vector<TextureSetup> textureSetups;
  std::vector<SamplerSetup> samplerSetups;
  std::vector<TextureStageSetup> textureStageSetups;
  std::vector<TextureTransformSetup> textureTransformSetups;
  std::vector<TextureBind> textureBinds;
  std::vector<RenderTargetSetup> renderTargetSetups;
  std::vector<ClipPlaneSetup> clipPlaneSetups;
  std::vector<RenderStateSetup> renderStateSetups;
  std::vector<SolidRectDraw> solidRectDraws;
  std::vector<SolidQuadXyzDepthDraw> solidQuadXyzDepthDraws;
  std::optional<ColorRGBA> clearColor;
  std::optional<Viewport> viewport;
  std::optional<Rect> scissor;
  bool drawQuad = false;
  bool drawDxmt9TexturedQuad = false;
  bool drawDxmt9TexturedQuadOverscan = false;
  bool drawDxmt9TexturedQuadTex2 = false;
  bool drawDxmt9TexturedQuadXyz = false;
  bool drawDxmt9SolidQuad = false;
  bool drawDxmt9VsColorTriangle = false;
  bool drawDxmt9VsMultistreamTexturedQuad = false;
  bool drawDxmt9VsSkinnedTriangle = false;
  bool drawDxmt9FfpVertexBlendTriangle = false;
  bool drawDxmt9FfpVertexBlend2WeightsTriangle = false;
  bool drawDxmt9FfpVertexBlend3WeightsTriangle = false;
  bool drawDxmt9FfpVertexBlendIndexedTriangle = false;
  bool drawDxmt9FfpVertexBlendFvfXyzb2Triangle = false;
  bool drawDxmt9ODepthOverlap = false;
  u32 clipPlaneMask = 0;
  std::optional<u32> colorWriteMask;
  std::optional<u32> zEnable;
  std::optional<u32> zWriteEnable;
  std::optional<u32> zFunc;
  std::optional<u32> cullMode;
  std::optional<float> clearDepth;
  bool alphaTestEnable = false;
  CompareFunc alphaTestFunc = CompareFunc::Always;
  float alphaRef = 0.0f;
  bool provenanceSeen = false;
  bool sourceSeen = false;
  bool oracleSeen = false;
};

[[noreturn]] void fail(std::string_view message) {
  throw std::runtime_error(std::string(message));
}

constexpr u32 kFvfXyzrhw = 0x0004u;
constexpr u32 kFvfXyz = 0x0002u;
constexpr u32 kFvfXyzB2 = 0x0008u;
constexpr u32 kFvfDiffuse = 0x0040u;
constexpr u32 kFvfTex1 = 0x0100u;
constexpr u32 kFvfTex2 = 0x0200u;
constexpr u32 kDeclTypeFloat1 = 0u;
constexpr u32 kDeclTypeFloat4 = 3u;
constexpr u32 kDeclTypeD3DColor = 4u;
constexpr u32 kDeclTypeFloat3 = 2u;
constexpr u32 kDeclTypeUByte4 = 5u;
constexpr u32 kDeclTypeShort4N = 10u;
constexpr u32 kDeclUsagePosition = 0u;
constexpr u32 kDeclUsageBlendWeight = 1u;
constexpr u32 kDeclUsageBlendIndices = 2u;
constexpr u32 kDeclUsageColor = 10u;
constexpr u32 kTextureArgDiffuse = 0u;
constexpr u32 kTextureArgCurrent = 1u;
constexpr u32 kTextureArgTexture = 2u;
constexpr u32 kTextureArgTFactor = 3u;
constexpr u32 kTextureArgSpecular = 4u;
constexpr u32 kTextureArgTemp = 5u;
constexpr u32 kTextureAddressWrap = 1u;
constexpr u32 kTextureAddressMirror = 2u;
constexpr u32 kTextureAddressClamp = 3u;
constexpr u32 kTextureAddressBorder = 4u;
constexpr u32 kTextureFilterNone = 0u;
constexpr u32 kTextureFilterPoint = 1u;
constexpr u32 kTextureFilterLinear = 2u;
constexpr u32 kTextureFilterAnisotropic = 3u;
constexpr u32 kDeclTypeFloat2 = 1u;

struct ScreenSpaceTexturedVertex {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float rhw = 1.0f;
  float u = 0.0f;
  float v = 0.0f;
};

struct ScreenSpaceTexturedVertexTex2 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float rhw = 1.0f;
  float u0 = 0.0f;
  float v0 = 0.0f;
  float u1 = 0.0f;
  float v1 = 0.0f;
};

struct ScreenSpaceTexturedVertexXyz {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float u = 0.0f;
  float v = 0.0f;
};

struct ScreenSpaceVertex {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float rhw = 1.0f;
};

struct ScreenSpaceColorVertex {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float rhw = 1.0f;
  u32 color = 0xffffffffu;
};

struct ProgrammedColorVertex {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float w = 1.0f;
  u32 color = 0xffffffffu;
};

struct MultiStreamPositionVertex {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float w = 1.0f;
};

struct MultiStreamTexcoordVertex {
  float pad0 = 0.0f;
  float pad1 = 0.0f;
  float pad2 = 0.0f;
  float u0 = 0.0f;
  float v0 = 0.0f;
  float u1 = 0.0f;
  float v1 = 0.0f;
};

struct SkinnedPositionVertex {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

struct SkinnedPoseVertex {
  std::int16_t weight0 = 0;
  std::int16_t weight1 = 0;
  std::int16_t weight2 = 0;
  std::int16_t weight3 = 0;
  u8 index0 = 0;
  u8 index1 = 0;
  u8 index2 = 0;
  u8 index3 = 0;
};

struct FfpVertexBlendVertex {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float weight = 0.0f;
  u32 color = 0xffffffffu;
};

struct FfpVertexBlend2WeightsVertex {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float weight0 = 0.0f;
  float weight1 = 0.0f;
  u32 color = 0xffffffffu;
};

struct FfpVertexBlendIndexedVertex {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float weight = 0.0f;
  u8 index0 = 0;
  u8 index1 = 0;
  u8 index2 = 0;
  u8 index3 = 0;
  u32 color = 0xffffffffu;
};

struct FfpVertexBlend3WeightsVertex {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float weight0 = 0.0f;
  float weight1 = 0.0f;
  float weight2 = 0.0f;
  u32 color = 0xffffffffu;
};

std::string trim(std::string_view text) {
  size_t begin = 0;
  while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
    ++begin;
  }
  size_t end = text.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
    --end;
  }
  return std::string(text.substr(begin, end - begin));
}

std::string normalizeToken(std::string_view text) {
  std::string result;
  for (unsigned char c : text) {
    if (std::isalnum(c) != 0) {
      result.push_back(static_cast<char>(std::tolower(c)));
    }
  }
  return result;
}

u8 clampToByte(float value) {
  value = std::clamp(value, 0.0f, 1.0f);
  return static_cast<u8>(std::lround(value * 255.0f)) & 0xffu;
}

std::array<u8, 4> toBGRA(const ColorRGBA& color) {
  return {clampToByte(color.b), clampToByte(color.g), clampToByte(color.r), clampToByte(color.a)};
}

std::optional<std::array<u8, 4>> parseRgbaColor(std::string_view text) {
  if (!text.starts_with("rgba(") || !text.ends_with(")")) {
    return std::nullopt;
  }
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 0.0f;
  if (std::sscanf(std::string(text).c_str(), "rgba(%f,%f,%f,%f)", &r, &g, &b, &a) != 4) {
    fail("invalid dxmt9 texture rgba color");
  }
  return toBGRA({r, g, b, a});
}

std::array<u8, 4> parseTextureColor(std::string_view text) {
  if (auto rgba = parseRgbaColor(text)) {
    return *rgba;
  }
  const auto token = normalizeToken(text);
  if (token == "black") {
    return toBGRA({0.0f, 0.0f, 0.0f, 1.0f});
  }
  if (token == "red") {
    return toBGRA({1.0f, 0.0f, 0.0f, 1.0f});
  }
  if (token == "green") {
    return toBGRA({0.0f, 1.0f, 0.0f, 1.0f});
  }
  if (token == "blue") {
    return toBGRA({0.0f, 0.0f, 1.0f, 1.0f});
  }
  if (token == "white") {
    return toBGRA({1.0f, 1.0f, 1.0f, 1.0f});
  }
  fail("unsupported dxmt9 texture color name");
}

Format parseDxmt9Format(std::string_view text) {
  const auto token = normalizeToken(text);
  if (token == "a8r8g8b8") {
    return Format::A8R8G8B8;
  }
  if (token == "l8") {
    return Format::L8;
  }
  if (token == "a8l8") {
    return Format::A8L8;
  }
  if (token == "dxt1") {
    return Format::DXT1;
  }
  if (token == "dxt3") {
    return Format::DXT3;
  }
  if (token == "dxt5") {
    return Format::DXT5;
  }
  fail("unsupported dxmt9 format");
}

void writeDxmt9TextureTexel(u8* dst, Format format, const std::array<u8, 4>& bgra) {
  switch (format) {
  case Format::A8R8G8B8:
    std::memcpy(dst, bgra.data(), bgra.size());
    return;
  case Format::L8:
    dst[0] = bgra[2];
    return;
  case Format::A8L8:
    dst[0] = bgra[2];
    dst[1] = bgra[3];
    return;
  default:
    fail("unsupported dxmt9 texture upload format");
  }
}

std::uint16_t rgb565FromBGRA(const std::array<u8, 4>& bgra) {
  const auto r = static_cast<std::uint16_t>((static_cast<u32>(bgra[2]) * 31u + 127u) / 255u);
  const auto g = static_cast<std::uint16_t>((static_cast<u32>(bgra[1]) * 63u + 127u) / 255u);
  const auto b = static_cast<std::uint16_t>((static_cast<u32>(bgra[0]) * 31u + 127u) / 255u);
  return static_cast<std::uint16_t>((r << 11u) | (g << 5u) | b);
}

void writeLe16(u8* dst, std::uint16_t value) {
  dst[0] = static_cast<u8>(value & 0xffu);
  dst[1] = static_cast<u8>((value >> 8u) & 0xffu);
}

bool isDxmt9CompressedTextureFormat(Format format) {
  return format == Format::DXT1 || format == Format::DXT3 || format == Format::DXT5;
}

void writeConstantDxtColorBlock(u8* dst, const std::array<u8, 4>& bgra) {
  writeLe16(dst, rgb565FromBGRA(bgra));
  writeLe16(dst + 2, 0u);
  std::memset(dst + 4, 0, 4);
}

void writeConstantDxtBlock(u8* dst, Format format, const std::array<u8, 4>& bgra) {
  switch (format) {
  case Format::DXT1:
    writeConstantDxtColorBlock(dst, bgra);
    return;
  case Format::DXT3: {
    const u8 a4 = static_cast<u8>((static_cast<u32>(bgra[3]) * 15u + 127u) / 255u);
    const u8 packed = static_cast<u8>(a4 | (a4 << 4u));
    std::memset(dst, packed, 8);
    writeConstantDxtColorBlock(dst + 8, bgra);
    return;
  }
  case Format::DXT5:
    dst[0] = bgra[3];
    dst[1] = 0u;
    std::memset(dst + 2, 0, 6);
    writeConstantDxtColorBlock(dst + 8, bgra);
    return;
  default:
    fail("unsupported dxmt9 compressed texture format");
  }
}

void writeDxmt9TextureLevel(u8* dst, u32 pitch, Format format,
                            const TextureLevelSetup& level) {
  if (!level.rawBytes.empty()) {
    const u32 rowPitch = formatRowPitch(format, level.width);
    const u32 rowCount = formatRowCount(format, level.height);
    if (rowPitch == 0 || rowCount == 0) {
      fail("dxmt9 raw texture upload requires a known row pitch");
    }
    const auto expectedSize =
        static_cast<size_t>(rowPitch) * static_cast<size_t>(rowCount);
    if (level.rawBytes.size() != expectedSize) {
      fail("dxmt9 raw texture byte count does not match format storage size");
    }
    if (pitch < rowPitch) {
      fail("dxmt9 raw texture upload pitch is too small");
    }
    for (u32 row = 0; row < rowCount; ++row) {
      std::memcpy(dst + static_cast<size_t>(row) * pitch,
                  level.rawBytes.data() + static_cast<size_t>(row) * rowPitch,
                  rowPitch);
    }
    return;
  }

  if (isDxmt9CompressedTextureFormat(format)) {
    const u32 blockWidth = formatBlockWidth(format);
    const u32 blockHeight = formatBlockHeight(format);
    const u32 blockBytes = formatBlockBytes(format);
    if (blockWidth == 0 || blockHeight == 0 || blockBytes == 0 ||
        (level.width % blockWidth) != 0 || (level.height % blockHeight) != 0) {
      fail("dxmt9 compressed texture level must be block-aligned");
    }
    for (u32 by = 0; by < level.height / blockHeight; ++by) {
      for (u32 bx = 0; bx < level.width / blockWidth; ++bx) {
        const auto& first = level.texels[
            static_cast<size_t>(by * blockHeight) * level.width +
            static_cast<size_t>(bx * blockWidth)];
        for (u32 y = 0; y < blockHeight; ++y) {
          for (u32 x = 0; x < blockWidth; ++x) {
            const auto& texel = level.texels[
                static_cast<size_t>(by * blockHeight + y) * level.width +
                static_cast<size_t>(bx * blockWidth + x)];
            if (texel != first) {
              fail("dxmt9 compressed texture writer only supports constant 4x4 blocks");
            }
          }
        }
        writeConstantDxtBlock(dst + static_cast<size_t>(by) * pitch +
                                  static_cast<size_t>(bx) * blockBytes,
                              format, first);
      }
    }
    return;
  }

  const u32 texelBytes = bytesPerPixel(format);
  if (texelBytes == 0) {
    fail("dxmt9 texture upload requires an uncompressed byte-addressable format");
  }
  for (u32 y = 0; y < level.height; ++y) {
    for (u32 x = 0; x < level.width; ++x) {
      const auto& texel = level.texels[static_cast<size_t>(y) * level.width + x];
      writeDxmt9TextureTexel(
          dst + static_cast<size_t>(y) * pitch + static_cast<size_t>(x) * texelBytes,
          format, texel);
    }
  }
}

u8 hexNibble(char value) {
  if (value >= '0' && value <= '9') {
    return static_cast<u8>(value - '0');
  }
  if (value >= 'a' && value <= 'f') {
    return static_cast<u8>(10 + value - 'a');
  }
  if (value >= 'A' && value <= 'F') {
    return static_cast<u8>(10 + value - 'A');
  }
  fail("invalid dxmt9 raw texture hex digit");
}

std::vector<u8> parseHexBytes(std::string_view text) {
  if (text.size() >= 2 && text[0] == '0' &&
      (text[1] == 'x' || text[1] == 'X')) {
    text.remove_prefix(2);
  }
  if ((text.size() & 1u) != 0) {
    fail("dxmt9 raw texture hex byte string must have an even length");
  }
  std::vector<u8> bytes;
  bytes.reserve(text.size() / 2u);
  for (size_t i = 0; i < text.size(); i += 2u) {
    bytes.push_back(static_cast<u8>((hexNibble(text[i]) << 4u) |
                                    hexNibble(text[i + 1u])));
  }
  return bytes;
}

u32 parseAddressMode(std::string_view text) {
  const auto token = normalizeToken(text);
  if (token == "wrap") {
    return kTextureAddressWrap;
  }
  if (token == "mirror") {
    return kTextureAddressMirror;
  }
  if (token == "clamp") {
    return kTextureAddressClamp;
  }
  if (token == "border") {
    return kTextureAddressBorder;
  }
  fail("unsupported dxmt9 sampler address mode");
}

u32 parseTextureFilter(std::string_view text) {
  const auto token = normalizeToken(text);
  if (token == "none") {
    return kTextureFilterNone;
  }
  if (token == "point") {
    return kTextureFilterPoint;
  }
  if (token == "linear") {
    return kTextureFilterLinear;
  }
  if (token == "anisotropic") {
    return kTextureFilterAnisotropic;
  }
  fail("unsupported dxmt9 sampler filter");
}

u32 parseU32Value(std::string_view text);
u32 parseBoolState(std::string_view text);

u32 bitCastFloatState(std::string_view text) {
  size_t parsed = 0;
  const auto value = std::stof(std::string(text), &parsed);
  if (parsed != text.size() || !std::isfinite(value)) {
    fail("invalid float render-state value");
  }
  return std::bit_cast<u32>(value);
}

u32 parseFogMode(std::string_view text) {
  const auto token = normalizeToken(text);
  if (token == "none" || token == "off" || token == "disabled") {
    return static_cast<u32>(FogMode::None);
  }
  if (token == "linear") {
    return static_cast<u32>(FogMode::Linear);
  }
  if (token == "exp") {
    return static_cast<u32>(FogMode::Exp);
  }
  if (token == "exp2") {
    return static_cast<u32>(FogMode::Exp2);
  }
  return parseU32Value(text);
}

u32 parseBlendFactor(std::string_view text) {
  const auto token = normalizeToken(text);
  if (token == "zero") {
    return static_cast<u32>(BlendFactor::Zero);
  }
  if (token == "one") {
    return static_cast<u32>(BlendFactor::One);
  }
  if (token == "srccolor" || token == "sourcecolor") {
    return static_cast<u32>(BlendFactor::SrcColor);
  }
  if (token == "invsrccolor" || token == "invsourcecolor" ||
      token == "oneminussrccolor" || token == "oneminussourcecolor") {
    return static_cast<u32>(BlendFactor::InvSrcColor);
  }
  if (token == "srcalpha" || token == "sourcealpha") {
    return static_cast<u32>(BlendFactor::SrcAlpha);
  }
  if (token == "invsrcalpha" || token == "invsourcealpha" ||
      token == "oneminussrcalpha" || token == "oneminussourcealpha") {
    return static_cast<u32>(BlendFactor::InvSrcAlpha);
  }
  if (token == "destalpha" || token == "dstalpha" || token == "destinationalpha") {
    return static_cast<u32>(BlendFactor::DestAlpha);
  }
  if (token == "invdestalpha" || token == "invdstalpha" ||
      token == "invdestinationalpha" || token == "oneminusdestalpha" ||
      token == "oneminusdstalpha" || token == "oneminusdestinationalpha") {
    return static_cast<u32>(BlendFactor::InvDestAlpha);
  }
  if (token == "destcolor" || token == "dstcolor" || token == "destinationcolor") {
    return static_cast<u32>(BlendFactor::DestColor);
  }
  if (token == "invdestcolor" || token == "invdstcolor" ||
      token == "invdestinationcolor" || token == "oneminusdestcolor" ||
      token == "oneminusdstcolor" || token == "oneminusdestinationcolor") {
    return static_cast<u32>(BlendFactor::InvDestColor);
  }
  if (token == "srcalphasat" || token == "sourcealphasat" ||
      token == "srcalphasaturate" || token == "sourcealphasaturate") {
    return static_cast<u32>(BlendFactor::SrcAlphaSat);
  }
  if (token == "blendfactor") {
    return static_cast<u32>(BlendFactor::BlendFactor);
  }
  if (token == "invblendfactor" || token == "oneminusblendfactor") {
    return static_cast<u32>(BlendFactor::InvBlendFactor);
  }
  return parseU32Value(text);
}

u32 parseBlendOp(std::string_view text) {
  const auto token = normalizeToken(text);
  if (token == "add") {
    return static_cast<u32>(BlendOp::Add);
  }
  if (token == "subtract" || token == "sub") {
    return static_cast<u32>(BlendOp::Subtract);
  }
  if (token == "revsubtract" || token == "reversesubtract" || token == "revsub") {
    return static_cast<u32>(BlendOp::RevSubtract);
  }
  if (token == "min") {
    return static_cast<u32>(BlendOp::Min);
  }
  if (token == "max") {
    return static_cast<u32>(BlendOp::Max);
  }
  return parseU32Value(text);
}

u32 parseFillMode(std::string_view text) {
  const auto token = normalizeToken(text);
  if (token == "point") {
    return 1u;
  }
  if (token == "wireframe" || token == "line" || token == "lines") {
    return 2u;
  }
  if (token == "solid" || token == "fill") {
    return 3u;
  }
  return parseU32Value(text);
}

u32 parseU32Value(std::string_view text) {
  size_t parsed = 0;
  const auto value = std::stoul(std::string(text), &parsed, 0);
  if (parsed != text.size() || value > std::numeric_limits<u32>::max()) {
    fail("invalid u32 value");
  }
  return static_cast<u32>(value);
}

u32 parseTextureStageOp(std::string_view text) {
  const auto token = normalizeToken(text);
  if (token == "disable" || token == "disabled") {
    return static_cast<u32>(TextureOp::Disable);
  }
  if (token == "selectarg1") {
    return static_cast<u32>(TextureOp::SelectArg1);
  }
  if (token == "selectarg2") {
    return static_cast<u32>(TextureOp::SelectArg2);
  }
  if (token == "modulate") {
    return static_cast<u32>(TextureOp::Modulate);
  }
  if (token == "modulate2x") {
    return static_cast<u32>(TextureOp::Modulate2x);
  }
  if (token == "modulate4x") {
    return static_cast<u32>(TextureOp::Modulate4x);
  }
  if (token == "add") {
    return static_cast<u32>(TextureOp::Add);
  }
  if (token == "addsigned") {
    return static_cast<u32>(TextureOp::AddSigned);
  }
  if (token == "addsigned2x") {
    return static_cast<u32>(TextureOp::AddSigned2x);
  }
  if (token == "subtract") {
    return static_cast<u32>(TextureOp::Subtract);
  }
  if (token == "addsmooth") {
    return static_cast<u32>(TextureOp::AddSmooth);
  }
  if (token == "dotproduct3") {
    return static_cast<u32>(TextureOp::DotProduct3);
  }
  if (token == "lerp") {
    return static_cast<u32>(TextureOp::Lerp);
  }
  return parseU32Value(text);
}

u32 parseTextureStageArg(std::string_view text) {
  const auto token = normalizeToken(text);
  if (token == "diffuse") {
    return kTextureArgDiffuse;
  }
  if (token == "current") {
    return kTextureArgCurrent;
  }
  if (token == "texture") {
    return kTextureArgTexture;
  }
  if (token == "tfactor") {
    return kTextureArgTFactor;
  }
  if (token == "specular") {
    return kTextureArgSpecular;
  }
  if (token == "temp") {
    return kTextureArgTemp;
  }
  return parseU32Value(text);
}

u32 parseTextureTransformFlags(std::string_view text) {
  const auto token = normalizeToken(text);
  if (token == "disable" || token == "disabled" || token == "none") {
    return 0u;
  }
  if (token == "count1") {
    return 1u;
  }
  if (token == "count2") {
    return 2u;
  }
  if (token == "count3") {
    return 3u;
  }
  if (token == "count4") {
    return 4u;
  }
  if (token == "projectedcount2" || token == "count2projected") {
    return 0x100u | 2u;
  }
  if (token == "projectedcount3" || token == "count3projected") {
    return 0x100u | 3u;
  }
  if (token == "projectedcount4" || token == "count4projected") {
    return 0x100u | 4u;
  }
  return parseU32Value(text);
}

std::optional<TextureSetup> parseDxmt9Texture(std::string_view line) {
  std::istringstream stream{std::string(line)};
  std::string command;
  std::string format;
  TextureSetup texture;
  if (!(stream >> command) || command != "dxmt9-texture") {
    return std::nullopt;
  }
  TextureLevelSetup level;
  if (!(stream >> texture.id >> level.width >> level.height >> format)) {
    fail("invalid dxmt9-texture command");
  }
  texture.format = parseDxmt9Format(format);
  if (level.width == 0 || level.height == 0) {
    fail("dxmt9-texture requires non-zero dimensions");
  }
  std::string color;
  while (stream >> color) {
    level.texels.push_back(parseTextureColor(color));
  }
  if (level.texels.size() != static_cast<size_t>(level.width) * level.height) {
    fail("dxmt9-texture texel count does not match dimensions");
  }
  texture.levels.push_back(std::move(level));
  return texture;
}

std::optional<TextureSetup> parseDxmt9TextureRaw(std::string_view line) {
  std::istringstream stream{std::string(line)};
  std::string command;
  std::string format;
  std::string hex;
  TextureSetup texture;
  TextureLevelSetup level;
  if (!(stream >> command) || command != "dxmt9-texture-raw") {
    return std::nullopt;
  }
  if (!(stream >> texture.id >> level.width >> level.height >> format >> hex)) {
    fail("invalid dxmt9-texture-raw command");
  }
  std::string extra;
  if (stream >> extra) {
    fail("dxmt9-texture-raw accepts exactly one hex byte string");
  }
  texture.format = parseDxmt9Format(format);
  if (level.width == 0 || level.height == 0) {
    fail("dxmt9-texture-raw requires non-zero dimensions");
  }
  level.rawBytes = parseHexBytes(hex);
  texture.levels.push_back(std::move(level));
  return texture;
}

bool parseDxmt9TextureMip(CorpusTest& test, std::string_view line) {
  std::istringstream stream{std::string(line)};
  std::string command;
  u32 textureId = 0;
  u32 levelIndex = 0;
  std::string format;
  TextureLevelSetup level;
  if (!(stream >> command) || command != "dxmt9-texture-mip") {
    return false;
  }
  if (!(stream >> textureId >> levelIndex >> level.width >> level.height >> format)) {
    fail("invalid dxmt9-texture-mip command");
  }
  const auto parsedFormat = parseDxmt9Format(format);
  if (level.width == 0 || level.height == 0) {
    fail("dxmt9-texture-mip requires non-zero dimensions");
  }
  std::string color;
  while (stream >> color) {
    level.texels.push_back(parseTextureColor(color));
  }
  if (level.texels.size() != static_cast<size_t>(level.width) * level.height) {
    fail("dxmt9-texture-mip texel count does not match dimensions");
  }

  auto existing = std::find_if(test.textureSetups.begin(), test.textureSetups.end(),
                               [textureId](const TextureSetup& setup) {
                                 return setup.id == textureId;
                               });
  if (existing == test.textureSetups.end()) {
    TextureSetup setup;
    setup.id = textureId;
    setup.format = parsedFormat;
    test.textureSetups.push_back(std::move(setup));
    existing = std::prev(test.textureSetups.end());
  } else if (existing->format != parsedFormat) {
    fail("dxmt9-texture-mip format mismatch");
  }

  if (existing->levels.size() <= levelIndex) {
    existing->levels.resize(static_cast<size_t>(levelIndex) + 1u);
  }
  if (existing->levels[levelIndex].width != 0 || existing->levels[levelIndex].height != 0) {
    fail("duplicate dxmt9-texture-mip level");
  }
  existing->levels[levelIndex] = std::move(level);
  return true;
}

std::optional<SamplerSetup> parseDxmt9Sampler(std::string_view line) {
  std::istringstream stream{std::string(line)};
  std::string command;
  SamplerSetup sampler;
  if (!(stream >> command) || command != "dxmt9-sampler") {
    return std::nullopt;
  }
  if (!(stream >> sampler.stage)) {
    fail("invalid dxmt9-sampler command");
  }
  std::string assignment;
  while (stream >> assignment) {
    const auto equals = assignment.find('=');
    if (equals == std::string::npos) {
      fail("dxmt9-sampler state must use key=value syntax");
    }
    const auto key = normalizeToken(std::string_view(assignment).substr(0, equals));
    const auto value = std::string_view(assignment).substr(equals + 1);
    if (key == "addressu") {
      sampler.states[SAMP_ADDRESS_U] = parseAddressMode(value);
    } else if (key == "addressv") {
      sampler.states[SAMP_ADDRESS_V] = parseAddressMode(value);
    } else if (key == "minfilter") {
      sampler.states[SAMP_MIN_FILTER] = parseTextureFilter(value);
    } else if (key == "magfilter") {
      sampler.states[SAMP_MAG_FILTER] = parseTextureFilter(value);
    } else if (key == "mipfilter") {
      sampler.states[SAMP_MIP_FILTER] = parseTextureFilter(value);
    } else if (key == "srgbtexture" || key == "srgb") {
      sampler.states[SAMP_SRGB_TEXTURE] = parseBoolState(value);
    } else {
      fail("unsupported dxmt9-sampler state");
    }
  }
  return sampler;
}

std::optional<TextureStageSetup> parseDxmt9TextureStage(std::string_view line) {
  std::istringstream stream{std::string(line)};
  std::string command;
  TextureStageSetup stage;
  if (!(stream >> command) || command != "dxmt9-texture-stage") {
    return std::nullopt;
  }
  if (!(stream >> stage.stage)) {
    fail("invalid dxmt9-texture-stage command");
  }
  std::string assignment;
  while (stream >> assignment) {
    const auto equals = assignment.find('=');
    if (equals == std::string::npos) {
      fail("dxmt9-texture-stage state must use key=value syntax");
    }
    const auto key = normalizeToken(std::string_view(assignment).substr(0, equals));
    const auto value = std::string_view(assignment).substr(equals + 1);
    if (key == "colorop") {
      stage.states[TSS_COLOR_OP] = parseTextureStageOp(value);
    } else if (key == "alphaop") {
      stage.states[TSS_ALPHA_OP] = parseTextureStageOp(value);
    } else if (key == "colorarg1") {
      stage.states[TSS_COLOR_ARG1] = parseTextureStageArg(value);
    } else if (key == "colorarg2") {
      stage.states[TSS_COLOR_ARG2] = parseTextureStageArg(value);
    } else if (key == "alphaarg1") {
      stage.states[TSS_ALPHA_ARG1] = parseTextureStageArg(value);
    } else if (key == "alphaarg2") {
      stage.states[TSS_ALPHA_ARG2] = parseTextureStageArg(value);
    } else if (key == "texcoordindex") {
      stage.states[TSS_TEXCOORD_INDEX] = parseU32Value(value);
    } else if (key == "texturetransform" || key == "texturetransformflags") {
      stage.states[TSS_TEXTURE_TRANSFORM_FLAGS] = parseTextureTransformFlags(value);
    } else {
      fail("unsupported dxmt9-texture-stage state");
    }
  }
  if (stage.states.empty()) {
    fail("dxmt9-texture-stage requires at least one state");
  }
  return stage;
}

std::optional<TextureTransformSetup> parseDxmt9TextureTransform(std::string_view line) {
  std::istringstream stream{std::string(line)};
  std::string command;
  TextureTransformSetup transform;
  if (!(stream >> command) || command != "dxmt9-texture-transform") {
    return std::nullopt;
  }
  if (!(stream >> transform.stage)) {
    fail("invalid dxmt9-texture-transform command");
  }
  for (auto& value : transform.matrix.m) {
    if (!(stream >> value)) {
      fail("dxmt9-texture-transform requires 16 float values");
    }
  }
  std::string extra;
  if (stream >> extra) {
    fail("dxmt9-texture-transform has extra values");
  }
  return transform;
}

std::optional<TextureBind> parseDxmt9BindTexture(std::string_view line) {
  std::istringstream stream{std::string(line)};
  std::string command;
  TextureBind bind;
  if (!(stream >> command) || command != "dxmt9-bind-texture") {
    return std::nullopt;
  }
  if (!(stream >> bind.stage >> bind.textureId)) {
    fail("invalid dxmt9-bind-texture command");
  }
  return bind;
}

std::optional<RenderTargetSetup> parseDxmt9RenderTarget(std::string_view line) {
  std::istringstream stream{std::string(line)};
  std::string command;
  std::string format;
  RenderTargetSetup target;
  if (!(stream >> command) || command != "dxmt9-render-target") {
    return std::nullopt;
  }
  if (!(stream >> target.slot >> format)) {
    fail("invalid dxmt9-render-target command");
  }
  target.format = parseDxmt9Format(format);
  return target;
}

std::optional<ClipPlaneSetup> parseDxmt9ClipPlane(std::string_view line) {
  std::istringstream stream{std::string(line)};
  std::string command;
  ClipPlaneSetup clipPlane;
  if (!(stream >> command) || command != "dxmt9-clip-plane") {
    return std::nullopt;
  }
  if (!(stream >> clipPlane.index >> clipPlane.plane[0] >> clipPlane.plane[1] >>
        clipPlane.plane[2] >> clipPlane.plane[3])) {
    fail("invalid dxmt9-clip-plane command");
  }
  if (clipPlane.index >= kMaxClipPlanes) {
    fail("dxmt9-clip-plane index out of range");
  }
  std::string extra;
  if (stream >> extra) {
    fail("dxmt9-clip-plane has extra values");
  }
  return clipPlane;
}

std::optional<SolidRectDraw> parseDxmt9SolidRect(std::string_view line) {
  std::istringstream stream{std::string(line)};
  std::string command;
  SolidRectDraw rect;
  if (!(stream >> command) || command != "dxmt9-draw-solid-rect") {
    return std::nullopt;
  }
  if (!(stream >> rect.left >> rect.top >> rect.right >> rect.bottom)) {
    fail("invalid dxmt9-draw-solid-rect command");
  }
  if (!(rect.left < rect.right) || !(rect.top < rect.bottom)) {
    fail("dxmt9-draw-solid-rect requires increasing coordinates");
  }
  return rect;
}

std::optional<SolidQuadXyzDepthDraw> parseDxmt9SolidQuadXyzDepth(std::string_view line) {
  std::istringstream stream{std::string(line)};
  std::string command;
  SolidQuadXyzDepthDraw draw;
  if (!(stream >> command) || command != "dxmt9-draw-solid-quad-xyz-depth") {
    return std::nullopt;
  }
  if (!(stream >> draw.z)) {
    fail("invalid dxmt9-draw-solid-quad-xyz-depth command");
  }
  std::string extra;
  if (stream >> extra) {
    fail("dxmt9-draw-solid-quad-xyz-depth has extra values");
  }
  return draw;
}

std::optional<Rect> parseDxmt9Scissor(std::string_view line) {
  std::istringstream stream{std::string(line)};
  std::string command;
  Rect rect;
  if (!(stream >> command) || command != "dxmt9-scissor") {
    return std::nullopt;
  }
  if (!(stream >> rect.left >> rect.top >> rect.right >> rect.bottom)) {
    fail("invalid dxmt9-scissor command");
  }
  if (rect.left > rect.right || rect.top > rect.bottom) {
    fail("dxmt9-scissor requires ordered edges");
  }
  std::string extra;
  if (stream >> extra) {
    fail("dxmt9-scissor has extra values");
  }
  return rect;
}

u32 parseColorWriteMask(std::string_view text) {
  const auto token = normalizeToken(text);
  if (token == "red" || token == "r") {
    return 0x1u;
  }
  if (token == "green" || token == "g") {
    return 0x2u;
  }
  if (token == "blue" || token == "b") {
    return 0x4u;
  }
  if (token == "alpha" || token == "a") {
    return 0x8u;
  }
  if (token == "rgb") {
    return 0x7u;
  }
  if (token == "rgba" || token == "all") {
    return 0xfu;
  }
  fail("unsupported dxmt9 color-write mask");
}

u32 parseBoolState(std::string_view text) {
  const auto token = normalizeToken(text);
  if (token == "true" || token == "enable" || token == "enabled" || token == "on" || token == "1") {
    return 1u;
  }
  if (token == "false" || token == "disable" || token == "disabled" || token == "off" || token == "0") {
    return 0u;
  }
  fail("unsupported dxmt9 boolean state");
}

CompareFunc parseCompareFunc(std::string_view text);

u32 parseCullMode(std::string_view text) {
  const auto token = normalizeToken(text);
  if (token == "none" || token == "off" || token == "disabled") {
    return static_cast<u32>(CullMode::None);
  }
  if (token == "cw" || token == "clockwise") {
    return static_cast<u32>(CullMode::Cw);
  }
  if (token == "ccw" || token == "counterclockwise") {
    return static_cast<u32>(CullMode::Ccw);
  }
  fail("unsupported dxmt9 cull mode");
}

bool parseDxmt9RenderState(CorpusTest& test, std::string_view line) {
  std::istringstream stream{std::string(line)};
  std::string command;
  if (!(stream >> command) || command != "dxmt9-render-state") {
    return false;
  }
  std::string assignment;
  bool sawState = false;
  while (stream >> assignment) {
    const auto equals = assignment.find('=');
    if (equals == std::string::npos) {
      fail("dxmt9-render-state state must use key=value syntax");
    }
    const auto key = normalizeToken(std::string_view(assignment).substr(0, equals));
    const auto value = std::string_view(assignment).substr(equals + 1);
    if (key == "colorwrite") {
      test.colorWriteMask = parseColorWriteMask(value);
      sawState = true;
    } else if (key == "zenable") {
      test.zEnable = parseBoolState(value);
      sawState = true;
    } else if (key == "zwrite") {
      test.zWriteEnable = parseBoolState(value);
      sawState = true;
    } else if (key == "zfunc") {
      test.zFunc = static_cast<u32>(parseCompareFunc(value));
      sawState = true;
    } else if (key == "cull" || key == "cullmode") {
      test.cullMode = parseCullMode(value);
      sawState = true;
    } else if (key == "fill" || key == "fillmode") {
      test.renderStateSetups.push_back({RS_FILL_MODE, parseFillMode(value)});
      sawState = true;
    } else if (key == "clipplanes" || key == "clipplane" ||
               key == "clipplaneenable" || key == "clipplaneenabled") {
      test.renderStateSetups.push_back({RS_CLIP_PLANE_ENABLE, parseU32Value(value)});
      sawState = true;
    } else if (key == "srgbwrite" || key == "srgbwriteenable" ||
               key == "srgbwriteenabled") {
      test.renderStateSetups.push_back({RS_SRGB_WRITE_ENABLE, parseBoolState(value)});
      sawState = true;
    } else if (key == "fogenable" || key == "fogenabled" || key == "fog") {
      test.renderStateSetups.push_back({RS_FOG_ENABLE, parseBoolState(value)});
      sawState = true;
    } else if (key == "fogtablemode" || key == "fogmode") {
      test.renderStateSetups.push_back({RS_FOG_TABLE_MODE, parseFogMode(value)});
      sawState = true;
    } else if (key == "fogstart") {
      test.renderStateSetups.push_back({RS_FOG_START, bitCastFloatState(value)});
      sawState = true;
    } else if (key == "fogend") {
      test.renderStateSetups.push_back({RS_FOG_END, bitCastFloatState(value)});
      sawState = true;
    } else if (key == "fogdensity") {
      test.renderStateSetups.push_back({RS_FOG_DENSITY, bitCastFloatState(value)});
      sawState = true;
    } else if (key == "alphablend" || key == "alphablendenable" ||
               key == "alphablendenabled") {
      test.renderStateSetups.push_back({RS_ALPHABLEND_ENABLE, parseBoolState(value)});
      sawState = true;
    } else if (key == "srcblend" || key == "sourceblend") {
      test.renderStateSetups.push_back({RS_SRC_BLEND, parseBlendFactor(value)});
      sawState = true;
    } else if (key == "destblend" || key == "dstblend" ||
               key == "destinationblend") {
      test.renderStateSetups.push_back({RS_DEST_BLEND, parseBlendFactor(value)});
      sawState = true;
    } else if (key == "blendop" || key == "blendoperation") {
      test.renderStateSetups.push_back({RS_BLEND_OP, parseBlendOp(value)});
      sawState = true;
    } else if (key == "separatealphablend" ||
               key == "separatealphablendenable" ||
               key == "separatealphablendenabled") {
      test.renderStateSetups.push_back({RS_SEPARATE_ALPHA_BLEND_ENABLE, parseBoolState(value)});
      sawState = true;
    } else if (key == "srcblendalpha" || key == "sourceblendalpha") {
      test.renderStateSetups.push_back({RS_SRC_BLEND_ALPHA, parseBlendFactor(value)});
      sawState = true;
    } else if (key == "destblendalpha" || key == "dstblendalpha" ||
               key == "destinationblendalpha") {
      test.renderStateSetups.push_back({RS_DEST_BLEND_ALPHA, parseBlendFactor(value)});
      sawState = true;
    } else if (key == "blendopalpha" || key == "blendoperationalpha") {
      test.renderStateSetups.push_back({RS_BLEND_OP_ALPHA, parseBlendOp(value)});
      sawState = true;
    } else if (key == "scissor" || key == "scissortest" ||
               key == "scissortestenable" || key == "scissortestenabled") {
      test.renderStateSetups.push_back({RS_SCISSOR_TEST_ENABLE, parseBoolState(value)});
      sawState = true;
    } else {
      fail("unsupported dxmt9-render-state state");
    }
  }
  if (!sawState) {
    fail("dxmt9-render-state requires at least one state");
  }
  return true;
}

CompareFunc parseCompareFunc(std::string_view text) {
  const auto token = normalizeToken(text);
  if (token == "never") {
    return CompareFunc::Never;
  }
  if (token == "less") {
    return CompareFunc::Less;
  }
  if (token == "equal") {
    return CompareFunc::Equal;
  }
  if (token == "lessequal" || token == "le") {
    return CompareFunc::LessEqual;
  }
  if (token == "greater") {
    return CompareFunc::Greater;
  }
  if (token == "notequal") {
    return CompareFunc::NotEqual;
  }
  if (token == "greaterequal" || token == "ge") {
    return CompareFunc::GreaterEqual;
  }
  if (token == "always") {
    return CompareFunc::Always;
  }
  fail("unsupported comparison function");
}

std::vector<u8> parseHexWords(std::string_view text) {
  std::vector<u8> bytes;
  std::string line(text);
  const auto semicolon = line.find(';');
  if (semicolon != std::string::npos) {
    line.resize(semicolon);
  }
  std::istringstream stream(line);
  std::string token;
  while (stream >> token) {
    if (token.empty()) {
      continue;
    }
    if (token.starts_with("0x") || token.starts_with("0X")) {
      token = token.substr(2);
    }
    token.erase(std::remove_if(token.begin(), token.end(), [](unsigned char c) { return c == ',' || c == ';'; }),
                token.end());
    if (token.empty()) {
      continue;
    }
    const auto word = static_cast<u32>(std::stoul(token, nullptr, 16));
    const auto raw = std::bit_cast<std::array<u8, 4>>(word);
    bytes.insert(bytes.end(), raw.begin(), raw.end());
  }
  return bytes;
}

std::optional<SourceExpectation> parseSourceExpectation(std::string_view line) {
  std::istringstream stream{std::string(line)};
  std::string expect;
  std::string source;
  std::string contains;
  if (!(stream >> expect >> source >> contains)) {
    return std::nullopt;
  }
  if (expect != "expect-source" || contains != "contains") {
    return std::nullopt;
  }
  SourceExpectation expectation;
  expectation.kind = normalizeToken(source) == "vertex" ? ShaderSection::Kind::Vertex : ShaderSection::Kind::Pixel;
  const auto firstQuote = line.find('"');
  const auto lastQuote = line.rfind('"');
  if (firstQuote == std::string_view::npos || lastQuote == firstQuote) {
    fail("expect-source requires a quoted substring");
  }
  expectation.needle = std::string(line.substr(firstQuote + 1, lastQuote - firstQuote - 1));
  return expectation;
}

std::optional<ProbeExpectation> parseProbe(std::string_view line) {
  ProbeExpectation probe;
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 0.0f;
  unsigned tolerance = 0;
  if (std::sscanf(std::string(line).c_str(), "probe (%u, %u) rgba(%f , %f , %f , %f) %u", &probe.x, &probe.y, &r, &g,
                  &b, &a, &tolerance) != 7 &&
      std::sscanf(std::string(line).c_str(), "probe (%u, %u) rgba(%f,%f,%f,%f) %u", &probe.x, &probe.y, &r, &g, &b,
                  &a, &tolerance) != 7) {
    return std::nullopt;
  }
  probe.expected = toBGRA({r, g, b, a});
  probe.tolerance = tolerance;
  return probe;
}

std::optional<ProbeExpectation> parseRenderTargetProbe(std::string_view line) {
  ProbeExpectation probe;
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 0.0f;
  unsigned tolerance = 0;
  if (std::sscanf(std::string(line).c_str(), "probe-rt %u (%u, %u) rgba(%f , %f , %f , %f) %u",
                  &probe.target, &probe.x, &probe.y, &r, &g, &b, &a, &tolerance) != 8 &&
      std::sscanf(std::string(line).c_str(), "probe-rt %u (%u, %u) rgba(%f,%f,%f,%f) %u",
                  &probe.target, &probe.x, &probe.y, &r, &g, &b, &a, &tolerance) != 8) {
    return std::nullopt;
  }
  probe.expected = toBGRA({r, g, b, a});
  probe.tolerance = tolerance;
  return probe;
}

std::optional<ColorRGBA> parseClear(std::string_view line) {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 0.0f;
  if (std::sscanf(std::string(line).c_str(), "clear rgba(%f , %f , %f , %f)", &r, &g, &b, &a) != 4 &&
      std::sscanf(std::string(line).c_str(), "clear rgba(%f,%f,%f,%f)", &r, &g, &b, &a) != 4) {
    return std::nullopt;
  }
  return ColorRGBA{r, g, b, a};
}

std::optional<float> parseClearDepth(std::string_view line) {
  float depth = 0.0f;
  if (std::sscanf(std::string(line).c_str(), "dxmt9-clear-depth %f", &depth) != 1) {
    return std::nullopt;
  }
  return std::clamp(depth, 0.0f, 1.0f);
}

std::optional<Viewport> parseDxmt9Viewport(std::string_view line) {
  std::istringstream stream{std::string(line)};
  std::string command;
  Viewport viewport;
  if (!(stream >> command) || command != "dxmt9-viewport") {
    return std::nullopt;
  }
  if (!(stream >> viewport.x >> viewport.y >> viewport.width >> viewport.height)) {
    fail("invalid dxmt9-viewport command");
  }
  if (viewport.width == 0 || viewport.height == 0) {
    fail("dxmt9-viewport requires non-zero dimensions");
  }
  if (stream >> viewport.minZ) {
    if (!(stream >> viewport.maxZ)) {
      fail("dxmt9-viewport requires both minZ and maxZ");
    }
    std::string extra;
    if (stream >> extra) {
      fail("dxmt9-viewport has extra values");
    }
  } else {
    viewport.minZ = 0.0f;
    viewport.maxZ = 1.0f;
  }
  return viewport;
}

void parseTestLine(CorpusTest& test, std::string_view rawLine) {
  auto line = trim(rawLine);
  if (line.empty() || line.starts_with(';')) {
    if (line == "; [provenance]") {
      test.provenanceSeen = true;
    } else if (line.starts_with("; source:")) {
      test.sourceSeen = true;
    } else if (line.starts_with("; oracle:")) {
      test.oracleSeen = true;
    }
    return;
  }

  if (line == "draw quad" || line == "draw triangle") {
    test.drawQuad = true;
    return;
  }

  if (line == "dxmt9-draw-textured-quad") {
    test.drawDxmt9TexturedQuad = true;
    return;
  }

  if (line == "dxmt9-draw-textured-quad-overscan") {
    test.drawDxmt9TexturedQuadOverscan = true;
    return;
  }

  if (line == "dxmt9-draw-textured-quad-tex2") {
    test.drawDxmt9TexturedQuadTex2 = true;
    return;
  }

  if (line == "dxmt9-draw-textured-quad-xyz") {
    test.drawDxmt9TexturedQuadXyz = true;
    return;
  }

  if (line == "dxmt9-draw-solid-quad") {
    test.drawDxmt9SolidQuad = true;
    return;
  }

  if (auto solidRect = parseDxmt9SolidRect(line)) {
    test.solidRectDraws.push_back(*solidRect);
    return;
  }

  if (auto solidQuadXyzDepth = parseDxmt9SolidQuadXyzDepth(line)) {
    test.solidQuadXyzDepthDraws.push_back(*solidQuadXyzDepth);
    return;
  }

  if (line == "dxmt9-draw-vs-color-triangle") {
    test.drawDxmt9VsColorTriangle = true;
    return;
  }

  if (line == "dxmt9-draw-vs-multistream-textured-quad") {
    test.drawDxmt9VsMultistreamTexturedQuad = true;
    return;
  }

  if (line == "dxmt9-draw-vs-skinned-triangle") {
    test.drawDxmt9VsSkinnedTriangle = true;
    return;
  }

  if (line == "dxmt9-draw-ffp-vertex-blend-triangle") {
    test.drawDxmt9FfpVertexBlendTriangle = true;
    return;
  }

  if (line == "dxmt9-draw-ffp-vertex-blend-2weights-triangle") {
    test.drawDxmt9FfpVertexBlend2WeightsTriangle = true;
    return;
  }

  if (line == "dxmt9-draw-ffp-vertex-blend-3weights-triangle") {
    test.drawDxmt9FfpVertexBlend3WeightsTriangle = true;
    return;
  }

  if (line == "dxmt9-draw-ffp-vertex-blend-indexed-triangle") {
    test.drawDxmt9FfpVertexBlendIndexedTriangle = true;
    return;
  }

  if (line == "dxmt9-draw-ffp-vertex-blend-fvf-xyzb2-triangle") {
    test.drawDxmt9FfpVertexBlendFvfXyzb2Triangle = true;
    return;
  }

  if (line == "dxmt9-draw-odepth-overlap") {
    test.drawDxmt9ODepthOverlap = true;
    return;
  }

  if (auto clear = parseClear(line)) {
    test.clearColor = *clear;
    return;
  }

  if (auto depth = parseClearDepth(line)) {
    test.clearDepth = *depth;
    return;
  }

  if (auto viewport = parseDxmt9Viewport(line)) {
    test.viewport = *viewport;
    return;
  }

  if (auto scissor = parseDxmt9Scissor(line)) {
    test.scissor = *scissor;
    return;
  }

  if (auto probe = parseProbe(line)) {
    test.probes.push_back(*probe);
    return;
  }

  if (auto probe = parseRenderTargetProbe(line)) {
    test.probes.push_back(*probe);
    return;
  }

  if (auto texture = parseDxmt9Texture(line)) {
    test.textureSetups.push_back(std::move(*texture));
    return;
  }

  if (auto texture = parseDxmt9TextureRaw(line)) {
    test.textureSetups.push_back(std::move(*texture));
    return;
  }

  if (parseDxmt9TextureMip(test, line)) {
    return;
  }

  if (auto sampler = parseDxmt9Sampler(line)) {
    test.samplerSetups.push_back(std::move(*sampler));
    return;
  }

  if (auto stage = parseDxmt9TextureStage(line)) {
    test.textureStageSetups.push_back(std::move(*stage));
    return;
  }

  if (auto transform = parseDxmt9TextureTransform(line)) {
    test.textureTransformSetups.push_back(*transform);
    return;
  }

  if (auto bind = parseDxmt9BindTexture(line)) {
    test.textureBinds.push_back(*bind);
    return;
  }

  if (auto target = parseDxmt9RenderTarget(line)) {
    test.renderTargetSetups.push_back(*target);
    return;
  }

  if (auto clipPlane = parseDxmt9ClipPlane(line)) {
    test.clipPlaneSetups.push_back(*clipPlane);
    return;
  }

  if (parseDxmt9RenderState(test, line)) {
    return;
  }

  if (auto sourceExpectation = parseSourceExpectation(line)) {
    test.sourceExpectations.push_back(*sourceExpectation);
    return;
  }

  if (line.starts_with("clip-plane-mask")) {
    std::istringstream stream{std::string(line)};
    std::string label;
    if (!(stream >> label >> test.clipPlaneMask)) {
      fail("invalid clip-plane-mask command");
    }
    return;
  }

  if (line.starts_with("alpha-test")) {
    std::istringstream stream{std::string(line)};
    std::string label;
    std::string mode;
    if (!(stream >> label >> mode)) {
      fail("invalid alpha-test command");
    }
    if (normalizeToken(mode) == "disable" || normalizeToken(mode) == "disabled") {
      test.alphaTestEnable = false;
      return;
    }
    std::string funcName;
    float ref = 0.0f;
    if (!(stream >> funcName >> ref)) {
      fail("invalid alpha-test enable command");
    }
    test.alphaTestEnable = true;
    test.alphaTestFunc = parseCompareFunc(funcName);
    test.alphaRef = ref;
    return;
  }

  fail(std::string("unrecognized test command: ") + std::string(line));
}

CorpusTest parseCorpusFile(const std::string& path) {
  std::ifstream file(path);
  if (!file) {
    fail("failed to open shader test file");
  }

  CorpusTest test;
  test.path = path;

  enum class Section {
    None,
    Require,
    VertexHex,
    PixelHex,
    Test,
  };

  Section section = Section::None;
  std::string line;
  while (std::getline(file, line)) {
    const auto trimmed = trim(line);
    if (trimmed.empty()) {
      continue;
    }
    if (trimmed.starts_with(';')) {
      parseTestLine(test, trimmed);
      continue;
    }
    if (trimmed.starts_with('[') && trimmed.ends_with(']')) {
      const auto header = normalizeToken(trimmed.substr(1, trimmed.size() - 2));
      if (header == "require") {
        section = Section::Require;
      } else if (header == "vertexshaderd3dbchex") {
        section = Section::VertexHex;
        test.vertexShader = ShaderSection{ShaderSection::Kind::Vertex, {}};
      } else if (header == "pixelshaderd3dbchex") {
        section = Section::PixelHex;
        test.pixelShader = ShaderSection{ShaderSection::Kind::Pixel, {}};
      } else if (header == "test") {
        section = Section::Test;
      } else {
        section = Section::None;
      }
      continue;
    }

    switch (section) {
      case Section::None:
        break;
      case Section::Require: {
        const auto requirement = normalizeToken(trimmed);
        if (requirement.find("shadermodel") != std::string::npos) {
          // Accepted but not enforced beyond the current backend's own shader model support.
        }
        break;
      }
      case Section::VertexHex:
        {
          auto chunk = parseHexWords(trimmed);
          auto& bytecode = test.vertexShader->bytecode;
          bytecode.insert(bytecode.end(), chunk.begin(), chunk.end());
        }
        break;
      case Section::PixelHex:
        {
          auto chunk = parseHexWords(trimmed);
          auto& bytecode = test.pixelShader->bytecode;
          bytecode.insert(bytecode.end(), chunk.begin(), chunk.end());
        }
        break;
      case Section::Test:
        parseTestLine(test, trimmed);
        break;
    }
  }

  if (!test.provenanceSeen || !test.sourceSeen || !test.oracleSeen) {
    fail("missing provenance block");
  }

  return test;
}

std::string shaderSourceToString(dxmt9_u64 shaderHandle) {
  const char* source = dxmt9_winemetal_shader_source(shaderHandle);
  if (!source) {
    fail("shader source missing");
  }
  const dxmt9_u64 size = dxmt9_winemetal_shader_source_size(shaderHandle);
  if (size == 0) {
    fail("shader source size missing");
  }
  return std::string(source, static_cast<size_t>(size));
}

std::string compileShaderSource(const ShaderSection& section, const CorpusTest& test) {
  WinemetalShaderCompileRequest request{};
  request.kind = section.kind == ShaderSection::Kind::Vertex ? WinemetalShaderKind_D3DBytecodeVertex
                                                             : WinemetalShaderKind_D3DBytecodePixel;
  request.bytecode = section.bytecode.empty() ? nullptr : section.bytecode.data();
  request.bytecodeSize = static_cast<dxmt9_u64>(section.bytecode.size());
  request.bytecodeHash = hashBytes(std::as_bytes(std::span(section.bytecode)));
  request.clipPlaneMask = test.clipPlaneMask;
  request.alphaTestEnable = test.alphaTestEnable ? 1u : 0u;
  request.alphaTestFunc = static_cast<u32>(test.alphaTestFunc);
  request.alphaRef = test.alphaRef;
  const auto handle = dxmt9_winemetal_compile_shader(&request);
  if (handle == 0) {
    fail("shader compilation failed");
  }
  const auto source = shaderSourceToString(handle);
  dxmt9_winemetal_destroy_shader(handle);
  return source;
}

std::string compileDefaultFfpSource(ShaderSection::Kind kind, const CorpusTest& test) {
  DeviceState state;
  state.reset();
  if (test.alphaTestEnable) {
    state.renderStates[RS_ALPHA_TEST_ENABLE] = 1;
    state.renderStates[RS_ALPHA_FUNC] = static_cast<u32>(test.alphaTestFunc);
    state.renderStates[RS_ALPHA_REF] = static_cast<u32>(std::lround(std::clamp(test.alphaRef, 0.0f, 1.0f) * 255.0f));
  }

  WinemetalShaderCompileRequest request{};
  request.clipPlaneMask = test.clipPlaneMask;
  request.alphaTestEnable = test.alphaTestEnable ? 1u : 0u;
  request.alphaTestFunc = static_cast<u32>(test.alphaTestFunc);
  request.alphaRef = test.alphaRef;

  if (kind == ShaderSection::Kind::Vertex) {
    const auto key = makeFfpVertexKey(state);
    request.kind = WinemetalShaderKind_FfpVertex;
    request.variantKey = &key;
    const auto handle = dxmt9_winemetal_compile_shader(&request);
    if (handle == 0) {
      fail("ffp vertex shader compilation failed");
    }
    const auto source = shaderSourceToString(handle);
    dxmt9_winemetal_destroy_shader(handle);
    return source;
  }

  const auto key = makeFfpPixelKey(state);
  request.kind = WinemetalShaderKind_FfpPixel;
  request.variantKey = &key;
  const auto handle = dxmt9_winemetal_compile_shader(&request);
  if (handle == 0) {
    fail("ffp pixel shader compilation failed");
  }
  const auto source = shaderSourceToString(handle);
  dxmt9_winemetal_destroy_shader(handle);
  return source;
}

ShaderRef makeShaderRef(const ShaderSection& section) {
  ShaderRef ref;
  ref.kind = ShaderRef::Kind::Bytecode;
  ref.bytecode.bytes = section.bytecode;
  ref.bytecode.hash = hashBytes(std::as_bytes(std::span(ref.bytecode.bytes)));
  return ref;
}

std::vector<std::shared_ptr<Texture>> createDxmt9Textures(Device& device, const CorpusTest& test) {
  std::vector<std::shared_ptr<Texture>> textures;
  for (const auto& setup : test.textureSetups) {
    if (setup.levels.empty() || setup.levels[0].width == 0 || setup.levels[0].height == 0) {
      fail("dxmt9 texture requires level 0");
    }
    if (setup.id >= textures.size()) {
      textures.resize(static_cast<size_t>(setup.id) + 1u);
    }

    const auto& base = setup.levels[0];
    auto texture =
        device.createTexture({base.width, base.height, 1, static_cast<u32>(setup.levels.size()), setup.format,
                              TextureType::TwoD, Pool::Managed, UsageTexture});
    if (!texture) {
      fail("failed to create dxmt9 texture");
    }

    for (u32 levelIndex = 0; levelIndex < setup.levels.size(); ++levelIndex) {
      const auto& level = setup.levels[levelIndex];
      if (level.width == 0 || level.height == 0) {
        fail("dxmt9 texture mip level missing");
      }
      const u32 expectedWidth = std::max(1u, base.width >> levelIndex);
      const u32 expectedHeight = std::max(1u, base.height >> levelIndex);
      if (level.width != expectedWidth || level.height != expectedHeight) {
        fail("dxmt9 texture mip level dimensions do not match base texture");
      }

      auto upload = texture->lockRect(levelIndex, nullptr, UsageDiscard);
      if (!upload.data) {
        fail("failed to lock dxmt9 texture");
      }
      if (upload.pitch < formatRowPitch(setup.format, level.width)) {
        fail("dxmt9 texture upload pitch is too small");
      }
      auto* bytes = static_cast<u8*>(upload.data);
      writeDxmt9TextureLevel(bytes, upload.pitch, setup.format, level);
      texture->unlockRect(levelIndex);
    }
    textures[setup.id] = std::move(texture);
  }
  return textures;
}

void applyDxmt9SamplerSetups(Device& device, const CorpusTest& test) {
  for (const auto& sampler : test.samplerSetups) {
    for (const auto& [state, value] : sampler.states) {
      if (device.setSamplerState(sampler.stage, state, value) != D3D_OK) {
        fail("dxmt9 sampler state setup failed");
      }
    }
  }
}

void applyDxmt9TextureStageSetups(Device& device, const CorpusTest& test) {
  for (const auto& stage : test.textureStageSetups) {
    for (const auto& [state, value] : stage.states) {
      if (device.setTextureStageState(stage.stage, state, value) != D3D_OK) {
        fail("dxmt9 texture stage state setup failed");
      }
    }
  }
}

void applyDxmt9TextureTransforms(Device& device, const CorpusTest& test) {
  for (const auto& transform : test.textureTransformSetups) {
    if (transform.stage >= kMaxTextureStages) {
      fail("dxmt9 texture transform stage out of range");
    }
    if (device.setTransform(XFORM_TEXTURE_BASE + transform.stage, transform.matrix) != D3D_OK) {
      fail("dxmt9 texture transform setup failed");
    }
  }
}

void applyDxmt9TextureBinds(Device& device, const CorpusTest& test,
                            const std::vector<std::shared_ptr<Texture>>& textures) {
  for (const auto& bind : test.textureBinds) {
    if (bind.textureId >= textures.size() || !textures[bind.textureId]) {
      fail("dxmt9-bind-texture references an unknown texture");
    }
    if (device.setTexture(bind.stage, textures[bind.textureId]) != D3D_OK) {
      fail("dxmt9 texture bind failed");
    }
  }
}

void drawDxmt9TexturedQuad(Device& device, u32 width, u32 height) {
  if (device.setFVF(kFvfXyzrhw | kFvfTex1) != D3D_OK) {
    fail("dxmt9 textured quad FVF setup failed");
  }

  const float w = static_cast<float>(width);
  const float h = static_cast<float>(height);
  const std::array<ScreenSpaceTexturedVertex, 6> quad{
      ScreenSpaceTexturedVertex{0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f},
      ScreenSpaceTexturedVertex{w, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f},
      ScreenSpaceTexturedVertex{0.0f, h, 0.0f, 1.0f, 0.0f, 1.0f},
      ScreenSpaceTexturedVertex{w, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f},
      ScreenSpaceTexturedVertex{w, h, 0.0f, 1.0f, 1.0f, 1.0f},
      ScreenSpaceTexturedVertex{0.0f, h, 0.0f, 1.0f, 0.0f, 1.0f},
  };
  const auto* bytes = reinterpret_cast<const u8*>(quad.data());
  if (device.drawPrimitiveUP(PrimitiveType::TriangleList, 2, std::span<const u8>(bytes, sizeof(quad)),
                             sizeof(ScreenSpaceTexturedVertex)) != D3D_OK) {
    fail("dxmt9 textured quad draw failed");
  }
}

void drawDxmt9TexturedQuadOverscan(Device& device, u32 width, u32 height) {
  if (device.setFVF(kFvfXyzrhw | kFvfTex1) != D3D_OK) {
    fail("dxmt9 overscan textured quad FVF setup failed");
  }

  const float w = static_cast<float>(width);
  const float h = static_cast<float>(height);
  const std::array<ScreenSpaceTexturedVertex, 6> quad{
      ScreenSpaceTexturedVertex{0.0f, 0.0f, 0.0f, 1.0f, -0.5f, -0.5f},
      ScreenSpaceTexturedVertex{w, 0.0f, 0.0f, 1.0f, 1.5f, -0.5f},
      ScreenSpaceTexturedVertex{0.0f, h, 0.0f, 1.0f, -0.5f, 1.5f},
      ScreenSpaceTexturedVertex{w, 0.0f, 0.0f, 1.0f, 1.5f, -0.5f},
      ScreenSpaceTexturedVertex{w, h, 0.0f, 1.0f, 1.5f, 1.5f},
      ScreenSpaceTexturedVertex{0.0f, h, 0.0f, 1.0f, -0.5f, 1.5f},
  };
  const auto* bytes = reinterpret_cast<const u8*>(quad.data());
  if (device.drawPrimitiveUP(PrimitiveType::TriangleList, 2, std::span<const u8>(bytes, sizeof(quad)),
                             sizeof(ScreenSpaceTexturedVertex)) != D3D_OK) {
    fail("dxmt9 overscan textured quad draw failed");
  }
}

void drawDxmt9TexturedQuadTex2(Device& device, u32 width, u32 height) {
  if (device.setFVF(kFvfXyzrhw | kFvfTex2) != D3D_OK) {
    fail("dxmt9 textured quad TEX2 FVF setup failed");
  }

  const float w = static_cast<float>(width);
  const float h = static_cast<float>(height);
  const std::array<ScreenSpaceTexturedVertexTex2, 6> quad{
      ScreenSpaceTexturedVertexTex2{0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f},
      ScreenSpaceTexturedVertexTex2{w, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f},
      ScreenSpaceTexturedVertexTex2{0.0f, h, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f},
      ScreenSpaceTexturedVertexTex2{w, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f},
      ScreenSpaceTexturedVertexTex2{w, h, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f},
      ScreenSpaceTexturedVertexTex2{0.0f, h, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f},
  };
  const auto* bytes = reinterpret_cast<const u8*>(quad.data());
  if (device.drawPrimitiveUP(PrimitiveType::TriangleList, 2, std::span<const u8>(bytes, sizeof(quad)),
                             sizeof(ScreenSpaceTexturedVertexTex2)) != D3D_OK) {
    fail("dxmt9 textured quad TEX2 draw failed");
  }
}

void drawDxmt9TexturedQuadXyz(Device& device, u32 width, u32 height) {
  (void)width;
  (void)height;
  if (device.setFVF(kFvfXyz | kFvfTex1) != D3D_OK) {
    fail("dxmt9 textured quad XYZ FVF setup failed");
  }

  const std::array<ScreenSpaceTexturedVertexXyz, 6> quad{
      ScreenSpaceTexturedVertexXyz{-1.0f, 1.0f, 0.0f, 0.0f, 0.0f},
      ScreenSpaceTexturedVertexXyz{1.0f, 1.0f, 0.0f, 1.0f, 0.0f},
      ScreenSpaceTexturedVertexXyz{-1.0f, -1.0f, 0.0f, 0.0f, 1.0f},
      ScreenSpaceTexturedVertexXyz{1.0f, 1.0f, 0.0f, 1.0f, 0.0f},
      ScreenSpaceTexturedVertexXyz{1.0f, -1.0f, 0.0f, 1.0f, 1.0f},
      ScreenSpaceTexturedVertexXyz{-1.0f, -1.0f, 0.0f, 0.0f, 1.0f},
  };
  const auto* bytes = reinterpret_cast<const u8*>(quad.data());
  if (device.drawPrimitiveUP(PrimitiveType::TriangleList, 2, std::span<const u8>(bytes, sizeof(quad)),
                             sizeof(ScreenSpaceTexturedVertexXyz)) != D3D_OK) {
    fail("dxmt9 textured quad XYZ draw failed");
  }
}

void drawDxmt9SolidRect(Device& device, const SolidRectDraw& rect) {
  if (device.setFVF(kFvfXyzrhw) != D3D_OK) {
    fail("dxmt9 solid quad FVF setup failed");
  }

  const std::array<ScreenSpaceVertex, 6> quad{
      ScreenSpaceVertex{rect.left, rect.top, 0.0f, 1.0f},
      ScreenSpaceVertex{rect.right, rect.top, 0.0f, 1.0f},
      ScreenSpaceVertex{rect.left, rect.bottom, 0.0f, 1.0f},
      ScreenSpaceVertex{rect.right, rect.top, 0.0f, 1.0f},
      ScreenSpaceVertex{rect.right, rect.bottom, 0.0f, 1.0f},
      ScreenSpaceVertex{rect.left, rect.bottom, 0.0f, 1.0f},
  };
  const auto* bytes = reinterpret_cast<const u8*>(quad.data());
  if (device.drawPrimitiveUP(PrimitiveType::TriangleList, 2, std::span<const u8>(bytes, sizeof(quad)),
                             sizeof(ScreenSpaceVertex)) != D3D_OK) {
    fail("dxmt9 solid quad draw failed");
  }
}

void drawDxmt9SolidQuad(Device& device, u32 width, u32 height) {
  drawDxmt9SolidRect(
      device, SolidRectDraw{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)});
}

void drawDxmt9SolidQuadXyzDepth(Device& device, float z) {
  if (device.setFVF(kFvfXyz) != D3D_OK) {
    fail("dxmt9 solid XYZ quad FVF setup failed");
  }

  const std::array<ScreenSpaceTexturedVertexXyz, 6> quad{
      ScreenSpaceTexturedVertexXyz{-1.0f, 1.0f, z, 0.0f, 0.0f},
      ScreenSpaceTexturedVertexXyz{1.0f, 1.0f, z, 0.0f, 0.0f},
      ScreenSpaceTexturedVertexXyz{-1.0f, -1.0f, z, 0.0f, 0.0f},
      ScreenSpaceTexturedVertexXyz{1.0f, 1.0f, z, 0.0f, 0.0f},
      ScreenSpaceTexturedVertexXyz{1.0f, -1.0f, z, 0.0f, 0.0f},
      ScreenSpaceTexturedVertexXyz{-1.0f, -1.0f, z, 0.0f, 0.0f},
  };
  const auto* bytes = reinterpret_cast<const u8*>(quad.data());
  if (device.drawPrimitiveUP(PrimitiveType::TriangleList, 2,
                             std::span<const u8>(bytes, sizeof(quad)),
                             sizeof(ScreenSpaceTexturedVertexXyz)) != D3D_OK) {
    fail("dxmt9 solid XYZ quad draw failed");
  }
}

void drawDxmt9ODepthQuad(Device& device, u32 width, u32 height, u32 color) {
  if (device.setFVF(kFvfXyzrhw | kFvfDiffuse) != D3D_OK) {
    fail("dxmt9 oDepth quad FVF setup failed");
  }

  const float w = static_cast<float>(width);
  const float h = static_cast<float>(height);
  const std::array<ScreenSpaceColorVertex, 6> quad{
      ScreenSpaceColorVertex{0.0f, 0.0f, 0.0f, 1.0f, color},
      ScreenSpaceColorVertex{w, 0.0f, 0.0f, 1.0f, color},
      ScreenSpaceColorVertex{0.0f, h, 0.0f, 1.0f, color},
      ScreenSpaceColorVertex{w, 0.0f, 0.0f, 1.0f, color},
      ScreenSpaceColorVertex{w, h, 0.0f, 1.0f, color},
      ScreenSpaceColorVertex{0.0f, h, 0.0f, 1.0f, color},
  };
  const auto* bytes = reinterpret_cast<const u8*>(quad.data());
  if (device.drawPrimitiveUP(PrimitiveType::TriangleList, 2, std::span<const u8>(bytes, sizeof(quad)),
                             sizeof(ScreenSpaceColorVertex)) != D3D_OK) {
    fail("dxmt9 oDepth quad draw failed");
  }
}

void drawDxmt9ODepthOverlap(Device& device, u32 width, u32 height) {
  drawDxmt9ODepthQuad(device, width, height, 0xbfff0000u);
  drawDxmt9ODepthQuad(device, width, height, 0x4000ff00u);
}

void drawDxmt9VsColorTriangle(Device& device) {
  const std::vector<VertexElement> declaration{
      VertexElement{0, 0, kDeclTypeFloat4, 0, kDeclUsagePosition, 0},
      VertexElement{0, 16, kDeclTypeD3DColor, 0, kDeclUsageColor, 0},
  };
  if (device.setVertexDeclaration(declaration) != D3D_OK) {
    fail("dxmt9 VS color triangle vertex declaration setup failed");
  }

  constexpr u32 kGreen = 0xff00ff00u;
  const std::array<ProgrammedColorVertex, 3> triangle{
      ProgrammedColorVertex{0.0f, -1.0f, 0.0f, 1.0f, kGreen},
      ProgrammedColorVertex{0.0f, 1.0f, 0.0f, 1.0f, kGreen},
      ProgrammedColorVertex{1.0f, -1.0f, 0.0f, 1.0f, kGreen},
  };
  const auto* bytes = reinterpret_cast<const u8*>(triangle.data());
  if (device.drawPrimitiveUP(PrimitiveType::TriangleList, 1, std::span<const u8>(bytes, sizeof(triangle)),
                             sizeof(ProgrammedColorVertex)) != D3D_OK) {
    fail("dxmt9 VS color triangle draw failed");
  }
}

template <typename T, size_t N>
std::shared_ptr<Buffer> createVertexBufferWithData(
    Device& device,
    const std::array<T, N>& data) {
  auto buffer = device.createBuffer({
      static_cast<u32>(sizeof(T) * N),
      Pool::Default,
      UsageVertexBuffer,
  });
  if (!buffer) {
    fail("dxmt9 vertex buffer creation failed");
  }
  auto region = buffer->lock(0, sizeof(T) * N, 0);
  if (!region.data || region.pitch < sizeof(T) * N) {
    fail("dxmt9 vertex buffer lock failed");
  }
  std::memcpy(region.data, data.data(), sizeof(T) * N);
  buffer->unlock();
  return buffer;
}

void drawDxmt9VsMultistreamTexturedQuad(Device& device) {
  constexpr u32 kDeclUsageTexcoord = 5u;
  const std::vector<VertexElement> declaration{
      VertexElement{0, 0, kDeclTypeFloat4, 0, kDeclUsagePosition, 0},
      VertexElement{1, 12, 1u, 0, kDeclUsageTexcoord, 0},
      VertexElement{1, 20, 1u, 0, kDeclUsageTexcoord, 1},
  };
  if (device.setVertexDeclaration(declaration) != D3D_OK) {
    fail("dxmt9 VS multistream vertex declaration setup failed");
  }

  const std::array<MultiStreamPositionVertex, 6> positions{
      MultiStreamPositionVertex{-1.0f, 1.0f, 0.0f, 1.0f},
      MultiStreamPositionVertex{1.0f, 1.0f, 0.0f, 1.0f},
      MultiStreamPositionVertex{-1.0f, -1.0f, 0.0f, 1.0f},
      MultiStreamPositionVertex{1.0f, 1.0f, 0.0f, 1.0f},
      MultiStreamPositionVertex{1.0f, -1.0f, 0.0f, 1.0f},
      MultiStreamPositionVertex{-1.0f, -1.0f, 0.0f, 1.0f},
  };
  const std::array<MultiStreamTexcoordVertex, 6> texcoords{
      MultiStreamTexcoordVertex{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.25f, 0.25f},
      MultiStreamTexcoordVertex{0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.25f, 0.25f},
      MultiStreamTexcoordVertex{0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.25f, 0.25f},
      MultiStreamTexcoordVertex{0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.25f, 0.25f},
      MultiStreamTexcoordVertex{0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.25f, 0.25f},
      MultiStreamTexcoordVertex{0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.25f, 0.25f},
  };

  auto positionBuffer = createVertexBufferWithData(device, positions);
  auto texcoordBuffer = createVertexBufferWithData(device, texcoords);
  if (device.setStreamSource(0, positionBuffer, 0, sizeof(MultiStreamPositionVertex)) != D3D_OK) {
    fail("dxmt9 VS multistream stream0 setup failed");
  }
  if (device.setStreamSource(1, texcoordBuffer, 0, sizeof(MultiStreamTexcoordVertex)) != D3D_OK) {
    fail("dxmt9 VS multistream stream1 setup failed");
  }
  if (device.drawPrimitive(PrimitiveType::TriangleList, 2, 0) != D3D_OK) {
    fail("dxmt9 VS multistream textured quad draw failed");
  }
}

void drawDxmt9VsSkinnedTriangle(Device& device) {
  const std::vector<VertexElement> declaration{
      VertexElement{0, 0, kDeclTypeFloat3, 0, kDeclUsagePosition, 0},
      VertexElement{1, 0, kDeclTypeShort4N, 0, kDeclUsageBlendWeight, 0},
      VertexElement{1, 8, kDeclTypeUByte4, 0, kDeclUsageBlendIndices, 0},
  };
  if (device.setVertexDeclaration(declaration) != D3D_OK) {
    fail("dxmt9 VS skinned triangle vertex declaration setup failed");
  }

  auto& constants = device.mutableState().vsConst.float4;
  constants[0] = {1.0f, 0.0f, 0.0f, 1.0f};
  constants[1] = {0.0f, 1.0f, 0.0f, 0.0f};
  constants[2] = {0.0f, 0.0f, 1.0f, 0.0f};
  constants[3] = {0.0f, 0.0f, 0.0f, 1.0f};
  constants[8] = {0.0f, 1.0f, 0.0f, 1.0f};

  const std::array<SkinnedPositionVertex, 3> positions{
      SkinnedPositionVertex{-1.0f, -1.0f, 0.0f},
      SkinnedPositionVertex{-1.0f, 1.0f, 0.0f},
      SkinnedPositionVertex{0.0f, -1.0f, 0.0f},
  };
  const std::array<SkinnedPoseVertex, 3> poses{
      SkinnedPoseVertex{32767, 0, 0, 0, 0, 0, 0, 0},
      SkinnedPoseVertex{32767, 0, 0, 0, 0, 0, 0, 0},
      SkinnedPoseVertex{32767, 0, 0, 0, 0, 0, 0, 0},
  };

  auto positionBuffer = createVertexBufferWithData(device, positions);
  auto poseBuffer = createVertexBufferWithData(device, poses);
  if (device.setStreamSource(0, positionBuffer, 0, sizeof(SkinnedPositionVertex)) != D3D_OK) {
    fail("dxmt9 VS skinned triangle stream0 setup failed");
  }
  if (device.setStreamSource(1, poseBuffer, 0, sizeof(SkinnedPoseVertex)) != D3D_OK) {
    fail("dxmt9 VS skinned triangle stream1 setup failed");
  }
  if (device.drawPrimitive(PrimitiveType::TriangleList, 1, 0) != D3D_OK) {
    fail("dxmt9 VS skinned triangle draw failed");
  }
}

Matrix4x4 makeIdentityMatrix() {
  Matrix4x4 matrix{};
  matrix.m = {1.0f, 0.0f, 0.0f, 0.0f,
              0.0f, 1.0f, 0.0f, 0.0f,
              0.0f, 0.0f, 1.0f, 0.0f,
              0.0f, 0.0f, 0.0f, 1.0f};
  return matrix;
}

Matrix4x4 makeTranslatedMatrix(float x, float y, float z) {
  Matrix4x4 matrix = makeIdentityMatrix();
  matrix.m[12] = x;
  matrix.m[13] = y;
  matrix.m[14] = z;
  return matrix;
}

void setFfpBlendTransforms(Device& device, float world0, float world1, float world2, float world3) {
  if (device.setTransform(XFORM_WORLD_BASE, makeTranslatedMatrix(world0, 0.0f, 0.0f)) != D3D_OK ||
      device.setTransform(XFORM_WORLD_BASE + 1u, makeTranslatedMatrix(world1, 0.0f, 0.0f)) != D3D_OK ||
      device.setTransform(XFORM_WORLD_BASE + 2u, makeTranslatedMatrix(world2, 0.0f, 0.0f)) != D3D_OK ||
      device.setTransform(XFORM_WORLD_BASE + 3u, makeTranslatedMatrix(world3, 0.0f, 0.0f)) != D3D_OK ||
      device.setTransform(XFORM_VIEW, makeIdentityMatrix()) != D3D_OK ||
      device.setTransform(XFORM_PROJECTION, makeIdentityMatrix()) != D3D_OK) {
    fail("dxmt9 FFP vertex blend transform setup failed");
  }
}

void drawDxmt9FfpVertexBlendTriangle(Device& device) {
  const std::vector<VertexElement> declaration{
      VertexElement{0, 0, kDeclTypeFloat3, 0, kDeclUsagePosition, 0},
      VertexElement{0, 12, kDeclTypeFloat1, 0, kDeclUsageBlendWeight, 0},
      VertexElement{0, 16, kDeclTypeD3DColor, 0, kDeclUsageColor, 0},
  };
  if (device.setVertexDeclaration(declaration) != D3D_OK) {
    fail("dxmt9 FFP vertex blend declaration setup failed");
  }

  if (device.setRenderState(RS_VERTEX_BLEND, 1) != D3D_OK) {
    fail("dxmt9 FFP vertex blend render-state setup failed");
  }
  setFfpBlendTransforms(device, -0.75f, 0.75f, -0.75f, -0.75f);

  constexpr u32 kGreen = 0xff00ff00u;
  const std::array<FfpVertexBlendVertex, 3> triangle{
      FfpVertexBlendVertex{-0.45f, -0.45f, 0.0f, 0.0f, kGreen},
      FfpVertexBlendVertex{-0.45f, 0.45f, 0.0f, 0.0f, kGreen},
      FfpVertexBlendVertex{0.15f, -0.45f, 0.0f, 0.0f, kGreen},
  };
  const auto* bytes = reinterpret_cast<const u8*>(triangle.data());
  if (device.drawPrimitiveUP(PrimitiveType::TriangleList, 1, std::span<const u8>(bytes, sizeof(triangle)),
                             sizeof(FfpVertexBlendVertex)) != D3D_OK) {
    fail("dxmt9 FFP vertex blend triangle draw failed");
  }
}

void drawDxmt9FfpVertexBlend2WeightsTriangle(Device& device) {
  const std::vector<VertexElement> declaration{
      VertexElement{0, 0, kDeclTypeFloat3, 0, kDeclUsagePosition, 0},
      VertexElement{0, 12, kDeclTypeFloat2, 0, kDeclUsageBlendWeight, 0},
      VertexElement{0, 20, kDeclTypeD3DColor, 0, kDeclUsageColor, 0},
  };
  if (device.setVertexDeclaration(declaration) != D3D_OK) {
    fail("dxmt9 FFP vertex blend 2weights declaration setup failed");
  }
  if (device.setRenderState(RS_VERTEX_BLEND, 2) != D3D_OK) {
    fail("dxmt9 FFP vertex blend 2weights render-state setup failed");
  }
  setFfpBlendTransforms(device, -0.75f, 0.75f, -0.75f, -0.75f);

  constexpr u32 kGreen = 0xff00ff00u;
  const std::array<FfpVertexBlend2WeightsVertex, 3> triangle{
      FfpVertexBlend2WeightsVertex{-0.45f, -0.45f, 0.0f, 0.0f, 1.0f, kGreen},
      FfpVertexBlend2WeightsVertex{-0.45f, 0.45f, 0.0f, 0.0f, 1.0f, kGreen},
      FfpVertexBlend2WeightsVertex{0.15f, -0.45f, 0.0f, 0.0f, 1.0f, kGreen},
  };
  const auto* bytes = reinterpret_cast<const u8*>(triangle.data());
  if (device.drawPrimitiveUP(PrimitiveType::TriangleList, 1, std::span<const u8>(bytes, sizeof(triangle)),
                             sizeof(FfpVertexBlend2WeightsVertex)) != D3D_OK) {
    fail("dxmt9 FFP vertex blend 2weights triangle draw failed");
  }
}

void drawDxmt9FfpVertexBlend3WeightsTriangle(Device& device) {
  const std::vector<VertexElement> declaration{
      VertexElement{0, 0, kDeclTypeFloat3, 0, kDeclUsagePosition, 0},
      VertexElement{0, 12, kDeclTypeFloat3, 0, kDeclUsageBlendWeight, 0},
      VertexElement{0, 24, kDeclTypeD3DColor, 0, kDeclUsageColor, 0},
  };
  if (device.setVertexDeclaration(declaration) != D3D_OK) {
    fail("dxmt9 FFP vertex blend 3weights declaration setup failed");
  }
  if (device.setRenderState(RS_VERTEX_BLEND, 3) != D3D_OK) {
    fail("dxmt9 FFP vertex blend 3weights render-state setup failed");
  }
  setFfpBlendTransforms(device, -0.75f, -0.75f, 0.75f, -0.75f);

  constexpr u32 kGreen = 0xff00ff00u;
  const std::array<FfpVertexBlend3WeightsVertex, 3> triangle{
      FfpVertexBlend3WeightsVertex{-0.45f, -0.45f, 0.0f, 0.0f, 0.0f, 1.0f, kGreen},
      FfpVertexBlend3WeightsVertex{-0.45f, 0.45f, 0.0f, 0.0f, 0.0f, 1.0f, kGreen},
      FfpVertexBlend3WeightsVertex{0.15f, -0.45f, 0.0f, 0.0f, 0.0f, 1.0f, kGreen},
  };
  const auto* bytes = reinterpret_cast<const u8*>(triangle.data());
  if (device.drawPrimitiveUP(PrimitiveType::TriangleList, 1, std::span<const u8>(bytes, sizeof(triangle)),
                             sizeof(FfpVertexBlend3WeightsVertex)) != D3D_OK) {
    fail("dxmt9 FFP vertex blend 3weights triangle draw failed");
  }
}

void drawDxmt9FfpVertexBlendIndexedTriangle(Device& device) {
  const std::vector<VertexElement> declaration{
      VertexElement{0, 0, kDeclTypeFloat3, 0, kDeclUsagePosition, 0},
      VertexElement{0, 12, kDeclTypeFloat1, 0, kDeclUsageBlendWeight, 0},
      VertexElement{0, 16, kDeclTypeUByte4, 0, kDeclUsageBlendIndices, 0},
      VertexElement{0, 20, kDeclTypeD3DColor, 0, kDeclUsageColor, 0},
  };
  if (device.setVertexDeclaration(declaration) != D3D_OK) {
    fail("dxmt9 FFP indexed vertex blend declaration setup failed");
  }
  if (device.setRenderState(RS_VERTEX_BLEND, 1) != D3D_OK ||
      device.setRenderState(RS_INDEXED_VERTEX_BLEND_ENABLE, 1) != D3D_OK) {
    fail("dxmt9 FFP indexed vertex blend render-state setup failed");
  }
  setFfpBlendTransforms(device, -0.75f, -0.75f, 0.75f, -0.75f);

  constexpr u32 kGreen = 0xff00ff00u;
  const std::array<FfpVertexBlendIndexedVertex, 3> triangle{
      FfpVertexBlendIndexedVertex{-0.45f, -0.45f, 0.0f, 0.0f, 0, 2, 0, 0, kGreen},
      FfpVertexBlendIndexedVertex{-0.45f, 0.45f, 0.0f, 0.0f, 0, 2, 0, 0, kGreen},
      FfpVertexBlendIndexedVertex{0.15f, -0.45f, 0.0f, 0.0f, 0, 2, 0, 0, kGreen},
  };
  const auto* bytes = reinterpret_cast<const u8*>(triangle.data());
  if (device.drawPrimitiveUP(PrimitiveType::TriangleList, 1, std::span<const u8>(bytes, sizeof(triangle)),
                             sizeof(FfpVertexBlendIndexedVertex)) != D3D_OK) {
    fail("dxmt9 FFP indexed vertex blend triangle draw failed");
  }
}

void drawDxmt9FfpVertexBlendFvfXyzb2Triangle(Device& device) {
  if (device.setFVF(kFvfXyzB2 | kFvfDiffuse) != D3D_OK) {
    fail("dxmt9 FFP FVF XYZB2 setup failed");
  }
  if (device.setRenderState(RS_VERTEX_BLEND, 2) != D3D_OK) {
    fail("dxmt9 FFP FVF XYZB2 render-state setup failed");
  }
  setFfpBlendTransforms(device, -0.75f, 0.75f, -0.75f, -0.75f);

  constexpr u32 kGreen = 0xff00ff00u;
  const std::array<FfpVertexBlend2WeightsVertex, 3> triangle{
      FfpVertexBlend2WeightsVertex{-0.45f, -0.45f, 0.0f, 0.0f, 1.0f, kGreen},
      FfpVertexBlend2WeightsVertex{-0.45f, 0.45f, 0.0f, 0.0f, 1.0f, kGreen},
      FfpVertexBlend2WeightsVertex{0.15f, -0.45f, 0.0f, 0.0f, 1.0f, kGreen},
  };
  const auto* bytes = reinterpret_cast<const u8*>(triangle.data());
  if (device.drawPrimitiveUP(PrimitiveType::TriangleList, 1, std::span<const u8>(bytes, sizeof(triangle)),
                             sizeof(FfpVertexBlend2WeightsVertex)) != D3D_OK) {
    fail("dxmt9 FFP FVF XYZB2 triangle draw failed");
  }
}

void expectContains(const std::string& haystack, const std::string& needle, const std::string& path) {
  if (haystack.find(needle) == std::string::npos) {
    std::ostringstream out;
    out << path << ": missing expected substring: " << needle;
    fail(out.str());
  }
}

void runCorpusFile(const std::string& path) {
  auto test = parseCorpusFile(path);

  std::string vertexSource;
  std::string pixelSource;
  const bool needsVertexSource = std::any_of(test.sourceExpectations.begin(), test.sourceExpectations.end(),
                                             [](const auto& expectation) {
                                               return expectation.kind == ShaderSection::Kind::Vertex;
                                             });
  const bool needsPixelSource = std::any_of(test.sourceExpectations.begin(), test.sourceExpectations.end(),
                                            [](const auto& expectation) {
                                              return expectation.kind == ShaderSection::Kind::Pixel;
                                            });
  if (test.vertexShader) {
    vertexSource = compileShaderSource(*test.vertexShader, test);
  } else if (needsVertexSource) {
    vertexSource = compileDefaultFfpSource(ShaderSection::Kind::Vertex, test);
  }
  if (test.pixelShader) {
    pixelSource = compileShaderSource(*test.pixelShader, test);
  } else if (needsPixelSource) {
    pixelSource = compileDefaultFfpSource(ShaderSection::Kind::Pixel, test);
  }

  for (const auto& expectation : test.sourceExpectations) {
    const auto& source = expectation.kind == ShaderSection::Kind::Vertex ? vertexSource : pixelSource;
    expectContains(source, expectation.needle, path);
  }

  const bool needsRuntime = test.clearColor.has_value() || !test.probes.empty() || test.drawQuad ||
                            test.drawDxmt9TexturedQuad || test.drawDxmt9TexturedQuadTex2 ||
                            test.drawDxmt9TexturedQuadOverscan ||
                            test.drawDxmt9TexturedQuadXyz || test.drawDxmt9SolidQuad ||
                            test.drawDxmt9VsColorTriangle ||
                            test.drawDxmt9VsMultistreamTexturedQuad ||
                            test.drawDxmt9VsSkinnedTriangle ||
                            test.drawDxmt9FfpVertexBlendTriangle ||
                            test.drawDxmt9ODepthOverlap ||
                            !test.solidRectDraws.empty() ||
                            !test.solidQuadXyzDepthDraws.empty() ||
                            !test.textureSetups.empty() || !test.samplerSetups.empty() ||
                            !test.textureStageSetups.empty() || !test.textureTransformSetups.empty() ||
                            !test.textureBinds.empty() || !test.renderTargetSetups.empty() ||
                            !test.clipPlaneSetups.empty() ||
                            !test.renderStateSetups.empty() ||
                            test.viewport.has_value() || test.scissor.has_value() ||
                            test.colorWriteMask.has_value() || test.zEnable.has_value() ||
                            test.zWriteEnable.has_value() || test.zFunc.has_value() ||
                            test.cullMode.has_value() ||
                            test.clearDepth.has_value() || test.alphaTestEnable;
  if (!needsRuntime) {
    return;
  }
  const bool needsDepthStencil = test.clearDepth.has_value() || test.drawDxmt9ODepthOverlap ||
                                 (test.zEnable.has_value() && *test.zEnable != 0u);

  BackendLimits limits{};
  limits.maxTextureSize = 4096;
  limits.maxColorAttachments = 4;
  limits.maxAnisotropy = 16;
  limits.supportsBgr10A2 = true;
  limits.supportsDepth32FloatStencil8 = true;

  Factory factory(limits);
  PresentParameters params{};
  params.backBufferWidth = 64;
  params.backBufferHeight = 64;
  params.backBufferFormat = Format::A8R8G8B8;
  params.windowed = true;
  params.presentationInterval = PresentInterval::Immediate;
  params.deviceWindow = Handle{0};
  params.enableAutoDepthStencil = needsDepthStencil;

  auto device = factory.createDevice(0, params);
  if (!device) {
    fail("failed to create dxmt9 device");
  }

  auto backBuffer = device->swapChain()->backBuffer();
  auto depthStencil = device->swapChain()->depthStencilSurface();
  if (!backBuffer) {
    fail("missing back buffer");
  }
  if (needsDepthStencil && !depthStencil) {
    fail("missing depth-stencil surface");
  }

  device->setViewport({0, 0, 64, 64, 0.0f, 1.0f});
  std::array<std::shared_ptr<Surface>, kMaxRenderTargets> renderTargets{};
  renderTargets[0] = backBuffer;
  if (device->setRenderTarget(0, backBuffer) != D3D_OK) {
    fail("dxmt9 render target setup failed");
  }
  for (const auto& target : test.renderTargetSetups) {
    if (target.slot >= renderTargets.size()) {
      fail("dxmt9-render-target slot out of range");
    }
    if (target.slot == 0) {
      renderTargets[0] = backBuffer;
      continue;
    }
    if (renderTargets[target.slot]) {
      fail("duplicate dxmt9-render-target slot");
    }
    auto surface = device->createSurface(
        {params.backBufferWidth, params.backBufferHeight, target.format, Pool::Default, UsageRenderTarget, true, false});
    if (!surface) {
      fail("failed to create dxmt9 render target");
    }
    if (device->setRenderTarget(target.slot, surface) != D3D_OK) {
      fail("dxmt9 render target setup failed");
    }
    renderTargets[target.slot] = std::move(surface);
  }
  if (test.viewport) {
    if (device->setViewport(*test.viewport) != D3D_OK) {
      fail("dxmt9 viewport setup failed");
    }
  }
  if (test.scissor) {
    if (device->setScissorRect(*test.scissor) != D3D_OK) {
      fail("dxmt9 scissor setup failed");
    }
  }
  device->setRenderState(RS_CULL_MODE, static_cast<u32>(CullMode::None));

  if (test.vertexShader) {
    device->setVertexShader(makeShaderRef(*test.vertexShader));
  }
  if (test.pixelShader) {
    device->setPixelShader(makeShaderRef(*test.pixelShader));
  }

  const auto dxmt9Textures = createDxmt9Textures(*device, test);
  applyDxmt9SamplerSetups(*device, test);
  applyDxmt9TextureStageSetups(*device, test);
  applyDxmt9TextureTransforms(*device, test);
  applyDxmt9TextureBinds(*device, test, dxmt9Textures);
  for (const auto& clipPlane : test.clipPlaneSetups) {
    if (device->setClipPlane(clipPlane.index, clipPlane.plane) != D3D_OK) {
      fail("dxmt9 clip-plane setup failed");
    }
  }

  if (test.alphaTestEnable) {
    device->setRenderState(RS_ALPHA_TEST_ENABLE, 1);
    device->setRenderState(RS_ALPHA_FUNC, static_cast<u32>(test.alphaTestFunc));
    device->setRenderState(RS_ALPHA_REF, static_cast<u32>(std::lround(std::clamp(test.alphaRef, 0.0f, 1.0f) * 255.0f)));
  }
  if (test.colorWriteMask) {
    device->setRenderState(RS_COLOR_WRITE_ENABLE, *test.colorWriteMask);
  }
  if (test.zEnable) {
    device->setRenderState(RS_Z_ENABLE, *test.zEnable);
  }
  if (test.zWriteEnable) {
    device->setRenderState(RS_Z_WRITE_ENABLE, *test.zWriteEnable);
  }
  if (test.zFunc) {
    device->setRenderState(RS_Z_FUNC, *test.zFunc);
  }
  if (test.cullMode) {
    device->setRenderState(RS_CULL_MODE, *test.cullMode);
  }
  for (const auto& renderState : test.renderStateSetups) {
    if (device->setRenderState(renderState.state, renderState.value) != D3D_OK) {
      fail("dxmt9 render-state setup failed");
    }
  }

  const bool needsClear = test.clearColor.has_value() || test.clearDepth.has_value() ||
                          !test.probes.empty() || test.drawQuad ||
                          test.drawDxmt9TexturedQuad || test.drawDxmt9TexturedQuadTex2 ||
                          test.drawDxmt9TexturedQuadOverscan ||
                          test.drawDxmt9TexturedQuadXyz || test.drawDxmt9SolidQuad ||
                          test.drawDxmt9VsColorTriangle ||
                          test.drawDxmt9VsMultistreamTexturedQuad ||
                          test.drawDxmt9VsSkinnedTriangle ||
                          test.drawDxmt9FfpVertexBlendTriangle ||
                          test.drawDxmt9ODepthOverlap ||
                          !test.solidRectDraws.empty() ||
                          !test.solidQuadXyzDepthDraws.empty() ||
                          !test.renderTargetSetups.empty();
  if (needsClear) {
    const auto clearColor = test.clearColor.value_or(ColorRGBA{0.0f, 0.0f, 0.0f, 1.0f});
    ClearDesc clear{};
    clear.clearColor = true;
    clear.color = clearColor;
    for (size_t i = 0; i < renderTargets.size(); ++i) {
      if (!renderTargets[i]) {
        continue;
      }
      clear.colorAttachments[i] = {renderTargets[i]->handle(), renderTargets[i]->level(),
                                   renderTargets[i]->multiSampleCount()};
    }
    if (test.clearDepth) {
      if (!depthStencil) {
        fail("depth clear requires a depth-stencil surface");
      }
      clear.clearDepth = true;
      clear.depth = *test.clearDepth;
      clear.depthStencil = {depthStencil->handle(), depthStencil->level(),
                            depthStencil->multiSampleCount()};
    }
    if (device->clear(clear) != D3D_OK) {
      fail("clear failed");
    }
  }

  if (test.drawQuad) {
    if (device->beginScene() != D3D_OK) {
      fail("beginScene failed");
    }
    if (device->drawPrimitive(PrimitiveType::TriangleList, 1) != D3D_OK) {
      fail("drawPrimitive failed");
    }
    if (device->endScene() != D3D_OK) {
      fail("endScene failed");
    }
  }

  if (test.drawDxmt9TexturedQuad) {
    if (device->beginScene() != D3D_OK) {
      fail("beginScene failed");
    }
    drawDxmt9TexturedQuad(*device, params.backBufferWidth, params.backBufferHeight);
    if (device->endScene() != D3D_OK) {
      fail("endScene failed");
    }
  }

  if (test.drawDxmt9TexturedQuadOverscan) {
    if (device->beginScene() != D3D_OK) {
      fail("beginScene failed");
    }
    drawDxmt9TexturedQuadOverscan(*device, params.backBufferWidth,
                                  params.backBufferHeight);
    if (device->endScene() != D3D_OK) {
      fail("endScene failed");
    }
  }

  if (test.drawDxmt9TexturedQuadTex2) {
    if (device->beginScene() != D3D_OK) {
      fail("beginScene failed");
    }
    drawDxmt9TexturedQuadTex2(*device, params.backBufferWidth, params.backBufferHeight);
    if (device->endScene() != D3D_OK) {
      fail("endScene failed");
    }
  }

  if (test.drawDxmt9TexturedQuadXyz) {
    if (device->beginScene() != D3D_OK) {
      fail("beginScene failed");
    }
    drawDxmt9TexturedQuadXyz(*device, params.backBufferWidth, params.backBufferHeight);
    if (device->endScene() != D3D_OK) {
      fail("endScene failed");
    }
  }

  if (test.drawDxmt9SolidQuad) {
    if (device->beginScene() != D3D_OK) {
      fail("beginScene failed");
    }
    drawDxmt9SolidQuad(*device, params.backBufferWidth, params.backBufferHeight);
    if (device->endScene() != D3D_OK) {
      fail("endScene failed");
    }
  }

  for (const auto& solidRect : test.solidRectDraws) {
    if (device->beginScene() != D3D_OK) {
      fail("beginScene failed");
    }
    drawDxmt9SolidRect(*device, solidRect);
    if (device->endScene() != D3D_OK) {
      fail("endScene failed");
    }
  }

  for (const auto& solidQuadXyzDepth : test.solidQuadXyzDepthDraws) {
    if (device->beginScene() != D3D_OK) {
      fail("beginScene failed");
    }
    drawDxmt9SolidQuadXyzDepth(*device, solidQuadXyzDepth.z);
    if (device->endScene() != D3D_OK) {
      fail("endScene failed");
    }
  }

  if (test.drawDxmt9VsColorTriangle) {
    if (device->beginScene() != D3D_OK) {
      fail("beginScene failed");
    }
    drawDxmt9VsColorTriangle(*device);
    if (device->endScene() != D3D_OK) {
      fail("endScene failed");
    }
  }

  if (test.drawDxmt9VsMultistreamTexturedQuad) {
    if (device->beginScene() != D3D_OK) {
      fail("beginScene failed");
    }
    drawDxmt9VsMultistreamTexturedQuad(*device);
    if (device->endScene() != D3D_OK) {
      fail("endScene failed");
    }
  }

  if (test.drawDxmt9VsSkinnedTriangle) {
    if (device->beginScene() != D3D_OK) {
      fail("beginScene failed");
    }
    drawDxmt9VsSkinnedTriangle(*device);
    if (device->endScene() != D3D_OK) {
      fail("endScene failed");
    }
  }

  if (test.drawDxmt9FfpVertexBlendTriangle) {
    if (device->beginScene() != D3D_OK) {
      fail("beginScene failed");
    }
    drawDxmt9FfpVertexBlendTriangle(*device);
    if (device->endScene() != D3D_OK) {
      fail("endScene failed");
    }
  }

  if (test.drawDxmt9FfpVertexBlend2WeightsTriangle) {
    if (device->beginScene() != D3D_OK) {
      fail("beginScene failed");
    }
    drawDxmt9FfpVertexBlend2WeightsTriangle(*device);
    if (device->endScene() != D3D_OK) {
      fail("endScene failed");
    }
  }

  if (test.drawDxmt9FfpVertexBlend3WeightsTriangle) {
    if (device->beginScene() != D3D_OK) {
      fail("beginScene failed");
    }
    drawDxmt9FfpVertexBlend3WeightsTriangle(*device);
    if (device->endScene() != D3D_OK) {
      fail("endScene failed");
    }
  }

  if (test.drawDxmt9FfpVertexBlendIndexedTriangle) {
    if (device->beginScene() != D3D_OK) {
      fail("beginScene failed");
    }
    drawDxmt9FfpVertexBlendIndexedTriangle(*device);
    if (device->endScene() != D3D_OK) {
      fail("endScene failed");
    }
  }

  if (test.drawDxmt9FfpVertexBlendFvfXyzb2Triangle) {
    if (device->beginScene() != D3D_OK) {
      fail("beginScene failed");
    }
    drawDxmt9FfpVertexBlendFvfXyzb2Triangle(*device);
    if (device->endScene() != D3D_OK) {
      fail("endScene failed");
    }
  }

  if (test.drawDxmt9ODepthOverlap) {
    if (device->beginScene() != D3D_OK) {
      fail("beginScene failed");
    }
    drawDxmt9ODepthOverlap(*device, params.backBufferWidth, params.backBufferHeight);
    if (device->endScene() != D3D_OK) {
      fail("endScene failed");
    }
  }

  if (!test.probes.empty()) {
    std::array<std::shared_ptr<Surface>, kMaxRenderTargets> probeSurfaces{};
    std::array<std::vector<u8>, kMaxRenderTargets> probePixels{};
    std::array<u32, kMaxRenderTargets> probePitches{};
    for (const auto& probe : test.probes) {
      if (probe.x >= params.backBufferWidth || probe.y >= params.backBufferHeight) {
        fail("probe coordinate out of range");
      }
      if (probe.target >= renderTargets.size() || !renderTargets[probe.target]) {
        fail("probe references an unbound render target");
      }
      if (!probeSurfaces[probe.target]) {
        probeSurfaces[probe.target] = device->createSurface(
            {params.backBufferWidth, params.backBufferHeight, Format::A8R8G8B8, Pool::Scratch, 0, false, false});
        if (!probeSurfaces[probe.target]) {
          fail("failed to create readback surface");
        }
        if (device->getRenderTargetData(renderTargets[probe.target], probeSurfaces[probe.target]) != D3D_OK) {
          fail("getRenderTargetData failed");
        }
        auto region = probeSurfaces[probe.target]->lockRect(nullptr, 0);
        if (!region.data) {
          fail("failed to lock readback surface");
        }
        probePitches[probe.target] = region.pitch;
        const auto byteCount =
            static_cast<size_t>(region.pitch) * static_cast<size_t>(params.backBufferHeight);
        const auto* bytes = static_cast<const u8*>(region.data);
        probePixels[probe.target].assign(bytes, bytes + byteCount);
        probeSurfaces[probe.target]->unlockRect();
      }
      const auto& pixels = probePixels[probe.target];
      const size_t offset =
          static_cast<size_t>(probe.y) * probePitches[probe.target] + static_cast<size_t>(probe.x) * 4;
      const std::array<u8, 4> actual{pixels[offset + 0], pixels[offset + 1], pixels[offset + 2], pixels[offset + 3]};
      for (size_t i = 0; i < actual.size(); ++i) {
        const int diff = std::abs(static_cast<int>(actual[i]) - static_cast<int>(probe.expected[i]));
        if (static_cast<u32>(diff) > probe.tolerance) {
          std::ostringstream out;
          out << path << ": probe rt" << probe.target << " (" << probe.x << ", " << probe.y << ") channel " << i
              << " expected " << static_cast<unsigned>(probe.expected[i]) << " got "
              << static_cast<unsigned>(actual[i]) << " tolerance " << probe.tolerance
              << " expected_bgra=(" << static_cast<unsigned>(probe.expected[0]) << ","
              << static_cast<unsigned>(probe.expected[1]) << "," << static_cast<unsigned>(probe.expected[2])
              << "," << static_cast<unsigned>(probe.expected[3]) << ") actual_bgra=("
              << static_cast<unsigned>(actual[0]) << "," << static_cast<unsigned>(actual[1]) << ","
              << static_cast<unsigned>(actual[2]) << "," << static_cast<unsigned>(actual[3]) << ")";
          fail(out.str());
        }
      }
    }
  }
}

std::vector<std::string> manifestFiles(const std::string& manifestPath) {
  std::ifstream file(manifestPath);
  if (!file) {
    fail("failed to open manifest");
  }
  std::vector<std::string> files;
  std::string line;
  while (std::getline(file, line)) {
    auto trimmed = trim(line);
    if (!trimmed.starts_with("file")) {
      continue;
    }
    const auto firstQuote = trimmed.find('"');
    const auto lastQuote = trimmed.rfind('"');
    if (firstQuote == std::string::npos || lastQuote <= firstQuote) {
      continue;
    }
    files.emplace_back(trimmed.substr(firstQuote + 1, lastQuote - firstQuote - 1));
  }
  return files;
}

int runManifest(const std::string& manifestPath) {
  const auto baseDir = std::filesystem::path(manifestPath).parent_path();
  for (const auto& file : manifestFiles(manifestPath)) {
    const auto path = (baseDir / file).string();
    std::cout << "=== " << file << " ===\n";
    runCorpusFile(path);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 2) {
      std::cerr << "usage: shader_runner_dxmt9 [--manifest manifest.toml | file.shader_test ...]\n";
      return 1;
    }

    if (std::string_view(argv[1]) == "--manifest") {
      if (argc != 3) {
        std::cerr << "usage: shader_runner_dxmt9 --manifest manifest.toml\n";
        return 1;
      }
      return runManifest(argv[2]);
    }

    for (int i = 1; i < argc; ++i) {
      std::cout << "=== " << argv[i] << " ===\n";
      runCorpusFile(argv[i]);
    }
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "shader_runner_dxmt9: " << ex.what() << '\n';
    return 1;
  }
}
