// Device-free spec for the shared render::DagObserver observe + DAG-export
// side-channel (Task B12, L1; backend-agnostic per R-BACK-39.7).
//
// The DAG dump is owned by render::DagObserver, which BOTH the FrameGraph and
// Traditional backends embed (backend.observer()): it builds a
// framegraph::FrameGraph from one core::ChunkSlot, dumps the pre-opt DAG, runs
// the owning backend's resolved optimizer options, and dumps the post-opt DAG.
// The real Metal encode stays on encoders::encodeChunk (byte-identical,
// R-BACK-40.5) and is NOT exercised here — there is no MTLDevice in the native
// test host, so we cannot call onChunkReady. Instead this spec drives the
// observer directly through backend.observer(), asserting the dump files are
// produced (or not) and contain the expected JSON keys.
//
// framegraph::dumpDagDir() caches DXMT9_RENDERER_DUMP_DAG on first read
// (static-const) and cannot be reset in-process. To test BOTH the
// "no dump dir → early-out" and the "export writes both DAGs" behaviors in one
// process, this spec:
//   * tests the early-out through the env-gated observeAndExport while the env
//     var is unset (this also primes the static cache to nullopt), then
//   * tests the export through observeAndExportDagToDir, which takes the dump
//     directory explicitly and therefore bypasses the cache.
//
// Fixture style mirrors tests/native/framegraph/fg_builder_spec.cpp, which also
// builds ChunkSlot SoA records by hand.

#include "../../../src/dxmt9/render/framegraph_backend.hpp"

#include "../../../src/dxmt9/dxmt9_backend_types.hpp"
#include "../../../src/dxmt9/dxmt9_perf_counters.hpp"
#include "../../../src/dxmt9/framegraph/fg_builder.hpp"
#include "../../../src/dxmt9/framegraph/fg_debug_export.hpp"
#include "../../../src/dxmt9/render/dag_observer.hpp"
#include "../../../src/dxmt9/render/traditional_backend.hpp"
#include "../framegraph/arena_payload_fixture.hpp"

#include <unistd.h>  // mkdtemp, rmdir

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using dxmt9::core::ChunkSlot;
using dxmt9::core::ClearDesc;
using dxmt9::core::CommandPayloadIndex;
using dxmt9::core::DrawDebugSnapshot;
using dxmt9::core::DrawRunCommandRecord;
using dxmt9::core::DrawShaderLayoutContext;
using dxmt9::core::FlatDrawStateRecord;
using dxmt9::core::Handle;
using dxmt9::core::MetalCommandHeader;
using dxmt9::core::MetalCommandKind;

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

using dxmt9::render::DagObserver;
using dxmt9::render::FrameGraphBackend;
using dxmt9::render::TraditionalBackend;

// The DAG observe path now lives in the shared render::DagObserver, owned by
// both backends (backend.observer()). onChunkReady itself is device-gated and
// the backends are `final`, so the test drives observer() directly.

// --- ChunkSlot SoA fixture helpers (subset of fg_builder_spec.cpp) ----------

void appendDrawRun(ChunkSlot& slot, Handle color0, Handle depth,
                   Handle sampled = {}, std::uint32_t paramCount = 1u) {
  FlatDrawStateRecord hot{};
  hot.colorAttachments[0].handle = color0;
  hot.depthStencil.handle = depth;
  if (color0.value != 0) {
    hot.renderTargetMask = 1u;
  }
  if (sampled.value != 0) {
    hot.textures[0] = sampled;
    hot.textureMask = 1u;
  }
  const auto stateIndex = static_cast<std::uint32_t>(slot.drawHotStates.size());
  slot.drawHotStates.push_back(hot);
  slot.drawShaderLayouts.push_back(DrawShaderLayoutContext{});
  slot.drawDebugSnapshots.push_back(DrawDebugSnapshot{});

  const auto recordIndex =
      static_cast<std::uint32_t>(slot.drawRunRecords.size());
  const auto firstParam = static_cast<std::uint32_t>(slot.drawParams.size());
  for (std::uint32_t i = 0; i < paramCount; ++i) {
    slot.drawParams.push_back(dxmt9::core::DrawParam{});
  }
  DrawRunCommandRecord record{};
  record.stateIndex = stateIndex;
  record.firstParam = firstParam;
  record.paramCount = paramCount;
  slot.drawRunRecords.push_back(record);
  slot.commandHeaders.push_back(MetalCommandHeader{
      .kind = MetalCommandKind::DrawRun,
      .payloadIndex = CommandPayloadIndex::fromU32(recordIndex),
  });
}

// A single draw run with KNOWN per-draw detail, for the DXMT9_RENDERER_DUMP_-
// DAG_DRAWS extension test: primitive type/count on each DrawParam ordinal,
// VS/PS hash on the debug snapshot, texture mask + a couple render states +
// stream0 stride on the hot record.
void appendKnownDrawRun(ChunkSlot& slot, Handle color0, Handle depth,
                        dxmt9::core::PrimitiveType primType,
                        std::uint32_t primCount, std::uint64_t vsHash,
                        std::uint64_t psHash, std::uint32_t alphaBlend,
                        std::uint32_t stream0Stride,
                        std::uint32_t paramCount = 1u) {
  FlatDrawStateRecord hot{};
  hot.colorAttachments[0].handle = color0;
  hot.depthStencil.handle = depth;
  if (color0.value != 0) {
    hot.renderTargetMask = 1u;
  }
  hot.textures[0] = Handle{0x7777u};
  hot.textureMask = 1u;
  hot.streamStrides[0] = stream0Stride;
  // Render states via the same FlatStateSet the encoder reads with flatStateOr.
  // FlatStateSet must stay sorted ascending by state id (findFlatState binary
  // searches): RS_Z_ENABLE=7 before RS_ALPHABLEND_ENABLE=27.
  hot.renderStates.entries[0] = {dxmt9::core::RS_Z_ENABLE, 1u};
  hot.renderStates.entries[1] = {dxmt9::core::RS_ALPHABLEND_ENABLE, alphaBlend};
  hot.renderStates.count = 2u;

  DrawDebugSnapshot debug{};
  debug.vertexShaderHash = vsHash;
  debug.pixelShaderHash = psHash;

  const auto stateIndex = static_cast<std::uint32_t>(slot.drawHotStates.size());
  slot.drawHotStates.push_back(hot);
  slot.drawShaderLayouts.push_back(DrawShaderLayoutContext{});
  slot.drawDebugSnapshots.push_back(debug);

  const auto recordIndex =
      static_cast<std::uint32_t>(slot.drawRunRecords.size());
  const auto firstParam = static_cast<std::uint32_t>(slot.drawParams.size());
  for (std::uint32_t i = 0; i < paramCount; ++i) {
    dxmt9::core::DrawParam param{};
    param.primitiveType = primType;
    param.primitiveCount = primCount;
    slot.drawParams.push_back(param);
  }
  DrawRunCommandRecord record{};
  record.stateIndex = stateIndex;
  record.firstParam = firstParam;
  record.paramCount = paramCount;
  slot.drawRunRecords.push_back(record);
  slot.commandHeaders.push_back(MetalCommandHeader{
      .kind = MetalCommandKind::DrawRun,
      .payloadIndex = CommandPayloadIndex::fromU32(recordIndex),
  });
}

