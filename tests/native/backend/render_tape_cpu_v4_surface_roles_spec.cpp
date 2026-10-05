#include "render_tape_cpu_v4_surface_roles.hpp"
#include "render_tape_completion_fixture.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace dxmt9;
using namespace dxmt9::test;
using Status = CpuTapeV4SurfaceRoleStatus;
using Role = CpuTapeResourceRoleKind;
using Version = std::shared_ptr<const CpuTapeResourceVersion>;
using Context = std::shared_ptr<const CpuTapeResourceContext>;
constexpr auto nullIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
constexpr D9CWireObjectIdentity first{D9C_CHUNK_HANDLE_KIND_SURFACE, 3u, 101u};
constexpr D9CWireObjectIdentity second{D9C_CHUNK_HANDLE_KIND_SURFACE, 7u, 102u};
constexpr D9CWireObjectIdentity depth{D9C_CHUNK_HANDLE_KIND_SURFACE, 5u, 103u};
constexpr D9CWireObjectIdentity parent{D9C_CHUNK_HANDLE_KIND_TEXTURE, 2u, 104u};
std::size_t checks = 0u;

void check(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}

bool identityIs(const CpuTapeV4SurfaceRoleBinding& value, D9CWireObjectIdentity identity) {
  return value.known && value.identity.kind == identity.kind &&
      value.identity.generation == identity.generation && value.identity.objectId == identity.objectId;
}

D9CCommandChunkWireHandleEntry wire(D9CWireObjectIdentity identity) {
  return {.kind = identity.kind, .generation = identity.generation, .objectId = identity.objectId};
}

struct Section {
  std::uint16_t kind = 0u, elementSize = 0u;
  std::uint32_t count = 0u;
  std::vector<std::byte> bytes{};
};

template <typename T>
Section section(std::uint16_t kind, std::span<const T> values) {
  const auto bytes = std::as_bytes(values);
  return {.kind = kind, .elementSize = sizeof(T), .count = static_cast<std::uint32_t>(values.size()),
      .bytes = {bytes.begin(), bytes.end()}};
}

std::vector<std::byte> sparsePayload(std::uint32_t type,
    std::span<const D9CCommandChunkWireRenderTargetBinding> targets,
    std::optional<D9CCommandChunkWireDepthStencilBinding> ds = std::nullopt,
    bool full = false) {
  std::vector<Section> sections;
  if (full) {
    std::array<D9CCommandChunkWireTextureBinding, D9C_DRAW_PACKET_MAX_TEXTURES> textures{};
    for (std::uint32_t slot = 0u; slot < textures.size(); ++slot)
      textures[slot] = {.slot = slot, .valid = 1u, .handleIndex = nullIndex};
    std::array<D9CCommandChunkWireStreamBinding, D9C_DRAW_PACKET_MAX_STREAMS> streams{};
    for (std::uint32_t slot = 0u; slot < streams.size(); ++slot)
      streams[slot] = {.slot = slot, .valid = 1u, .handleIndex = nullIndex};
    sections.push_back(section<D9CCommandChunkWireTextureBinding>(D9C_COMMAND_CHUNK_SECTION_TEXTURE, textures));
    sections.push_back(section<D9CCommandChunkWireStreamBinding>(D9C_COMMAND_CHUNK_SECTION_STREAM, streams));
  }
  if (!targets.empty()) sections.push_back(section<D9CCommandChunkWireRenderTargetBinding>(
      D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET, targets));
  if (ds) sections.push_back(section<D9CCommandChunkWireDepthStencilBinding>(
      D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL, std::span(&*ds, 1u)));
  const auto table = sizeof(D9CCommandChunkWireDrawHeader);
  const auto start = d3d9::test::alignUp(table + sections.size() * sizeof(D9CCommandChunkWireSectionDesc), 4u);
  D9CCommandChunkWireDrawHeader header{
      .flags = full ? D9C_COMMAND_CHUNK_DRAW_FLAG_FULL_SNAPSHOT : 0u,
      .primitiveType = type == D9C_COMMAND_RECORD_APPLY_STATE ? 0u : 4u,
      .primitiveCount = type == D9C_COMMAND_RECORD_APPLY_STATE ? 0u : 1u,
      .sectionCount = static_cast<std::uint32_t>(sections.size()),
      .sectionTableOffset = static_cast<std::uint32_t>(table),
      .sectionPayloadOffset = static_cast<std::uint32_t>(start)};
  std::vector<std::byte> payload(start);
  std::memcpy(payload.data(), &header, sizeof(header));
  for (std::size_t index = 0u; index < sections.size(); ++index) {
    const auto& value = sections[index];
    const D9CCommandChunkWireSectionDesc descriptor{.kind = value.kind,
        .elementSize = value.elementSize, .count = value.count,
        .payloadOffset = static_cast<std::uint32_t>(payload.size()),
        .byteSize = static_cast<std::uint32_t>(value.bytes.size())};
    std::memcpy(payload.data() + table + index * sizeof(descriptor), &descriptor, sizeof(descriptor));
    payload.insert(payload.end(), value.bytes.begin(), value.bytes.end());
  }
  return payload;
}

