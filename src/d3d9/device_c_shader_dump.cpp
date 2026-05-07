#include "device_c_common.hpp"

#include "util/config/config.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace dxmt9::d3d9::devicec {

namespace {

const char* dxmt9ShaderDumpDir() {
  static const std::string path = dxmt9::util::getenvString("DXMT_DUMP_SHADER_BYTECODE_DIR");
  return path.empty() ? nullptr : path.c_str();
}

constexpr uint32_t kD3DSIO_NOP = 0u;
constexpr uint32_t kD3DSIO_MOV = 1u;
constexpr uint32_t kD3DSIO_ADD = 2u;
constexpr uint32_t kD3DSIO_SUB = 3u;
constexpr uint32_t kD3DSIO_MAD = 4u;
constexpr uint32_t kD3DSIO_MUL = 5u;
constexpr uint32_t kD3DSIO_RCP = 6u;
constexpr uint32_t kD3DSIO_RSQ = 7u;
constexpr uint32_t kD3DSIO_DP3 = 8u;
constexpr uint32_t kD3DSIO_DP4 = 9u;
constexpr uint32_t kD3DSIO_MIN = 10u;
constexpr uint32_t kD3DSIO_MAX = 11u;
constexpr uint32_t kD3DSIO_SLT = 12u;
constexpr uint32_t kD3DSIO_SGE = 13u;
constexpr uint32_t kD3DSIO_EXP = 14u;
constexpr uint32_t kD3DSIO_LOG = 15u;
constexpr uint32_t kD3DSIO_LRP = 18u;
constexpr uint32_t kD3DSIO_FRC = 19u;
constexpr uint32_t kD3DSIO_M4x4 = 20u;
constexpr uint32_t kD3DSIO_M4x3 = 21u;
constexpr uint32_t kD3DSIO_M3x4 = 22u;
constexpr uint32_t kD3DSIO_M3x3 = 23u;
constexpr uint32_t kD3DSIO_M3x2 = 24u;
constexpr uint32_t kD3DSIO_CALL = 25u;
constexpr uint32_t kD3DSIO_CALLNZ = 26u;
constexpr uint32_t kD3DSIO_LOOP = 27u;
constexpr uint32_t kD3DSIO_RET = 28u;
constexpr uint32_t kD3DSIO_ENDLOOP = 29u;
constexpr uint32_t kD3DSIO_LABEL = 30u;
constexpr uint32_t kD3DSIO_POW = 32u;
constexpr uint32_t kD3DSIO_CRS = 33u;
constexpr uint32_t kD3DSIO_SGN = 34u;
constexpr uint32_t kD3DSIO_ABS = 35u;
constexpr uint32_t kD3DSIO_NRM = 36u;
constexpr uint32_t kD3DSIO_SINCOS = 37u;
constexpr uint32_t kD3DSIO_REP = 38u;
constexpr uint32_t kD3DSIO_ENDREP = 39u;
constexpr uint32_t kD3DSIO_IF = 40u;
constexpr uint32_t kD3DSIO_IFC = 41u;
constexpr uint32_t kD3DSIO_ELSE = 42u;
constexpr uint32_t kD3DSIO_ENDIF = 43u;
constexpr uint32_t kD3DSIO_BREAK = 44u;
constexpr uint32_t kD3DSIO_MOVA = 46u;
constexpr uint32_t kD3DSIO_DEFB = 47u;
constexpr uint32_t kD3DSIO_DEFI = 48u;
constexpr uint32_t kD3DSIO_TEXCOORD = 64u;
constexpr uint32_t kD3DSIO_TEXKILL = 65u;
constexpr uint32_t kD3DSIO_TEX = 66u;
constexpr uint32_t kD3DSIO_TEXBEM = 67u;
constexpr uint32_t kD3DSIO_TEXBEML = 68u;
constexpr uint32_t kD3DSIO_TEXREG2AR = 69u;
constexpr uint32_t kD3DSIO_TEXREG2GB = 70u;
constexpr uint32_t kD3DSIO_TEXM3x2PAD = 71u;
constexpr uint32_t kD3DSIO_TEXM3x2TEX = 72u;
constexpr uint32_t kD3DSIO_TEXM3x3PAD = 73u;
constexpr uint32_t kD3DSIO_TEXM3x3TEX = 74u;
constexpr uint32_t kD3DSIO_TEXM3x3DIFF = 75u;
constexpr uint32_t kD3DSIO_TEXM3x3SPEC = 76u;
constexpr uint32_t kD3DSIO_TEXM3x3VSPEC = 77u;
constexpr uint32_t kD3DSIO_EXPP = 78u;
constexpr uint32_t kD3DSIO_LOGP = 79u;
constexpr uint32_t kD3DSIO_CND = 80u;
constexpr uint32_t kD3DSIO_DEF = 81u;
constexpr uint32_t kD3DSIO_TEXREG2RGB = 82u;
constexpr uint32_t kD3DSIO_TEXDP3TEX = 83u;
constexpr uint32_t kD3DSIO_TEXM3x2DEPTH = 84u;
constexpr uint32_t kD3DSIO_TEXDP3 = 85u;
constexpr uint32_t kD3DSIO_TEXM3x3 = 86u;
constexpr uint32_t kD3DSIO_TEXDEPTH = 87u;
constexpr uint32_t kD3DSIO_CMP = 88u;
constexpr uint32_t kD3DSIO_BEM = 89u;
constexpr uint32_t kD3DSIO_DP2ADD = 90u;
constexpr uint32_t kD3DSIO_DSX = 91u;
constexpr uint32_t kD3DSIO_DSY = 92u;
constexpr uint32_t kD3DSIO_TEXLDD = 93u;
constexpr uint32_t kD3DSIO_SETP = 94u;
constexpr uint32_t kD3DSIO_TEXLDL = 95u;
constexpr uint32_t kD3DSIO_BREAKP = 96u;
constexpr uint32_t kD3DSIO_PHASE = 0xfffdu;
constexpr uint32_t kD3DSIO_COMMENT = 0xfffeu;
constexpr uint32_t kD3DSIO_END = 0xffffu;

uint32_t shaderFixedOperandCount(uint32_t opcode, bool* known) {
  *known = true;
  switch (opcode) {
    case kD3DSIO_NOP:
    case kD3DSIO_PHASE:
    case kD3DSIO_ELSE:
    case kD3DSIO_ENDIF:
    case kD3DSIO_ENDLOOP:
    case kD3DSIO_ENDREP:
    case kD3DSIO_RET:
    case kD3DSIO_BREAK:
    case kD3DSIO_COMMENT:
    case kD3DSIO_END:
      return 0;
    case kD3DSIO_MOV:
    case kD3DSIO_DEFB:
    case kD3DSIO_RCP:
    case kD3DSIO_RSQ:
    case kD3DSIO_FRC:
    case kD3DSIO_DSX:
    case kD3DSIO_DSY:
    case kD3DSIO_SETP:
    case kD3DSIO_BREAKP:
    case kD3DSIO_MOVA:
    case kD3DSIO_LOG:
    case kD3DSIO_LOGP:
    case kD3DSIO_EXP:
    case kD3DSIO_EXPP:
    case kD3DSIO_SGN:
    case kD3DSIO_ABS:
    case kD3DSIO_NRM:
    case kD3DSIO_TEX:
    case kD3DSIO_TEXCOORD:
    case kD3DSIO_TEXKILL:
    case kD3DSIO_TEXBEM:
    case kD3DSIO_TEXBEML:
    case kD3DSIO_TEXREG2AR:
    case kD3DSIO_TEXREG2GB:
    case kD3DSIO_TEXM3x2PAD:
    case kD3DSIO_TEXM3x2TEX:
    case kD3DSIO_TEXM3x3PAD:
    case kD3DSIO_TEXM3x3TEX:
    case kD3DSIO_TEXM3x3DIFF:
    case kD3DSIO_TEXM3x3SPEC:
    case kD3DSIO_TEXM3x3VSPEC:
    case kD3DSIO_TEXREG2RGB:
    case kD3DSIO_TEXDP3TEX:
    case kD3DSIO_TEXM3x2DEPTH:
    case kD3DSIO_TEXDP3:
    case kD3DSIO_TEXM3x3:
    case kD3DSIO_TEXDEPTH:
      return 2;
    case kD3DSIO_LABEL:
    case kD3DSIO_CALL:
    case kD3DSIO_CALLNZ:
    case kD3DSIO_IF:
    case kD3DSIO_IFC:
    case kD3DSIO_LOOP:
    case kD3DSIO_REP:
      return 1;
    case kD3DSIO_ADD:
    case kD3DSIO_SUB:
    case kD3DSIO_MUL:
    case kD3DSIO_DP3:
    case kD3DSIO_DP4:
    case kD3DSIO_MIN:
    case kD3DSIO_MAX:
    case kD3DSIO_POW:
    case kD3DSIO_CRS:
    case kD3DSIO_TEXLDD:
    case kD3DSIO_TEXLDL:
    case kD3DSIO_SLT:
    case kD3DSIO_SGE:
    case kD3DSIO_M4x4:
    case kD3DSIO_M4x3:
    case kD3DSIO_M3x4:
    case kD3DSIO_M3x3:
    case kD3DSIO_M3x2:
    case kD3DSIO_BEM:
    case kD3DSIO_SINCOS:
      return 3;
    case kD3DSIO_MAD:
    case kD3DSIO_LRP:
    case kD3DSIO_CND:
    case kD3DSIO_CMP:
    case kD3DSIO_DP2ADD:
      return 4;
    case kD3DSIO_DEF:
    case kD3DSIO_DEFI:
      return 5;
    default:
      *known = false;
      return 0;
  }
}

}  // namespace

