#include "ParameterPanel.h"
#include "../Utils/Localization.h"
#include "../Utils/ScaleUtils.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace
{
constexpr bool kShowPitchCard = false;

struct ScaleModeOption
{
    ScaleMode mode;
    const char* label;
};

struct DoubleClickSnapOption
{
    DoubleClickSnapMode mode;
    const char* label;
};

struct DetectedScaleCandidate
{
    int root = 0;
    ScaleMode mode = ScaleMode::Major;
    float score = 0.0f;
};

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

constexpr std::array<ScaleModeOption, 8> kScaleModeOptions {{
    { ScaleMode::None, "-" },
    { ScaleMode::Major, "Major" },
    { ScaleMode::Minor, "Minor" },
    { ScaleMode::Dorian, "Dorian" },
    { ScaleMode::Phrygian, "Phrygian" },
    { ScaleMode::Lydian, "Lydian" },
    { ScaleMode::Mixolydian, "Mixolydian" },
    { ScaleMode::Locrian, "Locrian" }
}};

constexpr std::array<DoubleClickSnapOption, 3> kDoubleClickSnapOptions {{
    { DoubleClickSnapMode::PitchCenter, "Pitch Center" },
    { DoubleClickSnapMode::NearestSemitone, "Nearest Semitone" },
    { DoubleClickSnapMode::NearestScale, "Nearest Scale" }
}};

constexpr std::array<const char*, 12> kScaleRootLabels {{
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
}};

constexpr std::array<int, 5> kReferencePresets {{
    415, 432, 435, 440, 442
}};

constexpr std::array<ScaleMode, 7> kDetectedScaleModes {{
    ScaleMode::Major,
    ScaleMode::Minor,
    ScaleMode::Dorian,
    ScaleMode::Phrygian,
    ScaleMode::Lydian,
    ScaleMode::Mixolydian,
    ScaleMode::Locrian
}};

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

juce::String getScaleModeLabel(ScaleMode mode)
{
    for (const auto& option : kScaleModeOptions)
        if (option.mode == mode)
            return option.label;
    return "-";
}

juce::String getScaleRootLabel(int rootNote)
{
    if (rootNote < 0 || rootNote > 11)
        return "-";
    return kScaleRootLabels[static_cast<size_t>(rootNote)];
}

juce::String getDoubleClickSnapLabel(DoubleClickSnapMode mode)
{
    for (const auto& option : kDoubleClickSnapOptions)
        if (option.mode == mode)
            return option.label;
    return kDoubleClickSnapOptions[0].label;
}

juce::String getDetectedScaleLabel(const DetectedScaleCandidate& candidate)
{
    const int percent = juce::roundToInt(candidate.score * 100.0f);
    return juce::String(getScaleRootLabel(candidate.root))
        + " "
        + getScaleModeLabel(candidate.mode)
        + "  ("
        + juce::String(percent)
        + "%)";
}

std::vector<DetectedScaleCandidate> detectScales(const Project* project)
{
    std::vector<DetectedScaleCandidate> result;
    if (project == nullptr)
        return result;

    std::array<double, 12> pitchClassWeights {};
    double totalWeight = 0.0;

    for (const auto& note : project->getNotes())
    {
        if (note.isRest())
            continue;

        const int pitchClass =
            (static_cast<int>(std::lround(note.getAdjustedMidiNote())) % 12 + 12) % 12;
        const double weight = static_cast<double>(juce::jmax(1, note.getDurationFrames()));
        pitchClassWeights[static_cast<size_t>(pitchClass)] += weight;
        totalWeight += weight;
    }

    if (totalWeight <= 0.0)
        return result;

    result.reserve(kDetectedScaleModes.size() * 12);
    for (int root = 0; root < 12; ++root)
    {
        for (const auto mode : kDetectedScaleModes)
        {
            double inScaleWeight = 0.0;
            for (int pitchClass = 0; pitchClass < 12; ++pitchClass)
            {
                if (ScaleUtils::isPitchClassInScale(mode, pitchClass, root))
                    inScaleWeight += pitchClassWeights[static_cast<size_t>(pitchClass)];
            }

            const double rootBoost = pitchClassWeights[static_cast<size_t>(root)] * 0.10;
            result.push_back({
                root,
                mode,
                static_cast<float>((inScaleWeight + rootBoost) / totalWeight)
            });
        }
    }

    std::sort(result.begin(), result.end(),
              [](const DetectedScaleCandidate& a, const DetectedScaleCandidate& b)
              {
                  if (std::abs(a.score - b.score) > 1.0e-6f)
                      return a.score > b.score;
                  if (a.root != b.root)
                      return a.root < b.root;
                  return static_cast<int>(a.mode) < static_cast<int>(b.mode);
              });

    if (result.size() > 8)
        result.resize(8);
    return result;
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

    for (auto* toggle : { &chromaticToggle, &scaleToggle, &showScaleColorsToggle,
                          &snapToSemitonesToggle, &timelineSnapCycleToggle })
    {
        addAndMakeVisible(toggle);
        toggle->setClickingTogglesState(true);
        toggle->addListener(this);
    }

    for (auto* radio : { &beatsTimelineToggle, &timeTimelineToggle })
    {
        addAndMakeVisible(radio);
        radio->setClickingTogglesState(true);
        radio->addListener(this);
    }

    for (auto* label : { &referenceLabel, &scaleRootLabel, &scaleModeLabel, &doubleClickSnapLabel,
                         &timelineBeatLabel, &timelineTempoLabel, &timelineGridLabel })
    {
        addAndMakeVisible(label);
        label->setColour(juce::Label::textColourId, APP_COLOR_TEXT_MUTED);
    }

    for (auto* label : { &timelineBeatLabel, &timelineTempoLabel, &timelineGridLabel })
    {
        label->setColour(juce::Label::textColourId, juce::Colour(0xFFE6E6E6u));
        label->setFont(AppFont::getFont(16.0f));
    }

    const std::array<juce::TextButton*, 7> textButtons {{
        &referenceMenuButton, &scaleRootButton, &scaleModeButton,
        &showDetectedScalesButton, &doubleClickSnapButton,
        &timelineBeatButton, &timelineGridButton
    }};
    for (auto* button : textButtons)
    {
        setupTextButton(*button);
        addAndMakeVisible(button);
        button->addListener(this);
    }

    referenceEditor.setInputRestrictions(3, "0123456789");
    referenceEditor.setText(juce::String(pitchReferenceHz), juce::dontSendNotification);
    referenceEditor.setJustification(juce::Justification::centred);
    referenceEditor.setColour(juce::TextEditor::backgroundColourId, APP_COLOR_SURFACE_ALT);
    referenceEditor.setColour(juce::TextEditor::textColourId, APP_COLOR_TEXT_PRIMARY);
    referenceEditor.setColour(juce::TextEditor::outlineColourId, APP_COLOR_BORDER.withAlpha(0.8f));
    referenceEditor.setColour(juce::TextEditor::focusedOutlineColourId, APP_COLOR_PRIMARY.withAlpha(0.85f));
    referenceEditor.addListener(this);
    addAndMakeVisible(referenceEditor);

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

    scaleRootButton.setButtonText(getScaleRootLabel(selectedScaleRootNote));
    scaleModeButton.setButtonText(getScaleModeLabel(selectedScaleMode));
    doubleClickSnapButton.setButtonText(getDoubleClickSnapLabel(doubleClickSnapMode));
    timelineBeatButton.setButtonText(getTimelineBeatLabel(timelineBeatNumerator, timelineBeatDenominator));
    timelineGridButton.setButtonText(getTimelineGridLabel(timelineGridDivision));
    timelineSnapCycleToggle.setToggleState(timelineSnapCycle, juce::dontSendNotification);

    const std::array<juce::Component*, 15> pitchComponents {{
        &pitchSectionLabel, &chromaticToggle, &scaleToggle,
        &referenceLabel, &referenceEditor, &referenceMenuButton,
        &scaleRootLabel, &scaleRootButton, &scaleModeLabel, &scaleModeButton,
        &showDetectedScalesButton, &showScaleColorsToggle,
        &snapToSemitonesToggle, &doubleClickSnapLabel, &doubleClickSnapButton
    }};
    for (auto* component : pitchComponents)
        component->setVisible(kShowPitchCard);

    refreshModeToggles();
    refreshScaleControlEnabling();
    refreshTimelineModeToggles();
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
}

void ParameterPanel::resized()
{
    auto outerBounds = getLocalBounds();

    constexpr int cardPadX = 5;    // horizontal padding outside cards
    constexpr int cardPadY = 6;    // vertical padding outside cards
    constexpr int cardGap = 6;     // gap between pitch and time cards
    constexpr int innerPadX = 10;  // padding inside each card
    constexpr int innerPadY = 8;   // padding inside each card
    constexpr int rowGap = 5;
    constexpr int columnGap = 6;

    // =========================================================================
    // PITCH CARD
    // =========================================================================
    auto cardArea = outerBounds.reduced(cardPadX, cardPadY);
    auto pitchCardStart = cardArea.getY();
    auto bounds = cardArea;
    bounds = bounds.reduced(innerPadX, innerPadY);

    pitchSectionLabel.setBounds(bounds.removeFromTop(20));
    bounds.removeFromTop(rowGap + 2);

    // Mode toggles: Chromatic | Scale
    auto modeRow = bounds.removeFromTop(22);
    auto chromaticArea = modeRow.removeFromLeft((modeRow.getWidth() - columnGap) / 2);
    modeRow.removeFromLeft(columnGap);
    chromaticToggle.setBounds(chromaticArea);
    scaleToggle.setBounds(modeRow);

    bounds.removeFromTop(rowGap + 1);

    // Reference row
    auto referenceRow = bounds.removeFromTop(26);
    referenceLabel.setBounds(referenceRow.removeFromLeft(110));
    auto refMenuArea = referenceRow.removeFromRight(30);
    referenceMenuButton.setBounds(refMenuArea);
    referenceEditor.setBounds(referenceRow.reduced(0, 1));

    bounds.removeFromTop(rowGap);

    // Scale Root / Mode labels
    auto scaleLabelRow = bounds.removeFromTop(14);
    auto rootLabelArea = scaleLabelRow.removeFromLeft((scaleLabelRow.getWidth() - columnGap) / 2);
    scaleLabelRow.removeFromLeft(columnGap);
    scaleRootLabel.setBounds(rootLabelArea);
    scaleModeLabel.setBounds(scaleLabelRow);

    // Scale Root / Mode buttons
    auto scaleValueRow = bounds.removeFromTop(26);
    auto rootValueArea = scaleValueRow.removeFromLeft((scaleValueRow.getWidth() - columnGap) / 2);
    scaleValueRow.removeFromLeft(columnGap);
    scaleRootButton.setBounds(rootValueArea);
    scaleModeButton.setBounds(scaleValueRow);

    bounds.removeFromTop(rowGap + 1);
    showDetectedScalesButton.setBounds(bounds.removeFromTop(26));

    bounds.removeFromTop(rowGap + 1);
    showScaleColorsToggle.setBounds(bounds.removeFromTop(22));
    bounds.removeFromTop(3);
    snapToSemitonesToggle.setBounds(bounds.removeFromTop(22));

    bounds.removeFromTop(rowGap + 1);
    auto doubleClickRow = bounds.removeFromTop(26);
    doubleClickSnapLabel.setBounds(doubleClickRow.removeFromLeft(120));
    doubleClickSnapButton.setBounds(doubleClickRow);

    // Pitch card ends here (content bottom + inner padding)
    int pitchCardBottom = bounds.getY() + innerPadY;
    pitchCardBounds = kShowPitchCard
        ? juce::Rectangle<int>(cardArea.getX(), pitchCardStart,
                               cardArea.getWidth(), pitchCardBottom - pitchCardStart)
        : juce::Rectangle<int>();

    // =========================================================================
    // TIME CARD
    // =========================================================================
    int timeCardStart = kShowPitchCard ? pitchCardBottom + cardGap : cardArea.getY();
    bounds = juce::Rectangle<int>(cardArea.getX() + innerPadX, timeCardStart + innerPadY,
                                   cardArea.getWidth() - innerPadX * 2, cardArea.getBottom() - timeCardStart - innerPadY * 2);

    timeSectionLabel.setBounds(bounds.removeFromTop(20));
    bounds.removeFromTop(rowGap + 2);

    // Timeline mode: Beats | Time
    auto timelineModeRow = bounds.removeFromTop(32);
    constexpr int radioButtonWidth = 82;
    beatsTimelineToggle.setBounds(timelineModeRow.removeFromLeft(radioButtonWidth));
    timelineModeRow.removeFromLeft(columnGap);
    timeTimelineToggle.setBounds(timelineModeRow.removeFromLeft(radioButtonWidth));

    bounds.removeFromTop(rowGap + 1);

    // Beat / Tempo row
    auto beatTempoRow = bounds.removeFromTop(26);
    timelineBeatLabel.setBounds(beatTempoRow.removeFromLeft(38));
    auto beatButtonArea = beatTempoRow.removeFromLeft(44);
    timelineBeatButton.setBounds(beatButtonArea.withSizeKeepingCentre(40, 22));
    beatTempoRow.removeFromLeft(10);
    timelineTempoLabel.setBounds(beatTempoRow.removeFromLeft(50));
    timelineTempoSlider.setBounds(beatTempoRow.reduced(0, 2));

    bounds.removeFromTop(rowGap);

    // Grid / Snap Cycle row
    auto gridRow = bounds.removeFromTop(26);
    timelineGridLabel.setBounds(gridRow.removeFromLeft(38));
    timelineGridButton.setBounds(
        gridRow.removeFromLeft(44).withSizeKeepingCentre(40, 22));
    gridRow.removeFromLeft(10);
    timelineSnapCycleToggle.setBounds(gridRow);

    int timeCardBottom = bounds.getY() + innerPadY;
    timeCardBounds = juce::Rectangle<int>(cardArea.getX(), timeCardStart,
                                           cardArea.getWidth(), timeCardBottom - timeCardStart);
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

    if (button == &scaleRootButton)
    {
        showScaleRootMenu();
        return;
    }
    if (button == &scaleModeButton)
    {
        showScaleModeMenu();
        return;
    }
    if (button == &referenceMenuButton)
    {
        showReferenceMenu();
        return;
    }
    if (button == &doubleClickSnapButton)
    {
        showDoubleClickSnapMenu();
        return;
    }
    if (button == &showDetectedScalesButton)
    {
        showDetectedScaleMenu();
        return;
    }
    if (button == &showScaleColorsToggle)
    {
        setShowScaleColorsInternal(showScaleColorsToggle.getToggleState(), true);
        if (onParameterChanged)
            onParameterChanged();
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
        else if (!scaleToggle.getToggleState())
        {
            setScaleModeInternal(ScaleMode::None, true);
        }

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
            setScaleModeInternal(target, true);
        }
        else if (!chromaticToggle.getToggleState())
        {
            setScaleModeInternal(ScaleMode::None, true);
        }

        if (onParameterChanged)
            onParameterChanged();
    }
}

