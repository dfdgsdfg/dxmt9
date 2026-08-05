// Frame Graph DAG debug export (Task B10, L1).
//
// Spec: specs/d3d9-renderer/spec.md §3.5, requirements.md R-BACK-39.7.
//
// See fg_debug_export.hpp for the ownership / determinism / one-snapshot
// contract. This file holds the pure serializers and the side-effect-neutral
// file-writing helpers.

#include "fg_debug_export.hpp"

#include "dxmt9/core_constants.hpp"     // core::RS_*, core::CompareFunc
#include "dxmt9/core_snapshots.hpp"     // core::flatStateOr
#include "util/log/log.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace dxmt9::framegraph {

namespace {

// Lowercase hex of a handle value, "0x"-prefixed. Deterministic (no locale).
std::string handleHex(ResourceHandle handle) {
  char buf[2 + 16 + 1];
  std::snprintf(buf, sizeof(buf), "0x%llx",
                static_cast<unsigned long long>(handle.value));
  return std::string(buf);
}

const char* passKindName(PassKind kind) {
  switch (kind) {
    case PassKind::Render:
      return "Render";
    case PassKind::Compute:
      return "Compute";
    case PassKind::Blit:
      return "Blit";
    case PassKind::Present:
      return "Present";
    case PassKind::Sync:
      return "Sync";
  }
  return "Unknown";
}

const char* accessKindName(AccessKind kind) {
  switch (kind) {
    case AccessKind::Read:
      return "read";
    case AccessKind::Write:
      return "write";
    case AccessKind::ReadWrite:
      return "readwrite";
    case AccessKind::Preserve:
      return "preserve";
    case AccessKind::Clear:
      return "clear";
  }
  return "unknown";
}

const char* accessStageName(AccessStage stage) {
  switch (stage) {
    case AccessStage::Vertex:
      return "vertex";
    case AccessStage::Fragment:
      return "fragment";
    case AccessStage::Compute:
      return "compute";
    case AccessStage::Copy:
      return "copy";
  }
  return "unknown";
}

const char* residencyName(ResidencyClass residency) {
  switch (residency) {
    case ResidencyClass::Persistent:
      return "Persistent";
    case ResidencyClass::MemorylessCandidate:
      return "MemorylessCandidate";
    case ResidencyClass::Memoryless:
      return "Memoryless";
  }
  return "Unknown";
}

const char* loadActionName(LoadAction action) {
  switch (action) {
    case LoadAction::DontCare:
      return "DontCare";
    case LoadAction::Load:
      return "Load";
    case LoadAction::Clear:
      return "Clear";
  }
  return "Unknown";
}

const char* storeActionName(StoreAction action) {
  switch (action) {
    case StoreAction::DontCare:
      return "DontCare";
    case StoreAction::Store:
      return "Store";
  }
  return "Unknown";
}

// "<Load>/<Store>" for the load_store JSON / labels (matches spec.md sample).
std::string loadStorePair(LoadAction load, StoreAction store) {
  std::string out = loadActionName(load);
  out.push_back('/');
  out += storeActionName(store);
  return out;
}

// Minimal JSON string escaping for the only free-form field we emit (stage).
std::string jsonEscape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(c);
        break;
    }
  }
  return out;
}

