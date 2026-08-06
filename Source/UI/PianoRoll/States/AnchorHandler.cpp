#include "AnchorHandler.h"
#include "../../../Undo/AnchorPitchAction.h"
#include "../../../Utils/Constants.h"
#include "../../../Utils/PitchCurveProcessor.h"
#include "../../../Utils/TransformParams.h"
#include "../../../Utils/UI/Theme.h"

#include <algorithm>
#include <cmath>
#include <limits>

AnchorHandler::AnchorHandler(PianoRollComponent& owner)
    : InteractionHandler(owner)
{
}

bool AnchorHandler::mouseDown(const juce::MouseEvent& e, float worldX,
                              float worldY)
{
  if (e.mods.isPopupMenu())
    return removeAnchorAt(worldX, worldY);

  if (!e.mods.isLeftButtonDown() || !owner_.project)
    return false;

  const int hitId = hitTest(worldX, worldY);
  if (hitId >= 0)
  {
    activeAnchorId = hitId;
    hoveredAnchorId = hitId;
    dragging = true;
    dragMoved = false;
    owner_.repaint();
    return true;
  }

  if (!isPitchRegionAtWorldX(worldX))
    return false;
  const int frame = frameFromWorldX(worldX);

  captureOriginalCurveIfNeeded();
  const float midi = midiFromWorldY(worldY);

  auto sameFrame = std::find_if(anchors.begin(), anchors.end(),
                                [frame](const Anchor& anchor)
                                { return anchor.frame == frame; });
  if (sameFrame != anchors.end())
  {
    sameFrame->midi = midi;
    activeAnchorId = sameFrame->id;
  }
  else
  {
    anchors.push_back({nextAnchorId++, frame, midi});
    activeAnchorId = anchors.back().id;
  }

  hoveredAnchorId = activeAnchorId;
  dragging = true;
  dragMoved = false;
  sortAnchors();
  rebuildPreview();
  applyPreviewToProject();
  notifyStateChanged();
  return true;
}

bool AnchorHandler::mouseDrag(const juce::MouseEvent&, float worldX,
                              float worldY)
{
  if (!dragging)
    return false;

  auto* anchor = findAnchorById(activeAnchorId);
  if (!anchor || !owner_.project)
    return false;

  int newFrame = frameFromWorldX(worldX);
  const auto current = std::find_if(anchors.begin(), anchors.end(),
                                    [this](const Anchor& item)
                                    { return item.id == activeAnchorId; });
  if (current != anchors.end())
  {
    if (current != anchors.begin())
      newFrame = std::max(newFrame, std::prev(current)->frame + 1);
    if (std::next(current) != anchors.end())
      newFrame = std::min(newFrame, std::next(current)->frame - 1);
  }

  newFrame = juce::jlimit(0,
                          static_cast<int>(owner_.project->getAudioData().f0.size()) - 1,
                          newFrame);
  if (frameBelongsToPitchNote(newFrame))
    anchor->frame = newFrame;
  anchor->midi = midiFromWorldY(worldY);

  sortAnchors();
  rebuildPreview();
  dragMoved = true;
  owner_.repaint();
  return true;
}

bool AnchorHandler::mouseUp(const juce::MouseEvent&, float, float)
{
  if (!dragging)
    return false;
  dragging = false;
  activeAnchorId = -1;
  if (dragMoved)
    applyPreviewToProject();
  dragMoved = false;
  owner_.repaint();
  return true;
}

void AnchorHandler::mouseMove(const juce::MouseEvent&, float worldX,
                              float worldY)
{
  const int newHovered = hitTest(worldX, worldY);
  const bool newPointerOverPitchRegion =
      isPitchRegionAtWorldX(worldX);
  if (newHovered == hoveredAnchorId &&
      newPointerOverPitchRegion == pointerOverPitchRegion)
    return;
  hoveredAnchorId = newHovered;
  pointerOverPitchRegion = newPointerOverPitchRegion;
  owner_.setMouseCursor(
      newHovered >= 0
          ? juce::MouseCursor::DraggingHandCursor
          : (pointerOverPitchRegion ? juce::MouseCursor::CrosshairCursor
                                    : juce::MouseCursor::NormalCursor));
  owner_.repaint();
}

