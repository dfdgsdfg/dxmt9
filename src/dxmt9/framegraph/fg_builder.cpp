// Frame Graph builder (Task B2, L1). See fg_builder.hpp for scope/contract.
//
// spec.md §4.1 maps chunk-record kinds onto builder actions. The real input
// is an immutable `core::SourcePayloadView`, so the same mapping applies to
// legacy ChunkSlot and arena-backed payload storage.
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

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <utility>

namespace dxmt9::framegraph {

namespace {

// The attachment-set identity the encoder splits render passes on
// (dxmt9_render_pass_internal.hpp makeAttachmentKey):
// color0..kMaxRenderTargets + depth.
// Two draws share a render pass iff their AttachmentSet compares equal.
AttachmentSet attachmentSetFromHot(const core::FlatDrawStateRecord& hot) {
  AttachmentSet set{};
  u32 count = 0;
  for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
    const auto& attachment = hot.colorAttachments[i];
    const auto handle = attachment.handle;
    set.color[i] = TextureHandle{handle.value};
    set.sample_count = std::max(set.sample_count, attachment.sampleCount);
    if (handle.value != 0) {
      // color_count = highest bound slot + 1 (matches the 8-wide spec field;
      // unbound interior slots keep their zero handle).
      count = static_cast<u32>(i) + 1u;
    }
  }
  set.color_count = count;
  set.depth = TextureHandle{hot.depthStencil.handle.value};
  set.sample_count =
      std::max(set.sample_count, hot.depthStencil.sampleCount);
  return set;
}

AttachmentSet attachmentSetFromClear(const core::ClearCommandView& clear) {
  AttachmentSet set{};
  u32 count = 0;
  if (clear.clearColor) {
    for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
      const auto& attachment = clear.colorAttachments[i];
      const auto handle = attachment.handle;
      set.color[i] = TextureHandle{handle.value};
      set.sample_count = std::max(set.sample_count, attachment.sampleCount);
      if (handle.value != 0) {
        count = static_cast<u32>(i) + 1u;
      }
    }
  }
  set.color_count = count;
  if (clear.clearDepth || clear.clearStencil) {
    set.depth = TextureHandle{clear.depthStencil.handle.value};
    set.sample_count =
        std::max(set.sample_count, clear.depthStencil.sampleCount);
  }
  return set;
}

// Per-build state machine. Owns the in-progress pass index so command handlers
// can append accesses / draws / edges against the current pass.
class Builder {
public:
  Builder(core::SourcePayloadView payload, u64 frame_id,
          ResourceAliasResolver aliasResolver, FrameGraph& graph)
      : payload_(payload), aliasResolver_(aliasResolver), graph_(graph) {
    graph_.reset();
    graph_.frame_id = frame_id;
  }

  void run() {
    const std::size_t commandCount = payload_.commandCount();
    for (std::size_t i = 0; i < commandCount; ++i) {
      switch (payload_.commandAt(i).kind()) {
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
      case core::MetalCommandKind::GenerateMipmaps:
        handleGenerateMipmaps(i);
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
        .kind = payload_.commandAt(commandIndex).kind(),
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
    const auto command = payload_.commandAt(commandIndex).command;
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
    const auto source = payload_.commandAt(commandIndex);
    if (!source.clear) {
      return;
    }
    const auto& clear = *source.clear;
    const AttachmentSet targets = attachmentSetFromClear(clear);
    // A rectangular clear preserves pixels outside the rect set. A
    // depth-only or stencil-only clear may preserve the other aspect of the
    // shared depth/stencil resource; ClearDesc does not carry enough format
    // information to prove that the other aspect is absent. Model either case
    // as ReadWrite so DCE cannot mistake it for a full-resource overwrite.
    const AccessKind colorAccessKind =
        clear.rects.empty() ? AccessKind::Clear : AccessKind::ReadWrite;
    const AccessKind depthStencilAccessKind =
        clear.rects.empty() &&
            aliasResolver_.depthStencilClearCoversResource(
                ResourceHandle{clear.depthStencil.handle.value},
                clear.clearDepth, clear.clearStencil)
            ? AccessKind::Clear
            : AccessKind::ReadWrite;
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
                 colorAccessKind, AccessStage::Fragment);
    }
    if (targets.depth.value != 0) {
      noteAccess(ResourceHandle{targets.depth.value}, currentPass_,
                 depthStencilAccessKind, AccessStage::Fragment);
    }
  }

  void handlePresent(std::size_t commandIndex) {
    const u32 pass_index = emitStandalonePass(PassKind::Present, commandIndex);
    const auto command = payload_.commandAt(commandIndex).command;
    if (command.present && command.present->presentSource.value != 0) {
      currentPass_ = pass_index;
      // Present observes the source surface. Recording the read keeps a final
      // backbuffer writer live even when a later chunk begins with a Clear.
      noteAccess(ResourceHandle{command.present->presentSource.value},
                 pass_index, AccessKind::Read, AccessStage::Copy);
    }
    graph_.flush_boundary = true;  // present is a frame boundary (R-BACK-32.4)
  }

