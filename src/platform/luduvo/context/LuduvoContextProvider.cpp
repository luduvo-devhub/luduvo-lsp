#include "Platform/luduvo/context/LuduvoContextProvider.hpp"

#include "LuauFileUtils.hpp"
#include "Platform/luduvo/context/LuduvoProcessIdentity.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace luduvo
{

using json = nlohmann::json;

static std::string executablePath;

bool ContextUpdates::empty() const
{
    return updatedWorlds.empty() && inferredScripts.empty();
}

ContextUpdates findContextUpdates(const ContextCatalog& previous, const ContextCatalog& current)
{
    ContextUpdates updates;
    for (const auto& [projectId, project] : current.projects)
    {
        const auto old = previous.projects.find(projectId);
        const uint64_t previousWorldGeneration = old == previous.projects.end() ? 0 : old->second.worldGeneration;
        if (project.worldGeneration > 0 && project.worldGeneration != previousWorldGeneration)
            updates.updatedWorlds.push_back(project.name);

        const std::vector<std::string> emptyScripts;
        const std::vector<std::string>& previousScripts = old == previous.projects.end() ? emptyScripts : old->second.attachedScripts;
        std::vector<std::string> addedScripts;
        std::set_difference(project.attachedScripts.begin(), project.attachedScripts.end(), previousScripts.begin(), previousScripts.end(),
            std::back_inserter(addedScripts));
        if (!addedScripts.empty())
            updates.inferredScripts.push_back({project.name, std::move(addedScripts)});
    }
    std::sort(updates.updatedWorlds.begin(), updates.updatedWorlds.end());
    std::sort(updates.inferredScripts.begin(), updates.inferredScripts.end(), [](const auto& left, const auto& right)
        {
            return left.projectName < right.projectName;
        });
    return updates;
}

#ifdef _WIN32
static std::string quoteArgument(const std::string& argument)
{
    std::string result = "\"";
    size_t backslashes = 0;
    for (char character : argument)
    {
        if (character == '\\')
            ++backslashes;
        else if (character == '"')
        {
            result.append(backslashes * 2 + 1, '\\');
            result.push_back('"');
            backslashes = 0;
        }
        else
        {
            result.append(backslashes, '\\');
            backslashes = 0;
            result.push_back(character);
        }
    }
    result.append(backslashes * 2, '\\');
    result.push_back('"');
    return result;
}
#endif

static bool spawnHelper(const std::vector<std::string>& arguments)
{
    if (executablePath.empty())
        return false;
#ifdef _WIN32
    std::string commandLine = quoteArgument(executablePath);
    for (const std::string& argument : arguments)
        commandLine += " " + quoteArgument(argument);
    std::wstring wideCommand = Luau::FileUtils::fromUtf8(commandLine);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        nullptr, wideCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &startup, &process);
    if (!created)
        return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
#else
    const pid_t process = fork();
    if (process < 0)
        return false;
    if (process == 0)
    {
        if (setsid() < 0)
            _exit(127);
        const pid_t helper = fork();
        if (helper < 0)
            _exit(127);
        if (helper > 0)
            _exit(0);
        const int nullDescriptor = open("/dev/null", O_RDWR);
        if (nullDescriptor >= 0)
        {
            dup2(nullDescriptor, STDIN_FILENO);
            dup2(nullDescriptor, STDOUT_FILENO);
            dup2(nullDescriptor, STDERR_FILENO);
            if (nullDescriptor > STDERR_FILENO)
                close(nullDescriptor);
        }
        std::vector<char*> values;
        values.push_back(const_cast<char*>(executablePath.c_str()));
        for (const std::string& argument : arguments)
            values.push_back(const_cast<char*>(argument.c_str()));
        values.push_back(nullptr);
        execv(executablePath.c_str(), values.data());
        _exit(127);
    }
    int status = 0;
    return waitpid(process, &status, 0) == process && WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

class ContextHelperSession
{
    ContextStore store;
    std::string leasePath;
    bool helperStarted = false;
    std::atomic<bool> stopping = false;
    std::thread monitor;
    std::mutex subscriberMutex;
    ContextCatalog lastCatalog;
    struct Subscriber
    {
        size_t references = 0;
        std::function<void(const ContextUpdates&)> callback;
    };
    std::map<const void*, Subscriber> subscribers;

    void monitorCatalog()
    {
        while (!stopping)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            if (stopping)
                break;
            const auto current = store.load();
            if (!current || current->generation == lastCatalog.generation)
                continue;
            const ContextUpdates updates = findContextUpdates(lastCatalog, *current);
            lastCatalog = *current;
            if (updates.empty())
                continue;

            std::vector<std::function<void(const ContextUpdates&)>> callbacks;
            {
                std::lock_guard guard(subscriberMutex);
                for (const auto& [_, subscriber] : subscribers)
                    callbacks.push_back(subscriber.callback);
            }
            for (const auto& callback : callbacks)
                callback(updates);
        }
    }

public:
    ContextHelperSession(std::string stateDirectory, std::string commandsLog, std::string studioLog)
        : store(std::move(stateDirectory))
    {
        lastCatalog = store.load().value_or(ContextCatalog{});
        const auto identity = currentProcessIdentity();
        if (!identity)
            return;
        leasePath = Luau::FileUtils::joinPaths(store.leasesDirectory(), std::to_string(identity->processId) + ".lease");
        if (!writeProcessIdentity(leasePath, *identity))
            return;
        helperStarted = spawnHelper({"context", "run", "--state-directory", store.directory(), "--commands-log", std::move(commandsLog),
            "--studio-log", std::move(studioLog), "--idle-timeout", "30"});
        if (helperStarted)
            monitor = std::thread([this]
                {
                    monitorCatalog();
                });
    }

    ~ContextHelperSession()
    {
        stopping = true;
        if (monitor.joinable())
            monitor.join();
        std::error_code error;
        std::filesystem::remove(std::filesystem::u8path(leasePath), error);
    }

    bool started() const
    {
        return helperStarted;
    }

    void subscribe(const void* key, std::function<void(const ContextUpdates&)> callback)
    {
        std::lock_guard guard(subscriberMutex);
        Subscriber& subscriber = subscribers[key];
        if (subscriber.references++ == 0)
            subscriber.callback = std::move(callback);
    }

    void unsubscribe(const void* key)
    {
        std::lock_guard guard(subscriberMutex);
        const auto subscriber = subscribers.find(key);
        if (subscriber != subscribers.end() && --subscriber->second.references == 0)
            subscribers.erase(subscriber);
    }
};

