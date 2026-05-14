#include "PianoRollWorkspaceView.h"
#include "../Utils/UI/Theme.h"
#include "../Utils/Constants.h"
#include <cmath>

PianoRollWorkspaceView::PianoRollWorkspaceView(PianoRollComponent &piano)
    : pianoRoll(piano)
{
  pianoCard.setPadding(0);
  pianoCard.setCornerRadius(10.0f);
  pianoCard.setBorderColour(APP_COLOR_BORDER_SUBTLE.withAlpha(0.35f));
  pianoCard.setContentComponent(&pianoRoll);

  overviewCard.setPadding(0);
  overviewCard.setCornerRadius(10.0f);
  overviewCard.setBorderColour(APP_COLOR_BORDER_SUBTLE.withAlpha(0.35f));
  overviewCard.setContentComponent(&overviewPanel);
  overviewPanel.setDrawBackground(false);

  overviewPanel.getViewState = [this]()
  {
    OverviewPanel::ViewState state;
    auto *project = pianoRoll.getProject();
    state.totalTime = project ? project->getAudioData().getDuration() : 0.0;
    state.scrollX = pianoRoll.getScrollX();
    state.pixelsPerSecond = pianoRoll.getPixelsPerSecond();
    state.visibleWidth = pianoRoll.getVisibleContentWidth();
    return state;
  };
  overviewPanel.onScrollXChanged = [this](double x)
  {
    pianoRoll.setScrollX(x);
    if (pianoRoll.onScrollChanged)
      pianoRoll.onScrollChanged(x);
  };
  overviewPanel.onZoomChanged = [this](float pps)
  {
    pianoRoll.setPixelsPerSecond(pps, false);
    if (pianoRoll.onZoomChanged)
      pianoRoll.onZoomChanged(pianoRoll.getPixelsPerSecond());
  };
  zoomXSlider.setSliderStyle(juce::Slider::LinearHorizontal);
  zoomXSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
  zoomXSlider.setRange(MIN_PIXELS_PER_SECOND, MAX_PIXELS_PER_SECOND, 0.1);
  zoomXSlider.setValue(pianoRoll.getPixelsPerSecond(),
                       juce::dontSendNotification);
  zoomXSlider.setColour(juce::Slider::trackColourId,
                        APP_COLOR_SURFACE_RAISED);
  zoomXSlider.setColour(juce::Slider::thumbColourId, APP_COLOR_PRIMARY);
  zoomXSlider.onValueChange = [this]()
  {
    pianoRoll.setPixelsPerSecond(static_cast<float>(zoomXSlider.getValue()),
                                 true);
    if (pianoRoll.onZoomChanged)
      pianoRoll.onZoomChanged(pianoRoll.getPixelsPerSecond());
  };

  zoomYSlider.setSliderStyle(juce::Slider::LinearVertical);
  zoomYSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
  zoomYSlider.setRange(MIN_PIXELS_PER_SEMITONE, MAX_PIXELS_PER_SEMITONE, 0.1);
  zoomYSlider.setValue(pianoRoll.getPixelsPerSemitone(),
                       juce::dontSendNotification);
  zoomYSlider.setColour(juce::Slider::trackColourId,
                        APP_COLOR_SURFACE_RAISED);
  zoomYSlider.setColour(juce::Slider::thumbColourId, APP_COLOR_PRIMARY);
  zoomYSlider.onValueChange = [this]()
  {
    pianoRoll.setPixelsPerSemitone(static_cast<float>(zoomYSlider.getValue()),
                                   static_cast<float>(pianoRoll.getVisibleContentHeight()) *
                                       0.5f);
  };

  overviewToggleButton.setClickingTogglesState(true);
  overviewToggleButton.setToggleState(overviewVisible,
                                      juce::dontSendNotification);
  overviewToggleButton.setColour(juce::TextButton::buttonColourId,
                                 APP_COLOR_SURFACE.withAlpha(0.9f));
  overviewToggleButton.setColour(juce::TextButton::buttonOnColourId,
                                 APP_COLOR_PRIMARY.withAlpha(0.9f));
  overviewToggleButton.setColour(juce::TextButton::textColourOffId,
                                 APP_COLOR_TEXT_PRIMARY);
  overviewToggleButton.setColour(juce::TextButton::textColourOnId,
                                 juce::Colours::white);
  overviewToggleButton.onClick = [this]()
  {
    overviewVisible = overviewToggleButton.getToggleState();
    updateOverviewVisibility();
    resized();
  };

  addAndMakeVisible(pianoCard);
  addAndMakeVisible(overviewCard);
  addAndMakeVisible(overviewToggleButton);
  addAndMakeVisible(zoomXSlider);
  addAndMakeVisible(zoomYSlider);

  updateOverviewVisibility();
  startTimerHz(10);
}

