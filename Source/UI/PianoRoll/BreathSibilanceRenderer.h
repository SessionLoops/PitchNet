#pragma once

#include "../../JuceHeader.h"
#include "../../Models/Project.h"
#include "CoordinateMapper.h"

#include <functional>
#include <optional>
#include <vector>

/**
 * Draws the detected breaths and sibilants (AudioData::breathSibilance).
 *
 * Inside a note body, the noise frames are hatched, so a note shows which of
 * its parts are unpitched consonants. Outside the notes, every breath is a
 * breath note (breathnotes, BreathNotes.h): its waveform, hatched, in the
 * colour of the note it belongs to and at that note's pitch; hovering
 * highlights it and dragging changes its level. A stray sibilant is not
 * drawn; it belongs to the note it touches. All of it stays put under pitch
 * edits, as the audio does, and follows a note that is moved or stretched in
 * time; a note moved or stretched over a breath cuts it back, since the
 * note's own audio plays there.
 */
namespace BreathSibilanceRenderer
{
/**
 * Hatch the noise frames of `note`, whose body was just filled as
 * `bodyShape` across [x, x + width] (world pixels). Label frames map onto
 * the body through the note's source range, so a retimed or stretched note
 * keeps its hatching on the audio it belongs to.
 */
void hatchNoteBody(juce::Graphics &g, const AudioData &audioData,
                   const Note &note, const juce::Path &bodyShape, float x,
                   float width);

/**
 * Bands at least `minHeight` tall, centred on `centreY`, over the noise
 * frames of `note`. Added to the waveform body before it is filled, so a
 * quiet consonant is still thick enough to show its hatching.
 */
juce::Path noiseBands(const AudioData &audioData, const Note &note, float x,
                      float width, float centreY, float minHeight);

/** How the breath notes are shown. */
struct BreathView
{
  // The colour a note is drawn in; a breath note takes its note's colour.
  std::function<juce::Colour(const Note &)> noteColour;
  // The breath note to highlight (breathnotes::Placement::key) - the one
  // under the pointer, or being dragged - or -1.
  int hovered = -1;
  // While levels are dragged: the level to show for a breath note, in dB.
  std::function<std::optional<float>(int key)> draggedDb;
  // The breath note whose level is shown in a label while dragged, or -1.
  int labelled = -1;
};

/**
 * Draw the breath notes and the sibilants against notes, within
 * [visibleStartSeconds, visibleEndSeconds]. World coordinates, like the
 * notes.
 */
void drawOutsideNotes(juce::Graphics &g, const Project &project,
                      const CoordinateMapper &mapper,
                      double visibleStartSeconds, double visibleEndSeconds,
                      const BreathView &view);

/**
 * The breath note at a world position, as its key
 * (breathnotes::Placement::key), or -1.
 */
int breathAt(const Project &project, const CoordinateMapper &mapper,
             float worldX, float worldY);
} // namespace BreathSibilanceRenderer
