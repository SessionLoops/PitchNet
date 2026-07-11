#include "ExportHelper.h"
#include "../StyledComponents.h"
#include <cmath>

namespace ExportHelper {

juce::String getFormatDisplayName(ExportFormat format) {
  switch (format) {
  case ExportFormat::wav:
    return "WAV";
  case ExportFormat::flac:
    return "FLAC";
  case ExportFormat::aiff:
    return "AIFF";
  case ExportFormat::ogg:
    return "OGG";
  }
  return "WAV";
}

juce::String getFormatExtension(ExportFormat format) {
  switch (format) {
  case ExportFormat::wav:
    return "wav";
  case ExportFormat::flac:
    return "flac";
  case ExportFormat::aiff:
    return "aiff";
  case ExportFormat::ogg:
    return "ogg";
  }
  return "wav";
}

juce::String getFormatWildcard(ExportFormat format) {
  return "*." + getFormatExtension(format);
}

juce::AudioBuffer<float> convertChannels(const juce::AudioBuffer<float> &input,
                                          int outChannels) {
  outChannels = juce::jlimit(1, 2, outChannels);
  const int inChannels = juce::jmax(1, input.getNumChannels());
  const int numSamples = input.getNumSamples();

  juce::AudioBuffer<float> output(outChannels, numSamples);
  output.clear();

  if (outChannels == 1) {
    float *dst = output.getWritePointer(0);
    for (int i = 0; i < numSamples; ++i) {
      float sum = 0.0f;
      for (int ch = 0; ch < inChannels; ++ch)
        sum += input.getSample(ch, i);
      dst[i] = sum / static_cast<float>(inChannels);
    }
    return output;
  }

  // Stereo output
  if (inChannels == 1) {
    output.copyFrom(0, 0, input, 0, 0, numSamples);
    output.copyFrom(1, 0, input, 0, 0, numSamples);
  } else {
    output.copyFrom(0, 0, input, 0, 0, numSamples);
    output.copyFrom(1, 0, input, 1, 0, numSamples);
  }
  return output;
}

juce::AudioBuffer<float> resampleAudio(const juce::AudioBuffer<float> &input,
                                        int sourceRate, int targetRate) {
  if (sourceRate <= 0 || targetRate <= 0 || sourceRate == targetRate)
    return input;

  const int channels = juce::jmax(1, input.getNumChannels());
  const int inSamples = input.getNumSamples();
  const double ratio = static_cast<double>(sourceRate) / targetRate;
  const int outSamples = juce::jmax(1, static_cast<int>(std::llround(inSamples / ratio)));

  juce::AudioBuffer<float> output(channels, outSamples);
  output.clear();

  for (int ch = 0; ch < channels; ++ch) {
    juce::LagrangeInterpolator interpolator;
    interpolator.reset();
    interpolator.process(ratio, input.getReadPointer(ch), output.getWritePointer(ch),
                         outSamples);
  }
  return output;
}

juce::AudioFormat *findFormatForExtension(juce::AudioFormatManager &manager,
                                           const juce::String &extension) {
  const auto normalizedExtension = extension.trimCharactersAtStart(".");
  for (int i = 0; i < manager.getNumKnownFormats(); ++i) {
    auto *fmt = manager.getKnownFormat(i);
    if (!fmt)
      continue;
    auto exts = fmt->getFileExtensions();
    for (const auto &ext : exts) {
      if (ext.trimCharactersAtStart(".").equalsIgnoreCase(normalizedExtension))
        return fmt;
    }
  }
  return nullptr;
}

// =============================================================================
// ExportSettingsContent — dialog component for choosing export parameters
// =============================================================================

class ExportSettingsContent final : public juce::Component, private juce::Button::Listener {
public:
  ExportSettingsContent(int inputSampleRate,
                        std::function<void(std::optional<ExportSettings>)> done)
      : onDone(std::move(done)) {
    setOpaque(false);

    addAndMakeVisible(title);
    title.setText("Export Settings", juce::dontSendNotification);
    title.setJustificationType(juce::Justification::centredLeft);
    title.setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);
    title.setFont(AppFont::getBoldFont(20.0f));

    setupCombo(formatBox, formatLabel, "Format", {"WAV", "FLAC", "AIFF", "OGG"}, 1);

    int srId = 3;
    if (inputSampleRate >= 47000)
      srId = 4;
    else if (inputSampleRate >= 43000)
      srId = 3;
    else if (inputSampleRate >= 30000)
      srId = 2;
    else
      srId = 1;
    setupCombo(sampleRateBox, sampleRateLabel, "Sample Rate",
               {"22050", "32000", "44100", "48000"}, srId);
    setupCombo(bitDepthBox, bitDepthLabel, "Bit Depth", {"16", "24", "32"}, 1);
    setupCombo(channelsBox, channelsLabel, "Channels", {"Mono", "Stereo"}, 1);

    addAndMakeVisible(cancelButton);
    addAndMakeVisible(exportButton);
    cancelButton.setButtonText("Cancel");
    exportButton.setButtonText("Export");
    configureButton(cancelButton);
    configureButton(exportButton);
    cancelButton.addListener(this);
    exportButton.addListener(this);
  }

