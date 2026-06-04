#pragma once

#include "../JuceHeader.h"
#include "../Models/Project.h"
#include "Buttons.h"
#include "PianoRollComponent.h"
#include "PianoRoll/OverviewPanel.h"
#include "Workspace/RoundedCard.h"

class PianoRollWorkspaceView : public juce::Component, private juce::Timer {
public:
  explicit PianoRollWorkspaceView(PianoRollComponent &pianoRoll);
  ~PianoRollWorkspaceView() override;

  void paint(juce::Graphics &g) override;
  void resized() override;
  void timerCallback() override;

  void setProject(Project *project);
  void refreshOverview();
  void setShowSegmentsDebug(bool show);
  PianoRollComponent &getPianoRoll() { return pianoRoll; }

private:
  class FloatingZoomSliderLookAndFeel final : public juce::LookAndFeel_V4
  {
  public:
    void drawLinearSlider(juce::Graphics &g,
                          int x,
                          int y,
                          int width,
                          int height,
                          float sliderPos,
                          float minSliderPos,
                          float maxSliderPos,
                          const juce::Slider::SliderStyle style,
                          juce::Slider &slider) override;
  };

  class FloatingControlBackground final : public juce::Component
  {
  public:
    void paint(juce::Graphics &g) override;
  };

  void updateOverviewVisibility();
  void updateOverviewAnimation();

  PianoRollComponent &pianoRoll;
  OverviewPanel overviewPanel;

  RoundedCard pianoCard;
  RoundedCard overviewCard;

  ToggleButton overviewToggleButton;
  bool overviewVisible = false;

  FloatingZoomSliderLookAndFeel floatingZoomSliderLookAndFeel;
  FloatingControlBackground zoomXBackground;
  FloatingControlBackground zoomYBackground;
  juce::Slider zoomXSlider;
  juce::Slider zoomYSlider;
  juce::Rectangle<float> zoomXBg;
  juce::Rectangle<float> zoomYBg;
  juce::Rectangle<float> toggleBg;
  double lastOverviewCursorTime = -1.0;
  float overviewAnimationProgress = 0.0f;
  float overviewAnimationStartProgress = 0.0f;
  juce::uint32 overviewAnimationStartMs = 0;
  bool overviewAnimationActive = false;

  static constexpr int overviewHeight = 84;
  static constexpr int overviewAnimationMs = 220;
  static constexpr int thumbnailOuterHorizontalPadding = 7;
  static constexpr int thumbnailOuterBottomPadding = 6;
  static constexpr int cardGap = 6;
  static constexpr int toggleSize = 22;
  static constexpr int floatingControlInset = 15;
  static constexpr int floatingSliderThickness = 16;
  static constexpr int zoomSliderWidth = floatingSliderThickness;
  static constexpr int zoomSliderHeight = 88;
  static constexpr int zoomSliderLength = 110;
  static constexpr int zoomGap = 6;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PianoRollWorkspaceView)
};
