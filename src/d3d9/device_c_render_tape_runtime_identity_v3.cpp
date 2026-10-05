#include "device_c_render_tape_runtime_identity_v3.hpp"
#include "device_c_common.hpp"
#include "device_c_replay_projection.hpp"
#include "../dxmt9/dxmt9_command_queue.hpp"
#include "../dxmt9/dxmt9_device.hpp"
#include "../dxmt9/framegraph/fg_builder.hpp"

#include <algorithm>
#include <limits>

namespace dxmt9::d3d9 {
namespace {
using Kind = RenderTapeIdentityV3DispositionKind;
using Command = RenderTapeIdentityV3CommandKind;
constexpr auto noIndex = kRenderTapeIdentityV3NoIndex;
} // namespace

bool RenderTapeRuntimeIdentityV3::begin(RenderTapeIdentityV3CaptureKey key,
    render::TapeRuntimeBaseline baseline, RenderTapeIdentityV3Limits limits) noexcept {
  std::lock_guard lock(mutex_);
  if (!beginLocked(key, baseline, limits)) return false;
  try {
    commands_.reserve(limits.maxCommands);
    settlements_.reserve(limits.maxSettlements);
    return true;
  } catch (...) { return reject(); }
}

std::uint64_t RenderTapeRuntimeIdentityV3::edge() noexcept {
  if (!nextEdge_ || nextEdge_ == UINT64_MAX) { reject(); return 0u; }
  return nextEdge_++;
}


bool RenderTapeRuntimeIdentityV3::beginReplayAttempt(std::uint64_t token,
    std::uint64_t eventOrdinal, std::uint64_t rawOrdinal,
    std::uint32_t recordCount) noexcept {
  std::lock_guard lock(mutex_);
  if (!nextAttemptGeneration_ || nextAttemptGeneration_ == UINT64_MAX) return reject();
  const auto generation = nextAttemptGeneration_++;
  return beginEventLocked(token, eventOrdinal, rawOrdinal, generation, recordCount);
}


bool RenderTapeRuntimeIdentityV3::beginEventLocked(std::uint64_t token,
    std::uint64_t eventOrdinal, std::uint64_t rawOrdinal, std::uint64_t buildGeneration,
    std::uint32_t recordCount) noexcept {
  if (!settlements_.empty() && (rawOrdinal <= settlements_.back().rawOrdinal ||
      eventOrdinal <= settlements_.back().eventOrdinal)) return reject();
  if (!beginContextLocked(token, eventOrdinal, rawOrdinal, buildGeneration, 0u, recordCount))
    return false;
  nextAttemptGeneration_ = std::max(nextAttemptGeneration_, buildGeneration + 1u);
  transactionCommitted_ = false;
  return true;
}

bool RenderTapeRuntimeIdentityV3::transactionCommitted(
    const ReplayTransactionState& state) noexcept {
  std::lock_guard lock(mutex_);
  if (!active_ || failed_ || !eventActive_ || recordActive_ || transactionCommitted_ ||
      nextRecord_ != recordCount_ || state.phase != ReplayTransactionPhase::Committed ||
      !state.identity.valid() || state.identity.sequence != rawOrdinal_ ||
      state.nextRecordOrdinal != recordCount_ || !state.destination.valid() ||
      state.destination.identity != state.identity) return reject();
  transactionCommitted_ = true;
  return true;
}


bool RenderTapeRuntimeIdentityV3::publish(const render::TapeRuntimeObservation& value) {
  const auto index = registerPublication(value, commands_.size());
  if (index == noIndex) return false;
  auto* owner = writer(value.writer);
  if (!owner) return false;
  // The Arena destination is private until this actual publication callback.
  // Join its immutable physical extent to the original sequential record
  // execution, rather than manufacturing one command for each raw record.
  if (value.payload.isArena()) {
    for (std::uint32_t physical = 0u; physical < owner->commandCount; ++physical) {
      const auto command = value.payload.commandAt(physical).command;
      const auto kind = commandKind(command.kind);
      if (kind == Command::DrawRun) {
        if (command.drawParams.size() > pendingDraws_.size() - consumedDraws_) return reject();
        for (std::uint32_t draw = 0u; draw < command.drawParams.size(); ++draw) {
          auto& record = records_[pendingDraws_[consumedDraws_++]];
          if (record.writer.valid()) return reject();
          record.writer = value.writer;
          record.physicalCommand = physical;
          record.disposition.firstSubdraw = draw;
          record.disposition.subdrawCount = 1u;
        }
      } else {
        auto record = std::find_if(records_.begin(), records_.end(), [&](const auto& candidate) {
          return candidate.disposition.kind == static_cast<std::uint32_t>(Kind::GpuCommand) &&
              candidate.physicalCommand == noIndex &&
              renderTapeIdentityV3RecordCommandKind(candidate.type) == kind;
        });
        if (record == records_.end()) return reject();
        record->writer = value.writer;
        record->physicalCommand = physical;
      }
    }
  }
  framegraph::FrameGraph graph;
  framegraph::buildFrameGraph(value.payload, value.metadata.sourceOrdinal, graph);
  std::vector<std::uint32_t> passes(owner->commandCount, noIndex);
  if (graph.passes.size() > UINT64_MAX - nextLogicalPass_) return reject();
  const auto passBase = nextLogicalPass_;
  nextLogicalPass_ += graph.passes.size();
  for (std::size_t pass = 0u; pass < graph.passes.size(); ++pass) {
    const auto extent = graph.passes[pass].commands;
    if (extent.first > graph.commands.size() || extent.count > graph.commands.size() - extent.first)
      return reject();
    for (std::size_t local = 0u; local < extent.count; ++local) {
      const auto physical = graph.commands[extent.first + local].command_index;
      if (physical >= passes.size() || passes[physical] != noIndex) return reject();
      passes[physical] = static_cast<std::uint32_t>(pass);
    }
  }
  const auto commandBase = commands_.size();
  for (std::uint32_t physical = 0u; physical < owner->commandCount; ++physical) {
    const auto actual = value.payload.commandAt(physical).command;
    const auto kind = commandKind(actual.kind);
    if (kind == Command::Invalid || actual.drawParams.size() > UINT32_MAX) return reject();
    RenderTapeIdentityV3Command command{.sourceIndex = index,
        .physicalCommandIndex = physical, .kind = static_cast<std::uint32_t>(kind),
        .drawCount = kind == Command::DrawRun ? static_cast<std::uint32_t>(actual.drawParams.size()) : 0u};
    if (passes[physical] == noIndex) {
      command.membership = static_cast<std::uint32_t>(RenderTapeIdentityV3Membership::NoPassCommand);
    } else {
      command.logicalPassId = passBase + passes[physical];
      command.membership = static_cast<std::uint32_t>(RenderTapeIdentityV3Membership::Pass);
      command.dagPassIndex = passes[physical];
      command.passKind = static_cast<std::uint32_t>(graph.passes[passes[physical]].kind) + 1u;
    }
    for (const auto& generated : generated_)
      if (generated.writer == value.writer && generated.command == physical)
        command.coordinatorIndex = generated.coordinator;
    commands_.push_back(command);
  }
  return finishPublication(value, index, commandBase);
}

std::uint32_t RenderTapeRuntimeIdentityV3::sourceIndex(
    const render::TapeRuntimeObservation& value) const {
  for (std::uint32_t index = 0u; index < sources_.size(); ++index) {
    if (sources_[index].seqId != value.sequence) continue;
    const auto owner = std::find_if(writers_.begin(), writers_.end(),
        [&](const auto& candidate) { return candidate.sourceIndex == index; });
    if (owner == writers_.end() || owner->identity.index != value.source.id.index ||
        owner->identity.generation != value.source.id.generation ||
        owner->identity.storageGeneration != value.source.storage.generation) return noIndex;
    return index;
  }
  return noIndex;
}


bool RenderTapeRuntimeIdentityV3::settleReplayAttempt(std::uint64_t rawOrdinal,
    bool ledgerSucceeded) noexcept {
  std::lock_guard lock(mutex_);
  if (failed_ || !eventActive_ || recordActive_ || !transactionCommitted_ ||
      !ledgerSucceeded || rawOrdinal != rawOrdinal_ || nextRecord_ != recordCount_ ||
      settlements_.size() >= limits_.maxSettlements) return reject();
  try {
    settlements_.push_back({.eventOrdinal = eventOrdinal_, .rawOrdinal = rawOrdinal_,
        .buildGeneration = buildGeneration_, .replayEdge = edge(), .recordCount = recordCount_});
    eventActive_ = false;
    transactionCommitted_ = false;
    cutKind_ = static_cast<std::uint32_t>(RenderTapeIdentityV3CutKind::AfterEvent);
    cutRecord_ = 0u;
    return !failed_;
  } catch (...) { return reject(); }
}


bool RenderTapeRuntimeIdentityV3::snapshot(RenderTapeIdentityV3CaptureKey key,
    RenderTapeIdentityV3View& out, bool requireComplete) noexcept {
  out = {};
  std::lock_guard lock(mutex_);
  if (key.token != context_.captureToken || key.epoch != context_.captureEpoch || !readyLocked(requireComplete))
    return false;
  try {
    context_.stamp(out.header);
    out.sources = sources_;
    out.initializers = initializers_;
    out.commands = commands_;
    out.coordinators = coordinators_;
    out.settlements = settlements_;
    copyDispositions(out.dispositions);
    return true;
  } catch (...) { out = {}; return false; }
}

bool beginRenderTapeRuntimeIdentityV3(D9CDevice* device,
    std::uint64_t token, std::uint64_t epoch) noexcept {
  if (!device || !token || !epoch || device->renderTapeRuntimeIdentityV3) return false;
  const auto upper = device->dev().upperDevice();
  if (!upper) return false;
  try {
    auto owner = std::make_unique<RenderTapeRuntimeIdentityV3>();
    render::TapeRuntimeBaseline baseline{};
    if (!upper->queue().installTapeRuntimeObserver(owner->sink(), baseline)) return false;
    if (!owner->begin({token, epoch}, baseline)) {
      (void)upper->queue().removeTapeRuntimeObserver(owner->sink());
      return false;
    }
    device->renderTapeRuntimeIdentityV3 = std::move(owner);
    return true;
  } catch (...) { return false; }
}

bool finishRenderTapeRuntimeIdentityV3(D9CDevice* device,
    std::uint64_t token, std::uint64_t epoch,
    std::uint64_t presentOrdinal, RenderTapeIdentityV3View& out) noexcept {
  out = {};
  if (!device || !device->renderTapeRuntimeIdentityV3) return false;
  auto& runtime = *device->renderTapeRuntimeIdentityV3;
  const RenderTapeIdentityV3CaptureKey key{token, epoch};
  if (!runtime.matches(key)) return false;
  const auto upper = device->dev().upperDevice();
  if (!upper) return false;
  if (!runtime.frozen()) {
    if (!upper->queue().hasCompletedPendingWork() ||
        !upper->queue().refreshTapeRuntimeCompletions(runtime.sink()) ||
        !upper->queue().removeTapeRuntimeObserver(runtime.sink())) return false;
    if (!runtime.freeze(key, presentOrdinal)) return false;
  }
  return runtime.freeze(key, presentOrdinal) && runtime.snapshot(key, out);
}

void cancelRenderTapeRuntimeIdentityV3(D9CDevice* device,
    std::uint64_t token, std::uint64_t epoch) noexcept {
  if (!device || !device->renderTapeRuntimeIdentityV3 ||
      !device->renderTapeRuntimeIdentityV3->matches({token, epoch})) return;
  detachRenderTapeRuntimeIdentityV3(device);
}

void detachRenderTapeRuntimeIdentityV3(D9CDevice* device) noexcept {
  if (!device || !device->renderTapeRuntimeIdentityV3) return;
  const auto upper = device->dev().upperDevice();
  if (upper)
    upper->queue().revokeTapeRuntimeObserver(device->renderTapeRuntimeIdentityV3->sink());
  device->renderTapeRuntimeIdentityV3.reset();
}

void observeRenderTapeRuntimeReplaySettlementV3(D9CDevice* device,
    const RawCommandChunk& raw, bool ledgerSucceeded) noexcept {
  if (!device || !raw.renderTapeCaptureToken || !raw.renderTapeEventOrdinal ||
      !device->renderTapeRuntimeIdentityV3) return;
  (void)device->renderTapeRuntimeIdentityV3->settleReplayAttempt(raw.replaySeq,
      ledgerSucceeded && !device->replayDrainLedger.terminal());
}

} // namespace dxmt9::d3d9
