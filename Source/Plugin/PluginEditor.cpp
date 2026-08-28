#include "PluginEditor.h"
#include "HostCompatibility.h"
#include "../Models/Project.h"
#include "../Utils/AppLogger.h"
#include "../Utils/Constants.h"

#include <algorithm>

#if JucePlugin_Enable_ARA
#include "ARADocumentController.h"
#endif

namespace
{
#if JucePlugin_Enable_ARA
// How often the Tracks card re-reads the host's region list.
constexpr int kRegionListRefreshHz = 8;
#endif

bool isLunaHostProcess()
{
#if JUCE_WINDOWS
  const auto host = juce::File::getSpecialLocation(
      juce::File::hostApplicationPath);
  return host.getFileNameWithoutExtension().equalsIgnoreCase("LUNA");
#else
  return false;
#endif
}
} // namespace

PitchNetAudioProcessorEditor::PitchNetAudioProcessorEditor(
    PitchNetAudioProcessor &p)
    : AudioProcessorEditor(&p), audioProcessor(p)
#if JucePlugin_Enable_ARA
      ,
      AudioProcessorEditorARAExtension(&p)
#endif
{
  // Fonts must exist before MainComponent constructs labels that retain them.
  initializeUiResources();
  mainView = createMainView(false);

  // Enable keyboard focus for the editor and the main view. This is required so
  // that when the host forwards a key event (see the IPlugView::onKeyDown patch
  // in juce_audio_plugin_client_VST3.cpp), MainComponent is the focused
  // component and its command-key mappings sit in the dispatch chain.
  setWantsKeyboardFocus(true);
  setMouseClickGrabsKeyboardFocus(true);

  addAndMakeVisible(*mainView->getComponent());
  mainView->getComponent()->setWantsKeyboardFocus(true);
  mainView->getComponent()->setMouseClickGrabsKeyboardFocus(true);
  audioProcessor.setMainComponent(mainView.get());
  addMouseListener(this, true);

#if JucePlugin_Enable_ARA
  setupARAMode();
#else
  setupNonARAMode();
#endif

  setupCallbacks();

  auto size = getDefaultMainViewSize(this);
  setSize(size.x, size.y);
  setResizable(true, true);

  // Grab keyboard focus once the host has actually shown the editor.
  requestMainViewKeyboardFocusAsync();
}

#if JucePlugin_Enable_ARA
juce::AudioProcessorEditorARAExtension *
PitchNetAudioProcessorEditor::getARAClientExtensions() {
  return this;
}
#endif

PitchNetAudioProcessorEditor::~PitchNetAudioProcessorEditor() {
  stopTimer();

  // Some ARA hosts briefly overlap editor instances while rebuilding the plug-
  // in view.  Only the editor that still owns the processor binding may tear
  // down shared callbacks; an older editor's destructor must not detach the
  // newer visible editor.
  const bool ownsProcessorMainView =
      audioProcessor.getMainComponent() == mainView.get();
#if JucePlugin_Enable_ARA
  if (auto *araEditorView = getARAEditorView()) {
    araEditorView->removeListener(this);
    if (auto *araDocController = araEditorView->getDocumentController()) {
      if (auto *pitchDocController = juce::ARADocumentControllerSpecialisation::
              getSpecialisedDocumentController<PitchNetDocumentController>(
                  araDocController)) {
        // Do NOT clear the realtime-processor binding or persistence callbacks
        // here: both are owned by the processor and must stay live so ARA
        // playback and project restore keep working headlessly after the editor
        // closes. The processor detaches them in its destructor.
        if (pitchDocController->getMainComponent() == mainView.get()) {
          pitchDocController->releaseEditorProcessor(&audioProcessor);
          pitchDocController->setAnalysisCallbacks(nullptr, nullptr);
          pitchDocController->setMainComponent(nullptr);
        }
      }
    }
  }
#endif

  removeMouseListener(this);
  if (ownsProcessorMainView) {
    // Preview/audition is editor-owned in non-ARA mode. Do not leave its
    // per-sample rendering path active after the controls that stop it are gone.
    audioProcessor.stopPluginAudition();
    audioProcessor.getTransportController().clearCallbacks();
    audioProcessor.setMainComponent(nullptr);
  }

  // Destroy all components (and their retained Font/Typeface objects) before
  // releasing the final shared UI resources. This is especially important for
  // DirectWrite memory fonts when a Windows host unloads the plugin DLL.
  mainView.reset();
  shutdownUiResources();
}

