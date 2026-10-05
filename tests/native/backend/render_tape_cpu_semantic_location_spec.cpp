#include "render_tape_cpu_semantics.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace dxmt9;
using namespace dxmt9::test;
using Kind = core::MetalCommandKind;
std::uint64_t checks = 0u;

void check(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}

struct Location {
  std::size_t command = 0u;
  Kind kind = Kind::DrawRun;
  std::size_t subdraw = kCpuTapeNoSubdraw;
  friend bool operator==(const Location&, const Location&) = default;
};

enum class Callback { Select, Resource, Snapshot, LiveBinding, Draw };
struct Event {
  Callback callback = Callback::Select;
  Location location{};
  std::uint64_t context = 0u, digest = 0u;
};

struct Observation {
  std::uint64_t digest = dxmt9::test::detail::kCpuTapeFnvOffset;
  std::string failure{};
  std::vector<Event> events{};
  std::vector<std::uint64_t> drawContexts{};
  std::size_t snapshots = 0u, liveBindings = 0u;
  bool requireSelection = false, selected = false;
  Location current{};
  std::uint64_t context = 0u;

  void effect(Callback callback, std::uint64_t hash) {
    check(!requireSelection || selected, "semantic callbacks cannot precede item context selection");
    events.push_back({callback, current, context, hash});
  }
  void selection(Location location, std::uint64_t value, bool accept) {
    events.push_back({Callback::Select, location, value, digest});
    if (!accept) return;
    selected = true;
    current = location;
    context = value;
  }
};

core::ChunkSlot makeSlot() {
  core::ChunkSlot slot;
  core::CanonicalDrawState state;
  state.hot.streamMask = state.hot.key.streamMask = 3u;
  state.hot.streamBuffers[0] = {101u};
  state.hot.streamBuffers[1] = {102u};
  state.hot.streamStrides[0] = state.hot.key.streamStrides[0] = 16u;
  state.hot.streamStrides[1] = state.hot.key.streamStrides[1] = 20u;
  state.hot.colorAttachments[0].handle = {201u};
  state.hot.renderTargetMask = state.hot.key.renderTargetMask = 1u;
  const std::array<core::DrawParam, 2> draws{{
      {.primitiveCount = 1u, .startVertex = 0u},
      {.primitiveCount = 2u, .startVertex = 3u}}};
  std::array<core::DrawBindingSnapshot, 2> snapshots{};
  std::array<core::DrawParamPayloadView, 2> payloads{};
  for (std::size_t index = 0u; index < snapshots.size(); ++index) {
    snapshots[index].streamMask = 1u;
    snapshots[index].streams[0] = {
        .buffer = {101u}, .offset = 0u, .stride = 16u,
        .snapshot = {.metalHandle = 301u + index, .contentRevision = 401u + index}};
    payloads[index].bindingSnapshotData = std::span<const core::u8>(
        reinterpret_cast<const core::u8*>(&snapshots[index]), sizeof(snapshots[index]));
  }
  check(slot.appendDrawRun(std::move(state), {}, draws, payloads),
      "actual DOD ChunkSlot owns a physical DrawRun with two draw parameters");
  core::ClearDesc clear;
  clear.clearColor = true;
  clear.colorAttachments[0].handle = {201u};
  clear.color = {0.1f, 0.2f, 0.3f, 1.0f};
  slot.appendClear(clear);
  core::SwapDesc present;
  present.width = 640u;
  present.height = 480u;
  present.sourceSurface = {202u};
  slot.appendPresent(present, present.sourceSurface);
  check(slot.commandCount() == 3u && slot.commandAt(0u).drawParams.size() == 2u &&
      slot.commandAt(1u).kind == Kind::Clear && slot.commandAt(2u).kind == Kind::Present,
      "fixture positions come from actual DrawRun/Clear/Present storage, not fabricated origins");
  return slot;
}

template <typename Payload, typename LocationFn = std::nullptr_t,
          typename ContentRevisionFn = std::nullptr_t, typename RoleResourceFn = std::nullptr_t>
bool observeProjection(const Payload& payload, Observation& observed, LocationFn&& locationFn = nullptr,
                       ContentRevisionFn&& contentRevisionFn = nullptr,
                       RoleResourceFn&& roleResourceFn = nullptr,
                       CpuTapeSemanticProjection projection = CpuTapeSemanticProjection::Legacy) {
  const auto resource = [&](std::uint32_t kind, core::Handle handle, std::uint64_t& digest) {
    observed.effect(Callback::Resource, digest);
    dxmt9::test::detail::cpuTapeDigestScalar(digest, kind);
    dxmt9::test::detail::cpuTapeDigestScalar(digest, handle.value);
    return true;
  };
  const auto draw = [&](std::uint64_t digest) {
    observed.effect(Callback::Draw, digest);
    observed.drawContexts.push_back(observed.context);
    return true;
  };
  const auto snapshot = [&](core::Handle buffer,
                             const core::DrawBufferBindingSnapshot& value, std::uint64_t& digest) {
    observed.effect(Callback::Snapshot, digest);
    check(buffer == core::Handle{101u}, "snapshot callback sees the actual captured stream binding");
    std::string failure;
    return dxmt9::test::detail::cpuTapeDigestBindingSnapshot(digest, value, failure);
  };
  const auto live = [&](core::Handle buffer, std::uint64_t& digest, std::uint64_t& revision) {
    observed.effect(Callback::LiveBinding, digest);
    check(buffer == core::Handle{102u}, "live callback sees the actual uncaptured stream binding");
    revision = 501u;
    dxmt9::test::detail::cpuTapeDigestScalar(digest, revision);
    return true;
  };
  return observeCpuTapePayload(payload, observed.digest, resource, draw,
      observed.failure, &observed.snapshots, snapshot, &observed.liveBindings, live,
      nullptr, std::forward<LocationFn>(locationFn),
      std::forward<ContentRevisionFn>(contentRevisionFn), std::forward<RoleResourceFn>(roleResourceFn),
      projection);
}

