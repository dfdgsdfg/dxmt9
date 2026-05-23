#include <cstdlib>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "dxmt9_bridge_ops.generated.h"
#include "util/unixcall_marshal.hpp"

namespace {

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

template <typename A, typename B>
void checkEq(const A& left, const B& right, std::string_view message) {
  if (!(left == right)) {
    std::ostringstream out;
    out << message << " (" << left << " vs " << right << ")";
    fail(out.str());
  }
}

unsigned int opcode(dxmt9::bridge::BridgeOpcode value) {
  return static_cast<unsigned int>(value);
}

void testBridgeOpcodeCountMatchesEnumSpan() {
  const auto first = opcode(dxmt9::bridge::BridgeOpcode::dxmt9c_factory_create);
  const auto last =
      opcode(dxmt9::bridge::BridgeOpcode::dxmt9c_vdecl_get_declaration);

  checkEq(first, static_cast<unsigned int>(DXMT9_WINEMETAL_BRIDGE_OP_BASE),
          "device_c bridge starts after shader unix-call slots");
  checkEq(dxmt9::bridge::kBridgeOpcodeCount, 144u,
          "generated bridge opcode count");
  check(last >= first, "bridge opcode enum is monotonic");
  checkEq(last - first + 1u, dxmt9::bridge::kBridgeOpcodeCount,
          "bridge opcode count matches enum span");
}

void testDodChunkBridgeOpsStaySingleCallShape() {
  const auto first = opcode(dxmt9::bridge::BridgeOpcode::dxmt9c_factory_create);
  const auto count = dxmt9::bridge::kBridgeOpcodeCount;

  const auto commitChunk =
      opcode(dxmt9::bridge::BridgeOpcode::dxmt9c_device_commit_chunk);
  const auto drawChunk =
      opcode(dxmt9::bridge::BridgeOpcode::dxmt9c_device_draw_primitive_chunk);
  const auto drawPacket =
      opcode(dxmt9::bridge::BridgeOpcode::dxmt9c_device_draw_primitive_packet);
  const auto legacyDraw =
      opcode(dxmt9::bridge::BridgeOpcode::dxmt9c_device_draw_primitive);

  check(commitChunk >= first && commitChunk < first + count,
        "DOD commit chunk opcode is inside generated bridge range");
  check(drawChunk >= first && drawChunk < first + count,
        "DOD draw chunk opcode is inside generated bridge range");
  checkEq(drawChunk, drawPacket + 1u,
          "DOD draw chunk opcode remains adjacent to draw packet bridge op");
  check(commitChunk != legacyDraw,
        "DOD commit chunk remains distinct from legacy per-draw bridge op");
}

void testWow64OpaqueHandleRegistryKeepsRetainedTokensAlive() {
  int native = 42;
  const auto token = dxmt9::util::marshal::wow64::encodeHandle(&native);
  check(token != 0, "wow64 opaque handle token is allocated");
  checkEq(dxmt9::util::marshal::wow64::decodeHandle<int*>(token), &native,
          "wow64 opaque handle decodes to native pointer");

  check(dxmt9::util::marshal::wow64::retainHandle(token),
        "wow64 opaque handle token can be retained");
  check(dxmt9::util::marshal::wow64::releaseHandle(token),
        "first wow64 opaque handle release succeeds");
  checkEq(dxmt9::util::marshal::wow64::decodeHandle<int*>(token), &native,
          "retained wow64 opaque handle survives first release");

  check(dxmt9::util::marshal::wow64::releaseHandle(token),
        "final wow64 opaque handle release succeeds");
  checkEq(dxmt9::util::marshal::wow64::decodeHandle<int*>(token),
          static_cast<int*>(nullptr),
          "wow64 opaque handle is erased after final release");
}

void testBridgeAbiHashIsNonZeroAndStable() {
  // Codegen guarantees a non-zero hash (FNV-1a + non-empty schema). The
  // static_assert in the generated header would already fire at compile
  // time, but assert at runtime as well so a regression in the python
  // codegen that emits a literal 0 is caught by this spec target rather
  // than only by a downstream link-time error.
  check(dxmt9::bridge::kBridgeAbiHash != 0u,
        "kBridgeAbiHash must be non-zero");

  // Slot 4 is the reserved fixed-position ABI-hash slot; bridge ops must
  // start at slot 5. If either drifts, the PE-side DllMain handshake
  // and the unix-side dispatch table get out of sync.
  checkEq(static_cast<unsigned int>(DXMT9_WINEMETAL_CALL_ABI_HASH), 4u,
          "ABI-hash slot is reserved at fixed index 4");
  checkEq(static_cast<unsigned int>(DXMT9_WINEMETAL_BRIDGE_OP_BASE), 5u,
          "bridge ops start at slot 5 (after shader+abi-hash)");
}

}  // namespace

int main() {
  try {
    testBridgeOpcodeCountMatchesEnumSpan();
    testDodChunkBridgeOpsStaySingleCallShape();
    testWow64OpaqueHandleRegistryKeepsRetainedTokensAlive();
    testBridgeAbiHashIsNonZeroAndStable();
  } catch (const TestFailure& e) {
    std::cerr << "bridge_ops_spec failed: " << e.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& e) {
    std::cerr << "bridge_ops_spec unexpected exception: " << e.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "bridge_ops_spec passed\n";
  return EXIT_SUCCESS;
}
