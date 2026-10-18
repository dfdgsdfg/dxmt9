#include "dxmt9_signposts.hpp"

namespace dxmt9::signposts {

os_log_t log() {
  // Lazy single-shot init. os_log_create returns a retained reference
  // owned by the runtime; we never release it (process lifetime).
  static os_log_t value = os_log_create("com.dxmt9.translator", "metal");
  return value;
}

}  // namespace dxmt9::signposts

#include "observations/diagnostic_sink.hpp"
namespace dxmt9::signposts {
void tracePoint(const char* name, std::uint64_t id, bool begin, std::uint64_t work) noexcept {
  if (!tracing::enabled()) return;
  observations::DiagnosticContext c; c.work = work;
  char value[192]{};
  std::snprintf(value, sizeof(value), "name=%s signpost_id=%llu phase=%s work=%llu clock=native-site-observed external_coverage=unknown",
      name, static_cast<unsigned long long>(id), begin ? "begin" : "end",
      static_cast<unsigned long long>(work));
  observations::report(observations::Source::Signpost, "instruments-signpost-v1", value, c);
}
} // namespace dxmt9::signposts
