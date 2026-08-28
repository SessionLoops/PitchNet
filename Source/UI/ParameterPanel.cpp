#include "ParameterPanel.h"
#include "../Utils/Localization.h"
#include <array>
#include <cmath>

namespace
{
constexpr bool kShowPitchCard = true;

// =========================================================================
// LAYOUT METRICS
//
// resized() lays the cards out strictly top down at these fixed sizes, so the
// height the panel needs is a sum of them rather than a function of the space
// it is given. kPreferredPanelHeight is that sum; the hosting panel uses it to
// decide when to scroll. resized() asserts the two agree, so changing a row
// height here without updating the sum is caught in a debug build.
// =========================================================================
constexpr int kCardPadX = 5;   // horizontal padding outside cards
constexpr int kCardPadY = 6;   // vertical padding outside cards
constexpr int kCardGap = 6;    // gap between cards
constexpr int kInnerPadX = 10; // padding inside each card
constexpr int kInnerPadY = 8;  // padding inside each card
constexpr int kRowGap = 5;
constexpr int kColumnGap = 6;

constexpr int kSectionLabelHeight = 20;
constexpr int kSectionLabelGap = kRowGap + 2;
constexpr int kRadioRowHeight = 22;         // Rendering / Pitch mode toggles
constexpr int kTimelineModeRowHeight = 32;  // Beats | Time
constexpr int kControlRowHeight = 26;       // label + control rows
constexpr int kBrightnessRowHeight = 28;
constexpr int kTracksRowHeight = 26;           // region selector row

constexpr int kSynthesisCardHeight =
    kInnerPadY + kSectionLabelHeight + kSectionLabelGap + kRadioRowHeight + kInnerPadY;
constexpr int kTimeCardHeight =
    kInnerPadY + kSectionLabelHeight + kSectionLabelGap + kTimelineModeRowHeight +
    (kRowGap + 1) + kControlRowHeight + kRowGap + kControlRowHeight + kInnerPadY;
constexpr int kPitchCardHeight =
    kInnerPadY + kSectionLabelHeight + kSectionLabelGap + kRadioRowHeight +
    (kRowGap + 1) + kControlRowHeight + (kRowGap + 1) + kControlRowHeight + kInnerPadY;
constexpr int kBrightnessCardHeight =
    kInnerPadY + kSectionLabelHeight + kSectionLabelGap + kBrightnessRowHeight + kInnerPadY;
constexpr int kTracksCardHeight =
    kInnerPadY + kSectionLabelHeight + kSectionLabelGap + kTracksRowHeight + kInnerPadY;

constexpr int kPreferredPanelHeight =
    kCardPadY * 2 + kCardGap * 3 + kSynthesisCardHeight + kTimeCardHeight +
    kPitchCardHeight + kBrightnessCardHeight;

// The Tracks card only exists in ARA plugin mode, so it is not part of the
// base sum - it adds itself, gap included, when it is showing.
constexpr int kTracksCardExtraHeight = kCardGap + kTracksCardHeight;

constexpr const char* kNoRegionsText = "No regions";
constexpr const char* kNoRegionSelectedText = "Select region";

struct TimelineBeatOption
{
    int numerator = 4;
    int denominator = 4;
    const char* label = "4/4";
};

struct TimelineGridOption
{
    TimelineGridDivision division = TimelineGridDivision::Quarter;
    const char* label = "1/4";
};

constexpr std::array<TimelineBeatOption, 6> kTimelineBeatOptions {{
    { 3, 4, "3/4" },
    { 4, 4, "4/4" },
    { 5, 4, "5/4" },
    { 6, 8, "6/8" },
    { 7, 8, "7/8" },
    { 12, 8, "12/8" }
}};

constexpr std::array<TimelineGridOption, 6> kTimelineGridOptions {{
    { TimelineGridDivision::Whole, "1/1" },
    { TimelineGridDivision::Half, "1/2" },
    { TimelineGridDivision::Quarter, "1/4" },
    { TimelineGridDivision::Eighth, "1/8" },
    { TimelineGridDivision::Sixteenth, "1/16" },
    { TimelineGridDivision::ThirtySecond, "1/32" }
}};

int normalizeTimelineBeatDenominator(int denominator)
{
    denominator = juce::jlimit(1, 32, denominator);
    int normalized = 1;
    while (normalized < denominator)
        normalized <<= 1;
    const int lower = normalized >> 1;
    if (lower >= 1 && (denominator - lower) < (normalized - denominator))
        normalized = lower;
    return juce::jlimit(1, 32, normalized);
}

juce::String getTimelineBeatLabel(int numerator, int denominator)
{
    const int normalizedNumerator = juce::jlimit(1, 32, numerator);
    const int normalizedDenominator = normalizeTimelineBeatDenominator(denominator);
    return juce::String(normalizedNumerator) + "/" + juce::String(normalizedDenominator);
}

juce::String getTimelineGridLabel(TimelineGridDivision division)
{
    for (const auto& option : kTimelineGridOptions)
        if (option.division == division)
            return option.label;
    return "1/4";
}

juce::String getDragSnapModeLabel(DragSnapMode mode)
{
    return mode == DragSnapMode::Scale ? "Scale" : "Chromatic";
}

class PitchPopupLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    PitchPopupLookAndFeel()
    {
        setColour(juce::PopupMenu::backgroundColourId, juce::Colours::transparentBlack);
        setColour(juce::PopupMenu::textColourId, APP_COLOR_TEXT_PRIMARY);
        setColour(juce::PopupMenu::highlightedBackgroundColourId,
                  juce::Colour(0xFF171717u));
        setColour(juce::PopupMenu::highlightedTextColourId, APP_COLOR_TEXT_PRIMARY);
    }

