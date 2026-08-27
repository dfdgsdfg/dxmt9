#pragma once

#include "d3d9_pe_retainer.hpp"
#include "device_c_chunk_schema.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>

namespace dxmt9::d3d9::pe {

// Payload values are copied verbatim into the bounded wire blob. Admission is
// intentionally closed over the fixed payload types used by the producer.
// Variable tails and sparse sections have separate closed adapters below; raw
// byte append/overwrite is private to CommandChunkBuilder.
template <typename T>
struct IsApprovedWirePayloadValue : std::false_type {};

#define DXMT9_PE_APPROVED_WIRE_PAYLOAD(Type) \
  template <>                                  \
  struct IsApprovedWirePayloadValue<Type> : std::true_type {}

DXMT9_PE_APPROVED_WIRE_PAYLOAD(D9CCommandChunkWireDrawHeader);
DXMT9_PE_APPROVED_WIRE_PAYLOAD(D9CCommandChunkWireConstantRange);
DXMT9_PE_APPROVED_WIRE_PAYLOAD(D9CCommandChunkWireSetConst);
DXMT9_PE_APPROVED_WIRE_PAYLOAD(D9CCommandChunkWireClear);
DXMT9_PE_APPROVED_WIRE_PAYLOAD(D9CCommandChunkWirePresent);
DXMT9_PE_APPROVED_WIRE_PAYLOAD(D9CCommandChunkWireStretchRect);
DXMT9_PE_APPROVED_WIRE_PAYLOAD(D9CCommandChunkWireColorFill);
DXMT9_PE_APPROVED_WIRE_PAYLOAD(D9CCommandChunkWireUpdateTexture);
DXMT9_PE_APPROVED_WIRE_PAYLOAD(D9CCommandChunkWireUpdateSurface);
DXMT9_PE_APPROVED_WIRE_PAYLOAD(D9CCommandChunkWireQueryIssue);
DXMT9_PE_APPROVED_WIRE_PAYLOAD(D9CCommandChunkWireReadback);
DXMT9_PE_APPROVED_WIRE_PAYLOAD(D9CCommandChunkWireReszDepthResolve);
DXMT9_PE_APPROVED_WIRE_PAYLOAD(D9CCommandChunkWireGenerateMipmaps);
DXMT9_PE_APPROVED_WIRE_PAYLOAD(std::uint32_t);

#undef DXMT9_PE_APPROVED_WIRE_PAYLOAD

template <typename T>
inline constexpr bool isWireSafePayloadValue =
    std::is_standard_layout_v<std::remove_cv_t<T>> &&
    std::is_trivially_copyable_v<std::remove_cv_t<T>> &&
    !std::is_reference_v<T> &&
    IsApprovedWirePayloadValue<std::remove_cv_t<T>>::value;

// Exact section-kind qualification for every POD section payload emitted by
// the PE producer. A new section payload type is unusable until it is added to
// this registry, which is the auditable pointer-free boundary for typed array
// copies. Byte-addressed constant and UP-data sections are admitted only by
// their dedicated, rule-checked adapters.
#define DXMT9_PE_WIRE_SECTION_SCHEMA(X)                                    \
  X(D9CCommandChunkWireRenderState, D9C_COMMAND_CHUNK_SECTION_RENDER_STATE) \
  X(D9CCommandChunkWireTextureBinding, D9C_COMMAND_CHUNK_SECTION_TEXTURE)   \
  X(D9CCommandChunkWireStreamBinding, D9C_COMMAND_CHUNK_SECTION_STREAM)     \
  X(D9CCommandChunkWireShaderBinding, D9C_COMMAND_CHUNK_SECTION_SHADER)     \
  X(D9CCommandChunkWireVertexInput, D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT) \
  X(D9CCommandChunkWireIndexBinding,                                        \
    D9C_COMMAND_CHUNK_SECTION_INDEX_BUFFER)                                 \
  X(D9CCommandChunkWireRenderTargetBinding,                                 \
    D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET)                                \
  X(D9CCommandChunkWireDepthStencilBinding,                                 \
    D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL)                                \
  X(D9CViewport, D9C_COMMAND_CHUNK_SECTION_VIEWPORT)                        \
  X(D9CRect, D9C_COMMAND_CHUNK_SECTION_SCISSOR)                             \
  X(D9CMaterial, D9C_COMMAND_CHUNK_SECTION_MATERIAL)                        \
  X(D9CCommandChunkWireClipPlane, D9C_COMMAND_CHUNK_SECTION_CLIP_PLANE)     \
  X(D9CDrawPacketTextureStageState,                                         \
    D9C_COMMAND_CHUNK_SECTION_TEXTURE_STAGE_STATE)                          \
  X(D9CDrawPacketSamplerState, D9C_COMMAND_CHUNK_SECTION_SAMPLER_STATE)     \
  X(D9CDrawPacketTransform, D9C_COMMAND_CHUNK_SECTION_TRANSFORM)            \
  X(D9CCommandChunkWireLight, D9C_COMMAND_CHUNK_SECTION_LIGHT)              \
  X(D9CCommandChunkWireLightEnable, D9C_COMMAND_CHUNK_SECTION_LIGHT_ENABLE)

template <typename T>
struct ApprovedWireSectionPayload {
  static constexpr std::uint16_t kind =
      std::numeric_limits<std::uint16_t>::max();
};

#define DXMT9_PE_APPROVED_WIRE_SECTION(Type, Kind)              \
  template <>                                                   \
  struct ApprovedWireSectionPayload<Type> {                     \
    static_assert(std::is_standard_layout_v<Type>);             \
    static_assert(std::is_trivially_copyable_v<Type>);          \
    static constexpr std::uint16_t kind = Kind;                 \
  };
DXMT9_PE_WIRE_SECTION_SCHEMA(DXMT9_PE_APPROVED_WIRE_SECTION)
#undef DXMT9_PE_APPROVED_WIRE_SECTION

struct WireSectionPayloadDescriptor {
  std::uint16_t kind;
  std::uint16_t elementSize;
};

#define DXMT9_PE_WIRE_SECTION_DESCRIPTOR(Type, Kind) \
  WireSectionPayloadDescriptor{Kind, sizeof(Type)},
inline constexpr auto kWireSectionPayloadDescriptors = std::array{
    DXMT9_PE_WIRE_SECTION_SCHEMA(DXMT9_PE_WIRE_SECTION_DESCRIPTOR)
};
#undef DXMT9_PE_WIRE_SECTION_DESCRIPTOR

consteval bool wireSectionPayloadRegistryComplete() noexcept {
  std::size_t typedRuleCount = 0u;
  for (const auto& rule : kSectionRules) {
    if ((rule.ruleFlags &
         (SectionRuleConstantRange | SectionRuleRawBytes)) != 0u) {
      continue;
    }
    ++typedRuleCount;
    std::size_t matches = 0u;
    for (const auto descriptor : kWireSectionPayloadDescriptors) {
      matches += descriptor.kind == rule.kind &&
                         descriptor.elementSize == rule.elementSize
          ? 1u : 0u;
    }
    if (matches != 1u) return false;
  }
  return typedRuleCount == kWireSectionPayloadDescriptors.size();
}

static_assert(wireSectionPayloadRegistryComplete(),
              "every POD section rule needs one exact typed registry row");

#undef DXMT9_PE_WIRE_SECTION_SCHEMA

template <typename T>
inline constexpr bool isWireSafeSectionPayload =
    std::is_standard_layout_v<std::remove_cv_t<T>> &&
    std::is_trivially_copyable_v<std::remove_cv_t<T>> &&
    !std::is_reference_v<T> && !std::is_pointer_v<std::remove_cv_t<T>> &&
    ApprovedWireSectionPayload<std::remove_cv_t<T>>::kind !=
        std::numeric_limits<std::uint16_t>::max();

struct PeWireObjectRef {
  D9CWireObjectIdentity identity{};
  void* object = nullptr;

