#pragma once

#include "../JuceHeader.h"

/**
 * Platform-specific path utilities.
 *
 * macOS:
 *   - Models: /Library/Application Support/<company>/<app>/models/
 *   - Logs: ~/Library/Logs/PitchNet/
 *   - Config: ~/Library/Application Support/PitchNet/
 *
 * Windows:
 *   - Models: %PROGRAMDATA%/<company>/<app>/models/
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
    inline juce::File getProjectResourcesDirectory()
    {
#if JUCE_DEBUG
#ifdef PITCHNET_PROJECT_RESOURCES_DIR
        auto configuredResources = juce::File(juce::String(PITCHNET_PROJECT_RESOURCES_DIR));
        if (configuredResources.isDirectory())
            return configuredResources;
#endif

        auto sourceRelativeProbe = juce::File(__FILE__)
                                       .getParentDirectory()
                                       .getParentDirectory()
                                       .getParentDirectory()
                                       .getChildFile("Resources");
        if (sourceRelativeProbe.isDirectory())
            return sourceRelativeProbe;

        auto cwdProbe = juce::File::getCurrentWorkingDirectory()
                            .getChildFile("Resources");
        if (cwdProbe.isDirectory())
            return cwdProbe;

        return {};
#else
        return juce::File::getSpecialLocation(juce::File::commonApplicationDataDirectory)
#if JUCE_MAC
            .getChildFile("Application Support")
#endif
            .getChildFile("Session Loops")
            .getChildFile("PitchNet");
#endif
    }

    inline juce::File getApplicationFile()
    {
        return juce::File::getSpecialLocation(juce::File::currentApplicationFile);
    }

    inline juce::File getDefaultLocalResourcesModelsDirectory()
    {
#if JUCE_MAC
        return getApplicationFile().getChildFile("Contents/Resources/models");
#else
        auto dir = getApplicationFile().getParentDirectory();
        const auto initialDir = dir;

        for (int i = 0; i < 8 && dir.exists(); ++i)
        {
            if (dir.getFileName().equalsIgnoreCase("Contents"))
                return dir.getChildFile("Resources/models");

            auto parent = dir.getParentDirectory();
            if (parent == dir)
                break;
            dir = parent;
        }

        return initialDir.getChildFile("Resources/models");
#endif
    }

    inline juce::File findLocalResourcesModelsDirectory()
    {
        auto isValid = [](const juce::File &candidate)
        {
            return candidate.isDirectory();
        };

        auto projectResources = getProjectResourcesDirectory();
        auto projectModels = projectResources.getChildFile("models");
        if (isValid(projectModels))
            return projectModels;

        auto dir = getApplicationFile().getParentDirectory();
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
        auto localResourcesModels = findLocalResourcesModelsDirectory();
        if (localResourcesModels.isDirectory())
            return localResourcesModels;

        return getDefaultLocalResourcesModelsDirectory();
    }

    inline juce::File getModelFile(const juce::String &fileName)
    {
        auto probe = getModelsDirectory().getChildFile(fileName);
        if (probe.exists())
            return probe;

        // Development fallback: <repo>/Resources/models/
        auto cwdProbe = juce::File::getCurrentWorkingDirectory()
                            .getChildFile("Resources/models")
                            .getChildFile(fileName);
        if (cwdProbe.exists())
            return cwdProbe;

        // Walk up from executable directory and probe both:
        //   <dir>/models/<file>
        //   <dir>/Resources/models/<file>
        auto dir = getApplicationFile().getParentDirectory();
        for (int i = 0; i < 8 && dir.exists(); ++i)
        {
            auto modelsCandidate = dir.getChildFile("models").getChildFile(fileName);
            if (modelsCandidate.exists())
                return modelsCandidate;

            auto resourcesCandidate = dir.getChildFile("Resources/models").getChildFile(fileName);
            if (resourcesCandidate.exists())
                return resourcesCandidate;

            auto parent = dir.getParentDirectory();
            if (parent == dir)
                break;
            dir = parent;
        }

        // Default path used in production packaging.
        return probe;
    }

    inline juce::File getVocoderModelFile()
    {
#if JUCE_MAC
        auto coreMLModel = getModelFile("pc_nsf_hifigan.mlmodelc");
        if (coreMLModel.isDirectory())
            return coreMLModel;
#endif
        return getModelFile("pc_nsf_hifigan.onnx");
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

        auto dir = getApplicationFile().getParentDirectory();
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
