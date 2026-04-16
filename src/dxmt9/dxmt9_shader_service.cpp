#include "dxmt9_shader_service.hpp"

#include "util/log/log.hpp"

#include <mutex>
#include <unordered_map>

namespace dxmt9::core {

namespace {

using u64 = shader_service::u64;

struct ShaderBlob {
  std::string source;
};

std::mutex gShaderBlobMutex;
std::unordered_map<u64, ShaderBlob> gShaderBlobRegistry;
u64 gNextShaderBlobHandle = 1;

u64 registerShaderBlob(std::string source) {
  std::lock_guard lock(gShaderBlobMutex);
  const u64 handle = gNextShaderBlobHandle++;
  gShaderBlobRegistry.emplace(handle, ShaderBlob{.source = std::move(source)});
  return handle;
}

}  // namespace

namespace shader_service {

u64 compile(const WinemetalShaderCompileRequest& request) {
  try {
    return registerShaderBlob(dxmt9::core::makeShaderSourceFromRequest(request));
  } catch (const std::exception& e) {
    dxmt9::util::logf(dxmt9::util::LogLevel::Error, "dxmt9-shader-service",
                      "compile failed: %s", e.what());
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