  bool valid(std::uint32_t expectedKind) const noexcept {
    return object && identity.kind == expectedKind &&
           identity.kind <= D9C_CHUNK_HANDLE_KIND_QUERY &&
           identity.generation != 0u && identity.objectId != 0u;
  }
};

static_assert(sizeof(PeWireObjectRef) == 24u);
static_assert(alignof(PeWireObjectRef) == 8u);
static_assert(std::is_trivially_copyable_v<PeWireObjectRef>);

template <std::uint32_t Kind>
struct PeLocalObjectRef : PeWireObjectRef {
  static constexpr std::uint32_t kind = Kind;

  bool valid() const noexcept { return PeWireObjectRef::valid(Kind); }
};

using TextureRef = PeLocalObjectRef<D9C_CHUNK_HANDLE_KIND_TEXTURE>;
using BufferRef = PeLocalObjectRef<D9C_CHUNK_HANDLE_KIND_BUFFER>;
using SurfaceRef = PeLocalObjectRef<D9C_CHUNK_HANDLE_KIND_SURFACE>;
using ShaderRef = PeLocalObjectRef<D9C_CHUNK_HANDLE_KIND_SHADER>;
using DeclarationRef = PeLocalObjectRef<D9C_CHUNK_HANDLE_KIND_VERTEX_DECL>;
using QueryRef = PeLocalObjectRef<D9C_CHUNK_HANDLE_KIND_QUERY>;

struct PeLocalObjectIdentity {
  std::uint32_t kind = 0u;
  void* object = nullptr;

  friend bool operator==(const PeLocalObjectIdentity&,
                         const PeLocalObjectIdentity&) = default;
};

// A logical Render Tape pending-chunk lease may only be issued by a builder
// after it has proved that the exact wrapper identity is present in the
// committed portion of the current chunk.  The constructor is private so a
// registry cannot manufacture a lease from a wire identity alone; the token
// is only a witness and owns no retain/release operation.
class CommittedPendingChunkLease final {
 public:
  CommittedPendingChunkLease(const CommittedPendingChunkLease&) = delete;
  CommittedPendingChunkLease& operator=(
      const CommittedPendingChunkLease&) = delete;
  CommittedPendingChunkLease(CommittedPendingChunkLease&&) = delete;
  CommittedPendingChunkLease& operator=(CommittedPendingChunkLease&&) = delete;

  const PeWireObjectRef& object() const noexcept { return object_; }
  PeLocalObjectIdentity localIdentity() const noexcept {
    return PeLocalObjectIdentity{.kind = object_.identity.kind,
                                 .object = object_.object};
  }

 private:
  friend class CommandChunkBuilder;

  explicit CommittedPendingChunkLease(PeWireObjectRef object) noexcept
      : object_(object) {}

