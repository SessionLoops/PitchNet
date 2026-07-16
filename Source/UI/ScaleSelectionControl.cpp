#include "ScaleSelectionControl.h"
#include "../Utils/ScaleUtils.h"
#include "Components/AppFont.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace
{
struct ScaleModeOption
{
    ScaleMode mode;
    const char* label;
};

struct DetectedScaleCandidate
{
    int root = 0;
    ScaleMode mode = ScaleMode::Major;
    float score = 0.0f;
};

constexpr std::array<const char*, 12> kRootLabels {{
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
}};

constexpr std::array<ScaleModeOption, 7> kModeOptions {{
    { ScaleMode::Major, "Major" },
    { ScaleMode::Minor, "Minor" },
    { ScaleMode::Dorian, "Dorian" },
    { ScaleMode::Phrygian, "Phrygian" },
    { ScaleMode::Lydian, "Lydian" },
    { ScaleMode::Mixolydian, "Mixolydian" },
    { ScaleMode::Locrian, "Locrian" }
}};

juce::String rootLabel(int root)
{
    return root >= 0 && root < static_cast<int>(kRootLabels.size())
        ? kRootLabels[static_cast<size_t>(root)]
        : "C";
}

juce::String modeLabel(ScaleMode mode)
{
    for (const auto& option : kModeOptions)
        if (option.mode == mode)
            return option.label;
    return "Major";
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

    for (int root = 0; root < 12; ++root)
    {
        for (const auto& option : kModeOptions)
        {
            double inScaleWeight = 0.0;
            for (int pitchClass = 0; pitchClass < 12; ++pitchClass)
                if (ScaleUtils::isPitchClassInScale(option.mode, pitchClass, root))
                    inScaleWeight += pitchClassWeights[static_cast<size_t>(pitchClass)];

            const double rootBoost = pitchClassWeights[static_cast<size_t>(root)] * 0.10;
            result.push_back({
                root, option.mode,
                static_cast<float>((inScaleWeight + rootBoost) / totalWeight)
            });
        }
    }

    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b)
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

class PopupChoiceButton final : public juce::TextButton
{
public:
    explicit PopupChoiceButton(const juce::String& text)
        : juce::TextButton(text)
    {
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }

    void paintButton(juce::Graphics& g, bool highlighted, bool down) override
    {
        auto bounds = getLocalBounds().toFloat().reduced(0.5f);
        auto fill = juce::Colour(0xFF111111u);
        if (down)
            fill = fill.brighter(0.12f);
        else if (highlighted)
            fill = fill.brighter(0.07f);

        g.setColour(fill);
        g.fillRoundedRectangle(bounds, 5.0f);
        g.setColour(juce::Colour(0xFF8A8A8Au)
                        .withMultipliedAlpha(isEnabled() ? 1.0f : 0.45f));
        g.drawRoundedRectangle(bounds, 5.0f, 1.0f);
        g.setColour(juce::Colour(0xFFE6E6E6u)
                        .withMultipliedAlpha(isEnabled() ? 1.0f : 0.45f));
        g.setFont(AppFont::getFont(13.0f));
        g.drawFittedText(getButtonText(), getLocalBounds().reduced(4, 0),
                         juce::Justification::centred, 1);
    }
};

class ScalePopupContent final : public juce::Component
{
public:
    explicit ScalePopupContent(ScaleSelectionControl& ownerToUse)
        : owner(ownerToUse),
          rootButton(owner.getButtonText().upToFirstOccurrenceOf(" ", false, false)),
          modeButton(owner.getButtonText().fromFirstOccurrenceOf(" ", false, false)),
          detectedButton("Show Detected Scales")
    {
        for (auto* button : { &rootButton, &modeButton, &detectedButton })
            addAndMakeVisible(button);

        rootButton.onClick = [this] { showRootMenu(); };
        modeButton.onClick = [this] { showModeMenu(); };
        detectedButton.onClick = [this] { showDetectedMenu(); };
        bool hasPitchedNote = false;
        if (const auto* project = owner.getProject())
            for (const auto& note : project->getNotes())
                if (!note.isRest())
                {
                    hasPitchedNote = true;
                    break;
                }
        detectedButton.setEnabled(hasPitchedNote);
        setSize(208, 68);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xFF0D0B0Bu));
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(4, 4);
        auto top = bounds.removeFromTop(26);
        rootButton.setBounds(top.removeFromLeft(97));
        top.removeFromLeft(5);
        modeButton.setBounds(top);
        bounds.removeFromTop(4);
        detectedButton.setBounds(bounds.removeFromTop(26));
    }

