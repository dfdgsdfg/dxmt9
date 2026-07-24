// Frame Graph builder (Task B2, L1). See fg_builder.hpp for scope/contract.
//
// spec.md §4.1 maps chunk-record kinds onto builder actions. The real input
// is the imported `core::ChunkSlot` SoA, so the mapping is read against
// `ChunkSlot::commandHeaders` (MetalCommandKind + payload index), not the
// PE-side wire enum (spec.md §2.1):
//
//   MetalCommandKind::DrawRun  -> append DrawRef(s) to the current Render pass;
//                                 start a new pass when the attachment set
//                                 differs (mirrors encodeChunk's AttachmentKey
//                                 split). Color attachments = Write, bound
//                                 textures = Read. Surface attachment writes
//                                 canonicalize to their owning texture hazard
//                                 identity when the caller supplies an alias
//                                 resolver.
//   MetalCommandKind::Clear    -> Clear access on the cleared attachments;
//                                 acts as a pass boundary (a fresh pass adopts
//                                 the cleared targets, like beginRenderPass'
//                                 pendingClear fold).
//   MetalCommandKind::Present  -> finalize current pass, emit PassKind::Present,
//                                 mark frame boundary.
//   SurfaceCopy / StretchRect / ColorFill / DepthResolve
//                              -> finalize current pass, emit PassKind::Blit
//                                 with src=Read / dst=Write access.
//   Readback                   -> finalize current pass, emit PassKind::Blit,
//                                 source read + readback_seen classifier bit
//                                 (DCE / memoryless cross-chunk safety gate).

#include "fg_builder.hpp"

#include <cstddef>
#include <utility>

namespace dxmt9::framegraph {

namespace {

// The attachment-set identity the encoder splits render passes on
// (dxmt9_draw_encoder.mm makeAttachmentKey): color0..kMaxRenderTargets + depth.
// Two draws share a render pass iff their AttachmentSet compares equal.
AttachmentSet attachmentSetFromHot(const core::FlatDrawStateRecord& hot) {
  AttachmentSet set{};
  u32 count = 0;
  for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
    const auto handle = hot.colorAttachments[i].handle;
    set.color[i] = TextureHandle{handle.value};
    if (handle.value != 0) {
      // color_count = highest bound slot + 1 (matches the 8-wide spec field;
      // unbound interior slots keep their zero handle).
      count = static_cast<u32>(i) + 1u;
    }
  }
  set.color_count = count;
  set.depth = TextureHandle{hot.depthStencil.handle.value};
  return set;
}

AttachmentSet attachmentSetFromClear(const core::ClearDesc& clear) {
  AttachmentSet set{};
  u32 count = 0;
  if (clear.clearColor) {
    for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
      const auto handle = clear.colorAttachments[i].handle;
      set.color[i] = TextureHandle{handle.value};
      if (handle.value != 0) {
        count = static_cast<u32>(i) + 1u;
      }
    }
  }
  set.color_count = count;
  if (clear.clearDepth || clear.clearStencil) {
    set.depth = TextureHandle{clear.depthStencil.handle.value};
  }
  return set;
}

// Per-build state machine. Owns the in-progress pass index so command handlers
// can append accesses / draws / edges against the current pass.
class Builder {
public:
  Builder(const core::ChunkSlot& slot, u64 frame_id,
          ResourceAliasResolver aliasResolver, FrameGraph& graph)
      : slot_(slot), aliasResolver_(aliasResolver), graph_(graph) {
    graph_.reset();
    graph_.frame_id = frame_id;
  }

  void run() {
    const std::size_t commandCount = slot_.commandHeaders.size();
    for (std::size_t i = 0; i < commandCount; ++i) {
      const auto& header = slot_.commandHeaders[i];
      switch (header.kind) {
      case core::MetalCommandKind::DrawRun:
        handleDrawRun(i);
        break;
      case core::MetalCommandKind::Clear:
        handleClear(i);
        break;
      case core::MetalCommandKind::Present:
        handlePresent(i);
        break;
      case core::MetalCommandKind::SurfaceCopy:
        handleSurfaceCopy(i);
        break;
      case core::MetalCommandKind::StretchRect:
        handleStretchRect(i);
        break;
      case core::MetalCommandKind::ColorFill:
        handleColorFill(i);
        break;
      case core::MetalCommandKind::DepthResolve:
        handleDepthResolve(i);
        break;
      case core::MetalCommandKind::Readback:
        handleReadback(i);
        break;
      }
    }
    finalizeCurrentPass();
  }

private:
  // --- Pass lifecycle -------------------------------------------------------

  // True while a Render pass is open and accepting draws.
  bool hasOpenRenderPass() const noexcept { return openRenderPass_; }