  ~ExportSettingsContent() override {
    cancelButton.removeListener(this);
    exportButton.removeListener(this);
    cancelButton.setLookAndFeel(nullptr);
    exportButton.setLookAndFeel(nullptr);
  }

  void paint(juce::Graphics &g) override {
    auto bounds = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xFF232323u));
    g.fillRoundedRectangle(bounds, 10.0f);

    auto card = cardBounds.toFloat();
    g.setColour(juce::Colour(0xFF151515u));
    g.fillRoundedRectangle(card, 8.0f);
    g.setColour(APP_COLOR_BORDER.withAlpha(0.55f));
    g.drawRoundedRectangle(card.reduced(0.5f), 8.0f, 0.75f);
  }

  void resized() override {
    auto area = getLocalBounds().reduced(16);
    title.setBounds(area.removeFromTop(28));
    area.removeFromTop(6);

    cardBounds = area;
    auto content = cardBounds.reduced(16, 12);
    const int rowHeight = 32;
    const int rowGap = 8;
    const int controlWidth = juce::jlimit(150, 190, content.getWidth() / 2);
    const int labelWidth = content.getWidth() - controlWidth - 24;

    auto layoutRow = [&](juce::Label &label, juce::Component &control) {
      auto row = content.removeFromTop(rowHeight);
      label.setBounds(row.removeFromLeft(labelWidth));
      control.setBounds(row.removeFromRight(controlWidth).reduced(0, 2));
      content.removeFromTop(rowGap);
    };

    layoutRow(formatLabel, formatBox);
    layoutRow(sampleRateLabel, sampleRateBox);
    layoutRow(bitDepthLabel, bitDepthBox);
    layoutRow(channelsLabel, channelsBox);

    auto btnRow = content.removeFromBottom(32);
    auto right = btnRow.removeFromRight(190);
    cancelButton.setBounds(right.removeFromLeft(90));
    right.removeFromLeft(10);
    exportButton.setBounds(right);
  }

private:
  void setupCombo(juce::ComboBox &box, juce::Label &label, const juce::String &labelText,
                  const juce::StringArray &items, int selectedId) {
    addAndMakeVisible(label);
    addAndMakeVisible(box);
    label.setText(labelText, juce::dontSendNotification);
    label.setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);
    label.setFont(AppFont::getFont(15.0f));
    label.setJustificationType(juce::Justification::centredLeft);
    box.addItemList(items, 1);
    box.setSelectedId(selectedId, juce::dontSendNotification);
  }

  static void configureButton(juce::TextButton &button) {
    button.setLookAndFeel(&DarkLookAndFeel::getInstance());
    button.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF5B5B5Bu));
    button.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xFF3E3E3Eu));
    button.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFEFEFEFu));
    button.setColour(juce::TextButton::textColourOnId, juce::Colour(0xFFEFEFEFu));
    button.setMouseCursor(juce::MouseCursor::PointingHandCursor);
  }

  ExportSettings getSettings() const {
    ExportSettings settings;
    switch (formatBox.getSelectedId()) {
    case 2:
      settings.format = ExportFormat::flac;
      break;
    case 3:
      settings.format = ExportFormat::aiff;
      break;
    case 4:
      settings.format = ExportFormat::ogg;
      break;
    default:
      settings.format = ExportFormat::wav;
      break;
    }
    settings.sampleRate = sampleRateBox.getText().getIntValue();
    settings.bitsPerSample = bitDepthBox.getText().getIntValue();
    settings.channels = channelsBox.getSelectedId() == 2 ? 2 : 1;
    return settings;
  }

  void buttonClicked(juce::Button *button) override {
    auto closeWith = [this](std::optional<ExportSettings> result) {
      if (onDone)
        onDone(std::move(result));
      if (auto *dw = findParentComponentOfClass<juce::DialogWindow>())
        dw->exitModalState(0);
    };

    if (button == &exportButton)
      closeWith(getSettings());
    else if (button == &cancelButton)
      closeWith(std::nullopt);
  }

  std::function<void(std::optional<ExportSettings>)> onDone;
  juce::Rectangle<int> cardBounds;
  juce::Label title;
  juce::Label formatLabel, sampleRateLabel, bitDepthLabel, channelsLabel;
  StyledComboBox formatBox, sampleRateBox, bitDepthBox, channelsBox;
  juce::TextButton cancelButton, exportButton;
};

void showExportSettingsDialogAsync(
    juce::Component *parent, int inputSampleRate,
    std::function<void(std::optional<ExportSettings>)> onDone) {
  auto *content = new ExportSettingsContent(inputSampleRate, std::move(onDone));
  content->setSize(420, 290);

  juce::DialogWindow::LaunchOptions opts;
  opts.content.setOwned(content);
  opts.dialogTitle = "Export Settings";
  opts.componentToCentreAround = parent;
  opts.dialogBackgroundColour = APP_COLOR_SURFACE;
  opts.escapeKeyTriggersCloseButton = true;
  opts.useNativeTitleBar = false;
  opts.resizable = false;
  opts.useBottomRightCornerResizer = false;
  if (auto *window = opts.launchAsync())
    window->setTitleBarHeight(0);
}

} // namespace ExportHelper
