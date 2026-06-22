#pragma once

#include "../../JuceHeader.h"
#include "../../Models/Project.h"
#include "CoordinateMapper.h"

/**
 * Draws the time-domain waveform behind the piano roll grid, with an internal
 * Image cache keyed on scroll + zoom + visible bounds. The cache is rebuilt
 * lazily on the next draw whenever those inputs change.
 */
class WaveformBackgroundRenderer
{
public:
  WaveformBackgroundRenderer() = default;

  void setCoordinateMapper(CoordinateMapper *mapper) { coordMapper = mapper; }
  void setProject(Project *proj)
  {
    project = proj;
    liveWaveform.setSize(0, 0);
    liveNumSamples = 0;
    liveSampleRate = 0.0;
    // Free old image when project changes; metadata reset forces rebuild.
    waveformCache = juce::Image();
    cachedScrollX = -1.0;
    cachedPixelsPerSecond = -1.0f;
    cachedWidth = 0;
    cachedHeight = 0;
  }

  void draw(juce::Graphics &g, const juce::Rectangle<int> &visibleArea);
  void beginLiveWaveform(double sampleRate, double timelineOffsetSeconds);
  void appendLiveWaveform(const juce::AudioBuffer<float> &buffer);

  // Soft invalidation: next draw will regenerate the cache. Does not free the
  // image buffer; matches the prior PianoRollComponent::invalidateWaveformCache.
  void invalidateCache() { cachedScrollX = -1.0; }

private:
  CoordinateMapper *coordMapper = nullptr;
  Project *project = nullptr;

  juce::Image waveformCache;
  double cachedScrollX = -1.0;
  float cachedPixelsPerSecond = -1.0f;
  int cachedWidth = 0;
  int cachedHeight = 0;
  juce::AudioBuffer<float> liveWaveform;
  int liveNumSamples = 0;
  double liveSampleRate = 0.0;
  double liveTimelineOffsetSeconds = 0.0;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformBackgroundRenderer)
};
