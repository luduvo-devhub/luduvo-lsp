#include "Platform/luduvo/context/LuduvoContextStore.hpp"

#include "LSP/Utils.hpp"
#include "LuauFileUtils.hpp"
#include "Platform/luduvo/context/LuduvoContextInspection.hpp"
#include "Platform/luduvo/context/LuduvoProcessIdentity.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
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
#include <unistd.h>
#endif

namespace luduvo
{

using json = nlohmann::json;

static int64_t unixTimeMilliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

static uint64_t processId()
{
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return uint64_t(getpid());
#endif
}

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

std::string normalizeContextPath(const std::string& path)
{
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::absolute(std::filesystem::u8path(path), error);
    if (error)
        absolute = std::filesystem::u8path(path);
    std::string result = absolute.lexically_normal().generic_u8string();
#ifdef _WIN32
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char character)
        {
            return char(std::tolower(character));
        });
#endif
    return result;
}

static bool replaceFile(const std::filesystem::path& source, const std::filesystem::path& destination, std::error_code& error)
{
#ifdef _WIN32
    if (MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
    error = std::error_code(int(GetLastError()), std::system_category());
    return false;
#else
    std::filesystem::rename(source, destination, error);
    return !error;
#endif
}

static bool writeAtomically(const std::string& path, const std::string& contents, std::string* errorMessage)
{
    const std::filesystem::path destination = std::filesystem::u8path(path);
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error)
    {
        if (errorMessage)
            *errorMessage = "could not create " + destination.parent_path().generic_u8string() + ": " + error.message();
        return false;
    }

    static std::atomic<uint64_t> temporarySequence{0};
    const std::filesystem::path temporary = destination.string() + ".tmp." + std::to_string(processId()) + "." +
                                            std::to_string(unixTimeMilliseconds()) + "." + std::to_string(temporarySequence++);
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(contents.data(), std::streamsize(contents.size()));
        if (!output)
        {
            if (errorMessage)
                *errorMessage = "could not write " + temporary.generic_u8string();
            return false;
        }
    }

    if (!replaceFile(temporary, destination, error))
    {
        std::filesystem::remove(temporary);
        if (errorMessage)
            *errorMessage = "could not replace " + destination.generic_u8string() + ": " + error.message();
        return false;
    }
    return true;
}

static std::shared_ptr<std::mutex> localCatalogMutex(const std::string& stateDirectory)
{
    static std::mutex registryMutex;
    static std::map<std::string, std::weak_ptr<std::mutex>> registry;
    std::lock_guard guard(registryMutex);
    std::weak_ptr<std::mutex>& entry = registry[normalizeContextPath(stateDirectory)];
    if (auto existing = entry.lock())
        return existing;
    auto created = std::make_shared<std::mutex>();
    entry = created;
    return created;
}

