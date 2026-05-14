#include "SettingsManager.h"
#include "../../Utils/AppLogger.h"

SettingsManager::SettingsManager()
{
  loadSettings();
  loadConfig();
}

juce::File SettingsManager::getSettingsFile()
{
  return juce::File::getSpecialLocation(
             juce::File::userApplicationDataDirectory)
      .getChildFile("PitchNet")
      .getChildFile("settings.xml");
}

juce::File SettingsManager::getConfigFile()
{
  return PlatformPaths::getConfigFile("config.json");
}

void SettingsManager::loadSettings()
{
  auto configFile = getConfigFile();
  if (configFile.existsAsFile())
    return;

  auto settingsFile = getSettingsFile();
  if (!settingsFile.existsAsFile())
    return;

  auto xml = juce::XmlDocument::parse(settingsFile);
  if (xml == nullptr)
    return;

  device = xml->getStringAttribute("device", device);
  threads = xml->getIntAttribute("threads", threads);

  juce::String pitchDetectorStr = xml->getStringAttribute(
      "pitchDetector", pitchDetectorTypeToString(pitchDetectorType));
  pitchDetectorType = stringToPitchDetectorType(pitchDetectorStr);

  gpuDeviceId = xml->getIntAttribute("gpuDeviceId", gpuDeviceId);
  language = xml->getStringAttribute("language", language);

  saveConfig();
}

void SettingsManager::applySettings()
{
  loadConfig();

  if (vocoder)
  {
    vocoder->setExecutionDevice(device);
    vocoder->setExecutionDeviceId(gpuDeviceId);
    if (vocoder->isLoaded())
      vocoder->reloadModel();
  }

  if (onSettingsChanged)
    onSettingsChanged();
}

void SettingsManager::loadConfig()
{
  auto configFile = getConfigFile();

  if (configFile.existsAsFile())
  {
    auto configText = configFile.loadFileAsString();
    auto config = juce::JSON::parse(configText);

    if (config.isObject())
    {
      auto configObj = config.getDynamicObject();
      if (configObj)
      {
        if (configObj->hasProperty("device"))
          device = configObj->getProperty("device").toString();

        if (configObj->hasProperty("threads"))
          threads = static_cast<int>(configObj->getProperty("threads"));

        if (configObj->hasProperty("pitchDetector"))
        {
          auto pitchDetectorStr =
              configObj->getProperty("pitchDetector").toString();
          pitchDetectorType = stringToPitchDetectorType(pitchDetectorStr);
        }

        if (configObj->hasProperty("gpuDeviceId"))
          gpuDeviceId = static_cast<int>(configObj->getProperty("gpuDeviceId"));

        if (configObj->hasProperty("language"))
          language = configObj->getProperty("language").toString();

        auto lastFile = configObj->getProperty("lastFile").toString();
        if (lastFile.isNotEmpty())
          lastFilePath = juce::File(lastFile);

        if (configObj->hasProperty("recentFiles"))
        {
          auto recentVar = configObj->getProperty("recentFiles");
          if (recentVar.isArray())
          {
            recentFiles.clear();
            for (int i = 0; i < recentVar.size(); ++i)
            {
              auto path = recentVar[i].toString();
              if (path.isNotEmpty())
                recentFiles.add(path);
            }
          }
        }

        if (configObj->hasProperty("windowWidth"))
          windowWidth = static_cast<int>(configObj->getProperty("windowWidth"));
        if (configObj->hasProperty("windowHeight"))
          windowHeight =
              static_cast<int>(configObj->getProperty("windowHeight"));
        if (configObj->hasProperty("showDeltaPitch"))
          showDeltaPitch =
              static_cast<bool>(configObj->getProperty("showDeltaPitch"));
        if (configObj->hasProperty("showBasePitch"))
          showBasePitch =
              static_cast<bool>(configObj->getProperty("showBasePitch"));
        if (configObj->hasProperty("showSegmentsDebug"))
          showSegmentsDebug =
              static_cast<bool>(configObj->getProperty("showSegmentsDebug"));
        if (configObj->hasProperty("showGameValuesDebug"))
          showGameValuesDebug =
              static_cast<bool>(configObj->getProperty("showGameValuesDebug"));
        // Backward compatibility: read old key name
        else if (configObj->hasProperty("showSomeValuesDebug"))
          showGameValuesDebug =
              static_cast<bool>(configObj->getProperty("showSomeValuesDebug"));
        if (configObj->hasProperty("showUvInterpolationDebug"))
          showUvInterpolationDebug =
              static_cast<bool>(configObj->getProperty("showUvInterpolationDebug"));
        if (configObj->hasProperty("showActualF0Debug"))
          showActualF0Debug =
              static_cast<bool>(configObj->getProperty("showActualF0Debug"));
        if (configObj->hasProperty("followSystemAudioOutput"))
          followSystemAudioOutput =
              static_cast<bool>(configObj->getProperty("followSystemAudioOutput"));
        if (configObj->hasProperty("preferredAudioOutputDevice"))
          preferredAudioOutputDevice =
              configObj->getProperty("preferredAudioOutputDevice").toString();
      }
    }
  }
}

void SettingsManager::saveConfig()
{
  auto configFile = getConfigFile();

  juce::DynamicObject::Ptr config = new juce::DynamicObject();

  config->setProperty("device", device);
  config->setProperty("threads", threads);
  config->setProperty("pitchDetector",
                      pitchDetectorTypeToString(pitchDetectorType));
  config->setProperty("gpuDeviceId", gpuDeviceId);
  config->setProperty("language", language);

  if (lastFilePath.existsAsFile())
    config->setProperty("lastFile", lastFilePath.getFullPathName());

  if (!recentFiles.isEmpty())
  {
    juce::Array<juce::var> recentArray;
    for (const auto &path : recentFiles)
      recentArray.add(path);
    config->setProperty("recentFiles", recentArray);
  }

  config->setProperty("windowWidth", windowWidth);
  config->setProperty("windowHeight", windowHeight);
  config->setProperty("showDeltaPitch", showDeltaPitch);
  config->setProperty("showBasePitch", showBasePitch);
  config->setProperty("showSegmentsDebug", showSegmentsDebug);
  config->setProperty("showGameValuesDebug", showGameValuesDebug);
  config->setProperty("showUvInterpolationDebug", showUvInterpolationDebug);
  config->setProperty("showActualF0Debug", showActualF0Debug);
  config->setProperty("followSystemAudioOutput", followSystemAudioOutput);
  config->setProperty("preferredAudioOutputDevice", preferredAudioOutputDevice);

  juce::String jsonText = juce::JSON::toString(juce::var(config.get()));
  configFile.replaceWithText(jsonText);
}
