#include "dxmt9/winemetal.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> readFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to open " + path.string());
  }
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

WinemetalShaderKind detectKind(const std::vector<std::uint8_t>& bytes) {
  if (bytes.size() < sizeof(std::uint32_t)) {
    throw std::runtime_error("bytecode too short");
  }
  std::uint32_t version = 0;
  std::memcpy(&version, bytes.data(), sizeof(version));
  const std::uint32_t shaderType = version >> 16;
  if (shaderType == 0xfffeu) {
    return WinemetalShaderKind_D3DBytecodeVertex;
  }
  if (shaderType == 0xffffu) {
    return WinemetalShaderKind_D3DBytecodePixel;
  }
  throw std::runtime_error("unknown D3D shader version token 0x" + std::to_string(version));
}

std::uint64_t fnv1a64(const std::vector<std::uint8_t>& bytes) {
  std::uint64_t hash = 1469598103934665603ull;
  for (std::uint8_t byte : bytes) {
    hash ^= static_cast<std::uint64_t>(byte);
    hash *= 1099511628211ull;
  }
  return hash;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: dump_winemetal_shader <bytecode.bin> <out.metal>\n";
    return 2;
  }

  try {
    const std::filesystem::path inputPath = argv[1];
    const std::filesystem::path outputPath = argv[2];
    const auto bytes = readFile(inputPath);

    WinemetalShaderCompileRequest request{};
    request.kind = detectKind(bytes);
    request.bytecode = bytes.data();
    request.bytecodeSize = static_cast<dxmt9_u64>(bytes.size());
    request.bytecodeHash = fnv1a64(bytes);
    request.sampleCount = 1;

    const dxmt9_u64 shader = dxmt9_winemetal_compile_shader(&request);
    if (shader == 0) {
      std::cerr << "compile returned null shader handle\n";
      return 1;
    }

    const char* source = dxmt9_winemetal_shader_source(shader);
    const dxmt9_u64 sourceSize = dxmt9_winemetal_shader_source_size(shader);
    if (!source || sourceSize == 0) {
      dxmt9_winemetal_destroy_shader(shader);
      std::cerr << "shader source unavailable\n";
      return 1;
    }

    std::ofstream out(outputPath, std::ios::binary);
    if (!out) {
      dxmt9_winemetal_destroy_shader(shader);
      std::cerr << "failed to open output file\n";
      return 1;
    }
    out.write(source, static_cast<std::streamsize>(sourceSize));
    dxmt9_winemetal_destroy_shader(shader);
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
}
