#pragma once

#include "Platform/luduvo/context/LuduvoWorldDump.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace luduvo
{

struct ContextProject
{
    std::string id;
    std::string name;
    std::string directory;
    std::string snapshotPath;
    std::string dumpPath;
    int64_t updatedAtUnixMs = 0;
    uint64_t worldGeneration = 0;
    size_t instanceCount = 0;
    size_t scriptAttachmentCount = 0;
    bool fresh = false;
    std::string staleReason;
    std::vector<std::string> declaredScripts;
    std::vector<std::string> autorunScripts;
    std::vector<std::string> worldScripts;
    std::vector<std::string> attachedScripts;
    std::vector<std::string> scripts;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ContextProject, id, name, directory, snapshotPath, dumpPath, updatedAtUnixMs, fresh, staleReason,
    worldGeneration, instanceCount, scriptAttachmentCount, declaredScripts, autorunScripts, worldScripts, attachedScripts, scripts)

struct ContextCatalog
{
    int formatVersion = 1;
    uint64_t generation = 0;
    std::string activeProjectId;
    std::map<std::string, ContextProject> projects;
    std::map<std::string, std::vector<std::string>> scriptProjects;
    std::map<std::string, std::map<std::string, std::string>> workspaceScriptSelections;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    ContextCatalog, formatVersion, generation, activeProjectId, projects, scriptProjects, workspaceScriptSelections)

struct ContextStoreResult
{
    bool success = false;
    std::optional<std::string> projectId;
    std::optional<std::string> error;
    size_t affected = 0;
};

struct ContextCleanupCandidate
{
    std::string id;
    std::string label;
    std::string path;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ContextCleanupCandidate, id, label, path)

struct ContextCleanupSummary
{
    std::vector<ContextCleanupCandidate> clients;
    std::vector<ContextCleanupCandidate> projects;
    std::vector<ContextCleanupCandidate> snapshots;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ContextCleanupSummary, clients, projects, snapshots)

struct ContextStatus
{
    bool running = false;
    std::string stateDirectory;
    ContextCatalog catalog;
    ContextCleanupSummary cleanup;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ContextStatus, running, stateDirectory, catalog, cleanup)

class ContextStore
{
    std::string stateDirectory;

    ContextStoreResult save(ContextCatalog& catalog) const;
    std::string selectProjectId(ContextCatalog& catalog, const std::string& name, const std::string& directory) const;

public:
    explicit ContextStore(std::string stateDirectory);

    const std::string& directory() const;
    std::string catalogPath() const;
    std::string leasesDirectory() const;
    std::string controlDirectory() const;
    std::string stopPath() const;
    std::string ownerPath() const;
    bool helperRunning() const;

    std::optional<ContextCatalog> load(std::string* error = nullptr) const;
    ContextStoreResult updateProjectFiles(const std::string& projectDirectory, std::optional<std::string> projectName = std::nullopt) const;
    ContextStoreResult updateWorld(
        WorldDumpSnapshot snapshot, const std::string& projectName, const std::optional<std::string>& projectDirectory = std::nullopt) const;
    ContextStoreResult setActiveProject(const std::string& projectName) const;
    ContextStoreResult markProjectStale(const std::string& projectName, std::string reason) const;
    ContextStoreResult attachScript(const std::string& projectName, const std::string& scriptPath) const;
    ContextStoreResult selectProjectForScript(
        const std::string& workspaceDirectory, const std::string& scriptPath, const std::string& projectId) const;
    ContextStoreResult resetProjectForScript(const std::string& workspaceDirectory, const std::string& scriptPath) const;
    std::optional<WorldDumpSnapshot> loadWorld(const std::string& projectId, std::string* error = nullptr) const;
    ContextCleanupSummary cleanupCandidates() const;
    std::optional<ContextStatus> status(std::string* error = nullptr) const;
    ContextStoreResult clean(const std::string& kind, const std::vector<std::string>& selectors, bool all) const;
    ContextStoreResult forget(const std::string& kind, const std::vector<std::string>& selectors, bool all) const;
};

std::string normalizeContextPath(const std::string& path);
std::string defaultContextStateDirectory(const std::string& configuredLocalDirectory = {}, const std::string& configuredContextDirectory = {});
std::string defaultCommandsLogPath(const std::string& configuredRoamingDirectory = {});
std::string defaultStudioLogPath(const std::string& configuredLocalDirectory = {});

} // namespace luduvo
