#include "dxmt9/device_c.h"
#include "dxmt9/com.hpp"
#include "dxmt9/core.hpp"
#include "dxmt9/runtime.hpp"
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <cstring>
#include <dlfcn.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(__APPLE__)
#include <mach/kern_return.h>
#include <mach/mach_init.h>
#include <mach/mach_vm.h>
#include <mach/vm_statistics.h>
#endif

namespace {

void dxmt9DebugLog(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  char buffer[2048];
  std::vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  dxmt9::runtime::logLine(dxmt9::runtime::LogLevel::Debug, "dxmt9-debug", buffer);
}

const char* dxmt9ShaderDumpDir() {
  static const std::string path = dxmt9::runtime::getenvString("DXMT_DUMP_SHADER_BYTECODE_DIR");
  return path.empty() ? nullptr : path.c_str();
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
  if ((usage & 0x00000001u) != 0) out |= UsageRenderTarget;   /* D3DUSAGE_RENDERTARGET */
  if ((usage & 0x00000002u) != 0) out |= UsageDepthStencil;   /* D3DUSAGE_DEPTHSTENCIL */
  if ((usage & 0x00000008u) != 0) out |= UsageWriteOnly;      /* D3DUSAGE_WRITEONLY */
  if ((usage & 0x00000200u) != 0) out |= UsageDynamic;        /* D3DUSAGE_DYNAMIC */
  if ((usage & 0x00000400u) != 0) out |= UsageAutoGenMipmap;  /* D3DUSAGE_AUTOGENMIPMAP */
  return out;
}

uint32_t usageToD3D(uint32_t usage) {
  using namespace dxmt9::core;
  uint32_t out = 0;
  if ((usage & UsageRenderTarget) != 0) out |= 0x00000001u;   /* D3DUSAGE_RENDERTARGET */
  if ((usage & UsageDepthStencil) != 0) out |= 0x00000002u;   /* D3DUSAGE_DEPTHSTENCIL */
  if ((usage & UsageWriteOnly) != 0) out |= 0x00000008u;      /* D3DUSAGE_WRITEONLY */
  if ((usage & UsageDynamic) != 0) out |= 0x00000200u;        /* D3DUSAGE_DYNAMIC */
  if ((usage & UsageAutoGenMipmap) != 0) out |= 0x00000400u;  /* D3DUSAGE_AUTOGENMIPMAP */
  return out;
}

struct Low4GBAllocation {
  void* ptr = nullptr;
  size_t size = 0;
  bool viaNt = false;
  bool viaHeap = false;

