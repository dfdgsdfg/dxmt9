#include "device_c_common.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

using dxmt9::d3d9::devicec::computeShaderBytecodeWordCount;

namespace {

void checkLength(const std::vector<uint32_t>& shader) {
  // The pointer-only production entry has no buffer size. Padding makes a
  // desynchronized walk deterministic and safe even in the failing regression.
  std::vector<uint32_t> padded(1u << 16, 0u);
  std::copy(shader.begin(), shader.end(), padded.begin());
  size_t words = 0;
  if (!computeShaderBytecodeWordCount(padded.data(), &words) || words != shader.size())
    throw std::runtime_error("shader END was not found at the exact instruction boundary");
}

void legacyTextureInstructions() {
  for (uint32_t minor = 1; minor <= 4; ++minor) {
    for (uint32_t opcode : {64u, 65u, 66u}) {
      std::vector<uint32_t> shader{0xffff0100u | minor, opcode, 0xb00f0000u};
      if (minor == 4 && opcode != 65u)
        shader.push_back(0xb0e40001u);
      // MAD's last source has MOV's low opcode bits. An off-by-one TEX walk
      // consumes the real END as a MOV operand instead of terminating.
      shader.insert(shader.end(), {4u, 0x800f0000u, 0xb0e40000u,
                                  0xa0e40000u, 0xa0e40001u, 0x0000ffffu});
      checkLength(shader);
    }
  }
}

}  // namespace

int main() {
  try {
    legacyTextureInstructions();
    checkLength({0xfffe0101u, 1u, 0xc00f0000u, 0x90e40000u, 0xffffu});
    checkLength({0xffff0101u, 0x0002fffeu, 0xffffu, 0xffffu,
                 81u, 0xa00f0000u, 0xffffu, 0xffffu, 0xffffu, 0xffffu,
                 66u, 0xb00f0000u, 0xffffu});
    checkLength({0xffff0104u, 66u, 0x800f0000u, 0xb0e40000u,
                 0xfffdu, 65u, 0x800f0000u, 0xffffu});
    // Preserve encoded-length handling for unknown opcodes and ordinary SM2 MOV.
    checkLength({0xffff0200u, 0x02000001u, 0x800f0000u, 0x90e40000u, 0xffffu});
    checkLength({0xffff0200u, 0x02001234u, 0xffffu, 0xffffu, 0xffffu});
    std::vector<uint32_t> missingEnd(1u << 16, 0u);
    missingEnd[0] = 0xffff0101u;
    size_t words = 0;
    if (computeShaderBytecodeWordCount(missingEnd.data(), &words) ||
        computeShaderBytecodeWordCount(nullptr, &words) ||
        computeShaderBytecodeWordCount(missingEnd.data(), nullptr))
      throw std::runtime_error("invalid input accepted");
    std::cout << "shader bytecode word count: pass\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
