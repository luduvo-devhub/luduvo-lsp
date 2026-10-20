#pragma once

#include "LSP/ClientConfiguration.hpp"
#include "Luau/TypeFwd.h"
#include "Platform/ColorPresentation.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum struct LuduvoDefinitionSide
{
    None,
    Server,
    Client,
};

struct LuduvoSymbolIdentity
{
    std::string title;
    LuduvoDefinitionSide side = LuduvoDefinitionSide::None;
};

struct LuduvoResolvedResourceBinding
{
    std::string parameter;
    std::string kind;
    size_t typeParameterIndex = 0;
    bool variadic = false;
    bool pluralResourceNoun = false;
};

struct LuduvoColorBinding
{
    std::string symbol;
    std::vector<std::string> parameters;
    ColorPresentationEncoding encoding = ColorPresentationEncoding::Rgb;
};

std::optional<LuduvoSymbolIdentity> getLuduvoSymbolIdentity(const std::optional<std::string>& documentationSymbol);
std::optional<std::string> canonicalLuduvoBindingSymbol(const std::optional<std::string>& documentationSymbol);

class LuduvoBindingRegistry
{
    struct ResourceBinding
    {
        std::string symbol;
        std::string parameter;
        std::string kind;
        bool variadic = false;
        bool pluralResourceNoun = false;
    };
    std::vector<ResourceBinding> resourceBindings;
    std::vector<LuduvoColorBinding> colorBindings;
    std::vector<std::string> configuredWarnings;

public:
    LuduvoBindingRegistry();

    void configure(const ClientLuduvoConfiguration& configuration);
    const std::vector<std::string>& configurationWarnings() const;

    std::vector<LuduvoResolvedResourceBinding> resolveResources(
        const std::optional<std::string>& documentationSymbol, Luau::TypeId callableType, std::vector<std::string>* warnings = nullptr) const;
    const std::vector<LuduvoColorBinding>& colors() const;
};