void PitchNetAudioProcessorEditor::setupARAMode() {
#if JucePlugin_Enable_ARA
  auto *editorView = getARAEditorView();
  if (!editorView) {
    setupNonARAMode();
    return;
  }

  auto *docController = editorView->getDocumentController();
  if (!docController) {
    setupNonARAMode();
    return;
  }

  auto *pitchDocController = juce::ARADocumentControllerSpecialisation::
      getSpecialisedDocumentController<PitchNetDocumentController>(
          docController);

  if (!pitchDocController) {
    setupNonARAMode();
    return;
  }

  mainView->setRecordControlVisible(false);
  mainView->setOnRecordArmChanged(nullptr);
  // ARA gives us a host link: transport controls and cycle editing are live.
  mainView->setHostTransportControlAvailable(true);

  // Listen for host selection changes to switch the canvas to the clicked
  // region (per-region Projects).
  editorView->addListener(this);

  // ARA is the only mode with playback regions, so the Tracks card lives here.
  // Picking an entry takes the same path as a host selection change.
  mainView->setRegionListVisible(true);
  mainView->setOnRegionSelected([this](const juce::String &regionKey) {
    activateAraRegionByKey(regionKey);
  });

  // Connect ARA controller to UI
  pitchDocController->setMainComponent(mainView.get());
  // Bind via the processor so the realtime-processor link outlives the editor:
  // ARA playback/bounce must keep working after the UI is closed.
  audioProcessor.setAraDocumentController(pitchDocController);
  pitchDocController->setEditorProcessor(&audioProcessor);
  pitchDocController->setAnalysisCallbacks(
      [this](std::uintptr_t sourceKey, double timelineOffsetSeconds,
             const std::vector<std::pair<double, double>> &regionRanges) {
        return audioProcessor.attachCachedAraAnalysis(sourceKey,
                                                      timelineOffsetSeconds,
                                                      regionRanges);
      },
      [this](std::uintptr_t sourceKey,
             const juce::AudioBuffer<float> &buffer, double sampleRate,
             double timelineOffsetSeconds,
             const std::vector<std::pair<double, double>> &regionRanges) {
        audioProcessor.requestAraSourceAnalysis(sourceKey, buffer, sampleRate,
                                                timelineOffsetSeconds,
                                                regionRanges);
      });
  // Persistence callbacks are owned by the processor (set in didBindToARA) so
  // that saved-project restore works with the UI closed; the editor must not
  // install editor-capturing callbacks that would dangle on close.
  mainView->setOnRequestBackendRender([this](const Project &project) {
    audioProcessor.requestPluginProjectRender(project);
  });
  mainView->setOnRequestBackendPreview(
      [pitchDocController](const Project &project, int startFrame,
                           int endFrame) {
        const auto &audioData = project.getAudioData();
        const double sampleRate =
            audioData.sampleRate > 0 ? static_cast<double>(audioData.sampleRate)
                                     : static_cast<double>(SAMPLE_RATE);
        pitchDocController->startPreviewRange(
            static_cast<double>(startFrame) * HOP_SIZE / sampleRate,
            static_cast<double>(endFrame) * HOP_SIZE / sampleRate);
      });
  mainView->setOnStopBackendPreview(
      [pitchDocController]() { pitchDocController->stopPreview(); });
  mainView->setOnRequestDragAudition(
      [pitchDocController](const juce::AudioBuffer<float> &buffer,
                           double sampleRate) {
        pitchDocController->startPreviewAudio(buffer, sampleRate);
      });
  mainView->setOnStopDragAudition(
      [pitchDocController]() { pitchDocController->stopPreview(); });

  mainView->setOnRequestHostPlayState([this](bool shouldPlay) {
    if (auto *araEditorView = getARAEditorView()) {
      if (auto *araDocController = araEditorView->getDocumentController()) {
        if (auto *playbackController =
                araDocController->getHostPlaybackController()) {
          if (shouldPlay)
            playbackController->requestStartPlayback();
          else
            playbackController->requestStopPlayback();
          return;
        }
      }
    }

    audioProcessor.requestHostPlayState(shouldPlay);
  });

  mainView->setOnRequestHostStop([this]() {
    if (auto *araEditorView = getARAEditorView()) {
      if (auto *araDocController = araEditorView->getDocumentController()) {
        if (auto *playbackController =
                araDocController->getHostPlaybackController()) {
          playbackController->requestStopPlayback();
          playbackController->requestSetPlaybackPosition(0.0);
          return;
        }
      }
    }

    audioProcessor.requestHostStop();
  });

  mainView->setOnRequestHostSeek([this](double timeInSeconds) {
    if (auto *araEditorView = getARAEditorView()) {
      if (auto *araDocController = araEditorView->getDocumentController()) {
        if (auto *playbackController =
                araDocController->getHostPlaybackController()) {
          playbackController->requestSetPlaybackPosition(timeInSeconds);
          return;
        }
      }
    }

    audioProcessor.requestHostSeek(timeInSeconds);
  });

  mainView->setOnRequestHostLoopRange(
      [this](double startSeconds, double endSeconds, bool enabled,
             bool hasRange) {
        if (auto *araEditorView = getARAEditorView()) {
          if (auto *araDocController = araEditorView->getDocumentController()) {
            if (auto *playbackController =
                    araDocController->getHostPlaybackController()) {
              if (hasRange && endSeconds > startSeconds)
                playbackController->requestSetCycleRange(
                    startSeconds, endSeconds - startSeconds);

              playbackController->requestEnableCycle(enabled);
              return;
            }
          }
        }

        juce::ignoreUnused(startSeconds, endSeconds, enabled, hasRange);
      });

  setupHostTransportUiSync(true);
  // VocalNet's working Studio One Event FX path reads the editor view's
  // current selection directly instead of waiting for a future listener
  // callback. Studio One can open the editor with a region already selected
  // without subsequently announcing a selection change.
  if (audioProcessor.wrapperType == juce::AudioProcessor::wrapperType_AAX)
    startTimerHz(60);
  else
    // Hosts announce region add/remove through the document controller, not to
    // the editor, so the Tracks card polls for its list. Slow enough to be
    // free, fast enough that dropping a region in feels immediate.
    startTimerHz(kRegionListRefreshHz);

  refreshAraRegionList();

  if (syncInitialARASelectionFromHost())
    return;

  // The playback renderer contains the regions assigned to this plugin
  // instance. Use that set to select the correct track/region sequence rather
  // than guessing from the first source in the shared ARA document.
  if (auto *renderer = audioProcessor.getPlaybackRenderer()) {
    if (pitchDocController->processPlaybackRegions(
            renderer->getPlaybackRegions(), audioProcessor.getSampleRate())) {
      // Hosts are not required to send a selection-change notification when
      // an ARA editor first opens. Activate the region discovered from this
      // instance's renderer immediately instead of relying on onNewSelection().
      audioProcessor.setActiveAraRegion(
          pitchDocController->getCurrentPlaybackRegion());
      return;
    }
  }

  // Check for existing audio sources
  auto *juceDocument = docController->getDocument();
  if (pitchDocController->processExistingAudioSources(
          static_cast<juce::ARADocument *>(juceDocument))) {
    audioProcessor.setActiveAraRegion(
        pitchDocController->getCurrentPlaybackRegion());
    return;
  }
#endif
}

