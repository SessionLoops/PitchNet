#include "OverviewPanel.h"
#include "VisualWaveformEnvelope.h"
#include "../../Utils/ScaleUtils.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
const juce::Colour thumbnailBackground(0xFF191717u);

struct NoteGradientColours {
  juce::Colour centre;
  juce::Colour side;
};

float getPitchCenterAmount(float midi, int pitchReferenceHz) {
  const float pitchCenter =
      ScaleUtils::snapMidiToSemitone(midi, pitchReferenceHz);
  const float distanceFromCenter = std::abs(midi - pitchCenter);

  return juce::jlimit(0.0f, 1.0f, distanceFromCenter / 0.5f);
}

juce::Colour interpolateLayeredColour(juce::Colour first,
                                      juce::Colour second,
                                      juce::Colour third,
                                      juce::Colour fourth,
                                      float amount) {
  if (amount < 1.0f / 3.0f)
    return first.interpolatedWith(second, amount * 3.0f);

  if (amount < 2.0f / 3.0f)
    return second.interpolatedWith(third, amount * 3.0f - 1.0f);

  return third.interpolatedWith(fourth, amount * 3.0f - 2.0f);
}

NoteGradientColours getNoteGradientColours(float midi, int pitchReferenceHz) {
  static const juce::Colour inTuneCentre(0xFF1983E0u);
  static const juce::Colour inTuneSide(0xFF0021E2u);
  static const juce::Colour firstLayerCentre(0xFFD66CFFu);
  static const juce::Colour firstLayerSide(0xFF971CFFu);
  static const juce::Colour secondLayerCentre(0xFFFF90EDu);
  static const juce::Colour secondLayerSide(0xFFF624B7u);
  static const juce::Colour outOfTuneCentre(0xFFF95D5Du);
  static const juce::Colour outOfTuneSide(0xFFFF0000u);

  const float amount = getPitchCenterAmount(midi, pitchReferenceHz);

  return {
      interpolateLayeredColour(inTuneCentre, firstLayerCentre,
                               secondLayerCentre, outOfTuneCentre, amount),
      interpolateLayeredColour(inTuneSide, firstLayerSide, secondLayerSide,
                               outOfTuneSide, amount)};
}

float noteRangeY(float midi, float minPitch, float maxPitch,
                 const juce::Rectangle<float> &content) {
  midi = juce::jlimit(minPitch, maxPitch, midi);
  const float pitchRange = std::max(1.0f, maxPitch - minPitch);
  return content.getY() + (maxPitch - midi) / pitchRange * content.getHeight();
}

} // namespace

void OverviewPanel::paint(juce::Graphics &g) {
  const auto localBounds = getLocalBounds();
  if (localBounds.isEmpty())
    return;

  if (staticCache.isNull() || staticCache.getWidth() != getWidth() ||
      staticCache.getHeight() != getHeight() || cacheDirty) {
    staticCache = juce::Image(juce::Image::ARGB, getWidth(), getHeight(), true);
    juce::Graphics cacheGraphics(staticCache);
    paintStaticContent(cacheGraphics);
    cacheDirty = false;
  }

  g.drawImageAt(staticCache, 0, 0);
  paintViewport(g);
  paintPlayhead(g);
}

void OverviewPanel::resized() {
  cacheDirty = true;
}

void OverviewPanel::repaintPlayhead(double previousTime, double newTime) {
  auto dirty = getPlayheadRepaintBounds(previousTime)
                   .getUnion(getPlayheadRepaintBounds(newTime));
  if (!dirty.isEmpty())
    repaint(dirty);
}

