// =============================================================================
// MainComponent member functions for project/audio file I/O.
// Split from MainComponent.cpp to reduce file size.
// =============================================================================

#include "../MainComponent.h"
#include "../../Models/ProjectSerializer.h"
#include "../../Utils/AppLogger.h"
#include "../../Utils/Constants.h"
#include "../../Utils/Localization.h"
#include "../../Utils/MelSpectrogram.h"
#include "../../Utils/PitchCurveProcessor.h"
#include "../../Utils/PlatformPaths.h"
#include "../../Utils/SHA256Utils.h"

void MainComponent::openProjectFile(const juce::File &file) {
  if (isLoadingAudio.load())
    return;
  addRecentFile(file);

  auto loadedProject = std::make_shared<Project>();
  if (!ProjectSerializer::loadFromFile(*loadedProject, file)) {
    StyledMessageBox::show(this, "Open failed",
                           "Failed to load project:\n" + file.getFullPathName(),
                           StyledMessageBox::WarningIcon);
    return;
  }
  loadedProject->setProjectFilePath(file);

  // .pitchnet files embed the rendered waveform and analysis data. Prefer this
  // self-contained state so projects reopen exactly as saved, even if the
  // original audio file has moved or changed.
  if (loadedProject->getAudioData().waveform.getNumSamples() > 0) {
    loadedProject->recomposeFromSynthIfPresent();

    if (editorController)
      editorController->setProject(std::make_unique<Project>(*loadedProject));

    auto *project = getProject();
    if (!project)
      return;

    pianoRoll.setProject(project);
    pianoRollView.setProject(project);
    parameterPanel.setProject(project);
    parameterPanel.setSelectedNote(nullptr);
    toolbar.setTotalTime(project->getAudioData().getDuration());
    toolbar.setLoopEnabled(project->getLoopRange().enabled);
    toolbar.setTransportEnabled(true);

    if (!isPluginMode()) {
      if (auto *engine = editorController ? editorController->getAudioEngine()
                                           : nullptr) {
        try {
          engine->loadWaveform(project->getAudioData().waveform,
                               project->getAudioData().sampleRate);
          const auto &loopRange = project->getLoopRange();
          if (loopRange.enabled)
            engine->setLoopRange(loopRange.startSeconds, loopRange.endSeconds);
          engine->setLoopEnabled(loopRange.enabled);
          engine->setVolumeDb(project->getVolume());
        } catch (...) {
        }
      }
    }

    if (hasAnalyzedProject())
      fitAnalyzedPitchRangeToView(*project);
    repaint();
    if (isPluginMode())
      notifyProjectDataChanged();
    return;
  }

  auto continueOpenWithAudio = [this, loadedProject](const juce::File &audioFile) {
    if (!audioFile.existsAsFile()) {
      StyledMessageBox::show(this, "Open failed",
                             "Project audio file not found:\n" +
                                 audioFile.getFullPathName(),
                             StyledMessageBox::WarningIcon);
      return;
    }

    loadedProject->setFilePath(audioFile);

    const juce::String currentAudioSha = SHA256Utils::fileSHA256(audioFile);
    const juce::String savedAudioSha = loadedProject->getAudioSha256();
    const bool shaMatched = savedAudioSha.isNotEmpty() &&
                            savedAudioSha.equalsIgnoreCase(currentAudioSha);

    auto proceedWithProject =
        [this, loadedProject, audioFile, currentAudioSha](bool reanalyze) {
          if (reanalyze) {
            loadAudioFile(audioFile);
            return;
          }

          isLoadingAudio = true;
          loadingProgress = 0.0;
          {
            const juce::ScopedLock sl(loadingMessageLock);
            loadingMessage = TR("progress.loading_audio");
          }
          toolbar.hideProgress();
          showAnalysisProgress(0.0);

          juce::Component::SafePointer<MainComponent> safeThis(this);
          fileManager->loadAudioFileAsync(
              audioFile,
              [safeThis](double p, const juce::String &msg) {
                if (safeThis == nullptr)
                  return;
                safeThis->loadingProgress = juce::jlimit(0.0, 1.0, p);
                const juce::ScopedLock sl(safeThis->loadingMessageLock);
                safeThis->loadingMessage = msg;
              },
              [safeThis, loadedProject, currentAudioSha](
                  juce::AudioBuffer<float> &&buffer, int sampleRate,
                  const juce::File &loadedFile) mutable {
                if (safeThis == nullptr)
                  return;

                auto projectToUse = std::make_unique<Project>(*loadedProject);
                projectToUse->setFilePath(loadedFile);
                projectToUse->setAudioSha256(currentAudioSha);

                auto &audioData = projectToUse->getAudioData();
                audioData.waveform = std::move(buffer);
                audioData.sampleRate = sampleRate;
                audioData.originalWaveform.makeCopyOf(audioData.waveform);

                // Recompute mel only; keep pitch/note edits from project file.
                if (audioData.waveform.getNumSamples() > 0) {
                  const float *samples = audioData.waveform.getReadPointer(0);
                  const int numSamples = audioData.waveform.getNumSamples();
                  MelSpectrogram melComputer(audioData.sampleRate, N_FFT,
                                             HOP_SIZE, NUM_MELS, FMIN, FMAX);
                  audioData.melSpectrogram =
                      melComputer.compute(samples, numSamples);
                }

                if (audioData.voicedMask.empty() && !audioData.f0.empty()) {
                  audioData.voicedMask.resize(audioData.f0.size(), false);
                  for (size_t i = 0; i < audioData.f0.size(); ++i)
                    audioData.voicedMask[i] = audioData.f0[i] > 0.0f;
                }

                if (audioData.basePitch.empty() || audioData.deltaPitch.empty()) {
                  if (!audioData.f0.empty()) {
                    PitchCurveProcessor::rebuildCurvesFromSource(*projectToUse,
                                                                 audioData.f0);
                  } else if (!audioData.melSpectrogram.empty()) {
                    // Legacy project fallback: rebuild base from notes and use
                    // zero delta so reopening can still synthesize edited notes.
                    PitchCurveProcessor::rebuildBaseFromNotes(*projectToUse);
                  }
                } else {
                  PitchCurveProcessor::composeF0InPlace(*projectToUse,
                                                        /*applyUvMask=*/false);
                }

                if (safeThis->undoManager)
                  safeThis->undoManager->clear();

                if (safeThis->editorController)
                  safeThis->editorController->setProject(std::move(projectToUse));

                auto *project = safeThis->getProject();
                if (!project) {
                  safeThis->isLoadingAudio = false;
                  return;
                }

                safeThis->pianoRoll.setProject(project);
                safeThis->pianoRollView.setProject(project);
                safeThis->parameterPanel.setProject(project);
                safeThis->toolbar.setTotalTime(project->getAudioData().getDuration());
                safeThis->toolbar.setLoopEnabled(project->getLoopRange().enabled);
                safeThis->toolbar.setTransportEnabled(true);

                auto &activeAudioData = project->getAudioData();
                if (safeThis->isPluginMode()) {
                  // plugin mode: no audio engine
                } else if (auto *engine = safeThis->editorController
                                               ? safeThis->editorController->getAudioEngine()
                                               : nullptr) {
                  try {
                    engine->loadWaveform(activeAudioData.waveform,
                                         activeAudioData.sampleRate);
                    const auto &loopRange = project->getLoopRange();
                    if (loopRange.enabled)
                      engine->setLoopRange(loopRange.startSeconds,
                                           loopRange.endSeconds);
                    engine->setLoopEnabled(loopRange.enabled);
                    engine->setVolumeDb(project->getVolume());
                  } catch (...) {
                  }
                }

                if (auto *vocoder = safeThis->editorController
                                        ? safeThis->editorController->getVocoder()
                                        : nullptr;
                    vocoder && !vocoder->isLoaded()) {
                  auto modelPath = PlatformPaths::getVocoderModelFile();
                  if (modelPath.exists()) {
                    if (!vocoder->loadModel(modelPath)) {
                      juce::AlertWindow::showMessageBoxAsync(
                          juce::AlertWindow::WarningIcon, "Inference failed",
                          "Failed to load vocoder model at:\n" +
                              modelPath.getFullPathName() +
                              "\n\nPlease check your model installation and try again.");
                      safeThis->isLoadingAudio = false;
                      return;
                    }
                  } else {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon, "Missing model file",
                        "The vocoder model was not found at:\n" +
                            modelPath.getFullPathName() +
                            "\n\nPlease install the required model files and try again.");
                    safeThis->isLoadingAudio = false;
                    return;
                  }
                }

                // Skip full re-analysis; run vocoder from loaded edits.
                const int totalFrames = std::max(
                    static_cast<int>(activeAudioData.melSpectrogram.size()),
                    std::max(static_cast<int>(activeAudioData.f0.size()),
                             static_cast<int>(activeAudioData.basePitch.size())));
                if (totalFrames > 0) {
                  project->setF0DirtyRange(0, totalFrames);

                  safeThis->toolbar.showProgress(TR("progress.synthesizing"));
                  safeThis->toolbar.setProgress(-1.0f);
                  safeThis->toolbar.setEnabled(false);

                  safeThis->editorController->resynthesizeIncrementalAsync(
                      *project,
                      [safeThis](const juce::String &message) {
                        if (safeThis == nullptr)
                          return;
                        safeThis->toolbar.showProgress(message);
                      },
                      [safeThis](bool success) {
                        if (safeThis == nullptr)
                          return;

                        safeThis->toolbar.setEnabled(true);
                        safeThis->toolbar.hideProgress();
                        safeThis->isLoadingAudio = false;
                        safeThis->repaint();

                        if (!success) {
                          StyledMessageBox::show(
                              safeThis.getComponent(), "Open warning",
                              "Project opened, but applying saved pitch edits failed.\n"
                              "You can click Re-analyze to rebuild pitch data.",
                              StyledMessageBox::WarningIcon);
                          return;
                        }

                        if (safeThis->isPluginMode())
                          safeThis->notifyProjectDataChanged();
                      },
                      safeThis->pendingIncrementalResynth,
                      safeThis->isPluginMode());
                  return;
                }

                safeThis->repaint();
                safeThis->isLoadingAudio = false;
                if (safeThis->isPluginMode())
                  safeThis->notifyProjectDataChanged();
              });
        };

    if (!shaMatched) {
      juce::AlertWindow::showOkCancelBox(
          juce::AlertWindow::WarningIcon, "Audio file changed",
          "The saved audio hash does not match current file:\n" +
              audioFile.getFullPathName() +
              "\n\nDo you want to re-analyze this audio?",
          "Re-analyze", "Use Saved Edits", this,
          juce::ModalCallbackFunction::create([proceedWithProject](int result) {
            proceedWithProject(result != 0);
          }));
      return;
    }

    proceedWithProject(false);
  };

  const juce::File audioFile = loadedProject->getFilePath();
  if (!audioFile.existsAsFile()) {
    juce::AlertWindow::showOkCancelBox(
        juce::AlertWindow::WarningIcon, "Audio file missing",
        "Project audio file was not found:\n" + audioFile.getFullPathName() +
            "\n\nDo you want to locate a replacement audio file?",
        "Locate Audio", "Cancel", this,
        juce::ModalCallbackFunction::create(
            [this, continueOpenWithAudio](int result) {
              if (result == 0)
                return;
              if (fileChooser != nullptr)
                return;
              fileChooser = std::make_unique<juce::FileChooser>(
                  TR("dialog.select_audio"), juce::File{},
                  "*.wav;*.mp3;*.flac;*.aiff;*.ogg;*.m4a");
              auto chooserFlags = juce::FileBrowserComponent::openMode |
                                  juce::FileBrowserComponent::canSelectFiles;
              juce::Component::SafePointer<MainComponent> safeThis(this);
              fileChooser->launchAsync(
                  chooserFlags, [safeThis, continueOpenWithAudio](
                                    const juce::FileChooser &fc) {
                    if (safeThis == nullptr)
                      return;
                    auto selected = fc.getResult();
                    safeThis->fileChooser.reset();
                    if (selected.existsAsFile())
                      continueOpenWithAudio(selected);
                  });
            }));
    return;
  }

  continueOpenWithAudio(audioFile);
}