void PitchNetAudioProcessorEditor::timerCallback() {
  syncAAXARAPlayheadStateFromHost();

#if JucePlugin_Enable_ARA
  // The AAX playhead sync needs 60 Hz; the region list does not. Divide down
  // rather than running a second timer.
  if (--regionListRefreshCountdown <= 0) {
    const int interval = juce::jmax(1, getTimerInterval());
    regionListRefreshCountdown =
        juce::jmax(1, (1000 / kRegionListRefreshHz) / interval);
    refreshAraRegionList();
  }
#endif
}

void PitchNetAudioProcessorEditor::syncAAXARAPlayheadStateFromHost() {
#if JucePlugin_Enable_ARA
  if (audioProcessor.wrapperType != juce::AudioProcessor::wrapperType_AAX)
    return;

  if (!getARAEditorView() || !mainView)
    return;

  auto *playHead = audioProcessor.getPlayHead();
  if (!playHead)
    return;

  const auto positionInfo = playHead->getPosition();
  if (!positionInfo.hasValue()) {
    if (lastSyncedHostPlayState) {
      lastSyncedHostPlayState = false;
      mainView->updateHostPlaybackState(false);
    }
    return;
  }

  double timeInSeconds = 0.0;
  if (auto time = positionInfo->getTimeInSeconds())
    timeInSeconds = *time;
  else if (auto samples = positionInfo->getTimeInSamples()) {
    const double sampleRate = audioProcessor.getHostSampleRate();
    if (sampleRate > 0.0)
      timeInSeconds = static_cast<double>(*samples) / sampleRate;
  }

  const bool isPlaying = positionInfo->getIsPlaying();
  const bool positionChanged = !hasSyncedHostPlayhead ||
                               !juce::approximatelyEqual(
                                   lastSyncedHostPlayheadSeconds,
                                   timeInSeconds);
  const bool playStateChanged =
      !hasSyncedHostPlayhead || lastSyncedHostPlayState != isPlaying;

  if (positionChanged) {
    lastSyncedHostPlayheadSeconds = timeInSeconds;
    mainView->updatePlaybackPosition(timeInSeconds);
  }

  if (playStateChanged) {
    lastSyncedHostPlayState = isPlaying;
    mainView->updateHostPlaybackState(isPlaying);
  }

  hasSyncedHostPlayhead = true;
#endif
}

