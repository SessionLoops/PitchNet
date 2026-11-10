#pragma once

#include "BreathSibilanceDetector.h"

#include <algorithm>
#include <cstdint>
#include <vector>

/**
 * Note edges around the sung vowels: the refinement after GAME's note
 * segmentation.
 *
 * GAME starts a note at its vowel's pitch onset, so the consonant leading
 * into a syllable lands at the end of the previous note (94% of the
 * consonants between two sung vowels on vocadito). Each pair of
 * neighbouring notes is re-split in the unvoiced gap between their vowels:
 *
 *  - onset: the consonant frames running back from a vowel join that
 *    vowel's note - an "s" belongs to the syllable it starts;
 *  - coda: consonant frames still left right after a vowel stay with that
 *    vowel's note. That happens when a pause or a breath separates them from
 *    the next vowel: a word-final "s" before an inhale;
 *  - breaths never join a note, and are cut off where GAME put one in it.
 *
 * A consonant frame is unvoiced, not a breath, and has audio energy (the VAD
 * mask) or is labelled sibilance, so a quiet "s" counts. Silent closures of
 * up to closureFrames inside a cluster ("st", "ks") are bridged. Voiced
 * frames never move between notes: where the voice runs straight from one
 * note into the next (legato), GAME's boundary stays. Notes only grow over
 * consonants and shrink where a neighbour's onset or coda takes frames or a
 * breath is cut off. JUCE-free.
 */
