#pragma once

#include "../winemetal/Metal.hpp"
#include "dxmt9_parallel_render_pass.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace dxmt9::encoders {

// Callbacks remain coordinator-owned. The adapter owns only the Metal parent
// and children; worker scheduling and native draw-state ownership stay behind
// emitChild/joinChild so the WMT lifetime surface does not retain source views.
struct ParallelPassMetalCallbacks {
  void* context = nullptr;
  bool (*beginPassActions)(void* context) noexcept = nullptr;
  bool (*replayLogicalCommands)(
      void* context,
      std::span<const ParallelPassChildPlan> children) noexcept = nullptr;
  bool (*emitChild)(void* context,
                    const ParallelPassChildPlan& child,
                    WMT::RenderCommandEncoder encoder) noexcept = nullptr;
  bool (*joinChild)(void* context, std::uint32_t ordinal) noexcept = nullptr;
  bool (*endPassActions)(void* context) noexcept = nullptr;
  bool (*publishSidecars)(void* context) noexcept = nullptr;
  bool (*publishCompletion)(void* context) noexcept = nullptr;
  void (*failStop)(void* context,
                   ParallelPassFailurePhase phase,
                   std::uint32_t child) noexcept = nullptr;
};

// Production WMT parent/child owner for executeParallelRenderPass(). CPU-only
// preparation occurs in prepareParent/createChild. The first Metal object is
// created from beginPassActions(), after the generic executor has crossed its
// irreversible effect boundary.
class ParallelPassMetalBackend {
public:
  ParallelPassMetalBackend(WMT::CommandBuffer commandBuffer,
                           const WMTRenderPassInfo& passInfo,
                           ParallelPassMetalCallbacks callbacks) noexcept;

  bool prepareParent() noexcept;
  bool createChild(const ParallelPassChildPlan& child) noexcept;
  void abandonPrepared() noexcept;
  bool beginPassActions() noexcept;
  bool replayLogicalCommands(
      std::span<const ParallelPassChildPlan> children) noexcept;
  bool emitChild(const ParallelPassChildPlan& child) noexcept;
  std::uint32_t emitChildren(
      std::span<const ParallelPassChildPlan> children) noexcept;
  bool endChild(std::uint32_t ordinal) noexcept;
  bool joinChild(std::uint32_t ordinal) noexcept;
  bool endPassActions() noexcept;
  bool endParent() noexcept;
  bool publishSidecars() noexcept;
  bool publishCompletion() noexcept;
  void failStop(ParallelPassFailurePhase phase, std::uint32_t child) noexcept;

private:
  bool createMetalEncoders() noexcept;
  void closeMetalEncoders() noexcept;

  WMT::CommandBuffer commandBuffer_{};
  WMTRenderPassInfo passInfo_{};
  ParallelPassMetalCallbacks callbacks_{};
  WMT::Reference<WMT::ParallelRenderCommandEncoder> parent_{};
  std::array<WMT::Reference<WMT::RenderCommandEncoder>,
             kParallelRenderPassChildCapacity> children_{};
  std::array<bool, kParallelRenderPassChildCapacity> childEnded_{};
  std::uint32_t preparedChildCount_ = 0;
  bool prepared_ = false;
  bool parentEnded_ = false;
};

}  // namespace dxmt9::encoders
