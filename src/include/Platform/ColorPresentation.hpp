#pragma once

#include "Protocol/LanguageFeatures.hpp"

#include <string_view>
#include <string>
#include <vector>

enum struct ColorPresentationEncoding
{
    Rgb,
    Rgb255,
    Hsv,
    Hex,
};

lsp::ColorPresentation colorCallPresentation(std::string_view symbol, ColorPresentationEncoding encoding, const lsp::Color& color);
std::vector<std::string> colorArguments(ColorPresentationEncoding encoding, const lsp::Color& color);
lsp::ColorPresentationResult color3Presentations(const lsp::Color& color);
