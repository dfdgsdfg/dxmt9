#include "device_c_render_tape_state_fold.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace dxmt9::d3d9 {
namespace {

constexpr std::uint32_t kNoIndex = 0xffffffffu;

template <typename T>
bool load(std::span<const std::byte> bytes, std::size_t offset,
          T& value) noexcept {
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) return false;
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return true;
}

bool checkedAdd(std::size_t left, std::size_t right,
                std::size_t& out) noexcept {
  if (right > std::numeric_limits<std::size_t>::max() - left) return false;
  out = left + right;
  return true;
}

bool checkedMul(std::size_t left, std::size_t right,
                std::size_t& out) noexcept {
  if (left != 0u && right > std::numeric_limits<std::size_t>::max() / left)
    return false;
  out = left * right;
  return true;
}

std::size_t alignUp(std::size_t value, std::size_t alignment) noexcept {
  return (value + alignment - 1u) & ~(alignment - 1u);
}

bool sameIdentity(const D9CWireObjectIdentity& left,
                  const D9CWireObjectIdentity& right) noexcept {
  return left.kind == right.kind && left.generation == right.generation &&
         left.objectId == right.objectId;
}

bool validIdentity(const D9CWireObjectIdentity& identity) noexcept {
  return identity.kind <= D9C_CHUNK_HANDLE_KIND_QUERY &&
         identity.generation != 0u && identity.objectId != 0u;
}

bool isDraw(std::uint32_t type) noexcept {
  switch (type) {
  case D9C_COMMAND_RECORD_DRAW_PRIMITIVE:
  case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE:
  case D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP:
  case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP:
    return true;
  default:
    return false;
  }
}

struct FoldedElement {
  std::uint64_t primaryKey = 0u;
  std::uint64_t secondaryKey = 0u;
  std::vector<std::byte> bytes{};
  bool hasHandle = false;
  bool nullHandle = true;
  std::uint16_t handleOffset = 0u;
  D9CWireObjectIdentity identity{};
};

struct FoldedSection {
  std::uint16_t kind = 0u;
  std::vector<FoldedElement> elements{};
};

struct ConstantFile {
  std::uint16_t kind = 0u;
  std::uint32_t elementSize = 0u;
  std::uint32_t registerCount = 0u;
  std::vector<std::byte> bytes{};
};

struct LifetimeEntry {
  D9CWireObjectIdentity identity{};
  bool live = false;
};

struct FoldState {
  std::array<FoldedSection, kRenderTapeStateCategoryCount> sections{};
  std::array<ConstantFile, 6u> constants{};
  std::vector<LifetimeEntry> lifetimes{};
  std::uint64_t coverageMask = 0u;

  FoldState() {
    for (std::uint16_t kind = 1u; kind <= sections.size(); ++kind)
      sections[kind - 1u].kind = kind;
    constants = {{
        {D9C_COMMAND_CHUNK_SECTION_VS_CONST_F, 16u,
         D9C_DRAW_PACKET_MAX_CONST_VS_F, {}},
        {D9C_COMMAND_CHUNK_SECTION_VS_CONST_I, 16u,
         D9C_DRAW_PACKET_MAX_CONST_VS_I, {}},
        {D9C_COMMAND_CHUNK_SECTION_VS_CONST_B, 4u,
         D9C_DRAW_PACKET_MAX_CONST_VS_B, {}},
        {D9C_COMMAND_CHUNK_SECTION_PS_CONST_F, 16u,
         D9C_DRAW_PACKET_MAX_CONST_PS_F, {}},
        {D9C_COMMAND_CHUNK_SECTION_PS_CONST_I, 16u,
         D9C_DRAW_PACKET_MAX_CONST_PS_I, {}},
        {D9C_COMMAND_CHUNK_SECTION_PS_CONST_B, 4u,
         D9C_DRAW_PACKET_MAX_CONST_PS_B, {}},
    }};
    resetBaseline();
  }

