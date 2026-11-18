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
#include "TestAssert.h"
#include <cmath>
#include <iostream>

namespace {

constexpr int kFrames = 120;

Project makeProject(double timelineOffsetSeconds) {
  Project project;
  project.uiWidth = 1234;
  project.uiHeight = 789;
  project.thumbnailVisible = true;
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

void expectNear(float a, float b, const char *what, float tolerance = 1.0e-4f) {
  if (std::abs(a - b) > tolerance) {
    std::cerr << "mismatch in " << what << ": " << a << " vs " << b << "\n";
    CHECK(false);
  }
}

void roundTripRebuildsMel(double timelineOffsetSeconds) {
  const Project source = makeProject(timelineOffsetSeconds);

  juce::MemoryBlock archive;
  CHECK(ProjectSerializer::toBinaryArchive(
      source, archive, ProjectSerializer::BinaryArchiveMode::selfContained));

  // The mel must genuinely be absent from the bytes, not merely cleared on
  // load: that saving is the whole point of the format change. Bound the
  // archive by everything it legitimately still carries, and show there is no
  // room left in it for a mel spectrogram.
  const auto &sourceAudio = source.getAudioData();
  const size_t melBytes =
      sourceAudio.melSpectrogram.size() * NUM_MELS * sizeof(float);
  CHECK(melBytes > 0);
  const size_t waveformBytes =
      2 * (size_t)sourceAudio.waveform.getNumSamples() * sizeof(float);
  const size_t curveBytes = 7 * (size_t)kFrames * sizeof(float);
  const size_t maskBytes = 2 * (size_t)kFrames;
  const size_t noteBytes = 4 * 30 * sizeof(float);
  const size_t payloadBytes = curveBytes + maskBytes + noteBytes;
  CHECK(archive.getSize() >= payloadBytes);

  // The host-backed archive is the same minus both waveforms, so it bounds the
  // non-audio payload (no room for a mel) and the difference is the encoded
  // audio, which FLAC keeps well under its raw float32 size.
  juce::MemoryBlock sourceless;
  CHECK(ProjectSerializer::toBinaryArchive(
      source, sourceless, ProjectSerializer::BinaryArchiveMode::hostBackedARA));
  CHECK(sourceless.getSize() < payloadBytes + melBytes);
  CHECK(archive.getSize() > sourceless.getSize());
  CHECK(archive.getSize() - sourceless.getSize() < waveformBytes / 2);

  Project restored;
  CHECK(ProjectSerializer::fromBinaryArchive(restored, archive.getData(),
                                              archive.getSize()));

  const auto &a = sourceAudio;
  CHECK(restored.uiWidth == source.uiWidth);
  CHECK(restored.uiHeight == source.uiHeight);
  CHECK(restored.thumbnailVisible == source.thumbnailVisible);
  const auto &b = restored.getAudioData();
  CHECK(b.sampleRate == a.sampleRate);
  CHECK(b.waveform.getNumSamples() == a.waveform.getNumSamples());
  CHECK(b.originalWaveform.getNumSamples() == a.originalWaveform.getNumSamples());
  expectNear(b.waveform.getSample(0, 5000), a.waveform.getSample(0, 5000),
             "rendered waveform");
  CHECK(b.f0.size() == a.f0.size());
  expectNear(b.deltaPitch[3], a.deltaPitch[3], "deltaPitch");
  CHECK(b.voicedMask == a.voicedMask);

  // Rebuilt, same length, same content - including the zero-filled frames in
  // front of the timeline offset.
  CHECK(!b.melSpectrogram.empty());
  CHECK(b.melSpectrogram.size() == a.melSpectrogram.size());
  for (size_t f = 0; f < a.melSpectrogram.size(); ++f) {
    CHECK(b.melSpectrogram[f].size() == (size_t)NUM_MELS);
    for (size_t bin = 0; bin < a.melSpectrogram[f].size(); ++bin)
      // Rebuilt from 24-bit FLAC audio, not the float32 original: bins near
      // the 24-bit noise floor (below ~-100 dB, log value -11.5) may drift.
      if (a.melSpectrogram[f][bin] > -11.5f)
        expectNear(b.melSpectrogram[f][bin], a.melSpectrogram[f][bin], "mel",
                   1.0e-2f);
  }

  CHECK(restored.getNotes().size() == 1);
  const auto &note = restored.getNotes()[0];
  CHECK(note.getStartFrame() == 10 && note.getEndFrame() == 40);
  CHECK(note.getLyric() == "la");
  expectNear(note.getVolumeDb(), -3.0f, "note volume");
  expectNear(note.getDeltaPitch()[0], 0.5f, "note deltaPitch");
  expectNear(note.getBakedDeltaPitch()[0], 0.75f, "note bakedDeltaPitch");
  expectNear(restored.getGlobalPitchOffset(), 2.0f, "global pitch offset");
}

void streamWriterMatchesMemoryBlockWriter() {
  const Project source = makeProject(0.0);

  juce::MemoryBlock viaBlock;
  CHECK(ProjectSerializer::toBinaryArchive(source, viaBlock));

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
    CHECK(ProjectSerializer::toBinaryArchive(source, out));
    archiveEnd = out.getPosition();
    out.setPosition(lengthField);
    out.writeInt64(archiveEnd - archiveStart);
    out.setPosition(archiveEnd);
  }

  CHECK(archiveEnd - archiveStart == (juce::int64)viaBlock.getSize());
  CHECK(std::memcmp(static_cast<const char *>(envelope.getData()) + archiveStart,
                     viaBlock.getData(), viaBlock.getSize()) == 0);

  // And the patched length prefix has to describe the archive that follows it.
  juce::MemoryInputStream in(envelope, false);
  CHECK((std::uint32_t)in.readInt() == 0x504E5053u);
  CHECK(in.readInt() == 1);
  CHECK(in.readInt64() == (juce::int64)viaBlock.getSize());

  Project restored;
  CHECK(ProjectSerializer::fromBinaryArchive(
      restored, static_cast<const char *>(envelope.getData()) + archiveStart,
      viaBlock.getSize()));
  CHECK(restored.getNotes().size() == 1);
  CHECK(!restored.getAudioData().melSpectrogram.empty());
}

void hostBackedArchiveStaysSourceless() {
  const Project source = makeProject(0.0);

  juce::MemoryBlock archive;
  CHECK(ProjectSerializer::toBinaryArchive(
      source, archive, ProjectSerializer::BinaryArchiveMode::hostBackedARA));

  Project restored;
  CHECK(ProjectSerializer::fromBinaryArchive(restored, archive.getData(),
                                              archive.getSize()));

  // No source waveform to rebuild from: the mel stays empty for the ARA
  // hydration path to fill, rather than being invented from nothing.
  const auto &b = restored.getAudioData();
  CHECK(b.originalWaveform.getNumSamples() == 0);
  CHECK(b.waveform.getNumSamples() == 0);
  CHECK(b.melSpectrogram.empty());
  CHECK(b.deltaPitch.size() == (size_t)kFrames);
  CHECK(restored.getNotes().size() == 1);
}

} // namespace

