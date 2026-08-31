#pragma once

// Chunk-scoped semantic ownership for the PE recorder.
//
// This is deliberately a value/typed layer.  A producer may pass its
// call-local typed views to admit(), but this owner never stores a wire object
// reference or a borrowed span.  Physical objects are pinned immediately and
// are represented by kind-qualified typed slots for the rest of the chunk.
// The owner is consumed by the opt-in all-family production lane. The
// call-local Present/Readback pilot remains separate and is not an owner.

#include "d3d9_pe_producer_views.hpp"
#include "d3d9_pe_retainer.hpp"
#include "d3d9_pe_semantic_tokens.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace dxmt9::d3d9::pe {

inline constexpr std::uint32_t kPeSemanticNoSlot =
    std::numeric_limits<std::uint32_t>::max();

template <std::uint32_t Kind>
struct PeSemanticQualifiedIdentity {
  D9CWireObjectIdentity value{};

  constexpr bool valid() const noexcept {
    return value.kind == Kind && value.generation != 0u &&
           value.objectId != 0u;
  }
  friend constexpr bool operator==(const PeSemanticQualifiedIdentity& left,
                                   const PeSemanticQualifiedIdentity& right) noexcept {
    return left.value.kind == right.value.kind &&
           left.value.generation == right.value.generation &&
           left.value.objectId == right.value.objectId;
  }
};

using PeSemanticSurfaceIdentity =
    PeSemanticQualifiedIdentity<D9C_CHUNK_HANDLE_KIND_SURFACE>;
using PeSemanticTextureIdentity =
    PeSemanticQualifiedIdentity<D9C_CHUNK_HANDLE_KIND_TEXTURE>;
using PeSemanticBufferIdentity =
    PeSemanticQualifiedIdentity<D9C_CHUNK_HANDLE_KIND_BUFFER>;
using PeSemanticShaderIdentity =
    PeSemanticQualifiedIdentity<D9C_CHUNK_HANDLE_KIND_SHADER>;
using PeSemanticDeclarationIdentity =
    PeSemanticQualifiedIdentity<D9C_CHUNK_HANDLE_KIND_VERTEX_DECL>;
using PeSemanticQueryIdentity =
    PeSemanticQualifiedIdentity<D9C_CHUNK_HANDLE_KIND_QUERY>;

template <typename Object, typename Identity>
struct PeSemanticPhysicalPin {
  Object* object = nullptr;
  Identity identity{};

  bool valid() const noexcept { return object != nullptr && identity.valid(); }
};

template <typename Wire, typename Identity>
struct PeSemanticOwnedBinding {
  Wire wire{};
  std::uint32_t pin = kPeSemanticNoSlot;
  bool hasPin = false;
};

struct PeSemanticArenaRange {
  std::uint32_t offset = 0u;
  std::uint32_t count = 0u;

  bool empty() const noexcept { return count == 0u; }
};

// The input is borrowed only during admit().  It is intentionally closed over
// the current producer vocabulary: fixed values remain typed and only
// constants, UP data, and clear rectangles are span inputs.
struct PeSemanticRecordInput {
  PeSemanticProducerKind producer = PeSemanticProducerKind::DrawPrimitive;
  std::uint32_t recordType = 0u;
  std::uint32_t recordFlags = D9C_COMMAND_CHUNK_RECORD_FLAG_NONE;
  std::uint64_t sourceOrdinal = 0u;
  std::uint64_t recordOrdinal = 0u;

  D9CCommandChunkWireDrawHeader draw{};
  D9CCommandChunkWireSetConst setConst{};
  D9CCommandChunkWireClear clear{};
  D9CCommandChunkWirePresent present{};
  D9CCommandChunkWireStretchRect stretchRect{};
  D9CCommandChunkWireColorFill colorFill{};
  D9CCommandChunkWireUpdateSurface updateSurface{};
  D9CCommandChunkWireQueryIssue queryIssue{};
  std::uint32_t updateFlags = 0u;
  std::uint32_t reszFlags = 0u;
  std::uint32_t mipmapFlags = 0u;

  // Direct fixed-record object inputs, already kind-qualified by the caller.
  SurfaceRef surface0{};
  SurfaceRef surface1{};
  TextureRef texture0{};
  TextureRef texture1{};
  BufferRef buffer0{};
  BufferRef buffer1{};
  ShaderRef shader0{};
  ShaderRef shader1{};
  DeclarationRef declaration{};
  QueryRef query{};

  SparseStateInput sparse{};
  std::span<const std::byte> constantBytes{};
  std::span<const D9CRect> clearRects{};
};

struct PeSemanticRecordSlot {
  PeSemanticProducerKind producer = PeSemanticProducerKind::DrawPrimitive;
  std::uint32_t recordType = 0u;
  std::uint32_t recordFlags = D9C_COMMAND_CHUNK_RECORD_FLAG_NONE;
  std::uint64_t sourceOrdinal = 0u;
  std::uint64_t recordOrdinal = 0u;

  D9CCommandChunkWireDrawHeader draw{};
  D9CCommandChunkWireSetConst setConst{};
  D9CCommandChunkWireClear clear{};
  D9CCommandChunkWirePresent present{};
  D9CCommandChunkWireStretchRect stretchRect{};
  D9CCommandChunkWireColorFill colorFill{};
  D9CCommandChunkWireUpdateSurface updateSurface{};
  D9CCommandChunkWireQueryIssue queryIssue{};
  std::uint32_t updateFlags = 0u;
  std::uint32_t reszFlags = 0u;
  std::uint32_t mipmapFlags = 0u;

  std::uint32_t surface0 = kPeSemanticNoSlot;
  std::uint32_t surface1 = kPeSemanticNoSlot;
  std::uint32_t texture0 = kPeSemanticNoSlot;
  std::uint32_t texture1 = kPeSemanticNoSlot;
  std::uint32_t buffer0 = kPeSemanticNoSlot;
  std::uint32_t buffer1 = kPeSemanticNoSlot;
  std::uint32_t shader0 = kPeSemanticNoSlot;
  std::uint32_t shader1 = kPeSemanticNoSlot;
  std::uint32_t declaration = kPeSemanticNoSlot;
  std::uint32_t query = kPeSemanticNoSlot;

  PeSemanticArenaRange constantBytes{};
  PeSemanticArenaRange vsFloatConstants{};
  PeSemanticArenaRange vsIntConstants{};
  PeSemanticArenaRange vsBoolConstants{};
  PeSemanticArenaRange psFloatConstants{};
  PeSemanticArenaRange psIntConstants{};
  PeSemanticArenaRange psBoolConstants{};
  PeSemanticArenaRange clearRects{};
  PeSemanticArenaRange upIndexBytes{};
  PeSemanticArenaRange upVertexBytes{};
  D9CCommandChunkWireConstantRange vsFloatConstant{};
  D9CCommandChunkWireConstantRange vsIntConstant{};
  D9CCommandChunkWireConstantRange vsBoolConstant{};
  D9CCommandChunkWireConstantRange psFloatConstant{};
  D9CCommandChunkWireConstantRange psIntConstant{};
  D9CCommandChunkWireConstantRange psBoolConstant{};

  PeSemanticArenaRange renderStates{};
  PeSemanticArenaRange textures{};
  PeSemanticArenaRange streams{};
  PeSemanticArenaRange shaders{};
  PeSemanticArenaRange vertexInputs{};
  PeSemanticArenaRange indexBuffers{};
  PeSemanticArenaRange renderTargets{};
  PeSemanticArenaRange depthStencils{};
  PeSemanticArenaRange viewports{};
  PeSemanticArenaRange scissors{};
  PeSemanticArenaRange materials{};
  PeSemanticArenaRange clipPlanes{};
  PeSemanticArenaRange textureStageStates{};
  PeSemanticArenaRange samplerStates{};
  PeSemanticArenaRange transforms{};
  PeSemanticArenaRange lights{};
  PeSemanticArenaRange lightEnables{};
};

static_assert(std::is_trivially_copyable_v<PeSemanticRecordInput>);
static_assert(std::is_trivially_copyable_v<PeSemanticRecordSlot>);

// Emission never allocates. The caller supplies the three fixed-role regions
// for normal segmented transport or one aligned extent for the contiguous
// ExactFixed differential oracle; the owner only writes after a complete
// checked plan has been formed.
struct PeSemanticSegmentedEmission {
  D9CCommandChunkSegmentedTransportV1 transport{};
  std::uint32_t wireBytes = 0u;

  bool valid() const noexcept {
    return transport.header.recordCount != 0u && wireBytes != 0u &&
           transport.recordReserved == 0u && transport.handleReserved == 0u &&
           transport.payloadReserved == 0u;
  }
};

struct PeSemanticExactFixedEmission {
  D9CCommandChunkSegmentedTransportV1 transport{};
  std::span<const std::byte> wire{};
  std::uint32_t wireBytes = 0u;

  bool valid() const noexcept {
    return wireBytes != 0u && wire.size() == wireBytes &&
           transport.header.recordCount != 0u;
  }
};

struct PeSemanticPinIndexEntry {
  D9CWireObjectIdentity identity{};
  std::uint32_t pin = kPeSemanticNoSlot;
  bool occupied = false;
};

template <std::size_t N>
using PeSemanticPinIndex = std::array<PeSemanticPinIndexEntry, N>;

template <std::size_t MaxRecords, std::size_t MaxPins,
          std::size_t MaxSemanticBytes, std::size_t MaxRects,
          std::size_t MaxSparseValues>
struct PeSemanticBatchStorage {
  static constexpr std::size_t pinIndexCapacity = MaxPins * 4u + 1u;
  std::array<PeSemanticRecordSlot, MaxRecords> records{};
  std::array<PeSemanticPhysicalPin<D9CSurface, PeSemanticSurfaceIdentity>, MaxPins> surfaces{};
  std::array<PeSemanticPhysicalPin<D9CTexture, PeSemanticTextureIdentity>, MaxPins> textures{};
  std::array<PeSemanticPhysicalPin<D9CBuffer, PeSemanticBufferIdentity>, MaxPins> buffers{};
  std::array<PeSemanticPhysicalPin<D9CShader, PeSemanticShaderIdentity>, MaxPins> shaders{};
  std::array<PeSemanticPhysicalPin<D9CVertexDecl, PeSemanticDeclarationIdentity>, MaxPins> declarations{};
  std::array<PeSemanticPhysicalPin<D9CQuery, PeSemanticQueryIdentity>, MaxPins> queries{};
  PeSemanticPinIndex<pinIndexCapacity> surfaceIdentityIndex{};
  PeSemanticPinIndex<pinIndexCapacity> surfaceObjectIndex{};
  PeSemanticPinIndex<pinIndexCapacity> textureIdentityIndex{};
  PeSemanticPinIndex<pinIndexCapacity> textureObjectIndex{};
  PeSemanticPinIndex<pinIndexCapacity> bufferIdentityIndex{};
  PeSemanticPinIndex<pinIndexCapacity> bufferObjectIndex{};
  PeSemanticPinIndex<pinIndexCapacity> shaderIdentityIndex{};
  PeSemanticPinIndex<pinIndexCapacity> shaderObjectIndex{};
  PeSemanticPinIndex<pinIndexCapacity> declarationIdentityIndex{};
  PeSemanticPinIndex<pinIndexCapacity> declarationObjectIndex{};
  PeSemanticPinIndex<pinIndexCapacity> queryIdentityIndex{};
  PeSemanticPinIndex<pinIndexCapacity> queryObjectIndex{};
  std::array<std::byte, MaxSemanticBytes> constantBytes{};
  std::array<D9CRect, MaxRects> rects{};
  std::array<D9CCommandChunkWireRenderState, MaxSparseValues> renderStates{};
  std::array<PeSemanticOwnedBinding<D9CCommandChunkWireTextureBinding, PeSemanticTextureIdentity>, MaxSparseValues> texturesArena{};
  std::array<PeSemanticOwnedBinding<D9CCommandChunkWireStreamBinding, PeSemanticBufferIdentity>, MaxSparseValues> streamsArena{};
  std::array<PeSemanticOwnedBinding<D9CCommandChunkWireShaderBinding, PeSemanticShaderIdentity>, MaxSparseValues> shadersArena{};
  std::array<PeSemanticOwnedBinding<D9CCommandChunkWireVertexInput, PeSemanticDeclarationIdentity>, MaxSparseValues> vertexInputsArena{};
  std::array<PeSemanticOwnedBinding<D9CCommandChunkWireIndexBinding, PeSemanticBufferIdentity>, MaxSparseValues> indexBuffersArena{};
  std::array<PeSemanticOwnedBinding<D9CCommandChunkWireRenderTargetBinding, PeSemanticSurfaceIdentity>, MaxSparseValues> renderTargetsArena{};
  std::array<PeSemanticOwnedBinding<D9CCommandChunkWireDepthStencilBinding, PeSemanticSurfaceIdentity>, MaxSparseValues> depthStencilsArena{};
  std::array<D9CViewport, MaxSparseValues> viewports{};
  std::array<D9CRect, MaxSparseValues> scissors{};
  std::array<D9CMaterial, MaxSparseValues> materials{};
  std::array<D9CCommandChunkWireClipPlane, MaxSparseValues> clipPlanes{};
  std::array<D9CDrawPacketTextureStageState, MaxSparseValues> textureStageStates{};
  std::array<D9CDrawPacketSamplerState, MaxSparseValues> samplerStates{};
  std::array<D9CDrawPacketTransform, MaxSparseValues> transforms{};
  std::array<D9CCommandChunkWireLight, MaxSparseValues> lights{};
  std::array<D9CCommandChunkWireLightEnable, MaxSparseValues> lightEnables{};
  // Fixed contiguous compatibility output. It is storage-owned so exact
  // emission never allocates after construction and remains valid through
  // bridge/capture settlement.
  static constexpr std::size_t maxWireBytes =
      MaxSemanticBytes + MaxSparseValues * 1024u +
      MaxRecords * sizeof(D9CCommandChunkWireRecordHeader) +
      MaxPins * 6u * sizeof(D9CCommandChunkWireHandleEntry) +
      sizeof(D9CCommandChunkWireHeader) + 64u;
  std::array<std::byte, maxWireBytes> wire{};
};

// The template capacities make overflow tests cheap while the defaults cover
// the ordinary bounded recorder chunk.  Sparse arrays are typed arenas; they
// are not a byte serialization of SparseStateInput.
template <std::size_t MaxRecords = 64u, std::size_t MaxPins = 256u,
          std::size_t MaxSemanticBytes = 1u << 20u,
          std::size_t MaxRects = 1024u, std::size_t MaxSparseValues = 2048u>
class PeSemanticBatchOwner final {
 public:
  enum class AdmissionFailure : std::uint8_t {
    None,
    Unavailable,
    Header,
    Fixed,
    DirectPins,
    Sparse,
    SparseSchema,
    SparseArena,
    SparseTextures,
    SparseStreams,
    SparseShaders,
    SparseVertexInputs,
    SparseIndexBuffers,
    SparseRenderTargets,
    SparseDepthStencils,
    VariablePayload,
    EmissionMetrics,
    Settlement,
  };

  using Storage = PeSemanticBatchStorage<MaxRecords, MaxPins,
                                         MaxSemanticBytes, MaxRects,
                                         MaxSparseValues>;
  static constexpr std::size_t maxRecords = MaxRecords;
  static constexpr std::size_t maxPins = MaxPins;
  static constexpr std::size_t maxSemanticBytes = MaxSemanticBytes;
  static constexpr std::size_t storageBytes = sizeof(Storage);
  static constexpr std::size_t warmRetainerCapacity =
      MaxPins * (D3D9PePendingCommandRetainer::kWarmEpochs + 2u);

  // Native tests use this tag to exercise the unavailable-owner contract
  // deterministically, without depending on host memory pressure.
  struct FailConstructionForTesting {};

  PeSemanticBatchOwner() noexcept
      : storage_(new (std::nothrow) Storage{}),
        retainer_(warmRetainerCapacity) {
    // Publish readiness only after both bounded heap storage and every
    // retainer reserve have completed successfully.  Neither admission nor
    // rollback needs to allocate after this point.
    ready_ = storage_ != nullptr && retainer_.constructionSucceeded();
  }
  explicit PeSemanticBatchOwner(FailConstructionForTesting) noexcept
      : storage_(new (std::nothrow) Storage{}),
        retainer_(MaxPins,
                  D3D9PePendingCommandRetainer::ConstructionMode::FailForTesting) {}
  PeSemanticBatchOwner(const PeSemanticBatchOwner&) = delete;
  PeSemanticBatchOwner& operator=(const PeSemanticBatchOwner&) = delete;

