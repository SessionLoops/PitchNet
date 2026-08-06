#include "ProjectSerializer.h"
#include "../Utils/PitchCurveProcessor.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace
{
constexpr std::uint32_t kProjectArchiveMagic = 0x504E4152u; // PNAR
constexpr int kProjectArchiveVersion = 9;
constexpr int kProjectArchiveHasOriginalWaveform = 1 << 0;
constexpr int kProjectArchiveHasMelSpectrogram = 1 << 1;
constexpr int kProjectArchiveHasRenderedWaveform = 1 << 2;

bool writeString(juce::OutputStream& out, const juce::String& text)
{
    const auto bytes = text.getNumBytesAsUTF8();
    return out.writeInt64(static_cast<juce::int64>(bytes)) &&
           out.write(text.toRawUTF8(), bytes);
}

juce::String readString(juce::InputStream& in)
{
    const auto bytes = in.readInt64();
    if (bytes < 0 || bytes > std::numeric_limits<int>::max())
        return {};

    juce::MemoryBlock data(static_cast<size_t>(bytes));
    if (bytes > 0 && in.read(data.getData(), static_cast<int>(bytes)) != bytes)
        return {};

    return juce::String(
        juce::CharPointer_UTF8(static_cast<const char*>(data.getData())),
        static_cast<size_t>(bytes));
}

bool writeFloatVector(juce::OutputStream& out, const std::vector<float>& values)
{
    if (!out.writeInt64(static_cast<juce::int64>(values.size())))
        return false;
    if (values.empty())
        return true;
    return out.write(values.data(), values.size() * sizeof(float));
}

bool readFloatVector(juce::InputStream& in, std::vector<float>& values)
{
    const auto count = in.readInt64();
    if (count < 0 || count > std::numeric_limits<int>::max())
        return false;

    values.resize(static_cast<size_t>(count));
    if (values.empty())
        return true;

    const auto bytes = static_cast<int>(values.size() * sizeof(float));
    return in.read(values.data(), bytes) == bytes;
}

bool writeBoolVector(juce::OutputStream& out, const std::vector<bool>& values)
{
    if (!out.writeInt64(static_cast<juce::int64>(values.size())))
        return false;

    for (bool value : values)
        if (!out.writeByte(value ? 1 : 0))
            return false;
    return true;
}

bool readBoolVector(juce::InputStream& in, std::vector<bool>& values)
{
    const auto count = in.readInt64();
    if (count < 0 || count > std::numeric_limits<int>::max())
        return false;

    values.assign(static_cast<size_t>(count), false);
    for (size_t i = 0; i < values.size(); ++i)
        values[i] = in.readByte() != 0;
    return true;
}

bool writeAudioBuffer(juce::OutputStream& out,
                      const juce::AudioBuffer<float>& buffer)
{
    if (!out.writeInt(buffer.getNumChannels()) ||
        !out.writeInt(buffer.getNumSamples()))
        return false;

    const auto bytes = static_cast<size_t>(buffer.getNumSamples()) *
                       sizeof(float);
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        if (bytes > 0 && !out.write(buffer.getReadPointer(ch), bytes))
            return false;
    return true;
}

bool readAudioBuffer(juce::InputStream& in, juce::AudioBuffer<float>& buffer)
{
    const int channels = in.readInt();
    const int samples = in.readInt();
    if (channels < 0 || samples < 0)
        return false;

    buffer.setSize(channels, samples, false, false, true);
    buffer.clear();
    const auto bytes = static_cast<size_t>(samples) * sizeof(float);
    for (int ch = 0; ch < channels; ++ch)
        if (bytes > 0 &&
            in.read(buffer.getWritePointer(ch), static_cast<int>(bytes)) !=
                static_cast<int>(bytes))
            return false;
    return true;
}

bool writeMel(juce::OutputStream& out,
              const std::vector<std::vector<float>>& mel)
{
    if (!out.writeInt64(static_cast<juce::int64>(mel.size())))
        return false;
    for (const auto& frame : mel)
        if (!writeFloatVector(out, frame))
            return false;
    return true;
}

bool readMel(juce::InputStream& in, std::vector<std::vector<float>>& mel)
{
    const auto frames = in.readInt64();
    if (frames < 0 || frames > std::numeric_limits<int>::max())
        return false;

    mel.resize(static_cast<size_t>(frames));
    for (auto& frame : mel)
        if (!readFloatVector(in, frame))
            return false;
    return true;
}
} // namespace