void OverviewPanel::paintStaticContent(juce::Graphics &g) {
  auto content = getContentBounds();
  const float cornerRadius = 6.0f;

  if (drawBackground) {
    g.setColour(thumbnailBackground);
    g.fillRoundedRectangle(content, cornerRadius);

    g.setColour(APP_COLOR_BORDER_SUBTLE.withAlpha(0.6f));
    g.drawRoundedRectangle(content, cornerRadius, 1.0f);
  } else {
    g.setColour(thumbnailBackground);
    g.fillRoundedRectangle(content, cornerRadius);
  }

  const int width = static_cast<int>(content.getWidth());
  const int height = static_cast<int>(content.getHeight());
  if (width <= 0 || height <= 0)
    return;

  const bool hasPreviews = regionPreviews && !regionPreviews->empty();
  const auto *audioData = project ? &project->getAudioData() : nullptr;
  const int numSamples =
      audioData ? audioData->waveform.getNumSamples() : 0;
  const bool hasProjectAudio =
      audioData != nullptr && numSamples > 0 && audioData->sampleRate > 0;
  if (!hasProjectAudio && !hasPreviews)
    return;

  const double audioDuration =
      hasProjectAudio
          ? static_cast<double>(numSamples) / audioData->sampleRate
          : 0.0;
  const auto state = getViewState ? getViewState() : ViewState{};
  // Track mode spans the song (state.totalTime); the take may be longer than
  // what is shown, so its duration does not stretch the thumbnail.
  const double timelineDuration =
      state.trackMode ? state.totalTime
                      : std::max(audioDuration, state.totalTime);
  if (timelineDuration <= 0.0)
    return;

  // Project seconds -> x. Region previews are host seconds; hostToProject
  // brings them onto the same axis.
  const auto timeToOverviewX = [&](double seconds) {
    return content.getX() +
           static_cast<float>(((seconds - state.viewStartSeconds) /
                               timelineDuration) *
                              content.getWidth());
  };
  const auto hostToProject = [&](double hostSeconds) {
    return hostSeconds - state.displayOffset;
  };
  // In Track mode only the active region's window of the take is shown.
  const bool clipToSpan = state.trackMode && state.hasActiveSpan;
  const auto inActiveSpan = [&](double startSeconds, double endSeconds) {
    return !clipToSpan || (endSeconds > state.activeSpanStart &&
                           startSeconds < state.activeSpanEnd);
  };

  const float centerY = content.getY() + content.getHeight() * 0.5f;
  const float waveformHeight = content.getHeight() * 0.8f;

  // Region backdrops: only the active region carries the backdrop colour; the
  // other regions of the track are outlined by their boundaries.
  if (hasPreviews) {
    for (const auto &region : *regionPreviews) {
      if (region.endSeconds <= region.startSeconds)
        continue;
      const float x1 = timeToOverviewX(hostToProject(region.startSeconds));
      const float x2 = timeToOverviewX(hostToProject(region.endSeconds));
      if (region.active) {
        g.setColour(juce::Colours::white.withAlpha(0.06f));
        g.fillRect(juce::Rectangle<float>(x1, content.getY(),
                                          std::max(1.0f, x2 - x1),
                                          content.getHeight()));
        g.setColour(juce::Colours::white.withAlpha(0.25f));
      } else {
        g.setColour(juce::Colours::white.withAlpha(0.12f));
      }
      g.fillRect(x1 - 0.5f, content.getY(), 1.0f, content.getHeight());
      g.fillRect(x2 - 0.5f, content.getY(), 1.0f, content.getHeight());
    }

  }

  // Every waveform, active or not, uses the same envelope, colour and one
  // shared scale, so the regions read as one continuous track.
  struct OverviewWaveform {
    int startPixel = 0;
    std::vector<float> envelope;
  };
  std::vector<OverviewWaveform> waveforms;
  const float overviewPixelsPerSecond =
      static_cast<float>(content.getWidth() / timelineDuration);
  const auto addWaveform = [&](const float *samples, int count,
                               double sampleRate, double startSeconds) {
    if (samples == nullptr || count <= 0 || sampleRate <= 0.0)
      return;
    const int startPixel =
        static_cast<int>(std::floor(timeToOverviewX(startSeconds)));
    const double durationSeconds = static_cast<double>(count) / sampleRate;
    const int width = std::max(
        1, static_cast<int>(std::ceil(content.getWidth() * durationSeconds /
                                      timelineDuration)));
    waveforms.push_back(
        {startPixel, VisualWaveformEnvelope::build(
                         samples, count, 0, count, width,
                         static_cast<float>(width), sampleRate,
                         overviewPixelsPerSecond, false)});
  };

  if (hasPreviews)
    for (const auto &region : *regionPreviews)
      if (!region.active && region.waveformSamples)
        addWaveform(region.waveformSamples->data(),
                    static_cast<int>(region.waveformSamples->size()),
                    region.waveformSampleRate,
                    hostToProject(region.waveformStartSeconds));
  if (hasProjectAudio && audioData->waveform.getNumChannels() > 0) {
    int first = 0;
    int last = numSamples;
    if (clipToSpan) {
      const double rate = static_cast<double>(audioData->sampleRate);
      first = juce::jlimit(0, numSamples,
                           static_cast<int>(state.activeSpanStart * rate));
      last = juce::jlimit(first, numSamples,
                          static_cast<int>(state.activeSpanEnd * rate));
    }
    addWaveform(audioData->waveform.getReadPointer(0) + first, last - first,
                audioData->sampleRate,
                static_cast<double>(first) / audioData->sampleRate);
  }

  float maxEnvelope = 0.0f;
  for (const auto &waveform : waveforms)
    for (const float value : waveform.envelope)
      maxEnvelope = std::max(maxEnvelope, value);
  const float envelopeScale = maxEnvelope > 0.0f ? 1.0f / maxEnvelope : 1.0f;

  g.setColour(juce::Colour(0xFF484546u));
  for (const auto &waveform : waveforms) {
    const int count = static_cast<int>(waveform.envelope.size());
    juce::Path waveformPath;
    waveformPath.startNewSubPath(static_cast<float>(waveform.startPixel),
                                 centerY);
    for (int px = 0; px < count; ++px)
      waveformPath.lineTo(
          static_cast<float>(waveform.startPixel + px),
          centerY - waveform.envelope[static_cast<size_t>(px)] *
                        envelopeScale * waveformHeight * 0.5f);
    for (int px = count - 1; px >= 0; --px)
      waveformPath.lineTo(
          static_cast<float>(waveform.startPixel + px),
          centerY + waveform.envelope[static_cast<size_t>(px)] *
                        envelopeScale * waveformHeight * 0.5f);
    waveformPath.closeSubPath();
    g.fillPath(waveformPath);
  }

  if (hasProjectAudio) {

    if (showSegmentsDebug) {
      g.setColour(juce::Colours::orange.withAlpha(0.16f));
      for (const auto &range : audioData->segmentChunkRanges) {
        const int startFrame = std::max(0, range.first);
        const int endFrame = std::max(startFrame, range.second);
        if (endFrame <= startFrame)
          continue;

        const double startTime =
            static_cast<double>(startFrame) * HOP_SIZE / SAMPLE_RATE;
        const double endTime =
            static_cast<double>(endFrame) * HOP_SIZE / SAMPLE_RATE;
        const float x1 = timeToOverviewX(startTime);
        const float x2 = timeToOverviewX(endTime);
        g.fillRect(juce::Rectangle<float>(x1, content.getY(),
                                          std::max(1.0f, x2 - x1),
                                          content.getHeight()));
      }

      g.setColour(juce::Colours::orange.withAlpha(0.75f));
      for (const auto &range : audioData->segmentChunkRanges) {
        const int startFrame = std::max(0, range.first);
        const int endFrame = std::max(startFrame, range.second);
        if (endFrame <= startFrame)
          continue;
        const double startTime =
            static_cast<double>(startFrame) * HOP_SIZE / SAMPLE_RATE;
        const float x = timeToOverviewX(startTime);
        g.drawLine(x, content.getY(), x, content.getBottom(), 1.0f);
      }
    }
  }

  // One pitch range across every region so notes line up between them.
  float minPitch = std::numeric_limits<float>::max();
  float maxPitch = std::numeric_limits<float>::lowest();
  bool hasNotePitch = false;

  if (project != nullptr) {
    for (const auto &note : project->getNotes()) {
      if (note.isRest())
        continue;
      if (!inActiveSpan(framesToSeconds(note.getVisualStartFrame()),
                        framesToSeconds(note.getVisualEndFrame())))
        continue;

      const float midi = note.getAdjustedMidiNote();
      minPitch = std::min(minPitch, midi);
      maxPitch = std::max(maxPitch, midi);
      hasNotePitch = true;

      for (const float delta : note.getActiveDeltaPitch()) {
        minPitch = std::min(minPitch, midi + delta);
        maxPitch = std::max(maxPitch, midi + delta);
      }
    }
  }
  if (hasPreviews) {
    for (const auto &region : *regionPreviews) {
      if (region.active)
        continue;
      for (const auto &note : region.notes) {
        minPitch = std::min(minPitch, note.midi);
        maxPitch = std::max(maxPitch, note.midi);
        hasNotePitch = true;
      }
    }
  }

  if (!hasNotePitch)
    return;

  minPitch -= 1.0f;
  maxPitch += 1.0f;
  if (maxPitch - minPitch < 2.0f) {
    const float centerPitch = (minPitch + maxPitch) * 0.5f;
    minPitch = centerPitch - 1.0f;
    maxPitch = centerPitch + 1.0f;
  }

  const float pitchRange = std::max(1.0f, maxPitch - minPitch);
  const float noteHeight =
      juce::jlimit(1.0f, 5.0f, content.getHeight() / pitchRange);
  const float thickness = juce::jlimit(1.0f, 3.0f, noteHeight);
  const int pitchReferenceHz =
      project != nullptr ? project->getPitchReferenceHz() : 440;

  if (hasPreviews) {
    for (const auto &region : *regionPreviews) {
      if (region.active)
        continue;
      for (const auto &note : region.notes) {
        if (note.endSeconds <= note.startSeconds)
          continue;
        const auto noteColour =
            getNoteGradientColours(note.midi, pitchReferenceHz).side;
        const float x1 = timeToOverviewX(hostToProject(note.startSeconds));
        const float x2 = timeToOverviewX(hostToProject(note.endSeconds));
        const float y = noteRangeY(note.midi, minPitch, maxPitch, content);
        // Same as the active region's notes, but grey.
        g.setColour(noteColour.withSaturation(0.0f).withAlpha(0.9f));
        g.drawLine(x1, y, x2, y, thickness);
      }
    }
  }

  if (project != nullptr) {
    for (const auto &note : project->getNotes()) {
      if (note.isRest())
        continue;

      const double startTime =
          static_cast<double>(note.getVisualStartFrame()) * HOP_SIZE /
          SAMPLE_RATE;
      const double endTime =
          static_cast<double>(note.getVisualEndFrame()) * HOP_SIZE /
          SAMPLE_RATE;

      if (endTime <= startTime || !inActiveSpan(startTime, endTime))
        continue;

      const float midi = note.getAdjustedMidiNote();
      const auto noteColour =
          getNoteGradientColours(midi, pitchReferenceHz).side;

      const float x1 = timeToOverviewX(startTime);
      const float x2 = timeToOverviewX(endTime);
      const float y = noteRangeY(midi, minPitch, maxPitch, content);
      g.setColour(noteColour.withAlpha(0.9f));
      g.drawLine(x1, y, x2, y, thickness);
    }
  }
}

