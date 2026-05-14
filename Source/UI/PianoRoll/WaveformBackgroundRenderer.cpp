#include "WaveformBackgroundRenderer.h"
#include "../../Utils/Constants.h"
#include "../../Utils/UI/Theme.h"

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

  waveformPath.startNewSubPath(0.0f, centerY);

  // Top half
  for (int px = 0; px < visibleWidth; ++px)
  {
    const double time = (scrollX + px) / pixelsPerSecond;
    int startSample = static_cast<int>(time * SAMPLE_RATE);
    int endSample =
        static_cast<int>((time + 1.0 / pixelsPerSecond) * SAMPLE_RATE);

    startSample = std::max(0, std::min(startSample, numSamples - 1));
    endSample = std::max(startSample + 1, std::min(endSample, numSamples));

    float maxVal = 0.0f;
    for (int i = startSample; i < endSample; ++i)
      maxVal = std::max(maxVal, std::abs(samples[i]));

    const float y = centerY - maxVal * waveformHeight * 0.5f;
    waveformPath.lineTo(static_cast<float>(px), y);
  }

  // Bottom half (reverse)
  for (int px = visibleWidth - 1; px >= 0; --px)
  {
    const double time = (scrollX + px) / pixelsPerSecond;
    int startSample = static_cast<int>(time * SAMPLE_RATE);
    int endSample =
        static_cast<int>((time + 1.0 / pixelsPerSecond) * SAMPLE_RATE);

    startSample = std::max(0, std::min(startSample, numSamples - 1));
    endSample = std::max(startSample + 1, std::min(endSample, numSamples));

    float maxVal = 0.0f;
    for (int i = startSample; i < endSample; ++i)
      maxVal = std::max(maxVal, std::abs(samples[i]));

    const float y = centerY + maxVal * waveformHeight * 0.5f;
    waveformPath.lineTo(static_cast<float>(px), y);
  }

  waveformPath.closeSubPath();

  cacheGraphics.setColour(APP_COLOR_WAVEFORM);
  cacheGraphics.fillPath(waveformPath);

  cachedScrollX = scrollX;
  cachedPixelsPerSecond = pixelsPerSecond;
  cachedWidth = visibleArea.getWidth();
  cachedHeight = visibleArea.getHeight();

  g.drawImageAt(waveformCache, visibleArea.getX(), visibleArea.getY());
}