    void drawPopupMenuBackground(juce::Graphics& g, int width, int height) override
    {
        const auto bounds = juce::Rectangle<float>(
            0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
        const auto area = bounds.reduced(0.5f);
        juce::Path shape;
        shape.addRoundedRectangle(area, 9.0f);

        g.setColour(juce::Colour(0xFF30302Eu));
        g.fillPath(shape);
        g.setColour(juce::Colour(0xFF3E3E3Eu));
        g.strokePath(shape, juce::PathStrokeType(1.0f));
    }

    void drawResizableFrame(juce::Graphics&, int, int,
                            const juce::BorderSize<int>&) override
    {
        // PopupMenu draws this frame when a parent component is provided.
        // The default implementation is a square outline that breaks rounded corners.
    }
};

PitchPopupLookAndFeel& getPitchPopupLookAndFeel()
{
    static PitchPopupLookAndFeel lookAndFeel;
    return lookAndFeel;
}

class HoverMenuItemComponent final : public juce::PopupMenu::CustomComponent
{
public:
    HoverMenuItemComponent(juce::String text, bool selected, std::function<void()> hoverCallback = {})
        : juce::PopupMenu::CustomComponent(true),
          itemText(std::move(text)),
          isSelected(selected),
          onHover(std::move(hoverCallback))
    {
        setOpaque(false);
    }

    void getIdealSize(int& idealWidth, int& idealHeight) override
    {
        const int textWidth = juce::GlyphArrangement::getStringWidthInt(
            AppFont::getFont(14.0f), itemText);
        idealWidth = textWidth + 44; // matching 22 px left/right insets
        idealHeight = 26;
    }

    void paint(juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat();
        if (isItemHighlighted())
        {
            g.setColour(juce::Colour(0xFF171717u));
            g.fillRoundedRectangle(area.reduced(2.0f, 1.0f), 5.0f);
        }

        if (isSelected)
        {
            g.setColour(juce::Colour(0xFFEFEFEFu));
            g.fillEllipse(8.0f, area.getCentreY() - 3.5f, 7.0f, 7.0f);
        }

        g.setColour(APP_COLOR_TEXT_PRIMARY);
        g.setFont(AppFont::getFont(14.0f));
        g.drawText(itemText, getLocalBounds().withTrimmedLeft(22),
                   juce::Justification::centredLeft, true);
    }

    void mouseEnter(const juce::MouseEvent& e) override
    {
        juce::PopupMenu::CustomComponent::mouseEnter(e);
        if (onHover)
            onHover();
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        juce::PopupMenu::CustomComponent::mouseMove(e);
        if (onHover)
            onHover();
    }

private:
    juce::String itemText;
    bool isSelected = false;
    std::function<void()> onHover;
};
}

ParameterPanel::ParameterPanel()
{
    addAndMakeVisible(pitchSectionLabel);
    pitchSectionLabel.setColour(juce::Label::textColourId, juce::Colour(0xFF9B9B9Bu));
    pitchSectionLabel.setFont(AppFont::getBoldFont(16.0f));
    addAndMakeVisible(timeSectionLabel);
    timeSectionLabel.setColour(juce::Label::textColourId, juce::Colour(0xFF9B9B9Bu));
    timeSectionLabel.setFont(AppFont::getBoldFont(16.0f));
    addAndMakeVisible(synthesisSectionLabel);
    synthesisSectionLabel.setColour(juce::Label::textColourId, juce::Colour(0xFF9B9B9Bu));
    synthesisSectionLabel.setFont(AppFont::getBoldFont(16.0f));
    addAndMakeVisible(brightnessSectionLabel);
    brightnessSectionLabel.setColour(juce::Label::textColourId,
                                     juce::Colour(0xFF9B9B9Bu));
    brightnessSectionLabel.setFont(AppFont::getBoldFont(16.0f));

    // Tracks card: added but not shown. Standalone and non-ARA plugin mode
    // have no playback regions, so the card only appears once a host tells us
    // otherwise (see setTracksCardVisible).
    addChildComponent(tracksSectionLabel);
    tracksSectionLabel.setColour(juce::Label::textColourId, juce::Colour(0xFF9B9B9Bu));
    tracksSectionLabel.setFont(AppFont::getBoldFont(16.0f));
    addChildComponent(tracksSelectorButton);
    tracksSelectorButton.addListener(this);
    tracksSelectorButton.setEnabled(false);

    for (auto* toggle : { &snapToSemitonesToggle, &timelineSnapCycleToggle })
    {
        addAndMakeVisible(toggle);
        toggle->setClickingTogglesState(true);
        toggle->addListener(this);
    }

    for (auto* radio : { &chromaticToggle, &scaleToggle,
                         &beatsTimelineToggle, &timeTimelineToggle,
                         &vocoderEngineToggle, &psolaEngineToggle })
    {
        addAndMakeVisible(radio);
        radio->setClickingTogglesState(true);
        radio->addListener(this);
    }

    // The button text stays plain language; the algorithm names live here, so
    // they are discoverable without putting "PC-NSF-HiFiGAN" in a 55px label.
    vocoderEngineToggle.setTooltip(
        "Neural vocoder (PC-NSF-HiFiGAN)\n"
        "Rebuilds the voice from its spectrum. Handles large pitch moves best, "
        "and reshapes tone across the whole edited region.");
    psolaEngineToggle.setTooltip(
        "Classic DSP (time-domain PSOLA)\n"
        "Shifts the original recording itself. Leaves untouched audio "
        "identical and needs no model, so it is far faster on CPU. Large "
        "pitch moves are rougher.");

    for (auto* label : { &referenceLabel, &timelineBeatLabel,
                         &timelineTempoLabel, &timelineGridLabel })
    {
        addAndMakeVisible(label);
        label->setColour(juce::Label::textColourId, APP_COLOR_TEXT_MUTED);
    }

    for (auto* label : { &referenceLabel, &timelineBeatLabel,
                         &timelineTempoLabel, &timelineGridLabel })
    {
        label->setColour(juce::Label::textColourId, juce::Colour(0xFFE6E6E6u));
        label->setFont(AppFont::getFont(16.0f));
    }
    referenceLabel.setFont(AppFont::getFont(15.0f));

    const std::array<juce::TextButton*, 3> textButtons {{
        &dragSnapModeButton, &timelineBeatButton, &timelineGridButton
    }};
    for (auto* button : textButtons)
    {
        setupTextButton(*button);
        addAndMakeVisible(button);
        button->addListener(this);
    }

    referenceSlider.setRange(430.0, 450.0, 1.0);
    referenceSlider.setValue(pitchReferenceHz, juce::dontSendNotification);
    referenceSlider.setNumDecimalPlacesToDisplay(0);
    referenceSlider.setTextValueSuffix(" Hz");
    referenceSlider.setHideSuffixWhileEditing(true);
    referenceSlider.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 64, 22);
    referenceSlider.setColour(juce::Slider::backgroundColourId,
                              juce::Colour(0xFF30302Eu));
    referenceSlider.setColour(juce::Slider::trackColourId,
                              juce::Colour(0xFF30302Eu));
    referenceSlider.setColour(juce::Slider::textBoxBackgroundColourId,
                              juce::Colours::transparentBlack);
    referenceSlider.setColour(juce::Slider::textBoxOutlineColourId,
                              juce::Colours::transparentBlack);
    referenceSlider.setColour(juce::Slider::textBoxTextColourId,
                              juce::Colour(0xFFE6E6E6u));
    referenceSlider.onValueChange = [this]()
    {
        if (!isUpdating)
            setPitchReferenceInternal(
                static_cast<int>(std::round(referenceSlider.getValue())), true);
    };
    addAndMakeVisible(referenceSlider);

    timelineTempoSlider.setRange(20.0, 300.0, 0.01);
    timelineTempoSlider.setValue(timelineTempoBpm, juce::dontSendNotification);
    timelineTempoSlider.setNumDecimalPlacesToDisplay(2);
    timelineTempoSlider.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 54, 22);
    timelineTempoSlider.setColour(juce::Slider::backgroundColourId,
                                  juce::Colour(0xFF30302Eu));
    timelineTempoSlider.setColour(juce::Slider::trackColourId,
                                  juce::Colour(0xFF30302Eu));
    timelineTempoSlider.setColour(juce::Slider::textBoxBackgroundColourId,
                                  juce::Colours::transparentBlack);
    timelineTempoSlider.setColour(juce::Slider::textBoxOutlineColourId,
                                  juce::Colours::transparentBlack);
    timelineTempoSlider.setColour(juce::Slider::textBoxTextColourId,
                                  juce::Colour(0xFFE6E6E6u));
    timelineTempoSlider.onValueChange = [this]()
    {
        if (!isUpdating)
            setTimelineTempoBpmInternal(timelineTempoSlider.getValue(), true);
    };
    addAndMakeVisible(timelineTempoSlider);

    brightnessSlider.setRange(75.0, 200.0, 1.0);
    brightnessSlider.setValue(uiBrightnessPercent, juce::dontSendNotification);
    brightnessSlider.setNumDecimalPlacesToDisplay(0);
    brightnessSlider.setTextValueSuffix(" %");
    brightnessSlider.onValueChange = [this]()
    {
        if (isUpdating)
            return;

        uiBrightnessPercent = brightnessSlider.getValue();
        if (onUiBrightnessChanged)
            onUiBrightnessChanged(uiBrightnessPercent);
    };
    addAndMakeVisible(brightnessSlider);

    timelineBeatButton.setButtonText(getTimelineBeatLabel(timelineBeatNumerator, timelineBeatDenominator));
    timelineGridButton.setButtonText(getTimelineGridLabel(timelineGridDivision));
    dragSnapModeButton.setButtonText(getDragSnapModeLabel(dragSnapMode));
    dragSnapModeButton.setEnabled(snapToSemitones);
    timelineSnapCycleToggle.setToggleState(timelineSnapCycle, juce::dontSendNotification);

    const std::array<juce::Component*, 7> pitchComponents {{
        &pitchSectionLabel, &chromaticToggle, &scaleToggle,
        &referenceLabel, &referenceSlider,
        &snapToSemitonesToggle, &dragSnapModeButton
    }};
    for (auto* component : pitchComponents)
        component->setVisible(kShowPitchCard);

    refreshModeToggles();
    refreshTimelineModeToggles();
    refreshSynthesisToggles();
}