  bool constructionSucceeded() const noexcept { return ready_; }
  AdmissionFailure lastAdmissionFailure() const noexcept {
    return lastAdmissionFailure_;
  }

  // Pure CapacityPre query. It checks only bounded owner storage; schema,
  // identity and callback validity remain admission concerns. Counts are
  // conservative for duplicate pins inside one input, which may seal an
  // experimental semantic chunk early but can never admit an overflowing
  // record or turn invalid semantics into valid semantics.
  bool canAdmitStorage(const PeSemanticRecordInput& input) const noexcept {
    if (!ready_ || recordCount_ >= MaxRecords) return false;
    const auto fitsSparse = [&](std::size_t kind,
                                std::size_t count) noexcept {
      return count <= MaxSparseValues - sparseCounts_.values[kind];
    };
    const auto& s = input.sparse;
    if (!fitsSparse(kRenderStates, s.renderStates.size()) ||
        !fitsSparse(kTextures, s.textures.size()) ||
        !fitsSparse(kStreams, s.streams.size()) ||
        !fitsSparse(kShaders, s.shaders.size()) ||
        !fitsSparse(kVertexInputs, s.vertexInputs.size()) ||
        !fitsSparse(kIndexBuffers, s.indexBuffers.size()) ||
        !fitsSparse(kRenderTargets, s.renderTargets.size()) ||
        !fitsSparse(kDepthStencils, s.depthStencils.size()) ||
        !fitsSparse(kViewports, s.viewports.size()) ||
        !fitsSparse(kScissors, s.scissors.size()) ||
        !fitsSparse(kMaterials, s.materials.size()) ||
        !fitsSparse(kClipPlanes, s.clipPlanes.size()) ||
        !fitsSparse(kTextureStageStates, s.textureStageStates.size()) ||
        !fitsSparse(kSamplerStates, s.samplerStates.size()) ||
        !fitsSparse(kTransforms, s.transforms.size()) ||
        !fitsSparse(kLights, s.lights.size()) ||
        !fitsSparse(kLightEnables, s.lightEnables.size()) ||
        input.clearRects.size() > MaxRects - rectCount_) {
      return false;
    }
    const auto fitsPins = [](std::size_t used, std::size_t direct,
                             std::size_t sparse) noexcept {
      return direct <= MaxPins - used && sparse <= MaxPins - used - direct;
    };
    if (!fitsPins(surfaceCount_,
                  static_cast<std::size_t>(input.surface0.valid()) +
                      static_cast<std::size_t>(input.surface1.valid()),
                  s.renderTargets.size() + s.depthStencils.size()) ||
        !fitsPins(textureCount_,
                  static_cast<std::size_t>(input.texture0.valid()) +
                      static_cast<std::size_t>(input.texture1.valid()),
                  s.textures.size()) ||
        !fitsPins(bufferCount_,
                  static_cast<std::size_t>(input.buffer0.valid()) +
                      static_cast<std::size_t>(input.buffer1.valid()),
                  s.streams.size() + s.indexBuffers.size()) ||
        !fitsPins(shaderCount_,
                  static_cast<std::size_t>(input.shader0.valid()) +
                      static_cast<std::size_t>(input.shader1.valid()),
                  s.shaders.size()) ||
        !fitsPins(declarationCount_,
                  static_cast<std::size_t>(input.declaration.valid()),
                  s.vertexInputs.size()) ||
        !fitsPins(queryCount_, static_cast<std::size_t>(input.query.valid()),
                  0u)) {
      return false;
    }
    std::size_t bytes = semanticBytes_;
    const auto addBytes = [&](std::size_t count) noexcept {
      if (count > MaxSemanticBytes - bytes) return false;
      bytes += count;
      return true;
    };
    return addBytes(input.constantBytes.size()) &&
           addBytes(s.vsFloatConstants.registerBytes.size()) &&
           addBytes(s.vsIntConstants.registerBytes.size()) &&
           addBytes(s.vsBoolConstants.registerBytes.size()) &&
           addBytes(s.psFloatConstants.registerBytes.size()) &&
           addBytes(s.psIntConstants.registerBytes.size()) &&
           addBytes(s.psBoolConstants.registerBytes.size()) &&
           addBytes(s.upIndexData.size()) && addBytes(s.upVertexData.size());
  }

  // A typed adapter used by producer-family call sites. It intentionally
  // aliases admission rather than exposing the owner internals to producers.
  bool appendOwnedRecord(const PeSemanticRecordInput& input) noexcept {
    return admit(input);
  }

  // A producer settlement callback runs while the admission checkpoint is
  // still live, so a PendingDelta validation failure rolls back atomically
  // without adding checkpoint state to the small owner shell.
  template <typename Commit>
    requires std::is_nothrow_invocable_r_v<bool, Commit&>
  bool appendOwnedRecord(const PeSemanticRecordInput& input,
                         Commit&& commit) noexcept {
    lastAdmissionFailure_ = AdmissionFailure::None;
    if (!ready_) {
      lastAdmissionFailure_ = AdmissionFailure::Unavailable;
      return false;
    }
    const auto checkpoint = checkpointState();
    retainedCheckpoint_ = checkpoint.retained;
    if (!validAdmissionHeader(input)) {
      lastAdmissionFailure_ = AdmissionFailure::Header;
      rollback(checkpoint);
      return false;
    }
    if (!copyFixedValues(input)) {
      lastAdmissionFailure_ = AdmissionFailure::Fixed;
      rollback(checkpoint);
      return false;
    }
    auto& slot = storage_->records[recordCount_];
    if (!copyDirectPins(input, slot)) {
      lastAdmissionFailure_ = AdmissionFailure::DirectPins;
      rollback(checkpoint);
      return false;
    }
    if (!copySparse(input, slot)) {
      if (lastAdmissionFailure_ == AdmissionFailure::None)
        lastAdmissionFailure_ = AdmissionFailure::Sparse;
      rollback(checkpoint);
      return false;
    }
    if (!copyVariablePayloads(input, slot)) {
      lastAdmissionFailure_ = AdmissionFailure::VariablePayload;
      rollback(checkpoint);
      return false;
    }
    if (!cacheEmissionMetrics(slot)) {
      lastAdmissionFailure_ = AdmissionFailure::EmissionMetrics;
      rollback(checkpoint);
      return false;
    }
    ++recordCount_;
    auto&& callback = commit;
    if (!callback()) {
      lastAdmissionFailure_ = AdmissionFailure::Settlement;
      rollback(checkpoint);
      return false;
    }
    lastSourceOrdinal_ = input.sourceOrdinal;
    lastRecordOrdinal_ = input.recordOrdinal;
    return true;
  }

  template <typename Visit>
    requires std::is_nothrow_invocable_r_v<
        bool, Visit&, PeSemanticProducerKind,
        const PeSemanticRecordSlot&>
  bool visitOwnedRecords(Visit&& visit) const noexcept {
    if (!ready_ || recordCount_ == 0u) return false;
    auto&& visitor = visit;
    for (std::size_t i = 0u; i < recordCount_; ++i) {
      if (!visitor(storage_->records[i].producer, storage_->records[i]))
        return false;
    }
    return true;
  }

  // Admission is atomic.  Every counter and every typed pin is restored when
  // any validation, arena, or retain operation fails.
  bool admit(const PeSemanticRecordInput& input) noexcept {
    lastAdmissionFailure_ = AdmissionFailure::None;
    if (!ready_) {
      lastAdmissionFailure_ = AdmissionFailure::Unavailable;
      return false;
    }
    const auto checkpoint = checkpointState();
    retainedCheckpoint_ = checkpoint.retained;
    if (!validAdmissionHeader(input)) {
      lastAdmissionFailure_ = AdmissionFailure::Header;
      rollback(checkpoint);
      return false;
    }
    if (!copyFixedValues(input)) {
      lastAdmissionFailure_ = AdmissionFailure::Fixed;
      rollback(checkpoint);
      return false;
    }
    auto& slot = storage_->records[recordCount_];
    if (!copyDirectPins(input, slot)) {
      lastAdmissionFailure_ = AdmissionFailure::DirectPins;
      rollback(checkpoint);
      return false;
    }
    if (!copySparse(input, slot)) {
      if (lastAdmissionFailure_ == AdmissionFailure::None)
        lastAdmissionFailure_ = AdmissionFailure::Sparse;
      rollback(checkpoint);
      return false;
    }
    if (!copyVariablePayloads(input, slot)) {
      lastAdmissionFailure_ = AdmissionFailure::VariablePayload;
      rollback(checkpoint);
      return false;
    }
    if (!cacheEmissionMetrics(slot)) {
      lastAdmissionFailure_ = AdmissionFailure::EmissionMetrics;
      rollback(checkpoint);
      return false;
    }
    ++recordCount_;
    lastSourceOrdinal_ = input.sourceOrdinal;
    lastRecordOrdinal_ = input.recordOrdinal;
    return true;
  }

  // Destructive reset used by Reset/teardown/discard. It drops both current
  // and warm retainer pins; successful chunk settlement uses settle() so the
  // legacy warm-pin epoch optimization remains intact.
  void reset() noexcept {
    retainer_.clear();
    clearChunkState();
  }

  // Successful settlement closes one chunk epoch while preserving recently
  // used retainer entries. The typed slots and arenas are still cleared, so a
  // later chunk reacquires its own slot; the retainer index refreshes the warm
  // entry without another AddRef. This keeps settlement O(1) in owner state
  // plus the retainer's bounded epoch sweep, matching CommandChunkBuilder.
  bool settle() noexcept {
    if (!ready_ || recordCount_ == 0u) return false;
    retainer_.endEpoch();
    clearChunkState();
    ++settledChunks_;
    return true;
  }

 private:
  void clearChunkState() noexcept {
    lastClearedBytes_ = 0u;
    if (!ready_) return;
    lastClearedBytes_ += clearUsed(storage_->records, recordCount_);
    lastClearedBytes_ += clearUsed(storage_->surfaces, surfaceCount_);
    lastClearedBytes_ += clearUsed(storage_->textures, textureCount_);
    lastClearedBytes_ += clearUsed(storage_->buffers, bufferCount_);
    lastClearedBytes_ += clearUsed(storage_->shaders, shaderCount_);
    lastClearedBytes_ += clearUsed(storage_->declarations, declarationCount_);
    lastClearedBytes_ += clearUsed(storage_->queries, queryCount_);
    lastClearedBytes_ += clearUsed(storage_->constantBytes, semanticBytes_);
    lastClearedBytes_ += clearUsed(storage_->rects, rectCount_);
    lastClearedBytes_ += clearUsed(storage_->renderStates,
                                   sparseCounts_.values[kRenderStates]);
    lastClearedBytes_ += clearUsed(storage_->texturesArena,
                                   sparseCounts_.values[kTextures]);
    lastClearedBytes_ += clearUsed(storage_->streamsArena,
                                   sparseCounts_.values[kStreams]);
    lastClearedBytes_ += clearUsed(storage_->shadersArena,
                                   sparseCounts_.values[kShaders]);
    lastClearedBytes_ += clearUsed(storage_->vertexInputsArena,
                                   sparseCounts_.values[kVertexInputs]);
    lastClearedBytes_ += clearUsed(storage_->indexBuffersArena,
                                   sparseCounts_.values[kIndexBuffers]);
    lastClearedBytes_ += clearUsed(storage_->renderTargetsArena,
                                   sparseCounts_.values[kRenderTargets]);
    lastClearedBytes_ += clearUsed(storage_->depthStencilsArena,
                                   sparseCounts_.values[kDepthStencils]);
    lastClearedBytes_ += clearUsed(storage_->viewports,
                                   sparseCounts_.values[kViewports]);
    lastClearedBytes_ += clearUsed(storage_->scissors,
                                   sparseCounts_.values[kScissors]);
    lastClearedBytes_ += clearUsed(storage_->materials,
                                   sparseCounts_.values[kMaterials]);
    lastClearedBytes_ += clearUsed(storage_->clipPlanes,
                                   sparseCounts_.values[kClipPlanes]);
    lastClearedBytes_ += clearUsed(storage_->textureStageStates,
                                   sparseCounts_.values[kTextureStageStates]);
    lastClearedBytes_ += clearUsed(storage_->samplerStates,
                                   sparseCounts_.values[kSamplerStates]);
    lastClearedBytes_ += clearUsed(storage_->transforms,
                                   sparseCounts_.values[kTransforms]);
    lastClearedBytes_ += clearUsed(storage_->lights,
                                   sparseCounts_.values[kLights]);
    lastClearedBytes_ += clearUsed(storage_->lightEnables,
                                   sparseCounts_.values[kLightEnables]);
    recordCount_ = 0u;
    semanticBytes_ = 0u;
    emissionHandleCount_ = 0u;
    emissionPayloadBytes_ = 0u;
    rectCount_ = 0u;
    sparseCounts_ = {};
    surfaceCount_ = textureCount_ = bufferCount_ = shaderCount_ =
        declarationCount_ = queryCount_ = 0u;
    clearPinIndexes();
  }

  template <typename Array>
  static std::size_t clearUsed(Array& values, std::size_t count) noexcept {
    count = std::min(count, values.size());
    std::fill_n(values.begin(), count, typename Array::value_type{});
    return count * sizeof(typename Array::value_type);
  }

 public:

  std::size_t size() const noexcept { return recordCount_; }
  std::size_t retainedCount() const noexcept { return retainer_.size(); }
  bool referencesBuffer(const D9CBuffer* object) const noexcept {
    if (!object) return false;
    for (std::size_t i = 0u; i < bufferCount_; ++i) {
      if (storage_->buffers[i].object == object) return true;
    }
    return false;
  }
  std::size_t semanticBytes() const noexcept { return semanticBytes_; }
  std::size_t clearedBytesLastBoundary() const noexcept {
    return lastClearedBytes_;
  }
  std::size_t rectCount() const noexcept { return rectCount_; }
  std::uint64_t settledChunks() const noexcept { return settledChunks_; }
  std::uint64_t nextSourceOrdinal() const noexcept {
    return lastSourceOrdinal_ == std::numeric_limits<std::uint64_t>::max()
        ? 0u : lastSourceOrdinal_ + 1u;
  }
  std::uint64_t nextRecordOrdinal() const noexcept {
    return lastRecordOrdinal_ == std::numeric_limits<std::uint64_t>::max()
        ? 0u : lastRecordOrdinal_ + 1u;
  }
  bool emissionMetrics(std::size_t& handles, std::size_t& payload,
                       std::size_t& wire) const noexcept {
    // Admission maintains these frontiers transactionally, so recorder
    // checkpoints and cadence probes are O(1). The full record walk remains
    // reserved for the final immutable emission validation/write.
    if (!ready_ || recordCount_ == 0u ||
        emissionHandleCount_ > std::numeric_limits<std::uint32_t>::max() ||
        emissionPayloadBytes_ > std::numeric_limits<std::uint32_t>::max()) {
      return false;
    }
    const auto layout = planExactCommandChunkLayout(
        static_cast<std::uint32_t>(recordCount_),
        static_cast<std::uint32_t>(emissionHandleCount_),
        static_cast<std::uint32_t>(emissionPayloadBytes_));
    if (!layout.valid() || layout.totalBytes > D9C_COMMAND_CHUNK_MAX_TOTAL_WIRE_BYTES)
      return false;
    handles = emissionHandleCount_;
    payload = emissionPayloadBytes_;
    wire = layout.totalBytes;
    return true;
  }

  // Normal P2 target: records, handles, and payload are independently owned
  // fixed roles. No header or role padding is copied into those spans.
  bool emitSegmented(std::span<std::byte> recordRegion,
                     std::span<std::byte> handleRegion,
                     std::span<std::byte> payloadRegion,
                     PeSemanticSegmentedEmission& out,
                     std::uint64_t renderTapeCaptureToken = 0u,
                     std::uint64_t renderTapeEventOrdinal = 0u) const noexcept {
    out = {};
    EmissionPlan plan{};
    if (!buildEmissionPlan(plan) ||
        recordRegion.size() < plan.recordBytes() ||
        handleRegion.size() < plan.handleBytes() ||
        payloadRegion.size() < plan.payloadBytes) {
      return false;
    }
    if (!writeEmission(plan, recordRegion, handleRegion, payloadRegion)) {
      return false;
    }
    out.transport = makeTransport(plan,
                                  recordRegion.first(plan.recordBytes()),
                                  handleRegion.first(plan.handleBytes()),
                                  payloadRegion.first(plan.payloadBytes),
                                  renderTapeCaptureToken,
                                  renderTapeEventOrdinal);
    out.wireBytes = plan.wireBytes;
    return out.valid();
  }

