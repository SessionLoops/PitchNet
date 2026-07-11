#pragma once

#include "../../JuceHeader.h"
#include "../../Utils/Constants.h"
#include "../../Utils/UI/Theme.h"
#include "../../Utils/Localization.h"
#include <functional>
#include <optional>

// =============================================================================
// Audio export utilities — extracted from the anonymous namespace in
// MainComponent.cpp to reduce file size and improve discoverability.
// =============================================================================

namespace ExportHelper {

enum class ExportFormat { wav, flac, aiff, ogg };

struct ExportSettings {
  ExportFormat format = ExportFormat::wav;
  int sampleRate = SAMPLE_RATE;
  int channels = 1;
  int bitsPerSample = 16;
};

juce::String getFormatDisplayName(ExportFormat format);
juce::String getFormatExtension(ExportFormat format);
juce::String getFormatWildcard(ExportFormat format);

juce::AudioBuffer<float> convertChannels(const juce::AudioBuffer<float> &input,
                                          int outChannels);
juce::AudioBuffer<float> resampleAudio(const juce::AudioBuffer<float> &input,
                                        int sourceRate, int targetRate);

juce::AudioFormat *findFormatForExtension(juce::AudioFormatManager &manager,
                                           const juce::String &extension);

void showExportSettingsDialogAsync(
    juce::Component *parent, int inputSampleRate,
    std::function<void(std::optional<ExportSettings>)> onDone);

} // namespace ExportHelper
