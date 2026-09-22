#pragma once

#include <algorithm>
#include <vector>

namespace OriginalWaveformRestore {
struct Range {
  int start;
  int end;
};

// All positions use the absolute waveform sample grid. Timing patches may
// protect part of a restore without disabling restoration elsewhere.
inline void copy(float* destination, const float* original, int start, int end,
                 const std::vector<Range>& protectedRanges) {
  auto ranges = protectedRanges;
  std::sort(ranges.begin(), ranges.end(),
            [](const Range& a, const Range& b) { return a.start < b.start; });
  int cursor = start;
  for (const auto& range : ranges) {
    if (range.end <= cursor || range.start >= end || range.end <= range.start)
      continue;
    const int copyEnd = std::min(end, range.start);
    if (copyEnd > cursor)
      std::copy(original + cursor, original + copyEnd, destination + cursor);
    cursor = std::min(end, std::max(cursor, range.end));
  }
  if (cursor < end)
    std::copy(original + cursor, original + end, destination + cursor);
}
} // namespace OriginalWaveformRestore
