#include "PlatformUtils.h"

// NOTE: use __APPLE__, not JUCE_MAC. This translation unit only includes
// PlatformUtils.h, which pulls in no JUCE headers, so JUCE_MAC is undefined
// here and the whole macOS implementation used to compile out to the no-op
// stubs below - leaving the standalone window with stock light title bar
// chrome. This file is Objective-C++ and is only ever compiled on Apple
// platforms, so __APPLE__ is the correct guard.
#if defined(__APPLE__)
#import <Cocoa/Cocoa.h>

namespace PlatformUtils
{
    namespace
    {
        NSWindow* getWindowFromNativeHandle(void* nativeHandle)
        {
            if (nativeHandle == nullptr)
                return nil;

            id nativeObject = (__bridge id)nativeHandle;

            if ([nativeObject isKindOfClass:[NSWindow class]])
                return static_cast<NSWindow*>(nativeObject);

            if ([nativeObject isKindOfClass:[NSView class]])
                return [static_cast<NSView*>(nativeObject) window];

            return nil;
        }

        NSColor* titleBarColour()
        {
            return [NSColor colorWithCalibratedRed:35.0 / 255.0
                                            green:35.0 / 255.0
                                             blue:35.0 / 255.0
                                            alpha:1.0];
        }
    }

    void setDarkAppearance(void* nativeHandle)
    {
        if (NSWindow* window = getWindowFromNativeHandle(nativeHandle))
        {
            window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
        }
    }

    void configureStandaloneTitleBar(void* nativeHandle)
    {
        if (NSWindow* window = getWindowFromNativeHandle(nativeHandle))
        {
            window.title = @"";
            window.titleVisibility = NSWindowTitleHidden;
            window.titlebarAppearsTransparent = YES;
            window.backgroundColor = titleBarColour();
        }
    }
}

#else

namespace PlatformUtils
{
    void setDarkAppearance(void*) {}
    void configureStandaloneTitleBar(void*) {}
}

#endif
