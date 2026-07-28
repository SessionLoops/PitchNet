#pragma once

#include <vector>

namespace PitchToolOperations {

/**
 * Applies a linear tilt around a pivot position.
 *
 * The value at the pivot stays unchanged, and the contour is shifted
 * linearly across the note. The furthest end from the pivot reaches
 * the full `amount` in semitones.
 */
std::vector<float> tiltDeltaPitch(const std::vector<float>& deltaPitch,
                                  float pivotPosition,
                                  float amount);

/**
 * Scales deviations from the note center (zero delta pitch).
 *
 * `factor = 0` flattens to zero (base MIDI note) and `factor = 1` keeps
 * the original contour unchanged.
 */
std::vector<float> scaleVibrato(const std::vector<float>& deltaPitch,
                                float factor);

/**
 * Smooths one boundary to connect with adjacent pitch context.
 *
 * For left side, fades from `targetPitch` to the note boundary.
 * For right side, fades from the note boundary to `targetPitch`.
 * Cosine interpolation is used to avoid abrupt slope changes.
 */
std::vector<float> smoothBoundary(const std::vector<float>& deltaPitch,
                                  int side,
                                  int transitionFrames,
                                  float targetPitch);

/**
 * Computes the arithmetic mean of a pitch contour.
 * Returns 0 when the input is empty.
 */
float computeMean(const std::vector<float>& deltaPitch);

/**
 * Context for adjacent notes (for boundary smoothing).
 * Stores boundary delta pitch values from temporally adjacent notes.
 */
struct AdjacentNoteContext
{
  bool hasLeft = false;           // True if a previous note exists
  bool hasRight = false;          // True if a next note exists
  float leftBoundaryDelta = 0.0f;  // Last delta value of previous note
  float rightBoundaryDelta = 0.0f; // First delta value of next note
};

/**
 * Applies all transformation parameters non-destructively.
 * 
 * This function chains multiple transformations in order:
 * 1. Vibrato scaling
 * 2. Tilt (left and right combined)
 * 3. Boundary smoothing (left and right)
 * 
 * @param originalDelta The pristine deltaPitch curve from analysis (never modified)
 * @param tiltLeft Tilt amount at left edge in semitones
 * @param tiltRight Tilt amount at right edge in semitones
 * @param vibrato Vibrato factor (1.0=original, 0.0=flat, >1.0=amplify, <0.0=invert)
 * @param smoothLeftFrames Smoothing transition length at left boundary
 * @param smoothRightFrames Smoothing transition length at right boundary
 * @param adjacentContext Context for adjacent notes (for boundary smoothing)
 * @return Transformed deltaPitch curve
 */
std::vector<float> applyAllTransformations(const std::vector<float>& originalDelta,
                                           float tiltLeft,
                                           float tiltRight,
                                           float vibrato,
                                           int smoothLeftFrames,
                                           int smoothRightFrames,
                                           const AdjacentNoteContext& adjacentContext = {});

} // namespace PitchToolOperations
