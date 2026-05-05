#include "device_c_common.hpp"

#include "util/config/config.hpp"
#include "util/dynamic_symbol.hpp"
#include "util/log/log.hpp"

#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#if defined(__APPLE__)
#include <mach/kern_return.h>
#include <mach/mach_init.h>
#include <mach/mach_vm.h>
#include <mach/vm_statistics.h>
#endif

namespace dxmt9::d3d9::devicec {

namespace {

thread_local uint32_t g_wow64ClientCallDepth = 0;

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
constexpr uint32_t kD3DPRESENT_INTERVAL_ONE = 0x00000001u;
constexpr uint32_t kD3DPRESENT_INTERVAL_TWO = 0x00000002u;
constexpr uint32_t kD3DPRESENT_INTERVAL_IMMEDIATE = 0x80000000u;

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

#if defined(__APPLE__)
using NtAllocateVirtualMemoryFn =
    int32_t (*)(void* process, void** baseAddress, uintptr_t zeroBits,
                size_t* regionSize, uint32_t allocationType, uint32_t protect);
using NtFreeVirtualMemoryFn =
    int32_t (*)(void* process, void** baseAddress, size_t* regionSize, uint32_t freeType);
using GetProcessHeapFn = void* (*)();
using RtlAllocateHeapFn = void* (*)(void* heap, uint32_t flags, size_t size);
using RtlFreeHeapFn = uint8_t (*)(void* heap, uint32_t flags, void* ptr);

NtAllocateVirtualMemoryFn resolveNtAllocateVirtualMemory() {
  static const auto fn =
      dxmt9::util::resolveDefaultSymbol<NtAllocateVirtualMemoryFn>("NtAllocateVirtualMemory",
                                                                   "_NtAllocateVirtualMemory");
  return fn;
}

NtFreeVirtualMemoryFn resolveNtFreeVirtualMemory() {
  static const auto fn =
      dxmt9::util::resolveDefaultSymbol<NtFreeVirtualMemoryFn>("NtFreeVirtualMemory",
                                                               "_NtFreeVirtualMemory");
  return fn;
}

GetProcessHeapFn resolveGetProcessHeap() {
  static const auto fn =
      dxmt9::util::resolveDefaultSymbol<GetProcessHeapFn>("GetProcessHeap", "_GetProcessHeap");
  return fn;
}

RtlAllocateHeapFn resolveRtlAllocateHeap() {
  static const auto fn =
      dxmt9::util::resolveDefaultSymbol<RtlAllocateHeapFn>("RtlAllocateHeap", "_RtlAllocateHeap");
  return fn;
}

RtlFreeHeapFn resolveRtlFreeHeap() {
  static const auto fn =
      dxmt9::util::resolveDefaultSymbol<RtlFreeHeapFn>("RtlFreeHeap", "_RtlFreeHeap");
  return fn;
}
#endif

}  // namespace

void dxmt9DebugLog(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  dxmt9::util::vlogf(dxmt9::util::LogLevel::Debug, "dxmt9-debug", fmt, args);
  va_end(args);
}

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

uint32_t usageFromD3D(uint32_t usage) {
  using namespace dxmt9::core;
  uint32_t out = 0;
  if ((usage & 0x00000001u) != 0) out |= UsageRenderTarget;
  if ((usage & 0x00000002u) != 0) out |= UsageDepthStencil;
  if ((usage & 0x00000008u) != 0) out |= UsageWriteOnly;
  if ((usage & 0x00000200u) != 0) out |= UsageDynamic;
  if ((usage & 0x00000400u) != 0) out |= UsageAutoGenMipmap;
  return out;
}

uint32_t usageToD3D(uint32_t usage) {
  using namespace dxmt9::core;
  uint32_t out = 0;
  if ((usage & UsageRenderTarget) != 0) out |= 0x00000001u;
  if ((usage & UsageDepthStencil) != 0) out |= 0x00000002u;
  if ((usage & UsageWriteOnly) != 0) out |= 0x00000008u;
  if ((usage & UsageDynamic) != 0) out |= 0x00000200u;
  if ((usage & UsageAutoGenMipmap) != 0) out |= 0x00000400u;
  return out;
}

dxmt9::core::Format fmtFromD3D(uint32_t d3d) {
  using F = dxmt9::core::Format;
  switch (d3d) {
    case 21: return F::A8R8G8B8;
    case 22: return F::X8R8G8B8;
    case 32: return F::A8B8G8R8;
    case 33: return F::X8B8G8R8;
    case 23: return F::R5G6B5;
    case 25: return F::A1R5G5B5;
    case 24: return F::X1R5G5B5;
    case 26: return F::A4R4G4B4;
    case 28: return F::A8;
    case 20: return F::R8G8B8;
    case 113: return F::A16B16G16R16F;
    case 116: return F::A32B32G32R32F;
    case 112: return F::G16R16F;
    case 111: return F::R16F;
    case 115: return F::G32R32F;
    case 114: return F::R32F;
    case 36: return F::A16B16G16R16;
    case 34: return F::G16R16;
    case 35: return F::A2R10G10B10;
    case 31: return F::A2B10G10R10;
    case 50: return F::L8;
    case 81: return F::L16;
    case 51: return F::A8L8;
    case 60: return F::V8U8;
    case 63: return F::Q8W8V8U8;
    case 64: return F::V16U16;
    case 117: return F::CxV8U8;
    case 827611204: return F::DXT1;
    case 844388420: return F::DXT2;
    case 861165636: return F::DXT3;
    case 877942852: return F::DXT4;
    case 894720068: return F::DXT5;
    case 826889281: return F::ATI1;
    case 843666497: return F::ATI2;
    case 75: return F::D24S8;
    case 77: return F::D24X8;
    case 80: return F::D16;
    case 71: return F::D32;
    case 82: return F::D32F_LOCKABLE;
    case 70: return F::D16_LOCKABLE;
    case 73: return F::D15S1;
    case 79: return F::D24X4S4;
    case 83: return F::D24FS8;
    case 85: return F::S8_LOCKABLE;
    case 101: return F::INDEX16;
    case 102: return F::INDEX32;
    default: return F::Unknown;
  }
}

uint32_t fmtToD3D(dxmt9::core::Format format) {
  using F = dxmt9::core::Format;
  switch (format) {
    case F::A8R8G8B8: return 21;
    case F::X8R8G8B8: return 22;
    case F::A8B8G8R8: return 32;
    case F::X8B8G8R8: return 33;
    case F::R5G6B5: return 23;
    case F::A1R5G5B5: return 25;
    case F::X1R5G5B5: return 24;
    case F::A4R4G4B4: return 26;
    case F::A8: return 28;
    case F::R8G8B8: return 20;
    case F::A16B16G16R16F: return 113;
    case F::A32B32G32R32F: return 116;
    case F::G16R16F: return 112;
    case F::R16F: return 111;
    case F::G32R32F: return 115;
    case F::R32F: return 114;
    case F::A16B16G16R16: return 36;
    case F::G16R16: return 34;
    case F::A2R10G10B10: return 35;
    case F::A2B10G10R10: return 31;
    case F::L8: return 50;
    case F::L16: return 81;
    case F::A8L8: return 51;
    case F::V8U8: return 60;
    case F::Q8W8V8U8: return 63;
    case F::V16U16: return 64;
    case F::CxV8U8: return 117;
    case F::DXT1: return 827611204u;
    case F::DXT2: return 844388420u;
    case F::DXT3: return 861165636u;
    case F::DXT4: return 877942852u;
    case F::DXT5: return 894720068u;
    case F::ATI1:
    case F::BC4: return 826889281u;
    case F::ATI2:
    case F::BC5: return 843666497u;
    case F::D24S8: return 75;
    case F::D24X8: return 77;
    case F::D16: return 80;
    case F::D32: return 71;
    case F::D32F_LOCKABLE: return 82;
    case F::D16_LOCKABLE: return 70;
    case F::D15S1: return 73;
    case F::D24X4S4: return 79;
    case F::D24FS8: return 83;
    case F::S8_LOCKABLE: return 85;
    case F::INDEX16: return 101;
    case F::INDEX32: return 102;
    case F::Unknown:
    default: return 0;
  }
}

dxmt9::core::MultiSampleType msTypeFromD3D(uint32_t d3d) {
  using M = dxmt9::core::MultiSampleType;
  switch (d3d) {
    case 0: return M::None;
    case 2: return M::Two;
    case 4: return M::Four;
    case 8: return M::Eight;
    default: return M::None;
  }
}

bool isSupportedD3DMultisample(uint32_t d3d) {
  switch (d3d) {
    case 0:
    case 2:
    case 4:
    case 8:
      return true;
    default:
      return false;
  }
}

uint32_t msTypeToD3D(dxmt9::core::MultiSampleType ms) {
  using M = dxmt9::core::MultiSampleType;
  switch (ms) {
    case M::Two: return 2;
    case M::Four: return 4;
    case M::Eight: return 8;
    case M::None:
    default: return 0;
  }
}

dxmt9::core::PresentInterval presentIntervalFromD3D(uint32_t d3d) {
  if (d3d == kD3DPRESENT_INTERVAL_IMMEDIATE) {
    return dxmt9::core::PresentInterval::Immediate;
  }
  if (d3d >= kD3DPRESENT_INTERVAL_TWO) {
    return dxmt9::core::PresentInterval::Two;
  }
  return dxmt9::core::PresentInterval::Default;
}

uint32_t presentIntervalToD3D(dxmt9::core::PresentInterval interval) {
  switch (interval) {
    case dxmt9::core::PresentInterval::Immediate:
      return kD3DPRESENT_INTERVAL_IMMEDIATE;
    case dxmt9::core::PresentInterval::Two:
      return kD3DPRESENT_INTERVAL_TWO;
    case dxmt9::core::PresentInterval::Default:
    default:
      return kD3DPRESENT_INTERVAL_ONE;
  }
}

dxmt9::core::Pool poolFromD3D(uint32_t d3d) {
  using P = dxmt9::core::Pool;
  switch (d3d) {
    case 1: return P::Managed;
    case 2: return P::SystemMem;
    case 3: return P::Scratch;
    default: return P::Default;
  }
}

uint32_t poolToD3D(dxmt9::core::Pool pool) {
  using P = dxmt9::core::Pool;
  switch (pool) {
    case P::Managed: return 1;
    case P::SystemMem: return 2;
    case P::Scratch: return 3;
    case P::Default:
    default: return 0;
  }
}

uint32_t textureTypeToResourceType(dxmt9::core::TextureType type) {
  using T = dxmt9::core::TextureType;
  switch (type) {
    case T::Volume: return 4;
    case T::Cube: return 5;
    case T::TwoD:
    case T::Array2D:
    default: return 3;
  }
}

dxmt9::core::PrimitiveType ptFromD3D(uint32_t d3d) {
  using P = dxmt9::core::PrimitiveType;
  if (d3d >= 1 && d3d <= 6) {
    return static_cast<P>(d3d - 1);
  }
  return P::TriangleList;
}

dxmt9::core::IndexType idxTypeFromD3D(uint32_t d3d) {
  return d3d == 102 ? dxmt9::core::IndexType::UInt32 : dxmt9::core::IndexType::UInt16;
}

dxmt9::core::PresentParameters ppFromC(const D9CPresentParams& c) {
  dxmt9::core::PresentParameters p;
  p.backBufferWidth = c.backBufferWidth;
  p.backBufferHeight = c.backBufferHeight;
  p.backBufferFormat = fmtFromD3D(c.backBufferFormat);
  p.backBufferCount = c.backBufferCount;
  p.windowed = c.windowed != 0;
  p.enableAutoDepthStencil = c.enableAutoDepthStencil != 0;
  p.autoDepthStencilFormat = fmtFromD3D(c.autoDepthStencilFormat);
  p.multiSampleType = msTypeFromD3D(c.multiSampleType);
  p.deviceWindow = dxmt9::core::Handle{c.deviceWindow};
  p.presentationInterval = presentIntervalFromD3D(c.presentationInterval);
  p.presentationIntervalRaw = c.presentationInterval;
  p.swapEffect = c.swapEffect;
  p.discardSwapEffect = c.swapEffect != 2;
  return p;
}

dxmt9::core::DisplayModeEx dmExFromC(const D9CDisplayModeEx& c) {
  dxmt9::core::DisplayModeEx m;
  m.width = c.width;
  m.height = c.height;
  m.refreshRate = c.refreshRate;
  m.format = fmtFromD3D(c.format);
  m.scanLineOrdering = static_cast<dxmt9::core::DisplayScanLineOrdering>(c.scanLineOrdering);
  return m;
}

void fillCCaps(const dxmt9::core::DeviceCaps& src, D9CCaps* out) {
  std::memset(out, 0, sizeof(*out));
  out->deviceType = static_cast<uint32_t>(src.deviceType);
  out->caps = src.caps;
  out->caps2 = src.caps2;
  out->caps3 = src.caps3;
  out->presentationIntervals = src.presentationIntervals;
  out->cursorCaps = src.cursorCaps;
  out->primitiveMiscCaps = src.primitiveMiscCaps;
  out->rasterCaps = src.rasterCaps;
  out->zCmpCaps = src.zCmpCaps;
  out->srcBlendCaps = src.srcBlendCaps;
  out->destBlendCaps = src.destBlendCaps;
  out->alphaBlendCaps = src.alphaCmpCaps;
  out->shadeCaps = src.shadeCaps;
  out->textureCaps = src.textureCaps;
  out->textureFilterCaps = src.textureFilterCaps;
  out->cubetextureFilterCaps = src.cubetextureFilterCaps;
  out->volumeTextureFilterCaps = src.volumeTextureFilterCaps;
  out->maxAnisotropy = src.maxAnisotropy;
  out->maxUserClipPlanes = src.maxUserClipPlanes;
  out->maxVertexW = src.maxVertexW;
  out->guardBandLeft = src.guardBandLeft;
  out->guardBandRight = src.guardBandRight;
  out->guardBandTop = src.guardBandTop;
  out->guardBandBottom = src.guardBandBottom;
  out->extentsAdjust = src.extentsAdjust;
  out->stencilCaps = src.stencilCaps;
  out->lineCaps = src.lineCaps;
  out->textureBlendCaps = src.textureBlendCaps;
  out->vertexShaderVersion = src.vertexShaderVersion;
  out->pixelShaderVersion = src.pixelShaderVersion;
  out->maxVertexShaderConst = src.maxVertexShaderConst;
  out->pixelShader1xMaxValue = src.pixelShader1xMaxValue;
  out->ps20DynamicFlowControlDepth = src.ps20DynamicFlowControlDepth;
  out->ps20NumTemps = src.ps20NumTemps;
  out->ps20StaticFlowControlDepth = src.ps20StaticFlowControlDepth;
  out->ps20NumInstructionSlots = src.ps20NumInstructionSlots;
  out->vs20DynamicFlowControlDepth = src.vs20DynamicFlowControlDepth;
  out->vs20NumTemps = src.vs20NumTemps;
  out->vs20StaticFlowControlDepth = src.vs20StaticFlowControlDepth;
  out->maxTextureWidth = src.maxTextureWidth;
  out->maxTextureHeight = src.maxTextureHeight;
  out->maxVolumeExtent = src.maxVolumeExtent;
  out->maxTextureRepeat = src.maxTextureRepeat;
  out->maxAnisotropy = src.maxAnisotropy;
  out->maxPointSize = src.maxPointSize;
  out->maxPrimitiveCount = src.maxPrimitiveCount;
  out->maxVertexIndex = src.maxVertexIndex;
  out->maxStreams = src.maxStreams;
  out->maxStreamStride = src.maxStreamStride;
  out->numSimultaneousRTs = src.numSimultaneousRTs;
  out->maxVertexBlendMatrices = src.maxVertexBlendMatrices;
  out->maxVertexBlendMatrixIndex = src.maxVertexBlendMatrixIndex;
  out->fvfCaps = src.fvfCaps;
  out->textureAddressCaps = src.textureAddressCaps;
  out->volumeTextureAddressCaps = src.volumeTextureAddressCaps;
  out->maxTextureAspectRatio = src.maxTextureAspectRatio;
  out->vs20Caps = src.vs20Caps;
  out->ps20Caps = src.ps20Caps;
  out->maxSimultaneousTextures = src.maxSimultaneousTextures;
  out->maxActiveLights = src.maxActiveLights;
  out->vertexProcessingCaps = src.vertexProcessingCaps;
  out->vertexTextureFilterCaps = src.vertexTextureFilterCaps;
  out->maxVShaderInstructionsExecuted = src.maxVShaderInstructionsExecuted;
  out->maxPShaderInstructionsExecuted = src.maxPShaderInstructionsExecuted;
  out->maxVertexShader30InstructionSlots = src.maxVertexShader30InstructionSlots;
  out->maxPixelShader30InstructionSlots = src.maxPixelShader30InstructionSlots;
  out->maxTextureBlendStages = 8;
  out->devCaps = src.devCaps;
  out->devCaps2 = src.devCaps2;
  out->declTypes = src.declTypes;
  out->stretchRectFilterCaps = src.stretchRectFilterCaps;
  out->masterAdapterOrdinal = src.masterAdapterOrdinal;
  out->adapterOrdinalInGroup = src.adapterOrdinalInGroup;
  out->numberOfAdaptersInGroup = src.numberOfAdaptersInGroup;
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

bool pointerFits32Bit(const void* ptr) {
  return reinterpret_cast<uintptr_t>(ptr) <= 0xffffffffu;
}

Low4GBAllocation allocateLow4GB(size_t size) {
#if defined(__APPLE__)
  if (size == 0) {
    return {};
  }
  const mach_vm_size_t rounded =
      static_cast<mach_vm_size_t>((size + vm_page_size - 1) & ~(static_cast<size_t>(vm_page_size) - 1));
  const uintptr_t limit = 0x100000000ull;
  const uintptr_t step =
      std::max<uintptr_t>(0x10000u, (static_cast<uintptr_t>(rounded) + 0xffffu) & ~0xffffull);

  if (auto getProcessHeap = resolveGetProcessHeap()) {
    if (auto rtlAlloc = resolveRtlAllocateHeap()) {
      void* heap = getProcessHeap();
      if (heap) {
        if (void* ptr = rtlAlloc(heap, 0, size)) {
          if (pointerFits32Bit(ptr)) {
            dxmt9DebugLog("allocateLow4GB using process heap ptr=%p size=%zu", ptr, size);
            return {ptr, size, false, true};
          }
          if (auto rtlFree = resolveRtlFreeHeap()) {
            rtlFree(heap, 0, ptr);
          }
        }
      }
    }
  }

  if (auto ntAlloc = resolveNtAllocateVirtualMemory()) {
    const auto ntFree = resolveNtFreeVirtualMemory();
    for (uintptr_t attempt = 0x10000000u; attempt + rounded < limit; attempt += step) {
      void* base = reinterpret_cast<void*>(attempt);
      size_t region = static_cast<size_t>(rounded);
      const int32_t status =
          ntAlloc(reinterpret_cast<void*>(static_cast<intptr_t>(-1)), &base, 0, &region,
                  0x3000u, 0x04u);
      if (status != 0) {
        continue;
      }
      if (reinterpret_cast<uintptr_t>(base) <= 0xffffffffu) {
        return {base, region, true, false};
      }
      if (ntFree) {
        size_t freeSize = 0;
        ntFree(reinterpret_cast<void*>(static_cast<intptr_t>(-1)), &base, &freeSize, 0x8000u);
      }
    }
  }

  auto tryAllocate = [&](mach_vm_address_t address, int flags) -> Low4GBAllocation {
    mach_vm_address_t candidate = address;
    const kern_return_t kr = mach_vm_allocate(mach_task_self(), &candidate, rounded, flags);
    if (kr != KERN_SUCCESS) {
      return {};
    }
    if (candidate > 0xffffffffu) {
      mach_vm_deallocate(mach_task_self(), candidate, rounded);
      return {};
    }
    return {reinterpret_cast<void*>(static_cast<uintptr_t>(candidate)),
            static_cast<size_t>(rounded), false, false};
  };

  if (auto alloc = tryAllocate(0, VM_FLAGS_ANYWHERE | VM_FLAGS_4GB_CHUNK)) {
    return alloc;
  }

  static mach_vm_address_t nextHint = 0x10000000u;
  for (mach_vm_address_t attempt = nextHint; attempt + rounded < limit; attempt += step) {
    if (auto alloc = tryAllocate(attempt, VM_FLAGS_FIXED)) {
      nextHint = attempt + step;
      return alloc;
    }
  }
  dxmt9DebugLog("allocateLow4GB failed size=%zu rounded=%llu", size,
                static_cast<unsigned long long>(rounded));
  return {};
#else
  (void)size;
  return {};
#endif
}

void freeLow4GB(Low4GBAllocation alloc) {
#if defined(__APPLE__)
  if (!alloc.ptr || alloc.size == 0) {
    return;
  }
  if (alloc.viaHeap) {
    if (auto getProcessHeap = resolveGetProcessHeap()) {
      if (auto rtlFree = resolveRtlFreeHeap()) {
        if (void* heap = getProcessHeap()) {
          rtlFree(heap, 0, alloc.ptr);
        }
      }
    }
    return;
  }
  if (alloc.viaNt) {
    if (auto ntFree = resolveNtFreeVirtualMemory()) {
      void* base = alloc.ptr;
      size_t freeSize = 0;
      ntFree(reinterpret_cast<void*>(static_cast<intptr_t>(-1)), &base, &freeSize, 0x8000u);
    }
    return;
  }
  mach_vm_deallocate(mach_task_self(),
                     static_cast<mach_vm_address_t>(reinterpret_cast<uintptr_t>(alloc.ptr)),
                     static_cast<mach_vm_size_t>(alloc.size));
#else
  (void)alloc;
#endif
}

void releaseShadowLock(ShadowLock& lock) {
  freeLow4GB(lock.shadow);
  lock = ShadowLock{};
}

bool requiresWow64PointerShadow() {
  return g_wow64ClientCallDepth != 0;
}

ScopedWow64ClientCall::ScopedWow64ClientCall() {
  ++g_wow64ClientCallDepth;
}

ScopedWow64ClientCall::~ScopedWow64ClientCall() {
  if (g_wow64ClientCallDepth != 0) {
    --g_wow64ClientCallDepth;
  }
}

uint32_t transformStateFromD3D(uint32_t state) {
  switch (state) {
    case 2u: return dxmt9::core::XFORM_VIEW;
    case 3u: return dxmt9::core::XFORM_PROJECTION;
    default:
      break;
  }
  if (state >= 16u && state < 24u) {
    return dxmt9::core::XFORM_TEXTURE_BASE + (state - 16u);
  }
  if (state >= 256u && state < 260u) {
    return dxmt9::core::XFORM_WORLD_BASE + (state - 256u);
  }
  return state;
}

int32_t setShaderFloatConst(D9CDevice* d, uint32_t start, const float* data, uint32_t cnt,
                            bool pixelShader) {
  auto& state = d->dev().mutableState();
  if (pixelShader) {
    auto& consts = state.psConst;
    for (uint32_t i = 0; i < cnt && (start + i) < consts.float4.size(); ++i) {
      consts.float4[start + i][0] = data[i * 4 + 0];
      consts.float4[start + i][1] = data[i * 4 + 1];
      consts.float4[start + i][2] = data[i * 4 + 2];
      consts.float4[start + i][3] = data[i * 4 + 3];
    }
  } else {
    auto& consts = state.vsConst;
    for (uint32_t i = 0; i < cnt && (start + i) < consts.float4.size(); ++i) {
      consts.float4[start + i][0] = data[i * 4 + 0];
      consts.float4[start + i][1] = data[i * 4 + 1];
      consts.float4[start + i][2] = data[i * 4 + 2];
      consts.float4[start + i][3] = data[i * 4 + 3];
    }
  }
  return dxmt9::core::D3D_OK;
}

}  // namespace dxmt9::d3d9::devicec
