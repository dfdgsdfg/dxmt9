#pragma once

#include "d3d9_pe_producer_views.hpp"
#include "d3d9_pe_semantic_tokens.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace dxmt9::d3d9::pe {

// Settles one accepted standalone Set*Constant* record.  The references are
// deliberately the narrow producer authorities: no owner, callback, or
// reentrancy state is part of this operation.
class PeSemanticConstantSettlement final {
 public:
  PeSemanticConstantSettlement(
      PeConstShadowBlock& constants, const PeHotStateShadow& pendingState,
      PendingDeltaTicket pendingTicket) noexcept
      : constants_(constants),
        pendingState_(pendingState),
        pendingTicket_(pendingTicket) {}

  bool settle(PeSemanticProducerKind producer,
              const D9CCommandChunkWireSetConst& setConst,
              std::span<const std::byte> constantBytes) noexcept {
    ConstShadow* shadow = nullptr;
    std::size_t elementBytes = 0u;
    switch (producer) {
      case PeSemanticProducerKind::VsFloatConstant:
        shadow = &constants_.vsConstF;
        elementBytes = sizeof(float) * 4u;
        break;
      case PeSemanticProducerKind::VsIntConstant:
        shadow = &constants_.vsConstI;
        elementBytes = sizeof(std::int32_t) * 4u;
        break;
      case PeSemanticProducerKind::VsBoolConstant:
        shadow = &constants_.vsConstB;
        elementBytes = sizeof(std::int32_t);
        break;
      case PeSemanticProducerKind::PsFloatConstant:
        shadow = &constants_.psConstF;
        elementBytes = sizeof(float) * 4u;
        break;
      case PeSemanticProducerKind::PsIntConstant:
        shadow = &constants_.psConstI;
        elementBytes = sizeof(std::int32_t) * 4u;
        break;
      case PeSemanticProducerKind::PsBoolConstant:
        shadow = &constants_.psConstB;
        elementBytes = sizeof(std::int32_t);
        break;
      default:
        return false;
    }

    const auto expected = static_cast<std::size_t>(setConst.registerCount) *
                          elementBytes;
    if (expected != constantBytes.size() || !shadow->dirty() ||
        shadow->dirtyStart != setConst.startRegister ||
        shadow->dirtyEnd - shadow->dirtyStart != setConst.registerCount) {
      return false;
    }

    const auto appendPlan = settleRecorderAppend({
        .phase = AppendSettlement::Prepared,
        .appendSucceeded = true,
    });
    if (!appendPlan.valid() || !appendPlan.consumeRepresentedPending() ||
        !appendPlan.recordDurable() ||
        (pendingTicket_.valid() &&
         !pendingState_.pendingTicketMatches(pendingTicket_))) {
      return false;
    }
    shadow->clear();
    return true;
  }

 private:
  PeConstShadowBlock& constants_;
  const PeHotStateShadow& pendingState_;
  PendingDeltaTicket pendingTicket_{};
};

}  // namespace dxmt9::d3d9::pe
