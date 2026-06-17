#pragma once

#include "../JuceHeader.h"
#include "../UI/IMainView.h"
#include "../UI/MainViewFactory.h"
#include <memory>
#include "PluginProcessor.h"

class PitchNetAudioProcessorEditor : public juce::AudioProcessorEditor
#if JucePlugin_Enable_ARA
    , public juce::AudioProcessorEditorARAExtension
#endif
{
public:
    explicit PitchNetAudioProcessorEditor(PitchNetAudioProcessor&);
    ~PitchNetAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    // Grab keyboard focus when the editor becomes visible / is clicked, so that
    // MainComponent holds focus and host-forwarded key events are dispatched to
    // its command-key mappings.
    void visibilityChanged() override;
    void mouseDown(const juce::MouseEvent& e) override;

private:
    void setupARAMode();
    void setupNonARAMode();
    void setupCallbacks();
    void setupHostTransportUiSync(bool includePlayState);
    void syncHostLoopSnapshotFromPlayHead();
    void requestMainViewKeyboardFocus();
    void requestMainViewKeyboardFocusAsync();

    PitchNetAudioProcessor& audioProcessor;
    std::unique_ptr<IMainView> mainView;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PitchNetAudioProcessorEditor)
};
