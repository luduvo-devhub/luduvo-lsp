#pragma once

#include "nlohmann/json.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace luduvo
{

struct ProjectFileInspection
{
    std::string name;
    std::string directory;
    std::string projectFile;
    size_t assetCount = 0;
    size_t scriptAssetCount = 0;
    size_t autorunScriptCount = 0;
    std::map<std::string, size_t> assetTypes;
    std::vector<std::string> declaredScripts;
    std::vector<std::string> autorunScripts;
    std::vector<std::string> scripts;
    std::vector<std::string> missingScripts;
};

struct ProjectFileInspectionResult
{
    std::optional<ProjectFileInspection> project;
    std::optional<std::string> error;
};

struct ContextInspectionResult
{
    std::optional<nlohmann::json> report;
    std::optional<std::string> error;
};

struct InstanceInspectionRequest
{
    std::optional<uint64_t> nodeId;
    std::optional<uint64_t> instanceId;
    std::optional<std::string> name;
    std::optional<std::string> projectName;
    std::optional<std::string> projectId;
    std::optional<std::string> projectDirectory;
    std::optional<std::string> scriptPath;
    std::optional<std::string> scriptDirectory;
    std::vector<std::string> sections;
};

ProjectFileInspectionResult inspectProjectFiles(const std::string& path, std::optional<std::string> configuredProjectName = std::nullopt);
ContextInspectionResult inspectContextInput(const std::string& path, const std::string& stateDirectory = {});
ContextInspectionResult inspectContextInstance(const InstanceInspectionRequest& request, const std::string& stateDirectory = {});

} // namespace luduvo
