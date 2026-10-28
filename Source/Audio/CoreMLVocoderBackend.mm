#include "CoreMLVocoderBackend.h"

#if __APPLE__

#import <CoreML/CoreML.h>
#import <Foundation/Foundation.h>
#include <algorithm>
#include <cmath>

struct CoreMLVocoderBackend::Impl
{
  MLModel *model = nil;

  ~Impl()
  {
#if !__has_feature(objc_arc)
    [model release];
#endif
  }
};

CoreMLVocoderBackend::CoreMLVocoderBackend() : impl(std::make_unique<Impl>())
{
}

CoreMLVocoderBackend::~CoreMLVocoderBackend() = default;

const char *CoreMLVocoderBackend::toString(ComputeUnits units)
{
  switch (units)
  {
  case ComputeUnits::All:
    return "All";
  case ComputeUnits::CPUAndGPU:
    return "CPUAndGPU";
  case ComputeUnits::CPUOnly:
    return "CPUOnly";
  }
  return "Unknown";
}

bool CoreMLVocoderBackend::load(const juce::File &modelPath,
                                ComputeUnits units, std::string &error)
{
  @autoreleasepool
  {
    if (!modelPath.isDirectory())
    {
      error = "Core ML model bundle not found: " +
              modelPath.getFullPathName().toStdString();
      return false;
    }

    NSString *path =
        [NSString stringWithUTF8String:modelPath.getFullPathName()
                                           .toRawUTF8()];
    NSURL *url = [NSURL fileURLWithPath:path isDirectory:YES];

    MLModelConfiguration *config = [[MLModelConfiguration alloc] init];
    switch (units)
    {
    case ComputeUnits::All:
      config.computeUnits = MLComputeUnitsAll;
      break;
    case ComputeUnits::CPUAndGPU:
      config.computeUnits = MLComputeUnitsCPUAndGPU;
      break;
    case ComputeUnits::CPUOnly:
      config.computeUnits = MLComputeUnitsCPUOnly;
      break;
    }

    NSError *nsError = nil;
    MLModel *loadedModel = [MLModel modelWithContentsOfURL:url
                                             configuration:config
                                                     error:&nsError];
    if (!loadedModel)
    {
      NSString *message = nsError.localizedDescription ?: @"unknown error";
      error = "Failed to load Core ML model (" + std::string(toString(units)) +
              "): " + std::string(message.UTF8String ?: "unknown error");
      return false;
    }
    computeUnits = units;

#if __has_feature(objc_arc)
    impl->model = loadedModel;
#else
    [impl->model release];
    impl->model = [loadedModel retain];
#endif
    return true;
  }
}