bool ProjectSerializer::saveToFile(const Project& project, const juce::File& file) {
    // A .pitchnet document is self-contained: retain its rendered waveform and
    // analysis caches so it can reopen without the original source audio.
    auto json = toJson(project, true, true);
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

juce::var ProjectSerializer::toJson(const Project& project,
                                    bool includeAnalysisCache,
                                    bool includePitchData) {
    auto* obj = new juce::DynamicObject();

    // Metadata
    obj->setProperty("formatVersion", FORMAT_VERSION);
    obj->setProperty("name", project.getName());
    obj->setProperty("audioPath", project.getFilePath().getFullPathName());
    obj->setProperty("audioSha256", project.getAudioSha256());

    // Audio settings
    obj->setProperty("sampleRate", project.getAudioData().sampleRate);
    obj->setProperty("timelineOffsetSeconds",
                     project.getAudioData().timelineOffsetSeconds);
    juce::Array<juce::var> regionRanges;
    for (const auto &[start, end] :
         project.getAudioData().playbackRegionRanges) {
        juce::Array<juce::var> range;
        range.add(start);
        range.add(end);
        regionRanges.add(range);
    }
    obj->setProperty("playbackRegionRanges", regionRanges);

    // Global parameters
    obj->setProperty("globalPitchOffset", project.getGlobalPitchOffset());
    obj->setProperty("formantShift", project.getFormantShift());
    obj->setProperty("volume", project.getVolume());
    obj->setProperty("pitchCenter", project.getPitchCenter());
    obj->setProperty("scaleMode", static_cast<int>(project.getScaleMode()));
    obj->setProperty("preferredScaleMode",
                     static_cast<int>(project.getPreferredScaleMode()));
    obj->setProperty("scaleRootNote", project.getScaleRootNote());
    obj->setProperty("pitchReferenceHz", project.getPitchReferenceHz());
    obj->setProperty("snapToSemitones", project.getSnapToSemitones());
    obj->setProperty("dragSnapMode",
                     static_cast<int>(project.getDragSnapMode()));
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
        notesArray.add(noteToJson(note, includeAnalysisCache,
                                  includePitchData));
    }
    obj->setProperty("notes", notesArray);

    if (includePitchData)
        obj->setProperty("pitchData", pitchDataToJson(project.getAudioData()));
    if (includeAnalysisCache) {
        obj->setProperty("waveform", audioBufferToJson(project.getAudioData().waveform));
        obj->setProperty("originalWaveform",
                         audioBufferToJson(project.getAudioData().originalWaveform));
        obj->setProperty("melSpectrogram",
                         melSpectrogramToJson(project.getAudioData().melSpectrogram));
    }

    juce::Array<juce::var> segmentRanges;
    for (const auto& range : project.getAudioData().segmentChunkRanges) {
        juce::Array<juce::var> rangeArray;
        rangeArray.add(range.first);
        rangeArray.add(range.second);
        segmentRanges.add(rangeArray);
    }
    obj->setProperty("segmentChunkRanges", segmentRanges);

    if (includeAnalysisCache) {
        juce::Array<juce::var> debugChunks;
        for (const auto& chunk : project.getAudioData().segmentDebugChunks) {
            auto* chunkObj = new juce::DynamicObject();
            chunkObj->setProperty("chunkIndex", chunk.chunkIndex);
            chunkObj->setProperty("startFrame", chunk.startFrame);
            chunkObj->setProperty("endFrame", chunk.endFrame);
            chunkObj->setProperty("shortRestThreshold", chunk.shortRestThreshold);
            juce::Array<juce::var> events;
            for (const auto& event : chunk.events) {
                auto* eventObj = new juce::DynamicObject();
                eventObj->setProperty("startFrame", event.startFrame);
                eventObj->setProperty("endFrame", event.endFrame);
                eventObj->setProperty("attachedStartFrame", event.attachedStartFrame);
                eventObj->setProperty("midiNote", event.midiNote);
                eventObj->setProperty("isRest", event.isRest);
                eventObj->setProperty("durationSeconds", event.durationSeconds);
                eventObj->setProperty("durationFrames", event.durationFrames);
                events.add(juce::var(eventObj));
            }
            chunkObj->setProperty("events", events);
            debugChunks.add(juce::var(chunkObj));
        }
        obj->setProperty("segmentDebugChunks", debugChunks);
    }

    return juce::var(obj);
}

