#include "device_c_common.hpp"
#include "device_c_render_tape_identity_transfer_v4.hpp"
#include "device_c_render_tape_runtime_binding_v4.hpp"
#include "device_c_render_tape_runtime_replay_v4.hpp"
#include "dxmt9/dxmt9_device.hpp"

#include <array>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace dxmt9;
using namespace dxmt9::d3d9;
std::uint64_t checks = 0u;

void check(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}

// A real frontend device delegates to the production inert queue constructor.
// It has no Metal handles or worker threads; ABI admission, retained lifetime,
// and frozen transport behavior still execute their real implementations.
struct BindingUpper final : Device {
  BindingUpper() : queue_(CommandQueue::ArenaLeaseTestQueueTag{}, limits_) {}
  WMT::Device wmtDevice() override { return WMT::Device{NULL_OBJECT_HANDLE}; }
  CommandQueue& queue() override { return queue_; }
  const core::BackendLimits& limits() const override { return limits_; }
  std::shared_ptr<core::BackendDevice> backend() override { return {}; }
  bool usesAsyncQueryFences() const noexcept override { return true; }
  query::EndReceipt publishQueryEnd() noexcept override {
    auto receipt = queue_.publishQueryEnd();
    if (incompleteReceipt) ++receipt.prefix.sequence; // Fault backend, never success evidence.
    return receipt;
  }
  query::Poll pollQueryFence(const query::EndReceipt& receipt, bool flush) noexcept override {
    return queue_.pollQueryFence(receipt, flush);
  }
  query::Progress queryProgress() noexcept override { return queue_.queryProgress(); }
  bool incompleteReceipt = false;
  core::BackendLimits limits_{};
  CommandQueue queue_;
};

struct Fixture {
  Fixture() {
    auto upper = std::make_unique<BindingUpper>();
    backend = upper.get();
    queue = &upper->queue();
    factory = com::Direct3DCreate9Ex(com::D3D_SDK_VERSION, std::move(upper));
    check(factory != nullptr, "native binding factory exists");
    core::PresentParameters params{};
    params.backBufferWidth = params.backBufferHeight = 8u;
    params.backBufferFormat = core::Format::A8R8G8B8;
    params.windowed = true;
    params.deviceWindow = core::Handle{1u};
    params.presentationInterval = core::PresentInterval::Immediate;
    iface = factory->CreateDeviceEx(0u, params, nullptr);
    check(iface != nullptr, "native binding device exists");
    iface->AddRef();
    device = std::make_unique<D9CDevice>(iface);
  }
  ~Fixture() {
    device.reset();
    if (iface) iface->Release();
    if (factory) factory->Release();
  }
  CommandQueue* queue = nullptr;
  BindingUpper* backend = nullptr;
  com::IDirect3D9Ex* factory = nullptr;
  com::IDirect3DDevice9Ex* iface = nullptr;
  std::unique_ptr<D9CDevice> device{};
};

using OwnedQuery = std::unique_ptr<D9CQuery, decltype(&dxmt9c_query_release)>;

OwnedQuery makeEvent(Fixture& fixture) {
  OwnedQuery query(dxmt9c_device_create_query(fixture.device.get(), 8u), dxmt9c_query_release);
  check(bool(query), "real frontend EVENT wrapper has registry-owned local identity");
  check(dxmt9c_query_issue(query.get(), 1u) == core::D3D_OK,
      "fresh local EVENT performs exactly one actual setup END");
  return query;
}

