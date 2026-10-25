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

struct LuduvoResolvedColorBinding
{
    std::string symbol;
    ColorPresentationEncoding encoding = ColorPresentationEncoding::Rgb;
    std::vector<size_t> typeParameterIndices;
    size_t overloadIndex = 0;
};

enum struct LuduvoBindingTargetKind
{
    CallParameter,
};

struct LuduvoBindingTarget
{
    LuduvoBindingTargetKind kind = LuduvoBindingTargetKind::CallParameter;
    std::vector<std::string> names;
    bool variadic = false;
};

class LuduvoBindingRegistry
{
    struct ResourceBinding
    {
        std::string symbol;
        LuduvoBindingTarget target;
        std::string kind;
        bool pluralResourceNoun = false;
    };
    struct ColorBinding
    {
        std::string symbol;
        LuduvoBindingTarget target;
        ColorPresentationEncoding encoding = ColorPresentationEncoding::Rgb;
    };
    std::vector<ResourceBinding> resourceBindings;
    std::vector<ColorBinding> colorBindings;
    std::vector<std::string> configuredWarnings;

public:
    LuduvoBindingRegistry();

    void configure(const ClientLuduvoConfiguration& configuration);
    const std::vector<std::string>& configurationWarnings() const;

    std::vector<LuduvoResolvedResourceBinding> resolveResources(
        const std::optional<std::string>& documentationSymbol, Luau::TypeId callableType, std::vector<std::string>* warnings = nullptr) const;
    std::vector<LuduvoResolvedColorBinding> resolveColors(std::string_view symbol, Luau::TypeId callableType) const;
};
