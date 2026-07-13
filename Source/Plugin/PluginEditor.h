#pragma once

#include "../JuceHeader.h"
#include "../UI/IMainView.h"
#include "../UI/MainViewFactory.h"
#include <memory>
#include "PluginProcessor.h"

class PitchNetAudioProcessorEditor : public juce::AudioProcessorEditor
    , private juce::Timer
#if JucePlugin_Enable_ARA
    , public juce::AudioProcessorEditorARAExtension
    , private juce::ARAEditorView::Listener
#endif
{
public:
    explicit PitchNetAudioProcessorEditor(PitchNetAudioProcessor&);
    ~PitchNetAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void timerCallback() override;

#if JucePlugin_Enable_ARA
    juce::AudioProcessorEditorARAExtension* getARAClientExtensions() override;
#endif

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
    void syncAAXARAPlayheadStateFromHost();

#if JucePlugin_Enable_ARA
    // ARAEditorView::Listener. When the host selection changes, switch the
    // canvas to the selected region's per-region Project (each region/track is
    // analysed and edited independently). Format-agnostic: works in AAX/VST3/AU.
    void onNewSelection(const juce::ARAViewSelection& viewSelection) override;
#endif

    void requestMainViewKeyboardFocus();
    void requestMainViewKeyboardFocusAsync();

    PitchNetAudioProcessor& audioProcessor;
    std::unique_ptr<IMainView> mainView;
    double lastSyncedHostPlayheadSeconds = 0.0;
    bool lastSyncedHostPlayState = false;
    bool hasSyncedHostPlayhead = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PitchNetAudioProcessorEditor)
};
