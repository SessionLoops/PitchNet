#pragma once

// Chords from an ARA musical context's kARAContentTypeSheetChords, as spans in
// host timeline seconds for the chord track.
//
// Kept free of ARA and JUCE types so it can be unit tested, like AraTempoMap.h:
// the document controller copies the host's ARAContentChord events into
// SheetChordEntry values.

#include "AraTempoMap.h"
#include "../Audio/Polyphonic/PolyChords.h"

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace pitchnet::ara {

struct SheetChordEntry {
  double quarterPosition = 0.0;      // ARAContentChord::position
  int root = 0;                      // ARAContentChord::root (circle of fifths, C = 0)
  int bass = 0;                      // ARAContentChord::bass
  std::array<std::uint8_t, 12> intervals{}; // ARAContentChord::intervals (0: unused)
  std::string name;                  // ARAContentChord::name, or empty
};

// Seconds at a quarter position. ARA maps time and quarters linearly between
// adjacent tempo entries; the first and last segments extend the map.
// NaN when the map is missing or degenerate.
inline double quarterToSeconds(const std::vector<TempoMapEntry> &tempoEntries,
                               double quarter) {
  if (tempoEntries.size() < 2 || !std::isfinite(quarter))
    return std::numeric_limits<double>::quiet_NaN();
  std::size_t left = 0;
  while (left + 2 < tempoEntries.size() &&
         quarter >= tempoEntries[left + 1].quarterPosition)
    ++left;
  const auto &a = tempoEntries[left];
  const auto &b = tempoEntries[left + 1];
  const double dq = b.quarterPosition - a.quarterPosition;
  const double dt = b.timeSeconds - a.timeSeconds;
  if (!std::isfinite(dq) || !std::isfinite(dt) || dq <= 0.0 || dt <= 0.0)
    return std::numeric_limits<double>::quiet_NaN();
  return a.timeSeconds + (quarter - a.quarterPosition) * dt / dq;
}

// The host's sheet symbols use U+266D / U+266F for flat and sharp; the chord
// track writes them as b and #, like the chords PitchNet detects.
inline std::string asciiChordName(std::string name) {
  const std::string flat = "\xE2\x99\xAD", sharp = "\xE2\x99\xAF";
  for (auto pos = name.find(flat); pos != std::string::npos; pos = name.find(flat, pos))
    name.replace(pos, flat.size(), "b");
  for (auto pos = name.find(sharp); pos != std::string::npos; pos = name.find(sharp, pos))
    name.replace(pos, sharp.size(), "#");
  return name;
}

// The chord name PitchNet shows: the host's own when it gives one, otherwise
// built from the root and bass as the host spells them and the intervals.
inline std::string sheetChordName(const SheetChordEntry &entry,
                                  std::uint16_t intervals) {
  if (!entry.name.empty())
    return asciiChordName(entry.name);
  std::string name = polyphonic::circleOfFifthsName(entry.root) +
                     polyphonic::chordSuffix(intervals);
  if (polyphonic::circleOfFifthsPitchClass(entry.bass) !=
      polyphonic::circleOfFifthsPitchClass(entry.root))
    name += "/" + polyphonic::circleOfFifthsName(entry.bass);
  return name;
}

// The chord track: each chord lasts until the next; the first one also holds
// before it and the last one after it (their open ends are -/+ infinity). An
// undefined chord (all intervals unused) is a gap. Entries must be in
// position order, as ARA delivers them. Empty without a usable tempo map.
inline std::vector<polyphonic::ChordSpan>
sheetChordSpans(const std::vector<TempoMapEntry> &tempoEntries,
                const std::vector<SheetChordEntry> &chords) {
  std::vector<polyphonic::ChordSpan> spans;
  constexpr double inf = std::numeric_limits<double>::infinity();
  for (std::size_t i = 0; i < chords.size(); ++i) {
    const auto &entry = chords[i];
    std::uint16_t intervals = 0;
    for (int k = 0; k < 12; ++k)
      if (entry.intervals[static_cast<std::size_t>(k)] != 0)
        intervals = static_cast<std::uint16_t>(intervals | (1u << k));
    if (intervals == 0)
      continue; // a gap in the progression

    const double start = i == 0 ? -inf : quarterToSeconds(tempoEntries, entry.quarterPosition);
    const double end = i + 1 < chords.size()
                           ? quarterToSeconds(tempoEntries, chords[i + 1].quarterPosition)
                           : inf;
    if (std::isnan(start) || std::isnan(end))
      return {};
    if (!(end > start))
      continue;

    polyphonic::ChordSpan span;
    span.startSeconds = start;
    span.endSeconds = end;
    span.chord.root = polyphonic::circleOfFifthsPitchClass(entry.root);
    span.chord.bass = polyphonic::circleOfFifthsPitchClass(entry.bass);
    span.chord.intervals = static_cast<std::uint16_t>(intervals | 1u);
    span.name = sheetChordName(entry, span.chord.intervals);

    // A chord repeated in the host's track is one span.
    if (!spans.empty() && spans.back().chord == span.chord &&
        spans.back().name == span.name && spans.back().endSeconds == span.startSeconds)
      spans.back().endSeconds = span.endSeconds;
    else
      spans.push_back(std::move(span));
  }
  return spans;
}

} // namespace pitchnet::ara