// Resolve the DEBUG-ONLY per-draw detail for one pass's DrawRange out of the
// source payload. Pure read: it walks fg.draws[first..first+count) (each a
// source-local DrawRef), resolves the draw run through SourcePayloadView, and
// copies the cheaply-available hot fields. No geometry bytes are decoded;
// every value comes from the already-resolved hot/debug snapshot or per-ordinal
// DrawParam. See SnapshotDraw in the header for the field-by-field source.
void resolvePassDrawDetail(const FrameGraph& fg,
                           core::SourcePayloadView payload,
                           const DrawRange& range,
                           std::vector<SnapshotDraw>& out) {
  const std::size_t first = range.first;
  const std::size_t last =
      first <= fg.draws.size() ? std::min<std::size_t>(
                                     first + range.count, fg.draws.size())
                               : fg.draws.size();
  for (std::size_t d = first; d < last; ++d) {
    const DrawRef& ref = fg.draws[d];
    const core::MetalCommandView command =
        payload.commandAt(ref.command_index).command;
    const core::FlatDrawStateRecord* hot = command.drawState.hot;
    const core::DrawDebugSnapshot* debug = command.drawState.debug;

    // VS/PS hash from the debug snapshot — the same hashes the 3dmark05
    // indexed-probe CSV reports. Absent state leaves the zero defaults.
    const u64 vs_hash = debug ? debug->vertexShaderHash : 0u;
    const u64 ps_hash = debug ? debug->pixelShaderHash : 0u;
    const u32 texture_mask = hot ? hot->textureMask : 0u;
    const u32 stream0_stride = hot ? hot->streamStrides[0] : 0u;

    // Render states via flatStateOr on the hot FlatStateSet, with the same
    // defaults the encoder's DrawDebugRecord uses (dxmt9_draw_encoder.mm).
    u32 alpha_blend = 0, z_enable = 0, z_write = 0, alpha_test = 0, cull = 0;
    u32 z_func = static_cast<u32>(core::CompareFunc::LessEqual);
    if (hot) {
      alpha_blend =
          core::flatStateOr(hot->renderStates, core::RS_ALPHABLEND_ENABLE, 0u);
      z_enable = core::flatStateOr(hot->renderStates, core::RS_Z_ENABLE, 0u);
      z_write = core::flatStateOr(hot->renderStates, core::RS_Z_WRITE_ENABLE, 0u);
      z_func = core::flatStateOr(hot->renderStates, core::RS_Z_FUNC, z_func);
      alpha_test =
          core::flatStateOr(hot->renderStates, core::RS_ALPHA_TEST_ENABLE, 0u);
      cull = core::flatStateOr(hot->renderStates, core::RS_CULL_MODE, 0u);
    }

    // One SnapshotDraw per draw-call ordinal covered by this DrawRef. The
    // per-ordinal DrawParam carries primitive type/count; the rest of the hot
    // state is shared across the run's ordinals.
    const std::uint32_t ordinalCount =
        static_cast<std::uint32_t>(command.drawParams.size());
    for (std::uint32_t o = 0; o < ordinalCount; ++o) {
      const core::DrawParam& param = command.drawParams[o];
      out.push_back(SnapshotDraw{
          .command_index = ref.command_index,
          .draw_ordinal = ref.param_first + o,
          .primitive_type = static_cast<u32>(param.primitiveType),
          .primitive_count = param.primitiveCount,
          .vs_hash = vs_hash,
          .ps_hash = ps_hash,
          .texture_mask = texture_mask,
          .alpha_blend = alpha_blend,
          .z_enable = z_enable,
          .z_write = z_write,
          .z_func = z_func,
          .alpha_test = alpha_test,
          .cull = cull,
          .stream0_stride = stream0_stride,
      });
    }
  }
}

}  // namespace

DagSnapshot buildSnapshot(const FrameGraph& fg, std::uint64_t chunk_seq_id,
                          const char* stage,
                          core::SourcePayloadView payload) {
  // DEBUG-ONLY per-draw detail is resolved only when a payload is supplied AND the
  // operator opted in. Reading the flag once here keeps the off-path zero-cost
  // (no draw walk, no allocation) and the JSON byte-identical to history.
  const bool wantDraws = payload.valid() && dumpDagDraws();
  DagSnapshot snap;
  snap.frame_id = fg.frame_id;
  snap.chunk_seq_id = chunk_seq_id;
  snap.stage = stage ? std::string(stage) : std::string();

  snap.passes.reserve(fg.passes.size());
  for (std::size_t i = 0; i < fg.passes.size(); ++i) {
    const PassNode& p = fg.passes[i];
    SnapshotPass sp;
    sp.index = static_cast<u32>(i);
    sp.kind = p.kind;
    const u32 color_count =
        p.targets.color_count <= static_cast<u32>(p.targets.color.size())
            ? p.targets.color_count
            : static_cast<u32>(p.targets.color.size());
    sp.color.reserve(color_count);
    for (u32 c = 0; c < color_count; ++c) {
      sp.color.push_back(p.targets.color[c]);
    }
    sp.depth = p.targets.depth;
    sp.has_depth = static_cast<bool>(p.targets.depth);
    sp.draws = p.draws;
    sp.state_profile = p.state_profile;
    sp.load_store = p.load_store;
    if (wantDraws) {
      resolvePassDrawDetail(fg, payload, p.draws, sp.draws_detail);
    }
    snap.passes.push_back(std::move(sp));
  }

  snap.resources.reserve(fg.resources.size());
  for (const ResourceNode& r : fg.resources) {
    SnapshotResource sr;
    sr.handle = r.handle;
    sr.residency = r.residency;
    sr.first_use_pass = r.first_use_pass;
    sr.last_use_pass = r.last_use_pass;
    sr.accesses.reserve(r.accesses.size());
    for (const AccessLog& a : r.accesses) {
      sr.accesses.push_back(SnapshotAccess{
          .pass_index = a.pass_index,
          .kind = static_cast<AccessKind>(a.access_kind),
          .stage = static_cast<AccessStage>(a.stage),
      });
    }
    snap.resources.push_back(std::move(sr));
  }

  snap.edges.reserve(fg.edges.size());
  for (const Edge& e : fg.edges) {
    snap.edges.push_back(SnapshotEdge{
        .src_pass = e.src_pass,
        .dst_pass = e.dst_pass,
        .resource = e.resource,
    });
  }

  return snap;
}

