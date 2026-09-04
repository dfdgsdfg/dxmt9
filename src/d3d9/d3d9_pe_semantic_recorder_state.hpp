#pragma once

#include "d3d9_pe_semantic_owner.hpp"

#include <cstddef>
#include <cstdint>
#include <new>

// Mandatory production final-wire ownership. Keep this state separate from
// optional diagnostics and capture owners: every negotiated PE device has one
// transaction, while observers remain nullable and cold.
using PeProductionSemanticBatchOwner = dxmt9::d3d9::pe::PeSemanticBatchOwner<
    256u, 256u, 1310720u, 1024u, 2048u>;

struct PeProductionSemanticRecorderState {
  PeProductionSemanticRecorderState() {
    if (!owner.constructionSucceeded()) {
      throw std::bad_alloc();
    }
  }

  PeProductionSemanticBatchOwner owner{};
  // Written once by armSemanticRecord and borrowed immutably by the append.
  // Destination-dependent draw spans are projected into destinationSparse;
  // non-draw families never copy the large SparseStateInput descriptor.
  dxmt9::d3d9::pe::PeSemanticRecordInput stagedInput{};
  dxmt9::d3d9::pe::SparseStateInput destinationSparse{};
  std::uint64_t sourceOrdinalHint = 0u;
  bool inputValid = false;
  // Stable size-hint cadence is deliberately separate from exact semantic
  // payload bytes. Changing cadence is a locality policy, not a promotion
  // side effect.
  std::size_t cadenceBytes = 0u;
};
