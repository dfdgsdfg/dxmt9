// Host coverage for the pure core of the DXMT9_PE_THREAD_SAMPLER diagnostic.
//
// The sampling thread itself is Win32-only and can only be compile-checked on
// the PE lanes, so everything that has a decidable answer — module-range
// normalization, PC -> module classification, the bounded self-PC histogram,
// and the deterministic top-N selections that shape the emitted lines — lives
// in host-compilable pure functions and is pinned here.

#include "d3d9_pe_thread_sampler.hpp"

#include <cstdint>
#include <iostream>
#include <vector>

using dxmt9::d3d9::pe::classifyPeSamplerPc;
using dxmt9::d3d9::pe::kPeSamplerInvalidIndex;
using dxmt9::d3d9::pe::kPeSamplerSelfPcGranularity;
using dxmt9::d3d9::pe::kPeSamplerSelfPcMaxUsed;
using dxmt9::d3d9::pe::kPeSamplerSelfPcSlots;
using dxmt9::d3d9::pe::normalizePeSamplerModuleRanges;
using dxmt9::d3d9::pe::PeSamplerModuleRange;
using dxmt9::d3d9::pe::PeSamplerSelfPcTable;
using dxmt9::d3d9::pe::PeSamplerTopBucket;
using dxmt9::d3d9::pe::PeSamplerTopModule;
using dxmt9::d3d9::pe::peSamplerBucketForPc;
using dxmt9::d3d9::pe::recordPeSamplerSelfPc;
using dxmt9::d3d9::pe::selectTopPeSamplerModules;
using dxmt9::d3d9::pe::selectTopPeSamplerSelfPc;
using dxmt9::d3d9::pe::clampPeSamplerHz;

