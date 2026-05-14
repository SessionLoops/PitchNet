#include "ProjectSerializer.h"
#include "../Utils/PitchCurveProcessor.h"

bool ProjectSerializer::saveToFile(const Project& project, const juce::File& file) {
    auto json = toJson(project);
    auto jsonString = juce::JSON::toString(json, true); // Pretty print

    return file.replaceWithText(jsonString);
}

bool ProjectSerializer::loadFromFile(Project& project, const juce::File& file) {
    auto jsonString = file.loadFileAsString();
    if (jsonString.isEmpty())
        return false;

    auto json = juce::JSON::parse(jsonString);
    if (!json.isObject())
        return false;

    return fromJson(project, json);
}

juce::var ProjectSerializer::toJson(const Project& project) {
    auto* obj = new juce::DynamicObject();

    // Metadata
    obj->setProperty("formatVersion", FORMAT_VERSION);
    obj->setProperty("name", project.getName());
    obj->setProperty("audioPath", project.getFilePath().getFullPathName());
    obj->setProperty("audioSha256", project.getAudioSha256());

    // Audio settings
    obj->setProperty("sampleRate", project.getAudioData().sampleRate);

    // Global parameters
    obj->setProperty("globalPitchOffset", project.getGlobalPitchOffset());
    obj->setProperty("formantShift", project.getFormantShift());
    obj->setProperty("volume", project.getVolume());
    obj->setProperty("scaleMode", static_cast<int>(project.getScaleMode()));
    obj->setProperty("scaleRootNote", project.getScaleRootNote());
    obj->setProperty("pitchReferenceHz", project.getPitchReferenceHz());
    obj->setProperty("showScaleColors", project.getShowScaleColors());
    obj->setProperty("snapToSemitones", project.getSnapToSemitones());
    obj->setProperty("doubleClickSnapMode",
                     static_cast<int>(project.getDoubleClickSnapMode()));
    obj->setProperty("timelineDisplayMode",
                     static_cast<int>(project.getTimelineDisplayMode()));
    obj->setProperty("timelineBeatNumerator", project.getTimelineBeatNumerator());
    obj->setProperty("timelineBeatDenominator", project.getTimelineBeatDenominator());
    obj->setProperty("timelineTempoBpm", project.getTimelineTempoBpm());
    obj->setProperty("timelineGridDivision",
                     static_cast<int>(project.getTimelineGridDivision()));
    obj->setProperty("timelineSnapCycle", project.getTimelineSnapCycle());

    // Loop range
    const auto& loopRange = project.getLoopRange();
    auto* loopObj = new juce::DynamicObject();
    loopObj->setProperty("enabled", loopRange.enabled);
    loopObj->setProperty("start", loopRange.startSeconds);
    loopObj->setProperty("end", loopRange.endSeconds);
    obj->setProperty("loop", juce::var(loopObj));

    // Notes array
    juce::Array<juce::var> notesArray;
    for (const auto& note : project.getNotes()) {
        notesArray.add(noteToJson(note));
    }
    obj->setProperty("notes", notesArray);

    // Pitch data
    obj->setProperty("pitchData", pitchDataToJson(project.getAudioData()));

    return juce::var(obj);
}

