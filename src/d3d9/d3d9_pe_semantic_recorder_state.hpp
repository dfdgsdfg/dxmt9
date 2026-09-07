#pragma once

#include "d3d9_pe_semantic_owner.hpp"

#include <cstddef>
#include <cstdint>
#include <new>
#include <span>

enum class PeSemanticStagingFamily : std::uint8_t {
  None,
  Cold,
  Draw,
  Constant,
  SparsePlan,
};

// Hot producers overwrite only the fields their family can publish.  The
// inactive fields are cleared when the family frontier changes, so a reused
// generic input cannot retain direct refs or variable spans from the prior
// family.  Same-family calls overwrite every active field and need no 844-byte
// aggregate construction/copy.
inline void stagePeSemanticDrawInput(
    dxmt9::d3d9::pe::PeSemanticRecordInput& staged,
    PeSemanticStagingFamily& family, bool& valid,
    dxmt9::d3d9::pe::PeSemanticProducerKind producer,
    std::uint32_t recordType,
    const D9CCommandChunkWireDrawHeader& draw,
    const dxmt9::d3d9::pe::SparseStateInput& sparse) noexcept {
  valid = false;
  if (family != PeSemanticStagingFamily::Draw) {
    staged.setConst = {};
    staged.clear = {};
    staged.present = {};
    staged.stretchRect = {};
    staged.colorFill = {};
    staged.updateSurface = {};
    staged.queryIssue = {};
    staged.updateFlags = 0u;
    staged.reszFlags = 0u;
    staged.mipmapFlags = 0u;
    staged.surface0 = {};
    staged.surface1 = {};
    staged.texture0 = {};
    staged.texture1 = {};
    staged.buffer0 = {};
    staged.buffer1 = {};
    staged.shader0 = {};
    staged.shader1 = {};
    staged.declaration = {};
    staged.query = {};
    staged.constantBytes = {};
    staged.clearRects = {};
  }
  staged.producer = producer;
  staged.recordType = recordType;
  staged.recordFlags = D9C_COMMAND_CHUNK_RECORD_FLAG_NONE;
  staged.sourceOrdinal = 0u;
  staged.recordOrdinal = 0u;
  staged.draw = draw;
  staged.sparse = sparse;
  family = PeSemanticStagingFamily::Draw;
  valid = true;
}

inline void stagePeSemanticConstantInput(
    dxmt9::d3d9::pe::PeSemanticConstantRecordInput& staged,
    PeSemanticStagingFamily& family, bool& valid,
    dxmt9::d3d9::pe::PeSemanticProducerKind producer,
    std::uint32_t recordType, const D9CCommandChunkWireSetConst& setConst,
    std::span<const std::byte> constantBytes) noexcept {
  valid = false;
  staged.producer = producer;
  staged.recordType = recordType;
  staged.recordFlags = D9C_COMMAND_CHUNK_RECORD_FLAG_NONE;
  staged.sourceOrdinal = 0u;
  staged.recordOrdinal = 0u;
  staged.setConst = setConst;
  staged.constantBytes = constantBytes;
  family = PeSemanticStagingFamily::Constant;
  valid = true;
}

// Generic staging remains available to cold/oracle fixtures whose full input
// envelope is itself the authority. Production constant arming selects the
// narrow overload above.
inline void stagePeSemanticConstantInput(
    dxmt9::d3d9::pe::PeSemanticRecordInput& staged,
    PeSemanticStagingFamily& family, bool& valid,
    dxmt9::d3d9::pe::PeSemanticProducerKind producer,
    std::uint32_t recordType, const D9CCommandChunkWireSetConst& setConst,
    std::span<const std::byte> constantBytes) noexcept {
  valid = false;
  if (family != PeSemanticStagingFamily::Constant) {
    staged.draw = {};
    staged.clear = {};
    staged.present = {};
    staged.stretchRect = {};
    staged.colorFill = {};
    staged.updateSurface = {};
    staged.queryIssue = {};
    staged.updateFlags = 0u;
    staged.reszFlags = 0u;
    staged.mipmapFlags = 0u;
    staged.surface0 = {};
    staged.surface1 = {};
    staged.texture0 = {};
    staged.texture1 = {};
    staged.buffer0 = {};
    staged.buffer1 = {};
    staged.shader0 = {};
    staged.shader1 = {};
    staged.declaration = {};
    staged.query = {};
    staged.sparse = {};
    staged.clearRects = {};
  }
  staged.producer = producer;
  staged.recordType = recordType;
  staged.recordFlags = D9C_COMMAND_CHUNK_RECORD_FLAG_NONE;
  staged.sourceOrdinal = 0u;
  staged.recordOrdinal = 0u;
  staged.setConst = setConst;
  staged.constantBytes = constantBytes;
  family = PeSemanticStagingFamily::Constant;
  valid = true;
}

// Mandatory production final-wire ownership. Keep this state separate from
// optional diagnostics and capture owners: every negotiated PE device has one
// transaction, while observers remain nullable and cold.
using PeProductionSemanticBatchOwner = dxmt9::d3d9::pe::PeSemanticBatchOwner<
    256u, 256u, 1310720u, 1024u, 2048u,
    dxmt9::core::ProductionCopyMaterializationLedgerAccess>;

struct PeProductionSemanticRecorderState {
  PeProductionSemanticRecorderState() {
    if (!owner.constructionSucceeded()) {
      throw std::bad_alloc();
    }
  }

  PeProductionSemanticBatchOwner owner{};
  // Written once by an arm adapter and borrowed immutably by the append.
  // Draw/ApplyState and constants use family-specific adapters; destination-
  // dependent draw spans are projected into destinationSparse. Generic cold
  // controls may still assign the complete input value.
  dxmt9::d3d9::pe::PeSemanticRecordInput stagedInput{};
  dxmt9::d3d9::pe::PeSemanticConstantRecordInput stagedConstantInput{};
  dxmt9::d3d9::pe::SparseStatePlan stagedSparsePlan{};
  dxmt9::d3d9::pe::PeSemanticProducerKind stagedSparsePlanProducer =
      dxmt9::d3d9::pe::PeSemanticProducerKind::DrawPrimitive;
  dxmt9::d3d9::pe::SparseStateInput destinationSparse{};
  std::uint64_t sourceOrdinalHint = 0u;
  PeSemanticStagingFamily stagedFamily = PeSemanticStagingFamily::None;
  bool inputValid = false;
  // Stable size-hint cadence is deliberately separate from exact semantic
  // payload bytes. Changing cadence is a locality policy, not a promotion
  // side effect.
  std::size_t cadenceBytes = 0u;
};
