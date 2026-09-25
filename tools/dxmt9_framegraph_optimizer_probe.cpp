#include "dxmt9/framegraph/fg_optimizer_fixture.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <libproc.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>

using namespace dxmt9::framegraph;
namespace {
u64 cpu() {
  std::uint64_t tid = 0;
  pthread_threadid_np(nullptr, &tid);
  proc_threadinfo info{};
  if (proc_pidinfo(getpid(), PROC_PIDTHREADID64INFO, tid, &info, sizeof(info)) != sizeof(info))
    throw std::runtime_error("native thread clock unavailable");
  return info.pth_user_time + info.pth_system_time;
}
u64 wall() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
void require(bool value) {
  if (!value) throw std::runtime_error("graph equality/fixture contract failed");
}
void selfTest() {
  OptimizerFixture f;
  f.sequence = 42; f.backend = 17; f.process = 9; f.observed_ns = 31;
  f.return_diagnostics = true;
  f.graph.frame_id = 7; f.graph.flush_boundary = true;
  for (u32 i = 0; i < 3; ++i) {
    PassNode p;
    p.targets.color[0] = ResourceHandle{i == 1 ? 2u : 1u}; p.targets.color_count = 1;
    p.targets.color[7] = ResourceHandle{999}; p.targets.sample_count = 4;
    p.draws = {i, 1}; p.commands = {i, 1}; p.state_profile = 123+i;
    p.flags.contains_occlusion_query = i == 1;
    p.flags.debug_marker = true;
    p.load_store.color_load[7] = LoadAction::Load;
    f.graph.passes.push_back(p);
    f.graph.draws.push_back({i, 11+i, 3});
    f.graph.commands.push_back({i, dxmt9::core::MetalCommandKind::DrawRun});
  }
  ResourceNode resource;
  resource.handle = ResourceHandle{1}; resource.classifier_flags.cross_frame_seen = true;
  resource.accesses = {{0, 1, 1}, {1, 0, 1}, {2, 1, 1}};
  f.graph.resources.push_back(resource);
  f.graph.edges = {{0, 1, ResourceHandle{1}}, {1, 2, ResourceHandle{1}}};
  const auto bytes = encodeOptimizerFixture(f);
  const auto decoded = decodeOptimizerFixture(bytes);
  require(equalOptimizerGraph(f.graph, decoded.graph));
  require(encodeOptimizerFixture(decoded) == bytes);
  auto allFields = f;
  auto& p = allFields.graph.passes[0];
  p.kind = PassKind::Sync;
  p.targets.depth = ResourceHandle{UINT64_MAX};
  p.flags = {true, true, true, true, true, true};
  p.load_store.color_load.fill(LoadAction::Clear);
  p.load_store.color_store.fill(StoreAction::Store);
  p.load_store.depth_load = LoadAction::Load;
  p.load_store.depth_store = StoreAction::Store;
  auto& r = allFields.graph.resources[0];
  r.first_use_pass = 1; r.last_use_pass = 2;
  r.residency = ResidencyClass::Memoryless;
  r.classifier_flags = {true, true, true};
  r.accesses[0] = {0, 4, 3};
  require(equalOptimizerGraph(allFields.graph,
      decodeOptimizerFixture(encodeOptimizerFixture(allFields)).graph));
  for (const auto& bad : {bytes.substr(0, bytes.size()/2), bytes+" 1", std::string("bad")}) {
    bool rejected = false;
    try { (void)decodeOptimizerFixture(bad); } catch (...) { rejected = true; }
    require(rejected);
  }
  auto invalid = f;
  invalid.graph.passes[0].commands.count = UINT32_MAX;
  bool rejected = false;
  try { (void)encodeOptimizerFixture(invalid); } catch (...) { rejected = true; }
  require(rejected);
  auto badEnum = bytes;
  badEnum.replace(badEnum.find_last_not_of(' '), 1, "9");
  rejected = false;
  try { (void)decodeOptimizerFixture(badEnum); } catch (...) { rejected = true; }
  require(rejected);
  auto off = f.graph, on = f.graph;
  OptimizerOptions options; options.passcoalesce = true;
  OptimizerStats a{}, b{};
  runOptimizer(off, options, nullptr, &a);
  options.collect_passcoalesce_return_diagnostics = true;
  runOptimizer(on, options, nullptr, &b);
  require(equalOptimizerGraph(off, on));
  require(a.pass_coalesced_count == b.pass_coalesced_count);
  require(b.pass_coalesce_return_candidates > 0);
  std::cout << "optimizer fixture self-test PASS\n";
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--runtime-image") {
      Dl_info image{};
      if (!dladdr(reinterpret_cast<void*>(&runOptimizer), &image) || !image.dli_fname)
        throw std::runtime_error("optimizer image unavailable");
      std::cout << image.dli_fname << '\n';
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--self-test") { selfTest(); return 0; }
    if (argc != 2) throw std::runtime_error("usage: optimizer-probe INPUT.fginput | --self-test");
    std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
    if (!file || file.tellg() < 0 || file.tellg() > 8*1024*1024)
      throw std::runtime_error("invalid/oversize fixture file");
    file.seekg(0);
    std::ostringstream bytes; bytes << file.rdbuf();
    const auto input = decodeOptimizerFixture(bytes.str());
    auto expected = input.graph;
    OptimizerOptions options; options.passcoalesce = true;
    OptimizerStats reference{};
    runOptimizer(expected, options, nullptr, &reference);
    std::cout << "round,diagnostics,mode,stage,wall_ns,cpu_ns,merged,candidates,passes,edges,blocked_cycle,second_non_draw,non_render_intervener,missing_invariant\n";
    // Two warmups per policy. Sixteen measured AB/BA pairs reverse order each
    // round; every phase trial starts from a fresh identical input graph.
    for (int round = -2; round < 16; ++round) {
      for (int position = 0; position < 2; ++position) {
        const bool diagnostic = ((round+2)%2) != position;
        options.collect_passcoalesce_return_diagnostics = diagnostic;
        for (bool split : {false, true}) {
          auto graph = input.graph; // copy/materialization is OUTSIDE timers
          OptimizerStats stats{};
          auto measure = [&](const char* stage, auto call) {
            const auto w0 = wall(), c0 = cpu();
            call();
            const auto c1 = cpu(), w1 = wall();
            if (c1 < c0 || w1 < w0 || c1-c0 > w1-w0+100000)
              throw std::runtime_error("invalid native clocks");
            if (round >= 0)
              std::cout << round << ',' << diagnostic << ',' << (split ? "split" : "whole")
                  << ',' << stage << ',' << w1-w0 << ',' << c1-c0 << ','
                  << stats.pass_coalesced_count << ',' << stats.pass_coalesce_return_candidates
                  << ',' << graph.passes.size() << ',' << graph.edges.size()
                  << ',' << stats.pass_coalesce_return_blocked_cycle
                  << ',' << stats.pass_coalesce_return_second_non_draw
                  << ',' << stats.pass_coalesce_return_non_render_intervener
                  << ',' << stats.pass_coalesce_return_missing_invariant << '\n';
          };
          if (split) {
            measure("lifetime", [&] { runLifetime(graph); });
            // Includes runPassCoalesce's production post-merge lifetime refresh.
            measure("coalesce", [&] { runPassCoalesce(graph, &stats, diagnostic); });
            measure("loadstore", [&] { runLoadStore(graph); });
          } else {
            measure("optimizer", [&] { runOptimizer(graph, options, nullptr, &stats); });
          }
          require(equalOptimizerGraph(graph, expected));
          require(stats.pass_coalesced_count == reference.pass_coalesced_count);
        }
      }
    }
    std::cerr << "PASS exact optimizer output equality; source=" << input.sequence
        << " process=" << input.process << " backend=" << input.backend
        << " commands=" << input.graph.commands.size()
        << " passes=" << input.graph.passes.size() << " edges=" << input.graph.edges.size() << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "unqualified: " << e.what() << '\n';
    return 1;
  }
}
