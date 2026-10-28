#pragma once

#include "../JuceHeader.h"
#include <memory>
#include <string>
#include <vector>

class CoreMLVocoderBackend
{
public:
  // Mirrors MLComputeUnits without pulling Core ML into C++ headers.
  enum class ComputeUnits
  {
    All,       // Core ML picks ANE / GPU / CPU
    CPUAndGPU,
    CPUOnly
  };

  CoreMLVocoderBackend();
  ~CoreMLVocoderBackend();

  bool load(const juce::File &modelPath, ComputeUnits units,
            std::string &error);
  ComputeUnits getComputeUnits() const { return computeUnits; }

  static const char *toString(ComputeUnits units);

  std::vector<float> infer(const std::vector<float> &mel,
                           const std::vector<float> &f0,
                           size_t numFrames,
                           int numMels,
                           int hopSize,
                           std::string &error);

private:
  ComputeUnits computeUnits = ComputeUnits::All;
  struct Impl;
  std::unique_ptr<Impl> impl;
};
