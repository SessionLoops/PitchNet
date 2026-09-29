#pragma once

#include "../JuceHeader.h"
#include <memory>
#include <vector>

class Project;

/**
 * Read-only picture of one ARA playback region, for drawing every region of a
 * track on the canvas and in the thumbnail while only one region (the active
 * one) is editable.
 *
 * All times are absolute host-timeline seconds. The UI must not depend on the
 * ARA SDK, so the plug-in builds these from each region's own Project and
 * hands them over as plain data.
 */
struct MainViewRegionPreview {
  struct NoteShape {
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    float midi = 0.0f; // adjusted pitch (includes the note's pitch offset)
  };

  juce::String key;
  juce::String name;
  double startSeconds = 0.0;
  double endSeconds = 0.0;
  bool active = false;
  // False when the region has not been analysed yet: only its range is known.
  bool hasContent = false;

  std::vector<NoteShape> notes;

  // The region's window of its modification's Project, rebased so its time
  // zero is the window start and cut down to what the note and pitch-curve
  // renderers read (notes, pitch arrays, the window's audio). Drawn in Track
  // mode with the same renderers as the active region, then greyed.
  // projectHostOffsetSeconds is the host time of this Project's zero.
  std::shared_ptr<Project> project;
  double projectHostOffsetSeconds = 0.0;

  // The region's own audio (first channel, the one the canvas draws),
  // starting at waveformStartSeconds. Inactive regions are drawn from it with
  // the same envelope code as the active region, so they look identical.
  // Shared so republishing previews never copies the audio.
  std::shared_ptr<const std::vector<float>> waveformSamples;
  double waveformSampleRate = 0.0;
  double waveformStartSeconds = 0.0;

  bool contains(double timeSeconds) const {
    return timeSeconds >= startSeconds && timeSeconds < endSeconds;
  }
};

using MainViewRegionPreviewList =
    std::shared_ptr<const std::vector<MainViewRegionPreview>>;
