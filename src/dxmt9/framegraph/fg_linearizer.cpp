// Frame Graph linearizer (Task B9, L1). See fg_linearizer.hpp for the contract.
//
// Part 1 — planLinearization() — is the device-free testable core.
// Part 2 — executeLinearization() — is the device-gated executor (wired by B12).
//
// design.md §6: for each PassNode in (optimized) order, open the pass, emit its
// draws in order, close the pass; Present / Blit passes are single ops. Dead
// passes (DCE, flags.dead) are skipped. Under default OptimizerOptions the plan
// reproduces the chunk's natural pass/draw OP ORDER (the parity baseline, §14 L1
// / R-BACK-39.1). NOTE: executeLinearization is NOT byte-identical to
// encoders::encodeChunk today — it re-derives clears via a per-pass scan, emits
// draws one param at a time (bypassing encodeChunk's draw-submission batching /
// binding-override path), and loadstore may pick a different first-pass color
// load action. Executor byte-exact fidelity is proven by the device conformance
// leg and is the deferred device-gated frontier; production encode stays on
// encoders::encodeChunk until then.

#include "fg_linearizer.hpp"

#include <cstddef>
#include <optional>

namespace dxmt9::framegraph {

// ---------------------------------------------------------------------------
// Part 1 — device-free plan.
// ---------------------------------------------------------------------------

namespace {

// A render pass needs draws OR a clear (LoadAction::Clear on any attachment) to
// be worth opening. A render pass that is purely a clear boundary (no draws,
// but a Clear load) linearizes as a standalone EmitClear (A3: a clear forces
// its own LoadActionClear pass). A render pass with neither draws nor a clear
// is a no-op and is skipped.
bool passHasClearLoad(const PassNode& pass) noexcept {
  for (const auto action : pass.load_store.color_load) {
    if (action == LoadAction::Clear) {
      return true;
    }
  }
  return pass.load_store.depth_load == LoadAction::Clear;
}

void emitRenderPass(const FrameGraph& graph, u32 pass_index,
                    LinearizationPlan& plan) {
  const PassNode& pass = graph.passes[pass_index];
  const bool hasDraws = pass.draws.count != 0;
  const bool hasClear = passHasClearLoad(pass);

  // Clear-only pass (no draws): standalone clear boundary. Still its own
  // LoadActionClear render pass at execute time, so model it as one EmitClear.
  if (!hasDraws && hasClear) {
    LinearOp clear{};
    clear.kind = LinearOpKind::EmitClear;
    clear.pass_index = pass_index;
    clear.pass_kind = PassKind::Render;
    clear.attachments = pass.targets;
    clear.load_store = pass.load_store;
    plan.ops.push_back(clear);
    return;
  }
  if (!hasDraws) {
    return;  // empty render pass; nothing to open.
  }

  LinearOp begin{};
  begin.kind = LinearOpKind::BeginPass;
  begin.pass_index = pass_index;
  begin.pass_kind = PassKind::Render;
  begin.attachments = pass.targets;
  begin.load_store = pass.load_store;
  // Carry the pass's FIRST DrawRef on the BeginPass op so the device-gated
  // executor can pull the pass's draw state (colorAttachments / depthStencil
  // drive beginRenderPass's render-pass descriptor) without re-walking. Safe:
  // a render pass reaching here always has >= 1 draw.
  begin.draw = graph.draws[pass.draws.first];
  plan.ops.push_back(begin);

  for (u32 i = 0; i < pass.draws.count; ++i) {
    const u32 draw_idx = pass.draws.first + i;
    LinearOp draw{};
    draw.kind = LinearOpKind::EmitDraw;
    draw.pass_index = pass_index;
    draw.draw = graph.draws[draw_idx];
    plan.ops.push_back(draw);
  }

  LinearOp end{};
  end.kind = LinearOpKind::EndPass;
  end.pass_index = pass_index;
  plan.ops.push_back(end);
}

void emitStandalonePass(u32 pass_index, PassKind kind, LinearizationPlan& plan) {
  LinearOp op{};
  op.kind = (kind == PassKind::Present) ? LinearOpKind::Present : LinearOpKind::Blit;
  op.pass_index = pass_index;
  op.pass_kind = kind;
  plan.ops.push_back(op);
}

}  // namespace

void planLinearization(const FrameGraph& graph, LinearizationPlan& plan) {
  plan.ops.clear();
  for (std::size_t p = 0; p < graph.passes.size(); ++p) {
    const PassNode& pass = graph.passes[p];
    if (pass.flags.dead) {
      continue;  // DCE-dropped (design.md §5.1); excluded from linear order.
    }
    const u32 pass_index = static_cast<u32>(p);
    switch (pass.kind) {
    case PassKind::Render:
      emitRenderPass(graph, pass_index, plan);
      break;
    case PassKind::Present:
      emitStandalonePass(pass_index, PassKind::Present, plan);
      break;
    case PassKind::Blit:
      emitStandalonePass(pass_index, PassKind::Blit, plan);
      break;
    case PassKind::Compute:
    case PassKind::Sync:
      // No L1 builder emits these; leave them out of the linear order until a
      // later layer defines their encode shape.
      break;
    }
  }
}

LinearizationPlan planLinearization(const FrameGraph& graph) {
  LinearizationPlan plan;
  planLinearization(graph, plan);
  return plan;
}

// ---------------------------------------------------------------------------
// Part 2 — device-gated executor (NOT exercised by the device-free unit test).
//
// Walks the SAME plan and drives the real encode through
// render::IExternalDrawEmitter + encoders::beginRenderPass / endEncoding. Pulls
// each draw's original state back out of the source ChunkSlot via the DrawRef
// (command_index selects the DrawRun; param_first/param_count select the
// draw-call ordinals). Needs a live WMT::Device / CommandBuffer, so it is
// compiled but never called by fg_linearizer_spec.cpp. B12 calls it from
// FrameGraphBackend::onChunkReady.
// ---------------------------------------------------------------------------

namespace {

// Recover the source ClearDesc for a render pass that begins with a clear. The
// builder folds a Clear command into a fresh pass that adopts the cleared
// targets, but does not stash the ClearDesc on the PassNode. At execute time we
// re-derive it by matching the slot's Clear records against the pass targets.
// Returns the matching ClearDesc, or nullopt if none matches (then the pass
// opens with a plain Load — the LoadStorePolicy already encodes Clear vs Load).
//
// B12 may replace this scan with a direct pass->clear-command index carried on
// the BeginPass op if profiling shows it matters; at L1 the clear count per
// chunk is small.
std::optional<core::ClearDesc> recoverClearDesc(const core::ChunkSlot& slot,
                                                const AttachmentSet& targets) {
  for (std::size_t i = 0; i < slot.commandHeaders.size(); ++i) {
    if (slot.commandHeaders[i].kind != core::MetalCommandKind::Clear) {
      continue;
    }
    const auto command = slot.commandAt(i);
    if (!command.clear) {
      continue;
    }
    const auto& clear = *command.clear;
    // Match on color0 / depth handle — the same identity the builder splits on.
    const bool colorMatches =
        clear.clearColor &&
        clear.colorAttachments[0].handle.value == targets.color[0].value;
    const bool depthMatches =
        (clear.clearDepth || clear.clearStencil) &&
        clear.depthStencil.handle.value == targets.depth.value;
    if (colorMatches || depthMatches) {
      return clear;
    }
  }
  return std::nullopt;
}

}  // namespace

void executeLinearization(const FrameGraph& graph,
                          const LinearizationPlan& plan,
                          const core::ChunkSlot& slot,
                          encoders::EncodeContext& ctx,
                          WMT::CommandBuffer& commandBuffer,
                          render::IExternalDrawEmitter& emitter) {
  (void)graph;  // plan is self-contained; graph kept for B12 cross-reference.
  const std::uint64_t seqId = slot.seqId;

  WMT::Reference<WMT::RenderCommandEncoder> activeEncoder;
  bool hasActiveEncoder = false;

  auto endActiveEncoder = [&]() {
    if (hasActiveEncoder) {
      // WMT::Reference<T> derives from T, so encoder methods use '.' directly.
      activeEncoder.endEncoding();
      hasActiveEncoder = false;
    }
  };

  for (const LinearOp& op : plan.ops) {
    switch (op.kind) {
    case LinearOpKind::BeginPass: {
      // Open the render pass. The plan's LoadStorePolicy already chose Clear vs
      // Load per attachment; recover the source ClearDesc only when a Clear
      // load is present (a LoadAction::Clear attachment makes beginRenderPass
      // open with Metal LoadActionClear — the A3 path).
      std::optional<core::ClearDesc> clear;
      bool wantsClear = (op.load_store.depth_load == LoadAction::Clear);
      for (const auto a : op.load_store.color_load) {
        wantsClear = wantsClear || (a == LoadAction::Clear);
      }
      if (wantsClear) {
        clear = recoverClearDesc(slot, op.attachments);
      }
      // Draw state for the pass open comes from the first draw of the pass; the
      // executor uses the DrawRun's FlatDrawStateView (its colorAttachments /
      // depthStencil drive beginRenderPass's render-pass descriptor, matching
      // encodeChunk).
      core::FlatDrawStateView drawState{};
      const auto firstCommand = slot.drawRunCommandAt(op.draw.command_index);
      drawState = firstCommand.drawState;
      activeEncoder =
          encoders::beginRenderPass(ctx, commandBuffer, drawState, clear);
      hasActiveEncoder = true;
      break;
    }
    case LinearOpKind::EmitDraw: {
      if (!hasActiveEncoder) {
        break;  // defensive: a draw with no open pass is a plan bug.
      }
      // Pull the original draw state out of the chunk slot via the DrawRef.
      const auto command = slot.drawRunCommandAt(op.draw.command_index);
      core::FlatDrawStateView drawState = command.drawState;
      const auto params = command.drawParams;
      const u32 begin = op.draw.param_first;
      const u32 end = begin + op.draw.param_count;
      core::DrawUniformPayload commandUniformScratch{};
      const core::DrawUniformPayload* commandUniformPayload =
          command.drawRunRecord
              ? core::drawRunUniformPayloadForHandle(
                    command, command.drawRunRecord->uniformHandle,
                    commandUniformScratch,
                    perf::DrawUniformPayloadMaterializeSite::FramegraphCommand)
              : command.drawUniformPayload;
      core::DrawUniformPayload uniformScratch{};
      for (u32 pi = begin; pi < end && pi < params.size(); ++pi) {
        const bool usesCommandUniform =
            command.drawRunRecord &&
            (!params[pi].uniformHandle.valid() ||
             params[pi].uniformHandle == command.drawRunRecord->uniformHandle);
        drawState.uniforms = usesCommandUniform
            ? commandUniformPayload
            : core::drawRunUniformPayloadForParam(
                  command, params[pi], uniformScratch,
                  perf::DrawUniformPayloadMaterializeSite::FramegraphParam);
        if (!drawState.uniforms) {
          continue;
        }
        emitter.emitDraw(ctx, commandBuffer, activeEncoder, drawState, seqId,
                         params[pi]);
      }
      break;
    }
    case LinearOpKind::EmitClear: {
      // Standalone clear boundary: end any open encoder (ClearBarrier split),
      // then emitClearWithinPass opens its own LoadActionClear render pass.
      endActiveEncoder();
      if (const auto clear = recoverClearDesc(slot, op.attachments)) {
        emitter.emitClearWithinPass(ctx, commandBuffer, *clear);
      }
      break;
    }
    case LinearOpKind::EndPass:
      endActiveEncoder();
      break;
    case LinearOpKind::Present:
      // Present is caller/presenter-owned; B12 forwards op.pass_index to the
      // presenter. Nothing to encode through the draw emitter here.
      endActiveEncoder();
      break;
    case LinearOpKind::Blit:
      // Blit / readback / copy is caller-owned (blit encoders); B12 forwards
      // op.pass_index to the blit path.
      endActiveEncoder();
      break;
    }
  }
  endActiveEncoder();
}

}  // namespace dxmt9::framegraph