void MainComponent::loadAudioFile(const juce::File &file) {
  if (isLoadingAudio.load())
    return;
  addRecentFile(file);

  isLoadingAudio = true;
  loadingProgress = 0.0;
  {
    const juce::ScopedLock sl(loadingMessageLock);
    loadingMessage = TR("progress.loading_audio");
  }
  toolbar.hideProgress();
  clearProjectForNewLoad();
  showAnalysisProgress(0.0);

  juce::Component::SafePointer<MainComponent> safeThis(this);
  if (!editorController) {
    isLoadingAudio = false;
    return;
  }

  editorController->loadAudioFileAsync(
      file,
      [safeThis](double p, const juce::String &msg) {
        if (safeThis == nullptr)
          return;
        safeThis->loadingProgress = juce::jlimit(0.0, 1.0, p);
        const juce::ScopedLock sl(safeThis->loadingMessageLock);
        safeThis->loadingMessage = msg;
      },
      [safeThis](const juce::AudioBuffer<float> &original) {
        if (safeThis == nullptr)
          return;

        // Clear undo history before replacing project to avoid dangling pointers
        if (safeThis->undoManager)
          safeThis->undoManager->clear();

        auto *project = safeThis->getProject();
        if (!project)
          return;
        if (!safeThis->isPluginMode())
          project->setTimelineDisplayMode(TimelineDisplayMode::Time);

        // Update UI
        safeThis->pianoRoll.setProject(project);
        safeThis->pianoRollView.setProject(project);
        safeThis->parameterPanel.setProject(project);
        safeThis->toolbar.setTotalTime(project->getAudioData().getDuration());
        safeThis->toolbar.setLoopEnabled(project->getLoopRange().enabled);
        safeThis->toolbar.setTransportEnabled(true);

        auto &audioData = project->getAudioData();

        if (safeThis->isPluginMode()) {
          // plugin mode: no audio engine
        } else if (auto *engine = safeThis->editorController
                                       ? safeThis->editorController->getAudioEngine()
                                       : nullptr) {
          try {
            engine->loadWaveform(audioData.waveform, audioData.sampleRate);
            const auto &loopRange = project->getLoopRange();
            if (loopRange.enabled)
              engine->setLoopRange(loopRange.startSeconds,
                                   loopRange.endSeconds);
            engine->setLoopEnabled(loopRange.enabled);
            engine->setVolumeDb(project->getVolume());
          } catch (...) {
          }
        }

        safeThis->fitAnalyzedPitchRangeToView(*project);

        if (auto *vocoder = safeThis->editorController
                                ? safeThis->editorController->getVocoder()
                                : nullptr;
            vocoder && !vocoder->isLoaded()) {
          auto modelPath = PlatformPaths::getVocoderModelFile();
          if (modelPath.exists()) {
            if (!vocoder->loadModel(modelPath)) {
              juce::AlertWindow::showMessageBoxAsync(
                  juce::AlertWindow::WarningIcon, "Inference failed",
                  "Failed to load vocoder model at:\n" +
                      modelPath.getFullPathName() +
                      "\n\nPlease check your model installation and try again.");
              return;
            }
          } else {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon, "Missing model file",
                "The vocoder model was not found at:\n" +
                    modelPath.getFullPathName() +
                    "\n\nPlease install the required model files and try again.");
            return;
          }
        }

        safeThis->repaint();
        safeThis->isLoadingAudio = false;

        if (safeThis->isPluginMode())
          safeThis->notifyProjectDataChanged();
      },
      [safeThis]() {
        if (safeThis == nullptr)
          return;
        safeThis->isLoadingAudio = false;
      });
}

void MainComponent::clearProjectForNewLoad() {
  isPlaying = false;
  pendingCursorTime.store(0.0);
  hasPendingCursorUpdate.store(false);

  if (undoManager)
    undoManager->clear();

  if (auto *audioEngine = editorController ? editorController->getAudioEngine() : nullptr) {
    audioEngine->stop();
    audioEngine->seek(0.0);
    audioEngine->clearLoopRange();
  }

  if (editorController) {
    auto emptyProject = std::make_unique<Project>();
    if (!isPluginMode())
      emptyProject->setTimelineDisplayMode(TimelineDisplayMode::Time);
    editorController->setProject(std::move(emptyProject));
  }

  auto *project = getProject();
  pianoRoll.setProject(project);
  pianoRollView.setProject(project);
  parameterPanel.setProject(project);
  parameterPanel.setSelectedNote(nullptr);
  toolbar.setPlaying(false);
  toolbar.setCurrentTime(0.0);
  toolbar.setTotalTime(0.0);
  toolbar.setLoopEnabled(false);
  toolbar.setTransportEnabled(isPluginMode());
  pianoRoll.setCursorTime(0.0);
}
