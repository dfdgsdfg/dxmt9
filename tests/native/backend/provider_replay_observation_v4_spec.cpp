#include "device_c_chunk_replay.hpp"
#include "device_c_common.hpp"
#include "device_c_query_range_route.hpp"
#include "device_c_render_tape_runtime_binding_v4.hpp"
#include "d3d9_pe_chunk_builder.hpp"
#include "dxmt9/dxmt9_device.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
using namespace dxmt9;
using namespace dxmt9::d3d9;
std::uint64_t checks = 0u;

void check(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}

struct Upper final : Device {
  Upper() : queue_(CommandQueue::ArenaLeaseTestQueueTag{}, limits_) {}
  WMT::Device wmtDevice() override { return WMT::Device{NULL_OBJECT_HANDLE}; }
  CommandQueue& queue() override { return queue_; }
  const core::BackendLimits& limits() const override { return limits_; }
  std::shared_ptr<core::BackendDevice> backend() override { return {}; }
  core::BackendLimits limits_{};
  CommandQueue queue_;
};

struct Fixture {
  Fixture() {
    factory = com::Direct3DCreate9Ex(com::D3D_SDK_VERSION, std::make_unique<Upper>());
    check(factory != nullptr, "observation fixture factory exists");
    core::PresentParameters params{};
    params.backBufferWidth = params.backBufferHeight = 8u;
    params.backBufferFormat = core::Format::A8R8G8B8;
    params.windowed = true;
    params.deviceWindow = core::Handle{1u};
    params.presentationInterval = core::PresentInterval::Immediate;
    iface = factory->CreateDeviceEx(0u, params, nullptr);
    check(iface != nullptr, "observation fixture device exists");
    iface->AddRef();
    device = std::make_unique<D9CDevice>(iface);
  }
  ~Fixture() {
    device.reset();
    if (iface) iface->Release();
    if (factory) factory->Release();
  }
  com::IDirect3D9Ex* factory = nullptr;
  com::IDirect3DDevice9Ex* iface = nullptr;
  std::unique_ptr<D9CDevice> device{};
};

struct Canonical {
  Canonical() {
    pe::CommandChunkBuilder builder;
    const std::array first{1.0f, 2.0f, 3.0f, 4.0f};
    const std::array second{5.0f, 6.0f, 7.0f, 8.0f};
    check(pe::appendSetConstants(builder, D9C_COMMAND_RECORD_SET_VS_CONST_F,
              0u, 1u, std::as_bytes(std::span(first))) &&
        pe::appendSetConstants(builder, D9C_COMMAND_RECORD_SET_VS_CONST_F,
              0u, 1u, std::as_bytes(std::span(second))),
        "original canonical state records append");
    const auto sealed = builder.seal();
    check(sealed.valid() && sealed.recordCount == 2u && !sealed.handleCount,
        "original canonical two-record raw seals");
    bytes.assign(sealed.blob.begin(), sealed.blob.end());
    envelope.recordCount = sealed.recordCount;
  }
  std::vector<std::byte> bytes{};
  CommandChunkEnvelope envelope{};
};

struct Probe {
  enum Phase : std::uint32_t { Started, Before, Dispatch, After, Settled };
  std::array<Phase, 8u> phases{};
  std::size_t used = 0u;
  std::uint64_t raw = 0u;
  bool correct = true;
  bool rejectDispatch = false;
  std::shared_ptr<RenderTapeRuntimeIdentityV4> root{};
  D9CDevice* cancelDevice = nullptr;
  D9CRenderTapeIdentityCaptureV4ArmResult cancelArm{};

