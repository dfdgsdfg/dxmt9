// R-BACK-2.52: exercise the real compact producer and the real replay queue
// across the inline-constant gate.  This deliberately observes the final
// SourcePayloadView bytes/counts, rather than comparing wire handles or hashes.

#include "device_c_common.hpp"
#include "device_c_replay_offload.hpp"
#include "d3d9_pe_const_shadow.hpp"
#include "d3d9_pe_producer.hpp"
#include "d3d9_pe_semantic_owner.hpp"
#include "d3d9_pe_wire_handle.hpp"
#include "dxmt9/com.hpp"
#include "dxmt9/core.hpp"
#include "dxmt9/device_c.h"
#include "dxmt9/dxmt9_command_queue.hpp"
#include "dxmt9/dxmt9_d3d9_bytecode.hpp"
#include "dxmt9/dxmt9_device.hpp"
#include "dxmt9/dxmt9_source_payload.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace pe = dxmt9::d3d9::pe;
using namespace dxmt9::core;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) throw TestFailure(std::string(message));
}

struct DrawSnapshot {
  bool indexed = false;
  std::uint32_t drawStart = 0u;
  std::int32_t baseVertex = 0;
  std::array<std::uint16_t, 3> vsCounts{};
  std::array<std::uint16_t, 3> psCounts{};
  std::vector<u8> vsBytes;
  std::vector<u8> psBytes;

  friend bool operator==(const DrawSnapshot&, const DrawSnapshot&) = default;
};

struct CaptureIdentityReceipt {
  std::uint64_t captureToken = 0u;
  std::uint64_t eventOrdinal = 0u;
  std::uint32_t emittedRecordCount = 0u;
  D9CRenderTapeIdentityCaptureResult identity{};
  std::vector<std::byte> identityBytes{};
};

std::vector<u8> bytesOf(const void* data, std::size_t size) {
  const auto* first = static_cast<const u8*>(data);
  return {first, first + size};
}

void copyDrawSnapshots(const SourcePayloadView& payload,
                       std::vector<DrawSnapshot>& out) {
  for (std::size_t commandIndex = 0; commandIndex < payload.commandCount();
       ++commandIndex) {
    const auto& command = payload.commandAt(commandIndex).command;
    if (command.kind != MetalCommandKind::DrawRun ||
        command.drawRunRecord == nullptr) {
      continue;
    }
    for (const auto& param : command.drawParams) {
      check(param.primitiveType == PrimitiveType::TriangleList &&
                param.primitiveCount == 1u,
            "logical draw topology and count must survive replay");
      const auto* uniforms = drawRunUniformPayloadRecord(
          command, param.uniformHandle);
      check(uniforms != nullptr, "draw must resolve its uniform payload");
      const auto* vs = drawRunVertexConstantsRecord(
          command, uniforms->vertexConstantsHandle);
      const auto* ps = drawRunPixelConstantsRecord(
          command, uniforms->pixelConstantsHandle);
      check(vs != nullptr && ps != nullptr,
            "draw must resolve both constant families");
      const auto vsBytes = drawRunVertexConstantsBytes(command, *vs);
      const auto psBytes = drawRunPixelConstantsBytes(command, *ps);
      check(vsBytes.size() == vs->constants.byteSize &&
                psBytes.size() == ps->constants.byteSize,
            "constant byte spans must be bounded by their records");
      out.push_back(DrawSnapshot{
          .indexed = param.indexed,
          .drawStart = param.indexed ? param.startIndex : param.startVertex,
          .baseVertex = param.baseVertexIndex,
          .vsCounts = {vs->constants.floatCount, vs->constants.intCount,
                       vs->constants.boolCount},
          .psCounts = {ps->constants.floatCount, ps->constants.intCount,
                       ps->constants.boolCount},
          .vsBytes = {vsBytes.begin(), vsBytes.end()},
          .psBytes = {psBytes.begin(), psBytes.end()},
      });
    }
  }
}

}  // namespace

namespace dxmt9 {

// This is intentionally test-local.  It is the same bounded completion seam
// used by the native CPU-ready routing specs and never enters production code.
struct CommandQueueArenaLeaseTestAccess {
  static std::size_t readyCount(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.cpuReadyTape_.readyCount();
  }

  static bool consumeOne(CommandQueue& queue,
                         std::vector<DrawSnapshot>& snapshots) {
    using namespace core::metalqueue;
    ReadySlotSnapshot source{};
    QueueCompletionSource completion{};
    {
      std::unique_lock lock(queue.mutex_);
      if (!queue.queueLifecycle_.dequeueReadySlot(lock, source)) return false;
      const auto resolved = queue.queueLifecycle_.resolveRepresentedSource(source);
      check(resolved.valid(), "ready source must resolve");
      copyDrawSnapshots(resolved.payload, snapshots);

      QueueSubmissionRecord record{};
      record.testOnlyAllowNullCommandBuffer = true;
      record.slotIndex = source.slotIndex;
      record.seqId = source.seqId;
      completion = completionSourceForReadySlot(source);
      const std::array sources{completion};
      check(record.assignFixedCompletionSources(sources),
            "completion source assignment must succeed");
      check(queue.queueLifecycle_.submitEncodedSubmission(lock, record),
            "encoded test submission must succeed");
    }

    core::metalqueue::QueueLifecycleController::PendingCompletion pending{};
    pending.slotIndex = completion.slotIndex;
    pending.seqId = completion.seqId;
    const std::array pendingSources{completion};
    check(pending.assignFixedCompletionSources(pendingSources),
          "pending completion assignment must succeed");
    queue.queueLifecycle_.enqueuePendingCompletionForTest(std::move(pending));
    check(queue.queueLifecycle_.processOnePendingCompletion(),
          "test completion must process");
    std::unique_lock lock(queue.mutex_);
    check(queue.queueLifecycle_.runFinishIteration(lock),
          "test completion must reclaim");
    return true;
  }
};

}  // namespace dxmt9

