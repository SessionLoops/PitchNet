#pragma once

#include <cmath>

// Audio constants
constexpr int SAMPLE_RATE = 44100;
constexpr int HOP_SIZE = 512;
constexpr int WIN_SIZE = 2048;
constexpr int N_FFT = 2048;
constexpr int NUM_MELS = 128;
constexpr float FMIN = 40.0f;
constexpr float FMAX = 16000.0f;

// MIDI constants
constexpr int MIN_MIDI_NOTE = 24; // C1
constexpr int MAX_MIDI_NOTE = 89; // F6
constexpr int MIDI_A4 = 69;
constexpr float FREQ_A4 = 440.0f;

// UI constants
constexpr float DEFAULT_PIXELS_PER_SECOND = 100.0f;
constexpr float DEFAULT_PIXELS_PER_SEMITONE = 45.0f;
constexpr float MIN_PIXELS_PER_SECOND = 20.0f;
constexpr float MAX_PIXELS_PER_SECOND = 500.0f;
constexpr float MIN_PIXELS_PER_SEMITONE = 8.0f;
constexpr float MAX_PIXELS_PER_SEMITONE = 120.0f;
constexpr float DEFAULT_EMPTY_PROJECT_DURATION_SECONDS = 30.0f;

// UI colors moved to Source/Utils/UI/Theme.h

// Utility functions
inline float midiToFreq(float midi) {
  return FREQ_A4 * std::pow(2.0f, (midi - MIDI_A4) / 12.0f);
}

inline float freqToMidi(float freq) {
  if (freq <= 0.0f)
    return 0.0f;
  return 12.0f * std::log2(freq / FREQ_A4) + MIDI_A4;
}

inline int secondsToFrames(float seconds) {
  return static_cast<int>(seconds * SAMPLE_RATE / HOP_SIZE);
}

inline float framesToSeconds(int frames) {
  return static_cast<float>(frames) * HOP_SIZE / SAMPLE_RATE;
}

inline float framesToSeconds(float frames) {
  return frames * HOP_SIZE / SAMPLE_RATE;
}
