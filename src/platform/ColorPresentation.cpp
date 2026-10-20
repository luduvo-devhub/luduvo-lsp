#include "Platform/ColorPresentation.hpp"

#include "LSP/ColorProvider.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace
{
std::string formatDouble(double value)
{
    std::string result = std::to_string(value);
    const size_t decimal = result.find('.');
    if (decimal != std::string::npos)
    {
        const size_t lastNonZero = std::max(result.find_last_not_of('0'), decimal);
        if (lastNonZero != std::string::npos)
            result.erase(lastNonZero + 1);
        if (result.back() == '.')
            result.pop_back();
    }
    return result;
}

RGB rgb255(const lsp::Color& color)
{
    return {
        static_cast<int>(std::floor(color.red * 255.0)),
        static_cast<int>(std::floor(color.green * 255.0)),
        static_cast<int>(std::floor(color.blue * 255.0)),
    };
}
} // namespace

lsp::ColorPresentation colorCallPresentation(std::string_view symbol, ColorPresentationEncoding encoding, const lsp::Color& color)
{
    std::string label(symbol);
    label += '(';

    switch (encoding)
    {
    case ColorPresentationEncoding::Rgb:
        label += formatDouble(color.red) + ", " + formatDouble(color.green) + ", " + formatDouble(color.blue);
        break;
    case ColorPresentationEncoding::Rgb255:
    {
        const RGB rgb = rgb255(color);
        label += std::to_string(rgb.r) + ", " + std::to_string(rgb.g) + ", " + std::to_string(rgb.b);
        break;
    }
    case ColorPresentationEncoding::Hsv:
    {
        const HSV hsv = rgbToHsv(rgb255(color));
        label += formatDouble(hsv.h) + ", " + formatDouble(hsv.s) + ", " + formatDouble(hsv.v);
        break;
    }
    case ColorPresentationEncoding::Hex:
        label += "\"" + rgbToHex(rgb255(color)) + "\"";
        break;
    }

    label += ')';
    return {std::move(label)};
}

lsp::ColorPresentationResult color3Presentations(const lsp::Color& color)
{
    return {
        colorCallPresentation("Color3.new", ColorPresentationEncoding::Rgb, color),
        colorCallPresentation("Color3.fromRGB", ColorPresentationEncoding::Rgb255, color),
        colorCallPresentation("Color3.fromHSV", ColorPresentationEncoding::Hsv, color),
        colorCallPresentation("Color3.fromHex", ColorPresentationEncoding::Hex, color),
    };
}
