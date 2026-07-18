#include "device_c_chunk_v2_schema.hpp"

#include <cstdlib>
#include <iostream>

int main() {
  static_assert(D9C_COMMAND_CHUNK_VERSION == 1u);
  static_assert(D9C_COMMAND_CHUNK_VERSION_V2 == 2u);
  std::cout << "chunk_record_v1_v2_equivalence_spec scaffold passed\n";
  return EXIT_SUCCESS;
}