bool ProjectSerializer::fromJson(Project& project, const juce::var& json) {
    if (!json.isObject())
        return false;

    // Metadata
    project.setName(json.getProperty("name", "Untitled").toString());
    project.setFilePath(juce::File(json.getProperty("audioPath", "").toString()));
    project.setAudioSha256(json.getProperty("audioSha256", "").toString());

    // Audio settings
    auto& audioData = project.getAudioData();
    audioData.sampleRate = json.getProperty("sampleRate", 44100);

    // Global parameters
    project.setGlobalPitchOffset(static_cast<float>(json.getProperty("globalPitchOffset", 0.0)));
    project.setFormantShift(static_cast<float>(json.getProperty("formantShift", 0.0)));
    project.setVolume(static_cast<float>(json.getProperty("volume", 0.0)));
    {
        const int scaleModeValue = static_cast<int>(json.getProperty(
            "scaleMode", static_cast<int>(ScaleMode::None)));
        if (scaleModeValue >= static_cast<int>(ScaleMode::None) &&
            scaleModeValue <= static_cast<int>(ScaleMode::Locrian))
            project.setScaleMode(static_cast<ScaleMode>(scaleModeValue));
        else
            project.setScaleMode(ScaleMode::None);
    }
    project.setScaleRootNote(static_cast<int>(json.getProperty("scaleRootNote", -1)));
    project.setPitchReferenceHz(static_cast<int>(json.getProperty("pitchReferenceHz", 440)));
    project.setShowScaleColors(static_cast<bool>(
        json.getProperty("showScaleColors", true)));
    project.setSnapToSemitones(static_cast<bool>(
        json.getProperty("snapToSemitones", false)));
    {
        const int snapModeValue = static_cast<int>(json.getProperty(
            "doubleClickSnapMode", static_cast<int>(DoubleClickSnapMode::PitchCenter)));
        if (snapModeValue >= static_cast<int>(DoubleClickSnapMode::PitchCenter) &&
            snapModeValue <= static_cast<int>(DoubleClickSnapMode::NearestScale))
            project.setDoubleClickSnapMode(static_cast<DoubleClickSnapMode>(snapModeValue));
        else
            project.setDoubleClickSnapMode(DoubleClickSnapMode::PitchCenter);
    }
    {
        const int modeValue = static_cast<int>(json.getProperty(
            "timelineDisplayMode", static_cast<int>(TimelineDisplayMode::Beats)));
        if (modeValue >= static_cast<int>(TimelineDisplayMode::Beats) &&
            modeValue <= static_cast<int>(TimelineDisplayMode::Time))
            project.setTimelineDisplayMode(static_cast<TimelineDisplayMode>(modeValue));
        else
            project.setTimelineDisplayMode(TimelineDisplayMode::Beats);
    }
    project.setTimelineBeatSignature(
        static_cast<int>(json.getProperty("timelineBeatNumerator", 4)),
        static_cast<int>(json.getProperty("timelineBeatDenominator", 4)));
    project.setTimelineTempoBpm(
        static_cast<double>(json.getProperty("timelineTempoBpm", 120.0)));
    {
        const int gridValue = static_cast<int>(json.getProperty(
            "timelineGridDivision", static_cast<int>(TimelineGridDivision::Quarter)));
        switch (gridValue)
        {
            case static_cast<int>(TimelineGridDivision::Whole):
            case static_cast<int>(TimelineGridDivision::Half):
            case static_cast<int>(TimelineGridDivision::Quarter):
            case static_cast<int>(TimelineGridDivision::Eighth):
            case static_cast<int>(TimelineGridDivision::Sixteenth):
            case static_cast<int>(TimelineGridDivision::ThirtySecond):
                project.setTimelineGridDivision(static_cast<TimelineGridDivision>(gridValue));
                break;
            default:
                project.setTimelineGridDivision(TimelineGridDivision::Quarter);
                break;
        }
    }
    project.setTimelineSnapCycle(static_cast<bool>(
        json.getProperty("timelineSnapCycle", false)));

    // Loop range
    auto loopVar = json.getProperty("loop", juce::var());
    if (loopVar.isObject()) {
        const double loopStart = loopVar.getProperty("start", 0.0);
        const double loopEnd = loopVar.getProperty("end", 0.0);
        project.setLoopRange(loopStart, loopEnd);
        project.setLoopEnabled(loopVar.getProperty("enabled", false));
    }

    // Notes
    project.clearNotes();
    auto notesVar = json.getProperty("notes", juce::var());
    if (notesVar.isArray()) {
        for (int i = 0; i < notesVar.size(); ++i) {
            Note note;
            if (noteFromJson(note, notesVar[i])) {
                project.addNote(std::move(note));
            }
        }
    }

    // Pitch data
    auto pitchDataVar = json.getProperty("pitchData", juce::var());
    if (pitchDataVar.isObject()) {
        pitchDataFromJson(audioData, pitchDataVar);
    }

    // Rebuild curves if needed
    if (!audioData.f0.empty() && (audioData.basePitch.empty() || audioData.deltaPitch.empty())) {
        PitchCurveProcessor::rebuildCurvesFromSource(project, audioData.f0);
    }

    // Ensure every note has originalDeltaPitch populated.
    // The serializer may not have persisted it (older format), or
    // rebuildCurvesFromSource was skipped because basePitch/deltaPitch
    // were already loaded from the file.  Extract from global deltaPitch
    // so that rebuildBaseFromNotes() (called during stretch/undo) can
    // resample correctly instead of producing zero delta.
    {
        const int totalFrames = static_cast<int>(audioData.deltaPitch.size());
        for (auto& note : project.getNotes())
        {
            if (note.isRest() || note.hasOriginalDeltaPitch())
                continue;

            const int startFrame = note.getStartFrame();
            const int endFrame = note.getEndFrame();
            const int numFrames = endFrame - startFrame;
            if (numFrames <= 0)
                continue;

            std::vector<float> origDelta(static_cast<size_t>(numFrames));
            for (int i = 0; i < numFrames; ++i)
            {
                const int globalIdx = startFrame + i;
                if (globalIdx >= 0 && globalIdx < totalFrames)
                    origDelta[static_cast<size_t>(i)] = audioData.deltaPitch[static_cast<size_t>(globalIdx)];
            }
            note.setOriginalDeltaPitch(std::move(origDelta));
        }
    }

    project.setModified(false);
    return true;
}