bool ProjectSerializer::fromJson(Project& project, const juce::var& json) {
    if (!json.isObject())
        return false;

    const int formatVersion =
        static_cast<int>(json.getProperty("formatVersion", 1));
    if (formatVersion < 1 || formatVersion > FORMAT_VERSION)
        return false;

    // Metadata
    project.setName(json.getProperty("name", "Untitled").toString());
    project.setFilePath(juce::File(json.getProperty("audioPath", "").toString()));
    project.setAudioSha256(json.getProperty("audioSha256", "").toString());

    // Audio settings
    auto& audioData = project.getAudioData();
    audioData.sampleRate = json.getProperty("sampleRate", 44100);
    audioData.timelineOffsetSeconds =
        static_cast<double>(json.getProperty("timelineOffsetSeconds", 0.0));
    audioData.playbackRegionRanges.clear();
    if (auto *ranges = json.getProperty("playbackRegionRanges", juce::var())
                           .getArray()) {
        for (const auto &rangeVar : *ranges) {
            if (auto *range = rangeVar.getArray(); range && range->size() >= 2)
                audioData.playbackRegionRanges.emplace_back(
                    static_cast<double>((*range)[0]),
                    static_cast<double>((*range)[1]));
        }
    }

    // Global parameters
    project.setGlobalPitchOffset(static_cast<float>(json.getProperty("globalPitchOffset", 0.0)));
    project.setFormantShift(static_cast<float>(json.getProperty("formantShift", 0.0)));
    project.setVolume(static_cast<float>(json.getProperty("volume", 0.0)));
    project.setPitchCenter(static_cast<float>(json.getProperty("pitchCenter", 0.0)));
    {
        const int scaleModeValue = static_cast<int>(json.getProperty(
            "scaleMode", static_cast<int>(ScaleMode::Chromatic)));
        if (scaleModeValue >= static_cast<int>(ScaleMode::Chromatic) &&
            scaleModeValue <= static_cast<int>(ScaleMode::WholeTone))
            project.setScaleMode(static_cast<ScaleMode>(scaleModeValue));
        else
            project.setScaleMode(ScaleMode::Chromatic);
    }
    {
        const ScaleMode activeMode = project.getScaleMode();
        const ScaleMode preferredDefault =
            activeMode != ScaleMode::Chromatic ? activeMode : ScaleMode::Major;
        const int preferredModeValue = static_cast<int>(json.getProperty(
            "preferredScaleMode", static_cast<int>(preferredDefault)));
        if (preferredModeValue >= static_cast<int>(ScaleMode::Major) &&
            preferredModeValue <= static_cast<int>(ScaleMode::WholeTone))
            project.setPreferredScaleMode(
                static_cast<ScaleMode>(preferredModeValue));
    }
    project.setScaleRootNote(static_cast<int>(json.getProperty("scaleRootNote", 0)));
    project.setPitchReferenceHz(static_cast<int>(json.getProperty("pitchReferenceHz", 440)));
    project.setSnapToSemitones(static_cast<bool>(
        json.getProperty("snapToSemitones", false)));
    {
        const int dragSnapModeValue = static_cast<int>(json.getProperty(
            "dragSnapMode", static_cast<int>(DragSnapMode::Chromatic)));
        if (dragSnapModeValue >= static_cast<int>(DragSnapMode::Chromatic) &&
            dragSnapModeValue <= static_cast<int>(DragSnapMode::Scale))
            project.setDragSnapMode(
                static_cast<DragSnapMode>(dragSnapModeValue));
        else
            project.setDragSnapMode(DragSnapMode::Chromatic);
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
    audioBufferFromJson(audioData.waveform,
                        json.getProperty("waveform", juce::var()));
    audioBufferFromJson(audioData.originalWaveform,
                        json.getProperty("originalWaveform", juce::var()));
    melSpectrogramFromJson(audioData.melSpectrogram,
                           json.getProperty("melSpectrogram", juce::var()));
    audioData.segmentChunkRanges.clear();
    if (auto *ranges = json.getProperty("segmentChunkRanges", juce::var())
                           .getArray()) {
        for (const auto& rangeVar : *ranges) {
            if (auto *range = rangeVar.getArray(); range && range->size() >= 2)
                audioData.segmentChunkRanges.emplace_back(
                    static_cast<int>((*range)[0]),
                    static_cast<int>((*range)[1]));
        }
    }
    audioData.segmentDebugChunks.clear();
    if (auto *chunks = json.getProperty("segmentDebugChunks", juce::var())
                           .getArray()) {
        for (const auto& chunkVar : *chunks) {
            if (!chunkVar.isObject())
                continue;
            AudioData::SegmentDebugChunk chunk;
            chunk.chunkIndex = static_cast<int>(chunkVar.getProperty("chunkIndex", 0));
            chunk.startFrame = static_cast<int>(chunkVar.getProperty("startFrame", 0));
            chunk.endFrame = static_cast<int>(chunkVar.getProperty("endFrame", 0));
            chunk.shortRestThreshold =
                static_cast<int>(chunkVar.getProperty("shortRestThreshold", 0));
            if (auto *events = chunkVar.getProperty("events", juce::var())
                                   .getArray()) {
                for (const auto& eventVar : *events) {
                    if (!eventVar.isObject())
                        continue;
                    AudioData::SegmentDebugEvent event;
                    event.startFrame = static_cast<int>(eventVar.getProperty("startFrame", 0));
                    event.endFrame = static_cast<int>(eventVar.getProperty("endFrame", 0));
                    event.attachedStartFrame =
                        static_cast<int>(eventVar.getProperty("attachedStartFrame", 0));
                    event.midiNote =
                        static_cast<float>(eventVar.getProperty("midiNote", 0.0));
                    event.isRest =
                        static_cast<bool>(eventVar.getProperty("isRest", false));
                    event.durationSeconds =
                        static_cast<float>(eventVar.getProperty("durationSeconds", 0.0));
                    event.durationFrames =
                        static_cast<int>(eventVar.getProperty("durationFrames", 0));
                    chunk.events.push_back(event);
                }
            }
            audioData.segmentDebugChunks.push_back(std::move(chunk));
        }
    }

    // Rebuild curves if needed
    if (!audioData.f0.empty() && (audioData.basePitch.empty() || audioData.deltaPitch.empty())) {
        PitchCurveProcessor::rebuildCurvesFromSource(project, audioData.f0);
    }

    // Ensure every note has originalDeltaPitch populated.
    // The serializer may not have persisted it (older format), or
    // rebuildCurvesFromSource was skipped because basePitch/deltaPitch
    // were already loaded from the file.  Extract from global deltaPitch
    // so that rebuildBaseFromNotes() can resample correctly instead of
    // producing zero delta.
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

bool ProjectSerializer::toBinaryArchive(const Project& project,
                                        juce::MemoryBlock& destData,
                                        BinaryArchiveMode mode)
{
    destData.setSize(0);
    juce::MemoryOutputStream out(destData, false);

    const auto metadataJson =
        juce::JSON::toString(toJson(project, false, false), false);
    const auto& audioData = project.getAudioData();
    const bool includeSourceDerivedData =
        mode == BinaryArchiveMode::selfContained;
    const int archiveFlags =
        includeSourceDerivedData
            ? kProjectArchiveHasOriginalWaveform |
                  kProjectArchiveHasMelSpectrogram |
                  kProjectArchiveHasRenderedWaveform
            : 0;

    if (!out.writeInt(static_cast<int>(kProjectArchiveMagic)) ||
        !out.writeInt(kProjectArchiveVersion) ||
        !out.writeInt(archiveFlags) ||
        !writeString(out, metadataJson) ||
        (includeSourceDerivedData &&
         !writeAudioBuffer(out, audioData.waveform)) ||
        (includeSourceDerivedData &&
         !writeAudioBuffer(out, audioData.originalWaveform)) ||
        !writeFloatVector(out, audioData.f0) ||
        !writeFloatVector(out, audioData.rawF0) ||
        !writeFloatVector(out, audioData.cleanedF0) ||
        !writeFloatVector(out, audioData.denseF0) ||
        !writeFloatVector(out, audioData.baseF0) ||
        !writeFloatVector(out, audioData.basePitch) ||
        !writeFloatVector(out, audioData.deltaPitch) ||
        !writeBoolVector(out, audioData.voicedMask) ||
        !writeBoolVector(out, audioData.vadMask) ||
        (includeSourceDerivedData &&
         !writeMel(out, audioData.melSpectrogram)))
        return false;

    if (!out.writeInt64(
            static_cast<juce::int64>(audioData.segmentDebugChunks.size())))
        return false;
    for (const auto& chunk : audioData.segmentDebugChunks) {
        if (!out.writeInt(chunk.chunkIndex) ||
            !out.writeInt(chunk.startFrame) ||
            !out.writeInt(chunk.endFrame) ||
            !out.writeInt(chunk.shortRestThreshold) ||
            !out.writeInt64(static_cast<juce::int64>(chunk.events.size())))
            return false;

        for (const auto& event : chunk.events) {
            if (!out.writeInt(event.startFrame) ||
                !out.writeInt(event.endFrame) ||
                !out.writeInt(event.attachedStartFrame) ||
                !out.writeFloat(event.midiNote) ||
                !out.writeBool(event.isRest) ||
                !out.writeFloat(event.durationSeconds) ||
                !out.writeInt(event.durationFrames))
                return false;
        }
    }

    const auto& notes = project.getNotes();
    if (!out.writeInt64(static_cast<juce::int64>(notes.size())))
        return false;
    for (const auto& note : notes) {
        if (!writeFloatVector(out, note.getOriginalDeltaPitch()) ||
            !writeFloatVector(out, note.getDeltaPitch()) ||
            !writeFloatVector(out, note.getBakedDeltaPitch()) ||
            !writeFloatVector(out, note.getF0Values()))
            return false;
    }

    return true;
}

bool ProjectSerializer::fromBinaryArchive(Project& project, const void* data,
                                          size_t sizeInBytes)
{
    if (!data || sizeInBytes < 8)
        return false;

    juce::MemoryInputStream in(data, sizeInBytes, false);
    if (static_cast<std::uint32_t>(in.readInt()) != kProjectArchiveMagic)
        return false;
    const int archiveVersion = in.readInt();
    if (archiveVersion < 1 || archiveVersion > kProjectArchiveVersion)
        return false;

    const int archiveFlags =
        archiveVersion >= 4 ? in.readInt() : kProjectArchiveHasOriginalWaveform;
    const bool hasOriginalWaveform =
        (archiveFlags & kProjectArchiveHasOriginalWaveform) != 0;
    // Versions before 7 always stored the rendered project waveform.
    const bool hasRenderedWaveform =
        archiveVersion < 7 ||
        (archiveFlags & kProjectArchiveHasRenderedWaveform) != 0;
    // Versions before 6 always stored the global mel spectrogram, including
    // host-backed archives that omitted only originalWaveform.
    const bool hasMelSpectrogram =
        archiveVersion < 6 ||
        (archiveFlags & kProjectArchiveHasMelSpectrogram) != 0;

    const auto metadataJson = readString(in);
    auto metadata = juce::JSON::parse(metadataJson);
    if (!metadata.isObject() || !fromJson(project, metadata))
        return false;

    auto& audioData = project.getAudioData();
    if (hasRenderedWaveform) {
        if (!readAudioBuffer(in, audioData.waveform))
            return false;
    } else {
        audioData.waveform.setSize(0, 0);
    }
    if (hasOriginalWaveform) {
        if (!readAudioBuffer(in, audioData.originalWaveform))
            return false;
    } else {
        audioData.originalWaveform.setSize(0, 0);
    }
    if (!readFloatVector(in, audioData.f0))
        return false;

    if (archiveVersion >= 3) {
        if (!readFloatVector(in, audioData.rawF0) ||
            !readFloatVector(in, audioData.cleanedF0) ||
            !readFloatVector(in, audioData.denseF0))
            return false;
    }

    if (!readFloatVector(in, audioData.baseF0) ||
        !readFloatVector(in, audioData.basePitch) ||
        !readFloatVector(in, audioData.deltaPitch) ||
        !readBoolVector(in, audioData.voicedMask) ||
        !readBoolVector(in, audioData.vadMask))
        return false;
    if (hasMelSpectrogram) {
        if (!readMel(in, audioData.melSpectrogram))
            return false;
    } else {
        audioData.melSpectrogram.clear();
    }

    const auto debugChunkCount = in.readInt64();
    if (debugChunkCount < 0 ||
        debugChunkCount > std::numeric_limits<int>::max())
        return false;
    audioData.segmentDebugChunks.clear();
    audioData.segmentDebugChunks.reserve(static_cast<size_t>(debugChunkCount));
    for (juce::int64 i = 0; i < debugChunkCount; ++i) {
        AudioData::SegmentDebugChunk chunk;
        chunk.chunkIndex = in.readInt();
        chunk.startFrame = in.readInt();
        chunk.endFrame = in.readInt();
        chunk.shortRestThreshold = in.readInt();

        const auto eventCount = in.readInt64();
        if (eventCount < 0 || eventCount > std::numeric_limits<int>::max())
            return false;
        chunk.events.reserve(static_cast<size_t>(eventCount));
        for (juce::int64 eventIndex = 0; eventIndex < eventCount;
             ++eventIndex) {
            AudioData::SegmentDebugEvent event;
            event.startFrame = in.readInt();
            event.endFrame = in.readInt();
            event.attachedStartFrame = in.readInt();
            event.midiNote = in.readFloat();
            event.isRest = in.readBool();
            event.durationSeconds = in.readFloat();
            event.durationFrames = in.readInt();
            chunk.events.push_back(event);
        }
        audioData.segmentDebugChunks.push_back(std::move(chunk));
    }

    const auto noteCount = in.readInt64();
    if (noteCount < 0 || noteCount > std::numeric_limits<int>::max() ||
        static_cast<size_t>(noteCount) != project.getNotes().size())
        return false;

    for (auto& note : project.getNotes()) {
        std::vector<float> originalDelta;
        std::vector<float> delta;
        std::vector<float> bakedDelta;
        std::vector<float> f0Values;

        if (!readFloatVector(in, originalDelta) ||
            !readFloatVector(in, delta))
            return false;
        if (archiveVersion >= 9 && !readFloatVector(in, bakedDelta))
            return false;
        if (!readFloatVector(in, f0Values))
            return false;

        // Version 1 stored two redundant per-note waveform caches. Consume
        // them for backward compatibility; version 2 derives both views from
        // the project-level waveform buffers.
        if (archiveVersion == 1) {
            std::vector<float> obsoleteClipWaveform;
            std::vector<float> obsoleteSrcClipWaveform;
            if (!readFloatVector(in, obsoleteClipWaveform) ||
                !readFloatVector(in, obsoleteSrcClipWaveform))
                return false;
        }

        // Versions through 7 stored a full per-note synthesized waveform.
        // Consume it for compatibility, retain only the semantic fact that
        // the note contributed rendered audio, and let the project-level or
        // processed-region composite remain authoritative.
        bool hadLegacySynthWaveform = false;
        if (archiveVersion <= 7) {
            in.readInt(); // obsolete synth preroll
            std::vector<float> obsoleteSynthWaveform;
            if (!readFloatVector(in, obsoleteSynthWaveform))
                return false;
            hadLegacySynthWaveform = !obsoleteSynthWaveform.empty();
        }

        // Versions 1-4 stored a redundant per-note copy of mel frames.
        // Source frame ranges identify the same data in the project-level mel
        // spectrogram, so consume the legacy field without retaining it.
        if (archiveVersion <= 4) {
            std::vector<std::vector<float>> obsoleteClipMel;
            if (!readMel(in, obsoleteClipMel))
                return false;
        }

        if (!originalDelta.empty())
            note.setOriginalDeltaPitch(std::move(originalDelta));
        if (!delta.empty())
            note.setDeltaPitch(std::move(delta));
        if (!bakedDelta.empty())
            note.setBakedDeltaPitch(std::move(bakedDelta));
        if (!f0Values.empty())
            note.setF0Values(std::move(f0Values));
        if (hadLegacySynthWaveform) {
            note.setRenderedEdit(true);
            note.setSynthDirty(false);
        }
    }

    if (audioData.baseF0.empty())
        audioData.baseF0 = audioData.f0;
    if (audioData.rawF0.empty()) {
        audioData.rawF0 = audioData.f0;
        if (audioData.voicedMask.size() == audioData.rawF0.size()) {
            for (size_t i = 0; i < audioData.rawF0.size(); ++i)
                if (!audioData.voicedMask[i])
                    audioData.rawF0[i] = 0.0f;
        }
    }
    if (audioData.cleanedF0.empty())
        audioData.cleanedF0 = audioData.rawF0;
    if (audioData.denseF0.empty())
        audioData.denseF0 = audioData.f0;

    project.setModified(false);
    return true;
}

juce::var ProjectSerializer::noteToJson(const Note& note,
                                        bool includeAnalysisCache,
                                        bool includePitchData) {
    auto* obj = new juce::DynamicObject();

    obj->setProperty("startFrame", note.getStartFrame());
    obj->setProperty("endFrame", note.getEndFrame());
    obj->setProperty("srcStartFrame", note.getSrcStartFrame());
    obj->setProperty("srcEndFrame", note.getSrcEndFrame());
    obj->setProperty("midiNote", note.getMidiNote());
    obj->setProperty("originalMidiNote", note.getOriginalMidiNote());
    obj->setProperty("pitchOffset", note.getPitchOffset());
    obj->setProperty("volumeDb", note.getVolumeDb());
    obj->setProperty("rest", note.isRest());

    // Lyric/Phoneme
    if (note.hasLyric())
        obj->setProperty("lyric", note.getLyric());
    if (note.hasPhoneme())
        obj->setProperty("phoneme", note.getPhoneme());
    if (note.hasRenderedEdit())
        obj->setProperty("renderedEdit", true);

    // Pitch tool transformation parameters (non-destructive)
    obj->setProperty("tiltLeft", note.getTiltLeft());
    obj->setProperty("tiltRight", note.getTiltRight());
    obj->setProperty("vibrato", note.getVibrato());
    obj->setProperty("smoothLeftFrames", note.getSmoothLeftFrames());
    obj->setProperty("smoothRightFrames", note.getSmoothRightFrames());

    // Per-note original delta pitch (pristine curve from analysis)
    if (includePitchData && note.hasOriginalDeltaPitch())
        obj->setProperty("originalDeltaPitch", floatArrayToString(note.getOriginalDeltaPitch(), 4));
    if (includePitchData && note.hasBakedDeltaPitch())
        obj->setProperty("bakedDeltaPitch", floatArrayToString(note.getBakedDeltaPitch(), 4));

    if (includeAnalysisCache) {
        if (note.hasDeltaPitch())
            obj->setProperty("deltaPitch", floatArrayToString(note.getDeltaPitch(), 4));
        if (!note.getF0Values().empty())
            obj->setProperty("f0Values", floatArrayToString(note.getF0Values(), 2));
    }

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
    const float midiNote =
        static_cast<float>(json.getProperty("midiNote", 60.0));
    note.setMidiNote(midiNote);
    note.setOriginalMidiNote(static_cast<float>(
        json.getProperty("originalMidiNote", midiNote)));
    note.setPitchOffset(static_cast<float>(json.getProperty("pitchOffset", 0.0)));
    note.setVolumeDb(static_cast<float>(json.getProperty("volumeDb", 0.0)));
    note.setRest(json.getProperty("rest", false));

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
    note.setVibrato(static_cast<float>(json.getProperty("vibrato", 1.0)));
    note.setSmoothLeftFrames(json.getProperty("smoothLeftFrames", 0));
    note.setSmoothRightFrames(json.getProperty("smoothRightFrames", 0));

    // Per-note original delta pitch (pristine curve from analysis)
    auto origDeltaStr = json.getProperty("originalDeltaPitch", juce::var());
    if (!origDeltaStr.isVoid() && origDeltaStr.toString().isNotEmpty())
        note.setOriginalDeltaPitch(stringToFloatArray(origDeltaStr.toString()));

    auto bakedDeltaStr = json.getProperty("bakedDeltaPitch", juce::var());
    if (!bakedDeltaStr.isVoid() && bakedDeltaStr.toString().isNotEmpty())
        note.setBakedDeltaPitch(stringToFloatArray(bakedDeltaStr.toString()));

    auto deltaStr = json.getProperty("deltaPitch", juce::var());
    if (!deltaStr.isVoid() && deltaStr.toString().isNotEmpty())
        note.setDeltaPitch(stringToFloatArray(deltaStr.toString()));
    auto f0ValuesStr = json.getProperty("f0Values", juce::var());
    if (!f0ValuesStr.isVoid() && f0ValuesStr.toString().isNotEmpty())
        note.setF0Values(stringToFloatArray(f0ValuesStr.toString()));
    const auto legacySynthWaveform =
        json.getProperty("synthWaveform", juce::var());
    const bool hasRenderedEdit =
        static_cast<bool>(json.getProperty("renderedEdit", false)) ||
        (!legacySynthWaveform.isVoid() &&
         legacySynthWaveform.toString().isNotEmpty());
    note.setRenderedEdit(hasRenderedEdit);
    if (hasRenderedEdit)
        note.setSynthDirty(false);
    // Per-note delta scale/offset
    note.setDeltaScale(static_cast<float>(json.getProperty("deltaScale", 1.0)));
    note.setDeltaOffset(static_cast<float>(json.getProperty("deltaOffset", 0.0)));

    return true;
}

juce::var ProjectSerializer::pitchDataToJson(const AudioData& audioData) {
    auto* obj = new juce::DynamicObject();

    // Store as compact strings for efficiency
    obj->setProperty("f0", floatArrayToString(audioData.f0, 2));
    obj->setProperty("rawF0", floatArrayToString(audioData.rawF0, 2));
    obj->setProperty("cleanedF0", floatArrayToString(audioData.cleanedF0, 2));
    obj->setProperty("denseF0", floatArrayToString(audioData.denseF0, 2));
    obj->setProperty("baseF0", floatArrayToString(audioData.baseF0, 2));
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
    audioData.rawF0 =
        stringToFloatArray(json.getProperty("rawF0", "").toString());
    audioData.cleanedF0 =
        stringToFloatArray(json.getProperty("cleanedF0", "").toString());
    audioData.denseF0 =
        stringToFloatArray(json.getProperty("denseF0", "").toString());
    audioData.baseF0 = stringToFloatArray(json.getProperty("baseF0", "").toString());
    if (audioData.baseF0.empty())
        audioData.baseF0 = audioData.f0; // Backward compatibility
    audioData.basePitch = stringToFloatArray(json.getProperty("basePitch", "").toString());
    audioData.deltaPitch = stringToFloatArray(json.getProperty("deltaPitch", "").toString());
    audioData.voicedMask = stringToBoolArray(json.getProperty("voicedMask", "").toString());
    audioData.vadMask = stringToBoolArray(json.getProperty("vadMask", "").toString());

    if (audioData.rawF0.empty()) {
        audioData.rawF0 = audioData.f0;
        if (audioData.voicedMask.size() == audioData.rawF0.size()) {
            for (size_t i = 0; i < audioData.rawF0.size(); ++i)
                if (!audioData.voicedMask[i])
                    audioData.rawF0[i] = 0.0f;
        }
    }
    if (audioData.cleanedF0.empty())
        audioData.cleanedF0 = audioData.rawF0;
    if (audioData.denseF0.empty())
        audioData.denseF0 = audioData.f0;

    return true;
}

juce::var ProjectSerializer::audioBufferToJson(const juce::AudioBuffer<float>& buffer) {
    auto* obj = new juce::DynamicObject();
    obj->setProperty("channels", buffer.getNumChannels());
    obj->setProperty("samples", buffer.getNumSamples());

    juce::Array<juce::var> channelData;
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
        std::vector<float> samples(static_cast<size_t>(buffer.getNumSamples()));
        if (!samples.empty())
            std::copy(buffer.getReadPointer(ch),
                      buffer.getReadPointer(ch) + buffer.getNumSamples(),
                      samples.begin());
        channelData.add(floatArrayToString(samples, 6));
    }
    obj->setProperty("data", channelData);
    return juce::var(obj);
}

bool ProjectSerializer::audioBufferFromJson(juce::AudioBuffer<float>& buffer, const juce::var& json) {
    if (!json.isObject())
        return false;

    auto* data = json.getProperty("data", juce::var()).getArray();
    if (!data)
        return false;

    const int channels = std::max(0, static_cast<int>(
        json.getProperty("channels", data->size())));
    const int samples = std::max(0, static_cast<int>(
        json.getProperty("samples", 0)));
    if (channels <= 0 || samples <= 0) {
        buffer.setSize(0, 0);
        return true;
    }

    buffer.setSize(channels, samples, false, false, true);
    buffer.clear();
    for (int ch = 0; ch < std::min(channels, data->size()); ++ch) {
        auto values = stringToFloatArray((*data)[ch].toString());
        const int count = std::min(samples, static_cast<int>(values.size()));
        if (count > 0)
            std::copy(values.begin(), values.begin() + count,
                      buffer.getWritePointer(ch));
    }
    return true;
}

juce::var ProjectSerializer::melSpectrogramToJson(const std::vector<std::vector<float>>& mel) {
    auto* obj = new juce::DynamicObject();
    obj->setProperty("frames", static_cast<int>(mel.size()));
    obj->setProperty("bins", mel.empty() ? 0 : static_cast<int>(mel.front().size()));

    std::vector<float> flat;
    size_t total = 0;
    for (const auto& frame : mel)
        total += frame.size();
    flat.reserve(total);
    for (const auto& frame : mel)
        flat.insert(flat.end(), frame.begin(), frame.end());
    obj->setProperty("data", floatArrayToString(flat, 6));
    return juce::var(obj);
}

bool ProjectSerializer::melSpectrogramFromJson(std::vector<std::vector<float>>& mel, const juce::var& json) {
    if (!json.isObject())
        return false;

    const int frames = std::max(0, static_cast<int>(json.getProperty("frames", 0)));
    const int bins = std::max(0, static_cast<int>(json.getProperty("bins", 0)));
    if (frames <= 0 || bins <= 0) {
        mel.clear();
        return true;
    }

    auto flat = stringToFloatArray(json.getProperty("data", "").toString());
    mel.assign(static_cast<size_t>(frames), std::vector<float>(static_cast<size_t>(bins), 0.0f));
    const size_t count = std::min(flat.size(), static_cast<size_t>(frames) * static_cast<size_t>(bins));
    for (size_t i = 0; i < count; ++i)
        mel[i / static_cast<size_t>(bins)][i % static_cast<size_t>(bins)] = flat[i];
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
