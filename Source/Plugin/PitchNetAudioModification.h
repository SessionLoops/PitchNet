#pragma once

#include "../JuceHeader.h"

#if JucePlugin_Enable_ARA

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

// Per-region PROCESSED audio stored on the ARA audio modification, so that each
// playback region/track carries its own analysed/processed result independent of
// the editor. This mirrors VocalNet's ARADemoPluginAudioModification /
// ConvertedRegionData model: the timeline draws each clip from here and the
// playback renderer mixes each region's processed audio from here, both of which
// work with the UI closed.
//
// Regions are keyed by pitchnetRegionKey() (modification persistent ID + current
// region index), computed by the caller and passed in as regionID.
class PitchNetAudioModification final : public juce::ARAAudioModification {
public:
  struct ProcessedRegionData {
    juce::AudioBuffer<float> audio;
    double sampleRate = 0.0;
    juce::int64 startSampleInModification = 0;

    juce::AudioFormatManager thumbnailFormatManager;
    juce::AudioThumbnailCache thumbnailCache{8};
    juce::AudioThumbnail thumbnail;

    ProcessedRegionData() : thumbnail(128, thumbnailFormatManager, thumbnailCache) {
      thumbnailFormatManager.registerBasicFormats();
    }

    void setAudio(const juce::AudioBuffer<float> &buffer, double sampleRateIn,
                  juce::int64 startSampleInModificationIn) {
      audio.makeCopyOf(buffer);
      sampleRate = sampleRateIn;
      startSampleInModification = startSampleInModificationIn;
      thumbnail.reset(audio.getNumChannels(), sampleRate, audio.getNumSamples());
      thumbnail.addBlock(0, audio, 0, audio.getNumSamples());
    }

    void clear() {
      audio.setSize(0, 0);
      sampleRate = 0.0;
      startSampleInModification = 0;
      thumbnail.clear();
    }

    bool hasAudio() const {
      return audio.getNumSamples() > 0 && sampleRate > 0.0;
    }
  };

  PitchNetAudioModification(
      juce::ARAAudioSource *audioSource,
      ARA::ARAAudioModificationHostRef hostRef,
      const juce::ARAAudioModification *optionalModificationToClone)
      : juce::ARAAudioModification(audioSource, hostRef,
                                   optionalModificationToClone) {
    if (const auto *sourceModification =
            dynamic_cast<const PitchNetAudioModification *>(
                optionalModificationToClone)) {
      const juce::SpinLock::ScopedLockType lock(
          sourceModification->processedAudioLock);
      for (const auto &[regionID, sourceData] :
           sourceModification->processedRegions) {
        if (sourceData == nullptr || !sourceData->hasAudio())
          continue;
        auto copy = std::make_unique<ProcessedRegionData>();
        copy->setAudio(sourceData->audio, sourceData->sampleRate,
                       sourceData->startSampleInModification);
        processedRegions[regionID] = std::move(copy);
      }
      regionProjectArchives = sourceModification->regionProjectArchives;

      // ARA clones an AudioModification before the host creates the clone's
      // PlaybackRegions.  The copied maps above are keyed with the source
      // modification's persistent ID/live host refs, so remember each source
      // region's keys and re-home its state when the corresponding cloned
      // region is subsequently added.  Without this, hosts such as Cubase can
      // create a track version successfully but the new region only sees its
      // unedited source audio.
      const auto sourcePersistentID =
          juce::String(sourceModification->getPersistentID());
      const auto &sourceRegions =
          sourceModification->getPlaybackRegions<juce::ARAPlaybackRegion>();
      clonedSourceRegionKeys.reserve(sourceRegions.size());
      for (size_t index = 0; index < sourceRegions.size(); ++index) {
        const auto *region = sourceRegions[index];
        const auto sourceRegionHostRef =
            region != nullptr
                ? reinterpret_cast<std::uintptr_t>(region->getHostRef())
                : 0;
        const auto objectRef = reinterpret_cast<std::uintptr_t>(region);
        const auto liveRef =
            sourceRegionHostRef != 0 ? sourceRegionHostRef : objectRef;
        clonedSourceRegionKeys.push_back(
            {sourcePersistentID + ":live:" +
                 juce::String::toHexString(static_cast<juce::int64>(liveRef)),
             sourcePersistentID + ":" +
                 juce::String(static_cast<int>(index))});
      }
    }
  }