juce::var ProjectSerializer::noteToJson(const Note& note) {
    auto* obj = new juce::DynamicObject();

    obj->setProperty("startFrame", note.getStartFrame());
    obj->setProperty("endFrame", note.getEndFrame());
    obj->setProperty("srcStartFrame", note.getSrcStartFrame());
    obj->setProperty("srcEndFrame", note.getSrcEndFrame());
    obj->setProperty("midiNote", note.getMidiNote());
    obj->setProperty("pitchOffset", note.getPitchOffset());
    obj->setProperty("volumeDb", note.getVolumeDb());
    obj->setProperty("rest", note.isRest());

    // Vibrato
    auto* vibrato = new juce::DynamicObject();
    vibrato->setProperty("enabled", note.isVibratoEnabled());
    vibrato->setProperty("rateHz", note.getVibratoRateHz());
    vibrato->setProperty("depthSemitones", note.getVibratoDepthSemitones());
    vibrato->setProperty("phaseRadians", note.getVibratoPhaseRadians());
    obj->setProperty("vibrato", juce::var(vibrato));

    // Lyric/Phoneme
    if (note.hasLyric())
        obj->setProperty("lyric", note.getLyric());
    if (note.hasPhoneme())
        obj->setProperty("phoneme", note.getPhoneme());

    // Pitch tool transformation parameters (non-destructive)
    obj->setProperty("tiltLeft", note.getTiltLeft());
    obj->setProperty("tiltRight", note.getTiltRight());
    obj->setProperty("varianceScale", note.getVarianceScale());
    obj->setProperty("smoothLeftFrames", note.getSmoothLeftFrames());
    obj->setProperty("smoothRightFrames", note.getSmoothRightFrames());

    // Per-note original delta pitch (pristine curve from analysis)
    if (note.hasOriginalDeltaPitch())
        obj->setProperty("originalDeltaPitch", floatArrayToString(note.getOriginalDeltaPitch(), 4));

    // Per-note delta scale/offset
    if (std::abs(note.getDeltaScale() - 1.0f) > 0.0001f)
        obj->setProperty("deltaScale", note.getDeltaScale());
    if (std::abs(note.getDeltaOffset()) > 0.0001f)
        obj->setProperty("deltaOffset", note.getDeltaOffset());

    return juce::var(obj);
}