ParameterPanel::~ParameterPanel()
{
}

void ParameterPanel::setupTextButton(juce::TextButton& button)
{
    button.setColour(juce::TextButton::buttonColourId, APP_COLOR_SURFACE_ALT);
    button.setColour(juce::TextButton::buttonOnColourId, APP_COLOR_SURFACE_RAISED);
    button.setColour(juce::TextButton::textColourOffId, APP_COLOR_TEXT_PRIMARY);
    button.setColour(juce::TextButton::textColourOnId, APP_COLOR_TEXT_PRIMARY);
}

void ParameterPanel::paint(juce::Graphics& g)
{
    // Two separate cards for Pitch and Time sections
    const float radius = 8.0f;

    if (!pitchCardBounds.isEmpty())
    {
        auto pitchRect = pitchCardBounds.toFloat();
        g.setColour(juce::Colour(0xFF171717u));
        g.fillRoundedRectangle(pitchRect, radius);
        g.setColour(APP_COLOR_BORDER.withAlpha(0.4f));
        g.drawRoundedRectangle(pitchRect.reduced(0.5f), radius, 0.75f);
    }

    if (!timeCardBounds.isEmpty())
    {
        auto timeRect = timeCardBounds.toFloat();
        g.setColour(juce::Colour(0xFF171717u));
        g.fillRoundedRectangle(timeRect, radius);
        g.setColour(APP_COLOR_BORDER.withAlpha(0.4f));
        g.drawRoundedRectangle(timeRect.reduced(0.5f), radius, 0.75f);
    }

    if (!synthesisCardBounds.isEmpty())
    {
        auto synthesisRect = synthesisCardBounds.toFloat();
        g.setColour(juce::Colour(0xFF171717u));
        g.fillRoundedRectangle(synthesisRect, radius);
        g.setColour(APP_COLOR_BORDER.withAlpha(0.4f));
        g.drawRoundedRectangle(synthesisRect.reduced(0.5f), radius, 0.75f);
    }

    if (tracksCardVisible && !tracksCardBounds.isEmpty())
    {
        auto tracksRect = tracksCardBounds.toFloat();
        g.setColour(juce::Colour(0xFF171717u));
        g.fillRoundedRectangle(tracksRect, radius);
        g.setColour(APP_COLOR_BORDER.withAlpha(0.4f));
        g.drawRoundedRectangle(tracksRect.reduced(0.5f), radius, 0.75f);
    }

    if (!brightnessCardBounds.isEmpty())
    {
        auto brightnessRect = brightnessCardBounds.toFloat();
        g.setColour(juce::Colour(0xFF171717u));
        g.fillRoundedRectangle(brightnessRect, radius);
        g.setColour(APP_COLOR_BORDER.withAlpha(0.4f));
        g.drawRoundedRectangle(brightnessRect.reduced(0.5f), radius, 0.75f);
    }
}

