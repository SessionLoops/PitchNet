#include "GAMEDetector.h"
#include "../Utils/AppLogger.h"
#include "../Utils/Localization.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <juce_core/juce_core.h>
#include <numeric>

GAMEDetector::GAMEDetector() = default;
GAMEDetector::~GAMEDetector() = default;

bool GAMEDetector::loadConfig(const juce::File &configPath)
{
    if (!configPath.existsAsFile())
        return false;

    auto jsonText = configPath.loadFileAsString();
    auto parsed = juce::JSON::parse(jsonText);
    if (parsed.isVoid())
        return false;

    if (auto *obj = parsed.getDynamicObject())
    {
        modelSampleRate =
            static_cast<int>(obj->getProperty("samplerate"));
        timestep = static_cast<float>(
            static_cast<double>(obj->getProperty("timestep")));
        supportsLoop = static_cast<bool>(obj->getProperty("loop"));
        embeddingDim =
            static_cast<int>(obj->getProperty("embedding_dim"));
    }

    LOG("GAME config: sr=" + juce::String(modelSampleRate) +
        " timestep=" + juce::String(timestep) +
        " loop=" + juce::String(supportsLoop ? "true" : "false") +
        " dim=" + juce::String(embeddingDim));
    return true;
}

std::vector<float> GAMEDetector::resampleToModelRate(const float *audio,
                                                     int numSamples,
                                                     int srcRate)
{
    if (srcRate == modelSampleRate)
        return std::vector<float>(audio, audio + numSamples);

    double ratio = static_cast<double>(modelSampleRate) / srcRate;
    int outSamples = static_cast<int>(numSamples * ratio);
    std::vector<float> resampled(outSamples);

    for (int i = 0; i < outSamples; ++i)
    {
        double srcPos = i / ratio;
        int srcIdx = static_cast<int>(srcPos);
        double frac = srcPos - srcIdx;

        if (srcIdx + 1 < numSamples)
            resampled[i] = static_cast<float>(audio[srcIdx] * (1.0 - frac) +
                                              audio[srcIdx + 1] * frac);
        else if (srcIdx < numSamples)
            resampled[i] = audio[srcIdx];
    }
    return resampled;
}

#ifdef HAVE_ONNXRUNTIME

// ─── ModelSession helpers ───────────────────────────────────────────

bool GAMEDetector::ModelSession::load(Ort::Env &env, const juce::File &path,
                                      const Ort::SessionOptions &options)
{
    try
    {
#ifdef _WIN32
        std::wstring modelPathW = path.getFullPathName().toWideCharPointer();
        session =
            std::make_unique<Ort::Session>(env, modelPathW.c_str(), options);
#else
        std::string modelPathStr = path.getFullPathName().toStdString();
        session =
            std::make_unique<Ort::Session>(env, modelPathStr.c_str(), options);
#endif

        Ort::AllocatorWithDefaultOptions allocator;

        inputNameStrings.clear();
        outputNameStrings.clear();
        inputNames.clear();
        outputNames.clear();

        size_t numInputs = session->GetInputCount();
        size_t numOutputs = session->GetOutputCount();

        for (size_t i = 0; i < numInputs; ++i)
        {
            auto namePtr = session->GetInputNameAllocated(i, allocator);
            inputNameStrings.push_back(namePtr.get());
        }
        for (size_t i = 0; i < numOutputs; ++i)
        {
            auto namePtr = session->GetOutputNameAllocated(i, allocator);
            outputNameStrings.push_back(namePtr.get());
        }

        for (const auto &name : inputNameStrings)
            inputNames.push_back(name.c_str());
        for (const auto &name : outputNameStrings)
            outputNames.push_back(name.c_str());

        return true;
    }
    catch (const Ort::Exception &e)
    {
        LOG("ONNX load failed for " + path.getFileName() + ": " +
            juce::String(e.what()));
        session.reset();
        return false;
    }
}

int GAMEDetector::ModelSession::findInput(const std::string &name) const
{
    for (size_t i = 0; i < inputNameStrings.size(); ++i)
    {
        if (inputNameStrings[i] == name)
            return static_cast<int>(i);
    }
    return -1;
}

