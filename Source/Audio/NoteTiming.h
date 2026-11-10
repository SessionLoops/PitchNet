#pragma once

#include "NoteTimeMap.h"
#include "../Models/Project.h"
#include "../Utils/CurveResampler.h"

#include <algorithm>
#include <cmath>
#include <vector>

/**
 * Time maps for notes (NoteTimeMap.h): which recorded frames a note plays
 * where, now that a stretched note holds its vowel and keeps its consonants
 * at their recorded length. Everything that lays a note's audio or curves out
 * over its frames goes through these - the vocoder's mel, PSOLA's read
 * position, the pitch contour, the blend with the recording, and the piano
 * roll - so the picture, the pitch and the sound stay together.
 */
namespace notetiming
{
/** The note as committed: its source frames over [start, end). */
inline TimeMap forNote(const AudioData &audioData, const Note &note)
{
  return TimeMap::build(audioData.voicedMask, audioData.breathSibilance,
                        note.getSrcStartFrame(), note.getSrcEndFrame(),
                        note.getStartFrame(), note.getEndFrame());
}

/** The note as drawn: follows a timing drag in progress. */
inline TimeMap forNoteOnScreen(const AudioData &audioData, const Note &note)
{
  return TimeMap::build(audioData.voicedMask, audioData.breathSibilance,
                        note.getSrcStartFrame(), note.getSrcEndFrame(),
                        note.getVisualStartFrame(), note.getVisualEndFrame());
}

/**
 * The map a per-frame curve stored with the note was written under. A curve
 * is stored at the length the note had when it was written - the recorded
 * length for the analysed contour, the note's length at the time for a drawn
 * one - so its frames sit where that note's audio played then.
 */
inline TimeMap forStoredCurve(const AudioData &audioData, const Note &note,
                              int curveLength)
{
  return TimeMap::build(audioData.voicedMask, audioData.breathSibilance,
                        note.getSrcStartFrame(), note.getSrcEndFrame(), 0.0,
                        static_cast<double>(curveLength));
}

/**
 * A curve stored with the note (a pitch contour), laid out over the note's
 * current frames. Each frame reads the curve where the same recorded frame
 * was when the curve was written, so a contour stays on its syllable - the
 * vibrato on the held vowel, the scoop on the consonant it follows - however
 * often the note is stretched. Unchanged when the note still has the length
 * the curve was written at.
 */
inline std::vector<float> curveOnNote(const AudioData &audioData,
                                      const Note &note,
                                      const std::vector<float> &stored)
{
  const int frames = note.getDurationFrames();
  if (frames <= 0 || stored.empty())
    return {};
  if (static_cast<int>(stored.size()) == frames)
    return stored;
  if (note.getSrcDurationFrames() <= 0)
    return CurveResampler::resampleLinear(stored, frames);

  const auto now = forNote(audioData, note);
  const auto then =
      forStoredCurve(audioData, note, static_cast<int>(stored.size()));
  const int last = static_cast<int>(stored.size()) - 1;
  std::vector<float> curve(static_cast<size_t>(frames));
  for (int i = 0; i < frames; ++i)
  {
    const double source =
        now.sourceAt(static_cast<double>(note.getStartFrame() + i) + 0.5);
    const double index =
        std::clamp(then.destinationAt(source) - 0.5, 0.0,
                   static_cast<double>(last));
    const int left = static_cast<int>(std::floor(index));
    const int right = std::min(left + 1, last);
    const float amount = static_cast<float>(index - left);
    curve[static_cast<size_t>(i)] =
        stored[static_cast<size_t>(left)] * (1.0f - amount) +
        stored[static_cast<size_t>(right)] * amount;
  }
  return curve;
}

/**
 * Where a curve stored with the note divides when the note's audio is cut at
 * source frame `sourceFrame` (a split, or a region boundary): the index of the
 * curve's first value on the far side.
 */
inline int curveIndexAtSource(const AudioData &audioData, const Note &note,
                              int curveLength, int sourceFrame)
{
  if (curveLength <= 0)
    return 0;
  const auto map = forStoredCurve(audioData, note, curveLength);
  return std::clamp(
      static_cast<int>(std::lround(map.destinationAt(sourceFrame))), 0,
      curveLength);
}
} // namespace notetiming
