#include "AudioFileManager.h"
#include "../../Utils/Localization.h"

AudioFileManager::AudioFileManager() {
  workerThread = std::thread([this]() {
    for (;;) {
      LoadTask task;
      {
        std::unique_lock<std::mutex> lock(queueMutex);
        queueCv.wait(lock, [this]() {
          return isShuttingDown.load() || !loadQueue.empty();
        });

        if (isShuttingDown.load() && loadQueue.empty())
          return;

        task = std::move(loadQueue.front());
        loadQueue.pop_front();
        currentCancelFlag = task.cancelFlag;
        isLoadingAudio = true;
      }

      auto finishTask = [this]() {
        std::lock_guard<std::mutex> lock(queueMutex);
        currentCancelFlag.reset();
        isLoadingAudio = false;
      };

      // Early cancel
      if (task.cancelFlag && task.cancelFlag->load()) {
        finishTask();
        continue;
      }

      if (isShuttingDown.load()) {
        finishTask();
        continue;
      }

      if (task.onProgress)
        task.onProgress(0.04, TR("progress.loading_audio"));

      juce::AudioFormatManager formatManager;
      formatManager.registerBasicFormats();

      std::unique_ptr<juce::AudioFormatReader> reader(
          formatManager.createReaderFor(task.file));
      if (reader == nullptr || (task.cancelFlag && task.cancelFlag->load()) ||
          isShuttingDown.load()) {
        finishTask();
        continue;
      }

      const int numSamples = static_cast<int>(reader->lengthInSamples);
      const int srcSampleRate = static_cast<int>(reader->sampleRate);

      if (task.onProgress)
        task.onProgress(0.08, TR("progress.reading_audio"));

      juce::AudioBuffer<float> buffer;
      if (reader->numChannels == 1) {
        buffer.setSize(1, numSamples);
        reader->read(&buffer, 0, numSamples, 0, true, false);
      } else {
        juce::AudioBuffer<float> stereoBuffer(2, numSamples);
        reader->read(&stereoBuffer, 0, numSamples, 0, true, true);
        buffer = convertToMono(stereoBuffer);
      }

      if ((task.cancelFlag && task.cancelFlag->load()) ||
          isShuttingDown.load()) {
        finishTask();
        continue;
      }

      if (srcSampleRate != SAMPLE_RATE) {
        if (task.onProgress)
          task.onProgress(0.14, TR("progress.resampling"));
        buffer = resampleIfNeeded(buffer, srcSampleRate, SAMPLE_RATE);
      }

      if ((task.cancelFlag && task.cancelFlag->load()) ||
          isShuttingDown.load()) {
        finishTask();
        continue;
      }

      if (task.onProgress)
        task.onProgress(0.15, TR("progress.audio_loaded"));

      auto onComplete = std::move(task.onComplete);
      auto cancelFlag = task.cancelFlag;
      auto file = task.file;
      finishTask();

      if ((cancelFlag && cancelFlag->load()) || isShuttingDown.load())
        continue;

      juce::MessageManager::callAsync(
          [onComplete, buffer = std::move(buffer), file]() mutable {
            if (onComplete)
              onComplete(std::move(buffer), SAMPLE_RATE, file);
          });
    }
  });
}

AudioFileManager::~AudioFileManager() {
  cancelLoading();
  isShuttingDown = true;
  queueCv.notify_all();
  if (workerThread.joinable())
    workerThread.join();
}

void AudioFileManager::cancelLoading() {
  std::lock_guard<std::mutex> lock(queueMutex);

  if (currentCancelFlag)
    currentCancelFlag->store(true);

  for (auto &task : loadQueue) {
    if (task.cancelFlag)
      task.cancelFlag->store(true);
  }
  loadQueue.clear();
}

