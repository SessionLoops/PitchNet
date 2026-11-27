#pragma once

#include "../../JuceHeader.h"
#include "../../Audio/Polyphonic/PolyChords.h"
#include "../../Models/Project.h"
#include "CoordinateMapper.h"

#include <memory>
#include <optional>
#include <vector>

/**
 * The chord track of a polyphonic take: a lane under the ruler with the
 * take's chords.
 *
 * In an ARA host that has a chord track (Studio One, for example) the chords
 * are the host's (setHostChords, from kARAContentTypeSheetChords); a plug-in
 * without them shows no chord track (PianoRollComponent::setChordTrackHostOnly).
 * In the standalone app they come from the take's notes with their edits, so re-pitching, moving or
 * deleting notes changes the chords shown: detected and segmented by
 * polyphonic::detectChords, or, once the user has split or merged chords
 * (Project::getChordBoundaries), one chord per span of that segmentation
 * (polyphonic::chordsOver). Chords are read again only when the take, its
 * edits, the segmentation or the beat grid change.
 *
 * Draws in screen coordinates, like TimelineRenderer, and tells what a point
 * of the lane is over (hitTest): a chord's name, the rest of a chord, or the
 * boundary between two.
 */
class ChordLane
{
public:
  /** The beat grid chords change on: seconds per beat and where a beat falls,
   *  in project time. beatSeconds 0: free time. */
  struct Grid
  {
    double beatSeconds = 0.0;
    double originProjectSeconds = 0.0;
    bool operator==(const Grid &o) const
    {
      return beatSeconds == o.beatSeconds && originProjectSeconds == o.originProjectSeconds;
    }
    bool operator!=(const Grid &o) const { return !(*this == o); }
  };

  void setCoordinateMapper(CoordinateMapper *mapper) { coordMapper = mapper; }
  void setProject(Project *proj);

  /** The host's chord track, in host timeline seconds; empty: none (the
   *  chords come from the take). */
  void setHostChords(std::vector<polyphonic::ChordSpan> timelineChords);
  bool showsHostChords() const { return !hostChords.empty(); }
  /** The user's split / merge segmentation is in use. */
  bool isSegmentedByUser() const;

  /** The chords shown, in project time (open ends are -/+ infinity): the
   *  host's, the user's segmentation, or the detected ones. In the user's
   *  segmentation a span that plays no chord is kept (chord none), so it can
   *  be merged. */
  const std::vector<polyphonic::ChordSpan> &getChords(const Grid &grid);
  /** The chords detected in the take (with its edits), in project time,
   *  whatever is shown: what the audio plays, chord by chord. */
  const std::vector<polyphonic::ChordSpan> &getDetectedChords(const Grid &grid);
  /** The chord shown at a project time, or null. */
  const polyphonic::ChordSpan *chordAt(double projectSeconds, const Grid &grid);
  /** The segmentation shown, as boundaries: where each chord starts and ends
   *  (what a first split or merge starts from). */
  std::vector<double> getShownBoundaries(const Grid &grid);
  /** Whether the take's key is written with flats (for naming chords). */
  bool prefersFlats() const { return flats; }

  struct Hit
  {
    enum class Kind { None, Name, Chord, Boundary, StartEdge, EndEdge };
    Kind kind = Kind::None;
    int span = -1;              // into getChords
    double boundarySeconds = 0; // Boundary: where the two chords meet; an edge: where it is
  };
  /** What a screen x in the lane is over. Boundaries are where two chords
   *  meet (a few pixels either side); edges are a chord's start or end with
   *  no chord beside it (a gap or the ends of the track); a chord's name is
   *  its text. */
  Hit hitTest(int screenX, int componentWidth, int scrollBarSize, const Grid &grid);

  /** The outline of the chord drawn at a project time (screen coordinates),
   *  if it is in view: where its name is edited. */
  std::optional<juce::Rectangle<float>> getChordBox(double projectSeconds, int componentWidth,
                                                    int scrollBarSize, const Grid &grid);
  juce::Colour getChordFill() const;
  juce::Colour getNameColour() const;
  static float getNameFontHeight();

  /** The pointer's preview: a split line at a project time, or a boundary
   *  about to go. Returns whether it changed. */
  bool setHover(std::optional<double> splitSeconds, std::optional<double> mergeBoundary);

  /** The lane: from CoordinateMapper::rulerHeight, chordLaneHeight high. */
  void draw(juce::Graphics &g, int componentWidth, int scrollBarSize, const Grid &grid);

private:
  struct Layout
  {
    int span;
    float x0, x1;                 // the chord's extent, clamped near the lane
    juce::Rectangle<float> box;   // its outline
    juce::Rectangle<float> text;  // where its name is written
  };
  std::vector<Layout> layout(int componentWidth, int scrollBarSize, const Grid &grid);
  double toScreenX(double projectSeconds, double laneLeft, double laneRight) const;
  std::vector<polyphonic::ChordNote> notesOf(const PolyphonicTake &take) const;

  CoordinateMapper *coordMapper = nullptr;
  Project *project = nullptr;
  std::vector<polyphonic::ChordSpan> hostChords; // timeline seconds

  // The host's chords in project time, and the offset they were placed with.
  std::vector<polyphonic::ChordSpan> hostProjectChords;
  bool hostValid = false;
  double hostOffset = 0.0;

  // The detected chords, and what they were computed from.
  std::vector<polyphonic::ChordSpan> detected;
  bool detectedValid = false;
  bool flats = false;
  std::shared_ptr<const PolyphonicTake> take;
  Project::PolyphonicEdits edits;
  Grid detectedGrid;

  // The chords of the user's segmentation, and what they were computed from.
  std::vector<polyphonic::ChordSpan> segmented;
  bool segmentedValid = false;
  std::shared_ptr<const PolyphonicTake> segmentedTake;
  Project::PolyphonicEdits segmentedEdits;
  std::vector<double> segmentedBoundaries;

  std::optional<double> hoverSplit, hoverMerge;
};
