#pragma once

#include "BreathSibilanceDetector.h"
#include "../Models/Project.h"
#include "../Utils/Constants.h"
#include "../Utils/NoteGainCurve.h"
#include "../Utils/TimingRegionUtils.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

/**
 * Breath notes: the breaths outside every note, laid out where they play.
 *
 * A breath is an object of its own: the piano roll draws it like a note, in
 * the colour of the note it belongs to, and dragging it changes its level.
 * It is not a Note, so nothing that works on pitched notes ever sees it.
 *
 * Noise outside the notes is gathered into events (gaps of up to 4 frames);
 * an event holding any breath is a breath. Where a note was moved or
 * stretched over an event, the note's audio plays there, so the event keeps
 * only what no note covers. A breath within ~0.5 s of the note it leads into
 * - or else of the note before it - reaches across the silence to meet it,
 * taking in any other noise on the way. A breath further from any note
 * stands alone, at the pitch of the nearest note. Other noise (a stray
 * sibilant) is only shown where it touches a note.
 *
 * Gains are kept per source frame in AudioData::breathGainDb, so they
 * survive re-layouts, saving and region reshaping. A breath note's gain is
 * the one at its first breath frame; setting it sets the whole note.
 */
namespace breathnotes
{
struct Placement
{
  // The noise, as visual frame indices [start, end).
  float start = 0.0f;
  float end = 0.0f;
  // Drawn extent in visual frames. Frame k is centred on k, so an event's
  // own edges sit half a frame earlier; an edge that meets a note lies on
  // the note's edge.
  float left = 0.0f;
  float right = 0.0f;
  // Visual minus source frames.
  float shift = 0.0f;
  bool breath = false;
  // False when taken into a breath, or a sibilant that touches no note.
  bool drawn = false;
  // The note it belongs to: its pitch row and colour.
  const Note *anchor = nullptr;