  // Open a fresh Render pass with the given attachment set. The draw range
  // starts empty and is grown as DrawRefs are appended.
  u32 openRenderPass(const AttachmentSet& targets, u64 state_profile) {
    PassNode pass{};
    pass.kind = PassKind::Render;
    pass.targets = targets;
    pass.draws = DrawRange{.first = static_cast<u32>(graph_.draws.size()),
                           .count = 0};
    pass.commands =
        CommandRange{.first = static_cast<u32>(graph_.commands.size()),
                     .count = 0};
    pass.state_profile = state_profile;
    graph_.passes.push_back(pass);
    currentPass_ = static_cast<u32>(graph_.passes.size() - 1u);
    openRenderPass_ = true;
    return currentPass_;
  }

  // Finalize whichever pass is current (render or a one-shot blit/present is
  // already self-contained). For a render pass this just clears the open flag;
  // lifetime first/last is computed lazily after the whole walk.
  void finalizeCurrentPass() { openRenderPass_ = false; }

  // Emit a self-contained, draw-less pass (Present / Blit). Any open render
  // pass is finalized first so submission order is preserved.
  u32 emitStandalonePass(PassKind kind, std::size_t commandIndex) {
    finalizeCurrentPass();
    PassNode pass{};
    pass.kind = kind;
    pass.draws = DrawRange{.first = static_cast<u32>(graph_.draws.size()),
                           .count = 0};
    pass.commands =
        CommandRange{.first = static_cast<u32>(graph_.commands.size()),
                     .count = 1};
    graph_.commands.push_back(CommandRef{
        .command_index = static_cast<u32>(commandIndex),
        .kind = slot_.commandHeaders[commandIndex].kind,
    });
    graph_.passes.push_back(pass);
    return static_cast<u32>(graph_.passes.size() - 1u);
  }

  // --- Resource bookkeeping -------------------------------------------------

  ResourceNode& resourceFor(ResourceHandle handle) {
    const std::size_t idx = findResourceIndex(graph_, handle);
    if (idx != graph_.resources.size()) {
      return graph_.resources[idx];
    }
    ResourceNode node{};
    node.handle = handle;
    node.first_use_pass = currentPass_;
    node.last_use_pass = currentPass_;
    graph_.resources.push_back(std::move(node));
    return graph_.resources.back();
  }

  void noteAccess(ResourceHandle handle, u32 pass_index, AccessKind kind,
                  AccessStage stage, bool resolve_alias = true) {
    if (handle.value == 0) {
      return;
    }
    if (resolve_alias) {
      handle = aliasResolver_(handle);
    }
    if (handle.value == 0) {
      return;
    }
    ResourceNode& node = resourceFor(handle);
    // Infer hazard edges against the STRICTLY-PRIOR access log (before this
    // access is appended). All edges point earlier_pass -> later_pass.
    addDependencyEdges(node, pass_index, kind, handle);
    recordAccess(node, pass_index, kind, stage);
    if (pass_index < node.first_use_pass) {
      node.first_use_pass = pass_index;
    }
    if (pass_index > node.last_use_pass) {
      node.last_use_pass = pass_index;
    }
  }

  static bool accessReads(AccessKind k) {
    return k == AccessKind::Read || k == AccessKind::ReadWrite;
  }
  static bool accessWrites(AccessKind k) {
    return k == AccessKind::Write || k == AccessKind::Clear ||
           k == AccessKind::ReadWrite;
  }

  // Insert a dependency edge `src -> dst` on `handle`. Self-edges (same pass)
  // are omitted and duplicate (src, dst, resource) edges are suppressed.
  void addEdgeOnce(u32 src, u32 dst, ResourceHandle handle) {
    if (src == dst) {
      return;
    }
    for (const auto& edge : graph_.edges) {
      if (edge.src_pass == src && edge.dst_pass == dst &&
          edge.resource == handle) {
        return;
      }
    }
    graph_.edges.push_back(
        Edge{.src_pass = src, .dst_pass = dst, .resource = handle});
  }

