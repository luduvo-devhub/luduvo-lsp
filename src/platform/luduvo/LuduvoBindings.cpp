#include "Platform/luduvo/LuduvoBindings.hpp"
#include "Platform/luduvo/LuduvoBindingRecords.hpp"
#include "Platform/luduvo/LuduvoSymbols.hpp"
#include "Platform/luduvo/LuduvoTargetResolver.hpp"

#include "Luau/Type.h"

#include <algorithm>
#include <unordered_set>

namespace
{
std::optional<ColorPresentationEncoding> colorEncoding(std::string_view encoding)
{
    if (encoding == "rgb")
        return ColorPresentationEncoding::Rgb;
    if (encoding == "rgb255")
        return ColorPresentationEncoding::Rgb255;
    if (encoding == "hsv")
        return ColorPresentationEncoding::Hsv;
    if (encoding == "hex")
        return ColorPresentationEncoding::Hex;
    return std::nullopt;
}

bool containsStringType(Luau::TypeId type, std::unordered_set<Luau::TypeId>& visited)
{
    type = Luau::follow(type);
    if (!visited.insert(type).second)
        return false;

    if (const auto primitive = Luau::get<Luau::PrimitiveType>(type))
        return primitive->type == Luau::PrimitiveType::String;
    if (const auto unionType = Luau::get<Luau::UnionType>(type))
        return std::any_of(unionType->options.begin(), unionType->options.end(),
            [&](Luau::TypeId option)
            {
                return containsStringType(option, visited);
            });
    if (const auto intersection = Luau::get<Luau::IntersectionType>(type))
        return std::any_of(intersection->parts.begin(), intersection->parts.end(),
            [&](Luau::TypeId part)
            {
                return containsStringType(part, visited);
            });
    return false;
}

bool containsStringType(Luau::TypeId type)
{
    std::unordered_set<Luau::TypeId> visited;
    return containsStringType(type, visited);
}

} // namespace

LuduvoBindingRegistry::LuduvoBindingRegistry()
{
    configure(ClientLuduvoConfiguration{});
}

void LuduvoBindingRegistry::configure(const ClientLuduvoConfiguration& configuration)
{
    resourceBindings.clear();
    colorBindings.clear();
    configuredWarnings.clear();
    resourceBindings.reserve(luduvoBuiltinResourceBindings.size() + configuration.resources.bindings.size() + configuration.bindings.size());
    for (const LuduvoBuiltinResourceBinding& binding : luduvoBuiltinResourceBindings)
        resourceBindings.push_back(
            {std::string(binding.symbol), {LuduvoBindingTargetKind::CallParameter, {std::string(binding.parameter)}, binding.variadic},
                std::string(binding.kind), binding.pluralResourceNoun});

    colorBindings.reserve(luduvoBuiltinColorBindings.size() + configuration.bindings.size());
    for (const LuduvoColorBinding& binding : luduvoBuiltinColorBindings)
        colorBindings.push_back({binding.symbol, {LuduvoBindingTargetKind::CallParameter, binding.parameters}, binding.encoding});

    auto configuredTarget = [&](const ClientLuduvoBindingConfiguration& configured, std::string_view label) -> std::optional<LuduvoBindingTarget>
    {
        LuduvoBindingTarget target;
        if (configured.target)
        {
            if (!configured.parameter.empty() || !configured.parameters.empty() || configured.variadic)
            {
                configuredWarnings.push_back(std::string(label) + " cannot combine target with legacy parameter fields.");
                return std::nullopt;
            }
            if (!configured.target->parameter.empty() && !configured.target->parameters.empty())
            {
                configuredWarnings.push_back(std::string(label) + " target cannot define both parameter and parameters.");
                return std::nullopt;
            }
            target.names = configured.target->parameters;
            if (!configured.target->parameter.empty())
                target.names.push_back(configured.target->parameter);
            target.variadic = configured.target->variadic;
        }
        else
        {
            target.names = configured.parameters;
            if (!configured.parameter.empty())
                target.names.push_back(configured.parameter);
            target.variadic = configured.variadic;
        }

        if (target.names.empty())
        {
            configuredWarnings.push_back(std::string(label) + " must target parameter or parameters.");
            return std::nullopt;
        }
        if (std::unordered_set<std::string>(target.names.begin(), target.names.end()).size() != target.names.size())
        {
            configuredWarnings.push_back(std::string(label) + " target contains duplicate parameters.");
            return std::nullopt;
        }
        if (target.variadic && target.names.size() != 1)
        {
            configuredWarnings.push_back(std::string(label) + " cannot combine a variadic target with multiple parameters.");
            return std::nullopt;
        }
        return target;
    };

    auto addResource =
        [&](const std::string& symbol, LuduvoBindingTarget target, const std::string& group, bool pluralResourceNoun, std::string_view label)
    {
        if (symbol.empty() || target.names.size() != 1 || group.empty())
        {
            configuredWarnings.push_back(std::string(label) + " must define a symbol, one target parameter, and a group.");
            return;
        }
        resourceBindings.erase(std::remove_if(resourceBindings.begin(), resourceBindings.end(),
                                   [&](const ResourceBinding& existing)
                                   {
                                       return existing.symbol == symbol && existing.target.names == target.names;
                                   }),
            resourceBindings.end());
        resourceBindings.push_back({symbol, std::move(target), group, pluralResourceNoun});
    };

    for (size_t index = 0; index < configuration.resources.bindings.size(); ++index)
    {
        const ClientLuduvoResourceBindingConfiguration& configured = configuration.resources.bindings[index];
        if (configured.symbol.empty() || configured.parameter.empty() || configured.kind.empty())
        {
            configuredWarnings.push_back(
                "Legacy resource binding #" + std::to_string(index + 1) + " must define non-empty symbol, parameter, and kind values.");
            continue;
        }
        addResource(configured.symbol, {LuduvoBindingTargetKind::CallParameter, {configured.parameter}, configured.variadic}, configured.kind,
            configured.pluralResourceNoun, "Legacy resource binding #" + std::to_string(index + 1));
    }

    for (size_t index = 0; index < configuration.bindings.size(); ++index)
    {
        const ClientLuduvoBindingConfiguration& configured = configuration.bindings[index];
        const std::string label = "Binding #" + std::to_string(index + 1);
        if (configured.kind != "resource" && configured.kind != "color")
        {
            configuredWarnings.push_back(label + " uses unsupported kind '" + configured.kind + "'. This version supports resource and color.");
            continue;
        }
        const std::optional<LuduvoBindingTarget> target = configuredTarget(configured, label);
        if (!target)
            continue;
        if (configured.kind == "resource")
        {
            addResource(configured.symbol, *target, configured.group, configured.pluralResourceNoun, label);
            continue;
        }
        if (target->variadic)
        {
            configuredWarnings.push_back(label + " color target cannot be variadic.");
            continue;
        }

        const auto encoding = colorEncoding(configured.encoding);
        if (configured.symbol.empty() || !encoding)
        {
            configuredWarnings.push_back(label + " must define a symbol and a supported color encoding.");
            continue;
        }
        const size_t expectedParameters = (*encoding == ColorPresentationEncoding::Rgb || *encoding == ColorPresentationEncoding::Rgb255 ||
                                              *encoding == ColorPresentationEncoding::Hsv)
                                              ? 3
                                              : 1;
        if (target->names.size() != expectedParameters)
        {
            configuredWarnings.push_back(label + " has " + std::to_string(target->names.size()) + " color parameters; encoding '" +
                                         configured.encoding + "' requires " + std::to_string(expectedParameters) + ".");
            continue;
        }
        colorBindings.erase(std::remove_if(colorBindings.begin(), colorBindings.end(),
                                [&](const ColorBinding& existing)
                                {
                                    return existing.symbol == configured.symbol;
                                }),
            colorBindings.end());
        colorBindings.push_back({configured.symbol, *target, *encoding});
    }
}

