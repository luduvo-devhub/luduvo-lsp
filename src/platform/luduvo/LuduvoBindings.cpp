#include "Platform/luduvo/LuduvoBindings.hpp"
#include "Platform/luduvo/LuduvoBindingRecords.hpp"
#include "Platform/luduvo/LuduvoSymbols.hpp"

#include "Luau/Type.h"
#include "Luau/TypePack.h"

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
            {std::string(binding.symbol), std::string(binding.parameter), std::string(binding.kind), binding.variadic, binding.pluralResourceNoun});

    colorBindings = luduvoBuiltinColorBindings;

    auto addResource = [&](const std::string& symbol, const std::string& parameter, const std::string& group, bool variadic, bool pluralResourceNoun,
                           std::string_view label)
    {
        if (symbol.empty() || parameter.empty() || group.empty())
        {
            configuredWarnings.push_back(std::string(label) + " must define non-empty symbol, parameter, and group values.");
            return;
        }
        resourceBindings.erase(std::remove_if(resourceBindings.begin(), resourceBindings.end(),
                                   [&](const ResourceBinding& existing)
                                   {
                                       return existing.symbol == symbol && existing.parameter == parameter;
                                   }),
            resourceBindings.end());
        resourceBindings.push_back({symbol, parameter, group, variadic, pluralResourceNoun});
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
        addResource(configured.symbol, configured.parameter, configured.kind, configured.variadic, configured.pluralResourceNoun,
            "Legacy resource binding #" + std::to_string(index + 1));
    }

    for (size_t index = 0; index < configuration.bindings.size(); ++index)
    {
        const ClientLuduvoBindingConfiguration& configured = configuration.bindings[index];
        const std::string label = "Binding #" + std::to_string(index + 1);
        if (configured.kind == "resource")
        {
            addResource(configured.symbol, configured.parameter, configured.group, configured.variadic, configured.pluralResourceNoun, label);
            continue;
        }
        if (configured.kind != "color")
        {
            configuredWarnings.push_back(label + " uses unsupported kind '" + configured.kind + "'. This version supports resource and color.");
            continue;
        }

        const auto encoding = colorEncoding(configured.encoding);
        std::vector<std::string> parameters = configured.parameters;
        if (parameters.empty() && !configured.parameter.empty())
            parameters.push_back(configured.parameter);
        if (configured.symbol.empty() || !encoding || parameters.empty())
        {
            configuredWarnings.push_back(label + " must define a symbol, a supported color encoding, and parameter or parameters.");
            continue;
        }
        const size_t expectedParameters = (*encoding == ColorPresentationEncoding::Rgb || *encoding == ColorPresentationEncoding::Rgb255 ||
                                              *encoding == ColorPresentationEncoding::Hsv)
                                              ? 3
                                              : 1;
        if (parameters.size() != expectedParameters)
        {
            configuredWarnings.push_back(label + " has " + std::to_string(parameters.size()) + " color parameters; encoding '" + configured.encoding +
                                         "' requires " + std::to_string(expectedParameters) + ".");
            continue;
        }
        colorBindings.erase(std::remove_if(colorBindings.begin(), colorBindings.end(),
                                [&](const LuduvoColorBinding& existing)
                                {
                                    return existing.symbol == configured.symbol;
                                }),
            colorBindings.end());
        colorBindings.push_back({configured.symbol, std::move(parameters), *encoding});
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

    struct CallableOverload
    {
        const Luau::FunctionType* function;
        size_t index;
    };

    std::vector<CallableOverload> functions;
    callableType = Luau::follow(callableType);
    if (const auto function = Luau::get<Luau::FunctionType>(callableType))
        functions.push_back({function, 0});
    else if (const auto intersection = Luau::get<Luau::IntersectionType>(callableType))
    {
        for (Luau::TypeId part : intersection->parts)
            if (const auto function = Luau::get<Luau::FunctionType>(Luau::follow(part)))
                functions.push_back({function, functions.size()});
    }

    std::vector<LuduvoResolvedResourceBinding> result;
    for (const ResourceBinding& binding : resourceBindings)
    {
        if (binding.symbol != *canonicalSymbol)
            continue;

        bool foundParameter = false;
        bool foundCompatibleParameter = false;
        for (const auto& [function, overloadIndex] : functions)
        {
            auto [arguments, tail] = Luau::flatten(function->argTypes);
            std::optional<size_t> parameterIndex;
            std::optional<Luau::TypeId> parameterType;
            if (binding.variadic)
            {
                if (!tail)
                    continue;
                const auto variadicType = Luau::get<Luau::VariadicTypePack>(Luau::follow(*tail));
                if (!variadicType)
                    continue;
                parameterIndex = arguments.size();
                parameterType = variadicType->ty;
            }
            else
            {
                for (size_t index = 0; index < function->argNames.size() && index < arguments.size(); ++index)
                {
                    if (function->argNames[index] && function->argNames[index]->name == binding.parameter)
                    {
                        parameterIndex = index;
                        parameterType = arguments[index];
                        break;
                    }
                }
            }

            if (!parameterIndex || !parameterType)
                continue;
            foundParameter = true;
            if (!containsStringType(*parameterType))
                continue;
            foundCompatibleParameter = true;

            const auto duplicate = std::find_if(result.begin(), result.end(),
                [&](const LuduvoResolvedResourceBinding& existing)
                {
                    return existing.parameter == binding.parameter && existing.kind == binding.kind &&
                           existing.typeParameterIndex == *parameterIndex && existing.variadic == binding.variadic &&
                           existing.overloadIndex == overloadIndex;
                });
            if (duplicate == result.end())
                result.push_back({binding.parameter, binding.kind, *parameterIndex, binding.variadic, binding.pluralResourceNoun, overloadIndex});
        }

        if (warnings && !foundCompatibleParameter)
        {
            if (!foundParameter)
                warnings->push_back(binding.symbol + ": " + (binding.variadic ? "variadic parameter '" : "parameter '") + binding.parameter +
                                    "' was not found in the callable type.");
            else
                warnings->push_back(binding.symbol + ": parameter '" + binding.parameter + "' does not accept string resource names.");
        }
    }

    return result;
}

const std::vector<LuduvoColorBinding>& LuduvoBindingRegistry::colors() const
{
    return colorBindings;
}
