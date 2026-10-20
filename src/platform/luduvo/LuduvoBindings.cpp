#include "Platform/LuduvoBindings.hpp"

#include "Luau/Type.h"
#include "Luau/TypePack.h"

#include <algorithm>
#include <unordered_set>

namespace
{
struct BuiltinBinding
{
    std::string_view symbol;
    std::string_view parameter;
    std::string_view kind;
    bool variadic = false;
    bool pluralResourceNoun = false;
};

constexpr BuiltinBinding kBuiltinBindings[] = {
    {"Instance.AddComponent", "name", "component", true, true},
    {"Instance.FindAncestorWithComponent", "name", "component"},
    {"Instance.HasComponent", "name", "component"},
    {"Instance.RemoveComponent", "name", "component", true, true},
    {"Query.With", "name", "component"},
    {"Query.Without", "name", "component"},
    {"game.Prefabs.Exists", "name", "prefab"},
    {"game.Prefabs.Id", "name", "prefab"},
    {"game.Prefabs.RevertAddedComponent", "name", "component"},
    {"game.Prefabs.Spawn", "name", "prefab"},
    {"game.World.Each", "component", "component"},
    {"game.World.HasComponent", "component", "component"},
    {"game.World.Query", "components", "component", true, true},
    {"game.World.Spawn", "components", "component", true, true},
    {"World.Each", "component", "component"},
    {"World.HasComponent", "component", "component"},
    {"World.Query", "components", "component", true, true},
    {"World.Spawn", "components", "component", true, true},
};

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

std::string_view stripOverloadSuffix(std::string_view symbol)
{
    if (const size_t overload = symbol.find("/overload/"); overload != std::string_view::npos)
        symbol = symbol.substr(0, overload);
    return symbol;
}

std::optional<std::string> canonicalDocumentationSymbol(const std::optional<std::string>& documentationSymbol)
{
    if (!documentationSymbol)
        return std::nullopt;

    if (auto identity = getLuduvoSymbolIdentity(documentationSymbol))
        return identity->title;

    std::string_view symbol = stripOverloadSuffix(*documentationSymbol);
    constexpr std::string_view globalMarker = "/global/";
    constexpr std::string_view globalTypeMarker = "/globaltype/";
    size_t marker = symbol.find(globalMarker);
    size_t markerLength = globalMarker.size();
    if (marker == std::string_view::npos)
    {
        marker = symbol.find(globalTypeMarker);
        markerLength = globalTypeMarker.size();
    }

    if (marker != std::string_view::npos)
        symbol.remove_prefix(marker + markerLength);
    else if (!symbol.empty() && symbol.front() == '@')
        return std::nullopt;

    return symbol.empty() ? std::nullopt : std::optional<std::string>{std::string(symbol)};
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

std::optional<LuduvoSymbolIdentity> getLuduvoSymbolIdentity(const std::optional<std::string>& documentationSymbol)
{
    if (!documentationSymbol)
        return std::nullopt;

    constexpr std::string_view serverPrefix = "@luduvo/server/";
    constexpr std::string_view clientPrefix = "@luduvo/client/";
    std::string_view symbol = *documentationSymbol;
    LuduvoDefinitionSide side = LuduvoDefinitionSide::None;

    if (symbol.substr(0, serverPrefix.size()) == serverPrefix)
    {
        symbol.remove_prefix(serverPrefix.size());
        side = LuduvoDefinitionSide::Server;
    }
    else if (symbol.substr(0, clientPrefix.size()) == clientPrefix)
    {
        symbol.remove_prefix(clientPrefix.size());
        side = LuduvoDefinitionSide::Client;
    }
    else
        return std::nullopt;

    constexpr std::string_view globalPrefix = "global/";
    constexpr std::string_view globalTypePrefix = "globaltype/";
    if (symbol.substr(0, globalPrefix.size()) == globalPrefix)
        symbol.remove_prefix(globalPrefix.size());
    else if (symbol.substr(0, globalTypePrefix.size()) == globalTypePrefix)
        symbol.remove_prefix(globalTypePrefix.size());
    else
        return std::nullopt;

    symbol = stripOverloadSuffix(symbol);
    return LuduvoSymbolIdentity{std::string(symbol), side};
}

std::optional<std::string> canonicalLuduvoBindingSymbol(const std::optional<std::string>& documentationSymbol)
{
    return canonicalDocumentationSymbol(documentationSymbol);
}

LuduvoBindingRegistry::LuduvoBindingRegistry()
{
    configure(ClientLuduvoConfiguration{});
}

void LuduvoBindingRegistry::configure(const ClientLuduvoConfiguration& configuration)
{
    resourceBindings.clear();
    colorBindings.clear();
    configuredWarnings.clear();
    resourceBindings.reserve(std::size(kBuiltinBindings) + configuration.resources.bindings.size() + configuration.bindings.size());
    for (const BuiltinBinding& binding : kBuiltinBindings)
        resourceBindings.push_back(
            {std::string(binding.symbol), std::string(binding.parameter), std::string(binding.kind), binding.variadic, binding.pluralResourceNoun});

    colorBindings = {
        {"Color3.new", {"r", "g", "b"}, ColorPresentationEncoding::Rgb},
        {"Color3.fromRGB", {"r", "g", "b"}, ColorPresentationEncoding::Rgb255},
        {"Color3.fromHSV", {"h", "s", "v"}, ColorPresentationEncoding::Hsv},
        {"Color3.fromHex", {"hex"}, ColorPresentationEncoding::Hex},
    };

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
    const auto canonicalSymbol = canonicalDocumentationSymbol(documentationSymbol);
    if (!canonicalSymbol)
        return {};

    std::vector<const Luau::FunctionType*> functions;
    callableType = Luau::follow(callableType);
    if (const auto function = Luau::get<Luau::FunctionType>(callableType))
        functions.push_back(function);
    else if (const auto intersection = Luau::get<Luau::IntersectionType>(callableType))
    {
        for (Luau::TypeId part : intersection->parts)
            if (const auto function = Luau::get<Luau::FunctionType>(Luau::follow(part)))
                functions.push_back(function);
    }

    std::vector<LuduvoResolvedResourceBinding> result;
    for (const ResourceBinding& binding : resourceBindings)
    {
        if (binding.symbol != *canonicalSymbol)
            continue;

        bool foundParameter = false;
        bool foundCompatibleParameter = false;
        for (const Luau::FunctionType* function : functions)
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
                           existing.typeParameterIndex == *parameterIndex && existing.variadic == binding.variadic;
                });
            if (duplicate == result.end())
                result.push_back({binding.parameter, binding.kind, *parameterIndex, binding.variadic, binding.pluralResourceNoun});
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
