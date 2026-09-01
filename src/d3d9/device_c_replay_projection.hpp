#pragma once

#include "dxmt9/core_snapshots.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>

namespace dxmt9::d3d9 {

// A source-wide replay transaction is deliberately a value protocol.  The
// source identity is immutable, while the phase and receipts are advanced by
// the single replay consumer.  This keeps the serial reference semantics
// usable by both the production sink and the bounded model tests.
struct ReplaySourceIdentity {
  std::uint64_t frame = 0;
  std::uint64_t source = 0;
  std::uint64_t lastSource = 0;
  std::uint64_t sequence = 0;

  constexpr bool valid() const noexcept {
    return sequence != 0 && source != 0 && lastSource >= source;
  }
  friend constexpr bool operator==(const ReplaySourceIdentity&,
                                   const ReplaySourceIdentity&) = default;
};

enum class ReplayTransactionPhase : std::uint8_t {
  Created,
  Projecting,
  Staged,
  EffectStarted,
  DestinationReceived,
  Committed,
  RolledBack,
  FailedStop,
};

struct ReplayStateProjection {
  std::uint64_t stateGeneration = 0;
  std::uint64_t source = 0;
  std::uint32_t recordOrdinal = 0;
};

struct ReplayStagedEmission {
  std::uint32_t commandCount = 0;
  std::uint32_t byteCount = 0;
};

enum class ReplayDestinationKind : std::uint8_t {
  Compatibility,
  DirectChunkSlot,
  Arena,
};

struct ReplayDestinationReceipt {
  ReplayDestinationKind kind = ReplayDestinationKind::Compatibility;
  ReplaySourceIdentity identity{};
  std::uint64_t queueSequence = 0;
  std::uint64_t buildGeneration = 0;
  std::uint64_t sourceGeneration = 0;
  std::uint64_t storageGeneration = 0;
  std::uint32_t controlIndex = std::numeric_limits<std::uint32_t>::max();
  std::uint32_t commandCount = 0;

  constexpr bool valid() const noexcept {
    if (!identity.valid() || queueSequence == 0u) return false;
    if (kind == ReplayDestinationKind::Compatibility) return true;
    return buildGeneration != 0u && sourceGeneration != 0u &&
        storageGeneration != 0u &&
        controlIndex != std::numeric_limits<std::uint32_t>::max();
  }
};

struct ReplayTransactionState {
  ReplaySourceIdentity identity{};
  ReplayTransactionPhase phase = ReplayTransactionPhase::Created;
  std::uint32_t nextRecordOrdinal = 0;
  std::uint64_t stateGeneration = 0;
  std::uint64_t currentSource = 0;
  std::uint32_t stagedCommandCount = 0;
  std::uint32_t stagedByteCount = 0;
  ReplayDestinationReceipt destination{};