void OverviewPanel::paintViewport(juce::Graphics &g) {
  auto viewport = computeViewport();
  if (viewport.valid) {
    const juce::Colour selectionBoxColour(0xFFEFEFEFu);

    g.setColour(selectionBoxColour.withAlpha(0.16f));
    g.fillRoundedRectangle(viewport.rect, 4.0f);

    g.setColour(selectionBoxColour.withAlpha(0.9f));
    g.drawRoundedRectangle(viewport.rect, 4.0f, 1.0f);

    const float handleWidth = 2.0f;
    const float handleInset = 3.0f;
    const float handleHeight = viewport.rect.getHeight() - handleInset * 2.0f;

    g.fillRect(viewport.rect.getX() + handleInset,
               viewport.rect.getY() + handleInset, handleWidth, handleHeight);
    g.fillRect(viewport.rect.getRight() - handleInset - handleWidth,
               viewport.rect.getY() + handleInset, handleWidth, handleHeight);
  }
}

void OverviewPanel::paintPlayhead(juce::Graphics &g) {
  if (!project)
    return;

  const auto &audioData = project->getAudioData();
  const int numSamples = audioData.waveform.getNumSamples();
  if (numSamples <= 0 || audioData.sampleRate <= 0)
    return;

  auto state = getViewState ? getViewState() : ViewState{};
  const double audioDuration =
      static_cast<double>(numSamples) / audioData.sampleRate;
  const double timelineDuration =
      state.trackMode ? state.totalTime
                      : std::max(audioDuration, state.totalTime);
  if (timelineDuration > 0.0 && state.cursorTime >= 0.0 &&
      state.cursorTime <= timelineDuration) {
    auto content = getContentBounds();
    const float playheadX =
        content.getX() +
        static_cast<float>((state.cursorTime / timelineDuration) *
                           content.getWidth());

    g.setColour(juce::Colour(0xFFC8C7C7u));
    g.fillRect(playheadX - 0.5f, content.getY(), 1.0f, content.getHeight());
  }
}

