#pragma once

#include "../JuceHeader.h"
#include "RegionPreview.h"
#include <functional>
#include <memory>
#include <vector>

class Project;
class Vocoder;
class RealtimePitchProcessor;
class EditorController;
class PitchUndoManager;

struct MainViewViewportState
{
  double scrollX = 0.0;
  double scrollY = 0.0;
  float pixelsPerSecond = 100.0f;
  float pixelsPerSemitone = 12.0f;
  bool valid = false;
};

/**
 * One selectable region, as the side panel sees it.
 *
 * The UI must not depend on the ARA SDK, so regions reach it as a stable key
 * (the plug-in's persistent region key) plus the name to display. The key is
 * what comes back when the user picks an entry.
 */
struct MainViewRegionEntry {
  juce::String key;
  juce::String name;
};

/**
 * One track (ARA region sequence) that has PitchNet on it, as the left side
 * panel sees it. The key is session-local and only used to mark the active
 * track; the colour is the DAW's track colour sent through ARA, when it sent
 * one. The panel decides whether to use it or its neutral default.
 */
struct MainViewTrackEntry {
  juce::String key;
  juce::String name;
  juce::Colour colour;        // the DAW's colour; only valid if hasHostColour
  bool hasHostColour = false; // false when the DAW sent no colour via ARA
  bool pinned = false;        // notes drawn on the canvas; never the active track
};

/** Track colour when the DAW sends none, or "Use DAW Track Color" is off. */
constexpr juce::uint32 kDefaultTrackColour = 0xFFE6E6E6u;

/** The colour a track is shown in, given the "Use DAW Track Color" switch. */
inline juce::Colour resolveTrackColour(const MainViewTrackEntry &track,
                                       bool useDawTrackColour) {
  return useDawTrackColour && track.hasHostColour
             ? track.colour
             : juce::Colour(kDefaultTrackColour);
}

class IMainView {
public:
  virtual ~IMainView() = default;