  constexpr bool irreversible() const noexcept {
    return phase == ReplayTransactionPhase::EffectStarted ||
           phase == ReplayTransactionPhase::DestinationReceived ||
           phase == ReplayTransactionPhase::Committed;
  }
  constexpr bool terminal() const noexcept {
    return phase == ReplayTransactionPhase::Committed ||
           phase == ReplayTransactionPhase::RolledBack ||
           phase == ReplayTransactionPhase::FailedStop;
  }
};

enum class ReplayTransactionEvent : std::uint8_t {
  ProjectState,
  StageEmission,
  StartIrreversibleEffect,
  ReceiveDestination,
  Commit,
  Rollback,
  FailStop,
};

struct ReplayTransactionTransition {
  ReplayTransactionState state{};
  bool accepted = false;
};

// Pure phase algebra.  An invalid transition leaves the input unchanged.
// Rollback is intentionally unavailable after StartIrreversibleEffect;
// callers must enter FailedStop once a visible side effect has begun.
constexpr ReplayTransactionTransition advanceReplayTransaction(
    const ReplayTransactionState& current,
    ReplayTransactionEvent event,
    ReplayStateProjection projection = {},
    ReplayStagedEmission emission = {},
    ReplayDestinationReceipt destination = {}) noexcept {
  ReplayTransactionTransition result{.state = current};
  if (current.terminal()) {
    return result;
  }
  switch (event) {
  case ReplayTransactionEvent::ProjectState:
    if (current.phase != ReplayTransactionPhase::DestinationReceived &&
        current.identity.valid() && projection.stateGeneration != 0u &&
        (current.stateGeneration == 0u ||
         current.stateGeneration == projection.stateGeneration) &&
        projection.source >= current.identity.source &&
        projection.source <= current.identity.lastSource &&
        (current.currentSource == 0u ||
         projection.source >= current.currentSource) &&
        projection.recordOrdinal == current.nextRecordOrdinal) {
      if (current.phase != ReplayTransactionPhase::EffectStarted) {
        result.state.phase = ReplayTransactionPhase::Projecting;
      }
      result.state.nextRecordOrdinal = projection.recordOrdinal + 1u;
      result.state.stateGeneration = projection.stateGeneration;
      result.state.currentSource = projection.source;
      result.accepted = true;
    }
    break;
  case ReplayTransactionEvent::StageEmission:
    if ((current.phase == ReplayTransactionPhase::Projecting ||
         current.phase == ReplayTransactionPhase::Staged ||
         current.phase == ReplayTransactionPhase::EffectStarted) &&
        emission.commandCount != 0 &&
        emission.commandCount <=
            std::numeric_limits<std::uint32_t>::max() -
                current.stagedCommandCount &&
        emission.byteCount <=
            std::numeric_limits<std::uint32_t>::max() -
                current.stagedByteCount) {
      if (current.phase != ReplayTransactionPhase::EffectStarted) {
        result.state.phase = ReplayTransactionPhase::Staged;
      }
      result.state.stagedCommandCount += emission.commandCount;
      result.state.stagedByteCount += emission.byteCount;
      result.accepted = true;
    }
    break;
  case ReplayTransactionEvent::StartIrreversibleEffect:
    if (current.phase == ReplayTransactionPhase::EffectStarted) {
      result.accepted = true;
    } else if (current.phase == ReplayTransactionPhase::Created ||
        current.phase == ReplayTransactionPhase::Projecting ||
        current.phase == ReplayTransactionPhase::Staged) {
      result.state.phase = ReplayTransactionPhase::EffectStarted;
      result.accepted = true;
    }
    break;
  case ReplayTransactionEvent::ReceiveDestination:
    if ((current.phase == ReplayTransactionPhase::Created ||
         current.phase == ReplayTransactionPhase::Projecting ||
         current.phase == ReplayTransactionPhase::Staged ||
         current.phase == ReplayTransactionPhase::EffectStarted) &&
        destination.valid() &&
        destination.identity == current.identity &&
        destination.commandCount == current.stagedCommandCount) {
      result.state.phase = ReplayTransactionPhase::DestinationReceived;
      result.state.destination = destination;
      result.accepted = true;
    }
    break;
  case ReplayTransactionEvent::Commit:
    if (current.phase == ReplayTransactionPhase::DestinationReceived) {
      result.state.phase = ReplayTransactionPhase::Committed;
      result.accepted = true;
    }
    break;
  case ReplayTransactionEvent::Rollback:
    if (!current.irreversible()) {
      result.state.phase = ReplayTransactionPhase::RolledBack;
      result.accepted = true;
    }
    break;
  case ReplayTransactionEvent::FailStop:
    result.state.phase = ReplayTransactionPhase::FailedStop;
    result.accepted = true;
    break;
  }
  return result;
}

// Sparse, fixed-capacity undo journal.  It stores only the first value seen
// for each changed slot.  Table metadata (occupied/dirty/count/hash) is also
// captured on first touch, so restoring two keys in one machine word is exact.
// Constants use one bounded byte arena instead of a full DeviceState copy.
class DeviceStateUndoJournal final {
 public:
  enum class ConstantKind : std::uint8_t {
    VertexFloat,
    VertexInt,
    VertexBool,
    PixelFloat,
    PixelInt,
    PixelBool,
  };

  static constexpr std::size_t kConstantUndoCapacity =
      core::kMaxVertexConstants + core::kMaxIntegerConstants +
      core::kMaxBoolConstants + core::kMaxPixelConstants +
      core::kMaxIntegerConstants + core::kMaxBoolConstants;
  static constexpr std::size_t kConstantArenaCapacity =
      sizeof(core::VertexShaderConstants) + sizeof(core::PixelShaderConstants);
  static constexpr std::size_t kConstantKindCount = 6u;
  static constexpr std::size_t kConstantPresenceWords =
      (std::max({core::kMaxVertexConstants, core::kMaxPixelConstants,
                 core::kMaxIntegerConstants, core::kMaxBoolConstants}) +
       63u) /
      64u;

