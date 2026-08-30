#pragma once

// Compatibility include for the value-only production predicate. The observer
// owns storage and counters; `classifyComposition` intentionally does not
// produce a composition plan. `MutationOrderingPolicy` is the shared pure
// ordering authority used by the observer and its bounded TLA/native binding.

#include "dxmt9/mutation_composition_observer.hpp"