#if JucePlugin_Enable_ARA
bool PitchNetAudioProcessorEditor::syncInitialARASelectionFromHost() {
  auto *editorView = getARAEditorView();
  if (editorView == nullptr)
    return false;

  const auto &viewSelection = editorView->getViewSelection();
  const auto regions =
      viewSelection.getPlaybackRegions<juce::ARAPlaybackRegion>();
  if (regions.empty() || regions.front() == nullptr)
    return false;

  onNewSelection(viewSelection);
  return true;
}

size_t PitchNetAudioProcessorEditor::documentControllerRegionCount() const {
  if (auto *editorView = getARAEditorView()) {
    if (auto *docController = editorView->getDocumentController()) {
      if (auto *pitchDocController = juce::ARADocumentControllerSpecialisation::
              getSpecialisedDocumentController<PitchNetDocumentController>(
                  docController))
        return pitchDocController->getCurrentPlaybackRegions().size();
    }
  }
  return 0;
}

size_t PitchNetAudioProcessorEditor::documentRegionCount() const {
  size_t count = 0;
  if (auto *editorView = getARAEditorView()) {
    if (auto *docController = editorView->getDocumentController()) {
      if (auto *document =
              static_cast<juce::ARADocument *>(docController->getDocument())) {
        for (auto *sequence :
             document->getRegionSequences<juce::ARARegionSequence>())
          if (sequence != nullptr)
            count += sequence->getPlaybackRegions<juce::ARAPlaybackRegion>().size();
      }
    }
  }
  return count;
}

std::vector<juce::ARAPlaybackRegion *>
PitchNetAudioProcessorEditor::collectAraPlaybackRegions() const {
  // A union, not a first-hit lookup. Hosts disagree about what a plug-in
  // instance "owns": some assign every region of the track to the renderer,
  // some assign only the one being played, and per-event hosts assign exactly
  // one. Taking the first non-empty source therefore listed a single region
  // and hid the rest of the track. Instead: gather regions from every source
  // we have, then expand to the full region sequence (track) each of them
  // belongs to, so every clip on the track is selectable no matter which one
  // the host handed us.
  std::vector<juce::ARAPlaybackRegion *> regions;
  std::vector<juce::ARARegionSequence *> sequences;

  const auto isUsable = [](juce::ARAPlaybackRegion *region) {
    return region != nullptr && region->getAudioModification() != nullptr &&
           region->getAudioModification()->getAudioSource() != nullptr;
  };

  const auto addSequence = [&sequences](juce::ARARegionSequence *sequence) {
    if (sequence != nullptr &&
        std::find(sequences.begin(), sequences.end(), sequence) ==
            sequences.end())
      sequences.push_back(sequence);
  };

  const auto addRegions =
      [&](const std::vector<juce::ARAPlaybackRegion *> &candidates) {
        for (auto *candidate : candidates) {
          if (!isUsable(candidate))
            continue;
          if (std::find(regions.begin(), regions.end(), candidate) ==
              regions.end())
            regions.push_back(candidate);
          addSequence(candidate->getRegionSequence());
        }
      };

  // Regions this instance renders.
  if (auto *renderer = audioProcessor.getPlaybackRenderer())
    addRegions(renderer->getPlaybackRegions<juce::ARAPlaybackRegion>());
  if (auto *editorRenderer = audioProcessor.getEditorRenderer())
    addRegions(editorRenderer->getPlaybackRegions<juce::ARAPlaybackRegion>());

  auto *editorView = getARAEditorView();
  if (editorView == nullptr)
    return regions;

  // What the host currently has selected, including the track when only a
  // track is selected.
  const auto &viewSelection = editorView->getViewSelection();
  addRegions(viewSelection.getPlaybackRegions<juce::ARAPlaybackRegion>());
  addRegions(viewSelection.getEffectivePlaybackRegions<juce::ARAPlaybackRegion>());
  for (auto *sequence :
       viewSelection.getRegionSequences<juce::ARARegionSequence>())
    addSequence(sequence);

  auto *docController = editorView->getDocumentController();
  if (docController == nullptr)
    return regions;

  auto *pitchDocController = juce::ARADocumentControllerSpecialisation::
      getSpecialisedDocumentController<PitchNetDocumentController>(
          docController);

  // What the document controller has been working with.
  if (pitchDocController != nullptr) {
    addRegions(pitchDocController->getCurrentPlaybackRegions());
    if (auto *current = pitchDocController->getCurrentPlaybackRegion())
      addRegions({current});
  }

  auto *document =
      static_cast<juce::ARADocument *>(docController->getDocument());

  // Every clip on each track we touched. This is what makes the other regions
  // of a track appear when the host only told us about one of them.
  for (size_t i = 0; i < sequences.size(); ++i)
    addRegions(sequences[i]->getPlaybackRegions<juce::ARAPlaybackRegion>());

  // Nothing anywhere: fall back to the whole document, so the card still lists
  // what PitchNet has detected even before a selection reaches us.
  if (regions.empty() && document != nullptr) {
    for (auto *sequence :
         document->getRegionSequences<juce::ARARegionSequence>())
      if (sequence != nullptr)
        addRegions(sequence->getPlaybackRegions<juce::ARAPlaybackRegion>());
  }

  // Timeline order, so the list reads the way the arrangement does.
  std::stable_sort(regions.begin(), regions.end(),
                   [](juce::ARAPlaybackRegion *a, juce::ARAPlaybackRegion *b) {
                     return a->getStartInPlaybackTime() <
                            b->getStartInPlaybackTime();
                   });
  return regions;
}

