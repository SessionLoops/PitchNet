#pragma once

#include "../JuceHeader.h"
#include <memory>
#include <string>
#include <vector>

class CoreMLVocoderBackend
{
public:
  CoreMLVocoderBackend();
  ~CoreMLVocoderBackend();

  bool load(const juce::File &modelPath, std::string &error);

  std::vector<float> infer(const std::vector<float> &mel,
                           const std::vector<float> &f0,
                           size_t numFrames,
                           int numMels,
                           int hopSize,
                           std::string &error);

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};
