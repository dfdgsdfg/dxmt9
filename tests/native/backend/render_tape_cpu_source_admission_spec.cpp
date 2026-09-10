#include "render_tape_cpu_source_admission.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace dxmt9::test;

void check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

void exhaustiveAdmissionTable() {
  std::size_t cases = 0u;
  for (unsigned bits = 0u; bits < 64u; ++bits) {
    for (std::size_t inflight = 0u; inflight <= 4u; ++inflight) {
      for (std::size_t limit = 0u; limit <= 4u; ++limit) {
        const CpuTapeSourceAdmissionInputs input{
            .stopped = bool(bits & 1u),
            .writerOwned = bool(bits & 2u),
            .writerValid = bool(bits & 4u),
            .writerEmpty = bool(bits & 8u),
            .nextControlFree = bool(bits & 16u),
            .tapeCanReserve = bool(bits & 32u),
            .inflightCount = inflight,
            .inflightLimit = limit};
        // Independent conjunction of the queue's acquire and commit
        // preconditions plus the fixture's distinct-source empty-writer rule.
        const bool acquire = input.writerOwned
            ? input.writerValid && input.writerEmpty
            : input.nextControlFree && input.tapeCanReserve;
        const bool commit = !input.stopped && limit != 0u && inflight < limit;
        check((admitCpuTapeSource(input) == CpuTapeSourceAdmission::Admitted) ==
                  (acquire && commit),
              "admission must prove both acquisition and publication");
        ++cases;
      }
    }
  }
  check(cases == 1600u, "complete admission truth table");
}

void eachMissingDimensionRejects() {
  const CpuTapeSourceAdmissionInputs ready{
      .nextControlFree = true, .tapeCanReserve = true,
      .inflightCount = 1u, .inflightLimit = 2u};
  check(admitCpuTapeSource(ready) == CpuTapeSourceAdmission::Admitted,
        "one remaining source credit admits one source");
  auto bad = ready;
  bad.stopped = true;
  check(admitCpuTapeSource(bad) == CpuTapeSourceAdmission::Stopped, "stop");
  bad = ready;
  bad.inflightLimit = 0u;
  check(admitCpuTapeSource(bad) == CpuTapeSourceAdmission::InvalidLimit, "zero cap");
  bad = ready;
  bad.inflightCount = bad.inflightLimit;
  check(admitCpuTapeSource(bad) == CpuTapeSourceAdmission::InflightPressure,
        "publication equality is pressure, not available credit");
  bad = ready;
  bad.nextControlFree = false;
  check(admitCpuTapeSource(bad) == CpuTapeSourceAdmission::ControlBusy,
        "a different free control cannot replace the actual next index");
  bad = ready;
  bad.tapeCanReserve = false;
  check(admitCpuTapeSource(bad) == CpuTapeSourceAdmission::TapePressure,
        "control headroom does not imply Tape headroom");
  bad = ready;
  bad.writerOwned = true;
  check(admitCpuTapeSource(bad) == CpuTapeSourceAdmission::InvalidWriter,
        "owned writer must resolve to its qualified payload");
  bad.writerValid = true;
  check(admitCpuTapeSource(bad) == CpuTapeSourceAdmission::WritingNotEmpty,
        "existing commands cannot be silently merged into the next source");
  bad.writerEmpty = true;
  bad.nextControlFree = false;
  bad.tapeCanReserve = false;
  check(admitCpuTapeSource(bad) == CpuTapeSourceAdmission::Admitted,
        "an existing empty reservation does not require another reservation");

  bad = ready;
  bad.inflightLimit = std::numeric_limits<std::size_t>::max();
  bad.inflightCount = bad.inflightLimit;
  check(admitCpuTapeSource(bad) == CpuTapeSourceAdmission::InflightPressure,
        "comparison cannot wrap at maximum capacity");
  --bad.inflightCount;
  check(admitCpuTapeSource(bad) == CpuTapeSourceAdmission::Admitted,
        "maximum capacity with one credit remains well-defined");
}

void windowPolicyIsBoundedAndExplicit() {
  for (std::size_t pending = 0u; pending < 4u; ++pending) {
    for (std::size_t limit = 0u; limit < 4u; ++limit) {
      for (unsigned flags = 0u; flags < 8u; ++flags) {
        const bool work = flags & 1u, eligible = flags & 2u, pendingEligible = flags & 4u;
        const auto action = cpuTapeWindowAction(pending, limit, work, eligible, pendingEligible);
        const bool valid = limit >= 1u && limit <= 2u && pending <= limit;
        check((action != CpuTapeWindowAction::Reject) == valid,
              "invalid window and impossible pending counts reject");
        if (!valid) continue;
        const std::size_t released = action == CpuTapeWindowAction::DrainAll ? pending :
            action == CpuTapeWindowAction::ReleaseOldest ? 1u : 0u;
        check(released <= pending, "policy cannot acknowledge an absent source");
        if (limit == 2u && eligible && (pending == 0u || pendingEligible)) {
          check(pending - released < limit && released == std::size_t(pending == limit),
                "one eligible new source uses at most one explicit FIFO release");
        } else {
          check(released == (work ? pending : 0u),
                "unqualified work drains while destroy/non-work preserves owners");
        }
      }
    }
  }
}
}  // namespace

int main() {
  try {
    exhaustiveAdmissionTable();
    eachMissingDimensionRejects();
    windowPolicyIsBoundedAndExplicit();
    std::cout << "CPU source admission: 1600 valuations and capacity negatives passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "CPU source admission failed: " << error.what() << '\n';
    return 1;
  }
}
