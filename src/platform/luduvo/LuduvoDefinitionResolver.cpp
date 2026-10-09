#include "Platform/LuduvoDefinitionResolver.hpp"

#include "LSP/Utils.hpp"
#include "LuauFileUtils.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

static std::string joinPath(std::string_view left, std::string_view right)
{
    return Luau::FileUtils::joinPaths(std::string(left), std::string(right));
}

#ifndef __APPLE__
static std::optional<std::string> environmentVariable(const char* name)
{
    if (const char* value = std::getenv(name); value && *value)
        return value;
    return std::nullopt;
}
#endif

std::optional<std::string> getDefaultLuduvoDataDirectory()
{
#ifdef _WIN32
    if (auto appData = environmentVariable("APPDATA"))
        return joinPath(joinPath(*appData, "Luduvo"), "Client");
#elif defined(__APPLE__)
    if (auto home = getHomeDirectory())
        return joinPath(joinPath(joinPath(*home, "Library"), "Application Support/Luduvo"), "Client");
#else
    std::optional<std::string> dataHome = environmentVariable("XDG_DATA_HOME");
    if (!dataHome)
    {
        if (auto home = getHomeDirectory())
            dataHome = joinPath(*home, ".local/share");
    }
    if (dataHome)
        return joinPath(joinPath(*dataHome, "Luduvo"), "Client");
#endif
    return std::nullopt;
}

static bool isSafeVersionName(std::string_view version)
{
    if (version.empty() || version == "." || version == "..")
        return false;

    return std::all_of(version.begin(), version.end(),
        [](unsigned char character)
        {
            return std::isalnum(character) || character == '.' || character == '_' || character == '-';
        });
}

std::optional<LuduvoActiveContent> resolveActiveLuduvoContent(const ClientLuduvoConfiguration& configuration, std::string* error)
{
    auto fail = [error](std::string message) -> std::optional<LuduvoActiveContent>
    {
        if (error)
            *error = std::move(message);
        return std::nullopt;
    };

    std::optional<std::string> dataDirectory;
    if (!configuration.dataDirectory.empty())
        dataDirectory = resolvePath(configuration.dataDirectory);
    else
        dataDirectory = getDefaultLuduvoDataDirectory();

    if (!dataDirectory)
        return fail("could not determine Luduvo's data directory");

    const std::string contentRoot = joinPath(*dataDirectory, "content");
    const std::string currentPath = joinPath(contentRoot, "current");
    auto current = Luau::FileUtils::readFile(currentPath);
    if (!current)
        return fail("could not read " + currentPath);

    trim(*current);
    if (!isSafeVersionName(*current))
        return fail("invalid content version " + *current);

    return LuduvoActiveContent{contentRoot, joinPath(joinPath(contentRoot, "versions"), *current), *current};
}

static LuduvoDefinitionCandidate readCandidate(std::string label, const std::string& path)
{
    LuduvoDefinitionCandidate candidate{std::move(label), Uri::file(path)};
    candidate.source = Luau::FileUtils::readFile(path);
    if (!candidate.source)
        candidate.error = "could not read " + path;
    return candidate;
}

static LuduvoDefinitionCandidate installedCandidate(LuduvoDefinitionKind kind, const ClientLuduvoConfiguration& configuration)
{
    std::string error;
    auto content = resolveActiveLuduvoContent(configuration, &error);
    if (!content)
        return {"installed Luduvo definitions", std::nullopt, std::nullopt, std::move(error)};

    const char* relativePath = kind == LuduvoDefinitionKind::Server ? "defs/luduvo.d.luau" : "defs/luduvo.client.d.luau";
    const std::string path = joinPath(content->versionRoot, relativePath);
    return readCandidate("installed Luduvo definitions", path);
}

std::vector<LuduvoDefinitionCandidate> resolveLuduvoDefinitionCandidates(
    LuduvoDefinitionKind kind, const ClientLuduvoConfiguration& configuration, std::string_view embeddedDefinitions)
{
    std::vector<LuduvoDefinitionCandidate> candidates;
    const std::string& overridePath =
        kind == LuduvoDefinitionKind::Server ? configuration.definitions.serverOverride : configuration.definitions.clientOverride;
    if (!overridePath.empty())
        candidates.push_back(readCandidate("configured Luduvo definitions", resolvePath(overridePath)));

    auto installed = installedCandidate(kind, configuration);
    installed.notifyOnFailure = true;
    candidates.push_back(std::move(installed));
    candidates.push_back({"embedded Luduvo definitions", std::nullopt, std::string(embeddedDefinitions), std::nullopt});
    return candidates;
}
