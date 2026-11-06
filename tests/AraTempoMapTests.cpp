// Regression tests for reading host tempo from an ARA musical context (#30:
// Pro Tools showed 120 BPM because only the playhead tempo was consulted).
#include "../Source/Plugin/AraTempoMap.h"
#include "TestAssert.h"
#include <cmath>
#include <iostream>

using namespace pitchnet::ara;

static bool near(double a, double b) { return std::abs(a - b) < 1.0e-9; }

int main() {
  // Constant 95 BPM: 95 quarters per 60 s.
  std::vector<TempoMapEntry> constant{{0.0, 0.0}, {60.0, 95.0}};
  auto t = musicalTimelineAt(constant, {}, 12.5);
  CHECK(t && near(t->bpm, 95.0));
  CHECK(!t->hasBarSignature);

  // Before the first entry and after the last one extend the edge segments.
  CHECK(near(musicalTimelineAt(constant, {}, -3.0)->bpm, 95.0));
  CHECK(near(musicalTimelineAt(constant, {}, 500.0)->bpm, 95.0));

  // Tempo change at 10 s: 120 BPM (20 quarters) then 60 BPM.
  std::vector<TempoMapEntry> change{{0.0, 0.0}, {10.0, 20.0}, {20.0, 30.0}};
  CHECK(near(musicalTimelineAt(change, {}, 5.0)->bpm, 120.0));
  CHECK(near(musicalTimelineAt(change, {}, 10.0)->bpm, 60.0));
  CHECK(near(musicalTimelineAt(change, {}, 30.0)->bpm, 60.0));

  // Bar signatures: 3/4 from the start, 6/8 from quarter 24 (12 s).
  std::vector<BarSignatureEntry> sigs{{3, 4, 0.0}, {6, 8, 24.0}};
  auto early = musicalTimelineAt(change, sigs, 1.0);
  CHECK(early->hasBarSignature && early->numerator == 3 && early->denominator == 4);
  auto late = musicalTimelineAt(change, sigs, 15.0);
  CHECK(late->numerator == 6 && late->denominator == 8);

  // The first signature also applies before it is defined.
  std::vector<BarSignatureEntry> laterFirst{{5, 4, 8.0}};
  CHECK(musicalTimelineAt(change, laterFirst, 0.0)->numerator == 5);

  // Degenerate maps are rejected rather than producing a bogus tempo.
  CHECK(!musicalTimelineAt({{0.0, 0.0}}, {}, 0.0));
  CHECK(!musicalTimelineAt({{0.0, 0.0}, {0.0, 4.0}}, {}, 0.0));
  CHECK(!musicalTimelineAt({{0.0, 0.0}, {1.0, 0.0}}, {}, 0.0));

  std::cout << "ARA tempo map regressions passed\n";
}