  void handleSurfaceCopy(std::size_t commandIndex) {
    const auto command = payload_.commandAt(commandIndex).command;
    if (!command.surfaceCopy) {
      return;
    }
    emitBlit(commandIndex, ResourceHandle{command.surfaceCopy->source.value},
             ResourceHandle{command.surfaceCopy->destination.value});
  }

  void handleStretchRect(std::size_t commandIndex) {
    const auto command = payload_.commandAt(commandIndex).command;
    if (!command.stretchRect) {
      return;
    }
    emitBlit(commandIndex, ResourceHandle{command.stretchRect->source.value},
             ResourceHandle{command.stretchRect->destination.value});
  }

  void handleColorFill(std::size_t commandIndex) {
    const auto command = payload_.commandAt(commandIndex).command;
    if (!command.colorFill) {
      return;
    }
    // Fill has no source read, only a destination write.
    emitBlit(commandIndex, ResourceHandle{},
             ResourceHandle{command.colorFill->destination.value});
  }

  void handleDepthResolve(std::size_t commandIndex) {
    const auto command = payload_.commandAt(commandIndex).command;
    if (!command.depthResolve) {
      return;
    }
    emitBlit(commandIndex,
             ResourceHandle{command.depthResolve->msaaDepth.value},
             ResourceHandle{command.depthResolve->intzDest.value});
  }

  void handleGenerateMipmaps(std::size_t commandIndex) {
    const auto command = payload_.commandAt(commandIndex).command;
    if (!command.generateMipmaps) {
      return;
    }
    const u32 pass_index =
        emitStandalonePass(PassKind::Blit, commandIndex);
    currentPass_ = pass_index;
    noteAccess(ResourceHandle{command.generateMipmaps->texture.value},
               pass_index, AccessKind::ReadWrite, AccessStage::Copy,
               /*resolve_alias=*/false);
  }

  void handleReadback(std::size_t commandIndex) {
    const auto command = payload_.commandAt(commandIndex).command;
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

  core::SourcePayloadView payload_{};
  ResourceAliasResolver aliasResolver_{};
  FrameGraph& graph_;
  u32 currentPass_ = 0;
  bool openRenderPass_ = false;
};

}  // namespace

ActiveRenderPlanningSeedResult applyActiveRenderPlanningSeed(
    FrameGraph& graph, const ActiveRenderPlanningSeed& seed,
    ResourceAliasResolver alias_resolver) {
  if (!seed.complete) {
    return ActiveRenderPlanningSeedResult::Incomplete;
  }
  if (seed.dependency_count > seed.write_dependencies.size()) {
    return ActiveRenderPlanningSeedResult::Overflow;
  }
  if (seed.targets.color_count > seed.targets.color.size() ||
      seed.targets.sample_count == 0 || seed.dependency_count == 0 ||
      (seed.targets.color_count == 0 && seed.targets.depth.value == 0)) {
    return ActiveRenderPlanningSeedResult::Invalid;
  }

  std::array<ResourceHandle, kActiveRenderPlanningDependencyCapacity>
      dependencies{};
  std::size_t dependency_count = 0;
  for (std::size_t i = 0; i < seed.dependency_count; ++i) {
    ResourceHandle handle = alias_resolver(seed.write_dependencies[i]);
    if (handle.value == 0) {
      return ActiveRenderPlanningSeedResult::Incomplete;
    }
    bool duplicate = false;
    for (std::size_t j = 0; j < dependency_count; ++j) {
      duplicate = duplicate || dependencies[j] == handle;
    }
    if (!duplicate) {
      dependencies[dependency_count++] = handle;
    }
  }

  auto containsDependency = [&](ResourceHandle raw_handle) {
    if (raw_handle.value == 0) {
      return true;
    }
    const ResourceHandle handle = alias_resolver(raw_handle);
    return std::find(dependencies.begin(),
                     dependencies.begin() + dependency_count,
                     handle) != dependencies.begin() + dependency_count;
  };
  for (u32 i = 0; i < seed.targets.color_count; ++i) {
    if (!containsDependency(seed.targets.color[i])) {
      return ActiveRenderPlanningSeedResult::Incomplete;
    }
  }
  if (!containsDependency(seed.targets.depth)) {
    return ActiveRenderPlanningSeedResult::Incomplete;
  }

  if (graph.passes.size() >= std::numeric_limits<u32>::max()) {
    return ActiveRenderPlanningSeedResult::Overflow;
  }
  for (const ResourceNode& resource : graph.resources) {
    for (const AccessLog& access : resource.accesses) {
      if (access.pass_index == std::numeric_limits<u32>::max()) {
        return ActiveRenderPlanningSeedResult::Overflow;
      }
    }
  }
  for (const Edge& edge : graph.edges) {
    if (edge.src_pass == std::numeric_limits<u32>::max() ||
        edge.dst_pass == std::numeric_limits<u32>::max()) {
      return ActiveRenderPlanningSeedResult::Overflow;
    }
  }

  for (ResourceNode& resource : graph.resources) {
    for (AccessLog& access : resource.accesses) {
      ++access.pass_index;
    }
  }
  for (Edge& edge : graph.edges) {
    ++edge.src_pass;
    ++edge.dst_pass;
  }

  PassNode virtual_pass{};
  virtual_pass.kind = PassKind::Render;
  virtual_pass.targets = seed.targets;
  virtual_pass.flags.active_render_seed = true;
  graph.passes.insert(graph.passes.begin(), virtual_pass);

  auto addSeedEdgeOnce = [&](u32 dst, ResourceHandle handle) {
    if (dst == 0) {
      return;
    }
    for (const Edge& edge : graph.edges) {
      if (edge.src_pass == 0 && edge.dst_pass == dst &&
          edge.resource == handle) {
        return;
      }
    }
    graph.edges.push_back(Edge{
        .src_pass = 0,
        .dst_pass = dst,
        .resource = handle,
    });
  };

  for (std::size_t i = 0; i < dependency_count; ++i) {
    const ResourceHandle handle = dependencies[i];
    std::size_t resource_index = findResourceIndex(graph, handle);
    if (resource_index == graph.resources.size()) {
      ResourceNode resource{};
      resource.handle = handle;
      graph.resources.push_back(std::move(resource));
      resource_index = graph.resources.size() - 1u;
    }
    ResourceNode& resource = graph.resources[resource_index];
    bool reached_current_write = false;
    for (const AccessLog& access : resource.accesses) {
      if (reached_current_write) {
        break;
      }
      const auto kind = static_cast<AccessKind>(access.access_kind);
      const bool reads = kind == AccessKind::Read ||
                         kind == AccessKind::ReadWrite;
      const bool writes = kind == AccessKind::Write ||
                          kind == AccessKind::Clear ||
                          kind == AccessKind::ReadWrite;
      if (reads || writes) {
        addSeedEdgeOnce(access.pass_index, handle);
      }
      reached_current_write = writes;
    }
    resource.accesses.insert(
        resource.accesses.begin(),
        AccessLog{
            .pass_index = 0,
            .access_kind = static_cast<u8>(AccessKind::Write),
            .stage = static_cast<u8>(AccessStage::Fragment),
        });
  }
  return ActiveRenderPlanningSeedResult::Applied;
}