namespace {

struct ReplayDevice final : dxmt9::Device {
  ReplayDevice(bool cpuReady, bool direct)
      : queue_(dxmt9::CommandQueue::ArenaLeaseTestQueueTag{}, limits_),
        cpuReady_(cpuReady),
        direct_(direct) {}

  WMT::Device wmtDevice() override { return WMT::Device{NULL_OBJECT_HANDLE}; }
  dxmt9::CommandQueue& queue() override { return queue_; }
  const BackendLimits& limits() const override { return limits_; }
  std::shared_ptr<BackendDevice> backend() override { return {}; }
  bool supportsCpuReadyArenaReplay() const noexcept override { return cpuReady_; }
  bool supportsDirectChunkSlotReplay() const noexcept override { return direct_; }

  BufferHandle createBuffer(const BufferDesc& desc) override {
    return queue_.pool().createBuffer(WMT::Device{NULL_OBJECT_HANDLE}, desc);
  }
  SurfaceHandle createSurface(const SurfaceDesc&) override {
    return SurfaceHandle{nextHandle_++};
  }

  void markChunkResources(std::span<const ChunkHandleEntry> entries) override {
    queue_.markChunkResources(entries);
  }
  ChunkBufferBindingCaptureResult captureChunkBufferBindings(
      std::span<const ChunkHandleEntry> entries,
      std::vector<ChunkBufferBindingSnapshot>& snapshots) override {
    return queue_.captureChunkBufferBindings(entries, snapshots);
  }
  void submitDrawRun(CanonicalDrawState state,
                     const DrawUniformPayload& uniforms,
                     std::span<const DrawParam> draws,
                     std::span<const DrawParamPayloadView> payloads) override {
    ++ordinaryDrawCalls;
    queue_.submitDrawRun(std::move(state), uniforms, draws, payloads);
  }
  DirectReplayDrawDisposition submitDirectReplayDraw(
      const DirectReplayDrawInput& input) noexcept override {
    const auto disposition = queue_.submitDirectReplayDraw(input);
    if (disposition == DirectReplayDrawDisposition::Appended) ++directDrawCalls;
    else ++rejectedDirectDrawCalls;
    return disposition;
  }
  void present(const SwapDesc& desc) override { queue_.submitPresent(desc); }

  BackendLimits limits_{};
  dxmt9::CommandQueue queue_;
  bool cpuReady_ = false;
  bool direct_ = false;
  std::uint64_t nextHandle_ = 1u;
  std::uint32_t ordinaryDrawCalls = 0u;
  std::uint32_t directDrawCalls = 0u;
  std::uint32_t rejectedDirectDrawCalls = 0u;
};

struct RuntimeFixture {
  RuntimeFixture(bool cpuReady, bool direct) {
    auto device = std::make_unique<ReplayDevice>(cpuReady, direct);
    routing = device.get();
    factory = dxmt9::com::Direct3DCreate9Ex(
        dxmt9::com::D3D_SDK_VERSION, std::move(device));
    check(factory != nullptr, "native replay factory must construct");
    PresentParameters params{};
    params.backBufferWidth = 16;
    params.backBufferHeight = 16;
    params.backBufferFormat = Format::A8R8G8B8;
    params.windowed = true;
    params.deviceWindow = Handle{91};
    params.presentationInterval = PresentInterval::Immediate;
    d3dDevice = factory->CreateDeviceEx(0, params, nullptr);
    check(d3dDevice != nullptr, "native replay device must construct");
    d3dDevice->AddRef();
    cDevice = std::make_unique<D9CDevice>(d3dDevice);
  }
  ~RuntimeFixture() {
    cDevice.reset();
    if (d3dDevice) (void)d3dDevice->Release();
    if (factory) (void)factory->Release();
  }

