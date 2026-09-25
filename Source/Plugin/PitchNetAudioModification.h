#pragma once

#include "../JuceHeader.h"

#if JucePlugin_Enable_ARA

#include <atomic>
#include <memory>

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
  // Persistence. Stream this modification's processed audio.
  bool writeProcessedAudioToStream(juce::OutputStream &output) const {
    const juce::SpinLock::ScopedLockType lock(processedAudioLock);
    if (processedAudio == nullptr || !processedAudio->hasAudio())
      return false;

    const auto &data = *processedAudio;
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

  bool readProcessedAudioFromStream(juce::InputStream &input) {
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

    setProcessedAudio(restored, sampleRateIn, startSampleIn);
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
};

#endif // JucePlugin_Enable_ARA