bool activeRenderPlanningSeedProvesReplayHead(
    const FrameGraph& graph, u32 replay_head_command) noexcept {
  for (const PassNode& pass : graph.passes) {
    if (!pass.flags.active_render_seed || pass.flags.dead ||
        pass.kind != PassKind::Render || pass.commands.count == 0 ||
        pass.commands.first >= graph.commands.size() ||
        pass.commands.count > graph.commands.size() - pass.commands.first) {
      continue;
    }
    return graph.commands[pass.commands.first].command_index ==
           replay_head_command;
  }
  return false;
}

void buildFrameGraph(core::SourcePayloadView payload, u64 frame_id,
                     FrameGraph& out) {
  buildFrameGraph(payload, frame_id, ResourceAliasResolver{}, out);
}

void buildFrameGraph(core::SourcePayloadView payload, u64 frame_id,
                     ResourceAliasResolver alias_resolver, FrameGraph& out) {
  Builder builder(payload, frame_id, alias_resolver, out);
  builder.run();
}

FrameGraph buildFrameGraph(core::SourcePayloadView payload, u64 frame_id) {
  return buildFrameGraph(payload, frame_id, ResourceAliasResolver{});
}

FrameGraph buildFrameGraph(core::SourcePayloadView payload, u64 frame_id,
                           ResourceAliasResolver alias_resolver) {
  FrameGraph graph;
  buildFrameGraph(payload, frame_id, alias_resolver, graph);
  return graph;
}

void buildFrameGraph(const core::ChunkSlot& slot, u64 frame_id,
                     FrameGraph& out) {
  buildFrameGraph(core::SourcePayloadView(slot), frame_id, out);
}

void buildFrameGraph(const core::ChunkSlot& slot, u64 frame_id,
                     ResourceAliasResolver alias_resolver, FrameGraph& out) {
  buildFrameGraph(core::SourcePayloadView(slot), frame_id, alias_resolver, out);
}

FrameGraph buildFrameGraph(const core::ChunkSlot& slot, u64 frame_id) {
  return buildFrameGraph(core::SourcePayloadView(slot), frame_id);
}

FrameGraph buildFrameGraph(const core::ChunkSlot& slot, u64 frame_id,
                           ResourceAliasResolver alias_resolver) {
  return buildFrameGraph(core::SourcePayloadView(slot), frame_id,
                         alias_resolver);
}

}  // namespace dxmt9::framegraph