  virtual juce::Component *getComponent() = 0;
  virtual Project *getProject() const = 0;
  virtual std::unique_ptr<Project>
  exchangeProject(std::unique_ptr<Project> project) = 0;
  virtual Vocoder *getVocoder() const = 0;
  virtual void bindBackendController(EditorController *controller) = 0;
  virtual void bindUndoManager(PitchUndoManager *manager) = 0;
  virtual MainViewViewportState getViewportState() const = 0;
  virtual void restoreViewportState(const MainViewViewportState &state) = 0;
  virtual bool hasAnalyzedProject() const = 0;
  virtual void bindRealtimeProcessor(RealtimePitchProcessor &processor) = 0;
  virtual juce::String serializeProjectJson() const = 0;
  virtual bool restoreProjectJson(const juce::String &json) = 0;
  virtual bool restoreProjectSnapshot(const Project &project) = 0;
  virtual void setStatusMessage(const juce::String &message) = 0;
  virtual void showAnalysisProgress(double progress) = 0;
  virtual void hideAnalysisProgress() = 0;
  virtual void finishBackendRender(bool success) = 0;
  virtual void setOnProjectDataChanged(std::function<void()> callback) = 0;
  virtual void setOnPitchEditFinished(std::function<void()> callback) = 0;
  virtual void setOnRequestBackendRender(
      std::function<void(const Project &)> callback) = 0;
  virtual void setOnRequestBackendPreview(
      std::function<void(const Project &, int, int)> callback) = 0;
  virtual void setOnStopBackendPreview(std::function<void()> callback) = 0;
  virtual void setOnRequestDragAudition(
      std::function<void(const juce::AudioBuffer<float> &, double)> callback) = 0;
  virtual void setOnStopDragAudition(std::function<void()> callback) = 0;
  virtual void setOnRequestHostPlayState(
      std::function<void(bool)> callback) = 0;
  virtual void setOnRequestHostStop(std::function<void()> callback) = 0;
  virtual void setOnRequestHostSeek(
      std::function<void(double)> callback) = 0;
  virtual void setOnRequestHostLoopRange(
      std::function<void(double, double, bool, bool)> callback) = 0;
  virtual void setOnRecordArmChanged(std::function<void(bool)> callback) = 0;
  virtual void setRecordControlVisible(bool visible) = 0;
  /**
   * Show or hide the Regions card in the side panel. Only ARA plugin mode has
   * playback regions to list, so only that mode turns it on.
   */
  virtual void setRegionListVisible(bool visible) = 0;
  /**
   * Replace the regions the Regions card lists and mark which one the canvas is
   * currently showing (an empty key means none of them).
   */
  virtual void updateRegionList(const std::vector<MainViewRegionEntry> &regions,
                                const juce::String &activeKey) = 0;
  /**
   * Replace the tracks the left side panel lists and mark the active one (an
   * empty key means none of them).
   */
  virtual void updateTrackList(const std::vector<MainViewTrackEntry> &tracks,
                               const juce::String &activeKey) = 0;
  /** Called with a track key when the user clicks an inactive track. */
  virtual void setOnTrackSelected(
      std::function<void(const juce::String &)> callback) = 0;
  /** Called with a track key and the new state when a track's pin is toggled. */
  virtual void setOnTrackPinChanged(
      std::function<void(const juce::String &, bool)> callback) = 0;
  /** State of the "Use DAW Track Color" switch. */
  virtual bool getUseDawTrackColour() const = 0;
  /** Called after the "Use DAW Track Color" switch changes. */
  virtual void setOnTrackColourModeChanged(std::function<void()> callback) = 0;
  /** Called with a region key when the user picks one in the Regions card. */
  virtual void setOnRegionSelected(
      std::function<void(const juce::String &)> callback) = 0;
  /**
   * Draw every region of the track on the canvas and thumbnail. Exactly one
   * entry may be flagged active; it keeps the editable backdrop, the others
   * are shown dimmed and read-only. An empty list shows only the canvas
   * project, as in standalone mode.
   */
  virtual void updateRegionPreviews(MainViewRegionPreviewList previews) = 0;
  /**
   * Called with a region key when the user clicks inside an inactive region on
   * the canvas (empty space or one of its notes). Unlike setOnRegionSelected,
   * the handler must not scroll or refit the view: the user clicked on what is
   * already visible.
   */
  virtual void setOnRegionActivationRequested(
      std::function<void(const juce::String &)> callback) = 0;
  /**
   * Clip / Track view (ARA only). Clip mode shows the selected region's
   * modification in its own time; Track mode shows every region of the track
   * on the host timeline (region previews are only drawn there). Making the
   * mode available shows its toggle button and restores the saved choice.
   */
  virtual void setTrackViewModeAvailable(bool available) = 0;
  virtual bool isTrackViewMode() const = 0;
  virtual void setOnTrackViewModeChanged(
      std::function<void(bool)> callback) = 0;
  /**
   * Tell the view whether the host transport can be driven from here.
   * False in non-ARA plugin mode: play / stop / cycle controls and cycle-range
   * editing are disabled, leaving capture as the only transport action.
   */
  virtual void setHostTransportControlAvailable(bool available) = 0;
  virtual void updateRecordArmState(bool armed) = 0;
  virtual void beginLiveRecording(double sampleRate,
                                  double timelineOffsetSeconds) = 0;
  virtual void appendLiveRecordingAudio(
      const juce::AudioBuffer<float> &buffer) = 0;

  virtual void setHostAudio(const juce::AudioBuffer<float> &buffer,
                            double sampleRate,
                            double timelineOffsetSeconds = 0.0) = 0;
  virtual void updateHostAudioTimelineOffset(double timelineOffsetSeconds) = 0;
  virtual void clearHostAudio() = 0;
  virtual void focusTimelineRange(double startSeconds, double endSeconds) = 0;
  virtual void updatePlaybackPosition(double timeSeconds) = 0;
  // Where the drawn project content sits on the host timeline, in seconds.
  // Display only; may be negative. See CoordinateMapper for the full contract.
  virtual void setTimelineDisplayOffset(double seconds) = 0;
  virtual void updateHostPlaybackState(bool isPlaying) = 0;
  virtual void updateHostTimelineState(double bpm, int numerator,
                                       int denominator) = 0;
  virtual void updateHostLoopRange(double startSeconds, double endSeconds,
                                   bool enabled, bool hasRange) = 0;
  virtual void notifyHostStopped() = 0;

  /**
   * Trigger re-synthesis from external parameter changes (e.g. DAW automation).
   * Called on the message thread when plugin parameters like pitch offset or
   * formant shift are changed via host automation.
   */
  virtual void triggerResynthesis() = 0;
};