  // Compatibility/differential target: one canonical contiguous D9C V2
  // extent. It traverses the same immutable slots and handle identities as
  // emitSegmented(), and therefore cannot drift in order or alignment.
  bool emitExactFixed(std::span<std::byte> destination,
                      PeSemanticExactFixedEmission& out) const noexcept {
    out = {};
    EmissionPlan plan{};
    if (!buildEmissionPlan(plan) || destination.size() < plan.wireBytes ||
        reinterpret_cast<std::uintptr_t>(destination.data()) %
                alignof(D9CCommandChunkWireHandleEntry) != 0u) {
      return false;
    }
    std::fill(destination.begin(), destination.begin() + plan.wireBytes,
              std::byte{0});
    std::memcpy(destination.data(), &plan.header, sizeof(plan.header));
    auto records = destination.subspan(plan.header.recordTableOffset,
                                        plan.recordBytes());
    auto handles = destination.subspan(plan.header.handleTableOffset,
                                        plan.handleBytes());
    auto payload = destination.subspan(plan.header.payloadArenaOffset,
                                       plan.payloadBytes);
    if (!writeEmission(plan, records, handles, payload)) return false;
    out.transport = makeTransport(plan, records, handles, payload, 0u, 0u);
    out.wire = std::span<const std::byte>(destination.data(), plan.wireBytes);
    out.wireBytes = plan.wireBytes;
    return out.valid();
  }

  bool emitExactFixed(PeSemanticExactFixedEmission& out) const noexcept {
    return emitExactFixed(std::span<std::byte>(storage_->wire), out);
  }

  // Production segmented target backed by the same fixed wire arena.  The
  // three role regions are disjoint and remain owned by this immutable owner
  // until bridge settlement; no temporary allocation or legacy builder is
  // involved.  Capture callers deliberately use emitExactFixed() instead.
  bool emitSegmented(PeSemanticSegmentedEmission& out,
                     std::uint64_t renderTapeCaptureToken = 0u,
                     std::uint64_t renderTapeEventOrdinal = 0u) const noexcept {
    out = {};
    EmissionPlan plan{};
    if (!buildEmissionPlan(plan)) return false;
    std::size_t cursor = 0u;
    std::size_t recordOffset = 0u;
    std::size_t handleOffset = 0u;
    std::size_t payloadOffset = 0u;
    if (!alignEmission(cursor, alignof(D9CCommandChunkWireRecordHeader),
                       recordOffset) ||
        recordOffset > storage_->wire.size() ||
        plan.recordBytes() > storage_->wire.size() - recordOffset) return false;
    cursor = recordOffset + plan.recordBytes();
    if (!alignEmission(cursor, alignof(D9CCommandChunkWireHandleEntry),
                       handleOffset) ||
        handleOffset > storage_->wire.size() ||
        plan.handleBytes() > storage_->wire.size() - handleOffset) return false;
    cursor = handleOffset + plan.handleBytes();
    if (!alignEmission(cursor, alignof(std::max_align_t), payloadOffset) ||
        payloadOffset > storage_->wire.size() ||
        plan.payloadBytes > storage_->wire.size() - payloadOffset) return false;
    if (!writeEmission(
            plan,
            std::span<std::byte>(storage_->wire).subspan(recordOffset,
                                                          plan.recordBytes()),
            std::span<std::byte>(storage_->wire).subspan(handleOffset,
                                                          plan.handleBytes()),
            std::span<std::byte>(storage_->wire).subspan(payloadOffset,
                                                          plan.payloadBytes))) {
      return false;
    }
    out.transport = makeTransport(
        plan,
        std::span<std::byte>(storage_->wire).subspan(recordOffset,
                                                      plan.recordBytes()),
        std::span<std::byte>(storage_->wire).subspan(handleOffset,
                                                      plan.handleBytes()),
        std::span<std::byte>(storage_->wire).subspan(payloadOffset,
                                                      plan.payloadBytes),
        renderTapeCaptureToken, renderTapeEventOrdinal);
    out.wireBytes = plan.wireBytes;
    return out.valid();
  }

  const PeSemanticRecordSlot& record(std::size_t index) const noexcept {
    return storage_->records[index];
  }
  const PeSemanticPhysicalPin<D9CSurface, PeSemanticSurfaceIdentity>&
  surfacePin(std::size_t index) const noexcept { return storage_->surfaces[index]; }
  const PeSemanticPhysicalPin<D9CTexture, PeSemanticTextureIdentity>&
  texturePin(std::size_t index) const noexcept { return storage_->textures[index]; }
  const PeSemanticPhysicalPin<D9CBuffer, PeSemanticBufferIdentity>&
  bufferPin(std::size_t index) const noexcept { return storage_->buffers[index]; }
  const PeSemanticPhysicalPin<D9CShader, PeSemanticShaderIdentity>&
  shaderPin(std::size_t index) const noexcept { return storage_->shaders[index]; }
  const PeSemanticPhysicalPin<D9CVertexDecl, PeSemanticDeclarationIdentity>&
  declarationPin(std::size_t index) const noexcept { return storage_->declarations[index]; }
  const PeSemanticPhysicalPin<D9CQuery, PeSemanticQueryIdentity>&
  queryPin(std::size_t index) const noexcept { return storage_->queries[index]; }

  std::span<const std::byte> constantBytes(const PeSemanticRecordSlot& slot) const noexcept {
    return {storage_->constantBytes.data() + slot.constantBytes.offset,
            slot.constantBytes.count};
  }
  std::span<const D9CRect> clearRects(const PeSemanticRecordSlot& slot) const noexcept {
    return {storage_->rects.data() + slot.clearRects.offset, slot.clearRects.count};
  }
  std::span<const std::byte> upIndexBytes(const PeSemanticRecordSlot& slot) const noexcept {
    return {storage_->constantBytes.data() + slot.upIndexBytes.offset,
            slot.upIndexBytes.count};
  }
  std::span<const std::byte> upVertexBytes(const PeSemanticRecordSlot& slot) const noexcept {
    return {storage_->constantBytes.data() + slot.upVertexBytes.offset,
            slot.upVertexBytes.count};
  }
  std::span<const PeSemanticOwnedBinding<
      D9CCommandChunkWireTextureBinding, PeSemanticTextureIdentity>>
  textureBindings(const PeSemanticRecordSlot& slot) const noexcept {
    return {storage_->texturesArena.data() + slot.textures.offset, slot.textures.count};
  }
  std::span<const PeSemanticOwnedBinding<
      D9CCommandChunkWireStreamBinding, PeSemanticBufferIdentity>>
  streamBindings(const PeSemanticRecordSlot& slot) const noexcept {
    return {storage_->streamsArena.data() + slot.streams.offset, slot.streams.count};
  }
  std::span<const PeSemanticOwnedBinding<
      D9CCommandChunkWireShaderBinding, PeSemanticShaderIdentity>>
  shaderBindings(const PeSemanticRecordSlot& slot) const noexcept {
    return {storage_->shadersArena.data() + slot.shaders.offset, slot.shaders.count};
  }
  std::span<const PeSemanticOwnedBinding<
      D9CCommandChunkWireVertexInput, PeSemanticDeclarationIdentity>>
  vertexInputs(const PeSemanticRecordSlot& slot) const noexcept {
    return {storage_->vertexInputsArena.data() + slot.vertexInputs.offset,
            slot.vertexInputs.count};
  }
  std::span<const PeSemanticOwnedBinding<
      D9CCommandChunkWireIndexBinding, PeSemanticBufferIdentity>>
  indexBuffers(const PeSemanticRecordSlot& slot) const noexcept {
    return {storage_->indexBuffersArena.data() + slot.indexBuffers.offset,
            slot.indexBuffers.count};
  }
  std::span<const PeSemanticOwnedBinding<
      D9CCommandChunkWireRenderTargetBinding, PeSemanticSurfaceIdentity>>
  renderTargets(const PeSemanticRecordSlot& slot) const noexcept {
    return {storage_->renderTargetsArena.data() + slot.renderTargets.offset,
            slot.renderTargets.count};
  }
  std::span<const PeSemanticOwnedBinding<
      D9CCommandChunkWireDepthStencilBinding, PeSemanticSurfaceIdentity>>
  depthStencils(const PeSemanticRecordSlot& slot) const noexcept {
    return {storage_->depthStencilsArena.data() + slot.depthStencils.offset,
            slot.depthStencils.count};
  }

 private:
  struct StateCheckpoint {
    std::size_t records = 0u;
    std::size_t semanticBytes = 0u;
    std::size_t emissionHandles = 0u;
    std::size_t emissionPayloadBytes = 0u;
    std::size_t rects = 0u;
    std::array<std::size_t, 17u> sparse{};
    std::array<std::size_t, 6u> pins{};
    D3D9PePendingCommandRetainer::Acquired retained{};
  };

  struct SparseCounts {
    std::array<std::size_t, 17u> values{};
  };

  static constexpr std::size_t kMaxRecordHandles = 64u;
  // Pin admission is on the producer hot path. Keep exact identity and
  // object-id membership in fixed, typed open-addressed tables so a repeated
  // warm pin does not scan every prior pin. The table is deliberately
  // over-provisioned; probing is bounded by this compile-time constant and
  // never allocates.
  static constexpr std::size_t kPinIndexCapacity = MaxPins * 4u + 1u;

  using PinIndexEntry = PeSemanticPinIndexEntry;

  template <std::size_t N>
  using PinIndex = PeSemanticPinIndex<N>;

  struct EmissionRecordPlan {
    std::size_t payloadOffset = 0u;
    std::size_t payloadBytes = 0u;
    std::size_t firstHandle = 0u;
    std::size_t handleCount = 0u;
  };

  struct EmissionPlan {
    std::array<EmissionRecordPlan, MaxRecords> records{};
    D9CCommandChunkWireHeader header{};
    std::size_t recordCount = 0u;
    std::size_t handleCount = 0u;
    std::size_t payloadBytes = 0u;
    std::size_t wireBytes = 0u;

    std::size_t recordBytes() const noexcept {
      return recordCount * sizeof(D9CCommandChunkWireRecordHeader);
    }
    std::size_t handleBytes() const noexcept {
      return handleCount * sizeof(D9CCommandChunkWireHandleEntry);
    }
  };

  struct EmissionHandleContext {
    std::array<D9CWireObjectIdentity, kMaxRecordHandles> identities{};
    std::size_t count = 0u;

    bool add(const D9CWireObjectIdentity& identity) noexcept {
      for (std::size_t i = 0u; i < count; ++i) {
        if (identities[i].kind == identity.kind &&
            identities[i].generation == identity.generation &&
            identities[i].objectId == identity.objectId) {
          return true;
        }
      }
      if (count == identities.size()) return false;
      identities[count++] = identity;
      return true;
    }

    bool indexOf(const D9CWireObjectIdentity& identity,
                 std::size_t& out) const noexcept {
      for (std::size_t i = 0u; i < count; ++i) {
        if (identities[i].kind == identity.kind &&
            identities[i].generation == identity.generation &&
            identities[i].objectId == identity.objectId) {
          out = i;
          return true;
        }
      }
      return false;
    }
  };

  static std::size_t pinHash(const D9CWireObjectIdentity& identity,
                             bool includeGeneration) noexcept {
    std::uint64_t value = identity.objectId * 0x9e3779b185ebca87ull;
    value ^= static_cast<std::uint64_t>(identity.kind) *
             0xc2b2ae3d27d4eb4full;
    if (includeGeneration) {
      value ^= identity.generation + 0x165667b19e3779f9ull +
               (value << 6u) + (value >> 2u);
    }
    return static_cast<std::size_t>(value);
  }

  template <std::size_t N>
  static bool findPinIndex(const PinIndex<N>& index,
                           const D9CWireObjectIdentity& identity,
                           bool includeGeneration,
                           std::size_t& out) noexcept {
    const std::size_t start = pinHash(identity, includeGeneration) % N;
    for (std::size_t probe = 0u; probe < N; ++probe) {
      const auto& entry = index[(start + probe) % N];
      if (!entry.occupied) return false;
      const bool matches = entry.identity.kind == identity.kind &&
          entry.identity.objectId == identity.objectId &&
          (!includeGeneration ||
           entry.identity.generation == identity.generation);
      if (matches) {
        out = entry.pin;
        return true;
      }
    }
    return false;
  }

  template <std::size_t N>
  static bool insertPinIndex(PinIndex<N>& index,
                             const D9CWireObjectIdentity& identity,
                             std::uint32_t pin,
                             bool includeGeneration = true) noexcept {
    const std::size_t start = pinHash(identity, includeGeneration) % N;
    for (std::size_t probe = 0u; probe < N; ++probe) {
      auto& entry = index[(start + probe) % N];
      if (!entry.occupied ||
          (entry.identity.kind == identity.kind &&
           entry.identity.objectId == identity.objectId &&
           (!includeGeneration ||
            entry.identity.generation == identity.generation))) {
        entry.identity = identity;
        entry.pin = pin;
        entry.occupied = true;
        return true;
      }
    }
    return false;
  }

  template <std::size_t N>
  static void clearPinIndex(PinIndex<N>& index) noexcept {
    for (auto& entry : index) entry = {};
  }

  template <typename PinArray, std::size_t N>
  static void rebuildPinIndex(const PinArray& pins, std::size_t count,
                              PinIndex<N>& exact, PinIndex<N>& object) noexcept {
    clearPinIndex(exact);
    clearPinIndex(object);
    for (std::size_t i = 0u; i < count; ++i) {
      if (!pins[i].valid()) continue;
      (void)insertPinIndex(exact, pins[i].identity.value,
                           static_cast<std::uint32_t>(i));
      (void)insertPinIndex(object, pins[i].identity.value,
                           static_cast<std::uint32_t>(i), false);
    }
  }

  void clearPinIndexes() noexcept {
    clearPinIndex(storage_->surfaceIdentityIndex);
    clearPinIndex(storage_->surfaceObjectIndex);
    clearPinIndex(storage_->textureIdentityIndex);
    clearPinIndex(storage_->textureObjectIndex);
    clearPinIndex(storage_->bufferIdentityIndex);
    clearPinIndex(storage_->bufferObjectIndex);
    clearPinIndex(storage_->shaderIdentityIndex);
    clearPinIndex(storage_->shaderObjectIndex);
    clearPinIndex(storage_->declarationIdentityIndex);
    clearPinIndex(storage_->declarationObjectIndex);
    clearPinIndex(storage_->queryIdentityIndex);
    clearPinIndex(storage_->queryObjectIndex);
  }

  void rebuildPinIndexes() noexcept {
    rebuildPinIndex(storage_->surfaces, surfaceCount_,
                    storage_->surfaceIdentityIndex, storage_->surfaceObjectIndex);
    rebuildPinIndex(storage_->textures, textureCount_,
                    storage_->textureIdentityIndex, storage_->textureObjectIndex);
    rebuildPinIndex(storage_->buffers, bufferCount_,
                    storage_->bufferIdentityIndex, storage_->bufferObjectIndex);
    rebuildPinIndex(storage_->shaders, shaderCount_,
                    storage_->shaderIdentityIndex, storage_->shaderObjectIndex);
    rebuildPinIndex(storage_->declarations, declarationCount_,
                    storage_->declarationIdentityIndex,
                    storage_->declarationObjectIndex);
    rebuildPinIndex(storage_->queries, queryCount_,
                    storage_->queryIdentityIndex, storage_->queryObjectIndex);
  }

  template <typename PinArray, typename Visit>
  static bool visitPin(const PinArray& pins, std::size_t count,
                       std::uint32_t index, Visit&& visit) noexcept {
    if (index == kPeSemanticNoSlot) return true;
    if (index >= count || !pins[index].valid()) return false;
    return visit(pins[index].identity.value);
  }

