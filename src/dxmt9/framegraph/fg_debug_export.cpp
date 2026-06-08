// Frame Graph DAG debug export (Task B10, L1).
//
// Spec: specs/d3d9-renderer/design.md §3.5, requirements.md R-BACK-39.7.
//
// See fg_debug_export.hpp for the ownership / determinism / one-snapshot
// contract. This file holds the pure serializers and the side-effect-neutral
// file-writing helpers.

#include "fg_debug_export.hpp"

#include "util/log/log.hpp"

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

// "<Load>/<Store>" for the load_store JSON / labels (matches design.md sample).
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

}  // namespace

DagSnapshot buildSnapshot(const FrameGraph& fg, std::uint64_t chunk_seq_id,
                          const char* stage) {
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
       << "\" } }";
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
                             const char* stage) {
  return serializeDagJson(buildSnapshot(fg, chunk_seq_id, stage));
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

bool chunkContainsPresent(const core::ChunkSlot& slot) {
  for (const auto& header : slot.commandHeaders) {
    if (header.kind == core::MetalCommandKind::Present) {
      return true;
    }
  }
  return false;
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
                  std::uint64_t chunk_seq_id, const char* stage) {
  const std::optional<std::string> dir = dumpDagDir();
  if (!dir.has_value()) {
    return;
  }

  // Resolve formats once (env read each call is cheap and keeps writeDagDump
  // free of additional process-global state).
  const std::vector<DumpFormat> formats =
      resolveDumpFormats(std::getenv("DXMT9_RENDERER_DUMP_DAG_FORMATS"));

  // One field-walk, reused by every selected format (R-BACK-39.7).
  const DagSnapshot snapshot = buildSnapshot(fg, chunk_seq_id, stage);

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

}  // namespace dxmt9::framegraph