int GAMEDetector::ModelSession::findOutput(const std::string &name) const
{
    for (size_t i = 0; i < outputNameStrings.size(); ++i)
    {
        if (outputNameStrings[i] == name)
            return static_cast<int>(i);
    }
    return -1;
}

Ort::SessionOptions
GAMEDetector::createSessionOptions(GPUProvider provider, int deviceId)
{
    Ort::SessionOptions options;

    options.SetGraphOptimizationLevel(
        GraphOptimizationLevel::ORT_ENABLE_ALL);
    options.EnableCpuMemArena();

    if (provider == GPUProvider::CPU)
    {
        const int numThreads =
            std::max(1u, std::thread::hardware_concurrency()) / 2;
        options.SetIntraOpNumThreads(std::max(numThreads, 2));
        options.EnableMemPattern();
    }
    else
    {
        options.SetIntraOpNumThreads(1);
        options.SetInterOpNumThreads(1);
    }

#if defined(_WIN32) && defined(USE_DIRECTML)
    if (provider == GPUProvider::DirectML)
    {
        try
        {
            const OrtApi &ortApi = Ort::GetApi();
            const OrtDmlApi *ortDmlApi = nullptr;
            Ort::ThrowOnError(ortApi.GetExecutionProviderApi(
                "DML", ORT_API_VERSION,
                reinterpret_cast<const void **>(&ortDmlApi)));
            options.DisableMemPattern();
            options.SetExecutionMode(ORT_SEQUENTIAL);
            Ort::ThrowOnError(
                ortDmlApi->SessionOptionsAppendExecutionProvider_DML(options,
                                                                     deviceId));
        }
        catch (const Ort::Exception &)
        {
        }
    }
    else
#endif
#ifdef USE_CUDA
        if (provider == GPUProvider::CUDA)
    {
        try
        {
            OrtCUDAProviderOptions cudaOptions{};
            cudaOptions.device_id = deviceId;
            cudaOptions.cudnn_conv_algo_search = OrtCudnnConvAlgoSearchDefault;
            cudaOptions.arena_extend_strategy = 1; // kSameAsRequested — tighter memory
            options.AppendExecutionProvider_CUDA(cudaOptions);
        }
        catch (const Ort::Exception &)
        {
        }
    }
    else
#endif
        if (provider == GPUProvider::CoreML)
    {
        try
        {
            options.AppendExecutionProvider("CoreML",
                                            {{"MLComputeUnits", "ALL"}});
        }
        catch (const Ort::Exception &)
        {
        }
    }

    return options;
}

#endif // HAVE_ONNXRUNTIME