  template <typename Visit>
  bool visitRecordHandles(const PeSemanticRecordSlot& slot,
                          Visit&& visit) const noexcept {
    const auto direct = [&](auto const& pins, std::size_t count,
                            std::uint32_t index) noexcept {
      return visitPin(pins, count, index, visit);
    };
    switch (slot.producer) {
      case PeSemanticProducerKind::Present:
        if (!direct(storage_->surfaces, surfaceCount_, slot.surface0)) return false;
        break;
      case PeSemanticProducerKind::StretchRect:
      case PeSemanticProducerKind::UpdateSurface:
      case PeSemanticProducerKind::Readback:
        if (!direct(storage_->surfaces, surfaceCount_, slot.surface0) ||
            !direct(storage_->surfaces, surfaceCount_, slot.surface1)) return false;
        break;
      case PeSemanticProducerKind::ColorFill:
        if (!direct(storage_->surfaces, surfaceCount_, slot.surface0)) return false;
        break;
      case PeSemanticProducerKind::UpdateTexture:
        if (!direct(storage_->textures, textureCount_, slot.texture0) ||
            !direct(storage_->textures, textureCount_, slot.texture1)) return false;
        break;
      case PeSemanticProducerKind::QueryIssue:
        if (!direct(storage_->queries, queryCount_, slot.query)) return false;
        break;
      case PeSemanticProducerKind::ReszDepthResolve:
        if (!direct(storage_->surfaces, surfaceCount_, slot.surface0) ||
            !direct(storage_->textures, textureCount_, slot.texture0)) return false;
        break;
      case PeSemanticProducerKind::GenerateMipmaps:
        if (!direct(storage_->textures, textureCount_, slot.texture0)) return false;
        break;
      case PeSemanticProducerKind::DrawPrimitive:
      case PeSemanticProducerKind::DrawIndexedPrimitive:
      case PeSemanticProducerKind::DrawPrimitiveUp:
      case PeSemanticProducerKind::DrawIndexedPrimitiveUp:
      case PeSemanticProducerKind::ApplyState:
        break;
      default:
        break;
    }
    if (slot.producer != PeSemanticProducerKind::DrawPrimitive &&
        slot.producer != PeSemanticProducerKind::DrawIndexedPrimitive &&
        slot.producer != PeSemanticProducerKind::DrawPrimitiveUp &&
        slot.producer != PeSemanticProducerKind::DrawIndexedPrimitiveUp &&
        slot.producer != PeSemanticProducerKind::ApplyState) {
      return true;
    }
    const auto textures = [&](const auto& item) noexcept {
      return !item.wire.valid || !item.hasPin ||
             visitPin(storage_->textures, textureCount_, item.pin, visit);
    };
    const auto streams = [&](const auto& item) noexcept {
      return !item.wire.valid || !item.hasPin ||
             visitPin(storage_->buffers, bufferCount_, item.pin, visit);
    };
    const auto shaders = [&](const auto& item) noexcept {
      return !item.wire.valid || !item.hasPin ||
             visitPin(storage_->shaders, shaderCount_, item.pin, visit);
    };
    const auto declarations = [&](const auto& item) noexcept {
      return !item.wire.valid || !item.hasPin ||
             visitPin(storage_->declarations, declarationCount_, item.pin, visit);
    };
    const auto surfaces = [&](const auto& item) noexcept {
      return !item.wire.valid || !item.hasPin ||
             visitPin(storage_->surfaces, surfaceCount_, item.pin, visit);
    };
    const auto buffers = [&](const auto& item) noexcept {
      return !item.wire.valid || !item.hasPin ||
             visitPin(storage_->buffers, bufferCount_, item.pin, visit);
    };
    const auto each = [&](const auto& arena, const PeSemanticArenaRange& range,
                          const auto& one) noexcept {
      if (range.offset > arena.size() || range.count > arena.size() - range.offset) return false;
      for (std::size_t i = 0u; i < range.count; ++i)
        if (!one(arena[range.offset + i])) return false;
      return true;
    };
    return each(storage_->texturesArena, slot.textures, textures) &&
           each(storage_->streamsArena, slot.streams, streams) &&
           each(storage_->shadersArena, slot.shaders, shaders) &&
           each(storage_->vertexInputsArena, slot.vertexInputs, declarations) &&
           each(storage_->indexBuffersArena, slot.indexBuffers, buffers) &&
           each(storage_->renderTargetsArena, slot.renderTargets, surfaces) &&
           each(storage_->depthStencilsArena, slot.depthStencils, surfaces);
  }

  static bool alignEmission(std::size_t value, std::size_t alignment,
                            std::size_t& out) noexcept {
    if (alignment == 0u || (alignment & (alignment - 1u)) != 0u ||
        value > std::numeric_limits<std::size_t>::max() - (alignment - 1u)) {
      return false;
    }
    out = (value + alignment - 1u) & ~(alignment - 1u);
    return true;
  }

  static bool addEmissionSection(std::uint16_t kind, std::size_t count,
                                 std::size_t bytes,
                                 std::size_t& cursor) noexcept {
    const auto* rule = sectionRule(kind);
    std::size_t aligned = 0u;
    if (!rule || count == 0u || count > rule->maxCount ||
        count > std::numeric_limits<std::uint32_t>::max() ||
        bytes > std::numeric_limits<std::uint32_t>::max() ||
        !alignEmission(cursor, rule->payloadAlignment, aligned) ||
        bytes > std::numeric_limits<std::size_t>::max() - aligned) {
      return false;
    }
    cursor = aligned + bytes;
    return true;
  }

  static bool addEmissionConstant(std::uint16_t kind,
                                  const PeSemanticArenaRange& bytes,
                                  const D9CCommandChunkWireConstantRange& range,
                                  std::size_t& cursor) noexcept {
    if (range.registerCount == 0u) return bytes.count == 0u;
    const auto* rule = sectionRule(kind);
    if (!rule || range.registerCount > rule->maxCount ||
        static_cast<std::uint64_t>(range.startRegister) +
                range.registerCount > rule->maxCount ||
        bytes.count != static_cast<std::size_t>(range.registerCount) *
                           rule->elementSize) {
      return false;
    }
    return addEmissionSection(kind, range.registerCount,
                              sizeof(D9CCommandChunkWireConstantRange) +
                                  bytes.count,
                              cursor);
  }

  std::size_t sparseSectionCount(const PeSemanticRecordSlot& slot) const noexcept {
    const auto present = [](const PeSemanticArenaRange& range) noexcept {
      return range.count != 0u;
    };
    return present(slot.renderStates) + present(slot.textures) +
           present(slot.streams) + present(slot.shaders) +
           present(slot.vertexInputs) + present(slot.indexBuffers) +
           present(slot.renderTargets) + present(slot.depthStencils) +
           present(slot.viewports) + present(slot.scissors) +
           present(slot.materials) + present(slot.clipPlanes) +
           present(slot.textureStageStates) + present(slot.samplerStates) +
           present(slot.transforms) + present(slot.lights) +
           present(slot.lightEnables) + present(slot.vsFloatConstants) +
           present(slot.vsIntConstants) + present(slot.vsBoolConstants) +
           present(slot.psFloatConstants) + present(slot.psIntConstants) +
           present(slot.psBoolConstants) + present(slot.upIndexBytes) +
           present(slot.upVertexBytes);
  }

  bool payloadSize(const PeSemanticRecordSlot& slot,
                   std::size_t& out) const noexcept {
    out = 0u;
    switch (slot.producer) {
      case PeSemanticProducerKind::DrawPrimitive:
      case PeSemanticProducerKind::DrawIndexedPrimitive:
      case PeSemanticProducerKind::DrawPrimitiveUp:
      case PeSemanticProducerKind::DrawIndexedPrimitiveUp:
      case PeSemanticProducerKind::ApplyState: {
        const auto sections = sparseSectionCount(slot);
        if (sections > D9C_COMMAND_CHUNK_SECTION_COUNT ||
            sizeof(D9CCommandChunkWireDrawHeader) >
                std::numeric_limits<std::size_t>::max() -
                    sections * sizeof(D9CCommandChunkWireSectionDesc)) return false;
        std::size_t cursor = sizeof(D9CCommandChunkWireDrawHeader) +
                             sections * sizeof(D9CCommandChunkWireSectionDesc);
        const auto addTyped = [&](std::uint16_t kind,
                                  const PeSemanticArenaRange& range,
                                  std::size_t element) noexcept {
          return range.count == 0u ||
                 (range.offset <= MaxSparseValues &&
                  range.count <= MaxSparseValues - range.offset &&
                  addEmissionSection(kind, range.count, range.count * element,
                                     cursor));
        };
        const auto addRawBytes = [&](std::uint16_t kind,
                                     const PeSemanticArenaRange& range) noexcept {
          return range.count == 0u ||
                 (range.offset <= MaxSemanticBytes &&
                  range.count <= MaxSemanticBytes - range.offset &&
                  addEmissionSection(kind, range.count, range.count,
                                     cursor));
        };
        if (!addTyped(D9C_COMMAND_CHUNK_SECTION_RENDER_STATE, slot.renderStates,
                      sizeof(D9CCommandChunkWireRenderState)) ||
            !addTyped(D9C_COMMAND_CHUNK_SECTION_TEXTURE, slot.textures,
                      sizeof(D9CCommandChunkWireTextureBinding)) ||
            !addTyped(D9C_COMMAND_CHUNK_SECTION_STREAM, slot.streams,
                      sizeof(D9CCommandChunkWireStreamBinding)) ||
            !addTyped(D9C_COMMAND_CHUNK_SECTION_SHADER, slot.shaders,
                      sizeof(D9CCommandChunkWireShaderBinding)) ||
            !addTyped(D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT, slot.vertexInputs,
                      sizeof(D9CCommandChunkWireVertexInput)) ||
            !addTyped(D9C_COMMAND_CHUNK_SECTION_INDEX_BUFFER, slot.indexBuffers,
                      sizeof(D9CCommandChunkWireIndexBinding)) ||
            !addTyped(D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET, slot.renderTargets,
                      sizeof(D9CCommandChunkWireRenderTargetBinding)) ||
            !addTyped(D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL, slot.depthStencils,
                      sizeof(D9CCommandChunkWireDepthStencilBinding)) ||
            !addTyped(D9C_COMMAND_CHUNK_SECTION_VIEWPORT, slot.viewports,
                      sizeof(D9CViewport)) ||
            !addTyped(D9C_COMMAND_CHUNK_SECTION_SCISSOR, slot.scissors,
                      sizeof(D9CRect)) ||
            !addTyped(D9C_COMMAND_CHUNK_SECTION_MATERIAL, slot.materials,
                      sizeof(D9CMaterial)) ||
            !addTyped(D9C_COMMAND_CHUNK_SECTION_CLIP_PLANE, slot.clipPlanes,
                      sizeof(D9CCommandChunkWireClipPlane)) ||
            !addTyped(D9C_COMMAND_CHUNK_SECTION_TEXTURE_STAGE_STATE,
                      slot.textureStageStates,
                      sizeof(D9CDrawPacketTextureStageState)) ||
            !addTyped(D9C_COMMAND_CHUNK_SECTION_SAMPLER_STATE,
                      slot.samplerStates, sizeof(D9CDrawPacketSamplerState)) ||
            !addTyped(D9C_COMMAND_CHUNK_SECTION_TRANSFORM, slot.transforms,
                      sizeof(D9CDrawPacketTransform)) ||
            !addTyped(D9C_COMMAND_CHUNK_SECTION_LIGHT, slot.lights,
                      sizeof(D9CCommandChunkWireLight)) ||
            !addTyped(D9C_COMMAND_CHUNK_SECTION_LIGHT_ENABLE,
                      slot.lightEnables, sizeof(D9CCommandChunkWireLightEnable)) ||
            !addEmissionConstant(D9C_COMMAND_CHUNK_SECTION_VS_CONST_F,
                                  slot.vsFloatConstants, slot.vsFloatConstant, cursor) ||
            !addEmissionConstant(D9C_COMMAND_CHUNK_SECTION_VS_CONST_I,
                                  slot.vsIntConstants, slot.vsIntConstant, cursor) ||
            !addEmissionConstant(D9C_COMMAND_CHUNK_SECTION_VS_CONST_B,
                                  slot.vsBoolConstants, slot.vsBoolConstant, cursor) ||
            !addEmissionConstant(D9C_COMMAND_CHUNK_SECTION_PS_CONST_F,
                                  slot.psFloatConstants, slot.psFloatConstant, cursor) ||
            !addEmissionConstant(D9C_COMMAND_CHUNK_SECTION_PS_CONST_I,
                                  slot.psIntConstants, slot.psIntConstant, cursor) ||
            !addEmissionConstant(D9C_COMMAND_CHUNK_SECTION_PS_CONST_B,
                                  slot.psBoolConstants, slot.psBoolConstant, cursor) ||
            !addRawBytes(D9C_COMMAND_CHUNK_SECTION_UP_INDEX_DATA,
                         slot.upIndexBytes) ||
            !addRawBytes(D9C_COMMAND_CHUNK_SECTION_UP_VERTEX_DATA,
                         slot.upVertexBytes)) return false;
        out = cursor;
        return true;
      }
      case PeSemanticProducerKind::VsFloatConstant:
      case PeSemanticProducerKind::VsIntConstant:
      case PeSemanticProducerKind::VsBoolConstant:
      case PeSemanticProducerKind::PsFloatConstant:
      case PeSemanticProducerKind::PsIntConstant:
      case PeSemanticProducerKind::PsBoolConstant:
        return slot.constantBytes.offset <= MaxSemanticBytes &&
               slot.constantBytes.count <= MaxSemanticBytes - slot.constantBytes.offset &&
               slot.constantBytes.count <= std::numeric_limits<std::size_t>::max() -
                   sizeof(D9CCommandChunkWireSetConst) &&
               (out = sizeof(D9CCommandChunkWireSetConst) + slot.constantBytes.count,
                true);
      case PeSemanticProducerKind::Clear:
        return slot.clearRects.count <= MaxRects - slot.clearRects.offset &&
               slot.clearRects.count <=
                   (std::numeric_limits<std::size_t>::max() -
                    sizeof(D9CCommandChunkWireClear)) / sizeof(D9CRect) &&
               (out = sizeof(D9CCommandChunkWireClear) +
                      slot.clearRects.count * sizeof(D9CRect), true);
      case PeSemanticProducerKind::Present: out = sizeof(slot.present); return true;
      case PeSemanticProducerKind::StretchRect: out = sizeof(slot.stretchRect); return true;
      case PeSemanticProducerKind::ColorFill: out = sizeof(slot.colorFill); return true;
      case PeSemanticProducerKind::UpdateTexture: out = sizeof(D9CCommandChunkWireUpdateTexture); return true;
      case PeSemanticProducerKind::UpdateSurface: out = sizeof(slot.updateSurface); return true;
      case PeSemanticProducerKind::QueryIssue: out = sizeof(slot.queryIssue); return true;
      case PeSemanticProducerKind::Readback: out = sizeof(D9CCommandChunkWireReadback); return true;
      case PeSemanticProducerKind::ReszDepthResolve: out = sizeof(D9CCommandChunkWireReszDepthResolve); return true;
      case PeSemanticProducerKind::GenerateMipmaps: out = sizeof(D9CCommandChunkWireGenerateMipmaps); return true;
      case PeSemanticProducerKind::Count: return false;
    }
    return false;
  }

  bool buildEmissionPlan(EmissionPlan& out) const noexcept {
    out = {};
    if (!ready_ || recordCount_ == 0u || recordCount_ > MaxRecords) return false;
    std::size_t payloadEnd = 0u;
    for (std::size_t i = 0u; i < recordCount_; ++i) {
      const auto& slot = storage_->records[i];
      const auto* rule = recordRule(slot.recordType);
      if (!rule) return false;
      EmissionHandleContext handles{};
      if (!visitRecordHandles(slot, [&](const D9CWireObjectIdentity& id) noexcept {
            return handles.add(id);
          })) return false;
      std::size_t bytes = 0u;
      std::size_t aligned = 0u;
      if (!payloadSize(slot, bytes) ||
          !alignEmission(payloadEnd, rule->payloadAlignment, aligned) ||
          bytes > std::numeric_limits<std::size_t>::max() - aligned ||
          out.handleCount > std::numeric_limits<std::size_t>::max() - handles.count) {
        return false;
      }
      out.records[i] = {.payloadOffset = aligned,
                        .payloadBytes = bytes,
                        .firstHandle = out.handleCount,
                        .handleCount = handles.count};
      payloadEnd = aligned + bytes;
      out.handleCount += handles.count;
    }
    out.recordCount = recordCount_;
    out.payloadBytes = payloadEnd;
    if (out.handleCount != emissionHandleCount_ ||
        out.payloadBytes != emissionPayloadBytes_) {
      return false;
    }
    if (out.recordCount > std::numeric_limits<std::uint32_t>::max() ||
        out.handleCount > std::numeric_limits<std::uint32_t>::max() ||
        out.payloadBytes > std::numeric_limits<std::uint32_t>::max()) return false;
    const auto layout = planExactCommandChunkLayout(
        static_cast<std::uint32_t>(out.recordCount),
        static_cast<std::uint32_t>(out.handleCount),
        static_cast<std::uint32_t>(out.payloadBytes));
    if (!layout.valid()) return false;
    out.header = {
        .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
        .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
        .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE,
        .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE,
        .recordTableOffset = layout.recordTableOffset,
        .recordCount = layout.recordCount,
        .handleTableOffset = layout.handleTableOffset,
        .handleCount = layout.handleCount,
        .payloadArenaOffset = layout.payloadArenaOffset,
        .payloadArenaSize = layout.payloadBytes,
        .reserved0 = 0u,
        .reserved1 = 0u,
    };
    if (layout.totalBytes > D9C_COMMAND_CHUNK_MAX_TOTAL_WIRE_BYTES) return false;
    out.wireBytes = layout.totalBytes;
    return true;
  }