  // §4.2 hazard-edge inference. The edge set must capture true (RAW), anti
  // (WAR), and output (WAW) dependencies so the edge-consuming optimizer passes
  // (reorder topo-sort §5.6, passcoalesce relocation §5.4) preserve D3D9
  // ordering. `node.accesses` holds only strictly-prior accesses here.
  //   new Read  : RAW = most-recent prior Write -> this read.
  //   new Write : WAW = most-recent prior Write -> this write, and
  //               WAR = every prior Read after that write -> this write.
  // ReadWrite is both a read and a write; Clear counts as a write. Every edge
  // points earlier_pass -> later_pass (prior accesses have pass <= current).
  void addDependencyEdges(const ResourceNode& node, u32 pass, AccessKind kind,
                          ResourceHandle handle) {
    const bool is_read = accessReads(kind);
    const bool is_write = accessWrites(kind);

    // Position of the most-recent prior write in the chronological log.
    int last_write_idx = -1;
    for (std::size_t i = 0; i < node.accesses.size(); ++i) {
      if (accessWrites(static_cast<AccessKind>(node.accesses[i].access_kind))) {
        last_write_idx = static_cast<int>(i);
      }
    }

    if (is_read && last_write_idx >= 0) {
      addEdgeOnce(node.accesses[last_write_idx].pass_index, pass, handle);  // RAW
    }

    if (is_write) {
      if (last_write_idx >= 0) {
        addEdgeOnce(node.accesses[last_write_idx].pass_index, pass, handle);  // WAW
      }
      // WAR: every read after the most-recent prior write (or every read when
      // there is no prior write) -> this write.
      for (std::size_t i = static_cast<std::size_t>(last_write_idx + 1);
           i < node.accesses.size(); ++i) {
        if (accessReads(
                static_cast<AccessKind>(node.accesses[i].access_kind))) {
          addEdgeOnce(node.accesses[i].pass_index, pass, handle);  // WAR
        }
      }
    }
  }

  // --- Command handlers -----------------------------------------------------

  void handleDrawRun(std::size_t commandIndex) {
    const auto command = slot_.drawRunCommandAt(commandIndex);
    if (!command.drawState.hot) {
      // Malformed/absent state (out-of-range stateIndex). Skip rather than
      // dereference; the linearizer would skip it too.
      return;
    }
    const auto& hot = *command.drawState.hot;
    const AttachmentSet targets = attachmentSetFromHot(hot);
    const u64 state_profile =
        command.drawRunRecord ? command.drawRunRecord->invariant.runStableBindingHash
                              : 0u;

    // Start a new pass when none is open or the attachment set changed
    // (mirrors encodeChunk's AttachmentKey split). A pending clear was already
    // folded into a fresh pass at handleClear, so a matching-target draw just
    // continues that pass.
    if (!hasOpenRenderPass() ||
        graph_.passes[currentPass_].targets != targets) {
      openRenderPass(targets, state_profile);
      // First draw of a fresh pass writes its attachments (no preceding clear
      // in this pass recorded them).
      recordAttachmentWrites(targets, currentPass_);
    } else if (graph_.passes[currentPass_].state_profile == 0) {
      graph_.passes[currentPass_].state_profile = state_profile;
    }

    // Read accesses: bound textures (textureMask/textures[]). Edge inference
    // runs off these reads.
    recordTextureReads(hot, currentPass_);

    // Append a single DrawRef covering the whole draw run's param range.
    // firstParam/paramCount come straight from the DrawRunCommandRecord, which
    // is exactly the (command_index, param_first, param_count) DrawRef shape
    // B1 defined.
    const auto* record = command.drawRunRecord;
    const u32 param_first = record ? record->firstParam : 0u;
    const u32 param_count =
        record ? record->paramCount
               : static_cast<u32>(core::drawRunDrawCount(command));
    graph_.draws.push_back(DrawRef{.command_index = static_cast<u32>(commandIndex),
                                   .param_first = param_first,
                                   .param_count = param_count});
    graph_.passes[currentPass_].draws.count += 1u;
    graph_.commands.push_back(CommandRef{
        .command_index = static_cast<u32>(commandIndex),
        .kind = core::MetalCommandKind::DrawRun,
    });
    graph_.passes[currentPass_].commands.count += 1u;
  }

  void recordAttachmentWrites(const AttachmentSet& targets, u32 pass_index) {
    for (u32 i = 0; i < targets.color_count; ++i) {
      noteAccess(ResourceHandle{targets.color[i].value}, pass_index,
                 AccessKind::Write, AccessStage::Fragment);
    }
    noteAccess(ResourceHandle{targets.depth.value}, pass_index,
               AccessKind::Write, AccessStage::Fragment);
  }

  void recordTextureReads(const core::FlatDrawStateRecord& hot, u32 pass_index) {
    if (hot.textureMask == 0) {
      return;
    }
    for (std::size_t stage = 0; stage < hot.textures.size(); ++stage) {
      if ((hot.textureMask & (1u << stage)) == 0u) {
        continue;
      }
      noteAccess(ResourceHandle{hot.textures[stage].value}, pass_index,
                 AccessKind::Read, AccessStage::Fragment,
                 /*resolve_alias=*/false);
    }
  }