void replayBaselineAdmission() {
  Fixture fixture;
  auto query = makeEvent(fixture);
  const auto snapshot = fixture.device->dev().captureQueryPollSnapshot(query->obj);
  const auto progress = fixture.queue->queryProgress();
  check(snapshot.eligible && snapshot.issueOrdinal == 1u && snapshot.issueDeviceEpoch &&
      query::classifyEnd(snapshot.backendFence, progress) == query::Poll::Complete,
      "setup receipt is genuinely complete against the actual inert queue baseline");
  const std::array inputs{RenderTapeReplayQueryBaselineV4{query.get(), snapshot}};
  D9CRenderTapeIdentityCaptureV4ArmResult arm{};
  check(beginRenderTapeIdentityCaptureV4ForReplay(fixture.device.get(), 51u, 52u,
      inputs, &arm) == core::D3D_OK && arm.status == D9C_RENDER_TAPE_IDENTITY_CAPTURE_COMPLETE,
      "private replay arm atomically exposes a root seeded with the real one-END issue");
  const auto root = acquireRenderTapeRuntimeIdentityV4(fixture.device.get());
  RenderTapeIdentityV4View identity;
  RenderTapeExecutionTopologyV1View topology;
  bool copied = false;
  const auto lease = fixture.queue->acquireTapeRuntimeCapture();
  check(root && lease.visit([&](render::TapeRuntimeCaptureAttachment& attachment) noexcept {
        if (&attachment == root.get()) copied = root->snapshot({51u, 52u}, identity, topology, false);
      }) && copied &&
      identity.coordinators.empty() && identity.sources.empty() && identity.commands.empty() &&
      identity.attempts.empty(), "baseline join fabricates no interval coordinator/source/attempt");
  dxmt9c_device_cancel_render_tape_identity_capture_v4(fixture.device.get(), 51u, 52u, arm.queue);
  check(!fixture.queue->acquireTapeRuntimeCapture(), "successful private arm retires normally");
}

void replayBaselineRejections() {
  for (std::uint32_t variant = 0u; variant < 18u; ++variant) {
    Fixture fixture;
    Fixture foreign;
    if (variant == 15u) fixture.backend->incompleteReceipt = true;
    auto query = makeEvent(fixture);
    auto snapshot = fixture.device->dev().captureQueryPollSnapshot(query->obj);
    auto wrongQuery = makeEvent(foreign);
    std::vector<RenderTapeReplayQueryBaselineV4> inputs{{query.get(), snapshot}};
    OwnedQuery second(nullptr, dxmt9c_query_release);
    const auto savedIdentity = query->wireIdentity;
    if (variant == 0u) inputs[0].query = nullptr;
    if (variant == 1u) inputs[0].query = wrongQuery.get();
    if (variant == 2u) ++query->wireIdentity.generation;
    if (variant == 3u) inputs[0].snapshot.owner = &foreign.device->dev();
    if (variant == 4u) inputs[0].snapshot.backend = foreign.device->dev().upperDevice();
    if (variant == 5u) ++inputs[0].snapshot.identity;
    if (variant == 6u) inputs[0].snapshot.issueOrdinal = 2u;
    if (variant == 7u) ++inputs[0].snapshot.issueDeviceEpoch;
    if (variant == 8u) inputs[0].snapshot.active = true;
    if (variant == 9u) inputs[0].snapshot.failed = true;
    if (variant == 10u) inputs[0].snapshot.eligible = false;
    if (variant == 11u) inputs[0].snapshot.backendFence.prefix.initializer = 1u;
    if (variant == 12u) inputs.push_back(inputs[0]);
    if (variant == 13u)
      check(dxmt9c_query_issue(query.get(), 1u) == core::D3D_OK,
          "rejection trace genuinely replaces the public issue after retaining setup receipt");
    if (variant == 14u) {
      auto marker = std::make_shared<query::EndMarker>(
          query::WriterIdentity{snapshot.backendFence.prefix.queue, 1u, 1u, 1u});
      marker->cancel();
      inputs[0].snapshot.backendFence.marker = std::move(marker);
    }
    if (variant == 16u) {
      fixture.backend->incompleteReceipt = true;
      second = makeEvent(fixture);
      inputs.push_back({second.get(), fixture.device->dev().captureQueryPollSnapshot(second->obj)});
    }
    if (variant == 17u) {
      core::PresentParameters params{};
      params.backBufferWidth = params.backBufferHeight = 8u;
      params.backBufferFormat = core::Format::A8R8G8B8;
      params.windowed = true;
      params.deviceWindow = core::Handle{1u};
      check(fixture.device->dev().reset(params) == core::D3D_OK,
          "actual reset invalidates the retained setup issue epoch");
    }
    D9CRenderTapeIdentityCaptureV4ArmResult arm{};
    check(beginRenderTapeIdentityCaptureV4ForReplay(fixture.device.get(), 61u, 62u,
        inputs, &arm) == core::D3DERR_NOTAVAILABLE &&
        arm.status == D9C_RENDER_TAPE_IDENTITY_CAPTURE_FAILED && !arm.queue &&
        !acquireRenderTapeRuntimeIdentityV4(fixture.device.get()) &&
        !fixture.device->renderTapeRuntimeBindingV4 && !fixture.queue->acquireTapeRuntimeCapture(),
        "invalid/duplicate/stale/foreign/incomplete setup batch leaves no exposed root or installed observer");
    query->wireIdentity = savedIdentity;
    fixture.backend->incompleteReceipt = false;
    check(dxmt9c_device_begin_render_tape_identity_capture_v4(fixture.device.get(),
        63u, 64u, &arm) == core::D3D_OK,
        "failed private admission cleans up even when actual root seeding rejects after install");
    dxmt9c_device_cancel_render_tape_identity_capture_v4(fixture.device.get(), 63u, 64u, arm.queue);
  }
}

