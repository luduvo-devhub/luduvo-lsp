#include "Platform/luduvo/context/LuduvoContextLogProcessor.hpp"

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
    return command.find("edit.") == 0 || command.find("insert.") == 0 || command.find("component.") == 0 ||
           command.find("tag.") == 0 || command == "file.import" || command == "file.new";
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
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value)
        {
            return char(std::tolower(value));
        });
    return extension == ".lua" || extension == ".luau";
}

static void reportStoreFailure(std::string_view operation, const ContextStoreResult& result)
{
    if (!result.success)
        std::cerr << "Luduvo context helper could not " << operation << ": " << result.error.value_or("unknown error") << '\n';
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
        reportStoreFailure("mark live projects stale", store.markProjectsStale(projects, std::move(reason)));
}

void ContextLogProcessor::ingestCommandLine(const std::string& line)
{
    const size_t separator = line.find(' ');
    if (separator == std::string::npos)
        return;
    uint64_t sequence = 0;
    const std::string sequenceText = line.substr(0, separator);
    const auto parsed = std::from_chars(sequenceText.data(), sequenceText.data() + sequenceText.size(), sequence);
    if (parsed.ec != std::errc{} || parsed.ptr != sequenceText.data() + sequenceText.size())
        return;
    const std::string value = line.substr(separator + 1);
    if (value == "ok" || value.empty())
        return;

    const auto [command, rawArguments] = splitCommand(value);
    const std::string arguments = unquote(rawArguments);
    if (command == "world.dump" && !arguments.empty())
    {
        pendingDumps.push_back({normalizeContextPath(arguments), std::chrono::steady_clock::now()});
        return;
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
        return;
    }
    if (command == "script.client" || command == "script.server")
    {
        pendingScriptCreation = std::chrono::steady_clock::now();
        return;
    }
    if (isWorldMutation(command))
        staleLiveProjects(command + " changed or may have changed a live project after the last world dump");
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

    if (pendingScriptCreation)
    {
        std::set<std::string> projects;
        for (const auto& [_, projectId] : added)
            projects.insert(projectId);
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
                const auto project = catalog->projects.find(matches.front());
                const bool alreadyPublished = project != catalog->projects.end() && project->second.fresh &&
                                              normalizeContextPath(project->second.dumpPath) == pending->path;
                if (!alreadyPublished)
                    reportStoreFailure("publish a world dump", store.updateWorldForProject(std::move(*parsed.snapshot), matches.front()));
            }
            pending = pendingDumps.erase(pending);
        }
        else if (std::chrono::steady_clock::now() - pending->observedAt > std::chrono::seconds(45))
            pending = pendingDumps.erase(pending);
        else
            ++pending;
    }
}

size_t ContextLogProcessor::pendingCommandCount() const
{
    return pendingDumps.size() + (pendingScriptCreation ? 1 : 0);
}

} // namespace luduvo
