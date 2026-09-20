// R-CORE-8.2(b): exhaustive bounded CB partitions, independent pass semantics.
#include "../../../src/dxmt9/render/query_scope_cb_policy.hpp"
#include "dxmt9/query_occlusion.hpp"
#include <array>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool value) {
  if (!value) throw std::runtime_error("query scope CB refinement");
}
}

int main() {
  using namespace dxmt9;
  try {
    for (bool scoped : {false, true}) {
      for (bool enabled : {false, true})
        check(render::queryScopeAllowsSession(scoped, enabled) ==
              (!scoped || enabled));
      for (bool deferred : {false, true})
        check(render::queryScopeClosesSourceEncoders(scoped, deferred) ==
              (scoped && deferred));
    }
    // Four sources, scopes {}, A, B, AB, every partition into consecutive
    // CBs, and every per-CB success/failure choice. Pass contribution values
    // distinguish source attribution. Failures are mapped back to the same
    // sources in the independent reference (not assumed identical driver faults).
    unsigned traces = 0;
    for (unsigned scopes = 0; scopes < 256; ++scopes)
      for (unsigned cuts = 0; cuts < 8; ++cuts)
        for (unsigned failed = 0; failed < 16; ++failed) {
          std::array<unsigned, 2> reference{}, composed{};
          std::array<bool, 2> referenceFailed{}, composedFailed{};
          std::array<query::OcclusionResult, 2> results{
              query::OcclusionResult(1, 1, 1), query::OcclusionResult(2, 1, 1)};
          unsigned cb = 0;
          for (unsigned source = 0; source < 4; ++source) {
            if (source && (cuts & (1u << (source - 1)))) ++cb;
            const unsigned scope = (scopes >> (source * 2)) & 3;
            const bool success = !(failed & (1u << cb));
            check(render::queryScopeAllowsSession(scope != 0, true));
            if (scope) check(render::queryScopeClosesSourceEncoders(true, true));
            for (unsigned q = 0; q < 2; ++q) if (scope & (1u << q)) {
              reference[q] += success ? (source + 1) * 17 : 0;
              referenceFailed[q] = referenceFailed[q] || !success;
              results[q].pending.fetch_add(1);
              check(!results[q].readableAfterFence());
            }
          }
          cb = 0;
          for (unsigned source = 0; source < 4; ++source) {
            if (source && (cuts & (1u << (source - 1)))) ++cb;
            const unsigned scope = (scopes >> (source * 2)) & 3;
            for (unsigned q = 0; q < 2; ++q) if (scope & (1u << q)) {
              if (failed & (1u << cb)) results[q].failed.store(true);
              else results[q].add((source + 1) * 17);
              check(results[q].pending.fetch_sub(1) > 0);
            }
          }
          for (unsigned q = 0; q < 2; ++q) {
            composed[q] = results[q].samples.load();
            composedFailed[q] = results[q].failed.load();
            check(results[q].pending.load() == 0);
            check(results[q].readableAfterFence() == !referenceFailed[q]);
          }
          check(reference == composed && referenceFailed == composedFailed);
          ++traces;
        }
    std::cout << traces << " bounded scope/partition/failure traces passed\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
