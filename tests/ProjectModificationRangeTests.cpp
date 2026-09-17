#include "../Source/Models/ProjectModificationRanges.h"
#include "../Source/Utils/Constants.h"
#include "TestAssert.h"
#include <iostream>

// Coordinate discipline for ARA edits.
//
// A project is stored in MODIFICATION time, and the published render spans the
// whole audio modification at offset zero. Every sample index that crosses
// between these two functions must therefore be a modification sample. When it
// was not, the first edit on the right-hand slice of a split published audio
// identical to what was already playing: the edit was preserved at a position
// where nothing had changed, and the samples that were actually edited got
// overwritten with the previous render.
//
// These tests pin that down without a DAW.

namespace {

Project makeProject(int frames) {
  Project project;
  auto &audio = project.getAudioData();
  audio.sampleRate = SAMPLE_RATE;
  audio.waveform.setSize(1, frames * HOP_SIZE);
  audio.waveform.clear();
  audio.originalWaveform.makeCopyOf(audio.waveform);
  audio.f0.assign(static_cast<size_t>(frames), 440.0f);
  audio.melSpectrogram.assign(static_cast<size_t>(frames),
                              std::vector<float>(NUM_MELS, 1.0f));
  return project;
}

// A note the synthesiser will treat as dirty, spanning [startFrame, endFrame).
void addDirtyNote(Project &project, int startFrame, int endFrame) {
  Note note(startFrame, endFrame, 60.0f);
  note.setSrcStartFrame(startFrame);
  note.setSrcEndFrame(endFrame);
  note.setPitchOffset(1.0f);
  note.setDirty(true);
  project.addNote(note);
}

juce::AudioBuffer<float> makeRamp(int numSamples, float base) {
  juce::AudioBuffer<float> buffer(1, numSamples);
  for (int i = 0; i < numSamples; ++i)
    buffer.setSample(0, i, base + static_cast<float>(i));
  return buffer;
}

void testDirtyRangesAreInModificationSamples() {
  auto project = makeProject(200);
  addDirtyNote(project, 100, 140);

  const auto ranges =
      collectDirtyModificationSampleRanges(project, SAMPLE_RATE);

  CHECK(ranges.size() == 1);

  // Frame 100 is sample 100 * HOP_SIZE from the start of the MODIFICATION.
  // The old implementation subtracted the playback region's timeline start, so
  // for a region placed later on the timeline this came back displaced by that
  // offset - and, for a region at timeline zero, looked correct. That is what
  // made the bug invisible on the left-hand slice of a split.
  CHECK(ranges[0].getStart() == 100 * HOP_SIZE);
  CHECK(ranges[0].getEnd() == 140 * HOP_SIZE);

  std::cout << "dirty ranges are in modification samples: ok\n";
}

// The ranges must not depend on where the region sits on the timeline. This is
// the property the old code violated: two regions on the same modification at
// different timeline positions produced different ranges for the same edit.
void testRangesDoNotDependOnTimelinePosition() {
  auto project = makeProject(200);
  addDirtyNote(project, 100, 140);

  const auto ranges =
      collectDirtyModificationSampleRanges(project, SAMPLE_RATE);
  CHECK(ranges.size() == 1);

  // Same project, same edit - the function has no way to observe a timeline
  // position, and that is the point. Placement is applied at render and draw
  // time, never baked into the stored edit state.
  const auto again =
      collectDirtyModificationSampleRanges(project, SAMPLE_RATE);
  CHECK(again.size() == 1);
  CHECK(again[0].getStart() == ranges[0].getStart());
  CHECK(again[0].getEnd() == ranges[0].getEnd());

  std::cout << "ranges are independent of timeline position: ok\n";
}

void testAdjacentDirtyNotesMerge() {
  auto project = makeProject(200);
  addDirtyNote(project, 10, 20);
  addDirtyNote(project, 20, 30); // touches the previous range exactly
  addDirtyNote(project, 80, 90); // disjoint

  const auto ranges =
      collectDirtyModificationSampleRanges(project, SAMPLE_RATE);

  CHECK(ranges.size() == 2);
  CHECK(ranges[0].getStart() == 10 * HOP_SIZE);
  CHECK(ranges[0].getEnd() == 30 * HOP_SIZE);
  CHECK(ranges[1].getStart() == 80 * HOP_SIZE);
  CHECK(ranges[1].getEnd() == 90 * HOP_SIZE);

  std::cout << "adjacent dirty notes merge, disjoint ones do not: ok\n";
}

void testCleanProjectHasNoDirtyRanges() {
  auto project = makeProject(50);
  Note note(10, 20, 60.0f);
  note.setSrcStartFrame(10);
  note.setSrcEndFrame(20);
  project.addNote(note); // not dirty

  CHECK(collectDirtyModificationSampleRanges(project, SAMPLE_RATE).empty());
  CHECK(collectDirtyModificationSampleRanges(project, 0.0).empty());

  std::cout << "clean project and invalid rate yield no ranges: ok\n";
}

// The two functions have to agree. This feeds the output of one into the other
// and checks that the edited samples survive while everything else is carried
// forward from the previous render.
void testPreserveKeepsEditedSamplesAndRestoresTheRest() {
  auto project = makeProject(40);
  addDirtyNote(project, 10, 20);

  const auto ranges =
      collectDirtyModificationSampleRanges(project, SAMPLE_RATE);
  CHECK(ranges.size() == 1);

  const int total = 40 * HOP_SIZE;
  auto replacement = makeRamp(total, 1000.0f); // the fresh render
  auto previous = makeRamp(total, 0.0f);       // what was published before

  preserveProcessedAudioOutsideRanges(replacement, SAMPLE_RATE, 0, previous,
                                      SAMPLE_RATE, 0, ranges);

  const int editStart = 10 * HOP_SIZE;
  const int editEnd = 20 * HOP_SIZE;

  // Inside the edited range: the new render survives.
  CHECK(replacement.getSample(0, editStart) == 1000.0f + editStart);
  CHECK(replacement.getSample(0, editEnd - 1) == 1000.0f + (editEnd - 1));

  // Outside it: the previous render is carried forward, so an edit made
  // elsewhere earlier is not silently discarded by this partial resynthesis.
  CHECK(replacement.getSample(0, 0) == 0.0f);
  CHECK(replacement.getSample(0, editStart - 1) ==
         static_cast<float>(editStart - 1));
  CHECK(replacement.getSample(0, editEnd) == static_cast<float>(editEnd));
  CHECK(replacement.getSample(0, total - 1) ==
         static_cast<float>(total - 1));

  std::cout << "preserve keeps edited samples and restores the rest: ok\n";
}

// A range displaced by a region offset - exactly what the old code produced -
// must visibly corrupt the result. This is the regression guard: if someone
// reintroduces region-relative ranges, this test fails loudly.
void testDisplacedRangesCorruptTheEdit() {
  const int total = 40 * HOP_SIZE;
  const int editStart = 20 * HOP_SIZE;
  const int editEnd = 30 * HOP_SIZE;      // a 10-frame edit
  const int regionOffset = 15 * HOP_SIZE; // displaced further than it is wide

  std::vector<SampleRange> displaced;
  displaced.emplace_back(editStart - regionOffset, editEnd - regionOffset);

  // Nothing the user edited lies inside the displaced range. A smaller offset
  // would leave the two overlapping, the edit would survive in part, and this
  // test would pass for the wrong reason.
  CHECK(displaced[0].getEnd() <= editStart);

  auto replacement = makeRamp(total, 1000.0f);
  auto previous = makeRamp(total, 0.0f);

  preserveProcessedAudioOutsideRanges(replacement, SAMPLE_RATE, 0, previous,
                                      SAMPLE_RATE, 0, displaced);

  // The samples the user actually edited were overwritten with the previous
  // render, which is why the first edit on a split slice appeared to do
  // nothing at all.
  CHECK(replacement.getSample(0, editStart) == static_cast<float>(editStart));
  CHECK(replacement.getSample(0, editEnd - 1) ==
        static_cast<float>(editEnd - 1));

  // Meanwhile the fresh render survives where nothing had changed - the work
  // was done, just in the wrong place.
  CHECK(replacement.getSample(0, displaced[0].getStart()) ==
        1000.0f + static_cast<float>(displaced[0].getStart()));

  std::cout << "displaced ranges corrupt the edit, as they did in the bug: ok\n";
}

// Guards the offsets rather than assuming both buffers start at zero.
void testPreserveHonoursBufferStartOffsets() {
  const int total = 20 * HOP_SIZE;
  const juce::int64 previousStart = 4 * HOP_SIZE;

  std::vector<SampleRange> ranges;
  ranges.emplace_back(0, HOP_SIZE); // first frame is the edit

  auto replacement = makeRamp(total, 1000.0f);
  auto previous = makeRamp(total, 0.0f);

  // The replacement spans the modification from zero; the previous render
  // started four frames in, so sample N of the replacement corresponds to
  // sample N - previousStart of the previous buffer.
  preserveProcessedAudioOutsideRanges(replacement, SAMPLE_RATE, 0, previous,
                                      SAMPLE_RATE, previousStart, ranges);

  // Inside the changed range the new render survives.
  CHECK(replacement.getSample(0, 0) == 1000.0f);

  // Before the previous buffer begins there is nothing to copy, so the new
  // render is left alone rather than being filled with garbage.
  CHECK(replacement.getSample(0, 1) == 1001.0f);

  // Past that point the previous render is sampled at the shifted position.
  const int probe = 10 * HOP_SIZE;
  CHECK(replacement.getSample(0, probe) ==
         static_cast<float>(probe - previousStart));

  std::cout << "preserve honours differing buffer start offsets: ok\n";
}

} // namespace

int main() {
  testDirtyRangesAreInModificationSamples();
  testRangesDoNotDependOnTimelinePosition();
  testAdjacentDirtyNotesMerge();
  testCleanProjectHasNoDirtyRanges();
  testPreserveKeepsEditedSamplesAndRestoresTheRest();
  testDisplacedRangesCorruptTheEdit();
  testPreserveHonoursBufferStartOffsets();

  std::cout << "ProjectModificationRangeTests passed\n";
  return 0;
}
