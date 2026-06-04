#include "PianoRollWorkspaceView.h"
#include "../Utils/UI/Theme.h"
#include "../Utils/Constants.h"
#include "BinaryData.h"
#include <cmath>

void PianoRollWorkspaceView::FloatingZoomSliderLookAndFeel::drawLinearSlider(
    juce::Graphics &g,
    int x,
    int y,
    int width,
    int height,
    float sliderPos,
    float minSliderPos,
    float maxSliderPos,
    const juce::Slider::SliderStyle style,
    juce::Slider &slider)
{
  juce::ignoreUnused(minSliderPos, maxSliderPos, slider);

  const auto bounds = juce::Rectangle<float>(static_cast<float>(x),
                                             static_cast<float>(y),
                                             static_cast<float>(width),
                                             static_cast<float>(height));
  const auto thumbRadius = 4.5f;
  const auto trackHeight = 2.0f;

  if (style == juce::Slider::LinearVertical)
  {
    const auto track = juce::Rectangle<float>(
        bounds.getCentreX() - trackHeight * 0.5f,
        bounds.getY(),
        trackHeight,
        juce::jmax(0.0f, bounds.getHeight()));

    g.setColour(juce::Colour(0xFF494949));
    g.fillRoundedRectangle(track, trackHeight * 0.5f);

    const auto clampedSliderPos = juce::jlimit(track.getY() + thumbRadius,
                                               track.getBottom() - thumbRadius,
                                               sliderPos);
    g.setColour(juce::Colour(0xFF9B9B9B));
    g.fillEllipse(bounds.getCentreX() - thumbRadius,
                  clampedSliderPos - thumbRadius,
                  thumbRadius * 2.0f,
                  thumbRadius * 2.0f);
    return;
  }

  const auto track = juce::Rectangle<float>(bounds.getX(),
                                           bounds.getCentreY() - trackHeight * 0.5f,
                                           juce::jmax(0.0f, bounds.getWidth()),
                                           trackHeight);

  g.setColour(juce::Colour(0xFF494949));
  g.fillRoundedRectangle(track, trackHeight * 0.5f);

  const auto clampedSliderPos = juce::jlimit(track.getX() + thumbRadius,
                                             track.getRight() - thumbRadius,
                                             sliderPos);
  g.setColour(juce::Colour(0xFF9B9B9B));
  g.fillEllipse(clampedSliderPos - thumbRadius,
                bounds.getCentreY() - thumbRadius,
                thumbRadius * 2.0f,
                thumbRadius * 2.0f);
}

void PianoRollWorkspaceView::FloatingControlBackground::paint(juce::Graphics &g)
{
  g.setColour(juce::Colour(0xFF0D0B0B));
  g.fillRoundedRectangle(getLocalBounds().toFloat(), 5.0f);
}