template <typename LocationFn = std::nullptr_t>
bool observe(const core::ChunkSlot& slot, Observation& observed, LocationFn&& locationFn = nullptr,
             CpuTapeSemanticProjection projection = CpuTapeSemanticProjection::Legacy) {
  return observeProjection(core::SourcePayloadView(slot), observed, std::forward<LocationFn>(locationFn),
      nullptr, nullptr, projection);
}

class NoncopyableProjection final {
public:
  explicit NoncopyableProjection(const core::ChunkSlot& slot) : payload_(slot) {}
  NoncopyableProjection(const NoncopyableProjection&) = delete;
  NoncopyableProjection(NoncopyableProjection&&) = delete;
  NoncopyableProjection& operator=(const NoncopyableProjection&) = delete;
  NoncopyableProjection& operator=(NoncopyableProjection&&) = delete;
  std::size_t commandCount() const { return payload_.commandCount(); }
  core::SourceCommandView commandAt(std::size_t index) const { return payload_.commandAt(index); }
private:
  core::SourcePayloadView payload_;
};

std::uint64_t contextFor(Location location) {
  // Synthetic context tags test the selection seam only. They are not V4
  // origins, record identities, resource epochs, or full-oracle evidence.
  if (location.command == 0u) {
    check(location.kind == Kind::DrawRun && location.subdraw < 2u,
        "selection receives exact subdraw indices within one physical DrawRun");
    return 11u + location.subdraw * 11u;
  }
  check(location.subdraw == kCpuTapeNoSubdraw &&
      ((location.command == 1u && location.kind == Kind::Clear) ||
       (location.command == 2u && location.kind == Kind::Present)),
      "non-draw selection receives actual kind/index and explicit no-subdraw sentinel");
  return 33u + (location.command - 1u) * 11u;
}

void distinctDrawContextsAndOrder() {
  const auto slot = makeSlot();
  Observation observed;
  observed.requireSelection = true;
  check(observe(slot, observed, [&](std::size_t command, Kind kind, std::size_t subdraw) {
    const Location location{command, kind, subdraw};
    observed.selection(location, contextFor(location), true);
    return true;
  }), "cold location selection accepts actual final payload items");
  const std::array expected{Location{0u, Kind::DrawRun, 0u}, Location{0u, Kind::DrawRun, 1u},
      Location{1u, Kind::Clear, kCpuTapeNoSubdraw}, Location{2u, Kind::Present, kCpuTapeNoSubdraw}};
  std::size_t selected = 0u;
  Location current;
  std::uint64_t context = 0u;
  std::array<std::size_t, 4> effects{};
  for (const auto& event : observed.events) {
    if (event.callback == Callback::Select) {
      check(selected < expected.size() && event.location == expected[selected],
          "every exact payload item is selected once in physical/subdraw order");
      current = event.location;
      context = event.context;
      ++selected;
    } else {
      check(selected && event.location == current && event.context == context,
          "resource, snapshot, live-binding and draw callbacks use the selected item context");
      ++effects[selected - 1u];
    }
  }
  check(selected == 4u && effects == std::array<std::size_t, 4>{8u, 8u, 1u, 1u} &&
      observed.drawContexts == std::vector<std::uint64_t>{11u, 22u} &&
      observed.snapshots == 2u && observed.liveBindings == 2u,
      "two same-kind draws select distinct context before captured/live binding callbacks");
  check(observed.events.front().callback == Callback::Select &&
      observed.events.front().digest == dxmt9::test::detail::kCpuTapeFnvOffset,
      "first item selection precedes every digest mutation and semantic callback");
  const std::array drawOrder{Callback::Select, Callback::Resource, Callback::Resource,
      Callback::Resource, Callback::Resource, Callback::Snapshot, Callback::Resource,
      Callback::LiveBinding, Callback::Draw};
  for (std::size_t draw = 0u; draw < 2u; ++draw) {
    for (std::size_t event = 0u; event < drawOrder.size(); ++event) {
      check(observed.events[draw * drawOrder.size() + event].callback == drawOrder[event],
          "each draw selects context before every actual resource/snapshot/live/draw callback");
    }
  }
}

void noOpSelectionPreservesDigest() {
  const auto slot = makeSlot();
  Observation original, selected;
  std::size_t selections = 0u;
  check(observe(slot, original) && observe(slot, selected,
      [&](std::size_t, Kind, std::size_t) { ++selections; return true; }),
      "old omitted selector and explicit no-op selector both accept the original payload");
  check(selections == 4u && original.digest == selected.digest &&
      original.snapshots == selected.snapshots && original.liveBindings == selected.liveBindings &&
      original.drawContexts.size() == selected.drawContexts.size(),
      "optional no-op selection changes neither semantic digest nor callback counts");
  const NoncopyableProjection projection(slot);
  Observation borrowed;
  check(observeProjection(projection, borrowed) && borrowed.digest == original.digest,
      "walker consumes noncopyable checked command projections without unwrapping or copying the payload");
  // This projection tests the shape only. It neither qualifies an invalid
  // SourcePayloadView identity nor supplies a generation witness; the consumer's
  // actual generation-qualified source borrow is responsible for those gates.
}

