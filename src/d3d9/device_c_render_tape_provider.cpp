#include "device_c_render_tape_provider.hpp"

#include "device_c_chunk_replay.hpp"
#include "device_c_common.hpp"
#include "device_c_render_tape_capture.hpp"
#include "device_c_render_tape_capture_layout.hpp"
#include "device_c_render_tape_identity.hpp"
#include "device_c_render_tape_initial_setup_v4.hpp"
#include "device_c_render_tape_action_profile_component.hpp"
#include "device_c_render_tape_identity_transfer_v4.hpp"
#include "device_c_render_tape_mutation_admission_v4.hpp"
#include "device_c_render_tape_runtime_binding_v4.hpp"
#include "device_c_render_tape_source_plan.hpp"
#include "device_c_render_tape_source_plan_v4.hpp"
#include "device_c_render_tape_runtime_objects.hpp"
#include "dxmt9/dxmt9_format_convert.hpp"
#include "dxmt9/dxmt9_device.hpp"
#include "dxmt9/dxmt9_presenter.hpp"
#include "dxmt9/dxmt9_render_tape_attachment_restore.hpp"
#include "dxmt9/dxmt9_cpu_supply_trace.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace dxmt9::d3d9 {

namespace render_tape_runtime {

PreparedObserverV4Session::PreparedObserverV4Session(RenderTapePreparedObserverV4* observer,
    RenderTapePreparedObserverV4Evidence& evidence, D9CDevice* device) noexcept
    : observer_(observer), evidence_(evidence), device_(device) {
  if (observer_) {
    evidence_.requested = true;
    evidence_.admitted = true;
    evidence_.failure = "none";
  }
}

bool PreparedObserverV4Session::enabled() const noexcept {
  return observer_ && !rejected_ && !evidence_.closed;
}

void PreparedObserverV4Session::reject(const char* reason) noexcept {
  if (!observer_ || rejected_) return;
  rejected_ = true;
  evidence_.complete = false;
  evidence_.failure = reason;
}

void PreparedObserverV4Session::buffer(const RenderTapePreparedBufferEventV4& event) noexcept {
  if (!enabled() || !observer_->buffer) return;
  ++evidence_.bufferCallbacks;
  const auto phase = static_cast<std::size_t>(event.phase);
  if (phase < evidence_.bufferPhases.size()) ++evidence_.bufferPhases[phase];
  if (!observer_->buffer(observer_->context, device_, event))
    reject("v4-observer-buffer-rejected");
}

void PreparedObserverV4Session::armed(std::int32_t result,
    const D9CRenderTapeIdentityCaptureV4ArmResult& arm,
    const std::shared_ptr<RenderTapeRuntimeIdentityV4>& root) noexcept {
  if (!enabled() || !observer_->armed) return;
  ++evidence_.armedCallbacks;
  if (!observer_->armed(observer_->context, device_, result, arm, root))
    reject("v4-observer-arm-rejected");
}

ProviderReplayObservationV4 PreparedObserverV4Session::observation(
    ProviderReplayObservationV4 original,
    std::span<const RenderTapePreparedHandleAssociationV4> associations) noexcept {
  if (!observer_) return original;
  original_ = original;
  associations_ = associations;
  auto forwarded = original;
  forwarded.context = this;
  forwarded.rawStarted = [](void* opaque, std::uint64_t event,
      std::uint64_t raw, std::uint32_t count) noexcept {
    const auto& session = *static_cast<PreparedObserverV4Session*>(opaque);
    return !session.original_.rawStarted || session.original_.rawStarted(
        session.original_.context, event, raw, count);
  };
  forwarded.beforeRecord = [](void* opaque, std::uint32_t index,
      std::uint32_t type) noexcept {
    auto& session = *static_cast<PreparedObserverV4Session*>(opaque);
    if (session.enabled() && session.observer_->beforeRecord) {
      ++session.evidence_.beforeRecordCallbacks;
      if (!session.observer_->beforeRecord(session.observer_->context, session.device_,
              session.original_.eventOrdinal, index, type, session.associations_))
        session.reject("v4-observer-record-rejected");
    }
    return !session.original_.beforeRecord || session.original_.beforeRecord(
        session.original_.context, index, type);
  };
  forwarded.beforeDispatch = [](void* opaque, std::uint32_t index,
      std::uint32_t type) noexcept {
    const auto& session = *static_cast<PreparedObserverV4Session*>(opaque);
    return !session.original_.beforeDispatch || session.original_.beforeDispatch(
        session.original_.context, index, type);
  };
  forwarded.recordDispatched = [](void* opaque, std::uint32_t index,
      std::uint32_t type, std::int32_t result) noexcept {
    const auto& session = *static_cast<PreparedObserverV4Session*>(opaque);
    return !session.original_.recordDispatched || session.original_.recordDispatched(
        session.original_.context, index, type, result);
  };
  forwarded.rawSettled = [](void* opaque, std::uint64_t raw,
      std::uint32_t count, bool succeeded) noexcept {
    const auto& session = *static_cast<PreparedObserverV4Session*>(opaque);
    return !session.original_.rawSettled || session.original_.rawSettled(
        session.original_.context, raw, count, succeeded);
  };
  return forwarded;
}

void PreparedObserverV4Session::observationComplete() noexcept {
  original_ = {};
  associations_ = {};
}

void PreparedObserverV4Session::closed(bool traversalComplete) noexcept {
  if (!observer_ || evidence_.closed) return;
  evidence_.closed = true;
  if (observer_->closed) {
    ++evidence_.closedCallbacks;
    if (!observer_->closed(observer_->context, device_))
      reject("v4-observer-closed-rejected");
  }
  if (!rejected_ && !traversalComplete)
    reject("v4-observer-traversal-incomplete");
  evidence_.complete = !rejected_ && traversalComplete;
}

} // namespace render_tape_runtime

namespace {

using render_tape_runtime::OwnedObject;
using Definition = render_tape_runtime::Definition;
using Chunk = render_tape_runtime::CommandChunkView;
using GeneralPreflightPlan = render_tape_runtime::GeneralPreflightPlan;

constexpr std::size_t kMaxSourceReplayPublications =
    static_cast<std::size_t>(kProviderSourceReplayMaxRecords) * 2u + 1u;

// Cold diagnostic plan: all record spans are admitted before any tape effect.
// Capture source/seq labels remain provenance; the queue owns fresh identities.
struct SourceBoundaryReplayPlan {
  CaptureSourceReplayPlan sources{};
  std::vector<std::vector<std::uint32_t>> batchCuts{};
};

bool buildSourceBoundaryReplayPlan(
    std::span<const std::byte> tape, const GeneralPreflightPlan& plan,
    std::span<const std::byte> sidecar,
    std::span<const RenderTapeBatchCut> batchCuts, SourceBoundaryReplayPlan& out,
    RenderTapeSourceBoundaryEvidence& evidence) noexcept {
  try {
    if (batchCuts.size() > 4096u) return false;
    for (std::size_t i = 1u; i < batchCuts.size(); ++i) {
      const auto& a = batchCuts[i - 1u];
      const auto& b = batchCuts[i];
      if (b.eventOrdinal < a.eventOrdinal ||
          (b.eventOrdinal == a.eventOrdinal && b.beforeRecord <= a.beforeRecord))
        return false;
    }
    SourceBoundaryReplayPlan candidate;
    if (buildCaptureSourceReplayPlan(tape, plan, sidecar, candidate.sources) !=
        CaptureSourceReplayPlanStatus::Valid) return false;
    candidate.batchCuts.resize(plan.commandChunks.size());
    std::size_t nextBatchCut = 0u;
    for (const auto& event : plan.events) {
      if (event.type != RenderTapeEventType::CommandChunk) continue;
      if (event.index >= candidate.batchCuts.size()) return false;
      const auto ordinal = plan.tape.event(event.eventIndex).header.ordinal;
      const auto ranges = candidate.sources.rangesForCommandChunk(event.index);
      auto& cuts = candidate.batchCuts[event.index];
      while (nextBatchCut < batchCuts.size() &&
             batchCuts[nextBatchCut].eventOrdinal == ordinal) {
        cuts.push_back(batchCuts[nextBatchCut++].beforeRecord);
      }
      if (!validateProviderReplayBatchCuts(
              plan.commandChunks[event.index].envelope.recordCount, ranges, cuts))
        return false;
      if (!cuts.empty()) {
        ImportedChunkView imported;
        const auto& chunk = plan.commandChunks[event.index];
        if (!importPrevalidatedCommandChunk(chunk.bytes, chunk.envelope, imported))
          return false;
        for (const auto cut : cuts) {
          const auto type = imported.records[cut].type;
          if (type != D9C_COMMAND_RECORD_DRAW_PRIMITIVE &&
              type != D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE) return false;
        }
      }
    }
    if (nextBatchCut != batchCuts.size()) return false;
    evidence.sourceCount = static_cast<std::uint32_t>(candidate.sources.rows().size());
    evidence.validated = true;
    evidence.batchCutCount = static_cast<std::uint32_t>(batchCuts.size());
    out = std::move(candidate);
    return true;
  } catch (...) {
    return false;
  }
}

template <typename T>
bool load(std::span<const std::byte> bytes, std::size_t offset, T& out) {
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) return false;
  std::memcpy(&out, bytes.data() + offset, sizeof(T));
  return true;
}

bool sameIdentity(const D9CWireObjectIdentity& a,
                  const D9CWireObjectIdentity& b) {
  return a.kind == b.kind && a.generation == b.generation &&
         a.objectId == b.objectId;
}

bool containsIdentity(std::span<const D9CWireObjectIdentity> identities,
                      const D9CWireObjectIdentity& identity) noexcept {
  return std::any_of(identities.begin(), identities.end(),
                     [&](const auto& value) { return sameIdentity(value, identity); });
}

bool sameDigest(const RenderTapeDigest& a, const RenderTapeDigest& b) {
  return std::equal(a.begin(), a.end(), b.begin());
}

bool nonZeroDigest(const RenderTapeDigest& digest) noexcept {
  return std::any_of(digest.begin(), digest.end(), [](std::byte value) {
    return value != std::byte{};
  });
}

bool checkedMul(std::uint64_t a, std::uint64_t b,
                std::uint64_t& out) noexcept {
  if (a != 0u && b > std::numeric_limits<std::uint64_t>::max() / a) {
    return false;
  }
  out = a * b;
  return true;
}

bool checkedAdd(std::uint64_t a, std::uint64_t b,
                std::uint64_t& out) noexcept {
  if (a > std::numeric_limits<std::uint64_t>::max() - b) return false;
  out = a + b;
  return true;
}

const RenderTapeProviderBlob* findBlob(
    std::span<const RenderTapeProviderBlob> blobs,
    const RenderTapeDigest& digest) {
  const auto it = std::find_if(blobs.begin(), blobs.end(), [&](const auto& blob) {
    return sameDigest(blob.digest, digest);
  });
  return it == blobs.end() ? nullptr : &*it;
}

RenderTapeBlobCatalogue makeCatalogue(
    std::span<const RenderTapeProviderBlob> blobs, bool& valid) {
  RenderTapeBlobCatalogue catalogue;
  valid = true;
  for (std::size_t i = 0; i < blobs.size(); ++i) {
    if (std::any_of(blobs.begin(), blobs.begin() + i, [&](const auto& prior) {
          return sameDigest(prior.digest, blobs[i].digest);
        })) {
      valid = false;
      return {};
    }
    const auto actual = RenderTapeCaptureSession::sha256(blobs[i].bytes);
    if (!sameDigest(actual, blobs[i].digest)) {
      valid = false;
      return {};
    }
    catalogue.blobs.push_back(RenderTapeBlob{
        .digest = blobs[i].digest,
        .size = blobs[i].bytes.size(),
        .verified = 1u,
    });
  }
  return catalogue;
}

bool fullRect(const D9CRect& rect, std::uint32_t width,
              std::uint32_t height) {
  return rect.left == 0 && rect.top == 0 &&
         rect.right == static_cast<std::int32_t>(width) &&
         rect.bottom == static_cast<std::int32_t>(height);
}

bool presentGeometrySupported(const D9CCommandChunkWirePresent& present,
                              const D9CSurfaceDesc& output) noexcept {
  return present.flags == 0u && present.hasSrc == present.hasDst &&
      (present.hasSrc == 0u ||
       (fullRect(present.src, output.width, output.height) &&
        fullRect(present.dst, output.width, output.height)));
}

bool applyGammaRamp(D9CDevice* device,
                    std::span<const std::byte> bytes) noexcept {
  if (!device || bytes.size() != kRenderTapeGammaRampBytes) return false;
  std::array<std::uint16_t, kRenderTapeGammaRampBytes / sizeof(std::uint16_t)>
      gamma{};
  std::memcpy(gamma.data(), bytes.data(), bytes.size());
  dxmt9c_device_set_gamma_ramp(device, gamma.data());
  return true;
}

struct IntervalPlan {
  std::vector<RenderTapeResourceMutationHeader> mutationsBefore{};
  std::vector<Chunk> frame{};
  RenderTapeDigestValidity expectedDigestValidity =
      RenderTapeDigestValidity::NotCaptured;
  RenderTapeDigest expectedDigest{};
  std::uint64_t presentOrdinal = 0u;
  std::uint64_t completionOrdinal = 0u;
  bool sawTexturedDraw = false;
};

struct PreflightPlan {
  ImportedRenderTapeView tape{};
  RenderTapeBlobCatalogue catalogue{};
  std::vector<Definition> definitions{};
  std::vector<RenderTapeResourceMutationHeader> initialMutations{};
  std::vector<Chunk> bootstrap{};
  std::uint32_t bootstrapProfileVersion = kRenderTapeBaselineProfileVersion;
  std::array<IntervalPlan, kRenderTapeMaxReplayIntervals> intervals{};
  std::uint32_t intervalCount = 0u;
  D9CWireObjectIdentity outputIdentity{};
  D9CSurfaceDesc outputDesc{};
  D9CWireObjectIdentity textureIdentity{};
  RenderTapeTextureDescriptorV2 textureDesc{};
  D9CSurfaceDesc textureLevel0{};
  bool textureProducedByCapturedPass = false;
  std::vector<D9CWireObjectIdentity> seededArmColorTextures{};
  std::vector<std::pair<D9CWireObjectIdentity, D9CSurfaceDesc>>
      producedStandaloneSurfaces{};
  std::vector<std::pair<D9CWireObjectIdentity, D9CSurfaceDesc>>
      seededStandaloneDepthSurfaces{};
  std::vector<std::pair<D9CWireObjectIdentity, D9CSurfaceDesc>>
      seededStandaloneColorSurfaces{};
  std::vector<D9CWireObjectIdentity> drawTextureIdentities{};
  D9CWireObjectIdentity vertexDeclarationIdentity{};
};

enum class FrameRecordState : std::uint8_t {
  ExpectClear,
  ExpectDrawOrPresent,
  ExpectPresent,
  Complete,
};

struct BoundedDrawState {
  D9CWireObjectIdentity texture{};
  D9CWireObjectIdentity vertexDeclaration{};
  std::uint32_t fvf = 0u;
  bool unsupportedBinding = false;
};

bool nullHandle(std::uint32_t handleIndex) {
  return handleIndex == D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
}

bool handleIdentity(const ImportedChunkView& chunk, std::uint32_t handleIndex,
                    std::uint32_t kind, D9CWireObjectIdentity& identity) {
  if (nullHandle(handleIndex)) {
    identity = {};
    return true;
  }
  if (handleIndex >= chunk.handles.size() ||
      chunk.handles[handleIndex].kind != kind) return false;
  const auto& handle = chunk.handles[handleIndex];
  identity = D9CWireObjectIdentity{
      .kind = handle.kind,
      .generation = handle.generation,
      .objectId = handle.objectId,
  };
  return true;
}

bool applyBoundedBindings(const ImportedChunkView& chunk,
                          const ImportedRecordView& record,
                          BoundedDrawState& state,
                          std::span<const std::byte>* upVertices = nullptr) {
  for (std::size_t index = 0u; index < record.sections.size(); ++index) {
    const auto section = record.section(index);
    switch (section.descriptor.kind) {
    case D9C_COMMAND_CHUNK_SECTION_TEXTURE:
      for (std::uint32_t bindingIndex = 0u;
           bindingIndex < section.descriptor.count; ++bindingIndex) {
        D9CCommandChunkWireTextureBinding binding{};
        if (!load(section.payload, bindingIndex * sizeof(binding), binding))
          return false;
        if (!binding.valid) continue;
        D9CWireObjectIdentity identity{};
        if (!handleIdentity(chunk, binding.handleIndex,
                            D9C_CHUNK_HANDLE_KIND_TEXTURE, identity)) return false;
        if (binding.slot == 0u) {
          state.texture = identity;
        } else if (identity.objectId != 0u) {
          state.unsupportedBinding = true;
        }
      }
      break;
    case D9C_COMMAND_CHUNK_SECTION_STREAM:
      for (std::uint32_t bindingIndex = 0u;
           bindingIndex < section.descriptor.count; ++bindingIndex) {
        D9CCommandChunkWireStreamBinding binding{};
        if (!load(section.payload, bindingIndex * sizeof(binding), binding))
          return false;
        if (binding.valid && !nullHandle(binding.handleIndex))
          state.unsupportedBinding = true;
      }
      break;
    case D9C_COMMAND_CHUNK_SECTION_SHADER:
      for (std::uint32_t bindingIndex = 0u;
           bindingIndex < section.descriptor.count; ++bindingIndex) {
        D9CCommandChunkWireShaderBinding binding{};
        if (!load(section.payload, bindingIndex * sizeof(binding), binding))
          return false;
        if (binding.valid && !nullHandle(binding.handleIndex))
          state.unsupportedBinding = true;
      }
      break;
    case D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT: {
      D9CCommandChunkWireVertexInput input{};
      if (section.descriptor.count != 1u ||
          !load(section.payload, 0u, input)) return false;
      if (!input.valid ||
          (input.kind != D9C_COMMAND_CHUNK_VERTEX_INPUT_FVF &&
           input.kind != D9C_COMMAND_CHUNK_VERTEX_INPUT_DECLARATION)) {
        state.unsupportedBinding = true;
      } else if (input.kind == D9C_COMMAND_CHUNK_VERTEX_INPUT_FVF) {
        if (!nullHandle(input.handleIndex)) state.unsupportedBinding = true;
        state.fvf = input.value;
        state.vertexDeclaration = {};
      } else {
        D9CWireObjectIdentity identity{};
        if (!handleIdentity(chunk, input.handleIndex,
                            D9C_CHUNK_HANDLE_KIND_VERTEX_DECL, identity) ||
            identity.objectId == 0u) {
          return false;
        }
        state.vertexDeclaration = identity;
        state.fvf = 0u;
      }
      break;
    }
    case D9C_COMMAND_CHUNK_SECTION_INDEX_BUFFER:
      for (std::uint32_t bindingIndex = 0u;
           bindingIndex < section.descriptor.count; ++bindingIndex) {
        D9CCommandChunkWireIndexBinding binding{};
        if (!load(section.payload, bindingIndex * sizeof(binding), binding))
          return false;
        if (binding.valid && !nullHandle(binding.handleIndex))
          state.unsupportedBinding = true;
      }
      break;
    case D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL: {
      D9CCommandChunkWireDepthStencilBinding binding{};
      if (section.descriptor.count != 1u ||
          !load(section.payload, 0u, binding)) return false;
      if (binding.valid && !nullHandle(binding.handleIndex))
        state.unsupportedBinding = true;
      break;
    }
    case D9C_COMMAND_CHUNK_SECTION_UP_INDEX_DATA:
      state.unsupportedBinding = true;
      break;
    case D9C_COMMAND_CHUNK_SECTION_UP_VERTEX_DATA:
      if (!upVertices || !upVertices->empty()) return false;
      *upVertices = section.payload;
      break;
    default:
      break;
    }
  }
  return true;
}

bool acceptedTexturedDraw(const ImportedChunkView& chunk,
                          const ImportedRecordView& record,
                          BoundedDrawState& state,
                          const D9CWireObjectIdentity& expectedVertexDeclaration) {
  constexpr std::uint32_t kTriangleList = 4u;
  constexpr std::uint32_t kXyzRhwDiffuseTex1Fvf = 0x144u;
  constexpr std::uint32_t kVertexStride = 28u;
  if (record.drawHeader.flags != 0u) return false;
  for (const auto& section : record.sections) {
    if (section.kind != D9C_COMMAND_CHUNK_SECTION_TEXTURE &&
        section.kind != D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT &&
        section.kind != D9C_COMMAND_CHUNK_SECTION_UP_VERTEX_DATA) {
      return false;
    }
  }
  std::span<const std::byte> vertices;
  if (!applyBoundedBindings(chunk, record, state, &vertices)) return false;
  const bool usesExactDeclaration =
      expectedVertexDeclaration.objectId != 0u &&
      sameIdentity(state.vertexDeclaration, expectedVertexDeclaration) &&
      state.fvf == 0u;
  const bool usesExactFvf = expectedVertexDeclaration.objectId == 0u &&
      state.vertexDeclaration.objectId == 0u &&
      state.fvf == kXyzRhwDiffuseTex1Fvf;
  if (state.unsupportedBinding || state.texture.objectId == 0u ||
      (!usesExactDeclaration && !usesExactFvf) ||
      record.drawHeader.primitiveType != kTriangleList ||
      record.drawHeader.primitiveCount != 1u ||
      record.drawHeader.stride != kVertexStride ||
      vertices.size() != 3u * kVertexStride) return false;
  return true;
}

bool mutationCatalogueMatches(
    const PreflightPlan& plan,
    std::span<const RenderTapeProviderBlob> blobs) {
  const auto matches = [&](const RenderTapeResourceMutationHeader& mutation) {
    const auto definition = std::find_if(
        plan.definitions.begin(), plan.definitions.end(),
        [&](const auto& value) {
          return sameIdentity(value.fixed.identity, mutation.identity);
        });
    const auto* blob = findBlob(blobs, mutation.digest);
    return definition != plan.definitions.end() && blob &&
           blob->bytes.size() == mutation.byteSize;
  };
  if (!std::all_of(plan.initialMutations.begin(),
                   plan.initialMutations.end(), matches)) {
    return false;
  }
  for (std::uint32_t i = 0u; i < plan.intervalCount; ++i) {
    if (!std::all_of(plan.intervals[i].mutationsBefore.begin(),
                     plan.intervals[i].mutationsBefore.end(), matches)) {
      return false;
    }
  }
  return true;
}

constexpr std::array<std::byte, 32u> kProductionTexturedVertexDeclaration{
    std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x03}, std::byte{0x00}, std::byte{0x09}, std::byte{0x00},
    std::byte{0x00}, std::byte{0x00}, std::byte{0x10}, std::byte{0x00},
    std::byte{0x04}, std::byte{0x00}, std::byte{0x0a}, std::byte{0x00},
    std::byte{0x00}, std::byte{0x00}, std::byte{0x14}, std::byte{0x00},
    std::byte{0x01}, std::byte{0x00}, std::byte{0x05}, std::byte{0x00},
    std::byte{0xff}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x11}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
};

bool acceptedProductionTexturedVertexDeclaration(
    const Definition& definition, std::span<const RenderTapeProviderBlob> blobs) {
  const auto& fixed = definition.fixed;
  if (fixed.identity.kind != D9C_CHUNK_HANDLE_KIND_VERTEX_DECL ||
      fixed.descriptorKind != static_cast<std::uint32_t>(
                                  RenderTapeDescriptorKind::VertexDeclaration) ||
      definition.descriptor.size() != sizeof(RenderTapeVertexDeclDescriptor) ||
      fixed.payloadValidity != static_cast<std::uint32_t>(
                                  RenderTapeDigestValidity::Sha256) ||
      fixed.immutablePayloadBytes != kProductionTexturedVertexDeclaration.size() ||
      fixed.expectedContentBytes != 0u || fixed.expectedContentCount != 0u) {
    return false;
  }
  RenderTapeVertexDeclDescriptor descriptor{};
  if (!load(definition.descriptor, 0u, descriptor) ||
      descriptor.elementCount != 4u || descriptor.elementBytes !=
                                             kProductionTexturedVertexDeclaration.size()) {
    return false;
  }
  const auto* blob = findBlob(blobs, fixed.immutablePayloadDigest);
  return blob && blob->bytes.size() == kProductionTexturedVertexDeclaration.size() &&
         std::equal(kProductionTexturedVertexDeclaration.begin(),
                    kProductionTexturedVertexDeclaration.end(), blob->bytes.begin());
}

bool parseChunks(std::span<const std::byte> bytes, std::uint32_t count,
                 std::vector<Chunk>& out, std::size_t trailingBytes = 0u) {
  if (trailingBytes > bytes.size()) return false;
  std::size_t offset = 0u;
  for (std::uint32_t i = 0u; i < count; ++i) {
    D9CCommandChunkWireHeader header{};
    if (!load(bytes, offset, header) ||
        header.payloadArenaOffset > bytes.size() - offset ||
        header.payloadArenaSize >
            bytes.size() - offset - header.payloadArenaOffset) return false;
    const std::size_t totalBytes =
        static_cast<std::size_t>(header.payloadArenaOffset) +
        header.payloadArenaSize;
    out.push_back(Chunk{
        .envelope = CommandChunkEnvelope{
            .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
            .recordCount = header.recordCount,
            .handleCount = header.handleCount,
        },
        .bytes = bytes.subspan(offset, totalBytes),
    });
    offset += totalBytes;
  }
  return offset == bytes.size() - trailingBytes;
}

bool acceptedOutputSurface(const D9CSurfaceDesc& desc) {
  if (desc.resourceType != 1u || desc.width == 0u || desc.height == 0u ||
      desc.depth != 1u || desc.pool != 0u || desc.multiSampleType != 0u ||
      desc.multiSampleQuality != 0u || (desc.usage & 1u) == 0u ||
      (desc.usage & 2u) != 0u) return false;
  return desc.format == 21u || desc.format == 22u;
}

std::uint32_t mipExtent(std::uint32_t extent,
                        std::uint32_t mipLevel) noexcept {
  return mipLevel >= 32u ? 1u : std::max(1u, extent >> mipLevel);
}

bool acceptedTextureDescriptor(std::span<const std::byte> descriptor,
                               const RenderTapeTextureDescriptorV2& texture,
                               D9CSurfaceDesc& level0) {
  RenderTapeTextureDescriptorV2 canonical{};
  const auto dimension =
      static_cast<RenderTapeTextureDimension>(texture.dimension);
  const bool texture2d = dimension == RenderTapeTextureDimension::Texture2D;
  const bool cube = dimension == RenderTapeTextureDimension::Cube;
  const bool completeSeed =
      texture.initialContentDisposition == static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::CompleteSeed);
  const bool produced =
      texture.initialContentDisposition == static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::ProducedByCapturedPass);
  const bool armSnapshotCompleteSeed =
      completeSeed && renderTapeArmColorSnapshotTextureSupported(descriptor);
  const bool acceptedShape =
      (completeSeed &&
       ((texture2d && texture.mipLevelCount != 0u &&
         texture.subresourceCount == texture.mipLevelCount) ||
        armSnapshotCompleteSeed)) ||
      (produced && renderTapeProducedTexturePolicy(descriptor) ==
                       RenderTapeProducedTextureContentPolicy::ColorRenderTarget);
  if (!renderTapeLoadTextureDescriptorV2(descriptor, canonical) ||
      canonical.dimension != texture.dimension ||
      canonical.mipLevelCount != texture.mipLevelCount ||
      canonical.subresourceCount != texture.subresourceCount ||
      canonical.initialContentDisposition !=
          texture.initialContentDisposition ||
      !acceptedShape ||
      texture.reserved0 != 0u ||
      descriptor.size() !=
          sizeof(texture) +
              static_cast<std::size_t>(texture.subresourceCount) *
                  sizeof(D9CSurfaceDesc) ||
      !load(descriptor, sizeof(texture), level0) ||
      level0.resourceType != (cube ? 5u : 3u) || level0.width == 0u ||
      level0.height == 0u || level0.depth != 1u || level0.pool != 0u ||
      level0.multiSampleType != 0u || level0.multiSampleQuality != 0u ||
      ((completeSeed && !armSnapshotCompleteSeed &&
        (level0.usage & 3u) != 0u) ||
       (produced && (level0.usage & 1u) == 0u))) return false;
  const auto format = devicec::fmtFromD3D(level0.format);
  if (format == core::Format::Unknown || core::isCompressedFormat(format))
    return false;
  for (std::uint32_t subresource = 1u;
       subresource < texture.subresourceCount; ++subresource) {
    D9CSurfaceDesc desc{};
    if (!load(descriptor, sizeof(texture) +
                              subresource * sizeof(D9CSurfaceDesc), desc) ||
        desc.format != level0.format ||
        desc.resourceType != (cube ? 5u : 3u) ||
        desc.usage != level0.usage || desc.pool != level0.pool ||
        desc.multiSampleType != 0u || desc.multiSampleQuality != 0u ||
        desc.width != mipExtent(
                          level0.width,
                          cube ? subresource % texture.mipLevelCount
                               : subresource) ||
        desc.height != mipExtent(
                           level0.height,
                           cube ? subresource % texture.mipLevelCount
                                : subresource) ||
        desc.depth != 1u) return false;
  }
  return true;
}

FrameTapeReplayResult buildPlan(std::span<const std::byte> bytes,
                                std::span<const RenderTapeProviderBlob> blobs,
                                PreflightPlan* plan) {
  FrameTapeReplayResult result;
  result.conservation.inputBlobs = static_cast<std::uint32_t>(blobs.size());
  bool catalogueValid = false;
  auto catalogue = makeCatalogue(blobs, catalogueValid);
  if (!catalogueValid) {
    result.status = FrameTapeReplayStatus::InvalidBlobCatalogue;
    return result;
  }

  ImportedRenderTapeView tape;
  RenderTapeValidationScratch scratch;
  const auto validation = validateRenderTape(bytes, catalogue, &tape, scratch);
  if (!validation.valid()) {
    result.status = FrameTapeReplayStatus::InvalidTape;
    result.failedEventIndex = validation.failedEventIndex;
    return result;
  }
  result.validity.structurallyValid = true;
  result.validity.digestsValid = true;
  result.profile = tape.header.profile;
  result.intervalCount = tape.header.presentCount;
  result.coverage.eventCount = tape.header.eventCount;

  PreflightPlan candidate{.tape = tape, .catalogue = std::move(catalogue)};
  candidate.intervalCount = tape.header.presentCount;
  FrameTapeBootstrapOutputDisposition bootstrapDisposition =
      FrameTapeBootstrapOutputDisposition::Malformed;
  bool bootstrapAccepted = false;
  std::array<FrameRecordState, kRenderTapeMaxReplayIntervals> recordStates{
      FrameRecordState::ExpectClear, FrameRecordState::ExpectClear};
  std::array<bool, kRenderTapeMaxReplayIntervals> intervalStarted{};
  std::uint32_t completedIntervals = 0u;
  bool anyTexturedDraw = false;
  BoundedDrawState drawState{};
  std::vector<RenderTapeDigest> referencedBlobs;
  const auto referenceBlob = [&](const RenderTapeDigest& digest) {
    if (std::none_of(referencedBlobs.begin(), referencedBlobs.end(),
                     [&](const auto& prior) {
                       return sameDigest(prior, digest);
                     })) {
      referencedBlobs.push_back(digest);
    }
  };
  std::uint32_t currentEventIndex = 0xffffffffu;
  for (std::uint32_t index = 0u; index < tape.events.size(); ++index) {
    currentEventIndex = index;
    const auto event = tape.event(index);
    switch (static_cast<RenderTapeEventType>(event.header.type)) {
    case RenderTapeEventType::BootstrapState: {
      RenderTapeBootstrapHeader fixed{};
      if (!load(event.payload, 0u, fixed) || fixed.overlayCount != 1u ||
          fixed.gammaRampBytes != 0u ||
          !parseChunks(event.payload.subspan(sizeof(fixed)), fixed.overlayCount,
                       candidate.bootstrap, fixed.gammaRampBytes)) goto unsupported;
      result.coverage.bootstrapChunks = fixed.overlayCount;
      candidate.bootstrapProfileVersion = fixed.baselineProfileVersion;
      break;
    }
    case RenderTapeEventType::ObjectDefine: {
      RenderTapeObjectDefineHeader fixed{};
      if (completedIntervals != 0u || intervalStarted[0] ||
          !load(event.payload, 0u, fixed)) goto unsupported;
      const auto descriptor = event.payload.subspan(sizeof(fixed));
      const bool noImmutablePayload =
          fixed.payloadValidity == static_cast<std::uint32_t>(
              RenderTapeDigestValidity::NotCaptured) &&
          fixed.immutablePayloadBytes == 0u;
      if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE) {
        RenderTapeSurfaceDescriptorV2 surface{};
        if (!noImmutablePayload || descriptor.size() != sizeof(surface) ||
            !load(descriptor, 0u, surface) ||
            surface.schemaVersion != kRenderTapeSurfaceDescriptorVersion2)
          goto unsupported;
        if (surface.storage == static_cast<std::uint32_t>(
                RenderTapeSurfaceStorage::SwapchainBackbuffer)) {
          if (surface.initialContentDisposition != static_cast<std::uint32_t>(
                  RenderTapeInitialContentDisposition::ProducedPresentOutput) ||
              surface.subresource != 0u ||
              !renderTapeZeroIdentity(surface.parentTexture) ||
              fixed.expectedContentBytes != 0u ||
              fixed.expectedContentCount != 0u ||
              candidate.outputIdentity.objectId != 0u ||
              !acceptedOutputSurface(surface.surface)) goto unsupported;
          candidate.outputIdentity = fixed.identity;
          candidate.outputDesc = surface.surface;
        } else if (surface.storage == static_cast<std::uint32_t>(
                       RenderTapeSurfaceStorage::TextureSubresource)) {
          if (surface.initialContentDisposition != static_cast<std::uint32_t>(
                  RenderTapeInitialContentDisposition::Unavailable) ||
              fixed.expectedContentBytes != 0u ||
              fixed.expectedContentCount != 0u ||
              surface.parentTexture.kind != D9C_CHUNK_HANDLE_KIND_TEXTURE ||
              surface.parentTexture.generation == 0u ||
              surface.parentTexture.objectId == 0u) goto unsupported;
        } else if (surface.storage == static_cast<std::uint32_t>(
                       RenderTapeSurfaceStorage::Standalone)) {
          const auto disposition =
              static_cast<RenderTapeInitialContentDisposition>(
                  surface.initialContentDisposition);
          std::uint64_t depthBytes = 0u;
          const bool depthLayout =
              renderTapeSnapshotStandaloneD24X8Supported(surface.surface) &&
              checkedMul(surface.surface.width, surface.surface.height,
                         depthBytes) && checkedMul(depthBytes, 4u, depthBytes);
          RenderTapeD24S8SnapshotLayout depthStencilLayout{};
          const bool seededDepthStencil =
              disposition == RenderTapeInitialContentDisposition::
                                 CompleteDepthStencilFloat32UInt8V1 &&
              renderTapeDeriveD24S8SnapshotLayout(surface.surface, depthStencilLayout) &&
              fixed.expectedContentBytes == depthStencilLayout.byteCount &&
              fixed.expectedContentCount == 1u;
          std::uint64_t colorBytes = 0u;
          const bool colorLayout =
              renderTapeArmColorSnapshotStandaloneSurfaceSupported(
                  surface.surface) &&
              checkedMul(surface.surface.width, surface.surface.height,
                         colorBytes) && checkedMul(colorBytes, 4u, colorBytes);
          const bool produced =
              disposition ==
                  RenderTapeInitialContentDisposition::ProducedByCapturedPass &&
              fixed.expectedContentBytes == 0u &&
              fixed.expectedContentCount == 0u &&
              renderTapeProducedStandaloneSurfaceSupported(surface.surface);
          const bool seededDepth =
              disposition == RenderTapeInitialContentDisposition::
                                 CompleteDepthFloat32V1 &&
              depthLayout && fixed.expectedContentBytes == depthBytes &&
              fixed.expectedContentCount == 1u;
          const bool seededColor =
              disposition ==
                  RenderTapeInitialContentDisposition::CompleteSeed &&
              colorLayout && fixed.expectedContentBytes == colorBytes &&
              fixed.expectedContentCount == 1u;
          if (surface.subresource != 0u ||
              !renderTapeZeroIdentity(surface.parentTexture) ||
              (!produced && !seededDepth && !seededDepthStencil && !seededColor)) goto unsupported;
          if (produced) {
            candidate.producedStandaloneSurfaces.push_back(
                {fixed.identity, surface.surface});
          } else if (seededDepth || seededDepthStencil) {
            candidate.seededStandaloneDepthSurfaces.push_back(
                {fixed.identity, surface.surface});
          } else {
            candidate.seededStandaloneColorSurfaces.push_back(
                {fixed.identity, surface.surface});
          }
        } else {
          goto unsupported;
        }
      } else if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER) {
        D9CBufferDesc buffer{};
        if (!noImmutablePayload || descriptor.size() != sizeof(buffer) ||
            !load(descriptor, 0u, buffer) ||
            buffer.size == 0u || fixed.expectedContentCount != 1u ||
            fixed.expectedContentBytes != buffer.size) goto unsupported;
      } else if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
        RenderTapeTextureDescriptorV2 texture{};
        D9CSurfaceDesc level0{};
        bool producedByCapturedPass = false;
        bool expectedContentShape = false;
        if (!noImmutablePayload || descriptor.size() < sizeof(texture) ||
            !load(descriptor, 0u, texture) ||
            !acceptedTextureDescriptor(descriptor, texture, level0))
          goto unsupported;
        producedByCapturedPass =
            texture.initialContentDisposition == static_cast<std::uint32_t>(
                RenderTapeInitialContentDisposition::ProducedByCapturedPass);
        expectedContentShape = producedByCapturedPass
            ? fixed.expectedContentBytes == 0u &&
                  fixed.expectedContentCount == 0u
            : fixed.expectedContentBytes != 0u &&
                  fixed.expectedContentCount == texture.subresourceCount;
        if (!expectedContentShape) goto unsupported;
        if (!producedByCapturedPass &&
            renderTapeArmColorSnapshotTextureSupported(descriptor)) {
          candidate.seededArmColorTextures.push_back(fixed.identity);
        } else {
          if (candidate.textureIdentity.objectId != 0u) goto unsupported;
          candidate.textureIdentity = fixed.identity;
          candidate.textureDesc = texture;
          candidate.textureLevel0 = level0;
          candidate.textureProducedByCapturedPass = producedByCapturedPass;
        }
      } else if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_VERTEX_DECL) {
        const Definition definition{.fixed = fixed, .descriptor = descriptor};
        if (candidate.vertexDeclarationIdentity.objectId != 0u ||
            !acceptedProductionTexturedVertexDeclaration(definition, blobs)) {
          goto unsupported;
        }
        candidate.vertexDeclarationIdentity = fixed.identity;
        referenceBlob(fixed.immutablePayloadDigest);
      } else {
        goto unsupported;
      }
      candidate.definitions.push_back({.fixed = fixed, .descriptor = descriptor});
      ++result.coverage.objectDefinitions;
      break;
    }
    case RenderTapeEventType::ResourceMutation: {
      RenderTapeResourceMutationHeader fixed{};
      if (!load(event.payload, 0u, fixed) || fixed.byteOffset != 0u ||
          (fixed.kind != static_cast<std::uint32_t>(RenderTapeMutationKind::Upload) &&
           fixed.kind != static_cast<std::uint32_t>(RenderTapeMutationKind::CpuUnlock)) ||
          !findBlob(blobs, fixed.digest)) goto unsupported;
      if (completedIntervals == 0u && !intervalStarted[0]) {
        candidate.initialMutations.push_back(fixed);
      } else if (tape.header.profile == kRenderTapeProfileSequence &&
                 completedIntervals == 1u && !intervalStarted[1]) {
        candidate.intervals[1].mutationsBefore.push_back(fixed);
      } else {
        goto unsupported;
      }
      ++result.coverage.seedMutations;
      referenceBlob(fixed.digest);
      break;
    }
    case RenderTapeEventType::CommandChunk: {
      RenderTapeCommandChunkHeader fixed{};
      if (completedIntervals >= candidate.intervalCount ||
          completedIntervals >= kRenderTapeMaxReplayIntervals ||
          (completedIntervals == 1u &&
           candidate.intervals[1].mutationsBefore.size() != 1u) ||
          !load(event.payload, 0u, fixed)) goto unsupported;
      const auto intervalIndex = completedIntervals;
      intervalStarted[intervalIndex] = true;
      candidate.intervals[intervalIndex].frame.push_back(Chunk{
          .envelope = CommandChunkEnvelope{fixed.wireVersion, fixed.recordCount,
                                            fixed.handleCount},
          .bytes = event.payload.subspan(sizeof(fixed)),
      });
      const auto& frame = candidate.intervals[intervalIndex].frame.back();
      ImportedChunkView chunk;
      if (!importPrevalidatedCommandChunk(frame.bytes, frame.envelope, chunk) ||
          chunk.records.empty()) goto unsupported;
      for (std::size_t recordIndex = 0u; recordIndex < chunk.records.size();
           ++recordIndex) {
        const auto record = chunk.record(recordIndex);
        ++result.coverage.commandRecords;
        auto& recordState = recordStates[intervalIndex];
        if (recordState == FrameRecordState::ExpectClear &&
            candidate.textureProducedByCapturedPass &&
            record.header.type == D9C_COMMAND_RECORD_APPLY_STATE) {
          if (!applyBoundedBindings(chunk, record, drawState)) goto unsupported;
        } else if (recordState == FrameRecordState::ExpectClear &&
                   record.header.type == D9C_COMMAND_RECORD_CLEAR) {
          D9CCommandChunkWireClear clear{};
          if (!load(record.payload, 0u, clear) || (clear.flags & 1u) == 0u ||
              (clear.flags & ~7u) != 0u || clear.rectCount != 0u)
            goto unsupported;
          ++result.coverage.clearRecords;
          recordState = FrameRecordState::ExpectDrawOrPresent;
        } else if (recordState == FrameRecordState::ExpectDrawOrPresent &&
                   record.header.type ==
                       D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP) {
          candidate.intervals[intervalIndex].sawTexturedDraw = true;
          ++result.coverage.drawPrimitiveUpRecords;
          recordState = FrameRecordState::ExpectPresent;
        } else if ((recordState == FrameRecordState::ExpectDrawOrPresent ||
                    recordState == FrameRecordState::ExpectPresent) &&
                   record.header.type == D9C_COMMAND_RECORD_PRESENT) {
          D9CCommandChunkWirePresent present{};
          if (!load(record.payload, 0u, present) || present.flags != 0u ||
              present.hasSrc != present.hasDst ||
              (present.hasSrc != 0u &&
               (!fullRect(present.src, candidate.outputDesc.width,
                          candidate.outputDesc.height) ||
                !fullRect(present.dst, candidate.outputDesc.width,
                          candidate.outputDesc.height)))) goto unsupported;
          if (record.header.handleCount == 1u) {
            if (present.sourceHandleIndex >= chunk.handles.size())
              goto unsupported;
            const auto& source = chunk.handles[present.sourceHandleIndex];
            const D9CWireObjectIdentity identity{
                .kind = source.kind,
                .generation = source.generation,
                .objectId = source.objectId,
            };
            if (!sameIdentity(identity, candidate.outputIdentity))
              goto unsupported;
            ++result.coverage.presentSourceMappings;
          }
          ++result.coverage.presentRecords;
          recordState = FrameRecordState::Complete;
        } else {
          goto unsupported;
        }
      }
      ++result.coverage.commandChunks;
      break;
    }
    case RenderTapeEventType::PresentComplete: {
      RenderTapePresentCompleteHeader fixed{};
      RenderTapeOracleAttachment oracle{};
      if (completedIntervals >= candidate.intervalCount ||
          !intervalStarted[completedIntervals] ||
          recordStates[completedIntervals] != FrameRecordState::Complete ||
          !load(event.payload, 0u, fixed) || fixed.oracleCount != 1u ||
          !load(event.payload, sizeof(fixed), oracle) ||
          !sameIdentity(oracle.identity, candidate.outputIdentity)) goto unsupported;
      auto& interval = candidate.intervals[completedIntervals];
      interval.presentOrdinal = fixed.presentOrdinal;
      interval.completionOrdinal = fixed.completionOrdinal;
      interval.expectedDigestValidity =
          static_cast<RenderTapeDigestValidity>(fixed.digestValidity);
      interval.expectedDigest = fixed.expectedDigest;
      result.intervals[completedIntervals].presentOrdinal = fixed.presentOrdinal;
      result.intervals[completedIntervals].completionOrdinal =
          fixed.completionOrdinal;
      result.intervals[completedIntervals].validity.expectedDigestCaptured =
          interval.expectedDigestValidity == RenderTapeDigestValidity::Sha256;
      ++completedIntervals;
      ++result.coverage.presentOutputs;
      break;
    }
    case RenderTapeEventType::ObjectDestroy:
    case RenderTapeEventType::OrderedControl:
      // The compact interval plan has no ordered-state stream. Force the
      // general provider, which preserves and applies GammaRampSet.
      goto unsupported;
    }
  }
  bootstrapDisposition = candidate.bootstrap.size() == 1u
      ? classifyFrameTapeBootstrapOutput(
            candidate.bootstrap.front().bytes,
            candidate.bootstrap.front().envelope,
            candidate.outputIdentity, candidate.bootstrapProfileVersion)
      : FrameTapeBootstrapOutputDisposition::Malformed;
  bootstrapAccepted =
      bootstrapDisposition == FrameTapeBootstrapOutputDisposition::ImplicitDefault ||
      bootstrapDisposition == FrameTapeBootstrapOutputDisposition::ExplicitExact;
  for (const auto& chunkBytes : candidate.bootstrap) {
    ImportedChunkView chunk;
    if (!importPrevalidatedCommandChunk(chunkBytes.bytes, chunkBytes.envelope,
                                        chunk)) goto unsupported;
    for (std::size_t recordIndex = 0u; recordIndex < chunk.records.size();
         ++recordIndex) {
      if (!applyBoundedBindings(chunk, chunk.record(recordIndex), drawState))
        goto unsupported;
    }
  }
  // Apply the effective draw state in exact interval order. Mutation events do
  // not alter bindings, so the state shadow intentionally survives Present 1.
  drawState = {};
  for (const auto& chunkBytes : candidate.bootstrap) {
    ImportedChunkView chunk;
    importPrevalidatedCommandChunk(chunkBytes.bytes, chunkBytes.envelope, chunk);
    for (std::size_t recordIndex = 0u; recordIndex < chunk.records.size();
         ++recordIndex) {
      if (!applyBoundedBindings(chunk, chunk.record(recordIndex), drawState))
        goto unsupported;
    }
  }
  for (std::uint32_t intervalIndex = 0u;
       intervalIndex < candidate.intervalCount; ++intervalIndex) {
    for (const auto& chunkBytes : candidate.intervals[intervalIndex].frame) {
      ImportedChunkView chunk;
      importPrevalidatedCommandChunk(chunkBytes.bytes, chunkBytes.envelope, chunk);
      for (std::size_t recordIndex = 0u; recordIndex < chunk.records.size();
           ++recordIndex) {
        const auto record = chunk.record(recordIndex);
        if (record.header.type == D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP) {
          if (!acceptedTexturedDraw(chunk, record, drawState,
                                    candidate.vertexDeclarationIdentity)) {
            goto unsupported;
          }
          const auto existing = std::find_if(
              candidate.drawTextureIdentities.begin(),
              candidate.drawTextureIdentities.end(), [&](const auto& value) {
                return sameIdentity(value, drawState.texture);
              });
          if (existing == candidate.drawTextureIdentities.end()) {
            candidate.drawTextureIdentities.push_back(drawState.texture);
          }
        } else if (record.sparseState() &&
                   !applyBoundedBindings(chunk, record, drawState)) {
          goto unsupported;
        }
      }
    }
  }
  if (completedIntervals != candidate.intervalCount ||
      candidate.outputIdentity.objectId == 0u ||
      !bootstrapAccepted ||
      referencedBlobs.size() != blobs.size() ||
      !mutationCatalogueMatches(candidate, blobs)) goto unsupported;
  anyTexturedDraw = std::any_of(
      candidate.intervals.begin(),
      candidate.intervals.begin() + candidate.intervalCount,
      [](const auto& interval) { return interval.sawTexturedDraw; });
  if (anyTexturedDraw) {
    if (candidate.drawTextureIdentities.size() != 1u) goto unsupported;
    const auto& drawTexture = candidate.drawTextureIdentities.front();
    if (candidate.textureIdentity.objectId == 0u) {
      const auto seeded = std::find_if(
          candidate.seededArmColorTextures.begin(),
          candidate.seededArmColorTextures.end(), [&](const auto& value) {
            return sameIdentity(value, drawTexture);
          });
      const auto duplicate = seeded == candidate.seededArmColorTextures.end()
          ? seeded
          : std::find_if(std::next(seeded),
                         candidate.seededArmColorTextures.end(),
                         [&](const auto& value) {
                           return sameIdentity(value, drawTexture);
                         });
      const auto definition = std::find_if(
          candidate.definitions.begin(), candidate.definitions.end(),
          [&](const auto& value) {
            return sameIdentity(value.fixed.identity, drawTexture);
          });
      if (seeded == candidate.seededArmColorTextures.end() ||
          duplicate != candidate.seededArmColorTextures.end() ||
          definition == candidate.definitions.end() ||
          !load(definition->descriptor, 0u, candidate.textureDesc) ||
          !load(definition->descriptor, sizeof(candidate.textureDesc),
                candidate.textureLevel0) ||
          !renderTapeArmColorSnapshotTextureSupported(
              definition->descriptor)) {
        goto unsupported;
      }
      candidate.textureIdentity = drawTexture;
      candidate.textureProducedByCapturedPass = false;
    } else if (!sameIdentity(candidate.textureIdentity, drawTexture)) {
      goto unsupported;
    }
  }
  if (!anyTexturedDraw && !candidate.textureProducedByCapturedPass &&
      (candidate.textureIdentity.objectId != 0u ||
       candidate.vertexDeclarationIdentity.objectId != 0u))
    goto unsupported;
  if (tape.header.profile == kRenderTapeProfileSequence && !anyTexturedDraw)
    goto unsupported;
  if (anyTexturedDraw || candidate.textureProducedByCapturedPass) {
    std::uint64_t texturePixels = 0u;
    std::uint64_t tightTextureBytes = 0u;
    if (!checkedMul(candidate.textureLevel0.width,
                    candidate.textureLevel0.height, texturePixels) ||
        !checkedMul(texturePixels, 4u, tightTextureBytes)) goto unsupported;
    const auto textureDefinition = std::find_if(
        candidate.definitions.begin(), candidate.definitions.end(),
        [&](const auto& definition) {
          return sameIdentity(definition.fixed.identity,
                              candidate.textureIdentity);
        });
    const bool selectedArmSnapshot = std::any_of(
        candidate.seededArmColorTextures.begin(),
        candidate.seededArmColorTextures.end(), [&](const auto& value) {
          return sameIdentity(value, candidate.textureIdentity);
        });
    const bool productionDeclaration =
        candidate.vertexDeclarationIdentity.objectId != 0u;
    const std::size_t expectedDefinitions =
        (productionDeclaration ? 3u : 2u) +
        candidate.seededArmColorTextures.size() -
        (selectedArmSnapshot ? 1u : 0u) +
        candidate.producedStandaloneSurfaces.size() +
        candidate.seededStandaloneDepthSurfaces.size() +
        candidate.seededStandaloneColorSurfaces.size();
    const std::size_t expectedSurfaceAliases = std::count_if(
        candidate.definitions.begin(), candidate.definitions.end(),
        [&](const auto& definition) {
          if (definition.fixed.identity.kind !=
              D9C_CHUNK_HANDLE_KIND_SURFACE)
            return false;
          RenderTapeSurfaceDescriptorV2 surface{};
          return load(definition.descriptor, 0u, surface) &&
                 surface.storage == static_cast<std::uint32_t>(
                     RenderTapeSurfaceStorage::TextureSubresource);
        });
    const bool aliasesNameCandidateTexture = std::all_of(
        candidate.definitions.begin(), candidate.definitions.end(),
        [&](const auto& definition) {
          if (definition.fixed.identity.kind !=
              D9C_CHUNK_HANDLE_KIND_SURFACE)
            return true;
          RenderTapeSurfaceDescriptorV2 surface{};
          if (!load(definition.descriptor, 0u, surface) ||
              surface.storage != static_cast<std::uint32_t>(
                  RenderTapeSurfaceStorage::TextureSubresource))
            return true;
          const bool parentAllowed =
              sameIdentity(surface.parentTexture,
                           candidate.textureIdentity) ||
              std::any_of(candidate.seededArmColorTextures.begin(),
                          candidate.seededArmColorTextures.end(),
                          [&](const auto& value) {
                            return sameIdentity(value,
                                                surface.parentTexture);
                          });
          const auto parent = std::find_if(
              candidate.definitions.begin(), candidate.definitions.end(),
              [&](const auto& value) {
                return sameIdentity(value.fixed.identity,
                                    surface.parentTexture);
              });
          return parentAllowed && parent != candidate.definitions.end() &&
                 renderTapeSurfaceAliasMatchesTextureSubresource(
                     parent->descriptor, parent->fixed.identity, surface);
        });
    const bool sequence = tape.header.profile == kRenderTapeProfileSequence;
    const auto mutationShapeMatches = [&](const auto& mutation) {
      return textureDefinition != candidate.definitions.end() &&
             sameIdentity(mutation.identity, candidate.textureIdentity) &&
             mutation.subresource == 0u && mutation.byteOffset == 0u &&
             mutation.byteSize == textureDefinition->fixed.expectedContentBytes;
    };
    const auto depthMutationShapeMatches = [&](const auto& mutation) {
      const auto seeded = std::find_if(
          candidate.seededStandaloneDepthSurfaces.begin(),
          candidate.seededStandaloneDepthSurfaces.end(),
          [&](const auto& value) {
            return sameIdentity(value.first, mutation.identity);
          });
      if (seeded == candidate.seededStandaloneDepthSurfaces.end() ||
          mutation.subresource != 0u || mutation.byteOffset != 0u) {
        return false;
      }
      const auto definition = std::find_if(
          candidate.definitions.begin(), candidate.definitions.end(),
          [&](const auto& value) {
            return sameIdentity(value.fixed.identity, mutation.identity);
          });
      return definition != candidate.definitions.end() &&
             mutation.byteSize == definition->fixed.expectedContentBytes;
    };
    const auto colorSurfaceMutationShapeMatches = [&](const auto& mutation) {
      const auto seeded = std::find_if(
          candidate.seededStandaloneColorSurfaces.begin(),
          candidate.seededStandaloneColorSurfaces.end(),
          [&](const auto& value) {
            return sameIdentity(value.first, mutation.identity);
          });
      if (seeded == candidate.seededStandaloneColorSurfaces.end() ||
          mutation.subresource != 0u || mutation.byteOffset != 0u) {
        return false;
      }
      const auto definition = std::find_if(
          candidate.definitions.begin(), candidate.definitions.end(),
          [&](const auto& value) {
            return sameIdentity(value.fixed.identity, mutation.identity);
          });
      return definition != candidate.definitions.end() &&
             mutation.byteSize == definition->fixed.expectedContentBytes;
    };
    std::size_t armTextureMutationCount = 0u;
    bool armTextureMutationSetMatches = true;
    for (const auto& identity : candidate.seededArmColorTextures) {
      const auto definition = std::find_if(
          candidate.definitions.begin(), candidate.definitions.end(),
          [&](const auto& value) {
            return sameIdentity(value.fixed.identity, identity);
          });
      RenderTapeTextureDescriptorV2 descriptor{};
      if (definition == candidate.definitions.end() ||
          !load(definition->descriptor, 0u, descriptor) ||
          !renderTapeArmColorSnapshotTextureSupported(
              definition->descriptor)) {
        armTextureMutationSetMatches = false;
        break;
      }
      armTextureMutationCount += descriptor.subresourceCount;
      for (std::uint32_t subresource = 0u;
           subresource < descriptor.subresourceCount; ++subresource) {
        D9CSurfaceDesc surface{};
        const auto expected = renderTapeTextureSubresourceDescriptor(
                                  definition->descriptor, subresource,
                                  surface)
            ? renderTapeDeriveExpectedSurfaceContent(surface)
            : RenderTapeExpectedContentContract{};
        const auto matches = std::count_if(
            candidate.initialMutations.begin(),
            candidate.initialMutations.end(), [&](const auto& mutation) {
              return sameIdentity(mutation.identity, identity) &&
                     mutation.kind == static_cast<std::uint32_t>(
                         RenderTapeMutationKind::Upload) &&
                     mutation.subresource == subresource &&
                     mutation.byteOffset == 0u &&
                     expected.status ==
                         RenderTapeExpectedContentStatus::Accepted &&
                     mutation.byteSize == expected.bytes;
            });
        if (matches != 1) armTextureMutationSetMatches = false;
      }
    }
    const std::size_t textureMutationCount =
        candidate.textureProducedByCapturedPass || selectedArmSnapshot
            ? 0u
            : 1u;
    const bool initialMutationSetMatches =
        candidate.initialMutations.size() ==
            textureMutationCount +
                armTextureMutationCount +
                candidate.seededStandaloneDepthSurfaces.size() +
                candidate.seededStandaloneColorSurfaces.size() &&
        armTextureMutationSetMatches &&
        static_cast<std::size_t>(std::count_if(
            candidate.initialMutations.begin(),
            candidate.initialMutations.end(), mutationShapeMatches)) ==
            textureMutationCount &&
        static_cast<std::size_t>(std::count_if(
            candidate.initialMutations.begin(),
            candidate.initialMutations.end(), depthMutationShapeMatches)) ==
            candidate.seededStandaloneDepthSurfaces.size() &&
        static_cast<std::size_t>(std::count_if(
            candidate.initialMutations.begin(),
            candidate.initialMutations.end(),
            colorSurfaceMutationShapeMatches)) ==
            candidate.seededStandaloneColorSurfaces.size();
    if (candidate.definitions.size() !=
            expectedDefinitions + expectedSurfaceAliases ||
        candidate.textureIdentity.objectId == 0u ||
        !aliasesNameCandidateTexture ||
        textureDefinition == candidate.definitions.end() ||
        (!candidate.textureProducedByCapturedPass &&
         !sameIdentity(drawState.texture, candidate.textureIdentity)) ||
        candidate.textureDesc.mipLevelCount != 1u ||
        (sequence && selectedArmSnapshot) ||
        ((!candidate.textureProducedByCapturedPass && !selectedArmSnapshot &&
          candidate.textureLevel0.format != 21u) ||
         (selectedArmSnapshot &&
          !renderTapeArmColorSnapshotTextureSupported(
              textureDefinition->descriptor)) ||
         (candidate.textureProducedByCapturedPass &&
          candidate.textureLevel0.format != 22u &&
          candidate.textureLevel0.format != 114u)) ||
        (!candidate.textureProducedByCapturedPass &&
         (textureDefinition->fixed.expectedContentBytes < tightTextureBytes ||
          textureDefinition->fixed.expectedContentBytes %
                  candidate.textureLevel0.height != 0u ||
          !initialMutationSetMatches ||
          textureDefinition->fixed.expectedContentCount !=
              (selectedArmSnapshot
                   ? candidate.textureDesc.subresourceCount
                   : 1u))) ||
        (candidate.textureProducedByCapturedPass &&
         (!initialMutationSetMatches ||
          textureDefinition->fixed.expectedContentCount != 0u))) {
      goto unsupported;
    }
    if (sequence) {
      const auto initialTextureMutation = std::find_if(
          candidate.initialMutations.begin(), candidate.initialMutations.end(),
          mutationShapeMatches);
      if (!candidate.intervals[0].sawTexturedDraw ||
          !candidate.intervals[1].sawTexturedDraw ||
          !candidate.intervals[0].mutationsBefore.empty() ||
          candidate.intervals[1].mutationsBefore.size() != 1u ||
          initialTextureMutation == candidate.initialMutations.end() ||
          !mutationShapeMatches(candidate.intervals[1].mutationsBefore[0]) ||
          sameDigest(initialTextureMutation->digest,
                     candidate.intervals[1].mutationsBefore[0].digest) ||
          candidate.intervals[0].expectedDigestValidity !=
              RenderTapeDigestValidity::Sha256 ||
          candidate.intervals[1].expectedDigestValidity !=
              RenderTapeDigestValidity::Sha256 ||
          sameDigest(candidate.intervals[0].expectedDigest,
                     candidate.intervals[1].expectedDigest) ||
          candidate.intervals[0].completionOrdinal >=
              candidate.intervals[1].completionOrdinal) {
        goto unsupported;
      }
    }
  }
  result.conservation.referencedBlobs =
      static_cast<std::uint32_t>(referencedBlobs.size());
  result.requirements = FrameTapeReplayRequirements{
      .outputWidth = candidate.outputDesc.width,
      .outputHeight = candidate.outputDesc.height,
      .outputFormat = candidate.outputDesc.format,
  };
  result.conservation.presentOrdinal =
      candidate.intervals[candidate.intervalCount - 1u].presentOrdinal;
  result.conservation.completionOrdinal =
      candidate.intervals[candidate.intervalCount - 1u].completionOrdinal;
  result.validity.expectedDigestCaptured = std::all_of(
      candidate.intervals.begin(),
      candidate.intervals.begin() + candidate.intervalCount,
      [](const auto& interval) {
        return interval.expectedDigestValidity == RenderTapeDigestValidity::Sha256;
      });
  result.status = FrameTapeReplayStatus::Complete;
  if (plan) *plan = std::move(candidate);
  return result;

unsupported:
  result.status = FrameTapeReplayStatus::UnsupportedGrammar;
  result.failedEventIndex = currentEventIndex;
  return result;
}

bool generalMutationShapeValid(
    const RenderTapeResourceMutationHeader& mutation,
    const Definition& definition,
    std::uint64_t blobBytes) noexcept {
  if (mutation.byteSize == 0u || blobBytes != mutation.byteSize ||
      (mutation.kind != static_cast<std::uint32_t>(RenderTapeMutationKind::Upload) &&
       mutation.kind != static_cast<std::uint32_t>(RenderTapeMutationKind::CpuUnlock))) {
    return false;
  }

  const auto kind = mutation.identity.kind;
  if (kind == D9C_CHUNK_HANDLE_KIND_BUFFER) {
    D9CBufferDesc buffer{};
    std::uint64_t end = 0u;
    return definition.descriptor.size() == sizeof(buffer) &&
           load(definition.descriptor, 0u, buffer) && buffer.size != 0u &&
           checkedAdd(mutation.byteOffset, mutation.byteSize, end) &&
           end <= buffer.size;
  }

  if (mutation.byteOffset != 0u) return false;
  if (kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
    D9CSurfaceDesc surface{};
    if (!renderTapeTextureSubresourceDescriptor(
            definition.descriptor, mutation.subresource, surface)) {
      return false;
    }
    const auto expected = renderTapeDeriveExpectedTextureSubresourceContent(surface);
    return expected.status == RenderTapeExpectedContentStatus::Accepted &&
           mutation.byteSize == expected.bytes;
  }

  if (kind != D9C_CHUNK_HANDLE_KIND_SURFACE || mutation.subresource != 0u)
    return false;
  RenderTapeSurfaceDescriptorV2 surface{};
  if (!renderTapeLoadSurfaceDescriptorV2(definition.descriptor, surface) ||
      (surface.storage != static_cast<std::uint32_t>(
           RenderTapeSurfaceStorage::Standalone) &&
       surface.storage != static_cast<std::uint32_t>(
           RenderTapeSurfaceStorage::SwapchainBackbuffer))) {
    return false;
  }
  const auto disposition = static_cast<RenderTapeInitialContentDisposition>(
      surface.initialContentDisposition);
  if (disposition == RenderTapeInitialContentDisposition::CompleteDepthFloat32V1) {
    std::uint64_t rowBytes = 0u;
    std::uint64_t expectedBytes = 0u;
    return renderTapeSnapshotStandaloneD24X8Supported(surface.surface) &&
           checkedMul(surface.surface.width, sizeof(float), rowBytes) &&
           checkedMul(rowBytes, surface.surface.height, expectedBytes) &&
           mutation.byteSize == expectedBytes;
  }
  if (disposition == RenderTapeInitialContentDisposition::
                         CompleteDepthStencilFloat32UInt8V1) {
    RenderTapeD24S8SnapshotLayout layout{};
    return surface.storage == static_cast<std::uint32_t>(
                                  RenderTapeSurfaceStorage::Standalone) &&
           renderTapeDeriveD24S8SnapshotLayout(surface.surface, layout) &&
           mutation.byteSize == layout.byteCount;
  }
  if (disposition != RenderTapeInitialContentDisposition::CompleteSeed ||
      (static_cast<RenderTapeSurfaceStorage>(surface.storage) ==
           RenderTapeSurfaceStorage::SwapchainBackbuffer &&
       !renderTapeArmColorSnapshotSwapchainSurfaceSupported(surface.surface)))
    return false;
  const auto expected = renderTapeDeriveExpectedSurfaceContent(surface.surface);
  return expected.status == RenderTapeExpectedContentStatus::Accepted &&
         mutation.byteSize == expected.bytes;
}

bool generalRecordTypeSupported(std::uint32_t type) noexcept {
  switch (type) {
  case D9C_COMMAND_RECORD_DRAW_PRIMITIVE:
  case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE:
  case D9C_COMMAND_RECORD_SET_VS_CONST_F:
  case D9C_COMMAND_RECORD_SET_VS_CONST_I:
  case D9C_COMMAND_RECORD_SET_VS_CONST_B:
  case D9C_COMMAND_RECORD_SET_PS_CONST_F:
  case D9C_COMMAND_RECORD_SET_PS_CONST_I:
  case D9C_COMMAND_RECORD_SET_PS_CONST_B:
  case D9C_COMMAND_RECORD_CLEAR:
  case D9C_COMMAND_RECORD_PRESENT:
  case D9C_COMMAND_RECORD_APPLY_STATE:
  case D9C_COMMAND_RECORD_GENERATE_MIPMAPS:
  case D9C_COMMAND_RECORD_QUERY_ISSUE:
    return true;
  default:
    return false;
  }
}

const RenderTapeBlob* findCatalogueBlob(
    const RenderTapeBlobCatalogue& catalogue, const RenderTapeDigest& digest) {
  const auto it = std::find_if(catalogue.blobs.begin(), catalogue.blobs.end(),
      [&](const auto& blob) { return sameDigest(blob.digest, digest); });
  return it == catalogue.blobs.end() ? nullptr : &*it;
}

// One grammar parser serves both byte-authenticated historical preflight and
// immutable v3 metadata rebinding. Semantic admission uses blob sizes only;
// callers must provide the already authenticated catalogue, never fake byte
// spans or a second implementation of the General event grammar.
FrameTapeReplayResult buildGeneralPlanFromCatalogue(
    std::span<const std::byte> bytes,
    RenderTapeBlobCatalogue catalogue,
    GeneralPreflightPlan* plan) {
  FrameTapeReplayResult result;
  result.conservation.inputBlobs = static_cast<std::uint32_t>(catalogue.blobs.size());

  ImportedRenderTapeView tape;
  RenderTapeValidationScratch scratch;
  const auto validation = validateRenderTape(bytes, catalogue, &tape, scratch);
  if (!validation.valid()) {
    result.status = FrameTapeReplayStatus::InvalidTape;
    result.failedEventIndex = validation.failedEventIndex;
    return result;
  }
  result.validity.structurallyValid = true;
  result.validity.digestsValid = true;
  result.profile = tape.header.profile;
  result.intervalCount = tape.header.presentCount;
  result.coverage.eventCount = tape.header.eventCount;

  GeneralPreflightPlan candidate{
      .tape = tape,
      .catalogue = std::move(catalogue),
  };
  std::vector<RenderTapeDigest> referencedBlobs;
  const auto referenceBlob = [&](const RenderTapeDigest& digest) {
    if (std::none_of(referencedBlobs.begin(), referencedBlobs.end(),
                     [&](const auto& prior) {
                       return sameDigest(prior, digest);
                     })) {
      referencedBlobs.push_back(digest);
    }
  };
  bool sawShader = false;
  bool sawBuffer = false;
  bool sawIndexedDraw = false;
  bool sawPresentComplete = false;
  std::uint32_t currentEventIndex = 0xffffffffu;
  std::size_t firstCommand = tape.events.size();
  // Storage identifies the primary backing, not the logical Present owner.
  // Distinct generation-qualified wrappers may explicitly alias that backing.
  std::vector<D9CWireObjectIdentity> liveBackbufferIdentities;

  for (std::uint32_t index = 0u; index < tape.events.size(); ++index) {
    currentEventIndex = index;
    const auto event = tape.event(index);
    switch (static_cast<RenderTapeEventType>(event.header.type)) {
    case RenderTapeEventType::BootstrapState: {
      RenderTapeBootstrapHeader fixed{};
      std::vector<Chunk> chunks;
      if (!load(event.payload, 0u, fixed) || fixed.overlayCount == 0u ||
          !parseChunks(event.payload.subspan(sizeof(fixed)), fixed.overlayCount,
                       chunks, fixed.gammaRampBytes)) {
        goto unsupported;
      }
      for (const auto& overlay : chunks) {
        ImportedChunkView imported;
        if (!importPrevalidatedCommandChunk(overlay.bytes, overlay.envelope,
                                            imported)) {
          goto unsupported;
        }
        for (const auto& handle : imported.handles) {
          if (handle.kind == 0u || handle.objectId == 0u) continue;
          candidate.bootstrapIdentities.push_back(D9CWireObjectIdentity{
              .kind = handle.kind,
              .generation = handle.generation,
              .objectId = handle.objectId,
          });
        }
      }
      const auto bootstrapIndex = static_cast<std::uint32_t>(
          candidate.bootstrapChunks.size());
      candidate.bootstrapChunks.insert(candidate.bootstrapChunks.end(),
                                       chunks.begin(), chunks.end());
      candidate.bootstrapGamma.emplace_back();
      if (fixed.gammaRampBytes != 0u) {
        const auto gamma = event.payload.subspan(
            event.payload.size() - fixed.gammaRampBytes, fixed.gammaRampBytes);
        candidate.bootstrapGamma.back().assign(gamma.begin(), gamma.end());
      }
      candidate.events.push_back(GeneralPreflightPlan::ReplayEvent{
          .type = RenderTapeEventType::BootstrapState,
          .eventIndex = index,
          .index = bootstrapIndex,
          .count = static_cast<std::uint32_t>(chunks.size()),
          .aux = static_cast<std::uint32_t>(candidate.bootstrapGamma.size() - 1u),
      });
      result.coverage.bootstrapChunks += fixed.overlayCount;
      break;
    }
    case RenderTapeEventType::ObjectDefine: {
      RenderTapeObjectDefineHeader fixed{};
      if (!load(event.payload, 0u, fixed)) goto unsupported;
      const auto descriptor = event.payload.subspan(sizeof(fixed));
      if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
        RenderTapeTextureDescriptorV2 texture{};
        D9CSurfaceDesc level0{};
        if (!renderTapeLoadTextureDescriptorV2(descriptor, texture) ||
            !renderTapeTextureSubresourceDescriptor(descriptor, 0u, level0) ||
            (texture.dimension != static_cast<std::uint32_t>(
                                      RenderTapeTextureDimension::Texture2D) &&
             texture.dimension != static_cast<std::uint32_t>(
                                      RenderTapeTextureDimension::Cube) &&
             texture.dimension != static_cast<std::uint32_t>(
                                      RenderTapeTextureDimension::Volume)) ||
            (static_cast<RenderTapeInitialContentDisposition>(
                 texture.initialContentDisposition) !=
                 RenderTapeInitialContentDisposition::CompleteSeed &&
             static_cast<RenderTapeInitialContentDisposition>(
                 texture.initialContentDisposition) !=
                 RenderTapeInitialContentDisposition::ProducedByCapturedPass &&
            !renderTapeNullRtDescriptorOnly(fixed.identity.kind, descriptor))) {
          goto unsupported;
        }
        if (texture.initialContentDisposition == static_cast<std::uint32_t>(
                RenderTapeInitialContentDisposition::ProducedByCapturedPass) &&
            renderTapeProducedTexturePolicy(descriptor) ==
                RenderTapeProducedTextureContentPolicy::Unsupported)
          goto unsupported;
        // Metal does not admit BCn 3D backing. Its canonical row/slice layout
        // remains supported by CPU capture/replay, not by this GPU provider.
        if (texture.dimension == static_cast<std::uint32_t>(
                                     RenderTapeTextureDimension::Volume) &&
            core::isCompressedFormat(devicec::fmtFromD3D(level0.format)))
          goto unsupported;
      } else if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE) {
        RenderTapeSurfaceDescriptorV2 surface{};
        if (!renderTapeLoadSurfaceDescriptorV2(descriptor, surface))
          goto unsupported;
        const auto storage =
            static_cast<RenderTapeSurfaceStorage>(surface.storage);
        const auto disposition = static_cast<RenderTapeInitialContentDisposition>(
            surface.initialContentDisposition);
        if (storage == RenderTapeSurfaceStorage::SwapchainBackbuffer) {
          const bool produced =
              disposition == RenderTapeInitialContentDisposition::ProducedPresentOutput;
          const bool seeded =
              disposition == RenderTapeInitialContentDisposition::CompleteSeed &&
              renderTapeArmColorSnapshotSwapchainSurfaceSupported(surface.surface);
          if ((!produced && !seeded) ||
              !acceptedOutputSurface(surface.surface) ||
              (seeded && (fixed.expectedContentBytes == 0u ||
                          fixed.expectedContentCount != 1u)) ||
              (produced && (fixed.expectedContentBytes != 0u ||
                            fixed.expectedContentCount != 0u))) {
            goto unsupported;
          }
          liveBackbufferIdentities.push_back(fixed.identity);
        } else if (storage == RenderTapeSurfaceStorage::TextureSubresource) {
          if (disposition != RenderTapeInitialContentDisposition::Unavailable)
            goto unsupported;
        } else if (storage == RenderTapeSurfaceStorage::Standalone) {
          if (disposition != RenderTapeInitialContentDisposition::CompleteSeed &&
              disposition != RenderTapeInitialContentDisposition::
                                 CompleteDepthFloat32V1 &&
              disposition != RenderTapeInitialContentDisposition::
                                 CompleteDepthStencilFloat32UInt8V1 &&
              disposition != RenderTapeInitialContentDisposition::
                                 ProducedByCapturedPass) {
            goto unsupported;
          }
        } else {
          goto unsupported;
        }
      } else if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER) {
        D9CBufferDesc buffer{};
        if (descriptor.size() != sizeof(buffer) ||
            !load(descriptor, 0u, buffer) || buffer.size == 0u) {
          goto unsupported;
        }
        sawBuffer = true;
      } else if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_SHADER) {
        RenderTapeShaderDescriptor shader{};
        const auto* blob = findCatalogueBlob(candidate.catalogue, fixed.immutablePayloadDigest);
        if (descriptor.size() != sizeof(shader) ||
            !load(descriptor, 0u, shader) || shader.stage > 1u || !blob ||
            shader.bytecodeBytes != fixed.immutablePayloadBytes ||
            blob->size != shader.bytecodeBytes ||
            shader.bytecodeBytes < sizeof(std::uint32_t) * 2u ||
            shader.bytecodeBytes % sizeof(std::uint32_t) != 0u) {
          goto unsupported;
        }
        referenceBlob(fixed.immutablePayloadDigest);
        sawShader = true;
      } else if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_VERTEX_DECL) {
        RenderTapeVertexDeclDescriptor declaration{};
        const auto* blob = findCatalogueBlob(candidate.catalogue, fixed.immutablePayloadDigest);
        if (descriptor.size() != sizeof(declaration) ||
            !load(descriptor, 0u, declaration) || !blob ||
            declaration.elementBytes != fixed.immutablePayloadBytes ||
            blob->size != declaration.elementBytes ||
            declaration.elementCount == 0u ||
            declaration.elementBytes % sizeof(D9CVertexElement) != 0u) {
          goto unsupported;
        }
        referenceBlob(fixed.immutablePayloadDigest);
      } else if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_QUERY) {
        RenderTapeQueryDescriptorDecoded query{};
        if (!renderTapeLoadQueryDescriptor(descriptor, query) ||
            !renderTapeQueryDescriptorIdentityMatches(query, fixed.identity))
          goto unsupported;
        result.queryReplay.present = true;
        ++result.queryReplay.queryDefinitions;
        if (query.schemaVersion == kRenderTapeQueryDescriptorVersion3)
          ++result.queryReplay.checkpointDefinitions;
        // Absolute timestamps are fresh-execution-dependent. No implicit
        // normalization may substitute for a qualified relational oracle.
        if (query.type == 10u) {
          result.queryReplay.failure =
              RenderTapeQueryReplayFailure::UnsupportedResultKind;
          goto unsupported;
        }
      } else {
        goto unsupported;
      }
      candidate.definitions.push_back(Definition{
          .fixed = fixed,
          .descriptor = descriptor,
      });
      candidate.events.push_back(GeneralPreflightPlan::ReplayEvent{
          .type = RenderTapeEventType::ObjectDefine,
          .eventIndex = index,
          .index = static_cast<std::uint32_t>(candidate.definitions.size() - 1u),
      });
      ++result.coverage.objectDefinitions;
      break;
    }
    case RenderTapeEventType::ObjectDestroy: {
      RenderTapeObjectDestroyHeader fixed{};
      if (!load(event.payload, 0u, fixed)) {
        goto unsupported;
      }
      std::erase_if(liveBackbufferIdentities, [&](const auto& identity) {
        return sameIdentity(identity, fixed.identity);
      });
      candidate.destroys.push_back(fixed);
      candidate.events.push_back(GeneralPreflightPlan::ReplayEvent{
          .type = RenderTapeEventType::ObjectDestroy,
          .eventIndex = index,
          .index = static_cast<std::uint32_t>(candidate.destroys.size() - 1u),
      });
      ++result.coverage.objectDestroys;
      break;
    }
    case RenderTapeEventType::ResourceMutation: {
      RenderTapeResourceMutationHeader fixed{};
      const auto *mutationBlob = load(event.payload, 0u, fixed)
          ? findCatalogueBlob(candidate.catalogue, fixed.digest)
          : nullptr;
      const auto definition = std::find_if(
          candidate.definitions.begin(), candidate.definitions.end(),
          [&](const auto& value) {
            return sameIdentity(value.fixed.identity, fixed.identity);
          });
      if (!mutationBlob || definition == candidate.definitions.end() ||
          !generalMutationShapeValid(fixed, *definition, mutationBlob->size)) {
        goto unsupported;
      }
      referenceBlob(fixed.digest);
      candidate.mutations.push_back(fixed);
      candidate.events.push_back(GeneralPreflightPlan::ReplayEvent{
          .type = RenderTapeEventType::ResourceMutation,
          .eventIndex = index,
          .index = static_cast<std::uint32_t>(candidate.mutations.size() - 1u),
      });
      ++result.coverage.seedMutations;
      break;
    }
    case RenderTapeEventType::CommandChunk: {
      RenderTapeCommandChunkHeader fixed{};
      ImportedChunkView chunk;
      if (!load(event.payload, 0u, fixed) ||
          !importPrevalidatedCommandChunk(
              event.payload.subspan(sizeof(fixed)),
              CommandChunkEnvelope{fixed.wireVersion, fixed.recordCount,
                                   fixed.handleCount},
              chunk)) {
        goto unsupported;
      }
      for (std::size_t recordIndex = 0u; recordIndex < chunk.records.size();
           ++recordIndex) {
        const auto record = chunk.record(recordIndex);
        if (!generalRecordTypeSupported(record.header.type)) goto unsupported;
        ++result.coverage.commandRecords;
        switch (record.header.type) {
        case D9C_COMMAND_RECORD_QUERY_ISSUE:
          result.queryReplay.present = true;
          ++result.queryReplay.issueRecords;
          break;
        case D9C_COMMAND_RECORD_DRAW_PRIMITIVE:
          ++result.coverage.drawPrimitiveRecords;
          break;
        case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE:
          ++result.coverage.drawIndexedPrimitiveRecords;
          sawIndexedDraw = true;
          break;
        case D9C_COMMAND_RECORD_SET_VS_CONST_F:
        case D9C_COMMAND_RECORD_SET_VS_CONST_I:
        case D9C_COMMAND_RECORD_SET_VS_CONST_B:
        case D9C_COMMAND_RECORD_SET_PS_CONST_F:
        case D9C_COMMAND_RECORD_SET_PS_CONST_I:
        case D9C_COMMAND_RECORD_SET_PS_CONST_B:
          ++result.coverage.stateConstantRecords;
          break;
        case D9C_COMMAND_RECORD_CLEAR:
          ++result.coverage.clearRecords;
          break;
        case D9C_COMMAND_RECORD_APPLY_STATE:
          ++result.coverage.applyStateRecords;
          break;
        case D9C_COMMAND_RECORD_PRESENT:
          {
          D9CCommandChunkWirePresent present{};
          if (!load(record.payload, 0u, present) ||
              record.header.handleCount > 1u) {
            goto unsupported;
          }
          D9CWireObjectIdentity outputIdentity{};
          if (record.header.handleCount == 1u) {
            if (present.sourceHandleIndex >= chunk.handles.size())
              goto unsupported;
            const auto& source = chunk.handles[present.sourceHandleIndex];
            outputIdentity = D9CWireObjectIdentity{
                .kind = source.kind,
                .generation = source.generation,
                .objectId = source.objectId,
            };
            if (!containsIdentity(liveBackbufferIdentities, outputIdentity))
              goto unsupported;
            ++result.coverage.presentSourceMappings;
          } else {
            // Legacy handle-less Present can name the implicit primary
            // backing only when one live logical candidate is unambiguous.
            if (liveBackbufferIdentities.size() != 1u) goto unsupported;
            outputIdentity = liveBackbufferIdentities.front();
          }
          const auto definition = std::find_if(candidate.definitions.begin(),
              candidate.definitions.end(), [&](const auto& value) {
                return sameIdentity(value.fixed.identity, outputIdentity);
              });
          RenderTapeSurfaceDescriptorV2 outputSurface{};
          if (definition == candidate.definitions.end() ||
              !renderTapeLoadSurfaceDescriptorV2(definition->descriptor,
                                                  outputSurface) ||
              outputSurface.storage != static_cast<std::uint32_t>(
                  RenderTapeSurfaceStorage::SwapchainBackbuffer) ||
              !presentGeometrySupported(present, outputSurface.surface))
            goto unsupported;
          candidate.outputIdentity = outputIdentity;
          candidate.outputDesc = outputSurface.surface;
          ++result.coverage.presentRecords;
          break;
          }
        default:
          break;
        }
      }
      candidate.commandChunks.push_back(Chunk{
          .envelope = CommandChunkEnvelope{fixed.wireVersion, fixed.recordCount,
                                            fixed.handleCount},
          .bytes = event.payload.subspan(sizeof(fixed)),
      });
      candidate.events.push_back(GeneralPreflightPlan::ReplayEvent{
          .type = RenderTapeEventType::CommandChunk,
          .eventIndex = index,
          .index = static_cast<std::uint32_t>(candidate.commandChunks.size() - 1u),
      });
      ++result.coverage.commandChunks;
      break;
    }
    case RenderTapeEventType::OrderedControl: {
      RenderTapeOrderedControlHeader fixed{};
      if (!load(event.payload, 0u, fixed) ||
          event.payload.size() != sizeof(fixed) + fixed.controlBytes) {
        goto unsupported;
      }
      if (fixed.kind == static_cast<std::uint32_t>(
                            RenderTapeControlKind::QueryGetData)) {
        RenderTapeQueryGetDataControlV2 poll{};
        if (fixed.controlBytes != sizeof(poll) ||
            !load(event.payload, sizeof(fixed), poll) ||
            poll.schemaVersion != kRenderTapeQueryTranscriptVersion2 ||
            poll.dataSize > std::numeric_limits<std::uint32_t>::max())
          goto unsupported;
        result.queryReplay.present = true;
        ++result.queryReplay.recordedPolls;
        if (fixed.resultCode == core::S_FALSE) ++result.queryReplay.capturedPending;
        if (fixed.resultCode == core::S_OK) {
          ++result.queryReplay.capturedReady;
          if (poll.definedBytes != 0u) ++result.queryReplay.requiredResults;
          ++result.queryReplay.readinessRequired;
        }
      } else if (fixed.kind != static_cast<std::uint32_t>(
                                  RenderTapeControlKind::GammaRampSet) ||
                 fixed.controlBytes != kRenderTapeGammaRampBytes) {
        goto unsupported;
      }
      candidate.controls.push_back(fixed);
      candidate.controlBytes.emplace_back(
          event.payload.subspan(sizeof(fixed)).begin(),
          event.payload.subspan(sizeof(fixed)).end());
      candidate.events.push_back(GeneralPreflightPlan::ReplayEvent{
          .type = RenderTapeEventType::OrderedControl,
          .eventIndex = index,
          .index = static_cast<std::uint32_t>(candidate.controls.size() - 1u),
      });
      break;
    }
    case RenderTapeEventType::PresentComplete: {
      RenderTapePresentCompleteHeader fixed{};
      RenderTapeOracleAttachment oracle{};
      if (sawPresentComplete || !load(event.payload, 0u, fixed) ||
          fixed.oracleCount != 1u ||
          !load(event.payload, sizeof(fixed), oracle) ||
          !sameIdentity(oracle.identity, candidate.outputIdentity) ||
          (fixed.digestValidity != static_cast<std::uint32_t>(
                                       RenderTapeDigestValidity::NotCaptured) &&
           fixed.digestValidity != static_cast<std::uint32_t>(
                                       RenderTapeDigestValidity::Sha256))) {
        goto unsupported;
      }
      candidate.expectedDigestValidity =
          static_cast<RenderTapeDigestValidity>(fixed.digestValidity);
      candidate.expectedDigest = fixed.expectedDigest;
      candidate.presentOrdinal = fixed.presentOrdinal;
      candidate.completionOrdinal = fixed.completionOrdinal;
      sawPresentComplete = true;
      ++result.coverage.presentOutputs;
      candidate.events.push_back(GeneralPreflightPlan::ReplayEvent{
          .type = RenderTapeEventType::PresentComplete,
          .eventIndex = index,
      });
      break;
    }
    }
  }

  if (!sawShader || !sawBuffer || !sawIndexedDraw || !sawPresentComplete ||
      candidate.outputIdentity.objectId == 0u || tape.header.presentCount != 1u ||
      result.coverage.presentRecords != 1u ||
      result.coverage.presentSourceMappings > result.coverage.presentRecords ||
      referencedBlobs.size() != candidate.catalogue.blobs.size()) {
    goto unsupported;
  }
  for (const auto& definition : candidate.definitions) {
    if (definition.fixed.identity.kind != D9C_CHUNK_HANDLE_KIND_SURFACE) continue;
    RenderTapeSurfaceDescriptorV2 surface{};
    if (!renderTapeLoadSurfaceDescriptorV2(definition.descriptor, surface))
      goto unsupported;
    // Explicit primary-backing tags establish aliasing. Equal standalone
    // descriptors never do; tagged aliases must nevertheless agree on shape.
    if (surface.storage == static_cast<std::uint32_t>(
            RenderTapeSurfaceStorage::SwapchainBackbuffer) &&
        !renderTapeSurfaceDescriptorsEqual(surface.surface, candidate.outputDesc))
      goto unsupported;
  }
  // Bootstrap overlays are replayed at the first command boundary (or at an
  // early destroy).  Every referenced object must therefore already have a
  // value-owned definition before either trigger; otherwise replay could
  // materialize a partially populated bootstrap and mutate Metal state before
  // discovering the missing object.
  for (const auto& identity : candidate.bootstrapIdentities) {
    const auto definition = std::find_if(
        candidate.definitions.begin(), candidate.definitions.end(),
        [&](const auto& value) { return sameIdentity(value.fixed.identity, identity); });
    if (definition == candidate.definitions.end()) goto unsupported;
    std::size_t definitionIndex = tape.events.size();
    std::size_t firstTrigger = tape.events.size();
    for (std::size_t eventIndex = 0u; eventIndex < tape.events.size();
         ++eventIndex) {
      const auto value = tape.event(eventIndex);
      if (value.header.type == static_cast<std::uint32_t>(
                                  RenderTapeEventType::ObjectDefine) &&
          definitionIndex == tape.events.size()) {
        RenderTapeObjectDefineHeader fixed{};
        if (load(value.payload, 0u, fixed) &&
            sameIdentity(fixed.identity, identity)) {
          definitionIndex = eventIndex;
        }
      }
      if (firstTrigger == tape.events.size() &&
          (value.header.type == static_cast<std::uint32_t>(
                                    RenderTapeEventType::CommandChunk) ||
           value.header.type == static_cast<std::uint32_t>(
                                    RenderTapeEventType::ObjectDestroy))) {
        firstTrigger = eventIndex;
      }
    }
    if (definitionIndex == tape.events.size() ||
        (firstTrigger != tape.events.size() && definitionIndex >= firstTrigger)) {
      goto unsupported;
    }
  }
  // A destroy can be the first bootstrap trigger.  Do not allow it to retire
  // an object that the deferred overlay still needs; otherwise the later
  // overlay application would dereference a value that has already been
  // released even though every definition was present.
  for (std::size_t eventIndex = 0u; eventIndex < tape.events.size();
       ++eventIndex) {
    const auto value = tape.event(eventIndex);
    if (value.header.type == static_cast<std::uint32_t>(
                                RenderTapeEventType::CommandChunk)) {
      firstCommand = eventIndex;
      break;
    }
  }
  if (firstCommand != tape.events.size()) {
    for (std::size_t eventIndex = 0u; eventIndex < firstCommand;
         ++eventIndex) {
      const auto value = tape.event(eventIndex);
      if (value.header.type != static_cast<std::uint32_t>(
                                   RenderTapeEventType::ObjectDestroy)) {
        continue;
      }
      RenderTapeObjectDestroyHeader destroy{};
      if (!load(value.payload, 0u, destroy) ||
          containsIdentity(candidate.bootstrapIdentities, destroy.identity)) {
        goto unsupported;
      }
    }
  }
  result.intervalCount = 1u;
  result.requirements = FrameTapeReplayRequirements{
      .outputWidth = candidate.outputDesc.width,
      .outputHeight = candidate.outputDesc.height,
      .outputFormat = candidate.outputDesc.format,
  };
  result.conservation.referencedBlobs =
      static_cast<std::uint32_t>(referencedBlobs.size());
  result.conservation.presentOrdinal = candidate.presentOrdinal;
  result.conservation.completionOrdinal = candidate.completionOrdinal;
  result.intervals[0].presentOrdinal = candidate.presentOrdinal;
  result.intervals[0].completionOrdinal = candidate.completionOrdinal;
  result.validity.expectedDigestCaptured =
      candidate.expectedDigestValidity == RenderTapeDigestValidity::Sha256;
  result.intervals[0].validity.expectedDigestCaptured =
      result.validity.expectedDigestCaptured;
  result.status = FrameTapeReplayStatus::Complete;
  if (plan) *plan = std::move(candidate);
  return result;

unsupported:
  result.status = FrameTapeReplayStatus::UnsupportedGrammar;
  result.failedEventIndex = currentEventIndex;
  return result;
}

FrameTapeReplayResult buildGeneralPlan(
    std::span<const std::byte> bytes,
    std::span<const RenderTapeProviderBlob> blobs,
    GeneralPreflightPlan* plan) {
  bool valid = false;
  auto catalogue = makeCatalogue(blobs, valid);
  if (!valid) {
    FrameTapeReplayResult result;
    result.conservation.inputBlobs = static_cast<std::uint32_t>(blobs.size());
    result.status = FrameTapeReplayStatus::InvalidBlobCatalogue;
    return result;
  }
  return buildGeneralPlanFromCatalogue(bytes, std::move(catalogue), plan);
}

template <typename T, typename Equal>
bool sameGeneralRows(const std::vector<T>& a, const std::vector<T>& b, Equal equal) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), equal);
}

bool sameGeneralBytes(std::span<const std::byte> a, std::span<const std::byte> b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

bool sameGeneralChunk(const Chunk& a, const Chunk& b) {
  const auto& x = a.envelope.producerIdentity;
  const auto& y = b.envelope.producerIdentity;
  return a.envelope.version == b.envelope.version &&
      a.envelope.recordCount == b.envelope.recordCount &&
      a.envelope.handleCount == b.envelope.handleCount &&
      x.firstEventOrdinal == y.firstEventOrdinal && x.lastEventOrdinal == y.lastEventOrdinal &&
      x.firstSourceOrdinal == y.firstSourceOrdinal && x.lastSourceOrdinal == y.lastSourceOrdinal &&
      sameGeneralBytes(a.bytes, b.bytes);
}

bool sameGeneralTapeHeader(const RenderTapeHeader& x, const RenderTapeHeader& y) {
  return x.magic == y.magic && x.version == y.version && x.headerSize == y.headerSize &&
      x.profile == y.profile && x.eventHeaderSize == y.eventHeaderSize &&
      x.eventTableOffset == y.eventTableOffset && x.eventCount == y.eventCount &&
      x.payloadArenaOffset == y.payloadArenaOffset && x.payloadArenaSize == y.payloadArenaSize &&
      x.wireVersion == y.wireVersion && x.presentCount == y.presentCount &&
      x.reserved0 == y.reserved0 && x.reserved1 == y.reserved1 &&
      x.reserved2 == y.reserved2 && x.reserved3 == y.reserved3;
}

bool sameGeneralDefinitionHeader(const RenderTapeObjectDefineHeader& x,
                                 const RenderTapeObjectDefineHeader& y) {
  return sameIdentity(x.identity, y.identity) && x.descriptorKind == y.descriptorKind &&
      x.descriptorBytes == y.descriptorBytes && x.payloadValidity == y.payloadValidity &&
      x.reserved0 == y.reserved0 && x.immutablePayloadBytes == y.immutablePayloadBytes &&
      x.expectedContentBytes == y.expectedContentBytes &&
      x.expectedContentCount == y.expectedContentCount && x.reserved1 == y.reserved1 &&
      sameDigest(x.immutablePayloadDigest, y.immutablePayloadDigest);
}

bool sameGeneralControlHeader(const RenderTapeOrderedControlHeader& x,
                              const RenderTapeOrderedControlHeader& y) {
  return sameIdentity(x.identity, y.identity) && x.kind == y.kind &&
      x.disposition == y.disposition && x.resultCode == y.resultCode &&
      x.controlBytes == y.controlBytes && x.completionOrdinal == y.completionOrdinal &&
      x.reserved0 == y.reserved0 && x.reserved1 == y.reserved1;
}

bool sameGeneralMutationHeader(const RenderTapeResourceMutationHeader& x,
                               const RenderTapeResourceMutationHeader& y) {
  // Bind semantic fields, not the implicit padding before byteOffset.
  return sameIdentity(x.identity, y.identity) && x.kind == y.kind &&
      x.subresource == y.subresource && x.bufferDisposition == y.bufferDisposition &&
      x.byteOffset == y.byteOffset && x.byteSize == y.byteSize &&
      sameDigest(x.digest, y.digest);
}

bool sameGeneralSurfaceDesc(const D9CSurfaceDesc& x, const D9CSurfaceDesc& y) {
  return x.format == y.format && x.resourceType == y.resourceType && x.usage == y.usage &&
      x.pool == y.pool && x.multiSampleType == y.multiSampleType &&
      x.multiSampleQuality == y.multiSampleQuality && x.width == y.width &&
      x.height == y.height && x.depth == y.depth;
}

bool sameGeneralCatalogue(const RenderTapeBlobCatalogue& a,
                          const RenderTapeBlobCatalogue& b) {
  if (a.blobs.size() != b.blobs.size()) return false;
  for (std::size_t i = 0u; i < a.blobs.size(); ++i) {
    const auto& blob = a.blobs[i];
    if (std::any_of(a.blobs.begin(), a.blobs.begin() + i, [&](const auto& prior) {
          return sameDigest(prior.digest, blob.digest);
        })) return false;
    const auto* other = findCatalogueBlob(b, blob.digest);
    if (!other || blob.size != other->size || blob.verified != other->verified ||
        blob.reserved0 != other->reserved0) return false;
  }
  return true;
}

bool sameGeneralPlan(const GeneralPreflightPlan& a, const GeneralPreflightPlan& b) {
  if (!sameGeneralTapeHeader(a.tape.header, b.tape.header) ||
      !sameGeneralBytes(std::as_bytes(a.tape.events), std::as_bytes(b.tape.events)) ||
      !sameGeneralBytes(a.tape.payloadArena, b.tape.payloadArena) ||
      !sameGeneralCatalogue(a.catalogue, b.catalogue) ||
      !sameGeneralRows(a.definitions, b.definitions, [](const auto& x, const auto& y) {
        return sameGeneralDefinitionHeader(x.fixed, y.fixed) &&
            sameGeneralBytes(x.descriptor, y.descriptor);
      }) ||
      !sameGeneralRows(a.events, b.events, [](const auto& x, const auto& y) {
        return x.type == y.type && x.eventIndex == y.eventIndex && x.index == y.index &&
            x.count == y.count && x.aux == y.aux;
      }) ||
      !sameGeneralRows(a.bootstrapChunks, b.bootstrapChunks, sameGeneralChunk) ||
      !sameGeneralRows(a.bootstrapGamma, b.bootstrapGamma, [](const auto& x, const auto& y) {
        return sameGeneralBytes(x, y);
      }) ||
      !sameGeneralRows(a.commandChunks, b.commandChunks, sameGeneralChunk) ||
      !sameGeneralRows(a.mutations, b.mutations, sameGeneralMutationHeader) ||
      !sameGeneralRows(a.destroys, b.destroys, [](const auto& x, const auto& y) {
        return sameIdentity(x.identity, y.identity);
      }) ||
      !sameGeneralRows(a.controls, b.controls, sameGeneralControlHeader) ||
      !sameGeneralRows(a.controlBytes, b.controlBytes, [](const auto& x, const auto& y) {
        return sameGeneralBytes(x, y);
      }) ||
      !sameGeneralRows(a.bootstrapIdentities, b.bootstrapIdentities,
          [](const auto& x, const auto& y) { return sameIdentity(x, y); })) return false;
  return sameIdentity(a.outputIdentity, b.outputIdentity) &&
      sameGeneralSurfaceDesc(a.outputDesc, b.outputDesc) &&
      a.expectedDigestValidity == b.expectedDigestValidity &&
      sameDigest(a.expectedDigest, b.expectedDigest) &&
      a.presentOrdinal == b.presentOrdinal && a.completionOrdinal == b.completionOrdinal;
}

template <typename SourcePlan>
bool generalReplayPlanMatchesAuthenticated(
    std::span<const std::byte> tape, const SourcePlan& sourcePlan,
    const GeneralPreflightPlan& supplied) noexcept {
  if (!sourcePlan.valid()) return false;
  try {
    if (tape.size() != sourcePlan.identity().header.eventsBytes ||
        !sameDigest(RenderTapeCaptureSession::sha256(tape),
                    sourcePlan.identity().header.eventsDigest)) return false;
    // Only the admitted owner's copied, authenticated references are trusted.
    // Supplied plan catalogues cannot add, resize or upgrade verified blobs.
    RenderTapeBlobCatalogue catalogue;
    catalogue.blobs.assign(sourcePlan.referencedBlobs().begin(),
                           sourcePlan.referencedBlobs().end());
    GeneralPreflightPlan canonical;
    return buildGeneralPlanFromCatalogue(tape, std::move(catalogue), &canonical).complete() &&
        sameGeneralPlan(canonical, supplied);
  } catch (...) { return false; }
}

} // namespace

namespace render_tape_runtime {

FrameTapeReplayResult buildGeneralReplayPlan(
    std::span<const std::byte> tape,
    std::span<const RenderTapeProviderBlob> blobs,
    GeneralPreflightPlan* plan) {
  return buildGeneralPlan(tape, blobs, plan);
}

bool generalReplayPlanMatchesAuthenticatedV3(
    std::span<const std::byte> tape, const CaptureSourceReplayPlanV3& sourcePlan,
    const GeneralPreflightPlan& supplied) noexcept {
  return generalReplayPlanMatchesAuthenticated(tape, sourcePlan, supplied);
}

bool generalReplayPlanMatchesAuthenticatedV4(
    std::span<const std::byte> tape, const CaptureSourceReplayPlanV4& sourcePlan,
    const GeneralPreflightPlan& supplied) noexcept {
  return generalReplayPlanMatchesAuthenticated(tape, sourcePlan, supplied);
}

void releaseOwnedObject(OwnedObject& object) noexcept {
  if (!object.value) return;
  switch (object.identity.kind) {
  case D9C_CHUNK_HANDLE_KIND_TEXTURE:
    dxmt9c_texture_release(static_cast<D9CTexture*>(object.value)); break;
  case D9C_CHUNK_HANDLE_KIND_SURFACE:
    dxmt9c_surface_release(static_cast<D9CSurface*>(object.value)); break;
  case D9C_CHUNK_HANDLE_KIND_BUFFER:
    dxmt9c_buffer_release(static_cast<D9CBuffer*>(object.value)); break;
  case D9C_CHUNK_HANDLE_KIND_SHADER:
    dxmt9c_shader_release(static_cast<D9CShader*>(object.value)); break;
  case D9C_CHUNK_HANDLE_KIND_VERTEX_DECL:
    dxmt9c_vdecl_release(static_cast<D9CVertexDecl*>(object.value)); break;
  case D9C_CHUNK_HANDLE_KIND_QUERY:
    dxmt9c_query_release(static_cast<D9CQuery*>(object.value)); break;
  default: break;
  }
  object.value = nullptr;
}

void* findExactObject(std::span<const OwnedObject> objects,
                      const D9CWireObjectIdentity& identity) noexcept {
  const auto it = std::find_if(objects.begin(), objects.end(), [&](const auto& object) {
    return object.value && sameIdentity(object.identity, identity);
  });
  return it == objects.end() ? nullptr : it->value;
}

bool resolveCommandHandles(const CommandChunkView& chunk,
                           std::span<const OwnedObject> objects,
                           std::span<void*> resolved,
                           std::size_t& resolvedCount) noexcept {
  resolvedCount = 0u;
  ImportedChunkView imported;
  if (!importPrevalidatedCommandChunk(chunk.bytes, chunk.envelope, imported)) return false;
  if (imported.handles.size() > resolved.size()) return false;
  // Validate the complete identity set before publishing any pointer into
  // caller scratch. A failed lookup is therefore transactional: count stays
  // zero and the output span remains untouched.
  for (const auto& handle : imported.handles) {
    void* object = findExactObject(objects, D9CWireObjectIdentity{
        .kind = handle.kind, .generation = handle.generation,
        .objectId = handle.objectId});
    if (!object) return false;
  }
  for (std::size_t index = 0u; index < imported.handles.size(); ++index) {
    const auto& handle = imported.handles[index];
    resolved[index] = findExactObject(objects, D9CWireObjectIdentity{
        .kind = handle.kind, .generation = handle.generation,
        .objectId = handle.objectId});
  }
  resolvedCount = imported.handles.size();
  return true;
}

} // namespace render_tape_runtime

namespace {

bool writeBlockRows(std::span<const std::byte> bytes, void* bits,
                    const RenderTapeBlockLockLayout& layout) {
  if (!bits || bytes.size() != layout.tightBytes || layout.rowBytes == 0u ||
      layout.rows == 0u || layout.pitch < layout.rowBytes ||
      layout.tightBytes !=
          static_cast<std::uint64_t>(layout.rowBytes) * layout.rows) {
    return false;
  }
  auto* destination = static_cast<std::byte*>(bits);
  for (std::uint32_t row = 0u; row < layout.rows; ++row) {
    std::memcpy(destination + static_cast<std::size_t>(row) * layout.pitch,
                bytes.data() + static_cast<std::size_t>(row) * layout.rowBytes,
                layout.rowBytes);
  }
  return true;
}

bool readbackTightSurface(D9CDevice* device, D9CSurface* surface,
                          std::uint32_t width, std::uint32_t height,
                          std::uint32_t format,
                          std::vector<std::byte>& out) {
  if (!device || !surface || !surface->obj || width == 0u || height == 0u ||
      format == 0u) {
    return false;
  }
  auto upper = device->dev().upperDevice();
  if (!upper) return false;
  core::ReadbackPixels pixels;
  if (!upper->readbackSurface(
          core::ReadbackDesc{.source = surface->obj->handle()}, pixels)) {
    return false;
  }
  const auto bytesPerPixel = core::bytesPerPixel(
      static_cast<core::Format>(surface->obj->desc().format));
  if (bytesPerPixel == 0u ||
      width > std::numeric_limits<std::uint32_t>::max() / bytesPerPixel) {
    return false;
  }
  const auto tightPitch = static_cast<std::uint64_t>(width) * bytesPerPixel;
  const auto tightBytes = tightPitch * height;
  const auto pitchedBytes = static_cast<std::uint64_t>(pixels.pitch) * height;
  if (pixels.pitch < tightPitch ||
      tightBytes > std::numeric_limits<std::size_t>::max() ||
      pixels.bytes.size() < pitchedBytes) {
    return false;
  }
  try {
    out.resize(static_cast<std::size_t>(tightBytes));
  } catch (...) {
    return false;
  }
  for (std::uint32_t row = 0u; row < height; ++row) {
    std::memcpy(out.data() + static_cast<std::size_t>(row * tightPitch),
                pixels.bytes.data() + static_cast<std::size_t>(row) * pixels.pitch,
                static_cast<std::size_t>(tightPitch));
  }
  return renderTapeCanonicalizeOpaqueAlpha(format, out);
}

} // namespace

namespace render_tape_runtime {

RenderTapeQueryReplayFailure settleQueryOracle(
    D9CDevice* device, const core::QueryPollSnapshot& snapshot,
    RenderTapeQueryReplayRow& row,
    RenderTapeQueryReplayEvidence& evidence) noexcept {
  using Failure = RenderTapeQueryReplayFailure;
  const auto started = std::chrono::steady_clock::now();
  const auto finish = [&](Failure failure) {
    evidence.oracleWallNanos += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count());
    return failure;
  };
  if (!device || snapshot.failed || snapshot.active ||
      snapshot.owner != &device->dev() ||
      snapshot.identity != row.localQueryIdentity ||
      snapshot.issueOrdinal != row.localIssueOrdinal ||
      snapshot.issueDeviceEpoch != row.localDeviceEpoch ||
      (snapshot.issueOrdinal && !snapshot.constant() &&
       !snapshot.backendFence.valid())) {
    return finish(Failure::SnapshotUnavailable);
  }
  auto oracleSnapshot = snapshot;
  if (!oracleSnapshot.eligible &&
      !device->dev().promotePublishedQueryPollSnapshot(oracleSnapshot)) {
    return finish(Failure::SnapshotUnavailable);
  }
  while (evidence.supplementalPolls < evidence.maxOraclePolls) {
    const auto elapsed = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count());
    if (evidence.oracleWallNanos >= evidence.maxOracleWallNanos ||
        elapsed >= evidence.maxOracleWallNanos - evidence.oracleWallNanos) {
      evidence.progressLimit = RenderTapeQueryProgressLimit::WallBudget;
      return finish(Failure::ProgressExhausted);
    }
    std::uint64_t staged = 0u;
    row.supplemental = true;
    ++row.oraclePolls;
    ++evidence.supplementalPolls;
    row.oracleResult = device->dev().pollQuerySnapshot(
        oracleSnapshot, row.hasOutput ? &staged : nullptr,
        std::min<std::uint64_t>(row.dataSize, sizeof(staged)),
        core::QUERY_GETDATA_FLUSH);
    const auto fence = snapshot.backendFence.snapshot();
    row.localQueue = fence.queue;
    row.localSequence = fence.sequence;
    row.localInitializer = fence.initializer;
    if (snapshot.backend) {
      const auto progress = snapshot.backend->queryProgress();
      row.completedSequence = progress.completed;
      row.completedInitializer = progress.initializerCompleted;
    }
    if (row.oracleResult == core::S_OK) {
      row.readinessSatisfied = true;
      ++evidence.readinessSatisfied;
      std::memcpy(row.replayBytes.data(), &staged, row.definedBytes);
      row.resultMatched = row.definedBytes != 0u && std::equal(
          row.capturedBytes.begin(), row.capturedBytes.begin() + row.definedBytes,
          row.replayBytes.begin());
      if (row.definedBytes != 0u && !row.resultMatched)
        return finish(Failure::ResultMismatch);
      if (row.definedBytes != 0u) ++evidence.matchedResults;
      return finish(Failure::None);
    }
    if (row.oracleResult != core::S_FALSE) return finish(Failure::PollFailed);
    std::this_thread::yield();
  }
  evidence.progressLimit = RenderTapeQueryProgressLimit::PollBudget;
  return finish(Failure::ProgressExhausted);
}

void* createRenderTapeObject(
    D9CDevice* device, const RenderTapeObjectDefineHeader& fixed,
    std::span<const std::byte> descriptor,
    std::span<const RenderTapeProviderBlob> blobs,
    std::span<const OwnedObject> objects) {
  if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE) {
    RenderTapeSurfaceDescriptorV2 surface{};
    if (!renderTapeLoadSurfaceDescriptorV2(descriptor, surface)) return nullptr;
    const auto storage = static_cast<RenderTapeSurfaceStorage>(surface.storage);
    if (storage == RenderTapeSurfaceStorage::TextureSubresource) {
      auto* parent = static_cast<D9CTexture*>(
          findExactObject(objects, surface.parentTexture));
      return parent && device
          ? static_cast<void*>(dxmt9c_texture_get_surface_level(
                parent, surface.subresource))
          : nullptr;
    }
    if (!device) return nullptr;
    if (storage == RenderTapeSurfaceStorage::SwapchainBackbuffer) {
      auto* swap = device->iface->GetSwapChain(0u);
      if (!swap) return nullptr;
      auto* value = new D9CSurface{swap->backBuffer(), nullptr, 0u, device};
      swap->Release();
      return value;
    }
    if (storage != RenderTapeSurfaceStorage::Standalone) return nullptr;
    if ((surface.surface.usage & 1u) != 0u) {
      return dxmt9c_device_create_render_target(
          device, surface.surface.width, surface.surface.height,
          surface.surface.format, surface.surface.multiSampleType,
          surface.surface.multiSampleQuality, 0u, nullptr);
    }
    if ((surface.surface.usage & 2u) != 0u) {
      return dxmt9c_device_create_depth_stencil(
          device, surface.surface.width, surface.surface.height,
          surface.surface.format, surface.surface.multiSampleType,
          surface.surface.multiSampleQuality, 0u, nullptr);
    }
    return nullptr;
  }
  if (!device) return nullptr;
  if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER) {
    D9CBufferDesc desc{};
    if (!load(descriptor, 0u, desc)) return nullptr;
    return desc.format
        ? static_cast<void*>(dxmt9c_device_create_index_buffer(
              device, desc.size, desc.usage, desc.format, desc.pool))
        : static_cast<void*>(dxmt9c_device_create_vertex_buffer(
              device, desc.size, desc.usage, desc.fvf, desc.pool));
  }
  if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
    RenderTapeTextureDescriptorV2 desc{};
    D9CSurfaceDesc level0{};
    if (!renderTapeLoadTextureDescriptorV2(descriptor, desc) ||
        !renderTapeTextureSubresourceDescriptor(descriptor, 0u, level0)) {
      return nullptr;
    }
    if (desc.dimension == static_cast<std::uint32_t>(
                              RenderTapeTextureDimension::Cube)) {
      return dxmt9c_device_create_cube_texture(
          device, level0.width, desc.mipLevelCount, level0.usage,
          level0.format, level0.pool);
    }
    if (desc.dimension == static_cast<std::uint32_t>(
                              RenderTapeTextureDimension::Texture2D)) {
      return dxmt9c_device_create_texture(
          device, level0.width, level0.height, desc.mipLevelCount,
          level0.usage, level0.format, level0.pool);
    }
    if (desc.dimension == static_cast<std::uint32_t>(
                              RenderTapeTextureDimension::Volume)) {
      return dxmt9c_device_create_volume_texture(
          device, level0.width, level0.height, level0.depth, desc.mipLevelCount,
          level0.usage, level0.format, level0.pool);
    }
    return nullptr;
  }
  if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_SHADER) {
    RenderTapeShaderDescriptor desc{};
    const auto* blob = findBlob(blobs, fixed.immutablePayloadDigest);
    if (!load(descriptor, 0u, desc) || !blob) return nullptr;
    const auto* bytecode =
        reinterpret_cast<const std::uint32_t*>(blob->bytes.data());
    return desc.stage == 0u
        ? static_cast<void*>(dxmt9c_device_create_vertex_shader(device,
                                                                 bytecode))
        : static_cast<void*>(dxmt9c_device_create_pixel_shader(device,
                                                                bytecode));
  }
  if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_VERTEX_DECL) {
    const auto* blob = findBlob(blobs, fixed.immutablePayloadDigest);
    return blob
        ? static_cast<void*>(dxmt9c_device_create_vertex_declaration(
              device,
              reinterpret_cast<const D9CVertexElement*>(blob->bytes.data())))
        : nullptr;
  }
  if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_QUERY) {
    RenderTapeQueryDescriptorDecoded desc{};
    if (!renderTapeLoadQueryDescriptor(descriptor, desc) ||
        !renderTapeQueryDescriptorIdentityMatches(desc, fixed.identity))
      return nullptr;
    auto* query = dxmt9c_device_create_query(device, desc.type);
    if (query && dxmt9c_query_get_data_size(query) != desc.dataBytes) {
      dxmt9c_query_release(query);
      return nullptr;
    }
    return query;
  }
  return nullptr;
}

} // namespace render_tape_runtime

namespace {

struct TexturePrestageEntry {
  Definition definition{};
  std::vector<RenderTapeDigest> seedDigests{};
  std::vector<bool> consumed{};
  std::size_t hiddenIndex = std::numeric_limits<std::size_t>::max();
  bool active = false;
};

// Owned by immutable preparation; indices refer only to its authenticated
// General plan. Runtime wrappers never live in this metadata owner.
struct NormalHistorySetup {
  RenderTapeAttachmentHistoryHeader header{};
  std::vector<RenderTapeAttachmentHistoryRow> rows;
  std::vector<std::uint32_t> surfaces, lateAliases;
  std::vector<D9CWireObjectIdentity> producedParents;
};

bool prepareNormalHistorySetup(const CaptureSourceReplayPlanV4& source,
    const GeneralPreflightPlan& general, std::span<const std::byte> tape,
    const RenderTapePreparedV4PreparationOptions& options, NormalHistorySetup& out) {
  const auto& identity = source.identity().header;
  RenderTapeActionProfileV1ComponentHeader component{};
  if (!nonZeroDigest(options.expectedNormalActionDigest) ||
      !nonZeroDigest(options.expectedNormalHistoryDigest) ||
      options.normalAttachmentHistory.size() > sizeof(RenderTapeAttachmentHistoryHeader) +
          kAttachmentHistoryMaxRows * sizeof(RenderTapeAttachmentHistoryRow) ||
      !importRenderTapeActionProfileV1Component(options.normalActionProfile, component) ||
      RenderTapeCaptureSession::sha256(options.normalActionProfile) != options.expectedNormalActionDigest ||
      RenderTapeCaptureSession::sha256(options.normalAttachmentHistory) != options.expectedNormalHistoryDigest ||
      !renderTapeActionProfileV1ComponentMatchesBinding(component,
          identity.captureToken, identity.captureEpoch, identity.queue,
          tape.size(), options.sourceBoundaryIdentity.size(), options.sourceExecutionTopology.size(),
          RenderTapeCaptureSession::sha256(tape),
          RenderTapeCaptureSession::sha256(options.sourceBoundaryIdentity),
          RenderTapeCaptureSession::sha256(options.sourceExecutionTopology)) ||
      component.witness.constructionProfile || component.witness.armProfile ||
      component.witness.freezeProfile || component.witness.armRevision || component.witness.freezeRevision ||
      !renderTapeAttachmentHistoryMatchesCapture(options.normalAttachmentHistory, identity, component.witness) ||
      !decodeRenderTapeAttachmentHistory(options.normalAttachmentHistory, out.header, out.rows)) return false;
  std::uint32_t prefix = 0u;
  if (!renderTapeInitialSetupV4SnapshotPrefix(general, prefix)) return false;
  bool haveCurrent = false;
  for (const auto& event : general.events) {
    if (event.type != RenderTapeEventType::ObjectDefine) continue;
    const auto& definition = general.definitions[event.index];
    if (definition.fixed.identity.kind != D9C_CHUNK_HANDLE_KIND_SURFACE) continue;
    const auto row = std::find_if(out.rows.begin(), out.rows.end(), [&](const auto& value) {
      return sameIdentity(value.surface, definition.fixed.identity);
    });
    RenderTapeSurfaceDescriptorV2 surface{};
    if (row == out.rows.end() || !renderTapeLoadSurfaceDescriptorV2(definition.descriptor, surface)) return false;
    for (const auto index : out.surfaces)
      if (general.definitions[index].fixed.identity.objectId == definition.fixed.identity.objectId) return false;
    const bool alias = surface.storage == static_cast<std::uint32_t>(RenderTapeSurfaceStorage::TextureSubresource);
    const bool backbuffer = surface.storage == static_cast<std::uint32_t>(RenderTapeSurfaceStorage::SwapchainBackbuffer);
    // The bounded Normal lane settles backbuffer seeds only at its pre-arm
    // checkpoint. Reject later writes before allocating or touching a device;
    // the ordinary mutation path has Exact-only post-copy settlement.
    if (backbuffer && std::any_of(general.events.begin(), general.events.end(),
            [&](const auto& value) {
              return value.eventIndex >= prefix &&
                  value.type == RenderTapeEventType::ResourceMutation &&
                  sameIdentity(general.mutations[value.index].identity, definition.fixed.identity);
            })) return false;
    if (!sameIdentity(row->storage, alias ? surface.parentTexture : definition.fixed.identity) ||
        row->subresource != surface.subresource || bool(row->currentBackbuffer) != backbuffer) return false;
    haveCurrent |= row->currentBackbuffer != 0u;
    out.surfaces.push_back(event.index);
    if (event.eventIndex < prefix) continue;
    if (!alias || definition.fixed.expectedContentBytes || definition.fixed.expectedContentCount) return false;
    out.lateAliases.push_back(event.index);
    const auto parent = std::find_if(general.definitions.begin(), general.definitions.end(),
        [&](const auto& value) { return sameIdentity(value.fixed.identity, surface.parentTexture); });
    RenderTapeTextureDescriptorV2 texture{};
    if (parent == general.definitions.end() ||
        !renderTapeLoadTextureDescriptorV2(parent->descriptor, texture)) return false;
    if (texture.initialContentDisposition == static_cast<std::uint32_t>(RenderTapeInitialContentDisposition::ProducedByCapturedPass) &&
        std::none_of(out.producedParents.begin(), out.producedParents.end(),
            [&](const auto& value) { return sameIdentity(value, surface.parentTexture); })) {
      const auto pe = std::find_if(general.events.begin(), general.events.end(), [&](const auto& value) {
        return value.type == RenderTapeEventType::ObjectDefine &&
            value.index == static_cast<std::size_t>(parent - general.definitions.begin());
      });
      if (pe == general.events.end()) return false;
      if (pe->eventIndex >= prefix) out.producedParents.push_back(surface.parentTexture);
    }
  }
  return haveCurrent == bool(out.header.flags & 2u) && !out.surfaces.empty();
}

bool resolveChunk(const Chunk& chunk,
                  std::span<const render_tape_runtime::OwnedObject> objects,
                  std::vector<void*>& resolved) {
  ImportedChunkView imported;
  if (!importPrevalidatedCommandChunk(chunk.bytes, chunk.envelope, imported)) return false;
  resolved.resize(imported.handles.size());
  std::size_t resolvedCount = 0u;
  return render_tape_runtime::resolveCommandHandles(
             render_tape_runtime::CommandChunkView{
                 .envelope = chunk.envelope, .bytes = chunk.bytes},
             objects, resolved, resolvedCount) &&
         resolvedCount == imported.handles.size();
}

bool writeGeneralLockedSurface(const D9CSurfaceDesc& desc,
                               const D9CLockedRect& locked,
                               std::span<const std::byte> bytes) {
  if (desc.format == 113u) {
    // RGBA16F is a snapshot-only RT texture shape, not a new admission into
    // the generic LockRect/LockBox format table. Preserve every half bit.
    return writeRenderTapeColorSnapshotRows(bytes, locked.bits, locked.pitch,
                                            desc);
  }
  if (renderTapeLinearBytesPerPixel(desc.format) != 0u) {
    RenderTapeLinearLockLayout layout{};
    return renderTapeLinearLockLayout(desc, locked.pitch, nullptr, layout) ==
               RenderTapeLinearLayoutStatus::Accepted &&
           writeRenderTapeLinearRows(bytes, locked.bits, layout);
  }
  RenderTapeBlockLockLayout layout{};
  return renderTapeBlockLockLayout(desc, locked.pitch, nullptr, layout) ==
             RenderTapeBlockLayoutStatus::Accepted &&
         writeBlockRows(bytes, locked.bits, layout);
}

bool seedStandaloneD24S8(D9CDevice* device, D9CSurface* wrapped,
                         const D9CSurfaceDesc& surface,
                         std::span<const std::byte> bytes) {
  RenderTapeD24S8SnapshotLayout layout{};
  auto upper = device ? device->dev().upperDevice() : nullptr;
  if (!upper || !wrapped || !wrapped->obj ||
      !renderTapeDeriveD24S8SnapshotLayout(surface, layout) ||
      layout.byteCount > std::numeric_limits<std::size_t>::max() ||
      bytes.size() != layout.byteCount) return false;
  core::CanonicalD24S8DepthStencil checkpoint{
      .bytes = std::vector<core::u8>(bytes.size()),
      .version = core::kCanonicalD24S8DepthStencilVersion1,
      .width = surface.width, .height = surface.height,
      .depthRowPitchBytes = layout.depthPitch,
      .stencilRowPitchBytes = layout.stencilPitch,
      .stencilOffsetBytes = layout.stencilOffset,
      .physicalFormat = D9C_RENDER_TAPE_D24S8_PHYSICAL_FORMAT_D32FLOAT_STENCIL8,
  };
  std::memcpy(checkpoint.bytes.data(), bytes.data(), bytes.size());
  return upper->seedCanonicalD24S8DepthStencil(wrapped->obj->handle(), checkpoint);
}

bool applyGeneralMutation(
    D9CDevice* device, const RenderTapeResourceMutationHeader& mutation,
    std::span<const RenderTapeProviderBlob> blobs,
    const std::vector<Definition>& definitions,
    std::vector<OwnedObject>& objects, bool deferNormalHistory = false) {
  // This is deliberately general-provider-only. It performs GPU-visible
  // lock/unlock work (and may require a queue flush); CPU completion consumers
  // must establish their own bounded host-visible transfer path instead of
  // calling this mutation helper.
  const auto* blob = findBlob(blobs, mutation.digest);
  void* object = render_tape_runtime::findExactObject(objects, mutation.identity);
  if (!blob || !object || blob->bytes.size() != mutation.byteSize) return false;
  const auto definition = std::find_if(
      definitions.begin(), definitions.end(), [&](const auto& value) {
        return sameIdentity(value.fixed.identity, mutation.identity);
      });
  if (definition == definitions.end()) return false;

  if (mutation.identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER) {
    RenderTapeBufferMutationReplayPlan replayPlan{};
    if (!renderTapeBufferMutationReplayPlan(mutation, replayPlan)) return false;
    void* mapped = nullptr;
    auto* buffer = static_cast<D9CBuffer*>(object);
    if (dxmt9c_buffer_lock(
            buffer, replayPlan.byteOffset, replayPlan.byteSize, &mapped,
            replayPlan.lockFlags) !=
            core::D3D_OK ||
        !mapped) {
      return false;
    }
    std::memcpy(mapped, blob->bytes.data(), blob->bytes.size());
    return dxmt9c_buffer_unlock(buffer) == core::D3D_OK;
  }

  if (mutation.identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
    if (mutation.byteOffset != 0u) return false;
    D9CSurfaceDesc desc{};
    if (!renderTapeTextureSubresourceDescriptor(
            definition->descriptor, mutation.subresource, desc)) {
      return false;
    }
    if (desc.format == 113u &&
        !renderTapeArmColorSnapshotTextureSupported(definition->descriptor))
      return false;
    D9CLockedRect locked{};
    auto* texture = static_cast<D9CTexture*>(object);
    if (dxmt9c_texture_lock_rect(texture, mutation.subresource, &locked,
                                 nullptr, 0u) != core::D3D_OK ||
        !locked.bits) {
      return false;
    }
    bool copied = false;
    if (desc.resourceType == 4u) {
      RenderTapeVolumeLockLayout canonical{}, layout{};
      // This full-mip C facade exposes RowPitch only. Native volume storage
      // packs storage rows (block rows for BC); a real WoW64 shadow, if active,
      // owns its explicitly recorded app-visible SlicePitch instead.
      std::uint64_t slicePitch = 0u;
      copied = renderTapeVolumeContentLayout(desc, canonical) ==
                   RenderTapeVolumeLayoutStatus::Accepted &&
          locked.pitch > 0 &&
          checkedMul(static_cast<std::uint64_t>(locked.pitch),
                     canonical.fullRows, slicePitch);
      const auto shadow = texture->wow64Locks.find(mutation.subresource);
      if (copied && shadow != texture->wow64Locks.end() && shadow->second.active) {
        if (shadow->second.shadow.ptr != locked.bits) copied = false;
        else slicePitch = shadow->second.shadowSlicePitch;
      }
      copied = copied && slicePitch <= std::numeric_limits<std::int64_t>::max() &&
          renderTapeVolumeLockLayout(desc, locked.pitch,
              static_cast<std::int64_t>(slicePitch), nullptr, layout) ==
              RenderTapeVolumeLayoutStatus::Accepted &&
          writeRenderTapeVolumeRows(blob->bytes, locked.bits, layout);
    } else {
      copied = writeGeneralLockedSurface(desc, locked, blob->bytes);
    }
    const bool unlocked =
        dxmt9c_texture_unlock_rect(texture, mutation.subresource) ==
        core::D3D_OK;
    return copied && unlocked;
  }

  if (mutation.identity.kind != D9C_CHUNK_HANDLE_KIND_SURFACE ||
      mutation.subresource != 0u || mutation.byteOffset != 0u) {
    return false;
  }
  RenderTapeSurfaceDescriptorV2 surface{};
  if (!renderTapeLoadSurfaceDescriptorV2(definition->descriptor, surface) ||
      (surface.storage != static_cast<std::uint32_t>(
           RenderTapeSurfaceStorage::Standalone) &&
       surface.storage != static_cast<std::uint32_t>(
           RenderTapeSurfaceStorage::SwapchainBackbuffer))) {
    return false;
  }
  auto* wrapped = static_cast<D9CSurface*>(object);
  const auto disposition = static_cast<RenderTapeInitialContentDisposition>(
      surface.initialContentDisposition);
  if (disposition == RenderTapeInitialContentDisposition::
                         CompleteDepthStencilFloat32UInt8V1) {
    return surface.storage == static_cast<std::uint32_t>(
                                  RenderTapeSurfaceStorage::Standalone) &&
           seedStandaloneD24S8(device, wrapped, surface.surface, blob->bytes);
  }
  if (disposition ==
      RenderTapeInitialContentDisposition::CompleteDepthFloat32V1) {
    auto upper = device->dev().upperDevice();
    if (!upper || !wrapped || !wrapped->obj) return false;
    core::CanonicalD24X8Depth canonical{
        .bytes = std::vector<core::u8>(blob->bytes.size()),
        .version = core::kCanonicalD24X8DepthVersion1,
        .width = surface.surface.width,
        .height = surface.surface.height,
        .pitch = surface.surface.width * 4u,
        .physicalFormat = static_cast<core::u32>(convert::toPixelFormat(
            core::Format::D24X8, upper->limits())),
    };
    std::memcpy(canonical.bytes.data(), blob->bytes.data(), blob->bytes.size());
    return upper->seedCanonicalD24X8Depth(wrapped->obj->handle(), canonical);
  }
  if (disposition != RenderTapeInitialContentDisposition::CompleteSeed ||
      (static_cast<RenderTapeSurfaceStorage>(surface.storage) ==
           RenderTapeSurfaceStorage::SwapchainBackbuffer &&
       !renderTapeArmColorSnapshotSwapchainSurfaceSupported(surface.surface)))
    return false;
  if (static_cast<RenderTapeSurfaceStorage>(surface.storage) ==
      RenderTapeSurfaceStorage::SwapchainBackbuffer) {
    auto upper = device ? device->dev().upperDevice() : nullptr;
    if (!upper || !wrapped || !wrapped->obj) return false;
    auto* stagingTexture = dxmt9c_device_create_texture(
        device, surface.surface.width, surface.surface.height, 1u, 0u,
        surface.surface.format, 0u);
    if (!stagingTexture || !stagingTexture->obj) {
      if (stagingTexture) dxmt9c_texture_release(stagingTexture);
      return false;
    }
    D9CLockedRect locked{};
    bool copied = dxmt9c_texture_lock_rect(
                      stagingTexture, 0u, &locked, nullptr, 0u) ==
                      core::D3D_OK &&
                  locked.bits &&
                  writeGeneralLockedSurface(surface.surface, locked,
                                             blob->bytes);
    if (locked.bits) {
      copied = (dxmt9c_texture_unlock_rect(stagingTexture, 0u) ==
                core::D3D_OK) && copied;
    }
    auto* staging = copied
        ? dxmt9c_texture_get_surface_level(stagingTexture, 0u)
        : nullptr;
    copied = copied && staging && staging->obj;
    if (copied) {
      const core::Rect extent{0, 0, static_cast<core::i32>(surface.surface.width),
                              static_cast<core::i32>(surface.surface.height)};
      upper->submitSurfaceCopy(core::SurfaceCopyDesc{
          .source = staging->obj->handle(),
          .destination = wrapped->obj->handle(),
          .sourceRect = extent,
          .destinationRect = extent,
      });
      upper->flush();
      // Normal setup settles every seed at its checked pre-arm checkpoint,
      // then restores the original discard/touch history in one transaction.
      // This branch is never completion or initial-content authority by itself.
      if (!deferNormalHistory)
        copied = upper->queue().completeRenderTapeBackbufferSeed(wrapped->obj->handle());
    }
    if (staging) dxmt9c_surface_release(staging);
    dxmt9c_texture_release(stagingTexture);
    return copied;
  }
  D9CLockedRect locked{};
  if (dxmt9c_surface_lock_rect(wrapped, &locked, nullptr, 0u) != core::D3D_OK ||
      !locked.bits) {
    return false;
  }
  const bool copied =
      writeGeneralLockedSurface(surface.surface, locked, blob->bytes);
  const bool unlocked = dxmt9c_surface_unlock_rect(wrapped) == core::D3D_OK;
  return copied && unlocked;
}

struct SeededSubresource {
  D9CWireObjectIdentity identity{};
  std::uint32_t subresource = 0u;
};

void restoreSeededColorAttachmentTouches(
    D9CDevice* device, const std::vector<Definition>& definitions,
    std::vector<OwnedObject>& objects,
    std::span<const SeededSubresource> seeded) {
  auto upper = device ? device->dev().upperDevice() : nullptr;
  if (!upper) return;
  for (const auto& definition : definitions) {
    if (definition.fixed.identity.kind != D9C_CHUNK_HANDLE_KIND_SURFACE)
      continue;
    RenderTapeSurfaceDescriptorV2 surface{};
    if (!renderTapeLoadSurfaceDescriptorV2(definition.descriptor, surface) ||
        (surface.surface.usage & 1u) == 0u) {
      continue;
    }
    const auto storage = static_cast<RenderTapeSurfaceStorage>(surface.storage);
    const bool hasSeed = std::any_of(
        seeded.begin(), seeded.end(), [&](const auto& value) {
          if (storage == RenderTapeSurfaceStorage::Standalone) {
            return value.subresource == 0u &&
                   sameIdentity(value.identity, definition.fixed.identity);
          }
          if (storage == RenderTapeSurfaceStorage::SwapchainBackbuffer) {
            return value.subresource == 0u &&
                   sameIdentity(value.identity, definition.fixed.identity);
          }
          return storage == RenderTapeSurfaceStorage::TextureSubresource &&
                 value.subresource == surface.subresource &&
                 sameIdentity(value.identity, surface.parentTexture);
        });
    if (!hasSeed) continue;
    auto* wrapped = static_cast<D9CSurface*>(
        render_tape_runtime::findExactObject(objects, definition.fixed.identity));
    if (wrapped && wrapped->obj) {
      upper->queue().markColorHandleTouched(wrapped->obj->handle());
    }
  }
}

bool chunkContainsPresent(const Chunk& chunk) {
  ImportedChunkView imported;
  if (!importPrevalidatedCommandChunk(chunk.bytes, chunk.envelope, imported))
    return false;
  return std::any_of(imported.records.begin(), imported.records.end(),
                     [](const auto& record) {
                       return record.type == D9C_COMMAND_RECORD_PRESENT;
                     });
}

// Phase timing is deliberately private to prepared replay.  A null timing
// sink keeps the historical entry points free of clock reads on their hot
// path while still allowing the cold harness to account for service phases.
struct PreparedPhaseTiming {
  std::uint64_t resourceSetupNanos = 0u;
  std::uint64_t chunkReplayNanos = 0u;
  std::uint64_t readbackDrainNanos = 0u;
  std::uint64_t cleanupNanos = 0u;
  std::uint64_t v4PlanAdmissionNanos = 0u;
  std::uint64_t v4ReplayAdmissionNanos = 0u;
  std::uint64_t v4PreArmCheckpointNanos = 0u;
  std::uint64_t v4FinishNanos = 0u;
};

class PreparedPhaseScope {
 public:
  using Field = std::uint64_t PreparedPhaseTiming::*;

  PreparedPhaseScope(PreparedPhaseTiming* timing, Field field) noexcept
      : timing_(timing), field_(field),
        begin_(timing ? std::chrono::steady_clock::now()
                      : std::chrono::steady_clock::time_point{}) {}

  ~PreparedPhaseScope() noexcept {
    if (!timing_) return;
    const auto end = std::chrono::steady_clock::now();
    timing_->*field_ += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin_)
            .count());
  }

  PreparedPhaseScope(const PreparedPhaseScope&) = delete;
  PreparedPhaseScope& operator=(const PreparedPhaseScope&) = delete;

 private:
  PreparedPhaseTiming* timing_;
  Field field_;
  std::chrono::steady_clock::time_point begin_;
};

// Cold prepared-V4 only. The existing supply sink owns native CPU clocks and
// bounded storage; no per-record hook, file output or extra renderer effect.
// Phase 0: original armed event interval; 1: raw import/resolve/replay;
// 2: observed renderer replay call; 3: Present readback/oracle.
class PreparedReplayCpuScope {
 public:
  PreparedReplayCpuScope(bool requested, const void* owner,
                        std::uint64_t id, std::uint64_t phase) noexcept
      : enabled_(requested && ::dxmt9::supply::enabled()),
        owner_(owner), id_(id), phase_(phase) {
    if (enabled_) ::dxmt9::supply::record(
        ::dxmt9::supply::Event::PreparedReplayPhaseBegin, owner_, id_, phase_);
  }
  ~PreparedReplayCpuScope() noexcept {
    if (enabled_) ::dxmt9::supply::record(
        ::dxmt9::supply::Event::PreparedReplayPhaseEnd, owner_, id_, phase_);
  }
  PreparedReplayCpuScope(const PreparedReplayCpuScope&) = delete;
  PreparedReplayCpuScope& operator=(const PreparedReplayCpuScope&) = delete;
 private:
  bool enabled_;
  const void* owner_;
  std::uint64_t id_, phase_;
};

class PreparedExecutionV4CancelScope {
 public:
  explicit PreparedExecutionV4CancelScope(RenderTapeExecutionV4* execution) noexcept
      : execution_(execution) {}
  ~PreparedExecutionV4CancelScope() noexcept {
    if (execution_) execution_->cancel();
  }
  PreparedExecutionV4CancelScope(const PreparedExecutionV4CancelScope&) = delete;
  PreparedExecutionV4CancelScope& operator=(const PreparedExecutionV4CancelScope&) = delete;

 private:
  RenderTapeExecutionV4* execution_;
};

std::uint64_t elapsedNanos(std::chrono::steady_clock::time_point begin,
                           std::chrono::steady_clock::time_point end) noexcept;

void auditInitialSeedMutations(
    D9CDevice* device, std::span<const Definition> definitions,
    std::span<const RenderTapeResourceMutationHeader> mutations,
    std::vector<OwnedObject>& objects,
    std::span<const RenderTapeProviderBlob> blobs,
    RenderTapeInitialSeedAuditEvidence& evidence) noexcept {
  const auto started = std::chrono::steady_clock::now();
  evidence.attempted = true;
  try {
    auto upper = device ? device->dev().upperDevice() : nullptr;
    bool ready = device && upper &&
        drainDeferredReplay(device, "render-tape-initial-seed-audit");
    if (ready) {
      // DEFAULT render-target uploads are deferred in Initializer, not in the
      // ordinary submission queue. A queue flush alone leaves those seeds
      // pending until encodeChunk's preamble, after this diagnostic readback.
      const auto uploaded = upper->queue().flushInitializerUploads();
      ready = uploaded.value == 0u ||
          (uploaded.event &&
           WMT::SharedEvent{uploaded.event.handle}.waitUntilSignaledValue(
               uploaded.value, /*timeout-ns*/ 1'000'000'000u));
    }
    if (!ready) {
      for (const auto& mutation : mutations) {
        evidence.entries.push_back({
            .kind = RenderTapeInitialSeedAuditKind::Color,
            .identity = mutation.identity,
            .subresource = mutation.subresource,
            .expectedDigest = mutation.digest,
            .failure = RenderTapeInitialSeedAuditFailure::CaptureFailed,
        });
        ++evidence.captureFailedCount;
      }
      evidence.count = static_cast<std::uint32_t>(evidence.entries.size());
      evidence.complete = false;
      evidence.wallNanos = elapsedNanos(started, std::chrono::steady_clock::now());
      return;
    }
    // Deliberately perturbative: both deferred initialization and ordinary
    // submissions must precede resource readback.
    upper->flush();
    for (const auto& mutation : mutations) {
      RenderTapeInitialSeedAuditEntry entry{
          .kind = RenderTapeInitialSeedAuditKind::Color,
          .identity = mutation.identity,
          .subresource = mutation.subresource,
          .expectedDigest = mutation.digest,
      };
      const auto definition = std::find_if(
          definitions.begin(), definitions.end(), [&](const auto& value) {
            return sameIdentity(value.fixed.identity, mutation.identity);
          });
      const auto* blob = findBlob(blobs, mutation.digest);
      void* object = render_tape_runtime::findExactObject(objects, mutation.identity);
      if (!blob) {
        entry.failure = RenderTapeInitialSeedAuditFailure::BlobUnavailable;
        ++evidence.captureFailedCount;
        evidence.entries.push_back(std::move(entry));
        continue;
      }
      if (definition == definitions.end() || !object) {
        entry.failure = RenderTapeInitialSeedAuditFailure::ObjectUnavailable;
        ++evidence.captureFailedCount;
        evidence.entries.push_back(std::move(entry));
        continue;
      }

      RenderTapeSurfaceDescriptorV2 surface{};
      RenderTapeTextureDescriptorV2 texture{};
      D9CSurfaceDesc surfaceDesc{};
      std::vector<std::byte> actual;
      bool captured = false;
      if (mutation.identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
        entry.kind = RenderTapeInitialSeedAuditKind::Color;
        const bool supported =
            renderTapeArmColorSnapshotTextureSupported(definition->descriptor) &&
            renderTapeTextureSubresourceDescriptor(
                definition->descriptor, mutation.subresource, surfaceDesc) &&
            renderTapeLoadTextureDescriptorV2(definition->descriptor, texture) &&
            mutation.byteOffset == 0u &&
            static_cast<RenderTapeInitialContentDisposition>(
                texture.initialContentDisposition) ==
                RenderTapeInitialContentDisposition::CompleteSeed;
        entry.supported = supported;
        if (supported) {
          auto* level = dxmt9c_texture_get_surface_level(
              static_cast<D9CTexture*>(object), mutation.subresource);
          if (level) {
            captured = readbackTightSurface(
                device, level, surfaceDesc.width, surfaceDesc.height,
                surfaceDesc.format, actual);
            dxmt9c_surface_release(level);
          }
        }
      } else if (mutation.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE &&
                 renderTapeLoadSurfaceDescriptorV2(definition->descriptor,
                                                   surface)) {
        surfaceDesc = surface.surface;
        const auto disposition = static_cast<RenderTapeInitialContentDisposition>(
            surface.initialContentDisposition);
        if (disposition == RenderTapeInitialContentDisposition::CompleteDepthFloat32V1) {
          entry.kind = RenderTapeInitialSeedAuditKind::Depth;
          entry.supported = renderTapeSnapshotStandaloneD24X8Supported(
              surface.surface);
          if (entry.supported && upper) {
            core::CanonicalD24X8Depth capturedDepth;
            captured = upper->captureCanonicalD24X8Depth(
                static_cast<D9CSurface*>(object)->obj->handle(), capturedDepth);
            if (captured) actual.assign(
                reinterpret_cast<const std::byte*>(capturedDepth.bytes.data()),
                reinterpret_cast<const std::byte*>(capturedDepth.bytes.data()) +
                    capturedDepth.bytes.size());
          }
        } else if (disposition == RenderTapeInitialContentDisposition::
                                      CompleteDepthStencilFloat32UInt8V1) {
          entry.kind = RenderTapeInitialSeedAuditKind::Depth;
          RenderTapeD24S8SnapshotLayout layout{};
          entry.supported = surface.storage == static_cast<std::uint32_t>(
                                                  RenderTapeSurfaceStorage::Standalone) &&
              renderTapeDeriveD24S8SnapshotLayout(surface.surface, layout);
          if (entry.supported && upper) {
            core::CanonicalD24S8DepthStencil checkpoint;
            captured = upper->captureCanonicalD24S8DepthStencil(
                static_cast<D9CSurface*>(object)->obj->handle(), checkpoint);
            if (captured) actual.assign(
                reinterpret_cast<const std::byte*>(checkpoint.bytes.data()),
                reinterpret_cast<const std::byte*>(checkpoint.bytes.data()) + checkpoint.bytes.size());
          }
        } else if (disposition == RenderTapeInitialContentDisposition::CompleteSeed) {
          entry.kind = RenderTapeInitialSeedAuditKind::Color;
          entry.supported =
              (surface.storage == static_cast<std::uint32_t>(
                                      RenderTapeSurfaceStorage::SwapchainBackbuffer)
                   ? renderTapeArmColorSnapshotSwapchainSurfaceSupported(
                         surface.surface)
                   : renderTapeArmColorSnapshotStandaloneSurfaceSupported(
                         surface.surface));
          if (entry.supported) {
            captured = readbackTightSurface(
                device, static_cast<D9CSurface*>(object), surfaceDesc.width,
                surfaceDesc.height, surfaceDesc.format, actual);
          }
        }
      }
      if (!entry.supported) {
        entry.failure = RenderTapeInitialSeedAuditFailure::Unsupported;
        ++evidence.unsupportedCount;
      } else if (!captured) {
        entry.failure = RenderTapeInitialSeedAuditFailure::CaptureFailed;
        ++evidence.supportedCount;
        ++evidence.captureFailedCount;
      } else {
        ++evidence.supportedCount;
        entry.bytes = actual.size();
        entry.actualDigest = RenderTapeCaptureSession::sha256(actual);
        entry.match = entry.bytes == blob->bytes.size() &&
                      sameDigest(entry.actualDigest, entry.expectedDigest);
        entry.failure = entry.match ? RenderTapeInitialSeedAuditFailure::None
                                    : RenderTapeInitialSeedAuditFailure::DigestMismatch;
        if (entry.match) {
          ++evidence.matched;
        } else {
          ++evidence.mismatchedCount;
        }
      }
      evidence.entries.push_back(std::move(entry));
    }
    evidence.count = static_cast<std::uint32_t>(evidence.entries.size());
    // `complete` means every requested entry was inspected. It is diagnostic
    // coverage only; callers must still use `matched` and the normal output
    // oracle for qualification.
    evidence.complete = true;
  } catch (...) {
    evidence.complete = false;
  }
  evidence.count = static_cast<std::uint32_t>(evidence.entries.size());
  evidence.wallNanos = elapsedNanos(started, std::chrono::steady_clock::now());
}

bool validateTextureInputAudit(
    const RenderTapeTextureInputAuditRequest& request,
    const GeneralPreflightPlan& plan, const CaptureSourceReplayPlan& sources,
    RenderTapeTextureInputAuditEvidence& evidence) noexcept {
  evidence.requested = true;
  evidence.identity = request.identity;
  evidence.afterCapturedSeqId = request.afterCapturedSeqId;
  evidence.byteCount = request.bytes.size();
  evidence.inputDigest = request.digest;
  if (request.identity.kind != D9C_CHUNK_HANDLE_KIND_TEXTURE ||
      request.identity.generation == 0u || request.identity.objectId == 0u ||
      request.afterCapturedSeqId == 0u || request.bytes.empty() ||
      request.bytes.size() > 64u * 1024u * 1024u ||
      !nonZeroDigest(request.digest) ||
      !sameDigest(request.digest, RenderTapeCaptureSession::sha256(request.bytes)))
    return false;
  const auto row = std::find_if(sources.rows().begin(), sources.rows().end(),
      [&](const auto& value) { return value.capturedSeqId == request.afterCapturedSeqId; });
  if (row == sources.rows().end() || row + 1 == sources.rows().end()) return false;
  bool alive = false;
  for (const auto& event : plan.events) {
    if (plan.tape.event(event.eventIndex).header.ordinal > row->eventOrdinal) break;
    if (event.type == RenderTapeEventType::ObjectDefine &&
        event.index < plan.definitions.size() &&
        sameIdentity(plan.definitions[event.index].fixed.identity, request.identity))
      alive = true;
    if (event.type == RenderTapeEventType::ObjectDestroy &&
        event.index < plan.destroys.size() &&
        sameIdentity(plan.destroys[event.index].identity, request.identity))
      alive = false;
  }
  if (!alive) return false;
  const auto definition = std::find_if(plan.definitions.begin(), plan.definitions.end(),
      [&](const auto& value) { return sameIdentity(value.fixed.identity, request.identity); });
  if (definition == plan.definitions.end()) return false;
  RenderTapeTextureDescriptorV2 texture{};
  D9CSurfaceDesc surface{};
  if (!renderTapeLoadTextureDescriptorV2(definition->descriptor, texture) ||
      texture.dimension != static_cast<std::uint32_t>(RenderTapeTextureDimension::Texture2D) ||
      texture.mipLevelCount != 1u || texture.subresourceCount != 1u ||
      !renderTapeTextureSubresourceDescriptor(definition->descriptor, 0u, surface) ||
      surface.format != render_tape_d3d_format::R32F ||
      surface.width == 0u || surface.height == 0u ||
      surface.width > 4096u || surface.height > 4096u ||
      request.bytes.size() != std::uint64_t{surface.width} * surface.height * 4u)
    return false;
  evidence.validated = true;
  return true;
}

bool applyTextureInputAudit(
    D9CDevice* device, const RenderTapeTextureInputAuditRequest& request,
    const std::vector<Definition>& definitions, std::vector<OwnedObject>& objects,
    RenderTapeTextureInputAuditEvidence& evidence) noexcept {
  if (!evidence.validated || evidence.attempted) return false;
  evidence.attempted = true;
  try {
    const auto definition = std::find_if(definitions.begin(), definitions.end(),
        [&](const auto& value) { return sameIdentity(value.fixed.identity, request.identity); });
    auto* object = static_cast<D9CTexture*>(
        render_tape_runtime::findExactObject(objects, request.identity));
    auto upper = device ? device->dev().upperDevice() : nullptr;
    D9CSurfaceDesc desc{};
    if (!upper || !object || definition == definitions.end() ||
        !renderTapeTextureSubresourceDescriptor(definition->descriptor, 0u, desc) ||
        !drainDeferredReplay(device, "render-tape-texture-input-audit")) return false;
    auto release = [](D9CSurface* value) { if (value) dxmt9c_surface_release(value); };
    std::unique_ptr<D9CSurface, decltype(release)> surface(
        dxmt9c_texture_get_surface_level(object, 0u), release);
    if (!surface) return false;
    upper->flush();
    std::vector<std::byte> before, after;
    if (!readbackTightSurface(device, surface.get(), desc.width, desc.height,
                             desc.format, before)) return false;
    evidence.beforeDigest = RenderTapeCaptureSession::sha256(before);
    const RenderTapeProviderBlob blob{request.digest, request.bytes};
    RenderTapeResourceMutationHeader mutation{};
    mutation.identity = request.identity;
    mutation.kind = static_cast<std::uint32_t>(RenderTapeMutationKind::Upload);
    mutation.byteSize = request.bytes.size();
    mutation.digest = request.digest;
    evidence.uploaded = applyGeneralMutation(device, mutation,
        std::span(&blob, 1u), definitions, objects);
    if (!evidence.uploaded) return false;
    const auto uploaded = upper->queue().flushInitializerUploads();
    if (uploaded.value != 0u && (!uploaded.event ||
        !WMT::SharedEvent{uploaded.event.handle}.waitUntilSignaledValue(
            uploaded.value, 1'000'000'000u))) return false;
    upper->flush();
    if (!readbackTightSurface(device, surface.get(), desc.width, desc.height,
                             desc.format, after)) return false;
    evidence.afterDigest = RenderTapeCaptureSession::sha256(after);
    evidence.readbackMatched = after.size() == request.bytes.size() &&
        std::equal(after.begin(), after.end(), request.bytes.begin()) &&
        sameDigest(evidence.afterDigest, request.digest);
    return evidence.readbackMatched;
  } catch (...) {
    return false;
  }
}

FrameTapeReplayResult& replayGeneralPlan(
    D9CDevice* device, const GeneralPreflightPlan& plan,
    FrameTapeReplayResult& result,
    std::span<const RenderTapeProviderBlob> blobs,
    ProviderReplayPolicy policy = ProviderReplayPolicy::Historical,
    PreparedPhaseTiming* timing = nullptr,
    RenderTapeInitialSeedAuditEvidence* initialSeedAudit = nullptr,
    RenderTapeFreshSeedDrainEvidence* freshSeedDrainAudit = nullptr,
    RenderTapeTexturePrestageEvidence* texturePrestageAudit = nullptr,
    const SourceBoundaryReplayPlan* sourceBoundaries = nullptr,
    RenderTapeSourceBoundaryEvidence* sourceBoundaryAudit = nullptr,
    const CaptureSourceReplayPlan* sourcePolicy = nullptr,
    const RenderTapeTextureInputAuditRequest* textureInput = nullptr,
    RenderTapeTextureInputAuditEvidence* textureInputEvidence = nullptr,
    const CaptureSourceReplayPlanV3* sourcePolicyV3 = nullptr,
    std::span<const std::byte> sourcePolicyTapeV3 = {},
    const CaptureSourceReplayPlanV4* exactDiagnosticV4 = nullptr,
    std::span<const std::byte> exactDiagnosticTapeV4 = {},
    const RenderTapeInitialSetupV4Plan* initialSetupV4 = nullptr,
    RenderTapeExecutionV4ProducerContract producerContractV4 =
        RenderTapeExecutionV4ProducerContract::None,
    RenderTapeExecutionV4PublicationMode publicationModeV4 =
        RenderTapeExecutionV4PublicationMode::ExactSourceDiagnostic,
    RenderTapePreparedObserverV4* observerV4 = nullptr,
    RenderTapeExecutionV4* admittedExecutionV4 = nullptr,
    const NormalHistorySetup* normalHistory = nullptr) {
  if (!device) {
    result.status = FrameTapeReplayStatus::ObjectCreationFailed;
    if (exactDiagnosticV4)
      result.exactDiagnosticV4.failure = "v4-provider-device-missing";
    if (observerV4)
      result.preparedObserverV4.failure = "v4-observer-device-missing";
    return result;
  }
  RenderTapeExecutionV3 executionV3;
  RenderTapeExecutionV4 localExecutionV4;
  // Keep cancellation at the old local executor's destruction tier, after
  // later replay locals unwind. Explicit cleanup still cancels before flush
  // and resource retirement on the ordinary completion/failure paths.
  PreparedExecutionV4CancelScope borrowedCancellation(admittedExecutionV4);
  auto& executionV4 = admittedExecutionV4 ? *admittedExecutionV4 : localExecutionV4;
  if (admittedExecutionV4 && !exactDiagnosticV4) {
    result.exactDiagnosticV4.failure = "v4-prepared-admission-plan-missing";
    result.status = FrameTapeReplayStatus::InvalidTape;
    return result;
  }
  if (sourcePolicyV3 && !executionV3.admit(*sourcePolicyV3, plan, sourcePolicyTapeV3,
                                         result.sourceReplayV3)) {
    result.status = FrameTapeReplayStatus::InvalidTape;
    return result;
  }
  if (exactDiagnosticV4) {
    bool admitted = false;
    if (admittedExecutionV4) {
      admitted = RenderTapeExecutionV4PreparedReplayAccess::consume(executionV4,
          *exactDiagnosticV4, plan, exactDiagnosticTapeV4,
          result.exactDiagnosticV4.execution, publicationModeV4, producerContractV4);
    } else {
      PreparedPhaseScope phase(timing,
                               &PreparedPhaseTiming::v4ReplayAdmissionNanos);
      admitted = executionV4.admit(*exactDiagnosticV4, plan,
          exactDiagnosticTapeV4, result.exactDiagnosticV4.execution,
          publicationModeV4, producerContractV4);
    }
    if (!admitted) {
      result.exactDiagnosticV4.failure = executionV4.failure();
      result.status = FrameTapeReplayStatus::InvalidTape;
      return result;
    }
  }
  if (exactDiagnosticV4 && texturePrestageAudit) {
    result.exactDiagnosticV4.failure = "v4-initial-setup-audit-conflict";
    result.status = FrameTapeReplayStatus::UnsupportedGrammar;
    return result;
  }
  if (exactDiagnosticV4 && (!initialSetupV4 || !initialSetupV4->valid() ||
      initialSetupV4->firstIntervalEvent() != executionV4.snapshotPrefixEvents() ||
      !sameDigest(initialSetupV4->sourcePlanDigest(), exactDiagnosticV4->digest()))) {
    result.exactDiagnosticV4.failure = "v4-initial-setup-plan-binding";
    result.status = FrameTapeReplayStatus::InvalidTape;
    return result;
  }
  if (normalHistory) {
    result.normalDiagnostic.requested = true;
    const auto upper = device->dev().upperDevice();
    const auto profile = upper ? upper->queue().renderTapeActionProfileSnapshot()
                               : dxmt9::RenderTapeActionProfileSnapshot{};
    if (!exactDiagnosticV4 || !profile.valid || profile.constructionProfile ||
        profile.currentProfile || profile.revision) {
      result.exactDiagnosticV4.failure = "v4-normal-construction-policy";
      result.status = FrameTapeReplayStatus::InvalidTape;
      return result;
    }
    result.normalDiagnostic.admitted = true;
    result.normalDiagnostic.requiredSurfaces = normalHistory->surfaces.size();
  }
  if (result.queryReplay.present) {
    const auto upper = device->dev().upperDevice();
    if (!upper || !upper->pool()) {
      result.queryReplay.failure = RenderTapeQueryReplayFailure::BackendUnavailable;
      result.status = FrameTapeReplayStatus::QueryOracleUnsupported;
      return result;
    }
    for (const auto& definition : plan.definitions) {
      if (definition.fixed.identity.kind != D9C_CHUNK_HANDLE_KIND_QUERY) continue;
      RenderTapeQueryDescriptorDecoded query{};
      if (!renderTapeLoadQueryDescriptor(definition.descriptor, query) ||
          (query.type == 9u && !upper->usesMetalOcclusion())) {
        result.queryReplay.failure = RenderTapeQueryReplayFailure::BackendUnavailable;
        result.status = FrameTapeReplayStatus::QueryOracleUnsupported;
        return result;
      }
    }
  }
  if (const auto upper = device->dev().upperDevice();
      upper && !upper->usesAsyncQueryFences()) {
    // The synchronous query-reference lane exposes only initializer signal
    // progress, not command-buffer success. It cannot qualify a GPU seed.
    const bool seededBackbuffer = std::any_of(
        plan.definitions.begin(), plan.definitions.end(), [](const auto& definition) {
          RenderTapeSurfaceDescriptorV2 surface{};
          return definition.fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE &&
              renderTapeLoadSurfaceDescriptorV2(definition.descriptor, surface) &&
              surface.storage == static_cast<std::uint32_t>(
                  RenderTapeSurfaceStorage::SwapchainBackbuffer) &&
              surface.initialContentDisposition == static_cast<std::uint32_t>(
                  RenderTapeInitialContentDisposition::CompleteSeed);
        });
    if (seededBackbuffer) {
      result.status = result.queryReplay.present
          ? FrameTapeReplayStatus::QueryOracleUnsupported
          : FrameTapeReplayStatus::UnsupportedGrammar;
      result.queryReplay.failure = RenderTapeQueryReplayFailure::BackendUnavailable;
      return result;
    }
  }
  if (sourcePolicy) {
    try {
      // Reserve on the final result owner before any queue publication effect.
      // The settlement callback must never be the first allocator.
      result.sourceReplay.publications.reserve(kMaxSourceReplayPublications);
    } catch (...) {
      result.status = FrameTapeReplayStatus::ObjectCreationFailed;
      return result;
    }
  }
  std::vector<OwnedObject> objects;
  std::vector<Definition> definitions;
  std::vector<Chunk> bootstrap;
  std::vector<void*> resolved;
  struct QueryState {
    D9CWireObjectIdentity identity{};
    RenderTapeQueryCycleState cycle{};
    std::uint64_t localQueryIdentity = 0u;
    std::uint64_t localIssueOrdinal = 0u;
    std::uint64_t localDeviceEpoch = 0u;
  };
  struct QueryObligation {
    std::size_t rowIndex = 0u;
    core::QueryPollSnapshot snapshot{};
  };
  std::vector<QueryState> queryStates;
  std::vector<QueryObligation> queryObligations;
  struct CheckpointQuery {
    OwnedObject object{};
    std::size_t evidenceIndex = 0u;
    D9CQuery* query = nullptr; // Stable during setup/arm, including prefix activation.
    core::QueryPollSnapshot snapshot{};
  };
  std::vector<CheckpointQuery> checkpointQueries;
  if (result.queryReplay.present) {
    // No transcript or result-owner storage is first allocated after an
    // original polling call has already produced an externally visible effect.
    try {
      queryStates.reserve(result.queryReplay.queryDefinitions);
      queryObligations.reserve(result.queryReplay.recordedPolls);
      result.queryReplay.rows.reserve(result.queryReplay.recordedPolls);
      result.queryReplay.checkpoints.reserve(result.queryReplay.checkpointDefinitions);
      checkpointQueries.reserve(result.queryReplay.checkpointDefinitions);
      objects.reserve(plan.definitions.size());
      definitions.reserve(plan.definitions.size());
    } catch (...) {
      result.status = FrameTapeReplayStatus::ObjectCreationFailed;
      return result;
    }
  }
  std::shared_ptr<OffscreenPresentOutput> output;
  D9CSurface* readbackTarget = nullptr;
  bool bootstrapReplayed = false;
  bool outputInstalled = false;
  bool sawSubmittedCommandWork = false;
  std::uint64_t commandEpoch = 0u;
  std::vector<std::uint64_t> definitionEpochs;
  std::vector<RenderTapeResourceMutationHeader> initialSeedMutations;
  bool initialSeedAuditDone = false;
  std::vector<SeededSubresource> seededSubresources;
  std::vector<OwnedObject> setupHiddenObjects;
  std::vector<Definition> setupDefinitions;
  std::vector<RenderTapeInitialSetupV4OwnerState> setupOwnerStates;
  std::vector<OwnedObject> normalHiddenAliases;
  std::vector<RenderTapeInitialSetupV4OwnerState> normalAliasStates;
  struct PrefixBufferSeedV4 {
    std::uint32_t definitionIndex = 0u, definitionEventIndex = 0u;
    std::uint32_t mutationIndex = 0u, mutationEventIndex = 0u;
    bool uploadReplayed = false, bytesVerified = false, checkpointCompleted = false;
  };
  std::vector<PrefixBufferSeedV4> prefixBufferSeeds;
  if (exactDiagnosticV4) {
    try {
      const auto entries = initialSetupV4->entries();
      setupHiddenObjects.resize(entries.size());
      setupDefinitions.resize(entries.size());
      setupOwnerStates.resize(entries.size());
      objects.reserve(plan.definitions.size());
      definitions.reserve(plan.definitions.size());
      seededSubresources.reserve(plan.mutations.size());
      if (normalHistory) {
        normalHiddenAliases.resize(normalHistory->lateAliases.size());
        normalAliasStates.resize(normalHistory->lateAliases.size());
        for (std::size_t i = 0u; i < normalHiddenAliases.size(); ++i) {
          normalHiddenAliases[i].identity = plan.definitions[normalHistory->lateAliases[i]].fixed.identity;
          normalAliasStates[i].mode = RenderTapeInitialSetupV4Mode::AllocationOnly;
        }
      }
      prefixBufferSeeds.reserve(plan.definitions.size());
      for (std::size_t i = 0u; i < entries.size(); ++i) {
        setupHiddenObjects[i].identity = entries[i].definition.identity;
        setupDefinitions[i] = {entries[i].definition, entries[i].descriptor};
        setupOwnerStates[i].seedCount = entries[i].seedCount;
        setupOwnerStates[i].mode = entries[i].mode;
      }
      // Retain original full prefix Upload witnesses, not the generic seeded
      // tail, which also records later partial writes. Physical byte proof and
      // checked completion are supplied only at the real pre-arm boundary.
      const auto prefix = executionV4.snapshotPrefixEvents();
      for (std::size_t i = 0u; i < plan.events.size(); ++i) {
        const auto& event = plan.events[i];
        if (event.eventIndex >= prefix) break;
        if (event.type != RenderTapeEventType::ObjectDefine ||
            event.index >= plan.definitions.size()) continue;
        const auto& definition = plan.definitions[event.index];
        D9CBufferDesc descriptor{};
        if (definition.fixed.identity.kind != D9C_CHUNK_HANDLE_KIND_BUFFER ||
            definition.descriptor.size() != sizeof(descriptor) ||
            !load(definition.descriptor, 0u, descriptor) || !descriptor.size ||
            definition.fixed.expectedContentCount != 1u ||
            definition.fixed.expectedContentBytes != descriptor.size) continue;
        std::size_t uploadPosition = plan.events.size();
        bool complete = true;
        for (std::size_t j = i + 1u; j < plan.events.size(); ++j) {
          const auto& candidate = plan.events[j];
          if (candidate.eventIndex >= prefix) break;
          if (candidate.type != RenderTapeEventType::ResourceMutation ||
              candidate.index >= plan.mutations.size()) continue;
          const auto& seed = plan.mutations[candidate.index];
          if (!sameIdentity(definition.fixed.identity, seed.identity)) continue;
          if (uploadPosition != plan.events.size() ||
              seed.kind != static_cast<std::uint32_t>(RenderTapeMutationKind::Upload) ||
              seed.bufferDisposition != static_cast<std::uint32_t>(
                  RenderTapeBufferMutationDisposition::Plain) ||
              seed.subresource != 0u || seed.byteOffset != 0u ||
              seed.byteSize != descriptor.size) {
            complete = false;
            break;
          }
          uploadPosition = j;
        }
        if (!complete || uploadPosition == plan.events.size()) continue;
        const auto& uploadEvent = plan.events[uploadPosition];
        prefixBufferSeeds.push_back({event.index, event.eventIndex,
                                    uploadEvent.index, uploadEvent.eventIndex});
      }
    } catch (...) {
      result.exactDiagnosticV4.failure = "v4-initial-setup-allocation-failed";
      result.status = FrameTapeReplayStatus::ObjectCreationFailed;
      return result;
    }
  }
  if (freshSeedDrainAudit) {
    definitionEpochs.reserve(plan.definitions.size());
    freshSeedDrainAudit->attempted = true;
    freshSeedDrainAudit->mutationCount = static_cast<std::uint32_t>(
        plan.mutations.size());
    freshSeedDrainAudit->complete = plan.mutations.empty();
  }
  std::vector<TexturePrestageEntry> prestaged;
  std::vector<Definition> prestageDefinitions;
  std::vector<OwnedObject> hiddenObjects;
  if (texturePrestageAudit) {
    texturePrestageAudit->attempted = true;
    try {
      objects.reserve(plan.definitions.size());
      definitions.reserve(plan.definitions.size());
      prestaged.reserve(plan.definitions.size());
      prestageDefinitions.reserve(plan.definitions.size());
      hiddenObjects.reserve(plan.definitions.size());
      std::vector<std::uint32_t> commandPositions;
      for (std::size_t position = 0u; position < plan.events.size(); ++position) {
        if (plan.events[position].type == RenderTapeEventType::CommandChunk)
          commandPositions.push_back(static_cast<std::uint32_t>(position));
      }
      for (std::size_t definitionIndex = 0u;
           definitionIndex < plan.definitions.size(); ++definitionIndex) {
        const auto& definition = plan.definitions[definitionIndex];
        if (definition.fixed.identity.kind != D9C_CHUNK_HANDLE_KIND_TEXTURE)
          continue;
        bool hasSurfaceAlias = false;
        bool hasSlotGenerationConflict = false;
        for (const auto& other : plan.definitions) {
          if (other.fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE) {
            RenderTapeSurfaceDescriptorV2 surface{};
            if (renderTapeLoadSurfaceDescriptorV2(other.descriptor, surface) &&
                surface.storage == static_cast<std::uint32_t>(
                    RenderTapeSurfaceStorage::TextureSubresource) &&
                sameIdentity(surface.parentTexture,
                             definition.fixed.identity)) {
              hasSurfaceAlias = true;
            }
          }
          if (other.fixed.identity.kind == definition.fixed.identity.kind &&
              other.fixed.identity.objectId ==
                  definition.fixed.identity.objectId &&
              other.fixed.identity.generation !=
                  definition.fixed.identity.generation) {
            hasSlotGenerationConflict = true;
          }
        }
        const auto definitionEvent = std::find_if(
            plan.events.begin(), plan.events.end(), [&](const auto& event) {
              return event.type == RenderTapeEventType::ObjectDefine &&
                     event.index == definitionIndex;
            });
        if (definitionEvent == plan.events.end()) continue;
        std::vector<std::uint32_t> seedPositions;
        for (std::size_t eventPosition = 0u;
             eventPosition < plan.events.size(); ++eventPosition) {
          const auto& event = plan.events[eventPosition];
          if (event.type == RenderTapeEventType::ResourceMutation &&
              event.index < plan.mutations.size() &&
              sameIdentity(plan.mutations[event.index].identity,
                           definition.fixed.identity)) {
            seedPositions.push_back(static_cast<std::uint32_t>(eventPosition));
          }
        }
        const auto definitionPosition = static_cast<std::uint32_t>(
            definitionEvent - plan.events.begin());
        const bool orderAllowed = renderTapeTexturePrestageOrderAllowed(
            definitionPosition, seedPositions, commandPositions);
        if (!renderTapeTexturePrestageCandidate(
                definition.fixed, definition.descriptor, plan.mutations,
                hasSurfaceAlias, hasSlotGenerationConflict, !orderAllowed)) {
          continue;
        }
        RenderTapeTextureDescriptorV2 texture{};
        if (!renderTapeLoadTextureDescriptorV2(definition.descriptor,
                                               texture)) continue;
        TexturePrestageEntry entry;
        entry.definition = definition;
        entry.seedDigests.resize(texture.subresourceCount);
        entry.consumed.resize(texture.subresourceCount, false);
        bool completeSeeds = true;
        for (const auto& mutation : plan.mutations) {
          if (!sameIdentity(mutation.identity, definition.fixed.identity))
            continue;
          const auto* blob = findBlob(blobs, mutation.digest);
          if (mutation.subresource >= entry.seedDigests.size() || !blob ||
              blob->bytes.size() != mutation.byteSize) {
            completeSeeds = false;
            break;
          }
          entry.seedDigests[mutation.subresource] = mutation.digest;
        }
        if (!completeSeeds) continue;
        prestageDefinitions.push_back(entry.definition);
        prestaged.push_back(std::move(entry));
        ++texturePrestageAudit->candidateObjects;
        texturePrestageAudit->candidateSubresources += texture.subresourceCount;
        for (const auto& digest : prestaged.back().seedDigests) {
          const auto* blob = findBlob(blobs, digest);
          if (!blob) {
            texturePrestageAudit->candidateObjects--;
            texturePrestageAudit->candidateSubresources -=
                texture.subresourceCount;
            prestaged.pop_back();
            prestageDefinitions.pop_back();
            completeSeeds = false;
            break;
          }
          texturePrestageAudit->candidateBytes += blob->bytes.size();
        }
        if (!completeSeeds) continue;
      }
    } catch (...) {
      texturePrestageAudit->complete = false;
      result.status = FrameTapeReplayStatus::ObjectCreationFailed;
      return result;
    }
    texturePrestageAudit->complete =
        texturePrestageAudit->candidateSubresources == 0u;
  }
  render_tape_runtime::PreparedObserverV4Session observerSession(
      observerV4, result.preparedObserverV4, device);
  const auto notifyBuffer = [&](RenderTapePreparedBufferPhaseV4 phase,
      std::uint32_t eventIndex, const Definition& definition,
      const RenderTapeResourceMutationHeader* mutation, void* value) {
    if (!observerSession.enabled() ||
        definition.fixed.identity.kind != D9C_CHUNK_HANDLE_KIND_BUFFER || !value) return;
    observerSession.buffer({
        .phase = phase, .eventIndex = eventIndex,
        .eventOrdinal = eventIndex == kRenderTapeIdentityV4NoIndex
            ? 0u : plan.tape.event(eventIndex).header.ordinal,
        .definition = &definition.fixed, .descriptor = definition.descriptor,
        .mutation = mutation, .buffer = static_cast<D9CBuffer*>(value)});
  };
  const auto notifyActiveBuffer = [&](RenderTapePreparedBufferPhaseV4 phase,
      std::uint32_t eventIndex, const D9CWireObjectIdentity& identity,
      const RenderTapeResourceMutationHeader* mutation, void* value) {
    if (!observerSession.enabled() || identity.kind != D9C_CHUNK_HANDLE_KIND_BUFFER) return;
    const auto definition = std::find_if(plan.definitions.begin(), plan.definitions.end(),
        [&](const auto& candidate) { return sameIdentity(candidate.fixed.identity, identity); });
    if (definition != plan.definitions.end())
      notifyBuffer(phase, eventIndex, *definition, mutation, value);
    else observerSession.reject("v4-observer-definition-unavailable");
  };
  auto replayUpper = device->dev().upperDevice();
  result.queryReplay.actualMetal = result.queryReplay.present &&
      replayUpper && replayUpper->pool();
  const bool priorExactAttachmentPreservation =
      replayUpper &&
      replayUpper->queue().renderTapeExactAttachmentPreservation();
  if (replayUpper && !normalHistory) {
    replayUpper->queue().setRenderTapeExactAttachmentPreservation(true);
  }
  const auto findPrestage = [&](const D9CWireObjectIdentity& identity)
      -> TexturePrestageEntry* {
    const auto it = std::find_if(
        prestaged.begin(), prestaged.end(), [&](const auto& entry) {
          return sameIdentity(entry.definition.fixed.identity, identity);
        });
    return it == prestaged.end() ? nullptr : &*it;
  };
  const auto findSetupOwner = [&](const D9CWireObjectIdentity& identity) {
    const auto it = std::find_if(setupHiddenObjects.begin(),
        setupHiddenObjects.end(), [&](const auto& owner) {
          return sameIdentity(owner.identity, identity);
        });
    return static_cast<std::size_t>(it - setupHiddenObjects.begin());
  };
  const auto releaseSetupState = [&](std::size_t ownerIndex) {
    if (ownerIndex < setupOwnerStates.size() &&
        !renderTapeInitialSetupV4Advance(setupOwnerStates[ownerIndex],
            RenderTapeInitialSetupV4Action::Release)) {
      // Invariant rejection cannot prevent recovery from releasing the sole
      // physical reference. It does prevent qualifying this execution.
      result.exactDiagnosticV4.failure = "v4-initial-setup-release-state";
      result.status = FrameTapeReplayStatus::ObjectCreationFailed;
    }
  };
  const auto releaseNormalAliasState = [&](const D9CWireObjectIdentity& identity) {
    const auto alias = std::find_if(normalHiddenAliases.begin(), normalHiddenAliases.end(),
        [&](const auto& value) { return sameIdentity(value.identity, identity); });
    if (alias == normalHiddenAliases.end()) return true;
    return renderTapeInitialSetupV4Advance(normalAliasStates[alias - normalHiddenAliases.begin()],
        RenderTapeInitialSetupV4Action::Release);
  };
  const auto cleanup = [&] {
    if (exactDiagnosticV4 && !result.exactDiagnosticV4.execution.complete) {
      if (executionV4.failed())
        result.exactDiagnosticV4.failure = executionV4.failure();
      else if (std::strcmp(result.exactDiagnosticV4.failure, "none") == 0)
        result.exactDiagnosticV4.failure = frameTapeReplayStatusName(result.status);
    }
    // Recovery work and owner retirement are outside the qualified session.
    executionV4.cancel();
    executionV3.cancel();
    PreparedPhaseScope phase(timing, &PreparedPhaseTiming::cleanupNanos);
    if (texturePrestageAudit) {
      texturePrestageAudit->stagedObjectsRemaining = 0u;
      texturePrestageAudit->stagedSubresourcesRemaining = 0u;
      texturePrestageAudit->stagedBytesRemaining = 0u;
      for (const auto& entry : prestaged) {
        if (entry.hiddenIndex < hiddenObjects.size() &&
            hiddenObjects[entry.hiddenIndex].value) {
          ++texturePrestageAudit->stagedObjectsRemaining;
        }
        for (std::size_t subresource = 0u;
             subresource < entry.seedDigests.size(); ++subresource) {
          if (entry.consumed[subresource]) continue;
          ++texturePrestageAudit->stagedSubresourcesRemaining;
          const auto& digest = entry.seedDigests[subresource];
          if (const auto* blob = findBlob(blobs, digest))
            texturePrestageAudit->stagedBytesRemaining += blob->bytes.size();
        }
      }
    }
    if (auto upper = device->dev().upperDevice()) upper->flush();
    if (replayUpper && !normalHistory) {
      replayUpper->queue().setRenderTapeExactAttachmentPreservation(
          priorExactAttachmentPreservation);
    }
    if (outputInstalled) {
      if (auto* swap = device->iface->GetSwapChain(0u)) {
        swap->coreSwapChain().restoreWindowPresenter();
        swap->Release();
      }
      outputInstalled = false;
    }
    if (readbackTarget) {
      dxmt9c_surface_release(readbackTarget);
      readbackTarget = nullptr;
    }
    for (auto& object : objects) {
      if (!object.value) continue;
      notifyActiveBuffer(RenderTapePreparedBufferPhaseV4::Retiring,
          kRenderTapeIdentityV4NoIndex, object.identity, nullptr, object.value);
      releaseSetupState(findSetupOwner(object.identity));
      if (!releaseNormalAliasState(object.identity)) {
        result.exactDiagnosticV4.failure = "v4-normal-alias-release-state";
        result.status = FrameTapeReplayStatus::ObjectCreationFailed;
      }
      if (texturePrestageAudit) {
        ++texturePrestageAudit->activeObjectsReleased;
        if (auto* entry = findPrestage(object.identity)) {
          entry->active = false;
        }
      }
      render_tape_runtime::releaseOwnedObject(object);
      ++result.conservation.objectsReleased;
    }
    for (auto& object : normalHiddenAliases) {
      if (!object.value) continue;
      if (!releaseNormalAliasState(object.identity)) {
        result.exactDiagnosticV4.failure = "v4-normal-hidden-release-state";
        result.status = FrameTapeReplayStatus::ObjectCreationFailed;
      }
      render_tape_runtime::releaseOwnedObject(object);
      ++result.conservation.objectsReleased;
    }
    for (std::size_t i = 0u; i < setupHiddenObjects.size(); ++i) {
      auto& object = setupHiddenObjects[i];
      if (!object.value) continue;
      notifyBuffer(RenderTapePreparedBufferPhaseV4::Retiring,
          kRenderTapeIdentityV4NoIndex, setupDefinitions[i], nullptr, object.value);
      releaseSetupState(i);
      render_tape_runtime::releaseOwnedObject(object);
      ++result.conservation.objectsReleased;
    }
    for (auto& object : hiddenObjects) {
      if (!object.value) continue;
      render_tape_runtime::releaseOwnedObject(object);
      if (texturePrestageAudit) {
        ++texturePrestageAudit->hiddenObjectsReleased;
      }
    }
    for (auto& checkpoint : checkpointQueries) {
      if (!checkpoint.object.value) continue;
      render_tape_runtime::releaseOwnedObject(checkpoint.object);
      ++result.conservation.objectsReleased;
      if (texturePrestageAudit) ++texturePrestageAudit->activeObjectsReleased;
    }
    if (texturePrestageAudit) {
      texturePrestageAudit->physicalObjectsCreated =
          texturePrestageAudit->hiddenObjectsCreated +
          texturePrestageAudit->activeObjectsCreated;
      texturePrestageAudit->physicalObjectsReleased =
          texturePrestageAudit->hiddenObjectsReleased +
          texturePrestageAudit->activeObjectsReleased;
      texturePrestageAudit->physicalConservation =
          texturePrestageAudit->physicalObjectsCreated ==
          texturePrestageAudit->physicalObjectsReleased;
    }
    observerSession.closed(result.exactDiagnosticV4.execution.canonicalTraversalComplete);
  };
  std::uint32_t deferredOutputMismatchEvent = 0xffffffffu;
  const auto fail = [&](FrameTapeReplayStatus status,
                        std::uint32_t eventIndex) -> FrameTapeReplayResult& {
    result.status = status;
    result.failedEventIndex = eventIndex;
    cleanup();
    return result;
  };
  const auto bufferSeedBackingFailure = [&](const OwnedObject& owned,
      const Definition& definition, const RenderTapeResourceMutationHeader& seed,
      bool verifyBytes) -> const char* {
    const char* unavailable = "v4-initial-setup-buffer-backing";
    if (!replayUpper || !replayUpper->pool() || !owned.value ||
        !sameIdentity(owned.identity, definition.fixed.identity) ||
        !sameIdentity(seed.identity, definition.fixed.identity)) return unavailable;
    std::lock_guard lock(replayUpper->queue().mutex_);
    D9CBufferDesc descriptor{};
    auto* buffer = static_cast<D9CBuffer*>(owned.value);
    const auto* record = buffer && buffer->obj
        ? replayUpper->pool()->findBuffer(buffer->obj->handle().value)
        : nullptr;
    if (!load(definition.descriptor, 0u, descriptor) || !record ||
        !record->buffer || !record->contents || record->destroyPending ||
        definition.fixed.expectedContentCount != 1u ||
        record->desc.size != descriptor.size ||
        record->desc.size != definition.fixed.expectedContentBytes ||
        seed.byteOffset != 0u || seed.byteSize != record->desc.size)
      return unavailable;
    if (verifyBytes) {
      const auto* blob = findBlob(blobs, seed.digest);
      if (!blob || blob->bytes.size() != seed.byteSize ||
          std::memcmp(record->contents, blob->bytes.data(), blob->bytes.size()) != 0)
        return "v4-initial-setup-buffer-bytes-mismatch";
    }
    return nullptr;
  };
  const auto setupBackingFailure = [&](std::size_t ownerIndex,
                                        bool verifyBufferBytes) -> const char* {
    const auto& entry = initialSetupV4->entries()[ownerIndex];
    const auto& owned = setupHiddenObjects[ownerIndex];
    const bool bufferFamily =
        entry.family == RenderTapeInitialSetupV4Family::VertexBuffer ||
        entry.family == RenderTapeInitialSetupV4Family::IndexBuffer;
    if (bufferFamily) {
      if (entry.mode != RenderTapeInitialSetupV4Mode::Seeded ||
          entry.seedCount != 1u || entry.firstSeedIndex >= initialSetupV4->seeds().size() ||
          entry.totalBytes != entry.definition.expectedContentBytes)
        return "v4-initial-setup-buffer-backing";
      return bufferSeedBackingFailure(owned, setupDefinitions[ownerIndex],
          initialSetupV4->seeds()[entry.firstSeedIndex].mutation, verifyBufferBytes);
    }
    const char* unavailable = "v4-initial-setup-texture-backing";
    if (!replayUpper || !replayUpper->pool() || !owned.value) return unavailable;
    // Hidden owners have no command consumers. Hold the existing queue owner
    // lock while inspecting their actual backing, never a shadow-only receipt.
    std::lock_guard lock(replayUpper->queue().mutex_);
    RenderTapeTextureDescriptorV2 descriptor{};
    D9CSurfaceDesc level0{};
    auto* texture = static_cast<D9CTexture*>(owned.value);
    const auto* record = texture && texture->obj
        ? replayUpper->pool()->findTexture(texture->obj->handle().value)
        : nullptr;
    const auto expectedType = entry.family == RenderTapeInitialSetupV4Family::TextureCube
        ? core::TextureType::Cube
        : entry.family == RenderTapeInitialSetupV4Family::TextureVolume
            ? core::TextureType::Volume : core::TextureType::TwoD;
    if (!renderTapeLoadTextureDescriptorV2(entry.descriptor, descriptor) ||
        !renderTapeTextureSubresourceDescriptor(entry.descriptor, 0u, level0) ||
        !record || !record->texture || record->destroyPending ||
        record->desc.type != expectedType ||
        record->desc.levels != descriptor.mipLevelCount ||
        record->physicalMipLevels < descriptor.mipLevelCount ||
        record->desc.width != level0.width || record->desc.height != level0.height ||
        record->desc.depth != std::max(1u, level0.depth) ||
        record->desc.format != devicec::fmtFromD3D(level0.format))
      return unavailable;
    return nullptr;
  };
  const auto omitNoOverwritePreflush = [&](std::uint32_t eventIndex,
      const RenderTapeResourceMutationHeader& mutation) {
    RenderTapeMutationDrainV4Input input{};
    input.sourcePlanDigest = exactDiagnosticV4->digest();
    input.snapshotPrefix = executionV4.snapshotPrefixEvents();
    input.mutationEventIndex = eventIndex;
    input.mutation = mutation;
    const auto active = std::find_if(objects.begin(), objects.end(), [&](const auto& owner) {
      return owner.value && sameIdentity(owner.identity, mutation.identity);
    });
    if (active != objects.end()) {
      input.activeIdentity = active->identity;
      input.ownerPhase = RenderTapeInitialSetupV4OwnerPhase::Active;
    }
    const auto ownerIndex = findSetupOwner(mutation.identity);
    if (ownerIndex < setupOwnerStates.size()) {
      const auto& entry = initialSetupV4->entries()[ownerIndex];
      const auto& state = setupOwnerStates[ownerIndex];
      input.definition = entry.definition;
      input.descriptor = entry.descriptor;
      input.definitionEventIndex = entry.definitionEventIndex;
      if (active != objects.end()) input.ownerPhase = state.phase;
      if (entry.mode == RenderTapeInitialSetupV4Mode::Seeded &&
          state.mode == RenderTapeInitialSetupV4Mode::Seeded &&
          state.phase == RenderTapeInitialSetupV4OwnerPhase::Active &&
          entry.seedCount == 1u && state.seedCount == 1u && state.consumedSeeds == 1u &&
          entry.firstSeedIndex < initialSetupV4->seeds().size() &&
          result.exactDiagnosticV4.initialSetupCompleted) {
        const auto& seed = initialSetupV4->seeds()[entry.firstSeedIndex];
        input.initialContent = {
            .provenance = RenderTapeMutationDrainV4ContentProvenance::SetupCheckpointAndConsumed,
            .sourcePlanDigest = initialSetupV4->sourcePlanDigest(),
            .definitionEventIndex = entry.definitionEventIndex,
            .initialUploadEventIndex = seed.eventIndex,
            .initialUpload = seed.mutation,
            .completedBytes = entry.totalBytes,
            .completedCount = entry.seedCount,
            .consumedSeeds = state.consumedSeeds,
        };
      }
    } else {
      const auto prefix = std::find_if(prefixBufferSeeds.begin(), prefixBufferSeeds.end(),
          [&](const auto& witness) {
            return sameIdentity(plan.definitions[witness.definitionIndex].fixed.identity,
                                mutation.identity);
          });
      if (prefix != prefixBufferSeeds.end()) {
        const auto& definition = plan.definitions[prefix->definitionIndex];
        input.definition = definition.fixed;
        input.descriptor = definition.descriptor;
        input.definitionEventIndex = prefix->definitionEventIndex;
        if (prefix->uploadReplayed && prefix->bytesVerified && prefix->checkpointCompleted) {
          const auto& seed = plan.mutations[prefix->mutationIndex];
          input.initialContent = {
              .provenance = RenderTapeMutationDrainV4ContentProvenance::PrefixCheckpoint,
              .sourcePlanDigest = exactDiagnosticV4->digest(),
              .definitionEventIndex = prefix->definitionEventIndex,
              .initialUploadEventIndex = prefix->mutationEventIndex,
              .initialUpload = seed,
              .completedBytes = seed.byteSize,
              .completedCount = 1u,
              .consumedSeeds = 1u,
          };
        }
      }
    }
    return renderTapeMutationDrainV4Decision(input) ==
        RenderTapeMutationDrainV4Decision::OmitProviderPreflush;
  };
  const auto installOutput = [&] {
    if (outputInstalled) return true;
    auto upper = device->dev().upperDevice();
    if (!upper || !upper->pool()) return true;
    PreparedPhaseScope phase(timing,
                             &PreparedPhaseTiming::resourceSetupNanos);
    readbackTarget = dxmt9c_device_create_render_target(
        device, plan.outputDesc.width, plan.outputDesc.height,
        plan.outputDesc.format, 0u, 0u, 0u, nullptr);
    auto* record = readbackTarget
        ? upper->pool()->findSurface(readbackTarget->obj->handle().value)
        : nullptr;
    auto* swap = device->iface->GetSwapChain(0u);
    if (!record || !record->texture || !swap) {
      if (swap) swap->Release();
      return false;
    }
    output = std::make_shared<OffscreenPresentOutput>(
        WMT::Texture{record->texture.handle}, plan.outputDesc.width,
        plan.outputDesc.height);
    outputInstalled = swap->coreSwapChain().installPresentOutput(output);
    swap->Release();
    return outputInstalled;
  };
  const auto replayChunk = [&](const Chunk& chunk,
                              std::span<const ProviderReplayRecordRange> cuts = {},
                              std::span<const std::uint32_t> batchCuts = {},
                              std::span<const CaptureSourceReplayRow> sourceRows = {},
                              bool v3Command = false,
                              bool v4Command = false) {
    PreparedPhaseScope phase(timing, &PreparedPhaseTiming::chunkReplayNanos);
    const auto diagnosticRaw = v4Command
        ? static_cast<std::uint64_t>(&chunk - plan.commandChunks.data()) : 0u;
    PreparedReplayCpuScope rawCpu(v4Command, &executionV4, diagnosticRaw, 1u);
    ImportedChunkView imported;
    if (!importPrevalidatedCommandChunk(chunk.bytes, chunk.envelope, imported) ||
        !resolveChunk(chunk, objects, resolved)) {
      return false;
    }
    for (std::size_t recordIndex = 0u;
         recordIndex < imported.records.size(); ++recordIndex) {
      const auto record = imported.record(recordIndex);
      if (record.header.type != D9C_COMMAND_RECORD_PRESENT) continue;
      D9CCommandChunkWirePresent present{};
      if (!load(record.payload, 0u, present) || record.header.handleCount > 1u ||
          !presentGeometrySupported(present, plan.outputDesc)) {
        return false;
      }
      if (record.header.handleCount == 1u) {
        if (present.sourceHandleIndex >= imported.handles.size() ||
            present.sourceHandleIndex >= resolved.size()) {
          return false;
        }
        const auto& handle = imported.handles[present.sourceHandleIndex];
        const D9CWireObjectIdentity identity{
            .kind = handle.kind, .generation = handle.generation,
            .objectId = handle.objectId};
        if (!sameIdentity(identity, plan.outputIdentity)) return false;
        auto* surface =
            static_cast<D9CSurface*>(resolved[present.sourceHandleIndex]);
        auto* swap = device->iface->GetSwapChain(0u);
        const bool same = surface && surface->obj && swap &&
                          surface->obj == swap->backBuffer();
        if (swap) swap->Release();
        if (!same) return false;
      }
    }
    if (v4Command) {
      // Replay the full original raw through the caller's normal renderer
      // route. Captured partitions and publication cuts are observations,
      // never commands to the replay path.
      if (!cuts.empty() || !batchCuts.empty() || !sourceRows.empty() || v3Command ||
          !executionV4.bindCommandChunk(chunk.bytes, chunk.envelope, resolved))
        return false;
      ProviderReplayObservationV4Result observed{};
      std::vector<RenderTapePreparedHandleAssociationV4> associations;
      if (observerSession.enabled()) {
        try {
          associations.reserve(imported.handles.size());
          for (std::size_t i = 0u; i < imported.handles.size(); ++i) {
            const auto& handle = imported.handles[i];
            associations.push_back({.handleIndex = static_cast<std::uint32_t>(i),
                .identity = {handle.kind, handle.generation, handle.objectId},
                .resolvedWrapper = resolved[i]});
          }
          ++result.preparedObserverV4.associationRaws;
          result.preparedObserverV4.handleAssociations += associations.size();
        } catch (...) {
          observerSession.reject("v4-observer-association-storage-failed");
          associations.clear();
        }
      }
      const auto hr = [&] {
        PreparedReplayCpuScope bodyCpu(true, &executionV4, diagnosticRaw, 2u);
        return replayPrevalidatedResolvedCommandChunkObservedV4(
          device, chunk.bytes, chunk.envelope, resolved, policy,
          observerSession.observation(executionV4.observation(), associations), observed);
      }();
      observerSession.observationComplete();
      if (hr != core::D3D_OK)
        result.exactDiagnosticV4.failure = "v4-renderer-replay-failed";
      else if (!observed.captureAccepted)
        result.exactDiagnosticV4.failure = "v4-observation-capture-rejected";
      else if (!observed.qualificationSucceeded)
        result.exactDiagnosticV4.failure = "v4-observation-qualification-rejected";
      else if (!observed.ledgerSettlementObserved)
        result.exactDiagnosticV4.failure = "v4-observation-ledger-unsettled";
      return hr == core::D3D_OK && observed.captureAccepted &&
          observed.qualificationSucceeded && observed.ledgerSettlementObserved;
    }
    if (!cuts.empty()) {
      if (sourcePolicy) {
        struct SettlementContext {
          RenderTapeSourceReplayEvidence& evidence;
          std::span<const ProviderReplayRecordRange> ranges;
          std::span<const CaptureSourceReplayRow> sourceRows;
          std::span<RenderTapeSourceReplayRowEvidence> rows;
          std::size_t next = 0u;
          D9CDevice* device = nullptr;
          const std::vector<Definition>* definitions = nullptr;
          std::vector<OwnedObject>* objects = nullptr;
          const RenderTapeTextureInputAuditRequest* textureInput = nullptr;
          RenderTapeTextureInputAuditEvidence* textureInputEvidence = nullptr;
        } context{result.sourceReplay, cuts, sourceRows, {}, 0u};
        context.device = device;
        context.definitions = &definitions;
        context.objects = &objects;
        context.textureInput = textureInput;
        context.textureInputEvidence = textureInputEvidence;
        if (sourceRows.size() != cuts.size()) return false;
        if (sourceRows.empty()) return false;
        const auto allRows = sourcePolicy->rows();
        const auto rowBegin = std::find_if(
            allRows.begin(), allRows.end(), [&](const auto& row) {
              return row.commandChunkIndex == sourceRows.front().commandChunkIndex &&
                     row.eventOrdinal == sourceRows.front().eventOrdinal;
            });
        if (rowBegin == allRows.end() ||
            static_cast<std::size_t>(allRows.end() - rowBegin) < sourceRows.size()) {
          return false;
        }
        context.rows = std::span<RenderTapeSourceReplayRowEvidence>(
            result.sourceReplay.sources.data() + (rowBegin - allRows.begin()),
            sourceRows.size());
        const ProviderReplaySourceConsumer consumer{
            .context = &context,
            .settled = [](void* opaque,
                          const ProviderReplaySourceSettlement& settlement) noexcept {
              auto& context = *static_cast<SettlementContext*>(opaque);
              if (context.next >= context.ranges.size() ||
                  context.next >= context.sourceRows.size() ||
                  context.next >= context.rows.size() ||
                  settlement.rangeIndex != context.next || settlement.rawOrdinal == 0u ||
                  settlement.range.firstRecord != context.ranges[context.next].firstRecord ||
                  settlement.range.recordCount != context.ranges[context.next].recordCount) {
                return false;
              }
              if (context.evidence.storageOverflow) return false;
              if (settlement.publications.size() >
                  context.evidence.publications.capacity() -
                      context.evidence.publications.size()) {
                context.evidence.storageOverflow = true;
                return false;
              }
              auto& row = context.rows[context.next];
              if (row.settled ||
                  row.eventOrdinal != context.sourceRows[context.next].eventOrdinal ||
                  row.sourceOrdinal != context.sourceRows[context.next].sourceOrdinal ||
                  row.capturedSeqId != context.sourceRows[context.next].capturedSeqId ||
                  row.firstRecord != context.sourceRows[context.next].firstRecord ||
                  row.recordCount != context.sourceRows[context.next].recordCount) {
                return false;
              }
              row.actualRawOrdinal = settlement.rawOrdinal;
              row.rangeIndex = settlement.rangeIndex;
              row.publicationOffset = static_cast<std::uint32_t>(
                  context.evidence.publications.size());
              row.publicationCount = static_cast<std::uint32_t>(
                  settlement.publications.size());
              row.settled = true;
              row.settlement = settlement.kind ==
                      ProviderReplaySourceSettlementKind::PresentPublished
                  ? RenderTapeSourceReplaySettlement::PresentPublished
                  : settlement.kind == ProviderReplaySourceSettlementKind::Published
                      ? RenderTapeSourceReplaySettlement::Published
                      : RenderTapeSourceReplaySettlement::Empty;
              for (const auto& publication : settlement.publications) {
                const auto source = publication.source;
                const auto metadata = publication.metadata;
                context.evidence.publications.push_back({
                    .sourceIdIndex = source.id.index,
                    .sourceIdGeneration = source.id.generation,
                    .storageFirstPage = source.storage.firstPage,
                    .storagePageCount = source.storage.pageCount,
                    .storageGeneration = source.storage.generation,
                    .metadataRawOrdinal = metadata.rawOrdinal,
                    .metadataSourceOrdinal = metadata.sourceOrdinal,
                    .metadataSeqId = metadata.seqId,
                    .metadataBuildGeneration = metadata.buildGeneration,
                    .metadataUsedBytes = metadata.usedBytes,
                    .metadataPageCount = metadata.pageCount,
                    .metadataPaddingPagesBefore = metadata.paddingPagesBefore,
                    .metadataStrictAdmission = metadata.strictAdmission,
                    .controlIndex = static_cast<std::uint64_t>(
                        publication.controlIndex),
                    .hasPresent = publication.hasPresent,
                });
              }
              ++context.next;
              auto& evidence = context.evidence;
              ++evidence.completedSourceCount;
              evidence.publicationCount += settlement.publications.size();
              if (settlement.kind == ProviderReplaySourceSettlementKind::Empty)
                ++evidence.emptySourceCount;
              if (settlement.kind == ProviderReplaySourceSettlementKind::PresentPublished)
                ++evidence.presentSourceCount;
              if (context.textureInput &&
                  row.capturedSeqId == context.textureInput->afterCapturedSeqId) {
                if (settlement.kind != ProviderReplaySourceSettlementKind::Published ||
                    !applyTextureInputAudit(context.device, *context.textureInput,
                        *context.definitions, *context.objects,
                        *context.textureInputEvidence)) return false;
              }
              return true;
            },
        };
        const auto hr = replayPrevalidatedResolvedCommandChunkBySources(
            device, chunk.bytes, chunk.envelope, resolved, policy, cuts, consumer);
        result.sourceReplay.complete = hr == core::D3D_OK &&
            result.sourceReplay.completedSourceCount == result.sourceReplay.sourceCount &&
            !result.sourceReplay.storageOverflow;
        return hr == core::D3D_OK && context.next == cuts.size();
      }
      std::uint32_t completed = 0u;
      std::uint32_t completedBatchCuts = 0u;
      const auto hr = replayPrevalidatedResolvedCommandChunkAtRecordBoundaries(
          device, chunk.bytes, chunk.envelope, resolved, cuts, completed,
          batchCuts, &completedBatchCuts);
      sourceBoundaryAudit->completedSourceCount += completed;
      sourceBoundaryAudit->completedBatchCutCount += completedBatchCuts;
      sourceBoundaryAudit->complete =
          sourceBoundaryAudit->completedSourceCount == sourceBoundaryAudit->sourceCount &&
          sourceBoundaryAudit->completedBatchCutCount == sourceBoundaryAudit->batchCutCount;
      return hr == core::D3D_OK;
    }
    if (v3Command) {
      return executionV3.bindCommandChunk(chunk.bytes, chunk.envelope, resolved) &&
          replayPrevalidatedResolvedCommandChunkV3(device, chunk.bytes, chunk.envelope,
              resolved, policy, executionV3.recordConsumer()) == core::D3D_OK;
    }
    return replayPrevalidatedResolvedCommandChunk(
               device, chunk.bytes, chunk.envelope, resolved, policy) ==
           core::D3D_OK;
  };

  if (texturePrestageAudit && !prestaged.empty()) {
    PreparedPhaseScope phase(timing, &PreparedPhaseTiming::resourceSetupNanos);
    try {
      for (auto& entry : prestaged) {
        void* value = render_tape_runtime::createRenderTapeObject(
            device, entry.definition.fixed, entry.definition.descriptor, blobs,
            hiddenObjects);
        if (!value) {
          result.status = FrameTapeReplayStatus::ObjectCreationFailed;
          cleanup();
          return result;
        }
        entry.hiddenIndex = hiddenObjects.size();
        hiddenObjects.push_back({
            .identity = entry.definition.fixed.identity, .value = value});
        ++texturePrestageAudit->hiddenObjectsCreated;
        bool deferredUpload = false;
        bool physicalTexture = false;
        if (auto upper = device->dev().upperDevice(); upper && upper->pool()) {
          // Cold hidden ownership excludes command consumers. Inspect the
          // actual backing under its queue lock, not a guessed D3D pool rule:
          // Shared textures use immediate replaceRegion, Private textures
          // require a deferred Initializer blit.
          std::lock_guard lock(upper->queue().mutex_);
          const auto* texture = static_cast<D9CTexture*>(value);
          const auto* record = texture && texture->obj
              ? upper->pool()->findTexture(texture->obj->handle().value)
              : nullptr;
          physicalTexture = record && record->texture;
          deferredUpload = physicalTexture && record->needsStagingBlit;
        }
        if (!physicalTexture) {
          result.status = FrameTapeReplayStatus::ObjectCreationFailed;
          cleanup();
          return result;
        }
        for (std::uint32_t subresource = 0u;
             subresource < entry.seedDigests.size(); ++subresource) {
          const auto mutation = std::find_if(
              plan.mutations.begin(), plan.mutations.end(),
              [&](const auto& candidate) {
                return sameIdentity(candidate.identity,
                                    entry.definition.fixed.identity) &&
                       candidate.subresource == subresource;
              });
          if (mutation == plan.mutations.end() ||
              !applyGeneralMutation(device, *mutation, blobs,
                                    prestageDefinitions, hiddenObjects)) {
            result.status = FrameTapeReplayStatus::MutationFailed;
            cleanup();
            return result;
          }
          if (deferredUpload) {
            ++texturePrestageAudit->deferredUploadSubresources;
          } else {
            ++texturePrestageAudit->immediateUploadSubresources;
          }
        }
      }
      if (auto upper = device->dev().upperDevice()) {
        const auto uploaded = upper->queue().flushInitializerUploads();
        texturePrestageAudit->initializerUploadValue = uploaded.value;
        if (uploaded.didFlush) {
          ++texturePrestageAudit->initializerFlushes;
        }
        bool waited = true;
        if (uploaded.didFlush ||
            texturePrestageAudit->deferredUploadSubresources != 0u) {
          // A previous owner flush may already have submitted these uploads.
          // Deferred bytes still require its event/value and a checked wait;
          // a no-op flush alone is not synchronization evidence.
          if (uploaded.value == 0u || !uploaded.event) {
            result.status = FrameTapeReplayStatus::MutationFailed;
            cleanup();
            return result;
          }
          ++texturePrestageAudit->initializerWaits;
          waited = WMT::SharedEvent{uploaded.event.handle}.waitUntilSignaledValue(
              uploaded.value, /*timeout-ns*/ 1'000'000'000u);
        }
        if (!waited) {
          result.status = FrameTapeReplayStatus::MutationFailed;
          cleanup();
          return result;
        }
      } else {
        result.status = FrameTapeReplayStatus::MutationFailed;
        cleanup();
        return result;
      }
    } catch (...) {
      result.status = FrameTapeReplayStatus::ObjectCreationFailed;
      cleanup();
      return result;
    }
  }

  if (exactDiagnosticV4) {
    PreparedPhaseScope phase(timing, &PreparedPhaseTiming::resourceSetupNanos);
    try {
      const auto entries = initialSetupV4->entries();
      const auto seeds = initialSetupV4->seeds();
      for (std::size_t i = 0u; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        void* value = render_tape_runtime::createRenderTapeObject(device,
            entry.definition, entry.descriptor, blobs, setupHiddenObjects);
        if (!value) {
          result.exactDiagnosticV4.failure = "v4-initial-setup-owner-creation";
          return fail(FrameTapeReplayStatus::ObjectCreationFailed, entry.definitionEventIndex);
        }
        ++result.conservation.objectsCreated;
        const auto releaseTransient = [&](void* pointer) {
          OwnedObject acquired{entry.definition.identity, pointer};
          render_tape_runtime::releaseOwnedObject(acquired);
          ++result.conservation.objectsReleased;
        };
        std::unique_ptr<void, decltype(releaseTransient)> transient(value, releaseTransient);
        if (!renderTapeInitialSetupV4Advance(setupOwnerStates[i],
                RenderTapeInitialSetupV4Action::Prepare)) {
          transient.reset();
          result.exactDiagnosticV4.failure = "v4-initial-setup-owner-preparation";
          return fail(FrameTapeReplayStatus::ObjectCreationFailed, entry.definitionEventIndex);
        }
        // Register the acquired reference before any fallible backing or upload
        // work. The pre-sized table cannot allocate during this transfer.
        setupHiddenObjects[i].value = value;
        transient.release();
        notifyBuffer(RenderTapePreparedBufferPhaseV4::HiddenDefined,
            entry.definitionEventIndex, setupDefinitions[i], nullptr, value);
        if (const auto* failure = setupBackingFailure(i, false)) {
          result.exactDiagnosticV4.failure = failure;
          return fail(FrameTapeReplayStatus::ObjectCreationFailed, entry.definitionEventIndex);
        }
        // AllocationOnly executes no mutation and has no first seed index.
        // Any normal creation-time zero fill joins the real checkpoint below;
        // its completion never grants this owner initial content authority.
        for (std::uint32_t relative = 0u; relative < entry.seedCount; ++relative) {
          const auto& seed = seeds[entry.firstSeedIndex + relative];
          if (!applyGeneralMutation(device, seed.mutation, blobs,
                  setupDefinitions, setupHiddenObjects)) {
            result.exactDiagnosticV4.failure = "v4-initial-setup-upload-stage";
            return fail(FrameTapeReplayStatus::MutationFailed, seed.eventIndex);
          }
          notifyBuffer(RenderTapePreparedBufferPhaseV4::HiddenSeedApplied,
              seed.eventIndex, setupDefinitions[i], &seed.mutation, value);
        }
        // MANAGED unlock can enqueue a software mutation before the GPU upload
        // stream. Join its real worker before inspecting the resulting backing.
        if (!drainDeferredReplay(device, "render-tape-v4-initial-setup")) {
          result.exactDiagnosticV4.failure = "v4-initial-setup-replay-drain";
          return fail(FrameTapeReplayStatus::MutationFailed, entry.definitionEventIndex);
        }
        if (const auto* failure = setupBackingFailure(i, true)) {
          result.exactDiagnosticV4.failure = failure;
          return fail(FrameTapeReplayStatus::MutationFailed, entry.definitionEventIndex);
        }
        if (replayUpper->queryProgress().failed) {
          result.exactDiagnosticV4.failure = "v4-initial-setup-upload-stage";
          return fail(FrameTapeReplayStatus::MutationFailed, entry.definitionEventIndex);
        }
        ++result.exactDiagnosticV4.initialSetupPrepared;
      }
    } catch (...) {
      result.exactDiagnosticV4.failure = "v4-initial-setup-exception";
      return fail(FrameTapeReplayStatus::ObjectCreationFailed, 0xffffffffu);
    }
  }

  // Completed initial EVENTs have no captured resource dependencies. Restore
  // them before transcript work, even when their ObjectDefine is materialized
  // lazily after a captured command. Activation only transfers this owner;
  // it must never insert a new mid-transcript END/publication or drain.
  try {
    for (const auto& definition : plan.definitions) {
      if (definition.fixed.identity.kind != D9C_CHUNK_HANDLE_KIND_QUERY) continue;
      RenderTapeQueryDescriptorDecoded descriptor{};
      if (!renderTapeLoadQueryDescriptor(definition.descriptor, descriptor))
        return fail(FrameTapeReplayStatus::QueryOracleUnsupported, 0xffffffffu);
      if (descriptor.schemaVersion != kRenderTapeQueryDescriptorVersion3) continue;
      const auto started = std::chrono::steady_clock::now();
      const auto finishSetup = [&] {
        result.queryReplay.checkpointSetupWallNanos += elapsedNanos(
            started, std::chrono::steady_clock::now());
      };
      auto* query = static_cast<D9CQuery*>(render_tape_runtime::createRenderTapeObject(
          device, definition.fixed, definition.descriptor, blobs, {}));
      if (!query) {
        finishSetup();
        return fail(FrameTapeReplayStatus::ObjectCreationFailed, 0xffffffffu);
      }
      checkpointQueries.push_back({
          .object = {.identity = definition.fixed.identity, .value = query},
          .evidenceIndex = result.queryReplay.checkpoints.size(),
          .query = query,
      });
      ++result.conservation.objectsCreated;
      // The audit's hidden category is exclusively texture prestage. Query
      // setup is a non-prestage physical acquisition, counted once here;
      // transcript activation transfers ownership without acquiring again.
      if (texturePrestageAudit) ++texturePrestageAudit->activeObjectsCreated;
      result.queryReplay.checkpoints.push_back({
          .identity = definition.fixed.identity,
          .capturedIssueCycle = descriptor.initialIssueCycle,
          .capturedIssueOrdinal = descriptor.checkpoint.issueOrdinal,
          .capturedDeviceEpoch = descriptor.checkpoint.deviceEpoch,
      });
      auto& row = result.queryReplay.checkpoints.back();
      const auto before = replayUpper->queryProgress();
      if (dxmt9c_query_issue(query, 1u) != core::D3D_OK) {
        finishSetup();
        result.queryReplay.failure = RenderTapeQueryReplayFailure::PollFailed;
        return fail(FrameTapeReplayStatus::QueryOracleFailed, 0xffffffffu);
      }
      row.setupIssued = true;
      ++result.queryReplay.checkpointSetupEnds;
      auto snapshot = device->dev().captureQueryPollSnapshot(query->obj);
      if (snapshot.type != core::QueryType::Event || snapshot.issueOrdinal != 1u ||
          snapshot.active || snapshot.failed || !snapshot.backendFence.valid() ||
          (!snapshot.eligible &&
           !device->dev().promotePublishedQueryPollSnapshot(snapshot))) {
        finishSetup();
        result.queryReplay.failure = RenderTapeQueryReplayFailure::SnapshotUnavailable;
        return fail(FrameTapeReplayStatus::QueryOracleFailed, 0xffffffffu);
      }
      bool complete = false;
      while (result.queryReplay.checkpointSetupPolls < result.queryReplay.maxOraclePolls) {
        const auto elapsed = elapsedNanos(started, std::chrono::steady_clock::now());
        if (result.queryReplay.checkpointSetupWallNanos >=
                result.queryReplay.maxOracleWallNanos ||
            elapsed >= result.queryReplay.maxOracleWallNanos -
                result.queryReplay.checkpointSetupWallNanos) break;
        ++row.setupPolls;
        ++result.queryReplay.checkpointSetupPolls;
        const auto polled = device->dev().pollQuerySnapshot(
            snapshot, nullptr, 0u, core::QUERY_GETDATA_FLUSH);
        if (polled == core::S_OK) { complete = true; break; }
        if (polled != core::S_FALSE) {
          finishSetup();
          result.queryReplay.failure = RenderTapeQueryReplayFailure::PollFailed;
          return fail(FrameTapeReplayStatus::QueryOracleFailed, 0xffffffffu);
        }
        ++result.queryReplay.checkpointSetupWaits;
        std::this_thread::yield();
      }
      const auto fence = snapshot.backendFence.snapshot();
      const auto after = replayUpper->queryProgress();
      row.localQueryIdentity = snapshot.identity;
      row.localIssueOrdinal = snapshot.issueOrdinal;
      row.localDeviceEpoch = snapshot.issueDeviceEpoch;
      row.localQueue = fence.queue;
      row.localSequence = fence.sequence;
      row.localInitializer = fence.initializer;
      row.completedSequence = after.completed;
      row.completedInitializer = after.initializerCompleted;
      if (after.published < before.published ||
          after.published - before.published > std::numeric_limits<std::uint32_t>::max())
        complete = false;
      else result.queryReplay.checkpointSetupPublications +=
          static_cast<std::uint32_t>(after.published - before.published);
      finishSetup();
      if (!complete || dxmt9::query::classifyEnd(snapshot.backendFence, after) !=
              dxmt9::query::Poll::Complete) {
        result.queryReplay.failure = complete
            ? RenderTapeQueryReplayFailure::PollFailed
            : RenderTapeQueryReplayFailure::ProgressExhausted;
        return fail(complete ? FrameTapeReplayStatus::QueryOracleFailed
                             : FrameTapeReplayStatus::QueryProgressExhausted,
                    0xffffffffu);
      }
      row.completed = true;
      checkpointQueries.back().snapshot = std::move(snapshot);
      ++result.queryReplay.checkpointCompleted;
    }
  } catch (...) {
    return fail(FrameTapeReplayStatus::ObjectCreationFailed, 0xffffffffu);
  }

  std::optional<PreparedReplayCpuScope> intervalCpu;
  try {
    for (const auto& replayEvent : plan.events) {
      const auto eventIndex = replayEvent.eventIndex;
      if (sourcePolicyV3 || exactDiagnosticV4) {
        const auto prefix = exactDiagnosticV4
            ? executionV4.snapshotPrefixEvents() : executionV3.snapshotPrefixEvents();
        if (eventIndex == prefix) {
          if (normalHistory) {
            // Parent lookup is a borrowed cold union, not semantic activation.
            // Retain each actual wrapper so Texture's weak cache cannot later
            // manufacture a different handle at its original ObjectDefine.
            auto parents = objects;
            parents.insert(parents.end(), setupHiddenObjects.begin(), setupHiddenObjects.end());
            for (std::size_t i = 0u; i < normalHiddenAliases.size(); ++i) {
              const auto& definition = plan.definitions[normalHistory->lateAliases[i]];
              void* value = render_tape_runtime::createRenderTapeObject(device,
                  definition.fixed, definition.descriptor, blobs, parents);
              if (!value) {
                result.exactDiagnosticV4.failure = "v4-normal-alias-preparation";
                return fail(FrameTapeReplayStatus::ObjectCreationFailed, eventIndex);
              }
              normalHiddenAliases[i].value = value;
              ++result.conservation.objectsCreated;
              if (!renderTapeInitialSetupV4Advance(normalAliasStates[i], RenderTapeInitialSetupV4Action::Prepare)) {
                result.exactDiagnosticV4.failure = "v4-normal-alias-prepare-state";
                return fail(FrameTapeReplayStatus::ObjectCreationFailed, eventIndex);
              }
              ++result.normalDiagnostic.aliasesPrepared;
            }
          } else restoreSeededColorAttachmentTouches(device, definitions, objects, seededSubresources);
          for (const auto& overlay : bootstrap) {
            if (!replayChunk(overlay))
              return fail(FrameTapeReplayStatus::BootstrapFailed, eventIndex);
          }
          bootstrapReplayed = true;
          // Only the admitted checkpoint prefix is setup. Interval mutations
          // and controls are replayed below after the local arm baseline.
          if (renderTapeV4SetupCheckpointRequired(exactDiagnosticV4, eventIndex, prefix)) {
            if (result.exactDiagnosticV4.initialSetupPrepared !=
                initialSetupV4->entries().size()) {
              result.exactDiagnosticV4.failure = "v4-initial-setup-owner-unprepared";
              return fail(FrameTapeReplayStatus::CommandReplayFailed, eventIndex);
            }
            if (!prefixBufferSeeds.empty()) {
              if (!drainDeferredReplay(device, "render-tape-v4-prefix-buffer-seeds")) {
                result.exactDiagnosticV4.failure = "v4-prefix-buffer-seed-replay-drain";
                return fail(FrameTapeReplayStatus::MutationFailed, eventIndex);
              }
              for (auto& witness : prefixBufferSeeds) {
                const auto& definition = plan.definitions[witness.definitionIndex];
                const auto active = std::find_if(objects.begin(), objects.end(),
                    [&](const auto& owner) {
                      return owner.value && sameIdentity(owner.identity, definition.fixed.identity);
                    });
                witness.bytesVerified = witness.uploadReplayed && active != objects.end() &&
                    !bufferSeedBackingFailure(*active, definition,
                        plan.mutations[witness.mutationIndex], true);
              }
            }
            const auto upper = device->dev().upperDevice();
            // An empty ordinary writer does not publish deferred texture seeds.
            // Complete both real streams outside the capture interval. The
            // checkpoint owns the queue lock only in this scope, before arm.
            const auto checkpoint = upper ? [&] {
              PreparedPhaseScope phase(timing,
                  &PreparedPhaseTiming::v4PreArmCheckpointNanos);
              return upper->queue().beginRenderTapeCheckpoint();
            }() : std::nullopt;
            if (!checkpoint) {
              result.exactDiagnosticV4.failure = "v4-setup-checkpoint-failed";
              return fail(FrameTapeReplayStatus::CommandReplayFailed, eventIndex);
            }
            result.exactDiagnosticV4.initialSetupCompleted = true;
            for (auto& witness : prefixBufferSeeds)
              witness.checkpointCompleted = witness.bytesVerified;
          } else if (auto upper = device->dev().upperDevice()) upper->flush();
          if (normalHistory) {
            // The checkpoint lock has left scope. Settlement independently
            // rechecks this exact local prefix, completion, policy and handles.
            std::vector<D9CWireObjectIdentity> required;
            std::vector<dxmt9::TapeAttachmentRestoreBinding> bindings;
            required.reserve(normalHistory->surfaces.size());
            bindings.reserve(normalHistory->surfaces.size());
            for (const auto index : normalHistory->surfaces) {
              const auto identity = plan.definitions[index].fixed.identity;
              auto* value = render_tape_runtime::findExactObject(objects, identity);
              if (!value) value = render_tape_runtime::findExactObject(normalHiddenAliases, identity);
              auto* surface = static_cast<D9CSurface*>(value);
              if (!surface || !surface->obj) {
                result.exactDiagnosticV4.failure = "v4-normal-surface-coverage";
                return fail(FrameTapeReplayStatus::ObjectCreationFailed, eventIndex);
              }
              RenderTapeSurfaceDescriptorV2 descriptor{};
              if (!renderTapeLoadSurfaceDescriptorV2(plan.definitions[index].descriptor, descriptor))
                return fail(FrameTapeReplayStatus::InvalidTape, eventIndex);
              if (descriptor.storage == static_cast<std::uint32_t>(RenderTapeSurfaceStorage::TextureSubresource)) {
                auto* parent = render_tape_runtime::findExactObject(objects, descriptor.parentTexture);
                if (!parent) parent = render_tape_runtime::findExactObject(setupHiddenObjects, descriptor.parentTexture);
                auto* texture = static_cast<D9CTexture*>(parent);
                const auto* backing = replayUpper->pool()->findSurface(surface->obj->handle().value);
                const bool nullRt = renderTapeNullRtSurfaceShapeSupported(descriptor.surface);
                if (!texture || !texture->obj || !backing || backing->destroyPending ||
                    surface->ownerTex != texture || surface->ownerLevel != descriptor.subresource ||
                    (!nullRt && backing->aliasTexture != texture->obj->handle())) {
                  result.exactDiagnosticV4.failure = "v4-normal-alias-physical-parent";
                  return fail(FrameTapeReplayStatus::ObjectCreationFailed, eventIndex);
                }
              }
              required.push_back(identity);
              bindings.push_back({identity, surface->obj->handle().value});
            }
            const auto progress = replayUpper->queryProgress();
            auto restore = dxmt9::TapeAttachmentRestorePlan::prepare(normalHistory->header,
                normalHistory->rows, required, bindings,
                {progress.queue, progress.published, progress.initializerPublished});
            if (!restore || !replayUpper->queue().restoreRenderTapeNormalAttachmentHistory(std::move(*restore))) {
              result.exactDiagnosticV4.failure = "v4-normal-history-settlement";
              return fail(FrameTapeReplayStatus::CommandReplayFailed, eventIndex);
            }
            result.normalDiagnostic.historyRestored = true;
          }
          if (exactDiagnosticV4) {
            std::vector<RenderTapeExecutionV4QueryBaselineInput> baselines;
            baselines.reserve(checkpointQueries.size());
            for (const auto& entry : checkpointQueries)
              baselines.push_back({entry.object.identity, entry.query, entry.snapshot});
            if (!executionV4.arm(device, baselines))
              return fail(FrameTapeReplayStatus::CommandReplayFailed, eventIndex);
            intervalCpu.emplace(true, &executionV4, eventIndex, 0u);
            if (observerSession.enabled())
              observerSession.armed(result.exactDiagnosticV4.execution.localArmHresult,
                  result.exactDiagnosticV4.execution.localArm,
                  acquireRenderTapeRuntimeIdentityV4(device));
          } else if (!executionV3.arm(device))
            return fail(FrameTapeReplayStatus::CommandReplayFailed, eventIndex);
        }
        if (!(exactDiagnosticV4 ? executionV4.beginEvent(eventIndex)
                               : executionV3.beginEvent(eventIndex)))
          return fail(FrameTapeReplayStatus::CommandReplayFailed, eventIndex);
      }
      switch (replayEvent.type) {
      case RenderTapeEventType::BootstrapState: {
        if (replayEvent.aux >= plan.bootstrapGamma.size() ||
            replayEvent.index + replayEvent.count > plan.bootstrapChunks.size()) {
          return fail(FrameTapeReplayStatus::BootstrapFailed, eventIndex);
        }
        bootstrap.assign(plan.bootstrapChunks.begin() + replayEvent.index,
                         plan.bootstrapChunks.begin() + replayEvent.index +
                             replayEvent.count);
        const auto& gamma = plan.bootstrapGamma[replayEvent.aux];
        if (!gamma.empty()) {
          if (!applyGammaRamp(device, gamma))
            return fail(FrameTapeReplayStatus::BootstrapFailed, eventIndex);
        }
        break;
      }
      case RenderTapeEventType::ObjectDefine: {
        PreparedPhaseScope phase(timing,
                                 &PreparedPhaseTiming::resourceSetupNanos);
        if (replayEvent.index >= plan.definitions.size())
          return fail(FrameTapeReplayStatus::ObjectCreationFailed, eventIndex);
        const auto& definition = plan.definitions[replayEvent.index];
        const auto normalAlias = std::find_if(normalHiddenAliases.begin(), normalHiddenAliases.end(),
            [&](const auto& value) { return sameIdentity(value.identity, definition.fixed.identity); });
        const auto normalAliasIndex = static_cast<std::size_t>(normalAlias - normalHiddenAliases.begin());
        auto normalAliasNext = normalAliasIndex < normalAliasStates.size()
            ? normalAliasStates[normalAliasIndex] : RenderTapeInitialSetupV4OwnerState{};
        const auto setupOwnerIndex = findSetupOwner(definition.fixed.identity);
        auto setupNext = setupOwnerIndex < setupOwnerStates.size()
            ? setupOwnerStates[setupOwnerIndex]
            : RenderTapeInitialSetupV4OwnerState{};
        if (exactDiagnosticV4) {
          const auto aliases = initialSetupV4->aliases();
          const auto alias = std::find_if(aliases.begin(), aliases.end(),
              [&](const auto& value) { return value.definitionIndex == replayEvent.index; });
          if (alias != aliases.end() &&
              (alias->eventIndex != eventIndex ||
               !sameIdentity(alias->identity, definition.fixed.identity) ||
               alias->parentOwnerIndex >= setupOwnerStates.size() ||
               setupOwnerStates[alias->parentOwnerIndex].phase !=
                   RenderTapeInitialSetupV4OwnerPhase::Active ||
               !render_tape_runtime::findExactObject(objects, alias->parentIdentity))) {
            result.exactDiagnosticV4.failure = "v4-initial-setup-alias-activation";
            return fail(FrameTapeReplayStatus::ObjectCreationFailed, eventIndex);
          }
        }
        auto* staged = texturePrestageAudit
            ? findPrestage(definition.fixed.identity)
            : nullptr;
        const auto checkpoint = std::find_if(checkpointQueries.begin(),
            checkpointQueries.end(), [&](const auto& entry) {
              return sameIdentity(entry.object.identity, definition.fixed.identity);
            });
        void* value = nullptr;
        bool ordinaryAcquisition = false;
        if (setupOwnerIndex < setupOwnerStates.size()) {
          const auto& entry = initialSetupV4->entries()[setupOwnerIndex];
          if (!result.exactDiagnosticV4.initialSetupCompleted ||
              entry.definitionIndex != replayEvent.index ||
              entry.definitionEventIndex != eventIndex ||
              !sameGeneralDefinitionHeader(entry.definition, definition.fixed) ||
              !sameGeneralBytes(entry.descriptor, definition.descriptor) ||
              !setupHiddenObjects[setupOwnerIndex].value ||
              !renderTapeInitialSetupV4Advance(setupNext,
                  RenderTapeInitialSetupV4Action::Activate)) {
            result.exactDiagnosticV4.failure = "v4-initial-setup-owner-activation";
            return fail(FrameTapeReplayStatus::ObjectCreationFailed, eventIndex);
          }
          value = setupHiddenObjects[setupOwnerIndex].value;
        } else if (normalAlias != normalHiddenAliases.end()) {
          if (!result.normalDiagnostic.historyRestored || !normalAlias->value ||
              !renderTapeInitialSetupV4Advance(normalAliasNext, RenderTapeInitialSetupV4Action::Activate)) {
            result.exactDiagnosticV4.failure = "v4-normal-alias-activation";
            return fail(FrameTapeReplayStatus::ObjectCreationFailed, eventIndex);
          }
          value = normalAlias->value;
        } else if (checkpoint != checkpointQueries.end()) {
          if (!checkpoint->object.value ||
              checkpoint->evidenceIndex >= result.queryReplay.checkpoints.size() ||
              !result.queryReplay.checkpoints[checkpoint->evidenceIndex].completed ||
              result.queryReplay.checkpoints[checkpoint->evidenceIndex].activated)
            return fail(FrameTapeReplayStatus::ObjectCreationFailed, eventIndex);
          value = checkpoint->object.value;
        } else if (staged) {
          if (staged->active || staged->hiddenIndex >= hiddenObjects.size() ||
              !hiddenObjects[staged->hiddenIndex].value) {
            return fail(FrameTapeReplayStatus::ObjectCreationFailed, eventIndex);
          }
          value = hiddenObjects[staged->hiddenIndex].value;
        } else {
          value = render_tape_runtime::createRenderTapeObject(
              device, definition.fixed, definition.descriptor, blobs, objects);
          if (!value)
            return fail(FrameTapeReplayStatus::ObjectCreationFailed, eventIndex);
          ordinaryAcquisition = true;
          ++result.conservation.objectsCreated;
          if (texturePrestageAudit) ++texturePrestageAudit->activeObjectsCreated;
        }
        // A freshly acquired wrapper is not yet visible to cleanup(). Keep a
        // transient owner until every metadata append succeeds. Hidden setup,
        // checkpoint and audit owners already have their owning tables.
        const auto releaseTransient = [&](void* pointer) {
          OwnedObject acquired{.identity = definition.fixed.identity, .value = pointer};
          render_tape_runtime::releaseOwnedObject(acquired);
          ++result.conservation.objectsReleased;
          if (texturePrestageAudit) ++texturePrestageAudit->activeObjectsReleased;
        };
        std::unique_ptr<void, decltype(releaseTransient)> transient(
            ordinaryAcquisition ? value : nullptr, releaseTransient);
        if (exactDiagnosticV4 && definition.fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_QUERY &&
            !executionV4.bindQueryObject(device, definition.fixed.identity,
                static_cast<D9CQuery*>(value))) {
          transient.reset();
          return fail(FrameTapeReplayStatus::ObjectCreationFailed, eventIndex);
        }
        definitions.push_back(definition);
        if (freshSeedDrainAudit) definitionEpochs.push_back(commandEpoch);
        objects.push_back({.identity = definition.fixed.identity, .value = value});
        transient.release();
        if (normalAlias != normalHiddenAliases.end()) {
          normalAlias->value = nullptr;
          normalAliasStates[normalAliasIndex] = normalAliasNext;
          ++result.normalDiagnostic.aliasesActivated;
        }
        if (setupOwnerIndex < setupOwnerStates.size()) {
          // No fallible operation intervenes between publishing the active
          // owner and clearing the hidden reference. Activation acquires none.
          setupHiddenObjects[setupOwnerIndex].value = nullptr;
          setupOwnerStates[setupOwnerIndex] = setupNext;
          ++result.exactDiagnosticV4.initialSetupActivated;
        }
        notifyBuffer(setupOwnerIndex < setupOwnerStates.size()
                ? RenderTapePreparedBufferPhaseV4::Activated
                : RenderTapePreparedBufferPhaseV4::Defined,
            eventIndex, definition, nullptr, value);
        if (checkpoint != checkpointQueries.end()) {
          checkpoint->object.value = nullptr;
          result.queryReplay.checkpoints[checkpoint->evidenceIndex].activated = true;
          ++result.queryReplay.checkpointActivated;
        }
        if (staged) {
          // Publish the active bookkeeping before transferring ownership of the
          // hidden wrapper.  If a bookkeeping allocation throws, cleanup still
          // sees the wrapper in the hidden table and releases it exactly once.
          hiddenObjects[staged->hiddenIndex].value = nullptr;
          staged->active = true;
          ++texturePrestageAudit->activatedObjects;
        }
        if (staged) ++result.conservation.objectsCreated;
        if (definition.fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_QUERY) {
          RenderTapeQueryDescriptorDecoded desc{};
          if (!renderTapeLoadQueryDescriptor(definition.descriptor, desc))
            return fail(FrameTapeReplayStatus::QueryOracleUnsupported, eventIndex);
          const auto local = device->dev().captureQueryPollSnapshot(
              static_cast<D9CQuery*>(value)->obj);
          queryStates.push_back({
              .identity = definition.fixed.identity,
              .cycle = renderTapeInitialQueryCycle(desc),
              .localQueryIdentity = local.identity,
              .localIssueOrdinal = local.issueOrdinal,
              .localDeviceEpoch = local.issueDeviceEpoch,
          });
        }
        break;
      }
      case RenderTapeEventType::ObjectDestroy: {
        if (!bootstrapReplayed) {
          // Bootstrap is a real state application, not metadata. Materialize it
          // before retiring any identity it may reference, even when the first
          // command chunk has not arrived yet.
          for (const auto& overlay : bootstrap) {
            if (!replayChunk(overlay))
              return fail(FrameTapeReplayStatus::BootstrapFailed, eventIndex);
          }
          bootstrapReplayed = true;
        }
        if (renderTapeProviderEventRequiresDrain(
                sawSubmittedCommandWork, RenderTapeEventType::ObjectDestroy)) {
          if (auto upper = device->dev().upperDevice()) upper->flush();
        }
        if (replayEvent.index >= plan.destroys.size())
          return fail(FrameTapeReplayStatus::ObjectCreationFailed, eventIndex);
        const auto& fixed = plan.destroys[replayEvent.index];
        const auto object = std::find_if(
            objects.begin(), objects.end(), [&](const auto& value) {
              return value.value && sameIdentity(value.identity, fixed.identity);
            });
        if (object == objects.end())
          return fail(FrameTapeReplayStatus::ObjectCreationFailed, eventIndex);
        const auto setupOwnerIndex = findSetupOwner(fixed.identity);
        if (setupOwnerIndex < setupOwnerStates.size() &&
            !renderTapeInitialSetupV4Advance(setupOwnerStates[setupOwnerIndex],
                RenderTapeInitialSetupV4Action::Release)) {
          result.exactDiagnosticV4.failure = "v4-initial-setup-owner-destruction";
          return fail(FrameTapeReplayStatus::ObjectCreationFailed, eventIndex);
        }
        if (texturePrestageAudit) {
          ++texturePrestageAudit->activeObjectsReleased;
          if (auto* entry = findPrestage(fixed.identity)) entry->active = false;
        }
        notifyActiveBuffer(RenderTapePreparedBufferPhaseV4::Retiring,
            eventIndex, fixed.identity, nullptr, object->value);
        if (!releaseNormalAliasState(fixed.identity)) {
          result.exactDiagnosticV4.failure = "v4-normal-alias-destruction";
          return fail(FrameTapeReplayStatus::ObjectCreationFailed, eventIndex);
        }
        render_tape_runtime::releaseOwnedObject(*object);
        ++result.conservation.objectsReleased;
        break;
      }
      case RenderTapeEventType::ResourceMutation: {
        PreparedPhaseScope phase(timing,
                                 &PreparedPhaseTiming::resourceSetupNanos);
        if (replayEvent.index >= plan.mutations.size()) {
          return fail(FrameTapeReplayStatus::MutationFailed, eventIndex);
        }
        const auto& fixed = plan.mutations[replayEvent.index];
        bool consumeSetupSeed = false;
        if (exactDiagnosticV4) {
          const auto seeds = initialSetupV4->seeds();
          const auto seed = std::find_if(seeds.begin(), seeds.end(),
              [&](const auto& value) { return value.mutationIndex == replayEvent.index; });
          if (seed != seeds.end()) {
            const auto seedIndex = static_cast<std::size_t>(seed - seeds.begin());
            if (!result.exactDiagnosticV4.initialSetupCompleted ||
                seed->eventIndex != eventIndex ||
                seed->ownerIndex >= setupOwnerStates.size() ||
                !sameGeneralMutationHeader(seed->mutation, fixed)) {
              result.exactDiagnosticV4.failure = "v4-initial-setup-seed-binding";
              return fail(FrameTapeReplayStatus::MutationFailed, eventIndex);
            }
            const auto& entry = initialSetupV4->entries()[seed->ownerIndex];
            if (entry.mode != RenderTapeInitialSetupV4Mode::Seeded ||
                seedIndex < entry.firstSeedIndex ||
                seedIndex - entry.firstSeedIndex >= entry.seedCount ||
                !render_tape_runtime::findExactObject(objects, fixed.identity) ||
                !renderTapeInitialSetupV4Advance(setupOwnerStates[seed->ownerIndex],
                    RenderTapeInitialSetupV4Action::ConsumeSeed,
                    static_cast<std::uint32_t>(seedIndex - entry.firstSeedIndex))) {
              result.exactDiagnosticV4.failure = "v4-initial-setup-seed-consumption";
              return fail(FrameTapeReplayStatus::MutationFailed, eventIndex);
            }
            consumeSetupSeed = true;
            ++result.exactDiagnosticV4.initialSetupConsumed;
          }
        }
        bool consumeStagedSeed = false;
        if (texturePrestageAudit) {
          if (auto* staged = findPrestage(fixed.identity)) {
            if (!staged->active || fixed.subresource >= staged->seedDigests.size() ||
                staged->consumed[fixed.subresource] ||
                !sameDigest(staged->seedDigests[fixed.subresource], fixed.digest)) {
              return fail(FrameTapeReplayStatus::MutationFailed, eventIndex);
            }
            staged->consumed[fixed.subresource] = true;
            consumeStagedSeed = true;
            ++texturePrestageAudit->consumedSeeds;
            texturePrestageAudit->complete =
                texturePrestageAudit->consumedSeeds ==
                texturePrestageAudit->candidateSubresources;
          }
        }
        bool hasSurfaceAlias = false;
        bool priorSeedForSubresource = false;
        bool freshBypass = false;
        if (freshSeedDrainAudit) {
          const auto definitionIndex = std::find_if(
              definitions.begin(), definitions.end(), [&](const auto& value) {
                return sameIdentity(value.fixed.identity, fixed.identity);
              });
          const auto definitionOffset = definitionIndex == definitions.end()
              ? definitions.size()
              : static_cast<std::size_t>(definitionIndex - definitions.begin());
          hasSurfaceAlias = fixed.identity.kind ==
                  D9C_CHUNK_HANDLE_KIND_TEXTURE &&
              std::any_of(plan.definitions.begin(), plan.definitions.end(),
                          [&](const auto& value) {
                            if (value.fixed.identity.kind !=
                                D9C_CHUNK_HANDLE_KIND_SURFACE) {
                              return false;
                            }
                            RenderTapeSurfaceDescriptorV2 surface{};
                            return renderTapeLoadSurfaceDescriptorV2(
                                       value.descriptor, surface) &&
                                   surface.storage == static_cast<std::uint32_t>(
                                       RenderTapeSurfaceStorage::TextureSubresource) &&
                                   sameIdentity(surface.parentTexture,
                                                fixed.identity);
                          });
          priorSeedForSubresource = std::any_of(
              seededSubresources.begin(), seededSubresources.end(),
              [&](const auto& value) {
                return value.subresource == fixed.subresource &&
                       sameIdentity(value.identity, fixed.identity);
              });
          const auto* seedBlob = findBlob(blobs, fixed.digest);
          freshBypass = definitionOffset < definitionEpochs.size() &&
              seedBlob &&
              renderTapeFreshSeedDrainBypassAllowed(
                  definitions[definitionOffset].fixed,
                  definitions[definitionOffset].descriptor, fixed,
                  seedBlob->bytes,
                  definitionEpochs[definitionOffset] == commandEpoch,
                  hasSurfaceAlias, priorSeedForSubresource);
        }
        const bool needsDrain = renderTapeProviderEventRequiresDrain(
            sawSubmittedCommandWork, RenderTapeEventType::ResourceMutation);
        const bool noOverwriteBypass = exactDiagnosticV4 && needsDrain &&
            omitNoOverwritePreflush(eventIndex, fixed);
        if (consumeSetupSeed || consumeStagedSeed) {
          // The hidden upload was already checked and completed; the logical
          // mutation event still advances the ordinary seeded-attachment state
          // below, but must not apply the bytes a second time.
        } else if (noOverwriteBypass) {
          // Preserve the original production NoOverwrite range write. Only
          // this provider's additional preflush is omitted; Lock/Unlock and
          // every other mutation/destruction synchronization remain unchanged.
        } else if (freshSeedDrainAudit && needsDrain && freshBypass) {
          ++freshSeedDrainAudit->bypassedDrains;
        } else if (needsDrain) {
          if (freshSeedDrainAudit) ++freshSeedDrainAudit->requiredDrains;
          if (auto upper = device->dev().upperDevice()) {
            upper->flush();
            if (exactDiagnosticV4 && eventIndex >= executionV4.snapshotPrefixEvents())
              ++result.exactDiagnosticV4.intervalMutationsWithExtraDrain;
          }
        }
        if (!consumeSetupSeed && !consumeStagedSeed &&
            !applyGeneralMutation(device, fixed, blobs, definitions, objects,
                normalHistory && eventIndex < executionV4.snapshotPrefixEvents())) {
          return fail(FrameTapeReplayStatus::MutationFailed, eventIndex);
        }
        if (observerSession.enabled() && fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER) {
          notifyActiveBuffer(consumeSetupSeed
                  ? RenderTapePreparedBufferPhaseV4::SeedConsumed
                  : RenderTapePreparedBufferPhaseV4::MutationApplied,
              eventIndex, fixed.identity, &fixed,
              render_tape_runtime::findExactObject(objects, fixed.identity));
        }
        if (noOverwriteBypass)
          ++result.exactDiagnosticV4.noOverwriteWritesWithoutExtraDrain;
        if (exactDiagnosticV4 && eventIndex < executionV4.snapshotPrefixEvents()) {
          const auto witness = std::find_if(prefixBufferSeeds.begin(), prefixBufferSeeds.end(),
              [&](const auto& value) {
                return value.mutationIndex == replayEvent.index &&
                    value.mutationEventIndex == eventIndex;
              });
          if (witness != prefixBufferSeeds.end()) witness->uploadReplayed = true;
        }
        if (freshSeedDrainAudit) {
          ++freshSeedDrainAudit->inspectedMutations;
          freshSeedDrainAudit->complete =
              freshSeedDrainAudit->inspectedMutations ==
              freshSeedDrainAudit->mutationCount;
        }
        if (initialSeedAudit && !sawSubmittedCommandWork &&
            !initialSeedAuditDone) {
          initialSeedMutations.push_back(fixed);
        }
        seededSubresources.push_back(
            {.identity = fixed.identity, .subresource = fixed.subresource});
        if (!normalHistory) restoreSeededColorAttachmentTouches(
            device, definitions, objects, seededSubresources);
        break;
      }
      case RenderTapeEventType::CommandChunk: {
        if (replayEvent.index >= plan.commandChunks.size())
          return fail(FrameTapeReplayStatus::CommandReplayFailed, eventIndex);
        const auto& chunk = plan.commandChunks[replayEvent.index];
        if (!bootstrapReplayed) {
          if (initialSeedAudit && !initialSeedAuditDone) {
            auditInitialSeedMutations(device, definitions, initialSeedMutations,
                                      objects, blobs, *initialSeedAudit);
            initialSeedAuditDone = true;
          }
          if (!normalHistory) restoreSeededColorAttachmentTouches(
              device, definitions, objects, seededSubresources);
          for (const auto& overlay : bootstrap) {
            if (!replayChunk(overlay))
              return fail(FrameTapeReplayStatus::BootstrapFailed, eventIndex);
          }
          bootstrapReplayed = true;
        }
        if (chunkContainsPresent(chunk) && !installOutput())
          return fail(FrameTapeReplayStatus::PresentOutputFailed, eventIndex);
        std::span<const ProviderReplayRecordRange> sourceCuts;
        std::span<const CaptureSourceReplayRow> sourceRows;
        if (sourcePolicy) {
          sourceCuts = sourcePolicy->rangesForCommandChunk(replayEvent.index);
          const auto rows = sourcePolicy->rows();
          const auto first = std::find_if(
              rows.begin(), rows.end(), [&](const auto& row) {
                return row.commandChunkIndex == replayEvent.index;
              });
          if (first != rows.end()) {
            const auto last = std::find_if(
                first, rows.end(), [&](const auto& row) {
                  return row.commandChunkIndex != replayEvent.index;
                });
            sourceRows = std::span<const CaptureSourceReplayRow>(
                &*first, static_cast<std::size_t>(last - first));
          }
        } else if (sourceBoundaries) {
          sourceCuts = sourceBoundaries->sources.rangesForCommandChunk(
              replayEvent.index);
        }
        if (!replayChunk(chunk, sourceCuts,
                               sourceBoundaries
                                   ? std::span<const std::uint32_t>(
                                         sourceBoundaries->batchCuts[replayEvent.index])
                                   : std::span<const std::uint32_t>{},
                               sourceRows, sourcePolicyV3 != nullptr,
                               exactDiagnosticV4 != nullptr))
          return fail(FrameTapeReplayStatus::CommandReplayFailed, eventIndex);
        if (result.queryReplay.present) {
          ImportedChunkView imported;
          if (!importPrevalidatedCommandChunk(chunk.bytes, chunk.envelope, imported))
            return fail(FrameTapeReplayStatus::CommandReplayFailed, eventIndex);
          for (std::size_t i = 0u; i < imported.records.size(); ++i) {
            const auto record = imported.record(i);
            if (record.header.type != D9C_COMMAND_RECORD_QUERY_ISSUE) continue;
            D9CCommandChunkWireQueryIssue issue{};
            if (!load(record.payload, 0u, issue) ||
                issue.queryHandleIndex >= imported.handles.size())
              return fail(FrameTapeReplayStatus::QueryOracleFailed, eventIndex);
            const auto& handle = imported.handles[issue.queryHandleIndex];
            const D9CWireObjectIdentity identity{handle.kind, handle.generation,
                                                handle.objectId};
            const auto state = std::find_if(queryStates.begin(), queryStates.end(),
                [&](const auto& value) { return sameIdentity(value.identity, identity); });
            if (state == queryStates.end() ||
                !renderTapeAdvanceQueryCycle(state->cycle, issue.flags, true)) {
              result.queryReplay.failure = RenderTapeQueryReplayFailure::CycleMismatch;
              return fail(FrameTapeReplayStatus::QueryOracleFailed, eventIndex);
            }
            ++state->localIssueOrdinal;
            state->localDeviceEpoch = static_cast<D9CQuery*>(
                render_tape_runtime::findExactObject(objects, identity))
                ->obj->issueDeviceEpoch();
            ++result.queryReplay.replayedIssueRecords;
          }
        }
        sawSubmittedCommandWork = true;
        if (freshSeedDrainAudit) ++commandEpoch;
        break;
      }
      case RenderTapeEventType::PresentComplete: {
        PreparedReplayCpuScope readbackCpu(
            exactDiagnosticV4 != nullptr, &executionV4, eventIndex, 3u);
        if (textureInput && (!textureInputEvidence ||
                            !textureInputEvidence->readbackMatched))
          return fail(FrameTapeReplayStatus::MutationFailed, eventIndex);
        PreparedPhaseScope phase(timing,
                                 &PreparedPhaseTiming::readbackDrainNanos);
        auto upper = device->dev().upperDevice();
        auto& evidence = result.intervals[0].validity;
        evidence.expectedDigestCaptured =
            plan.expectedDigestValidity == RenderTapeDigestValidity::Sha256;
        if (upper && readbackTarget) {
          upper->flush();
          core::ReadbackPixels pixels;
          if (!upper->readbackSurface(
                  core::ReadbackDesc{.source = readbackTarget->obj->handle()},
                  pixels)) {
            return fail(FrameTapeReplayStatus::ReadbackFailed, eventIndex);
          }
          constexpr std::uint64_t bytesPerPixel = 4u;
          std::uint64_t tightPitch = 0u;
          std::uint64_t tightBytes = 0u;
          std::uint64_t pitchedBytes = 0u;
          if (!checkedMul(plan.outputDesc.width, bytesPerPixel, tightPitch) ||
              !checkedMul(tightPitch, plan.outputDesc.height, tightBytes) ||
              !checkedMul(pixels.pitch, plan.outputDesc.height, pitchedBytes) ||
              pixels.pitch < tightPitch ||
              tightBytes > std::numeric_limits<std::size_t>::max() ||
              pixels.bytes.size() < pitchedBytes) {
            return fail(FrameTapeReplayStatus::ReadbackFailed, eventIndex);
          }
          std::vector<std::byte> tight(static_cast<std::size_t>(tightBytes));
          for (std::uint32_t row = 0u; row < plan.outputDesc.height; ++row) {
            std::memcpy(
                tight.data() + static_cast<std::size_t>(row * tightPitch),
                pixels.bytes.data() + static_cast<std::size_t>(row) * pixels.pitch,
                static_cast<std::size_t>(tightPitch));
          }
          evidence.outputReadback = true;
          evidence.outputBytes = tight.size();
          evidence.outputDigest = RenderTapeCaptureSession::sha256(tight);
          if (evidence.expectedDigestCaptured) {
            evidence.expectedOutputDigest = plan.expectedDigest;
          }
          evidence.expectedDigestMatched =
              evidence.expectedDigestCaptured &&
              sameDigest(evidence.outputDigest, plan.expectedDigest);
          const auto pixelSize = std::min<std::size_t>(4u, tight.size());
          for (std::size_t index = pixelSize; index < tight.size(); ++index) {
            if (tight[index] != tight[index % pixelSize]) {
              evidence.outputNonDegenerate = true;
              break;
            }
          }
          result.outputPixels = std::move(tight);
        }
        if (upper) {
          auto* source = static_cast<D9CSurface*>(
              render_tape_runtime::findExactObject(objects, plan.outputIdentity));
          std::vector<std::byte> sourcePixels;
          if (readbackTightSurface(device, source, plan.outputDesc.width,
                                   plan.outputDesc.height, plan.outputDesc.format,
                                   sourcePixels)) {
            evidence.sourceReadback = true;
            evidence.sourceBytes = sourcePixels.size();
            evidence.sourceDigest =
                RenderTapeCaptureSession::sha256(sourcePixels);
            result.sourcePixels = std::move(sourcePixels);
          }
        }
        if (evidence.expectedDigestCaptured && !evidence.expectedDigestMatched) {
          result.validity = evidence;
          result.validity.structurallyValid = true;
          result.validity.digestsValid = true;
          if (!renderTapeV4TerminalOutputMismatchMayDefer(
                  exactDiagnosticV4 != nullptr, replayEvent.type, eventIndex,
                  plan.events.back().eventIndex, evidence))
            return fail(FrameTapeReplayStatus::OutputMismatch, eventIndex);
          // Pixel rejection is independent of actual traversal/query/source
          // qualification. Continue once through the original terminal ack
          // and existing finalizers, never through recovery work or a retry.
          deferredOutputMismatchEvent = eventIndex;
        }
        break;
      }
      case RenderTapeEventType::OrderedControl: {
        if (replayEvent.index >= plan.controls.size() ||
            replayEvent.index >= plan.controlBytes.size()) {
          return fail(FrameTapeReplayStatus::UnsupportedGrammar, eventIndex);
        }
        const auto& fixed = plan.controls[replayEvent.index];
        if (fixed.kind == static_cast<std::uint32_t>(
                              RenderTapeControlKind::QueryGetData)) {
          const auto started = std::chrono::steady_clock::now();
          RenderTapeQueryGetDataControlV2 captured{};
          const auto& bytes = plan.controlBytes[replayEvent.index];
          auto* query = static_cast<D9CQuery*>(
              render_tape_runtime::findExactObject(objects, fixed.identity));
          const auto state = std::find_if(queryStates.begin(), queryStates.end(),
              [&](const auto& value) { return sameIdentity(value.identity, fixed.identity); });
          if (bytes.size() != sizeof(captured) || !load(bytes, 0u, captured) ||
              !query || !query->obj || state == queryStates.end() ||
              !renderTapeQueryPollCycleAdmitted(state->cycle, captured.issueCycle,
                                               fixed.resultCode)) {
            result.queryReplay.failure = RenderTapeQueryReplayFailure::CycleMismatch;
            return fail(FrameTapeReplayStatus::QueryOracleFailed, eventIndex);
          }
          std::uint64_t staged = 0u;
          const auto observed = dxmt9c_query_get_data(
              query, captured.hasOutput ? &staged : nullptr,
              static_cast<std::uint32_t>(captured.dataSize), captured.flags);
          auto snapshot = device->dev().captureQueryPollSnapshot(query->obj);
          if (snapshot.identity != state->localQueryIdentity ||
              snapshot.issueOrdinal != state->localIssueOrdinal ||
              snapshot.issueDeviceEpoch != state->localDeviceEpoch) {
            result.queryReplay.failure = RenderTapeQueryReplayFailure::CycleMismatch;
            return fail(FrameTapeReplayStatus::QueryOracleFailed, eventIndex);
          }
          const auto fence = snapshot.backendFence.snapshot();
          RenderTapeQueryReplayRow row{
              .eventIndex = eventIndex,
              .eventOrdinal = plan.tape.event(eventIndex).header.ordinal,
              .identity = fixed.identity,
              .issueCycle = captured.issueCycle,
              .flags = captured.flags,
              .hasOutput = captured.hasOutput != 0u,
              .dataSize = captured.dataSize,
              .capturedResult = fixed.resultCode,
              .replayResult = observed,
              .oracleResult = observed,
              .definedBytes = captured.definedBytes,
              .capturedBytes = captured.result,
              .localQueryIdentity = snapshot.identity,
              .localIssueOrdinal = snapshot.issueOrdinal,
              .localDeviceEpoch = snapshot.issueDeviceEpoch,
              .localQueue = fence.queue,
              .localSequence = fence.sequence,
              .localInitializer = fence.initializer,
              .resultRequired = fixed.resultCode == core::S_OK,
          };
          if (snapshot.backend) {
            const auto progress = snapshot.backend->queryProgress();
            row.completedSequence = progress.completed;
            row.completedInitializer = progress.initializerCompleted;
          }
          if (observed == core::S_OK) {
            ++result.queryReplay.replayReady;
            if (row.resultRequired) {
              row.readinessSatisfied = true;
              ++result.queryReplay.readinessSatisfied;
              std::memcpy(row.replayBytes.data(), &staged, row.definedBytes);
              row.resultMatched = row.definedBytes != 0u && std::equal(row.capturedBytes.begin(),
                  row.capturedBytes.begin() + row.definedBytes, row.replayBytes.begin());
              if (row.resultMatched && row.definedBytes != 0u)
                ++result.queryReplay.matchedResults;
            }
          } else if (observed == core::S_FALSE) {
            ++result.queryReplay.replayPending;
          }
          ++result.queryReplay.replayedPolls;
          result.queryReplay.rows.push_back(row);
          result.queryReplay.transcriptWallNanos += elapsedNanos(
              started, std::chrono::steady_clock::now());
          if ((fixed.resultCode < 0 && observed != fixed.resultCode) ||
              (fixed.resultCode >= 0 && observed != core::S_OK &&
               observed != core::S_FALSE)) {
            result.queryReplay.failure = RenderTapeQueryReplayFailure::PollFailed;
            return fail(FrameTapeReplayStatus::QueryOracleFailed, eventIndex);
          }
          if (row.resultRequired && row.definedBytes != 0u &&
              observed == core::S_OK && !row.resultMatched) {
            result.queryReplay.failure = RenderTapeQueryReplayFailure::ResultMismatch;
            return fail(FrameTapeReplayStatus::QueryOracleFailed, eventIndex);
          }
          if (row.resultRequired && observed == core::S_FALSE) {
            queryObligations.push_back({
                .rowIndex = result.queryReplay.rows.size() - 1u,
                .snapshot = std::move(snapshot),
            });
          }
          break;
        }
        if (fixed.kind != static_cast<std::uint32_t>(
                RenderTapeControlKind::GammaRampSet) ||
            fixed.controlBytes != kRenderTapeGammaRampBytes ||
            plan.controlBytes[replayEvent.index].size() !=
                kRenderTapeGammaRampBytes) {
          return fail(FrameTapeReplayStatus::UnsupportedGrammar, eventIndex);
        }
        const auto& gamma = plan.controlBytes[replayEvent.index];
        if (!applyGammaRamp(device, gamma))
          return fail(FrameTapeReplayStatus::UnsupportedGrammar, eventIndex);
        break;
      }
      }
      if (sourcePolicyV3 && replayEvent.type != RenderTapeEventType::CommandChunk &&
          !executionV3.replayedEvent(eventIndex))
        return fail(FrameTapeReplayStatus::CommandReplayFailed, eventIndex);
      if (exactDiagnosticV4 && replayEvent.type != RenderTapeEventType::CommandChunk &&
          !executionV4.replayedEvent(eventIndex))
        return fail(FrameTapeReplayStatus::CommandReplayFailed, eventIndex);
    }
  } catch (...) {
    // Staged owners are deliberately still visible to cleanup if any
    // bookkeeping allocation or backend wrapper operation throws while the
    // logical event stream is being consumed.
    if (!sourcePolicyV3 && !exactDiagnosticV4 && !texturePrestageAudit &&
        checkpointQueries.empty()) throw;
    result.status = FrameTapeReplayStatus::ObjectCreationFailed;
    cleanup();
    return result;
  }

  intervalCpu.reset();
  if (result.queryReplay.present) {
    result.queryReplay.transcriptComplete =
        result.queryReplay.replayedPolls == result.queryReplay.recordedPolls &&
        result.queryReplay.replayedIssueRecords == result.queryReplay.issueRecords;
    for (const auto& obligation : queryObligations) {
      auto& row = result.queryReplay.rows[obligation.rowIndex];
      const auto failure = render_tape_runtime::settleQueryOracle(
          device, obligation.snapshot, row, result.queryReplay);
      if (failure != RenderTapeQueryReplayFailure::None) {
        result.queryReplay.failure = failure;
        return fail(failure == RenderTapeQueryReplayFailure::ProgressExhausted
                        ? FrameTapeReplayStatus::QueryProgressExhausted
                        : FrameTapeReplayStatus::QueryOracleFailed,
                    row.eventIndex);
      }
    }
    result.queryReplay.oracleComplete = result.queryReplay.transcriptComplete &&
        result.queryReplay.readinessSatisfied == result.queryReplay.readinessRequired &&
        result.queryReplay.matchedResults == result.queryReplay.requiredResults;
    result.queryReplay.gpuResultOracle = result.queryReplay.actualMetal &&
        result.queryReplay.oracleComplete && result.queryReplay.capturedReady != 0u;
    if (!result.queryReplay.oracleComplete)
      return fail(FrameTapeReplayStatus::QueryOracleFailed, 0xffffffffu);
  }

  if (!bootstrapReplayed || (output && output->scheduledCount() != 1u)) {
    result.status = FrameTapeReplayStatus::PresentOutputFailed;
  }
  if (exactDiagnosticV4 &&
      (!result.exactDiagnosticV4.initialSetupCompleted ||
       result.exactDiagnosticV4.initialSetupActivated != initialSetupV4->entries().size() ||
       result.exactDiagnosticV4.initialSetupConsumed != initialSetupV4->seeds().size() ||
       std::any_of(setupOwnerStates.begin(), setupOwnerStates.end(), [](const auto& state) {
         return state.consumedSeeds != state.seedCount ||
             (state.phase != RenderTapeInitialSetupV4OwnerPhase::Active &&
              state.phase != RenderTapeInitialSetupV4OwnerPhase::Released);
       }))) {
    result.exactDiagnosticV4.failure = "v4-initial-setup-transcript-incomplete";
    return fail(FrameTapeReplayStatus::CommandReplayFailed, 0xffffffffu);
  }
  if (sourcePolicyV3 && !executionV3.finish())
    return fail(FrameTapeReplayStatus::CommandReplayFailed, 0xffffffffu);
  if (exactDiagnosticV4) {
    const auto finished = [&] {
      PreparedPhaseScope phase(timing, &PreparedPhaseTiming::v4FinishNanos);
      return executionV4.finish();
    }();
    if (!finished)
      return fail(FrameTapeReplayStatus::CommandReplayFailed, 0xffffffffu);
  }
  if (normalHistory) {
    const auto profile = replayUpper->queue().renderTapeActionProfileSnapshot();
    result.normalDiagnostic.unchangedNormal = profile.valid && !profile.constructionProfile &&
        !profile.currentProfile && !profile.revision;
    if (!result.normalDiagnostic.historyRestored || !result.normalDiagnostic.unchangedNormal ||
        result.normalDiagnostic.aliasesPrepared != normalHiddenAliases.size() ||
        result.normalDiagnostic.aliasesActivated != normalHiddenAliases.size()) {
      result.exactDiagnosticV4.failure = "v4-normal-history-transcript-incomplete";
      return fail(FrameTapeReplayStatus::CommandReplayFailed, 0xffffffffu);
    }
  }
  result.validity = result.intervals[0].validity;
  result.validity.structurallyValid = true;
  result.validity.digestsValid = true;
  cleanup();
  renderTapeReduceDeferredOutputMismatch(result, deferredOutputMismatchEvent);
  return result;
}

} // namespace

struct PreparedRenderTape::Impl {
  struct V4Preparation {
    CaptureSourceReplayPlanV4 sourcePlan{};
    RenderTapeExecutionV4Preparation execution{};
    RenderTapeInitialSetupV4Plan initialSetup{};
    std::unique_ptr<NormalHistorySetup> normalHistory;
    RenderTapeExecutionV4PublicationMode publicationMode =
        RenderTapeExecutionV4PublicationMode::ExactSourceDiagnostic;
    RenderTapeExecutionV4ProducerContract producerContract =
        RenderTapeExecutionV4ProducerContract::None;
  };
  std::vector<std::byte> tape{};
  std::vector<std::vector<std::byte>> blobStorage{};
  std::vector<RenderTapeProviderBlob> blobs{};
  PreflightPlan boundedPlan{};
  GeneralPreflightPlan generalPlan{};
  std::unique_ptr<V4Preparation> v4{};
  bool general = false;
  FrameTapeReplayResult preflight{};
  RenderTapePreparedEvidence evidence{};
};

namespace {

const FrameTapeReplayResult& emptyPreparedResult() noexcept {
  static const FrameTapeReplayResult result{};
  return result;
}

std::uint64_t elapsedNanos(
    std::chrono::steady_clock::time_point begin,
    std::chrono::steady_clock::time_point end) noexcept {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<
      std::chrono::nanoseconds>(end - begin).count());
}

void attachPreparedEvidence(FrameTapeReplayResult& result,
                            const RenderTapePreparedEvidence& evidence) {
  result.prepared = evidence;
}

enum class WarmResetAttempt : std::uint8_t {
  Succeeded,
  Unsupported,
  Failed,
};

WarmResetAttempt resetPreparedDevice(
    D9CDevice* device, const FrameTapeReplayRequirements& requirements) noexcept {
  if (!device || device->replayDrainLedger.terminal()) {
    return WarmResetAttempt::Unsupported;
  }
  if (!drainDeferredReplay(device, "render-tape-prepared-reset") ||
      device->wireObjects.activeCount() != 0u) {
    return WarmResetAttempt::Unsupported;
  }

  D9CPresentParams params{};
  params.backBufferWidth = requirements.outputWidth;
  params.backBufferHeight = requirements.outputHeight;
  params.backBufferFormat = requirements.outputFormat;
  params.backBufferCount = 1u;
  params.swapEffect = 1u;
  params.windowed = 1u;
  params.presentationInterval = 0x80000000u;
  if (dxmt9c_device_reset(device, &params) != core::D3D_OK) {
    return WarmResetAttempt::Failed;
  }
  if (!drainDeferredReplay(device, "render-tape-prepared-reset-post") ||
      device->wireObjects.activeCount() != 0u) {
    return WarmResetAttempt::Failed;
  }
  return WarmResetAttempt::Succeeded;
}

} // namespace

PreparedRenderTape::PreparedRenderTape() = default;
PreparedRenderTape::~PreparedRenderTape() = default;
PreparedRenderTape::PreparedRenderTape(PreparedRenderTape&&) noexcept = default;
PreparedRenderTape& PreparedRenderTape::operator=(PreparedRenderTape&&) noexcept =
    default;

const FrameTapeReplayResult& PreparedRenderTape::preflightResult() const noexcept {
  return impl_ ? impl_->preflight : emptyPreparedResult();
}

const FrameTapeReplayResult& PreparedRenderTape::preflight() const noexcept {
  return preflightResult();
}

const RenderTapePreparedEvidence& PreparedRenderTape::evidence() const noexcept {
  static const RenderTapePreparedEvidence empty{};
  return impl_ ? impl_->evidence : empty;
}

std::uint64_t PreparedRenderTape::prepareCount() const noexcept {
  return impl_ ? impl_->evidence.prepareCount : 0u;
}

bool PreparedRenderTape::valid() const noexcept {
  return impl_ && impl_->preflight.complete();
}

bool renderTapeV4TerminalOutputMismatchMayDefer(
    bool exactDiagnosticV4, RenderTapeEventType eventType,
    std::uint32_t eventIndex, std::uint32_t terminalEventIndex,
    const FrameTapeValidityEvidence& validity) noexcept {
  return exactDiagnosticV4 && eventType == RenderTapeEventType::PresentComplete &&
      eventIndex != 0xffffffffu && eventIndex == terminalEventIndex &&
      validity.outputReadback && validity.expectedDigestCaptured &&
      !validity.expectedDigestMatched;
}

void renderTapeReduceDeferredOutputMismatch(
    FrameTapeReplayResult& result, std::uint32_t mismatchedEventIndex) noexcept {
  if (mismatchedEventIndex == 0xffffffffu ||
      result.status != FrameTapeReplayStatus::Complete ||
      !result.validity.outputReadback || !result.validity.expectedDigestCaptured ||
      result.validity.expectedDigestMatched) return;
  result.status = FrameTapeReplayStatus::OutputMismatch;
  result.failedEventIndex = mismatchedEventIndex;
}

std::uint32_t renderTapeBufferMutationLockFlags(
    RenderTapeBufferMutationDisposition disposition) noexcept {
  // D3DLOCK_NOOVERWRITE and D3DLOCK_DISCARD are part of the PE-facing D3D9
  // contract, but this unix-side provider intentionally does not include the
  // Windows headers.
  switch (disposition) {
  case RenderTapeBufferMutationDisposition::Plain:
    return 0u;
  case RenderTapeBufferMutationDisposition::NoOverwrite:
    return 0x1000u;
  case RenderTapeBufferMutationDisposition::Discard:
    return 0x2000u;
  }
  return std::numeric_limits<std::uint32_t>::max();
}

bool renderTapeBufferMutationReplayPlan(
    const RenderTapeResourceMutationHeader& mutation,
    RenderTapeBufferMutationReplayPlan& out) noexcept {
  if ((mutation.kind !=
           static_cast<std::uint32_t>(RenderTapeMutationKind::CpuUnlock) &&
       mutation.kind !=
           static_cast<std::uint32_t>(RenderTapeMutationKind::Upload)) ||
      mutation.identity.kind != D9C_CHUNK_HANDLE_KIND_BUFFER ||
      mutation.subresource != 0u || mutation.byteSize == 0u ||
      mutation.byteOffset > std::numeric_limits<std::uint32_t>::max() ||
      mutation.byteSize > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  const auto flags = renderTapeBufferMutationLockFlags(
      static_cast<RenderTapeBufferMutationDisposition>(
          mutation.bufferDisposition));
  if (flags == std::numeric_limits<std::uint32_t>::max()) return false;
  out = {.byteOffset = static_cast<std::uint32_t>(mutation.byteOffset),
         .byteSize = static_cast<std::uint32_t>(mutation.byteSize),
         .lockFlags = flags};
  return true;
}

bool renderTapeFreshSeedDrainBypassAllowed(
    const RenderTapeObjectDefineHeader& definition,
    std::span<const std::byte> descriptor,
    const RenderTapeResourceMutationHeader& mutation,
    std::span<const std::byte> mutationBytes,
    bool defineEpochMatchesCommandEpoch,
    bool hasSurfaceAlias,
    bool priorSeedForSubresource) noexcept {
  if (!defineEpochMatchesCommandEpoch || hasSurfaceAlias ||
      priorSeedForSubresource ||
      !sameIdentity(definition.identity, mutation.identity) ||
      mutation.byteOffset != 0u || mutation.byteSize == 0u ||
      mutationBytes.size() != mutation.byteSize ||
      (mutation.kind != static_cast<std::uint32_t>(
                           RenderTapeMutationKind::Upload) &&
       mutation.kind != static_cast<std::uint32_t>(
                           RenderTapeMutationKind::CpuUnlock))) {
    return false;
  }

  if (mutation.identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER) {
    D9CBufferDesc buffer{};
    return mutation.subresource == 0u && descriptor.size() == sizeof(buffer) &&
           load(descriptor, 0u, buffer) && buffer.size != 0u &&
           definition.expectedContentCount == 1u &&
           definition.expectedContentBytes == buffer.size &&
           mutation.byteSize == buffer.size;
  }
  if (mutation.identity.kind != D9C_CHUNK_HANDLE_KIND_TEXTURE) return false;

  RenderTapeTextureDescriptorV2 texture{};
  D9CSurfaceDesc surface{};
  if (!renderTapeLoadTextureDescriptorV2(descriptor, texture) ||
      texture.initialContentDisposition != static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::CompleteSeed) ||
      mutation.subresource >= texture.subresourceCount ||
      definition.expectedContentCount != texture.subresourceCount ||
      !renderTapeTextureSubresourceDescriptor(descriptor, mutation.subresource,
                                               surface)) {
    return false;
  }
  const auto expected = renderTapeDeriveExpectedSurfaceContent(surface);
  return expected.status == RenderTapeExpectedContentStatus::Accepted &&
         expected.bytes == mutation.byteSize;
}

bool renderTapeTexturePrestageOrderAllowed(
    std::uint32_t definitionPosition,
    std::span<const std::uint32_t> seedPositions,
    std::span<const std::uint32_t> commandPositions) noexcept {
  if (seedPositions.empty()) return false;
  std::uint32_t lastSeed = definitionPosition;
  for (const auto position : seedPositions) {
    if (position <= definitionPosition) return false;
    lastSeed = std::max(lastSeed, position);
  }
  return std::none_of(commandPositions.begin(), commandPositions.end(),
                      [&](const auto position) {
                        return definitionPosition <= position &&
                               position <= lastSeed;
                      });
}

bool renderTapeTexturePrestageCandidate(
    const RenderTapeObjectDefineHeader& definition,
    std::span<const std::byte> descriptor,
    std::span<const RenderTapeResourceMutationHeader> mutations,
    bool hasSurfaceAlias, bool hasSlotGenerationConflict,
    bool invalidEventOrder) noexcept {
  if (hasSurfaceAlias || hasSlotGenerationConflict ||
      invalidEventOrder ||
      definition.identity.kind != D9C_CHUNK_HANDLE_KIND_TEXTURE ||
      definition.descriptorKind != static_cast<std::uint32_t>(
          RenderTapeDescriptorKind::Texture)) {
    return false;
  }
  RenderTapeTextureDescriptorV2 texture{};
  if (!renderTapeLoadTextureDescriptorV2(descriptor, texture) ||
      texture.initialContentDisposition != static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::CompleteSeed) ||
      texture.subresourceCount == 0u ||
      (texture.dimension != static_cast<std::uint32_t>(
           RenderTapeTextureDimension::Texture2D) &&
       texture.dimension != static_cast<std::uint32_t>(
           RenderTapeTextureDimension::Cube)) ||
      definition.expectedContentCount != texture.subresourceCount) {
    return false;
  }

  std::size_t ownerMutationCount = 0u;
  for (const auto& mutation : mutations) {
    if (!sameIdentity(mutation.identity, definition.identity)) continue;
    if (mutation.subresource >= texture.subresourceCount) return false;
    ++ownerMutationCount;
  }
  if (ownerMutationCount != texture.subresourceCount) return false;

  std::uint64_t totalBytes = 0u;
  for (std::uint32_t subresource = 0u;
       subresource < texture.subresourceCount; ++subresource) {
    D9CSurfaceDesc surface{};
    if (!renderTapeTextureSubresourceDescriptor(descriptor, subresource,
                                                surface)) {
      return false;
    }
    const auto expected = renderTapeDeriveExpectedSurfaceContent(surface);
    if (expected.status != RenderTapeExpectedContentStatus::Accepted ||
        expected.bytes > std::numeric_limits<std::uint64_t>::max() - totalBytes) {
      return false;
    }
    totalBytes += expected.bytes;
    std::size_t count = 0u;
    for (const auto& mutation : mutations) {
      if (!sameIdentity(mutation.identity, definition.identity) ||
          mutation.subresource != subresource) {
        continue;
      }
      ++count;
      if ((mutation.kind != static_cast<std::uint32_t>(
                              RenderTapeMutationKind::Upload) &&
           mutation.kind != static_cast<std::uint32_t>(
                              RenderTapeMutationKind::CpuUnlock)) ||
          mutation.byteOffset != 0u || mutation.byteSize != expected.bytes) {
        return false;
      }
    }
    if (count != 1u) return false;
  }
  return definition.expectedContentBytes == totalBytes;
}

bool renderTapeProviderEventRequiresDrain(
    bool submittedCommandWork, RenderTapeEventType event) noexcept {
  return submittedCommandWork &&
      (event == RenderTapeEventType::ResourceMutation ||
       event == RenderTapeEventType::ObjectDestroy);
}

FrameTapeReplayResult preflightRenderTapeIdentity(
    std::span<const std::byte> tape,
    std::span<const RenderTapeProviderBlob> blobs) noexcept {
  auto bounded = buildPlan(tape, blobs, nullptr);
  if (bounded.complete() ||
      bounded.status != FrameTapeReplayStatus::UnsupportedGrammar) {
    return bounded;
  }
  return render_tape_runtime::buildGeneralReplayPlan(tape, blobs, nullptr);
}

FrameTapeReplayResult preflightFrameTapeIdentity(
    std::span<const std::byte> tape,
    std::span<const RenderTapeProviderBlob> blobs) noexcept {
  auto result = preflightRenderTapeIdentity(tape, blobs);
  if (result.complete() && result.profile != kRenderTapeProfileFrame) {
    result.status = FrameTapeReplayStatus::UnsupportedGrammar;
  }
  return result;
}

PreparedRenderTape prepareRenderTapeIdentity(
    std::span<const std::byte> tape,
    std::span<const RenderTapeProviderBlob> blobs) noexcept {
  PreparedRenderTape prepared;
  try {
    prepared.impl_ = std::make_unique<PreparedRenderTape::Impl>();
    auto& impl = *prepared.impl_;
    const auto begin = std::chrono::steady_clock::now();

    impl.tape.assign(tape.begin(), tape.end());
    impl.blobStorage.reserve(blobs.size());
    impl.blobs.reserve(blobs.size());
    std::uint64_t blobBytes = 0u;
    for (const auto& blob : blobs) {
      impl.blobStorage.emplace_back(blob.bytes.begin(), blob.bytes.end());
      blobBytes += impl.blobStorage.back().size();
      impl.blobs.push_back(RenderTapeProviderBlob{
          .digest = blob.digest,
          .bytes = std::span<const std::byte>(impl.blobStorage.back()),
      });
    }

    impl.evidence.immutableInputOwned = true;
    impl.evidence.tapeBytes = impl.tape.size();
    impl.evidence.blobBytes = blobBytes;
    impl.evidence.blobCount = static_cast<std::uint32_t>(impl.blobs.size());
    impl.evidence.prepareCount = 1u;

    impl.preflight = buildPlan(impl.tape, impl.blobs, &impl.boundedPlan);
    if (impl.preflight.status == FrameTapeReplayStatus::UnsupportedGrammar) {
      impl.general = true;
      impl.preflight = render_tape_runtime::buildGeneralReplayPlan(
          impl.tape, impl.blobs, impl.generalPlan);
    }
    impl.evidence.preflightCompleted = true;
    impl.evidence.timing.prepareWallNanos = elapsedNanos(
        begin, std::chrono::steady_clock::now());
  } catch (...) {
    // Preparation is an admission boundary.  A failed allocation is an
    // invalid prepared input, never a partially usable plan.
    prepared.impl_.reset();
  }
  return prepared;
}

PreparedRenderTape prepareRenderTapeIdentityV4(
    std::span<const std::byte> tape, std::span<const RenderTapeProviderBlob> blobs,
    RenderTapePreparedV4PreparationOptions options) noexcept {
  const auto begin = std::chrono::steady_clock::now();
  PreparedRenderTape prepared;
  try {
    if (options.sourceBoundaryIdentity.empty() || options.sourceExecutionTopology.empty() ||
        !nonZeroDigest(options.expectedSourcePlanDigest)) return prepared;
    prepared = prepareRenderTapeIdentity(tape, blobs);
    if (!prepared.valid() || !prepared.impl_->general) {
      prepared.impl_.reset();
      return prepared;
    }
    auto& impl = *prepared.impl_;
    const auto v4Begin = std::chrono::steady_clock::now();
    auto v4 = std::make_unique<PreparedRenderTape::Impl::V4Preparation>();
    v4->publicationMode = options.publicationMode;
    v4->producerContract = options.producerContract;
    if (buildCaptureSourceReplayPlanV4(impl.tape, impl.generalPlan.catalogue,
            options.sourceBoundaryIdentity, options.sourceExecutionTopology, v4->sourcePlan) !=
            CaptureSourceReplayPlanV4Status::Valid ||
        !sameDigest(options.expectedSourcePlanDigest, v4->sourcePlan.digest())) {
      prepared.impl_.reset();
      return prepared;
    }
    const char* failure = "none";
    const bool normalRequested = !options.normalActionProfile.empty() ||
        !options.normalAttachmentHistory.empty() || nonZeroDigest(options.expectedNormalActionDigest) ||
        nonZeroDigest(options.expectedNormalHistoryDigest);
    if (normalRequested) {
      v4->normalHistory = std::make_unique<NormalHistorySetup>();
      if (!prepareNormalHistorySetup(v4->sourcePlan, impl.generalPlan, impl.tape,
              options, *v4->normalHistory)) {
        prepared.impl_.reset();
        return prepared;
      }
    }
    if (!RenderTapeExecutionV4PreparedReplayAccess::prepare(v4->execution,
            v4->sourcePlan, impl.generalPlan, impl.tape, options.publicationMode,
            options.producerContract, failure) ||
        buildRenderTapeInitialSetupV4(v4->sourcePlan, impl.generalPlan, impl.tape,
            v4->execution.snapshotPrefixEvents(), v4->initialSetup, {},
            v4->normalHistory ? std::span<const D9CWireObjectIdentity>(v4->normalHistory->producedParents)
                              : std::span<const D9CWireObjectIdentity>{}) !=
            RenderTapeInitialSetupV4Status::Valid) {
      prepared.impl_.reset();
      return prepared;
    }
    // The stationary inner owner is published only after every authority and
    // setup check. Moving the outer PIMPL never relocates these input bindings.
    impl.v4 = std::move(v4);
    impl.evidence.immutableV4PreparationOwned = true;
    impl.evidence.v4PreparationCount = 1u;
    const auto end = std::chrono::steady_clock::now();
    impl.evidence.timing.v4PreparationNanos = elapsedNanos(v4Begin, end);
    impl.evidence.timing.prepareWallNanos = elapsedNanos(begin, end);
  } catch (...) {
    // General preflight success is not V4 authority. No partial PIMPL survives
    // a failed V4 input check, allocation or immutable-template publication.
    prepared.impl_.reset();
  }
  return prepared;
}

FrameTapeReplayResult replayPreflightPlan(
    D9CDevice* device, const PreflightPlan& plan, FrameTapeReplayResult result,
    std::span<const RenderTapeProviderBlob> blobs,
    ProviderReplayPolicy policy = ProviderReplayPolicy::Historical,
    PreparedPhaseTiming* timing = nullptr,
    RenderTapeInitialSeedAuditEvidence* initialSeedAudit = nullptr) noexcept {
  if (!device) {
    result.status = FrameTapeReplayStatus::ObjectCreationFailed;
    return result;
  }

  std::vector<OwnedObject> objects;
  std::shared_ptr<OffscreenPresentOutput> output;
  D9CSurface* readbackTarget = nullptr;
  bool outputInstalled = false;
  auto replayUpper = device->dev().upperDevice();
  const bool priorExactAttachmentPreservation =
      replayUpper &&
      replayUpper->queue().renderTapeExactAttachmentPreservation();
  if (replayUpper) {
    replayUpper->queue().setRenderTapeExactAttachmentPreservation(true);
  }
  const auto cleanup = [&] {
    PreparedPhaseScope phase(timing, &PreparedPhaseTiming::cleanupNanos);
    if (auto upper = device->dev().upperDevice()) upper->flush();
    if (replayUpper) {
      replayUpper->queue().setRenderTapeExactAttachmentPreservation(
          priorExactAttachmentPreservation);
    }
    if (outputInstalled) {
      if (auto* swap = device->iface->GetSwapChain(0u)) {
        swap->coreSwapChain().restoreWindowPresenter();
        swap->Release();
      }
      outputInstalled = false;
    }
    if (readbackTarget) dxmt9c_surface_release(readbackTarget);
    for (auto& object : objects) {
      if (object.value) {
        render_tape_runtime::releaseOwnedObject(object);
        ++result.conservation.objectsReleased;
      }
    }
  };

  // Texture-derived surfaces borrow their parent storage. Create all owning
  // objects first, then materialize aliases so replay is independent of
  // ObjectDefine event order while preserving every identity exactly.
  for (unsigned creationPass = 0u; creationPass != 2u; ++creationPass) {
    for (const auto& definition : plan.definitions) {
      bool textureAlias = false;
      if (definition.fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE) {
        RenderTapeSurfaceDescriptorV2 surface{};
        textureAlias = load(definition.descriptor, 0u, surface) &&
                       surface.storage == static_cast<std::uint32_t>(
                           RenderTapeSurfaceStorage::TextureSubresource);
      }
      if ((creationPass == 1u) != textureAlias)
        continue;
      PreparedPhaseScope phase(timing,
                               &PreparedPhaseTiming::resourceSetupNanos);
      void* value = render_tape_runtime::createRenderTapeObject(
          device, definition.fixed, definition.descriptor, blobs, objects);
      if (!value) {
        result.status = FrameTapeReplayStatus::ObjectCreationFailed;
        cleanup();
        return result;
      }
      objects.push_back({.identity = definition.fixed.identity, .value = value});
      ++result.conservation.objectsCreated;
    }
  }

  const auto applyMutations = [&](std::span<const RenderTapeResourceMutationHeader>
                                      mutations) {
    for (const auto& mutation : mutations) {
      PreparedPhaseScope phase(timing,
                               &PreparedPhaseTiming::resourceSetupNanos);
      const auto* blob = findBlob(blobs, mutation.digest);
      void* object = render_tape_runtime::findExactObject(
          objects, mutation.identity);
      bool ok = blob && object;
      if (ok && mutation.identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER) {
        void* mapped = nullptr;
        RenderTapeBufferMutationReplayPlan replayPlan{};
        auto* buffer = static_cast<D9CBuffer*>(object);
        ok = renderTapeBufferMutationReplayPlan(mutation, replayPlan) &&
             dxmt9c_buffer_lock(
                 buffer, replayPlan.byteOffset, replayPlan.byteSize, &mapped,
                 replayPlan.lockFlags) == core::D3D_OK &&
             mapped;
        if (ok) {
          std::memcpy(mapped, blob->bytes.data(), blob->bytes.size());
          ok = dxmt9c_buffer_unlock(buffer) == core::D3D_OK;
        }
      } else if (ok &&
                 mutation.identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
        D9CSurfaceDesc levelDesc{};
        D9CLockedRect locked{};
        auto* texture = static_cast<D9CTexture*>(object);
        const auto definition = std::find_if(
            plan.definitions.begin(), plan.definitions.end(),
            [&](const auto& value) {
              return sameIdentity(value.fixed.identity, mutation.identity);
            });
        ok = definition != plan.definitions.end() &&
             renderTapeTextureSubresourceDescriptor(
                 definition->descriptor, mutation.subresource, levelDesc) &&
             dxmt9c_texture_lock_rect(texture, mutation.subresource, &locked,
                                      nullptr, 0u) == core::D3D_OK &&
             locked.bits;
        RenderTapeLinearLockLayout layout{};
        if (ok &&
            (renderTapeLinearLockLayout(levelDesc, locked.pitch, nullptr,
                                        layout) !=
                 RenderTapeLinearLayoutStatus::Accepted ||
             blob->bytes.size() != layout.tightBytes ||
             !writeRenderTapeLinearRows(blob->bytes, locked.bits, layout))) {
          (void)dxmt9c_texture_unlock_rect(texture, mutation.subresource);
          ok = false;
        } else if (ok) {
          ok = dxmt9c_texture_unlock_rect(texture, mutation.subresource) ==
               core::D3D_OK;
        }
      } else if (ok &&
                 mutation.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE) {
        const auto definition = std::find_if(
            plan.definitions.begin(), plan.definitions.end(),
            [&](const auto& value) {
              return sameIdentity(value.fixed.identity, mutation.identity);
            });
        RenderTapeSurfaceDescriptorV2 surface{};
        std::uint64_t pixelCount = 0u;
        std::uint64_t tightBytes = 0u;
        RenderTapeD24S8SnapshotLayout depthStencilLayout{};
        auto* wrapped = static_cast<D9CSurface*>(object);
        auto upper = device->dev().upperDevice();
        ok = definition != plan.definitions.end() &&
             load(definition->descriptor, 0u, surface) &&
             surface.storage == static_cast<std::uint32_t>(
                 RenderTapeSurfaceStorage::Standalone) &&
             mutation.subresource == 0u && mutation.byteOffset == 0u &&
             wrapped && wrapped->obj;
        const bool depthStencilSnapshot = ok &&
            surface.initialContentDisposition == static_cast<std::uint32_t>(
                RenderTapeInitialContentDisposition::CompleteDepthStencilFloat32UInt8V1) &&
            renderTapeDeriveD24S8SnapshotLayout(surface.surface, depthStencilLayout) &&
            blob->bytes.size() == depthStencilLayout.byteCount;
        ok = ok && (depthStencilSnapshot ||
             (checkedMul(surface.surface.width, surface.surface.height, pixelCount) &&
              checkedMul(pixelCount, 4u, tightBytes) && blob->bytes.size() == tightBytes));
        const bool depthSnapshot = ok &&
            surface.initialContentDisposition ==
                static_cast<std::uint32_t>(
                    RenderTapeInitialContentDisposition::
                        CompleteDepthFloat32V1) &&
            renderTapeSnapshotStandaloneD24X8Supported(surface.surface);
        const bool colorSnapshot = ok &&
            surface.initialContentDisposition ==
                static_cast<std::uint32_t>(
                    RenderTapeInitialContentDisposition::CompleteSeed) &&
            renderTapeArmColorSnapshotStandaloneSurfaceSupported(
                surface.surface);
        if (depthStencilSnapshot) {
          ok = seedStandaloneD24S8(device, wrapped, surface.surface, blob->bytes);
        } else if (depthSnapshot && upper) {
          core::CanonicalD24X8Depth canonical{
              .bytes = std::vector<core::u8>(blob->bytes.size()),
              .version = core::kCanonicalD24X8DepthVersion1,
              .width = surface.surface.width,
              .height = surface.surface.height,
              .pitch = surface.surface.width * 4u,
              .physicalFormat = static_cast<core::u32>(convert::toPixelFormat(
                  core::Format::D24X8, upper->limits())),
          };
          std::memcpy(canonical.bytes.data(), blob->bytes.data(),
                      blob->bytes.size());
          ok = upper->seedCanonicalD24X8Depth(wrapped->obj->handle(),
                                               canonical);
        } else if (colorSnapshot) {
          D9CLockedRect locked{};
          ok = dxmt9c_surface_lock_rect(wrapped, &locked, nullptr, 0u) ==
                   core::D3D_OK &&
               locked.bits;
          RenderTapeLinearLockLayout layout{};
          if (ok &&
              (renderTapeLinearLockLayout(surface.surface, locked.pitch,
                                          nullptr, layout) !=
                   RenderTapeLinearLayoutStatus::Accepted ||
               blob->bytes.size() != layout.tightBytes ||
               !writeRenderTapeLinearRows(blob->bytes, locked.bits, layout))) {
            (void)dxmt9c_surface_unlock_rect(wrapped);
            ok = false;
          } else if (ok) {
            ok = dxmt9c_surface_unlock_rect(wrapped) == core::D3D_OK;
          }
        } else {
          ok = false;
        }
      } else {
        ok = false;
      }
      if (!ok) {
        return false;
      }
    }
    return true;
  };

  if (!applyMutations(plan.initialMutations)) {
    result.status = FrameTapeReplayStatus::MutationFailed;
    cleanup();
    return result;
  }

  if (initialSeedAudit) {
    auditInitialSeedMutations(device, plan.definitions, plan.initialMutations,
                              objects, blobs, *initialSeedAudit);
  }

  // Seed all prior-boundary content before BootstrapState can bind it.
  std::vector<void*> resolved;
  for (const auto& chunk : plan.bootstrap) {
    PreparedPhaseScope phase(timing, &PreparedPhaseTiming::chunkReplayNanos);
    if (!resolveChunk(chunk, objects, resolved) ||
        replayPrevalidatedResolvedCommandChunk(
            device, chunk.bytes, chunk.envelope, resolved, policy) !=
            core::D3D_OK) {
      result.status = FrameTapeReplayStatus::BootstrapFailed;
      cleanup();
      return result;
    }
  }

  if (auto upper = device->dev().upperDevice(); upper && upper->pool()) {
    PreparedPhaseScope phase(timing,
                             &PreparedPhaseTiming::resourceSetupNanos);
    readbackTarget = dxmt9c_device_create_render_target(
        device, plan.outputDesc.width, plan.outputDesc.height,
        plan.outputDesc.format, 0u, 0u, 0u, nullptr);
    auto* record = readbackTarget
        ? upper->pool()->findSurface(readbackTarget->obj->handle().value)
        : nullptr;
    auto* swap = device->iface->GetSwapChain(0u);
    if (!record || !record->texture || !swap) {
      if (swap) swap->Release();
      result.status = FrameTapeReplayStatus::PresentOutputFailed;
      cleanup();
      return result;
    }
    output = std::make_shared<OffscreenPresentOutput>(
        WMT::Texture{record->texture.handle}, plan.outputDesc.width,
        plan.outputDesc.height);
    const bool installed = swap->coreSwapChain().installPresentOutput(output);
    swap->Release();
    if (!installed) {
      result.status = FrameTapeReplayStatus::PresentOutputFailed;
      cleanup();
      return result;
    }
    outputInstalled = true;
  }

  const auto readbackInterval = [&](std::uint32_t intervalIndex) {
    PreparedPhaseScope phase(timing,
                             &PreparedPhaseTiming::readbackDrainNanos);
    auto& evidence = result.intervals[intervalIndex].validity;
    const auto& interval = plan.intervals[intervalIndex];
    auto upper = device->dev().upperDevice();
    if (!upper || !readbackTarget) {
      return true;
    }
    upper->flush();
    core::ReadbackPixels pixels;
    if (!upper->readbackSurface(
            core::ReadbackDesc{.source = readbackTarget->obj->handle()}, pixels)) {
      return false;
    }
    evidence.outputReadback = true;
    constexpr std::uint32_t bytesPerPixel = 4u;
    std::uint64_t tightPitch = 0u;
    std::uint64_t tightBytes = 0u;
    std::uint64_t pitchedBytes = 0u;
    if (!checkedMul(plan.outputDesc.width, bytesPerPixel, tightPitch) ||
        !checkedMul(tightPitch, plan.outputDesc.height, tightBytes) ||
        !checkedMul(pixels.pitch, plan.outputDesc.height, pitchedBytes) ||
        pixels.pitch < tightPitch ||
        tightBytes > std::numeric_limits<std::size_t>::max() ||
        pixels.bytes.size() < pitchedBytes) {
      return false;
    }
    std::vector<std::byte> tight(static_cast<std::size_t>(tightBytes));
    for (std::uint32_t row = 0u; row < plan.outputDesc.height; ++row) {
      std::memcpy(tight.data() + static_cast<std::size_t>(row * tightPitch),
                  pixels.bytes.data() + static_cast<std::size_t>(row) * pixels.pitch,
                  static_cast<std::size_t>(tightPitch));
    }
    evidence.outputBytes = tight.size();
    evidence.outputDigest = RenderTapeCaptureSession::sha256(tight);
    evidence.expectedDigestCaptured =
        interval.expectedDigestValidity == RenderTapeDigestValidity::Sha256;
    if (evidence.expectedDigestCaptured) {
      evidence.expectedOutputDigest = interval.expectedDigest;
      evidence.expectedDigestMatched =
          sameDigest(evidence.outputDigest, interval.expectedDigest);
    }
    const auto pixelSize = std::min<std::size_t>(4u, tight.size());
    evidence.outputNonDegenerate = false;
    for (std::size_t index = pixelSize; index < tight.size(); ++index) {
      if (tight[index] != tight[index % pixelSize]) {
        evidence.outputNonDegenerate = true;
        break;
      }
    }
    result.outputPixels = std::move(tight);
    auto* source = static_cast<D9CSurface*>(
        render_tape_runtime::findExactObject(objects, plan.outputIdentity));
    std::vector<std::byte> sourcePixels;
    if (readbackTightSurface(device, source, plan.outputDesc.width,
                             plan.outputDesc.height, plan.outputDesc.format,
                             sourcePixels)) {
      evidence.sourceReadback = true;
      evidence.sourceBytes = sourcePixels.size();
      evidence.sourceDigest = RenderTapeCaptureSession::sha256(sourcePixels);
      result.sourcePixels = std::move(sourcePixels);
    }
    return true;
  };

  for (std::uint32_t intervalIndex = 0u;
       intervalIndex < plan.intervalCount; ++intervalIndex) {
    if (!applyMutations(plan.intervals[intervalIndex].mutationsBefore)) {
      result.status = FrameTapeReplayStatus::MutationFailed;
      cleanup();
      return result;
    }
    for (const auto& chunk : plan.intervals[intervalIndex].frame) {
      PreparedPhaseScope phase(timing, &PreparedPhaseTiming::chunkReplayNanos);
      if (!resolveChunk(chunk, objects, resolved) ||
          replayPrevalidatedResolvedCommandChunk(
              device, chunk.bytes, chunk.envelope, resolved, policy) !=
              core::D3D_OK) {
        result.status = FrameTapeReplayStatus::CommandReplayFailed;
        cleanup();
        return result;
      }
    }
    if (!readbackInterval(intervalIndex)) {
      result.status = FrameTapeReplayStatus::ReadbackFailed;
      cleanup();
      return result;
    }
    if (result.intervals[intervalIndex].validity.expectedDigestCaptured &&
        !result.intervals[intervalIndex].validity.expectedDigestMatched) {
      result.validity = result.intervals[intervalIndex].validity;
      result.validity.structurallyValid = true;
      result.validity.digestsValid = true;
      result.status = FrameTapeReplayStatus::OutputMismatch;
      cleanup();
      return result;
    }
  }
  if (output && output->scheduledCount() != plan.intervalCount) {
    result.status = FrameTapeReplayStatus::PresentOutputFailed;
  }
  if (plan.intervalCount != 0u) {
    result.validity = result.intervals[plan.intervalCount - 1u].validity;
    result.validity.structurallyValid = true;
    result.validity.digestsValid = true;
    result.validity.outputReadback = std::all_of(
        result.intervals.begin(), result.intervals.begin() + plan.intervalCount,
        [](const auto& interval) { return interval.validity.outputReadback; });
    result.validity.expectedDigestCaptured = std::all_of(
        result.intervals.begin(), result.intervals.begin() + plan.intervalCount,
        [](const auto& interval) {
          return interval.validity.expectedDigestCaptured;
        });
    result.validity.expectedDigestMatched = std::all_of(
        result.intervals.begin(), result.intervals.begin() + plan.intervalCount,
        [](const auto& interval) {
          return interval.validity.expectedDigestMatched;
        });
    result.validity.outputNonDegenerate = std::all_of(
        result.intervals.begin(), result.intervals.begin() + plan.intervalCount,
        [](const auto& interval) {
          return interval.validity.outputNonDegenerate;
        });
  }
  cleanup();
  return result;
}

FrameTapeReplayResult replayRenderTapeIdentity(
    D9CDevice* device, std::span<const std::byte> tape,
    std::span<const RenderTapeProviderBlob> blobs) noexcept {
  PreflightPlan plan;
  auto result = buildPlan(tape, blobs, &plan);
  if (result.complete()) {
    return replayPreflightPlan(device, plan, result, blobs);
  }
  if (result.status != FrameTapeReplayStatus::UnsupportedGrammar) return result;
  GeneralPreflightPlan generalPlan;
  auto generalResult = render_tape_runtime::buildGeneralReplayPlan(
      tape, blobs, generalPlan);
  if (!generalResult.complete()) return generalResult;
  replayGeneralPlan(device, generalPlan, generalResult, blobs,
                    ProviderReplayPolicy::Historical);
  return generalResult;
}

FrameTapeReplayResult replayPreparedRenderTapeIdentity(
    D9CDevice* device, const PreparedRenderTape& prepared,
    RenderTapePreparedExecutionOptions options) noexcept {
  if (!prepared.impl_) {
    FrameTapeReplayResult result;
    result.preparedObserverV4.requested = options.observerV4 != nullptr;
    if (options.observerV4)
      result.preparedObserverV4.failure = "v4-observer-preflight-incomplete";
    if (options.sourceBoundaryMode ==
        RenderTapeSourceBoundaryMode::CaptureSourceV4ExactDiagnostic) {
      result.exactDiagnosticV4.execution.requested = true;
      result.exactDiagnosticV4.execution.publicationMode = options.publicationModeV4;
      result.exactDiagnosticV4.failure = "v4-provider-preflight-incomplete";
    }
    return result;
  }
  const auto& impl = *prepared.impl_;
  if (impl.v4) {
    FrameTapeReplayResult result;
    result.status = FrameTapeReplayStatus::UnsupportedGrammar;
    result.exactDiagnosticV4.execution.requested = true;
    result.exactDiagnosticV4.failure = "v4-prepared-owner-requires-v4-entrypoint";
    attachPreparedEvidence(result, impl.evidence);
    return result;
  }
  auto result = impl.preflight;
  auto evidence = impl.evidence;
  evidence.requestedRoute = options.route;
  evidence.reset = options.reset;
  evidence.planReused = impl.preflight.complete();
  result.initialSeedAudit.requested = options.auditInitialSeeds;
  result.freshSeedDrainAudit.requested = options.auditFreshSeedDrains;
  result.texturePrestageAudit.requested = options.auditTexturePrestage;
  result.sourceBoundaryAudit.requested = options.auditSourceBoundaries;
  result.textureInputAudit.requested = options.auditTextureInput != nullptr;
  result.preparedObserverV4.requested = options.observerV4 != nullptr;
  if (options.observerV4)
    result.preparedObserverV4.failure = "v4-observer-not-admitted";
  const bool sourcePolicyV3Requested = options.sourceBoundaryMode ==
      RenderTapeSourceBoundaryMode::CaptureSourceV3;
  const bool exactDiagnosticV4Requested = options.sourceBoundaryMode ==
      RenderTapeSourceBoundaryMode::CaptureSourceV4ExactDiagnostic;
  const bool sourcePolicyRequested = options.sourceBoundaryMode ==
      RenderTapeSourceBoundaryMode::CaptureSource;
  result.sourceReplay.requested = sourcePolicyRequested;
  result.sourceReplayV3.requested = sourcePolicyV3Requested;
  result.exactDiagnosticV4.execution.requested = exactDiagnosticV4Requested;
  if (exactDiagnosticV4Requested)
    result.exactDiagnosticV4.execution.publicationMode = options.publicationModeV4;
  if (!impl.preflight.complete()) {
    if (exactDiagnosticV4Requested)
      result.exactDiagnosticV4.failure = "v4-provider-preflight-incomplete";
    attachPreparedEvidence(result, evidence);
    return result;
  }
  const unsigned diagnosticCount =
      static_cast<unsigned>(options.auditInitialSeeds) +
      static_cast<unsigned>(options.auditFreshSeedDrains) +
      static_cast<unsigned>(options.auditTexturePrestage) +
      static_cast<unsigned>(options.auditSourceBoundaries);
  if ((options.sourceBoundaryMode != RenderTapeSourceBoundaryMode::Chunk &&
       !sourcePolicyRequested && !sourcePolicyV3Requested &&
       !exactDiagnosticV4Requested) || diagnosticCount > 1u ||
      ((sourcePolicyRequested || sourcePolicyV3Requested || exactDiagnosticV4Requested) &&
       (diagnosticCount != 0u || !impl.general ||
        options.reset != RenderTapePreparedReset::FreshDevice)) ||
      (sourcePolicyV3Requested && options.route != RenderTapePreparedRoute::Compatibility) ||
      (exactDiagnosticV4Requested &&
       options.route != RenderTapePreparedRoute::Compatibility &&
       options.route != RenderTapePreparedRoute::ProductionSlot) ||
      (!exactDiagnosticV4Requested && !options.sourceExecutionTopology.empty()) ||
      (!exactDiagnosticV4Requested && options.observerV4) ||
      (!exactDiagnosticV4Requested && options.producerContractV4 !=
          RenderTapeExecutionV4ProducerContract::None) ||
      (!exactDiagnosticV4Requested && options.publicationModeV4 !=
          RenderTapeExecutionV4PublicationMode::ExactSourceDiagnostic) ||
      (options.publicationModeV4 != RenderTapeExecutionV4PublicationMode::ExactSourceDiagnostic &&
       options.publicationModeV4 != RenderTapeExecutionV4PublicationMode::SemanticMapping) ||
      (options.auditTexturePrestage && !impl.general) ||
      (!options.auditSourceBoundaries && !options.auditBatchCuts.empty()) ||
      (options.auditTextureInput &&
       (!sourcePolicyRequested || !options.hasExpectedSourcePlanDigest ||
        diagnosticCount != 0u || options.reset != RenderTapePreparedReset::FreshDevice)) ||
      (!options.auditSourceBoundaries && !sourcePolicyRequested && !sourcePolicyV3Requested &&
       !exactDiagnosticV4Requested &&
       !options.sourceBoundaryIdentity.empty()) ||
      (options.auditSourceBoundaries &&
       (!impl.general || options.route != RenderTapePreparedRoute::Compatibility ||
        options.reset != RenderTapePreparedReset::FreshDevice))) {
    result.status = FrameTapeReplayStatus::UnsupportedGrammar;
    if (exactDiagnosticV4Requested)
      result.exactDiagnosticV4.failure = "v4-provider-options-unsupported";
    if (options.observerV4)
      result.preparedObserverV4.failure = "v4-observer-options-unsupported";
    attachPreparedEvidence(result, evidence);
    return result;
  }
  CaptureSourceReplayPlan sourcePolicy;
  CaptureSourceReplayPlanV3 sourcePolicyV3;
  CaptureSourceReplayPlanV4 exactDiagnosticV4;
  // The stack executor and its evidence address stay fixed through General
  // replay. Its eventual destruction is outside both admission and replay
  // timers; those scopes no longer include the former temporary owners' frees.
  RenderTapeExecutionV4 admittedExecutionV4;
  RenderTapeInitialSetupV4Plan initialSetupV4;
  PreparedPhaseTiming phases{};
  if (exactDiagnosticV4Requested) {
    PreparedPhaseScope phase(&phases,
                             &PreparedPhaseTiming::v4PlanAdmissionNanos);
    // Authenticate the complete immutable pair, catalogue and exact derived
    // General plan before any reset, resource setup or queue effect.
    if (options.sourceBoundaryIdentity.empty() || options.sourceExecutionTopology.empty()) {
      result.status = FrameTapeReplayStatus::InvalidTape;
      result.exactDiagnosticV4.failure = "v4-required-pair-missing";
      attachPreparedEvidence(result, evidence);
      return result;
    }
    if (!options.hasExpectedSourcePlanDigest ||
        !nonZeroDigest(options.expectedSourcePlanDigest)) {
      result.status = FrameTapeReplayStatus::InvalidTape;
      result.exactDiagnosticV4.failure = "v4-required-plan-digest-missing";
      attachPreparedEvidence(result, evidence);
      return result;
    }
    const auto status = buildCaptureSourceReplayPlanV4(impl.tape,
        impl.generalPlan.catalogue, options.sourceBoundaryIdentity,
        options.sourceExecutionTopology, exactDiagnosticV4);
    if (status != CaptureSourceReplayPlanV4Status::Valid) {
      result.status = FrameTapeReplayStatus::InvalidTape;
      result.exactDiagnosticV4.failure = captureSourceReplayPlanV4StatusName(status);
      attachPreparedEvidence(result, evidence);
      return result;
    }
    if (!sameDigest(options.expectedSourcePlanDigest, exactDiagnosticV4.digest())) {
      result.status = FrameTapeReplayStatus::InvalidTape;
      result.exactDiagnosticV4.failure = "v4-plan-digest-mismatch";
      attachPreparedEvidence(result, evidence);
      return result;
    }
    if (!admittedExecutionV4.admit(exactDiagnosticV4, impl.generalPlan, impl.tape,
            result.exactDiagnosticV4.execution,
            options.publicationModeV4,
            options.producerContractV4)) {
      result.status = FrameTapeReplayStatus::UnsupportedGrammar;
      result.exactDiagnosticV4.failure = admittedExecutionV4.failure();
      attachPreparedEvidence(result, evidence);
      return result;
    }
    const auto setupStatus = buildRenderTapeInitialSetupV4(exactDiagnosticV4,
        impl.generalPlan, impl.tape, admittedExecutionV4.snapshotPrefixEvents(), initialSetupV4);
    if (setupStatus != RenderTapeInitialSetupV4Status::Valid) {
      result.status = setupStatus == RenderTapeInitialSetupV4Status::AllocationFailed
          ? FrameTapeReplayStatus::ObjectCreationFailed
          : FrameTapeReplayStatus::UnsupportedGrammar;
      result.exactDiagnosticV4.failure = "v4-initial-setup-admission-failed";
      attachPreparedEvidence(result, evidence);
      return result;
    }
    result.exactDiagnosticV4.initialSetupAdmitted = true;
    result.exactDiagnosticV4.initialSetupOwners = initialSetupV4.entries().size();
    result.exactDiagnosticV4.initialSetupSeeds = initialSetupV4.seeds().size();
    result.exactDiagnosticV4.initialSetupBytes = initialSetupV4.totalSeedBytes();
    result.exactDiagnosticV4.initialSetupAllocationOnlyOwners =
        static_cast<std::size_t>(std::count_if(initialSetupV4.entries().begin(),
            initialSetupV4.entries().end(), [](const auto& entry) {
              return entry.mode == RenderTapeInitialSetupV4Mode::AllocationOnly;
            }));
    result.exactDiagnosticV4.initialSetupAllocationBytes =
        initialSetupV4.totalAllocationBytes();
  }
  if (sourcePolicyV3Requested) {
    if (buildCaptureSourceReplayPlanV3(impl.tape, impl.generalPlan.catalogue,
            options.sourceBoundaryIdentity, sourcePolicyV3) !=
            CaptureSourceReplayPlanV3Status::Valid ||
        ((options.hasExpectedSourcePlanDigest || nonZeroDigest(options.expectedSourcePlanDigest)) &&
         !sameDigest(options.expectedSourcePlanDigest, sourcePolicyV3.digest()))) {
      result.status = FrameTapeReplayStatus::InvalidTape;
      attachPreparedEvidence(result, evidence);
      return result;
    }
    RenderTapeExecutionV3 admission;
    if (!admission.admit(sourcePolicyV3, impl.generalPlan, impl.tape, result.sourceReplayV3)) {
      result.status = FrameTapeReplayStatus::UnsupportedGrammar;
      attachPreparedEvidence(result, evidence);
      return result;
    }
  }
  if (sourcePolicyRequested) {
    if (buildCaptureSourceReplayPlan(impl.tape, impl.generalPlan,
                                     options.sourceBoundaryIdentity,
                                     sourcePolicy) != CaptureSourceReplayPlanStatus::Valid) {
      result.status = FrameTapeReplayStatus::InvalidTape;
      attachPreparedEvidence(result, evidence);
      return result;
    }
    if (sourcePolicy.rows().empty() ||
        sourcePolicy.rows().size() > kProviderSourceReplayMaxRecords) {
      result.status = FrameTapeReplayStatus::InvalidTape;
      attachPreparedEvidence(result, evidence);
      return result;
    }
    const bool expectedDigestProvided =
        options.hasExpectedSourcePlanDigest ||
        nonZeroDigest(options.expectedSourcePlanDigest);
    if (expectedDigestProvided &&
        (!nonZeroDigest(options.expectedSourcePlanDigest) ||
         !sameDigest(options.expectedSourcePlanDigest, sourcePolicy.digest()))) {
      result.status = FrameTapeReplayStatus::InvalidTape;
      attachPreparedEvidence(result, evidence);
      return result;
    }
    result.sourceReplay.validated = true;
    result.sourceReplay.planDigest = sourcePolicy.digest();
    result.sourceReplay.sourceCount = sourcePolicy.rows().size();
    try {
      // Settlement callbacks run after queue publication and are noexcept.
      // Reserve every evidence slot before the first replay effect so a
      // diagnostic allocation failure or publication fan-out becomes a
      // deterministic fail-closed result instead of a partial artifact.
      result.sourceReplay.sources.resize(sourcePolicy.rows().size());
      result.sourceReplay.publications.reserve(kMaxSourceReplayPublications);
      for (std::size_t index = 0u; index < sourcePolicy.rows().size(); ++index) {
        const auto& source = sourcePolicy.rows()[index];
        result.sourceReplay.sources[index] = {
            .eventOrdinal = source.eventOrdinal,
            .sourceOrdinal = source.sourceOrdinal,
            .capturedSeqId = source.capturedSeqId,
            .commandChunkIndex = source.commandChunkIndex,
            .firstRecord = source.firstRecord,
            .recordCount = source.recordCount,
        };
      }
    } catch (...) {
      result.status = FrameTapeReplayStatus::ObjectCreationFailed;
      attachPreparedEvidence(result, evidence);
      return result;
    }
  }
  SourceBoundaryReplayPlan sourceBoundaries;
  RenderTapeTextureInputAuditEvidence textureInputEvidence;
  if (options.auditTextureInput &&
      !validateTextureInputAudit(*options.auditTextureInput, impl.generalPlan,
                                 sourcePolicy, textureInputEvidence)) {
    result.status = FrameTapeReplayStatus::InvalidTape;
    result.textureInputAudit = textureInputEvidence;
    attachPreparedEvidence(result, evidence);
    return result;
  }
  RenderTapeSourceBoundaryEvidence sourceBoundaryAudit;
  sourceBoundaryAudit.requested = options.auditSourceBoundaries;
  if (options.auditSourceBoundaries &&
      !buildSourceBoundaryReplayPlan(impl.tape, impl.generalPlan,
                                     options.sourceBoundaryIdentity,
                                     options.auditBatchCuts,
                                     sourceBoundaries, sourceBoundaryAudit)) {
    result.status = FrameTapeReplayStatus::InvalidTape;
    result.sourceBoundaryAudit = sourceBoundaryAudit;
    attachPreparedEvidence(result, evidence);
    return result;
  }
  if (options.reset == RenderTapePreparedReset::WarmDevice) {
    if (options.auditInitialSeeds || options.auditFreshSeedDrains ||
        options.auditTexturePrestage) {
      result.status = FrameTapeReplayStatus::UnsupportedGrammar;
      attachPreparedEvidence(result, evidence);
      return result;
    }
    const auto resetBegin = std::chrono::steady_clock::now();
    const auto reset = resetPreparedDevice(device, result.requirements);
    evidence.timing.resetWallNanos = elapsedNanos(
        resetBegin, std::chrono::steady_clock::now());
    if (reset != WarmResetAttempt::Succeeded) {
      result.status = reset == WarmResetAttempt::Unsupported
          ? FrameTapeReplayStatus::WarmResetUnsupported
          : FrameTapeReplayStatus::WarmResetFailed;
      attachPreparedEvidence(result, evidence);
      return result;
    }
    evidence.resetPerformed = true;
    evidence.resetVerified = true;
  }
  const auto begin = std::chrono::steady_clock::now();
  const auto policy = options.route == RenderTapePreparedRoute::ProductionSlot
      ? ProviderReplayPolicy::ProductionSlot
      : ProviderReplayPolicy::Compatibility;
  // The bounded replay route still returns a separate result. Keep its audit
  // sinks outside that assignment; General replay retains this result owner.
  RenderTapeInitialSeedAuditEvidence seedAudit{};
  seedAudit.requested = options.auditInitialSeeds;
  RenderTapeFreshSeedDrainEvidence freshSeedDrainAudit{};
  freshSeedDrainAudit.requested = options.auditFreshSeedDrains;
  RenderTapeTexturePrestageEvidence texturePrestageAudit{};
  texturePrestageAudit.requested = options.auditTexturePrestage;
  if (impl.general) {
    replayGeneralPlan(device, impl.generalPlan, result, impl.blobs,
                               policy, &phases,
                               options.auditInitialSeeds
                                   ? &seedAudit
                                   : nullptr,
                               options.auditFreshSeedDrains
                                   ? &freshSeedDrainAudit
                                   : nullptr,
                               options.auditTexturePrestage
                                   ? &texturePrestageAudit
                                   : nullptr,
                               options.auditSourceBoundaries
                                   ? &sourceBoundaries
                                   : nullptr,
                               options.auditSourceBoundaries
                                   ? &sourceBoundaryAudit
                                   : nullptr,
                               sourcePolicyRequested ? &sourcePolicy : nullptr,
                               options.auditTextureInput,
                               options.auditTextureInput ? &textureInputEvidence : nullptr,
                               sourcePolicyV3Requested ? &sourcePolicyV3 : nullptr,
                               sourcePolicyV3Requested ? std::span<const std::byte>(impl.tape)
                                   : std::span<const std::byte>{},
                               exactDiagnosticV4Requested ? &exactDiagnosticV4 : nullptr,
                               exactDiagnosticV4Requested ? std::span<const std::byte>(impl.tape)
                                   : std::span<const std::byte>{},
                               exactDiagnosticV4Requested ? &initialSetupV4 : nullptr,
                               options.producerContractV4, options.publicationModeV4,
                               options.observerV4,
                               exactDiagnosticV4Requested ? &admittedExecutionV4 : nullptr);
  } else {
    result = replayPreflightPlan(device, impl.boundedPlan, result, impl.blobs,
                                 policy, &phases,
                                 options.auditInitialSeeds
                                     ? &seedAudit
                                     : nullptr);
  }
  result.initialSeedAudit = std::move(seedAudit);
  result.freshSeedDrainAudit = std::move(freshSeedDrainAudit);
  result.texturePrestageAudit = std::move(texturePrestageAudit);
  result.sourceBoundaryAudit = sourceBoundaryAudit;
  result.textureInputAudit = textureInputEvidence;
  if (exactDiagnosticV4Requested && !result.exactDiagnosticV4.execution.complete &&
      std::strcmp(result.exactDiagnosticV4.failure, "none") == 0)
    result.exactDiagnosticV4.failure = frameTapeReplayStatusName(result.status);
  evidence.timing.resourceSetupNanos = phases.resourceSetupNanos;
  evidence.timing.chunkReplayNanos = phases.chunkReplayNanos;
  evidence.timing.readbackDrainNanos = phases.readbackDrainNanos;
  evidence.timing.cleanupNanos = phases.cleanupNanos;
  evidence.timing.v4PlanAdmissionNanos = phases.v4PlanAdmissionNanos;
  evidence.timing.v4ReplayAdmissionNanos = phases.v4ReplayAdmissionNanos;
  evidence.timing.v4PreArmCheckpointNanos = phases.v4PreArmCheckpointNanos;
  evidence.timing.v4FinishNanos = phases.v4FinishNanos;
  evidence.timing.replayWallNanos = elapsedNanos(
      begin, std::chrono::steady_clock::now());
  attachPreparedEvidence(result, evidence);
  return result;
}

FrameTapeReplayResult replayPreparedRenderTapeIdentityV4(
    D9CDevice* device, const PreparedRenderTape& prepared,
    RenderTapePreparedRoute route, RenderTapePreparedObserverV4* observer) noexcept {
  FrameTapeReplayResult result;
  result.exactDiagnosticV4.execution.requested = true;
  result.preparedObserverV4.requested = observer != nullptr;
  if (!prepared.impl_ || !prepared.impl_->v4 || !prepared.valid() ||
      !prepared.impl_->v4->execution.valid()) {
    result.exactDiagnosticV4.failure = "v4-immutable-preparation-incomplete";
    if (observer) result.preparedObserverV4.failure = "v4-observer-preflight-incomplete";
    return result;
  }
  const auto& impl = *prepared.impl_;
  const auto& v4 = *impl.v4;
  try {
    // Copy result-owned query/output vectors before any local initialization
    // or runtime effect. A copy allocation failure claims no prepared reuse.
    result = impl.preflight;
  } catch (...) {
    FrameTapeReplayResult failed;
    failed.status = FrameTapeReplayStatus::ObjectCreationFailed;
    failed.exactDiagnosticV4.execution.requested = true;
    failed.exactDiagnosticV4.execution.publicationMode = v4.publicationMode;
    failed.exactDiagnosticV4.execution.producerContract = v4.producerContract;
    failed.exactDiagnosticV4.failure = "v4-replay-result-allocation";
    failed.preparedObserverV4.requested = observer != nullptr;
    if (observer) failed.preparedObserverV4.failure = "v4-observer-preflight-incomplete";
    return failed;
  }
  result.exactDiagnosticV4.execution.requested = true;
  result.exactDiagnosticV4.execution.publicationMode = v4.publicationMode;
  result.exactDiagnosticV4.execution.producerContract = v4.producerContract;
  result.preparedObserverV4.requested = observer != nullptr;
  if (observer) result.preparedObserverV4.failure = "v4-observer-not-admitted";
  auto evidence = impl.evidence;
  evidence.requestedRoute = route;
  evidence.reset = RenderTapePreparedReset::FreshDevice;
  evidence.planReused = true;
  if (route != RenderTapePreparedRoute::Compatibility &&
      route != RenderTapePreparedRoute::ProductionSlot) {
    result.status = FrameTapeReplayStatus::UnsupportedGrammar;
    result.exactDiagnosticV4.failure = "v4-provider-options-unsupported";
    if (observer) result.preparedObserverV4.failure = "v4-observer-options-unsupported";
    attachPreparedEvidence(result, evidence);
    return result;
  }
  RenderTapeExecutionV4 execution;
  PreparedPhaseTiming phases{};
  {
    // This V2 scope initializes one fresh local executor and copies setup
    // accounting. All input parsing, hashes and setup planning ran once above.
    PreparedPhaseScope phase(&phases, &PreparedPhaseTiming::v4PlanAdmissionNanos);
    if (!RenderTapeExecutionV4PreparedReplayAccess::initialize(execution,
            v4.execution, v4.sourcePlan, impl.generalPlan, impl.tape,
            result.exactDiagnosticV4.execution, v4.publicationMode, v4.producerContract)) {
      result.status = std::strcmp(execution.failure(), "v4-admission-allocation") == 0
          ? FrameTapeReplayStatus::ObjectCreationFailed : FrameTapeReplayStatus::InvalidTape;
      result.exactDiagnosticV4.failure = execution.failure();
      attachPreparedEvidence(result, evidence);
      return result;
    }
    evidence.reusedImmutablePreparation = true;
    result.exactDiagnosticV4.initialSetupAdmitted = true;
    result.exactDiagnosticV4.initialSetupOwners = v4.initialSetup.entries().size();
    result.exactDiagnosticV4.initialSetupSeeds = v4.initialSetup.seeds().size();
    result.exactDiagnosticV4.initialSetupBytes = v4.initialSetup.totalSeedBytes();
    result.exactDiagnosticV4.initialSetupAllocationOnlyOwners = static_cast<std::size_t>(
        std::count_if(v4.initialSetup.entries().begin(), v4.initialSetup.entries().end(),
            [](const auto& entry) { return entry.mode == RenderTapeInitialSetupV4Mode::AllocationOnly; }));
    result.exactDiagnosticV4.initialSetupAllocationBytes = v4.initialSetup.totalAllocationBytes();
    result.normalDiagnostic.requested = bool(v4.normalHistory);
  }
  const auto begin = std::chrono::steady_clock::now();
  const auto policy = route == RenderTapePreparedRoute::ProductionSlot
      ? ProviderReplayPolicy::ProductionSlot : ProviderReplayPolicy::Compatibility;
  replayGeneralPlan(device, impl.generalPlan, result, impl.blobs, policy, &phases,
      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
      nullptr, {}, &v4.sourcePlan, impl.tape, &v4.initialSetup,
      v4.producerContract, v4.publicationMode, observer, &execution, v4.normalHistory.get());
  if (!result.exactDiagnosticV4.execution.complete &&
      std::strcmp(result.exactDiagnosticV4.failure, "none") == 0)
    result.exactDiagnosticV4.failure = frameTapeReplayStatusName(result.status);
  evidence.timing.resourceSetupNanos = phases.resourceSetupNanos;
  evidence.timing.chunkReplayNanos = phases.chunkReplayNanos;
  evidence.timing.readbackDrainNanos = phases.readbackDrainNanos;
  evidence.timing.cleanupNanos = phases.cleanupNanos;
  evidence.timing.v4PlanAdmissionNanos = phases.v4PlanAdmissionNanos;
  evidence.timing.v4ReplayAdmissionNanos = phases.v4ReplayAdmissionNanos;
  evidence.timing.v4PreArmCheckpointNanos = phases.v4PreArmCheckpointNanos;
  evidence.timing.v4FinishNanos = phases.v4FinishNanos;
  evidence.timing.replayWallNanos = elapsedNanos(begin, std::chrono::steady_clock::now());
  attachPreparedEvidence(result, evidence);
  return result;
}

FrameTapeReplayResult replayFrameTapeIdentity(
    D9CDevice* device, std::span<const std::byte> tape,
    std::span<const RenderTapeProviderBlob> blobs) noexcept {
  const auto preflight = preflightFrameTapeIdentity(tape, blobs);
  if (!preflight.complete()) return preflight;
  return replayRenderTapeIdentity(device, tape, blobs);
}

FrameTapeBootstrapOutputDisposition classifyFrameTapeBootstrapOutput(
    std::span<const std::byte> bytes, const CommandChunkEnvelope& envelope,
    const D9CWireObjectIdentity& output,
    std::uint32_t baselineProfileVersion) noexcept {
  ImportedChunkView imported;
  if (!importPrevalidatedCommandChunk(bytes, envelope, imported) ||
      imported.records.empty()) {
    return FrameTapeBootstrapOutputDisposition::Malformed;
  }

  auto disposition = FrameTapeBootstrapOutputDisposition::ImplicitDefault;
  for (std::size_t recordIndex = 0u; recordIndex < imported.records.size(); ++recordIndex) {
    const auto record = imported.record(recordIndex);
    if (record.header.type != D9C_COMMAND_RECORD_APPLY_STATE ||
        !renderTapeBootstrapRecordShapeValid(baselineProfileVersion, recordIndex,
                                             record.drawHeader.flags,
                                             record.sections.size())) {
      return FrameTapeBootstrapOutputDisposition::Malformed;
    }
    if ((record.drawHeader.flags & D9C_COMMAND_CHUNK_DRAW_FLAG_FULL_SNAPSHOT) != 0u) {
      disposition = FrameTapeBootstrapOutputDisposition::ImplicitDefault;
    }
    bool sawSlotZero = false;
    for (std::size_t index = 0u; index < record.sections.size(); ++index) {
      const auto section = record.section(index);
      if (section.descriptor.count == 0u || section.descriptor.byteSize == 0u)
        return FrameTapeBootstrapOutputDisposition::Malformed;
      if (section.descriptor.kind != D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET) continue;
      for (std::uint32_t bindingIndex = 0u;
           bindingIndex < section.descriptor.count; ++bindingIndex) {
        D9CCommandChunkWireRenderTargetBinding binding{};
        if (!load(section.payload, bindingIndex * sizeof(binding), binding)) {
          return FrameTapeBootstrapOutputDisposition::Malformed;
        }
        if (binding.slot != 0u) {
          return FrameTapeBootstrapOutputDisposition::SlotOutOfRange;
        }
        if (sawSlotZero) {
          return FrameTapeBootstrapOutputDisposition::Ambiguous;
        }
        sawSlotZero = true;
        if (binding.valid != 1u ||
            binding.handleIndex == D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX) {
          disposition = FrameTapeBootstrapOutputDisposition::ExplicitNull;
          continue;
        }
        if (binding.handleIndex >= imported.handles.size()) {
          return FrameTapeBootstrapOutputDisposition::Malformed;
        }
        const auto& handle = imported.handles[binding.handleIndex];
        disposition = sameIdentity(output, D9CWireObjectIdentity{
            .kind = handle.kind, .generation = handle.generation,
            .objectId = handle.objectId})
            ? FrameTapeBootstrapOutputDisposition::ExplicitExact
            : FrameTapeBootstrapOutputDisposition::WrongIdentity;
      }
    }
  }
  return disposition;
}

bool applyRenderTapePixelOracleEnvelope(
    FrameTapeReplayResult& result,
    std::span<const std::byte> expectedPixels) noexcept {
  if (result.status != FrameTapeReplayStatus::OutputMismatch ||
      result.profile != kRenderTapeProfileFrame ||
      result.intervalCount != 1u || !result.validity.structurallyValid ||
      !result.validity.digestsValid || !result.validity.outputReadback ||
      !result.validity.expectedDigestCaptured ||
      result.validity.expectedDigestMatched || expectedPixels.empty() ||
      result.coverage.commandChunks == 0u ||
      result.coverage.commandRecords == 0u ||
      result.coverage.presentRecords != 1u ||
      result.coverage.presentSourceMappings != 1u ||
      result.coverage.presentOutputs != 1u ||
      result.conservation.inputBlobs !=
          result.conservation.referencedBlobs ||
      result.conservation.objectsCreated !=
          result.conservation.objectsReleased ||
      result.conservation.presentOrdinal == 0u ||
      result.conservation.completionOrdinal == 0u ||
      expectedPixels.size() != result.outputPixels.size() ||
      expectedPixels.size() != result.validity.outputBytes ||
      !sameDigest(RenderTapeCaptureSession::sha256(expectedPixels),
                  result.validity.expectedOutputDigest)) {
    return false;
  }

  std::uint64_t pixelCount = 0u;
  std::uint64_t expectedBytes = 0u;
  if (!checkedMul(result.requirements.outputWidth,
                  result.requirements.outputHeight, pixelCount) ||
      !checkedMul(pixelCount, 4u, expectedBytes) ||
      expectedBytes != expectedPixels.size()) {
    return false;
  }

  auto& validity = result.validity;
  validity.expectedPixelsCompared = true;
  for (std::uint64_t pixel = 0u; pixel < pixelCount; ++pixel) {
    const auto offset = static_cast<std::size_t>(pixel * 4u);
    bool differs = false;
    for (std::size_t channel = 0u; channel < 3u; ++channel) {
      const auto expected = std::to_integer<std::uint32_t>(
          expectedPixels[offset + channel]);
      const auto actual = std::to_integer<std::uint32_t>(
          result.outputPixels[offset + channel]);
      const auto delta = expected > actual ? expected - actual
                                           : actual - expected;
      differs = differs || delta != 0u;
      validity.totalRgbDelta += delta;
      validity.maxRgbDelta = std::max(validity.maxRgbDelta, delta);
    }
    if (expectedPixels[offset + 3u] != result.outputPixels[offset + 3u]) {
      differs = true;
      ++validity.differingAlphaPixels;
    }
    if (differs) {
      ++validity.differingPixels;
    }
  }

  // Independent executions may land on adjacent 8-bit quantization values at
  // a tiny number of filtered/rasterized edge pixels. Keep the envelope much
  // narrower than a visible or structural rendering change: at most 1/8192
  // pixels, RGB delta <= 3, no alpha delta, and average aggregate RGB delta
  // <= 2 across the entire differing-pixel allowance.
  const auto allowedPixels = std::min<std::uint64_t>(
      96u, std::max<std::uint64_t>(1u, pixelCount / 8192u));
  validity.allowedDifferingPixels = allowedPixels;
  validity.pixelEnvelopeMatched =
      validity.differingPixels <= allowedPixels &&
      validity.maxRgbDelta <= 3u && validity.differingAlphaPixels == 0u &&
      validity.totalRgbDelta <= allowedPixels * 2u;
  result.intervals[0].validity = validity;
  if (!validity.pixelEnvelopeMatched) {
    return false;
  }
  result.status = FrameTapeReplayStatus::Complete;
  result.failedEventIndex = 0xffffffffu;
  return true;
}

bool renderTapeSourceOracleMatchesOutputEvidence(
    std::span<const std::byte> expectedSource,
    std::span<const std::byte> actualSource,
    std::span<const std::byte> expectedOutput,
    std::span<const std::byte> actualOutput,
    bool outputPixelEnvelopeMatched) noexcept {
  const auto sameBytes = [](std::span<const std::byte> left,
                            std::span<const std::byte> right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin());
  };
  if (sameBytes(expectedSource, actualSource)) return true;
  return outputPixelEnvelopeMatched && !expectedOutput.empty() &&
         sameBytes(expectedSource, expectedOutput) &&
         sameBytes(actualSource, actualOutput);
}

const char* frameTapeReplayStatusName(FrameTapeReplayStatus status) noexcept {
  switch (status) {
  case FrameTapeReplayStatus::Complete: return "complete";
  case FrameTapeReplayStatus::InvalidTape: return "invalid-tape";
  case FrameTapeReplayStatus::InvalidBlobCatalogue: return "invalid-blob-catalogue";
  case FrameTapeReplayStatus::UnsupportedGrammar: return "unsupported-grammar";
  case FrameTapeReplayStatus::ObjectCreationFailed: return "object-creation-failed";
  case FrameTapeReplayStatus::MutationFailed: return "mutation-failed";
  case FrameTapeReplayStatus::BootstrapFailed: return "bootstrap-failed";
  case FrameTapeReplayStatus::CommandReplayFailed: return "command-replay-failed";
  case FrameTapeReplayStatus::PresentOutputFailed: return "present-output-failed";
  case FrameTapeReplayStatus::ReadbackFailed: return "readback-failed";
  case FrameTapeReplayStatus::OutputMismatch: return "output-mismatch";
  case FrameTapeReplayStatus::WarmResetUnsupported:
    return "warm-reset-unsupported";
  case FrameTapeReplayStatus::WarmResetFailed:
    return "warm-reset-failed";
  case FrameTapeReplayStatus::QueryOracleUnsupported:
    return "query-oracle-unsupported";
  case FrameTapeReplayStatus::QueryOracleFailed:
    return "query-oracle-failed";
  case FrameTapeReplayStatus::QueryProgressExhausted:
    return "query-progress-exhausted";
  }
  return "unknown";
}

} // namespace dxmt9::d3d9