  void resetBaseline() {
    for (auto& section : sections) section.elements.clear();
    for (auto& constant : constants)
      constant.bytes.assign(
          static_cast<std::size_t>(constant.elementSize) *
              constant.registerCount,
          std::byte{0});

    auto& textures = sections[D9C_COMMAND_CHUNK_SECTION_TEXTURE - 1u];
    for (std::uint32_t slot = 0u; slot < D9C_DRAW_PACKET_MAX_TEXTURES;
         ++slot) {
      D9CCommandChunkWireTextureBinding value{
          .slot = slot,
          .valid = 1u,
          .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX,
          .reserved0 = 0u,
      };
      FoldedElement element{
          .primaryKey = slot,
          .bytes = std::vector<std::byte>(sizeof(value)),
          .hasHandle = true,
          .nullHandle = true,
          .handleOffset = static_cast<std::uint16_t>(
              offsetof(D9CCommandChunkWireTextureBinding, handleIndex)),
      };
      std::memcpy(element.bytes.data(), &value, sizeof(value));
      textures.elements.push_back(std::move(element));
    }

    auto& streams = sections[D9C_COMMAND_CHUNK_SECTION_STREAM - 1u];
    for (std::uint32_t slot = 0u; slot < D9C_DRAW_PACKET_MAX_STREAMS; ++slot) {
      D9CCommandChunkWireStreamBinding value{
          .slot = slot,
          .valid = 1u,
          .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX,
          .offset = 0u,
          .stride = 0u,
          .frequency = 0u,
          .reserved0 = 0u,
      };
      FoldedElement element{
          .primaryKey = slot,
          .bytes = std::vector<std::byte>(sizeof(value)),
          .hasHandle = true,
          .nullHandle = true,
          .handleOffset = static_cast<std::uint16_t>(
              offsetof(D9CCommandChunkWireStreamBinding, handleIndex)),
      };
      std::memcpy(element.bytes.data(), &value, sizeof(value));
      streams.elements.push_back(std::move(element));
    }
  }
};

ConstantFile* constantForSection(FoldState& state,
                                 std::uint16_t kind) noexcept {
  const auto found = std::find_if(
      state.constants.begin(), state.constants.end(),
      [&](const auto& constant) { return constant.kind == kind; });
  return found == state.constants.end() ? nullptr : &*found;
}

std::uint16_t sectionForConstantRecord(std::uint32_t type) noexcept {
  switch (type) {
  case D9C_COMMAND_RECORD_SET_VS_CONST_F:
    return D9C_COMMAND_CHUNK_SECTION_VS_CONST_F;
  case D9C_COMMAND_RECORD_SET_VS_CONST_I:
    return D9C_COMMAND_CHUNK_SECTION_VS_CONST_I;
  case D9C_COMMAND_RECORD_SET_VS_CONST_B:
    return D9C_COMMAND_CHUNK_SECTION_VS_CONST_B;
  case D9C_COMMAND_RECORD_SET_PS_CONST_F:
    return D9C_COMMAND_CHUNK_SECTION_PS_CONST_F;
  case D9C_COMMAND_RECORD_SET_PS_CONST_I:
    return D9C_COMMAND_CHUNK_SECTION_PS_CONST_I;
  case D9C_COMMAND_RECORD_SET_PS_CONST_B:
    return D9C_COMMAND_CHUNK_SECTION_PS_CONST_B;
  default:
    return 0u;
  }
}

bool elementApplies(std::uint16_t kind,
                    std::span<const std::byte> bytes) noexcept {
  std::uint32_t valid = 1u;
  switch (kind) {
  case D9C_COMMAND_CHUNK_SECTION_TEXTURE:
    return load(bytes, offsetof(D9CCommandChunkWireTextureBinding, valid),
                valid) && valid != 0u;
  case D9C_COMMAND_CHUNK_SECTION_STREAM:
    return load(bytes, offsetof(D9CCommandChunkWireStreamBinding, valid),
                valid) && valid != 0u;
  case D9C_COMMAND_CHUNK_SECTION_SHADER:
    return load(bytes, offsetof(D9CCommandChunkWireShaderBinding, valid),
                valid) && valid != 0u;
  case D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT:
    return load(bytes, offsetof(D9CCommandChunkWireVertexInput, valid), valid) &&
           valid != 0u;
  case D9C_COMMAND_CHUNK_SECTION_INDEX_BUFFER:
    return load(bytes, offsetof(D9CCommandChunkWireIndexBinding, valid), valid) &&
           valid != 0u;
  case D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET:
    return load(bytes,
                offsetof(D9CCommandChunkWireRenderTargetBinding, valid),
                valid) && valid != 0u;
  case D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL:
    return load(bytes,
                offsetof(D9CCommandChunkWireDepthStencilBinding, valid),
                valid) && valid != 0u;
  default:
    return true;
  }
}

