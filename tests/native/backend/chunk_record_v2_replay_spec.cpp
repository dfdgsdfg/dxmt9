#include "device_c_chunk_v2_replay.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using dxmt9::d3d9::NonDrawReplaySinkV2;
using dxmt9::d3d9::ResolvedRecordV2View;
using dxmt9::d3d9::kCommandChunkV2DecodeFailure;
using dxmt9::d3d9::replayNonDrawRecordV2;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

template <typename T>
std::vector<std::byte> bytesOf(const T& value) {
  std::vector<std::byte> bytes(sizeof(T));
  std::memcpy(bytes.data(), &value, sizeof(value));
  return bytes;
}

template <typename T>
std::vector<std::byte> bytesWithTail(const T& value,
                                     std::span<const std::byte> tail) {
  auto bytes = bytesOf(value);
  bytes.insert(bytes.end(), tail.begin(), tail.end());
  return bytes;
}

ResolvedRecordV2View makeRecord(std::uint32_t type,
                                std::span<const std::byte> payload,
                                std::span<void* const> objects = {},
                                std::uint32_t firstHandle = 0u) {
  return ResolvedRecordV2View{
      .wire = dxmt9::d3d9::ImportedRecordV2View{
          .header = D9CCommandChunkWireRecordHeaderV2{
              .type = type,
              .flags = 0u,
              .payloadOffset = 0u,
              .payloadSize = static_cast<std::uint32_t>(payload.size()),
              .firstHandle = firstHandle,
              .handleCount = static_cast<std::uint32_t>(objects.size()),
          },
          .payload = payload,
      },
      .objects = objects,
  };
}

class RecordingSink final : public NonDrawReplaySinkV2 {
 public:
  std::array<std::uint32_t, 30> calls{};
  std::uint32_t currentType = 0u;
  std::uint32_t start = 0u;
  std::uint32_t count = 0u;
  std::size_t byteCount = 0u;
  void* firstObject = nullptr;
  void* secondObject = nullptr;

  std::int32_t result(std::uint32_t type) {
    currentType = type;
    ++calls[type];
    return static_cast<std::int32_t>(1000u + type);
  }

  std::int32_t setConstants(
      std::uint32_t type, const D9CCommandChunkWireSetConstV2& fixed,
      std::span<const std::byte> registerBytes) override {
    start = fixed.startRegister;
    count = fixed.registerCount;
    byteCount = registerBytes.size();
    return result(type);
  }

  std::int32_t clear(
      const D9CCommandChunkWireClearV2&,
      std::span<const D9CRect> rects) override {
    count = static_cast<std::uint32_t>(rects.size());
    return result(D9C_COMMAND_RECORD_CLEAR);
  }

  std::int32_t present(
      const D9CCommandChunkWirePresentV2&) override {
    return result(D9C_COMMAND_RECORD_PRESENT);
  }

  std::int32_t stretchRect(
      const D9CCommandChunkWireStretchRectV2&, void* src,
      void* dst) override {
    firstObject = src;
    secondObject = dst;
    return result(D9C_COMMAND_RECORD_STRETCH_RECT);
  }

  std::int32_t colorFill(
      const D9CCommandChunkWireColorFillV2&, void* surface) override {
    firstObject = surface;
    return result(D9C_COMMAND_RECORD_COLOR_FILL);
  }

  std::int32_t updateTexture(
      const D9CCommandChunkWireUpdateTextureV2&, void* src,
      void* dst) override {
    firstObject = src;
    secondObject = dst;
    return result(D9C_COMMAND_RECORD_UPDATE_TEXTURE);
  }

  std::int32_t updateSurface(
      const D9CCommandChunkWireUpdateSurfaceV2&, void* src,
      void* dst) override {
    firstObject = src;
    secondObject = dst;
    return result(D9C_COMMAND_RECORD_UPDATE_SURFACE);
  }

  std::int32_t queryIssue(
      const D9CCommandChunkWireQueryIssueV2&, void* query) override {
    firstObject = query;
    return result(D9C_COMMAND_RECORD_QUERY_ISSUE);
  }