void ParameterPanel::textEditorReturnKeyPressed(juce::TextEditor& editor)
{
    if (&editor == &referenceEditor)
        applyReferenceEditorValue(true);
}

void ParameterPanel::textEditorFocusLost(juce::TextEditor& editor)
{
    if (&editor == &referenceEditor)
        applyReferenceEditorValue(true);
}

void ParameterPanel::setProject(Project* proj)
{
    project = proj;
    updateGlobalSliders();
}

void ParameterPanel::setPluginMode(bool pluginMode)
{
    timelineBeatButton.setEnabled(!pluginMode);
    timelineTempoSlider.setEnabled(!pluginMode);
    timelineGridButton.setEnabled(true);
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

    if (project != nullptr)
    {
        selectedScaleRootNote = project->getScaleRootNote();
        selectedScaleMode = project->getScaleMode();
        if (selectedScaleMode != ScaleMode::None &&
            selectedScaleMode != ScaleMode::Chromatic)
            lastNonChromaticMode = selectedScaleMode;
        showScaleColors = project->getShowScaleColors();
        snapToSemitones = project->getSnapToSemitones();
        pitchReferenceHz = project->getPitchReferenceHz();
        doubleClickSnapMode = project->getDoubleClickSnapMode();
        timelineDisplayMode = project->getTimelineDisplayMode();
        timelineBeatNumerator = project->getTimelineBeatNumerator();
        timelineBeatDenominator = project->getTimelineBeatDenominator();
        timelineTempoBpm = project->getTimelineTempoBpm();
        timelineGridDivision = project->getTimelineGridDivision();
        timelineSnapCycle = project->getTimelineSnapCycle();
    }
    else
    {
        selectedScaleRootNote = -1;
        selectedScaleMode = ScaleMode::None;
        showScaleColors = true;
        snapToSemitones = false;
        pitchReferenceHz = 440;
        doubleClickSnapMode = DoubleClickSnapMode::PitchCenter;
        timelineDisplayMode = TimelineDisplayMode::Beats;
        timelineBeatNumerator = 4;
        timelineBeatDenominator = 4;
        timelineTempoBpm = 120.0;
        timelineGridDivision = TimelineGridDivision::Quarter;
        timelineSnapCycle = false;
    }

    scaleRootButton.setButtonText(getScaleRootLabel(selectedScaleRootNote));
    scaleModeButton.setButtonText(
        selectedScaleMode == ScaleMode::Chromatic ? "-" : getScaleModeLabel(selectedScaleMode));
    referenceEditor.setText(juce::String(pitchReferenceHz), juce::dontSendNotification);
    showScaleColorsToggle.setToggleState(showScaleColors, juce::dontSendNotification);
    snapToSemitonesToggle.setToggleState(snapToSemitones, juce::dontSendNotification);
    doubleClickSnapButton.setButtonText(getDoubleClickSnapLabel(doubleClickSnapMode));
    timelineBeatButton.setButtonText(
        getTimelineBeatLabel(timelineBeatNumerator, timelineBeatDenominator));
    timelineTempoSlider.setValue(timelineTempoBpm, juce::dontSendNotification);
    timelineGridButton.setButtonText(getTimelineGridLabel(timelineGridDivision));
    timelineSnapCycleToggle.setToggleState(timelineSnapCycle, juce::dontSendNotification);

    refreshModeToggles();
    refreshScaleControlEnabling();
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

void ParameterPanel::refreshScaleControlEnabling()
{
    const bool scaleEnabled = scaleToggle.getToggleState();
    scaleRootButton.setEnabled(scaleEnabled);
    scaleModeButton.setEnabled(scaleEnabled);
    scaleRootLabel.setEnabled(scaleEnabled);
    scaleModeLabel.setEnabled(scaleEnabled);

    bool hasAnyNote = false;
    if (project != nullptr)
    {
        for (const auto& note : project->getNotes())
        {
            if (!note.isRest())
            {
                hasAnyNote = true;
                break;
            }
        }
    }
    showDetectedScalesButton.setEnabled(hasAnyNote);
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
    scaleRootButton.setButtonText(getScaleRootLabel(selectedScaleRootNote));

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

    scaleModeButton.setButtonText(
        mode == ScaleMode::Chromatic ? "-" : getScaleModeLabel(mode));
    refreshModeToggles();
    refreshScaleControlEnabling();

    if (project != nullptr && changed)
        project->setScaleMode(mode);

    if (notify && changed && onScaleModeChanged)
        onScaleModeChanged(mode);
}

void ParameterPanel::setShowScaleColorsInternal(bool enabled, bool notify)
{
    const bool changed = showScaleColors != enabled;
    showScaleColors = enabled;
    showScaleColorsToggle.setToggleState(enabled, juce::dontSendNotification);

    if (project != nullptr && changed)
        project->setShowScaleColors(enabled);

    if (notify && changed && onShowScaleColorsChanged)
        onShowScaleColorsChanged(enabled);
}

void ParameterPanel::setSnapToSemitonesInternal(bool enabled, bool notify)
{
    const bool changed = snapToSemitones != enabled;
    snapToSemitones = enabled;
    snapToSemitonesToggle.setToggleState(enabled, juce::dontSendNotification);

    if (project != nullptr && changed)
        project->setSnapToSemitones(enabled);

    if (notify && changed && onSnapToSemitonesChanged)
        onSnapToSemitonesChanged(enabled);
}

void ParameterPanel::setPitchReferenceInternal(int hz, bool notify)
{
    const int normalized = juce::jlimit(380, 480, hz);
    const bool changed = pitchReferenceHz != normalized;
    pitchReferenceHz = normalized;
    referenceEditor.setText(juce::String(normalized), juce::dontSendNotification);

    if (project != nullptr && changed)
        project->setPitchReferenceHz(normalized);

    if (notify && changed && onPitchReferenceChanged)
        onPitchReferenceChanged(normalized);
}

void ParameterPanel::setDoubleClickSnapModeInternal(DoubleClickSnapMode mode, bool notify)
{
    const bool changed = doubleClickSnapMode != mode;
    doubleClickSnapMode = mode;
    doubleClickSnapButton.setButtonText(getDoubleClickSnapLabel(mode));

    if (project != nullptr && changed)
        project->setDoubleClickSnapMode(mode);

    if (notify && changed && onDoubleClickSnapModeChanged)
        onDoubleClickSnapModeChanged(mode);
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

void ParameterPanel::previewScaleRoot(std::optional<int> rootNote)
{
    if (onScaleRootPreviewChanged)
        onScaleRootPreviewChanged(rootNote);
}

void ParameterPanel::previewScaleMode(std::optional<ScaleMode> mode)
{
    if (onScaleModePreviewChanged)
        onScaleModePreviewChanged(mode);
}

void ParameterPanel::applyReferenceEditorValue(bool notify)
{
    const int parsed = referenceEditor.getText().getIntValue();
    setPitchReferenceInternal(parsed <= 0 ? pitchReferenceHz : parsed, notify);
}

void ParameterPanel::showScaleRootMenu()
{
    constexpr int menuBaseId = 7000;
    juce::PopupMenu menu;
    menu.setLookAndFeel(&getPitchPopupLookAndFeel());

    auto hoverNoneCallback = [safeThis = juce::Component::SafePointer<ParameterPanel>(this)]()
    {
        if (safeThis != nullptr)
            safeThis->previewScaleRoot(-1);
    };
    menu.addCustomItem(menuBaseId,
                       std::make_unique<HoverMenuItemComponent>(
                           "-", selectedScaleRootNote < 0, std::move(hoverNoneCallback)),
                       nullptr, "-");

    for (int i = 0; i < static_cast<int>(kScaleRootLabels.size()); ++i)
    {
        const juce::String label = kScaleRootLabels[static_cast<size_t>(i)];
        auto hoverCallback =
            [safeThis = juce::Component::SafePointer<ParameterPanel>(this), i]()
            {
                if (safeThis != nullptr)
                    safeThis->previewScaleRoot(i);
            };

        menu.addCustomItem(menuBaseId + i + 1,
                           std::make_unique<HoverMenuItemComponent>(
                               label, i == selectedScaleRootNote, std::move(hoverCallback)),
                           nullptr, label);
    }

    auto options = juce::PopupMenu::Options()
        .withTargetComponent(&scaleRootButton)
        .withParentComponent(this)
        .withMinimumWidth(juce::jmax(200, scaleRootButton.getWidth()));

    menu.showMenuAsync(options,
                       [safeThis = juce::Component::SafePointer<ParameterPanel>(this)](int result)
                       {
                           if (safeThis == nullptr)
                               return;

                           safeThis->previewScaleRoot(std::nullopt);
                           if (result == 0)
                               return;

                           constexpr int menuBaseId = 7000;
                           const int idx = result - menuBaseId;
                           if (idx == 0)
                               safeThis->setScaleRootInternal(-1, true);
                           else
                               safeThis->setScaleRootInternal(idx - 1, true);
                       });
}

void ParameterPanel::showScaleModeMenu()
{
    constexpr int menuBaseId = 7100;
    juce::PopupMenu menu;
    menu.setLookAndFeel(&getPitchPopupLookAndFeel());

    for (size_t i = 0; i < kScaleModeOptions.size(); ++i)
    {
        const auto mode = kScaleModeOptions[i].mode;
        const juce::String label = kScaleModeOptions[i].label;
        auto hoverCallback =
            [safeThis = juce::Component::SafePointer<ParameterPanel>(this), mode]()
            {
                if (safeThis != nullptr)
                    safeThis->previewScaleMode(mode);
            };

        menu.addCustomItem(menuBaseId + static_cast<int>(i),
                           std::make_unique<HoverMenuItemComponent>(
                               label, selectedScaleMode == mode, std::move(hoverCallback)),
                           nullptr, label);
    }

    auto options = juce::PopupMenu::Options()
        .withTargetComponent(&scaleModeButton)
        .withParentComponent(this)
        .withMinimumWidth(juce::jmax(200, scaleModeButton.getWidth()));

    menu.showMenuAsync(options,
                       [safeThis = juce::Component::SafePointer<ParameterPanel>(this)](int result)
                       {
                           if (safeThis == nullptr)
                               return;

                           safeThis->previewScaleMode(std::nullopt);
                           if (result == 0)
                               return;

                           constexpr int menuBaseId = 7100;
                           const int idx = result - menuBaseId;
                           if (idx >= 0 && idx < static_cast<int>(kScaleModeOptions.size()))
                               safeThis->setScaleModeInternal(
                                   kScaleModeOptions[static_cast<size_t>(idx)].mode, true);
                       });
}

void ParameterPanel::showDoubleClickSnapMenu()
{
    constexpr int menuBaseId = 7200;
    juce::PopupMenu menu;
    menu.setLookAndFeel(&getPitchPopupLookAndFeel());

    for (size_t i = 0; i < kDoubleClickSnapOptions.size(); ++i)
    {
        const auto option = kDoubleClickSnapOptions[i];
        menu.addCustomItem(menuBaseId + static_cast<int>(i),
                           std::make_unique<HoverMenuItemComponent>(
                               option.label, doubleClickSnapMode == option.mode),
                           nullptr, option.label);
    }

    auto options = juce::PopupMenu::Options()
        .withTargetComponent(&doubleClickSnapButton)
        .withParentComponent(this)
        .withMinimumWidth(juce::jmax(220, doubleClickSnapButton.getWidth()));

    menu.showMenuAsync(options,
                       [safeThis = juce::Component::SafePointer<ParameterPanel>(this)](int result)
                       {
                           if (safeThis == nullptr || result == 0)
                               return;

                           constexpr int menuBaseId = 7200;
                           const int idx = result - menuBaseId;
                           if (idx >= 0 && idx < static_cast<int>(kDoubleClickSnapOptions.size()))
                               safeThis->setDoubleClickSnapModeInternal(
                                   kDoubleClickSnapOptions[static_cast<size_t>(idx)].mode, true);
                       });
}

void ParameterPanel::showReferenceMenu()
{
    constexpr int menuBaseId = 7300;
    juce::PopupMenu menu;
    menu.setLookAndFeel(&getPitchPopupLookAndFeel());

    for (size_t i = 0; i < kReferencePresets.size(); ++i)
    {
        const int preset = kReferencePresets[i];
        menu.addItem(menuBaseId + static_cast<int>(i), juce::String(preset), true,
                     preset == pitchReferenceHz);
    }

    auto options = juce::PopupMenu::Options()
        .withTargetComponent(&referenceMenuButton)
        .withParentComponent(this)
        .withMinimumWidth(120);

    menu.showMenuAsync(options,
                       [safeThis = juce::Component::SafePointer<ParameterPanel>(this)](int result)
                       {
                           if (safeThis == nullptr || result == 0)
                               return;

                           constexpr int menuBaseId = 7300;
                           const int idx = result - menuBaseId;
                           if (idx >= 0 && idx < static_cast<int>(kReferencePresets.size()))
                               safeThis->setPitchReferenceInternal(
                                   kReferencePresets[static_cast<size_t>(idx)], true);
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

void ParameterPanel::showDetectedScaleMenu()
{
    constexpr int menuBaseId = 7400;
    juce::PopupMenu menu;
    menu.setLookAndFeel(&getPitchPopupLookAndFeel());

    const auto candidates = detectScales(project);
    if (candidates.empty())
    {
        menu.addItem(1, "No scale detected", false, false);
    }
    else
    {
        for (size_t i = 0; i < candidates.size(); ++i)
            menu.addItem(menuBaseId + static_cast<int>(i), getDetectedScaleLabel(candidates[i]));
    }

    auto options = juce::PopupMenu::Options()
        .withTargetComponent(&showDetectedScalesButton)
        .withParentComponent(this)
        .withMinimumWidth(240);

    menu.showMenuAsync(options,
                       [safeThis = juce::Component::SafePointer<ParameterPanel>(this),
                        candidates](int result)
                       {
                           if (safeThis == nullptr || result == 0)
                               return;

                           constexpr int menuBaseId = 7400;
                           const int idx = result - menuBaseId;
                           if (idx >= 0 && idx < static_cast<int>(candidates.size()))
                           {
                               const auto& candidate = candidates[static_cast<size_t>(idx)];
                               safeThis->setScaleRootInternal(candidate.root, true);
                               safeThis->setScaleModeInternal(candidate.mode, true);
                           }
                       });
}
