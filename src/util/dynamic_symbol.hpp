#pragma once

namespace dxmt9::util {

void* resolveDefaultSymbol(const char* primaryName, const char* alternateName = nullptr);

template <typename T>
T resolveDefaultSymbol(const char* primaryName, const char* alternateName = nullptr) {
  return reinterpret_cast<T>(resolveDefaultSymbol(primaryName, alternateName));
}

}  // namespace dxmt9::util
