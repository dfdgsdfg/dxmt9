#include <algorithm>
#include <bit>
#include <array>
#include <cstdio>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
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

struct CorpusTest {
  std::string path;
  std::optional<ShaderSection> vertexShader;
  std::optional<ShaderSection> pixelShader;
  std::vector<SourceExpectation> sourceExpectations;
  std::vector<ProbeExpectation> probes;
  std::optional<ColorRGBA> clearColor;
  bool drawQuad = false;
  u32 clipPlaneMask = 0;
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

  if (auto clear = parseClear(line)) {
    test.clearColor = *clear;
    return;
  }

  if (auto probe = parseProbe(line)) {
    test.probes.push_back(*probe);
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

ShaderRef makeShaderRef(const ShaderSection& section) {
  ShaderRef ref;
  ref.kind = ShaderRef::Kind::Bytecode;
  ref.bytecode.bytes = section.bytecode;
  ref.bytecode.hash = hashBytes(std::as_bytes(std::span(ref.bytecode.bytes)));
  return ref;
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
  if (test.vertexShader) {
    vertexSource = compileShaderSource(*test.vertexShader, test);
  }
  if (test.pixelShader) {
    pixelSource = compileShaderSource(*test.pixelShader, test);
  }

  for (const auto& expectation : test.sourceExpectations) {
    const auto& source = expectation.kind == ShaderSection::Kind::Vertex ? vertexSource : pixelSource;
    expectContains(source, expectation.needle, path);
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
  params.deviceWindow = Handle{1};

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

  if (test.vertexShader) {
    device->setVertexShader(makeShaderRef(*test.vertexShader));
  }
  if (test.pixelShader) {
    device->setPixelShader(makeShaderRef(*test.pixelShader));
  }

  if (test.alphaTestEnable) {
    device->setRenderState(RS_ALPHA_TEST_ENABLE, 1);
    device->setRenderState(RS_ALPHA_FUNC, static_cast<u32>(test.alphaTestFunc));
    device->setRenderState(RS_ALPHA_REF, static_cast<u32>(std::lround(std::clamp(test.alphaRef, 0.0f, 1.0f) * 255.0f)));
  }

  const bool needsClear = test.clearColor.has_value() || !test.probes.empty() || test.drawQuad;
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