DagSnapshot buildSnapshot(const FrameGraph& fg, std::uint64_t chunk_seq_id,
                          const char* stage, const core::ChunkSlot* slot) {
  return buildSnapshot(
      fg, chunk_seq_id, stage,
      slot ? core::SourcePayloadView(*slot) : core::SourcePayloadView{});
}

std::string serializeDagJson(const DagSnapshot& snapshot) {
  std::ostringstream os;
  os << "{\n";
  os << "  \"frame_id\": " << snapshot.frame_id << ",\n";
  os << "  \"chunk_seq_id\": " << snapshot.chunk_seq_id << ",\n";
  os << "  \"stage\": \"" << jsonEscape(snapshot.stage) << "\",\n";

  // passes[]
  os << "  \"passes\": [";
  for (std::size_t i = 0; i < snapshot.passes.size(); ++i) {
    const SnapshotPass& p = snapshot.passes[i];
    os << (i == 0 ? "\n" : ",\n");
    os << "    { \"index\": " << p.index << ", \"kind\": \""
       << passKindName(p.kind) << "\", \"color\": [";
    for (std::size_t c = 0; c < p.color.size(); ++c) {
      os << (c == 0 ? "" : ", ") << "\"" << handleHex(p.color[c]) << "\"";
    }
    os << "], ";
    if (p.has_depth) {
      os << "\"depth\": \"" << handleHex(p.depth) << "\", ";
    } else {
      os << "\"depth\": null, ";
    }
    os << "\"draws\": { \"first\": " << p.draws.first
       << ", \"count\": " << p.draws.count << " }, ";
    os << "\"state_profile\": \""
       << handleHex(ResourceHandle{p.state_profile}) << "\", ";
    os << "\"load_store\": { \"color\": [";
    for (std::size_t c = 0; c < p.color.size(); ++c) {
      os << (c == 0 ? "" : ", ") << "\""
         << loadStorePair(p.load_store.color_load[c],
                          p.load_store.color_store[c])
         << "\"";
    }
    os << "], \"depth\": \""
       << loadStorePair(p.load_store.depth_load, p.load_store.depth_store)
       << "\" }";
    // DEBUG-ONLY per-draw detail (DXMT9_RENDERER_DUMP_DAG_DRAWS). Emitted ONLY
    // when present, so the default JSON (no slot / flag off) is unchanged and
    // existing golden tests stay valid.
    if (!p.draws_detail.empty()) {
      os << ", \"draws_detail\": [";
      for (std::size_t d = 0; d < p.draws_detail.size(); ++d) {
        const SnapshotDraw& dd = p.draws_detail[d];
        os << (d == 0 ? "" : ", ")
           << "{ \"command_index\": " << dd.command_index
           << ", \"draw_ordinal\": " << dd.draw_ordinal
           << ", \"primitive_type\": " << dd.primitive_type
           << ", \"primitive_count\": " << dd.primitive_count
           << ", \"vs_hash\": \"" << handleHex(ResourceHandle{dd.vs_hash})
           << "\", \"ps_hash\": \"" << handleHex(ResourceHandle{dd.ps_hash})
           << "\", \"texture_mask\": " << dd.texture_mask
           << ", \"alpha_blend\": " << dd.alpha_blend
           << ", \"z_enable\": " << dd.z_enable
           << ", \"z_write\": " << dd.z_write
           << ", \"z_func\": " << dd.z_func
           << ", \"alpha_test\": " << dd.alpha_test
           << ", \"cull\": " << dd.cull
           << ", \"stream0_stride\": " << dd.stream0_stride << " }";
      }
      os << "]";
    }
    os << " }";
  }
  os << (snapshot.passes.empty() ? "" : "\n  ") << "],\n";

  // resources[]
  os << "  \"resources\": [";
  for (std::size_t i = 0; i < snapshot.resources.size(); ++i) {
    const SnapshotResource& r = snapshot.resources[i];
    os << (i == 0 ? "\n" : ",\n");
    os << "    { \"handle\": \"" << handleHex(r.handle) << "\", \"residency\": \""
       << residencyName(r.residency) << "\", \"first_use_pass\": "
       << r.first_use_pass << ", \"last_use_pass\": " << r.last_use_pass
       << ", \"accesses\": [";
    for (std::size_t a = 0; a < r.accesses.size(); ++a) {
      const SnapshotAccess& acc = r.accesses[a];
      os << (a == 0 ? "" : ", ") << "{ \"pass\": " << acc.pass_index
         << ", \"kind\": \"" << accessKindName(acc.kind) << "\", \"stage\": \""
         << accessStageName(acc.stage) << "\" }";
    }
    os << "] }";
  }
  os << (snapshot.resources.empty() ? "" : "\n  ") << "],\n";

  // edges[]
  os << "  \"edges\": [";
  for (std::size_t i = 0; i < snapshot.edges.size(); ++i) {
    const SnapshotEdge& e = snapshot.edges[i];
    os << (i == 0 ? "\n" : ",\n");
    os << "    { \"src_pass\": " << e.src_pass << ", \"dst_pass\": "
       << e.dst_pass << ", \"resource\": \"" << handleHex(e.resource) << "\" }";
  }
  os << (snapshot.edges.empty() ? "" : "\n  ") << "]\n";

  os << "}\n";
  return os.str();
}