void AnchorHandler::mouseDoubleClick(const juce::MouseEvent&, float worldX,
                                     float worldY)
{
  removeAnchorAt(worldX, worldY);
}

bool AnchorHandler::removeAnchorAt(float worldX, float worldY)
{
  const int hitId = hitTest(worldX, worldY);
  if (hitId < 0)
    return false;

  anchors.erase(std::remove_if(anchors.begin(), anchors.end(),
                               [hitId](const Anchor& anchor)
                               { return anchor.id == hitId; }),
                anchors.end());
  hoveredAnchorId = -1;
  activeAnchorId = -1;
  dragging = false;
  dragMoved = false;

  if (anchors.empty())
  {
    clearState(true);
    return true;
  }

  rebuildPreview();
  applyPreviewToProject();
  pointerOverPitchRegion = isPitchRegionAtWorldX(worldX);
  owner_.setMouseCursor(pointerOverPitchRegion
                            ? juce::MouseCursor::CrosshairCursor
                            : juce::MouseCursor::NormalCursor);
  notifyStateChanged();
  return true;
}

void AnchorHandler::clearHover()
{
  if (hoveredAnchorId < 0 || dragging)
    return;
  hoveredAnchorId = -1;
  owner_.repaint();
}

void AnchorHandler::draw(juce::Graphics& g)
{
  constexpr float radius = 5.0f;
  for (const auto& anchor : anchors)
  {
    const auto point = anchorPosition(anchor);
    const auto circle = juce::Rectangle<float>(point.x - radius, point.y - radius,
                                               radius * 2.0f, radius * 2.0f);
    const bool highlighted = anchor.id == hoveredAnchorId ||
                             anchor.id == activeAnchorId;
    g.setColour(APP_COLOR_PITCH_CURVE);
    if (highlighted)
      g.fillEllipse(circle);
    else
    {
      g.setColour(APP_COLOR_BACKGROUND);
      g.fillEllipse(circle);
      g.setColour(APP_COLOR_PITCH_CURVE);
      g.drawEllipse(circle, 1.5f);
    }
  }
}

bool AnchorHandler::isActive() const
{
  return dragging || !anchors.empty();
}

void AnchorHandler::cancel()
{
  clearState(true);
}

void AnchorHandler::clearState(bool restoreNoteStates)
{
  if (restoreNoteStates)
  {
    restoreOriginalNoteStates(true);
  }
  anchors.clear();
  originalNoteStates.clear();
  originalMidiCurve.clear();
  previewMidiCurve.clear();
  hoveredAnchorId = -1;
  pointerOverPitchRegion = false;
  activeAnchorId = -1;
  dragging = false;
  dragMoved = false;
  previewDirtyStart = -1;
  previewDirtyEnd = -1;
  notifyStateChanged();
}

bool AnchorHandler::apply()
{
  if (!owner_.project || anchors.empty() || previewMidiCurve.empty())
    return false;

  const int firstFrame = anchors.front().frame;
  const int lastFrame = anchors.back().frame;
  auto& project = *owner_.project;
  std::vector<AnchorPitchNoteState> before;
  std::vector<AnchorPitchNoteState> after;

  for (auto& note : project.getNotes())
  {
    if (note.isRest() || note.getEndFrame() <= firstFrame ||
        note.getStartFrame() > lastFrame)
      continue;

    const auto* original = originalStateFor(note);
    if (!original)
      continue;

    AnchorPitchNoteState oldState;
    oldState.note = &note;
    oldState.params = original->params;
    oldState.bakedDeltaPitch = original->bakedDeltaPitch;
    before.push_back(oldState);

    AnchorPitchNoteState newState;
    newState.note = &note;
    newState.params = TransformParams::fromNote(note);
    newState.bakedDeltaPitch = note.getBakedDeltaPitch();
    after.push_back(std::move(newState));
  }

  if (before.empty())
    return false;

  auto action = std::make_unique<AnchorPitchAction>(
      &project, std::move(before), std::move(after));
  if (owner_.undoManager)
    owner_.undoManager->addAction(std::move(action));

  project.setModified(true);
  clearState(false);
  return true;
}