void OverviewPanel::mouseDown(const juce::MouseEvent &e) {
  auto viewport = computeViewport();
  if (!viewport.valid)
    return;

  const float x = static_cast<float>(e.x);
  dragStartX = x;
  dragStartStartTime = viewport.startTime;
  dragStartEndTime = viewport.endTime;
  dragStartVisibleTime = viewport.endTime - viewport.startTime;

  const float leftEdge = viewport.rect.getX();
  const float rightEdge = viewport.rect.getRight();

  if (std::abs(x - leftEdge) <= handleHitWidth) {
    dragMode = DragMode::ResizeLeft;
  } else if (std::abs(x - rightEdge) <= handleHitWidth) {
    dragMode = DragMode::ResizeRight;
  } else if (viewport.rect.contains(static_cast<float>(e.x),
                                    static_cast<float>(e.y))) {
    dragMode = DragMode::Move;
  } else {
    dragMode = DragMode::None;
  }

  if (dragMode == DragMode::None) {
    auto state = getViewState ? getViewState() : ViewState{};
    if (state.totalTime <= 0.0 || state.pixelsPerSecond <= 0.0f ||
        state.visibleWidth <= 0)
      return;

    const auto content = getContentBounds();
    if (content.getWidth() <= 0.0f)
      return;
    double visibleTime =
        static_cast<double>(state.visibleWidth) / state.pixelsPerSecond;
    visibleTime = std::min(visibleTime, state.totalTime);

    double clickTime = timeForX(static_cast<float>(e.x), content);
    double newStart =
        juce::jlimit(0.0, std::max(0.0, state.totalTime - visibleTime),
                     clickTime - visibleTime * 0.5);
    if (onScrollXChanged)
      onScrollXChanged(newStart * state.pixelsPerSecond);
  }

  updateCursor(dragMode);
}