void PianoRollWorkspaceView::paint(juce::Graphics &g)
{
  const auto bg = APP_COLOR_SURFACE.withAlpha(0.85f);
  const auto border = APP_COLOR_BORDER_SUBTLE.withAlpha(0.7f);

  g.setColour(bg);
  g.fillRoundedRectangle(zoomXBg, 6.0f);
  g.fillRoundedRectangle(zoomYBg, 6.0f);
  g.fillRoundedRectangle(toggleBg, 6.0f);

  g.setColour(border);
  g.drawRoundedRectangle(zoomXBg, 6.0f, 1.0f);
  g.drawRoundedRectangle(zoomYBg, 6.0f, 1.0f);
  g.drawRoundedRectangle(toggleBg, 6.0f, 1.0f);
}

void PianoRollWorkspaceView::resized()
{
  auto bounds = getLocalBounds();

  if (overviewVisible)
  {
    auto overviewBounds = bounds.removeFromBottom(overviewHeight);
    bounds.removeFromBottom(cardGap);
    overviewCard.setBounds(overviewBounds);
  }
  else
  {
    overviewCard.setBounds({});
  }

  pianoCard.setBounds(bounds);

  auto overlay = pianoCard.getBounds();
  const int sliderBottom = overlay.getBottom() - toggleMargin;
  const int sliderRight = overlay.getRight() - toggleMargin;
  const int zoomXHeight = 20;
  const int zoomXTop = sliderBottom - zoomXHeight;
  const int zoomYBottom = zoomXTop - zoomGap;
  const int zoomCornerGap = 6;

  auto zoomXRect = juce::Rectangle<int>(
      sliderRight - zoomSliderLength - toggleSize - zoomCornerGap, zoomXTop,
      zoomSliderLength, zoomXHeight);
  auto zoomYRect = juce::Rectangle<int>(
      sliderRight - zoomSliderWidth, zoomYBottom - zoomSliderHeight,
      zoomSliderWidth, zoomSliderHeight);

  zoomXSlider.setBounds(zoomXRect);
  zoomYSlider.setBounds(zoomYRect);

  overviewToggleButton.setBounds(
      zoomXRect.getRight() + zoomCornerGap,
      zoomXRect.getY() + (zoomXHeight - toggleSize) / 2, toggleSize, toggleSize);

  zoomXBg = zoomXRect.toFloat().expanded(static_cast<float>(zoomBgPadding),
                                         static_cast<float>(zoomBgPadding));
  zoomYBg = zoomYRect.toFloat().expanded(static_cast<float>(zoomBgPadding),
                                         static_cast<float>(zoomBgPadding));
  toggleBg = overviewToggleButton.getBounds().toFloat().expanded(
      static_cast<float>(zoomBgPadding), static_cast<float>(zoomBgPadding));
}

void PianoRollWorkspaceView::setProject(Project *project)
{
  overviewPanel.setProject(project);
}

void PianoRollWorkspaceView::refreshOverview()
{
  if (overviewVisible)
    overviewPanel.repaint();
}

void PianoRollWorkspaceView::setShowSegmentsDebug(bool show)
{
  overviewPanel.setShowSegmentsDebug(show);
}

void PianoRollWorkspaceView::updateOverviewVisibility()
{
  overviewCard.setVisible(overviewVisible);
  overviewPanel.setVisible(overviewVisible);
}

void PianoRollWorkspaceView::timerCallback()
{
  const float pps = pianoRoll.getPixelsPerSecond();
  if (std::abs(zoomXSlider.getValue() - pps) > 0.05)
    zoomXSlider.setValue(pps, juce::dontSendNotification);

  const float ppsY = pianoRoll.getPixelsPerSemitone();
  if (std::abs(zoomYSlider.getValue() - ppsY) > 0.05)
    zoomYSlider.setValue(ppsY, juce::dontSendNotification);
}