PianoRollWorkspaceView::PianoRollWorkspaceView(PianoRollComponent &piano)
    : pianoRoll(piano)
{
  pianoCard.setPadding(0);
  pianoCard.setCornerRadius(0.0f);
  pianoCard.setBorderColour(juce::Colours::transparentBlack);
  pianoCard.setContentComponent(&pianoRoll);

  overviewCard.setPadding(0);
  overviewCard.setCornerRadius(0.0f);
  overviewCard.setBackgroundColour(juce::Colour(0xFF191717u));
  overviewCard.setBorderColour(juce::Colour(0xFF0D0B0Bu));
  overviewCard.setContentComponent(&overviewPanel);
  overviewPanel.setDrawBackground(false);

  overviewPanel.getViewState = [this]()
  {
    OverviewPanel::ViewState state;
    auto *project = pianoRoll.getProject();
    state.totalTime = project ? project->getAudioData().getDuration() : 0.0;
    state.cursorTime = pianoRoll.getCursorTime();
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
  zoomXSlider.setLookAndFeel(&floatingZoomSliderLookAndFeel);
  zoomXSlider.setRange(MIN_PIXELS_PER_SECOND, MAX_PIXELS_PER_SECOND, 0.1);
  zoomXSlider.setValue(pianoRoll.getPixelsPerSecond(),
                       juce::dontSendNotification);
  zoomXSlider.onValueChange = [this]()
  {
    pianoRoll.setPixelsPerSecond(static_cast<float>(zoomXSlider.getValue()),
                                 true);
    if (pianoRoll.onZoomChanged)
      pianoRoll.onZoomChanged(pianoRoll.getPixelsPerSecond());
  };

  zoomYSlider.setSliderStyle(juce::Slider::LinearVertical);
  zoomYSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
  zoomYSlider.setLookAndFeel(&floatingZoomSliderLookAndFeel);
  zoomYSlider.setRange(MIN_PIXELS_PER_SEMITONE, MAX_PIXELS_PER_SEMITONE, 0.1);
  zoomYSlider.setValue(pianoRoll.getPixelsPerSemitone(),
                       juce::dontSendNotification);
  zoomYSlider.onValueChange = [this]()
  {
    pianoRoll.setPixelsPerSemitone(static_cast<float>(zoomYSlider.getValue()),
                                   static_cast<float>(pianoRoll.getVisibleContentHeight()) *
                                       0.5f);
  };

  overviewToggleButton.setImage(juce::ImageFileFormat::loadFrom(
      BinaryData::thumbnail_png, static_cast<size_t>(BinaryData::thumbnail_pngSize)));
  overviewToggleButton.setClickingTogglesState(true);
  overviewToggleButton.setToggleState(overviewVisible,
                                      juce::dontSendNotification);
  overviewToggleButton.onClick = [this]()
  {
    overviewVisible = overviewToggleButton.getToggleState();
    updateOverviewVisibility();
  };

  addAndMakeVisible(pianoCard);
  addAndMakeVisible(overviewCard);
  addAndMakeVisible(zoomXBackground);
  addAndMakeVisible(zoomYBackground);
  addAndMakeVisible(overviewToggleButton);
  addAndMakeVisible(zoomXSlider);
  addAndMakeVisible(zoomYSlider);

  overviewCard.setVisible(false);
  overviewPanel.setVisible(false);
  pianoRoll.setHorizontalScrollBarVisible(true);
  startTimerHz(30);
}

PianoRollWorkspaceView::~PianoRollWorkspaceView()
{
  zoomXSlider.setLookAndFeel(nullptr);
  zoomYSlider.setLookAndFeel(nullptr);
}

void PianoRollWorkspaceView::paint(juce::Graphics &g)
{
  juce::ignoreUnused(g);
}

void PianoRollWorkspaceView::resized()
{
  auto bounds = getLocalBounds();

  const float progress = juce::jlimit(0.0f, 1.0f, overviewAnimationProgress);
  if (progress > 0.001f)
  {
    const int animatedOverviewHeight =
        static_cast<int>(std::round(static_cast<float>(overviewHeight) * progress));
    const int animatedGap =
        static_cast<int>(std::round(static_cast<float>(cardGap) * progress));
    const auto fullBounds = bounds;

    bounds.removeFromBottom(animatedOverviewHeight + animatedGap);

    auto overviewBounds = juce::Rectangle<int>(
        fullBounds.getX(),
        fullBounds.getBottom() - animatedOverviewHeight,
        fullBounds.getWidth(), overviewHeight);
    overviewBounds.removeFromLeft(thumbnailOuterHorizontalPadding);
    overviewBounds.removeFromRight(thumbnailOuterHorizontalPadding);
    overviewBounds.removeFromBottom(thumbnailOuterBottomPadding);
    overviewCard.setBounds(overviewBounds);
  }
  else
  {
    overviewCard.setBounds({});
  }

  pianoCard.setBounds(bounds);

  auto overlay = pianoCard.getBounds();
  const int sliderBottom = overlay.getBottom() - floatingControlInset - 3;
  const int sliderRight = overlay.getRight() - floatingControlInset;
  const int zoomXHeight = floatingSliderThickness;
  const int zoomXTop = sliderBottom - zoomXHeight;
  const int zoomYBottom = zoomXTop - zoomGap;
  const int zoomCornerGap = 6;

  auto zoomXRect = juce::Rectangle<int>(
      sliderRight - zoomSliderLength - toggleSize - zoomCornerGap, zoomXTop,
      zoomSliderLength, zoomXHeight)
      .translated(-8, 0);
  auto zoomYRect = juce::Rectangle<int>(
      sliderRight - zoomSliderWidth, zoomYBottom - zoomSliderHeight,
      zoomSliderWidth, zoomSliderHeight)
      .translated(-5, -8);

  zoomXSlider.setBounds(zoomXRect);
  zoomYSlider.setBounds(zoomYRect);

  overviewToggleButton.setBounds(
      zoomXRect.getRight() + zoomCornerGap + 6,
      zoomXRect.getY() + (zoomXHeight - toggleSize) / 2, toggleSize, toggleSize);

  zoomXBg = zoomXRect.toFloat();
  zoomYBg = zoomYRect.toFloat();
  toggleBg = overviewToggleButton.getBounds().toFloat();

  zoomXBackground.setBounds(zoomXBg.toNearestInt());
  zoomYBackground.setBounds(zoomYBg.toNearestInt());
}

void PianoRollWorkspaceView::setProject(Project *project)
{
  overviewPanel.setProject(project);
}

void PianoRollWorkspaceView::refreshOverview()
{
  if (overviewVisible)
    overviewPanel.invalidateThumbnailCache();
}

void PianoRollWorkspaceView::setShowSegmentsDebug(bool show)
{
  overviewPanel.setShowSegmentsDebug(show);
}

void PianoRollWorkspaceView::updateOverviewVisibility()
{
  overviewAnimationStartProgress = overviewAnimationProgress;
  overviewAnimationStartMs = juce::Time::getMillisecondCounter();
  overviewAnimationActive = true;

  if (overviewVisible)
  {
    overviewCard.setVisible(true);
    overviewPanel.setVisible(true);
    pianoRoll.setHorizontalScrollBarVisible(false);
  }

  resized();
  repaint();
}

void PianoRollWorkspaceView::updateOverviewAnimation()
{
  if (!overviewAnimationActive)
    return;

  const float target = overviewVisible ? 1.0f : 0.0f;
  const auto now = juce::Time::getMillisecondCounter();
  const float elapsed =
      static_cast<float>(now - overviewAnimationStartMs) /
      static_cast<float>(overviewAnimationMs);
  const float t = juce::jlimit(0.0f, 1.0f, elapsed);
  const float eased = 1.0f - std::pow(1.0f - t, 3.0f);

  overviewAnimationProgress =
      overviewAnimationStartProgress +
      (target - overviewAnimationStartProgress) * eased;

  if (t >= 1.0f)
  {
    overviewAnimationProgress = target;
    overviewAnimationActive = false;

    if (!overviewVisible)
    {
      overviewCard.setVisible(false);
      overviewPanel.setVisible(false);
      pianoRoll.setHorizontalScrollBarVisible(true);
    }
  }

  resized();
  repaint();
}

void PianoRollWorkspaceView::timerCallback()
{
  updateOverviewAnimation();

  const float pps = pianoRoll.getPixelsPerSecond();
  if (std::abs(zoomXSlider.getValue() - pps) > 0.05)
    zoomXSlider.setValue(pps, juce::dontSendNotification);

  const float ppsY = pianoRoll.getPixelsPerSemitone();
  if (std::abs(zoomYSlider.getValue() - ppsY) > 0.05)
    zoomYSlider.setValue(ppsY, juce::dontSendNotification);

  const double cursorTime = pianoRoll.getCursorTime();
  if (overviewVisible && std::abs(lastOverviewCursorTime - cursorTime) > 0.0001)
  {
    const double previousCursorTime = lastOverviewCursorTime;
    lastOverviewCursorTime = cursorTime;
    overviewPanel.repaintPlayhead(previousCursorTime, cursorTime);
  }
}