namespace {

int failures = 0;

bool check(bool condition, const char* what) {
  if (!condition) {
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
  }
  return condition;
}

bool testNormalizeSortsAndDropsUnusable() {
  PeSamplerModuleRange ranges[6]{};
  ranges[0] = {0x20000000, 0x1000, 0};
  ranges[1] = {0x10000000, 0x1000, 1};
  ranges[2] = {0x30000000, 0x0, 2};       // zero size: dropped
  ranges[3] = {0x10000800, 0x1000, 3};    // overlaps nameIndex 1: dropped
  ranges[4] = {0x40000000, 0x1000, 4};
  ranges[5] = {UINT64_MAX, 0x10, 5};      // clamps to zero size: dropped

  const std::size_t kept = normalizePeSamplerModuleRanges(ranges, 6);
  bool ok = check(kept == 3, "normalize keeps 3 usable ranges");
  ok &= check(ranges[0].nameIndex == 1, "normalize sorts by base (0x10000000)");
  ok &= check(ranges[1].nameIndex == 0, "normalize sorts by base (0x20000000)");
  ok &= check(ranges[2].nameIndex == 4, "normalize sorts by base (0x40000000)");
  ok &= check(normalizePeSamplerModuleRanges(nullptr, 4) == 0,
              "normalize tolerates a null table");
  ok &= check(normalizePeSamplerModuleRanges(ranges, 0) == 0,
              "normalize tolerates an empty table");
  return ok;
}

bool testClassifyPc() {
  PeSamplerModuleRange ranges[3]{};
  ranges[0] = {0x10000000, 0x100000, 7};   // d3d9.dll
  ranges[1] = {0x10200000, 0x10000, 8};    // winemetal_dxmt9.dll
  ranges[2] = {0x00400000, 0x50000, 9};    // game.exe
  const std::size_t kept = normalizePeSamplerModuleRanges(ranges, 3);
  bool ok = check(kept == 3, "classify fixture keeps every range");

  // Exact bounds: base is inside, base+size is not.
  ok &= check(ranges[classifyPeSamplerPc(ranges, kept, 0x10000000)].nameIndex == 7,
              "classify: first byte of a module belongs to it");
  ok &= check(ranges[classifyPeSamplerPc(ranges, kept, 0x100FFFFF)].nameIndex == 7,
              "classify: last byte of a module belongs to it");
  ok &= check(classifyPeSamplerPc(ranges, kept, 0x10100000) == kPeSamplerInvalidIndex,
              "classify: one past a module is unknown");
  ok &= check(ranges[classifyPeSamplerPc(ranges, kept, 0x00400010)].nameIndex == 9,
              "classify: lowest-based module resolves");
  ok &= check(ranges[classifyPeSamplerPc(ranges, kept, 0x10203000)].nameIndex == 8,
              "classify: highest-based module resolves");

  // Below every base, in a gap, and above every range.
  ok &= check(classifyPeSamplerPc(ranges, kept, 0x1000) == kPeSamplerInvalidIndex,
              "classify: address below every module is unknown");
  ok &= check(classifyPeSamplerPc(ranges, kept, 0x08000000) == kPeSamplerInvalidIndex,
              "classify: address in an inter-module gap is unknown");
  ok &= check(classifyPeSamplerPc(ranges, kept, 0x7fff00000000ull) ==
                  kPeSamplerInvalidIndex,
              "classify: 64-bit host address is unknown");
  ok &= check(classifyPeSamplerPc(ranges, 0, 0x10000000) == kPeSamplerInvalidIndex,
              "classify: empty table is unknown");
  ok &= check(classifyPeSamplerPc(nullptr, 3, 0x10000000) == kPeSamplerInvalidIndex,
              "classify: null table is unknown");

  // The binary search must agree with a linear scan over the whole fixture.
  for (std::uint64_t pc = 0x003ffff0; pc < 0x00400060; ++pc) {
    std::size_t linear = kPeSamplerInvalidIndex;
    for (std::size_t i = 0; i < kept; ++i) {
      if (pc >= ranges[i].base && pc - ranges[i].base < ranges[i].size) {
        linear = i;
        break;
      }
    }
    if (!check(classifyPeSamplerPc(ranges, kept, pc) == linear,
               "classify agrees with a linear scan across a module edge")) {
      return false;
    }
  }
  return ok;
}

bool testSelfPcHistogram() {
  PeSamplerSelfPcTable table;
  bool ok = check(peSamplerBucketForPc(0x1000003f) == 0x10000000,
                  "bucket granularity is 64 bytes");
  ok &= check(kPeSamplerSelfPcGranularity == 64, "granularity constant is 64");

  // Every PC inside one 64-byte bucket lands in the same slot.
  for (std::uint64_t offset = 0; offset < 64; ++offset) {
    recordPeSamplerSelfPc(table, 0x10000000 + offset);
  }
  recordPeSamplerSelfPc(table, 0x10000040);
  ok &= check(table.used == 2, "two distinct buckets are stored");
  ok &= check(table.overflow == 0, "no overflow below the load cap");

  PeSamplerTopBucket top[4]{};
  std::size_t rows = selectTopPeSamplerSelfPc(table, top, 4);
  ok &= check(rows == 2, "top-N reports both buckets");
  ok &= check(top[0].bucket == 0x10000000 && top[0].samples == 64,
              "top-N ranks the 64-sample bucket first");
  ok &= check(top[1].bucket == 0x10000040 && top[1].samples == 1,
              "top-N ranks the 1-sample bucket second");

  // Fill past the load cap: existing buckets keep counting, new ones overflow.
  PeSamplerSelfPcTable full;
  for (std::size_t i = 0; i < kPeSamplerSelfPcMaxUsed; ++i) {
    recordPeSamplerSelfPc(full, 0x20000000ull + i * 64ull);
  }
  ok &= check(full.used == kPeSamplerSelfPcMaxUsed, "load cap is reached exactly");
  ok &= check(full.overflow == 0, "no overflow at exactly the load cap");
  recordPeSamplerSelfPc(full, 0x20000000ull);  // existing key
  ok &= check(full.overflow == 0, "an existing bucket still counts when full");
  recordPeSamplerSelfPc(full, 0x70000000ull);  // new key
  ok &= check(full.overflow == 1, "a new bucket beyond the cap overflows");
  ok &= check(full.used == kPeSamplerSelfPcMaxUsed, "overflow does not grow used");
  ok &= check(kPeSamplerSelfPcMaxUsed < kPeSamplerSelfPcSlots,
              "load cap leaves probe headroom");
  return ok;
}

bool testDeterministicTieBreaks() {
  // Buckets: equal sample counts must order by ascending bucket address.
  PeSamplerSelfPcTable table;
  const std::uint64_t buckets[4] = {0x30000100, 0x30000040, 0x30000200,
                                    0x30000080};
  for (std::uint64_t bucket : buckets) {
    recordPeSamplerSelfPc(table, bucket);
  }
  PeSamplerTopBucket top[4]{};
  std::size_t rows = selectTopPeSamplerSelfPc(table, top, 4);
  bool ok = check(rows == 4, "all four equal-count buckets are reported");
  ok &= check(top[0].bucket == 0x30000040 && top[1].bucket == 0x30000080 &&
                  top[2].bucket == 0x30000100 && top[3].bucket == 0x30000200,
              "equal-count buckets order by ascending address");

  // Capacity: only the top 2 survive, and the tie-break decides which.
  ok &= check(selectTopPeSamplerSelfPc(table, top, 0) == 0,
              "zero-capacity bucket selection returns nothing");
  ok &= check(selectTopPeSamplerSelfPc(table, nullptr, 4) == 0,
              "null bucket output returns nothing");

  PeSamplerTopBucket capped[2]{};
  rows = selectTopPeSamplerSelfPc(table, capped, 2);
  ok &= check(rows == 2, "capped selection returns exactly the capacity");
  ok &= check(capped[0].bucket == 0x30000040 && capped[1].bucket == 0x30000080,
              "capped selection keeps the deterministic winners");

  // Modules: equal sample counts must order by ascending name index.
  std::uint64_t samples[6] = {5, 0, 5, 9, 5, 1};
  PeSamplerTopModule modules[6]{};
  rows = selectTopPeSamplerModules(samples, 6, modules, 6);
  ok &= check(rows == 5, "zero-count modules are omitted");
  ok &= check(modules[0].nameIndex == 3 && modules[0].samples == 9,
              "modules rank by descending samples");
  ok &= check(modules[1].nameIndex == 0 && modules[2].nameIndex == 2 &&
                  modules[3].nameIndex == 4,
              "equal-count modules order by ascending name index");
  ok &= check(modules[4].nameIndex == 5 && modules[4].samples == 1,
              "the smallest count sorts last");

  PeSamplerTopModule cappedModules[2]{};
  rows = selectTopPeSamplerModules(samples, 6, cappedModules, 2);
  ok &= check(rows == 2 && cappedModules[0].nameIndex == 3 &&
                  cappedModules[1].nameIndex == 0,
              "capped module selection keeps the deterministic winners");
  ok &= check(selectTopPeSamplerModules(samples, 6, modules, 0) == 0,
              "zero-capacity module selection returns nothing");
  ok &= check(selectTopPeSamplerModules(nullptr, 6, modules, 6) == 0,
              "null module counters return nothing");
  return ok;
}

bool testHzClamp() {
  bool ok = check(clampPeSamplerHz(0) == 50, "hz clamps up to the 50 floor");
  ok &= check(clampPeSamplerHz(49) == 50, "hz below the floor clamps up");
  ok &= check(clampPeSamplerHz(250) == 250, "hz inside the range is preserved");
  ok &= check(clampPeSamplerHz(1001) == 1000, "hz above the ceiling clamps down");
  ok &= check(clampPeSamplerHz(0xffffffffu) == 1000, "hz saturates at 1000");
  return ok;
}

}  // namespace

int main() {
  testNormalizeSortsAndDropsUnusable();
  testClassifyPc();
  testSelfPcHistogram();
  testDeterministicTieBreaks();
  testHzClamp();
  if (failures != 0) {
    std::cerr << failures << " check(s) failed\n";
    return 1;
  }
  return 0;
}
