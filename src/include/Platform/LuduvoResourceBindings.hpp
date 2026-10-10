#pragma once

#include "LSP/ClientConfiguration.hpp"
#include "Luau/TypeFwd.h"

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

std::optional<LuduvoSymbolIdentity> getLuduvoSymbolIdentity(const std::optional<std::string>& documentationSymbol);
std::optional<std::string> canonicalLuduvoResourceSymbol(const std::optional<std::string>& documentationSymbol);

class LuduvoResourceBindingRegistry
{
    struct Binding
    {
        std::string symbol;
        std::string parameter;
        std::string kind;
        bool variadic = false;
        bool pluralResourceNoun = false;
    };
    std::vector<Binding> bindings;
    std::vector<std::string> configuredWarnings;

public:
    LuduvoResourceBindingRegistry();

    void configure(const ClientLuduvoConfiguration& configuration);
    const std::vector<std::string>& configurationWarnings() const;

    std::vector<LuduvoResolvedResourceBinding> resolve(const std::optional<std::string>& documentationSymbol, Luau::TypeId callableType,
        std::vector<std::string>* warnings = nullptr) const;
};