void ParameterPanel::resized()
{
    auto outerBounds = getLocalBounds();

    // Metrics live at the top of this file so getPreferredHeight() can sum them.
    constexpr int cardPadY = kCardPadY;
    constexpr int cardGap = kCardGap;
    constexpr int innerPadX = kInnerPadX;
    constexpr int innerPadY = kInnerPadY;
    constexpr int rowGap = kRowGap;
    constexpr int columnGap = kColumnGap;

    auto cardArea = outerBounds.reduced(kCardPadX, cardPadY);
    // =========================================================================
    // SYNTHESIS CARD
    // =========================================================================
    const int synthesisCardStart = cardArea.getY();
    auto bounds = juce::Rectangle<int>(cardArea.getX() + innerPadX,
                                       synthesisCardStart + innerPadY,
                                       cardArea.getWidth() - innerPadX * 2,
                                       cardArea.getBottom() - synthesisCardStart - innerPadY * 2);

    synthesisSectionLabel.setBounds(bounds.removeFromTop(kSectionLabelHeight));
    bounds.removeFromTop(kSectionLabelGap);

    // Both options stay on one row, sharing it in proportion to how wide each
    // label actually is rather than in half. Measured with GlyphArrangement
    // against the real typeface at the real size, so this holds through a font
    // change, DPI scaling or translation - an estimate would not.
    //
    // Splitting the space left after the two radio circles, rather than the
    // row itself, is what makes the proportion work: each label then gets the
    // same fraction of what it needs, so if the pair cannot fit at full size
    // they shrink together instead of the long one being squashed while the
    // short one keeps its size. RadioButton paints through drawFittedText,
    // which only ever shrinks, so any slack simply renders at full size.
    auto engineRow = bounds.removeFromTop(kRadioRowHeight);
    const auto engineFont = AppFont::getFont(15.0f);
    constexpr int radioInset = 30; // circle plus gap, as paintButton trims

    const int vocoderTextWidth = juce::GlyphArrangement::getStringWidthInt(
        engineFont, vocoderEngineToggle.getButtonText());
    const int psolaTextWidth = juce::GlyphArrangement::getStringWidthInt(
        engineFont, psolaEngineToggle.getButtonText());
    const int totalTextWidth = juce::jmax(1, vocoderTextWidth + psolaTextWidth);

    const int sharedTextWidth =
        juce::jmax(0, engineRow.getWidth() - columnGap - 2 * radioInset);
    const int vocoderWidth =
        radioInset + (sharedTextWidth * vocoderTextWidth) / totalTextWidth;

    auto vocoderArea = engineRow.removeFromLeft(vocoderWidth);
    engineRow.removeFromLeft(columnGap);
    vocoderEngineToggle.setBounds(vocoderArea);
    psolaEngineToggle.setBounds(engineRow);

    const int synthesisCardBottom = bounds.getY() + innerPadY;
    synthesisCardBounds = juce::Rectangle<int>(
        cardArea.getX(), synthesisCardStart, cardArea.getWidth(),
        synthesisCardBottom - synthesisCardStart);

    // =========================================================================
    // TRACKS CARD (ARA plugin mode only)
    //
    // Sits directly below Rendering. When hidden it contributes nothing: no
    // bounds, no gap, and no height in getPreferredHeight().
    // =========================================================================
    int stackBottom = synthesisCardBottom;
    if (tracksCardVisible)
    {
        const int tracksCardStart = synthesisCardBottom + cardGap;
        bounds = juce::Rectangle<int>(cardArea.getX() + innerPadX,
                                      tracksCardStart + innerPadY,
                                      cardArea.getWidth() - innerPadX * 2,
                                      cardArea.getBottom() - tracksCardStart - innerPadY * 2);

        tracksSectionLabel.setBounds(bounds.removeFromTop(kSectionLabelHeight));
        bounds.removeFromTop(kSectionLabelGap);
        tracksSelectorButton.setBounds(bounds.removeFromTop(kTracksRowHeight));

        stackBottom = bounds.getY() + innerPadY;
        tracksCardBounds = juce::Rectangle<int>(cardArea.getX(), tracksCardStart,
                                                cardArea.getWidth(),
                                                stackBottom - tracksCardStart);
    }
    else
    {
        tracksCardBounds = {};
    }

    // =========================================================================
    // TIME CARD
    // =========================================================================
    const int timeCardStart = stackBottom + cardGap;
    bounds = juce::Rectangle<int>(cardArea.getX() + innerPadX, timeCardStart + innerPadY,
                                   cardArea.getWidth() - innerPadX * 2, cardArea.getBottom() - timeCardStart - innerPadY * 2);

    timeSectionLabel.setBounds(bounds.removeFromTop(kSectionLabelHeight));
    bounds.removeFromTop(kSectionLabelGap);

    // Timeline mode: Beats | Time
    auto timelineModeRow = bounds.removeFromTop(kTimelineModeRowHeight);
    constexpr int radioButtonWidth = 82;
    beatsTimelineToggle.setBounds(timelineModeRow.removeFromLeft(radioButtonWidth));
    timelineModeRow.removeFromLeft(columnGap);
    timeTimelineToggle.setBounds(timelineModeRow.removeFromLeft(radioButtonWidth));

    bounds.removeFromTop(rowGap + 1);

    // Beat / Tempo row
    auto beatTempoRow = bounds.removeFromTop(kControlRowHeight);
    timelineBeatLabel.setBounds(beatTempoRow.removeFromLeft(38));
    auto beatButtonArea = beatTempoRow.removeFromLeft(44);
    timelineBeatButton.setBounds(beatButtonArea.withSizeKeepingCentre(40, 22));
    beatTempoRow.removeFromLeft(10);
    timelineTempoLabel.setBounds(beatTempoRow.removeFromLeft(50));
    timelineTempoSlider.setBounds(beatTempoRow.reduced(0, 2));

    bounds.removeFromTop(rowGap);

    // Grid / Snap Cycle row
    auto gridRow = bounds.removeFromTop(kControlRowHeight);
    timelineGridLabel.setBounds(gridRow.removeFromLeft(38));
    timelineGridButton.setBounds(
        gridRow.removeFromLeft(44).withSizeKeepingCentre(40, 22));
    gridRow.removeFromLeft(10);
    timelineSnapCycleToggle.setBounds(gridRow);

    int timeCardBottom = bounds.getY() + innerPadY;
    timeCardBounds = juce::Rectangle<int>(cardArea.getX(), timeCardStart,
                                           cardArea.getWidth(), timeCardBottom - timeCardStart);

    // =========================================================================
    // PITCH CARD
    // =========================================================================
    const int pitchCardStart = timeCardBottom + cardGap;
    bounds = juce::Rectangle<int>(cardArea.getX() + innerPadX, pitchCardStart + innerPadY,
                                  cardArea.getWidth() - innerPadX * 2,
                                  cardArea.getBottom() - pitchCardStart - innerPadY * 2);

    pitchSectionLabel.setBounds(bounds.removeFromTop(kSectionLabelHeight));
    bounds.removeFromTop(kSectionLabelGap);

    // Mode toggles: Chromatic | Scale
    auto modeRow = bounds.removeFromTop(kRadioRowHeight);
    auto chromaticArea = modeRow.removeFromLeft((modeRow.getWidth() - columnGap) / 2);
    modeRow.removeFromLeft(columnGap);
    chromaticToggle.setBounds(chromaticArea);
    scaleToggle.setBounds(modeRow);

    bounds.removeFromTop(rowGap + 1);

    // Reference row
    auto referenceRow = bounds.removeFromTop(kControlRowHeight);
    referenceLabel.setBounds(referenceRow.removeFromLeft(110));
    referenceSlider.setBounds(referenceRow.removeFromLeft(70).reduced(0, 2));

    bounds.removeFromTop(rowGap + 1);
    auto dragSnapRow = bounds.removeFromTop(kControlRowHeight);
    snapToSemitonesToggle.setBounds(dragSnapRow.removeFromLeft(110));
    dragSnapRow.removeFromLeft(columnGap);
    dragSnapModeButton.setBounds(dragSnapRow.translated(-4, 0));

    const int pitchCardBottom = bounds.getY() + innerPadY;
    pitchCardBounds = kShowPitchCard
        ? juce::Rectangle<int>(cardArea.getX(), pitchCardStart,
                               cardArea.getWidth(), pitchCardBottom - pitchCardStart)
        : juce::Rectangle<int>();

    // =========================================================================
    // UI BRIGHTNESS CARD
    // =========================================================================
    const int brightnessCardStart = pitchCardBottom + cardGap;
    bounds = juce::Rectangle<int>(cardArea.getX() + innerPadX,
                                  brightnessCardStart + innerPadY,
                                  cardArea.getWidth() - innerPadX * 2,
                                  cardArea.getBottom() - brightnessCardStart - innerPadY * 2);

    brightnessSectionLabel.setBounds(bounds.removeFromTop(kSectionLabelHeight));
    bounds.removeFromTop(kSectionLabelGap);
    brightnessSlider.setBounds(bounds.removeFromTop(kBrightnessRowHeight));

    const int brightnessCardBottom = bounds.getY() + innerPadY;
    brightnessCardBounds = juce::Rectangle<int>(
        cardArea.getX(), brightnessCardStart, cardArea.getWidth(),
        brightnessCardBottom - brightnessCardStart);

    // The stack is laid out at fixed sizes, so given at least the height it
    // asks for its bottom edge must land exactly where getPreferredHeight()
    // says it will - that value is what the hosting panel scrolls against. A
    // shorter panel is not checked: removeFromTop clamps once the space runs
    // out, which is the very case the scrolling exists to avoid.
    const int preferredHeight = getPreferredHeight();
    jassert(outerBounds.getHeight() < preferredHeight ||
            brightnessCardBottom + cardPadY - outerBounds.getY() ==
                preferredHeight);
    juce::ignoreUnused(preferredHeight);
}

