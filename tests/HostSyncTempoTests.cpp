// Run with JUCE core/events, JUCE_MODAL_LOOPS_PERMITTED=1 and HostSyncService.cpp.
#include "../Source/Audio/Engine/HostSyncService.h"
#include <cassert>
#include <iostream>

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    HostSyncService sync;
    int notifications = 0;
    HostSyncService::TempoInfo delivered;
    sync.setTempoCallback([&](const auto& tempo) {
        ++notifications;
        delivered = tempo;
    });
    const auto drain = [] {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    };

    // Even initial default-valued metadata must notify the editor.
    juce::AudioPlayHead::PositionInfo info;
    info.setBpm(120.0);
    sync.updateFromPositionInfo(info, 48000.0);
    drain();
    assert(notifications == 1 && delivered.hasBpm);
    assert(!delivered.hasTimeSignature);

    // Rapid changes must deliver the final tempo, not the first queued value.
    info.setBpm(140.0);
    sync.updateFromPositionInfo(info, 48000.0);
    info.setBpm(172.0);
    sync.updateFromPositionInfo(info, 48000.0);
    info.setTimeSignature(juce::AudioPlayHead::TimeSignature{3, 4});
    info.setIsPlaying(true);
    info.setIsRecording(true);
    sync.updateFromPositionInfo(info, 48000.0);
    drain();
    assert(notifications == 2);
    assert(delivered.bpm == 172.0 && delivered.hasTimeSignature);
    assert(delivered.timeSigNumerator == 3);

    // Stopping with omitted metadata must retain the recording's tempo/meter.
    juce::AudioPlayHead::PositionInfo stopped;
    sync.updateFromPositionInfo(stopped, 48000.0);
    drain();
    auto tempo = sync.getCurrentState().tempo;
    assert(tempo.hasBpm && tempo.bpm == 172.0);
    assert(tempo.hasTimeSignature && tempo.timeSigNumerator == 3);
    assert(notifications == 2);

    // Tempo-only changes while stopped still preserve the known meter.
    stopped.setBpm(95.0);
    sync.updateFromPositionInfo(stopped, 48000.0);
    drain();
    assert(notifications == 3 && delivered.bpm == 95.0);
    assert(delivered.timeSigNumerator == 3);

    stopped.setBpm(0.0);
    stopped.setTimeSignature(juce::AudioPlayHead::TimeSignature{0, 0});
    sync.updateFromPositionInfo(stopped, 48000.0);
    assert(sync.getCurrentState().tempo.bpm == 95.0);
    assert(sync.getCurrentState().tempo.timeSigNumerator == 3);
    std::cout << "Host tempo sync regressions passed\n";
}
