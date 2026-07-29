// pe_producer_views_spec
//
// The producer's input views must be trivially copyable PODs, because the
// differential harness constructs them directly and the producer must retain
// nothing from them past the call. Scratch capacity must match the V2 section
// caps, or a full-width delta silently truncates.

#include "d3d9_pe_producer_views.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
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
  check(bindings.rtExplicitMask == 0u, "default rt mask must be zero");
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
  // D9CCommandChunkWireDrawHeaderV2::baseVertex is int32_t. If PeDrawParams
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

int main() {
  try {
    viewsAreTriviallyCopyable();
    payloadsAreEmptyByDefault();
    defaultBindingViewIsAllNull();
    defaultChunkContextClaimsNothingRetained();
    defaultDrawParamsAreZero();
    baseVertexIsSigned();
    scratchCapacityMatchesSectionCaps();
  } catch (const TestFailure& failure) {
    std::cerr << "pe_producer_views_spec FAILED: " << failure.what() << "\n";
    return 1;
  }
  std::cout << "pe_producer_views_spec OK\n";
  return 0;
}