std::vector<float>
CoreMLVocoderBackend::infer(const std::vector<float> &mel,
                            const std::vector<float> &f0,
                            size_t numFrames,
                            int numMels,
                            int hopSize,
                            std::string &error)
{
  @autoreleasepool
  {
    if (!impl->model)
    {
      error = "Core ML model is not loaded";
      return {};
    }

    NSError *nsError = nil;
    MLMultiArray *melArray =
        [[MLMultiArray alloc] initWithShape:@[
          @1, @(numMels), @(static_cast<NSInteger>(numFrames))
        ]
                                   dataType:MLMultiArrayDataTypeFloat32
                                      error:&nsError];
    if (!melArray)
    {
      NSString *message = nsError.localizedDescription ?: @"unknown error";
      error = "Failed to allocate Core ML mel input: " +
              std::string(message.UTF8String ?: "unknown error");
      return {};
    }

    MLMultiArray *f0Array =
        [[MLMultiArray alloc] initWithShape:@[
          @1, @(static_cast<NSInteger>(numFrames))
        ]
                                   dataType:MLMultiArrayDataTypeFloat32
                                      error:&nsError];
    if (!f0Array)
    {
      NSString *message = nsError.localizedDescription ?: @"unknown error";
      error = "Failed to allocate Core ML f0 input: " +
              std::string(message.UTF8String ?: "unknown error");
      return {};
    }

    std::copy(mel.begin(), mel.end(),
              static_cast<float *>(melArray.dataPointer));
    std::copy(f0.begin(), f0.end(), static_cast<float *>(f0Array.dataPointer));

    NSDictionary<NSString *, id> *features = @{
      @"mel" : melArray,
      @"f0" : f0Array,
    };
    MLDictionaryFeatureProvider *provider =
        [[MLDictionaryFeatureProvider alloc] initWithDictionary:features
                                                          error:&nsError];
    if (!provider)
    {
      NSString *message = nsError.localizedDescription ?: @"unknown error";
      error = "Failed to create Core ML feature provider: " +
              std::string(message.UTF8String ?: "unknown error");
      return {};
    }

    id<MLFeatureProvider> output = [impl->model predictionFromFeatures:provider
                                                                  error:&nsError];
    if (!output)
    {
      NSString *message = nsError.localizedDescription ?: @"unknown error";
      error = "Core ML inference failed: " +
              std::string(message.UTF8String ?: "unknown error");
      return {};
    }

    MLFeatureValue *audioFeature = [output featureValueForName:@"audio"];
    MLMultiArray *audioArray = audioFeature.multiArrayValue;
    if (!audioArray)
    {
      error = "Core ML inference returned no audio output";
      return {};
    }

    const size_t outputSize = static_cast<size_t>(audioArray.count);
    if (outputSize == 0)
    {
      error = "Core ML inference returned an empty audio buffer";
      return {};
    }

    // The shipped model casts its output to Float32, but guard against a
    // bundle whose output is Float16: reading that as float is garbage.
    std::vector<float> waveform(outputSize);
    if (audioArray.dataType == MLMultiArrayDataTypeFloat32)
    {
      const float *audioData =
          static_cast<const float *>(audioArray.dataPointer);
      std::copy_n(audioData, outputSize, waveform.begin());
    }
    else
    {
      for (size_t i = 0; i < outputSize; ++i)
        waveform[i] = [audioArray[static_cast<NSInteger>(i)] floatValue];
    }

    // Validate before clamping: std::clamp passes NaN straight through, and
    // NaN/Inf from an overflow in the model's FP16 conv/resblock body reaches
    // the mixer as silence.
    size_t nonFinite = 0;
    float peak = 0.0f;
    for (float &sample : waveform)
    {
      if (!std::isfinite(sample))
      {
        ++nonFinite;
        sample = 0.0f;
        continue;
      }
      peak = std::max(peak, std::abs(sample));
      sample = std::clamp(sample, -1.0f, 1.0f);
    }
    if (nonFinite > 0)
    {
      error = "Core ML inference produced " + std::to_string(nonFinite) +
              " non-finite samples (" + std::string(toString(computeUnits)) +
              ")";
      return {};
    }

    bool hasVoicedInput = false;
    for (float hz : f0)
      if (hz > 0.0f)
      {
        hasVoicedInput = true;
        break;
      }
    if (hasVoicedInput && peak < 1.0e-6f)
    {
      error = "Core ML inference produced silent output for voiced input (" +
              std::string(toString(computeUnits)) + ")";
      return {};
    }

    const size_t expectedSize = numFrames * static_cast<size_t>(hopSize);
    if (expectedSize > 0 && waveform.size() > expectedSize)
      waveform.resize(expectedSize);

    return waveform;
  }
}

#else

struct CoreMLVocoderBackend::Impl
{
};

CoreMLVocoderBackend::CoreMLVocoderBackend() : impl(std::make_unique<Impl>())
{
}

CoreMLVocoderBackend::~CoreMLVocoderBackend() = default;

const char *CoreMLVocoderBackend::toString(ComputeUnits)
{
  return "Unavailable";
}

bool CoreMLVocoderBackend::load(const juce::File &, ComputeUnits,
                                std::string &error)
{
  error = "Core ML is only available on Apple platforms";
  return false;
}

std::vector<float>
CoreMLVocoderBackend::infer(const std::vector<float> &,
                            const std::vector<float> &,
                            size_t,
                            int,
                            int,
                            std::string &error)
{
  error = "Core ML is only available on Apple platforms";
  return {};
}

#endif