  DeviceStateUndoJournal() noexcept = default;
  DeviceStateUndoJournal(const DeviceStateUndoJournal&) = delete;
  DeviceStateUndoJournal& operator=(const DeviceStateUndoJournal&) = delete;

  void clear() noexcept;
  bool overflowed() const noexcept { return overflowed_; }
  bool empty() const noexcept { return entryCount_ == 0; }
  std::size_t entryCount() const noexcept { return entryCount_; }
  std::size_t constantBytes() const noexcept { return constantArenaUsed_; }

  bool captureViewport(const core::DeviceState& state) noexcept;
  bool captureScissor(const core::DeviceState& state) noexcept;
  bool captureMaterial(const core::DeviceState& state) noexcept;
  bool captureLight(const core::DeviceState& state, core::u32 index) noexcept;
  bool captureLightEnabled(const core::DeviceState& state,
                           core::u32 index) noexcept;
  bool captureStream(const core::DeviceState& state, core::u32 index) noexcept;
  bool captureIndex(const core::DeviceState& state) noexcept;
  bool captureVertexDeclaration(const core::DeviceState& state) noexcept;
  bool captureFvf(const core::DeviceState& state) noexcept;
  bool captureShader(const core::DeviceState& state, bool vertex) noexcept;
  bool captureTexture(const core::DeviceState& state, core::u32 index) noexcept;
  bool captureRenderTarget(const core::DeviceState& state,
                          core::u32 index) noexcept;
  bool captureDepthStencil(const core::DeviceState& state) noexcept;
  bool captureClipPlane(const core::DeviceState& state,
                        core::u32 index) noexcept;
  bool captureInScene(const core::DeviceState& state) noexcept;
  bool captureRenderState(const core::DeviceState& state,
                          core::u32 key) noexcept;
  bool captureTextureStageState(const core::DeviceState& state,
                                core::u32 stage, core::u32 key) noexcept;
  bool captureSamplerState(const core::DeviceState& state,
                           core::u32 sampler, core::u32 key) noexcept;
  bool captureTransform(const core::DeviceState& state,
                        core::u32 key) noexcept;
  bool captureConstantRange(const core::DeviceState& state, ConstantKind kind,
                            core::u32 start, core::u32 count) noexcept;

  // Restore is valid only before an irreversible effect.  The caller owns
  // that phase guard; this method is intentionally a mechanical operation.
  void restore(core::DeviceState& state) noexcept;

 private:
  template <std::size_t MaxEntries>
  struct TableUndo {
    struct Entry {
      core::u32 value = 0;
      bool captured = false;
    };
    std::array<Entry, MaxEntries> entries{};
    std::array<std::uint64_t, (MaxEntries + 63u) / 64u> occupied{};
    std::array<std::uint64_t, (MaxEntries + 63u) / 64u> dirty{};
    std::array<bool, (MaxEntries + 63u) / 64u> wordCaptured{};
    core::u32 count = 0;
    std::uint64_t rollingHash = 0;
    bool metadataCaptured = false;
  };

  struct TransformUndo {
    struct Entry {
      core::Matrix4x4 value{};
      bool captured = false;
    };
    std::array<Entry, core::kMaxTransformSlots> entries{};
    std::array<std::uint64_t, (core::kMaxTransformSlots + 63u) / 64u>
        occupied{};
    std::array<std::uint64_t, (core::kMaxTransformSlots + 63u) / 64u> dirty{};
    std::array<bool, (core::kMaxTransformSlots + 63u) / 64u> wordCaptured{};
    core::u32 count = 0;
    std::uint64_t rollingHash = 0;
    bool metadataCaptured = false;
  };

  struct ConstantUndo {
    ConstantKind kind = ConstantKind::VertexFloat;
    core::u32 index = 0;
    std::uint32_t arenaOffset = 0;
    std::uint16_t byteSize = 0;
  };

  struct StreamUndo {
    std::shared_ptr<core::Buffer> buffer;
    core::u32 offset = 0;
    core::u32 stride = 0;
    core::u32 frequency = 1;
    bool captured = false;
  };

  template <std::size_t MaxEntries>
  bool captureTableEntry(TableUndo<MaxEntries>& undo,
                         const core::StateValueTable<MaxEntries>& table,
                         core::u32 key) noexcept;
  bool captureConstant(const core::DeviceState& state, ConstantKind kind,
                       core::u32 index) noexcept;
  bool reserveEntry() noexcept;

