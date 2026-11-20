#pragma once

#include <vector>

/**
 * Tempo and downbeat estimation for a polyphonic take, from its note onsets.
 *
 * The engine already knows where every note starts, so no onset detector is
 * needed: the onsets are clustered (a chord is one onset, weighted more than a
 * single note) and each candidate beat period P is scored by how strongly the
 * onsets agree on a phase - the length of the mean of exp(2 pi i t / P)
 * (a Rayleigh test; cf. the "circular" beat-phase methods). The argument of
 * that mean is the beat phase. Every whole fraction of the true period scores
 * well too, so the score also counts the half and double period, and a
 * log-normal prior around 120 BPM picks between octaves (as Ellis 2007 does
 * for tempo from autocorrelation). The downbeat is the beat of the bar that
 * the strongest, lowest onsets favour.
 *
 * A take played in free time has no tempo to find; `found` is false then and
 * the caller keeps the tempo it has.
 */
namespace polyphonic
{
struct Analysis;

struct TempoOnset
{
    double seconds = 0.0;
    double weight = 1.0; // how strong the attack is (> 0)
    double bass = 0.0;   // 0..1, how low the note is (downbeats favour bass notes)
};

struct TempoEstimate
{
    bool found = false;
    double bpm = 0.0;              // beats (quarter notes for a /4 signature) per minute
    double downbeatSeconds = 0.0;  // the first downbeat at or after 0, in [0, bar)
    double confidence = 0.0;       // phase agreement at the chosen tempo, 0..1
};

struct TempoOptions
{
    int beatsPerBar = 4;
    double minBpm = 60.0;
    double maxBpm = 180.0;
};

TempoEstimate estimateTempo(const std::vector<TempoOnset>& onsets, const TempoOptions& options = {});

/** The take's active notes as onsets (strength from their energy, bass from their pitch). */
std::vector<TempoOnset> tempoOnsets(const Analysis& analysis);

inline TempoEstimate estimateTempo(const Analysis& analysis, const TempoOptions& options = {})
{
    return estimateTempo(tempoOnsets(analysis), options);
}
} // namespace polyphonic
