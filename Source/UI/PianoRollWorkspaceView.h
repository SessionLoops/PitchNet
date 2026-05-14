#pragma once

#include "../JuceHeader.h"
#include "../Models/Project.h"
#include "PianoRollComponent.h"
#include "PianoRoll/OverviewPanel.h"
#include "Workspace/RoundedCard.h"

class PianoRollWorkspaceView : public juce::Component, private juce::Timer {
public:
  explicit PianoRollWorkspaceView(PianoRollComponent &pianoRoll);

  void paint(juce::Graphics &g) override;
  void resized() override;
  void timerCallback() override;

  void setProject(Project *project);
  void refreshOverview();
  void setShowSegmentsDebug(bool show);
  PianoRollComponent &getPianoRoll() { return pianoRoll; }

private:
  void updateOverviewVisibility();

  PianoRollComponent &pianoRoll;
  OverviewPanel overviewPanel;

  RoundedCard pianoCard;
  RoundedCard overviewCard;

  juce::TextButton overviewToggleButton{"[]"};
  bool overviewVisible = true;

  juce::Slider zoomXSlider;
  juce::Slider zoomYSlider;
  juce::Rectangle<float> zoomXBg;
  juce::Rectangle<float> zoomYBg;
  juce::Rectangle<float> toggleBg;

  static constexpr int overviewHeight = 60;
  static constexpr int cardGap = 6;
  static constexpr int toggleSize = 22;
  static constexpr int toggleMargin = 6;
  static constexpr int zoomSliderWidth = 18;
  static constexpr int zoomSliderHeight = 88;
  static constexpr int zoomSliderLength = 110;
  static constexpr int zoomGap = 6;
  static constexpr int zoomBgPadding = 5;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PianoRollWorkspaceView)
};
