#pragma once

#include "../JuceHeader.h"
#include "IMainView.h"
#include "Workspace/PanelContent.h"
#include "Components/StyledWidgets.h"
#include "../Utils/Localization.h"
#include "Buttons.h"
#include <memory>
#include <functional>
#include <vector>

/**
 * Content of the left side panel: every track (ARA region sequence) that has
 * PitchNet on it, one rounded row per track.
 *
 * The active track - the one whose region the canvas is showing - is drawn
 * filled with a solid border and its name in the track colour. The others keep
 * a border in their own track colour at reduced alpha and a muted name.
 *
 * Each inactive row has a pin toggle; a pinned track's notes are drawn on the
 * canvas. The active track's pin is always off and disabled.
 *
 * Below the rows, a "Use DAW Track Color" switch. Off - or when the DAW sent
 * no colour - rows use the default colour instead.
 */
class TrackListPanel : public juce::Component,
                       public PanelContent,
                       private juce::Button::Listener
{
public:
    TrackListPanel();

    void setTracks(const std::vector<MainViewTrackEntry>& tracks,
                   const juce::String& activeKey);

    void setUseDawTrackColour(bool use);
    bool getUseDawTrackColour() const { return useDawTrackColour; }

    int getPreferredHeight() const override;
    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;

    /** Fired with a track key when the user clicks an inactive track. */
    std::function<void(const juce::String&)> onTrackSelected;

    /** Fired with a track key and the new state when a pin is toggled. */
    std::function<void(const juce::String&, bool)> onTrackPinChanged;

    /** Fired when the user flips the "Use DAW Track Color" switch. */
    std::function<void(bool)> onUseDawTrackColourChanged;

    /** Fired when the number of rows changes, so the panel can re-measure. */
    std::function<void()> onPreferredHeightChanged;

    static constexpr int rowHeight = 40;
    static constexpr int rowGap = 10;
    static constexpr int switchGap = 6;       // rows -> switch
    static constexpr int switchRowHeight = 34; // fits a two-line label
    static constexpr int switchWidth = 40;    // the toggle's switch area
    static constexpr int switchLabelX = 35;   // label start, from the content edge
    static constexpr int pinButtonWidth = 20;
    static constexpr int pinButtonHeight = 24;
    static constexpr int pinRightInset = 8;  // pin button -> row's right edge

private:
    /** Index of the row under a point, or -1 (gaps between rows count as none). */
    int rowAt(juce::Point<int> position) const;
    int rowsHeight() const;
    juce::Rectangle<int> switchRowBounds() const;
    juce::Rectangle<int> switchLabelBounds() const;
    juce::Colour colourFor(const MainViewTrackEntry& track) const;
    void buttonClicked(juce::Button* button) override;
    void refreshLocalisedText();
    void rebuildPinButtons();
    void updatePinButtons();
    void layoutPinButtons();

    std::vector<MainViewTrackEntry> tracks;
    juce::String activeKey;
    bool useDawTrackColour = true;
    StyledToggleButton useDawColourToggle;
    juce::Image pinImage;
    std::vector<std::unique_ptr<ToggleButton>> pinButtons; // one per row

    LocalisationWatcher languageWatcher{[this] { refreshLocalisedText(); }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TrackListPanel)
};
