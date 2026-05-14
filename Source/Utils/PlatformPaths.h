#pragma once

#include "../JuceHeader.h"

/**
 * Platform-specific path utilities.
 *
 * macOS:
 *   - Models: App.app/Contents/Resources/models/
 *   - Logs: ~/Library/Logs/PitchNet/
 *   - Config: ~/Library/Application Support/PitchNet/
 *
 * Windows:
 *   - Models: <exe_dir>/models/
 *   - Logs: %APPDATA%/PitchNet/Logs/
 *   - Config: %APPDATA%/PitchNet/
 *
 * Linux:
 *   - Models: <exe_dir>/models/
 *   - Logs: ~/.config/PitchNet/logs/
 *   - Config: ~/.config/PitchNet/
 */
namespace PlatformPaths
{
    inline juce::File findLocalResourcesModelsDirectory()
    {
        auto isValid = [](const juce::File &candidate)
        {
            return candidate.isDirectory();
        };

        auto sourceRelativeProbe = juce::File(__FILE__)
                                       .getParentDirectory()
                                       .getParentDirectory()
                                       .getParentDirectory()
                                       .getChildFile("Resources/models");
        if (isValid(sourceRelativeProbe))
            return sourceRelativeProbe;

        auto cwdProbe = juce::File::getCurrentWorkingDirectory()
                            .getChildFile("Resources/models");
        if (isValid(cwdProbe))
            return cwdProbe;

        auto dir = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                       .getParentDirectory();
        for (int i = 0; i < 8 && dir.exists(); ++i)
        {
            auto resourcesCandidate = dir.getChildFile("Resources/models");
            if (isValid(resourcesCandidate))
                return resourcesCandidate;

            auto parent = dir.getParentDirectory();
            if (parent == dir)
                break;
            dir = parent;
        }

        return {};
    }

    inline juce::File getModelsDirectory()
    {
#if JUCE_DEBUG
        // Debug builds prefer the local repo Resources folder so model edits do
        // not require re-copying assets into the app/plugin bundle.
        auto localResourcesModels = findLocalResourcesModelsDirectory();
        if (localResourcesModels.isDirectory())
            return localResourcesModels;
#endif

#if JUCE_MAC
        // macOS: Use Resources folder inside app bundle
        auto appBundle = juce::File::getSpecialLocation(juce::File::currentApplicationFile);
        return appBundle.getChildFile("Contents/Resources/models");
#else
        // Windows/Linux: Use models folder next to executable
        return juce::File::getSpecialLocation(juce::File::currentExecutableFile)
            .getParentDirectory()
            .getChildFile("models");
#endif
    }

    inline juce::File getModelFile(const juce::String &fileName)
    {
        auto probe = getModelsDirectory().getChildFile(fileName);
        if (probe.existsAsFile())
            return probe;

        // Development fallback: <repo>/Resources/models/
        auto cwdProbe = juce::File::getCurrentWorkingDirectory()
                            .getChildFile("Resources/models")
                            .getChildFile(fileName);
        if (cwdProbe.existsAsFile())
            return cwdProbe;

        // Walk up from executable directory and probe both:
        //   <dir>/models/<file>
        //   <dir>/Resources/models/<file>
        auto dir = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                       .getParentDirectory();
        for (int i = 0; i < 8 && dir.exists(); ++i)
        {
            auto modelsCandidate = dir.getChildFile("models").getChildFile(fileName);
            if (modelsCandidate.existsAsFile())
                return modelsCandidate;

            auto resourcesCandidate = dir.getChildFile("Resources/models").getChildFile(fileName);
            if (resourcesCandidate.existsAsFile())
                return resourcesCandidate;

            auto parent = dir.getParentDirectory();
            if (parent == dir)
                break;
            dir = parent;
        }

        // Default path used in production packaging.
        return probe;
    }

    inline juce::File getModelSubDir(const juce::String &dirName,
                                     const juce::String &verifyFile = "")
    {
        // Helper: check if candidate dir is valid
        auto isValid = [&](const juce::File &candidate) -> bool
        {
            if (!candidate.isDirectory())
                return false;
            if (verifyFile.isEmpty())
                return true;
            return candidate.getChildFile(verifyFile).existsAsFile();
        };

        auto probe = getModelsDirectory().getChildFile(dirName);
        if (isValid(probe))
            return probe;

        auto cwdProbe = juce::File::getCurrentWorkingDirectory()
                            .getChildFile("Resources/models")
                            .getChildFile(dirName);
        if (isValid(cwdProbe))
            return cwdProbe;

        auto dir = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                       .getParentDirectory();
        for (int i = 0; i < 8 && dir.exists(); ++i)
        {
            auto modelsCandidate = dir.getChildFile("models").getChildFile(dirName);
            if (isValid(modelsCandidate))
                return modelsCandidate;

            auto resourcesCandidate = dir.getChildFile("Resources/models").getChildFile(dirName);
            if (isValid(resourcesCandidate))
                return resourcesCandidate;

            auto parent = dir.getParentDirectory();
            if (parent == dir)
                break;
            dir = parent;
        }

        return probe;
    }

    inline juce::File getLogsDirectory()
    {
#if JUCE_MAC
        // macOS: ~/Library/Logs/PitchNet/
        return juce::File::getSpecialLocation(juce::File::userHomeDirectory)
            .getChildFile("Library/Logs/PitchNet");
#elif JUCE_WINDOWS
        // Windows: %APPDATA%/PitchNet/Logs/
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("PitchNet/Logs");
#else
        // Linux: ~/.config/PitchNet/logs/
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("PitchNet/logs");
#endif
    }

    inline juce::File getConfigDirectory()
    {
        // All platforms use userApplicationDataDirectory
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("PitchNet");
    }

    inline juce::File getLogFile(const juce::String &name)
    {
        auto logsDir = getLogsDirectory();
        logsDir.createDirectory();
        return logsDir.getChildFile(name);
    }

    inline juce::File getConfigFile(const juce::String &name)
    {
        auto configDir = getConfigDirectory();
        configDir.createDirectory();
        return configDir.getChildFile(name);
    }
}
