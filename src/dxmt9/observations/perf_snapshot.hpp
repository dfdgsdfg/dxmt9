#pragma once
#include "snapshot_sink.hpp"
#include "../dxmt9_perf_counters.hpp"

namespace dxmt9::observations {
static_assert(std::size(perf_fields) == sizeof(perf::CounterSnapshot) / sizeof(std::uint64_t) + 1,
              "Every frame snapshot field must have an explicit projection");
inline void project_frame(std::uint64_t frame, const perf::CounterSnapshot& previous,
                          const perf::CounterSnapshot& current) noexcept {
  SnapshotSink sink(Source::PerfFrame, 1, 0, frame);
  std::uint16_t index = 0;
  std::uint64_t regressions = 0;
#define X(member, name, unit, aggregation) \
  if (unit[0] == 'i') { sink.value(index++, current.member); } else { \
    const bool regressed = current.member < previous.member; \
    regressions += regressed; \
    sink.value(index++, regressed ? 0 : current.member - previous.member); }
#include "perf_fields.inc"
#undef X
  sink.value(index, regressions);
}
}  // namespace dxmt9::observations
