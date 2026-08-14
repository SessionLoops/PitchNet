#include "ScaleSelectionControl.h"
#include "Components/PitchPopupMenu.h"
#include <array>

namespace
{
struct ScaleModeOption
{
    ScaleMode mode;
    const char* label;
};

constexpr std::array<const char*, 12> kRootLabels {{
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
}};

constexpr std::array<ScaleModeOption, 14> kModeOptions {{
    { ScaleMode::Major, "Major" },
    { ScaleMode::Minor, "Minor" },
    { ScaleMode::Blues, "Blues" },
    { ScaleMode::Dorian, "Dorian" },
    { ScaleMode::HarmonicMinor, "Harmonic Minor" },
    { ScaleMode::Locrian, "Locrian" },
    { ScaleMode::Lydian, "Lydian" },
    { ScaleMode::MajorPentatonic, "Major Pentatonic" },
    { ScaleMode::MelodicMinor, "Melodic Minor" },
    { ScaleMode::MinorPentatonic, "Minor Pentatonic" },
    { ScaleMode::Mixolydian, "Mixolydian" },
    { ScaleMode::Phrygian, "Phrygian" },
    { ScaleMode::PhrygianDominant, "Phrygian Dominant" },
    { ScaleMode::WholeTone, "Whole Tone" }
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

}

ScaleSelectionControl::ScaleSelectionControl()
    : CompactSelectionButton("C Major")
{
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
    constexpr int rootMenuBaseId = 7000;
    constexpr int modeMenuBaseId = 7100;

    juce::PopupMenu rootMenu;
    juce::PopupMenu modeMenu;
    juce::PopupMenu menu;
    auto* lookAndFeel = &pitchPopupMenu::getLookAndFeel();
    rootMenu.setLookAndFeel(lookAndFeel);
    modeMenu.setLookAndFeel(lookAndFeel);
    menu.setLookAndFeel(lookAndFeel);

    const int selectedRoot = project->getScaleRootNote();
    for (int i = 0; i < static_cast<int>(kRootLabels.size()); ++i)
    {
        const juce::String label = kRootLabels[static_cast<size_t>(i)];
        auto hoverCallback =
            [safeThis = juce::Component::SafePointer<ScaleSelectionControl>(this), i]()
            {
                if (safeThis != nullptr)
                    safeThis->previewScaleRoot(i);
            };
        rootMenu.addCustomItem(
            rootMenuBaseId + i + 1,
            std::make_unique<pitchPopupMenu::MenuItemComponent>(
                label, selectedRoot == i, std::move(hoverCallback)),
            nullptr, label);
    }

    ScaleMode selectedMode = project->getPreferredScaleMode();
    if (selectedMode == ScaleMode::None || selectedMode == ScaleMode::Chromatic)
        selectedMode = ScaleMode::Major;
    for (size_t i = 0; i < kModeOptions.size(); ++i)
    {
        const auto mode = kModeOptions[i].mode;
        const juce::String label = kModeOptions[i].label;
        auto hoverCallback =
            [safeThis = juce::Component::SafePointer<ScaleSelectionControl>(this), mode]()
            {
                if (safeThis != nullptr)
                    safeThis->previewScaleMode(mode);
            };
        modeMenu.addCustomItem(
            modeMenuBaseId + static_cast<int>(i),
            std::make_unique<pitchPopupMenu::MenuItemComponent>(
                label, selectedMode == mode, std::move(hoverCallback)),
            nullptr, label);
    }

    menu.addSubMenu("Root", rootMenu);
    menu.addSubMenu("Mode", modeMenu);
    menu.showMenuAsync(
        juce::PopupMenu::Options()
            .withTargetComponent(this)
            .withMinimumWidth(getWidth()),
        [safeThis = juce::Component::SafePointer<ScaleSelectionControl>(this)](int result)
        {
            if (safeThis == nullptr)
                return;

            safeThis->previewScaleRoot(std::nullopt);
            safeThis->previewScaleMode(std::nullopt);

            constexpr int rootMenuBaseId = 7000;
            constexpr int modeMenuBaseId = 7100;
            const int rootIndex = result - rootMenuBaseId;
            if (rootIndex >= 1 &&
                rootIndex <= static_cast<int>(kRootLabels.size()))
            {
                safeThis->setScaleRoot(rootIndex - 1);
                return;
            }

            const int modeIndex = result - modeMenuBaseId;
            if (modeIndex >= 0 &&
                modeIndex < static_cast<int>(kModeOptions.size()))
                safeThis->setScaleMode(
                    kModeOptions[static_cast<size_t>(modeIndex)].mode);
        });
}

void ScaleSelectionControl::refreshLabel()
{
    const int root = project != nullptr ? project->getScaleRootNote() : 0;
    ScaleMode mode =
        project != nullptr ? project->getPreferredScaleMode() : ScaleMode::Major;
    if (mode == ScaleMode::None || mode == ScaleMode::Chromatic)
        mode = ScaleMode::Major;
    const auto text = rootLabel(root) + " " + modeLabel(mode);
    if (getButtonText() == text)
        return;

    setButtonText(text);
    if (onPreferredWidthChanged)
        onPreferredWidthChanged();
}

int ScaleSelectionControl::getPreferredWidth() const
{
    constexpr int minimumWidth = 92;
    constexpr int horizontalPadding = 20;
    const int textWidth = juce::GlyphArrangement::getStringWidthInt(
        AppFont::getFont(13.0f), getButtonText());
    return juce::jmax(minimumWidth, textWidth + horizontalPadding);
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

void ScaleSelectionControl::previewScaleRoot(std::optional<int> rootNote)
{
    if (onScaleRootPreviewChanged)
        onScaleRootPreviewChanged(rootNote);
}

void ScaleSelectionControl::previewScaleMode(std::optional<ScaleMode> mode)
{
    if (onScaleModePreviewChanged)
        onScaleModePreviewChanged(mode);
}
