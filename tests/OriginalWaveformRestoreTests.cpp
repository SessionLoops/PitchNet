#include "../Source/Audio/Synthesis/OriginalWaveformRestore.h"
#include <cassert>
#include <cmath>
#include <iostream>

int main() {
  std::vector<float> original(8192);
  for (int i = 0; i < static_cast<int>(original.size()); ++i)
    original[i] = 0.7f * std::sin(i * 0.13f);
  auto waveform = original;
  // A remote timing gap used to disable the entire exact-restore pass.
  std::fill(waveform.begin() + 6000, waveform.begin() + 7000, 0.0f);
  const std::vector<OriginalWaveformRestore::Range> remoteGap{{5872, 7128}};
  for (int cycle = 0; cycle < 8; ++cycle) {
    // Simulate successive rendered pitch edits, including their edge fades.
    for (int edit = 0; edit < 5; ++edit)
      for (int i = 1024; i < 4096; ++i)
        waveform[i] += 0.3f * (std::sin(i * (0.21f + edit * 0.02f)) - waveform[i]);
    OriginalWaveformRestore::copy(waveform.data(), original.data(),
                                  1024, 4096, remoteGap);
    for (int i = 1024; i < 4096; ++i)
      assert(waveform[i] == original[i]);
    for (int i = 6000; i < 7000; ++i)
      assert(waveform[i] == 0.0f);
  }

  // Preserve overlapping timing patches and fades, even when unsorted;
  // restore the remaining samples exactly, including a nonzero start offset.
  std::fill(waveform.begin(), waveform.end(), -2.0f);
  OriginalWaveformRestore::copy(waveform.data(), original.data(), 100, 1000,
                                {{700, 1200}, {300, 450}, {400, 500},
                                 {-100, 150}, {600, 600}});
  for (int i = 0; i < static_cast<int>(waveform.size()); ++i) {
    const bool restored = (i >= 150 && i < 300) || (i >= 500 && i < 700);
    assert(waveform[i] == (restored ? original[i] : -2.0f));
  }
  OriginalWaveformRestore::copy(waveform.data(), original.data(), 0,
                                static_cast<int>(waveform.size()), {});
  assert(waveform == original);
  std::cout << "Original waveform restore tests passed\n";
}