  bool note(Phase phase) noexcept {
    if (used == phases.size()) { correct = false; return false; }
    phases[used++] = phase;
    return true;
  }
  ProviderReplayObservationV4 observation(const D9CRenderTapeIdentityCaptureV4ArmResult& arm) {
    return {.captureToken = arm.captureToken, .captureEpoch = arm.captureEpoch,
        .queue = arm.queue, .eventOrdinal = 17u, .context = this,
        .rawStarted = [](void* pointer, std::uint64_t event, std::uint64_t ordinal,
            std::uint32_t count) noexcept {
          auto& self = *static_cast<Probe*>(pointer);
          self.correct &= event == 17u && ordinal && count == 2u;
          self.raw = ordinal;
          if (self.cancelDevice)
            dxmt9c_device_cancel_render_tape_identity_capture_v4(self.cancelDevice,
                self.cancelArm.captureToken, self.cancelArm.captureEpoch, self.cancelArm.queue);
          return self.note(Started) && self.correct;
        },
        .beforeRecord = [](void* pointer, std::uint32_t index, std::uint32_t type) noexcept {
          auto& self = *static_cast<Probe*>(pointer);
          self.correct &= index < 2u && type == D9C_COMMAND_RECORD_SET_VS_CONST_F;
          return self.note(Before) && self.correct;
        },
        .beforeDispatch = [](void* pointer, std::uint32_t index, std::uint32_t type) noexcept {
          auto& self = *static_cast<Probe*>(pointer);
          self.correct &= index < 2u && type == D9C_COMMAND_RECORD_SET_VS_CONST_F;
          return self.note(Dispatch) && self.correct && !self.rejectDispatch;
        },
        .recordDispatched = [](void* pointer, std::uint32_t index, std::uint32_t type,
            std::int32_t result) noexcept {
          auto& self = *static_cast<Probe*>(pointer);
          self.correct &= index < 2u && type == D9C_COMMAND_RECORD_SET_VS_CONST_F &&
              result == core::D3D_OK;
          return self.note(After) && self.correct;
        },
        .rawSettled = [](void* pointer, std::uint64_t ordinal, std::uint32_t count,
            bool succeeded) noexcept {
          auto& self = *static_cast<Probe*>(pointer);
          self.correct &= ordinal == self.raw && count == 2u && succeeded;
          if (!self.rejectDispatch) {
            RenderTapeIdentityV4View identity;
            RenderTapeExecutionTopologyV1View topology;
            self.correct &= self.root->snapshot({5u, 6u}, identity, topology, false) &&
                identity.settlements.size() == 1u &&
                identity.settlements[0u].eventOrdinal == 17u &&
                identity.settlements[0u].rawOrdinal == ordinal;
          }
          return self.note(Settled) && self.correct;
        }};
  }
};

void observedReplay(bool rejectDispatch, bool stale) {
  Fixture fixture;
  Canonical canonical;
  auto* device = fixture.device.get();
  D9CRenderTapeIdentityCaptureV4ArmResult arm{};
  check(dxmt9c_device_begin_render_tape_identity_capture_v4(device, 5u, 6u, &arm) ==
      core::D3D_OK, "observation starts from real independently local arm");
  Probe probe;
  probe.rejectDispatch = rejectDispatch;
  probe.root = acquireRenderTapeRuntimeIdentityV4(device);
  auto observation = probe.observation(arm);
  if (stale) ++observation.captureEpoch;
  ProviderReplayObservationV4Result result;
  const auto hr = replayPrevalidatedResolvedCommandChunkObservedV4(device, canonical.bytes,
      canonical.envelope, {}, ProviderReplayPolicy::Compatibility, observation, result);
  check(hr == core::D3D_OK && device->dev().state().vsConst.float4[0u][0u] == 5.0f &&
      device->dev().state().vsConst.float4[0u][3u] == 8.0f &&
      !device->replayDrainLedger.terminal(),
      "stale/rejected observation preserves actual state effect, HRESULT and healthy ledger");
  check(result.ledgerSettlementObserved && result.localRawOrdinal,
      "actual ledger settlement is reported separately from qualification");
  if (stale) {
    check(!result.captureAccepted && !result.qualificationSucceeded && !probe.used &&
        !probe.root->failed(), "stale observer never borrows or fails another active arm");
  } else {
    const std::array expected{Probe::Started, Probe::Before, Probe::Dispatch, Probe::After,
        Probe::Before, Probe::Dispatch, Probe::After, Probe::Settled};
    const auto expectedCount = rejectDispatch ? 3u : expected.size();
    check(result.captureAccepted && result.qualificationSucceeded == !rejectDispatch &&
        probe.correct && probe.used == expectedCount &&
        std::equal(probe.phases.begin(), probe.phases.begin() + expectedCount, expected.begin()),
        "authentic callback order stops after rejection revokes capture admission");
    check(probe.root->failed() == rejectDispatch,
        "callback rejection fails only matching retained capture");
    if (!rejectDispatch) {
      RenderTapeIdentityV4View identity;
      RenderTapeExecutionTopologyV1View topology;
      check(probe.root->snapshot({5u, 6u}, identity, topology, false) &&
          identity.attempts.size() == 1u && identity.dispositions.size() == 2u &&
          identity.settlements.size() == 1u && identity.sources.empty(),
          "candidate uses real compatibility attempt/dispositions/ACK without forced publication");
    }
  }
  dxmt9c_device_cancel_render_tape_identity_capture_v4(device, 5u, 6u, arm.queue);
}

