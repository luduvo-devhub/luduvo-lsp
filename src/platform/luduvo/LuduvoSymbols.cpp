#include "Platform/luduvo/LuduvoSymbols.hpp"

#include <string_view>

namespace
{
std::string_view stripOverloadSuffix(std::string_view symbol)
{
    if (const size_t overload = symbol.find("/overload/"); overload != std::string_view::npos)
        symbol = symbol.substr(0, overload);
    return symbol;
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

std::optional<std::string> canonicalLuduvoSymbol(const std::optional<std::string>& documentationSymbol)
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
