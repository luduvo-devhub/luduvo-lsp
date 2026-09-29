#pragma once

#include "LSP/ClientConfiguration.hpp"
#include "LSP/DocumentationParser.hpp"

#include "Luau/Type.h"

#include <optional>
#include <string>

struct LuduvoRichHoverContext
{
    Luau::TypeId type;
    std::string typeDefinition;
    std::optional<std::string> documentationSymbol;
    std::optional<Luau::Property> property;
    std::optional<PrintedDocumentation> documentation;
    std::optional<std::string> fallbackTitle;
    bool preferLeafTitle = false;
};

std::optional<std::string> resolveLuduvoHoverDocumentationSymbol(
    Luau::TypeId type, const std::optional<std::string>& directDocumentationSymbol, const std::optional<std::string>& environmentName);
std::string renderLuduvoRichHover(const ClientLuduvoHoverConfiguration& config, const LuduvoRichHoverContext& context);
