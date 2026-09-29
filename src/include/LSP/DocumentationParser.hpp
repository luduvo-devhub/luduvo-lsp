#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann/json.hpp"
#include "Luau/Documentation.h"
#include "Luau/Module.h"
#include "Luau/Location.h"
#include "LSP/Client.hpp"

using json = nlohmann::json;

const std::string kDocumentationBreaker = "\n\n___\n\n";

Luau::FunctionParameterDocumentation parseDocumentationParameter(const json& j);
void parseDocumentationContents(
    std::string_view contents, const std::string& sourceName, Luau::DocumentationDatabase& database, const Client* client,
    bool overwriteExisting = true);
void parseDocumentation(const std::vector<std::string>& documentationFiles, Luau::DocumentationDatabase& database, const Client* client);

struct PrintedDocumentation
{
    std::string markdown;
    std::optional<std::string> learnMoreLink;
};

/// Returns documentation content with its link kept separate for clients that need custom presentation.
std::optional<PrintedDocumentation> getDocumentation(const Luau::DocumentationDatabase& database, const Luau::DocumentationSymbol& symbol);

/// Returns a markdown string of the provided documentation
/// If we can't find any documentation for the given symbol, then we return nullopt
std::optional<std::string> printDocumentation(const Luau::DocumentationDatabase& database, const Luau::DocumentationSymbol& symbol);

/// Returns a markdown string of moonwave-parsed comments
std::string printMoonwaveDocumentation(const std::vector<std::string>& comments);

/// Get comments attached to a node (given the node's location)
std::vector<Luau::Comment> getCommentLocations(const Luau::SourceModule* module, const Luau::Location& node);
