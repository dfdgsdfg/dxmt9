#include "dynamic_symbol.hpp"

#if defined(__APPLE__) || defined(__linux__) || defined(__unix__)
#include <dlfcn.h>
#endif

namespace dxmt9::util {

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
