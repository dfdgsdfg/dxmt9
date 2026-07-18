#include "device_c_chunk_v2_schema.hpp"

#include <cstdlib>
#include <iostream>

int main() {
  static_assert(dxmt9::d3d9::v2RecordSchemaComplete());
  std::cout << "resource_hazard_v2_spec scaffold passed\n";
  return EXIT_SUCCESS;
}
