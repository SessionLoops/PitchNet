// The chord track from an ARA host's sheet chords (AraChordTrack.h).
#include "../Source/Plugin/AraChordTrack.h"
#include "TestAssert.h"
#include <cmath>
#include <iostream>

using namespace pitchnet::ara;

static bool near(double a, double b) { return std::abs(a - b) < 1.0e-9; }

static SheetChordEntry chord(double quarter, int root, int bass,
                             std::initializer_list<int> intervals,
                             std::string name = {}) {
  SheetChordEntry e;
  e.quarterPosition = quarter;
  e.root = root;
  e.bass = bass;
  for (int i : intervals)
    e.intervals[static_cast<std::size_t>(i)] = 0xFF;
  e.name = std::move(name);
  return e;
}

int main() {
  // 120 BPM, then 60 BPM from quarter 16 (8 s).
  const std::vector<TempoMapEntry> tempo{{0.0, 0.0}, {8.0, 16.0}, {16.0, 24.0}};
  CHECK(near(quarterToSeconds(tempo, 4.0), 2.0));
  CHECK(near(quarterToSeconds(tempo, 20.0), 12.0));
  CHECK(near(quarterToSeconds(tempo, -2.0), -1.0));  // the first segment extends back
  CHECK(near(quarterToSeconds(tempo, 30.0), 22.0));  // ... and the last one on
  CHECK(std::isnan(quarterToSeconds({{0.0, 0.0}}, 1.0)));
  CHECK(std::isnan(quarterToSeconds({{0.0, 0.0}, {0.0, 4.0}}, 1.0)));

  // C | Am | F/A (spelled by the host) | gap | Bb7 with the host's own name
  const std::vector<SheetChordEntry> sheet{
      chord(0.0, 0, 0, {0, 4, 7}),
      chord(4.0, 3, 3, {0, 3, 7}),
      chord(8.0, -1, 3, {0, 4, 7}),
      chord(12.0, 0, 0, {}),  // undefined chord: no chord here
      chord(16.0, -2, -2, {0, 4, 7, 10}, "B\xE2\x99\xAD" "7"),
      chord(20.0, -2, -2, {0, 4, 7, 10}, "B\xE2\x99\xAD" "7"),  // repeated
  };
  const auto spans = sheetChordSpans(tempo, sheet);
  for (const auto &s : spans)
    std::cout << s.startSeconds << "-" << s.endSeconds << " " << s.name << "\n";
  CHECK(spans.size() == 4);
  CHECK(spans[0].name == "C" && std::isinf(spans[0].startSeconds) && spans[0].startSeconds < 0.0);
  CHECK(near(spans[0].endSeconds, 2.0));
  CHECK(spans[1].name == "Am" && spans[1].chord.root == 9 && near(spans[1].startSeconds, 2.0));
  CHECK(spans[2].name == "F/A" && spans[2].chord.root == 5 && spans[2].chord.bass == 9);
  CHECK(near(spans[2].endSeconds, 6.0)); // the gap starts at quarter 12
  CHECK(spans[3].name == "Bb7" && spans[3].chord.root == 10);
  CHECK(near(spans[3].startSeconds, 8.0) && std::isinf(spans[3].endSeconds));

  // Sharps and flats as the host spells them, without a name.
  CHECK(sheetChordSpans(tempo, {chord(0.0, 6, 6, {0, 3, 7})})[0].name == "F#m");
  CHECK(sheetChordSpans(tempo, {chord(0.0, -6, -6, {0, 4, 7, 11})})[0].name == "Gbmaj7");
  // Diatonic degrees count as used intervals.
  auto degrees = chord(0.0, 2, 2, {});
  degrees.intervals[0] = 0x01;
  degrees.intervals[3] = 0x03;
  degrees.intervals[7] = 0x05;
  degrees.intervals[10] = 0x07;
  CHECK(sheetChordSpans(tempo, {degrees})[0].name == "Dm7");

  // No usable tempo map: no chords rather than chords in the wrong place.
  CHECK(sheetChordSpans({}, sheet).empty());
  CHECK(sheetChordSpans(tempo, {}).empty());

  std::cout << "AraChordTrackTests passed\n";
  return 0;
}
