#include "WaveformBackgroundRenderer.h"
#include "VisualWaveformEnvelope.h"
#include "../../Utils/Constants.h"
#include "../../Utils/UI/Theme.h"

#include <cmath>

void WaveformBackgroundRenderer::draw(juce::Graphics &g,
                                      const juce::Rectangle<int> &visibleArea)
{
  if (!project || !coordMapper)
    return;

  const auto &audioData = project->getAudioData();
  if (audioData.waveform.getNumSamples() == 0)
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

  const float *samples = audioData.waveform.getReadPointer(0);
  const int numSamples = audioData.waveform.getNumSamples();

  const float visibleHeight = static_cast<float>(visibleArea.getHeight());
  const float centerY = visibleHeight * 0.5f;
  const float waveformHeight = visibleHeight * 0.8f;

  juce::Path waveformPath;
  const int visibleWidth = visibleArea.getWidth();
  if (visibleWidth <= 0 || visibleArea.getHeight() <= 0)
    return;

  const double samplesPerPixel =
      static_cast<double>(audioData.sampleRate) / pixelsPerSecond;
  const double waveformStartX = -scrollX;
  const double waveformEndX =
      static_cast<double>(numSamples) / samplesPerPixel - scrollX;
  const int firstPixel = juce::jlimit(
      0, visibleWidth, static_cast<int>(std::floor(waveformStartX)));
  const int lastPixel = juce::jlimit(
      0, visibleWidth, static_cast<int>(std::ceil(waveformEndX)));

  if (lastPixel > firstPixel)
  {
    // Map only the portion of the viewport covered by real waveform samples.
    // Clamping the sample range and then drawing it across visibleWidth would
    // stretch a short waveform to the right edge of the viewport.
    const int startSample = juce::jlimit(
        0, numSamples - 1,
        static_cast<int>(std::floor((scrollX + firstPixel) * samplesPerPixel)));
    const int endSample = juce::jlimit(
        startSample + 1, numSamples,
        static_cast<int>(std::ceil((scrollX + lastPixel) * samplesPerPixel)));
    const int pointCount = lastPixel - firstPixel;

    const auto displayEnvelope = VisualWaveformEnvelope::build(
        samples, numSamples, startSample, endSample, pointCount,
        static_cast<float>(pointCount), audioData.sampleRate, pixelsPerSecond);

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
  }

  cachedScrollX = scrollX;
  cachedPixelsPerSecond = pixelsPerSecond;
  cachedWidth = visibleArea.getWidth();
  cachedHeight = visibleArea.getHeight();

  g.drawImageAt(waveformCache, visibleArea.getX(), visibleArea.getY());
}