void bindingContract() {
  static_assert(sizeof(D9CRenderTapeIdentityCaptureV4ArmResult) == 32u);
  static_assert(sizeof(D9CRenderTapeIdentityCaptureV4Result) == 48u);
  Fixture fixture;
  auto* device = fixture.device.get();
  check(!acquireRenderTapeRuntimeIdentityV4(device) &&
      !fixture.queue->acquireTapeRuntimeCapture(), "unarmed capture has no retained root/lease");
  D9CRenderTapeIdentityCaptureV4ArmResult arm{};
  check(dxmt9c_device_begin_render_tape_identity_capture_v4(nullptr, 5u, 6u, &arm) ==
      core::D3DERR_INVALIDCALL && arm.status == D9C_RENDER_TAPE_IDENTITY_CAPTURE_FAILED,
      "null arm is invalid and exposes no success receipt");
  check(dxmt9c_device_begin_render_tape_identity_capture_v4(device, 5u, 6u, nullptr) ==
      core::D3DERR_INVALIDCALL, "null arm output is invalid");
  check(dxmt9c_device_begin_render_tape_identity_capture_v4(device, 0u, 6u, &arm) ==
      core::D3DERR_INVALIDCALL, "zero token is invalid");
  check(dxmt9c_device_begin_render_tape_identity_capture_v4(device, 5u, 6u, &arm) ==
      core::D3D_OK && arm.status == D9C_RENDER_TAPE_IDENTITY_CAPTURE_COMPLETE &&
      arm.version == 4u && arm.captureToken == 5u && arm.captureEpoch == 6u && arm.queue,
      "arm binds exact key to a real quiescent queue");
  const auto queue = arm.queue;
  const auto oldRoot = acquireRenderTapeRuntimeIdentityV4(device);
  const auto oldLease = fixture.queue->acquireTapeRuntimeCapture();
  check(oldRoot && oldLease.active(), "queue and device retain the same live arm");
  check(dxmt9c_device_begin_render_tape_identity_capture_v4(device, 5u, 7u, &arm) ==
      core::D3DERR_NOTAVAILABLE, "second arm cannot replace current capture");
  check(dxmt9c_device_note_render_tape_identity_cut_v4(device, 5u, 6u, queue, 1u,
      static_cast<std::uint32_t>(RenderTapeIdentityV3CutKind::AfterRecord)) ==
      core::D3DERR_INVALIDCALL, "external API cannot fabricate a replay record cut");
  check(dxmt9c_device_note_render_tape_identity_cut_v4(device, 5u, 7u, queue, 1u, 1u) ==
      core::D3DERR_NOTAVAILABLE, "stale epoch cannot change cursor");
  check(dxmt9c_device_note_render_tape_identity_cut_v4(device, 5u, 6u, queue + 1u, 1u, 1u) ==
      core::D3DERR_NOTAVAILABLE, "untrusted queue cannot change cursor");
  dxmt9c_device_cancel_render_tape_identity_capture_v4(device, 5u, 7u, queue);
  check(acquireRenderTapeRuntimeIdentityV4(device) == oldRoot && oldLease.active(),
      "stale cancel cannot revoke the actual arm");
  check(dxmt9c_device_note_render_tape_identity_cut_v4(device, 5u, 6u, queue, 1u, 1u) ==
      core::D3D_OK, "actual before-event boundary is admitted");
  check(dxmt9c_device_note_render_tape_identity_cut_v4(device, 5u, 6u, queue, 1u, 3u) ==
      core::D3D_OK, "actual after-event boundary is admitted");
  D9CRenderTapeIdentityCaptureV4Result queried{}, copied{};
  check(dxmt9c_device_finish_render_tape_identity_capture_v4(nullptr, 5u, 6u, queue,
      1u, &queried, nullptr, 0u) == core::D3DERR_INVALIDCALL &&
      queried.status == D9C_RENDER_TAPE_IDENTITY_CAPTURE_FAILED,
      "null finish is invalid and exposes no success receipt");
  check(dxmt9c_device_finish_render_tape_identity_capture_v4(device, 5u, 6u, queue,
      1u, nullptr, nullptr, 0u) == core::D3DERR_INVALIDCALL, "null finish output is invalid");
  check(dxmt9c_device_finish_render_tape_identity_capture_v4(device, 5u, 7u, queue,
      1u, &queried, nullptr, 0u) == core::D3DERR_NOTAVAILABLE, "stale finish fails closed");
  check(dxmt9c_device_finish_render_tape_identity_capture_v4(device, 7u, 6u, queue,
      1u, &queried, nullptr, 0u) == core::D3DERR_NOTAVAILABLE &&
      dxmt9c_device_finish_render_tape_identity_capture_v4(device, 5u, 6u, queue + 1u,
          1u, &queried, nullptr, 0u) == core::D3DERR_NOTAVAILABLE &&
      !device->renderTapeRuntimeBindingV4->actionWitnessFrozen,
      "wrong capture token or queue cannot sample/freeze another binding's policy witness");
  check(dxmt9c_device_finish_render_tape_identity_capture_v4(device, 5u, 6u, queue,
      1u, &queried, nullptr, 1u) == core::D3DERR_INVALIDCALL,
      "nonzero capacity requires a destination");
  check(dxmt9c_device_finish_render_tape_identity_capture_v4(device, 5u, 6u, queue,
      1u, &queried, nullptr, 0u) == core::D3D_OK &&
      queried.status == D9C_RENDER_TAPE_IDENTITY_CAPTURE_COMPLETE && queried.version == 4u &&
      queried.captureToken == 5u && queried.captureEpoch == 6u && queried.queue == queue &&
      queried.presentOrdinal == 1u && queried.byteCount == sizeof(RenderTapeIdentityV4TransferHeader),
      "genuine finish freezes paired empty-ledger size receipt without a Metal fence");
  check(!oldLease.active() && !fixture.queue->acquireTapeRuntimeCapture(),
      "frozen capture closes retained endpoint admission");
  std::vector<std::byte> bytes(queried.byteCount);
  check(dxmt9c_device_finish_render_tape_identity_capture_v4(device, 5u, 6u, queue,
      1u, &copied, bytes.data(), bytes.size() - 1u) == core::D3DERR_INVALIDCALL,
      "copy requires exact frozen size");
  check(dxmt9c_device_finish_render_tape_identity_capture_v4(device, 5u, 6u, queue,
      2u, &copied, bytes.data(), bytes.size()) == core::D3DERR_NOTAVAILABLE,
      "frozen receipt cannot be rebound to another Present");
  check(dxmt9c_device_finish_render_tape_identity_capture_v4(device, 5u, 6u, queue,
      1u, &copied, bytes.data(), bytes.size()) == core::D3D_OK &&
      std::memcmp(&copied, &queried, sizeof(copied)) == 0,
      "copy reproduces every scalar of the size-query receipt");
  RenderTapeIdentityV4View identity;
  RenderTapeExecutionTopologyV1View topology;
  RenderTapeActionWitnessV1 witness;
  check(decodeRenderTapeIdentityV4SnapshotTransfer(
      bytes, {5u, 6u}, queue, identity, topology, &witness) ==
      RenderTapeIdentityV4TransferStatus::Valid && identity.header.queue == queue &&
      topology.header.queue == queue &&
      renderTapeActionWitnessV1MatchesBinding(witness, 5u, 6u, queue) &&
      witness.constructionProfile == 0u && witness.armProfile == 0u &&
      witness.freezeProfile == 0u && !witness.armRevision && !witness.freezeRevision &&
      !renderTapeActionWitnessV1StableExact(witness),
      "pair carries genuine inert-queue normal policy, bound only to the trusted key/queue");
  const auto frozenBytes = bytes;
  fixture.queue->setRenderTapeExactAttachmentPreservation(true);
  fixture.queue->setRenderTapeExactAttachmentPreservation(false);
  check(fixture.queue->renderTapeActionProfileSnapshot().revision == 2u,
      "post-freeze queue policy genuinely changes through ABA");
  D9CRenderTapeIdentityCaptureV4Result repeated{};
  check(dxmt9c_device_finish_render_tape_identity_capture_v4(device, 5u, 6u, queue,
      1u, &repeated, nullptr, 0u) == core::D3D_OK &&
      std::memcmp(&repeated, &queried, sizeof(repeated)) == 0 &&
      dxmt9c_device_finish_render_tape_identity_capture_v4(device, 5u, 6u, queue,
          1u, &repeated, bytes.data(), bytes.size()) == core::D3D_OK &&
      bytes == frozenBytes && device->renderTapeRuntimeBindingV4->actionWitness == witness,
      "repeated frozen size/copy retains the witnessed policy bytes without resampling");
  // Model an allocation/transfer failure after observation by retaining the
  // actual frozen root/witness but removing the not-yet-committed receipt.
  device->renderTapeRuntimeBindingV4->receipt.reset();
  device->renderTapeRuntimeBindingV4->transfer.clear();
  check(dxmt9c_device_finish_render_tape_identity_capture_v4(device, 5u, 6u, queue,
      2u, &repeated, nullptr, 0u) == core::D3DERR_NOTAVAILABLE &&
      !device->renderTapeRuntimeBindingV4->receipt &&
      device->renderTapeRuntimeBindingV4->transfer.empty() &&
      device->renderTapeRuntimeBindingV4->frozenPresentOrdinal == 1u &&
      device->renderTapeRuntimeBindingV4->actionWitness == witness,
      "post-observation transfer retry cannot bind a frozen root/witness to another Present");
  check(dxmt9c_device_finish_render_tape_identity_capture_v4(device, 5u, 6u, queue,
      1u, &repeated, bytes.data(), bytes.size()) == core::D3D_OK &&
      bytes == frozenBytes && std::memcmp(&repeated, &queried, sizeof(repeated)) == 0,
      "same-Present transfer retry reconstructs only the original immutable frozen value");
  dxmt9c_device_cancel_render_tape_identity_capture_v4(device, 5u, 6u, queue);
  check(!acquireRenderTapeRuntimeIdentityV4(device) && !oldRoot->edges().active(),
      "matching cancel clears ownership while retained old root stays revoked");
  check(dxmt9c_device_begin_render_tape_identity_capture_v4(device, 7u, 8u, &arm) ==
      core::D3D_OK && arm.queue == queue &&
      acquireRenderTapeRuntimeIdentityV4(device) != oldRoot &&
      device->renderTapeRuntimeBindingV4->armActionProfile.revision == 2u &&
      !device->renderTapeRuntimeBindingV4->actionWitnessFrozen &&
      !device->renderTapeRuntimeBindingV4->frozenPresentOrdinal,
      "new arm on same queue has a fresh root and cannot revive old leases");
  dxmt9c_device_cancel_render_tape_identity_capture_v4(device, 5u, 6u, queue);
  check(acquireRenderTapeRuntimeIdentityV4(device)->matches({7u, 8u}),
      "old cancellation never targets replacement capture");
  dxmt9c_device_cancel_render_tape_identity_capture_v4(device, 7u, 8u, queue);
  dxmt9c_device_cancel_render_tape_identity_capture_v4(nullptr, 7u, 8u, queue);
}

