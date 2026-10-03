#include "../Source/Audio/RealtimePitchProcessor.h"
#include "../Source/Plugin/NonAraCaptureController.h"
#include "TestAssert.h"
#include <cmath>
#include <iostream>

namespace {
void captureStorageIsAllocatedOnlyWhenArmed() {
  NonAraCaptureController capture;
  capture.setMinCaptureSeconds(0.0);
  capture.prepare(48000.0, 2, 1);
  CHECK(capture.getState() == NonAraCaptureController::State::Idle);
  CHECK(capture.copyCapturedAudio(64).getNumSamples() == 0);

  capture.resetToWaiting();
  juce::AudioBuffer<float> input(2, 64);
  for (int i = 0; i < 64; ++i) {
    input.setSample(0, i, 0.25f);
    input.setSample(1, i, -0.5f);
  }
  capture.processBlock(input, true);
  capture.processBlock(input, false);
  CHECK(capture.shouldFinalize());
  NonAraCaptureController::FinalizeResult result;
  CHECK(capture.finalizeCapture(48000.0, result));
  CHECK(result.numSamples == 64 && result.numChannels == 2);
  auto recorded = capture.copyCapturedAudio(result.numSamples);
  CHECK(recorded.getSample(0, 63) == 0.25f);
  CHECK(recorded.getSample(1, 63) == -0.5f);
  capture.onAnalysisDispatched();

  // Re-arm after a host rate/layout change, and ensure the previous take is
  // neither retained as capture data nor mixed into a new silent recording.
  capture.prepare(44100.0, 1, 1);
  CHECK(capture.copyCapturedAudio(64).getNumSamples() == 0);
  capture.resetToWaiting();
  input.clear();
  capture.processBlock(input, true);
  capture.processBlock(input, false);
  CHECK(capture.finalizeCapture(44100.0, result));
  CHECK(result.numSamples == 64 && result.numChannels == 1);
  recorded = capture.copyCapturedAudio(result.numSamples);
  CHECK(recorded.getMagnitude(0, 64) == 0.0f);
}

void capturePreservesTimelineAndWaitsForStop() {
  NonAraCaptureController capture;
  capture.prepare(1000.0, 1, 1);
  capture.setMinCaptureSeconds(0.0);
  juce::AudioBuffer<float> input(1, 100);
  for (int i = 0; i < 100; ++i)
    input.setSample(0, i, 0.5f);
  capture.resetToWaiting();
  capture.processBlock(input, true, 2000);
  capture.processBlock(input, true, 2300); // two missing blocks
  CHECK(capture.getCapturedSampleCount() == 400);
  auto recorded = capture.copyCapturedAudio(400);
  CHECK(recorded.getSample(0, 99) == 0.5f);
  CHECK(recorded.getMagnitude(0, 100, 200) == 0.0f);
  CHECK(recorded.getSample(0, 300) == 0.5f);
  capture.processBlock(input, true, 2000); // loop back
  CHECK(capture.getHoldReason() == NonAraCaptureController::HoldReason::TimelineJump);
  CHECK(capture.getCapturedSampleCount() == 400);
  CHECK(!capture.shouldFinalize());
  capture.processBlock(input, false);
  NonAraCaptureController::FinalizeResult result;
  CHECK(capture.shouldFinalize());
  CHECK(capture.finalizeCapture(1000.0, result));
  CHECK(result.numSamples == 400);
  capture.onAnalysisDispatched();

  capture.resetToWaiting();
  CHECK(capture.getHoldReason() == NonAraCaptureController::HoldReason::None);
  for (int i = 0; i < 12; ++i)
    capture.processBlock(input, true, i * 100);
  CHECK(capture.getCapturedSampleCount() == 1000);
  CHECK(capture.getHoldReason() == NonAraCaptureController::HoldReason::Capacity);
  CHECK(!capture.shouldFinalize());
  capture.processBlock(input, false);
  CHECK(capture.shouldFinalize());
  CHECK(capture.finalizeCapture(1000.0, result));
  CHECK(result.numSamples == 1000);
  capture.onAnalysisDispatched();

  capture.resetToWaiting();
  capture.processBlock(input, true, 10000);
  capture.processBlock(input, true, 20000); // gap exceeds storage
  CHECK(capture.getCapturedSampleCount() == 1000);
  CHECK(capture.getHoldReason() == NonAraCaptureController::HoldReason::Capacity);
  CHECK(!capture.shouldFinalize());

  capture.resetToWaiting();
  capture.processBlock(input, true); // hosts without sample positions
  capture.processBlock(input, true);
  CHECK(capture.getCapturedSampleCount() == 200);
  CHECK(capture.getHoldReason() == NonAraCaptureController::HoldReason::None);
}

void playbackHonorsSmallHostSeeks() {
  Project project;
  auto &audio = project.getAudioData();
  audio.sampleRate = 44100;
  audio.waveform.setSize(1, 1000);
  for (int i = 0; i < 1000; ++i)
    audio.waveform.setSample(0, i, static_cast<float>(i) / 1000.0f);
  RealtimePitchProcessor processor;
  processor.prepareToPlay(44100.0, 64);
  processor.setProject(&project);
  processor.waitForPendingUpdate();
  juce::AudioBuffer<float> input(1, 64), output(1, 64);
  input.clear();
  juce::AudioPlayHead::PositionInfo position;
  position.setIsPlaying(true);
  position.setTimeInSamples(100);
  CHECK(processor.processBlock(input, output, &position));
  position.setTimeInSamples(132); // backwards seek smaller than a block
  CHECK(processor.processBlock(input, output, &position));
  CHECK(std::abs(output.getSample(0, 0) - 0.132f) < 0.00001f);
  position.setTimeInSamples(228); // forward seek smaller than a block
  CHECK(processor.processBlock(input, output, &position));
  CHECK(std::abs(output.getSample(0, 0) - 0.228f) < 0.00001f);
}

Project makeConstantProject(float value) {
  Project project;
  auto &audio = project.getAudioData();
  audio.sampleRate = 44100;
  audio.waveform.setSize(1, 44100);
  for (int i = 0; i < 44100; ++i)
    audio.waveform.setSample(0, i, value);
  return project;
}

void expectPlayback(RealtimePitchProcessor &processor, int sample, float value) {
  juce::AudioBuffer<float> input(1, 64), output(1, 64);
  input.clear();
  juce::AudioPlayHead::PositionInfo position;
  position.setTimeInSamples(sample);
  position.setIsPlaying(false);
  CHECK(processor.processBlock(input, output, &position));
  for (int i = 0; i < 64; ++i)
    CHECK(std::abs(output.getSample(0, i) - value) < 0.001f);
}

void lifecycleCompletesPendingCacheAndDropsDetachedAudio() {
  auto original = makeConstantProject(0.25f);
  RealtimePitchProcessor processor;
  processor.setProject(&original);
  processor.prepareToPlay(48000.0, 64);
  processor.waitForPendingUpdate();
  CHECK(processor.isReady());
  // Near the end of the host-rate cache, beyond the source-rate length.
  expectPlayback(processor, 46000, 0.25f);

  // Detach while another resampling job may still be running. It must never
  // republish the old project's audio after the binding has been cleared.
  processor.invalidate();
  processor.setProject(nullptr);
  processor.waitForPendingUpdate();
  CHECK(!processor.isReady());
  juce::AudioBuffer<float> dry(1, 64), output(1, 64);
  dry.clear();
  dry.setSample(0, 0, 0.75f);
  CHECK(!processor.processBlock(dry, output, nullptr));
  CHECK(output.getSample(0, 0) == 0.75f);

  auto replacement = makeConstantProject(-0.5f);
  processor.setProject(&replacement);
  processor.waitForPendingUpdate();
  expectPlayback(processor, 1000, -0.5f);

  // A new host rate must not blend samples from the old-rate cache into the
  // start of an offline render, even when prepare is called repeatedly.
  processor.prepareToPlay(96000.0, 64);
  processor.prepareToPlay(44100.0, 64);
  processor.waitForPendingUpdate();
  expectPlayback(processor, 1000, -0.5f);
  processor.waitForPendingUpdate(); // safe when there is no worker

  Project empty;
  processor.setProject(&empty);
  CHECK(!processor.isReady());
}
} // namespace

int main() {
  captureStorageIsAllocatedOnlyWhenArmed();
  capturePreservesTimelineAndWaitsForStop();
  playbackHonorsSmallHostSeeks();
  lifecycleCompletesPendingCacheAndDropsDetachedAudio();
  std::cout << "Non-ARA lifecycle tests passed\n";
}
