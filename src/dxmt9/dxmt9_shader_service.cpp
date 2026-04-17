#include "dxmt9_shader_service.hpp"

#include "util/util_hash.hpp"
#include "util/log/log.hpp"

#if defined(__APPLE__)
#include <execinfo.h>
#endif

#include <mutex>
#include <unordered_map>

namespace dxmt9::core {

namespace {

using u64 = shader_service::u64;

struct ShaderBlob {
  std::string source;
  u64 sourceHash = 0;
};

std::mutex gShaderBlobMutex;
std::unordered_map<u64, ShaderBlob> gShaderBlobRegistry;
u64 gNextShaderBlobHandle = 1;

u64 registerShaderBlob(std::string source) {
  std::lock_guard lock(gShaderBlobMutex);
  const u64 handle = gNextShaderBlobHandle++;
  const u64 sourceHash = dxmt9::util::fnv1a64(source);
  gShaderBlobRegistry.emplace(handle, ShaderBlob{.source = std::move(source), .sourceHash = sourceHash});
  dxmt9::util::logf(dxmt9::util::LogLevel::Debug, "dxmt9-shader-service",
                    "compiled shader handle=%llu source-hash=0x%llx",
                    static_cast<unsigned long long>(handle),
                    static_cast<unsigned long long>(sourceHash));
  return handle;
}

}  // namespace

namespace shader_service {

u64 compile(const WinemetalShaderCompileRequest& request) {
  try {
    return registerShaderBlob(dxmt9::core::makeShaderSourceFromRequest(request));
  } catch (const std::exception& e) {
    dxmt9::util::logf(dxmt9::util::LogLevel::Error, "dxmt9-shader-service",
                      "compile failed: %s kind=%u bytecode=%p size=%llu hash=0x%llx variant=%p sample=%u alpha=%u fog=%u",
                      e.what(),
                      static_cast<unsigned>(request.kind),
                      request.bytecode,
                      static_cast<unsigned long long>(request.bytecodeSize),
                      static_cast<unsigned long long>(request.bytecodeHash),
                      request.variantKey,
                      static_cast<unsigned>(request.sampleCount),
                      static_cast<unsigned>(request.alphaTestEnable),
                      static_cast<unsigned>(request.fogMode));
#if defined(__APPLE__)
    void* frames[16];
    const int frameCount = backtrace(frames, 16);
    if (frameCount > 0) {
      char** symbols = backtrace_symbols(frames, frameCount);
      if (symbols) {
        for (int i = 0; i < frameCount; ++i) {
          dxmt9::util::logf(dxmt9::util::LogLevel::Error, "dxmt9-shader-service",
                            "bt[%d]=%s", i, symbols[i]);
        }
        free(symbols);
      }
    }
#endif
    return 0;
  }
}

std::string source(u64 shaderHandle) {
  std::lock_guard lock(gShaderBlobMutex);
  if (auto it = gShaderBlobRegistry.find(shaderHandle); it != gShaderBlobRegistry.end()) {
    return it->second.source;
  }
  return {};
}

u64 sourceSize(u64 shaderHandle) {
  std::lock_guard lock(gShaderBlobMutex);
  if (auto it = gShaderBlobRegistry.find(shaderHandle); it != gShaderBlobRegistry.end()) {
    return static_cast<u64>(it->second.source.size());
  }
  return 0;
}

void destroy(u64 shaderHandle) {
  std::lock_guard lock(gShaderBlobMutex);
  gShaderBlobRegistry.erase(shaderHandle);
}

}  // namespace shader_service

}  // namespace dxmt9::core
