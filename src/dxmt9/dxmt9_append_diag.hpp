#pragma once
#include <array>
#include <cstdint>
#include <cstdlib>

namespace dxmt9::append_diag {
enum class Phase : unsigned {
  Batch, BatchReserve, Append, Preflight, UniformLookup, LocalReserve,
  UniformAppend, State, Soa, Count
};
inline constexpr std::array names{
    "batch", "batch_reserve", "append", "preflight", "uniform_lookup",
    "local_reserve", "uniform_append", "state", "soa"};
struct Row { std::uint64_t ns = 0, calls = 0; };
using Snapshot = std::array<Row, static_cast<unsigned>(Phase::Count)>;
// Mode 1 measures enclosing scopes; mode 2 adds the disjoint append phases.
inline unsigned mode() noexcept {
  static const unsigned value = [] {
    const char* env = std::getenv("DXMT9_COMPAT_APPEND_DIAGNOSTICS");
    return env && env[0] == '1' && !env[1] ? 1u :
           env && env[0] == '2' && !env[1] ? 2u : 0u;
  }();
  return value;
}
struct Token;
Token* begin(Phase phase) noexcept;
void finish(Token* token) noexcept;
void change(Token* token, Phase phase) noexcept;
Snapshot snapshot() noexcept;
// Only this register-sized observer handle lives at callsites. Clocks and
// counters live in cold bounded storage; no production signature changes.
class Scope {
 public:
  explicit Scope(Phase phase) noexcept
      : token_(mode() && (mode() == 2 || phase <= Phase::Append)
                   ? begin(phase) : nullptr) {}
  ~Scope() { if (token_) finish(token_); }
  void set(Phase phase) noexcept { if (token_) change(token_, phase); }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
 private:
  Token* token_;
};
}
