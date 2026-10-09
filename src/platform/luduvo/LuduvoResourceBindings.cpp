#include "Platform/LuduvoResourceBindings.hpp"

#include "Luau/Type.h"
#include "Luau/TypePack.h"

#include <algorithm>

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
    {"Instance.AddComponent", "name", "component"},
    {"Instance.FindAncestorWithComponent", "name", "component"},
    {"Instance.HasComponent", "name", "component"},
    {"Instance.RemoveComponent", "name", "component"},
    {"Query.With", "name", "component"},
    {"Query.Without", "name", "component"},
    {"game.Prefabs.Exists", "name", "prefab"},
    {"game.Prefabs.Id", "name", "prefab"},
    {"game.Prefabs.RevertAddedComponent", "name", "component"},
    {"game.Prefabs.Spawn", "name", "prefab"},
    {"game.World.Each", "component", "component"},
    {"game.World.Pair", "relation", "component"},
    {"game.World.Query", "components", "component", true, true},
};

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

std::optional<std::string> canonicalLuduvoResourceSymbol(const std::optional<std::string>& documentationSymbol)
{
    return canonicalDocumentationSymbol(documentationSymbol);
}

LuduvoResourceBindingRegistry::LuduvoResourceBindingRegistry()
{
    configure(ClientLuduvoConfiguration{});
}

void LuduvoResourceBindingRegistry::configure(const ClientLuduvoConfiguration& configuration)
{
    bindings.clear();
    bindings.reserve(std::size(kBuiltinBindings) + configuration.resources.bindings.size());
    for (const BuiltinBinding& binding : kBuiltinBindings)
        bindings.push_back(
            {std::string(binding.symbol), std::string(binding.parameter), std::string(binding.kind), binding.variadic, binding.pluralResourceNoun});

    for (const ClientLuduvoResourceBindingConfiguration& configured : configuration.resources.bindings)
    {
        if (configured.symbol.empty() || configured.parameter.empty() || configured.kind.empty())
            continue;

        bindings.erase(std::remove_if(bindings.begin(), bindings.end(),
                           [&](const Binding& existing)
                           {
                               return existing.symbol == configured.symbol && existing.parameter == configured.parameter;
                           }),
            bindings.end());
        bindings.push_back({configured.symbol, configured.parameter, configured.kind, false, false});
    }
}

std::vector<LuduvoResolvedResourceBinding> LuduvoResourceBindingRegistry::resolve(
    const std::optional<std::string>& documentationSymbol, Luau::TypeId callableType) const
{
    const auto canonicalSymbol = canonicalDocumentationSymbol(documentationSymbol);
    if (!canonicalSymbol)
        return {};

    std::vector<LuduvoResolvedResourceBinding> result;
    auto resolveFunction = [&](const Luau::FunctionType* function)
    {
        auto [arguments, tail] = Luau::flatten(function->argTypes);
        for (const Binding& binding : bindings)
        {
            if (binding.symbol != *canonicalSymbol)
                continue;

            std::optional<size_t> parameterIndex;
            bool variadic = binding.variadic;
            if (variadic)
            {
                if (!tail || !Luau::get<Luau::VariadicTypePack>(Luau::follow(*tail)))
                    continue;
                parameterIndex = arguments.size();
            }
            else
            {
                for (size_t index = 0; index < function->argNames.size() && index < arguments.size(); ++index)
                {
                    if (function->argNames[index] && function->argNames[index]->name == binding.parameter)
                    {
                        parameterIndex = index;
                        break;
                    }
                }
            }

            if (!parameterIndex)
                continue;

            const auto duplicate = std::find_if(result.begin(), result.end(),
                [&](const LuduvoResolvedResourceBinding& existing)
                {
                    return existing.parameter == binding.parameter && existing.kind == binding.kind &&
                           existing.typeParameterIndex == *parameterIndex && existing.variadic == variadic;
                });
            if (duplicate == result.end())
                result.push_back({binding.parameter, binding.kind, *parameterIndex, variadic, binding.pluralResourceNoun});
        }
    };

    callableType = Luau::follow(callableType);
    if (const auto function = Luau::get<Luau::FunctionType>(callableType))
        resolveFunction(function);
    else if (const auto intersection = Luau::get<Luau::IntersectionType>(callableType))
    {
        for (Luau::TypeId part : intersection->parts)
            if (const auto function = Luau::get<Luau::FunctionType>(Luau::follow(part)))
                resolveFunction(function);
    }

    return result;
}