void actionWitnessHistoryMapping() {
  for (const auto constructionExact : {false, true}) {
    for (std::uint32_t armChanges = 0u; armChanges < 5u; ++armChanges) {
      for (std::uint32_t freezeChanges = 0u; freezeChanges < 5u; ++freezeChanges) {
        RenderTapeActionProfileHistory armHistory, freezeHistory;
        check(armHistory.initialize(constructionExact) &&
            freezeHistory.initialize(constructionExact),
            "pure binding grid starts with the production queue reducer's construction policy");
        for (std::uint32_t change = 1u; change <= armChanges; ++change)
          armHistory.set(constructionExact != ((change & 1u) != 0u));
        for (std::uint32_t change = 1u; change <= freezeChanges; ++change)
          freezeHistory.set(constructionExact != ((change & 1u) != 0u));
        RenderTapeActionWitnessV1 witness;
        const auto accepted = renderTapeRuntimeBindingV4BuildActionWitness(
            armHistory.snapshot(), freezeHistory.snapshot(), 101u, 102u, 103u, witness);
        check(accepted == (freezeChanges >= armChanges),
            "binding mapper rejects backward history and preserves forward binary-policy changes");
        if (!accepted) {
          check(witness == RenderTapeActionWitnessV1{},
              "invalid history exposes no invented queue-observed witness");
          continue;
        }
        check(renderTapeActionWitnessV1Valid(witness) &&
            renderTapeActionWitnessV1MatchesBinding(witness, 101u, 102u, 103u) &&
            witness.constructionProfile == static_cast<std::uint32_t>(constructionExact) &&
            witness.armProfile == armHistory.snapshot().currentProfile &&
            witness.freezeProfile == freezeHistory.snapshot().currentProfile &&
            witness.armRevision == armChanges && witness.freezeRevision == freezeChanges &&
            renderTapeActionWitnessV1StableExact(witness) ==
                (constructionExact && armChanges == 0u && freezeChanges == 0u),
            "known diagnostics retain pre-arm changes/ABA while only construction-exact stable history qualifies");
        check(!renderTapeActionWitnessV1MatchesBinding(witness, 104u, 102u, 103u) &&
            !renderTapeActionWitnessV1MatchesBinding(witness, 101u, 104u, 103u) &&
            !renderTapeActionWitnessV1MatchesBinding(witness, 101u, 102u, 104u),
            "witness binding cannot borrow a token/epoch or queue from another capture");
      }
    }
  }
  RenderTapeActionProfileHistory history;
  check(history.initialize(true), "incoherent-grid setup constructs exact policy truthfully");
  const auto exact = history.snapshot();
  for (std::uint32_t variant = 0u; variant < 10u; ++variant) {
    auto arm = exact, freeze = exact;
    auto token = 101ull, epoch = 102ull, queue = 103ull;
    if (variant == 0u) arm.valid = false;
    if (variant == 1u) freeze.valid = false;
    if (variant == 2u) arm.version = 2u;
    if (variant == 3u) freeze.currentProfile = 2u;
    if (variant == 4u) freeze.constructionProfile = 2u;
    if (variant == 5u) arm.revision = 1u;
    if (variant == 6u) freeze = {.constructionProfile = 0u, .currentProfile = 0u, .valid = true};
    if (variant == 7u) token = 0u;
    if (variant == 8u) epoch = 0u;
    if (variant == 9u) queue = 0u;
    RenderTapeActionWitnessV1 out{
        .evidence = 1u, .constructionProfile = 1u, .armProfile = 1u,
        .freezeProfile = 1u, .captureToken = 101u, .captureEpoch = 102u, .queue = 103u};
    check(!renderTapeRuntimeBindingV4BuildActionWitness(arm, freeze, token, epoch, queue, out) &&
        out == RenderTapeActionWitnessV1{},
        "invalid/untrusted policy observations clear prior output rather than inventing observed evidence");
  }
}