namespace {

// Shared human-readable pass label used by both mermaid and dot:
//   "pass<i> <Kind> · <attachments> · draws <first>..<first+count>"
std::string passLabel(const SnapshotPass& p) {
  std::ostringstream os;
  os << "pass" << p.index << " " << passKindName(p.kind);

  std::string attachments;
  for (std::size_t c = 0; c < p.color.size(); ++c) {
    if (!attachments.empty()) attachments += ",";
    attachments += handleHex(p.color[c]);
  }
  if (p.has_depth) {
    if (!attachments.empty()) attachments += ",";
    attachments += handleHex(p.depth);
  }
  if (!attachments.empty()) {
    os << " - " << attachments;
  }

  os << " - draws " << p.draws.first << ".."
     << (p.draws.first + p.draws.count);
  return os.str();
}

}  // namespace

std::string serializeDagMermaid(const DagSnapshot& snapshot) {
  std::ostringstream os;
  os << "flowchart TD\n";
  for (const SnapshotPass& p : snapshot.passes) {
    // Mermaid node: P<index>["<label>"]. The label is quoted, so it tolerates
    // the spaces / dots / commas in passLabel.
    os << "  P" << p.index << "[\"" << passLabel(p) << "\"]\n";
  }
  // One edge per Edge, labeled by resource handle. A→B→A re-entry therefore
  // shows as two edges sharing a resource arriving at a later pass.
  for (const SnapshotEdge& e : snapshot.edges) {
    os << "  P" << e.src_pass << " -->|\"" << handleHex(e.resource) << "\"| P"
       << e.dst_pass << "\n";
  }
  return os.str();
}

std::string serializeDagDot(const DagSnapshot& snapshot) {
  std::ostringstream os;
  os << "digraph FrameGraph {\n";
  os << "  rankdir=TD;\n";
  os << "  node [shape=box];\n";
  for (const SnapshotPass& p : snapshot.passes) {
    os << "  P" << p.index << " [label=\"" << passLabel(p) << "\"];\n";
  }
  for (const SnapshotEdge& e : snapshot.edges) {
    os << "  P" << e.src_pass << " -> P" << e.dst_pass << " [label=\""
       << handleHex(e.resource) << "\"];\n";
  }
  os << "}\n";
  return os.str();
}

// const-FrameGraph overloads: build the snapshot once, delegate. (mermaid/dot
// do not get a chunk seq / stage; those only frame the JSON object.)
std::string serializeDagJson(const FrameGraph& fg, std::uint64_t chunk_seq_id,
                             const char* stage, const core::ChunkSlot* slot) {
  return serializeDagJson(buildSnapshot(fg, chunk_seq_id, stage, slot));
}

std::string serializeDagJson(const FrameGraph& fg, std::uint64_t chunk_seq_id,
                             const char* stage,
                             core::SourcePayloadView payload) {
  return serializeDagJson(buildSnapshot(fg, chunk_seq_id, stage, payload));
}

