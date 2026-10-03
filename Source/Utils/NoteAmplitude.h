#pragma once

#include "../Models/Project.h"
#include "Constants.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace NoteAmplitude
{
inline void update(Note& note, const AudioData& audio)
{
    const auto& waveform = audio.originalWaveform.getNumSamples() > 0
                               ? audio.originalWaveform : audio.waveform;
    const auto sampleAtFrame = [&](int frame)
    {
        return static_cast<int>(std::clamp<int64_t>(
            static_cast<int64_t>(frame) * HOP_SIZE * audio.sampleRate / SAMPLE_RATE,
            0, waveform.getNumSamples()));
    };
    const int start = sampleAtFrame(note.getSrcStartFrame());
    const int end = sampleAtFrame(note.getSrcEndFrame());
    double energy = 0.0;
    for (int channel = 0; channel < waveform.getNumChannels(); ++channel)
        for (int sample = start; sample < end; ++sample)
        {
            const double value = waveform.getSample(channel, sample);
            energy += value * value;
        }
    const double count = static_cast<double>(std::max(0, end - start)) * waveform.getNumChannels();
    note.setSourceAmplitude(count > 0.0 ? static_cast<float>(std::sqrt(energy / count)) : 0.0f);
}
}