struct AdmittedRecord {
  std::vector<std::byte> bytes;
  d3d9::ImportedChunkView imported;

  AdmittedRecord(std::uint32_t type, std::span<const std::byte> payload,
      std::span<const D9CCommandChunkWireHandleEntry> handles = {})
      : bytes(d3d9::test::singleRecordChunk(type, payload, handles)) {
    check(d3d9::validateCommandChunk(bytes, {.recordCount = 1u,
        .handleCount = static_cast<std::uint32_t>(handles.size())}, &imported).valid(),
        "canonical fixture passes full chunk validation before the role-only fold");
  }
  d3d9::ImportedRecordView record() const { return imported.record(0u); }
};

CpuTapeV4SurfaceRoles fold(CpuTapeV4SurfaceRoleFold& reducer, const AdmittedRecord& record) {
  CpuTapeV4SurfaceRoles out;
  check(reducer.fold(record.record(), out) == Status::Accepted, "admitted original roles fold");
  return out;
}

AdmittedRecord baseline() {
  const std::array targets{
      D9CCommandChunkWireRenderTargetBinding{.slot = 0u, .valid = 1u, .handleIndex = 0u},
      D9CCommandChunkWireRenderTargetBinding{.slot = 1u, .valid = 1u, .handleIndex = nullIndex},
      D9CCommandChunkWireRenderTargetBinding{.slot = 2u, .valid = 1u, .handleIndex = nullIndex},
      D9CCommandChunkWireRenderTargetBinding{.slot = 3u, .valid = 1u, .handleIndex = nullIndex}};
  const std::array handles{wire(first), wire(depth)};
  return {D9C_COMMAND_RECORD_APPLY_STATE, sparsePayload(D9C_COMMAND_RECORD_APPLY_STATE,
      targets, D9CCommandChunkWireDepthStencilBinding{.valid = 1u, .handleIndex = 1u}, true), handles};
}

AdmittedRecord draw(std::optional<D9CCommandChunkWireRenderTargetBinding> target = std::nullopt,
                   std::span<const D9CCommandChunkWireHandleEntry> handles = {}) {
  const auto values = target ? std::span(&*target, 1u)
                            : std::span<const D9CCommandChunkWireRenderTargetBinding>{};
  return {D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
      sparsePayload(D9C_COMMAND_RECORD_DRAW_PRIMITIVE, values), handles};
}

AdmittedRecord clear() {
  const D9CCommandChunkWireClear fixed{.flags = 1u, .colorARGB = 0xff001122u,
      .rectOffset = sizeof(D9CCommandChunkWireClear)};
  return {D9C_COMMAND_RECORD_CLEAR, std::as_bytes(std::span(&fixed, 1u))};
}