  PeWireObjectRef object_{};
};

template <std::uint32_t Kind>
constexpr PeLocalObjectIdentity localIdentity(
    const PeLocalObjectRef<Kind>& object) noexcept {
  return PeLocalObjectIdentity{.kind = Kind, .object = object.object};
}

template <typename Ref>
constexpr Ref qualifyLocalRef(const PeWireObjectRef& object) noexcept {
  static_assert(std::is_base_of_v<PeWireObjectRef, Ref>);
  Ref out{};
  if (object.identity.kind != Ref::kind) {
    return out;
  }
  out.identity = object.identity;
  out.object = object.object;
  return out;
}

#define DXMT9_PE_PIN_LOCAL_REF(Type)                      \
  static_assert(sizeof(Type) == sizeof(PeWireObjectRef)); \
  static_assert(alignof(Type) == alignof(PeWireObjectRef)); \
  static_assert(std::is_trivially_copyable_v<Type>)

DXMT9_PE_PIN_LOCAL_REF(TextureRef);
DXMT9_PE_PIN_LOCAL_REF(BufferRef);
DXMT9_PE_PIN_LOCAL_REF(SurfaceRef);
DXMT9_PE_PIN_LOCAL_REF(ShaderRef);
DXMT9_PE_PIN_LOCAL_REF(DeclarationRef);
DXMT9_PE_PIN_LOCAL_REF(QueryRef);

#undef DXMT9_PE_PIN_LOCAL_REF

void noteWireIdentityGetterCall() noexcept;
std::uint64_t wireIdentityGetterCallCount() noexcept;

template <typename Object, typename Getter>
bool cacheWireObjectRef(Object* object, std::uint32_t expectedKind,
                        Getter&& getter, PeWireObjectRef& out) {
  out = {};
  if (!object) {
    return false;
  }
  noteWireIdentityGetterCall();
  D9CWireObjectIdentity identity{};
  if (getter(object, &identity) < 0 || identity.kind != expectedKind ||
      identity.generation == 0u || identity.objectId == 0u) {
    return false;
  }
  out.identity = identity;
  out.object = object;
  return true;
}

// Production wrappers know their local kind at the call site. Keep that
// knowledge in the output type so a cache call cannot accidentally publish a
// texture identity into a buffer-local slot. The four-field wire contract is
// unchanged; the qualification exists only on the PE side.
template <typename Object, typename Getter, typename Ref>
bool cacheWireObjectRef(Object* object, Getter&& getter, Ref& out) {
  static_assert(std::is_base_of_v<PeWireObjectRef, Ref>);
  out = {};
  if (!object) {
    return false;
  }
  noteWireIdentityGetterCall();
  D9CWireObjectIdentity identity{};
  if (getter(object, &identity) < 0 || identity.kind != Ref::kind ||
      identity.generation == 0u || identity.objectId == 0u) {
    return false;
  }
  out.identity = identity;
  out.object = object;
  return true;
}

struct CommandChunkBuilderCapacities {
  std::size_t records = 64u;
  std::size_t handles = 256u;
  std::size_t payloadBytes = 256u * 1024u;
  std::size_t sealedBytes = 272u * 1024u;
};

struct SealedCommandChunk {
  std::span<const std::byte> blob{};
  std::uint32_t recordCount = 0u;
  std::uint32_t handleCount = 0u;

  bool valid() const noexcept { return !blob.empty(); }
};

// R-BACK-43.4 `producer-owned` (PE game thread). Every member below —
// `records_`, `handles_`, `handleObjects_`, `payload_`, `sealedBlob_`,
// `retainer_`, `active_`, `sealed_` — is written and read only on the thread
// driving the D3D9 recorder, and none of it is reachable from the replay
// worker, encode thread, or completion path: the builder's output crosses to
// unix as the sealed POD blob, never as live state.
//
// Enforcement is at the `D3D9DeviceImpl` call boundary
// (`assertRecorderThreadConfined()`, R-BACK-43.5 shape (c)), not with a token
// here — see the same note on `D3D9PePendingCommandRetainer` for why a
// builder-local construction-bound token would be incorrect under
// `D3DCREATE_MULTITHREADED` rather than merely duplicated.
class CommandChunkBuilder {
 public:
  explicit CommandChunkBuilder(
      const CommandChunkBuilderCapacities& capacities = {});
  ~CommandChunkBuilder() = default;

  CommandChunkBuilder(const CommandChunkBuilder&) = delete;
  CommandChunkBuilder& operator=(const CommandChunkBuilder&) = delete;

  bool beginRecord(std::uint32_t type) noexcept;

  template <typename T>
  bool appendPayloadValue(const T& value,
                          std::uint32_t* recordRelativeOffset = nullptr) {
    static_assert(isWireSafePayloadValue<T>,
                  "appendPayloadValue requires a standard-layout, trivially-copyable, pointer-free wire value");
    return appendPayload(
        std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(&value), sizeof(value)),
        alignof(T), recordRelativeOffset);
  }

  template <typename T>
    requires isWireSafeSectionPayload<T>
  bool appendSectionPayload(
      std::uint16_t kind, std::span<const T> values,
      std::uint32_t* recordRelativeOffset = nullptr) noexcept {
    const auto* rule = sectionRule(kind);
    const auto* activeRule = active_.active ? recordRule(active_.type) : nullptr;
    if (!rule || !activeRule ||
        (activeRule->ruleFlags & RecordRuleSparseState) == 0u ||
        kind != ApprovedWireSectionPayload<std::remove_cv_t<T>>::kind ||
        values.empty() || values.size() > rule->maxCount ||
        sizeof(T) != rule->elementSize ||
        values.size() > std::numeric_limits<std::uint32_t>::max()) {
      return failActiveRecord();
    }
    return appendPayload(std::as_bytes(values), rule->payloadAlignment,
                         recordRelativeOffset);
  }