void rejectionStopsBeforeItemEffects() {
  const auto slot = makeSlot();
  for (std::size_t reject = 0u; reject < 4u; ++reject) {
    Observation prefix;
    if (reject) {
      auto prefixSlot = makeSlot();
      prefixSlot.commandHeaders.resize(reject < 3u ? 1u : 2u);
      if (reject == 1u) prefixSlot.drawRunRecords[0].paramCount = 1u;
      check(observe(prefixSlot, prefix), "original walker digests only the actual accepted prefix");
    }
    Observation observed;
    observed.requireSelection = true;
    std::size_t selections = 0u;
    std::uint64_t rejectedDigest = 0u;
    check(!observe(slot, observed, [&](std::size_t command, Kind kind, std::size_t subdraw) {
      const Location location{command, kind, subdraw};
      const bool accept = selections++ != reject;
      observed.selection(location, contextFor(location), accept);
      if (!accept) rejectedDigest = observed.digest;
      return accept;
    }), "location selection failure rejects the actual item");
    check(observed.failure == "payload-location-selection-failed" && selections == reject + 1u &&
        observed.events.back().callback == Callback::Select && observed.digest == rejectedDigest &&
        observed.digest == prefix.digest && observed.events.size() == prefix.events.size() + reject + 1u &&
        observed.drawContexts.size() == std::min(reject, std::size_t{2u}),
        "suffix rejection hashes no rejected item and invokes no later semantic or location callbacks");
    if (!reject) {
      check(observed.digest == dxmt9::test::detail::kCpuTapeFnvOffset && observed.events.size() == 1u &&
          !observed.snapshots && !observed.liveBindings,
          "selection failure before first draw leaves original digest and effect counts unchanged");
    }
  }
}

void malformedPayloadStillRejects() {
  for (std::size_t fault = 0u; fault < 3u; ++fault) {
    auto slot = makeSlot();
    const char* failure = nullptr;
    if (fault == 0u) {
      slot.drawHotStates.clear();
      failure = "missing-first-draw-state";
    } else if (fault == 1u) {
      slot.drawRunRecords[0].paramCount = 0u;
      failure = "empty-draw-run";
    } else {
      slot.drawParams[0].bindingOverrideRange = {0u, 1u};
      failure = "malformed-binding-override";
    }
    Observation observed;
    std::size_t selections = 0u;
    check(!observe(slot, observed, [&](std::size_t, Kind, std::size_t) {
      ++selections;
      return true;
    }) && observed.failure == failure, "existing malformed DrawRun rejection remains fail-closed");
    check(observed.digest == dxmt9::test::detail::kCpuTapeFnvOffset && observed.events.empty() &&
        selections == (fault == 2u ? 1u : 0u),
        "missing state/empty run invent no item selection and malformed override hashes no item");
  }
  core::ChunkSlot unsupported;
  unsupported.appendSurfaceCopy({.source = {701u}, .destination = {702u}});
  Observation observed;
  std::size_t selections = 0u;
  check(!observe(unsupported, observed, [&](std::size_t command, Kind kind, std::size_t subdraw) {
    ++selections;
    check(command == 0u && kind == Kind::SurfaceCopy && subdraw == kCpuTapeNoSubdraw,
        "unsupported non-draw selection reports only its real physical location");
    return true;
  }) && observed.failure == "unsupported-command-kind" && selections == 1u &&
      observed.digest == dxmt9::test::detail::kCpuTapeFnvOffset && observed.events.empty(),
      "successful location selection cannot authorize an unsupported command kind");
}

core::ChunkSlot revisionSlot(bool captured, std::uint64_t vertexRevision,
                             std::uint64_t indexRevision, bool wrongStream = false) {
  core::CanonicalDrawState state;
  state.hot.streamMask = state.hot.key.streamMask = 1u;
  state.hot.streamBuffers[0] = {101u};
  state.hot.streamStrides[0] = state.hot.key.streamStrides[0] = 16u;
  state.hot.indexBuffer = {202u};
  const std::array<core::DrawParam, 1u> draws{{
      {.primitiveCount = 1u, .indexType = core::IndexType::UInt16, .indexed = true}}};
  core::DrawBindingSnapshot snapshots;
  snapshots.streamMask = 1u;
  snapshots.streams[0] = {.buffer = {wrongStream ? 103u : 101u}, .stride = 16u,
      .snapshot = {.metalHandle = 301u, .contentRevision = vertexRevision}};
  snapshots.indexBuffer = {202u};
  snapshots.indexType = core::IndexType::UInt16;
  snapshots.indexSnapshotValid = true;
  snapshots.indexSnapshot = {.metalHandle = 302u, .contentRevision = indexRevision};
  std::array<core::DrawParamPayloadView, 1u> payloads{};
  if (captured) payloads[0].bindingSnapshotData = std::span<const core::u8>(
      reinterpret_cast<const core::u8*>(&snapshots), sizeof(snapshots));
  core::ChunkSlot slot;
  check(slot.appendDrawRun(std::move(state), {}, draws, payloads),
      "revision fixture uses actual owned DrawRun and captured/live bindings");
  return slot;
}

