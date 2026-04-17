#include "dynamic_symbol.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#if defined(__APPLE__) || defined(__linux__) || defined(__unix__)
#include <dlfcn.h>
#endif

namespace dxmt9::util {

void* resolveModuleSymbol(void* module, const char* primaryName, const char* alternateName) {
#if defined(_WIN32)
  const auto handle = reinterpret_cast<HMODULE>(module);
  if (!handle) {
    return nullptr;
  }
  if (primaryName) {
    if (void* symbol = reinterpret_cast<void*>(GetProcAddress(handle, primaryName))) {
      return symbol;
    }
  }
  if (alternateName) {
    if (void* symbol = reinterpret_cast<void*>(GetProcAddress(handle, alternateName))) {
      return symbol;
    }
  }
#elif defined(__APPLE__) || defined(__linux__) || defined(__unix__)
  if (!module) {
    return resolveDefaultSymbol(primaryName, alternateName);
  }
  if (primaryName) {
    if (void* symbol = dlsym(module, primaryName)) {
      return symbol;
    }
  }
  if (alternateName) {
    if (void* symbol = dlsym(module, alternateName)) {
      return symbol;
    }
  }
#else
  (void)module;
  (void)primaryName;
  (void)alternateName;
#endif
  return nullptr;
}

void* resolveDefaultSymbol(const char* primaryName, const char* alternateName) {
#if defined(__APPLE__) || defined(__linux__) || defined(__unix__)
  if (primaryName) {
    if (void* symbol = dlsym(RTLD_DEFAULT, primaryName)) {
      return symbol;
    }
  }
  if (alternateName) {
    if (void* symbol = dlsym(RTLD_DEFAULT, alternateName)) {
      return symbol;
    }
  }
#else
  (void)primaryName;
  (void)alternateName;
#endif
  return nullptr;
}

}  // namespace dxmt9::util