void retiredDuringCallback() {
  Fixture fixture;
  Canonical canonical;
  auto* device = fixture.device.get();
  D9CRenderTapeIdentityCaptureV4ArmResult arm{};
  check(dxmt9c_device_begin_render_tape_identity_capture_v4(device, 5u, 6u, &arm) ==
      core::D3D_OK, "retirement fixture admits real local arm");
  Probe probe;
  probe.root = acquireRenderTapeRuntimeIdentityV4(device);
  probe.cancelDevice = device;
  probe.cancelArm = arm;
  const auto observation = probe.observation(arm);
  ProviderReplayObservationV4Result result;
  // The callback synchronously cancels its own capture. No endpoint activity
  // or binding/root lock may span arbitrary callback code and cause self-join.
  const auto hr = replayPrevalidatedResolvedCommandChunkObservedV4(device, canonical.bytes,
      canonical.envelope, {}, ProviderReplayPolicy::Compatibility, observation, result);
  check(hr == core::D3D_OK && device->dev().state().vsConst.float4[0u][0u] == 5.0f &&
      !device->replayDrainLedger.terminal() && result.ledgerSettlementObserved &&
      result.localRawOrdinal, "retired capture preserves actual renderer/ledger success");
  check(result.captureAccepted && !result.qualificationSucceeded && probe.correct &&
      probe.used == 1u && probe.phases[0u] == Probe::Started && !probe.root->failed(),
      "callback cancellation suppresses all later diagnostic mutation without self-join");
  RenderTapeIdentityV4View identity;
  RenderTapeExecutionTopologyV1View topology;
  check(probe.root->snapshot({5u, 6u}, identity, topology, false) &&
      identity.attempts.empty() && identity.dispositions.empty() && identity.settlements.empty(),
      "retired retained root receives no replay attempt, disposition or synthetic ACK");
}

void legacyRouteVetoPolicy() {
  std::array<D9CCommandChunkWireRecordHeader, 2u> headers{{
      {.type = D9C_COMMAND_RECORD_APPLY_STATE},
      {.type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE}}};
  ImportedChunkView imported;
  imported.records = headers;
  for (const bool tagged : {false, true}) {
    for (const bool selected : {false, true}) {
      const bool legacy = legacyRenderTapeCaptureRouteRequested(tagged, selected);
      check(legacy == (tagged && !selected),
          "V4 version selection suppresses legacy veto independently of live admission");
      check(queryRangeCompatibilityShortcut(imported, true, legacy) == !legacy &&
          !queryRangeCompatibilityShortcut(imported, false, legacy),
          "V4 tags do not veto ordinary query-range shortcut or enable disabled policy");
    }
  }
  headers.back().type = D9C_COMMAND_RECORD_READBACK;
  check(!queryRangeCompatibilityShortcut(imported, true,
            legacyRenderTapeCaptureRouteRequested(true, true)),
      "route-neutral V4 capture never bypasses real ordered-control shortcut constraint");
}