void AudioFileManager::showOpenDialog(
    std::function<void(const juce::File &)> onFileSelected) {
  if (!juce::MessageManager::getInstance()->isThisTheMessageThread()) {
    auto weakThis = juce::WeakReference<AudioFileManager>(this);
    juce::MessageManager::callAsync([weakThis, onFileSelected]() {
      if (weakThis != nullptr)
        weakThis->showOpenDialog(onFileSelected);
    });
    return;
  }

  if (fileChooser != nullptr)
    return;

  fileChooser = std::make_unique<juce::FileChooser>(
      TR("dialog.select_audio"), juce::File{}, "*.wav;*.mp3;*.flac;*.aiff");

  auto chooserFlags = juce::FileBrowserComponent::openMode |
                      juce::FileBrowserComponent::canSelectFiles;

  fileChooser->launchAsync(
      chooserFlags, [this, onFileSelected](const juce::FileChooser &fc) {
        auto file = fc.getResult();
        fileChooser.reset();
        if (file.existsAsFile() && onFileSelected)
          onFileSelected(file);
      });
}

void AudioFileManager::showSaveDialog(
    const juce::File &defaultPath,
    std::function<void(const juce::File &)> onFileSelected) {
  if (!juce::MessageManager::getInstance()->isThisTheMessageThread()) {
    auto weakThis = juce::WeakReference<AudioFileManager>(this);
    juce::MessageManager::callAsync([
        weakThis, defaultPath, onFileSelected]() mutable {
      if (weakThis != nullptr)
        weakThis->showSaveDialog(defaultPath, onFileSelected);
    });
    return;
  }

  if (fileChooser != nullptr)
    return;

  fileChooser = std::make_unique<juce::FileChooser>(TR("dialog.save_project"),
                                                    defaultPath, "*.pitchnet");

  auto chooserFlags = juce::FileBrowserComponent::saveMode |
                      juce::FileBrowserComponent::canSelectFiles |
                      juce::FileBrowserComponent::warnAboutOverwriting;

  fileChooser->launchAsync(
      chooserFlags, [this, onFileSelected](const juce::FileChooser &fc) {
        auto file = fc.getResult();
        fileChooser.reset();
        if (file != juce::File{} && onFileSelected) {
          auto finalFile = file.getFileExtension().isEmpty()
                               ? file.withFileExtension("pitchnet")
                               : file;
          onFileSelected(finalFile);
        }
      });
}

void AudioFileManager::showExportDialog(
    const juce::File &defaultPath,
    std::function<void(const juce::File &)> onFileSelected) {
  if (!juce::MessageManager::getInstance()->isThisTheMessageThread()) {
    auto weakThis = juce::WeakReference<AudioFileManager>(this);
    juce::MessageManager::callAsync([
        weakThis, defaultPath, onFileSelected]() mutable {
      if (weakThis != nullptr)
        weakThis->showExportDialog(defaultPath, onFileSelected);
    });
    return;
  }

  if (fileChooser != nullptr)
    return;

  fileChooser = std::make_unique<juce::FileChooser>(TR("dialog.export_audio"),
                                                    defaultPath, "*.wav");

  auto chooserFlags = juce::FileBrowserComponent::saveMode |
                      juce::FileBrowserComponent::canSelectFiles |
                      juce::FileBrowserComponent::warnAboutOverwriting;

  fileChooser->launchAsync(
      chooserFlags, [this, onFileSelected](const juce::FileChooser &fc) {
        auto file = fc.getResult();
        fileChooser.reset();
        if (file != juce::File{} && onFileSelected) {
          auto finalFile = file.getFileExtension().isEmpty()
                               ? file.withFileExtension("wav")
                               : file;
          onFileSelected(finalFile);
        }
      });
}

void AudioFileManager::loadAudioFileAsync(const juce::File &file,
                                          ProgressCallback onProgress,
                                          LoadCompleteCallback onComplete) {
  auto cancelFlag = std::make_shared<std::atomic<bool>>(false);

  {
    std::lock_guard<std::mutex> lock(queueMutex);
    loadQueue.push_back(LoadTask{file, std::move(onProgress),
                                 std::move(onComplete), cancelFlag});
  }
  queueCv.notify_one();
}