std::string serializeDagMermaid(const FrameGraph& fg) {
  return serializeDagMermaid(buildSnapshot(fg, 0, ""));
}

std::string serializeDagDot(const FrameGraph& fg) {
  return serializeDagDot(buildSnapshot(fg, 0, ""));
}

std::vector<DumpFormat> resolveDumpFormats(const char* env) {
  std::vector<DumpFormat> formats;
  auto add = [&formats](DumpFormat fmt) {
    for (DumpFormat existing : formats) {
      if (existing == fmt) return;  // de-dup, preserve first-seen order
    }
    formats.push_back(fmt);
  };

  if (env == nullptr || env[0] == '\0') {
    return {DumpFormat::Json};
  }

  std::string_view view(env);
  std::size_t start = 0;
  bool sawAny = false;
  while (start <= view.size()) {
    std::size_t comma = view.find(',', start);
    std::string_view token = view.substr(
        start, comma == std::string_view::npos ? std::string_view::npos
                                               : comma - start);
    if (token == "json") {
      add(DumpFormat::Json);
      sawAny = true;
    } else if (token == "dot") {
      add(DumpFormat::Dot);
      sawAny = true;
    } else if (token == "mermaid") {
      add(DumpFormat::Mermaid);
      sawAny = true;
    }
    // unknown / empty tokens ignored
    if (comma == std::string_view::npos) break;
    start = comma + 1;
  }

  if (!sawAny) {
    // Env was set but listed only unknown tokens; fall back to the default.
    return {DumpFormat::Json};
  }
  return formats;
}

std::optional<std::string> dumpDagDir() {
  static const std::optional<std::string> value = []() -> std::optional<std::string> {
    const char* env = std::getenv("DXMT9_RENDERER_DUMP_DAG");
    if (env == nullptr || env[0] == '\0') {
      return std::nullopt;
    }
    return std::string(env);
  }();
  return value;
}

bool resolveDumpDagDraws(const char* env) {
  // Repo env-flag semantics: "set" = non-empty string that is not "0".
  return env != nullptr && env[0] != '\0' &&
         !(env[0] == '0' && env[1] == '\0');
}

bool dumpDagDraws() {
  static const bool value =
      resolveDumpDagDraws(std::getenv("DXMT9_RENDERER_DUMP_DAG_DRAWS"));
  return value;
}

std::optional<std::uint64_t> resolveDumpDagFrame(const char* env) {
  if (env == nullptr || env[0] == '\0') {
    return std::nullopt;
  }
  // Parse a non-negative decimal integer; reject any non-digit content. "0" and
  // non-numeric strings resolve to nullopt ("dump all frames"). Frame numbers
  // are 1-based, so 0 is not a selectable frame.
  std::uint64_t value = 0;
  for (const char* c = env; *c != '\0'; ++c) {
    if (*c < '0' || *c > '9') {
      return std::nullopt;
    }
    value = value * 10u + static_cast<std::uint64_t>(*c - '0');
  }
  if (value == 0) {
    return std::nullopt;
  }
  return value;
}

std::optional<std::uint64_t> dumpDagFrame() {
  static const std::optional<std::uint64_t> value =
      resolveDumpDagFrame(std::getenv("DXMT9_RENDERER_DUMP_DAG_FRAME"));
  return value;
}

std::uint64_t resolveDumpDagFrameRadius(const char* env) {
  if (env == nullptr || env[0] == '\0') {
    return 0;
  }
  // Parse a non-negative decimal integer; reject any non-digit content. Empty /
  // "0" / non-numeric all resolve to 0 (single-frame filter — no widening).
  std::uint64_t value = 0;
  for (const char* c = env; *c != '\0'; ++c) {
    if (*c < '0' || *c > '9') {
      return 0;
    }
    value = value * 10u + static_cast<std::uint64_t>(*c - '0');
  }
  return value;
}

std::uint64_t dumpDagFrameRadius() {
  static const std::uint64_t value =
      resolveDumpDagFrameRadius(std::getenv("DXMT9_RENDERER_DUMP_DAG_FRAME_RADIUS"));
  return value;
}

