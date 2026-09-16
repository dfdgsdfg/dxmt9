#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace dxmt9::test {

// Test-controller-only diagnostics. Never used by the production renderer or
// accepted as low-observer policy timing. Nested phases pause their parent;
// process CPU includes all process threads, not just the controller thread.
enum class CpuDiagnosticPhase : std::uint8_t {
  Bookkeeping, FixtureSetup, Admission, HarnessSetup, ObjectSetup, Mutation,
  Replay, Publication, Completion, Cleanup, FixtureTeardown, Count,
};
inline constexpr std::array kCpuDiagnosticPhaseNames{
    "bookkeeping", "fixture_setup", "admission", "harness_setup", "object_setup",
    "mutation", "replay", "publication", "completion", "cleanup", "fixture_teardown"};
static_assert(kCpuDiagnosticPhaseNames.size() ==
              static_cast<std::size_t>(CpuDiagnosticPhase::Count));

struct CpuDiagnosticSample {
  std::uint64_t wall = 0u;
  std::uint64_t cpu = 0u;
  bool valid = true;
};

class CpuDiagnosticLedger {
 public:
  using Clock = CpuDiagnosticSample (*)() noexcept;
  struct Row {
    std::uint64_t wall = 0u, cpu = 0u, entries = 0u;
  };

  explicit CpuDiagnosticLedger(Clock clock) noexcept
      : clock_(clock), first_(clock()), last_(first_), valid_(first_.valid) {
    rows_[0].entries = 1u;
  }
  CpuDiagnosticLedger(const CpuDiagnosticLedger&) = delete;
  CpuDiagnosticLedger& operator=(const CpuDiagnosticLedger&) = delete;
  CpuDiagnosticLedger(CpuDiagnosticLedger&&) = default;

  class Scope {
   public:
    Scope(CpuDiagnosticLedger* ledger, CpuDiagnosticPhase phase) noexcept
        : ledger_(ledger), previous_(ledger ? ledger->active_ : phase) {
      set(phase);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    ~Scope() { if (ledger_) ledger_->enter(previous_); }
    void set(CpuDiagnosticPhase phase) noexcept {
      if (ledger_) ledger_->enter(phase);
    }
   private:
    CpuDiagnosticLedger* ledger_;
    CpuDiagnosticPhase previous_;
  };

  void finish() noexcept {
    if (finished_) return;
    charge(clock_());
    total_ = {last_.wall >= first_.wall ? last_.wall - first_.wall : 0u,
              last_.cpu >= first_.cpu ? last_.cpu - first_.cpu : 0u, valid_};
    finished_ = true;
  }
  bool valid() const noexcept { return finished_ && valid_; }
  const auto& rows() const noexcept { return rows_; }
  CpuDiagnosticSample total() const noexcept { return total_; }

 private:
  void charge(CpuDiagnosticSample now) noexcept {
    if (!now.valid || !last_.valid || now.wall < last_.wall || now.cpu < last_.cpu) {
      valid_ = false;
    } else {
      auto& row = rows_[static_cast<std::size_t>(active_)];
      row.wall += now.wall - last_.wall;
      row.cpu += now.cpu - last_.cpu;
    }
    last_ = now;
  }
  void enter(CpuDiagnosticPhase phase) noexcept {
    if (finished_) { valid_ = false; return; }
    charge(clock_());
    active_ = phase;
    ++rows_[static_cast<std::size_t>(phase)].entries;
  }
  Clock clock_;
  CpuDiagnosticSample first_, last_, total_{};
  bool valid_ = true, finished_ = false;
  CpuDiagnosticPhase active_ = CpuDiagnosticPhase::Bookkeeping;
  std::array<Row, kCpuDiagnosticPhaseNames.size()> rows_{};
};

}  // namespace dxmt9::test
