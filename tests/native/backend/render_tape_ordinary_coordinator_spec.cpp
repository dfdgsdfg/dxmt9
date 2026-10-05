#include "device_c_chunk_replay.hpp"
#include "device_c_common.hpp"
#include "device_c_cpu_ready_plan.hpp"
#include "device_c_render_tape_runtime_binding_v4.hpp"
#include "device_c_render_tape_runtime_replay_v4.hpp"
#include "d3d9_pe_chunk_builder.hpp"
#include "dxmt9/dxmt9_device.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace dxmt9 {
struct CommandQueueArenaLeaseTestAccess {
  // Set the fixture's immutable policy before any arm/replay begins. No
  // worker/session receipt is fabricated by this no-worker fixture.
  static void sessionLane(CommandQueue& queue, bool enabled) {
    queue.cpuReadySessionLaneEnabled_ = enabled;
  }
};
} // namespace dxmt9

namespace {
using namespace dxmt9;
using namespace dxmt9::d3d9;
using Authority = RenderTapeIdentityV4CoordinatorAuthority;
constexpr RenderTapeIdentityV4CaptureKey key{5u, 6u};
std::uint64_t checks = 0u;

void check(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}

struct Upper final : Device {
  explicit Upper(bool direct) : queue_(CommandQueue::ArenaLeaseTestQueueTag{}, limits_),
      direct_(direct) {}
  WMT::Device wmtDevice() override { return WMT::Device{NULL_OBJECT_HANDLE}; }
  CommandQueue& queue() override { return queue_; }
  const core::BackendLimits& limits() const override { return limits_; }
  std::shared_ptr<core::BackendDevice> backend() override { return {}; }
  bool supportsCpuReadyArenaReplay() const noexcept override { return true; }
  bool supportsDirectChunkSlotReplay() const noexcept override { return direct_; }
  void markChunkResources(std::span<const core::ChunkHandleEntry> entries) override {
    queue_.markChunkResources(entries);
  }
  void submitDrawRun(core::CanonicalDrawState state, const core::DrawUniformPayload& uniforms,
      std::span<const core::DrawParam> draws,
      std::span<const core::DrawParamPayloadView> payloads) override {
    queue_.submitDrawRun(std::move(state), uniforms, draws, payloads);
  }
  core::BackendLimits limits_{};
  CommandQueue queue_;
  bool direct_ = false;
};

struct Fixture {
  explicit Fixture(bool direct = false, bool session = false) {
    auto upper = std::make_unique<Upper>(direct);
    queue = &upper->queue();
    CommandQueueArenaLeaseTestAccess::sessionLane(*queue, session);
    factory = com::Direct3DCreate9Ex(com::D3D_SDK_VERSION, std::move(upper));
    check(factory != nullptr, "ordinary coordinator fixture factory exists");
    core::PresentParameters params{};
    params.backBufferWidth = params.backBufferHeight = 8u;
    params.backBufferFormat = core::Format::A8R8G8B8;
    params.windowed = true;
    params.deviceWindow = core::Handle{1u};
    params.presentationInterval = core::PresentInterval::Immediate;
    iface = factory->CreateDeviceEx(0u, params, nullptr);
    check(iface != nullptr, "ordinary coordinator fixture device exists");
    iface->AddRef();
    device = std::make_unique<D9CDevice>(iface);
  }
  ~Fixture() {
    device.reset();
    if (iface) iface->Release();
    if (factory) factory->Release();
  }
  CommandQueue* queue = nullptr;
  com::IDirect3D9Ex* factory = nullptr;
  com::IDirect3DDevice9Ex* iface = nullptr;
  std::unique_ptr<D9CDevice> device{};
};

struct ObservationRoot final : render::TapeRuntimeCaptureAttachment {
  render::TapeRuntimeObserverSink sink() noexcept override { return {this, observe}; }
  render::TapeTopologyCapture& topology() noexcept override { return topology_; }
  static void observe(void* pointer, const render::TapeRuntimeObservation& value) noexcept {
    auto& self = *static_cast<ObservationRoot*>(pointer);
    ++self.calls;
    self.last = value;
  }
  render::TapeTopologyCapture topology_{};
  render::TapeRuntimeObservation last{};
  std::uint32_t calls = 0u;
};

void queueOriginAndRetainedArmGate() {
  CommandQueue queue(CommandQueue::ArenaLeaseTestQueueTag{}, {});
  auto first = std::make_shared<ObservationRoot>();
  render::TapeRuntimeBaseline baseline{};
  check(queue.installTapeRuntimeCapture(first, baseline) && baseline.queue &&
      !baseline.sessionLaneEnabled, "actual no-session queue arm retains its immutable policy");
  const auto old = queue.acquireTapeRuntimeCapture();
  const auto same = queue.acquireTapeRuntimeCapture();
  check(old.matches(same) && !old.matches({}), "lease equality joins only the same retained arm");
  const render::TapeOrdinaryCoordinatorAdmission value{.captureToken = 5u,
      .captureEpoch = 6u, .eventOrdinal = 17u, .rawOrdinal = 19u,
      .attemptGeneration = 23u, .recordIndex = 3u,
      .recordType = D9C_COMMAND_RECORD_QUERY_ISSUE};
  queue.observeTapeRuntimeOrdinaryCoordinatorAdmission(old, value);
  check(first->calls == 1u &&
      first->last.edge == render::TapeRuntimeEdge::OrdinaryCoordinatorAdmission &&
      first->last.ordinaryAdmission.queue == baseline.queue &&
      first->last.ordinaryAdmission.captureToken == value.captureToken &&
      first->last.ordinaryAdmission.captureEpoch == value.captureEpoch &&
      first->last.ordinaryAdmission.eventOrdinal == value.eventOrdinal &&
      first->last.ordinaryAdmission.rawOrdinal == value.rawOrdinal &&
      first->last.ordinaryAdmission.attemptGeneration == value.attemptGeneration &&
      first->last.ordinaryAdmission.recordIndex == value.recordIndex &&
      first->last.ordinaryAdmission.recordType == value.recordType &&
      !first->last.release.valid() && !first->last.sequence && !first->last.completedSequence,
      "positive queue-origin admission preserves exact locator and invents no release/completion");

  CommandQueue otherQueue(CommandQueue::ArenaLeaseTestQueueTag{}, {});
  auto foreign = std::make_shared<ObservationRoot>();
  render::TapeRuntimeBaseline otherBaseline{};
  check(otherQueue.installTapeRuntimeCapture(foreign, otherBaseline), "foreign queue arm exists");
  queue.observeTapeRuntimeOrdinaryCoordinatorAdmission(otherQueue.acquireTapeRuntimeCapture(), value);
  queue.observeTapeRuntimeOrdinaryCoordinatorAdmission({}, value);
  check(first->calls == 1u && !foreign->calls, "foreign and absent leases admit no observation");
  queue.revokeTapeRuntimeObserver(first->sink());
  auto replacement = std::make_shared<ObservationRoot>();
  render::TapeRuntimeBaseline nextBaseline{};
  check(queue.installTapeRuntimeCapture(replacement, nextBaseline) &&
      nextBaseline.queue == baseline.queue && nextBaseline.sequence == baseline.sequence &&
      nextBaseline.completedSequence == baseline.completedSequence &&
      nextBaseline.initializer == baseline.initializer &&
      nextBaseline.completedInitializer == baseline.completedInitializer,
      "ordinary observation makes no source, submit, completion or arm-watermark change");
  const auto next = queue.acquireTapeRuntimeCapture();
  check(!old.active() && !old.matches(next), "replacement capture cannot revive old lease");
  queue.observeTapeRuntimeOrdinaryCoordinatorAdmission(old, value);
  check(!replacement->calls && first->calls == 1u,
      "old replay cannot target newly armed root on the same queue");
  queue.observeTapeRuntimeOrdinaryCoordinatorAdmission(next, value);
  check(replacement->calls == 1u, "only replacement's authentic lease can observe its boundary");
  queue.revokeTapeRuntimeObserver(replacement->sink());
  otherQueue.revokeTapeRuntimeObserver(foreign->sink());

  // Both retained and legacy cold arms report the actual configured policy.
  CommandQueue enabled(CommandQueue::ArenaLeaseTestQueueTag{}, {});
  CommandQueueArenaLeaseTestAccess::sessionLane(enabled, true);
  auto session = std::make_shared<ObservationRoot>();
  render::TapeRuntimeBaseline sessionBaseline{};
  check(enabled.installTapeRuntimeCapture(session, sessionBaseline) && sessionBaseline.sessionLaneEnabled,
      "session-enabled retained arm positively retains session domain");
  enabled.observeTapeRuntimeOrdinaryCoordinatorAdmission(enabled.acquireTapeRuntimeCapture(), value);
  check(!session->calls, "session-enabled queue never emits ordinary authority, even for empty writer");
  enabled.revokeTapeRuntimeObserver(session->sink());
  check(enabled.installTapeRuntimeObserver(session->sink(), sessionBaseline) &&
      sessionBaseline.sessionLaneEnabled, "legacy cold observer baseline also retains actual session policy");
  enabled.revokeTapeRuntimeObserver(session->sink());
}

void originalCoordinatorReplay(bool planning, bool separator, bool session) {
  Fixture fixture(separator, session);
  auto* device = fixture.device.get();
  auto* query = dxmt9c_device_create_query(device, 9u);
  check(query != nullptr, "real Occlusion query exists before capture arm");
  struct QueryScope {
    D9CQuery* value;
    ~QueryScope() { dxmt9c_query_release(value); }
  } queryScope{query};
  pe::CommandChunkBuilder builder;
  if (separator) {
    const std::array streams{pe::SparseBindingInput<D9CCommandChunkWireStreamBinding>{
        .wire = {.slot = 0u, .valid = 1u,
            .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX, .stride = 16u, .frequency = 1u}}};
    check(pe::appendSparseRecord(builder, D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
        {.primitiveType = 4u, .primitiveCount = 1u}, {.streams = streams}),
        "original direct-eligible draw precedes coordinator separator");
  }
  const auto reference = pe::qualifyLocalRef<pe::QueryRef>({query->wireIdentity, query});
  check(pe::appendQueryIssue(builder, 2u, reference), "original QUERY_BEGIN coordinator appends");
  const auto sealed = builder.seal();
  check(sealed.valid() && sealed.recordCount == (separator ? 2u : 1u), "original full raw seals once");
  CommandChunkEnvelope envelope{};
  envelope.recordCount = sealed.recordCount;
  envelope.handleCount = sealed.handleCount;
  RawCommandChunk raw;
  check(prepareOffloadChunk(sealed.blob, envelope, device->wireObjects,
      [](std::uint32_t kind, void* object) noexcept {
        if (kind == D9C_CHUNK_HANDLE_KIND_QUERY) dxmt9c_query_addref(static_cast<D9CQuery*>(object));
      }, raw), "original coordinator raw retains real resolved wrapper through preflight");
  struct RawScope {
    RawCommandChunk& value;
    ~RawScope() { releaseRetainedWrappers(value); }
  } rawScope{raw};
  raw.cpuReadyTapePlanningEnabled = planning;
  D9CRenderTapeIdentityCaptureV4ArmResult arm{};
  check(dxmt9c_device_begin_render_tape_identity_capture_v4(device, key.token, key.epoch, &arm) ==
      core::D3D_OK && device->renderTapeRuntimeV4Selected.load(std::memory_order_acquire),
      "real queue arm selects route-neutral V4 before tagged coordinator replay");
  const auto root = acquireRenderTapeRuntimeIdentityV4(device);
  const auto lease = fixture.queue->acquireTapeRuntimeCapture();
  check(device->replayDrainLedger.publishInline(raw), "actual replay ledger assigns this raw ordinal");
  // These are authentic original-production ingress tags from the real V4
  // arm, not provider-observation credentials. Permanent V4 selection above
  // suppresses the archived raw-token routing veto; the mixed receipt check
  // below proves that the Direct prefix retains its selected production path.
  raw.renderTapeCaptureToken = arm.captureToken;
  raw.renderTapeEventOrdinal = 17u;
  if (separator) {
    ImportedChunkView imported;
    check(importPrevalidatedCommandChunk(sealed.blob, envelope, imported), "mixed raw imports unchanged");
    const auto plan = planReplayEmission(imported, raw.replaySeq, 4096u);
    check(plan.spansExecutable() && plan.rotationFreeProductionEligible() &&
        plan.leaseOwningSpanCount() == 1u && plan.leaseSpans.size() == 2u &&
        plan.leaseSpans[0u].ownsLease && !plan.leaseSpans[1u].ownsLease &&
        plan.leaseSpans[1u].firstRecordIndex == 1u,
        "mixed production plan is one Direct span followed by original no-lease coordinator");
  }
  const auto hr = replayRawChunk(device, raw);
  const auto issue = device->dev().captureQueryPollSnapshot(query->obj);
  check(hr == core::D3D_OK && issue.active && issue.issueOrdinal == 1u && !issue.failed &&
      !device->replayDrainLedger.terminal(),
      "ordinary diagnostic success/failure preserves real query effect, HRESULT and ledger health");
  device->replayDrainLedger.publishReplayed(raw);
  if (session) {
    check(root->failed(), "missing real release in enabled session domain never infers ordinary authority");
  } else {
    RenderTapeIdentityV4View identity;
    RenderTapeExecutionTopologyV1View topology;
    check(!root->failed() && root->snapshot(key, identity, topology, false) &&
        identity.header.coordinatorDomain == static_cast<std::uint32_t>(Authority::OrdinarySerial) &&
        identity.coordinators.size() == 1u && identity.attempts.size() == (separator ? 2u : 1u) &&
        identity.settlements.empty(),
        "actual no-session replay captures authority independently of raw acknowledgment");
    const auto& coordinator = identity.coordinators.front();
    const auto& provenance = coordinator.provenance;
    const auto attempt = std::find_if(identity.attempts.begin(), identity.attempts.end(),
        [&](const auto& value) { return value.attemptGeneration == provenance.buildGeneration; });
    check(coordinator.authority == static_cast<std::uint32_t>(Authority::OrdinarySerial) &&
        coordinator.admissionEdge && !provenance.releaseOrdinal && !provenance.releaseGeneration &&
        !provenance.releaseAction && !provenance.releaseWriter && provenance.eventOrdinal == 17u &&
        provenance.rawOrdinal == raw.replaySeq && provenance.recordIndex == (separator ? 1u : 0u) &&
        provenance.recordType == D9C_COMMAND_RECORD_QUERY_ISSUE && provenance.flags == 2u &&
        provenance.dispatchResult == core::D3D_OK && attempt != identity.attempts.end() &&
        attempt->effectEdge && attempt->effectEdge < coordinator.admissionEdge &&
        coordinator.admissionEdge < provenance.dispatchEdge && provenance.dispatchEdge < attempt->terminalEdge,
        "queue admission joins authentic attempt and original locator after effect admission and before dispatch");
    if (separator)
      check(identity.attempts[0u].receipt.kind == static_cast<std::uint32_t>(
                RenderTapeIdentityV4ReceiptKind::DirectChunkSlot) &&
          identity.attempts[1u].receipt.kind == static_cast<std::uint32_t>(
                RenderTapeIdentityV4ReceiptKind::Compatibility) &&
          identity.attempts[0u].recordCount == 1u && identity.attempts[1u].firstRecord == 1u,
          "capture does not demote Direct prefix or renumber coordinator separator");
    const ReplayTransactionRawIdentity rawIdentity{key.token, 17u, raw.replaySeq, raw.recordCount};
    const RenderTapeRuntimeReplayAccessV4 access(root.get(), rawIdentity, lease);
    check(access.acknowledgeRaw(true) && root->snapshot(key, identity, topology, false) &&
        identity.settlements.size() == 1u && identity.settlements.front().rawOrdinal == raw.replaySeq &&
        identity.settlements.front().recordCount == raw.recordCount,
        "only actual test-ledger replay acknowledgment settles the complete original raw");
  }
  dxmt9c_device_cancel_render_tape_identity_capture_v4(device, key.token, key.epoch, arm.queue);
}

} // namespace

int main() {
  try {
    queueOriginAndRetainedArmGate();
    originalCoordinatorReplay(false, false, false);
    originalCoordinatorReplay(true, false, false);
    originalCoordinatorReplay(false, true, false);
    // This path performs no real release. A worker-free fixture cannot invent
    // one merely because its immutable session policy is enabled.
    originalCoordinatorReplay(false, false, true);
    std::cout << "render tape ordinary coordinator: " << checks << " checks\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