class CatalogWriteLock
{
    std::shared_ptr<std::mutex> localMutex;
    std::unique_lock<std::mutex> localLock;
    std::filesystem::path path;
    bool acquired = false;

public:
    CatalogWriteLock(const std::string& stateDirectory, std::string* errorMessage)
        : localMutex(localCatalogMutex(stateDirectory))
        , localLock(*localMutex)
        , path(std::filesystem::u8path(stateDirectory) / "catalog.lock")
    {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error)
        {
            if (errorMessage)
                *errorMessage = "could not create context state directory: " + error.message();
            return;
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline)
        {
            error.clear();
            if (std::filesystem::create_directory(path, error))
            {
                acquired = true;
                return;
            }
            if (error && error != std::errc::file_exists)
            {
                if (errorMessage)
                    *errorMessage = "could not acquire context catalog lock: " + error.message();
                return;
            }

            error.clear();
            const auto updated = std::filesystem::last_write_time(path, error);
            if (!error && std::filesystem::file_time_type::clock::now() - updated > std::chrono::seconds(30))
            {
                std::filesystem::remove_all(path, error);
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (errorMessage)
            *errorMessage = "timed out waiting for the context catalog lock";
    }

    ~CatalogWriteLock()
    {
        if (!acquired)
            return;
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    explicit operator bool() const
    {
        return acquired;
    }
};

static std::string projectSlug(std::string_view name)
{
    std::string result;
    bool separator = false;
    for (unsigned char character : name)
    {
        if (std::isalnum(character))
        {
            if (separator && !result.empty())
                result.push_back('-');
            result.push_back(char(std::tolower(character)));
            separator = false;
        }
        else
            separator = true;
    }
    return result.empty() ? "project" : result;
}

static std::string shortPathHash(std::string_view value)
{
    uint64_t hash = 14695981039346656037ull;
    for (unsigned char character : value)
    {
        hash ^= character;
        hash *= 1099511628211ull;
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(8) << uint32_t(hash);
    return output.str();
}

static void rebuildScriptIndex(ContextCatalog& catalog)
{
    catalog.scriptProjects.clear();
    for (const auto& [projectId, project] : catalog.projects)
        for (const std::string& script : project.scripts)
            catalog.scriptProjects[normalizeContextPath(script)].push_back(projectId);

    for (auto& [_, projects] : catalog.scriptProjects)
    {
        std::sort(projects.begin(), projects.end());
        projects.erase(std::unique(projects.begin(), projects.end()), projects.end());
    }
}

static void rebuildProjectScripts(ContextProject& project)
{
    std::set<std::string> scripts;
    scripts.insert(project.declaredScripts.begin(), project.declaredScripts.end());
    scripts.insert(project.autorunScripts.begin(), project.autorunScripts.end());
    scripts.insert(project.worldScripts.begin(), project.worldScripts.end());
    scripts.insert(project.attachedScripts.begin(), project.attachedScripts.end());
    project.scripts.assign(scripts.begin(), scripts.end());
}

ContextStore::ContextStore(std::string stateDirectory)
    : stateDirectory(normalizeContextPath(stateDirectory))
{
}

const std::string& ContextStore::directory() const
{
    return stateDirectory;
}

std::string ContextStore::catalogPath() const
{
    return joinPath(stateDirectory, "catalog.json");
}

std::string ContextStore::leasesDirectory() const
{
    return joinPath(stateDirectory, "clients");
}

std::string ContextStore::controlDirectory() const
{
    return joinPath(stateDirectory, "control");
}

std::string ContextStore::stopPath() const
{
    return joinPath(controlDirectory(), "stop");
}

std::string ContextStore::ownerPath() const
{
    return joinPath(controlDirectory(), "owner.json");
}

bool ContextStore::helperRunning() const
{
    std::error_code error;
    if (!std::filesystem::is_directory(std::filesystem::u8path(controlDirectory()) / "lock", error))
        return false;
    const auto owner = readProcessIdentity(ownerPath());
    return owner && isProcessAlive(*owner);
}

std::optional<ContextCatalog> ContextStore::load(std::string* error) const
{
    const std::optional<std::string> source = Luau::FileUtils::readFile(catalogPath());
    if (!source)
        return ContextCatalog{};
    try
    {
        ContextCatalog catalog = json::parse(*source).get<ContextCatalog>();
        if (catalog.formatVersion != 1)
        {
            if (error)
                *error = "unsupported context catalog version " + std::to_string(catalog.formatVersion);
            return std::nullopt;
        }
        return catalog;
    }
    catch (const json::exception& exception)
    {
        if (error)
            *error = "could not parse " + catalogPath() + ": " + exception.what();
        return std::nullopt;
    }
}

ContextStoreResult ContextStore::save(ContextCatalog& catalog) const
{
    rebuildScriptIndex(catalog);
    ++catalog.generation;
    std::string error;
    if (!writeAtomically(catalogPath(), json(catalog).dump(2) + "\n", &error))
        return {false, std::nullopt, std::move(error)};
    return {true, std::nullopt, std::nullopt};
}

std::string ContextStore::selectProjectId(ContextCatalog& catalog, const std::string& name, const std::string& directory) const
{
    const std::string normalizedDirectory = directory.empty() ? std::string{} : normalizeContextPath(directory);
    for (const auto& [id, project] : catalog.projects)
        if (project.name == name && (normalizedDirectory.empty() || normalizeContextPath(project.directory) == normalizedDirectory))
            return id;

    const std::string base = projectSlug(name);
    if (!catalog.projects.count(base))
        return base;
    return base + "-" + shortPathHash(normalizedDirectory);
}

ContextStoreResult ContextStore::updateProjectFiles(const std::string& projectDirectory, std::optional<std::string> configuredProjectName) const
{
    ProjectFileInspectionResult inspected = inspectProjectFiles(projectDirectory, configuredProjectName);
    if (!inspected.project)
        return {false, std::nullopt, inspected.error.value_or("could not inspect project files")};
    const std::string& directory = inspected.project->directory;

    std::string error;
    CatalogWriteLock lock(stateDirectory, &error);
    if (!lock)
        return {false, std::nullopt, std::move(error)};
    auto catalog = load(&error);
    if (!catalog)
        return {false, std::nullopt, std::move(error)};
    const std::string& name = inspected.project->name;
    const std::string id = selectProjectId(*catalog, name, directory);
    ContextProject& project = catalog->projects[id];
    project.id = id;
    project.name = name;
    project.directory = directory;
    project.updatedAtUnixMs = unixTimeMilliseconds();
    project.declaredScripts = inspected.project->declaredScripts;
    project.autorunScripts = inspected.project->autorunScripts;
    rebuildProjectScripts(project);
    ContextStoreResult saved = save(*catalog);
    saved.projectId = id;
    return saved;
}

ContextStoreResult ContextStore::updateWorld(
    WorldDumpSnapshot snapshot, const std::string& projectName, const std::optional<std::string>& projectDirectory) const
{
    std::string error;
    CatalogWriteLock lock(stateDirectory, &error);
    if (!lock)
        return {false, std::nullopt, std::move(error)};
    auto catalog = load(&error);
    if (!catalog)
        return {false, std::nullopt, std::move(error)};

    const std::string directory = projectDirectory.value_or("");
    const std::string id = selectProjectId(*catalog, projectName, directory);
    ContextProject& project = catalog->projects[id];
    project.id = id;
    project.name = projectName;
    if (!directory.empty())
        project.directory = normalizeContextPath(directory);
    project.snapshotPath = joinPath(joinPath(stateDirectory, "projects"), id + "/world.json");
    project.dumpPath = snapshot.sourcePath;
    project.updatedAtUnixMs = unixTimeMilliseconds();
    project.worldGeneration = catalog->generation + 1;
    project.instanceCount = snapshot.instances.size();
    project.scriptAttachmentCount = 0;
    for (const WorldInstance& instance : snapshot.instances)
        project.scriptAttachmentCount += instance.scripts.size();
    project.fresh = true;
    project.staleReason.clear();

    project.worldScripts.clear();
    if (!project.directory.empty())
    {
        for (const WorldInstance& instance : snapshot.instances)
            for (const std::string& script : instance.scripts)
                if (script.compare(0, 7, "core://") != 0)
                    project.worldScripts.push_back(normalizeContextPath(joinPath(project.directory, script)));
    }
    std::sort(project.worldScripts.begin(), project.worldScripts.end());
    project.worldScripts.erase(std::unique(project.worldScripts.begin(), project.worldScripts.end()), project.worldScripts.end());
    project.attachedScripts.clear();
    rebuildProjectScripts(project);

    if (!writeAtomically(project.snapshotPath, json(snapshot).dump(2) + "\n", &error))
        return {false, id, std::move(error)};
    ContextStoreResult saved = save(*catalog);
    saved.projectId = id;
    return saved;
}

ContextStoreResult ContextStore::setActiveProject(const std::string& projectName) const
{
    std::string error;
    CatalogWriteLock lock(stateDirectory, &error);
    if (!lock)
        return {false, std::nullopt, std::move(error)};
    auto catalog = load(&error);
    if (!catalog)
        return {false, std::nullopt, std::move(error)};
    const std::string id = selectProjectId(*catalog, projectName, "");
    ContextProject& project = catalog->projects[id];
    project.id = id;
    project.name = projectName;
    project.updatedAtUnixMs = unixTimeMilliseconds();
    catalog->activeProjectId = id;
    ContextStoreResult saved = save(*catalog);
    saved.projectId = id;
    return saved;
}

ContextStoreResult ContextStore::markProjectStale(const std::string& projectName, std::string reason) const
{
    std::string error;
    CatalogWriteLock lock(stateDirectory, &error);
    if (!lock)
        return {false, std::nullopt, std::move(error)};
    auto catalog = load(&error);
    if (!catalog)
        return {false, std::nullopt, std::move(error)};
    const std::string id = selectProjectId(*catalog, projectName, "");
    ContextProject& project = catalog->projects[id];
    project.id = id;
    project.name = projectName;
    project.updatedAtUnixMs = unixTimeMilliseconds();
    project.fresh = false;
    project.staleReason = std::move(reason);
    ContextStoreResult saved = save(*catalog);
    saved.projectId = id;
    return saved;
}

ContextStoreResult ContextStore::attachScript(const std::string& projectName, const std::string& scriptPath) const
{
    std::string error;
    CatalogWriteLock lock(stateDirectory, &error);
    if (!lock)
        return {false, std::nullopt, std::move(error)};
    auto catalog = load(&error);
    if (!catalog)
        return {false, std::nullopt, std::move(error)};
    const std::string id = selectProjectId(*catalog, projectName, "");
    ContextProject& project = catalog->projects[id];
    project.id = id;
    project.name = projectName;
    std::string path = scriptPath;
    if (!project.directory.empty() && !std::filesystem::u8path(path).is_absolute())
        path = joinPath(project.directory, path);
    project.attachedScripts.push_back(normalizeContextPath(path));
    std::sort(project.attachedScripts.begin(), project.attachedScripts.end());
    project.attachedScripts.erase(std::unique(project.attachedScripts.begin(), project.attachedScripts.end()), project.attachedScripts.end());
    rebuildProjectScripts(project);
    project.updatedAtUnixMs = unixTimeMilliseconds();
    ContextStoreResult saved = save(*catalog);
    saved.projectId = id;
    return saved;
}

ContextStoreResult ContextStore::selectProjectForScript(
    const std::string& workspaceDirectory, const std::string& scriptPath, const std::string& projectId) const
{
    std::string error;
    CatalogWriteLock lock(stateDirectory, &error);
    if (!lock)
        return {false, std::nullopt, std::move(error)};
    auto catalog = load(&error);
    if (!catalog)
        return {false, std::nullopt, std::move(error)};
    if (!catalog->projects.count(projectId))
        return {false, std::nullopt, "unknown context project: " + projectId};
    const std::string normalized = normalizeContextPath(scriptPath);
    const auto candidates = catalog->scriptProjects.find(normalized);
    if (candidates == catalog->scriptProjects.end() ||
        std::find(candidates->second.begin(), candidates->second.end(), projectId) == candidates->second.end())
        return {false, std::nullopt, "project '" + projectId + "' does not contain " + scriptPath};
    catalog->workspaceScriptSelections[normalizeContextPath(workspaceDirectory)][normalized] = projectId;
    ContextStoreResult saved = save(*catalog);
    saved.projectId = projectId;
    return saved;
}

ContextStoreResult ContextStore::resetProjectForScript(const std::string& workspaceDirectory, const std::string& scriptPath) const
{
    std::string error;
    CatalogWriteLock lock(stateDirectory, &error);
    if (!lock)
        return {false, std::nullopt, std::move(error)};
    auto catalog = load(&error);
    if (!catalog)
        return {false, std::nullopt, std::move(error)};
    const std::string workspace = normalizeContextPath(workspaceDirectory);
    if (auto selections = catalog->workspaceScriptSelections.find(workspace); selections != catalog->workspaceScriptSelections.end())
    {
        selections->second.erase(normalizeContextPath(scriptPath));
        if (selections->second.empty())
            catalog->workspaceScriptSelections.erase(selections);
    }
    return save(*catalog);
}

std::optional<WorldDumpSnapshot> ContextStore::loadWorld(const std::string& projectId, std::string* error) const
{
    std::string catalogError;
    const auto catalog = load(&catalogError);
    if (!catalog)
    {
        if (error)
            *error = std::move(catalogError);
        return std::nullopt;
    }
    const auto project = catalog->projects.find(projectId);
    if (project == catalog->projects.end() || project->second.snapshotPath.empty())
    {
        if (error)
            *error = "project '" + projectId + "' has no world snapshot";
        return std::nullopt;
    }
    const auto source = Luau::FileUtils::readFile(project->second.snapshotPath);
    if (!source)
    {
        if (error)
            *error = "could not read " + project->second.snapshotPath;
        return std::nullopt;
    }
    try
    {
        return json::parse(*source).get<WorldDumpSnapshot>();
    }
    catch (const json::exception& exception)
    {
        if (error)
            *error = "could not parse " + project->second.snapshotPath + ": " + exception.what();
        return std::nullopt;
    }
}

static bool matchesCandidate(const ContextCleanupCandidate& candidate, const std::string& selector)
{
    return selector == candidate.id || selector == candidate.label || normalizeContextPath(selector) == normalizeContextPath(candidate.path);
}

ContextCleanupSummary ContextStore::cleanupCandidates() const
{
    ContextCleanupSummary result;
    std::error_code error;
    const std::filesystem::path clients = std::filesystem::u8path(leasesDirectory());
    if (std::filesystem::is_directory(clients, error))
    {
        for (std::filesystem::directory_iterator iterator(clients, error), end; iterator != end && !error; iterator.increment(error))
        {
            if (!iterator->is_regular_file(error) || error)
                continue;
            const auto identity = readProcessIdentity(iterator->path().generic_u8string());
            if (identity && isProcessAlive(*identity))
                continue;
            const std::string id = identity ? std::to_string(identity->processId) : iterator->path().stem().generic_u8string();
            result.clients.push_back({id, "PID " + id, iterator->path().generic_u8string()});
        }
    }

    std::string catalogError;
    const auto catalog = load(&catalogError);
    if (!catalog)
        return result;
    std::set<std::string> referencedSnapshots;
    for (const auto& [id, project] : catalog->projects)
    {
        if (!project.snapshotPath.empty())
            referencedSnapshots.insert(normalizeContextPath(project.snapshotPath));
        if (project.directory.empty())
            continue;
        error.clear();
        if (!std::filesystem::is_directory(std::filesystem::u8path(project.directory), error))
            result.projects.push_back({id, project.name, project.directory});
    }

    const std::filesystem::path projects = std::filesystem::u8path(stateDirectory) / "projects";
    error.clear();
    if (std::filesystem::is_directory(projects, error))
    {
        for (std::filesystem::recursive_directory_iterator iterator(projects, error), end; iterator != end && !error; iterator.increment(error))
        {
            if (!iterator->is_regular_file(error) || error || iterator->path().filename() != "world.json")
                continue;
            const std::string path = normalizeContextPath(iterator->path().generic_u8string());
            if (!referencedSnapshots.count(path))
                result.snapshots.push_back({path, iterator->path().parent_path().filename().generic_u8string(), path});
        }
    }

    auto sortCandidates = [](std::vector<ContextCleanupCandidate>& candidates)
    {
        std::sort(candidates.begin(), candidates.end(),
            [](const ContextCleanupCandidate& left, const ContextCleanupCandidate& right)
            {
                if (left.label != right.label)
                    return left.label < right.label;
                return left.path < right.path;
            });
    };
    sortCandidates(result.clients);
    sortCandidates(result.projects);
    sortCandidates(result.snapshots);
    return result;
}

std::optional<ContextStatus> ContextStore::status(std::string* error) const
{
    const auto catalog = load(error);
    if (!catalog)
        return std::nullopt;

    ContextStatus result;
    result.stateDirectory = directory();
    result.catalogPath = this->catalogPath();
    result.catalogGeneration = catalog->generation;
    result.projectCount = catalog->projects.size();

    if (const auto owner = readProcessIdentity(ownerPath()); owner && isProcessAlive(*owner))
    {
        result.running = true;
        result.helperProcessId = owner->processId;
    }

    if (const auto active = catalog->projects.find(catalog->activeProjectId); active != catalog->projects.end())
    {
        result.hasActiveProject = true;
        result.activeProjectId = active->first;
        result.activeProjectName = active->second.name;
        result.activeProjectFresh = active->second.fresh;
    }

    std::error_code filesystemError;
    const std::filesystem::path clients = std::filesystem::u8path(leasesDirectory());
    if (std::filesystem::is_directory(clients, filesystemError))
    {
        for (std::filesystem::directory_iterator iterator(clients, filesystemError), end; iterator != end && !filesystemError;
             iterator.increment(filesystemError))
        {
            if (!iterator->is_regular_file(filesystemError) || filesystemError)
                continue;
            const auto identity = readProcessIdentity(iterator->path().generic_u8string());
            if (identity && isProcessAlive(*identity))
                result.clientProcessIds.push_back(identity->processId);
        }
    }
    std::sort(result.clientProcessIds.begin(), result.clientProcessIds.end());
    result.clientProcessIds.erase(std::unique(result.clientProcessIds.begin(), result.clientProcessIds.end()), result.clientProcessIds.end());

    const ContextCleanupSummary cleanup = cleanupCandidates();
    result.deadClientCount = cleanup.clients.size();
    result.missingProjectCount = cleanup.projects.size();
    result.orphanedSnapshotCount = cleanup.snapshots.size();
    return result;
}

std::optional<ContextOverview> ContextStore::overview(std::string* error) const
{
    const auto catalog = load(error);
    if (!catalog)
        return std::nullopt;
    return ContextOverview{directory(), *catalog, cleanupCandidates()};
}

ContextStoreResult ContextStore::clean(const std::string& kind, const std::vector<std::string>& selectors, bool all) const
{
    if (!all && selectors.empty())
        return {false, std::nullopt, "clean requires one or more selectors or --all"};
    if (!kind.empty() && kind != "client" && kind != "project" && kind != "snapshot")
        return {false, std::nullopt, "unsupported clean subject: " + kind};

    const ContextCleanupSummary candidates = cleanupCandidates();
    struct SelectedCandidate
    {
        std::string kind;
        const ContextCleanupCandidate* candidate;
    };
    std::vector<SelectedCandidate> selectedCandidates;
    auto appendCandidates = [&](const std::string& candidateKind, const std::vector<ContextCleanupCandidate>& values)
    {
        if (!kind.empty() && kind != candidateKind)
            return;
        for (const ContextCleanupCandidate& candidate : values)
            selectedCandidates.push_back({candidateKind, &candidate});
    };
    appendCandidates("client", candidates.clients);
    appendCandidates("project", candidates.projects);
    appendCandidates("snapshot", candidates.snapshots);

    if (!all)
    {
        std::vector<SelectedCandidate> resolved;
        for (const std::string& selector : selectors)
        {
            std::vector<SelectedCandidate> matches;
            std::copy_if(selectedCandidates.begin(), selectedCandidates.end(), std::back_inserter(matches),
                [&](const SelectedCandidate& candidate)
                {
                    return matchesCandidate(*candidate.candidate, selector);
                });
            if (matches.empty())
                return {false, std::nullopt, "no cleanup candidate matched: " + selector};
            if (matches.size() > 1)
                return {false, std::nullopt, "cleanup selector is ambiguous: " + selector};
            if (std::none_of(resolved.begin(), resolved.end(),
                    [&](const SelectedCandidate& candidate)
                    {
                        return candidate.kind == matches.front().kind && candidate.candidate->path == matches.front().candidate->path;
                    }))
                resolved.push_back(matches.front());
        }
        selectedCandidates = std::move(resolved);
    }

    auto isSelected = [&](const std::string& candidateKind, const ContextCleanupCandidate& candidate)
    {
        return std::any_of(selectedCandidates.begin(), selectedCandidates.end(),
            [&](const SelectedCandidate& selected)
            {
                return selected.kind == candidateKind && selected.candidate->path == candidate.path;
            });
    };
    size_t affected = 0;

    if (kind.empty() || kind == "client")
    {
        for (const ContextCleanupCandidate& candidate : candidates.clients)
        {
            if (!isSelected("client", candidate))
                continue;
            std::error_code error;
            if (!std::filesystem::remove(std::filesystem::u8path(candidate.path), error) && error)
                return {false, std::nullopt, "could not remove " + candidate.path + ": " + error.message(), affected};
            ++affected;
        }
    }

    std::vector<std::string> projectDirectories;
    bool catalogChanged = false;
    std::string error;
    CatalogWriteLock lock(stateDirectory, &error);
    if (!lock)
        return {false, std::nullopt, std::move(error), affected};
    auto catalog = load(&error);
    if (!catalog)
        return {false, std::nullopt, std::move(error), affected};

    if (kind.empty() || kind == "project")
    {
        for (const ContextCleanupCandidate& candidate : candidates.projects)
        {
            if (!isSelected("project", candidate))
                continue;
            const auto project = catalog->projects.find(candidate.id);
            if (project == catalog->projects.end())
                continue;
            if (!project->second.snapshotPath.empty())
                projectDirectories.push_back(std::filesystem::u8path(project->second.snapshotPath).parent_path().generic_u8string());
            catalog->projects.erase(project);
            if (catalog->activeProjectId == candidate.id)
                catalog->activeProjectId.clear();
            for (auto& [_, selections] : catalog->workspaceScriptSelections)
                for (auto selection = selections.begin(); selection != selections.end();)
                    selection = selection->second == candidate.id ? selections.erase(selection) : std::next(selection);
            catalogChanged = true;
            ++affected;
        }
    }

    if (catalogChanged)
    {
        ContextStoreResult saved = save(*catalog);
        if (!saved.success)
            return {false, std::nullopt, saved.error, affected};
    }

    for (const std::string& directory : projectDirectories)
    {
        std::error_code filesystemError;
        std::filesystem::remove_all(std::filesystem::u8path(directory), filesystemError);
        if (filesystemError)
            return {false, std::nullopt, "could not remove " + directory + ": " + filesystemError.message(), affected};
    }

    if (kind.empty() || kind == "snapshot")
    {
        for (const ContextCleanupCandidate& candidate : candidates.snapshots)
        {
            if (!isSelected("snapshot", candidate))
                continue;
            std::error_code filesystemError;
            if (!std::filesystem::remove(std::filesystem::u8path(candidate.path), filesystemError) && filesystemError)
                return {false, std::nullopt, "could not remove " + candidate.path + ": " + filesystemError.message(), affected};
            ++affected;
        }
    }

    return {true, std::nullopt, std::nullopt, affected};
}

ContextStoreResult ContextStore::forget(const std::string& kind, const std::vector<std::string>& selectors, bool all) const
{
    if (!all && selectors.empty())
        return {false, std::nullopt, "forget requires one or more selectors or --all"};
    if (!kind.empty() && kind != "project" && kind != "script")
        return {false, std::nullopt, "unsupported forget subject: " + kind};

    std::string error;
    CatalogWriteLock lock(stateDirectory, &error);
    if (!lock)
        return {false, std::nullopt, std::move(error)};
    auto catalog = load(&error);
    if (!catalog)
        return {false, std::nullopt, std::move(error)};

    std::set<std::string> selectedProjects;
    std::set<std::string> selectedScripts;
    if (all)
    {
        if (kind.empty() || kind == "project")
            for (const auto& [projectId, _] : catalog->projects)
                selectedProjects.insert(projectId);
        if (kind.empty() || kind == "script")
            for (const auto& [_, selections] : catalog->workspaceScriptSelections)
                for (const auto& [scriptPath, __] : selections)
                    selectedScripts.insert(scriptPath);
    }
    else
    {
        for (const std::string& selector : selectors)
        {
            std::vector<std::pair<std::string, std::string>> matches;
            if (kind.empty() || kind == "project")
            {
                for (const auto& [projectId, project] : catalog->projects)
                {
                    const std::string normalizedDirectory = project.directory.empty() ? std::string{} : normalizeContextPath(project.directory);
                    if (selector == projectId || selector == project.name ||
                        (!normalizedDirectory.empty() && normalizeContextPath(selector) == normalizedDirectory))
                        matches.emplace_back("project", projectId);
                }
            }
            if (kind.empty() || kind == "script")
            {
                const std::string normalizedSelector = normalizeContextPath(selector);
                for (const auto& [_, selections] : catalog->workspaceScriptSelections)
                    if (selections.count(normalizedSelector))
                        matches.emplace_back("script", normalizedSelector);
                std::sort(matches.begin(), matches.end());
                matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
            }
            if (matches.empty())
                return {false, std::nullopt, "no stored context matched: " + selector};
            if (matches.size() > 1)
                return {false, std::nullopt, "context selector is ambiguous: " + selector};
            if (matches.front().first == "project")
                selectedProjects.insert(matches.front().second);
            else
                selectedScripts.insert(matches.front().second);
        }
    }

    size_t affected = 0;
    std::vector<std::string> projectDirectories;
    if (kind.empty() || kind == "project")
    {
        for (auto project = catalog->projects.begin(); project != catalog->projects.end();)
        {
            if (!selectedProjects.count(project->first))
            {
                ++project;
                continue;
            }
            if (!project->second.snapshotPath.empty())
                projectDirectories.push_back(std::filesystem::u8path(project->second.snapshotPath).parent_path().generic_u8string());
            const std::string projectId = project->first;
            if (catalog->activeProjectId == projectId)
                catalog->activeProjectId.clear();
            for (auto& [_, selections] : catalog->workspaceScriptSelections)
                for (auto selection = selections.begin(); selection != selections.end();)
                    selection = selection->second == projectId ? selections.erase(selection) : std::next(selection);
            project = catalog->projects.erase(project);
            ++affected;
        }
    }

    if (kind.empty() || kind == "script")
    {
        for (auto workspace = catalog->workspaceScriptSelections.begin(); workspace != catalog->workspaceScriptSelections.end();)
        {
            for (auto selection = workspace->second.begin(); selection != workspace->second.end();)
            {
                if (!selectedScripts.count(selection->first))
                {
                    ++selection;
                    continue;
                }
                selection = workspace->second.erase(selection);
                ++affected;
            }
            workspace = workspace->second.empty() ? catalog->workspaceScriptSelections.erase(workspace) : std::next(workspace);
        }
    }

    ContextStoreResult saved = save(*catalog);
    if (!saved.success)
        return {false, std::nullopt, saved.error, affected};
    for (const std::string& directory : projectDirectories)
    {
        std::error_code filesystemError;
        std::filesystem::remove_all(std::filesystem::u8path(directory), filesystemError);
        if (filesystemError)
            return {false, std::nullopt, "could not remove " + directory + ": " + filesystemError.message(), affected};
    }
    return {true, std::nullopt, std::nullopt, affected};
}

std::string defaultContextStateDirectory(const std::string& configuredLocalDirectory, const std::string& configuredContextDirectory)
{
    if (!configuredContextDirectory.empty())
        return normalizeContextPath(configuredContextDirectory);
    if (!configuredLocalDirectory.empty())
        return joinPath(configuredLocalDirectory, "luduvo-lsp/context");
#ifdef _WIN32
    if (auto local = environmentVariable("LOCALAPPDATA"))
        return joinPath(*local, "Luduvo/luduvo-lsp/context");
#elif defined(__APPLE__)
    if (auto home = getHomeDirectory())
        return joinPath(*home, "Library/Caches/Luduvo/luduvo-lsp/context");
#else
    if (auto state = environmentVariable("XDG_STATE_HOME"))
        return joinPath(*state, "luduvo/context");
    if (auto home = getHomeDirectory())
        return joinPath(*home, ".local/state/luduvo/context");
#endif
    return "luduvo-context";
}

std::string defaultCommandsLogPath(const std::string& configuredRoamingDirectory)
{
    if (!configuredRoamingDirectory.empty())
        return joinPath(configuredRoamingDirectory, "Luduvo Studio/commands.log");
#ifdef _WIN32
    if (auto roaming = environmentVariable("APPDATA"))
        return joinPath(*roaming, "Luduvo/Luduvo Studio/commands.log");
#elif defined(__APPLE__)
    if (auto home = getHomeDirectory())
        return joinPath(*home, "Library/Application Support/Luduvo/Luduvo Studio/commands.log");
#else
    if (auto data = environmentVariable("XDG_DATA_HOME"))
        return joinPath(*data, "Luduvo/Luduvo Studio/commands.log");
    if (auto home = getHomeDirectory())
        return joinPath(*home, ".local/share/Luduvo/Luduvo Studio/commands.log");
#endif
    return "commands.log";
}

std::string defaultStudioLogPath(const std::string& configuredLocalDirectory)
{
    if (!configuredLocalDirectory.empty())
        return joinPath(configuredLocalDirectory, "studio.log");
#ifdef _WIN32
    if (auto local = environmentVariable("LOCALAPPDATA"))
        return joinPath(*local, "Luduvo/studio.log");
#elif defined(__APPLE__)
    if (auto home = getHomeDirectory())
        return joinPath(*home, "Library/Logs/Luduvo/studio.log");
#else
    if (auto state = environmentVariable("XDG_STATE_HOME"))
        return joinPath(*state, "Luduvo/studio.log");
    if (auto home = getHomeDirectory())
        return joinPath(*home, ".local/state/Luduvo/studio.log");
#endif
    return "studio.log";
}

} // namespace luduvo
