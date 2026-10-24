#include "SvgUtils.h"

namespace SvgUtils
{
    void tintDrawable(juce::Drawable* drawable, juce::Colour colour)
    {
        if (!drawable)
            return;

        // Replace black (default currentColor) with target colour
        drawable->replaceColour(juce::Colours::black, colour);

        // Handle DrawablePath directly
        if (auto* path = dynamic_cast<juce::DrawablePath*>(drawable))
        {
            path->setFill(colour);
            return;
        }

        // Handle composite drawables recursively
        if (auto* composite = dynamic_cast<juce::DrawableComposite*>(drawable))
        {
            for (int i = 0; i < composite->getNumChildren(); ++i)
                tintDrawable(&composite->getChild(i), colour);
        }
    }

    std::unique_ptr<juce::Drawable> loadSvg(const char* data, int size)
    {
        return juce::Drawable::createFromImageData(data, static_cast<size_t>(size));
    }

    std::unique_ptr<juce::Drawable> loadSvg(const char* data, int size, juce::Colour tintColour)
    {
        auto svg = loadSvg(data, size);
        if (svg)
            tintDrawable(svg.get(), tintColour);
        return svg;
    }

    std::unique_ptr<juce::Drawable> createDrawableFromSvg(const juce::String& svgString)
    {
        return juce::Drawable::createFromSVGString(svgString);
    }

    std::unique_ptr<juce::Drawable> createDrawableFromSvg(const juce::String& svgString, juce::Colour tintColour)
    {
        auto svg = createDrawableFromSvg(svgString);
        if (svg)
            tintDrawable(svg.get(), tintColour);
        return svg;
    }
}