void actionWitnessUsesActualQueueHistory() {
  for (std::uint32_t variant = 0u; variant < 6u; ++variant) {
    Fixture fixture;
    auto* device = fixture.device.get();
    if (variant == 1u || variant == 5u)
      fixture.queue->setRenderTapeExactAttachmentPreservation(true);
    if (variant == 2u) {
      fixture.queue->setRenderTapeExactAttachmentPreservation(true);
      fixture.queue->setRenderTapeExactAttachmentPreservation(false);
    }
    const auto armProfile = fixture.queue->renderTapeActionProfileSnapshot();
    const auto token = 201u + variant;
    D9CRenderTapeIdentityCaptureV4ArmResult arm{};
    check(dxmt9c_device_begin_render_tape_identity_capture_v4(device, token, 211u, &arm) ==
        core::D3D_OK && device->renderTapeRuntimeBindingV4->captureToken == token &&
        device->renderTapeRuntimeBindingV4->captureEpoch == 211u &&
        device->renderTapeRuntimeBindingV4->armActionProfile.revision == armProfile.revision &&
        device->renderTapeRuntimeBindingV4->armActionProfile.currentProfile == armProfile.currentProfile,
        "trusted arm samples the actual construction/current queue history under its exact key");
    if (variant == 3u || variant == 4u)
      fixture.queue->setRenderTapeExactAttachmentPreservation(true);
    if (variant == 4u)
      fixture.queue->setRenderTapeExactAttachmentPreservation(false);
    if (variant == 5u) {
      fixture.queue->setRenderTapeExactAttachmentPreservation(false);
      fixture.queue->setRenderTapeExactAttachmentPreservation(true);
    }
    const auto freezeProfile = fixture.queue->renderTapeActionProfileSnapshot();
    check(dxmt9c_device_note_render_tape_identity_cut_v4(device, token, 211u, arm.queue,
        1u, 1u) == core::D3D_OK &&
        dxmt9c_device_note_render_tape_identity_cut_v4(device, token, 211u, arm.queue,
            1u, 3u) == core::D3D_OK, "actual cold cuts close the inert history fixture");
    D9CRenderTapeIdentityCaptureV4Result receipt{};
    check(dxmt9c_device_finish_render_tape_identity_capture_v4(device, token, 211u,
        arm.queue, 1u, &receipt, nullptr, 0u) == core::D3D_OK,
        "known normal/pre-arm-changed/interval-changed histories remain legal diagnostic captures");
    std::vector<std::byte> bytes(receipt.byteCount);
    check(dxmt9c_device_finish_render_tape_identity_capture_v4(device, token, 211u,
        arm.queue, 1u, &receipt, bytes.data(), bytes.size()) == core::D3D_OK,
        "actual action witness is copied from the frozen production binding");
    RenderTapeIdentityV4View identity;
    RenderTapeExecutionTopologyV1View topology;
    RenderTapeActionWitnessV1 witness;
    check(decodeRenderTapeIdentityV4SnapshotTransfer(bytes, {token, 211u}, arm.queue,
        identity, topology, &witness) == RenderTapeIdentityV4TransferStatus::Valid &&
        renderTapeActionWitnessV1MatchesBinding(witness, token, 211u, arm.queue) &&
        witness.constructionProfile == 0u && witness.armProfile == armProfile.currentProfile &&
        witness.freezeProfile == freezeProfile.currentProfile &&
        witness.armRevision == armProfile.revision && witness.freezeRevision == freezeProfile.revision &&
        !renderTapeActionWitnessV1StableExact(witness),
        "constructor-normal queues never acquire stable-exact qualification, including endpoint-exact ABA");
    dxmt9c_device_cancel_render_tape_identity_capture_v4(device, token, 211u, arm.queue);
  }
}

