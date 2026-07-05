#pragma once

#include "../JuceHeader.h"
#include "GPUProvider.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

#ifdef HAVE_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#ifdef USE_DIRECTML
#include <dml_provider_factory.h>
#endif
#endif

/**
 * GAME (General Audio-to-MIDI Estimation) note detector.
 *
 * Pipeline (from ONNX.md):
 *   encoder  → x_seg, x_est, maskT
 *   segmenter (D3PM loop) → boundaries
 *   bd2dur   → durations
 *   estimator → presence, scores, maskN
 *
 * Output: list of NoteEvents with frame positions, MIDI pitch, voiced flag.
 */
class GAMEDetector
{
public:
    static constexpr int SAMPLE_RATE = 44100;
    static constexpr int HOP_SIZE = 512;

    struct NoteEvent
    {
        int startFrame; // system frame index (HOP_SIZE=512 based)
        int endFrame;
        float midiNote; // MIDI semitone (A4 = 69)
        bool isRest;    // true = unvoiced/rest
    };

    GAMEDetector();
    ~GAMEDetector();

    bool loadModels(const juce::File &gameDir,
                    GPUProvider provider = GPUProvider::CPU,
                    int deviceId = 0);
    bool isLoaded() const { return loaded; }

    std::vector<NoteEvent> detectNotes(const float *audio, int numSamples,
                                       int sampleRate);
    std::vector<NoteEvent>
    detectNotesWithProgress(const float *audio, int numSamples, int sampleRate,
                            std::function<void(double)> progressCallback);

    void setNumD3PMSteps(int steps) { numD3PMSteps = steps; }
    void setSegThreshold(float t) { segThreshold = t; }
    void setSegRadius(int r) { segRadius = r; }
    void setEstThreshold(float t) { estThreshold = t; }

    int getFrameForSample(int sampleIndex) const
    {
        return sampleIndex / HOP_SIZE;
    }
    int getSampleForFrame(int frameIndex) const
    {
        return frameIndex * HOP_SIZE;
    }

    // Chunk ranges from the last detectNotes call (sample-based, model sample rate)
    struct ChunkRange
    {
        int startSample;
        int endSample;
    };
    const std::vector<ChunkRange> &getLastChunkRanges() const { return lastChunkRanges; }

private:
    bool loaded = false;

    // D3PM and inference parameters
    int numD3PMSteps = 2;
    float segThreshold = 0.2f;
    int segRadius = 2;
    float estThreshold = 0.2f;

    // Config from model's config.json
    int modelSampleRate = 44100;
    float timestep = 0.01f;
    bool supportsLoop = true;
    int embeddingDim = 256;

    bool loadConfig(const juce::File &configPath);
    std::vector<float> resampleToModelRate(const float *audio, int numSamples,
                                           int srcRate);

    // Encoder max T=5000 frames → max ~50s per chunk
    static constexpr int MAX_ENCODER_FRAMES = 5000;
    // Samples per encoder frame (sampleRate * timestep = 441 at 44100/0.01)
    int samplesPerEncoderFrame() const { return static_cast<int>(modelSampleRate * timestep); }
    int maxChunkSamples() const { return MAX_ENCODER_FRAMES * samplesPerEncoderFrame(); }

    // RMS-based silence detection for chunking
    std::vector<ChunkRange> findSilenceChunks(const std::vector<float> &waveform) const;
    std::vector<ChunkRange> lastChunkRanges;

    // Run full pipeline on a single chunk, return NoteEvents with frame offsets relative to chunk start
    std::vector<NoteEvent> processChunk(const std::vector<float> &waveform, int chunkStartSample,
                                        std::function<void(double)> progressCallback,
                                        double progressBase, double progressSpan);

#ifdef HAVE_ONNXRUNTIME
    struct ModelSession
    {
        std::unique_ptr<Ort::Session> session;
        std::vector<std::string> inputNameStrings;
        std::vector<std::string> outputNameStrings;
        std::vector<const char *> inputNames;
        std::vector<const char *> outputNames;

        bool load(Ort::Env &env, const juce::File &path,
                  const Ort::SessionOptions &options);
        int findInput(const std::string &name) const;
        int findOutput(const std::string &name) const;
    };

    std::unique_ptr<Ort::Env> onnxEnv;
    ModelSession encoder;
    ModelSession segmenter;
    ModelSession estimator;
    ModelSession bd2dur;

    Ort::SessionOptions
    createSessionOptions(GPUProvider provider, int deviceId);
#endif
};