  ReplayDevice* routing = nullptr;
  dxmt9::com::IDirect3D9Ex* factory = nullptr;
  dxmt9::com::IDirect3DDevice9Ex* d3dDevice = nullptr;
  std::unique_ptr<D9CDevice> cDevice;
};

// Scanner fixture, not a GPU shader oracle. The current usage analysis keeps
// conservative full float/int files and four bool registers. Real D9CShader
// wrappers still bind qualified registry identities and balanced AddRefs.
std::uint32_t shaderRegisterType(std::uint32_t type) {
  return ((type & 0x7u) << 28) | (((type >> 3) & 0x3u) << 11);
}

std::uint32_t shaderInstruction(std::uint32_t opcode,
                                std::uint32_t operands) {
  return (opcode & 0xffffu) | ((operands & 0xfu) << 24);
}

std::uint32_t shaderDestination(std::uint32_t type, std::uint32_t index) {
  return (1u << 31) | shaderRegisterType(type) | (0xfu << 16) |
         (index & 0x7ffu);
}

std::uint32_t shaderSource(std::uint32_t type, std::uint32_t index) {
  return (1u << 31) | shaderRegisterType(type) | (0xe4u << 16) |
         (index & 0x7ffu);
}

std::vector<std::uint32_t> allConstantShaderCode(bool vertex) {
  using namespace dxmt9::d3d9bc;
  const auto output = vertex ? kD3DSPR_RASTOUT : kD3DSPR_COLOROUT;
  std::vector<std::uint32_t> words{
      ((vertex ? 0xfffeu : 0xffffu) << 16) | 0x0300u};
  if (vertex) {
    words.push_back(shaderInstruction(kD3DSIO_DCL, 2u));
    words.push_back((1u << 31) | 0u);
    words.push_back(shaderDestination(kD3DSPR_INPUT, 0u));
  }
  const std::array<std::uint32_t, 15> body{
      shaderInstruction(kD3DSIO_MOV, 2u),
      shaderDestination(kD3DSPR_TEMP, 0u),
      shaderSource(kD3DSPR_CONST, 3u),
      shaderInstruction(kD3DSIO_IF, 1u),
      shaderSource(kD3DSPR_CONSTBOOL, 3u),
      shaderInstruction(kD3DSIO_MOV, 2u),
      shaderDestination(kD3DSPR_TEMP, 0u),
      shaderSource(kD3DSPR_CONST, 3u),
      shaderInstruction(kD3DSIO_ENDIF, 0u),
      shaderInstruction(kD3DSIO_MOV, 2u),
      shaderDestination(kD3DSPR_TEMP, 1u),
      shaderSource(kD3DSPR_CONSTINT, 3u),
      shaderInstruction(kD3DSIO_MOV, 2u),
      shaderDestination(output, 0u),
      shaderSource(kD3DSPR_TEMP, 0u),
  };
  words.insert(words.end(), body.begin(), body.end());
  words.push_back(kD3DSIO_END);
  return words;
}

void initializeShader(D9CShader& shader, bool vertex) {
  const auto words = allConstantShaderCode(vertex);
  shader.bytecodeWords = words;
  shader.ref.kind = ShaderRef::Kind::Bytecode;
  shader.ref.bytecode.bytes = bytesOf(words.data(),
                                     words.size() * sizeof(words[0]));
  shader.ref.bytecode.hash = hashBytes(std::as_bytes(
      std::span<const std::uint32_t>(words.data(), words.size())));
  shader.ref.hash = shader.ref.bytecode.hash;
}

struct ConstantState {
  std::array<std::array<std::uint32_t, 4>, 4> vsF{};
  std::array<std::array<std::uint32_t, 4>, 4> vsI{};
  std::array<std::uint32_t, 4> vsB{};
  std::array<std::array<std::uint32_t, 4>, 4> psF{};
  std::array<std::array<std::uint32_t, 4>, 4> psI{};
  std::array<std::uint32_t, 4> psB{};
};

void touchAll(PeConstShadowBlock& constants, ConstantState& expected,
              std::uint32_t seed) {
  const auto fill = [seed](auto& array, std::uint32_t family) {
    for (std::size_t i = 0; i < array.size(); ++i) {
      array[i] = seed * 100u + family * 17u + static_cast<std::uint32_t>(i);
    }
  };
  fill(expected.vsF[0], 1u);
  fill(expected.vsF[1], 1u);
  fill(expected.vsF[2], 1u);
  fill(expected.vsF[3], 1u);
  fill(expected.vsI[0], 2u);
  fill(expected.vsI[1], 2u);
  fill(expected.vsI[2], 2u);
  fill(expected.vsI[3], 2u);
  fill(expected.psF[0], 3u);
  fill(expected.psF[1], 3u);
  fill(expected.psF[2], 3u);
  fill(expected.psF[3], 3u);
  fill(expected.psI[0], 4u);
  fill(expected.psI[1], 4u);
  fill(expected.psI[2], 4u);
  fill(expected.psI[3], 4u);
  for (std::size_t i = 0; i < 4u; ++i) {
    expected.vsB[i] = (i & 1u) ? 0u : seed * 10u + 1u;
    expected.psB[i] = (i & 1u) ? seed * 10u + 2u : 0u;
  }
  const std::uint32_t start = 0u;
  const std::uint32_t count = 4u;
  touchConstShadow(constants.vsConstF, start, count,
                   expected.vsF.data() + start, sizeof(expected.vsF[0]));
  touchConstShadow(constants.vsConstI, start, count,
                   expected.vsI.data() + start, sizeof(expected.vsI[0]));
  touchConstShadow(constants.vsConstB, start, count,
                   expected.vsB.data() + start, sizeof(expected.vsB[0]));
  touchConstShadow(constants.psConstF, start, count,
                   expected.psF.data() + start, sizeof(expected.psF[0]));
  touchConstShadow(constants.psConstI, start, count,
                   expected.psI.data() + start, sizeof(expected.psI[0]));
  touchConstShadow(constants.psConstB, start, count,
                   expected.psB.data() + start, sizeof(expected.psB[0]));
}

void touchPartial(PeConstShadowBlock& constants, ConstantState& expected,
                  std::uint32_t seed, std::uint32_t familyMask) {
  // Two disjoint writes merge across unchanged register two. Repeat register
  // one with another value before the draw to pin last-write-wins as well.
  for (const std::uint32_t i : {1u, 3u, 1u}) {
    const auto updateVector = [&](auto& rows, ConstShadow& shadow,
                                  std::uint32_t family) {
      if ((familyMask & (1u << family)) == 0u) return;
      for (std::size_t lane = 0; lane < 4u; ++lane) {
        rows[i][lane] = seed * 100u + family * 17u + i * 7u +
                        static_cast<std::uint32_t>(lane);
      }
      touchConstShadow(shadow, i, 1u, rows[i].data(), sizeof(rows[i]));
    };
    updateVector(expected.vsF, constants.vsConstF, 0u);
    updateVector(expected.vsI, constants.vsConstI, 1u);
    updateVector(expected.psF, constants.psConstF, 3u);
    updateVector(expected.psI, constants.psConstI, 4u);
    if (familyMask & (1u << 2u)) {
      expected.vsB[i] = seed * 10u + 1u;
      touchConstShadow(constants.vsConstB, i, 1u, &expected.vsB[i],
                       sizeof(std::uint32_t));
    }
    if (familyMask & (1u << 5u)) {
      expected.psB[i] = 0u;
      touchConstShadow(constants.psConstB, i, 1u, &expected.psB[i],
                       sizeof(std::uint32_t));
    }
    ++seed;
  }
}

std::array<DrawSnapshot, 4> expectedSnapshots(
    const std::array<ConstantState, 4>& states) {
  constexpr std::size_t vsFloatRegisters = 256u;
  constexpr std::size_t psFloatRegisters = 224u;
  constexpr std::size_t intRegisters = 16u;
  constexpr std::size_t boolRegisters = 4u;
  std::array<DrawSnapshot, 4> result{};
  for (std::size_t i = 0; i < states.size(); ++i) {
    const auto& s = states[i];
    result[i].indexed = (i & 1u) != 0u;
    result[i].drawStart = static_cast<std::uint32_t>(i);
    result[i].baseVertex = result[i].indexed ? -1 : 0;
    result[i].vsCounts = {vsFloatRegisters, intRegisters, boolRegisters};
    result[i].psCounts = {psFloatRegisters, intRegisters, boolRegisters};
    result[i].vsBytes.resize(vsFloatRegisters * 16u + intRegisters * 16u +
                             boolRegisters);
    result[i].psBytes.resize(psFloatRegisters * 16u + intRegisters * 16u +
                             boolRegisters);
    std::size_t v = 0u;
    for (const auto& row : s.vsF) {
      std::memcpy(result[i].vsBytes.data() + v, row.data(), sizeof(row));
      v += sizeof(row);
    }
    v = vsFloatRegisters * 16u;
    for (const auto& row : s.vsI) {
      std::memcpy(result[i].vsBytes.data() + v, row.data(), sizeof(row));
      v += sizeof(row);
    }
    v = vsFloatRegisters * 16u + intRegisters * 16u;
    for (std::size_t boolIndex = 0u; boolIndex < s.vsB.size(); ++boolIndex) {
      result[i].vsBytes[v + boolIndex] = s.vsB[boolIndex] != 0u ? 1u : 0u;
    }
    v = 0u;
    for (const auto& row : s.psF) {
      std::memcpy(result[i].psBytes.data() + v, row.data(), sizeof(row));
      v += sizeof(row);
    }
    v = psFloatRegisters * 16u;
    for (const auto& row : s.psI) {
      std::memcpy(result[i].psBytes.data() + v, row.data(), sizeof(row));
      v += sizeof(row);
    }
    v = psFloatRegisters * 16u + intRegisters * 16u;
    for (std::size_t boolIndex = 0u; boolIndex < s.psB.size(); ++boolIndex) {
      result[i].psBytes[v + boolIndex] = s.psB[boolIndex] != 0u ? 1u : 0u;
    }
  }
  return result;
}

void retainWireObject(std::uint32_t kind, void* object) noexcept {
  if (!object) return;
  switch (kind) {
    case D9C_CHUNK_HANDLE_KIND_SHADER:
      dxmt9c_shader_addref(static_cast<D9CShader*>(object));
      break;
    case D9C_CHUNK_HANDLE_KIND_BUFFER:
      dxmt9c_buffer_addref(static_cast<D9CBuffer*>(object));
      break;
    case D9C_CHUNK_HANDLE_KIND_SURFACE:
      dxmt9c_surface_addref(static_cast<D9CSurface*>(object));
      break;
    default:
      break;
  }
}

void appendStandaloneConstants(pe::PeSemanticBatchOwner<>& owner,
                               PeConstShadowBlock& constants,
                               PeHotStateShadow& shadow,
                               std::uint64_t& ordinal) {
  struct Family {
    ConstShadow* shadow;
    pe::PeSemanticProducerKind producer;
    std::uint32_t type;
    std::size_t elemBytes;
  } families[] = {
      {&constants.vsConstF, pe::PeSemanticProducerKind::VsFloatConstant,
       D9C_COMMAND_RECORD_SET_VS_CONST_F, 16u},
      {&constants.vsConstI, pe::PeSemanticProducerKind::VsIntConstant,
       D9C_COMMAND_RECORD_SET_VS_CONST_I, 16u},
      {&constants.vsConstB, pe::PeSemanticProducerKind::VsBoolConstant,
       D9C_COMMAND_RECORD_SET_VS_CONST_B, 4u},
      {&constants.psConstF, pe::PeSemanticProducerKind::PsFloatConstant,
       D9C_COMMAND_RECORD_SET_PS_CONST_F, 16u},
      {&constants.psConstI, pe::PeSemanticProducerKind::PsIntConstant,
       D9C_COMMAND_RECORD_SET_PS_CONST_I, 16u},
      {&constants.psConstB, pe::PeSemanticProducerKind::PsBoolConstant,
       D9C_COMMAND_RECORD_SET_PS_CONST_B, 4u},
  };
  for (const auto& family : families) {
    if (!family.shadow->dirty()) continue;
    const auto count = family.shadow->dirtyEnd - family.shadow->dirtyStart;
    const D9CCommandChunkWireSetConst setConst{
        .startRegister = family.shadow->dirtyStart,
        .registerCount = count,
    };
    const auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(family.shadow->values.data() +
                                           family.shadow->dirtyStart *
                                               family.elemBytes),
        static_cast<std::size_t>(count) * family.elemBytes);
    const auto recordOrdinal = ++ordinal;
    const pe::PeSemanticConstantRecordInput input{
        .producer = family.producer,
        .recordType = family.type,
        .sourceOrdinal = recordOrdinal,
        .recordOrdinal = recordOrdinal,
        .setConst = setConst,
        .constantBytes = bytes,
    };
    pe::PeSemanticConstantSettlement settlement(constants, shadow, {});
    check(owner.tryAppendConstantRecord(input, settlement),
          "standalone constant record must append");
  }
}

