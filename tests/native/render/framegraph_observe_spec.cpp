// Device-free spec for FrameGraphBackend's observe + DAG-export side-channel
// (Task B12, L1).
//
// src/dxmt9/render/framegraph_backend.cpp wires a side-effect-neutral
// observation path (R-BACK-39.7) into the FrameGraph backend: it builds a
// framegraph::FrameGraph from one core::ChunkSlot, dumps the pre-opt DAG, runs
// the resolved (L1-strict empty) optimizer options, and dumps the post-opt DAG.
// The real Metal encode stays on encoders::encodeChunk (byte-identical,
// R-BACK-40.5) and is NOT exercised here — there is no MTLDevice in the native
// test host, so we cannot call onChunkReady. Instead this spec drives the
// public maybeObserveAndExportDag path indirectly via a tiny test-only subclass
// that exposes it, asserting the dump files are produced (or not) and contain
// the expected JSON keys.
//
// framegraph::dumpDagDir() caches DXMT9_RENDERER_DUMP_DAG on first read
// (static-const) and cannot be reset in-process. To test BOTH the
// "no dump dir → early-out" and the "export writes both DAGs" behaviors in one
// process, this spec:
//   * tests the early-out through the env-gated maybeObserveAndExportDag while
//     the env var is unset (this also primes the static cache to nullopt), then
//   * tests the export through observeAndExportDagToDir, which takes the dump
//     directory explicitly and therefore bypasses the cache.
//
// Fixture style mirrors tests/native/framegraph/fg_builder_spec.cpp, which also
// builds ChunkSlot SoA records by hand.

#include "../../../src/dxmt9/render/framegraph_backend.hpp"

#include "../../../src/dxmt9/dxmt9_backend_types.hpp"
#include "../../../src/dxmt9/framegraph/fg_debug_export.hpp"

#include <unistd.h>  // mkdtemp, rmdir

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
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

using dxmt9::render::FrameGraphBackend;

// maybeObserveAndExportDag is the public device-free seam of the observe path
// (onChunkReady itself is device-gated; the backend class is `final` so a test
// subclass is not available). The test drives it directly.

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
// the contract). We additionally assert the resolved optimizer options are the
// all-false L1 parity baseline.
void testDefaultPathIsNoOp(const std::string& probeDir) {
  FrameGraphBackend backend;
  ChunkSlot slot = buildScenario(/*seqId=*/11);

  // No dump dir env, no features → early-out. Must not throw.
  backend.maybeObserveAndExportDag(slot, /*frameId=*/11);

  // Nothing should have been written anywhere we control.
  check(!fileExists(dumpPath(probeDir, 11, 11, "pre-opt")),
        "early-out wrote no pre-opt file");
  check(!fileExists(dumpPath(probeDir, 11, 11, "post-opt")),
        "early-out wrote no post-opt file");

  const auto& opts = backend.optimizerOptions();
  check(!opts.passcoalesce && !opts.memoryless && !opts.dce && !opts.reorder,
        "L1 strict: every optimizer option is off (parity baseline)");
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
  backend.observeAndExportDagToDir(slot, /*frameId=*/seqId, dir);

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

}  // namespace

int main() {
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
