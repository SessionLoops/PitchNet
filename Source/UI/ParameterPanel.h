#pragma once

#include "../JuceHeader.h"
#include "../Audio/SynthesisEngineType.h"
#include "../Models/Note.h"
#include "../Models/Project.h"
#include "../Undo/UndoActions.h"
#include "../Utils/UI/Theme.h"
#include "StyledComponents.h"
#include "Sliders.h"
#include <optional>

class ParameterPanel : public juce::Component,
                       public juce::Button::Listener
{
public:
    ParameterPanel();
    ~ParameterPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    void buttonClicked(juce::Button* button) override;

    void setProject(Project* proj);
    void setPluginMode(bool pluginMode);
    void setUiBrightness(double brightnessPercent);
    void setHostTimelineState(double bpm, int numerator, int denominator);
    void setUndoManager(PitchUndoManager* mgr) { juce::ignoreUnused(mgr); }
    void setSelectedNote(Note* note);
    void setSynthesisEngine(SynthesisEngineType type);
    SynthesisEngineType getSynthesisEngine() const { return synthesisEngine; }
    void updateFromNote();
    void updateGlobalSliders();
    std::function<void(Project*)> onProjectBound;

    int getPreferredHeight() const { return 545; }

    std::function<void()> onParameterChanged;
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

private:
    void setupTextButton(juce::TextButton& button);
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

    juce::Label pitchSectionLabel { {}, "Pitch" };
    juce::Rectangle<int> pitchCardBounds;
    juce::Label timeSectionLabel { {}, "Time" };
    juce::Rectangle<int> timeCardBounds;
    juce::Label synthesisSectionLabel { {}, "Rendering" };
    juce::Rectangle<int> synthesisCardBounds;
    juce::Label brightnessSectionLabel { {}, "UI Brightness" };
    juce::Rectangle<int> brightnessCardBounds;

    RadioButton chromaticToggle { "Chromatic" };
    RadioButton scaleToggle { "Scale" };

    // How an edited region is rendered. Only one of these is resynthesis in
    // the analysis/resynthesis sense - the model turns a spectrum back into a
    // signal, while PSOLA never leaves the time domain and emits the original
    // samples, windowed and repositioned. Hence "Rendering" as the umbrella
    // and the asymmetric labels.
    //
    // Not ranked by quality: neither is better everywhere. The model wins on
    // large pitch moves, PSOLA returns untouched audio bit for bit. The member
    // names track the enum, the button text is what the user reads.
    RadioButton vocoderEngineToggle { "AI Resynthesis" };
    RadioButton psolaEngineToggle { "Classic" };

    juce::Label referenceLabel { {}, "Reference (A4)" };
    SliderBox referenceSlider { "Pitch Reference" };

    StyledToggleButton snapToSemitonesToggle { "Drag Snap" };
    CompactSelectionButton dragSnapModeButton { "Chromatic" };

    RadioButton beatsTimelineToggle { "Beats" };
    RadioButton timeTimelineToggle { "Time" };
    juce::Label timelineBeatLabel { {}, "Beat" };
    CompactSelectionButton timelineBeatButton { "4/4" };
    juce::Label timelineTempoLabel { {}, "Tempo" };
    SliderBox timelineTempoSlider { "Tempo" };
    juce::Label timelineGridLabel { {}, "Grid" };
    CompactSelectionButton timelineGridButton { "1/4" };
    StyledToggleButton timelineSnapCycleToggle { "Snap Cycle" };
    MacroSlider brightnessSlider;

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
    SynthesisEngineType synthesisEngine = SynthesisEngineType::Vocoder;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ParameterPanel)
};