void canonicalContentRevisionProjection() {
  using RevisionSource = CpuTapeBufferRevisionSource;
  struct RevisionObservation {
    std::uint64_t digest = dxmt9::test::detail::kCpuTapeFnvOffset;
    std::string failure;
    std::size_t snapshotCalls = 0u, liveCalls = 0u, projections = 0u, draws = 0u;
    bool selected = false;
    std::uint64_t rejectedDigest = 0u;
  };
  // Authored logical epochs test the projection primitive, not original-input
  // context authority or physical backing evidence. Zero-byte snapshots below
  // exercise field ordering only; they are not read proofs.
  const auto observeRevision = [&](const core::ChunkSlot& slot, RevisionObservation& observed,
      bool captured, std::uint64_t vertexRevision, std::uint64_t indexRevision,
      std::uint64_t vertexEpoch, bool project, bool rejectIndex = false) {
    const auto resource = [](std::uint32_t kind, core::Handle handle, std::uint64_t& hash) {
      dxmt9::test::detail::cpuTapeDigestScalar(hash, kind);
      dxmt9::test::detail::cpuTapeDigestScalar(hash, handle.value);
      return true;
    };
    const auto draw = [&](std::uint64_t) { ++observed.draws; return true; };
    const auto snapshot = [&](core::Handle buffer, const core::DrawBufferBindingSnapshot& value,
                              std::uint64_t& hash) {
      check(observed.selected && captured && (buffer == core::Handle{101u} || buffer == core::Handle{202u}),
          "location selection precedes captured VB/IB observation");
      ++observed.snapshotCalls;
      return dxmt9::test::detail::cpuTapeDigestBindingSnapshot(hash, value, observed.failure);
    };
    const auto live = [&](core::Handle buffer, std::uint64_t& hash, std::uint64_t& revision) {
      check(observed.selected && !captured, "location selection precedes actual live VB/IB observation");
      ++observed.liveCalls;
      revision = buffer == core::Handle{101u} ? vertexRevision : indexRevision;
      // Same logical empty-byte field as the zero-size captured observation.
      dxmt9::test::detail::cpuTapeDigestScalar(hash, std::uint64_t{0});
      return true;
    };
    const auto location = [&](std::size_t command, Kind kind, std::size_t subdraw) {
      check(command == 0u && kind == Kind::DrawRun && subdraw == 0u,
          "revision projection consumes one real physical/subdraw location");
      observed.selected = true;
      return true;
    };
    const auto projection = [&](core::Handle buffer, RevisionSource source,
                                std::uint64_t physical, std::uint64_t& semantic) {
      check(observed.selected &&
          source == (captured ? RevisionSource::BindingSnapshot : RevisionSource::LiveBinding) &&
          physical == (buffer == core::Handle{101u} ? vertexRevision : indexRevision),
          "projection receives unchanged physical revision and its actual observation representation");
      ++observed.projections;
      if (rejectIndex && buffer == core::Handle{202u}) {
        observed.rejectedDigest = observed.digest;
        return false;
      }
      semantic = buffer == core::Handle{101u} ? vertexEpoch : 9u;
      return true;
    };
    if (project) return observeCpuTapePayload(core::SourcePayloadView(slot), observed.digest,
        resource, draw, observed.failure, nullptr, snapshot, nullptr, live, nullptr, location, projection);
    return observeCpuTapePayload(core::SourcePayloadView(slot), observed.digest,
        resource, draw, observed.failure, nullptr, snapshot, nullptr, live, nullptr, location);
  };
  for (const bool captured : {false, true}) {
    const auto original = revisionSlot(captured, 41u, 42u);
    const auto physicallyChanged = revisionSlot(captured, 141u, 242u);
    RevisionObservation legacy, changedLegacy, canonical, changedPhysical, changedLogical;
    check(observeRevision(original, legacy, captured, 41u, 42u, 7u, false) &&
        observeRevision(physicallyChanged, changedLegacy, captured, 141u, 242u, 7u, false) &&
        legacy.digest != changedLegacy.digest && !legacy.projections,
        "omitted projection keeps physical revision in the legacy stream for VB and IB");
    check(observeRevision(original, canonical, captured, 41u, 42u, 7u, true) &&
        observeRevision(physicallyChanged, changedPhysical, captured, 141u, 242u, 7u, true) &&
        canonical.digest == changedPhysical.digest && canonical.projections == 2u &&
        canonical.draws == 1u && canonical.snapshotCalls == (captured ? 2u : 0u) &&
        canonical.liveCalls == (captured ? 0u : 2u),
        "canonical logical epochs remove only physical revision variation, without skipping observation");
    check(observeRevision(original, changedLogical, captured, 41u, 42u, 8u, true) &&
        canonical.digest != changedLogical.digest,
        "a changed logical VB content epoch remains semantically visible");
    RevisionObservation rejected;
    check(!observeRevision(original, rejected, captured, 41u, 42u, 7u, true, true) &&
        rejected.failure == "buffer-content-revision-projection-failed" && rejected.projections == 2u &&
        !rejected.draws && rejected.digest == rejected.rejectedDigest,
        "projection rejection stops before the rejected scalar and draw callback");
  }
  const auto invalid = revisionSlot(true, 41u, 42u, true);
  RevisionObservation rejected;
  check(!observeRevision(invalid, rejected, true, 41u, 42u, 7u, true) &&
      rejected.failure == "captured-stream-binding-mismatch" && !rejected.projections &&
      !rejected.snapshotCalls && !rejected.draws,
      "content projection cannot authorize a mismatched actual snapshot binding");
}

