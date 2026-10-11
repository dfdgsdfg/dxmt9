#include "device_c_render_tape_runtime_binding_v4.hpp"
#include "device_c_render_tape_controlled_cpu_v4.hpp"
#include "device_c_common.hpp"
#include "device_c_render_tape_identity_transfer_v4.hpp"
#include "device_c_render_tape_runtime_identity_core.hpp"
#include "device_c_replay_offload.hpp"
#include "../dxmt9/dxmt9_command_queue.hpp"
#include "../dxmt9/dxmt9_device.hpp"
#include "util/log/log.hpp"
#include "dxmt9/render_tape_attachment_history.hpp"

#include <cstring>
#include <limits>

namespace dxmt9::d3d9 {

struct RenderTapeControlledCpuV4Authority {
  mutable std::mutex mutex{};
  RenderTapeControlledCpuV4Phase phase = RenderTapeControlledCpuV4Phase::Available;
  std::atomic<bool> revoked{false};
  D9CDevice* device = nullptr;
  std::shared_ptr<dxmt9::Device> backend{};
  std::weak_ptr<void> fixtureLifetime{};
  void* probeContext = nullptr;
  bool (*probe)(void*, D9CDevice*, const dxmt9::Device&,
      RenderTapeControlledCpuV4Boundary) noexcept = nullptr;
  std::weak_ptr<RenderTapeRuntimeIdentityV4> root{};
  RenderTapeIdentityV4CaptureKey key{};
  std::uint64_t queue = 0u;
};

namespace {

using Root = RenderTapeRuntimeIdentityV4;
using Key = RenderTapeIdentityV4CaptureKey;

bool sameQueryIssue(const core::QueryPollSnapshot& a,
    const core::QueryPollSnapshot& b) noexcept {
  const auto x = a.backendFence.snapshot(), y = b.backendFence.snapshot();
  return a.owner == b.owner && a.backend == b.backend && a.identity == b.identity &&
      a.type == b.type && a.issueOrdinal == b.issueOrdinal &&
      a.issueDeviceEpoch == b.issueDeviceEpoch && a.active == b.active &&
      a.failed == b.failed && a.issuedSequenceId == b.issuedSequenceId &&
      a.backendFence.marker == b.backendFence.marker &&
      a.backendFence.prefix.queue == b.backendFence.prefix.queue &&
      a.backendFence.prefix.sequence == b.backendFence.prefix.sequence &&
      a.backendFence.prefix.initializer == b.backendFence.prefix.initializer &&
      x.queue == y.queue && x.sequence == y.sequence && x.initializer == y.initializer;
}

struct InstalledCaptureGuard {
  dxmt9::Device& upper;
  Root& root;
  bool installed = false;
  ~InstalledCaptureGuard() {
    if (installed) upper.queue().revokeTapeRuntimeObserver(root.sink());
  }
};

struct RetainedArmSurfaces {
  std::vector<WireObjectRegistry::RetainedEntry> entries;
  ~RetainedArmSurfaces() {
    for (const auto& entry : entries)
      dxmt9c_surface_release(static_cast<D9CSurface*>(entry.object));
  }
  static void retain(std::uint32_t, void* object) noexcept {
    dxmt9c_surface_addref(static_cast<D9CSurface*>(object));
  }
};

std::int32_t beginCapture(D9CDevice* device, std::uint64_t token,
    std::uint64_t epoch, std::span<const RenderTapeReplayQueryBaselineV4> queries,
    bool replay, D9CRenderTapeIdentityCaptureV4ArmResult* out,
    const std::shared_ptr<RenderTapeControlledCpuV4Authority>& authority = {}) noexcept {
  if (!out) return core::D3DERR_INVALIDCALL;
  *out = {};
  out->status = D9C_RENDER_TAPE_IDENTITY_CAPTURE_FAILED;
  if (!device || !token || !epoch) return core::D3DERR_INVALIDCALL;
  // Retain outside the binding lock, including failed/exceptional installs.
  std::shared_ptr<Root> root;
  RetainedArmSurfaces surfaces;
  try {
    std::lock_guard lock(device->renderTapeRuntimeIdentityV4Mutex);
    if (acquireRenderTapeRuntimeIdentityV4(device) || device->renderTapeRuntimeBindingV4 ||
        queries.size() > RenderTapeRuntimeIdentityV4Limits{}.provenance.maxCoordinators ||
        !drainDeferredReplay(device, "render-tape-v4-arm")) return core::D3DERR_NOTAVAILABLE;
    const auto upper = device->dev().upperDevice();
    if (!upper) return core::D3DERR_NOTAVAILABLE;
    std::vector<RenderTapeRuntimeQueryBaselineV4> baselines;
    baselines.reserve(queries.size());
    for (const auto& input : queries) {
      const auto* query = input.query;
      const auto& snapshot = input.snapshot;
      // Replay creates a fresh EVENT and performs exactly one real setup END.
      // Reject reissue/reset/history substitution rather than adopting it.
      if (!query || query->device != device || !query->obj ||
          !device->wireObjects.contains(query->wireIdentity, query) ||
          snapshot.owner != &device->dev() || snapshot.backend != upper ||
          snapshot.type != core::QueryType::Event || !snapshot.identity ||
          snapshot.identity != query->obj->identity() || snapshot.issueOrdinal != 1u ||
          !snapshot.issueDeviceEpoch || snapshot.active || snapshot.failed || !snapshot.eligible ||
          !snapshot.backendFence.valid() ||
          (snapshot.backendFence.marker && snapshot.backendFence.marker->cancelled()) ||
          !device->dev().queryPollSnapshotCurrent(snapshot) ||
          !sameQueryIssue(snapshot, device->dev().captureQueryPollSnapshot(query->obj)))
        return core::D3DERR_NOTAVAILABLE;
      for (const auto& previous : baselines)
        if (renderTapeIdentityV3SameIdentity(previous.localIdentity, query->wireIdentity) ||
            previous.snapshot.identity == snapshot.identity) return core::D3DERR_NOTAVAILABLE;
      baselines.push_back({query->wireIdentity, snapshot});
    }
    root = std::make_shared<Root>();
    auto binding = std::make_unique<RenderTapeRuntimeBindingV4State>();
    const auto profile = upper->queue().renderTapeActionProfileSnapshot();
    const bool observeAttachments = !replay && !authority &&
        profile.valid && profile.currentProfile == kRenderTapeActionProfileNormal;
    std::vector<CommandQueue::TapeAttachmentProbe> probes;
    std::vector<RenderTapeAttachmentHistoryRow> rows;
    if (observeAttachments) {
      if (!device->wireObjects.snapshotAndRetainKind(D9C_CHUNK_HANDLE_KIND_SURFACE,
              kAttachmentHistoryMaxRows, surfaces.entries, RetainedArmSurfaces::retain))
        return core::D3DERR_NOTAVAILABLE;
      probes.reserve(surfaces.entries.size());
      rows.reserve(surfaces.entries.size());
      for (const auto& entry : surfaces.entries) {
        const auto* surface = static_cast<D9CSurface*>(entry.object);
        if (!surface || !surface->obj || surface->device != device ||
            !renderTapeIdentityV3SameIdentity(entry.identity, surface->wireIdentity))
          return core::D3DERR_NOTAVAILABLE;
        const auto* texture = surface->ownerTex;
        if (texture && (texture->device != device ||
            !device->wireObjects.contains(texture->wireIdentity, texture)))
          return core::D3DERR_NOTAVAILABLE;
        rows.push_back({.surface = entry.identity,
            .storage = texture ? texture->wireIdentity : entry.identity,
            .subresource = texture ? surface->ownerLevel : 0u});
        probes.push_back({.handle = surface->obj->handle()});
      }
    }
    CommandQueue::TapeAttachmentObservation observation{.probes = probes};
    InstalledCaptureGuard installed{*upper, *root};
    render::TapeRuntimeBaseline baseline{};
    if (!upper->queue().installTapeRuntimeCapture(root, baseline,
            observeAttachments ? &observation : nullptr)) return core::D3DERR_NOTAVAILABLE;
    installed.installed = true;
    if (observeAttachments) {
      if (!observation.actionProfile.valid ||
          observation.actionProfile.currentProfile != kRenderTapeActionProfileNormal)
        return core::D3DERR_NOTAVAILABLE;
      for (std::size_t i = 0; i < rows.size(); ++i) {
        rows[i].touched = probes[i].touched;
        rows[i].currentBackbuffer = probes[i].currentBackbuffer;
      }
      const RenderTapeAttachmentHistoryHeader history{
          .token = token, .epoch = epoch, .queue = baseline.queue,
          .completedSequence = baseline.completedSequence,
          .completedInitializer = baseline.completedInitializer,
          .count = static_cast<std::uint32_t>(rows.size()),
          .flags = (observation.discard ? 1u : 0u) |
                   (observation.hasCurrentBackbuffer ? 2u : 0u)};
      if (!encodeRenderTapeAttachmentHistory(history, rows, binding->attachmentHistory))
        return core::D3DERR_NOTAVAILABLE;
    }
    if (!root->begin({token, epoch}, baseline)) return core::D3DERR_NOTAVAILABLE;
    if (replay) {
      bool seeded = false;
      const auto lease = upper->queue().acquireTapeRuntimeCapture();
      if (!lease.visit([&](render::TapeRuntimeCaptureAttachment& admitted) noexcept {
            if (&admitted == root.get()) seeded = root->seedQueryBaselines({token, epoch}, baselines);
          }) || !seeded) return core::D3DERR_NOTAVAILABLE;
    }
    // Publication must still name each admitted physical issue, not a reset
    // or reissue that invalidated a retained receipt during queue admission.
    for (const auto& input : queries)
      if (!device->dev().queryPollSnapshotCurrent(input.snapshot) ||
          !sameQueryIssue(input.snapshot, device->dev().captureQueryPollSnapshot(input.query->obj)))
        return core::D3DERR_NOTAVAILABLE;
    binding->captureToken = token;
    binding->captureEpoch = epoch;
    binding->queue = baseline.queue;
    binding->armActionProfile = observeAttachments ? observation.actionProfile
        : upper->queue().renderTapeActionProfileSnapshot();
    if (!renderTapeRuntimeBindingV4ActionProfileCoherent(binding->armActionProfile) ||
        (!replay && !authority && !observeAttachments &&
         binding->armActionProfile.currentProfile == kRenderTapeActionProfileNormal))
      return core::D3DERR_NOTAVAILABLE;
    if (authority) {
      std::lock_guard authorityLock(authority->mutex);
      if (authority->revoked.load(std::memory_order_acquire) ||
          authority->device != device || authority->backend != upper ||
          !renderTapeControlledCpuV4Advance(authority->phase,
              RenderTapeControlledCpuV4Action::Bind, {true, true, true, true, false}))
        return core::D3DERR_NOTAVAILABLE;
      authority->root = root;
      authority->key = {token, epoch};
      authority->queue = baseline.queue;
      binding->domain = RenderTapeRuntimeBindingV4Domain::ControlledCpu;
      binding->controlledCpuAuthority = authority;
    }
    device->renderTapeRuntimeBindingV4 = std::move(binding);
    std::atomic_store_explicit(&device->renderTapeRuntimeIdentityV4, root, std::memory_order_release);
    device->renderTapeRuntimeV4Selected.store(true, std::memory_order_release);
    *out = {.status = D9C_RENDER_TAPE_IDENTITY_CAPTURE_COMPLETE,
        .version = 4u, .captureToken = token, .captureEpoch = epoch, .queue = baseline.queue};
    installed.installed = false;
    return core::D3D_OK;
  } catch (...) { return core::D3DERR_NOTAVAILABLE; }
}

bool matchesBinding(D9CDevice& device, const std::shared_ptr<Root>& root,
    Key key, std::uint64_t queue) noexcept {
  return root && queue && device.renderTapeRuntimeBindingV4 &&
      device.renderTapeRuntimeBindingV4->captureToken == key.token &&
      device.renderTapeRuntimeBindingV4->captureEpoch == key.epoch &&
      device.renderTapeRuntimeBindingV4->queue == queue &&
      root == acquireRenderTapeRuntimeIdentityV4(&device) && root->matches(key);
}

void logCutFailure(const std::shared_ptr<Root>& root, Key key,
    std::uint64_t queue, std::uint64_t event, std::uint32_t kind,
    const char* phase) noexcept {
  try {
    const auto first = root ? root->firstFailure() : RenderTapeRuntimeIdentityFailure{};
    const auto current = root ? root->diagnosticState() : RenderTapeRuntimeIdentityFailure{};
    util::logf(util::LogLevel::Info, "dxmt9-render-tape-v4",
        "cut_rejected phase=%s token=%llu epoch=%llu queue=%llu event=%llu cut=%u "
        "clock_active=%u root_active=%u root_failed=%u root_frozen=%u "
        "cursor_event=%llu cursor_raw=%llu cursor_cut=%u/%u cursor_record=%u cursor_type=%u "
        "event_active=%u record_active=%u first_event=%llu first_raw=%llu first_cut=%u/%u "
        "first_file=%s first_line=%u first_function=%s",
        phase, static_cast<unsigned long long>(key.token),
        static_cast<unsigned long long>(key.epoch), static_cast<unsigned long long>(queue),
        static_cast<unsigned long long>(event), kind,
        static_cast<unsigned>(root && root->edges().active()),
        static_cast<unsigned>(current.active), static_cast<unsigned>(current.failed),
        static_cast<unsigned>(current.frozen),
        static_cast<unsigned long long>(current.eventOrdinal),
        static_cast<unsigned long long>(current.rawOrdinal), current.cutKind, current.cutRecord,
        current.recordIndex, current.recordType, static_cast<unsigned>(current.eventActive),
        static_cast<unsigned>(current.recordActive),
        static_cast<unsigned long long>(first.eventOrdinal),
        static_cast<unsigned long long>(first.rawOrdinal), first.cutKind, first.cutRecord,
        first.location.file_name(), static_cast<unsigned>(first.location.line()),
        first.location.function_name());
  } catch (...) {} // Capture-only evidence must not alter the ABI result.
}

// Binding mutex held by the caller; root remains alive in caller-owned storage
// across the queue's revoke/join. Its destructor cannot run inside either lock.
void detachBindingLocked(D9CDevice& device, std::shared_ptr<Root>& root) noexcept {
  root = std::atomic_exchange_explicit(&device.renderTapeRuntimeIdentityV4,
      std::shared_ptr<Root>{}, std::memory_order_acq_rel);
  if (root) {
    const auto upper = device.dev().upperDevice();
    if (upper) upper->queue().revokeTapeRuntimeObserver(root->sink());
    else root->edges().revoke();
  }
  device.renderTapeRuntimeBindingV4.reset();
}

} // namespace

bool RenderTapeControlledCpuV4Lease::visitBackend(RenderTapeControlledCpuV4Boundary boundary,
    void* context, Visitor visitor) const noexcept {
  if (boundary != RenderTapeControlledCpuV4Boundary::Mint &&
      boundary != RenderTapeControlledCpuV4Boundary::Replay &&
      boundary != RenderTapeControlledCpuV4Boundary::Finish) return false;
  const auto authority = authority_.lock();
  if (!authority || !visitor || !backend_) return false;
  // The token precedes every borrowed device/probe-context access. No state
  // mutex survives the probe, binding check, or visitor (which may wait).
  const auto fixture = authority->fixtureLifetime.lock();
  if (!fixture || authority->revoked.load(std::memory_order_acquire)) return false;
  std::shared_ptr<Root> root;
  Key key{};
  std::uint64_t queue = 0u;
  RenderTapeControlledCpuV4Phase phase;
  {
    std::lock_guard lock(authority->mutex);
    phase = authority->phase;
    root = authority->root.lock();
    key = authority->key;
    queue = authority->queue;
  }
  using Phase = RenderTapeControlledCpuV4Phase;
  if (phase == Phase::Revoked || phase == Phase::Finalized ||
      (boundary == RenderTapeControlledCpuV4Boundary::Mint && phase != Phase::Available) ||
      (boundary == RenderTapeControlledCpuV4Boundary::Finish && phase != Phase::Bound) ||
      authority->backend != backend_ || !authority->device || !authority->probe ||
      !authority->probe(authority->probeContext, authority->device, *backend_, boundary)) return false;
  auto* device = authority->device;
  if (device->replayOffload || device->dev().upperDevice() != backend_) return false;
  {
    std::lock_guard lock(device->renderTapeRuntimeIdentityV4Mutex);
    if (phase == Phase::Available) {
      if (acquireRenderTapeRuntimeIdentityV4(device) || device->renderTapeRuntimeBindingV4) return false;
    } else if (!matchesBinding(*device, root, key, queue) ||
        device->renderTapeRuntimeBindingV4->domain != RenderTapeRuntimeBindingV4Domain::ControlledCpu ||
        device->renderTapeRuntimeBindingV4->controlledCpuAuthority.lock() != authority) return false;
  }
  if (authority->revoked.load(std::memory_order_acquire)) return false;
  return visitor(context, device, *backend_);
}

RenderTapeControlledCpuV4Capability::~RenderTapeControlledCpuV4Capability() { revoke(); }
RenderTapeControlledCpuV4Capability::RenderTapeControlledCpuV4Capability(
    RenderTapeControlledCpuV4Capability&& other) noexcept : authority_(std::move(other.authority_)) {}
RenderTapeControlledCpuV4Capability& RenderTapeControlledCpuV4Capability::operator=(
    RenderTapeControlledCpuV4Capability&& other) noexcept {
  if (this != &other) { revoke(); authority_ = std::move(other.authority_); }
  return *this;
}
void RenderTapeControlledCpuV4Capability::revoke() noexcept {
  if (!authority_) return;
  authority_->revoked.store(true, std::memory_order_release);
  std::lock_guard lock(authority_->mutex);
  (void)renderTapeControlledCpuV4Advance(authority_->phase,
      RenderTapeControlledCpuV4Action::Revoke, {});
}
RenderTapeControlledCpuV4Lease RenderTapeControlledCpuV4Capability::lease() const noexcept {
  RenderTapeControlledCpuV4Lease result;
  result.authority_ = authority_;
  if (authority_) result.backend_ = authority_->backend;
  return result;
}
RenderTapeControlledCpuV4Capability RenderTapeControlledCpuV4Capability::mint(D9CDevice* device,
    std::shared_ptr<dxmt9::Device> backend, std::weak_ptr<void> fixtureLifetime,
    void* probeContext, IsolationProbe probe) noexcept {
  RenderTapeControlledCpuV4Capability result;
  if (!device || !backend || !probe || fixtureLifetime.expired()) return result;
  try {
    auto authority = std::make_shared<RenderTapeControlledCpuV4Authority>();
    authority->device = device;
    authority->backend = std::move(backend);
    authority->fixtureLifetime = std::move(fixtureLifetime);
    authority->probeContext = probeContext;
    authority->probe = probe;
    result.authority_ = std::move(authority);
    if (!result.lease().visitBackend(RenderTapeControlledCpuV4Boundary::Mint, nullptr,
            [](void*, D9CDevice*, dxmt9::Device&) noexcept { return true; }))
      result.authority_.reset();
  } catch (...) { result.authority_.reset(); }
  return result;
}
std::int32_t RenderTapeControlledCpuV4Capability::beginCapture(D9CDevice* device,
    std::uint64_t token, std::uint64_t epoch,
    std::span<const RenderTapeReplayQueryBaselineV4> queries,
    D9CRenderTapeIdentityCaptureV4ArmResult* out) const noexcept {
  if (out) { *out = {}; out->status = D9C_RENDER_TAPE_IDENTITY_CAPTURE_FAILED; }
  if (!authority_ || authority_->device != device) return core::D3DERR_NOTAVAILABLE;
  struct Context {
    const std::shared_ptr<RenderTapeControlledCpuV4Authority>& authority;
    std::uint64_t token, epoch;
    std::span<const RenderTapeReplayQueryBaselineV4> queries;
    D9CRenderTapeIdentityCaptureV4ArmResult* out;
    std::int32_t result = core::D3DERR_NOTAVAILABLE;
  } context{authority_, token, epoch, queries, out};
  const bool visited = lease().visitBackend(RenderTapeControlledCpuV4Boundary::Replay, &context,
      [](void* opaque, D9CDevice* actual, dxmt9::Device&) noexcept {
        auto& input = *static_cast<Context*>(opaque);
        input.result = dxmt9::d3d9::beginCapture(actual, input.token, input.epoch, input.queries,
            true, input.out, input.authority);
        return input.result == core::D3D_OK;
      });
  return visited ? context.result : core::D3DERR_NOTAVAILABLE;
}
bool RenderTapeControlledCpuV4Capability::finishCapture(D9CDevice* device, Key key,
    std::uint64_t queue, std::uint64_t present, RenderTapeIdentityV4View& out,
    RenderTapeActionWitnessV1& witness) const noexcept {
  out = {};
  witness = {};
  if (!authority_ || authority_->device != device) return false;
  struct Context {
    const std::shared_ptr<RenderTapeControlledCpuV4Authority>& authority;
    Key key;
    std::uint64_t queue, present;
    RenderTapeIdentityV4View& out;
    RenderTapeActionWitnessV1& witness;
  } context{authority_, key, queue, present, out, witness};
  return lease().visitBackend(RenderTapeControlledCpuV4Boundary::Finish, &context,
      [](void* opaque, D9CDevice* actual, dxmt9::Device& upper) noexcept {
        auto& input = *static_cast<Context*>(opaque);
        try {
          std::lock_guard lock(actual->renderTapeRuntimeIdentityV4Mutex);
          const auto root = acquireRenderTapeRuntimeIdentityV4(actual);
          if (!matchesBinding(*actual, root, input.key, input.queue)) return false;
          auto& binding = *actual->renderTapeRuntimeBindingV4;
          if (binding.domain != RenderTapeRuntimeBindingV4Domain::ControlledCpu ||
              binding.controlledCpuAuthority.lock() != input.authority || binding.controlledCpuFinalized ||
              binding.receipt || !binding.transfer.empty()) return false;
          if (!binding.actionWitnessFrozen) {
            // No flush: the caller has closed production and joined/drained
            // only genuinely published sources through the controlled queue.
            if (!drainDeferredReplay(actual, "render-tape-v4-controlled-cpu-finish") ||
                !upper.queue().hasCompletedPendingWork() ||
                !upper.queue().refreshTapeRuntimeCompletions(root->sink()) ||
                !upper.queue().removeTapeRuntimeObserver(root->sink()) ||
                !root->freeze(input.key, input.present)) return false;
            binding.actionWitnessFrozen = true;
            binding.frozenPresentOrdinal = input.present;
            if (!renderTapeRuntimeBindingV4BuildActionWitness(binding.armActionProfile,
                    upper.queue().renderTapeActionProfileSnapshot(), input.key.token,
                    input.key.epoch, input.queue, binding.actionWitness)) return false;
          }
          if (binding.frozenPresentOrdinal != input.present ||
              !root->snapshotControlledCpu(input.key, input.out) ||
              input.out.header.queue != input.queue || input.out.header.presentOrdinal != input.present ||
              !renderTapeActionWitnessV1MatchesBinding(binding.actionWitness,
                  input.key.token, input.key.epoch, input.queue)) { input.out = {}; return false; }
          // Final authority transition occurs after all root provenance guards.
          {
            std::lock_guard authorityLock(input.authority->mutex);
            if (input.authority->revoked.load(std::memory_order_acquire) ||
                !renderTapeControlledCpuV4Advance(input.authority->phase,
                    RenderTapeControlledCpuV4Action::Finish, {true, true, true, true, true})) {
              input.out = {};
              return false;
            }
          }
          binding.controlledCpuFinalized = true;
          input.witness = binding.actionWitness;
          // The CPU value is not a reusable production capture receipt. Retire
          // the exact device-owned binding while the live fixture is retained;
          // executor cancellation can then avoid a raw wrapper dereference.
          std::shared_ptr<Root> retired;
          detachBindingLocked(*actual, retired);
          return true;
        } catch (...) { input.out = {}; input.witness = {}; return false; }
      });
}

std::shared_ptr<RenderTapeRuntimeIdentityV4>
acquireRenderTapeRuntimeIdentityV4(D9CDevice* device) noexcept {
  return device ? std::atomic_load_explicit(&device->renderTapeRuntimeIdentityV4,
      std::memory_order_acquire) : std::shared_ptr<Root>{};
}

void detachRenderTapeRuntimeIdentityV4(D9CDevice* device) noexcept {
  if (!device) return;
  std::shared_ptr<Root> retained;
  {
    std::lock_guard lock(device->renderTapeRuntimeIdentityV4Mutex);
    detachBindingLocked(*device, retained);
  }
}

std::int32_t beginRenderTapeIdentityCaptureV4ForReplay(D9CDevice* device,
    std::uint64_t token, std::uint64_t epoch,
    std::span<const RenderTapeReplayQueryBaselineV4> queries,
    D9CRenderTapeIdentityCaptureV4ArmResult* out) noexcept {
  return beginCapture(device, token, epoch, queries, true, out);
}

} // namespace dxmt9::d3d9