  // Materializes a typed section directly into its final record payload.
  // The callback may resolve/retain handles, but it never receives raw
  // payload storage: one value is prepared at a time and copied into the
  // already-reserved final range by record-relative offset. This keeps a
  // payload-vector reallocation or a failed handle append from invalidating a
  // borrowed pointer, while removing the former section-sized staging array.
  template <typename T, typename Generate>
    requires isWireSafeSectionPayload<T> &&
        std::is_nothrow_invocable_r_v<bool, Generate&, std::size_t, T&>
  bool appendGeneratedSectionPayload(
      std::uint16_t kind, std::size_t count, Generate&& generate,
      std::uint32_t* recordRelativeOffset = nullptr) noexcept {
    const auto* rule = sectionRule(kind);
    const auto* activeRule = active_.active ? recordRule(active_.type) : nullptr;
    if (!rule || !activeRule ||
        (activeRule->ruleFlags & RecordRuleSparseState) == 0u ||
        kind != ApprovedWireSectionPayload<std::remove_cv_t<T>>::kind ||
        count == 0u || count > rule->maxCount ||
        sizeof(T) != rule->elementSize ||
        count > std::numeric_limits<std::uint32_t>::max() ||
        count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
      return failActiveRecord();
    }

    std::uint32_t offset = 0u;
    if (!reservePayload(count * sizeof(T), rule->payloadAlignment, &offset)) {
      return false;
    }
    for (std::size_t i = 0u; i < count; ++i) {
      T value{};
      if (!generate(i, value)) {
        return failActiveRecord();
      }
      // Re-derive the address after the callback. Binding generators may
      // append handles and retain wrappers, but the final payload is named by
      // offset rather than by a pointer borrowed across that work.
      const std::size_t relative =
          static_cast<std::size_t>(offset) + i * sizeof(T);
      const std::size_t absolute = active_.payloadStart + relative;
      if (!active_.active || sealed_ ||
          absolute < active_.payloadStart ||
          absolute > payload_.size() ||
          sizeof(T) > payload_.size() - absolute) {
        return failActiveRecord();
      }
      std::memcpy(payload_.data() + absolute, &value, sizeof(T));
    }
    if (recordRelativeOffset) {
      *recordRelativeOffset = offset;
    }
    return true;
  }

  // Visits a typed source once and writes exactly `count` rows into the final
  // record payload. The visitor receives a call-local sink; it never receives
  // a mutable payload span or pointer that could be invalidated by handle
  // retention. Rejection rolls the complete active-record checkpoint back.
  template <typename T, typename Visit>
    requires isWireSafeSectionPayload<T>
  bool appendVisitedSectionPayload(
      std::uint16_t kind, std::size_t count, Visit&& visit,
      std::uint32_t* recordRelativeOffset = nullptr) noexcept {
    const auto* rule = sectionRule(kind);
    const auto* activeRule = active_.active ? recordRule(active_.type) : nullptr;
    if (!rule || !activeRule ||
        (activeRule->ruleFlags & RecordRuleSparseState) == 0u ||
        kind != ApprovedWireSectionPayload<std::remove_cv_t<T>>::kind ||
        count == 0u || count > rule->maxCount ||
        sizeof(T) != rule->elementSize ||
        count > std::numeric_limits<std::uint32_t>::max() ||
        count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
      return failActiveRecord();
    }

    std::uint32_t offset = 0u;
    if (!reservePayload(count * sizeof(T), rule->payloadAlignment, &offset)) {
      return false;
    }
    std::size_t emitted = 0u;
    const auto emit = [&](const T& value) noexcept {
      if (emitted >= count) {
        return false;
      }
      const std::size_t relative =
          static_cast<std::size_t>(offset) + emitted * sizeof(T);
      const std::size_t absolute = active_.payloadStart + relative;
      if (!active_.active || sealed_ || absolute < active_.payloadStart ||
          absolute > payload_.size() ||
          sizeof(T) > payload_.size() - absolute) {
        return false;
      }
      std::memcpy(payload_.data() + absolute, &value, sizeof(T));
      ++emitted;
      return true;
    };
    static_assert(std::is_nothrow_invocable_r_v<
                      bool, Visit&, const decltype(emit)&>,
                  "visited section producers must be noexcept");
    if (!visit(emit) || emitted != count) {
      return failActiveRecord();
    }
    if (recordRelativeOffset) {
      *recordRelativeOffset = offset;
    }
    return true;
  }

  bool appendConstantSectionPayload(
      std::uint16_t kind, std::uint32_t startRegister,
      std::uint32_t registerCount, std::span<const std::byte> registerBytes,
      std::uint32_t* recordRelativeOffset = nullptr) noexcept;
  bool appendUpDataSectionPayload(
      std::uint16_t kind, std::span<const std::byte> bytes,
      std::uint32_t* recordRelativeOffset = nullptr) noexcept;
  bool appendSectionTable(
      std::span<const D9CCommandChunkWireSectionDesc> sections) noexcept;
  bool overwriteSectionTable(
      std::uint32_t recordRelativeOffset,
      std::span<const D9CCommandChunkWireSectionDesc> sections) noexcept;
  bool appendConstantRecordTail(
      std::uint32_t registerCount,
      std::span<const std::byte> registerBytes) noexcept;
  bool appendClearRectTail(std::span<const D9CRect> rects) noexcept;
  bool appendHandle(const PeWireObjectRef& object,
                    std::uint32_t expectedKind,
                    std::uint32_t& absoluteIndex) noexcept;
  bool commitRecord() noexcept;
  void rollbackRecord() noexcept;

  SealedCommandChunk seal() noexcept;
  // Chunk boundary after a successful commit: keeps recently-named wrapper
  // pins warm across the boundary (see D3D9PePendingCommandRetainer).
  void reset() noexcept;
  // Discard: same as reset() but also releases every warm pin. Use at device
  // teardown / Reset / ResetEx.
  void resetAndReleaseRetained() noexcept;

  bool recordActive() const noexcept { return active_.active; }
  bool sealed() const noexcept { return sealed_; }
  std::size_t recordCount() const noexcept { return records_.size(); }
  std::size_t handleCount() const noexcept { return handles_.size(); }
  std::size_t payloadBytes() const noexcept { return payload_.size(); }
  std::size_t retainedObjectCount() const noexcept { return retainer_.size(); }
  // Unique local ordinal of the most recently committed record. This is PE
  // bookkeeping only; it never enters the D9C wire ABI.
  std::uint64_t lastCommittedRecordOrdinal() const noexcept {
    return lastCommittedRecordOrdinal_;
  }
  // The active ordinal is assigned by beginRecord() before emission. Emitters
  // must bind semantic witnesses to this record, not to the prior commit.
  std::uint64_t activeRecordOrdinal() const noexcept {
    return active_.active ? active_.recordOrdinal : 0u;
  }
  bool referencesObject(PeLocalObjectIdentity identity) const noexcept;

