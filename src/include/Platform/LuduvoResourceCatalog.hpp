#pragma once

#include "LSP/ClientConfiguration.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class LuduvoResourceCatalog
{
public:
    LuduvoResourceCatalog();
    explicit LuduvoResourceCatalog(std::string embeddedResources);

    void configure(const ClientLuduvoConfiguration& configuration);
    const std::vector<std::string>& names(std::string_view kind) const;

private:
    std::unordered_map<std::string, std::vector<std::string>> embeddedNames;
    std::unordered_map<std::string, std::vector<std::string>> configuredNames;
};
