// pe_producer_views_spec
//
// The producer's input views must be trivially copyable PODs, because the
// differential harness constructs them directly and the producer must retain
// nothing from them past the call. Scratch capacity must match the canonical section
// caps, or a full-width delta silently truncates.

#include "d3d9_pe_producer.hpp"
#include "d3d9_pe_producer_views.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <span>
#include <string_view>
#include <type_traits>

struct TestFailure : std::runtime_error {
  explicit TestFailure(std::string message)
      : std::runtime_error(std::move(message)) {}
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

namespace pe = dxmt9::d3d9::pe;

void viewsAreTriviallyCopyable() {
  static_assert(std::is_trivially_copyable_v<pe::PeStreamBinding>);
  static_assert(std::is_trivially_copyable_v<pe::PeBindingView>);
  static_assert(std::is_trivially_copyable_v<pe::PeChunkContext>);
  static_assert(std::is_trivially_copyable_v<pe::PeDrawParams>);
  check(true, "compile-time only");
}

void payloadsAreEmptyByDefault() {
  pe::PeDrawPayloads payloads{};
  check(payloads.upIndex.empty(), "default upIndex must be empty");
  check(payloads.upVertex.empty(), "default upVertex must be empty");
}

void defaultBindingViewIsAllNull() {
  pe::PeBindingView bindings{};
  for (const auto& texture : bindings.textures) {
    check(texture.object == nullptr, "default texture ref must be null");
  }
  for (const auto& stream : bindings.streams) {
    check(stream.buffer.object == nullptr,
          "default stream buffer must be null");
    check(stream.offset == 0u, "default stream offset must be zero");
    check(stream.stride == 0u, "default stream stride must be zero");
  }
  check(bindings.vs.object == nullptr, "default vs must be null");
  check(bindings.ps.object == nullptr, "default ps must be null");
  check(bindings.vdecl.object == nullptr, "default vdecl must be null");
  check(bindings.indexBuffer.object == nullptr, "default ib must be null");
  check(bindings.depthStencil.object == nullptr, "default ds must be null");
  for (const bool explicitRt : bindings.rtExplicitMask) {
    check(!explicitRt, "default rt explicit flags must all be false");
  }
  check(bindings.fvf == 0u, "default fvf must be zero");
}

void defaultChunkContextClaimsNothingRetained() {
  pe::PeChunkContext chunk{};
  check(chunk.retainedStreamMask == 0u, "a fresh chunk retains no streams");
  check(!chunk.indexBufferKnown, "a fresh chunk has no known index buffer");
  check(chunk.submittedIndexBufferWire == 0u,
        "a fresh chunk has no submitted index buffer wire value");
}

void defaultDrawParamsAreZero() {
  pe::PeDrawParams params{};
  check(params.recordType == 0u, "default recordType must be zero");
  check(params.primitiveType == 0u, "default primitiveType must be zero");
  check(params.baseVertex == 0, "default baseVertex must be zero");
  check(params.minVertex == 0u, "default minVertex must be zero");
  check(params.numVertices == 0u, "default numVertices must be zero");
  check(params.startVertex == 0u, "default startVertex must be zero");
  check(params.startIndex == 0u, "default startIndex must be zero");
  check(params.primitiveCount == 0u, "default primitiveCount must be zero");
  check(params.stride == 0u, "default stride must be zero");
  check(params.indexFormat == 0u, "default indexFormat must be zero");
}

void baseVertexIsSigned() {
  // D9CCommandChunkWireDrawHeader::baseVertex is int32_t. If PeDrawParams
  // narrowed it to unsigned, a negative BaseVertexIndex would silently wrap.
  static_assert(std::is_signed_v<decltype(pe::PeDrawParams{}.baseVertex)>);
  pe::PeDrawParams params{};
  params.baseVertex = -32;
  check(params.baseVertex == -32, "baseVertex must hold a negative value");
}

void scratchCapacityMatchesSectionCaps() {
  pe::PeSparseScratch scratch{};
  check(scratch.renderStates.size() == D9C_DRAW_PACKET_MAX_RENDER_STATES,
        "render state scratch must match the section cap");
  check(scratch.textures.size() == D9C_DRAW_PACKET_MAX_TEXTURES,
        "texture scratch must match the section cap");
  check(scratch.streams.size() == D9C_DRAW_PACKET_MAX_STREAMS,
        "stream scratch must match the section cap");
  check(scratch.renderTargets.size() == D9C_DRAW_PACKET_MAX_RENDER_TARGETS,
        "render target scratch must match the section cap");
  check(scratch.textureStageStates.size() == D9C_DRAW_PACKET_MAX_TSS,
        "TSS scratch must match the section cap");
  check(scratch.samplerStates.size() == D9C_DRAW_PACKET_MAX_SAMPLER,
        "sampler scratch must match the section cap");
  check(scratch.transforms.size() == D9C_DRAW_PACKET_MAX_TRANSFORMS,
        "transform scratch must match the section cap");
  check(scratch.lights.size() == D9C_DRAW_PACKET_MAX_LIGHTS,
        "light scratch must match the section cap");
  check(scratch.lightEnables.size() == D9C_DRAW_PACKET_MAX_LIGHTS,
        "light enable scratch must match the light cap");
}

// Regression pin for the GT1 indexed-draw corruption. addChunkContextSections
// decides "is this an indexed draw" solely from params.recordType, and on a
// non-indexed verdict it does not merely skip the index section -- it rebuilds
// the span as first(0), wiping whatever buildSparseState already emitted for
// pendingIb. The device forwarder used to stamp recordType onto a by-value copy
// of params, so the live call arrived with 0, and every indexed draw shipped
// with no index binding. SetIndices records nothing standalone in chunk mode, so
// draws replayed against a stale index buffer: sliver triangles, garbled HUD.
//
// The offline differential could not catch it -- its fixtures stamp params
// themselves, so they never reproduce the device's threading of the value. This
// pins the producer's half of the contract instead: 0 is refused outright, an
// indexed draw with a dirty IB keeps its section, and a non-indexed draw still
// gets none.
void unstampedRecordTypeIsRefused() {
  PeHotStateShadow shadow{};
  pe::PeBindingView bindings{};
  pe::PeSparseScratch scratch{};
  pe::SparseStateInput out{};
  int indexObject = 0;
  bindings.indexBuffer.object = &indexObject;
  bindings.indexBuffer.identity.kind = D9C_CHUNK_HANDLE_KIND_BUFFER;
  bindings.indexBuffer.identity.generation = 1u;
  bindings.indexBuffer.identity.objectId = 7u;
  shadow.writer().pendingIb() = true;

  pe::PeDrawParams unstamped{};  // recordType stays 0
  check(!pe::addChunkContextSections(pe::PeChunkContext{}, shadow, bindings,
                                     unstamped, scratch, out),
        "an unstamped recordType must be refused, not treated as non-indexed");
}

void indexedDrawKeepsItsIndexSection() {
  PeHotStateShadow shadow{};
  pe::PeBindingView bindings{};
  pe::PeSparseScratch scratch{};
  int indexObject = 0;
  bindings.indexBuffer.object = &indexObject;
  bindings.indexBuffer.identity.kind = D9C_CHUNK_HANDLE_KIND_BUFFER;
  bindings.indexBuffer.identity.generation = 1u;
  bindings.indexBuffer.identity.objectId = 7u;
  shadow.writer().pendingIb() = true;

  // Seed the section the way buildSparseState would have, so a rebuild that
  // wrongly drops it is visible rather than merely absent.
  pe::SparseStateInput out{};
  scratch.indexBuffers[0] = pe::SparseBindingInput<D9CCommandChunkWireIndexBinding>{};
  scratch.indexBuffers[0].wire.valid = 1u;
  scratch.indexBuffers[0].object = bindings.indexBuffer;
  out.indexBuffers = std::span(scratch.indexBuffers).first(1);

  pe::PeDrawParams indexed{};
  indexed.recordType = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
  check(pe::addChunkContextSections(pe::PeChunkContext{}, shadow, bindings,
                                    indexed, scratch, out),
        "an indexed draw with a dirty IB must succeed");
  check(out.indexBuffers.size() == 1,
        "an indexed draw with a dirty IB must keep its index section");
  check(out.indexBuffers[0].object.object == &indexObject,
        "the retained index section must name the bound index buffer");

  // Same inputs, non-indexed record: no section at all is the correct answer.
  pe::SparseStateInput nonIndexedOut{};
  pe::PeDrawParams nonIndexed{};
  nonIndexed.recordType = D9C_COMMAND_RECORD_DRAW_PRIMITIVE;
  check(pe::addChunkContextSections(pe::PeChunkContext{}, shadow, bindings,
                                    nonIndexed, scratch, nonIndexedOut),
        "a non-indexed draw must succeed");
  check(nonIndexedOut.indexBuffers.empty(),
        "a non-indexed draw must carry no index section");
}

void acceptedScalarProjectionConsumesExactValues() {
  PeHotStateShadow shadow{};
  PeConstShadowBlock constants{};
  pe::PeScalarSemanticTokenLedger tokens{};
  auto pending = shadow.writer().pendingRenderStatesTyped();
  const auto key = renderStateSlotKey(7u);
  pending.set(key, 42u);
  check(tokens.record(pe::ScalarSemanticCategory::RenderState, 7u),
        "production scalar source ordinal records");

  const std::array<float, 4> constantValue{1.0f, 0.0f, 0.0f, 0.0f};
  touchConstShadow(constants.vsConstF, 0u, 1u, constantValue.data(),
                   sizeof(constantValue));
  std::array<D9CCommandChunkWireRenderState, 1> renderStates{
      D9CCommandChunkWireRenderState{.state = 7u, .value = 41u}};
  pe::SparseStateInput state{};
  state.renderStates = renderStates;
  state.vsFloatConstants = pe::SparseConstantRangeInput{
      .startRegister = 0u,
      .registerCount = 1u,
      .registerBytes = std::span<const std::byte>(
          reinterpret_cast<const std::byte*>(constants.vsConstF.values.data()),
          sizeof(float) * 4u),
  };
  const auto accepted = pe::settleRecorderAppend({
      .phase = pe::AppendSettlement::Prepared,
      .appendSucceeded = true,
  });
  check(!pe::acceptPreparedSparseState(shadow, constants, state, accepted,
                                       nullptr, 11u),
        "default production preflight rejects scalar value mismatch");
  check(pending.contains(key) && constants.vsConstF.dirty() && tokens.size() == 1u,
        "default value mismatch preserves scalar, constant, and observer state");
  check(!pe::acceptPreparedSparseState(shadow, constants, state, accepted,
                                       &tokens, 11u),
        "scalar value mismatch rejects before any consumer");
  check(pending.contains(key) && constants.vsConstF.dirty() && tokens.size() == 1u,
        "value mismatch preserves scalar, constant, and ordinal state");

  renderStates[0].value = 42u;
  check(pe::acceptPreparedSparseState(shadow, constants, state, accepted,
                                      &tokens, 11u),
        "exact production projection accepts");
  check(!pending.contains(key) && !constants.vsConstF.dirty() && tokens.empty(),
        "accepted projection consumes scalar and constant state exactly once");
}

void scalarProjectionRejectsDuplicateAndOrderMismatchAtomically() {
  PeHotStateShadow shadow{};
  PeConstShadowBlock constants{};
  pe::PeScalarSemanticTokenLedger tokens{};
  auto pending = shadow.writer().pendingRenderStatesTyped();
  pending.set(renderStateSlotKey(3u), 30u);
  pending.set(renderStateSlotKey(7u), 70u);
  check(tokens.record(pe::ScalarSemanticCategory::RenderState, 3u) &&
            tokens.record(pe::ScalarSemanticCategory::RenderState, 7u),
        "two ordered source tokens record");
  const auto accepted = pe::settleRecorderAppend({
      .phase = pe::AppendSettlement::Prepared,
      .appendSucceeded = true,
  });

  std::array<D9CCommandChunkWireRenderState, 2> rows{{
      {.state = 3u, .value = 30u},
      {.state = 3u, .value = 30u},
  }};
  pe::SparseStateInput state{};
  state.renderStates = rows;
  check(!pe::acceptPreparedSparseState(shadow, constants, state, accepted,
                                       &tokens, 21u),
        "duplicate scalar tuple is rejected");
  check(pending.size() == 2u && tokens.size() == 2u,
        "duplicate rejection is atomic");

  rows = {{{.state = 7u, .value = 70u},
           {.state = 3u, .value = 30u}}};
  check(!pe::acceptPreparedSparseState(shadow, constants, state, accepted,
                                       &tokens, 21u),
        "non-canonical scalar order is rejected");
  check(pending.size() == 2u && tokens.size() == 2u,
        "order rejection is atomic");
}

void oversizedScalarProjectionConservesTokensAcrossBatches() {
  PeHotStateShadow shadow{};
  PeConstShadowBlock constants{};
  pe::PeScalarSemanticTokenLedger tokens{};
  auto pending = shadow.writer().pendingRenderStatesTyped();
  constexpr std::size_t firstBatchCount = D9C_DRAW_PACKET_MAX_RENDER_STATES;
  constexpr std::size_t totalCount = firstBatchCount + 1u;
  std::array<D9CCommandChunkWireRenderState, firstBatchCount> firstBatch{};
  std::array<D9CCommandChunkWireRenderState, 1> secondBatch{};
  for (std::size_t i = 0u; i < totalCount; ++i) {
    const auto key = renderStateSlotKey(static_cast<std::uint32_t>(i));
    const auto value = static_cast<std::uint32_t>(100u + i);
    pending.set(key, value);
    check(tokens.record(pe::ScalarSemanticCategory::RenderState,
                        static_cast<std::uint32_t>(i)),
          "oversized batch source ordinal records");
    if (i < firstBatchCount) {
      firstBatch[i] = {
          .state = static_cast<std::uint32_t>(i), .value = value};
    } else {
      secondBatch[0] = {
          .state = static_cast<std::uint32_t>(i), .value = value};
    }
  }
  pe::SparseStateInput firstState{};
  firstState.renderStates = firstBatch;
  const auto accepted = pe::settleRecorderAppend({
      .phase = pe::AppendSettlement::Prepared,
      .appendSucceeded = true,
  });
  check(pe::acceptPreparedSparseState(shadow, constants, firstState, accepted,
                                      &tokens, 17u),
        "first oversized scalar batch accepts exactly");
  check(pending.size() == 1u && tokens.size() == 1u,
        "first oversized batch leaves one pending scalar and token");
  pe::SparseStateInput secondState{};
  secondState.renderStates = secondBatch;
  check(pe::acceptPreparedSparseState(shadow, constants, secondState, accepted,
                                      &tokens, 18u),
        "second oversized scalar batch accepts exactly");
  check(pending.empty() && tokens.empty(),
        "oversized scalar batches conserve pending and token state");
}

void fullSnapshotScalarSettlementRoutes() {
  const std::array<std::uint32_t, 5> recordTypes = {
      D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
      D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE,
      D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP,
      D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP,
      D9C_COMMAND_RECORD_APPLY_STATE,
  };
  for (const std::uint32_t recordType : recordTypes) {
    PeHotStateShadow shadow{};
    PeConstShadowBlock constants{};
    pe::PeScalarSemanticTokenLedger tokens{};
    const auto cleanKey = renderStateSlotKey(3u);
    const auto pendingKey = renderStateSlotKey(7u);
    shadow.writer().renderStateShadowTyped().set(cleanKey, 30u);
    shadow.writer().renderStateShadowTyped().set(pendingKey, 70u);
    shadow.writer().pendingRenderStatesTyped().set(pendingKey, 70u);
    check(tokens.record(pe::ScalarSemanticCategory::RenderState, 7u),
          "full snapshot route records the pending source token");

    pe::PeBindingView bindings{};
    pe::PeDrawPayloads payloads{};
    pe::PeDrawParams params{};
    params.recordType = recordType;
    params.primitiveType = 4u;
    params.primitiveCount = 1u;
    pe::PeSparseScratch scratch{};
    pe::SparseStateInput state{};
    D9CCommandChunkWireDrawHeader header{};
    check(pe::buildSparseState(shadow, constants, bindings, payloads, params,
                               /*forceFullSnapshot=*/true,
                               /*inlineConstDelta=*/false, scratch, header,
                               state),
          "full snapshot route builds");
    check(state.fullSnapshot && state.renderStates.size() == 2u,
          "full snapshot route carries effective disposition and clean row");

    const auto accepted = pe::settleRecorderAppend({
        .phase = pe::AppendSettlement::Prepared,
        .appendSucceeded = true,
    });
    check(pe::acceptPreparedSparseState(
              shadow, constants, state, accepted, &tokens,
              /*recordOrdinal=*/1u),
          "full snapshot route settles");
    check(shadow.pendingRenderStatesTyped().empty() && tokens.empty(),
          "full snapshot route consumes represented pending row exactly once");
  }

  // A prepared snapshot may be a deliberately narrower projection (for
  // example, a bounded retry/test seam). An unrepresented pending row remains
  // available; only rows actually represented by the accepted record settle.
  PeHotStateShadow shadow{};
  PeConstShadowBlock constants{};
  pe::PeScalarSemanticTokenLedger tokens{};
  const auto cleanKey = renderStateSlotKey(3u);
  const auto unrepresentedKey = renderStateSlotKey(9u);
  shadow.writer().renderStateShadowTyped().set(cleanKey, 30u);
  shadow.writer().renderStateShadowTyped().set(unrepresentedKey, 90u);
  shadow.writer().pendingRenderStatesTyped().set(unrepresentedKey, 90u);
  check(tokens.record(pe::ScalarSemanticCategory::RenderState, 9u),
        "unrepresented snapshot row records its token");
  std::array<D9CCommandChunkWireRenderState, 1> cleanOnly = {{
      {.state = 3u, .value = 30u},
  }};
  pe::SparseStateInput cleanOnlyState{};
  cleanOnlyState.fullSnapshot = true;
  cleanOnlyState.renderStates = cleanOnly;
  const auto accepted = pe::settleRecorderAppend({
      .phase = pe::AppendSettlement::Prepared,
      .appendSucceeded = true,
  });
  check(pe::acceptPreparedSparseState(shadow, constants, cleanOnlyState,
                                      accepted, &tokens, 1u),
        "clean-only snapshot projection settles");
  check(shadow.pendingRenderStatesTyped().contains(unrepresentedKey) &&
            tokens.has(pe::ScalarSemanticCategory::RenderState, 9u),
        "unrepresented pending row is preserved");

  // Append failure is retryable and must leave the mixed snapshot inputs
  // untouched, including the clean/live projection and pending token.
  check(!pe::acceptPreparedSparseState(
            shadow, constants, cleanOnlyState,
            pe::settleRecorderAppend({
                .phase = pe::AppendSettlement::Prepared,
                .appendSucceeded = false,
            }),
            &tokens, 2u),
        "failed full snapshot settlement rejects");
  check(shadow.pendingRenderStatesTyped().contains(unrepresentedKey) &&
            tokens.has(pe::ScalarSemanticCategory::RenderState, 9u),
        "failed full snapshot settlement preserves retry state");

  check(!pe::acceptPreparedSparseState(
            shadow, constants, cleanOnlyState,
            pe::settleRecorderAppend({
                .phase = pe::AppendSettlement::Prepared,
                .appendSucceeded = false,
                .explicitDiscard = true,
            }),
            &tokens, 2u),
        "discarded full snapshot settlement rejects");
  check(shadow.pendingRenderStatesTyped().contains(unrepresentedKey) &&
            tokens.has(pe::ScalarSemanticCategory::RenderState, 9u),
        "discarded full snapshot settlement preserves semantic state");
}

int main() {
  try {
    viewsAreTriviallyCopyable();
    payloadsAreEmptyByDefault();
    defaultBindingViewIsAllNull();
    defaultChunkContextClaimsNothingRetained();
    defaultDrawParamsAreZero();
    baseVertexIsSigned();
    scratchCapacityMatchesSectionCaps();
    unstampedRecordTypeIsRefused();
    indexedDrawKeepsItsIndexSection();
    acceptedScalarProjectionConsumesExactValues();
    scalarProjectionRejectsDuplicateAndOrderMismatchAtomically();
    oversizedScalarProjectionConservesTokensAcrossBatches();
    fullSnapshotScalarSettlementRoutes();
  } catch (const TestFailure& failure) {
    std::cerr << "pe_producer_views_spec FAILED: " << failure.what() << "\n";
    return 1;
  }
  std::cout << "pe_producer_views_spec OK\n";
  return 0;
}
