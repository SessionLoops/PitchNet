// Regression cover for the non-ARA plug-in state archive.
//
// v10 stopped storing the mel spectrogram (it is a pure function of the
// pristine source waveform) and gained a streaming writer so plug-in state is
// emitted once instead of through an intermediate MemoryBlock. Both are easy
// to break silently: a project still loads, it just comes back without the
// mel the synthesizer needs, or with a length prefix that no longer matches.
#include "../Source/Models/ProjectSerializer.h"
#include "../Source/Utils/Constants.h"
#include "../Source/Utils/MelSpectrogram.h"
#include <cassert>
#include <cmath>
#include <iostream>

namespace {

constexpr int kFrames = 120;

Project makeProject(double timelineOffsetSeconds) {
  Project project;
  auto &audio = project.getAudioData();
  audio.sampleRate = SAMPLE_RATE;
  audio.timelineOffsetSeconds = timelineOffsetSeconds;

  const int numSamples = kFrames * HOP_SIZE;
  audio.originalWaveform.setSize(1, numSamples);
  const int offsetSamples =
      static_cast<int>(std::llround(timelineOffsetSeconds * SAMPLE_RATE));
  for (int i = 0; i < numSamples; ++i) {
    // Silence under the timeline padding, a voiced-ish tone after it, so the
    // rebuilt mel has something to actually get wrong.
    const float value =
        i < offsetSamples
            ? 0.0f
            : 0.4f * std::sin(2.0f * 3.14159265f * 220.0f *
                              static_cast<float>(i) / SAMPLE_RATE);
    audio.originalWaveform.setSample(0, i, value);
  }
  audio.waveform.makeCopyOf(audio.originalWaveform);
  audio.waveform.applyGain(0.5f); // stand in for a render

  audio.f0.assign(kFrames, 220.0f);
  audio.rawF0 = audio.f0;
  audio.cleanedF0 = audio.f0;
  audio.denseF0 = audio.f0;
  audio.baseF0 = audio.f0;
  audio.basePitch.assign(kFrames, 57.0f);
  audio.deltaPitch.assign(kFrames, 0.25f);
  audio.voicedMask.assign(kFrames, true);
  audio.vadMask.assign(kFrames, true);

  MelSpectrogram melComputer(audio.sampleRate, N_FFT, HOP_SIZE, NUM_MELS, FMIN,
                             FMAX);
  audio.melSpectrogram = melComputer.compute(
      audio.originalWaveform.getReadPointer(0), numSamples);

  const int offsetFrames = static_cast<int>(
      std::llround(timelineOffsetSeconds * SAMPLE_RATE / double(HOP_SIZE)));
  for (int i = 0; i < offsetFrames && i < (int)audio.melSpectrogram.size(); ++i)
    std::fill(audio.melSpectrogram[(size_t)i].begin(),
              audio.melSpectrogram[(size_t)i].end(), 0.0f);

  Note note(10, 40, 57);
  note.setLyric("la");
  note.setVolumeDb(-3.0f);
  note.setVibrato(0.4f);
  note.setDeltaPitch(std::vector<float>(30, 0.5f));
  note.setOriginalDeltaPitch(std::vector<float>(30, 0.25f));
  note.setBakedDeltaPitch(std::vector<float>(30, 0.75f));
  note.setF0Values(std::vector<float>(30, 220.0f));
  project.addNote(note);
  project.setGlobalPitchOffset(2.0f);
  project.setFormantShift(-1.0f);
  return project;
}

void expectNear(float a, float b, const char *what) {
  if (std::abs(a - b) > 1.0e-4f) {
    std::cerr << "mismatch in " << what << ": " << a << " vs " << b << "\n";
    assert(false);
  }
}

void roundTripRebuildsMel(double timelineOffsetSeconds) {
  const Project source = makeProject(timelineOffsetSeconds);

  juce::MemoryBlock archive;
  assert(ProjectSerializer::toBinaryArchive(
      source, archive, ProjectSerializer::BinaryArchiveMode::selfContained));

  // The mel must genuinely be absent from the bytes, not merely cleared on
  // load: that saving is the whole point of the format change. Bound the
  // archive by everything it legitimately still carries, and show there is no
  // room left in it for a mel spectrogram.
  const auto &sourceAudio = source.getAudioData();
  const size_t melBytes =
      sourceAudio.melSpectrogram.size() * NUM_MELS * sizeof(float);
  assert(melBytes > 0);
  const size_t waveformBytes =
      2 * (size_t)sourceAudio.waveform.getNumSamples() * sizeof(float);
  const size_t curveBytes = 7 * (size_t)kFrames * sizeof(float);
  const size_t maskBytes = 2 * (size_t)kFrames;
  const size_t noteBytes = 4 * 30 * sizeof(float);
  const size_t payloadBytes =
      waveformBytes + curveBytes + maskBytes + noteBytes;
  assert(archive.getSize() >= payloadBytes);
  assert(archive.getSize() < payloadBytes + melBytes);

  Project restored;
  assert(ProjectSerializer::fromBinaryArchive(restored, archive.getData(),
                                              archive.getSize()));

  const auto &a = sourceAudio;
  const auto &b = restored.getAudioData();
  assert(b.sampleRate == a.sampleRate);
  assert(b.waveform.getNumSamples() == a.waveform.getNumSamples());
  assert(b.originalWaveform.getNumSamples() == a.originalWaveform.getNumSamples());
  expectNear(b.waveform.getSample(0, 5000), a.waveform.getSample(0, 5000),
             "rendered waveform");
  assert(b.f0.size() == a.f0.size());
  expectNear(b.deltaPitch[3], a.deltaPitch[3], "deltaPitch");
  assert(b.voicedMask == a.voicedMask);

  // Rebuilt, same length, same content - including the zero-filled frames in
  // front of the timeline offset.
  assert(!b.melSpectrogram.empty());
  assert(b.melSpectrogram.size() == a.melSpectrogram.size());
  for (size_t f = 0; f < a.melSpectrogram.size(); ++f) {
    assert(b.melSpectrogram[f].size() == (size_t)NUM_MELS);
    for (size_t bin = 0; bin < a.melSpectrogram[f].size(); ++bin)
      expectNear(b.melSpectrogram[f][bin], a.melSpectrogram[f][bin], "mel");
  }

  assert(restored.getNotes().size() == 1);
  const auto &note = restored.getNotes()[0];
  assert(note.getStartFrame() == 10 && note.getEndFrame() == 40);
  assert(note.getLyric() == "la");
  expectNear(note.getVolumeDb(), -3.0f, "note volume");
  expectNear(note.getDeltaPitch()[0], 0.5f, "note deltaPitch");
  expectNear(note.getBakedDeltaPitch()[0], 0.75f, "note bakedDeltaPitch");
  expectNear(restored.getGlobalPitchOffset(), 2.0f, "global pitch offset");
}

void streamWriterMatchesMemoryBlockWriter() {
  const Project source = makeProject(0.0);

  juce::MemoryBlock viaBlock;
  assert(ProjectSerializer::toBinaryArchive(source, viaBlock));

  // The plug-in state path writes the archive into a stream that already has a
  // header in it and gets a length prefix patched in afterwards, so the writer
  // must not assume it starts at offset zero.
  juce::MemoryBlock envelope;
  juce::int64 archiveStart = 0;
  juce::int64 archiveEnd = 0;
  {
    juce::MemoryOutputStream out(envelope, false);
    out.writeInt(0x504E5053);
    out.writeInt(1);
    const auto lengthField = out.getPosition();
    out.writeInt64(0);
    archiveStart = out.getPosition();
    assert(ProjectSerializer::toBinaryArchive(source, out));
    archiveEnd = out.getPosition();
    out.setPosition(lengthField);
    out.writeInt64(archiveEnd - archiveStart);
    out.setPosition(archiveEnd);
  }

  assert(archiveEnd - archiveStart == (juce::int64)viaBlock.getSize());
  assert(std::memcmp(static_cast<const char *>(envelope.getData()) + archiveStart,
                     viaBlock.getData(), viaBlock.getSize()) == 0);

  // And the patched length prefix has to describe the archive that follows it.
  juce::MemoryInputStream in(envelope, false);
  assert((std::uint32_t)in.readInt() == 0x504E5053u);
  assert(in.readInt() == 1);
  assert(in.readInt64() == (juce::int64)viaBlock.getSize());

  Project restored;
  assert(ProjectSerializer::fromBinaryArchive(
      restored, static_cast<const char *>(envelope.getData()) + archiveStart,
      viaBlock.getSize()));
  assert(restored.getNotes().size() == 1);
  assert(!restored.getAudioData().melSpectrogram.empty());
}

void hostBackedArchiveStaysSourceless() {
  const Project source = makeProject(0.0);

  juce::MemoryBlock archive;
  assert(ProjectSerializer::toBinaryArchive(
      source, archive, ProjectSerializer::BinaryArchiveMode::hostBackedARA));

  Project restored;
  assert(ProjectSerializer::fromBinaryArchive(restored, archive.getData(),
                                              archive.getSize()));

  // No source waveform to rebuild from: the mel stays empty for the ARA
  // hydration path to fill, rather than being invented from nothing.
  const auto &b = restored.getAudioData();
  assert(b.originalWaveform.getNumSamples() == 0);
  assert(b.waveform.getNumSamples() == 0);
  assert(b.melSpectrogram.empty());
  assert(b.deltaPitch.size() == (size_t)kFrames);
  assert(restored.getNotes().size() == 1);
}

} // namespace

int main() {
  roundTripRebuildsMel(0.0);
  roundTripRebuildsMel(0.25);
  streamWriterMatchesMemoryBlockWriter();
  hostBackedArchiveStaysSourceless();
  std::cout << "PluginStateArchiveTests passed\n";
  return 0;
}