  std::int32_t readback(
      const D9CCommandChunkWireReadbackV2&, void* src,
      void* dst) override {
    firstObject = src;
    secondObject = dst;
    return result(D9C_COMMAND_RECORD_READBACK);
  }

  std::int32_t reszDepthResolve(
      const D9CCommandChunkWireReszDepthResolveV2&, void* msaaDepth,
      void* intzDest) override {
    firstObject = msaaDepth;
    secondObject = intzDest;
    return result(D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE);
  }

  std::int32_t applyState(
      const ResolvedRecordV2View&) override {
    return result(D9C_COMMAND_RECORD_APPLY_STATE);
  }
};

void testConstantReplayMatrix() {
  RecordingSink sink;
  const std::array types = {
      D9C_COMMAND_RECORD_SET_VS_CONST_F,
      D9C_COMMAND_RECORD_SET_VS_CONST_I,
      D9C_COMMAND_RECORD_SET_VS_CONST_B,
      D9C_COMMAND_RECORD_SET_PS_CONST_F,
      D9C_COMMAND_RECORD_SET_PS_CONST_I,
      D9C_COMMAND_RECORD_SET_PS_CONST_B,
  };
  std::array<std::byte, 16> data{};
  for (const auto type : types) {
    const auto bytes =
        type == D9C_COMMAND_RECORD_SET_VS_CONST_B ||
                type == D9C_COMMAND_RECORD_SET_PS_CONST_B
            ? std::span<const std::byte>(data).first(4u)
            : std::span<const std::byte>(data);
    const D9CCommandChunkWireSetConstV2 fixed{3u, 1u};
    const auto payload = bytesWithTail(fixed, bytes);
    const auto status = replayNonDrawRecordV2(
        makeRecord(type, payload), sink);
    check(status == static_cast<std::int32_t>(1000u + type) &&
              sink.start == 3u && sink.count == 1u &&
              sink.byteCount == bytes.size(),
          "constant replay preserves type, range, bytes, and sink HRESULT");
  }
}