int AnchorHandler::frameFromWorldX(float worldX) const
{
  if (!owner_.project || owner_.project->getAudioData().f0.empty())
    return 0;
  const int lastFrame =
      static_cast<int>(owner_.project->getAudioData().f0.size()) - 1;
  const int frame = static_cast<int>(std::round(
      secondsToFrames(static_cast<float>(owner_.xToTime(worldX)))));
  return juce::jlimit(0, lastFrame, frame);
}

float AnchorHandler::midiFromWorldY(float worldY) const
{
  float midi = owner_.yToMidi(worldY - owner_.pixelsPerSemitone * 0.5f);
  if (owner_.project)
    midi -= owner_.project->getGlobalPitchOffset();
  return juce::jlimit(static_cast<float>(MIN_MIDI_NOTE),
                      static_cast<float>(MAX_MIDI_NOTE), midi);
}

juce::Point<float> AnchorHandler::anchorPosition(const Anchor& anchor) const
{
  const float globalOffset = owner_.project
                                 ? owner_.project->getGlobalPitchOffset()
                                 : 0.0f;
  return {owner_.timeToX(framesToSeconds(anchor.frame)),
          owner_.midiToY(anchor.midi + globalOffset) +
              owner_.pixelsPerSemitone * 0.5f};
}

int AnchorHandler::hitTest(float worldX, float worldY) const
{
  constexpr float hitRadius = 8.0f;
  for (auto it = anchors.rbegin(); it != anchors.rend(); ++it)
  {
    if (anchorPosition(*it).getDistanceFrom({worldX, worldY}) <= hitRadius)
      return it->id;
  }
  return -1;
}

AnchorHandler::Anchor* AnchorHandler::findAnchorById(int id)
{
  auto it = std::find_if(anchors.begin(), anchors.end(),
                         [id](const Anchor& anchor) { return anchor.id == id; });
  return it == anchors.end() ? nullptr : &*it;
}

const AnchorHandler::Anchor* AnchorHandler::findAnchorById(int id) const
{
  auto it = std::find_if(anchors.begin(), anchors.end(),
                         [id](const Anchor& anchor) { return anchor.id == id; });
  return it == anchors.end() ? nullptr : &*it;
}

bool AnchorHandler::frameBelongsToPitchNote(int frame) const
{
  if (!owner_.project)
    return false;
  return std::any_of(owner_.project->getNotes().begin(),
                     owner_.project->getNotes().end(),
                     [frame](const Note& note)
                     {
                       return !note.isRest() && note.getStartFrame() <= frame &&
                              note.getEndFrame() > frame;
                     });
}

bool AnchorHandler::isPitchRegionAtWorldX(float worldX) const
{
  if (!owner_.project)
    return false;

  const auto& f0 = owner_.project->getAudioData().f0;
  const int frame = static_cast<int>(std::round(
      secondsToFrames(static_cast<float>(owner_.xToTime(worldX)))));
  return frame >= 0 && frame < static_cast<int>(f0.size()) &&
         frameBelongsToPitchNote(frame);
}

