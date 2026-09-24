#include "../../../src/dxmt9/dxmt9_prepared_encode_plan.hpp"
#include "prepared_linear_reference.generated.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <libproc.h>
#include <pthread.h>
#include <unistd.h>
#include <vector>

namespace {
using namespace dxmt9;
struct Row { core::ExactResourceSet reads{}, writes{}; };
void require(bool ok) { if (!ok) throw std::runtime_error("prepared benchmark mismatch"); }
std::vector<Row> input(unsigned commands, unsigned identities) {
  std::vector<Row> rows(commands);
  for (unsigned i = 0; i < commands; ++i) {
    auto& row = rows[i];
    row.reads.flags |= core::ExactResourceSetCanonicalized;
    row.writes.flags |= core::ExactResourceSetCanonicalized;
    for (unsigned j = 0; j < 8; ++j)
      require(row.reads.add((std::uint64_t((i * 7 + j) % identities + 1) << 32) | 7));
    require(row.writes.add(row.reads.handles[0]));
  }
  return rows;
}
template<class Key> Key key(unsigned commands) {
  return {.source = {.id = {2, 7}, .storage = {3, 2, 11}},
          .sequence = 19, .firstCommand = 0, .commandCount = commands};
}
template<class Builder, class Resolution>
void append(Builder& b, const std::vector<Row>& rows, Resolution stable) {
  for (unsigned i = 0; i < rows.size(); ++i)
    require(b.append(i, core::MetalCommandKind::DrawRun, rows[i].reads,
                     rows[i].writes, {}, stable));
}
void equivalent(const std::vector<Row>& rows) {
  const auto akey = key<encoders::PreparedSourceKey>(rows.size());
  const auto bkey = key<linear_probe::PreparedSourceKey>(rows.size());
  encoders::PreparedEncodePlanBuilder a(akey);
  linear_probe::PreparedEncodePlanBuilder b(bkey);
  append(a, rows, encoders::PreparedAccessResolution::CanonicalStable);
  append(b, rows, linear_probe::PreparedAccessResolution::CanonicalStable);
  auto ao = a.seal(); auto bo = b.seal();
  const auto* p = ao.view(akey); const auto* q = bo.view(bkey);
  require(p && q && p->identityCount == q->identityCount &&
          p->accessCount == q->accessCount && p->commandCount == q->commandCount &&
          p->rangeCount == q->rangeCount && p->needsEncodeResolution == q->needsEncodeResolution);
  for (unsigned i = 0; i < p->identityCount; ++i) require(p->identities[i] == q->identities[i]);
  for (unsigned i = 0; i < p->accessCount; ++i) require(p->accesses[i] == q->accesses[i]);
  for (unsigned i = 0; i < p->commandCount; ++i) {
    const auto& x = p->commands[i]; const auto& y = q->commands[i];
    require(x.kind == y.kind && x.readOffset == y.readOffset && x.readCount == y.readCount &&
            x.writeOffset == y.writeOffset && x.writeCount == y.writeCount &&
            unsigned(x.resolution) == unsigned(y.resolution));
  }
  for (unsigned i = 0; i < p->rangeCount; ++i) {
    const auto& x = p->ranges[i]; const auto& y = q->ranges[i];
    require(x.first == y.first && x.count == y.count && x.role == y.role && x.attachments == y.attachments);
  }
}
struct Stamp { std::uint64_t cpu, wall; };
Stamp stamp() {
  // Match the production CPU-supply observer: POSIX thread CPU clocks have
  // returned unscaled ticks under Rosetta on this host.
  static const auto tid = [] {
    std::uint64_t value = 0;
    require(pthread_threadid_np(nullptr, &value) == 0);
    return value;
  }();
  proc_threadinfo info{};
  require(proc_pidinfo(getpid(), PROC_PIDTHREADID64INFO, tid, &info,
                      sizeof(info)) == sizeof(info));
  return {info.pth_user_time + info.pth_system_time,
    std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count())};
}
struct Sample { Stamp acquire{}, append{}, seal{}, total{}; std::uint64_t checksum = 0; };
Stamp elapsed(Stamp a, Stamp b) { return {b.cpu-a.cpu, b.wall-a.wall}; }
template<class Workspace, class Key, class Resolution>
[[gnu::noinline]] Sample measure(Workspace& workspace, const std::vector<Row>& rows, Resolution stable) {
  const auto k = key<Key>(rows.size());
  Sample result;
  // Timing encloses identical full builder work, including independent seal
  // validation. Inputs are prebuilt; the workspace is warmed before samples.
  const auto begin = stamp();
  auto b = workspace.acquire(k);
  const auto acquired = stamp();
  append(b, rows, stable);
  const auto appended = stamp();
  auto owner = b.seal();
  const auto sealed = stamp();
  const auto* p = owner.view(k); require(p != nullptr);
  for (unsigned i = 0; i < p->identityCount; ++i) result.checksum += p->identities[i];
  for (unsigned i = 0; i < p->accessCount; ++i) result.checksum += p->accesses[i];
  workspace.recycle(std::move(owner)); workspace.recycle(std::move(b));
  const auto end = stamp();
  result.acquire = elapsed(begin, acquired); result.append = elapsed(acquired, appended);
  result.seal = elapsed(appended, sealed); result.total = elapsed(begin, end);
  return result;
}
void run(const char* shape, const std::vector<Row>& rows, bool reverse, bool aa) {
  equivalent(rows);
  encoders::PreparedEncodePlanWorkspace indexed, control;
  linear_probe::PreparedEncodePlanWorkspace linear;
  const auto a = [&] { return measure<decltype(indexed), encoders::PreparedSourceKey>(
      indexed, rows, encoders::PreparedAccessResolution::CanonicalStable); };
  const auto b = [&] { return aa
      ? measure<decltype(control), encoders::PreparedSourceKey>(control, rows, encoders::PreparedAccessResolution::CanonicalStable)
      : measure<decltype(linear), linear_probe::PreparedSourceKey>(linear, rows, linear_probe::PreparedAccessResolution::CanonicalStable); };
  for (unsigned i = 0; i < 2; ++i) { require(a().checksum == b().checksum); }
  std::vector<Sample> first(32), second(32);
  for (unsigned i = 0; i < 32; ++i) {
    if (bool(i % 2) != reverse) { second[i] = b(); first[i] = a(); }
    else { first[i] = a(); second[i] = b(); }
    require(first[i].checksum == second[i].checksum);
  }
  // Printing outside every timed sample avoids I/O between paired measurements.
  for (unsigned i = 0; i < 32; ++i)
    for (unsigned policy = 0; policy < 2; ++policy) {
      const auto& s = policy ? second[i] : first[i];
      std::cout << shape << ',' << (policy ? (aa ? "indexed_control" : "linear") : "indexed")
                << ',' << i << ',' << (bool(i % 2) != reverse ? "BA" : "AB")
                << ',' << s.acquire.cpu << ',' << s.append.cpu << ',' << s.seal.cpu
                << ',' << s.total.cpu << ',' << s.total.wall << ',' << s.checksum << '\n';
    }
}
}
int main(int argc, char** argv) {
  try {
    bool benchmark = false, reverse = false, aa = false;
    for (int i = 1; i < argc; ++i) {
      const std::string_view arg(argv[i]);
      if (arg == "--benchmark") benchmark = true;
      else if (arg == "--reverse") reverse = true;
      else if (arg == "--aa") aa = true;
      else throw std::runtime_error("unknown argument");
    }
    const auto small = input(64, 64), wide = input(270, 420);
    if (!benchmark) { equivalent(small); equivalent(wide); return 0; }
    std::cout << "shape,policy,sample,order,acquire_cpu_ns,append_cpu_ns,seal_cpu_ns,total_cpu_ns,total_wall_ns,checksum\n";
    run("synthetic_small", small, reverse, aa); run("synthetic_wide", wide, reverse, aa);
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