bool GAMEDetector::loadModels(const juce::File &gameDir, GPUProvider provider,
                              int deviceId)
{
#ifdef HAVE_ONNXRUNTIME
    if (!gameDir.isDirectory())
    {
        LOG("GAME model directory not found: " + gameDir.getFullPathName());
        return false;
    }

    // Load config.json
    if (!loadConfig(gameDir.getChildFile("config.json")))
    {
        LOG("Failed to load GAME config.json");
        return false;
    }

    try
    {
        // Destroy existing sessions BEFORE replacing the env they reference.
        encoder.session.reset();
        segmenter.session.reset();
        estimator.session.reset();
        bd2dur.session.reset();

        onnxEnv = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING,
                                             "GAMEDetector");
        auto heavyOpts = createSessionOptions(provider, deviceId);
        auto cpuHeavyOpts = createSessionOptions(GPUProvider::CPU, 0);

        // Lightweight options for small models (bd2dur, estimator):
        // single thread avoids thread-pool overhead on trivial ops.
        auto lightOpts = createSessionOptions(provider, deviceId);
        lightOpts.SetIntraOpNumThreads(1);
        auto cpuLightOpts = createSessionOptions(GPUProvider::CPU, 0);
        cpuLightOpts.SetIntraOpNumThreads(1);

        // Load all required models
        struct ModelEntry
        {
            const char *filename;
            ModelSession *session;
            Ort::SessionOptions *opts;
        };
        ModelEntry models[] = {
            {"encoder.onnx", &encoder, &heavyOpts},
            {"segmenter.onnx", &segmenter, &heavyOpts},
            {"estimator.onnx", &estimator, &lightOpts},
            {"bd2dur.onnx", &bd2dur, &lightOpts},
        };

        for (auto &entry : models)
        {
            auto path = gameDir.getChildFile(entry.filename);
            if (!path.existsAsFile())
            {
                LOG("GAME model not found: " + path.getFullPathName());
                loaded = false;
                return false;
            }
            if (!entry.session->load(*onnxEnv, path, *entry.opts))
            {
                if (provider == GPUProvider::CPU)
                {
                    loaded = false;
                    return false;
                }

                LOG("Retrying GAME " + juce::String(entry.filename) +
                    " with CPU");
                auto &cpuOpts = entry.opts == &lightOpts ? cpuLightOpts : cpuHeavyOpts;
                if (!entry.session->load(*onnxEnv, path, cpuOpts))
                {
                    loaded = false;
                    return false;
                }
            }
            LOG("Loaded GAME " + juce::String(entry.filename) +
                " (inputs=" +
                juce::String(
                    static_cast<int>(entry.session->inputNames.size())) +
                " outputs=" +
                juce::String(
                    static_cast<int>(entry.session->outputNames.size())) +
                ")");
        }

        loaded = true;
        return true;
    }
    catch (const Ort::Exception &e)
    {
        LOG("GAME model load error: " + juce::String(e.what()));
        loaded = false;
        return false;
    }
#else
    return false;
#endif
}

std::vector<GAMEDetector::NoteEvent>
GAMEDetector::detectNotes(const float *audio, int numSamples, int sampleRate)
{
    return detectNotesWithProgress(audio, numSamples, sampleRate, nullptr);
}

std::vector<GAMEDetector::NoteEvent> GAMEDetector::detectNotesWithProgress(
    const float *audio, int numSamples, int sampleRate,
    std::function<void(double)> progressCallback)
{
#ifdef HAVE_ONNXRUNTIME
    if (!loaded)
    {
        return {};
    }

    if (progressCallback)
        progressCallback(0.02);

    // 1. Resample to model sample rate
    std::vector<float> waveform = resampleToModelRate(audio, numSamples, sampleRate);

    if (progressCallback)
        progressCallback(0.05);

    // 2. Find chunk boundaries at silence points
    auto chunks = findSilenceChunks(waveform);
    lastChunkRanges = chunks;

    LOG("GAME: split audio into " + juce::String(static_cast<int>(chunks.size())) +
        " chunks (total " + juce::String(static_cast<int>(waveform.size())) + " samples)");

    // 3. Process each chunk and merge results
    std::vector<NoteEvent> allNotes;
    const double progressPerChunk = 0.90 / static_cast<double>(chunks.size());

    for (size_t ci = 0; ci < chunks.size(); ++ci)
    {
        const auto &chunk = chunks[ci];
        std::vector<float> chunkWaveform(
            waveform.begin() + chunk.startSample,
            waveform.begin() + chunk.endSample);

        double base = 0.05 + static_cast<double>(ci) * progressPerChunk;
        auto chunkNotes = processChunk(chunkWaveform, chunk.startSample,
                                       progressCallback, base, progressPerChunk);

        allNotes.insert(allNotes.end(), chunkNotes.begin(), chunkNotes.end());
    }

    if (progressCallback)
        progressCallback(1.0);

    LOG("GAME detection complete: " + juce::String(static_cast<int>(allNotes.size())) + " notes");

    return allNotes;
#else
    return {};
#endif
}

