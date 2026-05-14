#pragma once

#include "../JuceHeader.h"
#include "PlatformPaths.h"
#include <map>
#include <vector>

#if __has_include("BinaryData.h")
#include "BinaryData.h"
#define PITCHNET_HAS_BINARYDATA 1
#else
#define PITCHNET_HAS_BINARYDATA 0
#endif

class Localization {
public:
  static Localization &getInstance() {
    static Localization instance;
    return instance;
  }

  struct LangInfo {
    juce::String code;
    juce::String nativeName;
  };

  void setLanguage(const juce::String &langCode) {
    if (languages.count(langCode)) {
      currentLang = langCode;
      loadLanguageFile(langCode);
    }
  }

  juce::String getLanguage() const { return currentLang; }

  juce::String get(const juce::String &key) const {
    auto it = strings.find(key);
    if (it != strings.end())
      return it->second;
    auto enIt = englishStrings.find(key);
    if (enIt != englishStrings.end())
      return enIt->second;
    return "Missing translation";
  }

  const std::vector<LangInfo> &getAvailableLanguages() const {
    return availableLanguages;
  }

  static void detectSystemLanguage() {
    auto &inst = getInstance();
    auto locale = juce::SystemStats::getUserLanguage();

    juce::String langCode = "en";
    if (locale.startsWith("zh-TW") || locale.startsWith("zh_TW") ||
        locale.startsWith("zh-Hant"))
      langCode = "zh-TW";
    else if (locale.startsWith("zh"))
      langCode = "zh";
    else if (locale.startsWith("ja"))
      langCode = "ja";

    inst.setLanguage(langCode);
  }

  // Load language from saved settings (call before UI creation)
  static void loadFromSettings() {
    auto configFile = PlatformPaths::getConfigFile("config.json");
    if (configFile.existsAsFile()) {
      auto jsonText = configFile.loadFileAsString();
      auto json = juce::JSON::parse(jsonText);
      if (auto *obj = json.getDynamicObject()) {
        auto langCode = obj->getProperty("language").toString();
        if (langCode.isNotEmpty()) {
          if (langCode == "auto")
            detectSystemLanguage();
          else
            getInstance().setLanguage(langCode);
          return;
        }
      }
    }

    auto settingsFile =
        juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("PitchNet")
            .getChildFile("settings.xml");

    if (settingsFile.existsAsFile()) {
      auto xml = juce::XmlDocument::parse(settingsFile);
      if (xml != nullptr) {
        juce::String langCode = xml->getStringAttribute("language", "auto");
        if (langCode == "auto")
          detectSystemLanguage();
        else
          getInstance().setLanguage(langCode);
        return;
      }
    }

    detectSystemLanguage();
  }

  void scanAvailableLanguages() {
    availableLanguages.clear();
    languages.clear();
    std::vector<juce::String> knownCodes = {"en", "zh", "zh-TW", "ja"};
    auto defaultNativeName = [](const juce::String &code) {
      if (code == "en")
        return juce::String("English");
      if (code == "zh")
        return juce::String::fromUTF8(u8"\u7b80\u4f53\u4e2d\u6587");
      if (code == "zh-TW")
        return juce::String::fromUTF8(u8"\u7e41\u9ad4\u4e2d\u6587");
      if (code == "ja")
        return juce::String::fromUTF8(u8"\u65e5\u672c\u8a9e");
      return code;
    };

    for (const auto &code : knownCodes) {
      auto langFile = findLanguageFile(code);
      const bool hasEmbeddedEnglish = (code == "en" && !englishStrings.empty());
      if (!langFile.existsAsFile() && !hasEmbeddedEnglish)
        continue;

      juce::String nativeName = defaultNativeName(code);
      if (code == "en") {
        auto it = englishStrings.find("lang.en");
        if (it != englishStrings.end() && it->second.isNotEmpty())
          nativeName = it->second;
      } else if (langFile.existsAsFile()) {
        std::map<juce::String, juce::String> tmp;
        loadLanguageMapFromFile(langFile, tmp);
        auto it = tmp.find("lang." + code);
        if (it != tmp.end() && it->second.isNotEmpty())
          nativeName = it->second;
      }

      availableLanguages.push_back({code, nativeName});
      languages[code] = nativeName;
    }
  }

private:
  Localization() {
    loadEnglishBase();
    scanAvailableLanguages();
    loadLanguageFile("en");
  }

  void loadLanguageFile(const juce::String &langCode) {
    strings = englishStrings;

    auto langFile = findLanguageFile(langCode);
    if (!langFile.existsAsFile()) {
      currentLang = "en";
      return;
    }

    loadLanguageMapFromFile(langFile, strings);

    currentLang = langCode;
  }

  void loadEnglishBase() {
    englishStrings.clear();

#if PITCHNET_HAS_BINARYDATA
    loadLanguageMapFromJsonText(
        juce::String::fromUTF8(BinaryData::en_json, BinaryData::en_jsonSize),
        englishStrings);
#endif

    auto enFile = findLanguageFile("en");
    if (enFile.existsAsFile())
      loadLanguageMapFromFile(enFile, englishStrings);
  }

  static void loadLanguageMapFromFile(
      const juce::File &file, std::map<juce::String, juce::String> &target) {
    auto jsonText = file.loadFileAsString();
    loadLanguageMapFromJsonText(jsonText, target);
  }

  static void loadLanguageMapFromJsonText(
      const juce::String &jsonText,
      std::map<juce::String, juce::String> &target) {
    auto json = juce::JSON::parse(jsonText);
    if (auto *obj = json.getDynamicObject()) {
      for (const auto &prop : obj->getProperties())
        target[prop.name.toString()] = prop.value.toString();
    }
  }

  juce::File findLanguageFile(const juce::String &langCode) {
    auto fileName = langCode + ".json";

#if JUCE_MAC
    auto bundleDir =
        juce::File::getSpecialLocation(juce::File::currentApplicationFile);
    auto resourceFile = bundleDir.getChildFile("Contents/Resources/lang")
                            .getChildFile(fileName);
    if (resourceFile.existsAsFile())
      return resourceFile;
#endif

    auto exeDir =
        juce::File::getSpecialLocation(juce::File::currentExecutableFile)
            .getParentDirectory();
    auto exeFile = exeDir.getChildFile("lang").getChildFile(fileName);
    if (exeFile.existsAsFile())
      return exeFile;

    auto cwdFile = juce::File::getCurrentWorkingDirectory()
                       .getChildFile("Resources/lang")
                       .getChildFile(fileName);
    if (cwdFile.existsAsFile())
      return cwdFile;

    return {};
  }

  juce::String currentLang = "en";
  std::map<juce::String, juce::String> strings;
  std::map<juce::String, juce::String> englishStrings;
  std::map<juce::String, juce::String> languages;
  std::vector<LangInfo> availableLanguages;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Localization)
};

#define TR(key) Localization::getInstance().get(key)