void originalSurfaceRolesAndLegacyFieldStream() {
  using RoleKind = CpuTapeResourceRoleKind;
  struct RoleEvent { Location location; CpuTapeResourceRole role; core::Handle actual; };
  auto slot = makeSlot();
  slot.drawHotStates[0].colorAttachments[3].handle = {201u};
  slot.drawHotStates[0].depthStencil.handle = {201u};
  slot.clearRecords[0].colorAttachments[3].handle = {201u};
  slot.clearRecords[0].clearDepth = true;
  slot.clearRecords[0].depthStencil.handle = {201u};
  slot.presentRecords[0].presentSource = {201u};
  Observation legacy, extended;
  std::vector<RoleEvent> roles;
  const auto select = [&](std::size_t command, Kind kind, std::size_t subdraw) {
    const Location location{command, kind, subdraw};
    extended.selection(location, contextFor(location), true);
    return true;
  };
  const auto identityRevision = [](core::Handle, CpuTapeBufferRevisionSource,
                                  std::uint64_t physical, std::uint64_t& semantic) {
    semantic = physical;
    return true;
  };
  const auto passthrough = [&](std::uint32_t kind, core::Handle handle, std::uint64_t& hash,
                               CpuTapeResourceRole role) {
    check(extended.selected, "all resource roles follow the existing real item selection");
    if (role.kind != RoleKind::General) {
      check(kind == D9C_CHUNK_HANDLE_KIND_SURFACE, "RT/DS/Present roles qualify actual surface resources only");
      roles.push_back({extended.current, role, handle});
    }
    if (!handle) {
      dxmt9::test::detail::cpuTapeDigestScalar(hash, std::uint64_t{0});
      return true;
    }
    extended.effect(Callback::Resource, hash);
    dxmt9::test::detail::cpuTapeDigestScalar(hash, kind);
    dxmt9::test::detail::cpuTapeDigestScalar(hash, handle.value);
    return true;
  };
  check(observe(slot, legacy) && observeProjection(core::SourcePayloadView(slot), extended,
      select, identityRevision, passthrough) && legacy.digest == extended.digest &&
      legacy.snapshots == extended.snapshots && legacy.liveBindings == extended.liveBindings &&
      legacy.drawContexts.size() == extended.drawContexts.size(),
      "transparent role/revision extensions preserve the exact legacy field stream and observation counts");
  check(roles.size() == 16u, "two subdraws, Clear and Present enumerate actual RT[4]/DS/source roles including null");
  for (std::size_t item = 0u; item < 3u; ++item) {
    const auto expectedLocation = item < 2u ? Location{0u, Kind::DrawRun, item}
        : Location{1u, Kind::Clear, kCpuTapeNoSubdraw};
    for (std::uint32_t rt = 0u; rt < 4u; ++rt) {
      const auto& role = roles[item * 5u + rt];
      check(role.location == expectedLocation && role.role == CpuTapeResourceRole{RoleKind::RenderTarget, rt} &&
          role.actual == core::Handle{rt == 0u || rt == 3u ? 201u : 0u},
          "same native handle and explicit nulls retain their exact original RT slot role and item context");
    }
    const auto& depth = roles[item * 5u + 4u];
    check(depth.location == expectedLocation && depth.role == CpuTapeResourceRole{RoleKind::DepthStencil, 0u} &&
        depth.actual == core::Handle{201u}, "DS uses a distinct semantic role, not a native alias-class identity");
  }
  check(roles.back().location == Location{2u, Kind::Present, kCpuTapeNoSubdraw} &&
      roles.back().role == CpuTapeResourceRole{RoleKind::PresentSource, 0u} &&
      roles.back().actual == core::Handle{201u}, "Present forwards the effective source and its own original role");
  Observation rejected;
  bool sawActualNull = false;
  std::size_t roleCalls = 0u;
  const auto rejectMissingOriginal = [&](std::uint32_t kind, core::Handle handle, std::uint64_t& hash,
                                        CpuTapeResourceRole role) {
    if (role.kind != RoleKind::General) ++roleCalls;
    if (role == CpuTapeResourceRole{RoleKind::RenderTarget, 1u}) {
      sawActualNull = !handle;
      return false; // Authored expected non-null RT1 is absent in the payload.
    }
    if (!handle) dxmt9::test::detail::cpuTapeDigestScalar(hash, std::uint64_t{0});
    else {
      dxmt9::test::detail::cpuTapeDigestScalar(hash, kind);
      dxmt9::test::detail::cpuTapeDigestScalar(hash, handle.value);
    }
    return true;
  };
  check(!observeProjection(core::SourcePayloadView(slot), rejected, nullptr, nullptr, rejectMissingOriginal) &&
      rejected.failure == "resource-normalization-failed" && sawActualNull && roleCalls == 2u &&
      !rejected.snapshots && !rejected.liveBindings && rejected.drawContexts.empty(),
      "actual-null original-role mismatch rejects before bindings, draw, later roles or later payload items");
  for (const auto missingRole : {RoleKind::DepthStencil, RoleKind::PresentSource}) {
    auto missing = makeSlot();
    missing.presentRecords[0].presentSource = {};
    missing.presentRecords[0].present.sourceSurface = {};
    Observation missingObserved;
    bool nullChecked = false;
    const auto rejectMissingRole = [&](std::uint32_t kind, core::Handle handle, std::uint64_t& hash,
                                       CpuTapeResourceRole role) {
      if (role.kind == missingRole) {
        nullChecked = !handle;
        return false;
      }
      if (!handle) dxmt9::test::detail::cpuTapeDigestScalar(hash, std::uint64_t{0});
      else {
        dxmt9::test::detail::cpuTapeDigestScalar(hash, kind);
        dxmt9::test::detail::cpuTapeDigestScalar(hash, handle.value);
      }
      return true;
    };
    check(!observeProjection(core::SourcePayloadView(missing), missingObserved,
        nullptr, nullptr, rejectMissingRole) && nullChecked &&
        missingObserved.failure == "resource-normalization-failed" &&
        missingObserved.drawContexts.size() == (missingRole == RoleKind::PresentSource ? 2u : 0u),
        "explicit null DS and Present source are role-checked, not accepted through the generic zero marker");
  }
  auto fallback = makeSlot();
  fallback.presentRecords[0].presentSource = {};
  Observation fallbackObserved;
  bool fallbackChecked = false;
  const auto checkFallback = [&](std::uint32_t kind, core::Handle handle, std::uint64_t& hash,
                                 CpuTapeResourceRole role) {
    if (role.kind == RoleKind::PresentSource) fallbackChecked = handle == core::Handle{202u};
    if (!handle) dxmt9::test::detail::cpuTapeDigestScalar(hash, std::uint64_t{0});
    else {
      dxmt9::test::detail::cpuTapeDigestScalar(hash, kind);
      dxmt9::test::detail::cpuTapeDigestScalar(hash, handle.value);
    }
    return true;
  };
  check(observeProjection(core::SourcePayloadView(fallback), fallbackObserved,
      nullptr, nullptr, checkFallback) && fallbackChecked,
      "Present role receives the actual fallback swap source when explicit presentSource is absent");
}