std::vector<GAMEDetector::ChunkRange>
GAMEDetector::findSilenceChunks(const std::vector<float> &waveform) const
{
    const int totalSamples = static_cast<int>(waveform.size());
    if (totalSamples == 0)
        return {};

    // Slicer parameters (matching Python Slicer defaults, units: ms)
    //   threshold  = -40 dB   (RMS silence threshold)
    //   min_length = 1000 ms  (minimum voiced chunk length)
    //   min_interval = 200 ms (minimum silence gap to split on)
    //   max_sil_kept = 100 ms (silence padding kept at chunk boundaries)
    constexpr float thresholdDb = -40.0f;
    constexpr int minLengthMs = 1000;
    constexpr int minIntervalMs = 200;
    constexpr int maxSilKeptMs = 100;

    const float threshold = std::pow(10.0f, thresholdDb / 20.0f);
    const int hop = samplesPerEncoderFrame(); // 441
    const int numHops = (totalSamples + hop - 1) / hop;
    const int minLenHops = minLengthMs * modelSampleRate / (1000 * hop);
    const int minIntHops = minIntervalMs * modelSampleRate / (1000 * hop);
    const int maxSilHops = maxSilKeptMs * modelSampleRate / (1000 * hop);

    // 1. Compute RMS per hop window
    std::vector<float> rms(numHops, 0.0f);
    for (int w = 0; w < numHops; ++w)
    {
        int s = w * hop;
        int e = std::min(s + hop, totalSamples);
        double sum = 0.0;
        for (int i = s; i < e; ++i)
            sum += static_cast<double>(waveform[i]) * waveform[i];
        rms[w] = static_cast<float>(std::sqrt(sum / (e - s)));
    }

    // 2. Find silence intervals (contiguous silent hops >= minIntHops)
    struct Interval
    {
        int start;
        int end;
    };
    std::vector<Interval> silences;
    {
        int silStart = -1;
        for (int w = 0; w <= numHops; ++w)
        {
            bool silent = (w < numHops) && (rms[w] < threshold);
            if (silent && silStart < 0)
                silStart = w;
            if (!silent && silStart >= 0)
            {
                if (w - silStart >= minIntHops)
                    silences.push_back({silStart, w});
                silStart = -1;
            }
        }
    }

    // 3. Build voiced segments from gaps between silence intervals
    struct VoicedSeg
    {
        int startHop;
        int endHop;
    };
    std::vector<VoicedSeg> voiced;
    {
        int prevEnd = 0;
        for (const auto &sil : silences)
        {
            if (sil.start > prevEnd)
                voiced.push_back({prevEnd, sil.start});
            prevEnd = sil.end;
        }
        if (prevEnd < numHops)
            voiced.push_back({prevEnd, numHops});
    }

    // No silence found → treat as single segment
    if (voiced.empty())
        return {{0, totalSamples}};

    // 4. Merge short segments with neighbours
    for (size_t vi = 0; vi + 1 < voiced.size();)
    {
        if (voiced[vi].endHop - voiced[vi].startHop < minLenHops)
        {
            voiced[vi].endHop = voiced[vi + 1].endHop;
            voiced.erase(voiced.begin() + static_cast<ptrdiff_t>(vi) + 1);
        }
        else
        {
            ++vi;
        }
    }
    if (voiced.size() > 1 &&
        voiced.back().endHop - voiced.back().startHop < minLenHops)
    {
        voiced[voiced.size() - 2].endHop = voiced.back().endHop;
        voiced.pop_back();
    }

    // 5. Convert to sample ranges with max_sil_kept padding
    std::vector<ChunkRange> padded;
    for (const auto &seg : voiced)
    {
        int startSample = std::max(0, seg.startHop - maxSilHops) * hop;
        int endSample = std::min(totalSamples, (seg.endHop + maxSilHops) * hop);
        padded.push_back({startSample, endSample});
    }

    // 6. Further split chunks that exceed the encoder frame limit
    const int maxChunk = maxChunkSamples();
    std::vector<ChunkRange> result;
    for (const auto &chunk : padded)
    {
        if (chunk.endSample - chunk.startSample <= maxChunk)
        {
            result.push_back(chunk);
            continue;
        }

        int currentStart = chunk.startSample;
        while (currentStart < chunk.endSample)
        {
            int remaining = chunk.endSample - currentStart;
            if (remaining <= maxChunk)
            {
                result.push_back({currentStart, chunk.endSample});
                break;
            }

            // Find quietest hop in [maxChunk/2 .. maxChunk] from currentStart
            int searchStartW = (currentStart + maxChunk / 2) / hop;
            int searchEndW = std::min((currentStart + maxChunk) / hop, numHops);
            int bestW = -1;
            float bestRms = std::numeric_limits<float>::max();
            for (int w = searchStartW; w < searchEndW; ++w)
            {
                if (rms[w] < bestRms)
                {
                    bestRms = rms[w];
                    bestW = w;
                }
            }

            int splitSample = (bestW >= 0) ? bestW * hop + hop / 2
                                           : currentStart + maxChunk;
            splitSample = std::min(splitSample, chunk.endSample);
            result.push_back({currentStart, splitSample});
            currentStart = splitSample;
        }
    }

    return result;
}