void invalidActionHistoryFailsClosedAndCannotResample() {
  for (std::uint32_t variant = 0u; variant < 7u; ++variant) {
    Fixture fixture;
    auto* device = fixture.device.get();
    D9CRenderTapeIdentityCaptureV4ArmResult arm{};
    check(dxmt9c_device_begin_render_tape_identity_capture_v4(device, 301u, 302u, &arm) ==
        core::D3D_OK, "invalid-history fixture begins with an actual coherent queue");
    auto& binding = *device->renderTapeRuntimeBindingV4;
    const auto originalArm = binding.armActionProfile;
    if (variant == 0u) binding.armActionProfile.valid = false;
    if (variant == 1u) binding.armActionProfile.version = 2u;
    if (variant == 2u) binding.armActionProfile.currentProfile = 2u;
    if (variant == 3u) binding.armActionProfile.constructionProfile = 2u;
    if (variant == 4u) binding.armActionProfile.revision = 1u;
    if (variant == 5u) binding.armActionProfile.revision = 2u;
    if (variant == 6u) {
      binding.armActionProfile.constructionProfile = 1u;
      binding.armActionProfile.currentProfile = 1u;
    }
    check(dxmt9c_device_note_render_tape_identity_cut_v4(device, 301u, 302u, arm.queue,
        1u, 1u) == core::D3D_OK &&
        dxmt9c_device_note_render_tape_identity_cut_v4(device, 301u, 302u, arm.queue,
            1u, 3u) == core::D3D_OK, "fault fixture still closes the real capture root normally");
    D9CRenderTapeIdentityCaptureV4Result receipt{};
    check(dxmt9c_device_finish_render_tape_identity_capture_v4(device, 301u, 302u,
        arm.queue, 1u, &receipt, nullptr, 0u) == core::D3DERR_NOTAVAILABLE &&
        receipt.status == D9C_RENDER_TAPE_IDENTITY_CAPTURE_FAILED && !receipt.byteCount &&
        binding.actionWitnessFrozen && binding.frozenPresentOrdinal == 1u &&
        !binding.receipt && binding.transfer.empty() &&
        binding.actionWitness == RenderTapeActionWitnessV1{} &&
        !fixture.queue->acquireTapeRuntimeCapture(),
        "invalid arm/freeze history fails after retirement without publishing invented observed bytes");
    binding.armActionProfile = originalArm;
    fixture.queue->setRenderTapeExactAttachmentPreservation(true);
    check(dxmt9c_device_finish_render_tape_identity_capture_v4(device, 301u, 302u,
        arm.queue, 1u, &receipt, nullptr, 0u) == core::D3DERR_NOTAVAILABLE &&
        !binding.receipt && binding.transfer.empty() &&
        binding.actionWitness == RenderTapeActionWitnessV1{},
        "restoring the arm snapshot or changing queue policy cannot resample a failed frozen observation");
    dxmt9c_device_cancel_render_tape_identity_capture_v4(device, 301u, 302u, arm.queue);
    check(dxmt9c_device_begin_render_tape_identity_capture_v4(device, 303u, 304u, &arm) ==
        core::D3D_OK && !device->renderTapeRuntimeBindingV4->actionWitnessFrozen &&
        device->renderTapeRuntimeBindingV4->armActionProfile.currentProfile == 1u &&
        device->renderTapeRuntimeBindingV4->armActionProfile.revision == 1u,
        "cancel/re-arm discards failed binding evidence and observes fresh real diagnostic policy history");
    dxmt9c_device_cancel_render_tape_identity_capture_v4(device, 303u, 304u, arm.queue);
  }
}