int ParameterPanel::getPreferredHeight() const
{
    return kPreferredPanelHeight +
           (tracksCardVisible ? kTracksCardExtraHeight : 0);
}

void ParameterPanel::buttonClicked(juce::Button* button)
{
    if (isUpdating)
        return;

    if (button == &timelineBeatButton)
    {
        showTimelineBeatMenu();
        return;
    }
    if (button == &timelineGridButton)
    {
        showTimelineGridMenu();
        return;
    }
    if (button == &dragSnapModeButton)
    {
        showDragSnapModeMenu();
        return;
    }
    if (button == &tracksSelectorButton)
    {
        showTracksMenu();
        return;
    }
    if (button == &timelineSnapCycleToggle)
    {
        setTimelineSnapCycleInternal(timelineSnapCycleToggle.getToggleState(), true);
        return;
    }
    if (button == &beatsTimelineToggle)
    {
        if (beatsTimelineToggle.getToggleState())
        {
            setTimelineDisplayModeInternal(TimelineDisplayMode::Beats, true);
        }
        else if (!timeTimelineToggle.getToggleState())
        {
            setTimelineDisplayModeInternal(TimelineDisplayMode::Time, true);
        }
        return;
    }
    if (button == &timeTimelineToggle)
    {
        if (timeTimelineToggle.getToggleState())
        {
            setTimelineDisplayModeInternal(TimelineDisplayMode::Time, true);
        }
        else if (!beatsTimelineToggle.getToggleState())
        {
            setTimelineDisplayModeInternal(TimelineDisplayMode::Beats, true);
        }
        return;
    }

    if (button == &vocoderEngineToggle || button == &psolaEngineToggle)
    {
        auto picked = synthesisEngine;
        if (button == &vocoderEngineToggle && vocoderEngineToggle.getToggleState())
            picked = SynthesisEngineType::Vocoder;
        else if (button == &psolaEngineToggle && psolaEngineToggle.getToggleState())
            picked = SynthesisEngineType::Psola;

        // Clicking the active one toggles it off; put it back rather than
        // leaving the group with nothing selected.
        if (picked != synthesisEngine)
            setSynthesisEngineInternal(picked, true);
        else
            refreshSynthesisToggles();
        return;
    }

    if (button == &snapToSemitonesToggle)
    {
        setSnapToSemitonesInternal(snapToSemitonesToggle.getToggleState(), true);
        if (onParameterChanged)
            onParameterChanged();
        return;
    }
    if (button == &chromaticToggle)
    {
        if (chromaticToggle.getToggleState())
        {
            setScaleModeInternal(ScaleMode::Chromatic, true);
        }
        else
            refreshModeToggles();

        if (onParameterChanged)
            onParameterChanged();
        return;
    }
    if (button == &scaleToggle)
    {
        if (scaleToggle.getToggleState())
        {
            ScaleMode target = selectedScaleMode;
            if (target == ScaleMode::None || target == ScaleMode::Chromatic)
                target = (lastNonChromaticMode == ScaleMode::None ||
                          lastNonChromaticMode == ScaleMode::Chromatic)
                             ? ScaleMode::Major
                             : lastNonChromaticMode;
            if (selectedScaleRootNote < 0)
                setScaleRootInternal(0, true);
            setScaleModeInternal(target, true);
        }
        else
            refreshModeToggles();

        if (onParameterChanged)
            onParameterChanged();
    }
}