void appendClearColor(ChunkSlot& slot, Handle color0, Handle depth = {}) {
  ClearDesc desc{};
  desc.colorAttachments[0].handle = color0;
  desc.clearColor = true;
  if (depth.value != 0) {
    desc.depthStencil.handle = depth;
    desc.clearDepth = true;
  }
  slot.appendClear(desc);
}

void appendPresent(ChunkSlot& slot, Handle source) {
  slot.appendPresent(dxmt9::core::SwapDesc{}, source);
}

ChunkSlot buildScenario(std::uint64_t seqId) {
  ChunkSlot slot;
  slot.seqId = seqId;
  const Handle rt0{0xA000u};
  const Handle ds{0xD000u};
  const Handle rt1{0xB000u};
  appendClearColor(slot, rt0, ds);
  appendDrawRun(slot, rt0, ds, /*sampled=*/{}, /*paramCount=*/3);
  appendDrawRun(slot, rt0, ds, /*sampled=*/{}, /*paramCount=*/2);
  appendDrawRun(slot, rt1, {}, /*sampled=*/rt0, /*paramCount=*/1);
  appendPresent(slot, rt1);
  return slot;
}

// A coalesceable re-entry chunk (mirrors fg_optimizer_spec's testPassCoalesceSafe
// WAW shape, built through the ChunkSlot SoA helpers):
//   P0 writes RT_A, P1 writes a DIFFERENT RT_B (independent — no sampling of A,
//   no shared depth), P2 re-enters RT_A. P1 is independent of P0/P2, so
//   passcoalesce merges P0+P2 (3 passes -> 2). The depth handle is shared only
//   between the two RT_A passes, never RT_B, so P1 stays independent. No Present:
//   the caller drives the observer through the explicit dir seam.
ChunkSlot buildCoalesceableScenario(std::uint64_t seqId) {
  ChunkSlot slot;
  slot.seqId = seqId;
  const Handle rtA{0xA000u};
  const Handle rtB{0xB000u};
  const Handle ds{0xD000u};
  appendDrawRun(slot, rtA, ds, /*sampled=*/{}, /*paramCount=*/1);   // P0 -> RT_A
  appendDrawRun(slot, rtB, {}, /*sampled=*/{}, /*paramCount=*/1);   // P1 -> RT_B (independent)
  appendDrawRun(slot, rtA, ds, /*sampled=*/{}, /*paramCount=*/1);   // P2 -> RT_A (re-entry)
  return slot;
}