const std::vector<std::string>& LuduvoBindingRegistry::configurationWarnings() const
{
    return configuredWarnings;
}

std::vector<LuduvoResolvedResourceBinding> LuduvoBindingRegistry::resolveResources(
    const std::optional<std::string>& documentationSymbol, Luau::TypeId callableType, std::vector<std::string>* warnings) const
{
    const auto canonicalSymbol = canonicalLuduvoSymbol(documentationSymbol);
    if (!canonicalSymbol)
        return {};

    std::vector<LuduvoResolvedResourceBinding> result;
    for (const ResourceBinding& binding : resourceBindings)
    {
        if (binding.symbol != *canonicalSymbol)
            continue;
        if (binding.target.kind != LuduvoBindingTargetKind::CallParameter || binding.target.names.size() != 1)
            continue;

        const std::string& parameter = binding.target.names.front();

        bool foundParameter = false;
        bool foundCompatibleParameter = false;
        for (const luduvo::CallableTarget& target : luduvo::resolveCallableTarget(callableType, binding.target.names, binding.target.variadic))
        {
            foundParameter = true;
            if (target.types.size() != 1 || !containsStringType(target.types.front()))
                continue;
            foundCompatibleParameter = true;

            const auto duplicate = std::find_if(result.begin(), result.end(),
                [&](const LuduvoResolvedResourceBinding& existing)
                {
                    return existing.parameter == parameter && existing.kind == binding.kind &&
                           existing.typeParameterIndex == target.indices.front() && existing.variadic == binding.target.variadic &&
                           existing.overloadIndex == target.overloadIndex;
                });
            if (duplicate == result.end())
                result.push_back(
                    {parameter, binding.kind, target.indices.front(), binding.target.variadic, binding.pluralResourceNoun, target.overloadIndex});
        }

        if (warnings && !foundCompatibleParameter)
        {
            if (!foundParameter)
                warnings->push_back(binding.symbol + ": " + (binding.target.variadic ? "variadic parameter '" : "parameter '") + parameter +
                                    "' was not found in the callable type.");
            else
                warnings->push_back(binding.symbol + ": parameter '" + parameter + "' does not accept string resource names.");
        }
    }

    return result;
}

std::vector<LuduvoResolvedColorBinding> LuduvoBindingRegistry::resolveColors(std::string_view symbol, Luau::TypeId callableType) const
{
    std::vector<LuduvoResolvedColorBinding> result;
    for (const ColorBinding& binding : colorBindings)
    {
        if (binding.symbol != symbol)
            continue;
        if (binding.target.kind != LuduvoBindingTargetKind::CallParameter)
            continue;
        for (luduvo::CallableTarget& target : luduvo::resolveCallableTarget(callableType, binding.target.names, binding.target.variadic))
            result.push_back({binding.symbol, binding.encoding, std::move(target.indices), target.overloadIndex});
    }
    return result;
}
