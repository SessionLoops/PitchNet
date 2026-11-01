#include "../Source/UI/PianoRollWorkspaceView.h"
#include "../Source/UI/Dialogs/QuantizePitchDialog.h"
#include "../Source/Undo/PitchUndoManager.h"
#include "../Source/Utils/Localization.h"
#include "TestAssert.h"
#include <cmath>
#include <functional>
#include <iostream>

struct Fixture {
  Project a, b;
  PianoRollComponent roll;
  PianoRollWorkspaceView workspace{roll};
  PitchUndoManager history;
  int renders = 0, commits = 0;
  juce::Component* popup = nullptr;
  explicit Fixture(bool track = true,
                   std::function<void(Project&, Project&)> prepare = {}) {
    for (auto* project : {&a, &b}) {
      project->setPitchCenter(0);
      auto& audio = project->getAudioData();
      audio.sampleRate = SAMPLE_RATE;
      audio.f0.assign(50, 440.0f);
      audio.basePitch.assign(50, 60.0f);
      audio.deltaPitch.assign(50, 0.0f);
    }
    a.addNote(Note(5, 25, 60.4f));
    b.addNote(Note(5, 25, 60.7f));
    if (prepare) prepare(a, b);
    roll.setProject(&a);
    roll.setUndoManager(&history);
    workspace.setSize(800, 400);
    workspace.setTrackViewModeAvailable(true);
    workspace.setTrackViewMode(track);
    roll.onPitchPreviewRenderRequested = [&] { ++renders; };
    roll.onPitchEditFinished = [&] { ++renders; };
    roll.onPitchEditCommitted = [&] { ++commits; };
    workspace.getTrackPitchMacroTargets = [&] {
      return std::vector<TrackPitchMacroTarget>{
          {&a, {{0, 50}}, [] { return true; }, {}},
          {&b, {{0, 50}}, [] { return true; },
           [&](bool render) { if (render) ++renders; }, [&] { ++commits; }}};
    };
    workspace.showPitchCenterPopup();
    for (auto* child : workspace.getChildren()) {
      int sliders = 0, buttons = 0;
      for (auto* control : child->getChildren()) {
        sliders += dynamic_cast<juce::Slider*>(control) != nullptr;
        buttons += dynamic_cast<juce::TextButton*>(control) != nullptr;
      }
      if (sliders == 2 && buttons == 2) popup = child;
    }
    CHECK(popup);
  }
  ~Fixture() { delete popup; }
  juce::Slider& slider(int index) {
    for (auto* child : popup->getChildren())
      if (auto* found = dynamic_cast<juce::Slider*>(child))
        if (index-- == 0) return *found;
    std::abort();
  }
  juce::Slider& center() { return slider(0); }
  juce::Slider& drift() { return slider(1); }
  static juce::String text(juce::Slider& s) { return s.getTextFromValue(s.getValue()); }
  float strength(Project& p) { return p.getNotes()[0].getMacroPitchCenter(); }
  void press(bool ok) {
    const auto label = ok ? TR("dialog.ok") : TR("dialog.cancel");
    for (auto* child : popup->getChildren())
      if (auto* button = dynamic_cast<juce::TextButton*>(child))
        if (button->getButtonText() == label) { button->onClick(); return; }
    CHECK(false);
  }
  void drag(float value) {
    center().onDragStart();
    center().setValue(value, juce::sendNotificationSync);
    center().onDragEnd();
  }
};

