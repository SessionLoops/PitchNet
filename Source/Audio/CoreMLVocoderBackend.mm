#include "CoreMLVocoderBackend.h"

#if __APPLE__

#import <CoreML/CoreML.h>
#import <Foundation/Foundation.h>
#include <algorithm>

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

bool CoreMLVocoderBackend::load(const juce::File &modelPath, std::string &error)
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
    config.computeUnits = MLComputeUnitsAll;

    NSError *nsError = nil;
    MLModel *loadedModel = [MLModel modelWithContentsOfURL:url
                                             configuration:config
                                                     error:&nsError];
    if (!loadedModel)
    {
      NSString *message = nsError.localizedDescription ?: @"unknown error";
      error = "Failed to load Core ML model: " +
              std::string(message.UTF8String ?: "unknown error");
      return false;
    }

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
    const float *audioData = static_cast<const float *>(audioArray.dataPointer);
    std::vector<float> waveform(outputSize);
    for (size_t i = 0; i < outputSize; ++i)
      waveform[i] = std::clamp(audioData[i], -1.0f, 1.0f);

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

bool CoreMLVocoderBackend::load(const juce::File &, std::string &error)
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
