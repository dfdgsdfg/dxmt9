// R-BACK-12.22..12.26 — Stage 2 argbuf-hybrid populator spec.
//
// CPU-only spec for the per-encoder argbuf populator wired in this
// branch (commit `argbuf-populator`). Focus is on the value-transform
// shape — the actual Metal-side openArgbuf / populateConstantBuffers /
// populateResourceBindings calls require a live MTLDevice and are
// covered by the runtime-integration suite once shader-runner equality
// (R-BACK-12.26) lands. Here we exercise:
//
//   - ArgbufEncoderResource::initForTest sets the encodedLength /
//     alignment shape `openArgbuf` reads from on the hot path.
//   - dirtyBytesEstimate returns 0 when no per-frequency bit is set
//     and matches the four host-struct sizes when each category fires.
//   - dirtyBytesEstimate composes additively across categories so the
//     encoder counter accounts for every dirty bit.
//   - the Stage 2 argument-buffer recorder seam captures concrete
//     texture/sampler writes and argbuf [[id(N)]] ordering without a
//     live Metal device.
//
// `openArgbuf` itself reaches into CommandQueue::reserveTransientBuffer
// which requires a Metal device; the runtime suite covers that path.

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../../src/dxmt9/dxmt9_argbuf_hybrid.hpp"
#include "../../../src/dxmt9/dxmt9_draw_state.hpp"
#include "../../../src/dxmt9/dxmt9_resource_pool.hpp"
#include "../../../src/dxmt9/dxmt9_uniform_dirty.hpp"

namespace {

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string message) {
  throw TestFailure(std::move(message));
}

void check(bool condition, std::string_view message) {
  if (!condition) {
    fail(std::string(message));
  }
}

template <typename A, typename B>
void checkEq(const A& left, const B& right, std::string_view message) {
  if (!(left == right)) {
    std::ostringstream out;
    out << message << " (left=" << static_cast<std::uint64_t>(left)
        << " right=" << static_cast<std::uint64_t>(right) << ")";
    fail(out.str());
  }
}

enum class RecordedKind {
  SetTexture,
  SetSamplerState,
};

struct RecordedCommand {
  RecordedKind kind = RecordedKind::SetTexture;
  obj_handle_t handle = 0;
  std::uint32_t index = 0;
};

struct Capture {
  std::vector<RecordedCommand> commands;
};

void recordSetTexture(void* userdata, WMT::Texture texture, std::uint32_t index) {
  auto& capture = *static_cast<Capture*>(userdata);
  capture.commands.push_back(RecordedCommand{
      .kind = RecordedKind::SetTexture,
      .handle = texture.handle,
      .index = index,
  });
}

void recordSetSamplerState(void* userdata,
                           WMT::SamplerState sampler,
                           std::uint32_t index) {
  auto& capture = *static_cast<Capture*>(userdata);
  capture.commands.push_back(RecordedCommand{
      .kind = RecordedKind::SetSamplerState,
      .handle = sampler.handle,
      .index = index,
  });
}

dxmt9::argbuf_hybrid::ArgbufRecorder makeRecorder(Capture& capture,
                                                   obj_handle_t samplerHandle) {
  dxmt9::argbuf_hybrid::ArgbufRecorder recorder{};
  recorder.userdata = &capture;
  recorder.suppressMetalCalls = true;
  recorder.samplerState.handle = samplerHandle;
  recorder.setTexture = recordSetTexture;
  recorder.setSamplerState = recordSetSamplerState;
  return recorder;
}

const RecordedCommand& commandAt(const Capture& capture,
                                 std::size_t index,
                                 std::string_view message) {
  if (index >= capture.commands.size()) {
    fail(std::string(message));
  }
  return capture.commands[index];
}

struct Harness {
  dxmt9::core::BackendLimits limits{};
  dxmt9::resources::Pool pool{};
  std::vector<dxmt9::core::TextureHandle> patchedTextures;

  ~Harness() {
    clearPatchedTextureHandles();
  }