bool elementKey(std::uint16_t kind, std::span<const std::byte> bytes,
                std::uint64_t& primary, std::uint64_t& secondary) noexcept {
  primary = 0u;
  secondary = 0u;
  switch (kind) {
  case D9C_COMMAND_CHUNK_SECTION_RENDER_STATE: {
    D9CCommandChunkWireRenderState value{};
    if (!load(bytes, 0u, value)) return false;
    primary = value.state;
    return true;
  }
  case D9C_COMMAND_CHUNK_SECTION_TEXTURE:
  case D9C_COMMAND_CHUNK_SECTION_STREAM:
  case D9C_COMMAND_CHUNK_SECTION_SHADER:
  case D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET:
  case D9C_COMMAND_CHUNK_SECTION_CLIP_PLANE:
  case D9C_COMMAND_CHUNK_SECTION_LIGHT:
  case D9C_COMMAND_CHUNK_SECTION_LIGHT_ENABLE:
  case D9C_COMMAND_CHUNK_SECTION_TRANSFORM: {
    std::uint32_t key = 0u;
    if (!load(bytes, 0u, key)) return false;
    primary = key;
    return true;
  }
  case D9C_COMMAND_CHUNK_SECTION_TEXTURE_STAGE_STATE: {
    D9CDrawPacketTextureStageState value{};
    if (!load(bytes, 0u, value)) return false;
    primary = value.stage;
    secondary = value.type;
    return true;
  }
  case D9C_COMMAND_CHUNK_SECTION_SAMPLER_STATE: {
    D9CDrawPacketSamplerState value{};
    if (!load(bytes, 0u, value)) return false;
    primary = value.sampler;
    secondary = value.type;
    return true;
  }
  case D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT:
  case D9C_COMMAND_CHUNK_SECTION_INDEX_BUFFER:
  case D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL:
  case D9C_COMMAND_CHUNK_SECTION_VIEWPORT:
  case D9C_COMMAND_CHUNK_SECTION_SCISSOR:
  case D9C_COMMAND_CHUNK_SECTION_MATERIAL:
    return true;
  default:
    return false;
  }
}

void upsert(FoldedSection& section, FoldedElement element) {
  const auto found = std::find_if(
      section.elements.begin(), section.elements.end(), [&](const auto& prior) {
        return prior.primaryKey == element.primaryKey &&
               prior.secondaryKey == element.secondaryKey;
      });
  if (found == section.elements.end())
    section.elements.push_back(std::move(element));
  else
    *found = std::move(element);
}

bool applyConstantRange(FoldState& state, std::uint16_t kind,
                        std::uint32_t startRegister,
                        std::uint32_t registerCount,
                        std::span<const std::byte> bytes) noexcept {
  auto* constant = constantForSection(state, kind);
  if (!constant || registerCount == 0u ||
      startRegister > constant->registerCount ||
      registerCount > constant->registerCount - startRegister)
    return false;
  std::size_t byteOffset = 0u;
  std::size_t byteCount = 0u;
  if (!checkedMul(startRegister, constant->elementSize, byteOffset) ||
      !checkedMul(registerCount, constant->elementSize, byteCount) ||
      bytes.size() != byteCount ||
      byteOffset > constant->bytes.size() ||
      byteCount > constant->bytes.size() - byteOffset)
    return false;
  std::memcpy(constant->bytes.data() + byteOffset, bytes.data(), byteCount);
  state.coverageMask |= std::uint64_t{1} << (kind - 1u);
  return true;
}