void PitchNetAudioProcessorEditor::refreshAraRegionList() {
  if (mainView == nullptr || getARAEditorView() == nullptr)
    return;

  const auto regions = collectAraPlaybackRegions();

  std::vector<MainViewRegionEntry> entries;
  entries.reserve(regions.size());

  int index = 0;
  for (auto *region : regions) {
    ++index;

    juce::String name;
    // getEffectiveName() falls back through modification to audio source, so
    // this is the name the host shows for the clip wherever it set one.
    if (const char *effectiveName = region->getEffectiveName())
      name = juce::String::fromUTF8(effectiveName).trim();
    if (name.isEmpty())
      name = "Region " + juce::String(index);

    // Two clips of the same take share a name; number the repeats so the list
    // stays usable.
    int duplicates = 0;
    for (const auto &existing : entries)
      if (existing.name == name || existing.name.startsWith(name + " ("))
        ++duplicates;
    if (duplicates > 0)
      name << " (" << (duplicates + 1) << ")";

    entries.push_back({pitchnetRegionKey(*region), name});
  }

  const auto activeKey = audioProcessor.getActiveAraRegionKey();

  juce::String signature = activeKey;
  for (const auto &entry : entries)
    signature << "\n" << entry.key << "\t" << entry.name;

  if (regionListPublished && signature == lastPublishedRegionSignature)
    return;

  regionListPublished = true;
  lastPublishedRegionSignature = signature;
  mainView->updateRegionList(entries, activeKey);

  // Only fires when the list actually changes, so this stays quiet - one line
  // per change saying what the card is showing and where it came from.
  juce::String diagnostic;
  diagnostic << "ARA region list: " << static_cast<int>(entries.size())
             << " region(s), active='" << activeKey << "'";
  if (auto *renderer = audioProcessor.getPlaybackRenderer())
    diagnostic << " renderer="
               << static_cast<int>(
                      renderer->getPlaybackRegions<juce::ARAPlaybackRegion>()
                          .size());
  if (auto *editorRenderer = audioProcessor.getEditorRenderer())
    diagnostic << " editorRenderer="
               << static_cast<int>(
                      editorRenderer
                          ->getPlaybackRegions<juce::ARAPlaybackRegion>()
                          .size());
  diagnostic << " docController="
             << static_cast<int>(documentControllerRegionCount())
             << " document=" << static_cast<int>(documentRegionCount());
  for (size_t i = 0; i < entries.size() && i < regions.size(); ++i)
    diagnostic << "\n    [" << static_cast<int>(i) << "] '" << entries[i].name
               << "' " << juce::String(regions[i]->getStartInPlaybackTime(), 3)
               << "s key=" << entries[i].key;
  LOG(diagnostic);
}

