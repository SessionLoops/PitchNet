#pragma once

#include "BreathNotes.h"
#include "BreathSibilanceAnalysis.h"
#include "NoteTiming.h"
#include "../Models/Project.h"
#include "../Utils/Constants.h"
#include "../Utils/NoteGainCurve.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <utility>
#include <vector>

/**
 * Every gain-only edit in a project, as one set of gain regions: each note's
 * volume, its sibilant balance, and the level of the breath notes. The same
 * regions are applied when an edit is made (straight onto the rendered
 * audio, see GainEditAction) and whenever audio is rendered again, so the two
 * always agree.
 */
namespace projectgain
{
/**
 * A sibilant balance as gains in dB: {hatched frames, the rest of the note}.
 * Like Melodyne's Sibilant Balance it only ever cuts: below 0 the hatched
 * frames get quieter, above 0 the rest of the note does, down to -60 dB at
 * +-100% - the floor the amplitude handle uses, so the edit can always be
 * undone on the rendered audio.
 */
inline std::pair<float, float> balanceGainsDb(float balance)
{
  const auto toDb = [](float amplitude)
  {
    return amplitude <= 0.001f ? -60.0f : 20.0f * std::log10(amplitude);
  };
  if (balance < 0.0f)
    return {toDb(1.0f + balance), 0.0f};
  if (balance > 0.0f)
    return {0.0f, toDb(1.0f - balance)};
  return {0.0f, 0.0f};
}

/**
 * Absolute sample ranges of a note's hatched frames: where it plays audio
 * labelled sibilance or breath, through its time map. Frame k covers
 * k * hop +- hop / 2 as analysed; ranges are kept inside the note.
 */
inline std::vector<std::pair<int, int>> hatchedSamples(const AudioData &audioData,
                                                       const Note &note)
{
  std::vector<std::pair<int, int>> ranges;
  if (audioData.breathSibilance.empty() || note.getDurationFrames() <= 0 ||
      note.getSrcDurationFrames() <= 0)
    return ranges;
  const auto map = notetiming::forNote(audioData, note);
  const int hop = HOP_SIZE;
  const int noteFirst = note.getStartFrame() * hop;
  const int noteLast = note.getEndFrame() * hop;
  int frame = note.getStartFrame();
  while (frame < note.getEndFrame())
  {
    if (!breathsibilance::isNoise(
            breathsibilance::labelAtNoteFrame(audioData, map, frame)))
    {
      ++frame;
      continue;
    }
    const int start = frame;
    while (frame < note.getEndFrame() &&
           breathsibilance::isNoise(
               breathsibilance::labelAtNoteFrame(audioData, map, frame)))
      ++frame;
    const int first = std::max(noteFirst, start * hop - hop / 2);
    const int last = std::min(noteLast, frame * hop - hop / 2);
    if (last > first)
      ranges.emplace_back(first, last);
  }
  return ranges;
}

/** Whether a note has hatched frames for the sibilant balance to act on. */
inline bool hasHatchedFrames(const AudioData &audioData, const Note &note)
{
  return !hatchedSamples(audioData, note).empty();
}

/** The gain regions of one note: its volume and its sibilant balance. */
inline void appendNoteRegions(const AudioData &audioData, const Note &note,
                              std::vector<NoteGainCurve::Region> &regions)
{
  const int hop = HOP_SIZE;
  regions.push_back({note.getStartFrame() * hop, note.getEndFrame() * hop,
                     note.getVolumeDb()});
  if (note.getSibilantBalance() == 0.0f)
    return;
  const auto [hatchedDb, restDb] = balanceGainsDb(note.getSibilantBalance());
  // Regions multiply: the rest of the note's gain over the whole note, then
  // the difference over its hatched frames.
  if (restDb != 0.0f)
    regions.push_back(
        {note.getStartFrame() * hop, note.getEndFrame() * hop, restDb});
  for (const auto &[first, last] : hatchedSamples(audioData, note))
    regions.push_back({first, last, hatchedDb - restDb});
}

/**
 * All gain regions of the project, in absolute samples. `replaced` stands in
 * other versions of some of its notes - the same notes at other gains - and
 * `breathLevel` another level for one breath note, as for previewing an edit
 * in progress.
 */
/** `note`, or the version of it in `replaced`. */
inline const Note &
asReplaced(const Note &note,
           const std::vector<std::pair<const Note *, Note>> &replaced)
{
  for (const auto &[original, replacement] : replaced)
    if (original == &note)
      return replacement;
  return note;
}

/**
 * Stray sibilants: noise outside the notes, without a breath, touching a
 * note (breathnotes::place). Not drawn; they belong to the note they touch,
 * which they play next to, and take its volume and its sibilant balance's
 * cut for hatched frames.
 */
inline void appendStrayRegions(
    const Project &project,
    const std::vector<std::pair<const Note *, Note>> &replaced,
    std::vector<NoteGainCurve::Region> &regions)
{
  // Nothing to do while no note has a gain of its own.
  bool any = false;
  for (const auto &note : project.getNotes())
  {
    const Note &use = asReplaced(note, replaced);
    any = any || use.getVolumeDb() != 0.0f || use.getSibilantBalance() != 0.0f;
  }
  if (!any)
    return;
  const int hop = HOP_SIZE;
  for (const auto &placement :
       breathnotes::place(project, breathnotes::Layout::committed))
  {
    if (!placement.drawn || placement.breath || placement.anchor == nullptr)
      continue;
    const Note &note = asReplaced(*placement.anchor, replaced);
    const float db =
        note.getVolumeDb() + balanceGainsDb(note.getSibilantBalance()).first;
    if (db != 0.0f)
      regions.push_back({static_cast<int>(std::lround(placement.left * hop)),
                         static_cast<int>(std::lround(placement.right * hop)),
                         db});
  }
}

/**
 * Gain regions outside the notes, where the recording plays: breath levels,
 * and stray sibilants at the gain of their note.
 */
inline std::vector<NoteGainCurve::Region> outsideNoteRegions(
    const Project &project,
    const std::vector<std::pair<const Note *, Note>> &replaced = {},
    const breathnotes::LevelOverride &breathLevel = {})
{
  auto result = breathnotes::gainRegions(project, breathLevel);
  appendStrayRegions(project, replaced, result);
  return result;
}

inline std::vector<NoteGainCurve::Region> regions(
    const Project &project,
    const std::vector<std::pair<const Note *, Note>> &replaced = {},
    const breathnotes::LevelOverride &breathLevel = {})
{
  std::vector<NoteGainCurve::Region> result;
  const auto &audioData = project.getAudioData();
  for (const auto &note : project.getNotes())
    if (!note.isRest())
      appendNoteRegions(audioData, asReplaced(note, replaced), result);
  for (const auto &region :
       outsideNoteRegions(project, replaced, breathLevel))
    result.push_back(region);
  return result;
}

/**
 * The samples a gain change to `notes` reaches: the notes and the stray
 * sibilants that belong to them, {first, last}.
 */
inline std::pair<int, int> noteGainSpan(const Project &project,
                                        const std::vector<const Note *> &notes)
{
  const int hop = HOP_SIZE;
  int first = INT_MAX;
  int last = INT_MIN;
  for (const auto *note : notes)
    if (note)
    {
      first = std::min(first, note->getStartFrame() * hop);
      last = std::max(last, note->getEndFrame() * hop);
    }
  if (first > last)
    return {0, 0};
  for (const auto &placement :
       breathnotes::place(project, breathnotes::Layout::committed))
    if (placement.drawn && !placement.breath &&
        std::find(notes.begin(), notes.end(), placement.anchor) != notes.end())
    {
      first = std::min(first, static_cast<int>(std::lround(placement.left * hop)));
      last = std::max(last, static_cast<int>(std::lround(placement.right * hop)));
    }
  return {first, last};
}

/**
 * What going from the gain regions `before` to `after` does to the rendered
 * audio: the gain each sample in [first, first + count) is multiplied by.
 * Both curves are smoothed alike, so a gain edit applies exactly this, and a
 * preview drawn with it shows exactly what the edit will do - including where
 * an earlier edit left the audio on a smoothed edge.
 */
inline std::vector<float> changeRatio(
    const std::vector<NoteGainCurve::Region> &before,
    const std::vector<NoteGainCurve::Region> &after, int first, int count,
    int totalSamples)
{
  auto ratio =
      NoteGainCurve::build(after, first, count, totalSamples, HOP_SIZE);
  const auto old =
      NoteGainCurve::build(before, first, count, totalSamples, HOP_SIZE);
  for (size_t i = 0; i < ratio.size() && i < old.size(); ++i)
    ratio[i] /= old[i];
  return ratio;
}

/**
 * changeRatio over the samples a change to [first, last) can reach - those
 * and the gain curve's smoothing past them - as {first sample, gains}. In
 * analysis samples (HOP_SIZE per frame), like the regions.
 */
inline std::pair<int, std::vector<float>> changeRatioAround(
    const AudioData &audioData,
    const std::vector<NoteGainCurve::Region> &before,
    const std::vector<NoteGainCurve::Region> &after, int first, int last)
{
  if (audioData.sampleRate <= 0)
    return {0, {}};
  const int total = static_cast<int>(
      static_cast<long long>(audioData.waveform.getNumSamples()) *
      SAMPLE_RATE / audioData.sampleRate);
  // NoteGainCurve smooths over a box this far either side.
  const int reach = std::max(64, HOP_SIZE / 2) / 2;
  const int start = std::clamp(first - reach, 0, total);
  const int end = std::clamp(last + reach, start, total);
  if (end <= start)
    return {start, {}};
  return {start, changeRatio(before, after, start, end - start, total)};
}
} // namespace projectgain
