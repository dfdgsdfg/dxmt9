// Pure-data spec for the R-BACK-3.9 / R-BACK-3.10 archive-prewarm-
// hardening value transforms declared in dxmt9_archive_prewarm.hpp:
//   * parseMaxPrewarmMb() / shouldDemoteForSize() — the
//     DXMT9_ARCHIVE_MAX_PREWARM_MB size guard.
//   * shouldMilestoneSave() — the bounded mid-session save decision,
//     including the R-BACK-3.11 savePoisoned short-circuit.
// None of these functions touch a Metal device or the filesystem, so
// this spec runs without a GPU.

#include "../../../src/dxmt9/dxmt9_archive_prewarm.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using dxmt9::archive_prewarm::MilestoneState;
using dxmt9::archive_prewarm::kDefaultMilestonePresents;
using dxmt9::archive_prewarm::kDefaultQuiescencePresents;
using dxmt9::archive_prewarm::kDefaultReArmBatchEntries;
using dxmt9::archive_prewarm::parseMaxPrewarmMb;
using dxmt9::archive_prewarm::shouldDemoteForSize;
using dxmt9::archive_prewarm::shouldMilestoneSave;

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

// ---------------------------------------------------------------------
// parseMaxPrewarmMb — DXMT9_ARCHIVE_MAX_PREWARM_MB pure parse.
// ---------------------------------------------------------------------

void testParseNullIsNullopt() {
  check(parseMaxPrewarmMb(nullptr) == std::nullopt,
        "null env parses to nullopt (falls back to default)");
}

void testParseEmptyIsNullopt() {
  check(parseMaxPrewarmMb("") == std::nullopt,
        "empty-string env parses to nullopt");
}

void testParseZeroIsNullopt() {
  // "0" is documented as falling back to the default, matching the
  // repo's "0/unparseable falls back to default" numeric-env convention
  // rather than meaning "no cap".
  check(parseMaxPrewarmMb("0") == std::nullopt,
        "\"0\" parses to nullopt (falls back to default, not \"uncapped\")");
}

void testParseGarbageIsNullopt() {
  check(parseMaxPrewarmMb("not-a-number") == std::nullopt,
        "non-numeric env parses to nullopt");
}

void testParseValidNumber() {
  check(parseMaxPrewarmMb("256") == std::optional<std::uint64_t>(256),
        "\"256\" parses to 256");
}

void testParseLeadingDigitsStopAtFirstNonDigit() {
  // strtoull semantics: parses the numeric prefix, ignores trailing
  // garbage, matching every other numeric env parser in this codebase.
  check(parseMaxPrewarmMb("128MB") == std::optional<std::uint64_t>(128),
        "\"128MB\" parses the numeric prefix 128");
}

// ---------------------------------------------------------------------
// shouldDemoteForSize
// ---------------------------------------------------------------------

void testDemoteWhenOverGuard() {
  check(shouldDemoteForSize(600ull * (1ull << 20), 512ull * (1ull << 20)),
        "600 MiB archive with a 512 MiB guard demotes");
}

void testNoDemoteWhenUnderGuard() {
  check(!shouldDemoteForSize(100ull * (1ull << 20), 512ull * (1ull << 20)),
        "100 MiB archive with a 512 MiB guard does not demote");
}

void testNoDemoteWhenExactlyAtGuard() {
  // Strictly-greater comparison: exactly-at-guard is allowed through.
  check(!shouldDemoteForSize(512ull * (1ull << 20), 512ull * (1ull << 20)),
        "archive exactly at the guard does not demote");
}

void testNoDemoteWhenMissingFile() {
  check(!shouldDemoteForSize(0, 512ull * (1ull << 20)),
        "zero-byte (missing) archive never demotes");
}

// ---------------------------------------------------------------------
// shouldMilestoneSave — R-BACK-3.10 bounded mid-session save decision.
// ---------------------------------------------------------------------

MilestoneState baseState() {
  MilestoneState s{};
  s.presentCount = kDefaultMilestonePresents;
  s.lastNewEntryPresent = 1;  // quiescent: milestone - 1 >> quiescencePresents
  s.entriesAddedThisSession = 10;
  s.entriesAddedAtLastSave = 0;
  s.savesPerformed = 0;
  s.savePoisoned = false;
  return s;
}