bool applySparseRecord(FoldState& state, const ImportedChunkView& chunk,
                       const ImportedRecordView& record,
                       std::uint16_t& failedSectionKind) {
  if (!record.sparseState()) return false;
  if ((record.drawHeader.flags &
       D9C_COMMAND_CHUNK_DRAW_FLAG_FULL_SNAPSHOT) != 0u) {
    state.resetBaseline();
    state.coverageMask = kRenderTapeRequiredCategoryMask;
  }

  for (std::size_t sectionIndex = 0u;
       sectionIndex < record.sections.size(); ++sectionIndex) {
    const auto section = record.section(sectionIndex);
    const auto kind = section.descriptor.kind;
    failedSectionKind = kind;
    if (kind == D9C_COMMAND_CHUNK_SECTION_UP_INDEX_DATA ||
        kind == D9C_COMMAND_CHUNK_SECTION_UP_VERTEX_DATA)
      continue;
    if (kind == 0u || kind > kRenderTapeStateCategoryCount) return false;
    if (auto* constant = constantForSection(state, kind)) {
      D9CCommandChunkWireConstantRange range{};
      if (!load(section.payload, 0u, range) ||
          !applyConstantRange(
              state, kind, range.startRegister, range.registerCount,
              section.payload.subspan(sizeof(range))))
        return false;
      (void)constant;
      continue;
    }

    const auto* rule = sectionRule(kind);
    if (!rule || section.descriptor.elementSize != rule->elementSize)
      return false;
    auto& destination = state.sections[kind - 1u];
    for (std::uint32_t elementIndex = 0u;
         elementIndex < section.descriptor.count; ++elementIndex) {
      const auto elementBytes = section.payload.subspan(
          static_cast<std::size_t>(elementIndex) * rule->elementSize,
          rule->elementSize);
      if (!elementApplies(kind, elementBytes)) continue;
      FoldedElement element{};
      if (!elementKey(kind, elementBytes, element.primaryKey,
                      element.secondaryKey))
        return false;
      element.bytes.assign(elementBytes.begin(), elementBytes.end());
      if (const auto* handleRule = sectionHandleFieldRule(kind)) {
        std::uint32_t handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
        if (!load(elementBytes, handleRule->payloadOffset, handleIndex))
          return false;
        element.hasHandle = true;
        element.nullHandle =
            handleIndex == D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
        element.handleOffset = handleRule->payloadOffset;
        if (!element.nullHandle) {
          if (handleIndex >= chunk.handles.size()) return false;
          const auto& handle = chunk.handles[handleIndex];
          element.identity = D9CWireObjectIdentity{
              .kind = handle.kind,
              .generation = handle.generation,
              .objectId = handle.objectId,
          };
          if (handle.kind != handleRule->handleKind ||
              !validIdentity(element.identity))
            return false;
        }
        const auto nullIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
        std::memcpy(element.bytes.data() + element.handleOffset, &nullIndex,
                    sizeof(nullIndex));
      }
      upsert(destination, std::move(element));
    }
    state.coverageMask |= std::uint64_t{1} << (kind - 1u);
  }
  failedSectionKind = 0u;
  return true;
}

bool applyConstantRecord(FoldState& state,
                         const ImportedRecordView& record) noexcept {
  const auto kind = sectionForConstantRecord(record.header.type);
  if (kind == 0u) return false;
  D9CCommandChunkWireSetConst fixed{};
  return load(record.payload, 0u, fixed) &&
         applyConstantRange(state, kind, fixed.startRegister,
                            fixed.registerCount,
                            record.payload.subspan(sizeof(fixed)));
}

void noteDefined(FoldState& state,
                 const D9CWireObjectIdentity& identity) {
  const auto found = std::find_if(
      state.lifetimes.begin(), state.lifetimes.end(), [&](const auto& value) {
        return sameIdentity(value.identity, identity);
      });
  if (found == state.lifetimes.end())
    state.lifetimes.push_back({identity, true});
  else
    found->live = true;
}

void noteDestroyed(FoldState& state,
                   const D9CWireObjectIdentity& identity) noexcept {
  const auto found = std::find_if(
      state.lifetimes.begin(), state.lifetimes.end(), [&](const auto& value) {
        return sameIdentity(value.identity, identity);
      });
  if (found != state.lifetimes.end()) found->live = false;
}

bool identityLive(const FoldState& state,
                  const D9CWireObjectIdentity& identity) noexcept {
  const auto found = std::find_if(
      state.lifetimes.begin(), state.lifetimes.end(), [&](const auto& value) {
        return sameIdentity(value.identity, identity);
      });
  return found != state.lifetimes.end() && found->live;
}

