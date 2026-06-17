#pragma once

#include "../JuceHeader.h"
#include <functional>

class Project;
class Vocoder;
class RealtimePitchProcessor;

class IMainView {
public:
  virtual ~IMainView() = default;

  virtual juce::Component *getComponent() = 0;
  virtual Project *getProject() const = 0;
  virtual Vocoder *getVocoder() const = 0;
  virtual bool hasAnalyzedProject() const = 0;
  virtual void bindRealtimeProcessor(RealtimePitchProcessor &processor) = 0;
  virtual juce::String serializeProjectJson() const = 0;
  virtual bool restoreProjectJson(const juce::String &json) = 0;
  virtual void setStatusMessage(const juce::String &message) = 0;
  virtual void setOnProjectDataChanged(std::function<void()> callback) = 0;
  virtual void setOnPitchEditFinished(std::function<void()> callback) = 0;
  virtual void setOnRequestHostPlayState(
      std::function<void(bool)> callback) = 0;
  virtual void setOnRequestHostStop(std::function<void()> callback) = 0;
  virtual void setOnRequestHostSeek(
      std::function<void(double)> callback) = 0;
  virtual void setOnRequestHostLoopRange(
      std::function<void(double, double, bool, bool)> callback) = 0;

  virtual void setHostAudio(const juce::AudioBuffer<float> &buffer,
                            double sampleRate,
                            double timelineOffsetSeconds = 0.0) = 0;
  virtual void updateHostAudioTimelineOffset(double timelineOffsetSeconds) = 0;
  virtual void clearHostAudio() = 0;
  virtual void updatePlaybackPosition(double timeSeconds) = 0;
  virtual void updateHostPlaybackState(bool isPlaying) = 0;
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