void ParameterPanel::setProject(Project* proj)
{
    project = proj;
    updateGlobalSliders();
    if (onProjectBound)
        onProjectBound(project);
}

void ParameterPanel::setTracksCardVisible(bool visible)
{
    if (tracksCardVisible == visible)
        return;

    tracksCardVisible = visible;
    tracksSectionLabel.setVisible(visible);
    tracksSelectorButton.setVisible(visible);
    if (!visible)
        tracksCardBounds = {};

    resized();
    repaint();

    // The card stack just got taller or shorter, and the hosting panel caches
    // that height to decide when to scroll.
    if (onPreferredHeightChanged)
        onPreferredHeightChanged();
}

void ParameterPanel::setRegionList(const std::vector<MainViewRegionEntry>& regions,
                                   const juce::String& activeKey)
{
    regionEntries = regions;
    activeRegionKey = activeKey;
    refreshTracksButtonText();
}

void ParameterPanel::refreshTracksButtonText()
{
    juce::String text;
    for (const auto& entry : regionEntries)
    {
        if (entry.key == activeRegionKey)
        {
            text = entry.name;
            break;
        }
    }

    if (text.isEmpty())
        text = regionEntries.empty() ? kNoRegionsText : kNoRegionSelectedText;

    tracksSelectorButton.setButtonText(text);
    tracksSelectorButton.setEnabled(!regionEntries.empty());
}

void ParameterPanel::showTracksMenu()
{
    if (regionEntries.empty())
        return;

    constexpr int baseId = 7401;
    juce::PopupMenu menu;
    menu.setLookAndFeel(&getPitchPopupLookAndFeel());
    for (size_t i = 0; i < regionEntries.size(); ++i)
    {
        const auto& entry = regionEntries[i];
        menu.addCustomItem(baseId + static_cast<int>(i),
                           std::make_unique<HoverMenuItemComponent>(
                               entry.name, entry.key == activeRegionKey),
                           nullptr, entry.name);
    }

    menu.showMenuAsync(
        juce::PopupMenu::Options()
            .withTargetComponent(&tracksSelectorButton)
            .withParentComponent(this)
            .withMinimumWidth(tracksSelectorButton.getWidth()),
        [safeThis = juce::Component::SafePointer<ParameterPanel>(this)](int result)
        {
            if (safeThis == nullptr || result < baseId)
                return;

            const auto index = static_cast<size_t>(result - baseId);
            if (index >= safeThis->regionEntries.size())
                return;

            const auto key = safeThis->regionEntries[index].key;
            if (key == safeThis->activeRegionKey)
                return;

            // Show the pick straight away; the host round-trip that actually
            // switches the canvas confirms it on the next region-list update.
            safeThis->activeRegionKey = key;
            safeThis->refreshTracksButtonText();
            if (safeThis->onRegionSelected)
                safeThis->onRegionSelected(key);
        });
}

void ParameterPanel::setPluginMode(bool pluginMode)
{
    timelineBeatButton.setEnabled(!pluginMode);
    timelineTempoSlider.setEnabled(!pluginMode);
    timelineGridButton.setEnabled(true);
}

void ParameterPanel::setUiBrightness(double brightnessPercent)
{
    uiBrightnessPercent = juce::jlimit(75.0, 200.0, brightnessPercent);
    brightnessSlider.setValue(uiBrightnessPercent, juce::dontSendNotification);
}

void ParameterPanel::setHostTimelineState(double bpm, int numerator, int denominator)
{
    setTimelineTempoBpmInternal(bpm, false);
    setTimelineBeatSignatureInternal(numerator, denominator, false);
}

void ParameterPanel::setSelectedNote(Note* note)
{
    selectedNote = note;
    updateFromNote();
}

void ParameterPanel::updateFromNote()
{
    juce::ignoreUnused(selectedNote);
}

