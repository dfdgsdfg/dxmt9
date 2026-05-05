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
  u32 x = 0;
  u32 y = 0;
  std::array<u8, 4> expected{};
  u32 tolerance = 0;
};

struct TextureSetup {
  u32 id = 0;
  u32 width = 0;
  u32 height = 0;
  Format format = Format::A8R8G8B8;
  std::vector<std::array<u8, 4>> texels;
};

struct SamplerSetup {
  u32 stage = 0;
  std::unordered_map<u32, u32> states;
};

struct TextureBind {
  u32 stage = 0;
  u32 textureId = 0;
};

struct SolidRectDraw {
  float left = 0.0f;
  float top = 0.0f;
  float right = 0.0f;
  float bottom = 0.0f;
};

struct CorpusTest {
  std::string path;
  std::optional<ShaderSection> vertexShader;
  std::optional<ShaderSection> pixelShader;
  std::vector<SourceExpectation> sourceExpectations;
  std::vector<ProbeExpectation> probes;
  std::vector<TextureSetup> textureSetups;
  std::vector<SamplerSetup> samplerSetups;
  std::vector<TextureBind> textureBinds;
  std::vector<SolidRectDraw> solidRectDraws;
  std::optional<ColorRGBA> clearColor;
  std::optional<Viewport> viewport;
  bool drawQuad = false;
  bool drawDxmt9TexturedQuad = false;
  bool drawDxmt9SolidQuad = false;
  bool drawDxmt9VsColorTriangle = false;
  u32 clipPlaneMask = 0;
  std::optional<u32> colorWriteMask;
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
constexpr u32 kFvfTex1 = 0x0100u;
constexpr u32 kDeclTypeFloat4 = 3u;
constexpr u32 kDeclTypeD3DColor = 4u;
constexpr u32 kDeclUsagePosition = 0u;
constexpr u32 kDeclUsageColor = 10u;
constexpr u32 kTextureAddressWrap = 1u;
constexpr u32 kTextureAddressMirror = 2u;
constexpr u32 kTextureAddressClamp = 3u;
constexpr u32 kTextureAddressBorder = 4u;
constexpr u32 kTextureFilterNone = 0u;
constexpr u32 kTextureFilterPoint = 1u;
constexpr u32 kTextureFilterLinear = 2u;
constexpr u32 kTextureFilterAnisotropic = 3u;

struct ScreenSpaceTexturedVertex {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float rhw = 1.0f;
  float u = 0.0f;
  float v = 0.0f;
};

struct ScreenSpaceVertex {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float rhw = 1.0f;
};

struct ProgrammedColorVertex {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float w = 1.0f;
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

std::array<u8, 4> parseNamedColor(std::string_view text) {
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

std::optional<TextureSetup> parseDxmt9Texture(std::string_view line) {
  std::istringstream stream{std::string(line)};
  std::string command;
  std::string format;
  TextureSetup texture;
  if (!(stream >> command) || command != "dxmt9-texture") {
    return std::nullopt;
  }
  if (!(stream >> texture.id >> texture.width >> texture.height >> format)) {
    fail("invalid dxmt9-texture command");
  }
  if (normalizeToken(format) != "a8r8g8b8") {
    fail("dxmt9-texture currently supports only A8R8G8B8");
  }
  if (texture.width == 0 || texture.height == 0) {
    fail("dxmt9-texture requires non-zero dimensions");
  }
  std::string color;
  while (stream >> color) {
    texture.texels.push_back(parseNamedColor(color));
  }
  if (texture.texels.size() != static_cast<size_t>(texture.width) * texture.height) {
    fail("dxmt9-texture texel count does not match dimensions");
  }
  return texture;
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
    } else {
      fail("unsupported dxmt9-sampler state");
    }
  }
  return sampler;
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
  fail("unsupported alpha-test comparison function");
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
  viewport.minZ = 0.0f;
  viewport.maxZ = 1.0f;
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

  if (line == "dxmt9-draw-solid-quad") {
    test.drawDxmt9SolidQuad = true;
    return;
  }

  if (auto solidRect = parseDxmt9SolidRect(line)) {
    test.solidRectDraws.push_back(*solidRect);
    return;
  }

  if (line == "dxmt9-draw-vs-color-triangle") {
    test.drawDxmt9VsColorTriangle = true;
    return;
  }

  if (auto clear = parseClear(line)) {
    test.clearColor = *clear;
    return;
  }

  if (auto viewport = parseDxmt9Viewport(line)) {
    test.viewport = *viewport;
    return;
  }

  if (auto probe = parseProbe(line)) {
    test.probes.push_back(*probe);
    return;
  }

  if (auto texture = parseDxmt9Texture(line)) {
    test.textureSetups.push_back(std::move(*texture));
    return;
  }

  if (auto sampler = parseDxmt9Sampler(line)) {
    test.samplerSetups.push_back(std::move(*sampler));
    return;
  }

