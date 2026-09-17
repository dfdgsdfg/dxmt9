#include "dxmt9/query_end_marker.hpp"
#include <iostream>
#include <stdexcept>

namespace {
void check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
}
int main() {
  using namespace dxmt9::query;
  try {
    constexpr WriterIdentity admitted{7, 3, 0x100000001ull, 0x200000001ull};
    for (unsigned field = 0; field != 5; ++field) {
      auto candidate = admitted;
      if (field == 1) ++candidate.queue;
      if (field == 2) ++candidate.index;
      if (field == 3) candidate.generation = 1;
      if (field == 4) candidate.storageGeneration = 1;
      for (unsigned bound = 0; bound != 3; ++bound)
        for (unsigned seq = 0; seq != 3; ++seq)
          check(canBindEnd(admitted, candidate, bound, seq) ==
                    (field == 0 && !bound && seq != 0),
                "model Publish owner and exactly-once truth table");
    }
    auto marker = std::make_shared<EndMarker>(admitted);
    EndReceipt receipt{7, 0, 1};
    receipt.marker = marker;
    for (unsigned published = 0; published != 3; ++published)
      for (unsigned done = 0; done <= published; ++done)
        for (unsigned initDone = 0; initDone != 2; ++initDone)
          for (bool failed : {false, true}) {
            Progress progress{7, published, done, failed, 1, initDone};
            check(classifyEnd(receipt, progress) == (failed ? Poll::Failed : Poll::Pending),
                  "unbound marker is never a completed empty prefix");
          }
    check(marker->bind(admitted, 2), "bind actual publication");
    check(!marker->bind(admitted, 3), "no later rebind");
    for (unsigned done = 0; done != 4; ++done)
      for (unsigned initDone = 0; initDone != 2; ++initDone)
        check(classifyEnd(receipt, {7, 3, done, false, 1, initDone}) ==
                  (done >= 2 && initDone ? Poll::Complete : Poll::Pending),
              "bound receipt joins initializer and ordinary actual completion");
    check(classifyEnd(receipt, {8, 3, 3, false, 1, 1}) == Poll::Invalid,
          "queue incarnation is part of the authority");
    check(classifyEnd(receipt, {7, 3, 3, true, 1, 1}) == Poll::Failed,
          "terminal failure dominates high completion waterlines");
    marker.reset();
    check(receipt.snapshot().sequence == 2, "receipt owns marker beyond writer lifetime");
    std::cout << "query_end_marker_spec passed\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