  // Issues a non-owning capability only for a handle whose wire identity and
  // local wrapper pointer both match `expected`, and whose handle is below the
  // active record checkpoint. Thus an active-only handle cannot create a
  // logical pending lease; an active+committed identity can still do so from
  // its committed occurrence. The callback is invoked at most once.
  template <typename Visit>
    requires std::is_nothrow_invocable_r_v<
        bool, Visit&, const CommittedPendingChunkLease&>
  bool visitCommittedPendingChunkLease(const PeWireObjectRef& expected,
                                       Visit&& visit) const noexcept {
    if (!expected.object ||
        expected.identity.kind > D9C_CHUNK_HANDLE_KIND_QUERY ||
        expected.identity.generation == 0u ||
        expected.identity.objectId == 0u) {
      return false;
    }
    const std::size_t committedCount =
        active_.active ? active_.handleCheckpoint : handles_.size();
    if (committedCount > handles_.size() ||
        committedCount > handleObjects_.size()) {
      return false;
    }
    for (std::size_t i = 0u; i < committedCount; ++i) {
      const auto& wire = handles_[i];
      const auto& local = handleObjects_[i];
      if (wire.kind != expected.identity.kind ||
          wire.generation != expected.identity.generation ||
          wire.objectId != expected.identity.objectId ||
          local.kind != expected.identity.kind ||
          local.object != expected.object) {
        continue;
      }
      const CommittedPendingChunkLease lease(expected);
      // A matched capability whose consumer rejects settlement is not a
      // successful lease transfer; propagate that result so the caller can
      // take its ordinary unregister/fail path.
      return visit(lease);
    }
    return false;
  }

  // Enumerates the same typed witnesses for drain/reset consumers. Duplicate
  // wire handles are intentionally visited: the logical lifetime state owns
  // the exactly-once guard, while the builder remains the physical pin owner.
  template <typename Visit>
    requires std::is_nothrow_invocable_v<
        Visit&, const CommittedPendingChunkLease&>
  void visitCommittedPendingChunkLeases(Visit&& visit) const noexcept {
    const std::size_t committedCount =
        active_.active ? active_.handleCheckpoint : handles_.size();
    if (committedCount > handles_.size() ||
        committedCount > handleObjects_.size()) {
      return;
    }
    for (std::size_t i = 0u; i < committedCount; ++i) {
      const auto& wire = handles_[i];
      const auto& local = handleObjects_[i];
      if (!local.object || wire.kind > D9C_CHUNK_HANDLE_KIND_QUERY ||
          wire.generation == 0u || wire.objectId == 0u ||
          wire.kind != local.kind) {
        continue;
      }
      visit(CommittedPendingChunkLease(PeWireObjectRef{
          .identity = D9CWireObjectIdentity{
              .kind = wire.kind,
              .generation = wire.generation,
              .objectId = wire.objectId},
          .object = local.object}));
    }
  }

  const std::vector<D9CCommandChunkWireRecordHeader>& recordsForTest()
      const noexcept {
    return records_;
  }
  const std::vector<D9CCommandChunkWireHandleEntry>& handlesForTest()
      const noexcept {
    return handles_;
  }
  const std::vector<std::byte>& payloadForTest() const noexcept {
    return payload_;
  }