void canonicalFoldOwnershipAndNulls() {
  CpuTapeV4SurfaceRoleFold reducer;
  auto bootstrap = baseline();
  const auto initial = fold(reducer, bootstrap);
  check(identityIs(initial.renderTargets[0], first) && identityIs(initial.depthStencil, depth),
      "FULL_SNAPSHOT preserves exact generation-qualified RT0 and DS identities");
  for (std::size_t slot = 1u; slot < initial.renderTargets.size(); ++slot)
    check(initial.renderTargets[slot].null(), "explicit null RT slots are known, not absent");
  const auto firstDraw = fold(reducer, draw());
  check(identityIs(firstDraw.renderTargets[0], first), "draw without deltas preserves bootstrap RT0");
  const std::array handles{wire(second)};
  const auto changedDraw = fold(reducer, draw(D9CCommandChunkWireRenderTargetBinding{
      .slot = 0u, .valid = 1u, .handleIndex = 0u}, handles));
  check(identityIs(changedDraw.renderTargets[0], second) &&
      identityIs(firstDraw.renderTargets[0], first), "draw deltas apply before draw while earlier snapshot remains owned");
  const auto noOp = fold(reducer, draw(D9CCommandChunkWireRenderTargetBinding{
      .slot = 0u, .valid = 0u, .handleIndex = nullIndex}));
  check(identityIs(noOp.renderTargets[0], second), "valid=0 is a production no-op, not an explicit null");
  const AdmittedRecord fullWithoutRoles{D9C_COMMAND_RECORD_APPLY_STATE,
      sparsePayload(D9C_COMMAND_RECORD_APPLY_STATE, {}, std::nullopt, true)};
  fold(reducer, fullWithoutRoles);
  check(identityIs(fold(reducer, clear()).renderTargets[0], second),
      "FULL_SNAPSHOT without surface sections preserves prior roles for subsequent Clear");
  const auto removed = fold(reducer, draw(D9CCommandChunkWireRenderTargetBinding{
      .slot = 0u, .valid = 1u, .handleIndex = nullIndex}));
  check(removed.renderTargets[0].null(), "valid=1 plus null handle explicitly clears the role");
  bootstrap.bytes.assign(bootstrap.bytes.size(), std::byte{0xff});
  check(identityIs(initial.renderTargets[0], first) && identityIs(initial.depthStencil, depth),
      "frozen role identities do not retain imported record or handle spans");
}

void presentSourceIsRecordLocal() {
  CpuTapeV4SurfaceRoleFold reducer;
  fold(reducer, baseline());
  const D9CCommandChunkWirePresent fixed{.sourceHandleIndex = 0u};
  const std::array handles{wire(second)};
  const AdmittedRecord present{D9C_COMMAND_RECORD_PRESENT, std::as_bytes(std::span(&fixed, 1u)), handles};
  const auto snapshot = fold(reducer, present);
  check(identityIs(snapshot.presentSource, second) && identityIs(snapshot.renderTargets[0], first),
      "Present source is its explicit wire identity, not inferred from RT0");
  check(!fold(reducer, draw()).presentSource.known && !reducer.current().presentSource.known,
      "Present source does not leak into another original record");
  const AdmittedRecord implicit{D9C_COMMAND_RECORD_PRESENT, std::as_bytes(std::span(&fixed, 1u))};
  const auto before = reducer.current();
  auto preserved = snapshot;
  check(reducer.fold(implicit.record(), preserved) == Status::UnsupportedPresent &&
      reducer.current() == before && preserved == snapshot,
      "legacy handle-free Present rejects without inventing a source or changing output");
}

struct AliasContexts {
  std::shared_ptr<CpuTapeExpectedBufferBudget> budget;
  Version firstVersion, secondVersion, depthVersion;
  Context original;

  AliasContexts() {
    check(createCpuTapeExpectedBufferBudget({16u}, budget) == CpuTapeResourceContextStatus::Accepted,
        "explicit session owner is available for immutable versions");
    firstVersion = version(first, {801u}, 1u);
    secondVersion = version(second, {801u}, 9u);
    depthVersion = version(depth, {802u}, 2u);
    const std::array entries{firstVersion, secondVersion, depthVersion};
    check(buildCpuTapeV4SurfaceAliasInventory(entries, budget, original) == CpuTapeResourceContextStatus::Accepted,
        "inventory independently retains both wire aliases sharing one native surface");
  }

  Version version(D9CWireObjectIdentity identity, core::Handle native, std::uint64_t epoch) {
    const std::array receipts{CpuTapeResourceReceipt{.identity = parent, .subresource = 0u, .revision = epoch}};
    CpuTapeResourceObservation observation;
    check(buildCpuTapeResourceObservation({.identity = identity, .contentIdentity = parent,
        .aliasSubresource = 0u, .receipts = receipts}, observation) == CpuTapeResourceObservationStatus::Accepted,
        "exact alias observation owns parent-subresource receipt");
    Version out;
    check(buildBudgetedCpuTapeResourceVersion({.nativeKind = identity.kind, .nativeHandle = native,
        .contentEpoch = epoch}, observation, budget, out) == CpuTapeResourceContextStatus::Accepted,
        "alias resource version is immutable and session-associated");
    return out;
  }
};