class ContextNotificationSubscription
{
    std::shared_ptr<ContextHelperSession> session;
    const void* subscriber;

public:
    ContextNotificationSubscription(std::shared_ptr<ContextHelperSession> session, const void* subscriber)
        : session(std::move(session))
        , subscriber(subscriber)
    {
    }

    ~ContextNotificationSubscription()
    {
        session->unsubscribe(subscriber);
    }
};

ContextProvider::ContextProvider(std::string stateDirectory, std::string workspaceDirectory)
    : store(std::move(stateDirectory))
    , workspaceDirectory(normalizeContextPath(workspaceDirectory))
{
}

ContextStoreResult ContextProvider::updateProjectFiles() const
{
    return store.updateProjectFiles(workspaceDirectory);
}

ContextStoreResult ContextProvider::addProject(const std::string& projectDirectory) const
{
    return store.updateProjectFiles(projectDirectory);
}

std::vector<ProjectContextCandidate> ContextProvider::findAllForScript(const std::string& scriptPath, std::string* error) const
{
    std::vector<ProjectContextCandidate> result;
    const auto catalog = store.load(error);
    if (!catalog)
        return result;
    const std::string normalized = normalizeContextPath(scriptPath);
    const auto indexed = catalog->scriptProjects.find(normalized);
    if (indexed == catalog->scriptProjects.end())
        return result;

    for (const std::string& projectId : indexed->second)
    {
        const auto project = catalog->projects.find(projectId);
        if (project == catalog->projects.end())
            continue;
        std::string snapshotError;
        result.push_back({project->second, store.loadWorld(projectId, &snapshotError)});
    }
    return result;
}

std::vector<ProjectContextCandidate> ContextProvider::findForScript(const std::string& scriptPath, std::string* error) const
{
    std::vector<ProjectContextCandidate> candidates = findAllForScript(scriptPath, error);
    if (candidates.size() <= 1)
        return candidates;

    const auto catalog = store.load(error);
    if (!catalog)
        return {};
    const std::string normalized = normalizeContextPath(scriptPath);
    const auto workspaceSelections = catalog->workspaceScriptSelections.find(workspaceDirectory);
    if (workspaceSelections != catalog->workspaceScriptSelections.end())
    {
        const auto selected = workspaceSelections->second.find(normalized);
        if (selected != workspaceSelections->second.end())
        {
            const auto candidate = std::find_if(candidates.begin(), candidates.end(),
                [&](const ProjectContextCandidate& item)
                {
                    return item.project.id == selected->second;
                });
            if (candidate != candidates.end())
                return {*candidate};
        }
    }

    const auto active = std::find_if(candidates.begin(), candidates.end(),
        [&](const ProjectContextCandidate& item)
        {
            return item.project.id == catalog->activeProjectId;
        });
    if (active != candidates.end())
        return {*active};
    return candidates;
}