  /** Stable identity: the first source frame of its noise. */
  int key() const { return static_cast<int>(std::lround(start - shift)); }
  /** Visual frames it spans, [firstFrame, lastFrame). */
  int firstFrame() const
  {
    return static_cast<int>(std::floor(left + 0.5f));
  }
  int lastFrame() const { return static_cast<int>(std::floor(right + 0.5f)); }
};

/** Which frames a note is laid out over: as drawn (following a timing drag
 *  in progress) or as committed (what is rendered). */
enum class Layout
{
  onScreen,
  committed
};

namespace detail
{
inline float noteStart(const Note &note, Layout layout)
{
  return layout == Layout::onScreen ? note.getVisualStartFrame()
                                    : static_cast<float>(note.getStartFrame());
}

inline float noteEnd(const Note &note, Layout layout)
{
  return layout == Layout::onScreen ? note.getVisualEndFrame()
                                    : static_cast<float>(note.getEndFrame());
}
} // namespace detail

/**
 * How far audio at source frames [start, end) sits from where it was
 * recorded. A region's lead-in moves with its first note and its tail with
 * its last note (IncrementalSynthesizer's preserved timing spans); anything
 * else stays where it is.
 */
inline float displacementFrames(const Project &project, int start, int end,
                                Layout layout = Layout::onScreen)
{
  const auto &audioData = project.getAudioData();
  timingRegions::SourceRegion region{0, 0, audioData.getNumFrames()};
  if (!audioData.segmentChunkRanges.empty())
  {
    const int midpoint = start + (end - start) / 2;
    bool found = false;
    for (size_t i = 0; i < audioData.segmentChunkRanges.size(); ++i)
    {
      const auto &range = audioData.segmentChunkRanges[i];
      if (midpoint >= range.first && midpoint < range.second)
      {
        region = {static_cast<int>(i), range.first, range.second};
        found = true;
        break;
      }
    }
    if (!found)
      return 0.0f;
  }

  if (const auto *first = timingRegions::firstNote(project, region))
    if (start >= region.start && end <= first->getSrcStartFrame())
      return detail::noteStart(*first, layout) -
             static_cast<float>(first->getSrcStartFrame());
  if (const auto *last = timingRegions::lastNote(project, region))
    if (start >= last->getSrcEndFrame() && end <= region.end)
      return detail::noteEnd(*last, layout) -
             static_cast<float>(last->getSrcEndFrame());
  return 0.0f;
}

/** Every breath and stray sibilant outside the notes, placed. */
inline std::vector<Placement> place(const Project &project,
                                    Layout layout = Layout::onScreen)
{
  std::vector<Placement> placements;
  const auto &labels = project.getAudioData().breathSibilance;
  if (labels.empty())
    return placements;

  std::vector<const Note *> notes;
  std::vector<std::uint8_t> covered(labels.size(), 0);
  for (const auto &note : project.getNotes())
  {
    if (note.isRest())
      continue;
    notes.push_back(&note);
    const int first = std::max(0, note.getSrcStartFrame());
    const int last =
        std::min(note.getSrcEndFrame(), static_cast<int>(labels.size()));
    for (int frame = first; frame < last; ++frame)
      covered[static_cast<size_t>(frame)] = 1;
  }
  // With no notes there is no pitch to place the noise at.
  if (notes.empty())
    return placements;

  // A frame or two left over beside a note is the note's own consonant
  // edge; only what reads as a separate event is placed.
  constexpr int kMinOutsideFrames = 3;
  // Noise this close together outside the notes is one event.
  constexpr int kClusterGapFrames = 4;
  // A breath reaches across this much silence to meet its note (~0.5 s).
  constexpr float kMeetGapFrames = 43.0f;
  // Other noise this close to a note touches it, as it would join an event.
  constexpr float kTouchFrames = static_cast<float>(kClusterGapFrames);

  // Events outside the notes, in source frames.
  struct Event
  {
    int start = 0;
    int end = 0;
  };
  std::vector<Event> events;
  const int frameCount = static_cast<int>(labels.size());
  for (int frame = 0; frame < frameCount;)
  {
    if (covered[static_cast<size_t>(frame)] ||
        !breathsibilance::isNoise(labels[static_cast<size_t>(frame)]))
    {
      ++frame;
      continue;
    }
    Event event{frame, frame + 1};
    int next = frame + 1;
    while (next < frameCount && covered[static_cast<size_t>(next)] == 0 &&
           next - event.end <= kClusterGapFrames)
    {
      if (breathsibilance::isNoise(labels[static_cast<size_t>(next)]))
        event.end = next + 1;
      ++next;
    }
    if (event.end - event.start >= kMinOutsideFrames)
      events.push_back(event);
    frame = next;
  }

  // A note moved or stretched over an event plays its own audio there, so
  // the event keeps only what no note covers.
  struct Piece
  {
    float start = 0.0f;
    float end = 0.0f;
    bool cutStart = false;
    bool cutEnd = false;
  };
  for (const auto &event : events)
  {
    const float shift =
        displacementFrames(project, event.start, event.end, layout);
    std::vector<Piece> pieces{{static_cast<float>(event.start) + shift,
                               static_cast<float>(event.end) + shift, false,
                               false}};
    for (const auto *note : notes)
    {
      const float noteStart = detail::noteStart(*note, layout);
      const float noteEnd = detail::noteEnd(*note, layout);
      std::vector<Piece> uncovered;
      for (const auto &piece : pieces)
      {
        if (noteEnd <= piece.start || noteStart >= piece.end)
        {
          uncovered.push_back(piece);
          continue;
        }
        if (piece.start < noteStart)
          uncovered.push_back({piece.start, noteStart, piece.cutStart, true});
        if (noteEnd < piece.end)
          uncovered.push_back({noteEnd, piece.end, true, piece.cutEnd});
      }
      pieces = std::move(uncovered);
    }

    for (auto piece : pieces)
    {
      // Back to the noise frames the piece still holds.
      const int first = static_cast<int>(std::floor(piece.start - shift));
      const int last = static_cast<int>(std::ceil(piece.end - shift));
      int firstNoise = -1;
      int lastNoise = -1;
      bool breath = false;
      for (int frame = first; frame < last; ++frame)
      {
        const auto label = breathsibilance::labelAt(labels, frame);
        if (!breathsibilance::isNoise(label))
          continue;
        if (firstNoise < 0)
          firstNoise = frame;
        lastNoise = frame;
        breath = breath || label == breathsibilance::Breath;
      }
      if (firstNoise < 0)
        continue;
      if (static_cast<float>(firstNoise) + shift > piece.start)
      {
        piece.start = static_cast<float>(firstNoise) + shift;
        piece.cutStart = false;
      }
      if (static_cast<float>(lastNoise + 1) + shift < piece.end)
      {
        piece.end = static_cast<float>(lastNoise + 1) + shift;
        piece.cutEnd = false;
      }
      if (piece.end - piece.start < static_cast<float>(kMinOutsideFrames))
        continue;

      Placement placement;
      placement.start = piece.start;
      placement.end = piece.end;
      placement.left = piece.cutStart ? piece.start : piece.start - 0.5f;
      placement.right = piece.cutEnd ? piece.end : piece.end - 0.5f;
      placement.shift = shift;
      placement.breath = breath;
      placements.push_back(placement);
    }
  }

  // Which note each belongs to, and how far it reaches to meet it.
  std::vector<bool> absorbed(placements.size(), false);
  for (size_t index = 0; index < placements.size(); ++index)
  {
    auto &placement = placements[index];
    const bool breath = placement.breath;

    // The closest note on each side. No note overlaps a placement: notes
    // over an event were cut out of it above.
    const Note *following = nullptr;
    const Note *previous = nullptr;
    float followingGap = std::numeric_limits<float>::max();
    float previousGap = std::numeric_limits<float>::max();
    for (const auto *note : notes)
    {
      const float noteStart = detail::noteStart(*note, layout);
      const float noteEnd = detail::noteEnd(*note, layout);
      if (noteStart >= placement.end)
      {
        if (noteStart - placement.end < followingGap)
        {
          followingGap = noteStart - placement.end;
          following = note;
        }
      }
      else if (noteEnd <= placement.start)
      {
        if (placement.start - noteEnd < previousGap)
        {
          previousGap = placement.start - noteEnd;
          previous = note;
        }
      }
    }

    const float reach = breath ? kMeetGapFrames : kTouchFrames;
    const bool meetsFollowing = following != nullptr && followingGap <= reach;
    const bool meetsPrevious = previous != nullptr && previousGap <= reach;
    // A breath leads into its phrase, so it meets the note that follows
    // whenever that is in reach; other noise goes with the nearer note.
    if (meetsFollowing &&
        (breath || !meetsPrevious || followingGap <= previousGap))
    {
      const float meet = detail::noteStart(*following, layout);
      for (size_t other = 0; other < placements.size(); ++other)
        if (other != index && placements[other].start >= placement.start &&
            placements[other].end <= meet)
          absorbed[other] = true;
      placement.right = meet;
      placement.anchor = following;
      placement.drawn = true;
    }
    else if (meetsPrevious)
    {
      const float meet = detail::noteEnd(*previous, layout);
      for (size_t other = 0; other < placements.size(); ++other)
        if (other != index && placements[other].end <= placement.end &&
            placements[other].start >= meet)
          absorbed[other] = true;
      placement.left = meet;
      placement.anchor = previous;
      placement.drawn = true;
    }
    else if (breath)
    {
      // Out of reach of any note: a breath of its own, at the pitch of the
      // nearest note.
      placement.anchor = following != nullptr && followingGap <= previousGap
                             ? following
                             : (previous != nullptr ? previous : following);
      placement.drawn = placement.anchor != nullptr;
    }
  }
  for (size_t index = 0; index < placements.size(); ++index)
    if (absorbed[index])
      placements[index].drawn = false;
  return placements;
}

/** Gain in dB stored for a source frame. */
inline float gainAt(const AudioData &audioData, int sourceFrame)
{
  const auto &gains = audioData.breathGainDb;
  return sourceFrame >= 0 && sourceFrame < static_cast<int>(gains.size())
             ? gains[static_cast<size_t>(sourceFrame)]
             : 0.0f;
}

/** A breath note's gain in dB: the one at its first breath frame. */
inline float gainDb(const AudioData &audioData, const Placement &placement)
{
  const int first =
      static_cast<int>(std::lround(placement.start - placement.shift));
  const int last =
      static_cast<int>(std::lround(placement.end - placement.shift));
  for (int frame = first; frame < last; ++frame)
    if (breathsibilance::labelAt(audioData.breathSibilance, frame) ==
        breathsibilance::Breath)
      return gainAt(audioData, frame);
  return gainAt(audioData, first);
}

/** Set a breath note's gain across every frame it spans. */
inline void setGainDb(AudioData &audioData, const Placement &placement,
                      float db)
{
  auto &gains = audioData.breathGainDb;
  const int first = std::max(
      0, static_cast<int>(
             std::floor(placement.left - placement.shift + 0.5f)));
  const int last = static_cast<int>(
      std::floor(placement.right - placement.shift + 0.5f));
  if (last <= first)
    return;
  if (static_cast<int>(gains.size()) < last)
  {
    if (db == 0.0f && first >= static_cast<int>(gains.size()))
      return;
    gains.resize(static_cast<size_t>(last), 0.0f);
  }
  std::fill(gains.begin() + first, gains.begin() + last, db);
  while (!gains.empty() && gains.back() == 0.0f)
    gains.pop_back();
}

/** The breath note with this key, if it is drawn. */
inline const Placement *find(const std::vector<Placement> &placements,
                             int key)
{
  for (const auto &placement : placements)
    if (placement.drawn && placement.breath && placement.key() == key)
      return &placement;
  return nullptr;
}

/** A breath note's level to use instead of its stored one, as for previewing
 *  a level being dragged. */
struct LevelOverride
{
  int key = -1; // Placement::key, or -1 for none
  float db = 0.0f;
};

/**
 * Gain regions, in absolute samples, of the breath notes whose gain is not
 * 0, as rendered: over each note's whole extent, up to the edge of the note
 * it meets.
 */
inline std::vector<NoteGainCurve::Region> gainRegions(
    const Project &project, const LevelOverride &override = {})
{
  std::vector<NoteGainCurve::Region> regions;
  const auto &audioData = project.getAudioData();
  if (audioData.breathGainDb.empty() && override.key < 0)
    return regions;
  for (const auto &placement : place(project, Layout::committed))
  {
    if (!placement.drawn || !placement.breath)
      continue;
    const float db = placement.key() == override.key
                         ? override.db
                         : gainDb(audioData, placement);
    if (db == 0.0f)
      continue;
    regions.push_back(
        {static_cast<int>(std::lround(placement.left * HOP_SIZE)),
         static_cast<int>(std::lround(placement.right * HOP_SIZE)), db});
  }
  return regions;
}
} // namespace breathnotes