  template <typename T>
  static bool writeValue(std::span<std::byte> destination, std::size_t& cursor,
                         const T& value) noexcept {
    if (cursor > destination.size() || sizeof(T) > destination.size() - cursor)
      return false;
    std::memcpy(destination.data() + cursor, &value, sizeof(T));
    cursor += sizeof(T);
    return true;
  }

  template <typename T>
  static bool writeTypedSection(
      std::uint16_t kind, std::span<const T> values,
      std::span<std::byte> payload, std::size_t& cursor,
      std::array<D9CCommandChunkWireSectionDesc,
                 D9C_COMMAND_CHUNK_SECTION_COUNT>& descs,
      std::size_t& sectionIndex) noexcept {
    if (values.empty()) return true;
    const auto* rule = sectionRule(kind);
    std::size_t aligned = 0u;
    if (!rule || values.size() > rule->maxCount ||
        sizeof(T) != rule->elementSize ||
        !alignEmission(cursor, rule->payloadAlignment, aligned) ||
        aligned > payload.size() || values.size_bytes() > payload.size() - aligned ||
        sectionIndex == descs.size()) return false;
    cursor = aligned;
    std::memcpy(payload.data() + cursor, values.data(), values.size_bytes());
    descs[sectionIndex++] = {
        .kind = kind,
        .elementSize = rule->elementSize,
        .count = static_cast<std::uint32_t>(values.size()),
        .payloadOffset = static_cast<std::uint32_t>(cursor),
        .byteSize = static_cast<std::uint32_t>(values.size_bytes()),
    };
    cursor += values.size_bytes();
    return true;
  }

  template <typename Wire, typename Identity>
  bool writeBindingSection(
      std::uint16_t kind, std::span<const PeSemanticOwnedBinding<Wire, Identity>> values,
      std::span<std::byte> payload, std::size_t& cursor,
      std::array<D9CCommandChunkWireSectionDesc,
                 D9C_COMMAND_CHUNK_SECTION_COUNT>& descs,
      std::size_t& sectionIndex, const EmissionHandleContext& handles,
      std::size_t globalFirst) const noexcept {
    if (values.empty()) return true;
    const auto* rule = sectionRule(kind);
    std::size_t aligned = 0u;
    const auto wireBytes = values.size() * sizeof(Wire);
    if (!rule || values.size() > rule->maxCount ||
        sizeof(Wire) != rule->elementSize ||
        !alignEmission(cursor, rule->payloadAlignment, aligned) ||
        aligned > payload.size() || wireBytes > payload.size() - aligned ||
        sectionIndex == descs.size()) {
      return false;
    }
    cursor = aligned;
    for (const auto& source : values) {
      Wire value = source.wire;
      if constexpr (requires(Wire wire) { wire.reserved0; }) {
        value.reserved0 = 0u;
      }
      value.handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
      if (value.valid && source.hasPin) {
        D9CWireObjectIdentity identity{};
        if (!bindingIdentity(source, identity)) {
          return false;
        }
        std::size_t local = 0u;
        if (!handles.indexOf(identity, local) ||
            globalFirst + local > std::numeric_limits<std::uint32_t>::max()) {
          return false;
        }
        value.handleIndex = static_cast<std::uint32_t>(globalFirst + local);
      }
      if (!writeValue(payload, cursor, value)) return false;
    }
    descs[sectionIndex++] = {
        .kind = kind,
        .elementSize = rule->elementSize,
        .count = static_cast<std::uint32_t>(values.size()),
        .payloadOffset = static_cast<std::uint32_t>(aligned),
        .byteSize = static_cast<std::uint32_t>(wireBytes),
    };
    return true;
  }

  template <typename Wire, typename Identity>
  bool bindingIdentity(const PeSemanticOwnedBinding<Wire, Identity>& source,
                       D9CWireObjectIdentity& out) const noexcept {
    if constexpr (std::is_same_v<Identity, PeSemanticTextureIdentity>) {
      return source.pin < textureCount_ && source.pin != kPeSemanticNoSlot &&
             storage_->textures[source.pin].valid() &&
             (out = storage_->textures[source.pin].identity.value, true);
    } else if constexpr (std::is_same_v<Identity, PeSemanticBufferIdentity>) {
      return source.pin < bufferCount_ && source.pin != kPeSemanticNoSlot &&
             storage_->buffers[source.pin].valid() &&
             (out = storage_->buffers[source.pin].identity.value, true);
    } else if constexpr (std::is_same_v<Identity, PeSemanticShaderIdentity>) {
      return source.pin < shaderCount_ && source.pin != kPeSemanticNoSlot &&
             storage_->shaders[source.pin].valid() &&
             (out = storage_->shaders[source.pin].identity.value, true);
    } else if constexpr (std::is_same_v<Identity, PeSemanticDeclarationIdentity>) {
      return source.pin < declarationCount_ && source.pin != kPeSemanticNoSlot &&
             storage_->declarations[source.pin].valid() &&
             (out = storage_->declarations[source.pin].identity.value, true);
    } else if constexpr (std::is_same_v<Identity, PeSemanticSurfaceIdentity>) {
      return source.pin < surfaceCount_ && source.pin != kPeSemanticNoSlot &&
             storage_->surfaces[source.pin].valid() &&
             (out = storage_->surfaces[source.pin].identity.value, true);
    }
    return false;
  }

  static bool writeBytesSection(
      std::uint16_t kind, std::span<const std::byte> bytes,
      std::span<std::byte> payload, std::size_t& cursor,
      std::array<D9CCommandChunkWireSectionDesc,
                 D9C_COMMAND_CHUNK_SECTION_COUNT>& descs,
      std::size_t& sectionIndex) noexcept {
    if (bytes.empty()) return true;
    const auto* rule = sectionRule(kind);
    std::size_t aligned = 0u;
    if (!rule || bytes.size() > rule->maxCount ||
        !alignEmission(cursor, rule->payloadAlignment, aligned) ||
        aligned > payload.size() || bytes.size() > payload.size() - aligned ||
        sectionIndex == descs.size()) return false;
    cursor = aligned;
    std::memcpy(payload.data() + cursor, bytes.data(), bytes.size());
    descs[sectionIndex++] = {
        .kind = kind,
        .elementSize = rule->elementSize,
        .count = static_cast<std::uint32_t>(bytes.size()),
        .payloadOffset = static_cast<std::uint32_t>(cursor),
        .byteSize = static_cast<std::uint32_t>(bytes.size()),
    };
    cursor += bytes.size();
    return true;
  }

  bool directIdentity(std::uint32_t kind, std::uint32_t pin,
                      D9CWireObjectIdentity& out) const noexcept {
    switch (kind) {
      case D9C_CHUNK_HANDLE_KIND_SURFACE:
        return visitPin(storage_->surfaces, surfaceCount_, pin,
                        [&](const auto& value) noexcept { out = value; return true; });
      case D9C_CHUNK_HANDLE_KIND_TEXTURE:
        return visitPin(storage_->textures, textureCount_, pin,
                        [&](const auto& value) noexcept { out = value; return true; });
      case D9C_CHUNK_HANDLE_KIND_BUFFER:
        return visitPin(storage_->buffers, bufferCount_, pin,
                        [&](const auto& value) noexcept { out = value; return true; });
      case D9C_CHUNK_HANDLE_KIND_SHADER:
        return visitPin(storage_->shaders, shaderCount_, pin,
                        [&](const auto& value) noexcept { out = value; return true; });
      case D9C_CHUNK_HANDLE_KIND_VERTEX_DECL:
        return visitPin(storage_->declarations, declarationCount_, pin,
                        [&](const auto& value) noexcept { out = value; return true; });
      case D9C_CHUNK_HANDLE_KIND_QUERY:
        return visitPin(storage_->queries, queryCount_, pin,
                        [&](const auto& value) noexcept { out = value; return true; });
      default:
        return false;
    }
  }

  bool directHandle(std::uint32_t kind, std::uint32_t pin,
                    const EmissionHandleContext& handles,
                    std::size_t globalFirst, std::uint32_t& out) const noexcept {
    D9CWireObjectIdentity identity{};
    std::size_t local = 0u;
    if (!directIdentity(kind, pin, identity) ||
        !handles.indexOf(identity, local) ||
        globalFirst + local > std::numeric_limits<std::uint32_t>::max()) {
      return false;
    }
    out = static_cast<std::uint32_t>(globalFirst + local);
    return true;
  }

  bool writeConstantSection(
      std::uint16_t kind, const PeSemanticArenaRange& bytes,
      const D9CCommandChunkWireConstantRange& range,
      std::span<std::byte> payload, std::size_t& cursor,
      std::array<D9CCommandChunkWireSectionDesc,
                 D9C_COMMAND_CHUNK_SECTION_COUNT>& descs,
      std::size_t& sectionIndex) const noexcept {
    if (range.registerCount == 0u) return bytes.count == 0u;
    const auto* rule = sectionRule(kind);
    std::size_t aligned = 0u;
    if (!rule || !alignEmission(cursor, rule->payloadAlignment, aligned) ||
        aligned > payload.size() ||
        sizeof(range) + bytes.count > payload.size() - aligned ||
        sectionIndex == descs.size() || bytes.offset > MaxSemanticBytes ||
        bytes.count > MaxSemanticBytes - bytes.offset) return false;
    cursor = aligned;
    if (!writeValue(payload, cursor, range)) return false;
    const auto source = std::span<const std::byte>(
        storage_->constantBytes.data() + bytes.offset, bytes.count);
    if (cursor > payload.size() || source.size() > payload.size() - cursor) return false;
    std::memcpy(payload.data() + cursor, source.data(), source.size());
    cursor += source.size();
    descs[sectionIndex++] = {
        .kind = kind,
        .elementSize = rule->elementSize,
        .count = range.registerCount,
        .payloadOffset = static_cast<std::uint32_t>(aligned),
        .byteSize = static_cast<std::uint32_t>(sizeof(range) + bytes.count),
    };
    return true;
  }

  bool writeSparsePayload(const PeSemanticRecordSlot& slot,
                          std::span<std::byte> payload,
                          std::size_t globalFirst,
                          const EmissionHandleContext& handles) const noexcept {
    const auto count = sparseSectionCount(slot);
    if (count > D9C_COMMAND_CHUNK_SECTION_COUNT) return false;
    D9CCommandChunkWireDrawHeader draw = slot.draw;
    draw.sectionCount = static_cast<std::uint32_t>(count);
    draw.sectionTableOffset = sizeof(draw);
    draw.sectionPayloadOffset = static_cast<std::uint32_t>(
        sizeof(draw) + count * sizeof(D9CCommandChunkWireSectionDesc));
    draw.reserved0 = 0u;
    std::size_t cursor = 0u;
    if (!writeValue(payload, cursor, draw)) return false;
    std::array<D9CCommandChunkWireSectionDesc,
               D9C_COMMAND_CHUNK_SECTION_COUNT>
        descs{};
    const auto tableBytes = count * sizeof(D9CCommandChunkWireSectionDesc);
    if (cursor > payload.size() || tableBytes > payload.size() - cursor) return false;
    cursor += tableBytes;
    std::size_t sectionIndex = 0u;
    const auto textures = std::span<const PeSemanticOwnedBinding<
        D9CCommandChunkWireTextureBinding, PeSemanticTextureIdentity>>(
        storage_->texturesArena.data() + slot.textures.offset, slot.textures.count);
    const auto streams = std::span<const PeSemanticOwnedBinding<
        D9CCommandChunkWireStreamBinding, PeSemanticBufferIdentity>>(
        storage_->streamsArena.data() + slot.streams.offset, slot.streams.count);
    const auto shaders = std::span<const PeSemanticOwnedBinding<
        D9CCommandChunkWireShaderBinding, PeSemanticShaderIdentity>>(
        storage_->shadersArena.data() + slot.shaders.offset, slot.shaders.count);
    const auto vertexInputs = std::span<const PeSemanticOwnedBinding<
        D9CCommandChunkWireVertexInput, PeSemanticDeclarationIdentity>>(
        storage_->vertexInputsArena.data() + slot.vertexInputs.offset, slot.vertexInputs.count);
    const auto indexBuffers = std::span<const PeSemanticOwnedBinding<
        D9CCommandChunkWireIndexBinding, PeSemanticBufferIdentity>>(
        storage_->indexBuffersArena.data() + slot.indexBuffers.offset, slot.indexBuffers.count);
    const auto renderTargets = std::span<const PeSemanticOwnedBinding<
        D9CCommandChunkWireRenderTargetBinding, PeSemanticSurfaceIdentity>>(
        storage_->renderTargetsArena.data() + slot.renderTargets.offset, slot.renderTargets.count);
    const auto depthStencils = std::span<const PeSemanticOwnedBinding<
        D9CCommandChunkWireDepthStencilBinding, PeSemanticSurfaceIdentity>>(
        storage_->depthStencilsArena.data() + slot.depthStencils.offset, slot.depthStencils.count);
    if (!writeTypedSection(D9C_COMMAND_CHUNK_SECTION_RENDER_STATE,
                           std::span<const D9CCommandChunkWireRenderState>(
                               storage_->renderStates.data() + slot.renderStates.offset,
                               slot.renderStates.count), payload, cursor, descs, sectionIndex) ||
        !writeBindingSection(D9C_COMMAND_CHUNK_SECTION_TEXTURE, textures, payload, cursor,
                             descs, sectionIndex, handles, globalFirst) ||
        !writeBindingSection(D9C_COMMAND_CHUNK_SECTION_STREAM, streams, payload, cursor,
                             descs, sectionIndex, handles, globalFirst) ||
        !writeBindingSection(D9C_COMMAND_CHUNK_SECTION_SHADER, shaders, payload, cursor,
                             descs, sectionIndex, handles, globalFirst) ||
        !writeBindingSection(D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT, vertexInputs, payload,
                             cursor, descs, sectionIndex, handles, globalFirst) ||
        !writeBindingSection(D9C_COMMAND_CHUNK_SECTION_INDEX_BUFFER, indexBuffers, payload,
                             cursor, descs, sectionIndex, handles, globalFirst) ||
        !writeBindingSection(D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET, renderTargets, payload,
                             cursor, descs, sectionIndex, handles, globalFirst) ||
        !writeBindingSection(D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL, depthStencils, payload,
                             cursor, descs, sectionIndex, handles, globalFirst) ||
        !writeTypedSection(D9C_COMMAND_CHUNK_SECTION_VIEWPORT,
                           std::span<const D9CViewport>(
                               storage_->viewports.data() + slot.viewports.offset,
                               slot.viewports.count), payload, cursor, descs, sectionIndex) ||
        !writeTypedSection(D9C_COMMAND_CHUNK_SECTION_SCISSOR,
                           std::span<const D9CRect>(storage_->scissors.data() + slot.scissors.offset,
                                                    slot.scissors.count), payload, cursor, descs, sectionIndex) ||
        !writeTypedSection(D9C_COMMAND_CHUNK_SECTION_MATERIAL,
                           std::span<const D9CMaterial>(storage_->materials.data() + slot.materials.offset,
                                                        slot.materials.count), payload, cursor, descs, sectionIndex) ||
        !writeTypedSection(D9C_COMMAND_CHUNK_SECTION_CLIP_PLANE,
                           std::span<const D9CCommandChunkWireClipPlane>(
                               storage_->clipPlanes.data() + slot.clipPlanes.offset,
                               slot.clipPlanes.count), payload, cursor, descs, sectionIndex) ||
        !writeTypedSection(D9C_COMMAND_CHUNK_SECTION_TEXTURE_STAGE_STATE,
                           std::span<const D9CDrawPacketTextureStageState>(
                               storage_->textureStageStates.data() + slot.textureStageStates.offset,
                               slot.textureStageStates.count), payload, cursor, descs, sectionIndex) ||
        !writeTypedSection(D9C_COMMAND_CHUNK_SECTION_SAMPLER_STATE,
                           std::span<const D9CDrawPacketSamplerState>(
                               storage_->samplerStates.data() + slot.samplerStates.offset,
                               slot.samplerStates.count), payload, cursor, descs, sectionIndex) ||
        !writeTypedSection(D9C_COMMAND_CHUNK_SECTION_TRANSFORM,
                           std::span<const D9CDrawPacketTransform>(
                               storage_->transforms.data() + slot.transforms.offset,
                               slot.transforms.count), payload, cursor, descs, sectionIndex) ||
        !writeTypedSection(D9C_COMMAND_CHUNK_SECTION_LIGHT,
                           std::span<const D9CCommandChunkWireLight>(
                               storage_->lights.data() + slot.lights.offset,
                               slot.lights.count), payload, cursor, descs, sectionIndex) ||
        !writeTypedSection(D9C_COMMAND_CHUNK_SECTION_LIGHT_ENABLE,
                           std::span<const D9CCommandChunkWireLightEnable>(
                               storage_->lightEnables.data() + slot.lightEnables.offset,
                               slot.lightEnables.count), payload, cursor, descs, sectionIndex) ||
        !writeConstantSection(D9C_COMMAND_CHUNK_SECTION_VS_CONST_F, slot.vsFloatConstants,
                              slot.vsFloatConstant, payload, cursor, descs, sectionIndex) ||
        !writeConstantSection(D9C_COMMAND_CHUNK_SECTION_VS_CONST_I, slot.vsIntConstants,
                              slot.vsIntConstant, payload, cursor, descs, sectionIndex) ||
        !writeConstantSection(D9C_COMMAND_CHUNK_SECTION_VS_CONST_B, slot.vsBoolConstants,
                              slot.vsBoolConstant, payload, cursor, descs, sectionIndex) ||
        !writeConstantSection(D9C_COMMAND_CHUNK_SECTION_PS_CONST_F, slot.psFloatConstants,
                              slot.psFloatConstant, payload, cursor, descs, sectionIndex) ||
        !writeConstantSection(D9C_COMMAND_CHUNK_SECTION_PS_CONST_I, slot.psIntConstants,
                              slot.psIntConstant, payload, cursor, descs, sectionIndex) ||
        !writeConstantSection(D9C_COMMAND_CHUNK_SECTION_PS_CONST_B, slot.psBoolConstants,
                              slot.psBoolConstant, payload, cursor, descs, sectionIndex) ||
        !writeBytesSection(D9C_COMMAND_CHUNK_SECTION_UP_INDEX_DATA,
                           std::span<const std::byte>(storage_->constantBytes.data() + slot.upIndexBytes.offset,
                                                       slot.upIndexBytes.count), payload, cursor, descs, sectionIndex) ||
        !writeBytesSection(D9C_COMMAND_CHUNK_SECTION_UP_VERTEX_DATA,
                           std::span<const std::byte>(storage_->constantBytes.data() + slot.upVertexBytes.offset,
                                                       slot.upVertexBytes.count), payload, cursor, descs, sectionIndex)) return false;
    if (sectionIndex != count || cursor != payload.size()) return false;
    std::memcpy(payload.data() + draw.sectionTableOffset, descs.data(), tableBytes);
    return true;
  }