extern "C" int32_t dxmt9c_device_begin_render_tape_identity_capture_v4(
    D9CDevice* device, std::uint64_t token, std::uint64_t epoch,
    D9CRenderTapeIdentityCaptureV4ArmResult* out) {
  return dxmt9::d3d9::beginCapture(device, token, epoch, {}, false, out);
}

extern "C" int32_t dxmt9c_device_note_render_tape_identity_cut_v4(
    D9CDevice* device, std::uint64_t token, std::uint64_t epoch,
    std::uint64_t queue, std::uint64_t eventOrdinal, std::uint32_t cutKind) {
  using namespace dxmt9::d3d9;
  using Cut = RenderTapeIdentityV3CutKind;
  if (!device || !token || !epoch || !queue || !eventOrdinal ||
      (cutKind != static_cast<std::uint32_t>(Cut::BeforeEvent) &&
       cutKind != static_cast<std::uint32_t>(Cut::AfterEvent)))
    return dxmt9::core::D3DERR_INVALIDCALL;
  const auto root = acquireRenderTapeRuntimeIdentityV4(device);
  try {
    std::lock_guard lock(device->renderTapeRuntimeIdentityV4Mutex);
    const Key key{token, epoch};
    if (!matchesBinding(*device, root, key, queue)) {
      logCutFailure(root, key, queue, eventOrdinal, cutKind, "binding");
      return dxmt9::core::D3DERR_NOTAVAILABLE;
    }
    if (device->renderTapeRuntimeBindingV4->receipt) {
      logCutFailure(root, key, queue, eventOrdinal, cutKind, "receipt");
      return dxmt9::core::D3DERR_NOTAVAILABLE;
    }
    if (root->frozen()) {
      logCutFailure(root, key, queue, eventOrdinal, cutKind, "frozen");
      return dxmt9::core::D3DERR_NOTAVAILABLE;
    }
    if (!drainDeferredReplay(device, "render-tape-v4-external-cut")) {
      logCutFailure(root, key, queue, eventOrdinal, cutKind, "drain");
      return dxmt9::core::D3DERR_NOTAVAILABLE;
    }
    // A PE recorder flush may only enqueue work. This cursor is changed after
    // the actual preceding replay/ledger ACK, never at enqueue completion.
    if (root->setCut(eventOrdinal, static_cast<Cut>(cutKind))) return dxmt9::core::D3D_OK;
    logCutFailure(root, key, queue, eventOrdinal, cutKind,
        root->edges().active() ? "root" : "clock");
    return dxmt9::core::D3DERR_NOTAVAILABLE;
  } catch (...) {
    logCutFailure(root, {token, epoch}, queue, eventOrdinal, cutKind, "exception");
    return dxmt9::core::D3DERR_NOTAVAILABLE;
  }
}

