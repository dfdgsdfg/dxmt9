#include "dxmt9_append_diag.hpp"
#include <atomic>
#include <chrono>
#include <cassert>

namespace dxmt9::append_diag {
using Clock = std::chrono::steady_clock;
struct Token { Phase phase; Clock::time_point start; };
namespace {
struct Counter { std::atomic<std::uint64_t> ns{0}, calls{0}; };
std::array<Counter, names.size()> counters;
thread_local std::array<Token, 16> stack;
thread_local unsigned depth = 0;
void charge(Token& token, Clock::time_point end) noexcept {
  auto& row = counters[static_cast<unsigned>(token.phase)];
  row.ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
      end-token.start).count(), std::memory_order_relaxed);
  row.calls.fetch_add(1, std::memory_order_relaxed);
}
}
Token* begin(Phase phase) noexcept {
  // Scope admission belongs to ordinary compatibility batches only.
  if (!depth && phase != Phase::Batch) return nullptr;
  if (depth == stack.size()) std::abort();
  auto* token = &stack[depth++];
  token->phase = phase;
  token->start = Clock::now();
  return token;
}
void finish(Token* token) noexcept {
  assert(depth && token == &stack[depth-1]);
  charge(*token, Clock::now());
  --depth;
}
void change(Token* token, Phase phase) noexcept {
  assert(depth && token == &stack[depth-1]);
  const auto now = Clock::now();
  charge(*token, now);
  token->phase = phase;
  token->start = now;
}
Snapshot snapshot() noexcept {
  Snapshot result{};
  for (unsigned i = 0; i < result.size(); ++i) {
    result[i] = {counters[i].ns.load(std::memory_order_relaxed),
                 counters[i].calls.load(std::memory_order_relaxed)};
  }
  return result;
}
}