int main() {
  juce::ScopedJuceInitialiser_GUI gui;
  {
    Fixture f;
    f.center().onDragStart();
    f.center().setValue(100, juce::sendNotificationSync);
    CHECK(f.renders == 0); // Lightweight note/curve preview while held.
    f.center().onDragEnd();
    CHECK(f.renders == 2); // Active and peer regions each render once.
    f.press(true);
    CHECK(f.renders == 2 && f.commits == 2);
    CHECK(f.a.getPitchCenter() == 100 && f.b.getPitchCenter() == 100);
    CHECK(f.history.canUndo());
    f.history.undo();
    CHECK(f.renders == 4);
    CHECK(std::abs(f.a.getNotes()[0].getMidiNote() - 60.4f) < 1e-4f);
    CHECK(std::abs(f.b.getNotes()[0].getMidiNote() - 60.7f) < 1e-4f);
    f.history.redo();
    CHECK(f.renders == 6);
  }
  {
    Fixture f;
    f.drag(100);
    CHECK(f.renders == 2);
    f.press(false);
    CHECK(f.renders == 4 && !f.history.canUndo());
    CHECK(std::abs(f.a.getNotes()[0].getMidiNote() - 60.4f) < 1e-4f);
  }
  {
    Fixture f;
    f.drag(100);
    f.drag(100); // No changed value: no duplicate request.
    CHECK(f.renders == 2);
    f.drag(0); // Returning to the original sound must still render.
    CHECK(f.renders == 4);
    f.press(true);
    CHECK(f.renders == 4 && !f.history.canUndo());
  }
  {
    Fixture f;
    f.center().onDragStart();
    f.center().setValue(100, juce::sendNotificationSync);
    f.press(false); // Cancel an unrendered drag without synthesizing.
    CHECK(f.renders == 0 && !f.history.canUndo());
  }
  {
    Fixture f(false); // Clip Mode renders only its own project on release.
    f.drag(100);
    CHECK(f.renders == 1);
    f.press(true);
    CHECK(f.renders == 1 && f.commits == 1);
    CHECK(std::abs(f.b.getNotes()[0].getMidiNote() - 60.7f) < 1e-4f);
  }
  {
    Fixture f; // Each note stores the strength applied to it, with undo.
    f.drag(100);
    f.press(true);
    CHECK(f.strength(f.a) == 100 && f.strength(f.b) == 100);
    f.history.undo();
    CHECK(f.strength(f.a) == 0 && f.strength(f.b) == 0);
    f.history.redo();
    CHECK(f.strength(f.a) == 100 && f.strength(f.b) == 100);
  }
  {
    Fixture f(true, [](Project& a, Project&) { a.getNotes()[0].setMacroPitchCenter(70); });
    CHECK(Fixture::text(f.center()) == "0 % - 70 %"); // Melodyne-style range.
    CHECK(f.center().getValue() == 35); // Thumb at the average (0 and 70).
    f.press(true); // Untouched sliders change nothing.
    CHECK(f.renders == 0 && f.commits == 0 && !f.history.canUndo());
    CHECK(f.strength(f.a) == 70 && f.strength(f.b) == 0);
  }
  {
    Fixture f(true, [](Project& a, Project&) { a.getNotes()[0].setMacroPitchCenter(70); });
    f.drag(50);
    CHECK(Fixture::text(f.center()) == "50 %");
    f.press(true);
    CHECK(f.strength(f.a) == 50 && f.strength(f.b) == 50);
  }
  {
    Fixture f(true, [](Project& a, Project& b) {
      for (auto* p : {&a, &b}) {
        auto& note = p->getNotes()[0];
        note.setMacroPitchCenter(100);
        note.setMidiNoteFromPitchCorrection(std::round(note.getMidiNote()));
      }
    });
    CHECK(Fixture::text(f.center()) == "100 %");
    f.drift().setValue(40, juce::sendNotificationSync); // Drift only.
    f.press(true);
    CHECK(f.history.canUndo());
    CHECK(std::abs(f.a.getNotes()[0].getMidiNote() - 60.0f) < 1e-4f);
    CHECK(std::abs(f.b.getNotes()[0].getMidiNote() - 61.0f) < 1e-4f);
    CHECK(f.strength(f.a) == 100);
    CHECK(std::abs(f.a.getNotes()[0].getPitchDrift() - 0.6f) < 1e-4f);
  }
  {
    Note note(0, 10, 60.4f); // A regular pitch edit becomes uncorrected source.
    note.setMacroPitchCenter(50);
    note.setMidiNote(61.0f);
    CHECK(note.getMacroPitchCenter() == 0);
  }
  std::cout << "Pitch macro render tests passed\n";
}
