#include "TrackListPanel.h"
#include "Components/AppFont.h"
#include "../Utils/Localization.h"
#include "../Utils/UI/Theme.h"
#include "BinaryData.h"

namespace
{
constexpr float rowCorner = 6.0f;
constexpr float borderThickness = 1.5f;
constexpr int textInsetX = 15;
constexpr float inactiveBorderAlpha = 0.3f;

const juce::Colour activeFill{0xFF353535u};
const juce::Colour inactiveText{0xFF9A9A9Au};
} // namespace

TrackListPanel::TrackListPanel()
{
    setOpaque(false);

    pinImage = juce::ImageFileFormat::loadFrom(BinaryData::pin_png,
                                               static_cast<size_t>(BinaryData::pin_pngSize));

    // The label is painted by the panel so it can wrap in the narrow panel;
    // the toggle only draws the switch.
    useDawColourToggle.setClickingTogglesState(true);
    useDawColourToggle.setToggleState(useDawTrackColour, juce::dontSendNotification);
    useDawColourToggle.addListener(this);
    addAndMakeVisible(useDawColourToggle);
    refreshLocalisedText();
}

void TrackListPanel::refreshLocalisedText()
{
    useDawColourToggle.setTitle(TR("panel.use_daw_track_color"));
    for (auto& button : pinButtons)
        button->setTooltip(TR("tooltip.reference_track"));
    repaint();
}

void TrackListPanel::setUseDawTrackColour(bool use)
{
    useDawTrackColour = use;
    useDawColourToggle.setToggleState(use, juce::dontSendNotification);
    repaint();
}

void TrackListPanel::buttonClicked(juce::Button* button)
{
    if (button != &useDawColourToggle)
        return;

    useDawTrackColour = useDawColourToggle.getToggleState();
    repaint();
    if (onUseDawTrackColourChanged)
        onUseDawTrackColourChanged(useDawTrackColour);
}

juce::Colour TrackListPanel::colourFor(const MainViewTrackEntry& track) const
{
    return resolveTrackColour(track, useDawTrackColour);
}

void TrackListPanel::rebuildPinButtons()
{
    for (auto& button : pinButtons)
        removeChildComponent(button.get());
    pinButtons.clear();

    for (size_t i = 0; i < tracks.size(); ++i)
    {
        auto button = std::make_unique<ToggleButton>();
        button->setImage(pinImage);
        button->setTooltip(TR("tooltip.reference_track"));
        button->setClickingTogglesState(true);
        button->onClick = [this, i]
        {
            if (i >= tracks.size() || i >= pinButtons.size())
                return;
            const auto& track = tracks[i];
            if (track.key == activeKey)
                return;
            const bool pinned = pinButtons[i]->getToggleState();
            tracks[i].pinned = pinned; // the editor republishes to confirm
            if (onTrackPinChanged)
                onTrackPinChanged(track.key, pinned);
        };
        addAndMakeVisible(*button);
        pinButtons.push_back(std::move(button));
    }
}

void TrackListPanel::updatePinButtons()
{
    for (size_t i = 0; i < pinButtons.size() && i < tracks.size(); ++i)
    {
        const bool isActive = tracks[i].key == activeKey;
        // Always off and disabled on the active track.
        pinButtons[i]->setEnabled(!isActive);
        pinButtons[i]->setToggleState(!isActive && tracks[i].pinned,
                                      juce::dontSendNotification);
    }
}

void TrackListPanel::layoutPinButtons()
{
    for (size_t i = 0; i < pinButtons.size(); ++i)
    {
        const int rowY = static_cast<int>(i) * (rowHeight + rowGap);
        pinButtons[i]->setBounds(getWidth() - pinRightInset - pinButtonWidth,
                                 rowY + (rowHeight - pinButtonHeight) / 2,
                                 pinButtonWidth, pinButtonHeight);
    }
}

int TrackListPanel::rowsHeight() const
{
    // Room for the "no tracks" line when empty.
    if (tracks.empty())
        return rowHeight;

    const int n = static_cast<int>(tracks.size());
    return n * rowHeight + (n - 1) * rowGap;
}

juce::Rectangle<int> TrackListPanel::switchRowBounds() const
{
    return {0, rowsHeight() + switchGap, getWidth(), switchRowHeight};
}

juce::Rectangle<int> TrackListPanel::switchLabelBounds() const
{
    return switchRowBounds().withTrimmedLeft(switchLabelX);
}

