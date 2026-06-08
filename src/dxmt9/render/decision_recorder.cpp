#include "decision_recorder.hpp"

#include <cstdlib>
#include <cstring>

namespace dxmt9::render {

namespace {

// envFlagSet semantics, identical to dxmt9_presenter.mm: a flag is "set" when
// the value is non-null, non-empty, and not the literal "0". Side-effect-free.
bool envFlagSet(const char* env) {
  return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
}

}  // namespace

bool resolveLogDivergence(const char* env) {
  return envFlagSet(env);
}

bool logDivergenceEnabledFromEnv() {
  // Read once at first use via the repo's static-const-lambda pattern (see
  // layerDisplaySyncEnabled / resolveAcquirePolicyFromEnv in
  // dxmt9_presenter.mm). Changing the env after dxmt9 has loaded has no effect.
  static const bool value =
      resolveLogDivergence(std::getenv("DXMT9_RENDERER_LOG_DIVERGENCE"));
  return value;
}

DecisionDivergence compareDecisions(
    const std::vector<DecisionRecord>& modern,
    const std::vector<DecisionRecord>& reference) {
  const std::size_t common =
      modern.size() < reference.size() ? modern.size() : reference.size();
  for (std::size_t i = 0; i < common; ++i) {
    if (modern[i] != reference[i]) {
      return DecisionDivergence{true, i};
    }
  }
  // Shared prefix matches: a length mismatch diverges at the first missing
  // record (== common); equal length and equal records do not diverge.
  if (modern.size() != reference.size()) {
    return DecisionDivergence{true, common};
  }
  return DecisionDivergence{false, common};
}

}  // namespace dxmt9::render
