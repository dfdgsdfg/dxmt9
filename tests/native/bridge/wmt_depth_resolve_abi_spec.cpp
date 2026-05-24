// R-FORMAT-11 — winemetal depth-resolve ABI surface.
//
// Pins the WMTDepthAttachmentInfo multisample depth-resolve fields and the
// WMTMultisampleDepthResolveFilter enum added for the RESZ depth resolve.
// This is the DEPTH twin of the color resolve that already rides on
// WMTColorAttachmentInfo.resolve_texture + WMTStoreActionMultisampleResolve.
//
// The fields are shared in a single header (src/winemetal/winemetal.h) that
// both the PE-side winemetal.dll and the unix-side winemetal.so compile, and
// the bridge abi-hash (dxmt9::bridge::kBridgeAbiHash) is codegen from that
// header — so the PE and unix struct views cannot diverge. This spec asserts
// the value-level contract (enum codes, zero-init = "no resolve", field
// presence) that the unix mapping in winemetal_private_api.mm relies on. The
// actual GPU depth resolve needs an MSAA depth surface + readback and is
// deferred to a runtime probe.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <type_traits>

#include "../../../src/winemetal/winemetal.h"

namespace {

int g_failures = 0;

template <typename A, typename B>
void checkEq(const A& left, const B& right, std::string_view message) {
  if (!(left == right)) {
    std::cerr << "FAIL: " << message << " (left != right)\n";
    ++g_failures;
  }
}

void testFilterEnumValues() {
  // Codes must match MTLMultisampleDepthResolveFilter (Sample=0, Min=1,
  // Max=2). The unix mapping casts the wire enum straight to the Metal enum,
  // so the integer values are load-bearing.
  checkEq(static_cast<int>(WMTMultisampleDepthResolveFilterSample), 0,
          "WMTMultisampleDepthResolveFilterSample must be 0");
  checkEq(static_cast<int>(WMTMultisampleDepthResolveFilterMin), 1,
          "WMTMultisampleDepthResolveFilterMin must be 1");
  checkEq(static_cast<int>(WMTMultisampleDepthResolveFilterMax), 2,
          "WMTMultisampleDepthResolveFilterMax must be 2");
}

void testDepthAttachmentResolveFieldsPresentAndZeroInit() {
  // Field presence is a compile-time contract; assigning proves the members
  // exist with the expected types.
  WMTDepthAttachmentInfo depth{};
  // Zero-init must mean "no resolve" — the unix importer only wires the
  // resolve when resolve_texture is non-null, so a default-constructed depth
  // attachment must not accidentally request a resolve.
  checkEq(static_cast<unsigned long long>(depth.resolve_texture), 0ull,
          "zero-init WMTDepthAttachmentInfo.resolve_texture must be 0 (no resolve)");
  checkEq(static_cast<int>(depth.resolve_filter), 0,
          "zero-init resolve_filter must be Sample (0)");

  // The resolve shape mirrors the color attachment: an obj_handle_t resolve
  // target. Round-trip a sentinel handle + filter through the struct.
  depth.resolve_texture = static_cast<obj_handle_t>(0xDEADBEEFull);
  depth.resolve_filter = WMTMultisampleDepthResolveFilterSample;
  depth.store_action = WMTStoreActionMultisampleResolve;
  checkEq(static_cast<unsigned long long>(depth.resolve_texture), 0xDEADBEEFull,
          "resolve_texture must round-trip the assigned handle");
  checkEq(static_cast<int>(depth.store_action),
          static_cast<int>(WMTStoreActionMultisampleResolve),
          "store_action must accept WMTStoreActionMultisampleResolve");

  // resolve_texture must be the same wire type as the color attachment's
  // resolve_texture — they are marshalled by the same obj_handle_t path.
  static_assert(
      std::is_same_v<decltype(WMTDepthAttachmentInfo{}.resolve_texture),
                     decltype(WMTColorAttachmentInfo{}.resolve_texture)>,
      "depth/color resolve_texture must share the obj_handle_t wire type");
}

void testStoreActionContract() {
  // The DEPTH resolve reuses the existing store-action enum; the resolve
  // branch keys on WMTStoreActionMultisampleResolve (= 2), identical to the
  // color resolve. Pin the value so a reorder of the enum is caught here.
  checkEq(static_cast<int>(WMTStoreActionMultisampleResolve), 2,
          "WMTStoreActionMultisampleResolve must be 2 (depth + color share it)");
}

}  // namespace

int main() {
  testFilterEnumValues();
  testDepthAttachmentResolveFieldsPresentAndZeroInit();
  testStoreActionContract();
  if (g_failures != 0) {
    std::cerr << "wmt_depth_resolve_abi_spec failed: " << g_failures
              << " check(s)\n";
    return EXIT_FAILURE;
  }
  std::cout << "wmt_depth_resolve_abi_spec passed\n";
  return EXIT_SUCCESS;
}