void originalTaggedReplay(std::uint32_t scenario, bool planning) {
  Fixture fixture;
  Canonical canonical;
  auto* device = fixture.device.get();
  D9CRenderTapeIdentityCaptureV4ArmResult arm{};
  check(!device->renderTapeRuntimeV4Selected.load(std::memory_order_acquire) &&
      dxmt9c_device_begin_render_tape_identity_capture_v4(device, 0u, 6u, &arm) ==
          core::D3DERR_INVALIDCALL &&
      !device->renderTapeRuntimeV4Selected.load(std::memory_order_acquire),
      "failed arm cannot permanently select V4 capture");
  check(dxmt9c_device_begin_render_tape_identity_capture_v4(device, 5u, 6u, &arm) ==
      core::D3D_OK && device->renderTapeRuntimeV4Selected.load(std::memory_order_acquire),
      "successful real arm permanently selects route-neutral V4 capture");
  auto root = acquireRenderTapeRuntimeIdentityV4(device);
  if (scenario == 1u) {
    dxmt9c_device_cancel_render_tape_identity_capture_v4(device, 5u, 6u, arm.queue);
    D9CRenderTapeIdentityCaptureV4ArmResult nextArm{};
    check(dxmt9c_device_begin_render_tape_identity_capture_v4(device, 7u, 8u, &nextArm) ==
        core::D3D_OK && device->renderTapeRuntimeV4Selected.load(std::memory_order_acquire),
        "rearm keeps the device's already selected V4 route policy");
    dxmt9c_device_cancel_render_tape_identity_capture_v4(device, 7u, 8u, nextArm.queue);
  }
  if (scenario == 2u) {
    const auto lease = device->dev().upperDevice()->queue().acquireTapeRuntimeCapture();
    check(lease.visit([&](render::TapeRuntimeCaptureAttachment& admitted) noexcept {
      if (&admitted == root.get()) root->fail();
    }), "qualification failure uses real short capture admission");
  }
  const auto token = scenario == 3u ? 99u : 5u;
  // Seed only the archived value ledger as a sentinel. These rows are not
  // real queue/GPU evidence; V4 replay must neither append nor fail them.
  const std::array ranges{RenderTapeProductionPassRange{
      .firstRecord = 0u, .recordCount = 2u, .dagPassIndex = 0u, .passKind = 1u}};
  auto& legacy = device->renderTapeIdentityCapture;
  D9CRenderTapeIdentityCaptureResult before{}, after{};
  check(legacy.append(token, 1u, 1u, 1u, 2u, ranges) &&
      legacy.registerExpectedSettlement(token, 1u, 1u, 1u, 1u, 1u, 1u) &&
      legacy.completeSettlement(token, 1u) && legacy.copy(token, before, {}),
      "archived ledger sentinel is complete before original tagged replay");
  RawCommandChunk raw;
  check(prepareOffloadChunk(canonical.bytes, canonical.envelope, device->wireObjects,
            [](std::uint32_t, void*) noexcept {}, raw),
      "original raw passes actual owned preflight");
  raw.replaySeq = 17u;
  raw.cpuReadyTapePlanningEnabled = planning;
  raw.renderTapeCaptureToken = token;
  raw.renderTapeEventOrdinal = 17u;
  const auto hr = replayRawChunk(device, raw);
  check(hr == core::D3D_OK && device->dev().state().vsConst.float4[0u][0u] == 5.0f &&
      raw.renderTapeCaptureToken == token && raw.renderTapeEventOrdinal == 17u &&
      !device->replayDrainLedger.terminal(),
      "active/failed/stale/cancelled original V4 tags preserve real effects and identity");
  check(device->renderTapeRuntimeV4Selected.load(std::memory_order_acquire) &&
      legacy.copy(token, after, {}) && after.byteCount == before.byteCount &&
      after.sourceCount == before.sourceCount && after.rangeCount == before.rangeCount &&
      after.eventOrdinal == before.eventOrdinal && after.settlementCount == before.settlementCount,
      "V4 selection never downgrades to archived capture mutation or failure");
  check(root->failed() == (scenario == 2u || scenario == 3u),
      "unsupported/stale V4 qualification fails only its retained diagnostic root");
  dxmt9c_device_cancel_render_tape_identity_capture_v4(device, 5u, 6u, arm.queue);
  check(device->renderTapeRuntimeV4Selected.load(std::memory_order_acquire),
      "matching detach cannot clear permanent V4 route selection");
}

} // namespace

int main() {
  try {
    observedReplay(false, false);
    observedReplay(true, false);
    observedReplay(false, true);
    retiredDuringCallback();
    legacyRouteVetoPolicy();
    for (std::uint32_t scenario = 0u; scenario < 4u; ++scenario)
      for (const bool planning : {false, true}) originalTaggedReplay(scenario, planning);
    std::cout << "provider replay observation v4: " << checks << " checks\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