bool chunkBytes(std::span<const std::byte> bytes, std::size_t& total) noexcept {
  D9CCommandChunkWireHeader header{};
  if (!load(bytes, 0u, header)) return false;
  return checkedAdd(header.payloadArenaOffset, header.payloadArenaSize, total) &&
         total <= bytes.size();
}

bool appendUniqueIdentity(std::vector<D9CWireObjectIdentity>& identities,
                          const D9CWireObjectIdentity& identity,
                          std::uint32_t& index) {
  const auto found = std::find_if(
      identities.begin(), identities.end(), [&](const auto& prior) {
        return sameIdentity(prior, identity);
      });
  if (found != identities.end()) {
    index = static_cast<std::uint32_t>(found - identities.begin());
    return true;
  }
  if (identities.size() >= std::numeric_limits<std::uint32_t>::max())
    return false;
  index = static_cast<std::uint32_t>(identities.size());
  identities.push_back(identity);
  return true;
}

bool buildBootstrapChunk(FoldState& state,
                         RenderTapeStateFoldResult& result) {
  std::vector<FoldedSection*> present;
  for (auto& section : state.sections) {
    if (constantForSection(state, section.kind) || !section.elements.empty())
      present.push_back(&section);
  }
  if (present.empty()) return false;

  for (auto* section : present) {
    std::sort(section->elements.begin(), section->elements.end(),
              [](const auto& left, const auto& right) {
                return left.primaryKey < right.primaryKey ||
                       (left.primaryKey == right.primaryKey &&
                        left.secondaryKey < right.secondaryKey);
              });
  }

  const auto sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader);
  const auto sectionPayloadOffset = alignUp(
      sectionTableOffset +
          present.size() * sizeof(D9CCommandChunkWireSectionDesc),
      alignof(std::uint32_t));
  std::vector<D9CCommandChunkWireSectionDesc> descriptors;
  std::vector<std::byte> payload(sectionPayloadOffset, std::byte{0});
  descriptors.reserve(present.size());

  for (auto* section : present) {
    const auto* rule = sectionRule(section->kind);
    if (!rule) return false;
    payload.resize(alignUp(payload.size(), rule->payloadAlignment),
                   std::byte{0});
    const auto offset = payload.size();
    std::uint32_t count = 0u;
    if (auto* constant = constantForSection(state, section->kind)) {
      const D9CCommandChunkWireConstantRange range{
          .startRegister = 0u,
          .registerCount = constant->registerCount,
      };
      const auto* rangeBytes = reinterpret_cast<const std::byte*>(&range);
      payload.insert(payload.end(), rangeBytes, rangeBytes + sizeof(range));
      payload.insert(payload.end(), constant->bytes.begin(),
                     constant->bytes.end());
      count = constant->registerCount;
    } else {
      if (section->elements.size() > rule->maxCount) return false;
      count = static_cast<std::uint32_t>(section->elements.size());
      for (auto& element : section->elements) {
        if (element.bytes.size() != rule->elementSize) return false;
        if (element.hasHandle && !element.nullHandle) {
          std::uint32_t handleIndex = 0u;
          if (!appendUniqueIdentity(result.referencedIdentities,
                                    element.identity, handleIndex))
            return false;
          std::memcpy(element.bytes.data() + element.handleOffset,
                      &handleIndex, sizeof(handleIndex));
        }
        payload.insert(payload.end(), element.bytes.begin(),
                       element.bytes.end());
      }
    }
    descriptors.push_back(D9CCommandChunkWireSectionDesc{
        .kind = section->kind,
        .elementSize = rule->elementSize,
        .count = count,
        .payloadOffset = static_cast<std::uint32_t>(offset),
        .byteSize = static_cast<std::uint32_t>(payload.size() - offset),
    });
  }

  const D9CCommandChunkWireDrawHeader draw{
      .flags = D9C_COMMAND_CHUNK_DRAW_FLAG_FULL_SNAPSHOT,
      .primitiveType = 0u,
      .baseVertex = 0,
      .minVertex = 0u,
      .numVertices = 0u,
      .startVertex = 0u,
      .startIndex = 0u,
      .primitiveCount = 0u,
      .stride = 0u,
      .indexFormat = 0u,
      .sectionCount = static_cast<std::uint32_t>(descriptors.size()),
      .sectionTableOffset = static_cast<std::uint32_t>(sectionTableOffset),
      .sectionPayloadOffset = static_cast<std::uint32_t>(sectionPayloadOffset),
      .reserved0 = 0u,
  };
  std::memcpy(payload.data(), &draw, sizeof(draw));
  std::memcpy(payload.data() + sectionTableOffset, descriptors.data(),
              descriptors.size() * sizeof(descriptors[0]));

  const auto recordTableOffset = sizeof(D9CCommandChunkWireHeader);
  const auto handleTableOffset = alignUp(
      recordTableOffset + sizeof(D9CCommandChunkWireRecordHeader),
      alignof(D9CCommandChunkWireHandleEntry));
  const auto payloadArenaOffset = alignUp(
      handleTableOffset + result.referencedIdentities.size() *
                              sizeof(D9CCommandChunkWireHandleEntry),
      alignof(std::uint32_t));
  const D9CCommandChunkWireHeader header{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
      .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE,
      .recordTableOffset = static_cast<std::uint32_t>(recordTableOffset),
      .recordCount = 1u,
      .handleTableOffset = static_cast<std::uint32_t>(handleTableOffset),
      .handleCount = static_cast<std::uint32_t>(
          result.referencedIdentities.size()),
      .payloadArenaOffset = static_cast<std::uint32_t>(payloadArenaOffset),
      .payloadArenaSize = static_cast<std::uint32_t>(payload.size()),
      .reserved0 = 0u,
      .reserved1 = 0u,
  };
  const D9CCommandChunkWireRecordHeader record{
      .type = D9C_COMMAND_RECORD_APPLY_STATE,
      .flags = 0u,
      .payloadOffset = 0u,
      .payloadSize = static_cast<std::uint32_t>(payload.size()),
      .firstHandle = 0u,
      .handleCount = header.handleCount,
      .reserved0 = 0u,
      .reserved1 = 0u,
  };
  result.bootstrapChunk.assign(payloadArenaOffset + payload.size(),
                               std::byte{0});
  std::memcpy(result.bootstrapChunk.data(), &header, sizeof(header));
  std::memcpy(result.bootstrapChunk.data() + recordTableOffset, &record,
              sizeof(record));
  for (std::size_t index = 0u; index < result.referencedIdentities.size();
       ++index) {
    const auto& identity = result.referencedIdentities[index];
    const D9CCommandChunkWireHandleEntry handle{
        .kind = identity.kind,
        .generation = identity.generation,
        .objectId = identity.objectId,
    };
    std::memcpy(result.bootstrapChunk.data() + handleTableOffset +
                    index * sizeof(handle),
                &handle, sizeof(handle));
  }
  std::memcpy(result.bootstrapChunk.data() + payloadArenaOffset, payload.data(),
              payload.size());
  const auto validation = validateCommandChunk(
      result.bootstrapChunk,
      CommandChunkEnvelope{
          .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
          .recordCount = 1u,
          .handleCount = header.handleCount,
      });
  return validation.valid();
}

} // namespace

