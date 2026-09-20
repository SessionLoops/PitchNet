#pragma once

#include "../JuceHeader.h"
#include "../Audio/SynthesisEngineType.h"
#include "../Models/Note.h"
#include "../Models/Project.h"
#include "../Undo/UndoActions.h"
#include "../Utils/Localization.h"
#include "../Utils/UI/Theme.h"
#include "IMainView.h"
#include "StyledComponents.h"
#include "Sliders.h"
#include "Workspace/PanelContent.h"
#include <optional>
#include <vector>

class ParameterPanel : public juce::Component,
                       public juce::Button::Listener,
                       public PanelContent
{
public:
    ParameterPanel();
    ~ParameterPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    void buttonClicked(juce::Button* button) override;

    void setProject(Project* proj);
    void setPluginMode(bool pluginMode);

    // ===== Regions card =====
    // Only ARA plugin mode has playback regions, so the card is off by default
    // and the hosting panel is told to re-measure whenever it appears or goes
    // away - the card stack's natural height is what the panel scrolls against.
    /** Re-applies every caption and tooltip after a language change. */
    void refreshLocalisedText();

    void setRegionsCardVisible(bool visible);
    bool isRegionsCardVisible() const { return regionsCardVisible; }
    void setRegionList(const std::vector<MainViewRegionEntry>& regions,
                       const juce::String& activeKey);
    void setUiBrightness(double brightnessPercent);
    void setHostTimelineState(double bpm, int numerator, int denominator);
    void setUndoManager(PitchUndoManager* mgr) { juce::ignoreUnused(mgr); }
    void setSelectedNote(Note* note);
    void setSynthesisEngine(SynthesisEngineType type);
    SynthesisEngineType getSynthesisEngine() const { return synthesisEngine; }
    // Disables the AI Resynthesis radio on CPU-only machines.
    void setAiResynthesisAvailable(bool available);

    // ===== Rendering card: inference device =====
    // The row only earns its space when there is a choice to make, so it shows
    // only while AI Resynthesis is the engine and the execution provider
    // exposes more than one device. Pass a list of one - or none - to hide it.
    void setRenderDeviceOptions(const juce::StringArray& deviceNames,
                                int selectedIndex);
    void updateFromNote();
    void updateGlobalSliders();
    std::function<void(Project*)> onProjectBound;

    // Height the four cards need at full size. The hosting panel scrolls the
    // content when it has less room than this instead of squeezing the cards.
    int getPreferredHeight() const override;

    std::function<void()> onParameterChanged;
    std::function<void(const juce::String&)> onRegionSelected;

    // Fired with the code to persist ("auto", "en", "zh-TW", ...) whenever the
    // language card changes. The panel applies the language itself; the owner
    // only has to write the code to its settings.
    std::function<void(const juce::String&)> onLanguageChanged;
    std::function<void()> onPreferredHeightChanged;
    std::function<void()> onParameterEditFinished;
    std::function<void(int)> onScaleRootChanged;
    std::function<void(ScaleMode)> onScaleModeChanged;
    std::function<void(bool)> onSnapToSemitonesChanged;
    std::function<void(DragSnapMode)> onDragSnapModeChanged;
    std::function<void(int)> onPitchReferenceChanged;
    std::function<void(TimelineDisplayMode)> onTimelineDisplayModeChanged;
    std::function<void(int, int)> onTimelineBeatSignatureChanged;
    std::function<void(double)> onTimelineTempoChanged;
    std::function<void(TimelineGridDivision)> onTimelineGridDivisionChanged;
    std::function<void(bool)> onTimelineSnapCycleChanged;
    std::function<void(double)> onUiBrightnessChanged;
    std::function<void(SynthesisEngineType)> onSynthesisEngineChanged;
    std::function<void(int)> onRenderDeviceChanged;

private:
    void setupTextButton(juce::TextButton& button);
    void showRegionsMenu();
    void showLanguageMenu();
    void refreshLanguageButtonText();
    void applyLanguageSelection(const juce::String& languageCode);
    void refreshRegionsButtonText();
    void showDragSnapModeMenu();
    void showTimelineBeatMenu();
    void showTimelineGridMenu();

    void setScaleRootInternal(int rootNote, bool notify);
    void setScaleModeInternal(ScaleMode mode, bool notify);
    void setSnapToSemitonesInternal(bool enabled, bool notify);
    void setDragSnapModeInternal(DragSnapMode mode, bool notify);
    void setPitchReferenceInternal(int hz, bool notify);
    void refreshModeToggles();
    void refreshSynthesisToggles();
    void refreshRenderDeviceRow();
    void showRenderDeviceMenu();
    void setSynthesisEngineInternal(SynthesisEngineType type, bool notify);
    void refreshTimelineModeToggles();
    void setTimelineDisplayModeInternal(TimelineDisplayMode mode, bool notify);
    void setTimelineBeatSignatureInternal(int numerator, int denominator, bool notify);
    void setTimelineTempoBpmInternal(double bpm, bool notify);
    void setTimelineGridDivisionInternal(TimelineGridDivision division, bool notify);
    void setTimelineSnapCycleInternal(bool enabled, bool notify);

    Project* project = nullptr;
    Note* selectedNote = nullptr;
    bool isUpdating = false;

    // Card titles and control captions all come from the string table; see
    // refreshLocalisedText(), which is also what a language change calls.
    juce::Label regionsSectionLabel;
    juce::Rectangle<int> regionsCardBounds;
    juce::Label pitchSectionLabel;
    juce::Rectangle<int> pitchCardBounds;
    juce::Label timeSectionLabel;
    juce::Rectangle<int> timeCardBounds;
    juce::Label synthesisSectionLabel;
    juce::Rectangle<int> synthesisCardBounds;
    juce::Label languageSectionLabel;
    juce::Rectangle<int> languageCardBounds;
    juce::Label brightnessSectionLabel;
    juce::Rectangle<int> brightnessCardBounds;

    ComboSelectionButton regionsSelectorButton;

    // Interface language. "auto" follows the system language; anything else is
    // an explicit choice. The selector matches the Regions one: a button that
    // opens a ticked menu rather than a combo box.
    ComboSelectionButton languageSelectorButton;
    juce::String selectedLanguageCode { "auto" };

    RadioButton chromaticToggle;
    RadioButton scaleToggle;

    // How an edited region is rendered. Only one of these is resynthesis in
    // the analysis/resynthesis sense - the model turns a spectrum back into a
    // signal, while PSOLA never leaves the time domain and emits the original
    // samples, windowed and repositioned. Hence "Rendering" as the umbrella
    // and the asymmetric labels.
    //
    // Not ranked by quality: neither is better everywhere. The model wins on
    // large pitch moves, PSOLA returns untouched audio bit for bit. The member
    // names track the enum, the button text is what the user reads.
    RadioButton vocoderEngineToggle;
    RadioButton psolaEngineToggle;

    juce::Label renderDeviceLabel;
    ComboSelectionButton renderDeviceButton;

    juce::Label referenceLabel;
    SliderBox referenceSlider { "Pitch Reference" };

    StyledToggleButton snapToSemitonesToggle;
    CompactSelectionButton dragSnapModeButton;

    RadioButton beatsTimelineToggle;
    RadioButton timeTimelineToggle;
    juce::Label timelineBeatLabel;
    CompactSelectionButton timelineBeatButton { "4/4" };
    juce::Label timelineTempoLabel;
    SliderBox timelineTempoSlider { "Tempo" };
    juce::Label timelineGridLabel;
    CompactSelectionButton timelineGridButton { "1/4" };
    StyledToggleButton timelineSnapCycleToggle;
    MacroSlider brightnessSlider;

    std::vector<MainViewRegionEntry> regionEntries;
    juce::String activeRegionKey;
    bool regionsCardVisible = false;

    int selectedScaleRootNote = 0;
    ScaleMode selectedScaleMode = ScaleMode::Chromatic;
    ScaleMode lastNonChromaticMode = ScaleMode::Major;
    bool snapToSemitones = false;
    DragSnapMode dragSnapMode = DragSnapMode::Chromatic;
    int pitchReferenceHz = 440;
    TimelineDisplayMode timelineDisplayMode = TimelineDisplayMode::Beats;
    int timelineBeatNumerator = 4;
    int timelineBeatDenominator = 4;
    double timelineTempoBpm = 120.0;
    TimelineGridDivision timelineGridDivision = TimelineGridDivision::Quarter;
    bool timelineSnapCycle = false;
    double uiBrightnessPercent = 100.0;
    SynthesisEngineType synthesisEngine = defaultSynthesisEngineType();
    bool aiResynthesisAvailable = true;

    juce::StringArray renderDeviceNames;
    int renderDeviceIndex = 0;
    bool renderDeviceRowVisible = false;

    // Declared last so it is torn down before the widgets it refreshes.
    LocalisationWatcher languageWatcher{[this]
                                        {
                                            refreshLocalisedText();
                                            resized();
                                            repaint();
                                        }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ParameterPanel)
};