void retainedQueueRetirement() {
  Fixture fixture;
  auto* device = fixture.device.get();
  D9CRenderTapeIdentityCaptureV4ArmResult arm{};
  check(dxmt9c_device_begin_render_tape_identity_capture_v4(device, 11u, 12u, &arm) ==
      core::D3D_OK, "retirement regression arms the actual retained queue");
  const auto lease = fixture.queue->acquireTapeRuntimeCapture();
  check(lease.active(), "retirement regression acquires the actual retained lease");
  const auto expected = acquireRenderTapeRuntimeIdentityV4(device);
  RenderTapeRuntimeReplayAccessV4 access(expected.get(), {11u, 2u, 1u, 0u}, lease);
  std::mutex latchMutex;
  std::condition_variable latch;
  bool entered = false;
  bool admitted = false;
  bool visitorFinished = false;
  bool queueCompleted = false;
  bool clockRevoked = false;
  std::thread visitor([&] {
    admitted = access.visit([&](RenderTapeRuntimeIdentityV4& root) noexcept {
      {
        std::lock_guard lock(latchMutex);
        entered = true;
      }
      latch.notify_one();
      // Cancellation must close admission before joining this visit. After
      // that witnessed transition, acquiring the real queue mutex must still
      // be possible: joining while holding it would deadlock this trace.
      while (lease.active()) std::this_thread::yield();
      clockRevoked = !root.edges().active();
      queueCompleted = fixture.queue->hasCompletedPendingWork();
      visitorFinished = true;
      return true;
    });
  });
  {
    std::unique_lock lock(latchMutex);
    latch.wait(lock, [&] { return entered; });
  }
  dxmt9c_device_cancel_render_tape_identity_capture_v4(device, 11u, 12u, arm.queue);
  visitor.join();
  check(admitted && visitorFinished && queueCompleted && clockRevoked,
      "retirement joins admitted replay-facade visitor without holding queue mutex");
  check(!lease.active() && !fixture.queue->acquireTapeRuntimeCapture() &&
      !acquireRenderTapeRuntimeIdentityV4(device),
      "joined retirement leaves revoked lease and no current capture root");
}

} // namespace

int main() {
  try {
    bindingContract();
    actionWitnessHistoryMapping();
    actionWitnessUsesActualQueueHistory();
    invalidActionHistoryFailsClosedAndCannotResample();
    replayBaselineAdmission();
    replayBaselineRejections();
    retainedQueueRetirement();
    std::cout << "render tape runtime binding v4: " << checks << " checks\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
