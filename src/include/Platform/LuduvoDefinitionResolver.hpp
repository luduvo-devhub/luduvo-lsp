#pragma once

#include "LSP/ClientConfiguration.hpp"
#include "LSP/Uri.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum struct LuduvoDefinitionKind
{
    Server,
    Client,
};

struct LuduvoDefinitionCandidate
{
    std::string label;
    std::optional<Uri> sourceUri;
    std::optional<std::string> source;
    std::optional<std::string> error;
    bool notifyOnFailure = false;
};

/// Returns Luduvo's per-user Client directory for the current operating system.
std::optional<std::string> getDefaultLuduvoDataDirectory();

/// Resolves override, installed, and embedded definition candidates in priority order.
std::vector<LuduvoDefinitionCandidate> resolveLuduvoDefinitionCandidates(LuduvoDefinitionKind kind,
    const ClientLuduvoConfiguration& configuration, std::string_view embeddedDefinitions);