void malformedDrawRangesRejectBeforeAuthority() {
  using RangeMember = core::DrawPayloadRange core::DrawParam::*;
  const std::array<RangeMember, 4u> fields{
      &core::DrawParam::userVertexRange, &core::DrawParam::userIndexRange,
      &core::DrawParam::bindingOverrideRange, &core::DrawParam::bindingSnapshotRange};
  const std::array<const char*, 4u> reasons{
      "malformed-user-vertex-range", "malformed-user-index-range",
      "malformed-binding-override-range", "malformed-binding-snapshot-range"};
  const auto max = std::numeric_limits<core::u32>::max();
  for (std::size_t field = 0u; field < fields.size(); ++field) {
    for (std::size_t rejectedSubdraw = 0u; rejectedSubdraw < 2u; ++rejectedSubdraw) {
      Observation prefix;
      if (rejectedSubdraw) {
        auto acceptedPrefix = makeSlot();
        acceptedPrefix.commandHeaders.resize(1u);
        acceptedPrefix.drawRunRecords[0].paramCount = 1u;
        check(observe(acceptedPrefix, prefix), "range rejection reference observes only the accepted first draw");
      }
      for (std::uint32_t fault = 0u; fault < 6u; ++fault) {
        auto slot = makeSlot();
        check(slot.drawPayloadArena.size() < max, "range fixture has a bounded actual payload extent");
        const auto bytes = static_cast<core::u32>(slot.drawPayloadArena.size());
        const std::array<core::DrawPayloadRange, 6u> invalid{{
            {bytes + 1u, 1u}, {bytes, 1u}, {1u, bytes},
            {max, 1u}, {max - 4u, 8u}, {0u, max}}};
        slot.drawParams[rejectedSubdraw].*fields[field] = invalid[fault];
        Observation rejected;
        rejected.requireSelection = true;
        std::size_t selections = 0u;
        check(!observe(slot, rejected, [&](std::size_t, Kind, std::size_t) {
          ++selections;
          rejected.selected = true;
          return true;
        }), "every nonempty out-of-bounds or wrapping draw range fails closed");
        check(rejected.failure == reasons[field] && selections == rejectedSubdraw &&
            rejected.digest == prefix.digest && rejected.drawContexts.size() == rejectedSubdraw &&
            rejected.snapshots == rejectedSubdraw && rejected.liveBindings == rejectedSubdraw,
            "malformed range invokes no rejected-item location/binding/draw authority and preserves the accepted digest prefix");
      }
    }
  }
  core::CanonicalDrawState state;
  const std::array<core::DrawParam, 1u> draws{{{.primitiveCount = 1u}}};
  core::ChunkSlot absent;
  check(absent.appendDrawRun(std::move(state), {}, draws, {}),
      "zero-size regression owns an actual DrawRun with no optional byte ranges or buffer bindings");
  Observation original, zeroSized;
  check(observe(absent, original), "zero-size regression observes the original absent ranges");
  for (const auto field : fields) absent.drawParams[0].*field = {max, 0u};
  check(observe(absent, zeroSized) &&
      original.digest == zeroSized.digest && original.snapshots == zeroSized.snapshots &&
      original.liveBindings == zeroSized.liveBindings && original.drawContexts.size() == 1u &&
      zeroSized.drawContexts.size() == 1u,
      "size-zero ranges preserve absent-range semantics and exact legacy digest even at UINT32_MAX offset");
}

