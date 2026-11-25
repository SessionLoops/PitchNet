#include "../Source/UI/PianoRollComponent.h"
#include "../Source/Undo/PitchUndoManager.h"
#include "TestAssert.h"
#include <iostream>

static juce::MouseEvent event(PianoRollComponent& roll, juce::Point<float> point,
    int modifiers = juce::ModifierKeys::leftButtonModifier) {
  return {juce::Desktop::getInstance().getMainMouseSource(), point,
      juce::ModifierKeys(modifiers), 1.0f, 0, 0, 0, 0,
      &roll, &roll, juce::Time::getCurrentTime(), point, juce::Time::getCurrentTime(), 1, true};
}

int main() {
  juce::ScopedJuceInitialiser_GUI gui;
  Project a, b;
  for (auto* project : {&a, &b}) {
    project->getAudioData().sampleRate = SAMPLE_RATE;
    project->getAudioData().f0.assign(50, 440.0f);
    project->getAudioData().basePitch.assign(50, 88.0f);
    project->getAudioData().deltaPitch.assign(50, 0.0f);
    project->addNote(Note(3, 20, 88));
  }
  PianoRollComponent roll;
  PitchUndoManager history;
  roll.setSize(800, 400);
  roll.setProject(&a);
  roll.setUndoManager(&history);
  roll.setTrackViewMode(true);
  roll.setScrollX(0); roll.setScrollY(0);
  auto regions = std::make_shared<std::vector<MainViewRegionPreview>>();
  MainViewRegionPreview first;
  first.active = true; first.sourceProject = &a; first.startSeconds = 0; first.endSeconds = 0.5;
  regions->push_back(first);
  MainViewRegionPreview second;
  second.project = std::make_shared<Project>(b);
  second.sourceProject = &b; second.startSeconds = 1; second.endSeconds = 1.5;
  second.projectHostOffsetSeconds = 1;
  regions->push_back(second);
  roll.setRegionPreviews(regions);
  bool bAlive = true;
  int aRenders = 0, bRenders = 0;
  roll.onPitchEditFinished = [&] { ++aRenders; };
  roll.getTrackEditTargets = [&] {
    return std::vector<TrackPitchMacroTarget>{
        {&a, {{0, 50}}, [] { return true; }, {}},
        {&b, {{0, 50}}, [&] { return bAlive; }, [&](bool render) { if (render) ++bRenders; }}};
  };
  const float h = roll.getPixelsPerSemitone();
  const float header = static_cast<float>(roll.getHeaderHeight());
  // A newly focused region can be published before its analysis completes.
  // Its canvas then gains notes, but selection must also receive the new
  // source binding without recreating the component/editor.
  (*regions)[0].sourceProject = nullptr;
  roll.setRegionPreviews(regions);
  CHECK(roll.beginTrackGesture(event(roll, {31, header + 1})));
  roll.updateTrackGesture(event(roll, {220, header + 3*h}));
  roll.finishTrackGesture();
  CHECK(!a.getNotes()[0].isSelected());
  a.getNotes()[0].setSelected(false);
  b.getNotes()[0].setSelected(false);
  auto analyzedRegions = std::make_shared<std::vector<MainViewRegionPreview>>(*regions);
  (*analyzedRegions)[0].sourceProject = &a;
  roll.setRegionPreviews(analyzedRegions);
  CHECK(roll.beginTrackGesture(event(roll, {31, header + 1})));
  roll.updateTrackGesture(event(roll, {220, header + 3*h}));
  roll.finishTrackGesture();
  CHECK(a.getNotes()[0].isSelected() && b.getNotes()[0].isSelected());
  CHECK(second.project->getNotes()[0].isSelected());
  CHECK(!history.canUndo());
  const float x = PianoRollComponent::pianoKeysWidth + framesToSeconds(10) * roll.getPixelsPerSecond();
  CHECK(roll.beginTrackGesture(event(roll, {x, header + 1.5f*h})));
  roll.updateTrackGesture(event(roll, {x, header + 0.5f*h}));
  roll.finishTrackGesture();
  CHECK(a.getNotes()[0].getMidiNote() == 89 && b.getNotes()[0].getMidiNote() == 89);
  CHECK(history.getUndoName() == "Drag Track Notes");
  CHECK(aRenders == 1 && bRenders == 1);
  bAlive = false;
  history.undo();
  CHECK(a.getNotes()[0].getMidiNote() == 88 && b.getNotes()[0].getMidiNote() == 89);
  CHECK(bRenders == 1);
  history.redo();
  CHECK(a.getNotes()[0].getMidiNote() == 89);
  // A note in an overlapping peer region must get hover/tools even when
  // the bound region covers that same timeline position at another pitch.
  bAlive = true;
  a.getNotes()[0].setMidiNote(88);
  b.getNotes()[0].setMidiNote(86);
  auto peers = std::make_shared<std::vector<MainViewRegionPreview>>(*analyzedRegions);
  (*peers)[0].key = "a";
  (*peers)[0].active = true;
  (*peers)[0].project = std::make_shared<Project>(a);
  (*peers)[1].key = "b";
  (*peers)[1].active = false;
  (*peers)[1].startSeconds = 0;
  (*peers)[1].endSeconds = 0.5;
  (*peers)[1].projectHostOffsetSeconds = 0;
  (*peers)[1].project = std::make_shared<Project>(b);
  roll.setRegionPreviews(peers);
  juce::String routed;
  roll.onInactiveRegionClicked = [&](const juce::String& key) {
    routed = key;
    roll.setProject(key == "a" ? &a : &b);
    roll.setTimelineDisplayOffset(0);
    (*peers)[0].active = key == "a";
    (*peers)[1].active = key == "b";
    roll.setRegionPreviews(peers);
  };
  roll.mouseMove(event(roll, {x, header + 3.5f*h}, 0));
  CHECK(routed == "b" && roll.getProject() == &b);
  // Hover returns to A without a selection/activation click.
  roll.mouseMove(event(roll, {x, header + 1.5f*h}, 0));
  CHECK(routed == "a" && roll.getProject() == &a);
  // Flags affect only routing metadata, not which overlapping note can be hit.
  (*peers)[0].active = false;
  (*peers)[1].active = true;
  roll.setRegionPreviews(peers);
  routed.clear();
  roll.mouseMove(event(roll, {x, header + 3.5f*h}, 0));
  CHECK(routed == "b");
  // Clip Mode retains its existing local hover behavior.
  roll.setTrackViewMode(false);
  roll.setProject(&a);
  routed.clear();
  roll.mouseMove(event(roll, {x, header + 3.5f*h}, 0));
  CHECK(routed.isEmpty() && roll.getProject() == &a);
  std::cout << "Track gesture tests passed\n";
}
