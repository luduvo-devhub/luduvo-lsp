#pragma once

#include <optional>
#include <string>

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

std::optional<LuduvoSymbolIdentity> getLuduvoSymbolIdentity(const std::optional<std::string>& documentationSymbol);
std::optional<std::string> canonicalLuduvoSymbol(const std::optional<std::string>& documentationSymbol);