 private:
  template <typename T>
  bool readActivePayloadValue(std::uint32_t recordRelativeOffset,
                              T& value) const noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    if (!active_.active || sealed_ ||
        recordRelativeOffset > payload_.size() - active_.payloadStart ||
        sizeof(T) > payload_.size() - active_.payloadStart -
                        recordRelativeOffset) {
      return false;
    }
    std::memcpy(&value,
                payload_.data() + active_.payloadStart + recordRelativeOffset,
                sizeof(T));
    return true;
  }

  bool appendPayload(std::span<const std::byte> bytes,
                     std::uint32_t alignment = 1u,
                     std::uint32_t* recordRelativeOffset = nullptr) noexcept;
  bool reservePayload(std::size_t byteCount, std::uint32_t alignment,
                      std::uint32_t* recordRelativeOffset) noexcept;
  bool overwritePayload(std::uint32_t recordRelativeOffset,
                        std::span<const std::byte> bytes) noexcept;

  struct ActiveRecord {
    bool active = false;
    std::uint32_t type = 0u;
    std::size_t recordCheckpoint = 0u;
    std::size_t handleCheckpoint = 0u;
    std::size_t payloadCheckpoint = 0u;
    std::size_t payloadStart = 0u;
    // Monotonically increasing, assigned once per beginRecord() regardless
    // of whether the record eventually commits or rolls back, and never
    // reused (see RecordLocalDedupTable below). 0 is reserved as "no active
    // record" and is never handed out by beginRecord().
    std::uint64_t recordOrdinal = 0u;
    D3D9PePendingCommandRetainer::Acquired retainedCheckpoint{};
  };

  // R-BACK-43.7: `referencesObject()` used to be a `std::find` over the
  // whole builder-lifetime handle array, called per qualifying buffer Lock —
  // the full-arena O(n) shape this spec's process rule was written to catch.
  // This is a chunk-lifetime (kind, pointer) -> multiplicity accelerator:
  // `handleObjects_` stays the source of truth (an object can be named by more
  // than one handle across different records in the same chunk, so the table
  // stores a count, not a presence bit). On overflow it stops answering and
  // the caller falls back to the original linear scan, which stays correct
  // because every pushed local identity is still appended regardless of table
  // state. `reset()` / `resetAndReleaseRetained()` clear it in full;
  // `rollbackRecord()` decrements counts for exactly the range of handles a
  // failed record added, using the same handleCheckpoint bound the surrounding
  // rollback already computes.
  //
  // `appendHandle` normally uses its separate record-local identity table;
  // this table answers a different question — "does this qualified local
  // identity appear anywhere in the chunk?" — and therefore stores
  // chunk-lifetime multiplicity. If the record-local table overflows, appendHandle falls
  // back to its bounded current-record scan, which remains the correctness
  // fallback for that exceptional path.
  struct HandlePresenceTable {
    struct Slot {
      std::uint32_t kind = 0u;
      void* object = nullptr;
      std::uint32_t count = 0u;
    };

    std::vector<Slot> slots;
    std::size_t occupied = 0u;
    bool overflowed = false;

    void init(std::size_t handleCapacityHint) noexcept {
      std::size_t capacity = 64u;
      const std::size_t target =
          std::max<std::size_t>(handleCapacityHint * 2u, 64u);
      while (capacity < target) {
        capacity <<= 1u;
      }
      slots.assign(capacity, Slot{});
      occupied = 0u;
      overflowed = false;
    }

    // Finds `identity`'s slot, inserting a fresh zero-count slot if absent.
    // Returns nullptr (and sets `overflowed`) once the table has no more
    // room; callers must fall back to a linear scan for the rest of the
    // chunk's lifetime once that happens.
    Slot* findOrInsert(PeLocalObjectIdentity identity) noexcept {
      if (overflowed || slots.empty()) {
        return nullptr;
      }
      const auto mask = slots.size() - 1u;
      auto hash = reinterpret_cast<std::uintptr_t>(identity.object) >> 4u;
      hash ^= static_cast<std::uintptr_t>(identity.kind) * 0x9e3779b9u;
      auto idx = hash & mask;
      for (std::size_t probes = 0; probes < slots.size(); ++probes) {
        Slot& s = slots[idx];
        if (s.object == identity.object && s.kind == identity.kind) {
          return &s;
        }
        if (s.object == nullptr) {
          if (occupied * 4u >= slots.size() * 3u) {
            overflowed = true;
            return nullptr;
          }
          s.kind = identity.kind;
          s.object = identity.object;
          s.count = 0u;
          ++occupied;
          return &s;
        }
        idx = (idx + 1u) & mask;
      }
      overflowed = true;
      return nullptr;
    }

    // Mutable lookup: never inserts, never sets `overflowed`, but returns a
    // writable slot so a caller that already knows `key` is present (such as
    // rollbackRecord() undoing its own earlier increment) can adjust its
    // count without a second, insert-capable probe.
    Slot* find(PeLocalObjectIdentity identity) noexcept {
      if (overflowed || slots.empty()) {
        return nullptr;
      }
      const auto mask = slots.size() - 1u;
      auto hash = reinterpret_cast<std::uintptr_t>(identity.object) >> 4u;
      hash ^= static_cast<std::uintptr_t>(identity.kind) * 0x9e3779b9u;
      auto idx = hash & mask;
      for (std::size_t probes = 0; probes < slots.size(); ++probes) {
        Slot& s = slots[idx];
        if (s.object == identity.object && s.kind == identity.kind) {
          return &s;
        }
        if (s.object == nullptr) {
          return nullptr;
        }
        idx = (idx + 1u) & mask;
      }
      return nullptr;
    }

    // Non-mutating lookup for const query contexts (referencesObject()).
    const Slot* find(PeLocalObjectIdentity identity) const noexcept {
      if (overflowed || slots.empty()) {
        return nullptr;
      }
      const auto mask = slots.size() - 1u;
      auto hash = reinterpret_cast<std::uintptr_t>(identity.object) >> 4u;
      hash ^= static_cast<std::uintptr_t>(identity.kind) * 0x9e3779b9u;
      auto idx = hash & mask;
      for (std::size_t probes = 0; probes < slots.size(); ++probes) {
        const Slot& s = slots[idx];
        if (s.object == identity.object && s.kind == identity.kind) {
          return &s;
        }
        if (s.object == nullptr) {
          return nullptr;
        }
        idx = (idx + 1u) & mask;
      }
      return nullptr;
    }

    void clear() noexcept {
      std::fill(slots.begin(), slots.end(), Slot{});
      occupied = 0u;
      overflowed = false;
    }
  };

  // R-BACK-43.7 — appendHandle()'s *record-local* dedup lookup asks whether
  // the current record already appended this exact wire identity and, on a
  // hit, returns its absolute handle index. The table removes the previous
  // record-window scan from the normal path. On fixed-table overflow,
  // appendHandle falls back to the original bounded current-record scan.
  //
  // This is deliberately a SEPARATE structure from HandlePresenceTable
  // above, not an extension of it, because the two answer different
  // questions with different correctness requirements:
  //   - HandlePresenceTable (referencesObject()) asks "does this qualified
  //     local identity occur anywhere in the chunk" and is keyed by
  //     (kind, pointer), so a same-address wrapper from another kind cannot
  //     satisfy a buffer hazard or pending-destroy query.
  //   - This table must reproduce the original scan's *identity*-keyed
  //     semantics exactly: two different pointers that present the same
  //     generation-qualified identity within one record is a genuine
  //     integrity fault (a stale/duplicate wire-cache entry) that the
  //     original scan detects and fails the record for
  //     (`handleObjects_[i].object != object.object` -> failActiveRecord()). A
  //     pointer-keyed lookup cannot see that fault at all: the second,
  //     differently-pointered append would simply miss on its own pointer
  //     and be treated as a brand-new handle, silently losing the check.
  //     So this table is keyed by the (kind, generation, objectId) identity
  //     tuple, matching what the original loop actually compared first.
  //
  // Record-locality is achieved without a per-record clear or per-record
  // capacity: every slot is stamped with the record ordinal that last wrote
  // it. `ActiveRecord::recordOrdinal` is assigned once per beginRecord(),
  // monotonically increasing and never reused — including for a record that
  // is later rolled back, since it is handed out before the record's outcome
  // is known — so a slot stamped by a rolled-back or already-committed
  // record can never alias a later record's ordinal. A slot whose stamp does
  // not match the *current* record's ordinal is treated as absent for this
  // record's dedup query (a "miss") and is unconditionally overwritten by
  // the new entry in place, exactly like inserting into an empty slot, so
  // `occupied` — and therefore overflow risk — is bounded by the number of
  // *chunk-lifetime distinct identities*, not by records seen. Overflow
  // (fixed capacity, same 3/4 load-factor policy as HandlePresenceTable)
  // permanently falls callers back to the original per-record linear scan
  // for the remainder of the chunk's lifetime — identical fallback policy to
  // HandlePresenceTable, and correct for the same reason: every appended
  // handle is still recorded in `handles_`/`handleObjects_` regardless of
  // this table's state.
  struct RecordLocalDedupTable {
    struct Slot {
      bool used = false;
      std::uint32_t kind = 0u;
      std::uint32_t generation = 0u;
      std::uint64_t objectId = 0u;
      std::uint64_t recordOrdinal = 0u;
      std::uint32_t handleIndex = 0u;
      void* object = nullptr;
    };

    enum class Lookup { kMiss, kHit, kOverflowed };

    std::vector<Slot> slots;
    std::size_t occupied = 0u;
    bool overflowed = false;

    static std::size_t hashIdentity(
        const D9CWireObjectIdentity& identity) noexcept {
      // Plain multiplicative mix over the three identity fields; this table
      // is chunk-local and never persisted, so no cross-process stability
      // requirement applies, only in-memory distribution.
      std::uint64_t h = identity.objectId;
      h ^= static_cast<std::uint64_t>(identity.kind) * 0x9e3779b97f4a7c15ull;
      h ^= static_cast<std::uint64_t>(identity.generation) *
           0xc2b2ae3d27d4eb4full;
      h ^= h >> 33u;
      h *= 0xff51afd7ed558ccdull;
      h ^= h >> 33u;
      return static_cast<std::size_t>(h);
    }

    void init(std::size_t handleCapacityHint) noexcept {
      std::size_t capacity = 64u;
      const std::size_t target =
          std::max<std::size_t>(handleCapacityHint * 2u, 64u);
      while (capacity < target) {
        capacity <<= 1u;
      }
      slots.assign(capacity, Slot{});
      occupied = 0u;
      overflowed = false;
    }

    void clear() noexcept {
      std::fill(slots.begin(), slots.end(), Slot{});
      occupied = 0u;
      overflowed = false;
    }

    // Looks up `identity` scoped to `recordOrdinal`.
    //  - kOverflowed: table has no room to answer; caller must fall back to
    //    the original linear scan (and keeps doing so for the rest of the
    //    chunk's lifetime, mirroring HandlePresenceTable's policy).
    //  - kHit: this exact identity was already appended by the record with
    //    ordinal `recordOrdinal`; `*outIndex`/`*outObject` are the stored
    //    handle index and object pointer from that append.
    //  - kMiss: no live entry for this record. `*insertAt` names the slot a
    //    fresh insert belongs in (either genuinely empty, or holding a
    //    stale-ordinal entry for the same identity that is safe to
    //    overwrite in place).
    Lookup findForRecord(const D9CWireObjectIdentity& identity,
                         std::uint64_t recordOrdinal, Slot** insertAt,
                         std::uint32_t* outIndex, void** outObject) noexcept {
      if (insertAt) {
        *insertAt = nullptr;
      }
      if (overflowed || slots.empty()) {
        return Lookup::kOverflowed;
      }
      const auto mask = slots.size() - 1u;
      auto idx = hashIdentity(identity) & mask;
      for (std::size_t probes = 0; probes < slots.size(); ++probes) {
        Slot& s = slots[idx];
        if (!s.used) {
          if (occupied * 4u >= slots.size() * 3u) {
            overflowed = true;
            return Lookup::kOverflowed;
          }
          if (insertAt) {
            *insertAt = &s;
          }
          return Lookup::kMiss;
        }
        if (s.kind == identity.kind && s.generation == identity.generation &&
            s.objectId == identity.objectId) {
          if (s.recordOrdinal == recordOrdinal) {
            if (outIndex) {
              *outIndex = s.handleIndex;
            }
            if (outObject) {
              *outObject = s.object;
            }
            return Lookup::kHit;
          }
          // Same identity, stamped by an earlier (committed or
          // rolled-back) record: stale for this record's query. Reuse the
          // slot in place rather than probing further, so `occupied` does
          // not grow on repeat identities across records.
          if (insertAt) {
            *insertAt = &s;
          }
          return Lookup::kMiss;
        }
        idx = (idx + 1u) & mask;
      }
      overflowed = true;
      return Lookup::kOverflowed;
    }

    void insert(Slot& slot, const D9CWireObjectIdentity& identity,
               std::uint64_t recordOrdinal, std::uint32_t handleIndex,
               void* object) noexcept {
      const bool wasUsed = slot.used;
      slot.used = true;
      slot.kind = identity.kind;
      slot.generation = identity.generation;
      slot.objectId = identity.objectId;
      slot.recordOrdinal = recordOrdinal;
      slot.handleIndex = handleIndex;
      slot.object = object;
      if (!wasUsed) {
        ++occupied;
      }
    }
  };

  bool failActiveRecord() noexcept;
  bool appendNewHandleEntry(const PeWireObjectRef& object,
                            std::uint32_t& absoluteIndex) noexcept;

  std::vector<D9CCommandChunkWireRecordHeader> records_;
  std::vector<D9CCommandChunkWireHandleEntry> handles_;
  std::vector<PeLocalObjectIdentity> handleObjects_;
  std::vector<std::byte> payload_;
  std::vector<std::byte> sealedBlob_;
  D3D9PePendingCommandRetainer retainer_;
  HandlePresenceTable handlePresence_;
  RecordLocalDedupTable recordLocalDedup_;
  ActiveRecord active_{};
  // Never reused across the builder's whole lifetime (spans many chunks via
  // reset()/resetAndReleaseRetained()), so a RecordLocalDedupTable slot's
  // stamp can never alias a later record. Starts at 1 so 0 stays a safe
  // "no record" sentinel, though nothing currently relies on that.
  std::uint64_t nextRecordOrdinal_ = 1u;
  std::uint64_t lastCommittedRecordOrdinal_ = 0u;
  bool sealed_ = false;
};

