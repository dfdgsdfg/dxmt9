// §5.5 Load/Store Action Selection (Task B7, L1). See ../fg_optimizer.hpp.
//
// Per PassNode, for each color attachment and the depth attachment:
//   load:  Clear  if a same-handle Clear access precedes in THIS pass,
//          else Load.
//   store: DontCare if the attachment is Memoryless (R-BACK-33) or a documented
//                   store-proof applies (R-BACK-15.7/15.8) AND this pass is the
//                   resource's last use,
//          else Store.
//
// ALWAYS run, and ALWAYS AFTER reorder (R-BACK-32.5 / design §5): reorder can
// change which pass is the first or last access of an attachment, so running
// before it would mis-select Load/Store/DontCare for the final linear order.
// This pass therefore reads first_use_pass/last_use_pass which lifetime keeps
// authoritative (reorder reruns nothing else here).
//
// DETERMINISM (R-BACK-32.2): pure scan over the graph, no clock/thread/RNG.

#include "../fg_optimizer.hpp"

#include <cstddef>

namespace dxmt9::framegraph {

namespace {

// True if a Clear access on `handle` occurs in `pass_index` strictly before any
// other access in that pass — i.e. the pass begins by clearing the attachment.
// The access log is chronological, so the FIRST access in the pass is decisive.
bool clearedFirstInPass(const ResourceNode& node, u32 pass_index) {
  for (const AccessLog& access : node.accesses) {
    if (access.pass_index != pass_index) {
      continue;
    }
    // First access this pass touches the resource: Clear wins, anything else
    // means the pass loaded/wrote the existing contents.
    return static_cast<AccessKind>(access.access_kind) == AccessKind::Clear;
  }
  return false;
}

// The resource is Memoryless (memoryless pass promoted it) — its contents do
// not survive the encoder, so the store is always DontCare.
bool isMemoryless(const ResourceNode& node) {
  return node.residency == ResidencyClass::Memoryless;
}

// True when `pass_index` is the last pass that touches `node` — only then can
// the store be DontCare without losing content a later pass reads.
bool isLastUse(const ResourceNode& node, u32 pass_index) {
  return node.last_use_pass == pass_index;
}

LoadAction colorLoadFor(const FrameGraph& graph, TextureHandle handle,
                        u32 pass_index) {
  if (handle.value == 0) {
    return LoadAction::DontCare;  // unbound slot.
  }
  const std::size_t idx = findResourceIndex(graph, ResourceHandle{handle.value});
  if (idx == graph.resources.size()) {
    return LoadAction::Load;
  }
  const ResourceNode& node = graph.resources[idx];
  return clearedFirstInPass(node, pass_index) ? LoadAction::Clear
                                              : LoadAction::Load;
}

StoreAction colorStoreFor(const FrameGraph& graph, TextureHandle handle,
                          u32 pass_index) {
  if (handle.value == 0) {
    return StoreAction::DontCare;  // unbound slot.
  }
  const std::size_t idx = findResourceIndex(graph, ResourceHandle{handle.value});
  if (idx == graph.resources.size()) {
    return StoreAction::Store;
  }
  const ResourceNode& node = graph.resources[idx];
  // DontCare only when this is the last use AND the surface is memoryless. A
  // non-last-use pass must Store so the following pass can Load. (R-BACK-15.7/
  // 15.8 generic store-proofs are not yet wired at L1; memoryless is the one
  // DontCare source the optimizer can prove here.)
  if (isMemoryless(node) && isLastUse(node, pass_index)) {
    return StoreAction::DontCare;
  }
  return StoreAction::Store;
}

}  // namespace

void runLoadStore(FrameGraph& graph) {
  for (std::size_t p = 0; p < graph.passes.size(); ++p) {
    PassNode& pass = graph.passes[p];
    // Only render passes have load/store attachment actions; present/blit/sync
    // passes leave the default DontCare policy untouched.
    if (pass.kind != PassKind::Render) {
      continue;
    }
    const u32 pass_index = static_cast<u32>(p);
    LoadStorePolicy policy{};
    for (std::size_t c = 0; c < pass.targets.color.size(); ++c) {
      const TextureHandle handle = pass.targets.color[c];
      policy.color_load[c] = colorLoadFor(graph, handle, pass_index);
      policy.color_store[c] = colorStoreFor(graph, handle, pass_index);
    }
    policy.depth_load = colorLoadFor(graph, pass.targets.depth, pass_index);
    policy.depth_store = colorStoreFor(graph, pass.targets.depth, pass_index);
    pass.load_store = policy;
  }
}

}  // namespace dxmt9::framegraph