void logicalUniformProjectionRetainsValues() {
  constexpr auto logical = CpuTapeSemanticProjection::LogicalV4;
  core::DrawUniformPayload uniforms;
  uniforms.vertexConstantsHash = 101u;
  uniforms.pixelConstantsHash = 102u;
  uniforms.fixedPayloadHash = 103u;
  uniforms.hash = 104u;
  uniforms.vertexFloatConstantCount = static_cast<core::u16>(uniforms.vsConst.float4.size());
  uniforms.vertexIntConstantCount = static_cast<core::u16>(uniforms.vsConst.int4.size());
  uniforms.vertexBoolConstantCount = static_cast<core::u16>(uniforms.vsConst.bools.size());
  uniforms.pixelFloatConstantCount = static_cast<core::u16>(uniforms.psConst.float4.size());
  uniforms.pixelIntConstantCount = static_cast<core::u16>(uniforms.psConst.int4.size());
  uniforms.pixelBoolConstantCount = static_cast<core::u16>(uniforms.psConst.bools.size());
  const auto makeUniformSlot = [&](const core::DrawUniformPayload& values) {
    core::ChunkSlot slot;
    core::CanonicalDrawState state;
    state.hot.key.vertexDeclHash = 201u;
    state.hot.key.vertexShaderHash = 202u;
    state.hot.key.pixelShaderHash = 203u;
    const std::array<core::DrawParam, 1u> draws{{{.primitiveCount = 1u}}};
    check(slot.appendDrawRun(std::move(state), values, draws, {}),
        "uniform projection owns an actual DrawRun and full stage/fixed payload records");
    return slot;
  };
  const auto baseline = makeUniformSlot(uniforms);
  Observation omittedLegacy, explicitLegacy, projected;
  check(observe(baseline, omittedLegacy) &&
      observe(baseline, explicitLegacy, nullptr, CpuTapeSemanticProjection::Legacy) &&
      observe(baseline, projected, nullptr, logical) &&
      omittedLegacy.digest == explicitLegacy.digest,
      "omitted and explicit Legacy modes preserve the exact historical digest stream");
  using HashMember = core::u64 core::DrawUniformPayload::*;
  const std::array<HashMember, 4u> hashes{
      &core::DrawUniformPayload::vertexConstantsHash, &core::DrawUniformPayload::pixelConstantsHash,
      &core::DrawUniformPayload::fixedPayloadHash, &core::DrawUniformPayload::hash};
  for (const auto field : hashes) {
    auto changed = uniforms;
    changed.*field ^= 1u;
    const auto slot = makeUniformSlot(changed);
    Observation legacyChanged, logicalChanged;
    check(observe(slot, legacyChanged) && observe(slot, logicalChanged, nullptr, logical) &&
        legacyChanged.digest != omittedLegacy.digest && logicalChanged.digest == projected.digest &&
        logicalChanged.drawContexts.size() == 1u,
        "only LogicalV4 ignores each path-specific uniform cache hash after serializing actual values");
  }
  using Mutation = void (*)(core::DrawUniformPayload&);
  const std::array<Mutation, 19u> changes{
      [](auto& v) { v.vsConst.float4.back().back() += 1.0f; },
      [](auto& v) { ++v.vsConst.int4.back().back(); },
      [](auto& v) { v.vsConst.bools.back() = !v.vsConst.bools.back(); },
      [](auto& v) { v.psConst.float4.back().back() += 1.0f; },
      [](auto& v) { ++v.psConst.int4.back().back(); },
      [](auto& v) { v.psConst.bools.back() = !v.psConst.bools.back(); },
      [](auto& v) { v.worldViewProj.m.back() += 1.0f; },
      [](auto& v) { v.ffpView.m.back() += 1.0f; },
      [](auto& v) { v.ffpWorldView.m.back() += 1.0f; },
      [](auto& v) { v.ffpNormalMatrix.m.back() += 1.0f; },
      [](auto& v) { v.material.power += 1.0f; },
      [](auto& v) { v.lights.back().range += 1.0f; },
      [](auto& v) { v.ffpBlendWorldViewProj.back().m.back() += 1.0f; },
      [](auto& v) { v.ffpBlendWorldView.back().m.back() += 1.0f; },
      [](auto& v) { v.ffpBlendNormalMatrix.back().m.back() += 1.0f; },
      [](auto& v) { v.textureTransforms.back().m.back() += 1.0f; },
      [](auto& v) { ++v.clipPlaneMask; },
      [](auto& v) { v.clipPlanes.back().back() += 1.0f; },
      [](auto& v) { --v.pixelBoolConstantCount; }};
  for (const auto mutate : changes) {
    auto changed = uniforms;
    mutate(changed);
    const auto slot = makeUniformSlot(changed);
    Observation observed;
    check(observe(slot, observed, nullptr, logical) && observed.digest != projected.digest,
        "actual constants, fixed uniforms and retained counts still change the LogicalV4 draw digest");
  }
  // The serializer itself never gates full resolved values on usage counts.
  // Actual DOD materialization may zero uncaptured tails; hash what it resolves.
  auto zeroCounts = uniforms;
  zeroCounts.vertexFloatConstantCount = zeroCounts.vertexIntConstantCount = zeroCounts.vertexBoolConstantCount = 0u;
  zeroCounts.pixelFloatConstantCount = zeroCounts.pixelIntConstantCount = zeroCounts.pixelBoolConstantCount = 0u;
  auto completeDigest = dxmt9::test::detail::kCpuTapeFnvOffset;
  dxmt9::test::detail::cpuTapeDigestUniforms(completeDigest, zeroCounts, logical);
  for (std::size_t index = 0u; index < 6u; ++index) {
    auto changed = zeroCounts;
    changes[index](changed);
    auto changedDigest = dxmt9::test::detail::kCpuTapeFnvOffset;
    dxmt9::test::detail::cpuTapeDigestUniforms(changedDigest, changed, logical);
    check(changedDigest != completeDigest,
        "all resolved float/int/bool rows remain serialized even when their usage counts are zero");
  }
  const std::array<core::u64 core::FlatDrawStateKey::*, 3u> identities{
      &core::FlatDrawStateKey::vertexDeclHash, &core::FlatDrawStateKey::vertexShaderHash,
      &core::FlatDrawStateKey::pixelShaderHash};
  for (const auto field : identities) {
    auto slot = makeUniformSlot(uniforms);
    slot.drawHotStates[0].key.*field ^= 1u;
    Observation observed;
    check(observe(slot, observed, nullptr, logical) && observed.digest != projected.digest,
        "shader and declaration content-identity hashes remain authoritative in LogicalV4");
  }
}