  bool writeRecordPayload(const PeSemanticRecordSlot& slot,
                          std::span<std::byte> payload,
                          std::size_t globalFirst,
                          const EmissionHandleContext& handles) const noexcept {
    std::size_t cursor = 0u;
    switch (slot.producer) {
      case PeSemanticProducerKind::DrawPrimitive:
      case PeSemanticProducerKind::DrawIndexedPrimitive:
      case PeSemanticProducerKind::DrawPrimitiveUp:
      case PeSemanticProducerKind::DrawIndexedPrimitiveUp:
      case PeSemanticProducerKind::ApplyState:
        return writeSparsePayload(slot, payload, globalFirst, handles);
      case PeSemanticProducerKind::VsFloatConstant:
      case PeSemanticProducerKind::VsIntConstant:
      case PeSemanticProducerKind::VsBoolConstant:
      case PeSemanticProducerKind::PsFloatConstant:
      case PeSemanticProducerKind::PsIntConstant:
      case PeSemanticProducerKind::PsBoolConstant:
        return writeValue(payload, cursor, slot.setConst) &&
               slot.constantBytes.offset <= MaxSemanticBytes &&
               slot.constantBytes.count <= MaxSemanticBytes - slot.constantBytes.offset &&
               cursor + slot.constantBytes.count <= payload.size() &&
               (std::memcpy(payload.data() + cursor,
                            storage_->constantBytes.data() + slot.constantBytes.offset,
                            slot.constantBytes.count),
                cursor += slot.constantBytes.count, cursor == payload.size());
      case PeSemanticProducerKind::Clear: {
        auto fixed = slot.clear;
        fixed.rectCount = slot.clearRects.count;
        fixed.rectOffset = sizeof(fixed);
        if (!writeValue(payload, cursor, fixed) ||
            slot.clearRects.offset > MaxRects ||
            slot.clearRects.count > MaxRects - slot.clearRects.offset ||
            slot.clearRects.count * sizeof(D9CRect) > payload.size() - cursor) return false;
        std::memcpy(payload.data() + cursor,
                    storage_->rects.data() + slot.clearRects.offset,
                    slot.clearRects.count * sizeof(D9CRect));
        cursor += slot.clearRects.count * sizeof(D9CRect);
        return cursor == payload.size();
      }
      case PeSemanticProducerKind::Present: {
        auto fixed = slot.present;
        fixed.sourceHandleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
        if (!directHandle(D9C_CHUNK_HANDLE_KIND_SURFACE, slot.surface0, handles,
                          globalFirst, fixed.sourceHandleIndex) ||
            !writeValue(payload, cursor, fixed)) return false;
        return cursor == payload.size();
      }
      case PeSemanticProducerKind::StretchRect: {
        auto fixed = slot.stretchRect;
        fixed.reserved0 = 0u;
        if (!directHandle(D9C_CHUNK_HANDLE_KIND_SURFACE, slot.surface0, handles,
                          globalFirst, fixed.srcHandleIndex) ||
            !directHandle(D9C_CHUNK_HANDLE_KIND_SURFACE, slot.surface1, handles,
                          globalFirst, fixed.dstHandleIndex) ||
            !writeValue(payload, cursor, fixed)) return false;
        return cursor == payload.size();
      }
      case PeSemanticProducerKind::ColorFill: {
        auto fixed = slot.colorFill;
        fixed.reserved0 = 0u;
        if (!directHandle(D9C_CHUNK_HANDLE_KIND_SURFACE, slot.surface0, handles,
                          globalFirst, fixed.surfaceHandleIndex) ||
            !writeValue(payload, cursor, fixed)) return false;
        return cursor == payload.size();
      }
      case PeSemanticProducerKind::UpdateTexture: {
        D9CCommandChunkWireUpdateTexture fixed{};
        if (!directHandle(D9C_CHUNK_HANDLE_KIND_TEXTURE, slot.texture0, handles,
                          globalFirst, fixed.srcHandleIndex) ||
            !directHandle(D9C_CHUNK_HANDLE_KIND_TEXTURE, slot.texture1, handles,
                          globalFirst, fixed.dstHandleIndex) ||
            !writeValue(payload, cursor, fixed)) return false;
        return cursor == payload.size();
      }
      case PeSemanticProducerKind::UpdateSurface: {
        auto fixed = slot.updateSurface;
        if (!directHandle(D9C_CHUNK_HANDLE_KIND_SURFACE, slot.surface0, handles,
                          globalFirst, fixed.srcHandleIndex) ||
            !directHandle(D9C_CHUNK_HANDLE_KIND_SURFACE, slot.surface1, handles,
                          globalFirst, fixed.dstHandleIndex) ||
            !writeValue(payload, cursor, fixed)) return false;
        return cursor == payload.size();
      }
      case PeSemanticProducerKind::QueryIssue: {
        auto fixed = slot.queryIssue;
        if (!directHandle(D9C_CHUNK_HANDLE_KIND_QUERY, slot.query, handles,
                          globalFirst, fixed.queryHandleIndex) ||
            !writeValue(payload, cursor, fixed)) return false;
        return cursor == payload.size();
      }
      case PeSemanticProducerKind::Readback: {
        D9CCommandChunkWireReadback fixed{};
        if (!directHandle(D9C_CHUNK_HANDLE_KIND_SURFACE, slot.surface0, handles,
                          globalFirst, fixed.srcHandleIndex) ||
            !directHandle(D9C_CHUNK_HANDLE_KIND_SURFACE, slot.surface1, handles,
                          globalFirst, fixed.dstHandleIndex) ||
            !writeValue(payload, cursor, fixed)) return false;
        return cursor == payload.size();
      }
      case PeSemanticProducerKind::ReszDepthResolve: {
        D9CCommandChunkWireReszDepthResolve fixed{};
        if (!directHandle(D9C_CHUNK_HANDLE_KIND_SURFACE, slot.surface0, handles,
                          globalFirst, fixed.msaaDepthHandleIndex) ||
            !directHandle(D9C_CHUNK_HANDLE_KIND_TEXTURE, slot.texture0, handles,
                          globalFirst, fixed.intzDestHandleIndex) ||
            !writeValue(payload, cursor, fixed)) return false;
        return cursor == payload.size();
      }
      case PeSemanticProducerKind::GenerateMipmaps: {
        D9CCommandChunkWireGenerateMipmaps fixed{};
        if (!directHandle(D9C_CHUNK_HANDLE_KIND_TEXTURE, slot.texture0, handles,
                          globalFirst, fixed.textureHandleIndex) ||
            !writeValue(payload, cursor, fixed)) return false;
        return cursor == payload.size();
      }
      case PeSemanticProducerKind::Count:
        return false;
    }
    return false;
  }

  bool writeEmission(const EmissionPlan& plan,
                     std::span<std::byte> recordRegion,
                     std::span<std::byte> handleRegion,
                     std::span<std::byte> payloadRegion) const noexcept {
    if (recordRegion.size() < plan.recordBytes() ||
        handleRegion.size() < plan.handleBytes() ||
        payloadRegion.size() < plan.payloadBytes) return false;
    std::fill(payloadRegion.begin(), payloadRegion.begin() + plan.payloadBytes,
              std::byte{0});
    for (std::size_t i = 0u; i < plan.recordCount; ++i) {
      const auto& slot = storage_->records[i];
      EmissionHandleContext handles{};
      if (!visitRecordHandles(slot, [&](const D9CWireObjectIdentity& id) noexcept {
            return handles.add(id);
          }) || handles.count != plan.records[i].handleCount) return false;
      for (std::size_t j = 0u; j < handles.count; ++j) {
        const auto wire = D9CCommandChunkWireHandleEntry{
            .kind = handles.identities[j].kind,
            .generation = handles.identities[j].generation,
            .objectId = handles.identities[j].objectId,
        };
        std::memcpy(handleRegion.data() +
                        (plan.records[i].firstHandle + j) * sizeof(wire),
                    &wire, sizeof(wire));
      }
      const auto wireRecord = D9CCommandChunkWireRecordHeader{
          .type = slot.recordType,
          .flags = slot.recordFlags,
          .payloadOffset = static_cast<std::uint32_t>(plan.records[i].payloadOffset),
          .payloadSize = static_cast<std::uint32_t>(plan.records[i].payloadBytes),
          .firstHandle = static_cast<std::uint32_t>(plan.records[i].firstHandle),
          .handleCount = static_cast<std::uint32_t>(plan.records[i].handleCount),
          .reserved0 = 0u,
          .reserved1 = 0u,
      };
      std::memcpy(recordRegion.data() + i * sizeof(wireRecord), &wireRecord,
                  sizeof(wireRecord));
      auto recordPayload = payloadRegion.subspan(plan.records[i].payloadOffset,
                                                 plan.records[i].payloadBytes);
      if (!writeRecordPayload(slot, recordPayload,
                              plan.records[i].firstHandle, handles)) {
        return false;
      }
    }
    return true;
  }

  static D9CCommandChunkSegmentedTransportV1 makeTransport(
      const EmissionPlan& plan, std::span<const std::byte> records,
      std::span<const std::byte> handles, std::span<const std::byte> payload,
      std::uint64_t captureToken, std::uint64_t eventOrdinal) noexcept {
    return {
        .header = plan.header,
        .records = toWireHandle(records.data()),
        .recordBytes = static_cast<std::uint32_t>(records.size()),
        .recordReserved = 0u,
        .handles = toWireHandle(handles.data()),
        .handleBytes = static_cast<std::uint32_t>(handles.size()),
        .handleReserved = 0u,
        .payload = toWireHandle(payload.data()),
        .payloadBytes = static_cast<std::uint32_t>(payload.size()),
        .payloadReserved = 0u,
        .renderTapeCaptureToken = captureToken,
        .renderTapeEventOrdinal = eventOrdinal,
    };
  }

  static constexpr std::size_t kRenderStates = 0u;
  static constexpr std::size_t kTextures = 1u;
  static constexpr std::size_t kStreams = 2u;
  static constexpr std::size_t kShaders = 3u;
  static constexpr std::size_t kVertexInputs = 4u;
  static constexpr std::size_t kIndexBuffers = 5u;
  static constexpr std::size_t kRenderTargets = 6u;
  static constexpr std::size_t kDepthStencils = 7u;
  static constexpr std::size_t kViewports = 8u;
  static constexpr std::size_t kScissors = 9u;
  static constexpr std::size_t kMaterials = 10u;
  static constexpr std::size_t kClipPlanes = 11u;
  static constexpr std::size_t kTextureStageStates = 12u;
  static constexpr std::size_t kSamplerStates = 13u;
  static constexpr std::size_t kTransforms = 14u;
  static constexpr std::size_t kLights = 15u;
  static constexpr std::size_t kLightEnables = 16u;

  StateCheckpoint checkpointState() const noexcept {
    return {.records = recordCount_,
            .semanticBytes = semanticBytes_,
            .emissionHandles = emissionHandleCount_,
            .emissionPayloadBytes = emissionPayloadBytes_,
            .rects = rectCount_,
            .sparse = sparseCounts_.values,
            .pins = {surfaceCount_, textureCount_, bufferCount_, shaderCount_,
                     declarationCount_, queryCount_},
            .retained = retainer_.beginAcquire()};
  }

  void rollback(const StateCheckpoint& checkpoint) noexcept {
    retainer_.rollback(checkpoint.retained);
    recordCount_ = checkpoint.records;
    semanticBytes_ = checkpoint.semanticBytes;
    emissionHandleCount_ = checkpoint.emissionHandles;
    emissionPayloadBytes_ = checkpoint.emissionPayloadBytes;
    rectCount_ = checkpoint.rects;
    sparseCounts_.values = checkpoint.sparse;
    clearPins(storage_->surfaces, checkpoint.pins[0], surfaceCount_);
    clearPins(storage_->textures, checkpoint.pins[1], textureCount_);
    clearPins(storage_->buffers, checkpoint.pins[2], bufferCount_);
    clearPins(storage_->shaders, checkpoint.pins[3], shaderCount_);
    clearPins(storage_->declarations, checkpoint.pins[4], declarationCount_);
    clearPins(storage_->queries, checkpoint.pins[5], queryCount_);
    // A failed admission after the owner is already full has no speculative
    // record slot to clear; indexing records[MaxRecords] would be OOB.
    if (recordCount_ < MaxRecords) {
      storage_->records[recordCount_] = {};
    }
    rebuildPinIndexes();
  }

  bool cacheEmissionMetrics(const PeSemanticRecordSlot& slot) noexcept {
    const auto* rule = recordRule(slot.recordType);
    if (!rule) return false;
    EmissionHandleContext handles{};
    if (!visitRecordHandles(slot, [&](const D9CWireObjectIdentity& identity) noexcept {
          return handles.add(identity);
        })) {
      return false;
    }
    std::size_t bytes = 0u;
    std::size_t aligned = 0u;
    if (!payloadSize(slot, bytes) ||
        !alignEmission(emissionPayloadBytes_, rule->payloadAlignment, aligned) ||
        bytes > std::numeric_limits<std::size_t>::max() - aligned ||
        emissionHandleCount_ > std::numeric_limits<std::size_t>::max() -
            handles.count) {
      return false;
    }
    const auto nextPayloadBytes = aligned + bytes;
    const auto nextHandleCount = emissionHandleCount_ + handles.count;
    if (recordCount_ + 1u > std::numeric_limits<std::uint32_t>::max() ||
        nextPayloadBytes > std::numeric_limits<std::uint32_t>::max() ||
        nextHandleCount > std::numeric_limits<std::uint32_t>::max()) {
      return false;
    }
    const auto layout = planExactCommandChunkLayout(
        static_cast<std::uint32_t>(recordCount_ + 1u),
        static_cast<std::uint32_t>(nextHandleCount),
        static_cast<std::uint32_t>(nextPayloadBytes));
    if (!layout.valid() || layout.totalBytes > D9C_COMMAND_CHUNK_MAX_TOTAL_WIRE_BYTES)
      return false;
    emissionPayloadBytes_ = nextPayloadBytes;
    emissionHandleCount_ = nextHandleCount;
    return true;
  }

  template <typename PinArray>
  static void clearPins(PinArray& pins, std::size_t from,
                        std::size_t& count) noexcept {
    for (std::size_t i = from; i < count; ++i) pins[i] = {};
    count = from;
  }