std::vector<DrawSnapshot> produceAndReplay(bool inlineConstants, bool direct,
                                           std::uint32_t familyMask,
                                           std::uint32_t& directCalls,
                                           std::uint32_t& ordinaryCalls,
                                           CaptureIdentityReceipt*
                                               captureReceipt = nullptr) {
  RuntimeFixture fixture(/*cpuReady=*/direct, direct);
  D9CShader vs(&*fixture.cDevice);
  D9CShader ps(&*fixture.cDevice);
  initializeShader(vs, true);
  initializeShader(ps, false);

  pe::PeBindingView bindings{};
  bindings.vs.identity = vs.wireIdentity;
  bindings.vs.object = &vs;
  bindings.ps.identity = ps.wireIdentity;
  bindings.ps.object = &ps;
  PeHotStateShadow shadow{};
  shadow.writer().pendingVs() = true;
  shadow.writer().pendingPs() = true;
  PeConstShadowBlock constants{};
  pe::PeDrawPayloads payloads{};
  pe::PeChunkContext chunk{
      .retainedStreamMask = 0u,
      .indexBufferKnown = true,
      .submittedIndexBufferWire = 0u,
      .indexBufferRetained = false,
  };
  pe::PeSemanticBatchOwner<> owner;
  std::recursive_mutex mutex;
  ThreadOwnershipToken ownerToken;
  std::uint64_t epoch = 1u;
  pe::RecorderLockGuard guard(mutex, false);
  std::array<ConstantState, 4> expected{};
  std::uint64_t ordinal = 0u;

  for (std::size_t draw = 0; draw < 4u; ++draw) {
    if (draw == 0u) {
      touchAll(constants, expected[draw], 1u);
    } else if (draw == 2u) {
      expected[draw] = expected[1];
      touchPartial(constants, expected[draw], 2u, familyMask);
    } else if (draw == 3u) {
      expected[draw] = expected[2];
      touchAll(constants, expected[draw], 1u);
    } else {
      expected[draw] = expected[draw - 1u];
    }

    if (!inlineConstants && draw != 1u) {
      appendStandaloneConstants(owner, constants, shadow, ordinal);
    }

    pe::PeDrawParams params{
        .recordType = draw & 1u ? D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE
                                : D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
        .primitiveType = 4u,
        .baseVertex = draw & 1u ? -1 : 0,
        .numVertices = 3u,
        .startVertex = static_cast<std::uint32_t>(draw),
        .startIndex = static_cast<std::uint32_t>(draw),
        .primitiveCount = 1u,
        .stride = 16u,
        .indexFormat = 16u,
    };
    pe::SparseStatePlan plan;
    bool built = false;
    check(guard.withCapability(ownerToken, epoch, [&](const auto& access) noexcept {
      built = pe::buildSparseStatePlan(access, shadow, constants, bindings,
                                       payloads, params, false,
                                       inlineConstants, plan);
      return built && pe::finalizeSparseStatePlanChunkContext(access, chunk,
                                                               plan);
    }) && built,
          "compact plan must build");
    const auto settlement = pe::settleRecorderAppend({
        .phase = pe::AppendSettlement::Prepared,
        .appendSucceeded = true,
    });
    const auto drawOrdinal = ++ordinal;
    const bool drawAppended = guard.withCapability(
        ownerToken, epoch, [&](const auto& access) noexcept {
      return owner.tryAppendSparseStatePlan(
          access, plan,
          params.recordType == D9C_COMMAND_RECORD_DRAW_PRIMITIVE
              ? pe::PeSemanticProducerKind::DrawPrimitive
              : pe::PeSemanticProducerKind::DrawIndexedPrimitive,
          params.recordType, D9C_COMMAND_CHUNK_RECORD_FLAG_NONE, drawOrdinal,
          drawOrdinal, [&]() noexcept {
            const auto* source = plan.source(access);
            return source && pe::acceptSparseStatePlan(
                                 access, shadow, constants, plan, settlement,
                                 nullptr, drawOrdinal, source->pendingTicket);
          });
    });
    if (!drawAppended) {
      throw TestFailure("draw record must append and settle; draw=" +
                        std::to_string(draw) + " failure=" +
                        std::to_string(static_cast<unsigned>(
                            owner.lastAdmissionFailure())));
    }
  }

  pe::PeSemanticExactFixedEmission exact;
  check(owner.emitExactFixed(exact) && exact.valid(),
        "owner must emit exact fixed wire");
  const dxmt9::d3d9::CommandChunkEnvelope envelope{
      .version = exact.transport.header.version,
      .recordCount = exact.transport.header.recordCount,
      .handleCount = exact.transport.header.handleCount,
      .producerIdentity = exact.transport.producerIdentity,
  };
  dxmt9::d3d9::ImportedChunkView imported{};
  check(dxmt9::d3d9::validateCommandChunk(exact.wire, envelope, &imported)
            .valid(),
        "owner wire must pass canonical validation before replay");
  const auto standaloneCount = static_cast<std::size_t>(std::count_if(
      imported.records.begin(), imported.records.end(), [](const auto& record) {
        return record.type >= D9C_COMMAND_RECORD_SET_VS_CONST_F &&
               record.type <= D9C_COMMAND_RECORD_SET_PS_CONST_B;
      }));
  const auto expectedStandaloneCount = 6u + 2u * std::popcount(familyMask);
  check((inlineConstants && imported.records.size() == 4u &&
         standaloneCount == 0u) ||
            (!inlineConstants && imported.records.size() == 4u + expectedStandaloneCount &&
             standaloneCount == expectedStandaloneCount),
        "wire shape must distinguish inline sections from standalone records");
  if (captureReceipt) {
    check(direct, "capture identity binding requires direct arena replay");
    check(captureReceipt->captureToken != 0u &&
              captureReceipt->eventOrdinal != 0u,
          "capture identity binding requires a token/event pair");
    const D9CCommandChunk captureChunk{
        .version = envelope.version,
        .recordCount = envelope.recordCount,
        .recordBytes = exact.wireBytes,
        .records = toWireHandle(exact.wire.data()),
        .handleCount = envelope.handleCount,
        .handles = {},
        .renderTapeCaptureToken = captureReceipt->captureToken,
        .renderTapeEventOrdinal = captureReceipt->eventOrdinal,
        .producerIdentity = envelope.producerIdentity,
    };
    captureReceipt->emittedRecordCount = envelope.recordCount;
    check(dxmt9c_device_commit_chunk(fixture.cDevice.get(), &captureChunk) ==
              D3D_OK,
          "production descriptor commit must accept capture-bound owner wire");
    check(dxmt9::d3d9::drainDeferredReplay(
              fixture.cDevice.get(), "inline-constant-capture-binding"),
          "capture-bound production commit must drain its replay worker");
  } else {
    dxmt9::d3d9::RawCommandChunk raw;
    check(dxmt9::d3d9::prepareOffloadChunk(
              exact.wire, envelope, fixture.cDevice->wireObjects,
              retainWireObject, raw),
          "exact owner wire must pass production offload preparation");
    raw.replaySeq = 1u;
    raw.cpuReadyTapePlanningEnabled = direct;
    check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw) == D3D_OK,
          "production replay must accept compact owner wire");
  }
  fixture.routing->present(SwapDesc{});
  std::vector<DrawSnapshot> actual;
  const auto ready = dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
      fixture.routing->queue_);
  check(ready != 0u, "replay must publish a bounded ready source");
  for (std::size_t i = 0; i < ready; ++i) {
    check(dxmt9::CommandQueueArenaLeaseTestAccess::consumeOne(
              fixture.routing->queue_, actual),
          "ready source must complete");
  }
  if (captureReceipt) {
    D9CRenderTapeIdentityCaptureResult query{};
    check(dxmt9c_device_finish_render_tape_identity_capture(
              fixture.cDevice.get(), captureReceipt->captureToken, &query,
              nullptr, 0u) == D3D_OK &&
              query.status == D9C_RENDER_TAPE_IDENTITY_CAPTURE_COMPLETE &&
              query.captureToken == captureReceipt->captureToken &&
              query.eventOrdinal == captureReceipt->eventOrdinal &&
              query.sourceCount != 0u && query.rangeCount != 0u &&
              query.settlementCount != 0u && query.byteCount != 0u,
          "production capture identity must finish with nonzero source rows");
    captureReceipt->identity = query;
    captureReceipt->identityBytes.resize(
        static_cast<std::size_t>(query.byteCount));
    D9CRenderTapeIdentityCaptureResult copied{};
    check(dxmt9c_device_finish_render_tape_identity_capture(
              fixture.cDevice.get(), captureReceipt->captureToken, &copied,
              captureReceipt->identityBytes.data(),
              captureReceipt->identityBytes.size()) == D3D_OK &&
              copied.status == D9C_RENDER_TAPE_IDENTITY_CAPTURE_COMPLETE &&
              copied.byteCount == query.byteCount &&
              copied.sourceCount == query.sourceCount &&
              copied.rangeCount == query.rangeCount &&
              copied.settlementCount == query.settlementCount,
          "production capture identity exact copy must complete");
    const auto sourceBytes = static_cast<std::size_t>(copied.sourceCount) *
                             sizeof(D9CRenderTapeIdentitySourceEntry);
    const auto rangeBytes = static_cast<std::size_t>(copied.rangeCount) *
                            sizeof(D9CRenderTapeIdentityRangeEntry);
    const auto settlementBytes =
        static_cast<std::size_t>(copied.settlementCount) *
        sizeof(D9CRenderTapeIdentitySettlementEntry);
    check(copied.settlementTableOffset == sourceBytes + rangeBytes &&
              copied.byteCount == sourceBytes + rangeBytes + settlementBytes,
          "production capture identity rows must have exact table extent");
    std::size_t sourceRecordCount = 0u;
    for (std::size_t i = 0; i < copied.sourceCount; ++i) {
      D9CRenderTapeIdentitySourceEntry source{};
      std::memcpy(&source,
                  captureReceipt->identityBytes.data() +
                      i * sizeof(source),
                  sizeof(source));
      check(source.captureToken == captureReceipt->captureToken &&
                source.eventOrdinal == captureReceipt->eventOrdinal &&
                source.recordCount != 0u,
            "production capture identity source rows retain token/event");
      sourceRecordCount += source.recordCount;
    }
    check(sourceRecordCount == captureReceipt->emittedRecordCount,
          "capture identity source rows cover the exact owner record count");
  }
  check(actual.size() == 4u, "replay must expose all four logical draws");
  const auto expectedDraws = expectedSnapshots(expected);
  for (std::size_t i = 0; i < actual.size(); ++i) {
    if (!(actual[i] == expectedDraws[i])) {
      const auto firstDifference = [](const std::vector<u8>& left,
                                      const std::vector<u8>& right) {
        std::size_t i = 0u;
        while (i < left.size() && i < right.size() && left[i] == right[i]) ++i;
        return i;
      };
      std::fprintf(stderr,
                   "draw %zu mismatch: VS counts=%u/%u/%u expected=%u/%u/%u "
                   "PS counts=%u/%u/%u expected=%u/%u/%u VS bytes=%zu/%zu "
                   "PS bytes=%zu/%zu first=%zu/%zu\n",
                   i, actual[i].vsCounts[0], actual[i].vsCounts[1],
                   actual[i].vsCounts[2], expectedDraws[i].vsCounts[0],
                   expectedDraws[i].vsCounts[1], expectedDraws[i].vsCounts[2],
                   actual[i].psCounts[0], actual[i].psCounts[1],
                   actual[i].psCounts[2], expectedDraws[i].psCounts[0],
                   expectedDraws[i].psCounts[1], expectedDraws[i].psCounts[2],
                   actual[i].vsBytes.size(), expectedDraws[i].vsBytes.size(),
                   actual[i].psBytes.size(), expectedDraws[i].psBytes.size(),
                   firstDifference(actual[i].vsBytes, expectedDraws[i].vsBytes),
                   firstDifference(actual[i].psBytes, expectedDraws[i].psBytes));
      throw TestFailure("inline and standalone paths must preserve logical uniform bytes");
    }
    check(actual[i].vsCounts == std::array<std::uint16_t, 3>{256u, 16u, 4u} &&
              actual[i].psCounts == std::array<std::uint16_t, 3>{224u, 16u, 4u},
          "all six shader constant families must remain physically present");
  }
  check(fixture.routing->rejectedDirectDrawCalls == 0u,
        "direct receipts must exclude fallback and accepted-fail-stop");
  directCalls = fixture.routing->directDrawCalls;
  ordinaryCalls = fixture.routing->ordinaryDrawCalls;
  return actual;
}