void ParameterPanel::updateGlobalSliders()
{
    isUpdating = true;
    lastNonChromaticMode = ScaleMode::Major;

    if (project != nullptr)
    {
        selectedScaleRootNote = project->getScaleRootNote();
        if (selectedScaleRootNote < 0)
        {
            selectedScaleRootNote = 0;
            project->setScaleRootNote(selectedScaleRootNote);
        }
        selectedScaleMode = project->getScaleMode();
        if (selectedScaleMode == ScaleMode::None)
        {
            selectedScaleMode = ScaleMode::Chromatic;
            project->setScaleMode(selectedScaleMode);
        }
        lastNonChromaticMode = project->getPreferredScaleMode();
        snapToSemitones = project->getSnapToSemitones();
        dragSnapMode = project->getDragSnapMode();
        pitchReferenceHz = project->getPitchReferenceHz();
        timelineDisplayMode = project->getTimelineDisplayMode();
        timelineBeatNumerator = project->getTimelineBeatNumerator();
        timelineBeatDenominator = project->getTimelineBeatDenominator();
        timelineTempoBpm = project->getTimelineTempoBpm();
        timelineGridDivision = project->getTimelineGridDivision();
        timelineSnapCycle = project->getTimelineSnapCycle();
    }
    else
    {
        selectedScaleRootNote = 0;
        selectedScaleMode = ScaleMode::Chromatic;
        snapToSemitones = false;
        dragSnapMode = DragSnapMode::Chromatic;
        pitchReferenceHz = 440;
        timelineDisplayMode = TimelineDisplayMode::Beats;
        timelineBeatNumerator = 4;
        timelineBeatDenominator = 4;
        timelineTempoBpm = 120.0;
        timelineGridDivision = TimelineGridDivision::Quarter;
        timelineSnapCycle = false;
    }

    referenceSlider.setValue(pitchReferenceHz, juce::dontSendNotification);
    snapToSemitonesToggle.setToggleState(snapToSemitones, juce::dontSendNotification);
    dragSnapModeButton.setButtonText(getDragSnapModeLabel(dragSnapMode));
    dragSnapModeButton.setEnabled(snapToSemitones);
    timelineBeatButton.setButtonText(
        getTimelineBeatLabel(timelineBeatNumerator, timelineBeatDenominator));
    timelineTempoSlider.setValue(timelineTempoBpm, juce::dontSendNotification);
    timelineGridButton.setButtonText(getTimelineGridLabel(timelineGridDivision));
    timelineSnapCycleToggle.setToggleState(timelineSnapCycle, juce::dontSendNotification);

    refreshModeToggles();
    refreshTimelineModeToggles();
    isUpdating = false;
}

void ParameterPanel::refreshModeToggles()
{
    const bool isChromatic = selectedScaleMode == ScaleMode::Chromatic;
    const bool isScale =
        selectedScaleMode != ScaleMode::None && selectedScaleMode != ScaleMode::Chromatic;

    chromaticToggle.setToggleState(isChromatic, juce::dontSendNotification);
    scaleToggle.setToggleState(isScale, juce::dontSendNotification);
}

void ParameterPanel::setSynthesisEngine(SynthesisEngineType type)
{
    setSynthesisEngineInternal(type, false);
}

void ParameterPanel::setSynthesisEngineInternal(SynthesisEngineType type, bool notify)
{
    synthesisEngine = type;
    refreshSynthesisToggles();

    if (notify && onSynthesisEngineChanged)
        onSynthesisEngineChanged(synthesisEngine);
}

void ParameterPanel::refreshSynthesisToggles()
{
    vocoderEngineToggle.setToggleState(synthesisEngine == SynthesisEngineType::Vocoder,
                                       juce::dontSendNotification);
    psolaEngineToggle.setToggleState(synthesisEngine == SynthesisEngineType::Psola,
                                     juce::dontSendNotification);
}

void ParameterPanel::refreshTimelineModeToggles()
{
    const bool beatsMode = timelineDisplayMode == TimelineDisplayMode::Beats;
    beatsTimelineToggle.setToggleState(beatsMode, juce::dontSendNotification);
    timeTimelineToggle.setToggleState(!beatsMode, juce::dontSendNotification);
}

void ParameterPanel::setScaleRootInternal(int rootNote, bool notify)
{
    const int normalized = juce::jlimit(-1, 11, rootNote);
    const bool changed = selectedScaleRootNote != normalized;
    if (!changed && !notify)
        return;

    selectedScaleRootNote = normalized;
    if (project != nullptr && changed)
        project->setScaleRootNote(normalized);

    if (notify && changed && onScaleRootChanged)
        onScaleRootChanged(normalized);
}

void ParameterPanel::setScaleModeInternal(ScaleMode mode, bool notify)
{
    const bool changed = selectedScaleMode != mode;
    if (!changed && !notify)
        return;

    selectedScaleMode = mode;
    if (mode != ScaleMode::None && mode != ScaleMode::Chromatic)
        lastNonChromaticMode = mode;

    refreshModeToggles();
    if (project != nullptr && changed)
        project->setScaleMode(mode);

    if (notify && changed && onScaleModeChanged)
        onScaleModeChanged(mode);
}

void ParameterPanel::setSnapToSemitonesInternal(bool enabled, bool notify)
{
    const bool changed = snapToSemitones != enabled;
    snapToSemitones = enabled;
    snapToSemitonesToggle.setToggleState(enabled, juce::dontSendNotification);
    dragSnapModeButton.setEnabled(enabled);

    if (project != nullptr && changed)
        project->setSnapToSemitones(enabled);

    if (notify && changed && onSnapToSemitonesChanged)
        onSnapToSemitonesChanged(enabled);
}

void ParameterPanel::setDragSnapModeInternal(DragSnapMode mode, bool notify)
{
    const bool changed = dragSnapMode != mode;
    dragSnapMode = mode;
    dragSnapModeButton.setButtonText(getDragSnapModeLabel(mode));

    if (project != nullptr && changed)
        project->setDragSnapMode(mode);

    if (notify && changed && onDragSnapModeChanged)
        onDragSnapModeChanged(mode);
}

void ParameterPanel::setPitchReferenceInternal(int hz, bool notify)
{
    const int normalized = juce::jlimit(430, 450, hz);
    const bool changed = pitchReferenceHz != normalized;
    pitchReferenceHz = normalized;
    referenceSlider.setValue(normalized, juce::dontSendNotification);

    if (project != nullptr && changed)
        project->setPitchReferenceHz(normalized);

    if (notify && changed && onPitchReferenceChanged)
        onPitchReferenceChanged(normalized);
}

void ParameterPanel::setTimelineDisplayModeInternal(TimelineDisplayMode mode, bool notify)
{
    const bool changed = timelineDisplayMode != mode;
    if (!changed && !notify)
        return;

    timelineDisplayMode = mode;
    refreshTimelineModeToggles();

    if (project != nullptr && changed)
        project->setTimelineDisplayMode(mode);

    if (notify && changed && onTimelineDisplayModeChanged)
        onTimelineDisplayModeChanged(mode);
}