void PitchNetAudioProcessorEditor::activateAraRegionByKey(
    const juce::String &regionKey) {
  if (regionKey.isEmpty())
    return;

  // Match against freshly collected regions rather than a stored pointer: the
  // host may have destroyed a region since the list was last published.
  juce::ARAPlaybackRegion *target = nullptr;
  for (auto *region : collectAraPlaybackRegions()) {
    if (pitchnetRegionKey(*region) == regionKey) {
      target = region;
      break;
    }
  }

  if (target == nullptr) {
    // Whatever the card is showing no longer exists; republish on the next tick.
    lastPublishedRegionSignature.clear();
    return;
  }

  if (auto *araEditorView = getARAEditorView()) {
    if (auto *araDocController = araEditorView->getDocumentController()) {
      if (auto *pitchDocController = juce::ARADocumentControllerSpecialisation::
              getSpecialisedDocumentController<PitchNetDocumentController>(
                  araDocController))
        pitchDocController->setCurrentPlaybackRegion(target);
    }
  }

  audioProcessor.setActiveAraRegion(target);
  mainView->focusTimelineRange(target->getStartInPlaybackTime(),
                               target->getEndInPlaybackTime());

  // The active key just changed; let the next refresh publish it.
  lastPublishedRegionSignature.clear();
}

void PitchNetAudioProcessorEditor::onNewSelection(
    const juce::ARAViewSelection &viewSelection) {
  // Switch the canvas to whichever region the user selected in the host. Each
  // region/track carries its own persistent Project, so this swaps what is
  // shown and edited without disturbing the others. Prefer an explicitly
  // selected playback region; fall back to the effective selection (e.g. a
  // time-range/marquee selection that implies a set of regions).
  auto regions = viewSelection.getPlaybackRegions<juce::ARAPlaybackRegion>();
  const auto effective =
      viewSelection.getEffectivePlaybackRegions<juce::ARAPlaybackRegion>();
  const auto sequences =
      viewSelection.getRegionSequences<juce::ARARegionSequence>();

  std::vector<juce::ARAPlaybackRegion *> rendererRegions;
  if (auto *renderer = audioProcessor.getPlaybackRenderer())
    rendererRegions = renderer->getPlaybackRegions<juce::ARAPlaybackRegion>();

  const auto isAssignedToThisInstance =
      [&rendererRegions](juce::ARAPlaybackRegion *region) {
        return rendererRegions.empty() ||
               std::find(rendererRegions.begin(), rendererRegions.end(),
                         region) != rendererRegions.end();
      };

  const auto pickAssignedRegion =
      [&isAssignedToThisInstance](
          const std::vector<juce::ARAPlaybackRegion *> &candidates)
      -> juce::ARAPlaybackRegion * {
    for (auto *candidate : candidates)
      if (candidate != nullptr && isAssignedToThisInstance(candidate))
        return candidate;

    return nullptr;
  };

  juce::ARAPlaybackRegion *target = nullptr;
  if (!regions.empty())
    target = pickAssignedRegion(regions);
  if (target == nullptr && !effective.empty())
    target = pickAssignedRegion(effective);
  if (target == nullptr && !sequences.empty()) {
    // Only a track/region-sequence was selected: focus its first region that
    // belongs to this plugin instance. AAX can report selections from the
    // shared ARA document, where blindly taking the first region sticks to the
    // first track/audio source.
    for (auto *sequence : sequences) {
      if (sequence == nullptr)
        continue;

      const auto &seqRegions =
          sequence->getPlaybackRegions<juce::ARAPlaybackRegion>();
      target = pickAssignedRegion(seqRegions);
      if (target != nullptr)
        break;
    }
  }

  if (target == nullptr) {
    if (!regions.empty())
      target = regions.front();
    else if (!effective.empty())
      target = effective.front();
    else if (!sequences.empty()) {
      const auto &seqRegions =
          sequences.front()->getPlaybackRegions<juce::ARAPlaybackRegion>();
      if (!seqRegions.empty())
        target = seqRegions.front();
    }
  }

  if (target != nullptr) {
    if (auto *araEditorView = getARAEditorView()) {
      if (auto *araDocController = araEditorView->getDocumentController()) {
        if (auto *pitchDocController =
                juce::ARADocumentControllerSpecialisation::
                    getSpecialisedDocumentController<
                        PitchNetDocumentController>(araDocController))
          pitchDocController->setCurrentPlaybackRegion(target);
      }
    }
    audioProcessor.setActiveAraRegion(target);
    mainView->focusTimelineRange(target->getStartInPlaybackTime(),
                                 target->getEndInPlaybackTime());
  }

  // The selection is also what tells us which track's regions to list, so
  // update the Tracks card now instead of waiting for the next poll.
  refreshAraRegionList();
}
#endif