// Count "index" occurrences in the passes[] array of a dumped DAG JSON. Each
// SnapshotPass emits exactly one `"index": N` field, so this is the pass count.
std::size_t countPassesInJson(const std::string& json) {
  std::size_t count = 0;
  std::size_t pos = 0;
  const std::string_view needle = "\"index\":";
  while ((pos = json.find(needle, pos)) != std::string::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

// A minimal chunk: one draw, optionally terminated by a Present. A chunk that
// contains a Present is the LAST chunk of its inter-present frame.
ChunkSlot buildChunk(std::uint64_t seqId, bool withPresent) {
  ChunkSlot slot;
  slot.seqId = seqId;
  const Handle rt0{0xA000u};
  const Handle ds{0xD000u};
  appendDrawRun(slot, rt0, ds, /*sampled=*/{}, /*paramCount=*/1);
  if (withPresent) {
    appendPresent(slot, rt0);
  }
  return slot;
}

// --- temp dir + file helpers ------------------------------------------------

std::string makeTempDir() {
  // mkdtemp(3) mutates the template in place; keep it in a writable buffer.
  std::string templ = "/tmp/dxmt9-fg-observe-XXXXXX";
  std::vector<char> buf(templ.begin(), templ.end());
  buf.push_back('\0');
  const char* dir = mkdtemp(buf.data());
  if (dir == nullptr) {
    fail("mkdtemp failed");
  }
  return std::string(dir);
}

bool fileExists(const std::string& path) {
  std::ifstream f(path);
  return f.good();
}

std::string readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

bool contains(const std::string& haystack, std::string_view needle) {
  return haystack.find(needle) != std::string::npos;
}

void removeIfPresent(const std::string& path) {
  std::remove(path.c_str());
}

std::string dumpPath(const std::string& dir, std::uint64_t frameId,
                     std::uint64_t seqId, const char* stage) {
  std::ostringstream ss;
  ss << dir;
  if (!dir.empty() && dir.back() != '/') {
    ss << '/';
  }
  ss << "dag-frame" << frameId << "-chunk" << seqId << "-" << stage << ".json";
  return ss.str();
}

// --- Tests ------------------------------------------------------------------

// MUST run before the dump dir env is ever set: dumpDagDir() static-caches the
// (unset) value, proving the env-gated default render path does nothing (no
// files, no throw — byte-identical encode is preserved by onChunkReady's
// delegation). `probeDir` is a real temp dir the test owns; the early-out must
// leave it empty (the function has no dir to write to anyway, which is exactly
// the contract). We additionally assert that the promoted production option
// set contains only passcoalesce.
void testDefaultPathIsNoOp(const std::string& probeDir) {
  FrameGraphBackend backend;
  ChunkSlot slot = buildScenario(/*seqId=*/11);

  // No dump dir env → early-out (the dump is purely DXMT9_RENDERER_DUMP_DAG-
  // gated now). Must not throw.
  backend.observer().observeAndExport(slot);

  // Nothing should have been written anywhere we control.
  check(!fileExists(dumpPath(probeDir, 11, 11, "pre-opt")),
        "early-out wrote no pre-opt file");
  check(!fileExists(dumpPath(probeDir, 11, 11, "post-opt")),
        "early-out wrote no post-opt file");

  const auto& opts = backend.optimizerOptions();
  check(opts.passcoalesce && !opts.memoryless && !opts.dce && !opts.reorder,
        "default L1 enables only passcoalesce");
}

void testObserveExportsPreAndPostOptDags(const std::string& dir) {
  FrameGraphBackend backend;
  const std::uint64_t seqId = 60;
  ChunkSlot slot = buildScenario(seqId);

  const std::string pre = dumpPath(dir, seqId, seqId, "pre-opt");
  const std::string post = dumpPath(dir, seqId, seqId, "post-opt");
  removeIfPresent(pre);
  removeIfPresent(post);

  // Explicit-dir overload bypasses dumpDagDir()'s static cache.
  backend.observer().observeAndExportDagToDir(slot, /*frameId=*/seqId, dir);

  check(fileExists(pre), "pre-opt DAG dump was written");
  check(fileExists(post), "post-opt DAG dump was written");

  const std::string preJson = readFile(pre);
  for (std::string_view key : {"\"frame_id\"", "\"chunk_seq_id\"", "\"stage\"",
                               "\"passes\"", "\"resources\"", "\"edges\""}) {
    check(contains(preJson, key),
          "pre-opt JSON contains expected key");
  }
  check(contains(preJson, "\"stage\": \"pre-opt\""),
        "pre-opt JSON stamps stage=pre-opt");
  check(contains(preJson, "\"frame_id\": 60"),
        "pre-opt JSON stamps the frame id (slot.seqId)");
  check(contains(preJson, "\"chunk_seq_id\": 60"),
        "pre-opt JSON stamps the chunk seq id");

  const std::string postJson = readFile(post);
  check(contains(postJson, "\"stage\": \"post-opt\""),
        "post-opt JSON stamps stage=post-opt");

  removeIfPresent(pre);
  removeIfPresent(post);
}

// R-BACK-39.2 (Task B11, L1): the observe path bumps the framegraph_* perf
// counters. DXMT_PERF_COUNTERS is set in main() before any perf use (the perf
// system reads its enable flag once at process start). At L1-strict the
// optimizer options are all off, so coalesced/dead/memoryless stay 0 — but
// passes_built and dag_dumps_written MUST move on each observe export.
void testObserveMovesPerfCounters(const std::string& dir) {
  FrameGraphBackend backend;
  const std::uint64_t seqId = 77;
  ChunkSlot slot = buildScenario(seqId);

  const auto before = dxmt9::perf::test::snapshotFramegraphObserve();

  const std::string pre = dumpPath(dir, seqId, seqId, "pre-opt");
  const std::string post = dumpPath(dir, seqId, seqId, "post-opt");
  removeIfPresent(pre);
  removeIfPresent(post);
  backend.observer().observeAndExportDagToDir(slot, /*frameId=*/seqId, dir);
  removeIfPresent(pre);
  removeIfPresent(post);

  const auto after = dxmt9::perf::test::snapshotFramegraphObserve();

  check(after.passesBuilt > before.passesBuilt,
        "observe bumped framegraph_passes_built");
  check(after.dagDumpsWritten == before.dagDumpsWritten + 1,
        "observe bumped framegraph_dag_dumps_written by exactly one");
  // L1-strict parity baseline: no coalesce/dce/memoryless pass runs.
  check(after.passesCoalesced == before.passesCoalesced,
        "L1 strict leaves framegraph_passes_coalesced unchanged");
  check(after.passesDead == before.passesDead,
        "L1 strict leaves framegraph_passes_dead unchanged");
  check(after.resourcesMemoryless == before.resourcesMemoryless,
        "L1 strict leaves framegraph_resources_memoryless unchanged");
}

// DXMT9_RENDERER_DUMP_DAG_FRAME pure resolver: unset / empty / "0" / garbage all
// mean "no filter" (dump every chunk); a positive decimal selects that frame.
void testResolveDumpDagFrame() {
  using dxmt9::framegraph::resolveDumpDagFrame;
  check(!resolveDumpDagFrame(nullptr).has_value(),
        "resolveDumpDagFrame(nullptr) == nullopt");
  check(!resolveDumpDagFrame("").has_value(),
        "resolveDumpDagFrame(\"\") == nullopt");
  check(!resolveDumpDagFrame("0").has_value(),
        "resolveDumpDagFrame(\"0\") == nullopt");
  check(!resolveDumpDagFrame("abc").has_value(),
        "resolveDumpDagFrame(\"abc\") == nullopt");
  check(!resolveDumpDagFrame("2x").has_value(),
        "resolveDumpDagFrame(\"2x\") == nullopt (trailing garbage rejected)");
  const auto two = resolveDumpDagFrame("2");
  check(two.has_value() && *two == 2u, "resolveDumpDagFrame(\"2\") == 2");
  const auto big = resolveDumpDagFrame("100");
  check(big.has_value() && *big == 100u, "resolveDumpDagFrame(\"100\") == 100");
}

// DXMT9_RENDERER_DUMP_DAG_FRAME_RADIUS pure resolver: unset / empty / "0" /
// garbage all mean 0 (single-frame filter); a non-negative decimal is the
// window radius.
void testResolveDumpDagFrameRadius() {
  using dxmt9::framegraph::resolveDumpDagFrameRadius;
  check(resolveDumpDagFrameRadius(nullptr) == 0u,
        "resolveDumpDagFrameRadius(nullptr) == 0");
  check(resolveDumpDagFrameRadius("") == 0u,
        "resolveDumpDagFrameRadius(\"\") == 0");
  check(resolveDumpDagFrameRadius("0") == 0u,
        "resolveDumpDagFrameRadius(\"0\") == 0");
  check(resolveDumpDagFrameRadius("x") == 0u,
        "resolveDumpDagFrameRadius(\"x\") == 0");
  check(resolveDumpDagFrameRadius("3x") == 0u,
        "resolveDumpDagFrameRadius(\"3x\") == 0 (trailing garbage rejected)");
  check(resolveDumpDagFrameRadius("10") == 10u,
        "resolveDumpDagFrameRadius(\"10\") == 10");
  check(resolveDumpDagFrameRadius("1") == 1u,
        "resolveDumpDagFrameRadius(\"1\") == 1");
}

// chunkContainsPresent scans commandHeaders for MetalCommandKind::Present.
void testChunkContainsPresent() {
  using dxmt9::framegraph::chunkContainsPresent;
  const ChunkSlot withPresent = buildChunk(/*seqId=*/1, /*withPresent=*/true);
  const ChunkSlot noPresent = buildChunk(/*seqId=*/2, /*withPresent=*/false);
  check(chunkContainsPresent(withPresent),
        "chunkContainsPresent true for a present-terminated chunk");
  check(!chunkContainsPresent(noPresent),
        "chunkContainsPresent false for a draw-only chunk");
}

// Drive the observe path across a synthetic chunk stream:
//   chunk seq 100 (draw)              -> frame 1
//   chunk seq 101 (draw + Present)    -> frame 1, advances to frame 2 after
//   chunk seq 102 (draw)              -> frame 2  <- target
//   chunk seq 103 (draw + Present)    -> frame 2, advances to frame 3 after
//   chunk seq 104 (draw)              -> frame 3
// With the filter targeting frame 2, ONLY chunks 102 and 103 (frame-2 chunks)
// must produce dag-frame2-*.json; frame-1 (100,101) and frame-3 (104) chunks
// must produce nothing. Frame-2 chunks' files are named with frame_id=2 (the
// observe frame number) and chunk<seqId> (the chunk seq id).
void testFrameFilterDumpsOnlyTargetFrame(const std::string& dir) {
  FrameGraphBackend backend;

  struct Chunk {
    std::uint64_t seqId;
    bool present;
    std::uint64_t expectFrame;  // 0 => filtered out (no files)
  };
  const Chunk chunks[] = {
      {100, false, 0},  // frame 1, filtered
      {101, true, 0},   // frame 1, filtered (advances to 2)
      {102, false, 2},  // frame 2, DUMPED
      {103, true, 2},   // frame 2, DUMPED (advances to 3)
      {104, false, 0},  // frame 3, filtered
  };

  // Clean any pre-existing files for the seq ids we touch (across frames 1..3).
  auto pathFor = [&](std::uint64_t frame, std::uint64_t seq, const char* stage) {
    return dumpPath(dir, frame, seq, stage);
  };
  for (const Chunk& c : chunks) {
    for (std::uint64_t f = 1; f <= 3; ++f) {
      removeIfPresent(pathFor(f, c.seqId, "pre-opt"));
      removeIfPresent(pathFor(f, c.seqId, "post-opt"));
    }
  }

  for (const Chunk& c : chunks) {
    ChunkSlot slot = buildChunk(c.seqId, c.present);
    backend.observer().observeAndExportDagToDirForFrame(slot, dir,
                                             /*targetFrame=*/2u);
  }

  // The encode-thread-local frame counter must have advanced to 3 (two presents
  // seen: frame 1 -> 2 -> 3).
  check(backend.observer().observeFrame() == 3u,
        "observe_frame_ advanced past two presents to frame 3");

  for (const Chunk& c : chunks) {
    if (c.expectFrame == 0) {
      // Filtered-out chunks: NO files at any frame number.
      for (std::uint64_t f = 1; f <= 3; ++f) {
        check(!fileExists(pathFor(f, c.seqId, "pre-opt")),
              "filtered-out chunk wrote no pre-opt file");
        check(!fileExists(pathFor(f, c.seqId, "post-opt")),
              "filtered-out chunk wrote no post-opt file");
      }
    } else {
      // Target-frame chunks: files named dag-frame2-chunk<seq>-*.json exist;
      // the JSON stamps frame_id=2 (observe frame) and chunk_seq_id=<seq>.
      const std::string pre = pathFor(c.expectFrame, c.seqId, "pre-opt");
      const std::string post = pathFor(c.expectFrame, c.seqId, "post-opt");
      check(fileExists(pre), "frame-2 chunk wrote its pre-opt file");
      check(fileExists(post), "frame-2 chunk wrote its post-opt file");
      const std::string preJson = readFile(pre);
      check(contains(preJson, "\"frame_id\": 2"),
            "frame-2 dump stamps frame_id=2 (observe frame number)");
      check(contains(preJson,
                     "\"chunk_seq_id\": " + std::to_string(c.seqId)),
            "frame-2 dump stamps chunk_seq_id=<slot.seqId>");
    }
  }

  // Cleanup.
  for (const Chunk& c : chunks) {
    for (std::uint64_t f = 1; f <= 3; ++f) {
      removeIfPresent(pathFor(f, c.seqId, "pre-opt"));
      removeIfPresent(pathFor(f, c.seqId, "post-opt"));
    }
  }
}

// Drive the observe path across a synthetic five-frame stream with target frame
// 3 and radius 1 → the inclusive window is [2, 4], so ONLY frames 2, 3, 4 dump;
// frame 1 and frame 5 must produce no files. Each frame is a single
// present-terminated chunk so seq id == frame number for readability.
//   chunk seq 1 (present) -> frame 1, filtered (advances to 2)
//   chunk seq 2 (present) -> frame 2, DUMPED  (advances to 3)
//   chunk seq 3 (present) -> frame 3, DUMPED  (advances to 4)
//   chunk seq 4 (present) -> frame 4, DUMPED  (advances to 5)
//   chunk seq 5 (present) -> frame 5, filtered (advances to 6)
void testFrameWindowDumpsTargetPlusMinusRadius(const std::string& dir) {
  FrameGraphBackend backend;

  struct Chunk {
    std::uint64_t frame;   // == seqId here (one present chunk per frame)
    bool expectDumped;     // in window [2,4]?
  };
  const Chunk chunks[] = {
      {1, false}, {2, true}, {3, true}, {4, true}, {5, false},
  };

  auto cleanup = [&]() {
    for (const Chunk& c : chunks) {
      removeIfPresent(dumpPath(dir, c.frame, c.frame, "pre-opt"));
      removeIfPresent(dumpPath(dir, c.frame, c.frame, "post-opt"));
    }
  };
  cleanup();

  for (const Chunk& c : chunks) {
    ChunkSlot slot = buildChunk(/*seqId=*/c.frame, /*withPresent=*/true);
    backend.observer().observeAndExportDagToDirForFrame(slot, dir,
                                             /*targetFrame=*/3u, /*radius=*/1u);
  }

  // Five presents seen → counter advanced from 1 past frame 5 to frame 6.
  check(backend.observer().observeFrame() == 6u,
        "windowed: observe_frame_ advanced past five presents to frame 6");

  for (const Chunk& c : chunks) {
    const std::string pre = dumpPath(dir, c.frame, c.frame, "pre-opt");
    const std::string post = dumpPath(dir, c.frame, c.frame, "post-opt");
    if (c.expectDumped) {
      check(fileExists(pre), "windowed: in-window frame wrote its pre-opt file");
      check(fileExists(post),
            "windowed: in-window frame wrote its post-opt file");
    } else {
      check(!fileExists(pre),
            "windowed: out-of-window frame wrote no pre-opt file");
      check(!fileExists(post),
            "windowed: out-of-window frame wrote no post-opt file");
    }
  }

  cleanup();
}

// Low-clamp: target frame 1 with radius 5 → window is [max(1,1-5), 1+5] =
// [1, 6]; frame 1 dumps and there is never a frame 0 to select. Verify frame 1
// dumps and no dag-frame0-*.json is ever produced.
void testFrameWindowLowClampAtFrameOne(const std::string& dir) {
  FrameGraphBackend backend;

  const std::uint64_t seqA = 300;  // frame 1, draw-only
  const std::uint64_t seqB = 301;  // frame 1, present-terminated

  auto cleanup = [&]() {
    for (std::uint64_t f = 0; f <= 2; ++f) {
      for (std::uint64_t s : {seqA, seqB}) {
        removeIfPresent(dumpPath(dir, f, s, "pre-opt"));
        removeIfPresent(dumpPath(dir, f, s, "post-opt"));
      }
    }
  };
  cleanup();

  ChunkSlot a = buildChunk(seqA, /*withPresent=*/false);
  ChunkSlot b = buildChunk(seqB, /*withPresent=*/true);
  backend.observer().observeAndExportDagToDirForFrame(a, dir, /*targetFrame=*/1u,
                                           /*radius=*/5u);
  backend.observer().observeAndExportDagToDirForFrame(b, dir, /*targetFrame=*/1u,
                                           /*radius=*/5u);

  // Both frame-1 chunks dump under the clamped [1,6] window.
  check(fileExists(dumpPath(dir, 1, seqA, "pre-opt")),
        "low-clamp: frame-1 draw chunk dumped");
  check(fileExists(dumpPath(dir, 1, seqB, "post-opt")),
        "low-clamp: frame-1 present chunk dumped");
  // No frame 0 is ever selected (the low end clamps at 1).
  check(!fileExists(dumpPath(dir, 0, seqA, "pre-opt")),
        "low-clamp: no dag-frame0 file produced");
  check(!fileExists(dumpPath(dir, 0, seqB, "post-opt")),
        "low-clamp: no dag-frame0 file produced");

  cleanup();
}

// With no filter (nullopt target), every chunk is dumped and the counter still
// tracks inter-present frames (frame_id in the filename follows observe_frame_).
void testNoFilterDumpsEveryChunk(const std::string& dir) {
  FrameGraphBackend backend;

  // Two chunks: a draw chunk in frame 1, then a present-terminated chunk in
  // frame 1 that advances the counter to 2.
  const std::uint64_t seqA = 200;  // frame 1, draw-only
  const std::uint64_t seqB = 201;  // frame 1, present-terminated

  auto cleanup = [&]() {
    for (std::uint64_t f = 1; f <= 2; ++f) {
      for (std::uint64_t s : {seqA, seqB}) {
        removeIfPresent(dumpPath(dir, f, s, "pre-opt"));
        removeIfPresent(dumpPath(dir, f, s, "post-opt"));
      }
    }
  };
  cleanup();

  ChunkSlot a = buildChunk(seqA, /*withPresent=*/false);
  ChunkSlot b = buildChunk(seqB, /*withPresent=*/true);
  backend.observer().observeAndExportDagToDirForFrame(a, dir, /*targetFrame=*/std::nullopt);
  backend.observer().observeAndExportDagToDirForFrame(b, dir, /*targetFrame=*/std::nullopt);

  // Both chunks belong to frame 1, so both dump as dag-frame1-chunk<seq>.
  check(fileExists(dumpPath(dir, 1, seqA, "pre-opt")),
        "unfiltered: draw chunk dumped at frame 1");
  check(fileExists(dumpPath(dir, 1, seqB, "post-opt")),
        "unfiltered: present chunk dumped at frame 1");
  // The present chunk advanced the counter to 2.
  check(backend.observer().observeFrame() == 2u,
        "unfiltered: observe_frame_ advanced to 2 after the present");

  cleanup();
}

// The DAG dump is backend-agnostic (R-BACK-39.7): a TraditionalBackend owns the
// same render::DagObserver, constructed with default OptimizerOptions{} (no
// features — the order-preserving baseline). Driving its observer through the
// explicit dir/frame seam must write dag-frame<N>-...json for the target window
// exactly like the framegraph path, while the default (no-dir) observeAndExport
// path stays a no-op. This proves the dump works without DXMT9_RENDER_MODE.
void testTraditionalBackendObserverDumps(const std::string& dir) {
  TraditionalBackend backend;

  // Default-options parity baseline: traditional runs no optimizer passes.
  const auto& opts = backend.observer().options();
  check(!opts.passcoalesce && !opts.memoryless && !opts.dce && !opts.reorder,
        "traditional DagObserver uses default (all-false) OptimizerOptions");

  // Default path: no dump dir env primed to nullopt earlier → early-out, no
  // throw, nothing written (byte-identical traditional encode preserved).
  ChunkSlot probeSlot = buildScenario(/*seqId=*/41);
  backend.observer().observeAndExport(probeSlot);
  check(!fileExists(dumpPath(dir, 41, 41, "pre-opt")),
        "traditional early-out wrote no pre-opt file");
  check(!fileExists(dumpPath(dir, 41, 41, "post-opt")),
        "traditional early-out wrote no post-opt file");

  // Explicit-dir/frame seam: target frame 2, three chunks across two frames.
  //   chunk 500 (draw)            -> frame 1, filtered
  //   chunk 501 (draw + Present)  -> frame 1, filtered (advances to 2)
  //   chunk 502 (draw + Present)  -> frame 2, DUMPED   (advances to 3)
  struct Chunk {
    std::uint64_t seqId;
    bool present;
    bool expectDumped;
  };
  const Chunk chunks[] = {
      {500, false, false},
      {501, true, false},
      {502, true, true},
  };

  auto cleanup = [&]() {
    for (const Chunk& c : chunks) {
      for (std::uint64_t f = 1; f <= 3; ++f) {
        removeIfPresent(dumpPath(dir, f, c.seqId, "pre-opt"));
        removeIfPresent(dumpPath(dir, f, c.seqId, "post-opt"));
      }
    }
  };
  cleanup();

  for (const Chunk& c : chunks) {
    ChunkSlot slot = buildChunk(c.seqId, c.present);
    backend.observer().observeAndExportDagToDirForFrame(slot, dir,
                                                        /*targetFrame=*/2u);
  }

  // Two presents seen → counter advanced from 1 past frame 2 to frame 3.
  check(backend.observer().observeFrame() == 3u,
        "traditional observer advanced past two presents to frame 3");

  for (const Chunk& c : chunks) {
    if (c.expectDumped) {
      const std::string pre = dumpPath(dir, 2, c.seqId, "pre-opt");
      const std::string post = dumpPath(dir, 2, c.seqId, "post-opt");
      check(fileExists(pre), "traditional frame-2 chunk wrote its pre-opt file");
      check(fileExists(post),
            "traditional frame-2 chunk wrote its post-opt file");
      const std::string preJson = readFile(pre);
      check(contains(preJson, "\"frame_id\": 2"),
            "traditional dump stamps frame_id=2 (observe frame number)");
      check(contains(preJson,
                     "\"chunk_seq_id\": " + std::to_string(c.seqId)),
            "traditional dump stamps chunk_seq_id=<slot.seqId>");
    } else {
      for (std::uint64_t f = 1; f <= 3; ++f) {
        check(!fileExists(dumpPath(dir, f, c.seqId, "pre-opt")),
              "traditional filtered-out chunk wrote no pre-opt file");
        check(!fileExists(dumpPath(dir, f, c.seqId, "post-opt")),
              "traditional filtered-out chunk wrote no post-opt file");
      }
    }
  }

  cleanup();
}

// DXMT9_RENDERER_DUMP_DAG_OPTIMIZE pure resolver (R-BACK-39.7, analysis-only):
// nullptr / empty → nullopt (use backend options); a comma token list sets the
// named gated passes; unknown tokens are ignored but a set-but-all-unknown env
// still resolves to an (all-off) override (the operator opted in explicitly).
void testResolveDumpDagOptimize() {
  using dxmt9::framegraph::resolveDumpDagOptimize;

  check(!resolveDumpDagOptimize(nullptr).has_value(),
        "resolveDumpDagOptimize(nullptr) == nullopt");
  check(!resolveDumpDagOptimize("").has_value(),
        "resolveDumpDagOptimize(\"\") == nullopt");

  const auto pc = resolveDumpDagOptimize("passcoalesce");
  check(pc.has_value(), "\"passcoalesce\" → override present");
  check(pc->passcoalesce && !pc->reorder && !pc->dce && !pc->memoryless,
        "\"passcoalesce\" sets only passcoalesce");

  const auto pcr = resolveDumpDagOptimize("passcoalesce,reorder");
  check(pcr.has_value(), "\"passcoalesce,reorder\" → override present");
  check(pcr->passcoalesce && pcr->reorder && !pcr->dce && !pcr->memoryless,
        "\"passcoalesce,reorder\" sets both passcoalesce and reorder");

  const auto all = resolveDumpDagOptimize("dce,memoryless");
  check(all.has_value() && all->dce && all->memoryless && !all->passcoalesce &&
            !all->reorder,
        "\"dce,memoryless\" sets only dce and memoryless");

  // Unknown tokens ignored, but the env was set → an all-off override, not
  // nullopt (the post-opt snapshot then runs lifetime+loadstore only).
  const auto unk = resolveDumpDagOptimize("bogus,nope");
  check(unk.has_value() && !unk->passcoalesce && !unk->reorder && !unk->dce &&
            !unk->memoryless,
        "all-unknown tokens → set-but-all-off override (not nullopt)");

  // Known token mixed with unknown: the known one still applies.
  const auto mixed = resolveDumpDagOptimize("foo,passcoalesce,bar");
  check(mixed.has_value() && mixed->passcoalesce && !mixed->reorder,
        "unknown tokens around a known token are ignored");
}

// Analysis-only post-opt override behavior (R-BACK-39.7): a DagObserver
// constructed with passcoalesce options must produce a post-opt DAG JSON with
// FEWER passes than the pre-opt JSON for a coalesceable re-entry fixture, while
// the pre-opt JSON keeps the original (builder) pass count. This proves the
// pre/post diff = exactly what the chosen passes did, and that the post-opt
// snapshot honors the selected optimizer options. (The DXMT9_RENDERER_DUMP_DAG_-
// OPTIMIZE env override is exercised via resolveDumpDagOptimize above; here we
// drive the same effect through the observer's constructed options so the env
// static-cache need not be primed.)
void testPostOptHonorsPasscoalesce(const std::string& dir) {
  // Backend-provided options carrying passcoalesce — the env-override path
  // value_or()s onto exactly this when DXMT9_RENDERER_DUMP_DAG_OPTIMIZE is unset.
  dxmt9::framegraph::OptimizerOptions opts{};
  opts.passcoalesce = true;
  DagObserver observer(opts);

  const std::uint64_t seqId = 88;
  ChunkSlot slot = buildCoalesceableScenario(seqId);

  const std::string pre = dumpPath(dir, seqId, seqId, "pre-opt");
  const std::string post = dumpPath(dir, seqId, seqId, "post-opt");
  removeIfPresent(pre);
  removeIfPresent(post);

  observer.observeAndExportDagToDir(slot, /*frameId=*/seqId, dir);

  check(fileExists(pre), "coalesceable: pre-opt DAG written");
  check(fileExists(post), "coalesceable: post-opt DAG written");

  const std::size_t prePasses = countPassesInJson(readFile(pre));
  const std::size_t postPasses = countPassesInJson(readFile(post));

  check(prePasses == 3,
        "pre-opt keeps the original 3-pass (un-optimized) baseline");
  check(postPasses < prePasses,
        "post-opt with passcoalesce has FEWER passes than pre-opt");
  check(postPasses == 2,
        "post-opt coalesced the matching RT_A re-entry pair (3 -> 2)");

  removeIfPresent(pre);
  removeIfPresent(post);
}

// Control: with default (all-off) options the SAME coalesceable fixture is NOT
// coalesced — post-opt keeps all 3 passes. This isolates the override as the
// only variable producing the pass-count drop (no override → backend options →
// no coalesce).
void testPostOptDefaultOptionsDoNotCoalesce(const std::string& dir) {
  DagObserver observer;  // default OptimizerOptions{} (all gated passes off)

  const std::uint64_t seqId = 89;
  ChunkSlot slot = buildCoalesceableScenario(seqId);

  const std::string pre = dumpPath(dir, seqId, seqId, "pre-opt");
  const std::string post = dumpPath(dir, seqId, seqId, "post-opt");
  removeIfPresent(pre);
  removeIfPresent(post);

  observer.observeAndExportDagToDir(slot, /*frameId=*/seqId, dir);

  const std::size_t prePasses = countPassesInJson(readFile(pre));
  const std::size_t postPasses = countPassesInJson(readFile(post));
  check(prePasses == 3, "control pre-opt keeps 3 passes");
  check(postPasses == 3,
        "control post-opt with all-off options keeps 3 passes (no coalesce)");

  removeIfPresent(pre);
  removeIfPresent(post);
}

void testArenaPayloadObserverExport(const std::string& dir) {
  ChunkSlot slot;
  slot.seqId = 90;
  slot.appendClear({});
  dxmt9::tests::framegraph::ArenaPayloadFixture arena(slot);
  check(arena.valid(), "Arena observer fixture publishes");

  DagObserver observer;
  const std::string pre = dumpPath(dir, slot.seqId, slot.seqId, "pre-opt");
  const std::string post = dumpPath(dir, slot.seqId, slot.seqId, "post-opt");
  removeIfPresent(pre);
  removeIfPresent(post);
  observer.observeAndExportDagToDir(arena.view(), slot.seqId,
                                    /*frameId=*/slot.seqId, dir);
  check(fileExists(pre), "Arena payload writes pre-opt DAG");
  check(fileExists(post), "Arena payload writes post-opt DAG");
  check(contains(readFile(pre), "\"chunk_seq_id\": 90"),
        "Arena DAG preserves source sequence attribution");
  removeIfPresent(pre);
  removeIfPresent(post);
}

// DXMT9_RENDERER_DUMP_DAG_DRAWS pure resolver: repo env-flag semantics — "set"
// is a non-empty string that is not "0".
void testResolveDumpDagDraws() {
  using dxmt9::framegraph::resolveDumpDagDraws;
  check(!resolveDumpDagDraws(nullptr), "resolveDumpDagDraws(nullptr) == false");
  check(!resolveDumpDagDraws(""), "resolveDumpDagDraws(\"\") == false");
  check(!resolveDumpDagDraws("0"), "resolveDumpDagDraws(\"0\") == false");
  check(resolveDumpDagDraws("1"), "resolveDumpDagDraws(\"1\") == true");
  check(resolveDumpDagDraws("on"), "resolveDumpDagDraws(\"on\") == true");
  check(resolveDumpDagDraws("00"),
        "resolveDumpDagDraws(\"00\") == true (only exact \"0\" is off)");
}

// With DXMT9_RENDERER_DUMP_DAG_DRAWS set (latched in main) AND a slot supplied,
// the JSON gains per-pass "draws_detail" carrying the known per-draw values.
// With the slot ABSENT (the pure FrameGraph-only overload) there is NO
// "draws_detail" even with the flag on — the off-by-default / golden-stable
// invariant for the device-free serializers.
void testDrawsDetailEmittedWithSlotAndFlag() {
  using dxmt9::framegraph::buildFrameGraph;
  using dxmt9::framegraph::serializeDagJson;

  ChunkSlot slot;
  slot.seqId = 9000;
  const Handle rt0{0xA000u};
  const Handle ds{0xD000u};
  // Two draws in one pass (same attachments): a 5-tri triangle list with
  // vs=0x111/ps=0x222/alphaBlend=1/stride=32, then a 2-prim line list with
  // distinct hashes and alphaBlend=0/stride=12.
  appendKnownDrawRun(slot, rt0, ds, dxmt9::core::PrimitiveType::TriangleList,
                     /*primCount=*/5u, /*vsHash=*/0x111u, /*psHash=*/0x222u,
                     /*alphaBlend=*/1u, /*stream0Stride=*/32u);
  appendKnownDrawRun(slot, rt0, ds, dxmt9::core::PrimitiveType::LineList,
                     /*primCount=*/2u, /*vsHash=*/0x333u, /*psHash=*/0x444u,
                     /*alphaBlend=*/0u, /*stream0Stride=*/12u);

  const auto fg = buildFrameGraph(slot, /*frame_id=*/7u);

  // Slot-aware serialize (flag on): draws_detail present with both draws.
  const std::string withSlot = serializeDagJson(fg, slot.seqId, "pre-opt", &slot);
  check(contains(withSlot, "\"draws_detail\""),
        "slot+flag: JSON carries draws_detail");
  // Draw 0 fields (TriangleList=3, primCount 5, vs 0x111, ps 0x222, blend 1).
  check(contains(withSlot, "\"primitive_type\": 3"),
        "draw0 primitive_type == TriangleList(3)");
  check(contains(withSlot, "\"primitive_count\": 5"),
        "draw0 primitive_count == 5");
  check(contains(withSlot, "\"vs_hash\": \"0x111\""), "draw0 vs_hash == 0x111");
  check(contains(withSlot, "\"ps_hash\": \"0x222\""), "draw0 ps_hash == 0x222");
  check(contains(withSlot, "\"alpha_blend\": 1"), "draw0 alpha_blend == 1");
  check(contains(withSlot, "\"z_enable\": 1"), "draw0 z_enable == 1");
  check(contains(withSlot, "\"stream0_stride\": 32"),
        "draw0 stream0_stride == 32");
  check(contains(withSlot, "\"texture_mask\": 1"), "draw0 texture_mask == 1");
  check(contains(withSlot, "\"command_index\": 0"),
        "draw0 command_index == 0");
  check(contains(withSlot, "\"command_index\": 1"),
        "draw1 command_index == 1");
  // Draw 1 distinctive fields (LineList=1, primCount 2, vs 0x333, stride 12).
  check(contains(withSlot, "\"primitive_type\": 1"),
        "draw1 primitive_type == LineList(1)");
  check(contains(withSlot, "\"vs_hash\": \"0x333\""), "draw1 vs_hash == 0x333");
  check(contains(withSlot, "\"stream0_stride\": 12"),
        "draw1 stream0_stride == 12");

  // FrameGraph-only overload (no slot): NO draws_detail even with flag on.
  const std::string noSlot = serializeDagJson(fg, slot.seqId, "pre-opt");
  check(!contains(noSlot, "\"draws_detail\""),
        "no slot: JSON omits draws_detail (golden-stable, off-by-default)");
  // The base DAG keys are still present (the rest of the JSON is unchanged).
  for (std::string_view key : {"\"passes\"", "\"draws\"", "\"resources\"",
                               "\"edges\""}) {
    check(contains(noSlot, key), "no-slot JSON keeps the base DAG keys");
  }
}

}  // namespace

int main() {
  // DEBUG-ONLY per-draw detail opt-in. DXMT9_RENDERER_DUMP_DAG_DRAWS is
  // static-cached on first read (dumpDagDraws()), so set it before any
  // serialize/buildSnapshot runs.
  setenv("DXMT9_RENDERER_DUMP_DAG_DRAWS", "1", /*overwrite=*/1);

  // R-BACK-39.2 (Task B11): perf counters latch their enable flag once at
  // process start, so opt in before any perf::count* runs.
  setenv("DXMT_PERF_COUNTERS", "1", /*overwrite=*/1);

  std::string dir;
  try {
    // The DXMT9_RENDERER_DUMP_DAG env var is intentionally left UNSET for the
    // whole process so the env-gated early-out is observable; the export test
    // uses the explicit-dir overload instead.
    dir = makeTempDir();

    // 1. Env-gated early-out (no dump dir, no features) writes nothing.
    testDefaultPathIsNoOp(dir);

    // 2. Explicit-dir export writes both pre-opt and post-opt DAG JSON.
    testObserveExportsPreAndPostOptDags(dir);

    // 3. Observe path moves the R-BACK-39.2 framegraph_* perf counters.
    testObserveMovesPerfCounters(dir);

    // 4. DXMT9_RENDERER_DUMP_DAG_FRAME[_RADIUS] resolvers + present-scan helper.
    testResolveDumpDagFrame();
    testResolveDumpDagFrameRadius();
    testChunkContainsPresent();

    // 5. Per-frame chunk filter: only the target frame's chunks dump.
    testFrameFilterDumpsOnlyTargetFrame(dir);

    // 6. ±radius window: target=3, radius=1 dumps only frames 2,3,4.
    testFrameWindowDumpsTargetPlusMinusRadius(dir);

    // 7. Low-clamp: target=1, radius=5 → window starts at frame 1, no frame 0.
    testFrameWindowLowClampAtFrameOne(dir);

    // 8. No filter (nullopt) still dumps every chunk and tracks frames.
    testNoFilterDumpsEveryChunk(dir);

    // 9. Backend-agnostic: a TraditionalBackend-owned DagObserver dumps too,
    //    decoupled from DXMT9_RENDER_MODE (default options, no-dir early-out).
    testTraditionalBackendObserverDumps(dir);

    // 10. DXMT9_RENDERER_DUMP_DAG_OPTIMIZE analysis-only post-opt override.
    testResolveDumpDagOptimize();
    testPostOptHonorsPasscoalesce(dir);
    testPostOptDefaultOptionsDoNotCoalesce(dir);
    testArenaPayloadObserverExport(dir);

    // 11. DXMT9_RENDERER_DUMP_DAG_DRAWS DEBUG-ONLY per-draw detail extension.
    testResolveDumpDagDraws();
    testDrawsDetailEmittedWithSlotAndFlag();

    rmdir(dir.c_str());
  } catch (const TestFailure& failure) {
    std::cerr << "framegraph_observe_spec failed: " << failure.what() << '\n';
    return 1;
  } catch (const std::exception& ex) {
    std::cerr << "framegraph_observe_spec unexpected exception: " << ex.what()
              << '\n';
    return 1;
  }
  std::cout << "framegraph_observe_spec passed\n";
  return 0;
}
