#include <cstdlib>
#include <cstring>
#include <iostream>

#include "dxmt9/dxmt9.h"

int main() {
  const char* version = dxmt9_version();
  if (version == nullptr || std::strlen(version) == 0) {
    std::cerr << "dxmt9_version() returned an empty string\n";
    return EXIT_FAILURE;
  }

  if (std::strcmp(version, DXMT9_VERSION) != 0) {
    std::cerr << "unexpected version string: " << version << "\n";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}

