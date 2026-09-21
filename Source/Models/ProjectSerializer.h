#pragma once

#include "../JuceHeader.h"
#include "Project.h"

/**
 * Handles Project serialization to/from JSON format.
 *
 * Design principles:
 * - Decoupled from Project class (Project doesn't know about serialization details)
 * - Uses JUCE's built-in JSON support (no external dependencies)
 * - Stateless utility class
 */
class ProjectSerializer {
public:
    // 6: notes carry directF0Edit. Projects written at 5 or below predate it
    //    and are run through migrateLegacyDirectF0Edits() on load.
    static constexpr int FORMAT_VERSION = 6;

    enum class BinaryArchiveMode {
        // Standalone documents and conventional plug-in state must remain
        // usable without an external audio source.
        selfContained,

        // ARA region state can recover immutable source samples from the host,
        // cheaply rebuild source-derived features, and restore edited audio
        // from the separately persisted processed-region render. Keep edit
        // state, but omit all project-level waveform and mel buffers.
        hostBackedARA
    };

    /**
     * Save project to JSON file.
     */
    static bool saveToFile(const Project& project, const juce::File& file);

    /**
     * Load project from JSON file.
     */
    static bool loadFromFile(Project& project, const juce::File& file);

    /**
     * Convert project to JSON object.
     */
    static juce::var toJson(const Project& project,
                            bool includeAnalysisCache = false,
                            bool includePitchData = true);

    /**
     * Load project from JSON object.
     */
    static bool fromJson(Project& project, const juce::var& json);

    /**
     * Serialize project metadata as compact JSON plus heavy analysis/render
     * caches as raw binary buffers. Intended for DAW/plugin state archives.
     */
    static bool toBinaryArchive(const Project& project,
                                juce::MemoryBlock& destData,
                                BinaryArchiveMode mode =
                                    BinaryArchiveMode::selfContained);

    /**
     * Load a project from toBinaryArchive(). Falls back to false for unknown
     * input so callers can try legacy JSON.
     */
    static bool fromBinaryArchive(Project& project, const void* data,
                                  size_t sizeInBytes);

private:
    // Note serialization
    static juce::var noteToJson(const Note& note, bool includeAnalysisCache,
                                bool includePitchData);
    static bool noteFromJson(Note& note, const juce::var& json,
                             bool hadPitchCorrection);

    // Pitch data serialization
    static juce::var pitchDataToJson(const AudioData& audioData);
    static bool pitchDataFromJson(AudioData& audioData, const juce::var& json);

    /**
     * Recover directF0Edit on projects saved before the flag existed.
     *
     * A drawn curve is written straight into AudioData::f0 and clears the
     * note's deltaPitch, so a project written at FORMAT_VERSION <= 5 records
     * one as a note that is neutral by every stored field while the dense F0
     * over its frames disagrees with the pitch those fields describe. Nothing
     * else can produce that disagreement, so it is used as the signal.
     *
     * Deliberately not keyed on renderedEdit: that flag is historical rather
     * than comparative, so a note that was pitch-edited and then reset still
     * carries it while being genuinely neutral. Treating those as drawn edits
     * would pin their region to the resynthesis path and reintroduce the very
     * artifact this branch removes.
     *
     * Must run after pitchDataFromJson(), which supplies the dense F0.
     */
    static void migrateLegacyDirectF0Edits(Project& project,
                                           const AudioData& audioData);
    static juce::var audioBufferToJson(const juce::AudioBuffer<float>& buffer);
    static bool audioBufferFromJson(juce::AudioBuffer<float>& buffer, const juce::var& json);
    static juce::var melSpectrogramToJson(const std::vector<std::vector<float>>& mel);
    static bool melSpectrogramFromJson(std::vector<std::vector<float>>& mel, const juce::var& json);

    // Array helpers (compact string format)
    static juce::String floatArrayToString(const std::vector<float>& arr, int precision = 4);
    static std::vector<float> stringToFloatArray(const juce::String& str);
    static juce::String boolArrayToString(const std::vector<bool>& arr);
    static std::vector<bool> stringToBoolArray(const juce::String& str);

    ProjectSerializer() = delete;
};