std::vector<GAMEDetector::NoteEvent>
GAMEDetector::processChunk(const std::vector<float> &chunkWaveform, int chunkStartSample,
                           std::function<void(double)> progressCallback,
                           double progressBase, double progressSpan)
{
#ifdef HAVE_ONNXRUNTIME
    const int64_t L = static_cast<int64_t>(chunkWaveform.size());
    float duration = static_cast<float>(L) / static_cast<float>(modelSampleRate);

    static const auto memInfo =
        Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    static const Ort::RunOptions runOptions{nullptr};

    // Frame offset for this chunk in the global timeline
    const int chunkFrameOffset = chunkStartSample / HOP_SIZE;

    // ── Encoder ──────────────────────────────────────────────
    int64_t waveformShape[] = {1, L};
    int64_t durationShape[] = {1};

    // CreateTensor wraps the pointer without copying; ORT does not modify inputs.
    auto *waveformPtr = const_cast<float *>(chunkWaveform.data());

    std::vector<Ort::Value> encoderInputs;
    encoderInputs.reserve(encoder.inputNames.size());

    for (size_t i = 0; i < encoder.inputNameStrings.size(); ++i)
    {
        const auto &name = encoder.inputNameStrings[i];
        if (name == "waveform")
        {
            encoderInputs.emplace_back(Ort::Value::CreateTensor<float>(
                memInfo, waveformPtr, chunkWaveform.size(), waveformShape, 2));
        }
        else if (name == "duration")
        {
            encoderInputs.emplace_back(Ort::Value::CreateTensor<float>(
                memInfo, &duration, 1, durationShape, 1));
        }
        else
        {
            LOG("GAME encoder: unexpected input '" + juce::String(name) + "'");
            return {};
        }
    }

    auto t0_enc = std::chrono::high_resolution_clock::now();
    auto encoderOutputs = encoder.session->Run(
        runOptions, encoder.inputNames.data(), encoderInputs.data(),
        encoderInputs.size(), encoder.outputNames.data(),
        encoder.outputNames.size());
    auto t1_enc = std::chrono::high_resolution_clock::now();
    LOG("GAME encoder: " + juce::String(std::chrono::duration<double, std::milli>(t1_enc - t0_enc).count(), 1) + " ms" + " (L=" + juce::String(L) + ")");

    if (progressCallback)
        progressCallback(progressBase + progressSpan * 0.15);

    // Extract encoder outputs
    int xSegIdx = encoder.findOutput("x_seg");
    int xEstIdx = encoder.findOutput("x_est");
    int maskTIdx = encoder.findOutput("maskT");

    if (xSegIdx < 0 || xEstIdx < 0 || maskTIdx < 0)
    {
        LOG("GAME encoder outputs not found (x_seg/x_est/maskT)");
        return {};
    }

    auto maskTShape =
        encoderOutputs[maskTIdx].GetTensorTypeAndShapeInfo().GetShape();
    const int64_t T = maskTShape.size() >= 2 ? maskTShape[1] : maskTShape[0];

    auto xSegShape =
        encoderOutputs[xSegIdx].GetTensorTypeAndShapeInfo().GetShape();
    const int64_t C = xSegShape.size() >= 3 ? xSegShape[2] : embeddingDim;

    const size_t featureSize = static_cast<size_t>(T * C);
    std::vector<float> x_segData(featureSize);
    std::vector<float> x_estData(featureSize);
    auto maskTSize = static_cast<size_t>(T);

    std::memcpy(x_segData.data(),
                encoderOutputs[xSegIdx].GetTensorData<float>(),
                featureSize * sizeof(float));
    std::memcpy(x_estData.data(),
                encoderOutputs[xEstIdx].GetTensorData<float>(),
                featureSize * sizeof(float));

    auto maskTBoolData = std::make_unique<uint8_t[]>(maskTSize);
    std::memcpy(maskTBoolData.get(),
                encoderOutputs[maskTIdx].GetTensorData<bool>(), maskTSize);

    encoderOutputs.clear();

    // ── Segmenter (D3PM loop) ────────────────────────────────────
    auto knownBoundaries = std::make_unique<uint8_t[]>(maskTSize);
    std::memset(knownBoundaries.get(), 0, maskTSize);
    auto prevBoundaries = std::make_unique<uint8_t[]>(maskTSize);
    std::memset(prevBoundaries.get(), 0, maskTSize);

    int64_t featureShape[] = {1, T, C};
    int64_t frameMaskShape[] = {1, T};
    int64_t langShape[] = {1};
    int64_t language = 0;
    int64_t radiusVal = static_cast<int64_t>(segRadius);
    float tVal = 0.0f;
    int64_t tShape[] = {1};

    const int totalSteps = supportsLoop ? numD3PMSteps : 1;
    const int boundariesIdx = [&]()
    {
        int idx = segmenter.findOutput("boundaries");
        return idx >= 0 ? idx : 0;
    }();

    // Build input tensors once; only tVal and prevBoundaries data change per step.
    // Tensors wrap raw pointers, so updating the underlying data is sufficient.
    std::vector<Ort::Value> segInputs;
    segInputs.reserve(segmenter.inputNames.size());
    for (size_t i = 0; i < segmenter.inputNameStrings.size(); ++i)
    {
        const auto &name = segmenter.inputNameStrings[i];
        if (name == "x_seg")
            segInputs.emplace_back(Ort::Value::CreateTensor<float>(
                memInfo, x_segData.data(), featureSize, featureShape, 3));
        else if (name == "maskT")
            segInputs.emplace_back(Ort::Value::CreateTensor(
                memInfo, maskTBoolData.get(), maskTSize,
                frameMaskShape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL));
        else if (name == "known_boundaries")
            segInputs.emplace_back(Ort::Value::CreateTensor(
                memInfo, knownBoundaries.get(), maskTSize,
                frameMaskShape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL));
        else if (name == "prev_boundaries")
            segInputs.emplace_back(Ort::Value::CreateTensor(
                memInfo, prevBoundaries.get(), maskTSize,
                frameMaskShape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL));
        else if (name == "threshold")
            segInputs.emplace_back(Ort::Value::CreateTensor<float>(
                memInfo, &segThreshold, 1, nullptr, 0));
        else if (name == "radius")
            segInputs.emplace_back(Ort::Value::CreateTensor<int64_t>(
                memInfo, &radiusVal, 1, nullptr, 0));
        else if (name == "t")
            segInputs.emplace_back(Ort::Value::CreateTensor<float>(
                memInfo, &tVal, 1, tShape, 1));
        else if (name == "language")
            segInputs.emplace_back(Ort::Value::CreateTensor<int64_t>(
                memInfo, &language, 1, langShape, 1));
        else
        {
            LOG("GAME segmenter: unexpected input '" + juce::String(name) + "'");
            return {};
        }
    }

    auto t0_segAll = std::chrono::high_resolution_clock::now();
    for (int step = 0; step < totalSteps; ++step)
    {
        tVal = supportsLoop
                   ? static_cast<float>(step) / static_cast<float>(totalSteps)
                   : 0.0f;

        // segInputs wraps raw pointers → tVal and prevBoundaries already updated in place

        auto segOutputs = segmenter.session->Run(
            runOptions, segmenter.inputNames.data(), segInputs.data(),
            segInputs.size(), segmenter.outputNames.data(),
            segmenter.outputNames.size());

        std::memcpy(prevBoundaries.get(),
                    segOutputs[boundariesIdx].GetTensorData<bool>(), maskTSize);

        if (progressCallback)
            progressCallback(progressBase + progressSpan *
                                                (0.15 + 0.50 * static_cast<double>(step + 1) / totalSteps));
    }
    auto t1_segAll = std::chrono::high_resolution_clock::now();
    LOG("GAME segmenter (" + juce::String(totalSteps) + " steps): " + juce::String(std::chrono::duration<double, std::milli>(t1_segAll - t0_segAll).count(), 1) + " ms");

    // ── bd2dur ───────────────────────────────────────────────
    std::vector<Ort::Value> bd2durInputs;
    bd2durInputs.reserve(bd2dur.inputNames.size());

    for (size_t i = 0; i < bd2dur.inputNameStrings.size(); ++i)
    {
        const auto &name = bd2dur.inputNameStrings[i];
        if (name == "boundaries")
        {
            bd2durInputs.emplace_back(Ort::Value::CreateTensor(
                memInfo, prevBoundaries.get(), maskTSize, frameMaskShape, 2,
                ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL));
        }
        else if (name == "maskT")
        {
            bd2durInputs.emplace_back(Ort::Value::CreateTensor(
                memInfo, maskTBoolData.get(), maskTSize, frameMaskShape, 2,
                ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL));
        }
        else if (name == "duration")
        {
            bd2durInputs.emplace_back(Ort::Value::CreateTensor<float>(
                memInfo, &duration, 1, durationShape, 1));
        }
        else
        {
            LOG("GAME bd2dur: unexpected input '" + juce::String(name) + "'");
            return {};
        }
    }

    auto t0_bd = std::chrono::high_resolution_clock::now();
    auto bd2durOutputs = bd2dur.session->Run(
        runOptions, bd2dur.inputNames.data(), bd2durInputs.data(),
        bd2durInputs.size(), bd2dur.outputNames.data(),
        bd2dur.outputNames.size());
    auto t1_bd = std::chrono::high_resolution_clock::now();
    LOG("GAME bd2dur: " + juce::String(std::chrono::duration<double, std::milli>(t1_bd - t0_bd).count(), 1) + " ms");

    if (progressCallback)
        progressCallback(progressBase + progressSpan * 0.70);

    int durIdx = bd2dur.findOutput("durations");
    if (durIdx < 0)
        durIdx = 0;

    auto durShape = bd2durOutputs[durIdx].GetTensorTypeAndShapeInfo().GetShape();
    const int64_t N = durShape.size() >= 2 ? durShape[1] : durShape[0];
    const float *durationsData = bd2durOutputs[durIdx].GetTensorData<float>();

    int bd2durMaskNIdx = bd2dur.findOutput("maskN");

    // ── Estimator ────────────────────────────────────────────
    // estimator needs maskN from bd2dur
    const bool *bd2durMaskNData = (bd2durMaskNIdx >= 0)
                                      ? bd2durOutputs[bd2durMaskNIdx].GetTensorData<bool>()
                                      : nullptr;

    // Prepare maskN tensor for estimator input
    int64_t maskNShape[] = {1, N};
    auto maskNBuf = std::make_unique<uint8_t[]>(static_cast<size_t>(N));
    if (bd2durMaskNData)
    {
        std::memcpy(maskNBuf.get(), bd2durMaskNData, static_cast<size_t>(N));
    }
    else
    {
        std::memset(maskNBuf.get(), 1, static_cast<size_t>(N));
    }

    std::vector<Ort::Value> estInputs;
    estInputs.reserve(estimator.inputNames.size());

    for (size_t i = 0; i < estimator.inputNameStrings.size(); ++i)
    {
        const auto &name = estimator.inputNameStrings[i];
        if (name == "x_est")
        {
            estInputs.emplace_back(Ort::Value::CreateTensor<float>(
                memInfo, x_estData.data(), featureSize, featureShape, 3));
        }
        else if (name == "maskT")
        {
            estInputs.emplace_back(Ort::Value::CreateTensor(
                memInfo, maskTBoolData.get(), maskTSize, frameMaskShape, 2,
                ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL));
        }
        else if (name == "boundaries")
        {
            estInputs.emplace_back(Ort::Value::CreateTensor(
                memInfo, prevBoundaries.get(), maskTSize, frameMaskShape, 2,
                ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL));
        }
        else if (name == "maskN")
        {
            estInputs.emplace_back(Ort::Value::CreateTensor(
                memInfo, maskNBuf.get(), static_cast<size_t>(N), maskNShape, 2,
                ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL));
        }
        else if (name == "threshold")
        {
            estInputs.emplace_back(Ort::Value::CreateTensor<float>(
                memInfo, &estThreshold, 1, nullptr, 0));
        }
        else
        {
            LOG("GAME estimator: unexpected input '" + juce::String(name) + "'");
            return {};
        }
    }

    auto t0_est = std::chrono::high_resolution_clock::now();
    auto estOutputs = estimator.session->Run(
        runOptions, estimator.inputNames.data(), estInputs.data(),
        estInputs.size(), estimator.outputNames.data(),
        estimator.outputNames.size());
    auto t1_est = std::chrono::high_resolution_clock::now();
    LOG("GAME estimator: " + juce::String(std::chrono::duration<double, std::milli>(t1_est - t0_est).count(), 1) + " ms");

    if (progressCallback)
        progressCallback(progressBase + progressSpan * 0.85);

    // Extract estimator outputs
    int presenceIdx = estimator.findOutput("presence");
    int scoresIdx = estimator.findOutput("scores");
    int estMaskNIdx = estimator.findOutput("maskN");

    if (scoresIdx < 0)
    {
        LOG("GAME estimator: 'scores' output not found");
        return {};
    }

    auto scoresShape = estOutputs[scoresIdx].GetTensorTypeAndShapeInfo().GetShape();
    const int64_t estN = scoresShape.size() >= 2 ? scoresShape[1] : scoresShape[0];
    const int64_t noteCount = std::min(N, estN);

    const float *scoresData = estOutputs[scoresIdx].GetTensorData<float>();
    const bool *presenceData =
        presenceIdx >= 0 ? estOutputs[presenceIdx].GetTensorData<bool>() : nullptr;
    const bool *finalMaskN =
        estMaskNIdx >= 0 ? estOutputs[estMaskNIdx].GetTensorData<bool>()
                         : bd2durMaskNData;

    // ── Build NoteEvents with global frame offset ───────────
    std::vector<NoteEvent> notes;
    notes.reserve(static_cast<size_t>(noteCount));

    const double secondsPerFrame =
        static_cast<double>(HOP_SIZE) / static_cast<double>(SAMPLE_RATE);

    double cumulativeTime = 0.0;
    for (int64_t i = 0; i < noteCount; ++i)
    {
        if (finalMaskN && !finalMaskN[i])
            break;

        double noteStartTime = cumulativeTime;
        cumulativeTime += static_cast<double>(durationsData[i]);
        double noteEndTime = cumulativeTime;

        int startFrame = chunkFrameOffset +
                         static_cast<int>(std::round(noteStartTime / secondsPerFrame));
        int endFrame = chunkFrameOffset +
                       static_cast<int>(std::round(noteEndTime / secondsPerFrame));

        if (endFrame <= startFrame)
            endFrame = startFrame + 1;

        bool voiced = presenceData ? presenceData[i] : true;

        NoteEvent event;
        event.startFrame = startFrame;
        event.endFrame = endFrame;
        event.midiNote = scoresData[i];
        event.isRest = !voiced;
        notes.push_back(event);
    }

    if (progressCallback)
        progressCallback(progressBase + progressSpan);

    return notes;
#else
    return {};
#endif
}