void logicalAlphaProjectionIsPartitionInvariant() {
  constexpr auto logical = CpuTapeSemanticProjection::LogicalV4;
  const auto makeState = [](core::u32 function, core::u32 reference, bool explicitAlpha = true) {
    core::CanonicalDrawState state;
    state.hot.streamMask = state.hot.key.streamMask = 3u;
    state.hot.streamBuffers[0] = {101u};
    state.hot.streamBuffers[1] = {102u};
    state.hot.streamStrides[0] = state.hot.key.streamStrides[0] = 16u;
    state.hot.streamStrides[1] = state.hot.key.streamStrides[1] = 20u;
    auto& states = state.hot.renderStates;
    states.entries[states.count++] = {core::RS_CULL_MODE, 1u};
    if (explicitAlpha) {
      states.entries[states.count++] = {core::RS_ALPHA_TEST_ENABLE, function != 0u ? 1u : 0u};
      states.entries[states.count++] = {core::RS_ALPHA_FUNC,
          function != 0u ? function : static_cast<core::u32>(core::CompareFunc::Always)};
      states.entries[states.count++] = {core::RS_ALPHA_REF, reference};
    }
    std::sort(states.entries.begin(), states.entries.begin() + states.count,
        [](const auto& a, const auto& b) { return a.state < b.state; });
    return state;
  };
  const auto makeAlphaSlot = [&](bool batched, core::u32 secondReference = 2u) {
    core::ChunkSlot slot;
    const std::array<core::DrawParam, 2u> draws{{
        {.primitiveCount = 1u}, {.primitiveCount = 2u, .startVertex = 3u}}};
    const std::array<core::u32, 2u> functions{
        static_cast<core::u32>(core::CompareFunc::Less),
        static_cast<core::u32>(core::CompareFunc::Greater)};
    const std::array<core::u32, 2u> references{1u, secondReference};
    std::array<core::DrawBindingOverride, 2u> overrides{};
    std::array<core::DrawBindingSnapshot, 2u> snapshots{};
    std::array<core::DrawParamPayloadView, 2u> payloads{};
    for (std::size_t index = 0u; index < 2u; ++index) {
      overrides[index].alphaTestStateValid = true;
      overrides[index].alphaTestEnable = 1u;
      overrides[index].alphaTestFunc = functions[index];
      overrides[index].alphaTestRef = references[index];
      snapshots[index].streamMask = 1u;
      snapshots[index].streams[0] = {.buffer = {101u}, .offset = 0u, .stride = 16u,
          .snapshot = {.metalHandle = 301u + index, .contentRevision = 401u + index}};
      payloads[index].bindingOverrideData = std::span<const core::u8>(
          reinterpret_cast<const core::u8*>(&overrides[index]), sizeof(overrides[index]));
      payloads[index].bindingSnapshotData = std::span<const core::u8>(
          reinterpret_cast<const core::u8*>(&snapshots[index]), sizeof(snapshots[index]));
    }
    if (batched) {
      check(slot.appendDrawRun(makeState(functions[0], references[0]), {}, draws, payloads),
          "batched alpha fixture owns actual per-draw override and binding snapshot bytes");
    } else {
      for (std::size_t index = 0u; index < 2u; ++index) {
        check(slot.appendDrawRun(makeState(functions[index], references[index]), {},
            std::span<const core::DrawParam>(draws).subspan(index, 1u),
            std::span<const core::DrawParamPayloadView>(payloads).subspan(index, 1u)),
            "split alpha fixture retains each actual canonical hot state and captured binding");
      }
    }
    return slot;
  };
  const auto split = makeAlphaSlot(false), batched = makeAlphaSlot(true);
  check(split.commandCount() == 2u && batched.commandCount() == 1u,
      "alpha counterexample uses physically split versus combined actual DrawRuns");
  Observation splitLegacy, batchedLegacy, splitLogical, batchedLogical;
  check(observe(split, splitLegacy) && observe(batched, batchedLegacy) &&
      observe(split, splitLogical, nullptr, logical) && observe(batched, batchedLogical, nullptr, logical) &&
      splitLegacy.digest != batchedLegacy.digest && splitLogical.digest == batchedLogical.digest &&
      splitLogical.drawContexts.size() == 2u && batchedLogical.drawContexts.size() == 2u &&
      splitLogical.snapshots == 2u && batchedLogical.snapshots == 2u &&
      splitLogical.liveBindings == 2u && batchedLogical.liveBindings == 2u,
      "LogicalV4 retains effective per-draw alpha state independent of raw shared-run alpha representation");
  const auto altered = makeAlphaSlot(true, 3u);
  Observation effectiveChanged;
  check(observe(altered, effectiveChanged, nullptr, logical) && effectiveChanged.digest != batchedLogical.digest,
      "changing the effective per-draw override still changes the LogicalV4 digest");
  auto otherState = makeAlphaSlot(true);
  auto& states = otherState.drawHotStates[0].renderStates;
  const auto end = states.entries.begin() + states.count;
  const auto cull = std::find_if(states.entries.begin(), end,
      [](const auto& entry) { return entry.state == core::RS_CULL_MODE; });
  check(cull != end, "alpha projection fixture retains a non-alpha render state");
  cull->value = 2u;
  Observation otherChanged;
  check(observe(otherState, otherChanged, nullptr, logical) && otherChanged.digest != batchedLogical.digest,
      "the bounded alpha projection does not omit non-alpha render-state values");
  core::ChunkSlot absent, explicitDefaults;
  const std::array<core::DrawParam, 1u> draws{{{.primitiveCount = 1u}}};
  check(absent.appendDrawRun(makeState(0u, 0u, false), {}, draws, {}) &&
      explicitDefaults.appendDrawRun(makeState(0u, 0u), {}, draws, {}),
      "absent and explicit default alpha states use actual DrawRun storage");
  // These default fixtures need no live/snapshot byte authority.
  for (auto* slot : {&absent, &explicitDefaults}) {
    slot->drawHotStates[0].streamMask = slot->drawHotStates[0].key.streamMask = 0u;
    slot->drawHotStates[0].streamBuffers = {};
  }
  Observation absentLegacy, defaultsLegacy, absentLogical, defaultsLogical;
  check(observe(absent, absentLegacy) && observe(explicitDefaults, defaultsLegacy) &&
      observe(absent, absentLogical, nullptr, logical) &&
      observe(explicitDefaults, defaultsLogical, nullptr, logical) &&
      absentLegacy.digest != defaultsLegacy.digest && absentLogical.digest == defaultsLogical.digest,
      "LogicalV4 canonical count excludes only raw alpha presence; absent and explicit effective defaults agree");
  explicitDefaults.drawHotStates[0].renderStates.overflow = true;
  Observation overflowChanged;
  check(observe(explicitDefaults, overflowChanged, nullptr, logical) &&
      overflowChanged.digest != defaultsLogical.digest,
      "LogicalV4 still retains the render-state overflow signal");
}

} // namespace

int main() {
  try {
    distinctDrawContextsAndOrder();
    noOpSelectionPreservesDigest();
    rejectionStopsBeforeItemEffects();
    malformedPayloadStillRejects();
    canonicalContentRevisionProjection();
    originalSurfaceRolesAndLegacyFieldStream();
    malformedDrawRangesRejectBeforeAuthority();
    logicalUniformProjectionRetainsValues();
    logicalAlphaProjectionIsPartitionInvariant();
    std::cout << "CPU semantic payload location: " << checks << " checks\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "CPU semantic payload location: " << error.what() << '\n';
    return 1;
  }
}
