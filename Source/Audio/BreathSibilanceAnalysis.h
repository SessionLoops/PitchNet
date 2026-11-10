#pragma once

#include "BreathSibilanceDetector.h"
#include "NoteTiming.h"
#include "../Models/Project.h"
#include "../Utils/Constants.h"

#include <cmath>

// AudioData glue for the JUCE-free detector in BreathSibilanceDetector.h.
namespace breathsibilance
{
/**
 * Label every analysis frame of `source` (the audio the pitch detector saw)
 * into audioData.breathSibilance, one label per voicedMask frame.
 */
inline void analyse(AudioData &audioData,
                    const juce::AudioBuffer<float> &source)
{
  const int numFrames = static_cast<int>(audioData.voicedMask.size());
  if (numFrames <= 0 || source.getNumChannels() <= 0 ||
      source.getNumSamples() <= 0)
  {
    audioData.breathSibilance.clear();
    return;
  }

  audioData.breathSibilance = detect(
      source.getReadPointer(0), source.getNumSamples(),
      audioData.sampleRate > 0 ? audioData.sampleRate : SAMPLE_RATE,
      HOP_SIZE, numFrames, audioData.voicedMask);
}

/**
 * Labels are derived data and are not stored in project files or plug-in
 * state. Rebuild them from the pristine source when they are missing or no
 * longer match the frame grid. Returns true when it relabelled.
 */
inline bool ensure(AudioData &audioData)
{
  if (audioData.voicedMask.empty() ||
      audioData.breathSibilance.size() == audioData.voicedMask.size())
    return false;
  if (audioData.originalWaveform.getNumSamples() <= 0)
    return false;

  analyse(audioData, audioData.originalWaveform);
  return true;
}

/**
 * Label of the audio a note plays at timeline frame `frame`, given the
 * note's time map (notetiming::forNote). Labels are indexed by source frame;
 * a moved or stretched note reads its frames through the map - the same map
 * the synthesizer renders it with and the piano roll hatches it with, so
 * curve gaps, hatching and sound line up.
 */
inline std::uint8_t labelAtNoteFrame(const AudioData &audioData,
                                     const notetiming::TimeMap &map,
                                     int frame)
{
  return labelAt(audioData.breathSibilance,
                 static_cast<int>(std::floor(
                     map.sourceAt(static_cast<double>(frame) + 0.5))));
}

/** The same for one frame of `note`, building its map. */
inline std::uint8_t labelAtNoteFrame(const AudioData &audioData,
                                     const Note &note, int frame)
{
  if (note.getSrcDurationFrames() <= 0 || note.getDurationFrames() <= 0)
    return labelAt(audioData.breathSibilance, frame);
  return labelAtNoteFrame(audioData, notetiming::forNote(audioData, note),
                          frame);
}
} // namespace breathsibilance
