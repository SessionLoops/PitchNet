#include "../Source/Models/ProjectRegionSlice.h"
#include "../Source/Models/ProjectSerializer.h"
#include <cassert>
#include <iostream>

int main() {
  Project original;
  auto &audio = original.getAudioData();
  audio.sampleRate = SAMPLE_RATE;
  audio.waveform.setSize(1, 20 * HOP_SIZE);
  for (int i = 0; i < audio.waveform.getNumSamples(); ++i)
    audio.waveform.setSample(0, i, static_cast<float>(i));
  audio.originalWaveform.makeCopyOf(audio.waveform);
  audio.f0.assign(20, 440.0f);
  audio.melSpectrogram.assign(20, std::vector<float>(NUM_MELS, 1.0f));
  original.setPitchCenter(65.0f);
  Note note(4, 16, 67.0f);
  note.setSrcStartFrame(2);
  note.setSrcEndFrame(20); // A time-edited note must retain proportional source bounds.
  note.setPitchOffset(2.0f);
  note.setVolumeDb(-3.0f);
  note.setLyric("word");
  note.setPhoneme("w");
  note.setVibrato(0.5f);
  note.setTiltLeft(1.5f);
  note.setRenderedEdit(true);
  std::vector<float> curve;
  for (int i = 0; i < 12; ++i) curve.push_back(static_cast<float>(i));
  note.setDeltaPitch(curve);
  note.setOriginalDeltaPitch(curve);
  note.setBakedDeltaPitch(curve);
  note.setF0Values(curve);
  original.addNote(note);
  original.addNote(Note(0, 4, 60.0f));
  original.addNote(Note(16, 20, 72.0f));

  Project left = original, right = original;
  // Offset inside the sample gives stable floor-to-frame conversion.
  const double cut = (10.0 * HOP_SIZE + 0.25) / SAMPLE_RATE;
  clipProjectToPlaybackRange(left, 0.0, cut);
  clipProjectToPlaybackRange(right, cut, 20.0 * HOP_SIZE / SAMPLE_RATE);
  assert(left.getNotes().size() == 2 && right.getNotes().size() == 2);
  const auto &l = *left.getNoteAtFrame(5), &r = *right.getNoteAtFrame(11);
  assert(l.getStartFrame() == 4 && l.getEndFrame() == 10);
  assert(r.getStartFrame() == 10 && r.getEndFrame() == 16);
  assert(l.getSrcEndFrame() == r.getSrcStartFrame());
  assert(l.getSrcStartFrame() == 2 && r.getSrcEndFrame() == 20);
  assert(l.getDeltaPitch().size() == 6 && r.getDeltaPitch().size() == 6);
  assert(l.getDeltaPitch().back() == 5 && r.getDeltaPitch().front() == 6);
  for (const auto *part : {&l, &r}) {
    assert(part->getMidiNote() == 67 && part->getPitchOffset() == 2);
    assert(part->getVolumeDb() == -3 && part->getLyric() == "word");
    assert(part->getPhoneme() == "w" && part->getVibrato() == 0.5f);
    assert(part->getTiltLeft() == 1.5f && part->hasRenderedEdit());
    assert(part->getOriginalDeltaPitch() == part->getDeltaPitch());
    assert(part->getBakedDeltaPitch() == part->getDeltaPitch());
    assert(part->getF0Values() == part->getDeltaPitch());
  }
  assert(left.getAudioData().waveform.getNumSamples() == 10 * HOP_SIZE);
  assert(right.getAudioData().waveform.getSample(0, 10 * HOP_SIZE) == 10 * HOP_SIZE);
  assert(right.getAudioData().waveform.getSample(0, 0) == 0);
  assert(right.getAudioData().f0 == audio.f0);
  assert(right.getAudioData().melSpectrogram == audio.melSpectrogram);
  assert(right.getPitchCenter() == 65);
  assert(original.getNoteAtFrame(5)->getEndFrame() == 16);
  assert(original.getAudioData().waveform.getNumSamples() == 20 * HOP_SIZE);
  // Editor-closed restore uses archives without waveforms/mel. Both halves,
  // especially the surviving left identity, must retain their new bounds and
  // edits independently of hydration from the shorter host region.
  for (const auto *part : {&left, &right}) {
    juce::MemoryBlock archive;
    assert(ProjectSerializer::toBinaryArchive(*part, archive,
        ProjectSerializer::BinaryArchiveMode::hostBackedARA));
    Project reopened;
    assert(ProjectSerializer::fromBinaryArchive(reopened, archive.getData(), archive.getSize()));
    assert(reopened.getAudioData().waveform.getNumSamples() == 0);
    assert(reopened.getAudioData().melSpectrogram.empty());
    assert(reopened.getAudioData().playbackRegionRanges.size() == 1);
    const auto actual = reopened.getAudioData().playbackRegionRanges.front();
    const auto expected = part->getAudioData().playbackRegionRanges.front();
    // Timeline metadata is serialized through JSON decimal numbers.
    assert(std::abs(actual.first - expected.first) < 1.0e-8);
    assert(std::abs(actual.second - expected.second) < 1.0e-8);
    assert(reopened.getAudioData().f0 == part->getAudioData().f0);
    assert(reopened.getNotes().size() == part->getNotes().size());
    const auto *restored = reopened.getNoteAtFrame(part == &left ? 5 : 11);
    assert(restored && restored->getLyric() == "word");
    assert(restored->getPitchOffset() == 2 && restored->getVolumeDb() == -3);
    assert(restored->getBakedDeltaPitch() == (part == &left ? l : r).getBakedDeltaPitch());
    assert(restored->getEndFrame() == (part == &left ? 10 : 16));
  }
  std::cout << "ARA region slice tests passed\n";
}
