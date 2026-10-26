#pragma once

#include "LSP/ClientConfiguration.hpp"
#include "Platform/luduvo/context/LuduvoContextStore.hpp"

#include <memory>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace luduvo
{

struct ProjectContextCandidate
{
    ContextProject project;
    std::optional<WorldDumpSnapshot> snapshot;
};

struct WorldDumpRequest
{
    std::string command;
    std::string outputPath;
    std::string projectId;
};

struct ContextInferredScripts
{
    std::string projectName;
    std::vector<std::string> scripts;
};

struct ContextUpdates
{
    std::vector<std::string> updatedWorlds;
    std::vector<ContextInferredScripts> inferredScripts;

    bool empty() const;
};

ContextUpdates findContextUpdates(const ContextCatalog& previous, const ContextCatalog& current);

class ContextProvider
{
    ContextStore store;
    std::string workspaceDirectory;

    std::vector<ProjectContextCandidate> findAllForScript(const std::string& scriptPath, std::string* error = nullptr) const;

public:
    ContextProvider(std::string stateDirectory, std::string workspaceDirectory);

    ContextStoreResult updateProjectFiles() const;
    ContextStoreResult addProject(const std::string& projectDirectory) const;
    std::vector<ProjectContextCandidate> findForScript(const std::string& scriptPath, std::string* error = nullptr) const;
    nlohmann::json inspect(const std::string& scriptPath) const;
    ContextStoreResult select(const std::string& scriptPath, const std::string& projectId) const;
    ContextStoreResult reset(const std::string& scriptPath) const;
    std::optional<WorldDumpRequest> createDumpRequest(const std::string& scriptPath, std::string* error = nullptr) const;
    const ContextStore& contextStore() const;
};

class ContextHelperSession;
class ContextNotificationSubscription;

void setContextHelperExecutablePath(std::string path);
std::shared_ptr<ContextHelperSession> acquireContextHelper(const ClientLuduvoConfiguration& configuration, std::string* error = nullptr);
std::shared_ptr<ContextNotificationSubscription> subscribeContextUpdates(const std::shared_ptr<ContextHelperSession>& session, const void* subscriber,
    std::function<void(const ContextUpdates&)> callback);

} // namespace luduvo