private:
    void showRootMenu()
    {
        juce::PopupMenu menu;
        for (int i = 0; i < static_cast<int>(kRootLabels.size()); ++i)
            menu.addItem(i + 1, kRootLabels[static_cast<size_t>(i)]);
        menu.showMenuAsync(
            juce::PopupMenu::Options().withTargetComponent(&rootButton)
                                      .withMinimumWidth(rootButton.getWidth()),
            [safeThis = juce::Component::SafePointer<ScalePopupContent>(this)](int result)
            {
                if (safeThis != nullptr && result > 0)
                {
                    safeThis->owner.setScaleRoot(result - 1);
                    safeThis->rootButton.setButtonText(rootLabel(result - 1));
                }
            });
    }

    void showModeMenu()
    {
        juce::PopupMenu menu;
        for (size_t i = 0; i < kModeOptions.size(); ++i)
            menu.addItem(static_cast<int>(i) + 1, kModeOptions[i].label);
        menu.showMenuAsync(
            juce::PopupMenu::Options().withTargetComponent(&modeButton)
                                      .withMinimumWidth(modeButton.getWidth()),
            [safeThis = juce::Component::SafePointer<ScalePopupContent>(this)](int result)
            {
                if (safeThis != nullptr && result > 0 &&
                    result <= static_cast<int>(kModeOptions.size()))
                {
                    const auto mode = kModeOptions[static_cast<size_t>(result - 1)].mode;
                    safeThis->owner.setScaleMode(mode);
                    safeThis->modeButton.setButtonText(modeLabel(mode));
                }
            });
    }

    void showDetectedMenu()
    {
        const auto candidates = detectScales(owner.getProject());
        juce::PopupMenu menu;
        if (candidates.empty())
            menu.addItem(1, "No scale detected", false, false);
        else
            for (size_t i = 0; i < candidates.size(); ++i)
                menu.addItem(
                    static_cast<int>(i) + 1,
                    rootLabel(candidates[i].root) + " " +
                        modeLabel(candidates[i].mode) + "  (" +
                        juce::String(juce::roundToInt(candidates[i].score * 100.0f)) +
                        "%)");

        menu.showMenuAsync(
            juce::PopupMenu::Options().withTargetComponent(&detectedButton)
                                      .withMinimumWidth(240),
            [safeThis = juce::Component::SafePointer<ScalePopupContent>(this),
             candidates](int result)
            {
                if (safeThis == nullptr || result <= 0 ||
                    result > static_cast<int>(candidates.size()))
                    return;
                const auto& candidate = candidates[static_cast<size_t>(result - 1)];
                safeThis->owner.setDetectedScale(candidate.root, candidate.mode);
                safeThis->rootButton.setButtonText(rootLabel(candidate.root));
                safeThis->modeButton.setButtonText(modeLabel(candidate.mode));
            });
    }

    ScaleSelectionControl& owner;
    PopupChoiceButton rootButton;
    PopupChoiceButton modeButton;
    PopupChoiceButton detectedButton;
};
}

ScaleSelectionControl::ScaleSelectionControl()
    : CompactSelectionButton("C Major")
{
    setTooltip("Scale Root and Mode");
    onClick = [this] { showPopup(); };
}

void ScaleSelectionControl::setProject(Project* projectToUse)
{
    project = projectToUse;
    refreshLabel();
    setEnabled(project != nullptr);
}

void ScaleSelectionControl::showPopup()
{
    if (project == nullptr)
        return;

    refreshLabel();
    juce::CallOutBox::launchAsynchronously(
        std::make_unique<ScalePopupContent>(*this),
        getScreenBounds(), nullptr);
}

void ScaleSelectionControl::refreshLabel()
{
    const int root = project != nullptr ? project->getScaleRootNote() : 0;
    ScaleMode mode =
        project != nullptr ? project->getPreferredScaleMode() : ScaleMode::Major;
    if (mode == ScaleMode::None || mode == ScaleMode::Chromatic)
        mode = ScaleMode::Major;
    setButtonText(rootLabel(root) + " " + modeLabel(mode));
}

void ScaleSelectionControl::setScaleRoot(int rootNote)
{
    if (project == nullptr)
        return;
    const int normalized = juce::jlimit(0, 11, rootNote);
    project->setScaleRootNote(normalized);
    refreshLabel();
    if (onScaleRootChanged)
        onScaleRootChanged(normalized);
}

void ScaleSelectionControl::setScaleMode(ScaleMode mode)
{
    if (project == nullptr)
        return;
    if (mode == ScaleMode::None || mode == ScaleMode::Chromatic)
        mode = ScaleMode::Major;

    project->setPreferredScaleMode(mode);
    if (project->getScaleMode() != ScaleMode::Chromatic &&
        project->getScaleMode() != ScaleMode::None)
        project->setScaleMode(mode);
    if (onScaleModeChanged)
        onScaleModeChanged(mode);
    refreshLabel();
}

void ScaleSelectionControl::setDetectedScale(int rootNote, ScaleMode mode)
{
    if (project == nullptr)
        return;
    project->setScaleRootNote(juce::jlimit(0, 11, rootNote));
    project->setPreferredScaleMode(mode);
    project->setScaleMode(mode);
    refreshLabel();
    if (onScaleRootChanged)
        onScaleRootChanged(project->getScaleRootNote());
    if (onScaleModeChanged)
        onScaleModeChanged(mode);
}