bool ProjectSerializer::noteFromJson(Note& note, const juce::var& json) {
    if (!json.isObject())
        return false;

    const int startFrame = json.getProperty("startFrame", 0);
    const int endFrame = json.getProperty("endFrame", 0);
    note.setStartFrame(startFrame);
    note.setEndFrame(endFrame);
    // Backward compat: if srcStartFrame/srcEndFrame not in file, default to startFrame/endFrame
    note.setSrcStartFrame(static_cast<int>(json.getProperty("srcStartFrame", startFrame)));
    note.setSrcEndFrame(static_cast<int>(json.getProperty("srcEndFrame", endFrame)));
    note.setMidiNote(static_cast<float>(json.getProperty("midiNote", 60.0)));
    note.setPitchOffset(static_cast<float>(json.getProperty("pitchOffset", 0.0)));
    note.setVolumeDb(static_cast<float>(json.getProperty("volumeDb", 0.0)));
    note.setRest(json.getProperty("rest", false));

    // Vibrato
    auto vibratoVar = json.getProperty("vibrato", juce::var());
    if (vibratoVar.isObject()) {
        note.setVibratoEnabled(vibratoVar.getProperty("enabled", false));
        note.setVibratoRateHz(static_cast<float>(vibratoVar.getProperty("rateHz", 5.0)));
        note.setVibratoDepthSemitones(static_cast<float>(vibratoVar.getProperty("depthSemitones", 0.0)));
        note.setVibratoPhaseRadians(static_cast<float>(vibratoVar.getProperty("phaseRadians", 0.0)));
    }

    // Lyric/Phoneme
    auto lyric = json.getProperty("lyric", juce::var());
    if (!lyric.isVoid())
        note.setLyric(lyric.toString());

    auto phoneme = json.getProperty("phoneme", juce::var());
    if (!phoneme.isVoid())
        note.setPhoneme(phoneme.toString());

    // Pitch tool transformation parameters (with defaults for backwards compatibility)
    note.setTiltLeft(static_cast<float>(json.getProperty("tiltLeft", 0.0)));
    note.setTiltRight(static_cast<float>(json.getProperty("tiltRight", 0.0)));
    note.setVarianceScale(static_cast<float>(json.getProperty("varianceScale", 1.0)));
    note.setSmoothLeftFrames(json.getProperty("smoothLeftFrames", 0));
    note.setSmoothRightFrames(json.getProperty("smoothRightFrames", 0));

    // Per-note original delta pitch (pristine curve from analysis)
    auto origDeltaStr = json.getProperty("originalDeltaPitch", juce::var());
    if (!origDeltaStr.isVoid() && origDeltaStr.toString().isNotEmpty())
        note.setOriginalDeltaPitch(stringToFloatArray(origDeltaStr.toString()));

    // Per-note delta scale/offset
    note.setDeltaScale(static_cast<float>(json.getProperty("deltaScale", 1.0)));
    note.setDeltaOffset(static_cast<float>(json.getProperty("deltaOffset", 0.0)));

    return true;
}

juce::var ProjectSerializer::pitchDataToJson(const AudioData& audioData) {
    auto* obj = new juce::DynamicObject();

    // Store as compact strings for efficiency
    obj->setProperty("f0", floatArrayToString(audioData.f0, 2));
    obj->setProperty("basePitch", floatArrayToString(audioData.basePitch, 4));
    obj->setProperty("deltaPitch", floatArrayToString(audioData.deltaPitch, 4));
    obj->setProperty("voicedMask", boolArrayToString(audioData.voicedMask));
    obj->setProperty("vadMask", boolArrayToString(audioData.vadMask));

    return juce::var(obj);
}

bool ProjectSerializer::pitchDataFromJson(AudioData& audioData, const juce::var& json) {
    if (!json.isObject())
        return false;

    audioData.f0 = stringToFloatArray(json.getProperty("f0", "").toString());
    audioData.baseF0 = audioData.f0; // Initialize baseF0 from loaded f0
    audioData.basePitch = stringToFloatArray(json.getProperty("basePitch", "").toString());
    audioData.deltaPitch = stringToFloatArray(json.getProperty("deltaPitch", "").toString());
    audioData.voicedMask = stringToBoolArray(json.getProperty("voicedMask", "").toString());
    audioData.vadMask = stringToBoolArray(json.getProperty("vadMask", "").toString());

    return true;
}

juce::String ProjectSerializer::floatArrayToString(const std::vector<float>& arr, int precision) {
    if (arr.empty())
        return {};

    juce::StringArray parts;
    parts.ensureStorageAllocated(static_cast<int>(arr.size()));

    for (float v : arr) {
        parts.add(juce::String(v, precision));
    }

    return parts.joinIntoString(" ");
}

std::vector<float> ProjectSerializer::stringToFloatArray(const juce::String& str) {
    if (str.isEmpty())
        return {};

    juce::StringArray parts;
    parts.addTokens(str, " ", "");

    std::vector<float> result;
    result.reserve(static_cast<size_t>(parts.size()));

    for (const auto& p : parts) {
        if (p.isNotEmpty())
            result.push_back(p.getFloatValue());
    }

    return result;
}

juce::String ProjectSerializer::boolArrayToString(const std::vector<bool>& arr) {
    if (arr.empty())
        return {};

    juce::String result;
    result.preallocateBytes(arr.size());

    for (bool b : arr) {
        result << (b ? '1' : '0');
    }

    return result;
}

std::vector<bool> ProjectSerializer::stringToBoolArray(const juce::String& str) {
    if (str.isEmpty())
        return {};

    std::vector<bool> result;
    result.reserve(static_cast<size_t>(str.length()));

    for (int i = 0; i < str.length(); ++i) {
        result.push_back(str[i] == '1');
    }

    return result;
}
