#pragma once

namespace dxmt9::util {

void* resolveModuleSymbol(void* module, const char* primaryName, const char* alternateName = nullptr);
void* resolveDefaultSymbol(const char* primaryName, const char* alternateName = nullptr);

template <typename T>
T resolveModuleSymbol(void* module, const char* primaryName, const char* alternateName = nullptr) {
  return reinterpret_cast<T>(resolveModuleSymbol(module, primaryName, alternateName));
}

template <typename T>
T resolveDefaultSymbol(const char* primaryName, const char* alternateName = nullptr) {
  return reinterpret_cast<T>(resolveDefaultSymbol(primaryName, alternateName));
}

}  // namespace dxmt9::util