void AnchorHandler::captureOriginalCurveIfNeeded()
{
  if (!originalMidiCurve.empty() || !owner_.project)
    return;

  const auto& audioData = owner_.project->getAudioData();
  const size_t size = audioData.f0.size();
  originalMidiCurve.assign(size, 0.0f);
  for (size_t i = 0; i < size; ++i)
  {
    const float base = i < audioData.basePitch.size() ? audioData.basePitch[i] : 0.0f;
    const float delta = i < audioData.deltaPitch.size() ? audioData.deltaPitch[i] : 0.0f;
    originalMidiCurve[i] = base + delta;
  }
  previewMidiCurve = originalMidiCurve;

  originalNoteStates.clear();
  originalNoteStates.reserve(owner_.project->getNotes().size());
  for (auto& note : owner_.project->getNotes())
    if (!note.isRest())
      originalNoteStates.push_back(
          {&note, TransformParams::fromNote(note), note.getBakedDeltaPitch()});
}

void AnchorHandler::sortAnchors()
{
  std::sort(anchors.begin(), anchors.end(),
            [](const Anchor& a, const Anchor& b) { return a.frame < b.frame; });
}

void AnchorHandler::rebuildPreview()
{
  previewMidiCurve = originalMidiCurve;
  if (anchors.empty() || previewMidiCurve.empty())
    return;

  if (anchors.size() == 1)
  {
    previewMidiCurve[static_cast<size_t>(anchors.front().frame)] =
        anchors.front().midi;
    updateAffectedNotePositions();
    return;
  }

  for (size_t segment = 1; segment < anchors.size(); ++segment)
  {
    const auto& left = anchors[segment - 1];
    const auto& right = anchors[segment];
    const int length = std::max(1, right.frame - left.frame);
    for (int frame = left.frame; frame <= right.frame; ++frame)
    {
      const float t = static_cast<float>(frame - left.frame) /
                      static_cast<float>(length);
      // Quintic smootherstep: flat at both anchors, with a steeper transition
      // through the middle than linear interpolation.
      const float shapedT = t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
      previewMidiCurve[static_cast<size_t>(frame)] =
          juce::jmap(shapedT, left.midi, right.midi);
    }
  }

  updateAffectedNotePositions();
}

void AnchorHandler::updateAffectedNotePositions()
{
  if (!owner_.project || anchors.empty() || previewMidiCurve.empty())
    return;

  // Start from the exact pre-anchor positions on every preview update, so
  // dragging an anchor or shrinking its range cannot accumulate movement.
  restoreOriginalNoteStates(false);

  const int firstFrame = anchors.front().frame;
  const int lastFrame = anchors.back().frame;
  const auto& audioData = owner_.project->getAudioData();

  for (auto& note : owner_.project->getNotes())
  {
    if (note.isRest() || note.getEndFrame() <= firstFrame ||
        note.getStartFrame() > lastFrame)
      continue;

    const int start = std::max(0, note.getStartFrame());
    const int end = std::min(note.getEndFrame(),
                             static_cast<int>(previewMidiCurve.size()));
    double sum = 0.0;
    int count = 0;
    for (int frame = start; frame < end; ++frame)
    {
      const bool voiced =
          audioData.voicedMask.empty() ||
          (frame < static_cast<int>(audioData.voicedMask.size()) &&
           audioData.voicedMask[static_cast<size_t>(frame)]);
      if (!voiced || !std::isfinite(previewMidiCurve[static_cast<size_t>(frame)]))
        continue;
      sum += previewMidiCurve[static_cast<size_t>(frame)];
      ++count;
    }

    if (count > 0)
      note.setMidiNote(static_cast<float>(sum / static_cast<double>(count)));
  }

  if (owner_.onPitchEdited)
    owner_.onPitchEdited();
}

