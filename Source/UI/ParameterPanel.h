#pragma once

#include "../JuceHeader.h"
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
    void setHostTimelineState(double bpm, int numerator, int denominator);
    void setUndoManager(PitchUndoManager* mgr) { juce::ignoreUnused(mgr); }
    void setSelectedNote(Note* note);
    void updateFromNote();
    void updateGlobalSliders();
    std::function<void(Project*)> onProjectBound;

    int getPreferredHeight() const { return 470; }

    std::function<void()> onParameterChanged;
    std::function<void()> onParameterEditFinished;
    std::function<void(int)> onScaleRootChanged;
    std::function<void(ScaleMode)> onScaleModeChanged;
    std::function<void(bool)> onSnapToSemitonesChanged;
    std::function<void(int)> onPitchReferenceChanged;
    std::function<void(DoubleClickSnapMode)> onDoubleClickSnapModeChanged;
    std::function<void(TimelineDisplayMode)> onTimelineDisplayModeChanged;
    std::function<void(int, int)> onTimelineBeatSignatureChanged;
    std::function<void(double)> onTimelineTempoChanged;
    std::function<void(TimelineGridDivision)> onTimelineGridDivisionChanged;
    std::function<void(bool)> onTimelineSnapCycleChanged;

private:
    void setupTextButton(juce::TextButton& button);
    void showDoubleClickSnapMenu();
    void showTimelineBeatMenu();
    void showTimelineGridMenu();

    void setScaleRootInternal(int rootNote, bool notify);
    void setScaleModeInternal(ScaleMode mode, bool notify);
    void setSnapToSemitonesInternal(bool enabled, bool notify);
    void setPitchReferenceInternal(int hz, bool notify);
    void setDoubleClickSnapModeInternal(DoubleClickSnapMode mode, bool notify);
    void refreshModeToggles();
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

    RadioButton chromaticToggle { "Chromatic" };
    RadioButton scaleToggle { "Scale" };

    juce::Label referenceLabel { {}, "Reference (A4)" };
    SliderBox referenceSlider { "Pitch Reference" };

    StyledToggleButton snapToSemitonesToggle { "Snap To Semitones" };

    juce::Label doubleClickSnapLabel { {}, "Double Click Snap" };
    juce::TextButton doubleClickSnapButton { "Pitch Center" };

    RadioButton beatsTimelineToggle { "Beats" };
    RadioButton timeTimelineToggle { "Time" };
    juce::Label timelineBeatLabel { {}, "Beat" };
    CompactSelectionButton timelineBeatButton { "4/4" };
    juce::Label timelineTempoLabel { {}, "Tempo" };
    SliderBox timelineTempoSlider { "Tempo" };
    juce::Label timelineGridLabel { {}, "Grid" };
    CompactSelectionButton timelineGridButton { "1/4" };
    StyledToggleButton timelineSnapCycleToggle { "Snap Cycle" };

    int selectedScaleRootNote = 0;
    ScaleMode selectedScaleMode = ScaleMode::Chromatic;
    ScaleMode lastNonChromaticMode = ScaleMode::Major;
    bool snapToSemitones = false;
    int pitchReferenceHz = 440;
    DoubleClickSnapMode doubleClickSnapMode = DoubleClickSnapMode::PitchCenter;
    TimelineDisplayMode timelineDisplayMode = TimelineDisplayMode::Beats;
    int timelineBeatNumerator = 4;
    int timelineBeatDenominator = 4;
    double timelineTempoBpm = 120.0;
    TimelineGridDivision timelineGridDivision = TimelineGridDivision::Quarter;
    bool timelineSnapCycle = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ParameterPanel)
};