void PitchNetAudioProcessorEditor::setupNonARAMode() {
  // Without ARA the plugin cannot drive the host transport, so play / stop /
  // cycle and cycle-range editing are disabled - capture is the only transport
  // action available here.
  mainView->setHostTransportControlAvailable(false);
  mainView->setRecordControlVisible(true);
  mainView->updateRecordArmState(audioProcessor.isCaptureArmed());
  mainView->setOnRecordArmChanged([this](bool armed) {
    if (armed)
      audioProcessor.startCapture();
    else
      audioProcessor.stopCapture();
  });
  mainView->setOnRequestBackendRender([this](const Project &project) {
    audioProcessor.requestPluginProjectRender(project);
  });
  mainView->setOnRequestBackendPreview(
      [this](const Project &project, int startFrame, int endFrame) {
        const auto &audioData = project.getAudioData();
        const double sampleRate =
            audioData.sampleRate > 0 ? static_cast<double>(audioData.sampleRate)
                                     : static_cast<double>(SAMPLE_RATE);
        audioProcessor.startPluginPreview(
            static_cast<double>(startFrame) * HOP_SIZE / sampleRate,
            static_cast<double>(endFrame) * HOP_SIZE / sampleRate);
      });
  mainView->setOnStopBackendPreview(
      [this]() { audioProcessor.stopPluginPreview(); });
  mainView->setOnRequestDragAudition(
      [this](const juce::AudioBuffer<float> &buffer, double sampleRate) {
        audioProcessor.startPluginAudition(buffer, sampleRate);
      });
  mainView->setOnStopDragAudition(
      [this]() { audioProcessor.stopPluginAudition(); });

  // Setup host transport control callbacks for non-ARA mode
  mainView->setOnRequestHostPlayState([this](bool shouldPlay) {
    audioProcessor.requestHostPlayState(shouldPlay);
  });

  mainView->setOnRequestHostStop([this]() {
    audioProcessor.requestHostStop();
  });

  mainView->setOnRequestHostSeek([this](double timeInSeconds) {
    audioProcessor.requestHostSeek(timeInSeconds);
  });

  mainView->setOnRequestHostLoopRange(nullptr);

  setupHostTransportUiSync(true);
}

void PitchNetAudioProcessorEditor::setupHostTransportUiSync(
    bool includePlayState) {
  auto safeMain =
      juce::Component::SafePointer<juce::Component>(mainView->getComponent());

  if (includePlayState) {
    audioProcessor.getTransportController().setPlayStateCallback(
        [safeMain](bool isPlaying) {
          auto *view = dynamic_cast<IMainView *>(safeMain.getComponent());
          if (!view)
            return;

          view->updateHostPlaybackState(isPlaying);
        });
  } else {
    audioProcessor.getTransportController().setPlayStateCallback(nullptr);
  }

  audioProcessor.getTransportController().setPositionCallback(
      [safeMain](double timeInSeconds) {
        auto *view = dynamic_cast<IMainView *>(safeMain.getComponent());
        if (!view)
          return;

        view->updatePlaybackPosition(timeInSeconds);
      });

  audioProcessor.getTransportController().setLoopCallback(
      [safeMain](const HostSyncService::LoopInfo &loop) {
        if (auto *view = dynamic_cast<IMainView *>(safeMain.getComponent()))
          view->updateHostLoopRange(loop.loopStartSeconds, loop.loopEndSeconds,
                                    loop.isLoopEnabled, loop.hasLoopPoints);
      });

  audioProcessor.getTransportController().setTempoCallback(
      [safeMain](const HostSyncService::TempoInfo &tempo) {
        if (!tempo.hasBpm || !tempo.hasTimeSignature)
          return;

        if (auto *view = dynamic_cast<IMainView *>(safeMain.getComponent()))
          view->updateHostTimelineState(tempo.bpm, tempo.timeSigNumerator,
                                        tempo.timeSigDenominator);
      });

  const auto hostState =
      audioProcessor.getTransportController().getCurrentState();
  if (hostState.tempo.hasBpm && hostState.tempo.hasTimeSignature)
    if (auto *view = dynamic_cast<IMainView *>(safeMain.getComponent()))
      view->updateHostTimelineState(hostState.tempo.bpm,
                                    hostState.tempo.timeSigNumerator,
                                    hostState.tempo.timeSigDenominator);

  syncHostLoopSnapshotFromPlayHead();
}

