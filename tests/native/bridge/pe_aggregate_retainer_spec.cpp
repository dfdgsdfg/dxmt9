#include "d3d9_pe_semantic_owner.hpp"
#include "d3d9_pe_producer.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

struct RefCounter { unsigned refs = 1u; };
struct D9CSurface : RefCounter {};
struct D9CTexture : RefCounter {};
struct D9CBuffer : RefCounter {};
struct D9CShader : RefCounter {};
struct D9CVertexDecl : RefCounter {};
struct D9CQuery : RefCounter {};
static unsigned acquisitions = 0u;
#define REF_STUBS(name, Type) \
  extern "C" void dxmt9c_##name##_addref(Type* p) { ++p->refs; ++acquisitions; } \
  extern "C" std::uint32_t dxmt9c_##name##_release(Type* p) { return --p->refs; }
REF_STUBS(surface, D9CSurface)
REF_STUBS(texture, D9CTexture)
REF_STUBS(buffer, D9CBuffer)
REF_STUBS(shader, D9CShader)
REF_STUBS(vdecl, D9CVertexDecl)
REF_STUBS(query, D9CQuery)
#undef REF_STUBS

namespace {
using namespace dxmt9::d3d9::pe;
using Owner = PeSemanticBatchOwner<16u, 1u, 4096u, 4u, 16u>;
using Outcome = PeSemanticAdmissionOutcome;

template <std::uint32_t Kind, typename T>
PeLocalObjectRef<Kind> ref(T& object, std::uint32_t id) {
  PeLocalObjectRef<Kind> result{};
  result.identity = {.kind = Kind, .generation = 1u, .objectId = id};
  result.object = &object;
  return result;
}

PeSemanticRecordInput draw(std::uint64_t ordinal) {
  const auto kind = PeSemanticProducerKind::DrawPrimitive;
  return {.producer = kind,
          .recordType = kPeSemanticProducerPolicyTable[
              static_cast<std::size_t>(kind)].recordType,
          .sourceOrdinal = ordinal, .recordOrdinal = ordinal};
}

bool check(bool ok, const char* why) {
  if (!ok) std::cerr << "pe_aggregate_retainer_spec: " << why << '\n';
  return ok;
}

// R-CORE-REC-7.30: four independently fitting pin families do not imply that
// the aggregate retainer (three entries here) fits. This failed late in the
// old pin acquisition loop rather than in non-mutating CapacityPre.
bool mixedKindCapacityPre() {
  D9CSurface surface;
  D9CTexture texture;
  D9CBuffer buffer;
  D9CShader shader;
  Owner owner;
  auto input = draw(1u);
  input.surface0 = ref<D9C_CHUNK_HANDLE_KIND_SURFACE>(surface, 1u);
  input.texture0 = ref<D9C_CHUNK_HANDLE_KIND_TEXTURE>(texture, 2u);
  input.buffer0 = ref<D9C_CHUNK_HANDLE_KIND_BUFFER>(buffer, 3u);
  input.shader0 = ref<D9C_CHUNK_HANDLE_KIND_SHADER>(shader, 4u);
  PeSemanticPreparedRecord prepared{};
  const auto before = acquisitions;
  bool settled = false;
  return check(owner.prepareAdmission(input, prepared) == Outcome::Capacity,
               "aggregate overflow must reject during pure preparation") &&
         check(!owner.tryAppendOwnedRecord(input, [&]() noexcept {
                 settled = true;
                 return true;
               }) && owner.lastAdmissionFailure() == Owner::AdmissionFailure::Capacity,
               "aggregate overflow must classify as Capacity") &&
         check(!settled && acquisitions == before && owner.size() == 0u &&
                   owner.retainedCount() == 0u,
               "capacity denial must acquire nothing and settle nothing");
}

bool warmCapacityAndPrefix() {
  D9CSurface surface;
  D9CTexture texture;
  D9CBuffer buffer;
  D9CShader shader;
  Owner owner;
  auto seed = draw(1u);
  seed.surface0 = ref<D9C_CHUNK_HANDLE_KIND_SURFACE>(surface, 1u);
  seed.texture0 = ref<D9C_CHUNK_HANDLE_KIND_TEXTURE>(texture, 2u);
  seed.buffer0 = ref<D9C_CHUNK_HANDLE_KIND_BUFFER>(buffer, 3u);
  if (!check(owner.admit(seed) && owner.settle() && owner.retainedCount() == 3u,
             "seed fills a mixed-kind warm epoch")) return false;

  auto novel = draw(1u);
  novel.shader0 = ref<D9C_CHUNK_HANDLE_KIND_SHADER>(shader, 4u);
  PeSemanticPreparedRecord prepared{};
  const auto before = acquisitions;
  if (!check(owner.prepareAdmission(novel, prepared) == Outcome::Capacity &&
                 !owner.admit(novel) && owner.size() == 0u && acquisitions == before,
             "warm saturation rejects without manufacturing an empty epoch")) return false;

  // All three current-kind arenas are empty; the pure aggregate proof must
  // still recognize warm duplicates, including multiple occurrences.
  auto warm = draw(1u);
  warm.texture0 = seed.texture0;
  warm.texture1 = seed.texture0;
  if (!check(owner.prepareAdmission(warm, prepared) == Outcome::Admissible &&
                 owner.admit(warm) && acquisitions == before,
             "warm duplicate at ceiling needs no additional acquisition")) return false;
  PeSemanticExactFixedEmission emission{};
  if (!check(owner.emitExactFixed(emission), "capture committed prefix")) return false;
  const std::vector<std::byte> prefix(emission.wire.begin(), emission.wire.end());
  novel.sourceOrdinal = novel.recordOrdinal = 2u;
  if (!check(!owner.admit(novel) &&
                 owner.lastAdmissionFailure() == Owner::AdmissionFailure::Capacity &&
                 owner.emitExactFixed(emission) && emission.wire.size() == prefix.size() &&
                 std::memcmp(emission.wire.data(), prefix.data(), prefix.size()) == 0 &&
                 owner.size() == 1u && acquisitions == before,
             "capacity denial preserves committed bytes and retain multiplicity")) return false;

  // Closing this real chunk need not reclaim the prior epoch yet. No retry
  // loop or early warm eviction is permitted to hide that fact.
  if (!check(owner.settle(), "settle real warm-touch chunk")) return false;
  novel.sourceOrdinal = novel.recordOrdinal = 1u;
  if (!check(owner.prepareAdmission(novel, prepared) == Outcome::Capacity,
             "one real commit does not promise enough aggregate headroom")) return false;
  auto noResources = draw(1u);
  if (!check(owner.admit(noResources) && owner.settle(),
             "advance through a legitimate resource-free record")) return false;
  if (!check(owner.prepareAdmission(novel, prepared) == Outcome::Admissible &&
                 owner.admit(novel) && shader.refs == 2u,
             "retry succeeds after reference expiry releases headroom")) return false;
  owner.reset();
  return check(surface.refs == 1u && texture.refs == 1u && buffer.refs == 1u &&
                   shader.refs == 1u,
               "reset releases each remaining pin exactly once");
}

bool compactCapacityPre() {
  D9CSurface surface;
  D9CTexture texture;
  D9CBuffer buffer;
  D9CShader shader;
  Owner owner;
  PeHotStateShadow shadow{};
  PeConstShadowBlock constants{};
  PeBindingView bindings{};
  bindings.renderTargets[0] = ref<D9C_CHUNK_HANDLE_KIND_SURFACE>(surface, 1u);
  bindings.textures[0] = ref<D9C_CHUNK_HANDLE_KIND_TEXTURE>(texture, 2u);
  bindings.streams[0].buffer = ref<D9C_CHUNK_HANDLE_KIND_BUFFER>(buffer, 3u);
  bindings.vs = ref<D9C_CHUNK_HANDLE_KIND_SHADER>(shader, 4u);
  shadow.writer().pendingRtMask() = 1u;
  shadow.writer().pendingTextureMask() = 1u;
  shadow.writer().pendingStreamMask() = 1u;
  shadow.writer().pendingVs() = true;
  const PeDrawParams params{.recordType = D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
                            .primitiveType = 4u, .primitiveCount = 1u};
  SparseStatePlan plan;
  std::recursive_mutex mutex;
  dxmt9::core::ThreadOwnershipToken thread;
  std::uint64_t epoch = 1u;
  RecorderLockGuard guard(mutex, false);
  const auto before = acquisitions;
  bool settled = false;
  const bool rejected = guard.withCapability(thread, epoch,
      [&](const auto& access) noexcept {
        if (!buildSparseStatePlan(access, shadow, constants, bindings, {},
                                  params, false, false, plan) ||
            !finalizeSparseStatePlanChunkContext(access, {}, plan)) return false;
        PeSemanticCompactPreparedRecord prepared{};
        const PeSemanticSparsePlanRecordInput input{
            .access = &access, .sparsePlan = &plan,
            .producer = PeSemanticProducerKind::DrawPrimitive,
            .recordType = params.recordType, .sourceOrdinal = 1u,
            .recordOrdinal = 1u};
        return owner.prepareAdmission(input, prepared) == Outcome::Capacity &&
               prepared.identityWitness.count == 4u &&
               !owner.tryAppendSparseStatePlan(
                   access, plan, input.producer, input.recordType, 0u, 1u, 1u,
                   [&]() noexcept { settled = true; return true; }) &&
               owner.lastAdmissionFailure() == Owner::AdmissionFailure::Capacity;
      });
  return check(rejected && !settled && acquisitions == before &&
                   owner.size() == 0u && shadow.pendingTextureMask() == 1u,
               "real compact producer rejects aggregate capacity before any effect");
}
} // namespace

int main() {
  return mixedKindCapacityPre() && warmCapacityAndPrefix() && compactCapacityPre()
      ? 0 : 1;
}
