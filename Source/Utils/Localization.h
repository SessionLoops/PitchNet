#pragma once

#include "../JuceHeader.h"
#include "PlatformPaths.h"
#include <functional>
#include <map>
#include <vector>

#if __has_include("BinaryData.h")
#include "BinaryData.h"
#define PITCHNET_HAS_BINARYDATA 1
#else
#define PITCHNET_HAS_BINARYDATA 0
#endif

/**
 * Application-wide string table.
 *
 * Every user-visible string lives in Resources/lang/<code>.json and is reached
 * through the TR() macro. All of those files are compiled into the binary, so
 * a language works even when the Resources folder did not ship with the build;
 * a file on disk with the same name still wins, which keeps the edit-and-rerun
 * loop working for translators.
 *
 * Localization is a ChangeBroadcaster: components that hold text register as
 * listeners and re-apply their strings when the language changes, so switching
 * language takes effect without a restart.
 */
class Localization : public juce::ChangeBroadcaster {
public:
  static Localization &getInstance() {
    static Localization instance;
    return instance;
  }

  struct LangInfo {
    juce::String code;
    juce::String nativeName;
  };

  /** Switches to an explicit language. "auto" follows the system language. */
  void setLanguage(const juce::String &langCode) {
    if (langCode == "auto") {
      useSystemLanguage();
      return;
    }

    if (!languages.count(langCode))
      return;

    followSystem = false;
    if (currentLang == langCode)
      return;

    loadLanguageFile(langCode);
    sendChangeMessage();
  }

  /** Follows whatever language the OS reports. */
  void useSystemLanguage() {
    const auto detected = detectSystemLanguageCode();
    const bool wasFollowing = followSystem;
    followSystem = true;

    if (currentLang == detected && wasFollowing)
      return;

    loadLanguageFile(detected);
    sendChangeMessage();
  }

  juce::String getLanguage() const { return currentLang; }

  /** True while the language tracks the OS rather than an explicit choice. */
  bool isFollowingSystemLanguage() const { return followSystem; }

  /** The code to persist: "auto" while following the system. */
  juce::String getPersistedLanguageCode() const {
    return followSystem ? juce::String("auto") : currentLang;
  }

  juce::String get(const juce::String &key) const {
    auto it = strings.find(key);
    if (it != strings.end())
      return it->second;
    auto enIt = englishStrings.find(key);
    if (enIt != englishStrings.end())
      return enIt->second;
    // A key with no entry anywhere reads better as itself than as a
    // placeholder: it says which string is missing instead of hiding it.
    return key;
  }

  const std::vector<LangInfo> &getAvailableLanguages() const {
    return availableLanguages;
  }

  /** Maps the OS language to one of the codes this build ships. */
  static juce::String detectSystemLanguageCode() {
    const auto locale = juce::SystemStats::getUserLanguage();
    const auto region = juce::SystemStats::getUserRegion();

    if (locale.startsWithIgnoreCase("zh")) {
      // Traditional Chinese is spelled several ways depending on platform:
      // the script subtag, the region subtag, or the region on its own.
      if (locale.containsIgnoreCase("Hant") || locale.containsIgnoreCase("TW") ||
          locale.containsIgnoreCase("HK") || locale.containsIgnoreCase("MO") ||
          region.equalsIgnoreCase("TW") || region.equalsIgnoreCase("HK") ||
          region.equalsIgnoreCase("MO"))
        return "zh-TW";
      return "zh";
    }

    if (locale.startsWithIgnoreCase("ja"))
      return "ja";
    if (locale.startsWithIgnoreCase("ko"))
      return "ko";
    if (locale.startsWithIgnoreCase("es"))
      return "es";

    return "en";
  }

  static void detectSystemLanguage() { getInstance().useSystemLanguage(); }