  void clearPatchedTextureHandles() {
    for (const auto& handle : patchedTextures) {
      if (auto* record = pool.findTexture(handle.value)) {
        record->texture.handle = 0;
        record->shaderReadTexture.handle = 0;
      }
    }
  }

  void restorePatchedTextureHandle(dxmt9::core::TextureHandle handle,
                                   obj_handle_t textureHandle,
                                   obj_handle_t shaderReadHandle = 0) {
    auto* record = pool.findTexture(handle.value);
    check(record != nullptr, "patched texture record remains live");
    record->texture.handle = textureHandle;
    record->shaderReadTexture.handle = shaderReadHandle;
  }

  dxmt9::core::TextureHandle createTexture(obj_handle_t textureHandle,
                                           obj_handle_t shaderReadHandle = 0) {
    clearPatchedTextureHandles();

    dxmt9::core::TextureDesc desc{};
    desc.width = 2u;
    desc.height = 2u;
    desc.levels = 1u;
    desc.pool = dxmt9::core::Pool::Default;
    auto handle = pool.createTexture(WMT::Device{}, limits, desc);
    check(pool.findTexture(handle.value) != nullptr,
          "pool creates texture record");
    patchedTextures.push_back(handle);
    restorePatchedTextureHandle(handle, textureHandle, shaderReadHandle);
    return handle;
  }
};

// ---------------------------------------------------------------------
// E1 — ArgbufEncoderResource shape

void testEncoderResourceDefaultUninitialized() {
  dxmt9::argbuf_hybrid::ArgbufEncoderResource res;
  check(!res.initialized(),
        "default-constructed ArgbufEncoderResource is uninitialized");
  checkEq(res.encodedLength(), std::uint64_t{0},
          "encodedLength is 0 before init");
  // Default alignment is 16 — matches the constant-block alignment of
  // the four [[id(0..3)]] cbuf entries.
  checkEq(res.alignment(), std::uint64_t{16},
          "alignment defaults to 16 B before init");
}

void testEncoderResourceInitForTestPopulatesShape() {
  dxmt9::argbuf_hybrid::ArgbufEncoderResource res;
  res.initForTest(/*encodedLength=*/512u, /*alignment=*/32u);
  check(res.initialized(), "initForTest flips initialized");
  checkEq(res.encodedLength(), std::uint64_t{512u},
          "initForTest sets encodedLength");
  checkEq(res.alignment(), std::uint64_t{32u},
          "initForTest sets alignment when >=16");
}

void testEncoderResourceAlignmentFloor() {
  // Test fixtures may pass alignment=0 / alignment=4 — the resource
  // floors alignment to 16 so reservations always satisfy the
  // constant-block alignment of the cbuf entries.
  dxmt9::argbuf_hybrid::ArgbufEncoderResource res;
  res.initForTest(/*encodedLength=*/256u, /*alignment=*/0u);
  checkEq(res.alignment(), std::uint64_t{16u},
          "alignment is floored to 16 even when caller passes 0");
}

// ---------------------------------------------------------------------
// E2 — dirtyBytesEstimate

void testDirtyBytesEstimateEmpty() {
  dxmt9::uniform::DirtyState clean{};
  checkEq(dxmt9::argbuf_hybrid::dirtyBytesEstimate(clean),
          std::uint64_t{0},
          "no dirty bits => 0 bytes rewritten");
}

void testDirtyBytesEstimateVsAny() {
  dxmt9::uniform::DirtyState s{};
  dxmt9::uniform::setBit(s, dxmt9::uniform::DirtyBit::VsF);
  checkEq(dxmt9::argbuf_hybrid::dirtyBytesEstimate(s),
          std::uint64_t{sizeof(dxmt9::state::VsConsts)},
          "VsF dirty => sizeof(VsConsts) bytes");
}

void testDirtyBytesEstimatePsAny() {
  dxmt9::uniform::DirtyState s{};
  dxmt9::uniform::setBit(s, dxmt9::uniform::DirtyBit::PsB);
  checkEq(dxmt9::argbuf_hybrid::dirtyBytesEstimate(s),
          std::uint64_t{sizeof(dxmt9::state::PsConsts)},
          "PsB dirty => sizeof(PsConsts) bytes");
}

