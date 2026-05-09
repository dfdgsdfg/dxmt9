#include "dxmt9_signposts.hpp"

namespace dxmt9::signposts {

os_log_t log() {
  // Lazy single-shot init. os_log_create returns a retained reference
  // owned by the runtime; we never release it (process lifetime).
  static os_log_t value = os_log_create("com.dxmt9.translator", "metal");
  return value;
}

}  // namespace dxmt9::signposts
