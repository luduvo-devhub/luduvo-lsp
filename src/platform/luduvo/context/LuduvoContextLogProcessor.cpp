#include "Platform/luduvo/context/LuduvoContextLogProcessor.hpp"

#include "LSP/Logs.hpp"
#include "Platform/luduvo/context/LuduvoProcessIdentity.hpp"
#include "LSP/AtomicFile.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <iostream>

namespace luduvo
{

namespace
{

static bool isWorldMutation(std::string_view command)
{
    return command.find("edit.") == 0 || command.find("insert.") == 0 || command.find("component.") == 0 || command.find("tag.") == 0 ||
           command == "file.import" || command == "file.new" || command == "file.save";
}

static std::pair<std::string, std::string> splitCommand(std::string_view line)
{
    const size_t separator = line.find(' ');
    if (separator == std::string_view::npos)
        return {std::string(line), {}};
    return {std::string(line.substr(0, separator)), std::string(line.substr(separator + 1))};
}

static std::string unquote(std::string value)
{
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        return value.substr(1, value.size() - 2);
    return value;
}

static bool isScriptFile(const std::filesystem::path& path)
{
    std::string extension = path.extension().generic_u8string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char value)
        {
            return char(std::tolower(value));
        });
    return extension == ".lua" || extension == ".luau";
}

static void reportStoreFailure(std::string_view operation, const ContextStoreResult& result)
{
    if (!result.success)
        logging::write(
            logging::Level::Error, "helper.storage", "Could not " + std::string(operation) + ": " + result.error.value_or("unknown error"));
}

} // namespace

ContextLogProcessor::ContextLogProcessor(ContextStore store)
    : store(std::move(store))
{
    const auto files = scanScriptFiles();
    for (const auto& [path, _] : files)
        knownScriptFiles.insert(path);
}

std::map<std::string, std::string> ContextLogProcessor::scanScriptFiles() const
{
    std::map<std::string, std::string> result;
    const auto catalog = store.load();
    if (!catalog)
        return result;
    const std::vector<std::string> liveValues = store.liveProjectIds();
    const std::set<std::string> live(liveValues.begin(), liveValues.end());
    for (const auto& [projectId, project] : catalog->projects)
    {
        if (!live.count(projectId) || project.directory.empty())
            continue;
        std::error_code error;
        const std::filesystem::path directory = std::filesystem::u8path(project.directory);
        if (!std::filesystem::is_directory(directory, error))
            continue;
        for (std::filesystem::recursive_directory_iterator iterator(directory, error), end; iterator != end && !error; iterator.increment(error))
        {
            if (!iterator->is_regular_file(error) || error || !isScriptFile(iterator->path()))
                continue;
            result[normalizeContextPath(iterator->path().generic_u8string())] = projectId;
        }
    }
    return result;
}

void ContextLogProcessor::staleLiveProjects(std::string reason, const std::vector<std::string>& preferred)
{
    const std::vector<std::string> projects = preferred.empty() ? store.liveProjectIds() : preferred;
    if (!projects.empty())
    {
        const auto result = store.markProjectsStale(projects, reason);
        if (result.success && result.affected)
            logging::write(logging::Level::Info, "helper.commands", "Marked " + std::to_string(result.affected) + " project(s) stale: " + reason);
        reportStoreFailure("mark live projects stale", result);
    }
}

bool ContextLogProcessor::ingestCommandLine(const std::string& line)
{
    if (line.empty() || line.compare(0, 4, "seq=") == 0)
        return true;
    const size_t separator = line.find(' ');
    if (separator == std::string::npos)
        return false;
    uint64_t sequence = 0;
    const std::string sequenceText = line.substr(0, separator);
    const auto parsed = std::from_chars(sequenceText.data(), sequenceText.data() + sequenceText.size(), sequence);
    if (parsed.ec != std::errc{} || parsed.ptr != sequenceText.data() + sequenceText.size())
        return false;
    const std::string value = line.substr(separator + 1);
    if (value.empty())
        return false;
    if (value == "ok")
        return true;

    const auto [command, rawArguments] = splitCommand(value);
    if (command.empty() || std::any_of(command.begin(), command.end(),
                               [](unsigned char character)
                               {
                                   return !std::isalnum(character) && character != '.' && character != '_' && character != '-';
                               }))
        return false;
    const std::string arguments = unquote(rawArguments);
    logging::write(logging::Level::Verbose, "helper.commands", "Observed command " + command + " sequence " + std::to_string(sequence));
    const bool worldMutation = isWorldMutation(command);
    if (worldMutation || command == "script.server" || command == "script.client" || (command == "script.attach" && !arguments.empty()))
        pendingDumps.clear(); // An older queued dump cannot make a later observed mutation fresh.

    if (command == "world.dump" && !arguments.empty())
    {
        pendingDumps.push_back({normalizeContextPath(arguments), std::chrono::steady_clock::now()});
        return true;
    }
    if (command == "script.attach" && !arguments.empty())
    {
        std::vector<std::string> matches;
        const auto catalog = store.load();
        const std::vector<std::string> live = store.liveProjectIds();
        if (catalog)
        {
            const std::string normalized = normalizeContextPath(arguments);
            for (const std::string& projectId : live)
            {
                const auto project = catalog->projects.find(projectId);
                if (project != catalog->projects.end() &&
                    std::find(project->second.scripts.begin(), project->second.scripts.end(), normalized) != project->second.scripts.end())
                    matches.push_back(projectId);
            }
        }
        if (matches.size() == 1)
        {
            reportStoreFailure("attach a script", store.attachScriptById(matches.front(), arguments));
            staleLiveProjects("script.attach changed the world after the last world dump", matches);
        }
        else
            staleLiveProjects("script.attach may have changed a live project after the last world dump");
        return true;
    }
    if (command == "script.client" || command == "script.server")
    {
        pendingScriptCreation = std::chrono::steady_clock::now();
        return true;
    }
    if (worldMutation)
        staleLiveProjects(command + " changed or may have changed a live project after the last world dump");
    return true;
}