void AudioFileManager::exportAudioFileAsync(
    const juce::File &file, const juce::AudioBuffer<float> &buffer,
    int sampleRate, ProgressCallback onProgress,
    ExportCompleteCallback onComplete) {
  if (onProgress)
    onProgress(0.0, TR("progress.exporting"));

  // Export synchronously for now (could be made async if needed)
  juce::WavAudioFormat wavFormat;
  auto writerOptions = juce::AudioFormatWriterOptions{}
                           .withSampleRate(sampleRate)
                           .withNumChannels(buffer.getNumChannels())
                           .withBitsPerSample(16);
  auto fileStream = std::make_unique<juce::FileOutputStream>(file);
  if (!fileStream->openedOk()) {
    if (onProgress)
      onProgress(1.0, TR("progress.export_failed"));
    if (onComplete)
      onComplete(false);
    return;
  }
  std::unique_ptr<juce::OutputStream> outputStream = std::move(fileStream);
  std::unique_ptr<juce::AudioFormatWriter> writer(
      wavFormat.createWriterFor(outputStream, writerOptions));

  bool success = false;
  if (writer) {
    success =
        writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
  }

  if (onProgress)
    onProgress(1.0, success ? TR("progress.export_complete")
                            : TR("progress.export_failed"));

  if (onComplete)
    onComplete(success);
}

bool AudioFileManager::isInterestedInFileDrag(const juce::StringArray &files) {
  for (const auto &f : files) {
    juce::File file(f);
    auto ext = file.getFileExtension().toLowerCase();
    if (ext == ".wav" || ext == ".mp3" || ext == ".flac" || ext == ".aiff" ||
        ext == ".pitchnet")
      return true;
  }
  return false;
}

juce::File AudioFileManager::getFirstAudioFile(const juce::StringArray &files) {
  for (const auto &f : files) {
    juce::File file(f);
    auto ext = file.getFileExtension().toLowerCase();
    if (ext == ".wav" || ext == ".mp3" || ext == ".flac" || ext == ".aiff" ||
        ext == ".pitchnet")
      return file;
  }
  return {};
}

juce::AudioBuffer<float>
AudioFileManager::resampleIfNeeded(const juce::AudioBuffer<float> &buffer,
                                   int srcSampleRate, int targetSampleRate) {
  if (srcSampleRate == targetSampleRate)
    return buffer;

  const double ratio = static_cast<double>(srcSampleRate) / targetSampleRate;
  const int numSamples = buffer.getNumSamples();
  const int newNumSamples = static_cast<int>(numSamples / ratio);

  juce::AudioBuffer<float> resampledBuffer(1, newNumSamples);
  const float *src = buffer.getReadPointer(0);
  float *dst = resampledBuffer.getWritePointer(0);

  for (int i = 0; i < newNumSamples; ++i) {
    const double srcPos = i * ratio;
    const int srcIndex = static_cast<int>(srcPos);
    const double frac = srcPos - srcIndex;

    if (srcIndex + 1 < numSamples)
      dst[i] = static_cast<float>(src[srcIndex] * (1.0 - frac) +
                                  src[srcIndex + 1] * frac);
    else
      dst[i] = src[srcIndex];
  }

  return resampledBuffer;
}

juce::AudioBuffer<float>
AudioFileManager::convertToMono(const juce::AudioBuffer<float> &stereoBuffer) {
  const int numSamples = stereoBuffer.getNumSamples();
  juce::AudioBuffer<float> monoBuffer(1, numSamples);

  const float *left = stereoBuffer.getReadPointer(0);
  const float *right = stereoBuffer.getReadPointer(1);
  float *mono = monoBuffer.getWritePointer(0);

  for (int i = 0; i < numSamples; ++i)
    mono[i] = (left[i] + right[i]) * 0.5f;

  return monoBuffer;
}
