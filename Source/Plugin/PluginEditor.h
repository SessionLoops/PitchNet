#pragma once

#include <set>

#include "../JuceHeader.h"
#include "../UI/IMainView.h"
#include "../UI/MainViewFactory.h"
#include <cstddef>
#include <map>
#include <memory>
#include <vector>
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
    void parentHierarchyChanged() override;

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
    bool syncInitialARASelectionFromHost();
    // Regions this plugin instance can edit, newest host state each call:
    // the renderer's assignment when the host made one, otherwise whatever the
    // document controller discovered.
    std::vector<juce::ARAPlaybackRegion*> collectAraPlaybackRegions() const;
    // Diagnostics for the "No regions" case: how many regions each lookup sees.
    size_t documentControllerRegionCount() const;
    size_t documentRegionCount() const;
    // Rebuild the Regions card's region list and push it to the UI, but only
    // when it actually differs from what the card already shows.
    void refreshAraRegionList();
    // Switch the canvas to a region the user picked in the Regions card, or
    // clicked on the canvas. A canvas click leaves the view where it is
    // (focusView = false): the region is already on screen.
    void activateAraRegionByKey(const juce::String& regionKey,
                                bool focusView = true);
    // Make a live region the canvas region and republish the lists.
    void activateAraRegion(juce::ARAPlaybackRegion* target, bool focusView);
    // Switch the canvas to a track the user clicked in the left side panel:
    // the region last edited on that track, else its first region in time.
    void activateAraTrackByKey(const juce::String& trackKey);
    // Push a preview of every region on the active region's track to the
    // canvas and thumbnail, rebuilding only regions whose content changed.
    void publishAraRegionPreviews(
        const std::vector<juce::ARAPlaybackRegion*>& regions,
        const std::vector<MainViewRegionEntry>& entries,
        const juce::String& activeKey);
    // Push every track (region sequence) in the ARA document - i.e. every
    // track that has PitchNet on it - to the left side panel, marking the
    // track of the active region. Only republishes when something changed.
    void publishAraTrackList(
        const std::vector<juce::ARAPlaybackRegion*>& regions,
        const std::vector<MainViewRegionEntry>& entries,
        const juce::String& activeKey);
    // ARAEditorView::Listener. When the host selection changes, switch the
    // canvas to the selected region's per-region Project (each region/track is
    // analysed and edited independently). Format-agnostic: works in AAX/VST3/AU.
    void onNewSelection(const juce::ARAViewSelection& viewSelection) override;
#endif

    void requestMainViewKeyboardFocus();
    void requestMainViewKeyboardFocusAsync();
    void applyLunaSoftwareRenderer();

    PitchNetAudioProcessor& audioProcessor;
    std::unique_ptr<IMainView> mainView;
    double lastSyncedHostPlayheadSeconds = 0.0;
    bool lastSyncedHostPlayState = false;
    bool hasSyncedHostPlayhead = false;
#if JucePlugin_Enable_ARA
    juce::String lastPublishedRegionSignature;
    bool regionListPublished = false;
    struct RegionPreviewCacheEntry {
        const Project* project = nullptr;
        bool fromCanvas = false;
        std::uint64_t revision = 0;
        double startSeconds = 0.0;
        double endSeconds = 0.0;
        double startInModificationSeconds = 0.0;
        MainViewRegionPreview preview;
    };
    std::map<juce::String, RegionPreviewCacheEntry> regionPreviewCache;
    juce::String lastPublishedPreviewSignature;
    juce::String lastPublishedTrackSignature;
    // Track key -> selector of the region last active on that track, so
    // switching back to a track returns to the clip you were editing.
    std::map<juce::String, juce::String> lastActiveRegionByTrack;
    // Tracks pinned in the track list (session-local keys). Their notes are
    // drawn on the canvas. Never contains the active track.
    std::set<juce::String> pinnedTrackKeys;
    int regionListRefreshCountdown = 0;
#endif
    bool lunaSoftwareRendererApplied = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PitchNetAudioProcessorEditor)
};