void runGateMatrix() {
  for (std::uint32_t mask = 0u; mask < 64u; ++mask) {
    for (const bool direct : {false, true}) {
      try {
        std::uint32_t onDirect = 0u, onOrdinary = 0u;
        std::uint32_t offDirect = 0u, offOrdinary = 0u;
        const auto on = produceAndReplay(true, direct, mask, onDirect, onOrdinary);
        const auto off = produceAndReplay(false, direct, mask, offDirect, offOrdinary);
        check(on == off, "inline gate must be byte-equivalent at final draw state");
        if (direct) {
          check(onDirect == 4u && offDirect == 4u && onOrdinary == 0u &&
                    offOrdinary == 0u,
                "direct-slot mode must use four accepted direct draws");
        } else {
          check(onOrdinary == 4u && offOrdinary == 4u && onDirect == 0u &&
                    offDirect == 0u,
                "compatibility mode must use four ordinary submissions");
        }
      } catch (const TestFailure& failure) {
        throw TestFailure("mask=" + std::to_string(mask) +
                          " direct=" + std::to_string(direct) + ": " +
                          failure.what());
      }
    }
  }
}

void runCaptureIdentityBindingCase() {
  CaptureIdentityReceipt onReceipt{
      .captureToken = 701u,
      .eventOrdinal = 11u,
  };
  CaptureIdentityReceipt offReceipt{
      .captureToken = 702u,
      .eventOrdinal = 11u,
  };
  std::uint32_t onDirect = 0u;
  std::uint32_t onOrdinary = 0u;
  std::uint32_t offDirect = 0u;
  std::uint32_t offOrdinary = 0u;
  const auto on = produceAndReplay(true, true, 0x3fu, onDirect, onOrdinary,
                                   &onReceipt);
  const auto off = produceAndReplay(false, true, 0x3fu, offDirect,
                                    offOrdinary, &offReceipt);
  check(on == off,
        "capture-bound inline and standalone draws preserve effective files");
  check(onDirect == 4u && offDirect == 4u && onOrdinary == 0u &&
            offOrdinary == 0u,
        "capture-bound cases remain direct four-draw productions");
  check(onReceipt.emittedRecordCount == 4u &&
            offReceipt.emittedRecordCount == 22u &&
            onReceipt.identity.sourceCount != 0u &&
            offReceipt.identity.sourceCount != 0u &&
            onReceipt.identity.captureToken == onReceipt.captureToken &&
            offReceipt.identity.captureToken == offReceipt.captureToken,
        "capture-bound identity rows distinguish inline and standalone wire shape");
}

