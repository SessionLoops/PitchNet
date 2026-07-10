#include <windows.h>

namespace juce
{
    double getScaleFactorForWindow(HWND h)
    {
        return static_cast<double>(GetDpiForWindow(h)) / USER_DEFAULT_SCREEN_DPI;
    }
}