  /** Applies the saved language. Call before the UI is built. */
  static void loadFromSettings() {
    auto configFile = PlatformPaths::getConfigFile("config.json");
    if (configFile.existsAsFile()) {
      auto jsonText = configFile.loadFileAsString();
      auto json = juce::JSON::parse(jsonText);
      if (auto *obj = json.getDynamicObject()) {
        auto langCode = obj->getProperty("language").toString();
        if (langCode.isNotEmpty()) {
          getInstance().applyStoredCode(langCode);
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
        getInstance().applyStoredCode(
            xml->getStringAttribute("language", "auto"));
        return;
      }
    }

    // Nothing saved yet: the system language is the default.
    getInstance().useSystemLanguage();
  }

  void scanAvailableLanguages() {
    availableLanguages.clear();
    languages.clear();

    for (const auto &code : getKnownLanguageCodes()) {
      std::map<juce::String, juce::String> table;
      const bool hasBinary = loadLanguageMapFromBinaryData(code, table);
      auto langFile = findLanguageFile(code);
      if (langFile.existsAsFile())
        loadLanguageMapFromFile(langFile, table);

      if (!hasBinary && !langFile.existsAsFile())
        continue;

      juce::String nativeName = getFallbackNativeName(code);
      auto it = table.find("lang." + code);
      if (it != table.end() && it->second.isNotEmpty())
        nativeName = it->second;

      availableLanguages.push_back({code, nativeName});
      languages[code] = nativeName;
    }
  }

  /** The codes this build knows about, in the order the UI lists them. */
  static const std::vector<juce::String> &getKnownLanguageCodes() {
    static const std::vector<juce::String> codes = {"en",    "es", "ja",
                                                    "ko",    "zh", "zh-TW"};
    return codes;
  }

private:
  Localization() {
    loadEnglishBase();
    scanAvailableLanguages();
    loadLanguageFile("en");
  }

  /** Applies a code read from disk, without broadcasting (nothing listens yet). */
  void applyStoredCode(const juce::String &langCode) {
    if (langCode.isEmpty() || langCode == "auto") {
      followSystem = true;
      loadLanguageFile(detectSystemLanguageCode());
      return;
    }

    followSystem = false;
    if (languages.count(langCode))
      loadLanguageFile(langCode);
    else
      loadLanguageFile("en");
  }

  static juce::String getFallbackNativeName(const juce::String &code) {
    if (code == "en")
      return juce::String("English");
    if (code == "es")
      return juce::String::fromUTF8("Espa\xc3\xb1ol");
    if (code == "ja")
      return juce::String::fromUTF8("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e");
    if (code == "ko")
      return juce::String::fromUTF8("\xed\x95\x9c\xea\xb5\xad\xec\x96\xb4");
    if (code == "zh")
      return juce::String::fromUTF8("\xe7\xae\x80\xe4\xbd\x93\xe4\xb8\xad\xe6\x96\x87");
    if (code == "zh-TW")
      return juce::String::fromUTF8("\xe7\xb9\x81\xe9\xab\x94\xe4\xb8\xad\xe6\x96\x87");
    return code;
  }

  void loadLanguageFile(const juce::String &langCode) {
    strings = englishStrings;

    std::map<juce::String, juce::String> table;
    const bool hasBinary = loadLanguageMapFromBinaryData(langCode, table);

    auto langFile = findLanguageFile(langCode);
    if (langFile.existsAsFile())
      loadLanguageMapFromFile(langFile, table);

    if (!hasBinary && !langFile.existsAsFile()) {
      currentLang = "en";
      return;
    }

    for (const auto &entry : table)
      strings[entry.first] = entry.second;

    currentLang = langCode;
  }

  void loadEnglishBase() {
    englishStrings.clear();
    loadLanguageMapFromBinaryData("en", englishStrings);

    auto enFile = findLanguageFile("en");
    if (enFile.existsAsFile())
      loadLanguageMapFromFile(enFile, englishStrings);
  }

  /** "zh-TW" -> "zh_TW_json", the name juce_add_binary_data generates. */
  static juce::String getBinaryResourceName(const juce::String &langCode) {
    return langCode.replaceCharacter('-', '_') + "_json";
  }

  static bool loadLanguageMapFromBinaryData(
      const juce::String &langCode,
      std::map<juce::String, juce::String> &target) {
#if PITCHNET_HAS_BINARYDATA
    int dataSize = 0;
    const auto resourceName = getBinaryResourceName(langCode);
    if (const char *data =
            BinaryData::getNamedResource(resourceName.toRawUTF8(), dataSize)) {
      if (dataSize > 0) {
        loadLanguageMapFromJsonText(juce::String::fromUTF8(data, dataSize),
                                    target);
        return true;
      }
    }
#else
    juce::ignoreUnused(langCode, target);
#endif
    return false;
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

    auto projectResources = PlatformPaths::getProjectResourcesDirectory();
    auto projectFile = projectResources.getChildFile("lang").getChildFile(fileName);
    if (projectFile.existsAsFile())
      return projectFile;

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
  bool followSystem = true;
  std::map<juce::String, juce::String> strings;
  std::map<juce::String, juce::String> englishStrings;
  std::map<juce::String, juce::String> languages;
  std::vector<LangInfo> availableLanguages;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Localization)
};

#define TR(key) Localization::getInstance().get(key)

/**
 * Holds a callback that runs whenever the language changes.
 *
 * Components keep one as a member (declared last, so it is torn down first)
 * rather than inheriting a listener, which keeps it usable by classes that
 * already listen to something else:
 *
 *     LocalisationWatcher languageWatcher { [this] { refreshLocalisedText(); } };
 */
class LocalisationWatcher : private juce::ChangeListener {
public:
  explicit LocalisationWatcher(std::function<void()> onLanguageChanged)
      : callback(std::move(onLanguageChanged)) {
    Localization::getInstance().addChangeListener(this);
  }

  ~LocalisationWatcher() override {
    Localization::getInstance().removeChangeListener(this);
  }

private:
  void changeListenerCallback(juce::ChangeBroadcaster *) override {
    if (callback)
      callback();
  }

  std::function<void()> callback;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LocalisationWatcher)
};