void testOrderingAndResourceReplayMatrix() {
  RecordingSink sink;
  const std::array rects = {D9CRect{0, 0, 4, 4}, D9CRect{4, 4, 8, 8}};
  D9CCommandChunkWireClearV2 clear{};
  clear.rectCount = static_cast<std::uint32_t>(rects.size());
  clear.rectOffset = sizeof(clear);
  const auto clearPayload = bytesWithTail(clear, std::as_bytes(std::span(rects)));
  check(replayNonDrawRecordV2(
            makeRecord(D9C_COMMAND_RECORD_CLEAR, clearPayload), sink) ==
            1000 + D9C_COMMAND_RECORD_CLEAR &&
            sink.count == rects.size(),
        "Clear replay exposes bounded rect span");

  const D9CCommandChunkWirePresentV2 present{};
  const auto presentPayload = bytesOf(present);
  check(replayNonDrawRecordV2(
            makeRecord(D9C_COMMAND_RECORD_PRESENT, presentPayload), sink) ==
            1000 + D9C_COMMAND_RECORD_PRESENT,
        "Present replay preserves sink boundary result");

  int first = 1;
  int second = 2;
  std::array<void*, 2> objects{&first, &second};
  constexpr std::uint32_t firstHandle = 9u;

  const D9CCommandChunkWireStretchRectV2 stretch{
      .srcHandleIndex = firstHandle,
      .dstHandleIndex = firstHandle + 1u,
  };
  auto payload = bytesOf(stretch);
  check(replayNonDrawRecordV2(
            makeRecord(D9C_COMMAND_RECORD_STRETCH_RECT, payload, objects,
                       firstHandle),
            sink) == 1000 + D9C_COMMAND_RECORD_STRETCH_RECT &&
            sink.firstObject == &first && sink.secondObject == &second,
        "StretchRect absolute indices resolve through record slice");

  const D9CCommandChunkWireColorFillV2 color{
      .surfaceHandleIndex = firstHandle,
  };
  payload = bytesOf(color);
  check(replayNonDrawRecordV2(
            makeRecord(D9C_COMMAND_RECORD_COLOR_FILL, payload,
                       std::span<void* const>(objects).first(1u), firstHandle),
            sink) == 1000 + D9C_COMMAND_RECORD_COLOR_FILL &&
            sink.firstObject == &first,
        "ColorFill resolves its surface descriptor");

  const auto replayTwo = [&](std::uint32_t type, const auto& fixed) {
    const auto fixedBytes = bytesOf(fixed);
    const auto status = replayNonDrawRecordV2(
        makeRecord(type, fixedBytes, objects, firstHandle), sink);
    check(status == static_cast<std::int32_t>(1000u + type) &&
              sink.firstObject == &first && sink.secondObject == &second,
          "two-handle non-draw replay resolves in source order");
  };
  replayTwo(D9C_COMMAND_RECORD_UPDATE_TEXTURE,
            D9CCommandChunkWireUpdateTextureV2{firstHandle,
                                                firstHandle + 1u});
  replayTwo(D9C_COMMAND_RECORD_UPDATE_SURFACE,
            D9CCommandChunkWireUpdateSurfaceV2{
                .srcHandleIndex = firstHandle,
                .dstHandleIndex = firstHandle + 1u,
            });
  replayTwo(D9C_COMMAND_RECORD_READBACK,
            D9CCommandChunkWireReadbackV2{firstHandle,
                                          firstHandle + 1u});
  replayTwo(D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE,
            D9CCommandChunkWireReszDepthResolveV2{firstHandle,
                                                   firstHandle + 1u});

  const D9CCommandChunkWireQueryIssueV2 issue{firstHandle, 1u};
  payload = bytesOf(issue);
  check(replayNonDrawRecordV2(
            makeRecord(D9C_COMMAND_RECORD_QUERY_ISSUE, payload,
                       std::span<void* const>(objects).first(1u), firstHandle),
            sink) == 1000 + D9C_COMMAND_RECORD_QUERY_ISSUE &&
            sink.firstObject == &first,
        "Query::Issue resolves and preserves ordering");

  D9CCommandChunkWireDrawHeaderV2 apply{};
  apply.sectionTableOffset = sizeof(apply);
  apply.sectionPayloadOffset = sizeof(apply);
  payload = bytesOf(apply);
  check(replayNonDrawRecordV2(
            makeRecord(D9C_COMMAND_RECORD_APPLY_STATE, payload), sink) ==
            1000 + D9C_COMMAND_RECORD_APPLY_STATE,
        "APPLY_STATE routes through the sparse-state replay sink");
}

void testRejectsUnresolvedAndDrawRecords() {
  RecordingSink sink;
  const D9CCommandChunkWireUpdateTextureV2 update{0u, 1u};
  const auto payload = bytesOf(update);
  std::array<void*, 2> unresolved{reinterpret_cast<void*>(1u), nullptr};
  check(replayNonDrawRecordV2(
            makeRecord(D9C_COMMAND_RECORD_UPDATE_TEXTURE, payload, unresolved),
            sink) == kCommandChunkV2DecodeFailure,
        "unresolved registry object rejects before sink entry");

  D9CCommandChunkWireDrawHeaderV2 draw{};
  const auto drawPayload = bytesOf(draw);
  check(replayNonDrawRecordV2(
            makeRecord(D9C_COMMAND_RECORD_DRAW_PRIMITIVE, drawPayload), sink) ==
            kCommandChunkV2DecodeFailure,
        "draw opcode cannot enter non-draw replay path");
}

}  // namespace

int main() {
  try {
    testConstantReplayMatrix();
    testOrderingAndResourceReplayMatrix();
    testRejectsUnresolvedAndDrawRecords();
  } catch (const TestFailure& error) {
    std::cerr << "chunk_record_v2_replay_spec failed: " << error.what()
              << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "chunk_record_v2_replay_spec passed\n";
  return EXIT_SUCCESS;
}