  static bool kindMatches(const PeSemanticRecordInput& input) noexcept {
    const auto* policy = peSemanticProducerPolicy(input.recordType);
    return policy && policy->kind == input.producer;
  }

  bool validAdmissionHeader(const PeSemanticRecordInput& input) const noexcept {
    if (recordCount_ >= MaxRecords || input.sourceOrdinal == 0u ||
        input.recordOrdinal == 0u || !kindMatches(input)) return false;
    if (recordCount_ != 0u &&
        (input.recordOrdinal <= storage_->records[recordCount_ - 1u].recordOrdinal ||
         input.sourceOrdinal <= storage_->records[recordCount_ - 1u].sourceOrdinal)) {
      return false;
    }
    const auto* rule = recordRule(input.recordType);
    if (!rule || (input.recordFlags & ~rule->allowedRecordFlags) != 0u) {
      return false;
    }
    const bool constantProducer =
        input.producer == PeSemanticProducerKind::VsFloatConstant ||
        input.producer == PeSemanticProducerKind::VsIntConstant ||
        input.producer == PeSemanticProducerKind::VsBoolConstant ||
        input.producer == PeSemanticProducerKind::PsFloatConstant ||
        input.producer == PeSemanticProducerKind::PsIntConstant ||
        input.producer == PeSemanticProducerKind::PsBoolConstant;
    const bool upProducer =
        input.producer == PeSemanticProducerKind::DrawPrimitiveUp ||
        input.producer == PeSemanticProducerKind::DrawIndexedPrimitiveUp;
    if ((!constantProducer && !input.constantBytes.empty()) ||
        (!upProducer && (!input.sparse.upIndexData.empty() ||
                         !input.sparse.upVertexData.empty())) ||
        (input.producer != PeSemanticProducerKind::Clear &&
         !input.clearRects.empty())) {
      return false;
    }
    switch (input.producer) {
      case PeSemanticProducerKind::Present:
        return input.surface0.valid();
      case PeSemanticProducerKind::StretchRect:
      case PeSemanticProducerKind::UpdateSurface:
      case PeSemanticProducerKind::Readback:
        return input.surface0.valid() && input.surface1.valid();
      case PeSemanticProducerKind::ColorFill:
        return input.surface0.valid();
      case PeSemanticProducerKind::UpdateTexture:
        return input.texture0.valid() && input.texture1.valid();
      case PeSemanticProducerKind::QueryIssue:
        return input.query.valid();
      case PeSemanticProducerKind::ReszDepthResolve:
        return input.surface0.valid() && input.texture0.valid();
      case PeSemanticProducerKind::GenerateMipmaps:
        return input.texture0.valid();
      case PeSemanticProducerKind::DrawPrimitive:
      case PeSemanticProducerKind::DrawIndexedPrimitive:
      case PeSemanticProducerKind::DrawPrimitiveUp:
      case PeSemanticProducerKind::DrawIndexedPrimitiveUp:
      case PeSemanticProducerKind::ApplyState:
        return true;
      case PeSemanticProducerKind::VsFloatConstant:
      case PeSemanticProducerKind::VsIntConstant:
      case PeSemanticProducerKind::VsBoolConstant:
      case PeSemanticProducerKind::PsFloatConstant:
      case PeSemanticProducerKind::PsIntConstant:
      case PeSemanticProducerKind::PsBoolConstant:
        return validConstantInput(input);
      case PeSemanticProducerKind::Clear:
        return input.clear.rectCount == input.clearRects.size();
      case PeSemanticProducerKind::Count:
        return false;
    }
    return false;
  }

  bool copyFixedValues(const PeSemanticRecordInput& input) noexcept {
    auto& slot = storage_->records[recordCount_];
    slot.producer = input.producer;
    slot.recordType = input.recordType;
    slot.recordFlags = input.recordFlags;
    slot.sourceOrdinal = input.sourceOrdinal;
    slot.recordOrdinal = input.recordOrdinal;
    slot.draw = input.draw;
    slot.setConst = input.setConst;
    slot.clear = input.clear;
    slot.present = input.present;
    slot.stretchRect = input.stretchRect;
    slot.colorFill = input.colorFill;
    slot.updateSurface = input.updateSurface;
    slot.queryIssue = input.queryIssue;
    slot.updateFlags = input.updateFlags;
    slot.reszFlags = input.reszFlags;
    slot.mipmapFlags = input.mipmapFlags;
    return true;
  }

  static bool validConstantInput(const PeSemanticRecordInput& input) noexcept {
    const bool boolean = input.producer == PeSemanticProducerKind::VsBoolConstant ||
                         input.producer == PeSemanticProducerKind::PsBoolConstant;
    const auto elementBytes = boolean ? 4u : 16u;
    const auto expected = static_cast<std::uint64_t>(
        input.setConst.registerCount) * elementBytes;
    const auto limit = boolean ? 16u : 256u;
    return input.setConst.registerCount <= limit && expected == input.constantBytes.size();
  }

  template <typename Object, typename Identity>
  PinIndex<kPinIndexCapacity>& exactPinIndex() noexcept {
    if constexpr (std::is_same_v<Object, D9CSurface>)
      return storage_->surfaceIdentityIndex;
    if constexpr (std::is_same_v<Object, D9CTexture>)
      return storage_->textureIdentityIndex;
    if constexpr (std::is_same_v<Object, D9CBuffer>)
      return storage_->bufferIdentityIndex;
    if constexpr (std::is_same_v<Object, D9CShader>)
      return storage_->shaderIdentityIndex;
    if constexpr (std::is_same_v<Object, D9CVertexDecl>)
      return storage_->declarationIdentityIndex;
    return storage_->queryIdentityIndex;
  }

  template <typename Object, typename Identity>
  PinIndex<kPinIndexCapacity>& objectPinIndex() noexcept {
    if constexpr (std::is_same_v<Object, D9CSurface>)
      return storage_->surfaceObjectIndex;
    if constexpr (std::is_same_v<Object, D9CTexture>)
      return storage_->textureObjectIndex;
    if constexpr (std::is_same_v<Object, D9CBuffer>)
      return storage_->bufferObjectIndex;
    if constexpr (std::is_same_v<Object, D9CShader>)
      return storage_->shaderObjectIndex;
    if constexpr (std::is_same_v<Object, D9CVertexDecl>)
      return storage_->declarationObjectIndex;
    return storage_->queryObjectIndex;
  }

  template <typename Object, typename Identity, typename Ref, typename PinArray,
            typename Retain>
  bool pin(Ref ref, PinArray& pins, std::size_t& count, std::uint32_t& out,
           Retain retain) noexcept {
    if (!ref.object) {
      if (ref.identity.kind != 0u || ref.identity.generation != 0u ||
          ref.identity.objectId != 0u) {
        return false;
      }
      out = kPeSemanticNoSlot;
      return true;
    }
    if (!ref.valid()) return false;
    const Identity identity{.value = ref.identity};
    if (!identity.valid()) return false;
    auto& exactIndex = exactPinIndex<Object, Identity>();
    auto& objectIndex = objectPinIndex<Object, Identity>();
    std::size_t existing = 0u;
    if (findPinIndex(objectIndex, identity.value, false, existing)) {
      // The object-id index also rejects a stale generation for the same
      // object identity. An exact hit is the only duplicate admitted.
      std::size_t exact = 0u;
      if (!findPinIndex(exactIndex, identity.value, true, exact) ||
          exact != existing || pins[existing].object != ref.object) {
        return false;
      }
      out = static_cast<std::uint32_t>(existing);
      return true;
    }
    if (findPinIndex(exactIndex, identity.value, true, existing)) {
      return false;
    }
    const std::size_t index = count;
    const bool warmObject = [&]() noexcept {
      if constexpr (std::is_same_v<Object, D9CSurface>)
        return retainer_.containsSurface(static_cast<D9CSurface*>(ref.object));
      if constexpr (std::is_same_v<Object, D9CTexture>)
        return retainer_.containsTexture(static_cast<D9CTexture*>(ref.object));
      if constexpr (std::is_same_v<Object, D9CBuffer>)
        return retainer_.containsBuffer(static_cast<D9CBuffer*>(ref.object));
      if constexpr (std::is_same_v<Object, D9CShader>)
        return retainer_.containsShader(static_cast<D9CShader*>(ref.object));
      if constexpr (std::is_same_v<Object, D9CVertexDecl>)
        return retainer_.containsVdecl(static_cast<D9CVertexDecl*>(ref.object));
      if constexpr (std::is_same_v<Object, D9CQuery>)
        return retainer_.containsQuery(static_cast<D9CQuery*>(ref.object));
      return false;
    }();
    if (index == MaxPins ||
        (!warmObject && retainer_.size() >= warmRetainerCapacity) ||
        !retain(static_cast<Object*>(ref.object),
                                    retainedCheckpoint_)) return false;
    pins[index] = {.object = static_cast<Object*>(ref.object),
                   .identity = identity};
    if (!insertPinIndex(exactIndex, identity.value,
                        static_cast<std::uint32_t>(index)) ||
        !insertPinIndex(objectIndex, identity.value,
                        static_cast<std::uint32_t>(index), false)) {
      return false;
    }
    ++count;
    out = static_cast<std::uint32_t>(index);
    return true;
  }

  template <typename Ref>
  static bool typedOptionalRef(const PeWireObjectRef& source,
                               Ref& out) noexcept {
    out = qualifyLocalRef<Ref>(source);
    const bool hasObject = source.object != nullptr;
    const bool hasIdentity = source.identity.kind != 0u ||
                             source.identity.generation != 0u ||
                             source.identity.objectId != 0u;
    return hasObject ? out.valid() : !hasIdentity;
  }

  bool copyDirectPins(const PeSemanticRecordInput& input,
                      PeSemanticRecordSlot& slot) noexcept {
    const auto retainSurface = [this](D9CSurface* p,
                                      D3D9PePendingCommandRetainer::Acquired&) {
      return retainer_.retainSurface(p, retainedCheckpoint_);
    };
    const auto retainTexture = [this](D9CTexture* p,
                                      D3D9PePendingCommandRetainer::Acquired&) {
      return retainer_.retainTexture(p, retainedCheckpoint_);
    };
    const auto retainBuffer = [this](D9CBuffer* p,
                                     D3D9PePendingCommandRetainer::Acquired&) {
      return retainer_.retainBuffer(p, retainedCheckpoint_);
    };
    const auto retainShader = [this](D9CShader* p,
                                     D3D9PePendingCommandRetainer::Acquired&) {
      return retainer_.retainShader(p, retainedCheckpoint_);
    };
    const auto retainDeclaration = [this](D9CVertexDecl* p,
                                          D3D9PePendingCommandRetainer::Acquired&) {
      return retainer_.retainVdecl(p, retainedCheckpoint_);
    };
    const auto retainQuery = [this](D9CQuery* p,
                                    D3D9PePendingCommandRetainer::Acquired&) {
      return retainer_.retainQuery(p, retainedCheckpoint_);
    };
    return pin<D9CSurface, PeSemanticSurfaceIdentity>(input.surface0,
                                                       storage_->surfaces, surfaceCount_, slot.surface0,
                                                       retainSurface) &&
           pin<D9CSurface, PeSemanticSurfaceIdentity>(input.surface1,
                                                       storage_->surfaces, surfaceCount_, slot.surface1,
                                                       retainSurface) &&
           pin<D9CTexture, PeSemanticTextureIdentity>(input.texture0,
                                                       storage_->textures, textureCount_, slot.texture0,
                                                       retainTexture) &&
           pin<D9CTexture, PeSemanticTextureIdentity>(input.texture1,
                                                       storage_->textures, textureCount_, slot.texture1,
                                                       retainTexture) &&
           pin<D9CBuffer, PeSemanticBufferIdentity>(input.buffer0, storage_->buffers, bufferCount_,
                                                     slot.buffer0, retainBuffer) &&
           pin<D9CBuffer, PeSemanticBufferIdentity>(input.buffer1, storage_->buffers, bufferCount_,
                                                     slot.buffer1, retainBuffer) &&
           pin<D9CShader, PeSemanticShaderIdentity>(input.shader0, storage_->shaders, shaderCount_,
                                                     slot.shader0, retainShader) &&
           pin<D9CShader, PeSemanticShaderIdentity>(input.shader1, storage_->shaders, shaderCount_,
                                                     slot.shader1, retainShader) &&
           pin<D9CVertexDecl, PeSemanticDeclarationIdentity>(
               input.declaration, storage_->declarations, declarationCount_, slot.declaration,
               retainDeclaration) &&
           pin<D9CQuery, PeSemanticQueryIdentity>(input.query, storage_->queries, queryCount_,
                                                  slot.query, retainQuery);
  }

  template <typename T, std::size_t N>
  bool appendSparse(std::span<const T> source, std::array<T, N>& arena,
                    std::size_t& used, PeSemanticArenaRange& range) noexcept {
    if (source.empty()) { range = {}; return true; }
    if (source.size() > MaxSparseValues - used) return false;
    range = {.offset = static_cast<std::uint32_t>(used),
             .count = static_cast<std::uint32_t>(source.size())};
    std::copy(source.begin(), source.end(), arena.begin() + used);
    used += source.size();
    return true;
  }

  bool appendBytes(std::span<const std::byte> source,
                   std::array<std::byte, MaxSemanticBytes>& arena,
                   std::size_t& used, PeSemanticArenaRange& range) noexcept {
    if (source.empty()) { range = {}; return true; }
    if (source.size() > MaxSemanticBytes - used ||
        source.size() > std::numeric_limits<std::uint32_t>::max()) return false;
    range = {.offset = static_cast<std::uint32_t>(used),
             .count = static_cast<std::uint32_t>(source.size())};
    std::copy(source.begin(), source.end(), arena.begin() + used);
    used += source.size();
    return true;
  }

  bool copySparse(const PeSemanticRecordInput& input,
                  PeSemanticRecordSlot& slot) noexcept {
    const auto& s = input.sparse;
    const auto withinSchema = [](std::uint16_t kind,
                                 std::size_t count) noexcept {
      const auto* rule = sectionRule(kind);
      return rule && count <= rule->maxCount;
    };
    if (!withinSchema(D9C_COMMAND_CHUNK_SECTION_RENDER_STATE,
                      s.renderStates.size()) ||
        !withinSchema(D9C_COMMAND_CHUNK_SECTION_TEXTURE, s.textures.size()) ||
        !withinSchema(D9C_COMMAND_CHUNK_SECTION_STREAM, s.streams.size()) ||
        !withinSchema(D9C_COMMAND_CHUNK_SECTION_SHADER, s.shaders.size()) ||
        !withinSchema(D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT,
                      s.vertexInputs.size()) ||
        !withinSchema(D9C_COMMAND_CHUNK_SECTION_INDEX_BUFFER,
                      s.indexBuffers.size()) ||
        !withinSchema(D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET,
                      s.renderTargets.size()) ||
        !withinSchema(D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL,
                      s.depthStencils.size()) ||
        !withinSchema(D9C_COMMAND_CHUNK_SECTION_VIEWPORT,
                      s.viewports.size()) ||
        !withinSchema(D9C_COMMAND_CHUNK_SECTION_SCISSOR, s.scissors.size()) ||
        !withinSchema(D9C_COMMAND_CHUNK_SECTION_MATERIAL, s.materials.size()) ||
        !withinSchema(D9C_COMMAND_CHUNK_SECTION_CLIP_PLANE,
                      s.clipPlanes.size()) ||
        !withinSchema(D9C_COMMAND_CHUNK_SECTION_TEXTURE_STAGE_STATE,
                      s.textureStageStates.size()) ||
        !withinSchema(D9C_COMMAND_CHUNK_SECTION_SAMPLER_STATE,
                      s.samplerStates.size()) ||
        !withinSchema(D9C_COMMAND_CHUNK_SECTION_TRANSFORM,
                      s.transforms.size()) ||
        !withinSchema(D9C_COMMAND_CHUNK_SECTION_LIGHT, s.lights.size()) ||
        !withinSchema(D9C_COMMAND_CHUNK_SECTION_LIGHT_ENABLE,
                      s.lightEnables.size())) {
      lastAdmissionFailure_ = AdmissionFailure::SparseSchema;
      return false;
    }
    if (!appendSparse(s.renderStates, storage_->renderStates, sparseCounts_.values[kRenderStates], slot.renderStates) ||
        !appendSparse(s.viewports, storage_->viewports, sparseCounts_.values[kViewports], slot.viewports) ||
        !appendSparse(s.scissors, storage_->scissors, sparseCounts_.values[kScissors], slot.scissors) ||
        !appendSparse(s.materials, storage_->materials, sparseCounts_.values[kMaterials], slot.materials) ||
        !appendSparse(s.clipPlanes, storage_->clipPlanes, sparseCounts_.values[kClipPlanes], slot.clipPlanes) ||
        !appendSparse(s.textureStageStates, storage_->textureStageStates, sparseCounts_.values[kTextureStageStates], slot.textureStageStates) ||
        !appendSparse(s.samplerStates, storage_->samplerStates, sparseCounts_.values[kSamplerStates], slot.samplerStates) ||
        !appendSparse(s.transforms, storage_->transforms, sparseCounts_.values[kTransforms], slot.transforms) ||
        !appendSparse(s.lights, storage_->lights, sparseCounts_.values[kLights], slot.lights) ||
        !appendSparse(s.lightEnables, storage_->lightEnables, sparseCounts_.values[kLightEnables], slot.lightEnables)) {
      lastAdmissionFailure_ = AdmissionFailure::SparseArena;
      return false;
    }
    return copyBindingSparse(s, slot);
  }