template <typename Wire>
struct SparseBindingInput {
  static_assert(std::is_trivially_copyable_v<Wire>);

  Wire wire{};
  PeWireObjectRef object{};
};

struct SparseConstantRangeInput {
  std::uint32_t startRegister = 0u;
  std::uint32_t registerCount = 0u;
  std::span<const std::byte> registerBytes{};

  bool present() const noexcept { return registerCount != 0u; }
};

struct SparseStateInput {
  // This is producer metadata, not wire data.  It carries the effective
  // disposition from state preparation through append and settlement so a
  // full snapshot may project clean LiveShadow rows without inventing
  // PendingDelta tokens.
  bool fullSnapshot = false;
  std::span<const D9CCommandChunkWireRenderState> renderStates{};
  std::span<const SparseBindingInput<
      D9CCommandChunkWireTextureBinding>> textures{};
  std::span<const SparseBindingInput<
      D9CCommandChunkWireStreamBinding>> streams{};
  std::span<const SparseBindingInput<
      D9CCommandChunkWireShaderBinding>> shaders{};
  std::span<const SparseBindingInput<
      D9CCommandChunkWireVertexInput>> vertexInputs{};
  std::span<const SparseBindingInput<
      D9CCommandChunkWireIndexBinding>> indexBuffers{};
  std::span<const SparseBindingInput<
      D9CCommandChunkWireRenderTargetBinding>> renderTargets{};
  std::span<const SparseBindingInput<
      D9CCommandChunkWireDepthStencilBinding>> depthStencils{};
  std::span<const D9CViewport> viewports{};
  std::span<const D9CRect> scissors{};
  std::span<const D9CMaterial> materials{};
  std::span<const D9CCommandChunkWireClipPlane> clipPlanes{};
  std::span<const D9CDrawPacketTextureStageState> textureStageStates{};
  std::span<const D9CDrawPacketSamplerState> samplerStates{};
  std::span<const D9CDrawPacketTransform> transforms{};
  std::span<const D9CCommandChunkWireLight> lights{};
  std::span<const D9CCommandChunkWireLightEnable> lightEnables{};
  SparseConstantRangeInput vsFloatConstants{};
  SparseConstantRangeInput vsIntConstants{};
  SparseConstantRangeInput vsBoolConstants{};
  SparseConstantRangeInput psFloatConstants{};
  SparseConstantRangeInput psIntConstants{};
  SparseConstantRangeInput psBoolConstants{};
  std::span<const std::byte> upIndexData{};
  std::span<const std::byte> upVertexData{};
};