void ParameterPanel::setTimelineBeatSignatureInternal(int numerator, int denominator, bool notify)
{
    const int normalizedNumerator = juce::jlimit(1, 32, numerator);
    const int normalizedDenominator = normalizeTimelineBeatDenominator(denominator);
    const bool changed = timelineBeatNumerator != normalizedNumerator ||
                         timelineBeatDenominator != normalizedDenominator;
    if (!changed && !notify)
        return;

    timelineBeatNumerator = normalizedNumerator;
    timelineBeatDenominator = normalizedDenominator;
    timelineBeatButton.setButtonText(
        getTimelineBeatLabel(timelineBeatNumerator, timelineBeatDenominator));

    if (project != nullptr && changed)
        project->setTimelineBeatSignature(normalizedNumerator, normalizedDenominator);

    if (notify && changed && onTimelineBeatSignatureChanged)
        onTimelineBeatSignatureChanged(normalizedNumerator, normalizedDenominator);
}

void ParameterPanel::setTimelineTempoBpmInternal(double bpm, bool notify)
{
    const double normalized = juce::jlimit(20.0, 300.0, bpm);
    const bool changed = std::abs(timelineTempoBpm - normalized) > 1.0e-6;
    timelineTempoBpm = normalized;
    timelineTempoSlider.setValue(normalized, juce::dontSendNotification);

    if (project != nullptr && changed)
        project->setTimelineTempoBpm(normalized);

    if (notify && changed && onTimelineTempoChanged)
        onTimelineTempoChanged(normalized);
}

void ParameterPanel::setTimelineGridDivisionInternal(TimelineGridDivision division, bool notify)
{
    const bool changed = timelineGridDivision != division;
    if (!changed && !notify)
        return;

    timelineGridDivision = division;
    timelineGridButton.setButtonText(getTimelineGridLabel(division));

    if (project != nullptr && changed)
        project->setTimelineGridDivision(division);

    if (notify && changed && onTimelineGridDivisionChanged)
        onTimelineGridDivisionChanged(division);
}

void ParameterPanel::setTimelineSnapCycleInternal(bool enabled, bool notify)
{
    const bool changed = timelineSnapCycle != enabled;
    timelineSnapCycle = enabled;
    timelineSnapCycleToggle.setToggleState(enabled, juce::dontSendNotification);

    if (project != nullptr && changed)
        project->setTimelineSnapCycle(enabled);

    if (notify && changed && onTimelineSnapCycleChanged)
        onTimelineSnapCycleChanged(enabled);
}

void ParameterPanel::showDragSnapModeMenu()
{
    constexpr int chromaticId = 7301;
    constexpr int scaleId = 7302;
    juce::PopupMenu menu;
    menu.setLookAndFeel(&getPitchPopupLookAndFeel());
    menu.addCustomItem(
        chromaticId,
        std::make_unique<HoverMenuItemComponent>(
            "Chromatic", dragSnapMode == DragSnapMode::Chromatic),
        nullptr, "Chromatic");
    menu.addCustomItem(
        scaleId,
        std::make_unique<HoverMenuItemComponent>(
            "Scale", dragSnapMode == DragSnapMode::Scale),
        nullptr, "Scale");

    menu.showMenuAsync(
        juce::PopupMenu::Options()
            .withTargetComponent(&dragSnapModeButton)
            .withParentComponent(this)
            .withMinimumWidth(dragSnapModeButton.getWidth()),
        [safeThis = juce::Component::SafePointer<ParameterPanel>(this)](int result)
        {
            if (safeThis == nullptr)
                return;
            if (result == 7301)
                safeThis->setDragSnapModeInternal(
                    DragSnapMode::Chromatic, true);
            else if (result == 7302)
                safeThis->setDragSnapModeInternal(DragSnapMode::Scale, true);
        });
}

void ParameterPanel::showTimelineBeatMenu()
{
    constexpr int menuBaseId = 7500;
    juce::PopupMenu menu;
    menu.setLookAndFeel(&getPitchPopupLookAndFeel());

    for (size_t i = 0; i < kTimelineBeatOptions.size(); ++i)
    {
        const auto& option = kTimelineBeatOptions[i];
        const bool selected = timelineBeatNumerator == option.numerator &&
                              timelineBeatDenominator == option.denominator;
        menu.addCustomItem(menuBaseId + static_cast<int>(i),
                           std::make_unique<HoverMenuItemComponent>(
                               option.label, selected),
                           nullptr, option.label);
    }

    auto options = juce::PopupMenu::Options()
        .withTargetComponent(&timelineBeatButton)
        .withParentComponent(this)
        .withMinimumWidth(timelineBeatButton.getWidth());

    menu.showMenuAsync(options,
                       [safeThis = juce::Component::SafePointer<ParameterPanel>(this)](int result)
                       {
                           if (safeThis == nullptr || result == 0)
                               return;

                           constexpr int menuBaseId = 7500;
                           const int idx = result - menuBaseId;
                           if (idx >= 0 && idx < static_cast<int>(kTimelineBeatOptions.size()))
                           {
                               const auto& option = kTimelineBeatOptions[static_cast<size_t>(idx)];
                               safeThis->setTimelineBeatSignatureInternal(
                                   option.numerator, option.denominator, true);
                           }
                       });
}

void ParameterPanel::showTimelineGridMenu()
{
    constexpr int menuBaseId = 7600;
    juce::PopupMenu menu;
    menu.setLookAndFeel(&getPitchPopupLookAndFeel());

    for (size_t i = 0; i < kTimelineGridOptions.size(); ++i)
    {
        const auto& option = kTimelineGridOptions[i];
        menu.addCustomItem(menuBaseId + static_cast<int>(i),
                           std::make_unique<HoverMenuItemComponent>(
                               option.label, timelineGridDivision == option.division),
                           nullptr, option.label);
    }

    auto options = juce::PopupMenu::Options()
        .withTargetComponent(&timelineGridButton)
        .withParentComponent(this)
        .withMinimumWidth(timelineGridButton.getWidth());

    menu.showMenuAsync(options,
                       [safeThis = juce::Component::SafePointer<ParameterPanel>(this)](int result)
                       {
                           if (safeThis == nullptr || result == 0)
                               return;

                           constexpr int menuBaseId = 7600;
                           const int idx = result - menuBaseId;
                           if (idx >= 0 && idx < static_cast<int>(kTimelineGridOptions.size()))
                           {
                               safeThis->setTimelineGridDivisionInternal(
                                   kTimelineGridOptions[static_cast<size_t>(idx)].division, true);
                           }
                       });
}
