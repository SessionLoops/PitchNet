#pragma once

#include "../Models/Project.h"

#include <algorithm>
#include <cstddef>

namespace timingRegions
{
struct SourceRegion
{
    int index = 0;
    int start = 0;
    int end = 0;
};

inline int sourceMidpoint(const Note& note)
{
    return note.getSrcStartFrame() +
           std::max(0, note.getSrcDurationFrames()) / 2;
}

inline SourceRegion getSourceRegion(const Project& project, const Note& note)
{
    const auto& audioData = project.getAudioData();
    const auto& ranges = audioData.segmentChunkRanges;
    if (ranges.empty())
        return {0, 0, audioData.getNumFrames()};

    const int midpoint = sourceMidpoint(note);
    for (size_t i = 0; i < ranges.size(); ++i)
        if (midpoint >= ranges[i].first && midpoint < ranges[i].second)
            return {static_cast<int>(i), ranges[i].first, ranges[i].second};

    return {0, 0, audioData.getNumFrames()};
}

inline bool belongsTo(const Note& note, const SourceRegion& region)
{
    const int midpoint = sourceMidpoint(note);
    return midpoint >= region.start && midpoint < region.end;
}

inline const Note* firstNote(const Project& project,
                             const SourceRegion& region)
{
    const Note* result = nullptr;
    for (const auto& note : project.getNotes())
    {
        if (note.isRest() || !belongsTo(note, region))
            continue;
        if (!result || note.getSrcStartFrame() < result->getSrcStartFrame())
            result = &note;
    }
    return result;
}

inline const Note* lastNote(const Project& project,
                            const SourceRegion& region)
{
    const Note* result = nullptr;
    for (const auto& note : project.getNotes())
    {
        if (note.isRest() || !belongsTo(note, region))
            continue;
        if (!result || note.getSrcEndFrame() > result->getSrcEndFrame())
            result = &note;
    }
    return result;
}

inline bool isFirstNote(const Project& project, const Note& note)
{
    return firstNote(project, getSourceRegion(project, note)) == &note;
}

inline bool isLastNote(const Project& project, const Note& note)
{
    return lastNote(project, getSourceRegion(project, note)) == &note;
}

inline SourceRegion regionAt(const Project& project, int index)
{
    const auto& audioData = project.getAudioData();
    const auto& ranges = audioData.segmentChunkRanges;
    if (ranges.empty())
        return {0, 0, audioData.getNumFrames()};
    index = std::clamp(index, 0, static_cast<int>(ranges.size()) - 1);
    return {index, ranges[static_cast<size_t>(index)].first,
            ranges[static_cast<size_t>(index)].second};
}

inline int regionCount(const Project& project)
{
    const auto count = project.getAudioData().segmentChunkRanges.size();
    return count == 0 ? 1 : static_cast<int>(count);
}

inline float visualStart(const Project& project, const SourceRegion& region)
{
    if (const auto* note = firstNote(project, region))
        return static_cast<float>(region.start) +
               note->getVisualStartFrame() -
               static_cast<float>(note->getSrcStartFrame());
    return static_cast<float>(region.start);
}

inline float visualEnd(const Project& project, const SourceRegion& region)
{
    if (const auto* note = lastNote(project, region))
        return static_cast<float>(region.end) +
               note->getVisualEndFrame() -
               static_cast<float>(note->getSrcEndFrame());
    return static_cast<float>(region.end);
}
}