  if (auto bind = parseDxmt9BindTexture(line)) {
    test.textureBinds.push_back(*bind);
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
    if (setup.id >= textures.size()) {
      textures.resize(static_cast<size_t>(setup.id) + 1u);
    }

    auto texture =
        device.createTexture({setup.width, setup.height, 1, 1, setup.format, TextureType::TwoD, Pool::Managed,
                              UsageTexture});
    if (!texture) {
      fail("failed to create dxmt9 texture");
    }

    auto upload = texture->lockRect(0, nullptr, UsageDiscard);
    if (!upload.data) {
      fail("failed to lock dxmt9 texture");
    }
    auto* bytes = static_cast<u8*>(upload.data);
    for (u32 y = 0; y < setup.height; ++y) {
      for (u32 x = 0; x < setup.width; ++x) {
        const auto& texel = setup.texels[static_cast<size_t>(y) * setup.width + x];
        std::memcpy(bytes + static_cast<size_t>(y) * upload.pitch + static_cast<size_t>(x) * texel.size(),
                    texel.data(), texel.size());
      }
    }
    texture->unlockRect(0);
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
                            test.drawDxmt9TexturedQuad || test.drawDxmt9SolidQuad ||
                            test.drawDxmt9VsColorTriangle || !test.solidRectDraws.empty() ||
                            !test.textureSetups.empty() || !test.samplerSetups.empty() ||
                            !test.textureBinds.empty() || test.viewport.has_value() ||
                            test.colorWriteMask.has_value() || test.alphaTestEnable;
  if (!needsRuntime) {
    return;
  }

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

  auto device = factory.createDevice(0, params);
  if (!device) {
    fail("failed to create dxmt9 device");
  }

  auto backBuffer = device->swapChain()->backBuffer();
  auto depthStencil = device->swapChain()->depthStencilSurface();
  (void)depthStencil;
  if (!backBuffer) {
    fail("missing back buffer");
  }

  auto probeSurface = device->createSurface({64, 64, Format::A8R8G8B8, Pool::Scratch, 0, false, false});
  if (!probeSurface) {
    fail("failed to create readback surface");
  }

  device->setViewport({0, 0, 64, 64, 0.0f, 1.0f});
  device->setRenderTarget(0, backBuffer);
  if (test.viewport) {
    if (device->setViewport(*test.viewport) != D3D_OK) {
      fail("dxmt9 viewport setup failed");
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
  applyDxmt9TextureBinds(*device, test, dxmt9Textures);

  if (test.alphaTestEnable) {
    device->setRenderState(RS_ALPHA_TEST_ENABLE, 1);
    device->setRenderState(RS_ALPHA_FUNC, static_cast<u32>(test.alphaTestFunc));
    device->setRenderState(RS_ALPHA_REF, static_cast<u32>(std::lround(std::clamp(test.alphaRef, 0.0f, 1.0f) * 255.0f)));
  }
  if (test.colorWriteMask) {
    device->setRenderState(RS_COLOR_WRITE_ENABLE, *test.colorWriteMask);
  }

  const bool needsClear = test.clearColor.has_value() || !test.probes.empty() || test.drawQuad ||
                          test.drawDxmt9TexturedQuad || test.drawDxmt9SolidQuad ||
                          test.drawDxmt9VsColorTriangle || !test.solidRectDraws.empty();
  if (needsClear) {
    const auto clearColor = test.clearColor.value_or(ColorRGBA{0.0f, 0.0f, 0.0f, 1.0f});
    ClearDesc clear{};
    clear.clearColor = true;
    clear.color = clearColor;
    clear.colorAttachments[0] = {backBuffer->handle(), backBuffer->level(), backBuffer->multiSampleCount()};
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

  if (test.drawDxmt9VsColorTriangle) {
    if (device->beginScene() != D3D_OK) {
      fail("beginScene failed");
    }
    drawDxmt9VsColorTriangle(*device);
    if (device->endScene() != D3D_OK) {
      fail("endScene failed");
    }
  }

  if (!test.probes.empty()) {
    if (device->getRenderTargetData(backBuffer, probeSurface) != D3D_OK) {
      fail("getRenderTargetData failed");
    }
    auto region = probeSurface->lockRect(nullptr, 0);
    if (!region.data) {
      fail("failed to lock readback surface");
    }
    const auto* pixels = static_cast<const u8*>(region.data);
    for (const auto& probe : test.probes) {
      const size_t offset = static_cast<size_t>(probe.y) * region.pitch + static_cast<size_t>(probe.x) * 4;
      const std::array<u8, 4> actual{pixels[offset + 0], pixels[offset + 1], pixels[offset + 2], pixels[offset + 3]};
      for (size_t i = 0; i < actual.size(); ++i) {
        const int diff = std::abs(static_cast<int>(actual[i]) - static_cast<int>(probe.expected[i]));
        if (static_cast<u32>(diff) > probe.tolerance) {
          std::ostringstream out;
          out << path << ": probe (" << probe.x << ", " << probe.y << ") channel " << i
              << " expected " << static_cast<unsigned>(probe.expected[i]) << " got "
              << static_cast<unsigned>(actual[i]) << " tolerance " << probe.tolerance;
          fail(out.str());
        }
      }
    }
    probeSurface->unlockRect();
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
