#pragma once

#include <cstddef>
#include <cstdint>

namespace dxmt9::d3d9 {

/// Result of the bounded END-token scan shared by the PE-side shader
/// bytecode validator and the validated-bytecode hash.
enum class ShaderBytecodeScanResult : std::uint8_t {
  EndFound,
  MissingEnd,
  RunawaySentinel,
};

/// Scan a D3D9 shader token stream (starting after the version token at
/// word 0) for the END token (0x0000FFFF), skipping comment blocks
/// (opcode 0xFFFE, payload length in bits 16..30) as opaque data.
///
/// Comment payloads are arbitrary bytes: d3dx9-compiled shaders built with
/// debug info carry DBUG/CTAB comments that can contain 0xFFFFFFFF or
/// 0x0000FFFF words. A raw word scan misread those as the runaway sentinel
/// (rejecting valid shaders — S.T.A.L.K.E.R. CoP's runtime-compiled vs_3_0
/// failed CreateVertexShader this way and clamped its renderer to r1,
/// 2026-08-29) or as a premature END (truncating the hashed range).
///
/// Outside comment blocks 0xFFFFFFFF is not a valid instruction token
/// (opcode 0xFFFF is END, which never carries those flag bits); it keeps
/// rejecting the NULL-bytecode runaway pattern fuzzers use.
///
/// On EndFound, *endWordCount (when non-null) receives the total word
/// count including the version and END tokens.
inline ShaderBytecodeScanResult scanShaderBytecodeForEnd(
    const std::uint32_t* code, std::size_t boundWords,
    std::size_t* endWordCount = nullptr) {
  for (std::size_t i = 1; i < boundWords;) {
    const std::uint32_t t = code[i];
    if (t == 0x0000FFFFu) {
      if (endWordCount) {
        *endWordCount = i + 1u;
      }
      return ShaderBytecodeScanResult::EndFound;
    }
    if ((t & 0xFFFFu) == 0xFFFEu) {
      i += 1u + ((t >> 16) & 0x7FFFu);
      continue;
    }
    if (t == 0xFFFFFFFFu) {
      return ShaderBytecodeScanResult::RunawaySentinel;
    }
    ++i;
  }
  return ShaderBytecodeScanResult::MissingEnd;
}

}  // namespace dxmt9::d3d9
