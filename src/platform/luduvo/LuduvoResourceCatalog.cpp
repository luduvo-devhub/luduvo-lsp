#include "Platform/LuduvoResourceCatalog.hpp"

#include "LuduvoResources.hpp"
#include "Platform/LuduvoDefinitionResolver.hpp"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

using json = nlohmann::json;

static void normalizeNames(std::vector<std::string>& names)
{
    names.erase(std::remove_if(names.begin(), names.end(),
                    [](const std::string& name)
                    {
                        return name.empty();
                    }),
        names.end());
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
}

static std::unordered_map<std::string, std::vector<std::string>> parseEmbeddedResources(std::string_view source)
{
    std::unordered_map<std::string, std::vector<std::string>> result;
    try
    {
        const json document = json::parse(source);
        if (document.value("formatVersion", 0) != 1 || !document.contains("resources") || !document["resources"].is_object())
            return result;

        for (const auto& [kind, value] : document["resources"].items())
        {
            if (!value.is_array())
                continue;

            std::vector<std::string> names;
            for (const auto& name : value)
            {
                if (name.is_string())
                    names.push_back(name.get<std::string>());
            }
            normalizeNames(names);
            result.emplace(kind, std::move(names));
        }
    }
    catch (const json::exception&)
    {
    }
    return result;
}

static std::optional<std::vector<std::string>> installedPrefabNames(const ClientLuduvoConfiguration& configuration)
{
    auto content = resolveActiveLuduvoContent(configuration);
    if (!content)
        return std::nullopt;

    const std::filesystem::path directory = std::filesystem::u8path(content->versionRoot) / "prefabs";
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error) || error)
        return std::nullopt;

    std::vector<std::string> result;
    std::filesystem::directory_iterator iterator(directory, error);
    if (error)
        return std::nullopt;

    const std::filesystem::directory_iterator end;
    while (iterator != end)
    {
        const std::filesystem::directory_entry& entry = *iterator;
        const bool regularFile = entry.is_regular_file(error);
        if (error)
            return std::nullopt;
        if (regularFile)
        {
            std::string extension = entry.path().extension().u8string();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                [](unsigned char character)
                {
                    return char(std::tolower(character));
                });
            if (extension == ".ldv")
                result.push_back(entry.path().stem().u8string());
        }

        iterator.increment(error);
        if (error)
            return std::nullopt;
    }

    normalizeNames(result);
    return result;
}

LuduvoResourceCatalog::LuduvoResourceCatalog()
    : LuduvoResourceCatalog(LUDUVO_RESOURCES)
{
}

LuduvoResourceCatalog::LuduvoResourceCatalog(std::string embeddedResources)
    : embeddedNames(parseEmbeddedResources(embeddedResources))
{
}

void LuduvoResourceCatalog::configure(const ClientLuduvoConfiguration& configuration)
{
    configuredNames = embeddedNames;

    if (configuration.resources.overrides.find("prefab") == configuration.resources.overrides.end())
    {
        if (auto installed = installedPrefabNames(configuration))
            configuredNames["prefab"] = std::move(*installed);
    }

    for (const auto& [kind, names] : configuration.resources.overrides)
    {
        auto replacement = names;
        normalizeNames(replacement);
        configuredNames[kind] = std::move(replacement);
    }
}

const std::vector<std::string>& LuduvoResourceCatalog::names(std::string_view kind) const
{
    static const std::vector<std::string> empty;
    auto result = configuredNames.find(std::string(kind));
    return result == configuredNames.end() ? empty : result->second;
}