std::optional<OptimizerOptions> resolveDumpDagOptimize(const char* env) {
  if (env == nullptr || env[0] == '\0') {
    // No override: the observer uses the backend-provided options (the current
    // pre-opt-baseline vs backend-post-opt behavior).
    return std::nullopt;
  }

  // The operator explicitly set the override, so even an all-unknown token list
  // resolves to an (all-off) OptimizerOptions — the post-opt snapshot then runs
  // only the always-on lifetime + loadstore passes. This is analysis-only and
  // never feeds the Metal encode.
  OptimizerOptions options{};
  std::string_view view(env);
  std::size_t start = 0;
  while (start <= view.size()) {
    const std::size_t comma = view.find(',', start);
    const std::string_view token = view.substr(
        start, comma == std::string_view::npos ? std::string_view::npos
                                               : comma - start);
    if (token == "passcoalesce") {
      options.passcoalesce = true;
    } else if (token == "reorder") {
      options.reorder = true;
    } else if (token == "dce") {
      options.dce = true;
    } else if (token == "memoryless") {
      options.memoryless = true;
    }
    // unknown / empty tokens ignored
    if (comma == std::string_view::npos) break;
    start = comma + 1;
  }
  return options;
}

std::optional<OptimizerOptions> dumpDagOptimizeOverride() {
  static const std::optional<OptimizerOptions> value =
      resolveDumpDagOptimize(std::getenv("DXMT9_RENDERER_DUMP_DAG_OPTIMIZE"));
  return value;
}

bool chunkContainsPresent(core::SourcePayloadView payload) {
  for (std::size_t i = 0; i < payload.commandCount(); ++i) {
    if (payload.commandAt(i).kind() == core::MetalCommandKind::Present) {
      return true;
    }
  }
  return false;
}

bool chunkContainsPresent(const core::ChunkSlot& slot) {
  return chunkContainsPresent(core::SourcePayloadView(slot));
}

namespace {

const char* formatExtension(DumpFormat fmt) {
  switch (fmt) {
    case DumpFormat::Json:
      return "json";
    case DumpFormat::Dot:
      return "dot";
    case DumpFormat::Mermaid:
      return "mermaid";
  }
  return "json";
}

}  // namespace

void writeDagDump(const FrameGraph& fg, std::uint64_t frame_id,
                  std::uint64_t chunk_seq_id, const char* stage,
                  core::SourcePayloadView payload) {
  const std::optional<std::string> dir = dumpDagDir();
  if (!dir.has_value()) {
    return;
  }

  // Resolve formats once (env read each call is cheap and keeps writeDagDump
  // free of additional process-global state).
  const std::vector<DumpFormat> formats =
      resolveDumpFormats(std::getenv("DXMT9_RENDERER_DUMP_DAG_FORMATS"));

  // One field-walk, reused by every selected format (R-BACK-39.7). The optional
  // per-draw detail is resolved inside buildSnapshot only when payload is valid
  // AND DXMT9_RENDERER_DUMP_DAG_DRAWS is set (DEBUG-ONLY; JSON-only).
  const DagSnapshot snapshot = buildSnapshot(fg, chunk_seq_id, stage, payload);

  const std::string stageStr = stage ? std::string(stage) : std::string();

  // Warn at most once per process if the dump dir turns out to be unwritable.
  static std::atomic<bool> warned{false};

  for (DumpFormat fmt : formats) {
    std::string path = *dir;
    if (!path.empty() && path.back() != '/') {
      path.push_back('/');
    }
    path += "dag-frame";
    path += std::to_string(frame_id);
    path += "-chunk";
    path += std::to_string(chunk_seq_id);
    path += "-";
    path += stageStr;
    path += ".";
    path += formatExtension(fmt);

    std::string contents;
    switch (fmt) {
      case DumpFormat::Json:
        contents = serializeDagJson(snapshot);
        break;
      case DumpFormat::Dot:
        contents = serializeDagDot(snapshot);
        break;
      case DumpFormat::Mermaid:
        contents = serializeDagMermaid(snapshot);
        break;
    }

    std::ofstream out(path, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!out.is_open() || !(out << contents)) {
      if (!warned.exchange(true)) {
        util::logf(util::LogLevel::Warn, "dxmt9-renderer",
                   "DAG dump: cannot write '%s' (dump dir unwritable?); "
                   "skipping further DAG dumps",
                   path.c_str());
      }
      return;  // never fail a render; subsequent dumps are skipped silently
    }
  }
}

void writeDagDump(const FrameGraph& fg, std::uint64_t frame_id,
                  std::uint64_t chunk_seq_id, const char* stage,
                  const core::ChunkSlot* slot) {
  writeDagDump(
      fg, frame_id, chunk_seq_id, stage,
      slot ? core::SourcePayloadView(*slot) : core::SourcePayloadView{});
}

}  // namespace dxmt9::framegraph
