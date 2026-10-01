#include "device_c_render_tape_query_checkpoint.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>

using namespace dxmt9;
using namespace dxmt9::d3d9;

namespace {
void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void cycleBinding() {
  RenderTapeQueryCycleState cycle{};
  std::uint64_t epoch = 0u;
  core::QueryPollSnapshot snapshot{.identity = 17u,
      .type = core::QueryType::Event, .issueOrdinal = 1u,
      .issueDeviceEpoch = 3u};
  check(renderTapeObserveEventQueryCheckpointIssue(cycle, epoch, 1u, snapshot),
        "first END binds independent cycle/ordinal");
  check(cycle.issueCycle == 1u && cycle.issueOrdinal == 1u && epoch == 3u,
        "END-only mapping");
  const auto beforeDuplicate = cycle;
  check(!renderTapeObserveEventQueryCheckpointIssue(cycle, epoch, 1u, snapshot) &&
        cycle.issueCycle == beforeDuplicate.issueCycle &&
        cycle.issueOrdinal == beforeDuplicate.issueOrdinal,
        "duplicate snapshot observation cannot count another Issue");
  snapshot.issueOrdinal = 2u;
  snapshot.active = true;
  check(renderTapeObserveEventQueryCheckpointIssue(cycle, epoch, 2u, snapshot),
        "BEGIN is a real admitted Issue");
  snapshot.issueOrdinal = 3u;
  snapshot.active = false;
  check(renderTapeObserveEventQueryCheckpointIssue(cycle, epoch, 1u, snapshot) &&
        cycle.issueCycle == 2u && cycle.issueOrdinal == 3u,
        "BEGIN/END closes one cycle but two absolute Issues");
  snapshot.issueOrdinal = 4u;
  snapshot.issueDeviceEpoch = 4u;
  check(renderTapeObserveEventQueryCheckpointIssue(cycle, epoch, 1u, snapshot) &&
        epoch == 4u && cycle.issueCycle == 3u,
        "post-reset successful Issue keeps absolute counters and new epoch");
  snapshot.failed = true;
  check(!renderTapeObserveEventQueryCheckpointIssue(cycle, epoch, 1u, snapshot),
        "failed execution establishes no mapping");
  snapshot.failed = false;
  cycle.issueOrdinal = std::numeric_limits<std::uint64_t>::max();
  check(!renderTapeObserveEventQueryCheckpointIssue(cycle, epoch, 1u, snapshot),
        "ordinal overflow fails closed");
}

void authorityTruthTable() {
  const RenderTapeEventQueryCheckpointAuthority accepted{
      .coreQueryIdentity = 7u, .issueCycle = 589u, .issueOrdinal = 591u,
      .deviceEpoch = 3u, .rawOrdinal = 991u, .queue = 41u,
      .sequence = 0u, .initializer = 0u, .mappingValid = true};
  std::uint64_t checked = 0u;
  for (std::uint32_t flags = 0u; flags < 64u; ++flags) {
    for (std::uint32_t wrong = 0u; wrong < 8u; ++wrong) {
      auto authority = accepted;
      const bool ready = (flags & 1u) == 0u;
      authority.active = (flags & 2u) != 0u;
      authority.failed = (flags & 4u) != 0u;
      authority.cancelled = (flags & 8u) != 0u;
      authority.mappingValid = (flags & 16u) == 0u;
      if (flags & 32u) authority.queue = 0u;
      if (wrong == 1u) ++authority.issueCycle;
      if (wrong == 2u) ++authority.issueOrdinal;
      if (wrong == 3u) ++authority.rawOrdinal;
      if (wrong == 4u) authority.coreQueryIdentity = 0u;
      if (wrong == 5u) authority.deviceEpoch = 0u;
      if (wrong == 6u) authority.issueCycle = 0u;
      if (wrong == 7u) authority.issueOrdinal = 0u;
      const auto admitted = renderTapeEventQueryCheckpointAuthorityAdmitted(
          589u, 591u, authority, 991u, ready);
      check(admitted == (flags == 0u && wrong == 0u),
            "only exact current closed completed-replay mapping is admitted");
      ++checked;
    }
  }
  check(renderTapeEventQueryCheckpointAuthorityAdmitted(
      589u, 591u, accepted, 991u, true), "actual empty published END is valid");
  auto replacement = accepted;
  ++replacement.issueOrdinal;
  check(!(accepted == replacement), "reissue invalidates final immutable tuple");
  replacement = accepted;
  ++replacement.deviceEpoch;
  check(!(accepted == replacement), "epoch invalidates final immutable tuple");
  replacement = accepted;
  ++replacement.sequence;
  check(!(accepted == replacement), "publication substitution invalidates tuple");
  std::cout << "EVENT checkpoint authority: " << checked
            << " bounded states; stale cycle/ordinal/raw/identity/epoch, active, "
               "failure/cancellation, duplicate observation and empty END pinned\n";
}
} // namespace

int main() {
  try { cycleBinding(); authorityTruthTable(); return 0; }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