RenderTapeStateFoldResult foldRenderTapeStateForDraw(
    std::span<const std::byte> source,
    const RenderTapeBlobCatalogue& verifiedCatalogue,
    std::uint64_t commandEventOrdinal,
    std::uint32_t recordIndex) noexcept {
  RenderTapeStateFoldResult result{};
  try {
    ImportedRenderTapeView tape;
    result.sourceValidation =
        validateRenderTape(source, verifiedCatalogue, &tape);
    if (!result.sourceValidation.valid()) {
      result.status = RenderTapeStateFoldStatus::InvalidSource;
      result.failedEventIndex = result.sourceValidation.failedEventIndex;
      return result;
    }
    if (tape.header.profile != kRenderTapeProfileFrame ||
        commandEventOrdinal == 0u) {
      result.status = RenderTapeStateFoldStatus::InvalidSelection;
      return result;
    }

    FoldState state;
    bool sawBootstrap = false;
    bool selected = false;
    for (std::uint32_t eventIndex = 0u; eventIndex < tape.events.size();
         ++eventIndex) {
      result.failedEventIndex = eventIndex;
      const auto event = tape.event(eventIndex);
      const auto type = static_cast<RenderTapeEventType>(event.header.type);
      if (type == RenderTapeEventType::BootstrapState) {
        RenderTapeBootstrapHeader fixed{};
        if (sawBootstrap || !load(event.payload, 0u, fixed) ||
            fixed.requiredCategoryMask != kRenderTapeRequiredCategoryMask ||
            fixed.stateCategoryCount != kRenderTapeStateCategoryCount) {
          result.status = RenderTapeStateFoldStatus::IncompleteCoverage;
          return result;
        }
        sawBootstrap = true;
        state.coverageMask = fixed.requiredCategoryMask;
        const auto tail = event.payload.subspan(sizeof(fixed));
        std::size_t walk = 0u;
        for (std::uint32_t overlayIndex = 0u;
             overlayIndex < fixed.overlayCount; ++overlayIndex) {
          std::size_t total = 0u;
          if (walk > tail.size() || !chunkBytes(tail.subspan(walk), total)) {
            result.status = RenderTapeStateFoldStatus::InvalidStateRecord;
            return result;
          }
          D9CCommandChunkWireHeader header{};
          if (!load(tail, walk, header)) {
            result.status = RenderTapeStateFoldStatus::InvalidStateRecord;
            return result;
          }
          ImportedChunkView chunk;
          if (!importPrevalidatedCommandChunk(
                  tail.subspan(walk, total),
                  CommandChunkEnvelope{
                      .version = header.version,
                      .recordCount = header.recordCount,
                      .handleCount = header.handleCount,
                  },
                  chunk)) {
            result.status = RenderTapeStateFoldStatus::InvalidStateRecord;
            return result;
          }
          for (std::uint32_t overlayRecord = 0u;
               overlayRecord < chunk.records.size(); ++overlayRecord) {
            result.failedRecordIndex = overlayRecord;
            const auto record = chunk.record(overlayRecord);
            if (record.header.type != D9C_COMMAND_RECORD_APPLY_STATE ||
                !applySparseRecord(state, chunk, record,
                                   result.failedSectionKind)) {
              result.status = RenderTapeStateFoldStatus::InvalidStateRecord;
              return result;
            }
          }
          walk += total;
        }
        if (walk > tail.size() || fixed.gammaRampBytes > tail.size() - walk ||
            tail.size() - walk != fixed.gammaRampBytes) {
          result.status = RenderTapeStateFoldStatus::InvalidStateRecord;
          return result;
        }
        result.gammaRamp.assign(tail.begin() + walk, tail.end());
        continue;
      }
      if (type == RenderTapeEventType::ObjectDefine) {
        RenderTapeObjectDefineHeader fixed{};
        if (!load(event.payload, 0u, fixed)) {
          result.status = RenderTapeStateFoldStatus::InvalidSource;
          return result;
        }
        noteDefined(state, fixed.identity);
        continue;
      }
      if (type == RenderTapeEventType::ObjectDestroy) {
        RenderTapeObjectDestroyHeader fixed{};
        if (!load(event.payload, 0u, fixed)) {
          result.status = RenderTapeStateFoldStatus::InvalidSource;
          return result;
        }
        noteDestroyed(state, fixed.identity);
        continue;
      }
      if (type == RenderTapeEventType::OrderedControl &&
          event.header.ordinal < commandEventOrdinal) {
        result.status = RenderTapeStateFoldStatus::UnsupportedOrderedInput;
        return result;
      }
      if (type != RenderTapeEventType::CommandChunk) continue;

      RenderTapeCommandChunkHeader fixed{};
      if (!load(event.payload, 0u, fixed)) {
        result.status = RenderTapeStateFoldStatus::InvalidSource;
        return result;
      }
      ImportedChunkView chunk;
      if (!importPrevalidatedCommandChunk(
              event.payload.subspan(sizeof(fixed), fixed.chunkBytes),
              CommandChunkEnvelope{
                  .version = fixed.wireVersion,
                  .recordCount = fixed.recordCount,
                  .handleCount = fixed.handleCount,
              },
              chunk)) {
        result.status = RenderTapeStateFoldStatus::InvalidStateRecord;
        return result;
      }
      const bool targetEvent = event.header.ordinal == commandEventOrdinal;
      if (targetEvent && recordIndex >= chunk.records.size()) {
        result.status = RenderTapeStateFoldStatus::InvalidSelection;
        result.failedRecordIndex = recordIndex;
        return result;
      }
      for (std::uint32_t index = 0u; index < chunk.records.size(); ++index) {
        if (targetEvent && index > recordIndex) break;
        result.failedRecordIndex = index;
        const auto record = chunk.record(index);
        if (record.sparseState()) {
          if (!applySparseRecord(state, chunk, record,
                                 result.failedSectionKind)) {
            result.status = RenderTapeStateFoldStatus::InvalidStateRecord;
            return result;
          }
        } else if (sectionForConstantRecord(record.header.type) != 0u) {
          if (!applyConstantRecord(state, record)) {
            result.status = RenderTapeStateFoldStatus::InvalidStateRecord;
            return result;
          }
        }
        if (targetEvent && index == recordIndex) {
          if (!isDraw(record.header.type)) {
            result.status = RenderTapeStateFoldStatus::InvalidSelection;
            return result;
          }
          result.selectedRecordWasFullSnapshot =
              (record.drawHeader.flags &
               D9C_COMMAND_CHUNK_DRAW_FLAG_FULL_SNAPSHOT) != 0u;
          selected = true;
          break;
        }
      }
      if (selected) break;
    }

    if (!sawBootstrap) {
      result.status = RenderTapeStateFoldStatus::MissingBootstrap;
      return result;
    }
    if (!selected) {
      result.status = RenderTapeStateFoldStatus::InvalidSelection;
      return result;
    }
    result.coverageMask = state.coverageMask;
    if (result.coverageMask != kRenderTapeRequiredCategoryMask) {
      result.status = RenderTapeStateFoldStatus::IncompleteCoverage;
      return result;
    }
    if (!buildBootstrapChunk(state, result)) {
      result.status = RenderTapeStateFoldStatus::OutputBuildFailed;
      result.bootstrapChunk.clear();
      result.referencedIdentities.clear();
      return result;
    }
    for (const auto& identity : result.referencedIdentities) {
      if (!identityLive(state, identity)) {
        result.status = RenderTapeStateFoldStatus::MissingLiveIdentity;
        result.bootstrapChunk.clear();
        result.referencedIdentities.clear();
        return result;
      }
    }
    result.status = RenderTapeStateFoldStatus::Valid;
    result.failedEventIndex = kNoIndex;
    result.failedRecordIndex = kNoIndex;
    result.failedSectionKind = 0u;
    return result;
  } catch (...) {
    result.status = RenderTapeStateFoldStatus::AllocationFailed;
    result.bootstrapChunk.clear();
    result.gammaRamp.clear();
    result.referencedIdentities.clear();
    return result;
  }
}

const char* renderTapeStateFoldStatusName(
    RenderTapeStateFoldStatus status) noexcept {
  switch (status) {
  case RenderTapeStateFoldStatus::Valid: return "valid";
  case RenderTapeStateFoldStatus::InvalidSource: return "invalid-source";
  case RenderTapeStateFoldStatus::InvalidSelection:
    return "invalid-selection";
  case RenderTapeStateFoldStatus::MissingBootstrap:
    return "missing-bootstrap";
  case RenderTapeStateFoldStatus::IncompleteCoverage:
    return "incomplete-coverage";
  case RenderTapeStateFoldStatus::InvalidStateRecord:
    return "invalid-state-record";
  case RenderTapeStateFoldStatus::UnsupportedOrderedInput:
    return "unsupported-ordered-input";
  case RenderTapeStateFoldStatus::MissingLiveIdentity:
    return "missing-live-identity";
  case RenderTapeStateFoldStatus::OutputBuildFailed:
    return "output-build-failed";
  case RenderTapeStateFoldStatus::AllocationFailed:
    return "allocation-failed";
  }
  return "unknown";
}

} // namespace dxmt9::d3d9
