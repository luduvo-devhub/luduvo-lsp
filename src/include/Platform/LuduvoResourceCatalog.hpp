#pragma once

#include "LSP/ClientConfiguration.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct LuduvoResourceName
{
    std::string name;
    std::optional<std::string> documentation;
    bool builtIn = false;
};

struct LuduvoResourceGroup
{
    std::string kind;
    std::string dataType = "string";
    std::vector<LuduvoResourceName> resources;
};

class LuduvoResourceCatalog
{
public:
    LuduvoResourceCatalog();
    explicit LuduvoResourceCatalog(std::string embeddedResources);

    void configure(const ClientLuduvoConfiguration& configuration);
    const LuduvoResourceGroup* group(std::string_view kind) const;
    std::vector<std::string> names(std::string_view kind) const;
    const std::vector<std::string>& configurationWarnings() const;

private:
    std::unordered_map<std::string, LuduvoResourceGroup> embeddedGroups;
    std::unordered_map<std::string, LuduvoResourceGroup> configuredGroups;
    std::vector<std::string> configuredWarnings;
};