void exactAliasRoleResolution() {
  AliasContexts resources;
  CpuTapeV4SurfaceRoleFold reducer;
  fold(reducer, baseline());
  const auto earlier = fold(reducer, draw());
  const std::array handles{wire(second)};
  const auto later = fold(reducer, draw(D9CCommandChunkWireRenderTargetBinding{
      .slot = 0u, .valid = 1u, .handleIndex = 0u}, handles));
  check(!resources.original->find(D9C_CHUNK_HANDLE_KIND_SURFACE, {801u}) &&
      !resources.original->hasSemanticUniqueNativeAuthority(), "native alias ambiguity remains intentional");
  Version selected;
  check(resolveCpuTapeV4SurfaceRole(earlier, *resources.original, {Role::RenderTarget, 0u},
      {801u}, selected) == Status::Accepted && selected == resources.firstVersion,
      "original RT role selects first full wire identity without first-native-match lookup");
  auto firstHash = std::uint64_t{1469598103934665603ull};
  check(appendCpuTapeResourceObservation(selected->observation(), firstHash), "first alias logical receipt hashes");
  check(resolveCpuTapeV4SurfaceRole(later, *resources.original, {Role::RenderTarget, 0u},
      {801u}, selected) == Status::Accepted && selected == resources.secondVersion,
      "same native storage resolves the other exact original alias at another draw");
  auto secondHash = std::uint64_t{1469598103934665603ull};
  check(appendCpuTapeResourceObservation(selected->observation(), secondHash) && firstHash != secondHash,
      "logical projection preserves exact wire identity/receipt rather than hashing an indistinguishable alias class");
  check(resolveCpuTapeV4SurfaceRole(earlier, *resources.original, {Role::DepthStencil, 0u},
      {802u}, selected) == Status::Accepted && selected == resources.depthVersion,
      "DS resolver uses original dedicated role");
  check(resolveCpuTapeV4SurfaceRole(earlier, *resources.original, {Role::RenderTarget, 1u},
      {}, selected) == Status::Accepted && !selected, "known-null role validates actual null and clears result");
  const D9CCommandChunkWirePresent presentFixed{.sourceHandleIndex = 0u};
  const AdmittedRecord present{D9C_COMMAND_RECORD_PRESENT,
      std::as_bytes(std::span(&presentFixed, 1u)), handles};
  const auto presentRoles = fold(reducer, present);
  check(resolveCpuTapeV4SurfaceRole(presentRoles, *resources.original, {Role::PresentSource, 0u},
      {801u}, selected) == Status::Accepted && selected == resources.secondVersion,
      "Present source resolves its exact original alias independently of attachment roles");
  selected = resources.firstVersion;
  const auto reject = [&](CpuTapeV4SurfaceRoles roles, CpuTapeResourceRole role,
                          core::Handle actual, Status expected) {
    check(resolveCpuTapeV4SurfaceRole(roles, *resources.original, role, actual, selected) == expected &&
        selected == resources.firstVersion, "rejected role/native association preserves prior result");
  };
  reject(earlier, {Role::General, 0u}, {801u}, Status::InvalidRole);
  reject(earlier, {Role::RenderTarget, 4u}, {801u}, Status::InvalidRole);
  reject(earlier, {Role::DepthStencil, 1u}, {802u}, Status::InvalidRole);
  reject(earlier, {Role::PresentSource, 0u}, {801u}, Status::RoleNotApplicable);
  reject(presentRoles, {Role::PresentSource, 1u}, {801u}, Status::InvalidRole);
  reject(presentRoles, {Role::PresentSource, 0u}, {}, Status::NativeMismatch);
  reject(earlier, {Role::RenderTarget, 0u}, {802u}, Status::NativeMismatch);
  reject(earlier, {Role::RenderTarget, 0u}, {}, Status::NativeMismatch);
  reject(earlier, {Role::RenderTarget, 1u}, {801u}, Status::NativeMismatch);
  auto missing = earlier;
  ++missing.renderTargets[0].identity.generation;
  reject(missing, {Role::RenderTarget, 0u}, {801u}, Status::MissingVersion);
  auto unknown = earlier;
  unknown.renderTargets[1] = {};
  reject(unknown, {Role::RenderTarget, 1u}, {}, Status::UnknownRole);
  auto wrongRecord = earlier;
  wrongRecord.recordType = D9C_COMMAND_RECORD_APPLY_STATE;
  reject(wrongRecord, {Role::RenderTarget, 0u}, {801u}, Status::RoleNotApplicable);
  const auto replacement = resources.version(first, {901u}, 2u);
  Context current;
  const std::array next{replacement, resources.secondVersion, resources.depthVersion};
  check(buildCpuTapeV4SurfaceAliasInventory(next, resources.budget, current) == CpuTapeResourceContextStatus::Accepted,
      "later immutable context can hold a replacement native/version association");
  check(resolveCpuTapeV4SurfaceRole(earlier, *resources.original, {Role::RenderTarget, 0u},
      {801u}, selected) == Status::Accepted && selected == resources.firstVersion &&
      selected->contentEpoch() == 1u, "original context, not later current ledger, controls historical version");
  check(resolveCpuTapeV4SurfaceRole(earlier, *current, {Role::RenderTarget, 0u},
      {801u}, selected) == Status::NativeMismatch && selected == resources.firstVersion,
      "substituting later context fails native association instead of silently rebinding history");
}

