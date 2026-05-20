#include "OverviewPanel.h"
#include <algorithm>
#include <cmath>

namespace {
juce::Colour getPitchColour(float midi) {
  const float norm = juce::jlimit(
      0.0f, 1.0f,
      (midi - static_cast<float>(MIN_MIDI_NOTE)) /
          static_cast<float>(MAX_MIDI_NOTE - MIN_MIDI_NOTE));
  return APP_COLOR_PRIMARY.interpolatedWith(APP_COLOR_PITCH_CURVE, norm);
}

float f0ToMidi(float f0) {
  if (f0 <= 0.0f)
    return 0.0f;
  return 69.0f + 12.0f * std::log2(f0 / 440.0f);
}
} // namespace

void OverviewPanel::paint(juce::Graphics &g) {
  auto content = getContentBounds();
  const float cornerRadius = 6.0f;

  if (drawBackground) {
    g.setColour(APP_COLOR_SURFACE_ALT);
    g.fillRoundedRectangle(content, cornerRadius);

    g.setColour(APP_COLOR_BORDER_SUBTLE.withAlpha(0.6f));
    g.drawRoundedRectangle(content, cornerRadius, 1.0f);
  }

  if (!project)
    return;

  const auto &audioData = project->getAudioData();
  const int numSamples = audioData.waveform.getNumSamples();
  if (numSamples <= 0 || audioData.sampleRate <= 0)
    return;

  const float *samples = audioData.waveform.getReadPointer(0);
  const int width = static_cast<int>(content.getWidth());
  const int height = static_cast<int>(content.getHeight());

  if (width <= 0 || height <= 0)
    return;

  const double totalTime = static_cast<double>(numSamples) / audioData.sampleRate;

  const float centerY = content.getY() + content.getHeight() * 0.5f;
  const float amplitude = content.getHeight() * 0.58f;

  g.setColour(APP_COLOR_WAVEFORM.brighter(0.2f).withAlpha(0.9f));
  for (int px = 0; px < width; ++px) {
    const double t0 = static_cast<double>(px) / width;
    const double t1 = static_cast<double>(px + 1) / width;
    int startSample = static_cast<int>(t0 * numSamples);
    int endSample = static_cast<int>(t1 * numSamples);

    startSample = juce::jlimit(0, numSamples - 1, startSample);
    endSample = juce::jlimit(startSample + 1, numSamples, endSample);

    float maxVal = 0.0f;
    for (int i = startSample; i < endSample; ++i) {
      maxVal = std::max(maxVal, std::abs(samples[i]));
    }
    maxVal = std::sqrt(maxVal);

    float x = content.getX() + static_cast<float>(px);
    float y1 = centerY - maxVal * amplitude;
    float y2 = centerY + maxVal * amplitude;
    g.drawLine(x, y1, x, y2);
  }

  if (showSegmentsDebug && totalTime > 0.0) {
    g.setColour(juce::Colours::orange.withAlpha(0.16f));
    for (const auto &range : audioData.segmentChunkRanges) {
      const int startFrame = std::max(0, range.first);
      const int endFrame = std::max(startFrame, range.second);
      if (endFrame <= startFrame)
        continue;

      const double startTime =
          static_cast<double>(startFrame) * HOP_SIZE / SAMPLE_RATE;
      const double endTime =
          static_cast<double>(endFrame) * HOP_SIZE / SAMPLE_RATE;
      const float x1 = content.getX() +
                       static_cast<float>((startTime / totalTime) *
                                          content.getWidth());
      const float x2 = content.getX() +
                       static_cast<float>((endTime / totalTime) *
                                          content.getWidth());
      g.fillRect(juce::Rectangle<float>(x1, content.getY(),
                                        std::max(1.0f, x2 - x1),
                                        content.getHeight()));
    }

    g.setColour(juce::Colours::orange.withAlpha(0.75f));
    for (const auto &range : audioData.segmentChunkRanges) {
      const int startFrame = std::max(0, range.first);
      const int endFrame = std::max(startFrame, range.second);
      if (endFrame <= startFrame)
        continue;
      const double startTime =
          static_cast<double>(startFrame) * HOP_SIZE / SAMPLE_RATE;
      const float x = content.getX() +
                      static_cast<float>((startTime / totalTime) *
                                         content.getWidth());
      g.drawLine(x, content.getY(), x, content.getBottom(), 1.0f);
    }
  }

  if (totalTime > 0.0) {
    const float pitchRange =
        static_cast<float>(MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1);
    if (pitchRange > 0.0f) {
      const float noteHeight =
          juce::jlimit(1.0f, 4.0f, content.getHeight() / pitchRange);
      const float thickness =
          juce::jlimit(1.0f, 3.0f, noteHeight);

      for (const auto &note : project->getNotes()) {
        if (note.isRest())
          continue;

        const double startTime =
            static_cast<double>(note.getStartFrame()) * HOP_SIZE / SAMPLE_RATE;
        const double endTime =
            static_cast<double>(note.getEndFrame()) * HOP_SIZE / SAMPLE_RATE;

        if (endTime <= startTime)
          continue;

        float midi = note.getAdjustedMidiNote();
        if (midi < MIN_MIDI_NOTE - 1 || midi > MAX_MIDI_NOTE + 1)
          continue;

        const auto baseColour =
            note.isSelected() ? APP_COLOR_NOTE_SELECTED : getPitchColour(midi);
        g.setColour(baseColour.withAlpha(0.18f));

        const auto &delta = note.getDeltaPitch();
        if (delta.size() > 1) {
          juce::Path path;
          const double duration = endTime - startTime;
          for (size_t i = 0; i < delta.size(); ++i) {
            const float t =
                static_cast<float>(i) /
                static_cast<float>(std::max<size_t>(1, delta.size() - 1));
            const double time = startTime + duration * t;
            const float x = content.getX() +
                            static_cast<float>((time / totalTime) *
                                               content.getWidth());
            const float pitch = midi + delta[i];
            const float y = content.getY() +
                            (MAX_MIDI_NOTE - pitch) / pitchRange *
                                content.getHeight();
            if (i == 0)
              path.startNewSubPath(x, y);
            else
              path.lineTo(x, y);
          }
          g.strokePath(path,
                       juce::PathStrokeType(thickness * 2.6f,
                                            juce::PathStrokeType::curved,
                                            juce::PathStrokeType::rounded));
          g.setColour(baseColour.withAlpha(0.75f));
          g.strokePath(path,
                       juce::PathStrokeType(thickness,
                                            juce::PathStrokeType::curved,
                                            juce::PathStrokeType::rounded));
        } else {
          const float x1 = content.getX() +
                           static_cast<float>((startTime / totalTime) *
                                              content.getWidth());
          const float x2 = content.getX() +
                           static_cast<float>((endTime / totalTime) *
                                              content.getWidth());
          const float y = content.getY() +
                          (MAX_MIDI_NOTE - midi) / pitchRange *
                              content.getHeight();
          g.setColour(baseColour.withAlpha(0.18f));
          g.drawLine(x1, y, x2, y, thickness * 2.6f);
          g.setColour(baseColour.withAlpha(0.75f));
          g.drawLine(x1, y, x2, y, thickness);
        }
      }

      const auto &f0 = audioData.f0;
      if (!f0.empty()) {
        const auto &voiced = audioData.voicedMask;
        const int widthPx = static_cast<int>(content.getWidth());
        juce::Path f0Path;
        bool hasSegment = false;

        for (int px = 0; px < widthPx; ++px) {
          const double t0 = (static_cast<double>(px) / widthPx) * totalTime;
          const double t1 =
              (static_cast<double>(px + 1) / widthPx) * totalTime;
          const int startFrame =
              juce::jlimit(0, static_cast<int>(f0.size()) - 1,
                           static_cast<int>(t0 * SAMPLE_RATE / HOP_SIZE));
          const int endFrame =
              juce::jlimit(startFrame + 1, static_cast<int>(f0.size()),
                           static_cast<int>(t1 * SAMPLE_RATE / HOP_SIZE));

          float midi = 0.0f;
          bool found = false;
          for (int i = startFrame; i < endFrame; ++i) {
            if (i < 0 || i >= static_cast<int>(f0.size()))
              continue;
            if (!voiced.empty() &&
                (i >= static_cast<int>(voiced.size()) || !voiced[i]))
              continue;
            if (f0[static_cast<size_t>(i)] <= 0.0f)
              continue;
            midi = f0ToMidi(f0[static_cast<size_t>(i)]);
            found = true;
            break;
          }

          if (!found) {
            hasSegment = false;
            continue;
          }

          const float x = content.getX() + static_cast<float>(px);
          const float y = content.getY() +
                          (MAX_MIDI_NOTE - midi) / pitchRange *
                              content.getHeight();
          if (!hasSegment) {
            f0Path.startNewSubPath(x, y);
            hasSegment = true;
          } else {
            f0Path.lineTo(x, y);
          }
        }

        g.setColour(APP_COLOR_PITCH_CURVE.withAlpha(0.2f));
        g.strokePath(
            f0Path,
            juce::PathStrokeType(thickness * 1.4f,
                                 juce::PathStrokeType::curved,
                                 juce::PathStrokeType::rounded));
      }
    }
  }

  auto viewport = computeViewport();
  if (viewport.valid) {
    g.setColour(APP_COLOR_SELECTION_OVERLAY.withAlpha(0.35f));
    g.fillRoundedRectangle(viewport.rect, 4.0f);

    g.setColour(APP_COLOR_PRIMARY.withAlpha(0.9f));
    g.drawRoundedRectangle(viewport.rect, 4.0f, 1.0f);

    const float handleWidth = 2.0f;
    const float handleInset = 3.0f;
    const float handleHeight = viewport.rect.getHeight() - handleInset * 2.0f;

    g.fillRect(viewport.rect.getX() + handleInset,
               viewport.rect.getY() + handleInset, handleWidth, handleHeight);
    g.fillRect(viewport.rect.getRight() - handleInset - handleWidth,
               viewport.rect.getY() + handleInset, handleWidth, handleHeight);
  }

  auto state = getViewState ? getViewState() : ViewState{};
  if (totalTime > 0.0 && state.cursorTime >= 0.0 &&
      state.cursorTime <= totalTime) {
    const float playheadX =
        content.getX() +
        static_cast<float>((state.cursorTime / totalTime) * content.getWidth());

    g.setColour(APP_COLOR_PRIMARY);
    g.fillRect(playheadX - 0.5f, content.getY(), 1.0f, content.getHeight());

    constexpr float markerWidth = 7.0f;
    constexpr float markerHeight = 5.0f;
    juce::Path marker;
    marker.addTriangle(playheadX - markerWidth * 0.5f, content.getY(),
                       playheadX + markerWidth * 0.5f, content.getY(),
                       playheadX, content.getY() + markerHeight);
    g.fillPath(marker);
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

void OverviewPanel::updateCursor(DragMode mode) {
  if (mode == DragMode::ResizeLeft || mode == DragMode::ResizeRight) {
    setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
  } else if (mode == DragMode::Move) {
    setMouseCursor(juce::MouseCursor::DraggingHandCursor);
  } else {
    setMouseCursor(juce::MouseCursor::NormalCursor);
  }
}
