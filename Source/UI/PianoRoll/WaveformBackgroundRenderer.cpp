#include "WaveformBackgroundRenderer.h"
#include "VisualWaveformEnvelope.h"
#include "../../Utils/Constants.h"
#include "../../Utils/UI/Theme.h"

#include <cmath>

void WaveformBackgroundRenderer::beginLiveWaveform(
    double sampleRate, double timelineOffsetSeconds)
{
  liveWaveform.setSize(0, 0);
  liveNumSamples = 0;
  liveSampleRate = sampleRate;
  liveTimelineOffsetSeconds = std::max(0.0, timelineOffsetSeconds);
  invalidateCache();
}

void WaveformBackgroundRenderer::appendLiveWaveform(
    const juce::AudioBuffer<float> &buffer)
{
  if (buffer.getNumSamples() <= 0 || liveSampleRate <= 0.0)
    return;

  const int required = liveNumSamples + buffer.getNumSamples();
  const int capacity = liveWaveform.getNumSamples();
  if (capacity < required) {
    const int newCapacity = std::max(required, std::max(32768, capacity * 2));
    liveWaveform.setSize(buffer.getNumChannels(), newCapacity, true, true,
                         false);
  }

  const int channels = std::min(liveWaveform.getNumChannels(),
                                buffer.getNumChannels());
  for (int ch = 0; ch < channels; ++ch)
    liveWaveform.copyFrom(ch, liveNumSamples, buffer, ch, 0,
                          buffer.getNumSamples());
  liveNumSamples = required;
  invalidateCache();
}

void WaveformBackgroundRenderer::draw(juce::Graphics &g,
                                      const juce::Rectangle<int> &visibleArea)
{
  if (!coordMapper)
    return;

  const bool drawingLive = liveNumSamples > 0 && liveSampleRate > 0.0;
  const auto *projectAudio = project ? &project->getAudioData() : nullptr;
  const bool drawingProject =
      projectAudio && projectAudio->waveform.getNumSamples() > 0 &&
      projectAudio->sampleRate > 0;
  if (!drawingLive && !drawingProject)
    return;

  const double scrollX = coordMapper->getScrollX();
  const float pixelsPerSecond = coordMapper->getPixelsPerSecond();

  const bool cacheValid = waveformCache.isValid() &&
                          std::abs(cachedScrollX - scrollX) < 1.0 &&
                          std::abs(cachedPixelsPerSecond - pixelsPerSecond) < 0.01f &&
                          cachedWidth == visibleArea.getWidth() &&
                          cachedHeight == visibleArea.getHeight();

  if (cacheValid)
  {
    g.drawImageAt(waveformCache, visibleArea.getX(), visibleArea.getY());
    return;
  }

  waveformCache = juce::Image(juce::Image::ARGB, visibleArea.getWidth(),
                              visibleArea.getHeight(), true);
  juce::Graphics cacheGraphics(waveformCache);

  const float visibleHeight = static_cast<float>(visibleArea.getHeight());
  const float centerY = visibleHeight * 0.5f;
  const float waveformHeight = visibleHeight * 0.8f;

  const int visibleWidth = visibleArea.getWidth();
  if (visibleWidth <= 0 || visibleArea.getHeight() <= 0)
    return;

  auto drawWaveform = [&](const juce::AudioBuffer<float> &source,
                          int numSamples, double sampleRate,
                          double timelineOffset) {
    if (numSamples <= 0 || sampleRate <= 0.0 || source.getNumChannels() <= 0)
      return;

    const double samplesPerPixel = sampleRate / pixelsPerSecond;
    const double offsetPixels = timelineOffset * pixelsPerSecond;
    const double waveformStartX = offsetPixels - scrollX;
    const double waveformEndX =
        offsetPixels + static_cast<double>(numSamples) / samplesPerPixel -
        scrollX;
    const int firstPixel = juce::jlimit(
        0, visibleWidth, static_cast<int>(std::floor(waveformStartX)));
    const int lastPixel = juce::jlimit(
        0, visibleWidth, static_cast<int>(std::ceil(waveformEndX)));
    if (lastPixel <= firstPixel)
      return;

    // Map only the portion of the viewport covered by real waveform samples.
    // Clamping the sample range and then drawing it across visibleWidth would
    // stretch a short waveform to the right edge of the viewport.
    const int startSample = juce::jlimit(
        0, numSamples - 1,
        static_cast<int>(std::floor((scrollX + firstPixel - offsetPixels) *
                                    samplesPerPixel)));
    const int endSample = juce::jlimit(
        startSample + 1, numSamples,
        static_cast<int>(std::ceil((scrollX + lastPixel - offsetPixels) *
                                   samplesPerPixel)));
    const int pointCount = lastPixel - firstPixel;

    const auto displayEnvelope = VisualWaveformEnvelope::build(
        source.getReadPointer(0), numSamples, startSample, endSample, pointCount,
        static_cast<float>(pointCount), sampleRate, pixelsPerSecond);

    juce::Path waveformPath;
    waveformPath.startNewSubPath(static_cast<float>(firstPixel), centerY);

    // Top half
    for (int px = 0; px < pointCount; ++px)
    {
      const float y =
          centerY -
          displayEnvelope[static_cast<size_t>(px)] * waveformHeight * 0.5f;
      waveformPath.lineTo(static_cast<float>(firstPixel + px), y);
    }

    // Bottom half (reverse)
    for (int px = pointCount - 1; px >= 0; --px)
    {
      const float y =
          centerY +
          displayEnvelope[static_cast<size_t>(px)] * waveformHeight * 0.5f;
      waveformPath.lineTo(static_cast<float>(firstPixel + px), y);
    }

    waveformPath.closeSubPath();

    cacheGraphics.setColour(juce::Colours::white.withAlpha(0.05f));
    cacheGraphics.fillPath(waveformPath);
  };

  // Keep completed captures visible while a new region is being recorded.
  if (drawingProject)
    drawWaveform(projectAudio->waveform, projectAudio->waveform.getNumSamples(),
                 projectAudio->sampleRate, 0.0);
  if (drawingLive)
    drawWaveform(liveWaveform, liveNumSamples, liveSampleRate,
                 liveTimelineOffsetSeconds);

  cachedScrollX = scrollX;
  cachedPixelsPerSecond = pixelsPerSecond;
  cachedWidth = visibleArea.getWidth();
  cachedHeight = visibleArea.getHeight();

  g.drawImageAt(waveformCache, visibleArea.getX(), visibleArea.getY());
}
