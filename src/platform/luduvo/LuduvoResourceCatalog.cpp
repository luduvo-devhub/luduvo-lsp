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

static std::vector<LuduvoResourceName> resourceEntries(std::vector<std::string> names, bool builtIn)
{
    normalizeNames(names);
    std::vector<LuduvoResourceName> result;
    result.reserve(names.size());
    for (std::string& name : names)
        result.push_back({std::move(name), std::nullopt, builtIn});
    return result;
}

static std::vector<LuduvoResourceName> resourceEntries(const std::vector<ClientLuduvoResourcesConfiguration::Source::Value>& values, bool builtIn)
{
    std::vector<LuduvoResourceName> result;
    result.reserve(values.size());
    for (const ClientLuduvoResourcesConfiguration::Source::Value& value : values)
    {
        if (!value.name.empty())
            result.push_back({value.name, value.documentation, builtIn});
    }
    return result;
}

static void addResourceEntries(LuduvoResourceGroup& group, const std::vector<LuduvoResourceName>& resources)
{
    for (const LuduvoResourceName& resource : resources)
    {
        const auto existing = std::find_if(group.resources.begin(), group.resources.end(),
            [&](const LuduvoResourceName& candidate)
            {
                return candidate.name == resource.name;
            });
        if (existing == group.resources.end())
        {
            group.resources.push_back(resource);
            continue;
        }
        if (!existing->documentation && resource.documentation)
            existing->documentation = resource.documentation;
    }
    std::sort(group.resources.begin(), group.resources.end(),
        [](const LuduvoResourceName& left, const LuduvoResourceName& right)
        {
            return left.name < right.name;
        });
}

static std::unordered_map<std::string, std::vector<LuduvoResourceName>> parseEmbeddedResources(std::string_view source)
{
    std::unordered_map<std::string, std::vector<LuduvoResourceName>> result;
    try
    {
        const json document = json::parse(source);
        const int formatVersion = document.value("formatVersion", 0);
        if ((formatVersion != 1 && formatVersion != 2) || !document.contains("resources") || !document["resources"].is_object())
            return result;

        for (const auto& [kind, value] : document["resources"].items())
        {
            if (!value.is_array())
                continue;

            LuduvoResourceGroup group{kind, "string", {}};
            for (const auto& item : value)
            {
                if (item.is_string())
                    addResourceEntries(group, {{item.get<std::string>(), std::nullopt, true}});
                else if (formatVersion >= 2 && item.is_object() && item.contains("name") && item["name"].is_string())
                {
                    std::optional<std::string> documentation;
                    if (item.contains("documentation") && item["documentation"].is_string())
                        documentation = item["documentation"].get<std::string>();
                    addResourceEntries(group, {{item["name"].get<std::string>(), std::move(documentation), true}});
                }
            }
            result.emplace(kind, std::move(group.resources));
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
{
    for (auto& [kind, resources] : parseEmbeddedResources(embeddedResources))
    {
        std::string groupKind = kind;
        embeddedGroups.emplace(std::move(kind), LuduvoResourceGroup{std::move(groupKind), "string", std::move(resources)});
    }
}

void LuduvoResourceCatalog::configure(const ClientLuduvoConfiguration& configuration)
{
    configuredWarnings.clear();
    configuredGroups = embeddedGroups;

    if (auto installed = installedPrefabNames(configuration))
        configuredGroups["prefab"] = LuduvoResourceGroup{"prefab", "string", resourceEntries(std::move(*installed), true)};

    for (const auto& [kind, configuredGroup] : configuration.resources.groups)
    {
        LuduvoResourceGroup group{kind, configuredGroup.dataType, {}};
        if (configuredGroup.dataType != "string")
            configuredWarnings.push_back("Resource group '" + kind + "' uses unsupported dataType '" + configuredGroup.dataType +
                                         "'. This version supports only string groups.");

        if (configuredGroup.includeBuiltins)
        {
            auto builtins = configuredGroups.find(kind);
            if (builtins == configuredGroups.end())
                configuredWarnings.push_back("Resource group '" + kind + "' enables built-ins, but no built-in source exists for that group.");
            else
                addResourceEntries(group, builtins->second.resources);
        }

        for (const std::string& sourceName : configuredGroup.sources)
        {
            auto source = configuration.resources.sources.find(sourceName);
            if (source == configuration.resources.sources.end())
            {
                configuredWarnings.push_back("Resource group '" + kind + "' references unknown source '" + sourceName + "'.");
                continue;
            }
            if (source->second.provider != "static")
            {
                configuredWarnings.push_back("Resource source '" + sourceName + "' uses unsupported provider '" + source->second.provider + "'.");
                continue;
            }
            addResourceEntries(group, resourceEntries(source->second.values, false));
        }
        configuredGroups[kind] = std::move(group);
    }
}

const LuduvoResourceGroup* LuduvoResourceCatalog::group(std::string_view kind) const
{
    auto result = configuredGroups.find(std::string(kind));
    return result == configuredGroups.end() ? nullptr : &result->second;
}

std::vector<std::string> LuduvoResourceCatalog::names(std::string_view kind) const
{
    std::vector<std::string> result;
    const LuduvoResourceGroup* resourceGroup = group(kind);
    if (!resourceGroup)
        return result;
    for (const LuduvoResourceName& entry : resourceGroup->resources)
        result.push_back(entry.name);
    return result;
}

const std::vector<std::string>& LuduvoResourceCatalog::configurationWarnings() const
{
    return configuredWarnings;
}