json ContextProvider::inspect(const std::string& scriptPath) const
{
    std::string error;
    const std::vector<ProjectContextCandidate> candidates = findAllForScript(scriptPath, &error);
    json result = {{"script", normalizeContextPath(scriptPath)}, {"candidates", json::array()}};
    const auto catalog = store.load();
    if (!error.empty())
        result["error"] = error;
    for (const ProjectContextCandidate& candidate : candidates)
    {
        json item = candidate.project;
        item["active"] = false;
        item["selected"] = false;
        if (catalog)
        {
            item["active"] = catalog->activeProjectId == candidate.project.id;
            const auto workspaceSelections = catalog->workspaceScriptSelections.find(workspaceDirectory);
            if (workspaceSelections != catalog->workspaceScriptSelections.end())
            {
                const auto selected = workspaceSelections->second.find(normalizeContextPath(scriptPath));
                item["selected"] = selected != workspaceSelections->second.end() && selected->second == candidate.project.id;
            }
        }
        item["hasSnapshot"] = candidate.snapshot.has_value();
        if (candidate.snapshot)
        {
            item["tick"] = candidate.snapshot->tick;
            item["instanceCount"] = candidate.snapshot->instances.size();
        }
        result["candidates"].push_back(std::move(item));
    }
    return result;
}

ContextStoreResult ContextProvider::select(const std::string& scriptPath, const std::string& projectId) const
{
    return store.selectProjectForScript(workspaceDirectory, scriptPath, projectId);
}

ContextStoreResult ContextProvider::reset(const std::string& scriptPath) const
{
    return store.resetProjectForScript(workspaceDirectory, scriptPath);
}

std::optional<WorldDumpRequest> ContextProvider::createDumpRequest(const std::string& scriptPath, std::string* error) const
{
    const std::vector<ProjectContextCandidate> candidates = findForScript(scriptPath, error);
    if (candidates.size() != 1)
    {
        if (error && error->empty())
            *error = candidates.empty() ? "no project context contains this script" : "more than one project context contains this script";
        return std::nullopt;
    }
    const auto catalog = store.load(error);
    if (!catalog)
        return std::nullopt;
    if (catalog->activeProjectId != candidates[0].project.id)
    {
        if (error)
            *error = "the selected project is not active in Luduvo Studio";
        return std::nullopt;
    }
    if (candidates[0].project.fresh && candidates[0].snapshot)
    {
        if (error)
            *error = "the selected project already has a current world dump";
        return std::nullopt;
    }
    const std::filesystem::path output = std::filesystem::u8path(store.directory()) / "projects" / candidates[0].project.id / "world.dump.txt";
    const std::string outputPath = output.generic_u8string();
    return WorldDumpRequest{"world.dump " + outputPath, outputPath, candidates[0].project.id};
}

const ContextStore& ContextProvider::contextStore() const
{
    return store;
}

void setContextHelperExecutablePath(std::string path)
{
    executablePath = normalizeContextPath(path);
}

std::shared_ptr<ContextHelperSession> acquireContextHelper(const ClientLuduvoConfiguration& configuration, std::string* error)
{
    static std::mutex registryMutex;
    static std::map<std::string, std::weak_ptr<ContextHelperSession>> registry;
    if (executablePath.empty())
    {
        if (error)
            *error = "the language-server executable path is unavailable";
        return {};
    }
    const std::string stateDirectory = defaultContextStateDirectory(configuration.directories.local, configuration.directories.context);
    std::lock_guard guard(registryMutex);
    if (auto existing = registry[stateDirectory].lock())
        return existing;
    auto session = std::make_shared<ContextHelperSession>(
        stateDirectory, defaultCommandsLogPath(configuration.directories.roaming), defaultStudioLogPath(configuration.directories.local));
    if (!session->started())
    {
        if (error)
            *error = "the helper process could not be started";
        return {};
    }
    registry[stateDirectory] = session;
    return session;
}

std::shared_ptr<ContextNotificationSubscription> subscribeContextUpdates(const std::shared_ptr<ContextHelperSession>& session, const void* subscriber,
    std::function<void(const ContextUpdates&)> callback)
{
    if (!session || !subscriber)
        return {};
    session->subscribe(subscriber, std::move(callback));
    return std::make_shared<ContextNotificationSubscription>(session, subscriber);
}

} // namespace luduvo
