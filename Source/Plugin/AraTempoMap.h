#pragma once

// Tempo / bar-signature lookup over an ARA musical context's content.
//
// Kept free of ARA and JUCE types so the arithmetic can be unit tested. The
// document controller copies the host's kARAContentTypeTempoEntries and
// kARAContentTypeBarSignatures readers into these vectors.

#include <cmath>
#include <optional>
#include <vector>

namespace pitchnet::ara {

struct TempoMapEntry {
  double timeSeconds = 0.0;   // ARAContentTempoEntry::timePosition
  double quarterPosition = 0.0; // ARAContentTempoEntry::quarterPosition
};

struct BarSignatureEntry {
  int numerator = 4;
  int denominator = 4;
  double quarterPosition = 0.0; // ARAContentBarSignature::position
};

struct MusicalTimeline {
  double bpm = 120.0;
  int numerator = 4;
  int denominator = 4;
  bool hasBarSignature = false;
};

// Returns the tempo (and bar signature, if the host provided one) in effect at
// timeSeconds. ARA requires at least two tempo entries; tempo is linear between
// adjacent entries and the first/last segment extends before/after the map.
// Returns nullopt when the map is missing or degenerate.
inline std::optional<MusicalTimeline>
musicalTimelineAt(const std::vector<TempoMapEntry> &tempoEntries,
                  const std::vector<BarSignatureEntry> &barSignatures,
                  double timeSeconds) {
  if (tempoEntries.size() < 2 || !std::isfinite(timeSeconds))
    return std::nullopt;

  // Pick the segment containing timeSeconds, clamped to the first/last one.
  std::size_t left = 0;
  while (left + 2 < tempoEntries.size() &&
         timeSeconds >= tempoEntries[left + 1].timeSeconds)
    ++left;

  const auto &a = tempoEntries[left];
  const auto &b = tempoEntries[left + 1];
  const double dt = b.timeSeconds - a.timeSeconds;
  const double dq = b.quarterPosition - a.quarterPosition;
  if (!std::isfinite(dt) || !std::isfinite(dq) || dt <= 0.0 || dq <= 0.0)
    return std::nullopt;

  MusicalTimeline result;
  result.bpm = 60.0 * dq / dt;
  if (!std::isfinite(result.bpm) || result.bpm <= 0.0)
    return std::nullopt;

  // Each bar signature is valid until the next; the first one also applies to
  // everything before it.
  const double quarter = a.quarterPosition + (timeSeconds - a.timeSeconds) * dq / dt;
  const BarSignatureEntry *signature = nullptr;
  for (const auto &entry : barSignatures) {
    if (entry.numerator <= 0 || entry.denominator <= 0)
      continue;
    if (signature == nullptr || entry.quarterPosition <= quarter)
      signature = &entry;
    if (entry.quarterPosition > quarter)
      break;
  }

  if (signature != nullptr) {
    result.numerator = signature->numerator;
    result.denominator = signature->denominator;
    result.hasBarSignature = true;
  }

  return result;
}

} // namespace pitchnet::ara