void testDirtyBytesEstimateFfpVsAny() {
  dxmt9::uniform::DirtyState s{};
  dxmt9::uniform::setBit(s, dxmt9::uniform::DirtyBit::FfpVsViewport);
  checkEq(dxmt9::argbuf_hybrid::dirtyBytesEstimate(s),
          std::uint64_t{sizeof(dxmt9::state::FfpVsConsts)},
          "FfpVsViewport dirty => sizeof(FfpVsConsts) bytes");
}

void testDirtyBytesEstimateFfpPsAny() {
  dxmt9::uniform::DirtyState s{};
  dxmt9::uniform::setBit(s, dxmt9::uniform::DirtyBit::FfpPsAlpha);
  checkEq(dxmt9::argbuf_hybrid::dirtyBytesEstimate(s),
          std::uint64_t{sizeof(dxmt9::state::FfpPsConsts)},
          "FfpPsAlpha dirty => sizeof(FfpPsConsts) bytes");
}

void testDirtyBytesEstimateAllCategories() {
  // markAllDirty sets every category bit; the byte total must equal
  // the sum of the four host-struct sizes (Stage 1 worst case).
  dxmt9::uniform::DirtyState s{};
  dxmt9::uniform::markAllDirty(s);
  const std::uint64_t expected = sizeof(dxmt9::state::VsConsts) +
                                  sizeof(dxmt9::state::PsConsts) +
                                  sizeof(dxmt9::state::FfpVsConsts) +
                                  sizeof(dxmt9::state::FfpPsConsts);
  checkEq(dxmt9::argbuf_hybrid::dirtyBytesEstimate(s), expected,
          "all-dirty mask => sum of four per-frequency host structs");
}

void testDirtyBytesEstimateAdditive() {
  // Two non-overlapping categories should compose additively. Use VsF +
  // FfpPsTexFactor — they hit disjoint dirty masks.
  dxmt9::uniform::DirtyState a{};
  dxmt9::uniform::setBit(a, dxmt9::uniform::DirtyBit::VsF);
  dxmt9::uniform::DirtyState b{};
  dxmt9::uniform::setBit(b, dxmt9::uniform::DirtyBit::FfpPsTexFactor);
  dxmt9::uniform::DirtyState ab{};
  dxmt9::uniform::setBit(ab, dxmt9::uniform::DirtyBit::VsF);
  dxmt9::uniform::setBit(ab, dxmt9::uniform::DirtyBit::FfpPsTexFactor);
  const auto sum = dxmt9::argbuf_hybrid::dirtyBytesEstimate(a) +
                   dxmt9::argbuf_hybrid::dirtyBytesEstimate(b);
  checkEq(dxmt9::argbuf_hybrid::dirtyBytesEstimate(ab), sum,
          "VsF + FfpPsTexFactor => additive byte total");
}

// ---------------------------------------------------------------------
// E3 — PopulatedArgbuf default + boolean conversion

void testPopulatedArgbufDefaultIsEmpty() {
  dxmt9::argbuf_hybrid::PopulatedArgbuf p{};
  check(!static_cast<bool>(p),
        "default-constructed PopulatedArgbuf is empty (length==0)");
  checkEq(p.length, std::uint64_t{0}, "default length is 0");
  checkEq(p.offset, std::uint64_t{0}, "default offset is 0");
}

// ---------------------------------------------------------------------
// E4 — Stage 2 texture/sampler MTLArgumentEncoder write seam