extern "C" int32_t dxmt9c_device_finish_render_tape_identity_capture_v4(
    D9CDevice* device, std::uint64_t token, std::uint64_t epoch,
    std::uint64_t queue, std::uint64_t presentOrdinal,
    D9CRenderTapeIdentityCaptureV4Result* out, void* bytes,
    std::uint64_t capacity) {
  using namespace dxmt9::d3d9;
  if (!out) return dxmt9::core::D3DERR_INVALIDCALL;
  *out = {};
  out->status = D9C_RENDER_TAPE_IDENTITY_CAPTURE_FAILED;
  if (!device || !token || !epoch || !queue || !presentOrdinal ||
      (capacity && !bytes) || capacity > std::numeric_limits<std::size_t>::max())
    return dxmt9::core::D3DERR_INVALIDCALL;
  const auto root = acquireRenderTapeRuntimeIdentityV4(device);
  try {
    std::lock_guard lock(device->renderTapeRuntimeIdentityV4Mutex);
    const Key key{token, epoch};
    if (!matchesBinding(*device, root, key, queue))
      return dxmt9::core::D3DERR_NOTAVAILABLE;
    auto& binding = *device->renderTapeRuntimeBindingV4;
    // A controlled CPU arm cannot acquire a production paired receipt even
    // when its provenance happens to be structurally empty/complete.
    if (binding.domain != RenderTapeRuntimeBindingV4Domain::Production)
      return dxmt9::core::D3DERR_NOTAVAILABLE;
    if (!binding.receipt) {
      if (binding.actionWitnessFrozen && binding.frozenPresentOrdinal != presentOrdinal)
        return dxmt9::core::D3DERR_NOTAVAILABLE;
      if (!binding.actionWitnessFrozen) {
        if (!drainDeferredReplay(device, "render-tape-v4-finish"))
          return dxmt9::core::D3DERR_NOTAVAILABLE;
        const auto upper = device->dev().upperDevice();
        if (!upper || !upper->queue().hasCompletedPendingWork() ||
            !upper->queue().refreshTapeRuntimeCompletions(root->sink()) ||
            !upper->queue().removeTapeRuntimeObserver(root->sink()) ||
            !root->freeze(key, presentOrdinal))
          return dxmt9::core::D3DERR_NOTAVAILABLE;
        binding.actionWitnessFrozen = true;
        binding.frozenPresentOrdinal = presentOrdinal;
        const auto freezeProfile = upper->queue().renderTapeActionProfileSnapshot();
        if (!renderTapeRuntimeBindingV4BuildActionWitness(binding.armActionProfile,
                freezeProfile, key.token, key.epoch, queue, binding.actionWitness))
          return dxmt9::core::D3DERR_NOTAVAILABLE;
      }
      if (!root->frozen() || !renderTapeActionWitnessV1MatchesBinding(
              binding.actionWitness, key.token, key.epoch, queue))
        return dxmt9::core::D3DERR_NOTAVAILABLE;
      RenderTapeIdentityV4View identity;
      RenderTapeExecutionTopologyV1View topology;
      if (!root->snapshot(key, identity, topology) ||
          identity.header.presentOrdinal != presentOrdinal || identity.header.queue != queue ||
          topology.header.queue != queue ||
          encodeRenderTapeIdentityV4SnapshotTransfer(
              identity, topology, binding.actionWitness, binding.attachmentHistory, binding.transfer) !=
              RenderTapeIdentityV4TransferStatus::Valid)
        return dxmt9::core::D3DERR_NOTAVAILABLE;
      binding.receipt = D9CRenderTapeIdentityCaptureV4Result{
          .status = D9C_RENDER_TAPE_IDENTITY_CAPTURE_COMPLETE,
          .version = 4u, .captureToken = token, .captureEpoch = epoch,
          .queue = queue, .presentOrdinal = presentOrdinal,
          .byteCount = binding.transfer.size()};
    }
    const auto& receipt = *binding.receipt;
    if (receipt.presentOrdinal != presentOrdinal)
      return dxmt9::core::D3DERR_NOTAVAILABLE;
    if (capacity && capacity != receipt.byteCount)
      return dxmt9::core::D3DERR_INVALIDCALL;
    if (capacity) std::memcpy(bytes, binding.transfer.data(), binding.transfer.size());
    *out = receipt;
    return dxmt9::core::D3D_OK;
  } catch (...) { return dxmt9::core::D3DERR_NOTAVAILABLE; }
}

extern "C" void dxmt9c_device_cancel_render_tape_identity_capture_v4(
    D9CDevice* device, std::uint64_t token, std::uint64_t epoch,
    std::uint64_t queue) {
  using namespace dxmt9::d3d9;
  if (!device || !token || !epoch || !queue) return;
  std::shared_ptr<Root> retained;
  {
    std::lock_guard lock(device->renderTapeRuntimeIdentityV4Mutex);
    const auto root = acquireRenderTapeRuntimeIdentityV4(device);
    if (matchesBinding(*device, root, {token, epoch}, queue))
      detachBindingLocked(*device, retained);
  }
}
