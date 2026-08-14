#include "TimingHandler.h"

#include "../../PianoRollComponent.h"
#include "../../../Utils/Constants.h"
#include "../../../Utils/TimingRegionUtils.h"
#include "../../../Utils/UI/Theme.h"

#include <algorithm>
#include <cmath>
#include <limits>

TimingHandler::TimingHandler(PianoRollComponent& owner)
    : InteractionHandler(owner) {}

std::vector<TimingHandler::Boundary> TimingHandler::buildBoundaries() const
{
  std::vector<Note*> notes;
  if (!owner_.project)
    return {};

  for (auto& note : owner_.project->getNotes())
    if (!note.isRest())
      notes.push_back(&note);
  std::sort(notes.begin(), notes.end(), [](const Note* a, const Note* b)
  {
    return a->getStartFrame() < b->getStartFrame();
  });

  std::vector<Boundary> result;
  auto addEdge = [&](float frame, Note* left, Note* right)
  {
    auto existing = std::find_if(result.begin(), result.end(),
                                 [frame](const Boundary& b)
                                 { return std::abs(b.frame - frame) < 0.001f; });
    if (existing == result.end())
    {
      result.push_back({frame, left, right, nullptr, 0.0f, 0.0f});
      return;
    }
    if (left)
      existing->left = left;
    if (right)
      existing->right = right;
  };

  for (auto* note : notes)
  {
    addEdge(note->getVisualStartFrame(), nullptr, note);
    addEdge(note->getVisualEndFrame(), note, nullptr);
  }

  for (auto& boundary : result)
  {
    std::vector<Note*> drawnNotes;
    if (boundary.left)
      drawnNotes.push_back(boundary.left);
    if (boundary.right)
      drawnNotes.push_back(boundary.right);

    if (!boundary.left && boundary.right)
    {
      for (auto* note : notes)
        if (note->getVisualEndFrame() < boundary.frame &&
            (!boundary.restCompanion ||
             note->getVisualEndFrame() >
                 boundary.restCompanion->getVisualEndFrame()))
          boundary.restCompanion = note;
    }
    else if (boundary.left && !boundary.right)
    {
      for (auto* note : notes)
        if (note->getVisualStartFrame() > boundary.frame &&
            (!boundary.restCompanion ||
             note->getVisualStartFrame() <
                 boundary.restCompanion->getVisualStartFrame()))
          boundary.restCompanion = note;
    }

    float top = std::numeric_limits<float>::max();
    float bottom = std::numeric_limits<float>::lowest();
    for (auto* note : drawnNotes)
    {
      const float y = owner_.midiToY(note->getAdjustedMidiNote());
      top = std::min(top, y);
      bottom = std::max(bottom, y + owner_.pixelsPerSemitone);
    }
    boundary.top = top;
    boundary.bottom = bottom;
  }

  std::sort(result.begin(), result.end(), [](const Boundary& a,
                                             const Boundary& b)
  { return a.frame < b.frame; });
  return result;
}

int TimingHandler::findBoundary(float worldX, float,
                                const std::vector<Boundary>& boundaries) const
{
  constexpr float hitPadding = 6.0f;
  int best = -1;
  float bestDistance = hitPadding + 1.0f;
  for (size_t i = 0; i < boundaries.size(); ++i)
  {
    const auto& boundary = boundaries[i];
    const float x = static_cast<float>(framesToSeconds(boundary.frame) *
                                       owner_.pixelsPerSecond);
    const float distance = std::abs(worldX - x);
    if (distance <= hitPadding && distance < bestDistance)
    {
      best = static_cast<int>(i);
      bestDistance = distance;
    }
  }
  return best;
}

std::vector<Note*> TimingHandler::collectAffectedNotes(
    const Boundary& boundary) const
{
  std::vector<Note*> notes;
  for (auto* note : {boundary.left, boundary.right, boundary.restCompanion})
    if (note && std::find(notes.begin(), notes.end(), note) == notes.end())
      notes.push_back(note);
  return notes;
}

std::vector<NoteTimingState> TimingHandler::captureAffected(
    const Boundary& boundary) const
{
  std::vector<NoteTimingState> states;
  for (auto* note : collectAffectedNotes(boundary))
    states.push_back(NoteTimingState::capture(*note));
  return states;
}

float TimingHandler::clampFrame(const Boundary& boundary,
                                float requestedFrame) const
{
  float minimum = 0.0f;
  float maximum = owner_.project
                      ? static_cast<float>(
                            owner_.project->getAudioData().getNumFrames())
                      : requestedFrame;
  if (boundary.left)
    minimum = std::max(minimum,
                       boundary.left->getVisualStartFrame() + 1.0f);
  if (boundary.right)
    maximum = std::min(maximum,
                       boundary.right->getVisualEndFrame() - 1.0f);

  // A boundary next to a rest may approach the neighboring region/note, but
  // never cross it or remove the required one-frame rest.
  if (!boundary.left && boundary.restCompanion)
    minimum = std::max(minimum,
                       boundary.restCompanion->getVisualEndFrame() + 1.0f);
  if (!boundary.right && boundary.restCompanion)
    maximum = std::min(maximum,
                       boundary.restCompanion->getVisualStartFrame() - 1.0f);

  // The unpitched audio attached to a region's first/last note moves without
  // changing length. Constrain the derived region edge, not just the note,
  // so neighboring regions always retain at least one frame of separation.
  if (boundary.right &&
      timingRegions::isFirstNote(*owner_.project, *boundary.right))
  {
    const auto region =
        timingRegions::getSourceRegion(*owner_.project, *boundary.right);
    const float leadLength = static_cast<float>(
        boundary.right->getSrcStartFrame() - region.start);
    float earliestRegionStart = 0.0f;
    if (region.index > 0)
      earliestRegionStart =
          timingRegions::visualEnd(
              *owner_.project,
              timingRegions::regionAt(*owner_.project, region.index - 1)) +
          1.0f;
    minimum = std::max(minimum, earliestRegionStart + leadLength);
  }

  if (boundary.left &&
      timingRegions::isLastNote(*owner_.project, *boundary.left))
  {
    const auto region =
        timingRegions::getSourceRegion(*owner_.project, *boundary.left);
    const float tailLength = static_cast<float>(
        region.end - boundary.left->getSrcEndFrame());
    float latestRegionEnd = static_cast<float>(
        owner_.project->getAudioData().getNumFrames());
    if (region.index + 1 < timingRegions::regionCount(*owner_.project))
      latestRegionEnd =
          timingRegions::visualStart(
              *owner_.project,
              timingRegions::regionAt(*owner_.project, region.index + 1)) -
          1.0f;
    maximum = std::min(maximum, latestRegionEnd - tailLength);
  }

  return minimum <= maximum ? std::clamp(requestedFrame, minimum, maximum)
                            : boundary.frame;
}

