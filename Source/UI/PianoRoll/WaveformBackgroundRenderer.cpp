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

  int startSample = static_cast<int>(std::floor(
      scrollX * static_cast<double>(audioData.sampleRate) / pixelsPerSecond));
  int endSample = static_cast<int>(std::ceil(
      (scrollX + visibleWidth) * static_cast<double>(audioData.sampleRate) /
      pixelsPerSecond));

  startSample = std::max(0, std::min(startSample, numSamples - 1));
  endSample = std::max(startSample + 1, std::min(endSample, numSamples));

  const auto displayEnvelope = VisualWaveformEnvelope::build(
      samples, numSamples, startSample, endSample, visibleWidth,
      static_cast<float>(visibleWidth), audioData.sampleRate, pixelsPerSecond);

  waveformPath.startNewSubPath(0.0f, centerY);

  // Top half
  for (int px = 0; px < visibleWidth; ++px)
  {
    const float y =
        centerY -
        displayEnvelope[static_cast<size_t>(px)] * waveformHeight * 0.5f;
    waveformPath.lineTo(static_cast<float>(px), y);
  }

  // Bottom half (reverse)
  for (int px = visibleWidth - 1; px >= 0; --px)
  {
    const float y =
        centerY +
        displayEnvelope[static_cast<size_t>(px)] * waveformHeight * 0.5f;
    waveformPath.lineTo(static_cast<float>(px), y);
  }

  waveformPath.closeSubPath();

  cacheGraphics.setColour(juce::Colours::white.withAlpha(0.05f));
  cacheGraphics.fillPath(waveformPath);

  cachedScrollX = scrollX;
  cachedPixelsPerSecond = pixelsPerSecond;
  cachedWidth = visibleArea.getWidth();
  cachedHeight = visibleArea.getHeight();

  g.drawImageAt(waveformCache, visibleArea.getX(), visibleArea.getY());
}