  constexpr explicit operator bool() const noexcept { return ptr != nullptr; }
};

struct ShadowLock {
  void* nativePtr = nullptr;
  uint32_t nativePitch = 0;
  uint32_t rowBytes = 0;
  uint32_t rows = 0;
  Low4GBAllocation shadow{};
};

constexpr uint32_t kShaderEndToken = 0x0000ffffu;
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
constexpr uint32_t kD3DSIO_DCL = 31u;
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
constexpr uint32_t kD3DSIO_BREAKC = 45u;
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

bool computeShaderBytecodeWordCount(const uint32_t* bytecode, size_t* outWords) {
  constexpr size_t kMaxShaderDwords = 1u << 16;
  if (!bytecode || !outWords) return false;
  size_t words = 1; /* version token */
  while (words < kMaxShaderDwords) {
    const uint32_t token = bytecode[words++];
    const uint32_t opcode = token & 0xffffu;
    if (opcode == kD3DSIO_END) {
      *outWords = words;
      return true;
    }
    if (opcode == kD3DSIO_COMMENT) {
      const size_t commentWords = static_cast<size_t>((token >> 16) & 0x7fffu);
      if (commentWords > kMaxShaderDwords - words) return false;
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
    if (operandCount > kMaxShaderDwords - words) return false;
    words += operandCount;
  }
  return false;
}

bool pointerFits32Bit(const void* ptr) {
  return reinterpret_cast<uintptr_t>(ptr) <= 0xffffffffu;
}

using NtAllocateVirtualMemoryFn =
    int32_t (*)(void* process, void** baseAddress, uintptr_t zeroBits,
                size_t* regionSize, uint32_t allocationType, uint32_t protect);
using NtFreeVirtualMemoryFn =
    int32_t (*)(void* process, void** baseAddress, size_t* regionSize, uint32_t freeType);
using GetProcessHeapFn = void* (*)();
using RtlAllocateHeapFn = void* (*)(void* heap, uint32_t flags, size_t size);
using RtlFreeHeapFn = uint8_t (*)(void* heap, uint32_t flags, void* ptr);

NtAllocateVirtualMemoryFn resolveNtAllocateVirtualMemory() {
  static const auto fn = [] {
    if (auto* sym = dlsym(RTLD_DEFAULT, "NtAllocateVirtualMemory")) {
      return reinterpret_cast<NtAllocateVirtualMemoryFn>(sym);
    }
    if (auto* sym = dlsym(RTLD_DEFAULT, "_NtAllocateVirtualMemory")) {
      return reinterpret_cast<NtAllocateVirtualMemoryFn>(sym);
    }
    return static_cast<NtAllocateVirtualMemoryFn>(nullptr);
  }();
  return fn;
}

NtFreeVirtualMemoryFn resolveNtFreeVirtualMemory() {
  static const auto fn = [] {
    if (auto* sym = dlsym(RTLD_DEFAULT, "NtFreeVirtualMemory")) {
      return reinterpret_cast<NtFreeVirtualMemoryFn>(sym);
    }
    if (auto* sym = dlsym(RTLD_DEFAULT, "_NtFreeVirtualMemory")) {
      return reinterpret_cast<NtFreeVirtualMemoryFn>(sym);
    }
    return static_cast<NtFreeVirtualMemoryFn>(nullptr);
  }();
  return fn;
}

GetProcessHeapFn resolveGetProcessHeap() {
  static const auto fn = [] {
    if (auto* sym = dlsym(RTLD_DEFAULT, "GetProcessHeap")) {
      return reinterpret_cast<GetProcessHeapFn>(sym);
    }
    if (auto* sym = dlsym(RTLD_DEFAULT, "_GetProcessHeap")) {
      return reinterpret_cast<GetProcessHeapFn>(sym);
    }
    return static_cast<GetProcessHeapFn>(nullptr);
  }();
  return fn;
}

RtlAllocateHeapFn resolveRtlAllocateHeap() {
  static const auto fn = [] {
    if (auto* sym = dlsym(RTLD_DEFAULT, "RtlAllocateHeap")) {
      return reinterpret_cast<RtlAllocateHeapFn>(sym);
    }
    if (auto* sym = dlsym(RTLD_DEFAULT, "_RtlAllocateHeap")) {
      return reinterpret_cast<RtlAllocateHeapFn>(sym);
    }
    return static_cast<RtlAllocateHeapFn>(nullptr);
  }();
  return fn;
}

RtlFreeHeapFn resolveRtlFreeHeap() {
  static const auto fn = [] {
    if (auto* sym = dlsym(RTLD_DEFAULT, "RtlFreeHeap")) {
      return reinterpret_cast<RtlFreeHeapFn>(sym);
    }
    if (auto* sym = dlsym(RTLD_DEFAULT, "_RtlFreeHeap")) {
      return reinterpret_cast<RtlFreeHeapFn>(sym);
    }
    return static_cast<RtlFreeHeapFn>(nullptr);
  }();
  return fn;
}

Low4GBAllocation allocateLow4GB(size_t size) {
#if defined(__APPLE__)
  if (size == 0) return {};
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
    for (uintptr_t attempt = 0x10000000u;
         attempt + rounded < limit;
         attempt += step) {
      void* base = reinterpret_cast<void*>(attempt);
      size_t region = static_cast<size_t>(rounded);
      const int32_t status =
          ntAlloc(reinterpret_cast<void*>(static_cast<intptr_t>(-1)), &base, 0, &region,
                  0x3000u /* MEM_COMMIT|MEM_RESERVE */, 0x04u /* PAGE_READWRITE */);
      if (status != 0) {
        continue;
      }
      if (reinterpret_cast<uintptr_t>(base) <= 0xffffffffu) {
        return {base, region, true, false};
      }
      if (ntFree) {
        size_t freeSize = 0;
        ntFree(reinterpret_cast<void*>(static_cast<intptr_t>(-1)), &base, &freeSize,
               0x8000u /* MEM_RELEASE */);
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
  for (mach_vm_address_t attempt = nextHint;
       attempt + rounded < limit;
       attempt += step) {
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
  if (!alloc.ptr || alloc.size == 0) return;
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
      ntFree(reinterpret_cast<void*>(static_cast<intptr_t>(-1)), &base, &freeSize,
             0x8000u /* MEM_RELEASE */);
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

}  // namespace

/* ── format conversion ───────────────────────────────────────────────────── */

static dxmt9::core::Format fmtFromD3D(uint32_t d3d) {
  using F = dxmt9::core::Format;
  switch (d3d) {
    case 21:   return F::A8R8G8B8;
    case 22:   return F::X8R8G8B8;
    case 32:   return F::A8B8G8R8;
    case 33:   return F::X8B8G8R8;
    case 23:   return F::R5G6B5;
    case 25:   return F::A1R5G5B5;
    case 24:   return F::X1R5G5B5;
    case 26:   return F::A4R4G4B4;
    case 28:   return F::A8;
    case 20:   return F::R8G8B8;
    case 113:  return F::A16B16G16R16F;
    case 116:  return F::A32B32G32R32F;
    case 112:  return F::G16R16F;
    case 111:  return F::R16F;
    case 115:  return F::G32R32F;
    case 114:  return F::R32F;
    case 36:   return F::A16B16G16R16;
    case 34:   return F::G16R16;
    case 35:   return F::A2R10G10B10;
    case 31:   return F::A2B10G10R10;
    case 50:   return F::L8;
    case 81:   return F::L16;
    case 51:   return F::A8L8;
    case 60:   return F::V8U8;
    case 63:   return F::Q8W8V8U8;
    case 64:   return F::V16U16;
    case 117:  return F::CxV8U8;
    case 827611204: return F::DXT1;  /* MAKEFOURCC('D','X','T','1') */
    case 844388420: return F::DXT2;  /* MAKEFOURCC('D','X','T','2') */
    case 861165636: return F::DXT3;  /* MAKEFOURCC('D','X','T','3') */
    case 877942852: return F::DXT4;  /* MAKEFOURCC('D','X','T','4') */
    case 894720068: return F::DXT5;  /* MAKEFOURCC('D','X','T','5') */
    case 826889281: return F::ATI1;  /* MAKEFOURCC('A','T','I','1') */
    case 843666497: return F::ATI2;  /* MAKEFOURCC('A','T','I','2') */
    case 75:   return F::D24S8;
    case 77:   return F::D24X8;
    case 80:   return F::D16;
    case 71:   return F::D32;
    case 82:   return F::D32F_LOCKABLE;
    case 70:   return F::D16_LOCKABLE;
    case 73:   return F::D15S1;
    case 79:   return F::D24X4S4;
    case 83:   return F::D24FS8;
    case 85:   return F::S8_LOCKABLE;
    case 101:  return F::INDEX16;
    case 102:  return F::INDEX32;
    default:   return F::Unknown;
  }
}

static uint32_t fmtToD3D(dxmt9::core::Format format) {
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
    case F::Unknown: default: return 0;
  }
}

static dxmt9::core::MultiSampleType msTypeFromD3D(uint32_t d3d) {
  using M = dxmt9::core::MultiSampleType;
  switch (d3d) {
    case 0:  return M::None;
    case 2:  return M::Two;
    case 4:  return M::Four;
    case 8:  return M::Eight;
    default: return M::None;
  }
}

static bool isSupportedD3DMultisample(uint32_t d3d) {
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

static uint32_t msTypeToD3D(dxmt9::core::MultiSampleType ms) {
  using M = dxmt9::core::MultiSampleType;
  switch (ms) {
    case M::Two: return 2;
    case M::Four: return 4;
    case M::Eight: return 8;
    case M::None: default: return 0;
  }
}

static dxmt9::core::Pool poolFromD3D(uint32_t d3d) {
  using P = dxmt9::core::Pool;
  switch (d3d) {
    case 1:  return P::Managed;
    case 2:  return P::SystemMem;
    case 3:  return P::Scratch;
    default: return P::Default;
  }
}

static uint32_t poolToD3D(dxmt9::core::Pool pool) {
  using P = dxmt9::core::Pool;
  switch (pool) {
    case P::Managed: return 1;
    case P::SystemMem: return 2;
    case P::Scratch: return 3;
    case P::Default: default: return 0;
  }
}

static uint32_t textureTypeToResourceType(dxmt9::core::TextureType type) {
  using T = dxmt9::core::TextureType;
  switch (type) {
    case T::Volume: return 4;      /* D3DRTYPE_VOLUMETEXTURE */
    case T::Cube: return 5;        /* D3DRTYPE_CUBETEXTURE */
    case T::TwoD:
    case T::Array2D:
    default: return 3;             /* D3DRTYPE_TEXTURE */
  }
}

static dxmt9::core::PrimitiveType ptFromD3D(uint32_t d3d) {
  using P = dxmt9::core::PrimitiveType;
  /* D3D is 1-indexed; dxmt9 is 0-indexed */
  if (d3d >= 1 && d3d <= 6)
    return static_cast<P>(d3d - 1);
  return P::TriangleList;
}

static dxmt9::core::IndexType idxTypeFromD3D(uint32_t d3d) {
  return (d3d == 102 /* D3DFMT_INDEX32 */)
    ? dxmt9::core::IndexType::UInt32
    : dxmt9::core::IndexType::UInt16;
}

static dxmt9::core::PresentParameters ppFromC(const D9CPresentParams& c) {
  dxmt9::core::PresentParameters p;
  p.backBufferWidth  = c.backBufferWidth;
  p.backBufferHeight = c.backBufferHeight;
  p.backBufferFormat = fmtFromD3D(c.backBufferFormat);
  p.backBufferCount  = c.backBufferCount;
  p.windowed         = (c.windowed != 0);
  p.enableAutoDepthStencil   = (c.enableAutoDepthStencil != 0);
  p.autoDepthStencilFormat   = fmtFromD3D(c.autoDepthStencilFormat);
  p.multiSampleType  = msTypeFromD3D(c.multiSampleType);
  p.deviceWindow     = dxmt9::core::Handle{c.deviceWindow};
  /* presentationInterval: 0=Immediate,1=Default,2+ */
  if (c.presentationInterval == 0)
    p.presentationInterval = dxmt9::core::PresentInterval::Immediate;
  else if (c.presentationInterval >= 2)
    p.presentationInterval = dxmt9::core::PresentInterval::Two;
  else
    p.presentationInterval = dxmt9::core::PresentInterval::Default;
  /* swapEffect: 2=FLIP → discard=false */
  p.discardSwapEffect = (c.swapEffect != 2);
  return p;
}

static dxmt9::core::DisplayModeEx dmExFromC(const D9CDisplayModeEx& c) {
  dxmt9::core::DisplayModeEx m;
  m.width       = c.width;
  m.height      = c.height;
  m.refreshRate = c.refreshRate;
  m.format      = fmtFromD3D(c.format);
  m.scanLineOrdering = static_cast<dxmt9::core::DisplayScanLineOrdering>(c.scanLineOrdering);
  return m;
}

/* ── D9CCaps from DeviceCaps ─────────────────────────────────────────────── */

static void fillCCaps(const dxmt9::core::DeviceCaps& src, D9CCaps* out) {
  std::memset(out, 0, sizeof(*out));
  out->deviceType              = static_cast<uint32_t>(src.deviceType);
  out->caps                    = src.caps;
  out->caps2                   = src.caps2;
  out->caps3                   = src.caps3;
  out->presentationIntervals   = src.presentationIntervals;
  out->cursorCaps              = src.cursorCaps;
  out->primitiveMiscCaps       = src.primitiveMiscCaps;
  out->rasterCaps              = src.rasterCaps;
  out->zCmpCaps                = src.zCmpCaps;
  out->srcBlendCaps            = src.srcBlendCaps;
  out->destBlendCaps           = src.destBlendCaps;
  out->alphaBlendCaps          = src.alphaCmpCaps;
  out->shadeCaps               = src.shadeCaps;
  out->textureCaps             = src.textureCaps;
  out->textureFilterCaps       = src.textureFilterCaps;
  out->cubetextureFilterCaps   = src.cubetextureFilterCaps;
  out->volumeTextureFilterCaps = src.volumeTextureFilterCaps;
  out->maxAnisotropy           = src.maxAnisotropy;
  out->maxUserClipPlanes       = src.maxUserClipPlanes;
  out->maxVertexW              = src.maxVertexW;
  out->guardBandLeft           = src.guardBandLeft;
  out->guardBandRight          = src.guardBandRight;
  out->guardBandTop            = src.guardBandTop;
  out->guardBandBottom         = src.guardBandBottom;
  out->extentsAdjust           = src.extentsAdjust;
  out->stencilCaps             = src.stencilCaps;
  out->lineCaps                = src.lineCaps;
  out->textureBlendCaps        = src.textureBlendCaps;
  out->vertexShaderVersion     = src.vertexShaderVersion;
  out->pixelShaderVersion      = src.pixelShaderVersion;
  out->maxVertexShaderConst    = src.maxVertexShaderConst;
  out->pixelShader1xMaxValue   = src.pixelShader1xMaxValue;
  out->ps20DynamicFlowControlDepth = src.ps20DynamicFlowControlDepth;
  out->ps20NumTemps            = src.ps20NumTemps;
  out->ps20StaticFlowControlDepth  = src.ps20StaticFlowControlDepth;
  out->ps20NumInstructionSlots = src.ps20NumInstructionSlots;
  out->vs20DynamicFlowControlDepth = src.vs20DynamicFlowControlDepth;
  out->vs20NumTemps            = src.vs20NumTemps;
  out->vs20StaticFlowControlDepth  = src.vs20StaticFlowControlDepth;
  out->maxTextureWidth         = src.maxTextureWidth;
  out->maxTextureHeight        = src.maxTextureHeight;
  out->maxVolumeExtent         = src.maxVolumeExtent;
  out->maxTextureRepeat        = src.maxTextureRepeat;
  out->maxAnisotropy           = src.maxAnisotropy;
  out->maxPointSize            = src.maxPointSize;
  out->maxPrimitiveCount       = src.maxPrimitiveCount;
  out->maxVertexIndex          = src.maxVertexIndex;
  out->maxStreams               = src.maxStreams;
  out->maxStreamStride         = src.maxStreamStride;
  out->numSimultaneousRTs      = src.numSimultaneousRTs;
  out->maxVertexBlendMatrices  = src.maxVertexBlendMatrices;
  out->maxVertexBlendMatrixIndex = src.maxVertexBlendMatrixIndex;
  out->fvfCaps                 = src.fvfCaps;
  out->textureAddressCaps      = src.textureAddressCaps;
  out->volumeTextureAddressCaps= src.volumeTextureAddressCaps;
  out->maxTextureAspectRatio   = src.maxTextureAspectRatio;
  out->vs20Caps                = src.vs20Caps;
  out->ps20Caps                = src.ps20Caps;
  out->maxSimultaneousTextures = src.maxSimultaneousTextures;
  out->maxActiveLights         = src.maxActiveLights;
  out->vertexProcessingCaps    = src.vertexProcessingCaps;
  out->vertexTextureFilterCaps = src.vertexTextureFilterCaps;
  out->maxVShaderInstructionsExecuted = src.maxVShaderInstructionsExecuted;
  out->maxPShaderInstructionsExecuted = src.maxPShaderInstructionsExecuted;
  out->maxVertexShader30InstructionSlots = src.maxVertexShader30InstructionSlots;
  out->maxPixelShader30InstructionSlots = src.maxPixelShader30InstructionSlots;
  out->maxTextureBlendStages   = 8;   /* fixed D3D9 max */
  out->devCaps                 = src.devCaps;
  out->devCaps2                = src.devCaps2;
  out->declTypes               = src.declTypes;
  out->stretchRectFilterCaps   = src.stretchRectFilterCaps;
  out->masterAdapterOrdinal    = src.masterAdapterOrdinal;
  out->adapterOrdinalInGroup   = src.adapterOrdinalInGroup;
  out->numberOfAdaptersInGroup = src.numberOfAdaptersInGroup;
}

/* ── ref-counted C wrappers ──────────────────────────────────────────────── */

template<typename T>
struct RefWrap {
  T                    obj;
  std::atomic<uint32_t> refs{1};

  template<typename... Args>
  explicit RefWrap(Args&&... args) : obj(std::forward<Args>(args)...) {}

  void addRef()           { refs.fetch_add(1); }
  uint32_t release()      { return refs.fetch_sub(1) - 1; }
};

/* factory wraps dxmt9::com::IDirect3D9Ex */
struct D9CFactory {
  dxmt9::com::IDirect3D9Ex*  iface;
  std::atomic<uint32_t>      refs{1};

  explicit D9CFactory(dxmt9::com::IDirect3D9Ex* i) : iface(i) {}
  ~D9CFactory() { if (iface) iface->Release(); }
};

/* device wraps the underlying core::Device via dxmt9::com::IDirect3DDevice9Ex */
struct D9CDevice {
  dxmt9::com::IDirect3DDevice9Ex* iface;
  std::atomic<uint32_t>           refs{1};
  bool                            stateBlockRecording = false;
  std::optional<dxmt9::core::DeviceState> stateBlockBaseState;
  std::unordered_set<uint32_t>    stateBlockRenderStates;

  explicit D9CDevice(dxmt9::com::IDirect3DDevice9Ex* i) : iface(i) {}
  ~D9CDevice() { if (iface) iface->Release(); }

  dxmt9::core::Device& dev() { return iface->coreDevice(); }
};

struct D9CSwapChain {
  dxmt9::com::IDirect3DSwapChain9* iface;
  std::atomic<uint32_t>            refs{1};

  explicit D9CSwapChain(dxmt9::com::IDirect3DSwapChain9* i) : iface(i) {}
  ~D9CSwapChain() { if (iface) iface->Release(); }
};

struct D9CTexture {
  std::shared_ptr<dxmt9::core::Texture> obj;
  D9CDevice*                            device; /* back-pointer, non-owning */
  std::atomic<uint32_t>                 refs{1};
  std::unordered_map<uint32_t, ShadowLock> wow64Locks;
};

struct D9CBuffer {
  std::shared_ptr<dxmt9::core::Buffer> obj;
  std::atomic<uint32_t>                refs{1};
  ShadowLock wow64Lock;
  D9CBufferDesc                        desc{};
};

struct D9CSurface {
  std::shared_ptr<dxmt9::core::Surface> obj;
  D9CTexture*                           ownerTex{nullptr}; /* if texture level */
  std::atomic<uint32_t>                 refs{1};
  ShadowLock                            wow64Lock;
};

struct D9CShader {
  dxmt9::core::ShaderRef       ref;
  std::vector<uint32_t>        bytecodeWords;
  std::atomic<uint32_t>        refs{1};
};

struct D9CVertexDecl {
  std::vector<dxmt9::core::VertexElement> elements;
  std::vector<D9CVertexElement>           raw;
  std::atomic<uint32_t>                   refs{1};
};

struct D9CQuery {
  std::shared_ptr<dxmt9::core::Query> obj;
  D9CDevice*                          device; /* non-owning */
  std::atomic<uint32_t>               refs{1};
};

struct D9CStateBlock {
  std::shared_ptr<dxmt9::core::StateBlock> obj;
  D9CDevice*                               device; /* non-owning */
  std::atomic<uint32_t>                    refs{1};
};

/* ── factory ─────────────────────────────────────────────────────────────── */

extern "C" D9CFactory* dxmt9c_factory_create(void) {
  using namespace dxmt9;
  dxmt9DebugLog("factory_create begin");
  auto* ex = com::Direct3DCreate9Ex(com::D3D_SDK_VERSION,
                                     core::makeBackendDevice());
  if (!ex) {
    dxmt9DebugLog("factory_create failed");
    return nullptr;
  }
  dxmt9DebugLog("factory_create ok iface=%p", static_cast<void*>(ex));
  return new D9CFactory(ex);
}

extern "C" void dxmt9c_factory_addref(D9CFactory* f) {
  if (f) f->refs.fetch_add(1);
}

extern "C" uint32_t dxmt9c_factory_release(D9CFactory* f) {
  if (!f) return 0;
  uint32_t r = f->refs.fetch_sub(1) - 1;
  if (r == 0) delete f;
  return r;
}

extern "C" uint32_t dxmt9c_factory_adapter_count(D9CFactory* f) {
  return static_cast<uint32_t>(f->iface->GetAdapterCount());
}

extern "C" int32_t dxmt9c_factory_get_adapter_identifier(D9CFactory* f,
                                                           uint32_t adapter,
                                                           D9CAdapterIdentifier* out) {
  if (!out) return dxmt9::core::D3DERR_INVALIDCALL;
  auto id = f->iface->GetAdapterIdentifier(adapter);
  std::memset(out, 0, sizeof(*out));
  std::strncpy(out->driver, id.driver.c_str(), sizeof(out->driver)-1);
  std::strncpy(out->description, id.description.c_str(), sizeof(out->description)-1);
  std::strncpy(out->deviceName, id.deviceName.c_str(), sizeof(out->deviceName)-1);
  out->driverVersion = id.driverVersion;
  out->vendorId = id.vendorId;
  out->deviceId = id.deviceId;
  out->subSysId = id.subSysId;
  out->revision = id.revision;
  return dxmt9::core::D3D_OK;
}

extern "C" uint32_t dxmt9c_factory_get_adapter_mode_count(D9CFactory* f,
                                                            uint32_t adapter,
                                                            uint32_t d3dFmt) {
  auto modes = f->iface->EnumAdapterModes(adapter, fmtFromD3D(d3dFmt));
  return static_cast<uint32_t>(modes.size());
}

extern "C" int32_t dxmt9c_factory_enum_adapter_modes(D9CFactory* f, uint32_t adapter,
                                                       uint32_t d3dFmt, uint32_t modeIdx,
                                                       uint32_t* outW, uint32_t* outH,
                                                       uint32_t* outRefresh, uint32_t* outFmt) {
  auto modes = f->iface->EnumAdapterModes(adapter, fmtFromD3D(d3dFmt));
  if (modeIdx >= modes.size()) return dxmt9::core::D3DERR_INVALIDCALL;
  auto& m = modes[modeIdx];
  if (outW)       *outW       = m.width;
  if (outH)       *outH       = m.height;
  if (outRefresh) *outRefresh = m.refreshRate;
  if (outFmt)     *outFmt     = fmtToD3D(m.format);
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_factory_get_adapter_display_mode(D9CFactory* f, uint32_t adapter,
                                                             uint32_t* outW, uint32_t* outH,
                                                             uint32_t* outRefresh, uint32_t* outFmt) {
  auto m = f->iface->GetAdapterDisplayMode(adapter);
  if (outW)       *outW       = m.width;
  if (outH)       *outH       = m.height;
  if (outRefresh) *outRefresh = m.refreshRate;
  if (outFmt)     *outFmt     = fmtToD3D(m.format);
  return dxmt9::core::D3D_OK;
}

extern "C" uint64_t dxmt9c_factory_get_adapter_monitor(D9CFactory* f, uint32_t adapter) {
  return static_cast<uint64_t>(f->iface->GetAdapterMonitor(adapter));
}

extern "C" int32_t dxmt9c_factory_check_device_type(D9CFactory* f, uint32_t adapter,
                                                      uint32_t devType, uint32_t adapterFmt,
                                                      uint32_t backFmt, uint32_t windowed) {
  return f->iface->CheckDeviceType(adapter,
                                   static_cast<dxmt9::core::DeviceType>(devType),
                                   fmtFromD3D(adapterFmt), fmtFromD3D(backFmt),
                                   windowed != 0);
}

extern "C" int32_t dxmt9c_factory_check_device_format(D9CFactory* f, uint32_t adapter,
                                                        uint32_t d3dFmt, uint32_t usage) {
  return f->iface->CheckDeviceFormat(adapter, fmtFromD3D(d3dFmt), usageFromD3D(usage));
}

extern "C" int32_t dxmt9c_factory_check_device_multisample(D9CFactory* f, uint32_t adapter,
                                                             uint32_t d3dFmt, uint32_t msType,
                                                             uint32_t windowed) {
  if (!isSupportedD3DMultisample(msType)) {
    (void)windowed;
    return dxmt9::core::D3DERR_NOTAVAILABLE;
  }
  return f->iface->CheckDeviceMultiSampleType(adapter, fmtFromD3D(d3dFmt),
                                               msTypeFromD3D(msType));
  (void)windowed;
}

extern "C" int32_t dxmt9c_factory_get_caps(D9CFactory* f, uint32_t adapter, D9CCaps* out) {
  if (!out) return dxmt9::core::D3DERR_INVALIDCALL;
  fillCCaps(f->iface->GetDeviceCaps(adapter), out);
  out->adapterOrdinal = adapter;
  dxmt9DebugLog("factory_get_caps adapter=%u vs=0x%x ps=0x%x maxTex=%ux%u maxRT=%u maxLights=%u maxStreams=%u maxAniso=%u intervals=0x%x devCaps=0x%x rasterCaps=0x%x texCaps=0x%x textureOpCaps=0x%x",
                adapter,
                out->vertexShaderVersion,
                out->pixelShaderVersion,
                out->maxTextureWidth,
                out->maxTextureHeight,
                out->numSimultaneousRTs,
                out->maxActiveLights,
                out->maxStreams,
                out->maxAnisotropy,
                out->presentationIntervals,
                out->devCaps,
                out->rasterCaps,
                out->textureCaps,
                out->textureBlendCaps);
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_factory_get_adapter_luid(D9CFactory* f, uint32_t adapter,
                                                     uint32_t* lowPart, int32_t* highPart) {
  dxmt9::core::Luid luid{};
  if (!f->iface->GetAdapterLUID(adapter, &luid)) return dxmt9::core::D3DERR_INVALIDCALL;
  if (lowPart)  *lowPart  = luid.lowPart;
  if (highPart) *highPart = luid.highPart;
  return dxmt9::core::D3D_OK;
}

extern "C" D9CDevice* dxmt9c_factory_create_device(D9CFactory* f, uint32_t adapter,
                                                     const D9CPresentParams* pp,
                                                     uint32_t behaviorFlags,
                                                     const D9CDisplayModeEx* fullscreen) {
  if (!pp) return nullptr;
  dxmt9DebugLog("factory_create_device begin adapter=%u windowed=%u size=%ux%u fmt=%u hwnd=%llu behavior=0x%x fullscreen=%d",
                adapter,
                pp->windowed,
                pp->backBufferWidth,
                pp->backBufferHeight,
                pp->backBufferFormat,
                static_cast<unsigned long long>(pp->deviceWindow),
                behaviorFlags,
                fullscreen ? 1 : 0);
  auto params = ppFromC(*pp);
  dxmt9::com::IDirect3DDevice9Ex* dev = nullptr;

  if (fullscreen) {
    auto dmex = dmExFromC(*fullscreen);
    dev = f->iface->CreateDeviceEx(adapter, params, &dmex, behaviorFlags);
  } else {
    dev = f->iface->CreateDeviceEx(adapter, params, nullptr, behaviorFlags);
  }
  if (!dev) {
    dxmt9DebugLog("factory_create_device failed");
    return nullptr;
  }
  dxmt9DebugLog("factory_create_device ok iface=%p", static_cast<void*>(dev));
  return new D9CDevice(dev);
}

/* ── device ──────────────────────────────────────────────────────────────── */

extern "C" void dxmt9c_device_addref(D9CDevice* d) {
  if (d) d->refs.fetch_add(1);
}

extern "C" uint32_t dxmt9c_device_release(D9CDevice* d) {
  if (!d) return 0;
  uint32_t r = d->refs.fetch_sub(1) - 1;
  if (r == 0) delete d;
  return r;
}

extern "C" int32_t dxmt9c_device_get_caps(D9CDevice* d, D9CCaps* out) {
  if (!out) return dxmt9::core::D3DERR_INVALIDCALL;
  fillCCaps(d->iface->GetDeviceCaps(), out);
  dxmt9DebugLog("device_get_caps vs=0x%x ps=0x%x maxTex=%ux%u maxRT=%u maxLights=%u maxStreams=%u maxAniso=%u intervals=0x%x devCaps=0x%x rasterCaps=0x%x texCaps=0x%x textureOpCaps=0x%x",
                out->vertexShaderVersion,
                out->pixelShaderVersion,
                out->maxTextureWidth,
                out->maxTextureHeight,
                out->numSimultaneousRTs,
                out->maxActiveLights,
                out->maxStreams,
                out->maxAnisotropy,
                out->presentationIntervals,
                out->devCaps,
                out->rasterCaps,
                out->textureCaps,
                out->textureBlendCaps);
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_device_test_cooperative_level(D9CDevice* d) {
  return d->iface->TestCooperativeLevel();
}
extern "C" int32_t dxmt9c_device_check_device_state(D9CDevice* d, uint64_t w) {
  return d->iface->CheckDeviceState({w});
}
extern "C" int32_t dxmt9c_device_reset(D9CDevice* d, const D9CPresentParams* pp) {
  if (!pp) return dxmt9::core::D3DERR_INVALIDCALL;
  return d->iface->Reset(ppFromC(*pp));
}
extern "C" int32_t dxmt9c_device_reset_ex(D9CDevice* d, const D9CPresentParams* pp,
                                           const D9CDisplayModeEx* dm) {
  if (!pp) return dxmt9::core::D3DERR_INVALIDCALL;
  auto params = ppFromC(*pp);
  if (dm) {
    auto dmex = dmExFromC(*dm);
    return d->iface->ResetEx(params, &dmex);
  }
  return d->iface->ResetEx(params, nullptr);
}
extern "C" int32_t dxmt9c_device_present(D9CDevice* d,
                                          const D9CRect* src, const D9CRect* dst,
                                          uint64_t destWindow, const void* dirty,
                                          uint32_t flags) {
  dxmt9DebugLog("device_present begin destWindow=%llu flags=0x%x src=%d dst=%d",
                static_cast<unsigned long long>(destWindow),
                flags,
                src ? 1 : 0,
                dst ? 1 : 0);
  using R = dxmt9::core::Rect;
  R* ps = src ? new R{src->left,src->top,src->right,src->bottom} : nullptr;
  R* pd = dst ? new R{dst->left,dst->top,dst->right,dst->bottom} : nullptr;
  auto hr = d->iface->PresentEx(ps, pd, {destWindow}, dirty, flags);
  delete ps; delete pd;
  dxmt9DebugLog("device_present hr=0x%08x", static_cast<unsigned>(hr));
  return hr;
}
extern "C" int32_t dxmt9c_device_begin_scene(D9CDevice* d) {
  return d->iface->BeginScene();
}
extern "C" int32_t dxmt9c_device_end_scene(D9CDevice* d) {
  return d->iface->EndScene();
}

extern "C" int32_t dxmt9c_device_clear(D9CDevice* d, uint32_t count,
                                        const D9CRect* rects, uint32_t flags,
                                        uint32_t colorARGB, float z, uint32_t stencil) {
  dxmt9::core::ClearDesc desc;
  desc.clearColor   = (flags & 1) != 0;   /* D3DCLEAR_TARGET = 1 */
  desc.clearDepth   = (flags & 2) != 0;   /* D3DCLEAR_ZBUFFER = 2 */
  desc.clearStencil = (flags & 4) != 0;   /* D3DCLEAR_STENCIL = 4 */
  /* ARGB → RGBA float */
  desc.color.a = ((colorARGB >> 24) & 0xff) / 255.0f;
  desc.color.r = ((colorARGB >> 16) & 0xff) / 255.0f;
  desc.color.g = ((colorARGB >>  8) & 0xff) / 255.0f;
  desc.color.b = ((colorARGB      ) & 0xff) / 255.0f;
  desc.depth   = z;
  desc.stencil = stencil;
  for (uint32_t i = 0; i < count; ++i) {
    desc.rects.push_back({rects[i].left, rects[i].top,
                          rects[i].right, rects[i].bottom});
  }
  return d->iface->Clear(desc);
}

extern "C" int32_t dxmt9c_device_set_viewport(D9CDevice* d, const D9CViewport* vp) {
  if (!vp) return dxmt9::core::D3DERR_INVALIDCALL;
  return d->iface->SetViewport({vp->x,vp->y,vp->width,vp->height,vp->minZ,vp->maxZ});
}
extern "C" void dxmt9c_device_get_viewport(D9CDevice* d, D9CViewport* vp) {
  if (!d || !vp) return;
  const auto value = d->iface->GetViewport();
  vp->x = value.x;
  vp->y = value.y;
  vp->width = value.width;
  vp->height = value.height;
  vp->minZ = value.minZ;
  vp->maxZ = value.maxZ;
}
extern "C" int32_t dxmt9c_device_set_scissor_rect(D9CDevice* d, const D9CRect* r) {
  if (!r) return dxmt9::core::D3DERR_INVALIDCALL;
  return d->iface->SetScissorRect({r->left,r->top,r->right,r->bottom});
}
extern "C" void dxmt9c_device_get_scissor_rect(D9CDevice* d, D9CRect* r) {
  if (!d || !r) return;
  const auto value = d->iface->GetScissorRect();
  r->left = value.left;
  r->top = value.top;
  r->right = value.right;
  r->bottom = value.bottom;
}

static uint32_t transformStateFromD3D(uint32_t state) {
  switch (state) {
    case 2u:
      return dxmt9::core::XFORM_VIEW;
    case 3u:
      return dxmt9::core::XFORM_PROJECTION;
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

extern "C" int32_t dxmt9c_device_set_transform(D9CDevice* d, uint32_t state,
                                                const D9CMatrix* m) {
  if (!m) return dxmt9::core::D3DERR_INVALIDCALL;
  dxmt9::core::Matrix4x4 mat;
  std::memcpy(mat.m.data(), m->m, 16 * sizeof(float));
  return d->iface->SetTransform(transformStateFromD3D(state), mat);
}
extern "C" int32_t dxmt9c_device_get_transform(D9CDevice* d, uint32_t state,
                                                D9CMatrix* m) {
  (void)d; (void)state; (void)m;
  return dxmt9::core::D3D_OK;
}
extern "C" int32_t dxmt9c_device_set_material(D9CDevice* d, const D9CMaterial* m) {
  if (!m) return dxmt9::core::D3DERR_INVALIDCALL;
  dxmt9::core::Material mat;
  std::memcpy(&mat.diffuse,  &m->diffuse,  sizeof(dxmt9::core::ColorRGBA));
  std::memcpy(&mat.ambient,  &m->ambient,  sizeof(dxmt9::core::ColorRGBA));
  std::memcpy(&mat.specular, &m->specular, sizeof(dxmt9::core::ColorRGBA));
  std::memcpy(&mat.emissive, &m->emissive, sizeof(dxmt9::core::ColorRGBA));
  mat.power = m->power;
  return d->iface->SetMaterial(mat);
}
extern "C" int32_t dxmt9c_device_get_material(D9CDevice* d, D9CMaterial* m) {
  (void)d; (void)m;
  return dxmt9::core::D3D_OK;
}
extern "C" int32_t dxmt9c_device_set_light(D9CDevice* d, uint32_t idx,
                                            const D9CLight* l) {
  if (!l) return dxmt9::core::D3DERR_INVALIDCALL;
  dxmt9::core::Light light;
  light.type = static_cast<dxmt9::core::LightType>(l->type);
  std::memcpy(&light.diffuse,  &l->diffuse,  sizeof(dxmt9::core::ColorRGBA));
  std::memcpy(&light.specular, &l->specular, sizeof(dxmt9::core::ColorRGBA));
  std::memcpy(&light.ambient,  &l->ambient,  sizeof(dxmt9::core::ColorRGBA));
  std::memcpy(light.position.data(),  l->position,  3*sizeof(float));
  std::memcpy(light.direction.data(), l->direction, 3*sizeof(float));
  light.range = l->range; light.falloff = l->falloff;
  light.attenuation0 = l->attenuation0;
  light.attenuation1 = l->attenuation1;
  light.attenuation2 = l->attenuation2;
  light.theta = l->theta; light.phi = l->phi;
  return d->iface->SetLight(idx, light);
}
extern "C" int32_t dxmt9c_device_light_enable(D9CDevice* d, uint32_t i, uint32_t en) {
  return d->iface->LightEnable(i, en != 0);
}

extern "C" int32_t dxmt9c_device_set_render_state(D9CDevice* d, uint32_t s, uint32_t v) {
  if (d && d->stateBlockRecording) {
    d->stateBlockRenderStates.insert(s);
  }
  return d->iface->SetRenderState(s, v);
}
extern "C" uint32_t dxmt9c_device_get_render_state(D9CDevice* d, uint32_t s) {
  return d->iface->GetRenderState(s);
}
extern "C" int32_t dxmt9c_device_set_texture_stage_state(D9CDevice* d, uint32_t st,
                                                          uint32_t type, uint32_t val) {
  return d->iface->SetTextureStageState(st, type, val);
}
extern "C" uint32_t dxmt9c_device_get_texture_stage_state(D9CDevice* d, uint32_t st,
                                                            uint32_t type) {
  return d->iface->GetTextureStageState(st, type);
}
extern "C" int32_t dxmt9c_device_set_sampler_state(D9CDevice* d, uint32_t s,
                                                    uint32_t type, uint32_t val) {
  return d->iface->SetSamplerState(s, type, val);
}
extern "C" uint32_t dxmt9c_device_get_sampler_state(D9CDevice* d, uint32_t s,
                                                      uint32_t type) {
  return d->iface->GetSamplerState(s, type);
}
extern "C" int32_t dxmt9c_device_set_clip_plane(D9CDevice* d, uint32_t idx,
                                                 const float plane[4]) {
  dxmt9::core::ClipPlane cp{plane[0],plane[1],plane[2],plane[3]};
  return d->iface->SetClipPlane(idx, cp);
}
extern "C" int32_t dxmt9c_device_get_clip_plane(D9CDevice* d, uint32_t idx,
                                                  float plane[4]) {
  (void)d; (void)idx; (void)plane;
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_device_set_fvf(D9CDevice* d, uint32_t fvf) {
  return d->iface->SetFVF(fvf);
}
extern "C" uint32_t dxmt9c_device_get_fvf(D9CDevice* d) {
  (void)d; return 0;
}
extern "C" int32_t dxmt9c_device_set_vertex_declaration(D9CDevice* d, D9CVertexDecl* vd) {
  if (!vd) return d->iface->SetVertexDeclaration({});
  return d->iface->SetVertexDeclaration(vd->elements);
}
extern "C" int32_t dxmt9c_device_set_stream_source(D9CDevice* d, uint32_t stream,
                                                    D9CBuffer* buf, uint32_t off,
                                                    uint32_t stride) {
  auto bufPtr = buf ? buf->obj : nullptr;
  return d->iface->SetStreamSource(stream, bufPtr, off, stride);
}
extern "C" int32_t dxmt9c_device_set_stream_source_freq(D9CDevice* d, uint32_t stream,
                                                          uint32_t freq) {
  (void)d; (void)stream; (void)freq;
  return dxmt9::core::D3D_OK;
}
extern "C" int32_t dxmt9c_device_set_indices(D9CDevice* d, D9CBuffer* buf) {
  auto bufPtr = buf ? buf->obj : nullptr;
  return d->iface->SetIndices(bufPtr);
}
extern "C" int32_t dxmt9c_device_set_texture(D9CDevice* d, uint32_t stage,
                                              D9CTexture* tex) {
  auto texPtr = tex ? tex->obj : nullptr;
  return d->iface->SetTexture(stage, texPtr);
}

extern "C" int32_t dxmt9c_device_set_vertex_shader(D9CDevice* d, D9CShader* s) {
  if (!s) return d->iface->SetVertexShader({});
  return d->iface->SetVertexShader(s->ref);
}
extern "C" int32_t dxmt9c_device_set_pixel_shader(D9CDevice* d, D9CShader* s) {
  if (!s) return d->iface->SetPixelShader({});
  return d->iface->SetPixelShader(s->ref);
}

static int32_t setVsConst(D9CDevice* d, uint32_t start, const float* data, uint32_t cnt,
                           bool ps) {
  auto& state = d->dev().mutableState();
  if (ps) {
    auto& consts = state.psConst;
    for (uint32_t i = 0; i < cnt && (start+i) < consts.float4.size(); ++i) {
      consts.float4[start+i][0] = data[i*4+0];
      consts.float4[start+i][1] = data[i*4+1];
      consts.float4[start+i][2] = data[i*4+2];
      consts.float4[start+i][3] = data[i*4+3];
    }
  } else {
    auto& consts = state.vsConst;
    for (uint32_t i = 0; i < cnt && (start+i) < consts.float4.size(); ++i) {
      consts.float4[start+i][0] = data[i*4+0];
      consts.float4[start+i][1] = data[i*4+1];
      consts.float4[start+i][2] = data[i*4+2];
      consts.float4[start+i][3] = data[i*4+3];
    }
  }
  return dxmt9::core::D3D_OK;
}
extern "C" int32_t dxmt9c_device_set_vs_const_f(D9CDevice* d, uint32_t s,
                                                  const float* data, uint32_t cnt) {
  return setVsConst(d, s, data, cnt, false);
}
extern "C" int32_t dxmt9c_device_get_vs_const_f(D9CDevice* d, uint32_t s,
                                                  float* data, uint32_t cnt) {
  auto& consts = d->dev().state().vsConst;
  for (uint32_t i = 0; i < cnt && (s+i) < consts.float4.size(); ++i) {
    data[i*4+0] = consts.float4[s+i][0];
    data[i*4+1] = consts.float4[s+i][1];
    data[i*4+2] = consts.float4[s+i][2];
    data[i*4+3] = consts.float4[s+i][3];
  }
  return dxmt9::core::D3D_OK;
}
extern "C" int32_t dxmt9c_device_set_ps_const_f(D9CDevice* d, uint32_t s,
                                                  const float* data, uint32_t cnt) {
  return setVsConst(d, s, data, cnt, true);
}
extern "C" int32_t dxmt9c_device_get_ps_const_f(D9CDevice* d, uint32_t s,
                                                  float* data, uint32_t cnt) {
  auto& consts = d->dev().state().psConst;
  for (uint32_t i = 0; i < cnt && (s+i) < consts.float4.size(); ++i) {
    data[i*4+0] = consts.float4[s+i][0];
    data[i*4+1] = consts.float4[s+i][1];
    data[i*4+2] = consts.float4[s+i][2];
    data[i*4+3] = consts.float4[s+i][3];
  }
  return dxmt9::core::D3D_OK;
}
extern "C" int32_t dxmt9c_device_set_vs_const_i(D9CDevice* d, uint32_t s,
                                                  const int32_t* data, uint32_t cnt) {
  auto& consts = d->dev().mutableState().vsConst;
  for (uint32_t i = 0; i < cnt && (s+i) < consts.int4.size(); ++i) {
    consts.int4[s+i][0] = data[i*4+0];
    consts.int4[s+i][1] = data[i*4+1];
    consts.int4[s+i][2] = data[i*4+2];
    consts.int4[s+i][3] = data[i*4+3];
  }
  return dxmt9::core::D3D_OK;
}
extern "C" int32_t dxmt9c_device_set_ps_const_i(D9CDevice* d, uint32_t s,
                                                  const int32_t* data, uint32_t cnt) {
  auto& consts = d->dev().mutableState().psConst;
  for (uint32_t i = 0; i < cnt && (s+i) < consts.int4.size(); ++i) {
    consts.int4[s+i][0] = data[i*4+0];
    consts.int4[s+i][1] = data[i*4+1];
    consts.int4[s+i][2] = data[i*4+2];
    consts.int4[s+i][3] = data[i*4+3];
  }
  return dxmt9::core::D3D_OK;
}
extern "C" int32_t dxmt9c_device_set_vs_const_b(D9CDevice* d, uint32_t s,
                                                  const uint32_t* data, uint32_t cnt) {
  auto& consts = d->dev().mutableState().vsConst;
  for (uint32_t i = 0; i < cnt && (s+i) < consts.bools.size(); ++i)
    consts.bools[s+i] = (data[i] != 0);
  return dxmt9::core::D3D_OK;
}
extern "C" int32_t dxmt9c_device_set_ps_const_b(D9CDevice* d, uint32_t s,
                                                  const uint32_t* data, uint32_t cnt) {
  auto& consts = d->dev().mutableState().psConst;
  for (uint32_t i = 0; i < cnt && (s+i) < consts.bools.size(); ++i)
    consts.bools[s+i] = (data[i] != 0);
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_device_set_render_target(D9CDevice* d, uint32_t idx,
                                                    D9CSurface* surf) {
  return d->iface->SetRenderTarget(idx, surf ? surf->obj : nullptr);
}
extern "C" D9CSurface* dxmt9c_device_get_render_target(D9CDevice* d, uint32_t idx) {
  auto sw = d->iface->GetSwapChain(0);
  if (!sw) return nullptr;
  auto surf = (idx == 0) ? sw->backBuffer() : nullptr;
  if (!surf) return nullptr;
  auto* wrap = new D9CSurface{surf};
  return wrap;
}
extern "C" int32_t dxmt9c_device_set_depth_stencil(D9CDevice* d, D9CSurface* surf) {
  return d->iface->SetDepthStencilSurface(surf ? surf->obj : nullptr);
}
extern "C" D9CSurface* dxmt9c_device_get_depth_stencil(D9CDevice* d) {
  auto sw = d->iface->GetSwapChain(0);
  if (!sw) return nullptr;
  auto surf = sw->depthStencilSurface();
  if (!surf) return nullptr;
  return new D9CSurface{surf};
}

extern "C" int32_t dxmt9c_device_draw_primitive(D9CDevice* d, uint32_t type,
                                                  uint32_t startVertex, uint32_t count) {
  return d->iface->DrawPrimitive(ptFromD3D(type), count, startVertex);
}
extern "C" int32_t dxmt9c_device_draw_indexed_primitive(D9CDevice* d, uint32_t type,
                                                          int32_t baseVertex, uint32_t minV,
                                                          uint32_t numV, uint32_t startIdx,
                                                          uint32_t count) {
  (void)minV; (void)numV;
  auto& st = d->dev().state();
  return d->iface->DrawIndexedPrimitive(ptFromD3D(type), count, 0,
                                         baseVertex, startIdx, st.indexType);
}
extern "C" int32_t dxmt9c_device_draw_primitive_up(D9CDevice* d, uint32_t type,
                                                     uint32_t count, const void* data,
                                                     uint32_t stride) {
  /* approximate vertex count */
  size_t bytes = stride * (count + 2) * 3;
  auto sp = std::span<const dxmt9::core::u8>(
    reinterpret_cast<const dxmt9::core::u8*>(data), bytes);
  return d->iface->DrawPrimitiveUP(ptFromD3D(type), count, sp);
}
extern "C" int32_t dxmt9c_device_draw_indexed_primitive_up(D9CDevice* d, uint32_t type,
                                                             uint32_t minV, uint32_t numV,
                                                             uint32_t count,
                                                             const void* idxData,
                                                             uint32_t idxFmt,
                                                             const void* vtxData,
                                                             uint32_t stride) {
  size_t vtxBytes = stride * (minV + numV);
  uint32_t idxSize = (idxFmt == 102) ? 4 : 2;
  size_t idxBytes = idxSize * count * 3;
  auto vsp = std::span<const dxmt9::core::u8>(
    reinterpret_cast<const dxmt9::core::u8*>(vtxData), vtxBytes);
  auto isp = std::span<const dxmt9::core::u8>(
    reinterpret_cast<const dxmt9::core::u8*>(idxData), idxBytes);
  return d->iface->DrawIndexedPrimitiveUP(ptFromD3D(type), count, vsp, isp,
                                           idxTypeFromD3D(idxFmt));
}

extern "C" int32_t dxmt9c_device_update_surface(D9CDevice* d, D9CSurface* src,
                                                  const D9CRect*, D9CSurface* dst,
                                                  const D9CRect*) {
  if (!src || !dst) return dxmt9::core::D3DERR_INVALIDCALL;
  return d->iface->UpdateSurface(src->obj, dst->obj);
}
extern "C" int32_t dxmt9c_device_update_texture(D9CDevice* d, D9CTexture* src,
                                                  D9CTexture* dst) {
  if (!src || !dst) return dxmt9::core::D3DERR_INVALIDCALL;
  return d->iface->UpdateTexture(src->obj, dst->obj);
}
extern "C" int32_t dxmt9c_device_stretch_rect(D9CDevice* d, D9CSurface* src,
                                               const D9CRect* sr, D9CSurface* dst,
                                               const D9CRect* dr, uint32_t filter) {
  if (!src || !dst) return dxmt9::core::D3DERR_INVALIDCALL;
  dxmt9::core::Rect* ps = sr ? new dxmt9::core::Rect{sr->left,sr->top,sr->right,sr->bottom} : nullptr;
  dxmt9::core::Rect* pd = dr ? new dxmt9::core::Rect{dr->left,dr->top,dr->right,dr->bottom} : nullptr;
  auto hr = d->iface->StretchRect(src->obj, ps, dst->obj, pd, filter != 1);
  delete ps; delete pd;
  return hr;
}
extern "C" int32_t dxmt9c_device_color_fill(D9CDevice* d, D9CSurface* surf,
                                             const D9CRect* r, uint32_t colorARGB) {
  if (!surf) return dxmt9::core::D3DERR_INVALIDCALL;
  dxmt9::core::Rect* pr = r ? new dxmt9::core::Rect{r->left,r->top,r->right,r->bottom} : nullptr;
  dxmt9::core::ColorRGBA rgba{
    ((colorARGB>>16)&0xff)/255.0f,
    ((colorARGB>> 8)&0xff)/255.0f,
    ((colorARGB    )&0xff)/255.0f,
    ((colorARGB>>24)&0xff)/255.0f,
  };
  auto hr = d->iface->FillSurface(surf->obj, pr, rgba);
  delete pr;
  return hr;
}
extern "C" int32_t dxmt9c_device_get_render_target_data(D9CDevice* d, D9CSurface* rt,
                                                          D9CSurface* dst) {
  if (!rt || !dst) return dxmt9::core::D3DERR_INVALIDCALL;
  return d->iface->GetRenderTargetData(rt->obj, dst->obj);
}

extern "C" int32_t dxmt9c_device_set_maximum_frame_latency(D9CDevice* d, uint32_t l) {
  return d->iface->SetMaximumFrameLatency(l);
}
extern "C" uint32_t dxmt9c_device_get_maximum_frame_latency(D9CDevice* d) {
  return d->iface->GetMaximumFrameLatency();
}
extern "C" int32_t dxmt9c_device_wait_for_vblank(D9CDevice* d, uint32_t idx) {
  return d->iface->WaitForVBlank(idx);
}
extern "C" int32_t dxmt9c_device_check_device_multisample(D9CDevice* d,
                                                            uint32_t fmt, uint32_t msType,
                                                            uint32_t windowed) {
  if (!isSupportedD3DMultisample(msType)) {
    (void)windowed;
    return dxmt9::core::D3DERR_NOTAVAILABLE;
  }
  return d->iface->CheckDeviceMultiSampleType(fmtFromD3D(fmt), msTypeFromD3D(msType));
  (void)windowed;
}
extern "C" D9CSwapChain* dxmt9c_device_get_swap_chain(D9CDevice* d, uint32_t idx) {
  auto* sw = d->iface->GetSwapChain(idx);
  if (!sw) return nullptr;
  return new D9CSwapChain(sw);
}
extern "C" uint32_t dxmt9c_device_get_swap_chain_count(D9CDevice* d) {
  return static_cast<uint32_t>(d->iface->GetSwapChainCount());
}
extern "C" D9CSwapChain* dxmt9c_device_create_additional_swap_chain(D9CDevice* d,
                                                                      const D9CPresentParams* pp) {
  if (!pp) return nullptr;
  auto* sw = d->iface->CreateAdditionalSwapChain(ppFromC(*pp));
  if (!sw) return nullptr;
  return new D9CSwapChain(sw);
}

/* ── resource creation ───────────────────────────────────────────────────── */

extern "C" D9CTexture* dxmt9c_device_create_texture(D9CDevice* d, uint32_t w, uint32_t h,
                                                      uint32_t levels, uint32_t usage,
                                                      uint32_t fmt, uint32_t pool) {
  dxmt9DebugLog("device_create_texture begin device=%p size=%ux%u levels=%u usage=0x%x fmt=%u(%s) pool=%u",
                static_cast<void*>(d), w, h, levels, usage, fmt,
                dxmt9::core::formatName(fmtFromD3D(fmt)).c_str(), pool);
  dxmt9::core::TextureDesc desc;
  desc.width = w; desc.height = h; desc.levels = levels ? levels : 1;
  desc.format = fmtFromD3D(fmt);
  desc.pool = poolFromD3D(pool);
  desc.usage = usageFromD3D(usage);
  desc.type = dxmt9::core::TextureType::TwoD;
  auto tex = d->iface->CreateTexture(desc);
  if (!tex) {
    dxmt9DebugLog("device_create_texture failed device=%p", static_cast<void*>(d));
    return nullptr;
  }
  dxmt9DebugLog("device_create_texture ok texture=%p levels=%u",
                static_cast<void*>(tex.get()), tex->levelCount());
  return new D9CTexture{tex, d};
}
extern "C" D9CTexture* dxmt9c_device_create_cube_texture(D9CDevice* d, uint32_t size,
                                                           uint32_t levels, uint32_t usage,
                                                           uint32_t fmt, uint32_t pool) {
  dxmt9::core::TextureDesc desc;
  desc.width = size; desc.height = size; desc.levels = levels ? levels : 1;
  desc.format = fmtFromD3D(fmt);
  desc.pool = poolFromD3D(pool);
  desc.usage = usageFromD3D(usage);
  desc.type = dxmt9::core::TextureType::Cube;
  auto tex = d->iface->CreateTexture(desc);
  if (!tex) return nullptr;
  return new D9CTexture{tex, d};
}
extern "C" D9CTexture* dxmt9c_device_create_volume_texture(D9CDevice* d, uint32_t w,
                                                             uint32_t h, uint32_t depth,
                                                             uint32_t levels, uint32_t usage,
                                                             uint32_t fmt, uint32_t pool) {
  dxmt9::core::TextureDesc desc;
  desc.width = w; desc.height = h; desc.depth = depth;
  desc.levels = levels ? levels : 1;
  desc.format = fmtFromD3D(fmt);
  desc.pool = poolFromD3D(pool);
  desc.usage = usageFromD3D(usage);
  desc.type = dxmt9::core::TextureType::Volume;
  auto tex = d->iface->CreateTexture(desc);
  if (!tex) return nullptr;
  return new D9CTexture{tex, d};
}

extern "C" D9CBuffer* dxmt9c_device_create_vertex_buffer(D9CDevice* d, uint32_t len,
                                                           uint32_t usage, uint32_t fvf,
                                                           uint32_t pool) {
  dxmt9::core::BufferDesc desc{len, poolFromD3D(pool),
                               static_cast<uint32_t>(usageFromD3D(usage) |
                                                     dxmt9::core::UsageVertexBuffer)};
  auto buf = d->iface->CreateBuffer(desc);
  if (!buf) return nullptr;
  auto* out = new D9CBuffer{buf};
  out->desc.size = len;
  out->desc.usage = usage;
  out->desc.pool = pool;
  out->desc.fvf = fvf;
  out->desc.format = 0;
  return out;
}
extern "C" D9CBuffer* dxmt9c_device_create_index_buffer(D9CDevice* d, uint32_t len,
                                                          uint32_t usage, uint32_t fmt,
                                                          uint32_t pool) {
  dxmt9::core::BufferDesc desc{len, poolFromD3D(pool),
                               static_cast<uint32_t>(usageFromD3D(usage) |
                                                     dxmt9::core::UsageIndexBuffer)};
  auto buf = d->iface->CreateBuffer(desc);
  if (!buf) return nullptr;
  auto* out = new D9CBuffer{buf};
  out->desc.size = len;
  out->desc.usage = usage;
  out->desc.pool = pool;
  out->desc.fvf = 0;
  out->desc.format = fmt;
  return out;
}

extern "C" D9CSurface* dxmt9c_device_create_render_target(D9CDevice* d, uint32_t w,
                                                            uint32_t h, uint32_t fmt,
                                                            uint32_t msType, uint32_t /*msQ*/,
                                                            uint32_t /*lockable*/,
                                                            uint64_t* /*shared*/) {
  dxmt9::core::SurfaceDesc desc;
  desc.width = w; desc.height = h;
  desc.format = fmtFromD3D(fmt);
  desc.renderTarget = true;
  desc.multiSampleType = msTypeFromD3D(msType);
  auto surf = d->iface->CreateSurface(desc);
  if (!surf) return nullptr;
  return new D9CSurface{surf};
}
extern "C" D9CSurface* dxmt9c_device_create_depth_stencil(D9CDevice* d, uint32_t w,
                                                            uint32_t h, uint32_t fmt,
                                                            uint32_t msType, uint32_t /*msQ*/,
                                                            uint32_t /*discard*/,
                                                            uint64_t* /*shared*/) {
  dxmt9::core::SurfaceDesc desc;
  desc.width = w; desc.height = h;
  desc.format = fmtFromD3D(fmt);
  desc.depthStencil = true;
  desc.multiSampleType = msTypeFromD3D(msType);
  auto surf = d->iface->CreateSurface(desc);
  if (!surf) return nullptr;
  return new D9CSurface{surf};
}
extern "C" D9CSurface* dxmt9c_device_create_offscreen_surface(D9CDevice* d, uint32_t w,
                                                                uint32_t h, uint32_t fmt,
                                                                uint32_t pool,
                                                                uint64_t* /*shared*/) {
  dxmt9::core::SurfaceDesc desc;
  desc.width = w; desc.height = h;
  desc.format = fmtFromD3D(fmt);
  desc.pool = poolFromD3D(pool);
  auto surf = d->iface->CreateSurface(desc);
  if (!surf) return nullptr;
  return new D9CSurface{surf};
}

extern "C" D9CShader* dxmt9c_device_create_vertex_shader(D9CDevice* d,
                                                           const uint32_t* bytecode) {
  dxmt9DebugLog("device_create_vertex_shader begin device=%p bytecode=%p",
                static_cast<void*>(d), bytecode);
  if (!bytecode) {
    dxmt9DebugLog("device_create_vertex_shader failed: null bytecode");
    return nullptr;
  }
  size_t n = 0;
  if (!computeShaderBytecodeWordCount(bytecode, &n)) {
    dxmt9DebugLog("device_create_vertex_shader failed: invalid bytecode layout bytecode=%p",
                  bytecode);
    return nullptr;
  }
  dxmt9DebugLog("device_create_vertex_shader bytecode=%p dwords=%zu version=0x%08x end=0x%08x",
                bytecode, n, bytecode[0], bytecode[n - 1]);
  dxmt9::core::ShaderBytecode bc;
  bc.bytes.assign(reinterpret_cast<const uint8_t*>(bytecode),
                  reinterpret_cast<const uint8_t*>(bytecode) + n*4);
  bc.hash = dxmt9::core::hashBytes(
    std::span<const std::byte>(reinterpret_cast<const std::byte*>(bc.bytes.data()),
                                bc.bytes.size()));
  dxmt9::core::ShaderRef ref;
  ref.kind = dxmt9::core::ShaderRef::Kind::Bytecode;
  ref.hash = bc.hash;
  ref.bytecode = std::move(bc);
  maybeDumpShaderBytecode("shader", bytecode, n, ref.hash);
  auto* s = new D9CShader;
  s->ref = std::move(ref);
  s->bytecodeWords.assign(bytecode, bytecode + n);
  return s;
}
extern "C" D9CShader* dxmt9c_device_create_pixel_shader(D9CDevice* d,
                                                          const uint32_t* bytecode) {
  (void)d;
  dxmt9DebugLog("device_create_pixel_shader bytecode=%p", bytecode);
  return dxmt9c_device_create_vertex_shader(d, bytecode); /* same logic */
}

extern "C" D9CVertexDecl* dxmt9c_device_create_vertex_declaration(
    D9CDevice* /*d*/, const D9CVertexElement* elems) {
  auto* vd = new D9CVertexDecl;
  for (const D9CVertexElement* e = elems;
       !(e->stream == 0xff && e->type == 17 /* D3DDECLTYPE_UNUSED */); ++e) {
    dxmt9::core::VertexElement ve;
    ve.stream = e->stream;
    ve.offset = e->offset;
    ve.type   = e->type;
    ve.method = e->method;
    ve.usage  = e->usage;
    ve.usageIndex = e->usageIndex;
    vd->elements.push_back(ve);
    vd->raw.push_back(*e);
  }
  /* sentinel */
  vd->raw.push_back({0xff,0,17,0,0,0});
  return vd;
}

extern "C" D9CQuery* dxmt9c_device_create_query(D9CDevice* d, uint32_t type) {
  dxmt9::core::QueryType qt;
  switch (type) {
    case 8:  qt = dxmt9::core::QueryType::Occlusion; break;
    case 9:  qt = dxmt9::core::QueryType::Timestamp; break;
    case 10: qt = dxmt9::core::QueryType::TimestampDisjoint; break;
    case 11: qt = dxmt9::core::QueryType::TimestampFreq; break;
    default: qt = dxmt9::core::QueryType::Event; break;
  }
  auto q = d->iface->CreateQuery(qt);
  if (!q) return nullptr;
  return new D9CQuery{q, d};
}

extern "C" D9CStateBlock* dxmt9c_device_create_state_block(D9CDevice* d, uint32_t /*type*/) {
  auto sb = d->iface->CreateStateBlock();
  if (!sb) return nullptr;
  return new D9CStateBlock{sb, d};
}
extern "C" int32_t dxmt9c_device_begin_state_block(D9CDevice* d) {
  if (!d || d->stateBlockRecording) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  d->stateBlockBaseState = d->dev().state();
  d->stateBlockRenderStates.clear();
  d->stateBlockRecording = true;
  return dxmt9::core::D3D_OK;
}
extern "C" int32_t dxmt9c_device_end_state_block(D9CDevice* d, D9CStateBlock** out) {
  if (!out) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  *out = nullptr;
  if (!d || !d->stateBlockRecording) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  d->stateBlockRecording = false;
  if (!d->stateBlockBaseState.has_value()) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  auto sb = std::make_shared<dxmt9::core::StateBlock>();
  sb->captureDelta(*d->stateBlockBaseState, d->dev().state(), d->stateBlockRenderStates);
  d->stateBlockBaseState.reset();
  d->stateBlockRenderStates.clear();
  *out = new D9CStateBlock{sb, d};
  return dxmt9::core::D3D_OK;
}

/* ── swap chain ──────────────────────────────────────────────────────────── */

extern "C" void dxmt9c_swapchain_addref(D9CSwapChain* s) {
  if (s) s->refs.fetch_add(1);
}
extern "C" uint32_t dxmt9c_swapchain_release(D9CSwapChain* s) {
  if (!s) return 0;
  uint32_t r = s->refs.fetch_sub(1) - 1;
  if (r == 0) delete s;
  return r;
}
extern "C" int32_t dxmt9c_swapchain_present(D9CSwapChain* s,
                                             const D9CRect*, const D9CRect*,
                                             uint64_t, const void*, uint32_t) {
  return s->iface->Present();
}
extern "C" D9CSurface* dxmt9c_swapchain_get_back_buffer(D9CSwapChain* s, uint32_t,
                                                          uint32_t) {
  auto surf = s->iface->backBuffer();
  if (!surf) return nullptr;
  return new D9CSurface{surf};
}
extern "C" D9CSurface* dxmt9c_swapchain_get_depth_stencil(D9CSwapChain* s) {
  auto surf = s->iface->depthStencilSurface();
  if (!surf) return nullptr;
  return new D9CSurface{surf};
}
extern "C" int32_t dxmt9c_swapchain_get_present_params(D9CSwapChain* s,
                                                         D9CPresentParams* out) {
  if (!out) return dxmt9::core::D3DERR_INVALIDCALL;
  auto& p = s->iface->presentParameters();
  std::memset(out, 0, sizeof(*out));
  out->backBufferWidth  = p.backBufferWidth;
  out->backBufferHeight = p.backBufferHeight;
  out->backBufferFormat = fmtToD3D(p.backBufferFormat);
  out->backBufferCount = p.backBufferCount;
  out->multiSampleType = msTypeToD3D(p.multiSampleType);
  out->swapEffect = p.discardSwapEffect ? 1u : 2u;
  out->deviceWindow = p.deviceWindow.value;
  out->windowed         = p.windowed;
  out->enableAutoDepthStencil = p.enableAutoDepthStencil;
  out->autoDepthStencilFormat = fmtToD3D(p.autoDepthStencilFormat);
  switch (p.presentationInterval) {
    case dxmt9::core::PresentInterval::Immediate:
      out->presentationInterval = 0;
      break;
    case dxmt9::core::PresentInterval::Two:
      out->presentationInterval = 2;
      break;
    case dxmt9::core::PresentInterval::Default:
    default:
      out->presentationInterval = 1;
      break;
  }
  return dxmt9::core::D3D_OK;
}

/* ── texture ─────────────────────────────────────────────────────────────── */

extern "C" void dxmt9c_texture_addref(D9CTexture* t) {
  if (t) t->refs.fetch_add(1);
}
extern "C" uint32_t dxmt9c_texture_release(D9CTexture* t) {
  if (!t) return 0;
  uint32_t r = t->refs.fetch_sub(1) - 1;
  if (r == 0) {
    for (auto& [_, lock] : t->wow64Locks) {
      releaseShadowLock(lock);
    }
    delete t;
  }
  return r;
}
extern "C" int32_t dxmt9c_texture_lock_rect(D9CTexture* t, uint32_t level,
                                             D9CLockedRect* out, const D9CRect* r,
                                             uint32_t flags) {
  if (!out) return dxmt9::core::D3DERR_INVALIDCALL;
  if (r) {
    dxmt9DebugLog("texture_lock_rect begin texture=%p level=%u flags=0x%x rect=(%d,%d)-(%d,%d)",
                  static_cast<void*>(t), level, flags, r->left, r->top, r->right, r->bottom);
  } else {
    dxmt9DebugLog("texture_lock_rect begin texture=%p level=%u flags=0x%x rect=<full>",
                  static_cast<void*>(t), level, flags);
  }
  dxmt9::core::Rect* pr = r ? new dxmt9::core::Rect{r->left,r->top,r->right,r->bottom} : nullptr;
  auto lr = t->obj->lockRect(level, pr, flags);
  delete pr;
  out->pitch = static_cast<int32_t>(lr.pitch);
  out->bits  = lr.data;
  if (lr.data && !pointerFits32Bit(lr.data)) {
    const auto& desc = t->obj->desc();
    const uint32_t levelWidth = std::max(1u, desc.width >> std::min(level, 31u));
    const uint32_t levelHeight = std::max(1u, desc.height >> std::min(level, 31u));
    const uint32_t bpp = dxmt9::core::bytesPerPixel(desc.format);
    const int32_t left = r ? std::clamp(r->left, 0, static_cast<int32_t>(levelWidth)) : 0;
    const int32_t top = r ? std::clamp(r->top, 0, static_cast<int32_t>(levelHeight)) : 0;
    const int32_t right = r ? std::clamp(r->right, left, static_cast<int32_t>(levelWidth))
                            : static_cast<int32_t>(levelWidth);
    const int32_t bottom = r ? std::clamp(r->bottom, top, static_cast<int32_t>(levelHeight))
                             : static_cast<int32_t>(levelHeight);
    const uint32_t rowBytes = static_cast<uint32_t>(std::max<int32_t>(0, right - left)) * bpp;
    const uint32_t rows = static_cast<uint32_t>(std::max<int32_t>(0, bottom - top));
    const size_t shadowBytes = static_cast<size_t>(rowBytes) * rows;
    auto& shadow = t->wow64Locks[level];
    if (rowBytes == 0 || rows == 0) {
      dxmt9DebugLog("texture_lock_rect shadow alloc failed texture=%p level=%u nativeBits=%p rowBytes=%u rows=%u",
                    static_cast<void*>(t), level, lr.data, rowBytes, rows);
      out->pitch = 0;
      out->bits = nullptr;
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
    if (!shadow.shadow || shadow.shadow.size < shadowBytes) {
      releaseShadowLock(shadow);
      shadow.shadow = allocateLow4GB(shadowBytes);
    }
    if (!shadow.shadow) {
      dxmt9DebugLog("texture_lock_rect shadow alloc failed texture=%p level=%u nativeBits=%p rowBytes=%u rows=%u",
                    static_cast<void*>(t), level, lr.data, rowBytes, rows);
      out->pitch = 0;
      out->bits = nullptr;
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
    shadow.nativePtr = lr.data;
    shadow.nativePitch = lr.pitch;
    shadow.rowBytes = rowBytes;
    shadow.rows = rows;
    auto* dst = static_cast<uint8_t*>(shadow.shadow.ptr);
    auto* src = static_cast<const uint8_t*>(shadow.nativePtr);
    for (uint32_t row = 0; row < rows; ++row) {
      std::memcpy(dst + static_cast<size_t>(row) * rowBytes,
                  src + static_cast<size_t>(row) * shadow.nativePitch,
                  rowBytes);
    }
    out->pitch = static_cast<int32_t>(rowBytes);
    out->bits = shadow.shadow.ptr;
    dxmt9DebugLog("texture_lock_rect shadow texture=%p level=%u nativeBits=%p shadowBits=%p rowBytes=%u rows=%u",
                  static_cast<void*>(t), level, shadow.nativePtr, out->bits, rowBytes, rows);
  }
  dxmt9DebugLog("texture_lock_rect ok texture=%p level=%u pitch=%d bits=%p",
                static_cast<void*>(t), level, out->pitch, out->bits);
  return dxmt9::core::D3D_OK;
}
extern "C" int32_t dxmt9c_texture_unlock_rect(D9CTexture* t, uint32_t level) {
  if (auto it = t->wow64Locks.find(level); it != t->wow64Locks.end()) {
    auto& shadow = it->second;
    auto* dst = static_cast<uint8_t*>(shadow.nativePtr);
    auto* src = static_cast<const uint8_t*>(shadow.shadow.ptr);
    for (uint32_t row = 0; row < shadow.rows; ++row) {
      std::memcpy(dst + static_cast<size_t>(row) * shadow.nativePitch,
                  src + static_cast<size_t>(row) * shadow.rowBytes,
                  shadow.rowBytes);
    }
    dxmt9DebugLog("texture_unlock_rect shadow texture=%p level=%u nativeBits=%p shadowBits=%p rowBytes=%u rows=%u",
                  static_cast<void*>(t), level, shadow.nativePtr, shadow.shadow.ptr,
                  shadow.rowBytes, shadow.rows);
    releaseShadowLock(shadow);
    t->wow64Locks.erase(it);
  }
  dxmt9DebugLog("texture_unlock_rect texture=%p level=%u", static_cast<void*>(t), level);
  t->obj->unlockRect(level);
  return dxmt9::core::D3D_OK;
}
extern "C" D9CSurface* dxmt9c_texture_get_surface_level(D9CTexture* t, uint32_t level) {
  auto surf = t->obj->surfaceLevel(level);
  if (!surf) return nullptr;
  auto* wrap = new D9CSurface{surf};
  wrap->ownerTex = t;
  t->refs.fetch_add(1); /* keep texture alive while surface is alive */
  return wrap;
}
extern "C" uint32_t dxmt9c_texture_get_level_count(D9CTexture* t) {
  return t->obj->levelCount();
}
extern "C" int32_t dxmt9c_texture_get_level_desc(D9CTexture* t, uint32_t level,
                                                   D9CSurfaceDesc* out) {
  if (!out) return dxmt9::core::D3DERR_INVALIDCALL;
  auto& d = t->obj->desc();
  std::memset(out, 0, sizeof(*out));
  const uint32_t shift = std::min<uint32_t>(level, 31);
  out->format = fmtToD3D(d.format);
  out->resourceType = textureTypeToResourceType(d.type);
  out->usage = usageToD3D(d.usage);
  out->pool = poolToD3D(d.pool);
  out->multiSampleType = 0;
  out->multiSampleQuality = 0;
  out->width  = std::max(1u, d.width >> shift);
  out->height = std::max(1u, d.height >> shift);
  dxmt9DebugLog("texture_get_level_desc texture=%p level=%u fmt=%u(%s) usage=0x%x pool=%u size=%ux%u",
                static_cast<void*>(t), level, out->format,
                dxmt9::core::formatName(d.format).c_str(), out->usage, out->pool,
                out->width, out->height);
  return dxmt9::core::D3D_OK;
}
extern "C" int32_t dxmt9c_texture_generate_mip_sublevels(D9CTexture* /*t*/) {
  return dxmt9::core::D3D_OK;
}

/* ── buffer ──────────────────────────────────────────────────────────────── */

extern "C" void dxmt9c_buffer_addref(D9CBuffer* b) {
  if (b) b->refs.fetch_add(1);
}
extern "C" uint32_t dxmt9c_buffer_release(D9CBuffer* b) {
  if (!b) return 0;
  uint32_t r = b->refs.fetch_sub(1) - 1;
  if (r == 0) {
    releaseShadowLock(b->wow64Lock);
    delete b;
  }
  return r;
}
extern "C" int32_t dxmt9c_buffer_lock(D9CBuffer* b, uint32_t offset, uint32_t size,
                                       void** data, uint32_t flags) {
  if (!data) return dxmt9::core::D3DERR_INVALIDCALL;
  dxmt9DebugLog("buffer_lock begin buffer=%p offset=%u size=%u flags=0x%x",
                static_cast<void*>(b), offset, size, flags);
  const uint32_t actualSize = size ? size : b->obj->desc().size;
  auto lr = b->obj->lock(offset, actualSize, flags);
  *data = lr.data;
  if (lr.data && !pointerFits32Bit(lr.data)) {
    if (!b->wow64Lock.shadow || b->wow64Lock.shadow.size < actualSize) {
      releaseShadowLock(b->wow64Lock);
      b->wow64Lock.shadow = allocateLow4GB(actualSize);
    }
    if (!b->wow64Lock.shadow) {
      dxmt9DebugLog("buffer_lock shadow alloc failed buffer=%p native=%p size=%u",
                    static_cast<void*>(b), lr.data, actualSize);
      *data = nullptr;
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
    b->wow64Lock.nativePtr = lr.data;
    b->wow64Lock.rowBytes = actualSize;
    b->wow64Lock.rows = 1;
    std::memcpy(b->wow64Lock.shadow.ptr, lr.data, actualSize);
    *data = b->wow64Lock.shadow.ptr;
    dxmt9DebugLog("buffer_lock shadow buffer=%p native=%p shadow=%p size=%u",
                  static_cast<void*>(b), lr.data, *data, actualSize);
  }
  dxmt9DebugLog("buffer_lock ok buffer=%p data=%p", static_cast<void*>(b), *data);
  return dxmt9::core::D3D_OK;
}
extern "C" int32_t dxmt9c_buffer_unlock(D9CBuffer* b) {
  if (b->wow64Lock.shadow) {
    std::memcpy(b->wow64Lock.nativePtr, b->wow64Lock.shadow.ptr, b->wow64Lock.rowBytes);
    dxmt9DebugLog("buffer_unlock shadow buffer=%p native=%p shadow=%p size=%u",
                  static_cast<void*>(b), b->wow64Lock.nativePtr, b->wow64Lock.shadow.ptr,
                  b->wow64Lock.rowBytes);
    releaseShadowLock(b->wow64Lock);
  }
  b->obj->unlock();
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_buffer_get_desc(D9CBuffer* b, D9CBufferDesc* out) {
  if (!b || !out) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  *out = b->desc;
  return dxmt9::core::D3D_OK;
}

/* ── surface ─────────────────────────────────────────────────────────────── */

extern "C" void dxmt9c_surface_addref(D9CSurface* s) {
  if (s) s->refs.fetch_add(1);
}
extern "C" uint32_t dxmt9c_surface_release(D9CSurface* s) {
  if (!s) return 0;
  uint32_t r = s->refs.fetch_sub(1) - 1;
  if (r == 0) {
    releaseShadowLock(s->wow64Lock);
    if (s->ownerTex) dxmt9c_texture_release(s->ownerTex);
    delete s;
  }
  return r;
}
extern "C" int32_t dxmt9c_surface_lock_rect(D9CSurface* s, D9CLockedRect* out,
                                             const D9CRect* r, uint32_t flags) {
  if (!out) return dxmt9::core::D3DERR_INVALIDCALL;
  dxmt9::core::Rect* pr = r ? new dxmt9::core::Rect{r->left,r->top,r->right,r->bottom} : nullptr;
  auto lr = s->obj->lockRect(pr, flags);
  delete pr;
  out->pitch = static_cast<int32_t>(lr.pitch);
  out->bits  = lr.data;
  if (lr.data && !pointerFits32Bit(lr.data)) {
    const auto& desc = s->obj->desc();
    const int32_t top = r ? r->top : 0;
    const int32_t bottom = r ? r->bottom : static_cast<int32_t>(desc.height);
    const uint32_t rows = bottom > top ? static_cast<uint32_t>(bottom - top) : 0u;
    const uint32_t rowBytes = static_cast<uint32_t>(std::abs(out->pitch));
    const size_t bytes = static_cast<size_t>(rows) * static_cast<size_t>(rowBytes);
    if (bytes != 0) {
      if (!s->wow64Lock.shadow || s->wow64Lock.shadow.size < bytes) {
        releaseShadowLock(s->wow64Lock);
        s->wow64Lock.shadow = allocateLow4GB(bytes);
      }
      if (!s->wow64Lock.shadow) {
        out->bits = nullptr;
        return dxmt9::core::D3DERR_INVALIDCALL;
      }
      s->wow64Lock.nativePtr = lr.data;
      s->wow64Lock.nativePitch = rowBytes;
      s->wow64Lock.rowBytes = rowBytes;
      s->wow64Lock.rows = rows;
      std::memcpy(s->wow64Lock.shadow.ptr, lr.data, bytes);
      out->bits = s->wow64Lock.shadow.ptr;
      dxmt9DebugLog("surface_lock_rect shadow surface=%p native=%p shadow=%p pitch=%u rows=%u bytes=%zu",
                    static_cast<void*>(s), lr.data, out->bits, rowBytes, rows, bytes);
    }
  }
  return dxmt9::core::D3D_OK;
}
extern "C" int32_t dxmt9c_surface_unlock_rect(D9CSurface* s) {
  if (s->wow64Lock.shadow) {
    const size_t bytes = static_cast<size_t>(s->wow64Lock.rowBytes) *
                         static_cast<size_t>(s->wow64Lock.rows);
    if (bytes != 0) {
      std::memcpy(s->wow64Lock.nativePtr, s->wow64Lock.shadow.ptr, bytes);
      dxmt9DebugLog("surface_unlock_rect shadow surface=%p native=%p shadow=%p bytes=%zu",
                    static_cast<void*>(s), s->wow64Lock.nativePtr, s->wow64Lock.shadow.ptr, bytes);
    }
    releaseShadowLock(s->wow64Lock);
  }
  s->obj->unlockRect();
  return dxmt9::core::D3D_OK;
}
extern "C" int32_t dxmt9c_surface_get_desc(D9CSurface* s, D9CSurfaceDesc* out) {
  if (!out) return dxmt9::core::D3DERR_INVALIDCALL;
  auto& d = s->obj->desc();
  std::memset(out, 0, sizeof(*out));
  out->format = fmtToD3D(d.format);
  out->resourceType = 1; /* D3DRTYPE_SURFACE */
  out->usage = usageToD3D(d.usage);
  if (d.renderTarget) out->usage |= 0x00000001u;  /* D3DUSAGE_RENDERTARGET */
  if (d.depthStencil) out->usage |= 0x00000002u;  /* D3DUSAGE_DEPTHSTENCIL */
  out->pool = poolToD3D(d.pool);
  out->multiSampleType = msTypeToD3D(d.multiSampleType);
  out->multiSampleQuality = d.multiSampleType == dxmt9::core::MultiSampleType::None ? 0u : 1u;
  out->width  = d.width;
  out->height = d.height;
  return dxmt9::core::D3D_OK;
}
extern "C" D9CTexture* dxmt9c_surface_get_container_texture(D9CSurface* s) {
  return s->ownerTex;
}

/* ── shader ──────────────────────────────────────────────────────────────── */

extern "C" void dxmt9c_shader_addref(D9CShader* s) {
  if (s) s->refs.fetch_add(1);
}
extern "C" uint32_t dxmt9c_shader_release(D9CShader* s) {
  dxmt9DebugLog("shader_release begin shader=%p", static_cast<void*>(s));
  if (!s) return 0;
  uint32_t r = s->refs.fetch_sub(1) - 1;
  dxmt9DebugLog("shader_release shader=%p refs=%u dwords=%zu",
                static_cast<void*>(s), r, s->bytecodeWords.size());
  if (r == 0) delete s;
  return r;
}
extern "C" int32_t dxmt9c_shader_get_bytecode(D9CShader* s, void* data, uint32_t* size) {
  dxmt9DebugLog("shader_get_bytecode begin shader=%p data=%p size_ptr=%p",
                static_cast<void*>(s), data, static_cast<void*>(size));
  if (!s) {
    dxmt9DebugLog("shader_get_bytecode failed: null shader");
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  uint32_t bytes = static_cast<uint32_t>(s->bytecodeWords.size() * 4);
  if (!data) {
    dxmt9DebugLog("shader_get_bytecode query shader=%p bytes=%u", static_cast<void*>(s), bytes);
    if (size) *size = bytes;
    return dxmt9::core::D3D_OK;
  }
  if (size && *size < bytes) {
    dxmt9DebugLog("shader_get_bytecode too-small shader=%p provided=%u required=%u",
                  static_cast<void*>(s), *size, bytes);
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  dxmt9DebugLog("shader_get_bytecode copy shader=%p dst=%p bytes=%u",
                static_cast<void*>(s), data, bytes);
  std::memcpy(data, s->bytecodeWords.data(), bytes);
  if (size) *size = bytes;
  return dxmt9::core::D3D_OK;
}

/* ── vertex declaration ──────────────────────────────────────────────────── */

extern "C" void dxmt9c_vdecl_addref(D9CVertexDecl* v) {
  if (v) v->refs.fetch_add(1);
}
extern "C" uint32_t dxmt9c_vdecl_release(D9CVertexDecl* v) {
  if (!v) return 0;
  uint32_t r = v->refs.fetch_sub(1) - 1;
  if (r == 0) delete v;
  return r;
}
extern "C" int32_t dxmt9c_vdecl_get_declaration(D9CVertexDecl* v, D9CVertexElement* out,
                                                   uint32_t* count) {
  uint32_t n = static_cast<uint32_t>(v->raw.size());
  if (!out) { if (count) *count = n; return dxmt9::core::D3D_OK; }
  std::memcpy(out, v->raw.data(), n * sizeof(D9CVertexElement));
  if (count) *count = n;
  return dxmt9::core::D3D_OK;
}

/* ── query ───────────────────────────────────────────────────────────────── */

extern "C" void dxmt9c_query_addref(D9CQuery* q) {
  if (q) q->refs.fetch_add(1);
}
extern "C" uint32_t dxmt9c_query_release(D9CQuery* q) {
  if (!q) return 0;
  uint32_t r = q->refs.fetch_sub(1) - 1;
  if (r == 0) delete q;
  return r;
}
extern "C" int32_t dxmt9c_query_issue(D9CQuery* q, uint32_t flags) {
  return q->device->iface->IssueQuery(q->obj, (flags & 2) != 0);
}
extern "C" int32_t dxmt9c_query_get_data(D9CQuery* q, void* data, uint32_t size,
                                          uint32_t flags) {
  return q->device->iface->GetQueryData(q->obj, data, size, flags);
}
extern "C" uint32_t dxmt9c_query_get_data_size(D9CQuery* q) {
  switch (q->obj->type()) {
    case dxmt9::core::QueryType::Occlusion:  return 8;
    case dxmt9::core::QueryType::Timestamp:  return 8;
    case dxmt9::core::QueryType::TimestampFreq: return 8;
    default: return 0;
  }
}
extern "C" uint32_t dxmt9c_query_get_type(D9CQuery* q) {
  switch (q->obj->type()) {
    case dxmt9::core::QueryType::Occlusion:        return 8;
    case dxmt9::core::QueryType::Timestamp:        return 9;
    case dxmt9::core::QueryType::TimestampDisjoint: return 10;
    case dxmt9::core::QueryType::TimestampFreq:    return 11;
    default:                                        return 7; /* EVENT */
  }
}

/* ── state block ─────────────────────────────────────────────────────────── */

extern "C" void dxmt9c_stateblock_addref(D9CStateBlock* s) {
  if (s) s->refs.fetch_add(1);
}
extern "C" uint32_t dxmt9c_stateblock_release(D9CStateBlock* s) {
  if (!s) return 0;
  uint32_t r = s->refs.fetch_sub(1) - 1;
  if (r == 0) delete s;
  return r;
}
extern "C" int32_t dxmt9c_stateblock_capture(D9CStateBlock* s) {
  s->obj->capture(s->device->dev().state());
  return dxmt9::core::D3D_OK;
}
extern "C" int32_t dxmt9c_stateblock_apply(D9CStateBlock* s) {
  s->obj->apply(s->device->dev());
  const auto& state = s->device->dev().state();
  const auto rsValue = [&](uint32_t key) -> uint32_t {
    const auto it = state.renderStates.find(key);
    return it != state.renderStates.end() ? it->second : 0u;
  };
  dxmt9DebugLog("stateblock_apply device=%p alphaBlend=%u srcBlend=%u dstBlend=%u alphaTest=%u alphaFunc=%u alphaRef=%u lighting=%u colorOp0=%u alphaOp0=%u",
                static_cast<void*>(s->device),
                rsValue(dxmt9::core::RS_ALPHABLEND_ENABLE),
                rsValue(dxmt9::core::RS_SRC_BLEND),
                rsValue(dxmt9::core::RS_DEST_BLEND),
                rsValue(dxmt9::core::RS_ALPHA_TEST_ENABLE),
                rsValue(dxmt9::core::RS_ALPHA_FUNC),
                rsValue(dxmt9::core::RS_ALPHA_REF),
                rsValue(dxmt9::core::RS_LIGHTING),
                state.textureStageStates[0].contains(dxmt9::core::TSS_COLOR_OP)
                    ? state.textureStageStates[0].at(dxmt9::core::TSS_COLOR_OP)
                    : 0u,
                state.textureStageStates[0].contains(dxmt9::core::TSS_ALPHA_OP)
                    ? state.textureStageStates[0].at(dxmt9::core::TSS_ALPHA_OP)
                    : 0u);
  return dxmt9::core::D3D_OK;
}