void testNoSaveBeforeMilestonePresents() {
  MilestoneState s = baseState();
  s.presentCount = kDefaultMilestonePresents - 1;
  check(!shouldMilestoneSave(s),
        "present count below the milestone never saves");
}

void testNoSaveWhileStillActivelyCompiling() {
  MilestoneState s = baseState();
  // Only a handful of presents since the last new entry -> not quiescent.
  s.lastNewEntryPresent = s.presentCount - (kDefaultQuiescencePresents - 1);
  check(!shouldMilestoneSave(s),
        "compile set still active (not quiescent) never saves");
}

void testNoSaveWhenNoNewEntriesEver() {
  MilestoneState s = baseState();
  s.entriesAddedThisSession = 0;
  s.entriesAddedAtLastSave = 0;
  check(!shouldMilestoneSave(s),
        "no archive entries compiled this session -> nothing to save");
}

void testFirstMilestoneSaveFires() {
  MilestoneState s = baseState();
  check(shouldMilestoneSave(s),
        "milestone reached + quiescent + new entries + first save -> saves");
}

void testSavePoisonedNeverSaves() {
  MilestoneState s = baseState();
  s.savePoisoned = true;
  check(!shouldMilestoneSave(s),
        "R-BACK-3.11: savePoisoned short-circuits to never-save");
}

void testSecondSaveBlockedWithoutLargeBatch() {
  MilestoneState s = baseState();
  s.savesPerformed = 1;
  s.entriesAddedAtLastSave = 10;
  s.entriesAddedThisSession = 10 + (kDefaultReArmBatchEntries - 1);
  check(!shouldMilestoneSave(s),
        "second save is blocked when fewer than the re-arm batch size of "
        "new entries arrived since the first save");
}

void testSecondSaveArmedByLargeBatch() {
  MilestoneState s = baseState();
  s.savesPerformed = 1;
  s.entriesAddedAtLastSave = 10;
  s.entriesAddedThisSession = 10 + kDefaultReArmBatchEntries;
  check(shouldMilestoneSave(s),
        "second save fires once at least the re-arm batch size of new "
        "entries arrived since the first save");
}

void testThirdSaveNeverFires() {
  MilestoneState s = baseState();
  s.savesPerformed = 2;
  s.entriesAddedAtLastSave = 10;
  s.entriesAddedThisSession = 10 + kDefaultReArmBatchEntries * 10;
  check(!shouldMilestoneSave(s),
        "bounded to at most two saves per process — a third never fires "
        "regardless of how much new content arrives");
}

void testCustomThresholdsHonored() {
  MilestoneState s{};
  s.presentCount = 50;
  s.lastNewEntryPresent = 40;
  s.entriesAddedThisSession = 5;
  s.entriesAddedAtLastSave = 0;
  s.savesPerformed = 0;
  s.savePoisoned = false;
  check(shouldMilestoneSave(s, /*presentMilestone=*/50,
                            /*quiescencePresents=*/10,
                            /*reArmBatchEntries=*/1),
        "custom (smaller) thresholds are honored, not just the defaults");
  check(!shouldMilestoneSave(s, /*presentMilestone=*/51,
                             /*quiescencePresents=*/10,
                             /*reArmBatchEntries=*/1),
        "custom presentMilestone above presentCount blocks the save");
}

}  // namespace

int main() {
  try {
    testParseNullIsNullopt();
    testParseEmptyIsNullopt();
    testParseZeroIsNullopt();
    testParseGarbageIsNullopt();
    testParseValidNumber();
    testParseLeadingDigitsStopAtFirstNonDigit();

    testDemoteWhenOverGuard();
    testNoDemoteWhenUnderGuard();
    testNoDemoteWhenExactlyAtGuard();
    testNoDemoteWhenMissingFile();

    testNoSaveBeforeMilestonePresents();
    testNoSaveWhileStillActivelyCompiling();
    testNoSaveWhenNoNewEntriesEver();
    testFirstMilestoneSaveFires();
    testSavePoisonedNeverSaves();
    testSecondSaveBlockedWithoutLargeBatch();
    testSecondSaveArmedByLargeBatch();
    testThirdSaveNeverFires();
    testCustomThresholdsHonored();
  } catch (const TestFailure& e) {
    std::cerr << "FAIL: " << e.what() << "\n";
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "FAIL (unexpected): " << e.what() << "\n";
    return 2;
  }
  std::cout << "ok\n";
  return 0;
}
