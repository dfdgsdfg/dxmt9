// Task A9 / R-BACK-39.3 — divergence logging seam unit tests.
//
// Tests the PURE pieces only: the DXMT9_RENDERER_LOG_DIVERGENCE env resolver,
// the VectorDecisionRecorder recording, and the compareDecisions helper. NO
// Metal, NO encodeChunk — the full dry-run reproduce-and-compare engine is
// deferred to L1 (see decision_recorder.hpp scope note).

#include "../../../src/dxmt9/render/decision_recorder.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};
void check(bool c, std::string_view m) {
  if (!c) throw TestFailure(std::string(m));
}

void testResolveLogDivergence() {
  using namespace dxmt9::render;
  check(resolveLogDivergence(nullptr) == false, "null → off");
  check(resolveLogDivergence("") == false, "empty → off");
  check(resolveLogDivergence("0") == false, "\"0\" → off");
  check(resolveLogDivergence("1") == true, "\"1\" → on");
  check(resolveLogDivergence("yes") == true, "\"yes\" → on");
  check(resolveLogDivergence("anything") == true, "non-\"0\" → on");
  // "00" is non-empty and not the literal "0" → set, per envFlagSet semantics.
  check(resolveLogDivergence("00") == true, "\"00\" → on (only \"0\" is off)");
}

void testVectorDecisionRecorderRecords() {
  using namespace dxmt9::render;
  VectorDecisionRecorder rec;
  check(rec.size() == 0, "starts empty");

  rec.recordPassBegin(0x10, 0x20, 1);
  rec.recordDraw(0xABC, 36, 0x1);
  rec.recordLoadStore(0x10, 2, 3);
  rec.recordEncoderSplit(4);
  check(rec.size() == 4, "recorded 4 decisions");

  const auto& r = rec.records();
  check(r[0].kind == DecisionKind::PassBegin, "0 is PassBegin");
  check(r[0].a == 0x10 && r[0].b == 0x20 && r[0].c == 1, "PassBegin fields");
  check(r[1].kind == DecisionKind::Draw, "1 is Draw");
  check(r[1].a == 0xABC && r[1].b == 36 && r[1].c == 0x1, "Draw fields");
  check(r[2].kind == DecisionKind::LoadStore, "2 is LoadStore");
  check(r[2].a == 0x10 && r[2].b == 2 && r[2].c == 3, "LoadStore fields");
  check(r[3].kind == DecisionKind::EncoderSplit, "3 is EncoderSplit");
  check(r[3].a == 4 && r[3].b == 0 && r[3].c == 0, "EncoderSplit fields");

  rec.clear();
  check(rec.size() == 0, "clear empties");
}

void testCompareDecisionsEqual() {
  using namespace dxmt9::render;
  VectorDecisionRecorder a;
  VectorDecisionRecorder b;
  for (auto* r : {&a, &b}) {
    r->recordPassBegin(1, 2, 1);
    r->recordDraw(7, 12, 0);
    r->recordEncoderSplit(0);
  }
  auto d = compareDecisions(a.records(), b.records());
  check(d.diverged == false, "identical sequences do not diverge");
  check(d.index == 3, "non-diverged index == shared length");
}

void testCompareDecisionsDifferingRecord() {
  using namespace dxmt9::render;
  VectorDecisionRecorder a;
  VectorDecisionRecorder b;
  a.recordPassBegin(1, 2, 1);
  b.recordPassBegin(1, 2, 1);
  a.recordDraw(7, 12, 0);
  b.recordDraw(7, /*differs*/ 99, 0);  // diverge at index 1
  a.recordEncoderSplit(0);
  b.recordEncoderSplit(0);
  auto d = compareDecisions(a.records(), b.records());
  check(d.diverged == true, "differing record diverges");
  check(d.index == 1, "divergence index is first differing position");
}

void testCompareDecisionsDifferingKind() {
  using namespace dxmt9::render;
  VectorDecisionRecorder a;
  VectorDecisionRecorder b;
  a.recordPassBegin(1, 2, 1);
  b.recordPassBegin(1, 2, 1);
  a.recordDraw(7, 12, 0);
  b.recordLoadStore(7, 12, 0);  // same lanes, different kind → diverge at 1
  auto d = compareDecisions(a.records(), b.records());
  check(d.diverged == true, "differing kind diverges");
  check(d.index == 1, "divergence index 1 on kind mismatch");
}

void testCompareDecisionsLengthMismatch() {
  using namespace dxmt9::render;
  VectorDecisionRecorder shortRec;
  VectorDecisionRecorder longRec;
  shortRec.recordPassBegin(1, 2, 1);
  longRec.recordPassBegin(1, 2, 1);
  longRec.recordDraw(7, 12, 0);  // extra record beyond shared prefix
  auto d = compareDecisions(shortRec.records(), longRec.records());
  check(d.diverged == true, "length mismatch diverges");
  check(d.index == 1, "divergence at first missing record (shared length)");

  // Symmetry: modern longer than reference.
  auto d2 = compareDecisions(longRec.records(), shortRec.records());
  check(d2.diverged == true, "length mismatch diverges (swapped)");
  check(d2.index == 1, "divergence at shared length (swapped)");
}

void testCompareDecisionsEmpty() {
  using namespace dxmt9::render;
  std::vector<dxmt9::render::DecisionRecord> empty;
  auto d = compareDecisions(empty, empty);
  check(d.diverged == false, "two empty sequences do not diverge");
  check(d.index == 0, "empty divergence index 0");
}

}  // namespace

int main() {
  try {
    testResolveLogDivergence();
    testVectorDecisionRecorderRecords();
    testCompareDecisionsEqual();
    testCompareDecisionsDifferingRecord();
    testCompareDecisionsDifferingKind();
    testCompareDecisionsLengthMismatch();
    testCompareDecisionsEmpty();
  } catch (const std::exception& e) {
    std::cerr << "divergence_spec failed: " << e.what() << '\n';
    return 1;
  }
  std::cout << "divergence_spec passed\n";
  return 0;
}
