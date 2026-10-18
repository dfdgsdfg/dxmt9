#include "d3d9_pe_render_tape_arm_request.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>

using namespace dxmt9::d3d9;
using State = RenderTapeArmRequestState;
using Input = RenderTapeArmRequestInput;
using Decision = RenderTapeArmRequestDecision;

void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

RenderTapeArmRequestGate gate() {
  return {.processId = 7u, .processCreation = 11u, .nonce = {13u, 17u},
          .readyMs = 100u, .lastMs = 100u};
}
RenderTapeArmRequest request() {
  return {.processId = 7u, .processCreation = 11u, .nonce = {13u, 17u},
          .issuedMs = 110u, .expiresMs = 200u};
}

// Reference automaton deliberately has no packet predicate. Distinguishing
// actions have independently known validity; field-level tests bind validity.
std::uint64_t histories = 0u;
void explore(RenderTapeArmRequestGate production, State reference,
             unsigned accepts, unsigned depth) {
  if (!depth) { ++histories; return; }
  for (unsigned action = 0; action < 7u; ++action) {
    auto next = production;
    auto packet = request();
    auto input = Input::Missing;
    std::uint64_t now = 120u;
    if (action == 1u) input = Input::WriterBusy;
    if (action == 2u) input = Input::Ready;
    if (action == 3u) input = Input::Invalid;
    if (action == 4u) { input = Input::Ready; ++packet.nonce[1]; }
    if (action == 5u) { input = Input::Ready; now = 200u; }
    if (action == 6u) now = 600100u;
    const auto decision = next.observe(input, packet, now);
    const bool waiting = reference == State::Waiting;
    const auto expected = !waiting ? Decision::Terminal :
        action < 2u ? Decision::Wait : action == 2u ? Decision::Accept : Decision::Reject;
    require(decision == expected, "production/refinement transition mismatch");
    const auto nextReference = !waiting || action < 2u ? reference :
        action == 2u ? State::Consumed : State::Rejected;
    require(next.state == nextReference, "terminal ownership mismatch");
    const unsigned nextAccepts = accepts + (decision == Decision::Accept);
    require(nextAccepts <= 1u, "duplicate request starts second bootstrap");
    // Abort/reset/retry are deliberately not transitions in the gate: later
    // observations must keep the consumed capability terminal.
    explore(next, nextReference, nextAccepts, depth - 1u);
  }
}

int main() {
  try {
    std::array<std::uint64_t, 2> nonce{99u, 99u};
    for (auto text : {"", "0", "00000000000000000000000000000000",
                      "0000000000000000000000000000000G",
                      "0000000000000000000000000000000A",
                      " 0000000000000000000000000000001"})
      require(!parseRenderTapeArmNonce(text, nonce) && nonce[0] == 99u,
              "invalid nonce accepted or mutated output");
    require(parseRenderTapeArmNonce("0123456789abcdeffedcba9876543210", nonce) &&
            nonce[0] == 0x0123456789abcdefull && nonce[1] == 0xfedcba9876543210ull,
            "nonce does not round-trip");
    for (auto path : {"", "Z:", "Z:\\", "Z:relative", "/tmp/request", "\\\\host\\share"})
      require(!renderTapeArmRequestPathValid(path), "non-local/relative path admitted");
    require(renderTapeArmRequestPathValid("Z:\\tmp\\request.bin"), "valid path rejected");

    for (unsigned mutation = 0; mutation < 16u; ++mutation) {
      auto p = request();
      switch (mutation) {
      case 0: ++p.magic; break;
      case 1: ++p.version; break;
      case 2: --p.bytes; break;
      case 3: ++p.reserved; break;
      case 4: ++p.processId; break;
      case 5: ++p.processCreation; break;
      case 6: ++p.nonce[0]; break;
      case 7: ++p.nonce[1]; break;
      case 8: p.issuedMs = 99u; break;
      case 9: p.issuedMs = 121u; break;
      case 10: p.expiresMs = p.issuedMs; break;
      case 11: p.expiresMs = p.issuedMs - 1u; break;
      case 12: p.expiresMs = 120u; break;
      case 13: p.expiresMs = 119u; break;
      case 14: p.expiresMs = p.issuedMs + RenderTapeArmRequestGate::kMaxRequestMs + 1u; break;
      case 15: p.expiresMs = std::numeric_limits<std::uint64_t>::max(); break;
      }
      auto g = gate();
      require(g.observe(Input::Ready, p, 120u) == Decision::Reject,
              "malformed/stale/expired packet accepted");
      require(g.observe(Input::Ready, request(), 120u) == Decision::Terminal,
              "rejected packet permits later retry");
    }
    auto g = gate();
    require(g.observe(Input::Missing, {}, 125u) == Decision::Wait &&
            g.observe(Input::Ready, request(), 124u) == Decision::Reject,
            "backward clock accepted");
    g = gate();
    require(g.observe(Input::Ready, request(), 120u) == Decision::Accept &&
            g.mayFinishBootstrap(120u) && g.mayFinishBootstrap(199u) &&
            !g.mayFinishBootstrap(119u) && !g.mayFinishBootstrap(200u),
            "bootstrap expiry endpoints incorrect");
    g = gate();
    require(g.observe(Input::Missing, {}, 600099u) == Decision::Wait &&
            g.observe(Input::WriterBusy, {}, 600100u) == Decision::Reject,
            "missing/busy wait is not bounded");
    explore(gate(), State::Waiting, 0u, 6u);
    require(histories == 117649u, "exhaustive history count drift");
    std::cout << "arm-request refinement: " << histories << " histories passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