  std::size_t entryCount_ = 0;
  bool overflowed_ = false;
  std::array<std::byte, kConstantArenaCapacity> constantArena_{};
  std::size_t constantArenaUsed_ = 0;
  std::array<ConstantUndo, kConstantUndoCapacity> constants_{};
  std::size_t constantCount_ = 0;
  std::array<std::array<std::uint64_t, kConstantPresenceWords>,
             kConstantKindCount>
      constantCaptured_{};

  bool viewportCaptured_ = false;
  core::Viewport viewport_{};
  bool scissorCaptured_ = false;
  core::Rect scissor_{ };
  bool scissorEnabled_ = false;
  bool materialCaptured_ = false;
  core::Material material_{};
  std::array<bool, core::kMaxLights> lightCaptured_{};
  std::array<core::Light, core::kMaxLights> lights_{};
  std::array<bool, core::kMaxLights> lightEnabledCaptured_{};
  std::array<bool, core::kMaxLights> lightEnabled_{};
  std::array<StreamUndo, core::kMaxStreams> streams_{};
  bool indexCaptured_ = false;
  std::shared_ptr<core::Buffer> indexBuffer_;
  core::IndexType indexType_ = core::IndexType::UInt16;
  bool vertexDeclarationCaptured_ = false;
  core::VertexDeclSnapshot vertexDeclaration_{};
  bool fvfCaptured_ = false;
  core::u32 fvf_ = 0;
  bool vertexShaderCaptured_ = false;
  core::ShaderRef vertexShader_{};
  bool pixelShaderCaptured_ = false;
  core::ShaderRef pixelShader_{};
  std::array<bool, core::kMaxTextures> textureCaptured_{};
  std::array<std::shared_ptr<core::Texture>, core::kMaxTextures> textures_{};
  std::array<bool, core::kMaxRenderTargets> renderTargetCaptured_{};
  std::array<core::RenderTargetAttachment, core::kMaxRenderTargets>
      renderTargets_{};
  bool depthStencilCaptured_ = false;
  core::RenderTargetAttachment depthStencil_{};
  std::array<bool, core::kMaxClipPlanes> clipPlaneCaptured_{};
  std::array<core::ClipPlane, core::kMaxClipPlanes> clipPlanes_{};
  bool inSceneCaptured_ = false;
  bool inScene_ = false;

  TableUndo<core::kMaxStateSlots> renderStates_{};
  std::array<TableUndo<core::kMaxTextureStageStates>, core::kMaxTextureStages>
      textureStageStates_{};
  std::array<TableUndo<core::kMaxSamplerStates>, core::kMaxSamplers>
      samplerStates_{};
  TransformUndo transforms_{};
};

// Source-wide owner used by the production replay loop.  It keeps the phase
// protocol and its state journal together, so a caller cannot accidentally
// commit a destination while retaining a rollback-only journal.  The owner
// remains allocation-free; the journal is bounded and lives with the source.
class ReplayTransaction final {
 public:
  ReplayTransaction() noexcept = default;

  void begin(ReplaySourceIdentity identity,
             const core::Device* device = nullptr) noexcept {
    state_ = ReplayTransactionState{.identity = identity};
    journal_.clear();
    progress_.reset();
    if (device) progress_ = device->replayProgressCheckpoint();
  }

  const ReplayTransactionState& state() const noexcept { return state_; }
  DeviceStateUndoJournal& journal() noexcept { return journal_; }
  const DeviceStateUndoJournal& journal() const noexcept { return journal_; }

  bool project(ReplayStateProjection projection) noexcept;
  bool stage(ReplayStagedEmission emission) noexcept;
  bool startIrreversibleEffect() noexcept;
  bool receiveDestination(ReplayDestinationReceipt receipt) noexcept;
  bool commit() noexcept;
  bool rollback(core::Device& device) noexcept;
  bool failStop() noexcept;

 private:
  bool advance(ReplayTransactionEvent event,
               ReplayStateProjection projection = {},
               ReplayStagedEmission emission = {},
               ReplayDestinationReceipt destination = {}) noexcept;

  ReplayTransactionState state_{};
  DeviceStateUndoJournal journal_{};
  std::optional<core::Device::ReplayProgressCheckpoint> progress_{};
};

}  // namespace dxmt9::d3d9
