#pragma once

#include "../../JuceHeader.h"
#include "../../Models/Project.h"
#include "CoordinateMapper.h"

class PitchToolController;

/**
 * Draws the time-domain waveform behind the piano roll grid, with an internal
 * Image cache. The cache is a strip wider than the viewport (in world pixels),
 * so scrolling just blits a different slice of it and only rebuilds once the
 * viewport leaves the strip. While the zoom level is changing (mouse-wheel
 * zoom), the stale strip is stretched to the new zoom and a single rebuild is
 * done shortly after the zoom settles.
 */
class WaveformBackgroundRenderer : private juce::Timer
{
public:
  WaveformBackgroundRenderer() = default;
  ~WaveformBackgroundRenderer() override { stopTimer(); }

  // Called (on the message thread) when the renderer wants a repaint, e.g.
  // after a zoom has settled and the cache should be rebuilt at full quality.
  std::function<void()> onRepaintRequested;

  void setPitchToolController(PitchToolController* controller) { pitchToolController = controller; }
  void setCoordinateMapper(CoordinateMapper *mapper) { coordMapper = mapper; }
  void setProject(Project *proj)
  {
    project = proj;
    liveWaveform.setSize(0, 0);
    liveNumSamples = 0;
    liveSampleRate = 0.0;
    // Free old image when project changes; metadata reset forces rebuild.
    waveformCache = juce::Image();
    cachedStripStartX = 0;
    cachedPixelsPerSecond = -1.0f;
    cachedHeight = 0;
    cacheDirty = true;
    zoomSettled = false;
    stopTimer();
  }

  void draw(juce::Graphics &g, const juce::Rectangle<int> &visibleArea);
  void beginLiveWaveform(double sampleRate, double timelineOffsetSeconds);
  void appendLiveWaveform(const juce::AudioBuffer<float> &buffer);

  // Soft invalidation: next draw will regenerate the cache. Does not free the
  // image buffer; matches the prior PianoRollComponent::invalidateWaveformCache.
  void invalidateCache() { cacheDirty = true; }

private:
  void timerCallback() override;

  static constexpr int zoomSettleMs = 90;

  CoordinateMapper *coordMapper = nullptr;
  Project *project = nullptr;
  PitchToolController* pitchToolController = nullptr;
  bool cachedAmplitudePreview = false;

  juce::Image waveformCache;
  // Left edge of the cached strip, in world pixels at cachedPixelsPerSecond.
  int cachedStripStartX = 0;
  float cachedPixelsPerSecond = -1.0f;
  int cachedHeight = 0;
  bool cacheDirty = true;   // content changed; rebuild on next draw
  bool zoomSettled = false; // zoom stopped changing; rebuild instead of stretch
  juce::AudioBuffer<float> liveWaveform;
  int liveNumSamples = 0;
  double liveSampleRate = 0.0;
  double liveTimelineOffsetSeconds = 0.0;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformBackgroundRenderer)
};
