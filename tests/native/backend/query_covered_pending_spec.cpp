#include "dxmt9/query_fence.hpp"
#include "../../../src/dxmt9/dxmt9_query_policy.hpp"
#include <cassert>
#include <cstdlib>
#include <initializer_list>

int main(int argc, char**) {
  using namespace dxmt9::query;
  if (argc > 1 && coveredPendingEnabled()) std::abort();
  // Exhaustive bounded native binding: independent conservative observations,
  // stale/wrong identities, terminal and marker all use the production reducer.
  for (unsigned identity = 0; identity != 3; ++identity)
  for (unsigned sequence = 0; sequence != 4; ++sequence)
  for (unsigned upload = 0; upload != 3; ++upload)
  for (unsigned covered = 0; covered != 4; ++covered)
  for (unsigned done = 0; done != 4; ++done)
  for (unsigned success = 0; success != 3; ++success)
  for (bool terminal : {false, true})
  for (bool marker : {false, true}) {
    const Fence f{identity, sequence, upload};
    const bool expected = identity == 1 && sequence > 0 && sequence <= covered &&
        done < sequence && upload <= success && !terminal && !marker;
    if (coveredPending(f, 1, covered, done, success, terminal, marker) != expected)
      std::abort();
    if (expected && classify(f, {1, covered, done, false, success, success}) != Poll::Pending)
      std::abort();
  }
}
