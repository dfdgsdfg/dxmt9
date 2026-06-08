#pragma once

// IDecisionRecorder — divergence-logging seam for the modern-renderer
// transition (Task A9, R-BACK-39.3; cross-spec ownership noted in
// specs/d3d9-renderer/design.md §15.4 and specs/backend/design.md §3.1).
//
// PURPOSE / SCOPE (L0 SCAFFOLD ONLY).
//   R-BACK-39.3 requires a side-effect-neutral divergence logger behind
//   DXMT9_RENDERER_LOG_DIVERGENCE=1 that compares the modern path's decision
//   sequence against a *dry-run recorder* of the traditional path. At L0 the
//   FrameGraph backend is a pure delegate (byte-identical to traditional), so
//   there is nothing to diverge yet: the full dry-run reproduce-and-compare
//   engine is only meaningful once L1 makes FrameGraph build a DAG. This file
//   therefore lands ONLY the recording interface, a POD record type, a
//   std::vector-backed concrete recorder, and a pure comparison helper. The
//   actual "walk a chunk through a recorder-only adapter that reproduces the
//   TraditionalBackend decisions" engine is DEFERRED TO L1.
//
// SIDE-EFFECT NEUTRALITY INVARIANT (R-BACK-39.3).
//   A recorder is a pure *observer*. An IDecisionRecorder implementation MUST
//   NOT: call any Metal API, allocate or commit an MTLCommandBuffer, invoke the
//   presenter, mutate the PSO/shader cache or MTLBinaryArchive, touch MTLHeap
//   residency or retained-handle sets, or update queue-level fence state. A
//   VectorDecisionRecorder owning its own std::vector is the canonical
//   side-effect-neutral implementation: the only state it mutates is storage it
//   exclusively owns. Recording a decision must never change which Metal
//   commands a non-divergence run would emit.
//
// The record-point names below are modeled on the REAL decision points in the
// encode path so an L1 recorder adapter can be wired without reshaping the
// interface:
//   - recordPassBegin   ~ encoders::beginRenderPass (attachment set +
//                         load/store action choice; see dxmt9_draw_encoder.hpp
//                         beginRenderPass / RenderPassActionSummary).
//   - recordDraw        ~ encoders::encodeDraw (the per-draw PSO/draw decision;
//                         dxmt9_draw_encoder.hpp:360).
//   - recordLoadStore   ~ the per-attachment MTLLoadAction/MTLStoreAction proof
//                         result (RenderPass{Depth,Color}StoreProof in
//                         dxmt9_perf_counters.hpp).
//   - recordEncoderSplit~ an encoder split / new-pass boundary
//                         (perf::EncoderSplitReason in dxmt9_perf_counters.hpp).
// They are kept minimal and forward-looking; new kinds may be appended without
// breaking the POD layout (append-only enum).

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dxmt9::render {

// Kind of a recorded decision. Append-only: new kinds go at the end so a
// recorded sequence stays comparable across builds.
enum class DecisionKind : std::uint8_t {
  PassBegin,
  Draw,
  LoadStore,
  EncoderSplit,
};

// POD decision record. Deliberately a small flat value (no owning pointers, no
// COM/ObjC/unix handles) so a sequence of them is trivially copyable and
// comparable. The meaning of a/b/c is kind-specific:
//   PassBegin    : a = color attachment handle (rt0), b = depth handle,
//                  c = color-attachment count.
//   Draw         : a = pso/shader-variant key, b = vertex/index count,
//                  c = draw flags (indexed, instanced, ...).
//   LoadStore    : a = attachment handle, b = load action code,
//                  c = store action / proof code.
//   EncoderSplit : a = EncoderSplitReason value, b/c unused (0).
// The fields are intentionally generic u64 lanes so the L1 recorder adapter can
// pack real decision keys without an interface change.
struct DecisionRecord {
  DecisionKind kind = DecisionKind::PassBegin;
  std::uint64_t a = 0;
  std::uint64_t b = 0;
  std::uint64_t c = 0;

  friend bool operator==(const DecisionRecord& lhs, const DecisionRecord& rhs) {
    return lhs.kind == rhs.kind && lhs.a == rhs.a && lhs.b == rhs.b &&
           lhs.c == rhs.c;
  }
  friend bool operator!=(const DecisionRecord& lhs, const DecisionRecord& rhs) {
    return !(lhs == rhs);
  }
};

// Recording interface. A backend (modern or the L1 dry-run traditional
// adapter) emits its decision sequence through this. Implementations MUST honor
// the side-effect-neutrality invariant documented at the top of this file.
class IDecisionRecorder {
 public:
  virtual ~IDecisionRecorder() = default;

  // A render pass begins: rt0 color handle, depth handle, color-attachment
  // count. Mirrors encoders::beginRenderPass attachment selection.
  virtual void recordPassBegin(std::uint64_t colorHandle0,
                               std::uint64_t depthHandle,
                               std::uint32_t colorAttachmentCount) = 0;

  // A draw is encoded: PSO/shader-variant key, vertex-or-index count, draw
  // flags. Mirrors encoders::encodeDraw.
  virtual void recordDraw(std::uint64_t psoKey,
                          std::uint64_t elementCount,
                          std::uint64_t drawFlags) = 0;

  // A per-attachment load/store action / proof decision. Mirrors the
  // RenderPass{Depth,Color}StoreProof selection.
  virtual void recordLoadStore(std::uint64_t attachmentHandle,
                               std::uint32_t loadAction,
                               std::uint32_t storeOrProofCode) = 0;

  // An encoder split / new-pass boundary. `reason` is a perf::EncoderSplitReason
  // value (kept as a plain u32 here so this header does not depend on the perf
  // counter header).
  virtual void recordEncoderSplit(std::uint32_t reason) = 0;
};

// Concrete recorder that appends decisions to a vector it exclusively owns.
// This is the canonical side-effect-neutral recorder (R-BACK-39.3): the only
// state it mutates is its own `records_` storage. Cheap to default-construct;
// the vector grows on demand and may be reserved/cleared for reuse.
class VectorDecisionRecorder final : public IDecisionRecorder {
 public:
  void recordPassBegin(std::uint64_t colorHandle0,
                       std::uint64_t depthHandle,
                       std::uint32_t colorAttachmentCount) override {
    records_.push_back(DecisionRecord{DecisionKind::PassBegin, colorHandle0,
                                      depthHandle, colorAttachmentCount});
  }

  void recordDraw(std::uint64_t psoKey,
                  std::uint64_t elementCount,
                  std::uint64_t drawFlags) override {
    records_.push_back(
        DecisionRecord{DecisionKind::Draw, psoKey, elementCount, drawFlags});
  }

  void recordLoadStore(std::uint64_t attachmentHandle,
                       std::uint32_t loadAction,
                       std::uint32_t storeOrProofCode) override {
    records_.push_back(DecisionRecord{DecisionKind::LoadStore, attachmentHandle,
                                      loadAction, storeOrProofCode});
  }

  void recordEncoderSplit(std::uint32_t reason) override {
    records_.push_back(DecisionRecord{DecisionKind::EncoderSplit, reason, 0, 0});
  }

  const std::vector<DecisionRecord>& records() const { return records_; }
  std::size_t size() const { return records_.size(); }
  void clear() { records_.clear(); }

 private:
  std::vector<DecisionRecord> records_;
};

// Result of comparing two recorded decision sequences. `diverged` is true iff
// the sequences are not identical; `index` is the first differing position when
// diverged (or, when the sequences share a common prefix and one is shorter,
// the index of the first missing record). When not diverged, `index` equals the
// shared length.
struct DecisionDivergence {
  bool diverged = false;
  std::size_t index = 0;
};

// Pure comparison helper: reports whether two decision sequences diverge and at
// which index. No allocation, no side effects. The L1 divergence logger will
// use this to emit per-chunk divergence points (chunk-record index + decision
// kind) once the dry-run traditional adapter exists.
DecisionDivergence compareDecisions(const std::vector<DecisionRecord>& modern,
                                    const std::vector<DecisionRecord>& reference);

// Pure env resolver for DXMT9_RENDERER_LOG_DIVERGENCE. envFlagSet semantics:
// enabled iff non-null, non-empty, and not "0". Pure so it is unit-testable
// without touching the process environment (matches resolveDisableVsync /
// resolveAcquirePolicy in dxmt9_presenter).
bool resolveLogDivergence(const char* env);

// Process-wide cached read of DXMT9_RENDERER_LOG_DIVERGENCE via the repo's
// static-const-lambda pattern (see layerDisplaySyncEnabled /
// resolveAcquirePolicyFromEnv in dxmt9_presenter.mm). Read once at first use.
bool logDivergenceEnabledFromEnv();

}  // namespace dxmt9::render