void OverviewPanel::mouseDrag(const juce::MouseEvent &e) {
  if (dragMode == DragMode::None)
    return;

  auto state = getViewState ? getViewState() : ViewState{};
  if (state.totalTime <= 0.0 || state.pixelsPerSecond <= 0.0f ||
      state.visibleWidth <= 0)
    return;

  auto content = getContentBounds();
  if (content.getWidth() <= 0.0f)
    return;
  const double minVisibleTime =
      static_cast<double>(state.visibleWidth) / MAX_PIXELS_PER_SECOND;
  const double maxVisibleTime =
      static_cast<double>(state.visibleWidth) / MIN_PIXELS_PER_SECOND;

  double visibleTime = dragStartVisibleTime;
  double startTime = dragStartStartTime;
  double endTime = dragStartEndTime;

  if (dragMode == DragMode::Move) {
    const double deltaTime =
        (static_cast<float>(e.x) - dragStartX) / content.getWidth() *
        state.totalTime;
    visibleTime = dragStartVisibleTime;
    startTime = dragStartStartTime + deltaTime;
    startTime =
        juce::jlimit(0.0, std::max(0.0, state.totalTime - visibleTime),
                     startTime);
    if (onScrollXChanged)
      onScrollXChanged(startTime * state.pixelsPerSecond);
    repaint();
    return;
  }

  if (dragMode == DragMode::ResizeLeft) {
    startTime = timeForX(static_cast<float>(e.x), content);
    startTime = juce::jlimit(0.0, dragStartEndTime - minVisibleTime, startTime);
    visibleTime = dragStartEndTime - startTime;
    visibleTime = juce::jlimit(minVisibleTime, maxVisibleTime, visibleTime);
    startTime = dragStartEndTime - visibleTime;
  } else if (dragMode == DragMode::ResizeRight) {
    endTime = timeForX(static_cast<float>(e.x), content);
    endTime = juce::jlimit(dragStartStartTime + minVisibleTime, state.totalTime,
                           endTime);
    visibleTime = endTime - dragStartStartTime;
    visibleTime = juce::jlimit(minVisibleTime, maxVisibleTime, visibleTime);
    endTime = dragStartStartTime + visibleTime;
    startTime = dragStartStartTime;
  }

  const float newPps =
      juce::jlimit(MIN_PIXELS_PER_SECOND, MAX_PIXELS_PER_SECOND,
                   static_cast<float>(state.visibleWidth / visibleTime));
  if (onZoomChanged)
    onZoomChanged(newPps);
  if (onScrollXChanged)
    onScrollXChanged(startTime * newPps);

  repaint();
}

void OverviewPanel::mouseUp(const juce::MouseEvent &) {
  dragMode = DragMode::None;
  updateCursor(dragMode);
}