void TrackListPanel::resized()
{
    const auto row = switchRowBounds();
    // Nudged 3px left of the content edge; the switch is drawn 4px inside
    // the toggle's bounds, so nothing is clipped.
    layoutPinButtons();
    useDawColourToggle.setBounds(row.withWidth(switchWidth)
                                     .withSizeKeepingCentre(switchWidth, 24)
                                     .translated(-3, 0));
}

int TrackListPanel::rowAt(juce::Point<int> position) const
{
    if (position.x < 0 || position.x >= getWidth() || position.y < 0)
        return -1;

    const int stride = rowHeight + rowGap;
    const int index = position.y / stride;
    if (index >= static_cast<int>(tracks.size()) || position.y % stride >= rowHeight)
        return -1;

    return index;
}

void TrackListPanel::mouseMove(const juce::MouseEvent& e)
{
    const int index = rowAt(e.getPosition());
    const bool clickable = (index >= 0 && tracks[static_cast<size_t>(index)].key != activeKey)
                           || switchLabelBounds().contains(e.getPosition());
    setMouseCursor(clickable ? juce::MouseCursor::PointingHandCursor
                             : juce::MouseCursor::NormalCursor);
}

void TrackListPanel::mouseExit(const juce::MouseEvent&)
{
    setMouseCursor(juce::MouseCursor::NormalCursor);
}

void TrackListPanel::mouseUp(const juce::MouseEvent& e)
{
    if (e.mouseWasDraggedSinceMouseDown() || e.mods.isPopupMenu())
        return;

    // The label toggles the switch too.
    if (switchLabelBounds().contains(e.getPosition()))
    {
        useDawColourToggle.triggerClick();
        return;
    }

    const int index = rowAt(e.getPosition());
    if (index < 0)
        return;

    const auto& key = tracks[static_cast<size_t>(index)].key;
    if (key == activeKey)
        return;

    // The editor confirms by republishing the list with this track active.
    if (onTrackSelected)
        onTrackSelected(key);
}

void TrackListPanel::setTracks(const std::vector<MainViewTrackEntry>& newTracks,
                               const juce::String& newActiveKey)
{
    const bool countChanged = newTracks.size() != tracks.size();
    tracks = newTracks;
    activeKey = newActiveKey;

    if (countChanged)
        rebuildPinButtons();
    updatePinButtons();

    if (countChanged)
    {
        resized(); // the switch sits below the rows
        if (onPreferredHeightChanged)
            onPreferredHeightChanged();
    }

    repaint();
}

int TrackListPanel::getPreferredHeight() const
{
    return rowsHeight() + switchGap + switchRowHeight;
}

void TrackListPanel::paint(juce::Graphics& g)
{
    // Switch label, wrapped to two lines if the panel is too narrow for one.
    // Same 14px as the switch labels DarkLookAndFeel draws in the right panel.
    g.setFont(AppFont::getFont(14.0f));
    g.setColour(APP_COLOR_TEXT_PRIMARY);
    g.drawFittedText(TR("panel.use_daw_track_color"), switchLabelBounds(),
                     juce::Justification::centredLeft, 2, 1.0f);

    const auto font = AppFont::getFont(13.0f);
    g.setFont(font);

    if (tracks.empty())
    {
        g.setColour(APP_COLOR_TEXT_MUTED);
        g.drawText(TR("panel.no_tracks"),
                   juce::Rectangle<int>(0, 0, getWidth(), rowHeight).withTrimmedLeft(textInsetX),
                   juce::Justification::centredLeft, true);
        return;
    }

    int y = 0;
    for (const auto& track : tracks)
    {
        const bool isActive = track.key == activeKey;
        const auto colour = colourFor(track);
        const auto row = juce::Rectangle<int>(0, y, getWidth(), rowHeight);
        const auto rowF = row.toFloat().reduced(borderThickness * 0.5f);

        if (isActive)
        {
            g.setColour(activeFill);
            g.fillRoundedRectangle(rowF, rowCorner);
        }

        g.setColour(isActive ? colour : colour.withAlpha(inactiveBorderAlpha));
        g.drawRoundedRectangle(rowF, rowCorner, borderThickness);

        g.setColour(isActive ? colour : inactiveText);
        // Leave room for the pin button on the right.
        g.drawText(track.name,
                   row.withTrimmedLeft(textInsetX)
                       .withTrimmedRight(pinRightInset + pinButtonWidth + 4),
                   juce::Justification::centredLeft, true);

        y += rowHeight + rowGap;
    }
}