void testPopulateResourceBindingsRecordsTextureSamplerWrites() {
  constexpr obj_handle_t kTexture0 = 0x6000000000000010ull;
  constexpr obj_handle_t kTexture1Storage = 0x6000000000000011ull;
  constexpr obj_handle_t kTexture1ShaderRead = 0x6000000000000012ull;
  constexpr obj_handle_t kSampler = 0x6000000000000020ull;
  constexpr std::uint32_t kTextureBase = dxmt9::shaders::kArgbufHybridTexture2DBase;
  constexpr std::uint32_t kSamplerBase = dxmt9::shaders::kArgbufHybridSamplerBase;

  Harness harness;
  auto texture0 = harness.createTexture(kTexture0);
  auto texture1 = harness.createTexture(kTexture1Storage, kTexture1ShaderRead);
  harness.restorePatchedTextureHandle(texture0, kTexture0);

  dxmt9::core::FlatDrawStateRecord hot{};
  hot.textures[0] = texture0;
  hot.textures[1] = texture1;
  hot.samplerStates[0].entries[0] =
      dxmt9::core::FlatStateEntry{dxmt9::core::SAMP_ADDRESS_U, 3u};
  hot.samplerStates[0].count = 1u;
  hot.samplerStates[1].entries[0] =
      dxmt9::core::FlatStateEntry{dxmt9::core::SAMP_MIN_FILTER, 2u};
  hot.samplerStates[1].count = 1u;

  dxmt9::argbuf_hybrid::ArgbufEncoderResource encoderResource;
  encoderResource.initForTest(/*encodedLength=*/512u, /*alignment=*/16u);
  Capture capture;
  auto recorder = makeRecorder(capture, kSampler);

  dxmt9::argbuf_hybrid::populateResourceBindings(
      WMT::Reference<WMT::Device>{},
      harness.pool,
      encoderResource,
      dxmt9::core::FlatDrawStateView{.hot = &hot},
      &recorder);

  checkEq(capture.commands.size(), std::size_t{4},
          "Stage 2 resource write command count");

  const auto& texture0Command = commandAt(capture, 0, "missing texture0 write");
  check(texture0Command.kind == RecordedKind::SetTexture,
        "texture0 write is first");
  checkEq(texture0Command.handle, kTexture0,
          "texture0 writes the storage texture handle");
  checkEq(texture0Command.index, kTextureBase,
          "texture0 writes argbuf id 4");

  const auto& sampler0Command = commandAt(capture, 1, "missing sampler0 write");
  check(sampler0Command.kind == RecordedKind::SetSamplerState,
        "sampler0 write follows texture0");
  checkEq(sampler0Command.handle, kSampler,
          "sampler0 writes the recorder sampler handle");
  checkEq(sampler0Command.index, kSamplerBase,
          "sampler0 writes argbuf id 28");

  const auto& texture1Command = commandAt(capture, 2, "missing texture1 write");
  check(texture1Command.kind == RecordedKind::SetTexture,
        "texture1 write follows sampler0");
  checkEq(texture1Command.handle, kTexture1ShaderRead,
          "texture1 writes the shader-read texture view when present");
  checkEq(texture1Command.index, kTextureBase + 1u,
          "texture1 writes argbuf id 5");

  const auto& sampler1Command = commandAt(capture, 3, "missing sampler1 write");
  check(sampler1Command.kind == RecordedKind::SetSamplerState,
        "sampler1 write follows texture1");
  checkEq(sampler1Command.handle, kSampler,
          "sampler1 writes the recorder sampler handle");
  checkEq(sampler1Command.index, kSamplerBase + 1u,
          "sampler1 writes argbuf id 29");
}

}  // namespace

int main() {
  try {
    testEncoderResourceDefaultUninitialized();
    testEncoderResourceInitForTestPopulatesShape();
    testEncoderResourceAlignmentFloor();
    testDirtyBytesEstimateEmpty();
    testDirtyBytesEstimateVsAny();
    testDirtyBytesEstimatePsAny();
    testDirtyBytesEstimateFfpVsAny();
    testDirtyBytesEstimateFfpPsAny();
    testDirtyBytesEstimateAllCategories();
    testDirtyBytesEstimateAdditive();
    testPopulatedArgbufDefaultIsEmpty();
    testPopulateResourceBindingsRecordsTextureSamplerWrites();
  } catch (const TestFailure& failure) {
    std::cerr << "argbuf_populator_spec failed: " << failure.what() << '\n';
    return 1;
  } catch (const std::exception& ex) {
    std::cerr << "argbuf_populator_spec unexpected exception: " << ex.what() << '\n';
    return 1;
  }
  std::cout << "argbuf_populator_spec passed\n";
  return 0;
}
