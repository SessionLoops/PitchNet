#pragma once

#include "../JuceHeader.h"

#if JucePlugin_Enable_ARA

#include <atomic>
#include <memory>
#include <vector>

// PROCESSED audio and project archive owned by the ARA audio modification, so
// the analysed/processed result lives independently of the editor: the timeline
// draws each clip from here and the playback renderer mixes from here, both of
// which work with the UI closed.
//
// State is held in single slots, not keyed containers. ARA gives persistent
// identity to the audio modification, and every playback region referencing one
// is a window onto the same edit layer, so a modification owns exactly one
// project archive and one processed-audio buffer. A key would carry no
// information.
//
// The modification's host-assigned persistent ID is deliberately NOT used as an
// identity here. ARA treats it as an archive-reconnection token and a mutable
// model property - hosts may adjust it on restore or import, and REAPER changes
// it from an absolute to a project-relative path on a project's first save. It
// belongs in the archive stream and in diagnostics, nowhere else. Live identity
// is getLiveKey(), a process-local serial minted at construction.
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
                                   optionalModificationToClone),
        liveKey("mod:" + juce::String(nextLiveKeySerial().fetch_add(1))) {
    // ARA defines the source passed here as the object whose internal state the
    // clone must copy, and the host assigns the clone's own properties straight
    // afterwards. Copying synchronously here is therefore complete: nothing can
    // arrive on the source in between, and later edits to the source correctly
    // do not bleed into an existing clone.
    if (const auto *sourceModification =
            dynamic_cast<const PitchNetAudioModification *>(
                optionalModificationToClone)) {
      const juce::SpinLock::ScopedLockType lock(
          sourceModification->processedAudioLock);
      if (sourceModification->processedAudio != nullptr &&
          sourceModification->processedAudio->hasAudio()) {
        processedAudio = std::make_unique<ProcessedRegionData>();
        processedAudio->setAudio(
            sourceModification->processedAudio->audio,
            sourceModification->processedAudio->sampleRate,
            sourceModification->processedAudio->startSampleInModification);
      }
      projectArchive = sourceModification->projectArchive;
      legacyEntries = sourceModification->legacyEntries;
    }
  }

  //============================================================================
  // Live identity
  //
  // Stable for this object's lifetime and unique within the process. A raw
  // address would also be lifetime-stable, but destruction lets the allocator
  // reuse it, so a stale key could come to equal a live object's key. A serial
  // cannot collide.
  //
  // MUST NEVER be written to any archive. It is meaningless across runs.
  const juce::String &getLiveKey() const noexcept { return liveKey; }

  //============================================================================
  // Mutators
  void setProcessedAudio(const juce::AudioBuffer<float> &buffer,
                         double sampleRateIn,
                         juce::int64 startSampleInModificationIn) {
    const juce::SpinLock::ScopedLockType lock(processedAudioLock);
    if (processedAudio == nullptr)
      processedAudio = std::make_unique<ProcessedRegionData>();
    processedAudio->setAudio(buffer, sampleRateIn, startSampleInModificationIn);
  }

  void clearProcessedAudio() {
    const juce::SpinLock::ScopedLockType lock(processedAudioLock);
    processedAudio.reset();
  }

  void setProjectArchive(const void *data, size_t sizeInBytes) const {
    if (data == nullptr || sizeInBytes == 0)
      return;

    const juce::SpinLock::ScopedLockType lock(processedAudioLock);
    projectArchive = juce::MemoryBlock(data, sizeInBytes);
  }

  //============================================================================
  // Queries
  bool copyProjectArchive(juce::MemoryBlock &dest) const {
    const juce::SpinLock::ScopedLockType lock(processedAudioLock);
    if (projectArchive.getSize() == 0)
      return false;

    dest = projectArchive;
    return true;
  }

  bool hasProjectArchive() const {
    const juce::SpinLock::ScopedLockType lock(processedAudioLock);
    return projectArchive.getSize() > 0;
  }

  bool hasProcessedAudio() const {
    const juce::SpinLock::ScopedLockType lock(processedAudioLock);
    return processedAudio != nullptr && processedAudio->hasAudio();
  }

  bool copyProcessedAudio(juce::AudioBuffer<float> &buffer,
                          double &sampleRateOut,
                          juce::int64 &startSampleOut) const {
    const juce::SpinLock::ScopedLockType lock(processedAudioLock);
    if (processedAudio == nullptr || !processedAudio->hasAudio())
      return false;

    buffer.makeCopyOf(processedAudio->audio);
    sampleRateOut = processedAudio->sampleRate;
    startSampleOut = processedAudio->startSampleInModification;
    return true;
  }

  // Read access must hold the lock while the returned pointer is used on the
  // audio thread. tryLockProcessedAudio() gives a try-lock for the renderer so
  // it never blocks; getProcessedData() returns the raw pointer.
  juce::SpinLock::ScopedTryLockType tryLockProcessedAudio() const {
    return juce::SpinLock::ScopedTryLockType(processedAudioLock);
  }

  const ProcessedRegionData *getProcessedData() const noexcept {
    return processedAudio.get();
  }

  ProcessedRegionData *getProcessedData() noexcept {
    return processedAudio.get();
  }

  // Every playback region referencing this modification shares its edit layer,
  // so they all serve the same processed audio. This reverses the earlier
  // per-region rule, which existed when state was owned by regions rather than
  // by the modification.

  //============================================================================
  // Legacy (pre-v4) edits
  //
  // Edits saved by PitchNet builds that wrote ARA archive v3, kept exactly as
  // they were archived: one entry per playback region of the old build, each
  // with its editable project and, when the region was edited, its rendered
  // audio. They are never installed as v4 state. The v3 project is
  // timeline-anchored and the v4 editor cannot use it, so a modification
  // holding legacy entries is played back and saved but not edited.
  //
  // The rendered audio needs no conversion. It is region-local and carries its
  // start in modification time - the convention the playback mixer already
  // uses - so it plays through the ordinary mixer as saved.
  //
  // The project bytes are kept verbatim, although nothing reads them yet, so a
  // later migration to v4 still has everything the old build wrote. Clearing
  // the entries is the only way out of the legacy state; a migration would
  // install v4 state and then call clearLegacyEntries().
  struct LegacyEntry {
    int regionIndex = 0;
    juce::MemoryBlock projectArchive;
    juce::AudioBuffer<float> audio;
    double sampleRate = 0.0;
    juce::int64 startSampleInModification = 0;

    bool hasAudio() const {
      return audio.getNumSamples() > 0 && sampleRate > 0.0;
    }
  };

  void setLegacyEntries(std::vector<LegacyEntry> entries) {
    const juce::SpinLock::ScopedLockType lock(processedAudioLock);
    legacyEntries = std::move(entries);
  }

  void clearLegacyEntries() {
    const juce::SpinLock::ScopedLockType lock(processedAudioLock);
    legacyEntries.clear();
  }

  bool hasLegacyEntries() const {
    const juce::SpinLock::ScopedLockType lock(processedAudioLock);
    return !legacyEntries.empty();
  }

  // The entry whose rendered audio a region starting at startInModification
  // should play, or nullptr to play the raw source. Legacy clips could not be
  // moved or resized in the builds that wrote them, so an entry starts exactly
  // where its region does; covering is the fallback for a region trimmed
  // since. An old split sibling left unedited has no audio and matches
  // nothing. Caller holds tryLockProcessedAudio(); allocation-free.
  const LegacyEntry *findLegacyEntryFor(juce::int64 startInModification,
                                        double modificationSampleRate) const
      noexcept {
    const LegacyEntry *covering = nullptr;
    for (const auto &entry : legacyEntries) {
      if (!entry.hasAudio())
        continue;
      if (entry.startSampleInModification == startInModification)
        return &entry;

      const double lengthInModification =
          modificationSampleRate > 0.0
              ? entry.audio.getNumSamples() * modificationSampleRate /
                    entry.sampleRate
              : static_cast<double>(entry.audio.getNumSamples());
      const auto end = entry.startSampleInModification +
                       static_cast<juce::int64>(lengthInModification);
      if (covering == nullptr &&
          startInModification > entry.startSampleInModification &&
          startInModification < end)
        covering = &entry;
    }
    return covering;
  }

  // Writes the entries in the archive-v3 region-entry layout, so the bytes
  // restored are the bytes saved.
  bool writeLegacyEntriesToStream(juce::OutputStream &output) const {
    const juce::SpinLock::ScopedLockType lock(processedAudioLock);
    if (!output.writeInt(static_cast<int>(legacyEntries.size())))
      return false;
    for (const auto &entry : legacyEntries) {
      if (!output.writeInt(entry.regionIndex) ||
          !output.writeInt64(
              static_cast<juce::int64>(entry.projectArchive.getSize())))
        return false;
      if (entry.projectArchive.getSize() > 0 &&
          !output.write(entry.projectArchive.getData(),
                        entry.projectArchive.getSize()))
        return false;
      if (!output.writeInt(entry.hasAudio() ? 1 : 0))
        return false;
      if (entry.hasAudio() &&
          !writeAudioToStream(output, entry.audio, entry.sampleRate,
                              entry.startSampleInModification))
        return false;
    }
    return true;
  }

  //============================================================================
  // Persistence. Stream this modification's processed audio.
  bool writeProcessedAudioToStream(juce::OutputStream &output) const {
    const juce::SpinLock::ScopedLockType lock(processedAudioLock);
    if (processedAudio == nullptr || !processedAudio->hasAudio())
      return false;

    const auto &data = *processedAudio;
    return writeAudioToStream(output, data.audio, data.sampleRate,
                              data.startSampleInModification);
  }

  bool readProcessedAudioFromStream(juce::InputStream &input) {
    juce::AudioBuffer<float> restored;
    double sampleRateIn = 0.0;
    juce::int64 startSampleIn = 0;
    if (!readAudioFromStream(input, restored, sampleRateIn, startSampleIn))
      return false;

    setProcessedAudio(restored, sampleRateIn, startSampleIn);
    return true;
  }

  // The processed-audio wire format, shared by v4 processed audio and legacy
  // entries: it has not changed since archive v3.
  static bool writeAudioToStream(juce::OutputStream &output,
                                 const juce::AudioBuffer<float> &audio,
                                 double sampleRate,
                                 juce::int64 startSampleInModification) {
    output.writeDouble(sampleRate);
    output.writeInt64(startSampleInModification);
    output.writeInt(audio.getNumChannels());
    output.writeInt(audio.getNumSamples());
    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
      if (!output.write(audio.getReadPointer(ch),
                        static_cast<size_t>(audio.getNumSamples()) *
                            sizeof(float)))
        return false;
    return true;
  }

  static bool readAudioFromStream(juce::InputStream &input,
                                  juce::AudioBuffer<float> &audio,
                                  double &sampleRate,
                                  juce::int64 &startSampleInModification) {
    sampleRate = input.readDouble();
    startSampleInModification = input.readInt64();
    const auto numChannels = input.readInt();
    const auto numSamples = input.readInt();
    if (sampleRate <= 0.0 || numChannels < 0 || numSamples < 0)
      return false;

    audio.setSize(numChannels, numSamples);
    for (int ch = 0; ch < numChannels; ++ch)
      if (input.read(audio.getWritePointer(ch),
                     numSamples * static_cast<int>(sizeof(float))) !=
          numSamples * static_cast<int>(sizeof(float)))
        return false;
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
  static std::atomic<juce::uint64> &nextLiveKeySerial() {
    static std::atomic<juce::uint64> serial{1};
    return serial;
  }

  const juce::String liveKey;
  mutable juce::SpinLock processedAudioLock;
  std::unique_ptr<ProcessedRegionData> processedAudio;
  mutable juce::MemoryBlock projectArchive;
  std::vector<LegacyEntry> legacyEntries;
};

#endif // JucePlugin_Enable_ARA