void TimingHandler::applyPreviewFrame(float frame)
{
  frame = clampFrame(activeBoundary, frame);
  previewBoundaryFrame = frame;
  if (activeBoundary.left)
    activeBoundary.left->setTimingPreviewEndFrame(frame);
  if (activeBoundary.right)
    activeBoundary.right->setTimingPreviewStartFrame(frame);
  owner_.repaint();
}

void TimingHandler::clearAffectedPreviews()
{
  for (auto* note : collectAffectedNotes(activeBoundary))
    note->clearTimingPreview();
}

bool TimingHandler::mouseDown(const juce::MouseEvent&, float worldX,
                              float worldY)
{
  const auto boundaries = buildBoundaries();
  const int index = findBoundary(worldX, worldY, boundaries);
  if (index < 0)
    return false;

  activeBoundary = boundaries[static_cast<size_t>(index)];
  before = captureAffected(activeBoundary);
  previewBoundaryFrame = activeBoundary.frame;
  dragging = true;
  return true;
}

bool TimingHandler::mouseDrag(const juce::MouseEvent&, float worldX, float)
{
  if (!dragging || !owner_.project)
    return false;

  const float seconds = worldX / std::max(1.0f, owner_.pixelsPerSecond);
  const float frame = seconds * SAMPLE_RATE / static_cast<float>(HOP_SIZE);
  applyPreviewFrame(frame);
  if (owner_.onPitchEdited)
    owner_.onPitchEdited();
  return true;
}

bool TimingHandler::mouseUp(const juce::MouseEvent&, float, float)
{
  if (!dragging || !owner_.project)
    return false;

  dragging = false;
  const int snappedFrame = static_cast<int>(std::lround(previewBoundaryFrame));
  clearAffectedPreviews();
  const int committedFrame = static_cast<int>(std::lround(
      clampFrame(activeBoundary, static_cast<float>(snappedFrame))));
  if (activeBoundary.left)
    activeBoundary.left->setEndFrame(committedFrame);
  if (activeBoundary.right)
    activeBoundary.right->setStartFrame(committedFrame);
  auto after = captureAffected(activeBoundary);
  bool changed = before.size() == after.size();
  if (changed)
  {
    changed = false;
    for (size_t i = 0; i < before.size(); ++i)
      if (before[i].startFrame != after[i].startFrame ||
          before[i].endFrame != after[i].endFrame)
      {
        changed = true;
        break;
      }
  }

  if (changed)
  {
    applyNoteTimingStates(*owner_.project, after);
    owner_.invalidateBasePitchCache();
    if (owner_.undoManager)
      owner_.undoManager->addAction(std::make_unique<TimingAction>(
          *owner_.project, before, after));
  }
  if (changed && owner_.onPitchEditFinished)
    owner_.onPitchEditFinished();
  before.clear();
  owner_.repaint();
  return true;
}

void TimingHandler::mouseMove(const juce::MouseEvent&, float worldX,
                              float worldY)
{
  const auto boundaries = buildBoundaries();
  const int index = findBoundary(worldX, worldY, boundaries);
  const float nextHover = index >= 0
                              ? boundaries[static_cast<size_t>(index)].frame
                              : -1.0f;
  if (std::abs(nextHover - hoveredBoundaryFrame) > 0.001f)
  {
    hoveredBoundaryFrame = nextHover;
    owner_.repaint();
  }
  owner_.setMouseCursor(index >= 0
                            ? juce::MouseCursor::LeftRightResizeCursor
                            : juce::MouseCursor::NormalCursor);
}

void TimingHandler::draw(juce::Graphics& g)
{
  const float canvasHeight =
      (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * owner_.pixelsPerSemitone;
  for (const auto& boundary : buildBoundaries())
  {
    const float x = static_cast<float>(framesToSeconds(boundary.frame) *
                                       owner_.pixelsPerSecond);
    const bool highlighted = dragging
                                 ? std::abs(boundary.frame -
                                            previewBoundaryFrame) < 0.001f
                                 : std::abs(boundary.frame -
                                            hoveredBoundaryFrame) < 0.001f;
    g.setColour(highlighted ? APP_COLOR_PRIMARY
                            : APP_COLOR_TEXT_PRIMARY.withAlpha(0.72f));
    g.fillRect(x - (highlighted ? 1.0f : 0.5f), 0.0f,
               highlighted ? 2.0f : 1.0f, canvasHeight);
  }
}

void TimingHandler::cancel()
{
  if (!dragging || !owner_.project)
    return;
  dragging = false;
  clearAffectedPreviews();
  before.clear();
  owner_.repaint();
}