void ContextLogProcessor::invalidateJournal()
{
    pendingDumps.clear();
    pendingScriptCreation.reset();
    staleLiveProjects("The command log contains unrecognized data; world changes may have been lost");
}

void ContextLogProcessor::pollFilesystem()
{
    const auto files = scanScriptFiles();
    std::map<std::string, std::string> added;
    for (const auto& [path, projectId] : files)
        if (!knownScriptFiles.count(path))
            added[path] = projectId;
    knownScriptFiles.clear();
    for (const auto& [path, _] : files)
        knownScriptFiles.insert(path);

    const auto observedAt = std::chrono::steady_clock::now();
    for (const auto& file : added)
        recentScriptFiles[file.first] = {file.second, observedAt};
    for (auto file = recentScriptFiles.begin(); file != recentScriptFiles.end();)
        if (observedAt - file->second.second > std::chrono::seconds(2))
            file = recentScriptFiles.erase(file);
        else
            ++file;

    if (pendingScriptCreation)
    {
        std::set<std::string> projects;
        for (const auto& file : recentScriptFiles)
            projects.insert(file.second.first);
        if (projects.size() == 1)
        {
            staleLiveProjects("script.client or script.server created and attached a script after the last world dump", {*projects.begin()});
            pendingScriptCreation.reset();
        }
        else if (!projects.empty() || std::chrono::steady_clock::now() - *pendingScriptCreation > std::chrono::seconds(2))
        {
            staleLiveProjects("script.client or script.server may have changed a live project after the last world dump");
            pendingScriptCreation.reset();
        }
    }

    const auto discovered = store.load();
    if (discovered)
        for (const auto& projectId : store.liveProjectIds())
        {
            const auto found = discovered->projects.find(projectId);
            if (found == discovered->projects.end() || found->second.directory.empty())
                continue;
            const auto& directory = found->second.directory;
            const auto source = readSharedFile((std::filesystem::u8path(directory) / "project.json").generic_u8string());
            if (!source)
                continue;
            const auto old = knownProjectFiles.find(projectId);
            const bool newFile = std::any_of(added.begin(), added.end(),
                [&](const auto& file)
                {
                    return file.second == projectId;
                });
            if (old == knownProjectFiles.end() || old->second != *source || newFile)
            {
                // Refresh membership without changing the existing catalog identity.
                const auto updated = store.updateProjectFiles(directory, found->second.name);
                reportStoreFailure("update discovered project scripts", updated);
                if (!updated.success)
                    continue;
            }
            knownProjectFiles[projectId] = *source;
        }

    const auto catalog = store.load();
    const std::vector<std::string> live = store.liveProjectIds();
    for (auto pending = pendingDumps.begin(); pending != pendingDumps.end();)
    {
        WorldDumpParseResult parsed = parseWorldDumpFile(pending->path);
        if (parsed.snapshot && catalog)
        {
            std::vector<std::string> matches;
            for (const auto& [projectId, project] : catalog->projects)
            {
                const std::string expected = normalizeContextPath(
                    (std::filesystem::u8path(store.directory()) / "projects" / project.id / "world.dump.txt").generic_u8string());
                if (pending->path == expected)
                    matches.push_back(projectId);
            }
            if (matches.empty() && live.size() == 1)
                matches = live;
            if (matches.size() == 1)
            {
                // The automatic-dump worker publishes provenance with the snapshot. Do not
                // race it by importing its command-log entry as an external dump first.
                const auto lock = std::filesystem::u8path(store.controlDirectory()) / ("autodump-" + matches.front() + ".lock") / "owner.json";
                const auto owner = readProcessIdentity(lock.generic_u8string());
                if (owner && isProcessAlive(*owner))
                {
                    ++pending;
                    continue;
                }
                const auto project = catalog->projects.find(matches.front());
                const auto published = store.loadWorld(matches.front());
                const bool alreadyPublished =
                    project != catalog->projects.end() && project->second.fresh && normalizeContextPath(project->second.dumpPath) == pending->path &&
                    published && published->tick == parsed.snapshot->tick && published->commandSequence == parsed.snapshot->commandSequence &&
                    nlohmann::json(published->instances) == nlohmann::json(parsed.snapshot->instances);
                if (!alreadyPublished)
                    reportStoreFailure("publish a world dump", store.updateWorldForProject(std::move(*parsed.snapshot), matches.front()));
            }
            logging::write(logging::Level::Debug, "helper.commands",
                "Processed world.dump at " + pending->path + "; matching projects: " + std::to_string(matches.size()));
            pending = pendingDumps.erase(pending);
        }
        else if (std::chrono::steady_clock::now() - pending->observedAt > std::chrono::seconds(45))
        {
            logging::write(logging::Level::Warning, "helper.commands",
                "Timed out waiting for dump " + pending->path + ": " + parsed.error.value_or("no valid snapshot"));
            pending = pendingDumps.erase(pending);
        }
        else
            ++pending;
    }
}

size_t ContextLogProcessor::pendingCommandCount() const
{
    return pendingDumps.size() + (pendingScriptCreation ? 1 : 0);
}

} // namespace luduvo