void boundedDecodeAndTransactionalRejection() {
  CpuTapeV4SurfaceRoleFold reducer;
  const auto initial = fold(reducer, baseline());
  const auto before = reducer.current();
  auto preserved = initial;
  const auto unchangedDraw = draw();
  auto record = unchangedDraw.record();
  // The following inputs deliberately corrupt copied view metadata. They are
  // bounded decoder negatives, not claims of authenticated V4 provenance.
  record.header.handleCount = 1u;
  check(reducer.fold(record, preserved) == Status::InvalidRecord &&
      reducer.current() == before && preserved == initial, "invalid handle span preserves fold/output");
  const std::array handles{wire(second)};
  const auto update = draw(D9CCommandChunkWireRenderTargetBinding{
      .slot = 0u, .valid = 1u, .handleIndex = 0u}, handles);
  record = update.record();
  record.header.firstHandle = 1u;
  check(reducer.fold(record, preserved) == Status::InvalidHandle &&
      reducer.current() == before && preserved == initial,
      "absolute wire index cannot escape before a record-local handle slice");
  auto payload = sparsePayload(D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
      std::array{D9CCommandChunkWireRenderTargetBinding{.slot = 0u, .valid = 1u, .handleIndex = 1u}});
  std::array<D9CCommandChunkWireSectionDesc, 1u> sections;
  D9CCommandChunkWireDrawHeader drawHeader;
  std::memcpy(&drawHeader, payload.data(), sizeof(drawHeader));
  std::memcpy(sections.data(), payload.data() + drawHeader.sectionTableOffset, sizeof(sections));
  record = {.header = {.type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
      .payloadSize = static_cast<std::uint32_t>(payload.size()), .firstHandle = 1u, .handleCount = 1u},
      .payload = payload, .handles = handles, .drawHeader = drawHeader, .sections = sections};
  check(reducer.fold(record, preserved) == Status::Accepted && identityIs(preserved.renderTargets[0], second),
      "table-absolute handle index subtracts firstHandle before indexing local slice");
  const auto accepted = preserved;
  const auto state = reducer.current();
  sections[0].payloadOffset = static_cast<std::uint32_t>(payload.size() + 1u);
  check(reducer.fold(record, preserved) == Status::InvalidSection &&
      preserved == accepted && reducer.current() == state,
      "out-of-range section rejects before reading bytes and preserves accepted state");
  const auto bootstrap = baseline();
  auto malformed = bootstrap.record();
  std::vector<D9CCommandChunkWireSectionDesc> descriptors(malformed.sections.begin(), malformed.sections.end());
  malformed.sections = descriptors;
  --descriptors.back().elementSize;
  check(reducer.fold(malformed, preserved) == Status::InvalidSection &&
      preserved == accepted && reducer.current() == state,
      "later malformed DS section cannot commit the earlier RT update from the same record");
  descriptors.back().elementSize = sizeof(D9CCommandChunkWireDepthStencilBinding);
  auto duplicatedPayload = std::vector<std::byte>(malformed.payload.begin(), malformed.payload.end());
  malformed.payload = duplicatedPayload;
  const auto rtOffset = descriptors[descriptors.size() - 2u].payloadOffset;
  D9CCommandChunkWireRenderTargetBinding duplicate;
  std::memcpy(&duplicate, duplicatedPayload.data() + rtOffset + sizeof(duplicate), sizeof(duplicate));
  duplicate.slot = 0u;
  std::memcpy(duplicatedPayload.data() + rtOffset + sizeof(duplicate), &duplicate, sizeof(duplicate));
  check(reducer.fold(malformed, preserved) == Status::DuplicateBinding &&
      preserved == accepted && reducer.current() == state,
      "duplicate canonical role slots cannot select one ambiguous binding or partially change state");
  auto wrongHandles = std::array{wire(parent), wire(depth)};
  malformed = bootstrap.record();
  malformed.handles = wrongHandles;
  check(reducer.fold(malformed, preserved) == Status::InvalidHandle &&
      preserved == accepted && reducer.current() == state,
      "a texture wire handle cannot authorize a surface role despite sharing identity fields");
  CpuTapeV4SurfaceRoleFold empty;
  const auto unknown = fold(empty, draw());
  check(!unknown.renderTargets[0].known && !unknown.depthStencil.known,
      "absent bootstrap/default authority never becomes a guessed null binding");
}

