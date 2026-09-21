#include "WaveformBackgroundRenderer.h"
#include "VisualWaveformEnvelope.h"
#include "PitchToolController.h"
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

void WaveformBackgroundRenderer::timerCallback()
{
  stopTimer();
  zoomSettled = true;
  if (onRepaintRequested)
    onRepaintRequested();
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

  const int viewWidth = visibleArea.getWidth();
  const int viewHeight = visibleArea.getHeight();
  if (viewWidth <= 0 || viewHeight <= 0)
    return;

  // PianoRollComponent places scrolled content at an integer origin
  // (static_cast<int>(scrollX)); use the same here so the waveform stays
  // aligned with the grid and notes.
  const int viewLeft = static_cast<int>(coordMapper->getScrollX());
  const int viewRight = viewLeft + viewWidth;
  const float pixelsPerSecond = coordMapper->getPixelsPerSecond();

  const bool amplitudePreview = pitchToolController && pitchToolController->isDragging() &&
      pitchToolController->getActiveHandleType() == PitchToolHandles::HandleType::Amplitude;

  const bool cacheUsable = waveformCache.isValid() && !cacheDirty &&
                           cachedAmplitudePreview == amplitudePreview &&
                           cachedHeight == viewHeight;
  const int cachedImageWidth = waveformCache.getWidth();

  // Fast path: same zoom and the viewport is inside the cached strip. Scrolling
  // only changes which slice of the strip is blitted.
  if (cacheUsable &&
      std::abs(cachedPixelsPerSecond - pixelsPerSecond) < 0.01f &&
      viewLeft >= cachedStripStartX &&
      viewRight <= cachedStripStartX + cachedImageWidth)
  {
    g.drawImageAt(waveformCache,
                  visibleArea.getX() + cachedStripStartX - viewLeft,
                  visibleArea.getY());
    return;
  }

  // Zoom in progress: stretch the stale strip to the new zoom instead of
  // rebuilding the envelope for every wheel event, then rebuild once the zoom
  // has stopped changing (timerCallback).
  if (cacheUsable && !zoomSettled && cachedPixelsPerSecond > 0.0f)
  {
    const double scale =
        static_cast<double>(pixelsPerSecond) / cachedPixelsPerSecond;
    const int stretchedStart =
        juce::roundToInt(static_cast<double>(cachedStripStartX) * scale);
    const int stretchedWidth =
        juce::jmax(1, juce::roundToInt(static_cast<double>(cachedImageWidth) * scale));
    if (scale >= 0.25 && scale <= 4.0 && viewLeft >= stretchedStart &&
        viewRight <= stretchedStart + stretchedWidth)
    {
      g.drawImage(waveformCache,
                  visibleArea.getX() + stretchedStart - viewLeft,
                  visibleArea.getY(), stretchedWidth, viewHeight, 0, 0,
                  cachedImageWidth, waveformCache.getHeight());
      startTimer(zoomSettleMs);
      return;
    }
  }

  // Rebuild. When the content itself changed (live recording, amplitude drag,
  // new project) keep the strip viewport-sized so per-frame rebuilds cost what
  // they used to; otherwise add half a viewport of margin on each side so the
  // next scrolls and zoom-outs are pure blits.
  int stripWidth = viewWidth;
  if (!cacheDirty)
    stripWidth = juce::jmax(viewWidth, juce::jmin(viewWidth * 2, 8192));
  const int stripStart = viewLeft - (stripWidth - viewWidth) / 2;

  waveformCache = juce::Image(juce::Image::ARGB, stripWidth, viewHeight, true);
  juce::Graphics cacheGraphics(waveformCache);

  const float visibleHeight = static_cast<float>(visibleArea.getHeight());
  const float centerY = visibleHeight * 0.5f;
  const float waveformHeight = visibleHeight * 0.8f;

  // The strip is built in world pixels: pixel 0 of the image is world x =
  // stripStart, so the lambda below treats stripStart as its "scrollX".
  const int visibleWidth = stripWidth;
  const double scrollX = static_cast<double>(stripStart);

  auto drawWaveform = [&](const juce::AudioBuffer<float> &source,
                          int numSamples, double sampleRate,
                          double timelineOffset, bool previewGain) {
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

    std::vector<VisualWaveformEnvelope::GainRegion> gainRegions;
    if (previewGain && amplitudePreview && project)
      for (const auto* note : pitchToolController->getAffectedNotes())
      {
        if (!note || note->isRest())
          continue;
        gainRegions.push_back({
            static_cast<int>(framesToSeconds(note->getStartFrame()) * sampleRate),
            static_cast<int>(framesToSeconds(note->getEndFrame()) * sampleRate),
            pitchToolController->getAmplitudePreviewGain(*note)});
      }

    const auto displayEnvelope = VisualWaveformEnvelope::build(
        source.getReadPointer(0), numSamples, startSample, endSample, pointCount,
        static_cast<float>(pointCount), sampleRate, pixelsPerSecond, true, 1.0f, gainRegions);

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
                 projectAudio->sampleRate, 0.0, true);
  if (drawingLive)
    drawWaveform(liveWaveform, liveNumSamples, liveSampleRate,
                 liveTimelineOffsetSeconds, false);

  cachedAmplitudePreview = amplitudePreview;
  cachedStripStartX = stripStart;
  cachedPixelsPerSecond = pixelsPerSecond;
  cachedHeight = viewHeight;
  cacheDirty = false;
  zoomSettled = false;
  stopTimer();

  g.drawImageAt(waveformCache, visibleArea.getX() + stripStart - viewLeft,
                visibleArea.getY());
}