  // Complete ARA clone-state transfer once the host has created a playback
  // region on this new modification.  State is placed under the clone's stable
  // archive key; all readers already fall back from the live key to that key.
  void materializeClonedStateForRegion(juce::ARAPlaybackRegion &region) {
    const auto &regions = getPlaybackRegions<juce::ARAPlaybackRegion>();
    const auto regionIt = std::find(regions.begin(), regions.end(), &region);
    if (regionIt == regions.end())
      return;

    const auto index = static_cast<size_t>(
        std::distance(regions.begin(), regionIt));
    if (index >= clonedSourceRegionKeys.size())
      return;

    const auto destinationPersistentID = juce::String(getPersistentID());
    if (destinationPersistentID.isEmpty())
      return;

    const auto destinationKey =
        destinationPersistentID + ":" +
        juce::String(static_cast<int>(index));
    const auto sourceKeys = clonedSourceRegionKeys[index];

    const juce::SpinLock::ScopedLockType lock(processedAudioLock);

    auto moveProcessedAudio = [&](const juce::String &sourceKey) {
      const auto sourceIt = processedRegions.find(sourceKey);
      if (sourceIt == processedRegions.end() || sourceIt->second == nullptr)
        return false;
      processedRegions[destinationKey] = std::move(sourceIt->second);
      processedRegions.erase(sourceIt);
      return true;
    };
    if (!moveProcessedAudio(sourceKeys.liveKey))
      moveProcessedAudio(sourceKeys.archivedKey);

    auto moveProjectArchive = [&](const juce::String &sourceKey) {
      const auto sourceIt = regionProjectArchives.find(sourceKey);
      if (sourceIt == regionProjectArchives.end() ||
          sourceIt->second.getSize() == 0)
        return false;
      regionProjectArchives[destinationKey] = std::move(sourceIt->second);
      regionProjectArchives.erase(sourceIt);
      return true;
    };
    if (!moveProjectArchive(sourceKeys.liveKey))
      moveProjectArchive(sourceKeys.archivedKey);

    // A live and an indexed source entry can coexist after a project reload.
    // The live entry above is newer; discard its stale copied counterpart.
    if (sourceKeys.liveKey != destinationKey)
      processedRegions.erase(sourceKeys.liveKey);
    if (sourceKeys.archivedKey != destinationKey)
      processedRegions.erase(sourceKeys.archivedKey);
    if (sourceKeys.liveKey != destinationKey)
      regionProjectArchives.erase(sourceKeys.liveKey);
    if (sourceKeys.archivedKey != destinationKey)
      regionProjectArchives.erase(sourceKeys.archivedKey);
  }

  //============================================================================
  // Mutators
  void setProcessedAudioForRegion(const juce::String &regionID,
                                  const juce::AudioBuffer<float> &buffer,
                                  double sampleRateIn,
                                  juce::int64 startSampleInModificationIn) {
    const juce::SpinLock::ScopedLockType lock(processedAudioLock);
    auto &data = processedRegions[regionID];
    if (data == nullptr)
      data = std::make_unique<ProcessedRegionData>();
    data->setAudio(buffer, sampleRateIn, startSampleInModificationIn);
  }

  void clearProcessedAudioForRegion(const juce::String &regionID) {
    const juce::SpinLock::ScopedLockType lock(processedAudioLock);
    processedRegions.erase(regionID);
  }

  void clearProcessedAudio() {
    const juce::SpinLock::ScopedLockType lock(processedAudioLock);
    processedRegions.clear();
  }

  void setProjectArchiveForRegion(const juce::String &regionID,
                                  const void *data, size_t sizeInBytes) const {
    if (regionID.isEmpty() || data == nullptr || sizeInBytes == 0)
      return;

    const juce::SpinLock::ScopedLockType lock(processedAudioLock);
    regionProjectArchives[regionID] = juce::MemoryBlock(data, sizeInBytes);
  }

  bool copyProjectArchiveForRegion(const juce::String &regionID,
                                   juce::MemoryBlock &dest) const {
    const juce::SpinLock::ScopedLockType lock(processedAudioLock);
    const auto it = regionProjectArchives.find(regionID);
    if (it == regionProjectArchives.end() || it->second.getSize() == 0)
      return false;

    dest = it->second;
    return true;
  }

  bool hasProjectArchiveForRegion(const juce::String &regionID) const {
    const juce::SpinLock::ScopedLockType lock(processedAudioLock);
    const auto it = regionProjectArchives.find(regionID);
    return it != regionProjectArchives.end() && it->second.getSize() > 0;
  }

  //============================================================================
  // Queries
  bool hasProcessedAudioForRegion(const juce::String &regionID) const {
    const juce::SpinLock::ScopedLockType lock(processedAudioLock);
    if (const auto it = processedRegions.find(regionID);
        it != processedRegions.end() && it->second != nullptr)
      return it->second->hasAudio();
    return false;
  }

  bool copyProcessedAudioForRegion(const juce::String &regionID,
                                   juce::AudioBuffer<float> &buffer,
                                   double &sampleRateOut,
                                   juce::int64 &startSampleOut) const {
    const juce::SpinLock::ScopedLockType lock(processedAudioLock);
    const auto it = processedRegions.find(regionID);
    if (it == processedRegions.end() || it->second == nullptr ||
        !it->second->hasAudio())
      return false;

    buffer.makeCopyOf(it->second->audio);
    sampleRateOut = it->second->sampleRate;
    startSampleOut = it->second->startSampleInModification;
    return true;
  }