void AnchorHandler::applyPreviewToProject()
{
  if (!owner_.project || anchors.empty() || previewMidiCurve.empty())
    return;

  const int firstFrame = anchors.front().frame;
  const int lastFrame = anchors.back().frame;
  auto& project = *owner_.project;
  auto& audioData = project.getAudioData();
  std::vector<Note*> affected;

  for (auto& note : project.getNotes())
  {
    if (note.isRest() || note.getEndFrame() <= firstFrame ||
        note.getStartFrame() > lastFrame)
      continue;

    // Keep the live note center calculated by updateAffectedNotePositions(),
    // but make the per-note transforms neutral before deriving the base curve
    // for the baked preview contour.
    note.setTiltLeft(0.0f);
    note.setTiltRight(0.0f);
    note.setVibrato(1.0f);
    note.setSmoothLeftFrames(0);
    note.setSmoothRightFrames(0);
    note.setDeltaScale(1.0f);
    note.setDeltaOffset(0.0f);
    affected.push_back(&note);
  }

  if (affected.empty())
    return;

  PitchCurveProcessor::rebuildBaseFromNotes(project);
  int dirtyStart = std::numeric_limits<int>::max();
  int dirtyEnd = std::numeric_limits<int>::min();
  for (auto* note : affected)
  {
    const int duration = std::max(0, note->getDurationFrames());
    std::vector<float> bakedDelta(static_cast<size_t>(duration), 0.0f);
    for (int i = 0; i < duration; ++i)
    {
      const int frame = note->getStartFrame() + i;
      if (frame >= 0 && frame < static_cast<int>(previewMidiCurve.size()) &&
          frame < static_cast<int>(audioData.basePitch.size()))
        bakedDelta[static_cast<size_t>(i)] =
            previewMidiCurve[static_cast<size_t>(frame)] -
            audioData.basePitch[static_cast<size_t>(frame)];
    }
    note->setBakedDeltaPitch(std::move(bakedDelta));
    note->markDirty();
    note->markSynthDirty();
    dirtyStart = std::min(dirtyStart, note->getStartFrame());
    dirtyEnd = std::max(dirtyEnd, note->getEndFrame());
  }

  PitchCurveProcessor::rebuildBaseFromNotes(project);
  project.setF0DirtyRange(dirtyStart, dirtyEnd);
  previewDirtyStart = previewDirtyStart < 0
                          ? dirtyStart
                          : std::min(previewDirtyStart, dirtyStart);
  previewDirtyEnd = previewDirtyEnd < 0
                        ? dirtyEnd
                        : std::max(previewDirtyEnd, dirtyEnd);
  owner_.invalidateBasePitchCache();
  if (owner_.onPitchEdited)
    owner_.onPitchEdited();
  if (owner_.onPitchPreviewRenderRequested)
    owner_.onPitchPreviewRenderRequested();
}

void AnchorHandler::restoreOriginalNoteStates(bool requestRender)
{
  if (!owner_.project || originalNoteStates.empty())
    return;

  for (const auto& state : originalNoteStates)
  {
    if (!state.note)
      continue;
    state.params.applyToNote(*state.note);
    state.note->setBakedDeltaPitch(state.bakedDeltaPitch);
    if (requestRender && previewDirtyStart >= 0 &&
        state.note->getStartFrame() < previewDirtyEnd &&
        state.note->getEndFrame() > previewDirtyStart)
    {
      state.note->markDirty();
      state.note->markSynthDirty();
    }
  }

  if (!requestRender)
    return;

  PitchCurveProcessor::rebuildBaseFromNotes(*owner_.project);
  if (previewDirtyStart >= 0 && previewDirtyEnd > previewDirtyStart)
    owner_.project->setF0DirtyRange(previewDirtyStart, previewDirtyEnd);
  owner_.invalidateBasePitchCache();
  if (owner_.onPitchEdited)
    owner_.onPitchEdited();
  if (previewDirtyStart >= 0 && owner_.onPitchPreviewRenderRequested)
    owner_.onPitchPreviewRenderRequested();
}

const AnchorHandler::OriginalNoteState*
AnchorHandler::originalStateFor(const Note& note) const
{
  const auto found = std::find_if(
      originalNoteStates.begin(), originalNoteStates.end(),
      [&note](const OriginalNoteState& state) { return state.note == &note; });
  return found != originalNoteStates.end() ? &*found : nullptr;
}

void AnchorHandler::notifyStateChanged()
{
  owner_.updateAnchorConfirmation();
  owner_.repaint();
}