  bool copyVariablePayloads(const PeSemanticRecordInput& input,
                            PeSemanticRecordSlot& slot) noexcept {
    slot.vsFloatConstant = {.startRegister = input.sparse.vsFloatConstants.startRegister,
                            .registerCount = input.sparse.vsFloatConstants.registerCount};
    slot.vsIntConstant = {.startRegister = input.sparse.vsIntConstants.startRegister,
                          .registerCount = input.sparse.vsIntConstants.registerCount};
    slot.vsBoolConstant = {.startRegister = input.sparse.vsBoolConstants.startRegister,
                           .registerCount = input.sparse.vsBoolConstants.registerCount};
    slot.psFloatConstant = {.startRegister = input.sparse.psFloatConstants.startRegister,
                            .registerCount = input.sparse.psFloatConstants.registerCount};
    slot.psIntConstant = {.startRegister = input.sparse.psIntConstants.startRegister,
                          .registerCount = input.sparse.psIntConstants.registerCount};
    slot.psBoolConstant = {.startRegister = input.sparse.psBoolConstants.startRegister,
                           .registerCount = input.sparse.psBoolConstants.registerCount};
    if (!appendBytes(input.constantBytes, storage_->constantBytes, semanticBytes_,
                     slot.constantBytes) ||
        !appendBytes(input.sparse.vsFloatConstants.registerBytes,
                     storage_->constantBytes, semanticBytes_, slot.vsFloatConstants) ||
        !appendBytes(input.sparse.vsIntConstants.registerBytes,
                     storage_->constantBytes, semanticBytes_, slot.vsIntConstants) ||
        !appendBytes(input.sparse.vsBoolConstants.registerBytes,
                     storage_->constantBytes, semanticBytes_, slot.vsBoolConstants) ||
        !appendBytes(input.sparse.psFloatConstants.registerBytes,
                     storage_->constantBytes, semanticBytes_, slot.psFloatConstants) ||
        !appendBytes(input.sparse.psIntConstants.registerBytes,
                     storage_->constantBytes, semanticBytes_, slot.psIntConstants) ||
        !appendBytes(input.sparse.psBoolConstants.registerBytes,
                     storage_->constantBytes, semanticBytes_, slot.psBoolConstants) ||
        !appendBytes(input.sparse.upIndexData, storage_->constantBytes, semanticBytes_,
                     slot.upIndexBytes) ||
        !appendBytes(input.sparse.upVertexData, storage_->constantBytes, semanticBytes_,
                     slot.upVertexBytes)) return false;
    if (!input.clearRects.empty()) {
      if (input.clearRects.size() > MaxRects - rectCount_) return false;
      slot.clearRects = {.offset = static_cast<std::uint32_t>(rectCount_),
                         .count = static_cast<std::uint32_t>(input.clearRects.size())};
      std::copy(input.clearRects.begin(), input.clearRects.end(),
                storage_->rects.begin() + rectCount_);
      rectCount_ += input.clearRects.size();
    }
    return true;
  }

  bool copyBindingSparse(const SparseStateInput& s,
                         PeSemanticRecordSlot& slot) noexcept {
    // The typed sparse arenas are copied by the small per-family routines
    // below.  They validate the source's kind before asking the retainer for a
    // pin, so an invalid/foreign binding cannot leak a retain.
    if (!copyTextures(s.textures, slot)) {
      lastAdmissionFailure_ = AdmissionFailure::SparseTextures;
      return false;
    }
    if (!copyStreams(s.streams, slot)) {
      lastAdmissionFailure_ = AdmissionFailure::SparseStreams;
      return false;
    }
    if (!copyShaders(s.shaders, slot)) {
      lastAdmissionFailure_ = AdmissionFailure::SparseShaders;
      return false;
    }
    if (!copyVertexInputs(s.vertexInputs, slot)) {
      lastAdmissionFailure_ = AdmissionFailure::SparseVertexInputs;
      return false;
    }
    if (!copyIndexBuffers(s.indexBuffers, slot)) {
      lastAdmissionFailure_ = AdmissionFailure::SparseIndexBuffers;
      return false;
    }
    if (!copyRenderTargets(s.renderTargets, slot)) {
      lastAdmissionFailure_ = AdmissionFailure::SparseRenderTargets;
      return false;
    }
    if (!copyDepthStencils(s.depthStencils, slot)) {
      lastAdmissionFailure_ = AdmissionFailure::SparseDepthStencils;
      return false;
    }
    return true;
  }

  bool copyTextures(std::span<const SparseBindingInput<D9CCommandChunkWireTextureBinding>> source,
                    PeSemanticRecordSlot& slot) noexcept {
    if (source.size() > MaxSparseValues - sparseCounts_.values[kTextures]) return false;
    const auto start = sparseCounts_.values[kTextures];
    for (const auto& item : source) {
      auto& out = storage_->texturesArena[sparseCounts_.values[kTextures]++];
      out.wire = item.wire;
      if (item.wire.valid) {
        auto ref = TextureRef{};
        if (!typedOptionalRef(item.object, ref)) return false;
        if (ref.object) {
          if (!ref.valid() || !pin<D9CTexture, PeSemanticTextureIdentity>(ref, storage_->textures, textureCount_, out.pin,
                                                         retainTextureFn())) return false;
          out.hasPin = true;
        }
      }
    }
    slot.textures = {.offset = static_cast<std::uint32_t>(start),
                     .count = static_cast<std::uint32_t>(source.size())};
    return true;
  }

  bool copyStreams(std::span<const SparseBindingInput<D9CCommandChunkWireStreamBinding>> source,
                   PeSemanticRecordSlot& slot) noexcept {
    if (source.size() > MaxSparseValues - sparseCounts_.values[kStreams]) return false;
    const auto start = sparseCounts_.values[kStreams];
    for (const auto& item : source) {
      auto& out = storage_->streamsArena[sparseCounts_.values[kStreams]++];
      out.wire = item.wire;
      if (item.wire.valid) {
        auto ref = BufferRef{};
        if (!typedOptionalRef(item.object, ref)) return false;
        if (ref.object) {
          if (!ref.valid() || !pin<D9CBuffer, PeSemanticBufferIdentity>(ref, storage_->buffers, bufferCount_, out.pin,
                                                         retainBufferFn())) return false;
          out.hasPin = true;
        }
      }
    }
    slot.streams = {.offset = static_cast<std::uint32_t>(start),
                    .count = static_cast<std::uint32_t>(source.size())};
    return true;
  }

  bool copyShaders(std::span<const SparseBindingInput<D9CCommandChunkWireShaderBinding>> source,
                   PeSemanticRecordSlot& slot) noexcept {
    if (source.size() > MaxSparseValues - sparseCounts_.values[kShaders]) return false;
    const auto start = sparseCounts_.values[kShaders];
    for (const auto& item : source) {
      auto& out = storage_->shadersArena[sparseCounts_.values[kShaders]++];
      out.wire = item.wire;
      if (item.wire.valid) {
        auto ref = ShaderRef{};
        if (!typedOptionalRef(item.object, ref)) return false;
        if (ref.object) {
          if (!ref.valid() || !pin<D9CShader, PeSemanticShaderIdentity>(ref, storage_->shaders, shaderCount_, out.pin,
                                                         retainShaderFn())) return false;
          out.hasPin = true;
        }
      }
    }
    slot.shaders = {.offset = static_cast<std::uint32_t>(start),
                    .count = static_cast<std::uint32_t>(source.size())};
    return true;
  }

  bool copyVertexInputs(std::span<const SparseBindingInput<D9CCommandChunkWireVertexInput>> source,
                        PeSemanticRecordSlot& slot) noexcept {
    if (source.size() > MaxSparseValues - sparseCounts_.values[kVertexInputs]) return false;
    const auto start = sparseCounts_.values[kVertexInputs];
    for (const auto& item : source) {
      auto& out = storage_->vertexInputsArena[sparseCounts_.values[kVertexInputs]++];
      out.wire = item.wire;
      if (item.wire.valid && item.wire.kind == D9C_COMMAND_CHUNK_VERTEX_INPUT_DECLARATION) {
        auto ref = DeclarationRef{};
        if (!typedOptionalRef(item.object, ref)) return false;
        if (ref.object) {
          if (!ref.valid() || !pin<D9CVertexDecl, PeSemanticDeclarationIdentity>(ref, storage_->declarations, declarationCount_, out.pin,
                                                                   retainDeclarationFn())) return false;
          out.hasPin = true;
        }
      } else if (item.wire.valid && item.wire.kind == D9C_COMMAND_CHUNK_VERTEX_INPUT_FVF && item.object.object) {
        return false;
      }
    }
    slot.vertexInputs = {.offset = static_cast<std::uint32_t>(start),
                         .count = static_cast<std::uint32_t>(source.size())};
    return true;
  }

  bool copyIndexBuffers(std::span<const SparseBindingInput<D9CCommandChunkWireIndexBinding>> source,
                        PeSemanticRecordSlot& slot) noexcept {
    if (source.size() > MaxSparseValues - sparseCounts_.values[kIndexBuffers]) return false;
    const auto start = sparseCounts_.values[kIndexBuffers];
    for (const auto& item : source) {
      auto& out = storage_->indexBuffersArena[sparseCounts_.values[kIndexBuffers]++];
      out.wire = item.wire;
      if (item.wire.valid) {
        auto ref = BufferRef{};
        if (!typedOptionalRef(item.object, ref)) return false;
        if (ref.object) {
          if (!ref.valid() || !pin<D9CBuffer, PeSemanticBufferIdentity>(ref, storage_->buffers, bufferCount_, out.pin,
                                                         retainBufferFn())) return false;
          out.hasPin = true;
        }
      }
    }
    slot.indexBuffers = {.offset = static_cast<std::uint32_t>(start),
                         .count = static_cast<std::uint32_t>(source.size())};
    return true;
  }

  bool copyRenderTargets(std::span<const SparseBindingInput<D9CCommandChunkWireRenderTargetBinding>> source,
                         PeSemanticRecordSlot& slot) noexcept {
    if (source.size() > MaxSparseValues - sparseCounts_.values[kRenderTargets]) return false;
    const auto start = sparseCounts_.values[kRenderTargets];
    for (const auto& item : source) {
      auto& out = storage_->renderTargetsArena[sparseCounts_.values[kRenderTargets]++];
      out.wire = item.wire;
      if (item.wire.valid) {
        auto ref = SurfaceRef{};
        if (!typedOptionalRef(item.object, ref)) return false;
        if (ref.object) {
          if (!ref.valid() || !pin<D9CSurface, PeSemanticSurfaceIdentity>(ref, storage_->surfaces, surfaceCount_, out.pin,
                                                          retainSurfaceFn())) return false;
          out.hasPin = true;
        }
      }
    }
    slot.renderTargets = {.offset = static_cast<std::uint32_t>(start),
                          .count = static_cast<std::uint32_t>(source.size())};
    return true;
  }

  bool copyDepthStencils(std::span<const SparseBindingInput<D9CCommandChunkWireDepthStencilBinding>> source,
                         PeSemanticRecordSlot& slot) noexcept {
    if (source.size() > MaxSparseValues - sparseCounts_.values[kDepthStencils]) return false;
    const auto start = sparseCounts_.values[kDepthStencils];
    for (const auto& item : source) {
      auto& out = storage_->depthStencilsArena[sparseCounts_.values[kDepthStencils]++];
      out.wire = item.wire;
      if (item.wire.valid) {
        auto ref = SurfaceRef{};
        if (!typedOptionalRef(item.object, ref)) return false;
        if (ref.object) {
          if (!ref.valid() || !pin<D9CSurface, PeSemanticSurfaceIdentity>(ref, storage_->surfaces, surfaceCount_, out.pin,
                                                          retainSurfaceFn())) return false;
          out.hasPin = true;
        }
      }
    }
    slot.depthStencils = {.offset = static_cast<std::uint32_t>(start),
                          .count = static_cast<std::uint32_t>(source.size())};
    return true;
  }

  // Typed retain functors keep the owner API free of type-erased object sinks.
  auto retainSurfaceFn() noexcept {
    return [this](D9CSurface* p, D3D9PePendingCommandRetainer::Acquired&) {
      return retainer_.retainSurface(p, retainedCheckpoint_);
    };
  }
  auto retainTextureFn() noexcept {
    return [this](D9CTexture* p, D3D9PePendingCommandRetainer::Acquired&) {
      return retainer_.retainTexture(p, retainedCheckpoint_);
    };
  }
  auto retainBufferFn() noexcept {
    return [this](D9CBuffer* p, D3D9PePendingCommandRetainer::Acquired&) {
      return retainer_.retainBuffer(p, retainedCheckpoint_);
    };
  }
  auto retainShaderFn() noexcept {
    return [this](D9CShader* p, D3D9PePendingCommandRetainer::Acquired&) {
      return retainer_.retainShader(p, retainedCheckpoint_);
    };
  }
  auto retainDeclarationFn() noexcept {
    return [this](D9CVertexDecl* p, D3D9PePendingCommandRetainer::Acquired&) {
      return retainer_.retainVdecl(p, retainedCheckpoint_);
    };
  }
  auto retainQueryFn() noexcept {
    return [this](D9CQuery* p, D3D9PePendingCommandRetainer::Acquired&) {
      return retainer_.retainQuery(p, retainedCheckpoint_);
    };
  }

  std::size_t surfaceCount_ = 0u;
  std::size_t textureCount_ = 0u;
  std::size_t bufferCount_ = 0u;
  std::size_t shaderCount_ = 0u;
  std::size_t declarationCount_ = 0u;
  std::size_t queryCount_ = 0u;

  std::unique_ptr<Storage> storage_;
  bool ready_ = false;
  D3D9PePendingCommandRetainer retainer_{MaxPins};
  D3D9PePendingCommandRetainer::Acquired retainedCheckpoint_{};
  std::size_t recordCount_ = 0u;
  std::size_t semanticBytes_ = 0u;
  // Cached emission frontiers are updated with each admission and restored by
  // rollback. They are intentionally separate from semanticBytes_, which is
  // the borrowed-variable arena cursor rather than wire payload size.
  std::size_t emissionHandleCount_ = 0u;
  std::size_t emissionPayloadBytes_ = 0u;
  std::size_t rectCount_ = 0u;
  std::size_t lastClearedBytes_ = 0u;
  SparseCounts sparseCounts_{};
  std::uint64_t settledChunks_ = 0u;
  std::uint64_t lastSourceOrdinal_ = 0u;
  AdmissionFailure lastAdmissionFailure_ = AdmissionFailure::None;
  std::uint64_t lastRecordOrdinal_ = 0u;
};

// CapacityPre needs a current payload frontier even before the first record.
// An empty owner has no emit-ready chunk, so emissionMetrics() correctly
// returns false there; for cadence purposes that same state is the valid zero
// frontier. Keep this distinction shared by production and native tests.
template <typename Owner>
bool resolvePeSemanticCadenceMetrics(const Owner& owner, std::size_t& handles,
                                     std::size_t& payload,
                                     std::size_t& wire) noexcept {
  handles = payload = wire = 0u;
  return owner.size() == 0u || owner.emissionMetrics(handles, payload, wire);
}

template <std::size_t MaxRecords, std::size_t MaxPins,
          std::size_t MaxSemanticBytes, std::size_t MaxRects,
          std::size_t MaxSparseValues>
bool appendOwnedRecord(
    PeSemanticBatchOwner<MaxRecords, MaxPins, MaxSemanticBytes, MaxRects,
                         MaxSparseValues>& owner,
    const PeSemanticRecordInput& input) noexcept {
  return owner.appendOwnedRecord(input);
}

// The old Present/Readback candidate remains a call-local pilot.  It is not a
// chunk owner and must not be used as the all-family semantic owner.
static_assert(static_cast<std::size_t>(PeSemanticProducerKind::Count) == 21u);

}  // namespace dxmt9::d3d9::pe
