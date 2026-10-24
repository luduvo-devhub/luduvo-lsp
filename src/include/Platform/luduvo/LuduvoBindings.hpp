#pragma once

#include "LSP/ClientConfiguration.hpp"
#include "Luau/TypeFwd.h"
#include "LuduvoBindingRecords.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct LuduvoResolvedResourceBinding
{
    std::string parameter;
    std::string kind;
    size_t typeParameterIndex = 0;
    bool variadic = false;
    bool pluralResourceNoun = false;
    size_t overloadIndex = 0;
};

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