int main() {
  {
    auto shared = std::make_shared<MacroParameters>();
    Project verse, chorus;
    verse.setMacroParameters(shared);
    chorus.setMacroParameters(shared);
    verse.setProjectKey(0, ScaleMode::Major);
    verse.setDetectedKey(9, ScaleMode::Minor);
    CHECK(verse.getUseProjectKey());
    CHECK(verse.getScaleRootNote() == 0);
    chorus.setScaleRootNote(2);
    chorus.setPreferredScaleMode(ScaleMode::Major);
    CHECK(!chorus.getUseProjectKey());
    chorus.setDetectedKey(4, ScaleMode::Minor);
    CHECK(chorus.getScaleRootNote() == 2);
    verse.setProjectKey(7, ScaleMode::Minor);
    CHECK(verse.getScaleRootNote() == 7);
    CHECK(verse.getPreferredScaleMode() == ScaleMode::Minor);
    CHECK(chorus.getScaleRootNote() == 2);
    CHECK(chorus.getPreferredScaleMode() == ScaleMode::Major);
    Project restoredOverride;
    CHECK(ProjectSerializer::fromJson(restoredOverride, ProjectSerializer::toJson(chorus)));
    CHECK(!restoredOverride.getUseProjectKey());
    CHECK(restoredOverride.getScaleRootNote() == 2);
    CHECK(restoredOverride.getProjectKeyRoot() == 7);
    chorus.setUseProjectKey(true);
    CHECK(chorus.getScaleRootNote() == 7);
    Project restoredInherited;
    CHECK(ProjectSerializer::fromJson(restoredInherited, ProjectSerializer::toJson(chorus)));
    CHECK(restoredInherited.getUseProjectKey());
    CHECK(restoredInherited.getScaleRootNote() == 7);
    restoredInherited.setUseProjectKey(false);
    CHECK(restoredInherited.getScaleRootNote() == 7);
    CHECK(restoredInherited.getPreferredScaleMode() == ScaleMode::Minor);
    auto legacy = ProjectSerializer::toJson(chorus);
    legacy.getDynamicObject()->removeProperty("useProjectKey");
    CHECK(ProjectSerializer::fromJson(restoredOverride, legacy));
    CHECK(restoredOverride.getUseProjectKey());
    CHECK(restoredOverride.getScaleRootNote() == chorus.getProjectKeyRoot());
  }

  {
    Project source;
    source.uiWidth = 1440;
    source.uiHeight = 900;
    source.thumbnailVisible = true;
    auto json = ProjectSerializer::toJson(source);
    Project restored;
    CHECK(ProjectSerializer::fromJson(restored, json));
    CHECK(restored.uiWidth == 1440 && restored.uiHeight == 900);
    CHECK(restored.thumbnailVisible);
    json.getDynamicObject()->removeProperty("uiWidth");
    json.getDynamicObject()->removeProperty("uiHeight");
    json.getDynamicObject()->removeProperty("thumbnailVisible");
    CHECK(ProjectSerializer::fromJson(restored, json));
    CHECK(restored.uiWidth == 0 && restored.uiHeight == 0);
    CHECK(!restored.thumbnailVisible);
  }

  roundTripRebuildsMel(0.0);
  roundTripRebuildsMel(0.25);
  streamWriterMatchesMemoryBlockWriter();
  hostBackedArchiveStaysSourceless();
  std::cout << "PluginStateArchiveTests passed\n";
  return 0;
}
