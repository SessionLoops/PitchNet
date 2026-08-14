#include "LoopDragHandler.h"
#include "../../PianoRollComponent.h"

LoopDragHandler::LoopDragHandler(PianoRollComponent &owner)
    : InteractionHandler(owner) {}

bool LoopDragHandler::mouseDown(const juce::MouseEvent &e, float worldX,
                                float worldY) {
  juce::ignoreUnused(worldY);

  // Only handle clicks in the loop timeline area
  if (e.y < PianoRollComponent::timelineHeight ||
      e.y >= PianoRollComponent::headerHeight ||
      e.x < PianoRollComponent::pianoKeysWidth)
    return false;

  auto *project = owner_.project;
  if (!project)
    return false;

  float adjustedX =
      e.x - PianoRollComponent::pianoKeysWidth + static_cast<float>(owner_.scrollX);

  const auto &loopRange = project->getLoopRange();
  if (loopRange.endSeconds > loopRange.startSeconds) {
    const float startX =
        static_cast<float>(PianoRollComponent::pianoKeysWidth) +
        owner_.timeToX(loopRange.startSeconds) -
        static_cast<float>(owner_.scrollX);
    const float endX =
        static_cast<float>(PianoRollComponent::pianoKeysWidth) +
        owner_.timeToX(loopRange.endSeconds) -
        static_cast<float>(owner_.scrollX);

    if (std::abs(static_cast<float>(e.x) - startX) <= loopHandleHitPadding) {
      loopDragMode = LoopDragMode::ResizeStart;
      loopDragStartSeconds = loopRange.startSeconds;
      loopDragEndSeconds = loopRange.endSeconds;
      owner_.repaint();
      return true;
    }
    if (std::abs(static_cast<float>(e.x) - endX) <= loopHandleHitPadding) {
      loopDragMode = LoopDragMode::ResizeEnd;
      loopDragStartSeconds = loopRange.startSeconds;
      loopDragEndSeconds = loopRange.endSeconds;
      owner_.repaint();
      return true;
    }
    if (static_cast<float>(e.x) >= startX &&
        static_cast<float>(e.x) <= endX) {
      loopDragMode = LoopDragMode::Move;
      loopDragAnchorSeconds =
          owner_.snapTimeToTimelineGrid(owner_.xToTime(adjustedX));
      loopDragOriginalStart = loopRange.startSeconds;
      loopDragOriginalEnd = loopRange.endSeconds;
      loopDragStartSeconds = loopRange.startSeconds;
      loopDragEndSeconds = loopRange.endSeconds;
      owner_.repaint();
      return true;
    }
  }

  loopDragMode = LoopDragMode::Create;
  loopDragStartX = static_cast<float>(e.x);
  loopDragStartSeconds =
      owner_.snapTimeToTimelineGrid(owner_.xToTime(adjustedX));
  loopDragEndSeconds = loopDragStartSeconds;
  owner_.repaint();
  return true;
}

bool LoopDragHandler::mouseDrag(const juce::MouseEvent &e, float worldX,
                                float worldY) {
  juce::ignoreUnused(worldY);

  if (loopDragMode == LoopDragMode::None)
    return false;

  float adjustedX =
      e.x - PianoRollComponent::pianoKeysWidth + static_cast<float>(owner_.scrollX);
  const double dragTime =
      owner_.snapTimeToTimelineGrid(owner_.xToTime(adjustedX));

  switch (loopDragMode) {
  case LoopDragMode::ResizeStart:
    loopDragStartSeconds = dragTime;
    break;
  case LoopDragMode::ResizeEnd:
    loopDragEndSeconds = dragTime;
    break;
  case LoopDragMode::Create:
    loopDragEndSeconds = dragTime;
    break;
  case LoopDragMode::Move: {
    double delta = dragTime - loopDragAnchorSeconds;
    double newStart = loopDragOriginalStart + delta;
    double newEnd = loopDragOriginalEnd + delta;

    if (owner_.project) {
      const double duration = owner_.project->getAudioData().getDuration();
      if (duration > 0.0) {
        if (newStart < 0.0) {
          newEnd -= newStart;
          newStart = 0.0;
        }
        if (newEnd > duration) {
          double overflow = newEnd - duration;
          newStart -= overflow;
          newEnd = duration;
          if (newStart < 0.0)
            newStart = 0.0;
        }
      }
    }

    loopDragStartSeconds = newStart;
    loopDragEndSeconds = newEnd;
    break;
  }
  case LoopDragMode::None:
    break;
  }

  owner_.repaint();
  return true;
}

bool LoopDragHandler::mouseUp(const juce::MouseEvent &e, float worldX,
                              float worldY) {
  juce::ignoreUnused(worldX, worldY);

  if (loopDragMode == LoopDragMode::None)
    return false;

  constexpr float minDragDistance = 4.0f;
  const bool isCreate = loopDragMode == LoopDragMode::Create;
  loopDragMode = LoopDragMode::None;

  if (!owner_.project) {
    owner_.repaint();
    return true;
  }

  if (!isCreate ||
      std::abs(static_cast<float>(e.x) - loopDragStartX) >= minDragDistance) {
    owner_.project->setLoopRange(loopDragStartSeconds, loopDragEndSeconds);
    if (owner_.onLoopRangeChanged)
      owner_.onLoopRangeChanged(owner_.project->getLoopRange());
  }
  owner_.repaint();
  return true;
}

void LoopDragHandler::mouseMove(const juce::MouseEvent &e, float worldX,
                                float worldY) {
  juce::ignoreUnused(worldX, worldY);

  hoveringHandle = false;
  if (!owner_.project || e.y < PianoRollComponent::timelineHeight ||
      e.y >= PianoRollComponent::headerHeight ||
      e.x < PianoRollComponent::pianoKeysWidth) {
    return;
  }

  const auto &loopRange = owner_.project->getLoopRange();
  if (loopRange.endSeconds > loopRange.startSeconds) {
    const float startX =
        static_cast<float>(PianoRollComponent::pianoKeysWidth) +
        owner_.timeToX(loopRange.startSeconds) -
        static_cast<float>(owner_.scrollX);
    const float endX =
        static_cast<float>(PianoRollComponent::pianoKeysWidth) +
        owner_.timeToX(loopRange.endSeconds) -
        static_cast<float>(owner_.scrollX);

    if (std::abs(static_cast<float>(e.x) - startX) <= loopHandleHitPadding ||
        std::abs(static_cast<float>(e.x) - endX) <= loopHandleHitPadding) {
      hoveringHandle = true;
      owner_.setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
    } else {
      owner_.setMouseCursor(juce::MouseCursor::NormalCursor);
    }
  } else {
    owner_.setMouseCursor(juce::MouseCursor::NormalCursor);
  }
}

bool LoopDragHandler::isActive() const {
  return loopDragMode != LoopDragMode::None;
}

void LoopDragHandler::cancel() {
  loopDragMode = LoopDragMode::None;
  hoveringHandle = false;
}