void explicitlyQualifiedFreshDefaults() {
  static_assert(!std::is_copy_assignable_v<CpuTapeV4SurfaceRoleFold>);
  static_assert(!std::is_move_assignable_v<CpuTapeV4SurfaceRoleFold>);
  CpuTapeV4SurfaceRoleFold plain;
  for (const auto& binding : plain.current().renderTargets)
    check(!binding.known, "default fold retains unknown surface role authority");
  check(!plain.current().depthStencil.known, "default fold does not assume auto-depth configuration");
  CpuTapeV4SurfaceRoleFold fresh;
  check(fresh.seedFreshNoAutoDepthDefaults() == Status::Accepted,
      "caller explicitly selects the fresh no-auto-depth baseline");
  check(!fresh.current().renderTargets[0].known,
      "fresh no-auto-depth baseline cannot identify the default swapchain RT0");
  for (std::size_t slot = 1u; slot < fresh.current().renderTargets.size(); ++slot)
    check(fresh.current().renderTargets[slot].null(), "fresh baseline qualifies only unused MRT nulls");
  check(fresh.current().depthStencil.null(), "fresh no-auto-depth baseline qualifies null DS");
  const auto seeded = fresh.current();
  check(fresh.seedFreshNoAutoDepthDefaults() == Status::InvalidBaseline && fresh.current() == seeded,
      "fresh baseline seeding is once-only and does not reset existing authority");
  fold(fresh, baseline());
  const auto canonical = fold(fresh, draw());
  check(identityIs(canonical.renderTargets[0], first) && identityIs(canonical.depthStencil, depth),
      "canonical explicit bindings replace permitted fresh baseline nulls");
  const auto before = fresh.current();
  check(fresh.seedFreshNoAutoDepthDefaults() == Status::InvalidBaseline && fresh.current() == before,
      "reseeding after canonical work cannot erase DS/RT history");
  fold(plain, draw());
  const auto used = plain.current();
  check(plain.seedFreshNoAutoDepthDefaults() == Status::InvalidBaseline && plain.current() == used,
      "first seed after original records have folded rejects rather than retroactively supplying defaults");
  CpuTapeV4SurfaceRoleFold supplied(seeded);
  check(supplied.seedFreshNoAutoDepthDefaults() == Status::InvalidBaseline,
      "caller-supplied initial baseline cannot subsequently be reset by fresh-default seeding");
}

} // namespace

int main() {
  try {
    canonicalFoldOwnershipAndNulls();
    presentSourceIsRecordLocal();
    exactAliasRoleResolution();
    boundedDecodeAndTransactionalRejection();
    explicitlyQualifiedFreshDefaults();
    std::cout << "render-tape-cpu-v4-surface-roles-spec: " << checks << " checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "render-tape-cpu-v4-surface-roles-spec: " << error.what() << '\n';
    return 1;
  }
}