void maybeDumpShaderBytecode(const char* label, const uint32_t* bytecode, size_t wordCount, uint64_t hash) {
  const char* dir = dxmt9ShaderDumpDir();
  if (!dir || !label || !bytecode || wordCount == 0) {
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    return;
  }
  const auto base = std::filesystem::path(dir) /
                    (std::string(label) + "-" + std::to_string(hash));
  const auto binPath = base.string() + ".bin";
  const auto txtPath = base.string() + ".txt";
  if (!std::filesystem::exists(binPath, ec)) {
    std::ofstream bin(binPath, std::ios::binary);
    if (bin) {
      bin.write(reinterpret_cast<const char*>(bytecode), static_cast<std::streamsize>(wordCount * sizeof(uint32_t)));
    }
  }
  if (!std::filesystem::exists(txtPath, ec)) {
    std::ofstream txt(txtPath);
    if (txt) {
      txt << "words=" << wordCount << "\n";
      for (size_t i = 0; i < wordCount; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%08x", bytecode[i]);
        txt << i << ": 0x" << buf << "\n";
      }
    }
  }
}

bool computeShaderBytecodeWordCount(const uint32_t* bytecode, size_t* outWords) {
  constexpr size_t kMaxShaderDwords = 1u << 16;
  if (!bytecode || !outWords) {
    return false;
  }
  size_t words = 1;
  while (words < kMaxShaderDwords) {
    const uint32_t token = bytecode[words++];
    const uint32_t opcode = token & 0xffffu;
    if (opcode == kD3DSIO_END) {
      *outWords = words;
      return true;
    }
    if (opcode == kD3DSIO_COMMENT) {
      const size_t commentWords = static_cast<size_t>((token >> 16) & 0x7fffu);
      if (commentWords > kMaxShaderDwords - words) {
        return false;
      }
      words += commentWords;
      continue;
    }
    if (opcode == kD3DSIO_PHASE) {
      continue;
    }
    bool known = false;
    size_t operandCount = shaderFixedOperandCount(opcode, &known);
    if (!known) {
      operandCount = static_cast<size_t>((token >> 24) & 0x0fu);
    }
    if (operandCount > kMaxShaderDwords - words) {
      return false;
    }
    words += operandCount;
  }
  return false;
}

}  // namespace dxmt9::d3d9::devicec
