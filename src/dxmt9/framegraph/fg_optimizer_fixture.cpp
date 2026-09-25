#include "fg_optimizer_fixture.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <unistd.h>

namespace dxmt9::framegraph {
namespace {
constexpr std::size_t kMaxBytes = 8 * 1024 * 1024;
constexpr u64 kMaxItems = 65536;

template<bool Reading> struct Archive {
  static constexpr bool reading = Reading;
  std::stringstream stream;
  u64 words = 0;
  template<typename T> void operator()(T& value, u64 max = UINT64_MAX) {
    if (++words > 1048576) throw std::runtime_error("fixture word limit");
    if constexpr (Reading) {
      std::string token;
      if (!(stream >> token) || token.size() > 20)
        throw std::runtime_error("truncated/oversize fixture scalar");
      u64 number = 0;
      auto [end, ec] = std::from_chars(token.data(), token.data()+token.size(), number);
      if (ec != std::errc{} || end != token.data()+token.size() || number > max)
        throw std::runtime_error("invalid fixture scalar");
      if constexpr (!std::is_enum_v<T>) {
        if (number > static_cast<u64>(std::numeric_limits<T>::max()))
          throw std::runtime_error("fixture scalar overflow");
      }
      value = static_cast<T>(number);
    } else {
      if (static_cast<u64>(value) > max) throw std::runtime_error("invalid fixture value");
      stream << static_cast<u64>(value) << ' ';
    }
  }
  template<typename V, typename F> void vector(V& values, F fields, u64 max = kMaxItems) {
    u64 size = values.size();
    (*this)(size, max);
    if constexpr (Reading) values.resize(size);
    for (auto& value : values) fields(value);
  }
};

// One field list for both directions. Never serialize padding or vector internals.
template<typename A, typename F> void fields(A& a, F& f) {
  a(f.sequence); a(f.process); a(f.backend); a(f.observed_ns); a(f.return_diagnostics, 1);
  auto& g = f.graph;
  a(g.frame_id); a(g.flush_boundary, 1);
  a.vector(g.passes, [&](auto& p) {
    a(p.kind, 4);
    for (auto& h : p.targets.color) a(h.value);
    a(p.targets.depth.value); a(p.targets.color_count, 8); a(p.targets.sample_count);
    a(p.draws.first); a(p.draws.count); a(p.commands.first); a(p.commands.count);
    a(p.state_profile);
    for (auto& x : p.load_store.color_load) a(x, 2);
    for (auto& x : p.load_store.color_store) a(x, 1);
    a(p.load_store.depth_load, 2); a(p.load_store.depth_store, 1);
    a(p.flags.contains_lock, 1); a(p.flags.contains_occlusion_query, 1);
    a(p.flags.contains_event_query, 1); a(p.flags.dead, 1);
    a(p.flags.debug_marker, 1); a(p.flags.active_render_seed, 1);
  }, 4096);
  a.vector(g.resources, [&](auto& r) {
    a(r.handle.value); a(r.first_use_pass); a(r.last_use_pass); a(r.residency, 2);
    a(r.classifier_flags.lock_seen, 1); a(r.classifier_flags.readback_seen, 1);
    a(r.classifier_flags.cross_frame_seen, 1);
    a.vector(r.accesses, [&](auto& x) {
      a(x.pass_index); a(x.access_kind, 4); a(x.stage, 3);
    });
  });
  a.vector(g.edges, [&](auto& e) { a(e.src_pass); a(e.dst_pass); a(e.resource.value); });
  a.vector(g.draws, [&](auto& d) { a(d.command_index); a(d.param_first); a(d.param_count); });
  a.vector(g.commands, [&](auto& c) { a(c.command_index); a(c.kind, 8); });
}

void validate(const FrameGraph& g) {
  for (const auto& p : g.passes) {
    if (u64(p.draws.first)+p.draws.count > g.draws.size() ||
        u64(p.commands.first)+p.commands.count > g.commands.size())
      throw std::runtime_error("fixture pass range");
  }
  for (const auto& r : g.resources)
    for (const auto& a : r.accesses)
      if (a.pass_index >= g.passes.size()) throw std::runtime_error("fixture access index");
  for (const auto& e : g.edges)
    if (e.src_pass >= g.passes.size() || e.dst_pass >= g.passes.size())
      throw std::runtime_error("fixture edge index");
}

bool captureBounded(const FrameGraph& g) {
  if (g.commands.size() < 128 || g.commands.size() > kMaxItems ||
      g.passes.size() > 4096 || g.resources.size() > kMaxItems ||
      g.edges.size() > kMaxItems || g.draws.size() > kMaxItems) return false;
  u64 accesses = 0;
  for (const auto& r : g.resources) {
    if (r.accesses.size() > kMaxItems) return false;
    accesses += r.accesses.size();
    if (accesses > kMaxItems) return false;
  }
  return true;
}
}  // namespace

std::string encodeOptimizerFixture(const OptimizerFixture& f) {
  validate(f.graph);
  Archive<false> a;
  a.stream << "DXMT9FG1 ";
  fields(a, f);
  auto bytes = a.stream.str();
  if (bytes.size() > kMaxBytes) throw std::runtime_error("fixture byte limit");
  return bytes;
}

OptimizerFixture decodeOptimizerFixture(std::string_view bytes) {
  if (bytes.size() > kMaxBytes) throw std::runtime_error("fixture byte limit");
  Archive<true> a;
  a.stream.str(std::string(bytes));
  std::string magic;
  a.stream >> magic;
  if (magic != "DXMT9FG1") throw std::runtime_error("fixture schema");
  OptimizerFixture f;
  fields(a, f);
  a.stream >> std::ws;
  if (!a.stream.eof()) throw std::runtime_error("fixture trailing data");
  validate(f.graph);
  return f;
}

bool equalOptimizerGraph(const FrameGraph& a, const FrameGraph& b) {
  return a.passes == b.passes && a.resources == b.resources &&
      a.edges == b.edges && a.draws == b.draws && a.commands == b.commands &&
      a.frame_id == b.frame_id && a.flush_boundary == b.flush_boundary;
}

bool optimizerFixtureEnabled() noexcept {
  static const bool enabled = [] {
    const char* p = std::getenv("DXMT9_FRAMEGRAPH_OPTIMIZER_FIXTURE_DIR");
    return p && *p && std::string_view(p) != "0";
  }();
  return enabled;
}

void runOptimizerWithFixture(FrameGraph& graph, const OptimizerOptions& options,
                             OptimizerStats* stats, DceLookaheadProof lookahead,
                             u64 sequence, const void* backend) {
  static std::atomic<unsigned> published{0};
  std::optional<OptimizerFixture> input;
  // The only admitted algorithm configuration is the production default.
  // DCE/lookahead and memoryless histories are not represented by this schema.
  if (published.load() < 4 && options.passcoalesce && !options.dce &&
      !options.memoryless && !options.reorder && captureBounded(graph)) {
    try {
      input.emplace(OptimizerFixture{graph, sequence, static_cast<u64>(getpid()),
          reinterpret_cast<std::uintptr_t>(backend), 0,
          options.collect_passcoalesce_return_diagnostics});
    } catch (...) { /* Capture failure must not skip or retry production work. */ }
  }
  const auto start = std::chrono::steady_clock::now();
  runOptimizer(graph, options, nullptr, stats, lookahead);
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now()-start).count();
  if (!input || elapsed < 5000000 || published.fetch_add(1) >= 4) return;
  try {
    input->observed_ns = elapsed;
    const auto bytes = encodeOptimizerFixture(*input);
    // Decode and compare all fields before publishing a usable fixture.
    if (!equalOptimizerGraph(input->graph, decodeOptimizerFixture(bytes).graph)) return;
    const auto path = std::string(std::getenv("DXMT9_FRAMEGRAPH_OPTIMIZER_FIXTURE_DIR"))+
        "/optimizer-"+std::to_string(input->process)+"-"+
        std::to_string(input->backend)+"-"+std::to_string(sequence)+".fginput";
    if (FILE* file = std::fopen(path.c_str(), "wx")) {
      const bool wrote = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
      const bool closed = std::fclose(file) == 0;
      std::fprintf(stderr, "[optimizer-fixture] seq=%llu bytes=%zu complete=%u path=%s\n",
          static_cast<unsigned long long>(sequence), bytes.size(), wrote && closed, path.c_str());
    }
  } catch (...) { /* The app already executed its original optimizer once. */ }
}
}  // namespace dxmt9::framegraph