  void handleClear(std::size_t commandIndex) {
    const auto command = slot_.commandAt(commandIndex);
    if (!command.clear) {
      return;
    }
    const AttachmentSet targets = attachmentSetFromClear(*command.clear);
    // A clear is a pass boundary: open a fresh render pass that adopts the
    // cleared targets, mirroring beginRenderPass folding a pendingClear into
    // the next encoder. A following same-target draw continues this pass.
    finalizeCurrentPass();
    openRenderPass(targets, /*state_profile=*/0u);
    graph_.commands.push_back(CommandRef{
        .command_index = static_cast<u32>(commandIndex),
        .kind = core::MetalCommandKind::Clear,
    });
    graph_.passes[currentPass_].commands.count += 1u;
    for (u32 i = 0; i < targets.color_count; ++i) {
      noteAccess(ResourceHandle{targets.color[i].value}, currentPass_,
                 AccessKind::Clear, AccessStage::Fragment);
    }
    if (targets.depth.value != 0) {
      noteAccess(ResourceHandle{targets.depth.value}, currentPass_,
                 AccessKind::Clear, AccessStage::Fragment);
    }
  }

  void handlePresent(std::size_t commandIndex) {
    const u32 pass_index = emitStandalonePass(PassKind::Present, commandIndex);
    (void)pass_index;
    graph_.flush_boundary = true;  // present is a frame boundary (R-BACK-32.4)
  }

  void handleSurfaceCopy(std::size_t commandIndex) {
    const auto command = slot_.commandAt(commandIndex);
    if (!command.surfaceCopy) {
      return;
    }
    emitBlit(commandIndex, ResourceHandle{command.surfaceCopy->source.value},
             ResourceHandle{command.surfaceCopy->destination.value});
  }

  void handleStretchRect(std::size_t commandIndex) {
    const auto command = slot_.commandAt(commandIndex);
    if (!command.stretchRect) {
      return;
    }
    emitBlit(commandIndex, ResourceHandle{command.stretchRect->source.value},
             ResourceHandle{command.stretchRect->destination.value});
  }

  void handleColorFill(std::size_t commandIndex) {
    const auto command = slot_.commandAt(commandIndex);
    if (!command.colorFill) {
      return;
    }
    // Fill has no source read, only a destination write.
    emitBlit(commandIndex, ResourceHandle{},
             ResourceHandle{command.colorFill->destination.value});
  }

  void handleDepthResolve(std::size_t commandIndex) {
    const auto command = slot_.commandAt(commandIndex);
    if (!command.depthResolve) {
      return;
    }
    emitBlit(commandIndex,
             ResourceHandle{command.depthResolve->msaaDepth.value},
             ResourceHandle{command.depthResolve->intzDest.value});
  }

  void handleReadback(std::size_t commandIndex) {
    const auto command = slot_.commandAt(commandIndex);
    if (!command.readback) {
      return;
    }
    const u32 pass_index = emitStandalonePass(PassKind::Blit, commandIndex);
    currentPass_ = pass_index;
    const ResourceHandle source{command.readback->source.value};
    noteAccess(source, pass_index, AccessKind::Read, AccessStage::Copy);
    // CPU readback: mark the readback classifier bit on the source so DCE /
    // memoryless cross-chunk safety gates (spec.md §5.1/§5.3) can see it.
    if (source.value != 0) {
      resourceFor(source).classifier_flags.readback_seen = true;
    }
  }

  void emitBlit(std::size_t commandIndex, ResourceHandle source,
                ResourceHandle dest) {
    const u32 pass_index =
        emitStandalonePass(PassKind::Blit, commandIndex);
    currentPass_ = pass_index;
    if (source.value != 0) {
      noteAccess(source, pass_index, AccessKind::Read, AccessStage::Copy);
    }
    if (dest.value != 0) {
      noteAccess(dest, pass_index, AccessKind::Write, AccessStage::Copy);
    }
  }

  const core::ChunkSlot& slot_;
  ResourceAliasResolver aliasResolver_{};
  FrameGraph& graph_;
  u32 currentPass_ = 0;
  bool openRenderPass_ = false;
};

}  // namespace

void buildFrameGraph(const core::ChunkSlot& slot, u64 frame_id, FrameGraph& out) {
  buildFrameGraph(slot, frame_id, ResourceAliasResolver{}, out);
}

void buildFrameGraph(const core::ChunkSlot& slot, u64 frame_id,
                     ResourceAliasResolver alias_resolver, FrameGraph& out) {
  Builder builder(slot, frame_id, alias_resolver, out);
  builder.run();
}

FrameGraph buildFrameGraph(const core::ChunkSlot& slot, u64 frame_id) {
  return buildFrameGraph(slot, frame_id, ResourceAliasResolver{});
}

FrameGraph buildFrameGraph(const core::ChunkSlot& slot, u64 frame_id,
                           ResourceAliasResolver alias_resolver) {
  FrameGraph graph;
  buildFrameGraph(slot, frame_id, alias_resolver, graph);
  return graph;
}

}  // namespace dxmt9::framegraph