void OverviewPanel::mouseMove(const juce::MouseEvent &e) {
  auto viewport = computeViewport();
  if (!viewport.valid) {
    updateCursor(DragMode::None);
    return;
  }

  const float x = static_cast<float>(e.x);
  const float leftEdge = viewport.rect.getX();
  const float rightEdge = viewport.rect.getRight();

  if (std::abs(x - leftEdge) <= handleHitWidth)
    updateCursor(DragMode::ResizeLeft);
  else if (std::abs(x - rightEdge) <= handleHitWidth)
    updateCursor(DragMode::ResizeRight);
  else if (viewport.rect.contains(static_cast<float>(e.x),
                                  static_cast<float>(e.y)))
    updateCursor(DragMode::Move);
  else
    updateCursor(DragMode::None);
}

void OverviewPanel::mouseExit(const juce::MouseEvent &) {
  updateCursor(DragMode::None);
}

OverviewPanel::ViewportInfo OverviewPanel::computeViewport() const {
  auto state = getViewState ? getViewState() : ViewState{};
  ViewportInfo info;

  if (state.totalTime <= 0.0 || state.pixelsPerSecond <= 0.0f ||
      state.visibleWidth <= 0)
    return info;

  auto content = getContentBounds();
  if (content.getWidth() <= 0.0f)
    return info;

  double visibleTime =
      static_cast<double>(state.visibleWidth) / state.pixelsPerSecond;
  visibleTime = std::max(0.0, visibleTime);

  if (visibleTime >= state.totalTime) {
    visibleTime = state.totalTime;
  }

  double startTime =
      juce::jlimit(0.0, std::max(0.0, state.totalTime - visibleTime),
                   state.scrollX / state.pixelsPerSecond);
  double endTime = startTime + visibleTime;

  float startX =
      content.getX() +
      static_cast<float>((startTime / state.totalTime) * content.getWidth());
  float endX =
      content.getX() +
      static_cast<float>((endTime / state.totalTime) * content.getWidth());

  if (endX - startX < minViewportPixels) {
    float centerX = (startX + endX) * 0.5f;
    startX = centerX - minViewportPixels * 0.5f;
    endX = centerX + minViewportPixels * 0.5f;
    startX = std::max(content.getX(), startX);
    endX = std::min(content.getRight(), endX);
  }

  info.valid = true;
  info.totalTime = state.totalTime;
  info.startTime = startTime;
  info.endTime = endTime;
  info.startX = startX;
  info.endX = endX;
  info.rect = juce::Rectangle<float>(startX, content.getY(),
                                     endX - startX, content.getHeight());
  return info;
}

double OverviewPanel::timeForX(float x,
                               const juce::Rectangle<float> &content) const {
  auto state = getViewState ? getViewState() : ViewState{};
  if (state.totalTime <= 0.0)
    return 0.0;

  float t = (x - content.getX()) / content.getWidth();
  t = juce::jlimit(0.0f, 1.0f, t);
  return static_cast<double>(t) * state.totalTime;
}

juce::Rectangle<float> OverviewPanel::getContentBounds() const {
  auto bounds = getLocalBounds().toFloat();
  bounds.reduce(static_cast<float>(padding), static_cast<float>(padding));
  return bounds;
}

juce::Rectangle<int> OverviewPanel::getPlayheadRepaintBounds(double time) const {
  if (!project)
    return {};

  const auto &audioData = project->getAudioData();
  const int numSamples = audioData.waveform.getNumSamples();
  if (numSamples <= 0 || audioData.sampleRate <= 0)
    return {};

  const double audioDuration =
      static_cast<double>(numSamples) / audioData.sampleRate;
  const auto state = getViewState ? getViewState() : ViewState{};
  const double timelineDuration =
      state.trackMode ? state.totalTime
                      : std::max(audioDuration, state.totalTime);
  if (timelineDuration <= 0.0 || time < 0.0 || time > timelineDuration)
    return {};

  const auto content = getContentBounds();
  if (content.isEmpty())
    return {};

  const float x =
      content.getX() +
      static_cast<float>((time / timelineDuration) * content.getWidth());
  return juce::Rectangle<float>(x - 2.0f, content.getY(), 4.0f,
                                content.getHeight())
      .getSmallestIntegerContainer()
      .expanded(1, 1)
      .getIntersection(getLocalBounds());
}

void OverviewPanel::updateCursor(DragMode mode) {
  if (mode == DragMode::ResizeLeft || mode == DragMode::ResizeRight) {
    setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
  } else if (mode == DragMode::Move) {
    setMouseCursor(juce::MouseCursor::DraggingHandCursor);
  } else {
    setMouseCursor(juce::MouseCursor::NormalCursor);
  }
}