  bool hasProcessedAudio() const {
    const juce::SpinLock::ScopedLockType lock(processedAudioLock);
    for (const auto &[_, data] : processedRegions)
      if (data != nullptr && data->hasAudio())
        return true;
    return false;
  }

  // Read access must hold the lock while the returned pointer is used on the
  // audio thread. tryLockProcessedAudio() gives a try-lock for the renderer so
  // it never blocks; getProcessedRegionData() returns the raw pointer.
  juce::SpinLock::ScopedTryLockType tryLockProcessedAudio() const {
    return juce::SpinLock::ScopedTryLockType(processedAudioLock);
  }

  const ProcessedRegionData *
  getProcessedRegionData(const juce::String &regionID) const noexcept {
    if (const auto it = processedRegions.find(regionID);
        it != processedRegions.end())
      return it->second.get();
    return nullptr;
  }

  ProcessedRegionData *
  getProcessedRegionData(const juce::String &regionID) noexcept {
    if (const auto it = processedRegions.find(regionID);
        it != processedRegions.end())
      return it->second.get();
    return nullptr;
  }

  // NOTE: there is deliberately no "only one processed region" fallback
  // (VocalNet's getOnlyConvertedRegionData()): returning another region's
  // processed audio makes every region sharing this modification play the
  // edited region's audio. A region either finds ITS OWN processed audio by
  // key, or it plays its raw source.

  std::vector<juce::String> getProcessedRegionIDs() const {
    const juce::SpinLock::ScopedLockType lock(processedAudioLock);
    std::vector<juce::String> ids;
    ids.reserve(processedRegions.size());
    for (const auto &[regionID, data] : processedRegions)
      if (data != nullptr && data->hasAudio())
        ids.push_back(regionID);
    return ids;
  }

  //============================================================================
  // Persistence (Stage D). Stream one region's processed audio.
  bool writeProcessedAudioForRegionToStream(const juce::String &regionID,
                                            juce::OutputStream &output) const {
    const juce::SpinLock::ScopedLockType lock(processedAudioLock);
    const auto it = processedRegions.find(regionID);
    if (it == processedRegions.end() || it->second == nullptr ||
        !it->second->hasAudio())
      return false;

    const auto &data = *it->second;
    output.writeDouble(data.sampleRate);
    output.writeInt64(data.startSampleInModification);
    output.writeInt(data.audio.getNumChannels());
    output.writeInt(data.audio.getNumSamples());
    for (int ch = 0; ch < data.audio.getNumChannels(); ++ch)
      if (!output.write(data.audio.getReadPointer(ch),
                        static_cast<size_t>(data.audio.getNumSamples()) *
                            sizeof(float)))
        return false;
    return true;
  }

  bool readProcessedAudioForRegionFromStream(const juce::String &regionID,
                                             juce::InputStream &input) {
    const auto sampleRateIn = input.readDouble();
    const auto startSampleIn = input.readInt64();
    const auto numChannels = input.readInt();
    const auto numSamples = input.readInt();
    if (sampleRateIn <= 0.0 || numChannels < 0 || numSamples < 0)
      return false;

    juce::AudioBuffer<float> restored(numChannels, numSamples);
    for (int ch = 0; ch < numChannels; ++ch)
      if (input.read(restored.getWritePointer(ch),
                     numSamples * static_cast<int>(sizeof(float))) !=
          numSamples * static_cast<int>(sizeof(float)))
        return false;

    setProcessedAudioForRegion(regionID, restored, sampleRateIn, startSampleIn);
    return true;
  }

  static bool skipProcessedAudioFromStream(juce::InputStream &input) {
    input.readDouble();
    input.readInt64();
    const auto numChannels = input.readInt();
    const auto numSamples = input.readInt();
    if (numChannels < 0 || numSamples < 0)
      return false;
    juce::HeapBlock<char> skip(static_cast<size_t>(numSamples) * sizeof(float));
    for (int ch = 0; ch < numChannels; ++ch)
      if (input.read(skip.get(), numSamples * static_cast<int>(sizeof(float))) !=
          numSamples * static_cast<int>(sizeof(float)))
        return false;
    return true;
  }

private:
  struct ClonedSourceRegionKeys {
    juce::String liveKey;
    juce::String archivedKey;
  };

  mutable juce::SpinLock processedAudioLock;
  std::map<juce::String, std::unique_ptr<ProcessedRegionData>> processedRegions;
  mutable std::map<juce::String, juce::MemoryBlock> regionProjectArchives;
  std::vector<ClonedSourceRegionKeys> clonedSourceRegionKeys;
};

#endif // JucePlugin_Enable_ARA
