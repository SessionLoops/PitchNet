#pragma once

// Platform-specific window utilities
namespace PlatformUtils
{
    // Set macOS window to dark appearance
    void setDarkAppearance(void* nativeHandle);

    // Configure the standalone macOS title bar chrome.
    void configureStandaloneTitleBar(void* nativeHandle);
}