bool appendSparseRecord(CommandChunkBuilder& builder,
                          std::uint32_t type,
                          D9CCommandChunkWireDrawHeader draw,
                          const SparseStateInput& state) noexcept;
bool appendApplyState(CommandChunkBuilder& builder,
                        std::uint32_t flags,
                        const SparseStateInput& state) noexcept;

bool appendSetConstants(
    CommandChunkBuilder& builder, std::uint32_t type,
    std::uint32_t startRegister, std::uint32_t registerCount,
    std::span<const std::byte> registerBytes) noexcept;
bool appendClear(CommandChunkBuilder& builder,
                   D9CCommandChunkWireClear fixed,
                   std::span<const D9CRect> rects) noexcept;
bool appendPresent(CommandChunkBuilder& builder,
                     D9CCommandChunkWirePresent fixed,
                     const SurfaceRef& source) noexcept;
bool appendStretchRect(CommandChunkBuilder& builder,
                         D9CCommandChunkWireStretchRect fixed,
                         const SurfaceRef& src,
                         const SurfaceRef& dst) noexcept;
bool appendColorFill(CommandChunkBuilder& builder,
                       D9CCommandChunkWireColorFill fixed,
                       const SurfaceRef& surface) noexcept;
bool appendUpdateTexture(CommandChunkBuilder& builder,
                           const TextureRef& src,
                           const TextureRef& dst) noexcept;
bool appendUpdateSurface(CommandChunkBuilder& builder,
                           D9CCommandChunkWireUpdateSurface fixed,
                           const SurfaceRef& src,
                           const SurfaceRef& dst) noexcept;
bool appendQueryIssue(CommandChunkBuilder& builder,
                        std::uint32_t flags,
                        const QueryRef& query) noexcept;
bool appendReadback(CommandChunkBuilder& builder,
                      const SurfaceRef& src,
                      const SurfaceRef& dst) noexcept;
bool appendReszDepthResolve(CommandChunkBuilder& builder,
                            const SurfaceRef& msaaDepth,
                            const TextureRef& intzDest) noexcept;
bool appendGenerateMipmaps(CommandChunkBuilder& builder,
                           const TextureRef& texture) noexcept;


}  // namespace dxmt9::d3d9::pe