void PitchNetAudioProcessorEditor::syncHostLoopSnapshotFromPlayHead() {
  auto *playHead = audioProcessor.getPlayHead();
  if (!playHead)
    return;

  auto posInfo = playHead->getPosition();
  if (!posInfo.hasValue())
    return;

  double startSeconds = 0.0;
  double endSeconds = 0.0;
  bool hasRange = false;

  if (auto loopPoints = posInfo->getLoopPoints()) {
    if (auto bpm = posInfo->getBpm()) {
      if (*bpm > 0.0) {
        startSeconds = loopPoints->ppqStart * 60.0 / *bpm;
        endSeconds = loopPoints->ppqEnd * 60.0 / *bpm;
        hasRange = endSeconds > startSeconds;
      }
    }
  }

  if (auto *view = mainView.get())
    view->updateHostLoopRange(startSeconds, endSeconds,
                              posInfo->getIsLooping(), hasRange);
}

void PitchNetAudioProcessorEditor::setupCallbacks() {
  // When project data changes (analysis complete or synthesis complete)
  mainView->setOnProjectDataChanged([this]() {
    if (auto *project = mainView->getProject())
      audioProcessor.updateProjectStateFromEditor(*project);
    mainView->bindRealtimeProcessor(audioProcessor.getRealtimeProcessor());
    audioProcessor.getRealtimeProcessor().invalidate();
  });

  // onPitchEditFinished is handled by onProjectDataChanged (called after async
  // synthesis completes) No need for separate callback here
}

void PitchNetAudioProcessorEditor::paint(juce::Graphics &) {
  // MainComponent handles all painting
}

void PitchNetAudioProcessorEditor::resized() {
  mainView->getComponent()->setBounds(getLocalBounds());
  requestMainViewKeyboardFocusAsync();
}

void PitchNetAudioProcessorEditor::parentHierarchyChanged() {
  AudioProcessorEditor::parentHierarchyChanged();
  applyLunaSoftwareRenderer();
}

void PitchNetAudioProcessorEditor::visibilityChanged() {
  if (isVisible()) {
    applyLunaSoftwareRenderer();
    requestMainViewKeyboardFocusAsync();
  }
}

void PitchNetAudioProcessorEditor::mouseDown(const juce::MouseEvent &e) {
  juce::ignoreUnused(e);
  requestMainViewKeyboardFocusAsync();
}

void PitchNetAudioProcessorEditor::requestMainViewKeyboardFocus() {
  auto *component = mainView ? mainView->getComponent() : nullptr;
  if (!component || (!component->isShowing() && !component->isOnDesktop()))
    return;

  if (auto *focused = juce::Component::getCurrentlyFocusedComponent()) {
    if (focused == this || isParentOf(focused) || focused == component ||
        component->isParentOf(focused))
      return;
  }

  if (auto *peer = getPeer())
    peer->grabFocus();

  grabKeyboardFocus();
  component->grabKeyboardFocus();
}

void PitchNetAudioProcessorEditor::requestMainViewKeyboardFocusAsync() {
  juce::Component::SafePointer<PitchNetAudioProcessorEditor> safeThis(this);
  juce::MessageManager::callAsync([safeThis]() {
    if (safeThis != nullptr) {
      safeThis->applyLunaSoftwareRenderer();
      safeThis->requestMainViewKeyboardFocus();
    }
  });
}

void PitchNetAudioProcessorEditor::applyLunaSoftwareRenderer() {
#if JUCE_WINDOWS
  if (lunaSoftwareRendererApplied || !isLunaHostProcess())
    return;

  auto *peer = getPeer();
  if (!peer)
    return;

  const auto engines = peer->getAvailableRenderingEngines();
  const int softwareRenderer = engines.indexOf("Software Renderer");
  if (softwareRenderer < 0) {
    lunaSoftwareRendererApplied = true;
    LOG("LUNA host detected, but JUCE software renderer is unavailable");
    return;
  }

  lunaSoftwareRendererApplied = true;
  if (peer->getCurrentRenderingEngine() != softwareRenderer)
    peer->setCurrentRenderingEngine(softwareRenderer);

  LOG("LUNA host detected: forcing JUCE software renderer");
#endif
}