void failedStandaloneAppendCanRetry() {
  // A standalone range whose bytes do not match its declared count must fail
  // admission without consuming dirty state. This does not claim late-section
  // importer or post-retain rollback coverage.
  PeConstShadowBlock constants{};
  PeHotStateShadow shadow{};
  const std::array<std::uint32_t, 4> value{1u, 2u, 3u, 4u};
  touchConstShadow(constants.vsConstF, 0u, 1u, value.data(), sizeof(value));
  pe::PeSemanticBatchOwner<> owner;
  const std::array<std::byte, sizeof(value)> bytes = [&] {
    std::array<std::byte, sizeof(value)> result{};
    std::memcpy(result.data(), value.data(), result.size());
    return result;
  }();
  const pe::PeSemanticConstantRecordInput malformed{
      .producer = pe::PeSemanticProducerKind::VsFloatConstant,
      .recordType = D9C_COMMAND_RECORD_SET_VS_CONST_F,
      .sourceOrdinal = 1u,
      .recordOrdinal = 1u,
      .setConst = {.startRegister = 0u, .registerCount = 2u},
      .constantBytes = bytes,
  };
  pe::PeSemanticConstantSettlement failedSettlement(constants, shadow, {});
  check(!owner.tryAppendConstantRecord(malformed, failedSettlement) &&
            constants.vsConstF.dirty(),
        "malformed constant append must preserve its pending shadow");
  const pe::PeSemanticConstantRecordInput retry{
      .producer = pe::PeSemanticProducerKind::VsFloatConstant,
      .recordType = D9C_COMMAND_RECORD_SET_VS_CONST_F,
      .sourceOrdinal = 1u,
      .recordOrdinal = 2u,
      .setConst = {.startRegister = 0u, .registerCount = 1u},
      .constantBytes = std::span<const std::byte>(bytes),
  };
  pe::PeSemanticConstantSettlement retrySettlement(constants, shadow, {});
  check(owner.tryAppendConstantRecord(retry, retrySettlement) &&
            !constants.vsConstF.dirty(),
        "the same pending constant must settle on a valid retry");
}

}  // namespace

int main() {
  try {
    failedStandaloneAppendCanRetry();
    runGateMatrix();
    runCaptureIdentityBindingCase();
    return 0;
  } catch (const TestFailure& failure) {
    std::fprintf(stderr, "inline constant replay spec: %s\n", failure.what());
    return 1;
  }
}