namespace noteedges
{
struct Span
{
  int start = 0;
  int end = 0;
};

struct Config
{
  int maxOnsetFrames = 26; // ~300 ms of consonants before a vowel
  int maxCodaFrames = 22;  // ~255 ms after the last vowel of a phrase
  int closureFrames = 4;   // ~46 ms of silence bridged inside a cluster
  int dropoutFrames = 2;   // voicing gaps this short stay inside a vowel
  int minNoteFrames = 3;   // a breath cut never leaves a note shorter
};

/**
 * @param notes      note spans [start, end) in frames, sorted by start
 * @param voiced     the pitch detector's per-frame voicing (voicedMask)
 * @param vad        per-frame audio energy (AudioData::vadMask)
 * @param labels     breathsibilance labels per frame; may be empty
 * @param numFrames  frame count; spans are kept inside [0, numFrames)
 * @return           the refined spans, in the same order
 */
inline std::vector<Span> attachConsonants(std::vector<Span> notes,
                                          const std::vector<bool> &voiced,
                                          const std::vector<bool> &vad,
                                          const std::vector<std::uint8_t> &labels,
                                          int numFrames,
                                          const Config &config = {})
{
  const int count = static_cast<int>(notes.size());
  if (count == 0 || numFrames <= 0)
    return notes;

  const auto maskAt = [](const std::vector<bool> &mask, int frame)
  {
    return frame >= 0 && frame < static_cast<int>(mask.size()) &&
           mask[static_cast<size_t>(frame)];
  };
  const auto labelOf = [&labels](int frame)
  { return breathsibilance::labelAt(labels, frame); };
  const auto inside = [numFrames](int frame)
  { return frame >= 0 && frame < numFrames; };

  // Vowel frames: voiced, with short dropouts filled when nothing in them is
  // labelled breath or sibilance.
  std::vector<std::uint8_t> vowel(static_cast<size_t>(numFrames), 0);
  for (int frame = 0, last = -1; frame < numFrames; ++frame)
  {
    if (!maskAt(voiced, frame))
      continue;
    vowel[static_cast<size_t>(frame)] = 1;
    const int gap = frame - last - 1;
    if (last >= 0 && gap > 0 && gap <= config.dropoutFrames)
    {
      bool noise = false;
      for (int g = last + 1; g < frame; ++g)
        noise = noise || breathsibilance::isNoise(labelOf(g));
      if (!noise)
        std::fill(vowel.begin() + last + 1, vowel.begin() + frame, 1);
    }
    last = frame;
  }
  const auto vowelAt = [&](int frame)
  { return inside(frame) && vowel[static_cast<size_t>(frame)] != 0; };
  const auto breathAt = [&](int frame)
  { return labelOf(frame) == breathsibilance::Breath; };
  const auto consonantAt = [&](int frame)
  {
    return inside(frame) && !vowelAt(frame) && !breathAt(frame) &&
           (maskAt(vad, frame) ||
            labelOf(frame) == breathsibilance::Sibilance);
  };
  const auto closureAt = [&](int frame)
  {
    return inside(frame) && !vowelAt(frame) && !breathAt(frame) &&
           !consonantAt(frame);
  };

  // First frame of the consonant cluster ending at `from` (exclusive),
  // searching back no further than `floor`; `from` when there is none.
  const auto onsetStart = [&](int from, int floor)
  {
    floor = std::max({floor, from - config.maxOnsetFrames, 0});
    int start = from;
    int frame = from - 1;
    while (frame >= floor)
    {
      if (consonantAt(frame))
      {
        start = frame--;
        continue;
      }
      int behind = frame;
      int closure = 0;
      while (behind >= floor && closure < config.closureFrames &&
             closureAt(behind))
      {
        --behind;
        ++closure;
      }
      if (closure > 0 && behind >= floor && consonantAt(behind))
      {
        frame = behind;
        continue;
      }
      break;
    }
    return start;
  };

  // One past the consonant cluster starting at `from`, stopping before
  // `ceiling`; `from` when there is none.
  const auto codaEnd = [&](int from, int ceiling)
  {
    ceiling = std::min({ceiling, from + config.maxCodaFrames, numFrames});
    int end = from;
    int frame = from;
    while (frame < ceiling)
    {
      if (consonantAt(frame))
      {
        end = ++frame;
        continue;
      }
      int ahead = frame;
      int closure = 0;
      while (ahead < ceiling && closure < config.closureFrames &&
             closureAt(ahead))
      {
        ++ahead;
        ++closure;
      }
      if (closure > 0 && ahead < ceiling && consonantAt(ahead))
      {
        frame = ahead;
        continue;
      }
      break;
    }
    return end;
  };

  // Each note's vowel: the vowel run holding its first vowel frame, through
  // the run holding its last, followed past the note's own edges.
  struct Vowel
  {
    bool present = false;
    int start = 0;
    int end = 0;
  };
  std::vector<Vowel> vowels(static_cast<size_t>(count));
  for (int k = 0; k < count; ++k)
  {
    auto &note = notes[static_cast<size_t>(k)];
    note.start = std::clamp(note.start, 0, numFrames);
    note.end = std::clamp(note.end, note.start, numFrames);
    int first = -1;
    int last = -1;
    for (int frame = note.start; frame < note.end; ++frame)
      if (vowelAt(frame))
      {
        if (first < 0)
          first = frame;
        last = frame;
      }
    if (first < 0)
      continue;
    auto &v = vowels[static_cast<size_t>(k)];
    v.present = true;
    v.start = first;
    while (vowelAt(v.start - 1))
      --v.start;
    v.end = last + 1;
    while (vowelAt(v.end))
      ++v.end;
  }
  // Voice running straight from one note's vowel into the next's.
  const auto legato = [&](int k)
  {
    const auto &a = vowels[static_cast<size_t>(k)];
    const auto &b = vowels[static_cast<size_t>(k + 1)];
    return a.present && b.present && a.end >= b.start;
  };

  std::vector<Span> out(notes);

  // 1) Onsets first: a consonant cluster before a vowel belongs to it, even
  //    where GAME put it in the previous note. The search stops at the
  //    previous vowel, so no note loses voiced frames.
  for (int k = 0; k < count; ++k)
  {
    const auto &v = vowels[static_cast<size_t>(k)];
    if (!v.present || (k > 0 && legato(k - 1)))
      continue;
    int floor = 0;
    if (k > 0)
    {
      const auto &previous = vowels[static_cast<size_t>(k - 1)];
      floor = previous.present ? previous.end
                               : notes[static_cast<size_t>(k - 1)].end;
    }
    out[static_cast<size_t>(k)].start =
        std::min(notes[static_cast<size_t>(k)].start,
                 onsetStart(v.start, floor));
  }

  // 2) Codas: consonants right after a vowel that no onset took, up to the
  //    next note's new start.
  for (int k = 0; k < count; ++k)
  {
    const int ceiling =
        k + 1 < count ? out[static_cast<size_t>(k + 1)].start : numFrames;
    auto &span = out[static_cast<size_t>(k)];
    span.end = std::min(notes[static_cast<size_t>(k)].end, ceiling);
    const auto &v = vowels[static_cast<size_t>(k)];
    if (v.present && !(k + 1 < count && legato(k)) && v.end <= ceiling)
      span.end = std::max(span.end, codaEnd(v.end, ceiling));
    if (k + 1 < count)
      out[static_cast<size_t>(k + 1)].start =
          std::max(out[static_cast<size_t>(k + 1)].start, span.end);
  }

  // 3) Breaths stay outside notes, including any GAME placed in a note's
  //    unvoiced head or tail. Cut at the breath nearest the vowel: a
  //    consonant between that breath and the voice stays in the note.
  for (auto &span : out)
  {
    int first = -1;
    int last = -1;
    for (int frame = span.start; frame < span.end; ++frame)
      if (maskAt(voiced, frame))
      {
        if (first < 0)
          first = frame;
        last = frame;
      }
    if (first < 0)
      continue;
    int start = span.start;
    for (int frame = first - 1; frame >= span.start; --frame)
      if (breathAt(frame))
      {
        start = frame + 1;
        break;
      }
    int end = span.end;
    for (int frame = last + 1; frame < span.end; ++frame)
      if (breathAt(frame))
      {
        end = frame;
        break;
      }
    if (end - start >= config.minNoteFrames)
    {
      span.start = start;
      span.end = end;
    }
  }
  return out;
}
} // namespace noteedges
