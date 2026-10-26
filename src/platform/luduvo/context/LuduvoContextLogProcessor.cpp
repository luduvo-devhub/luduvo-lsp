#include "Platform/luduvo/context/LuduvoContextLogProcessor.hpp"

#include <charconv>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

namespace luduvo
{

static std::optional<std::string> openedProjectName(const std::string& line)
{
    constexpr std::string_view prefix = "Opened project '";
    const size_t begin = line.find(prefix);
    if (begin == std::string::npos)
        return std::nullopt;
    const size_t nameStart = begin + prefix.size();
    const size_t end = line.find("' with ", nameStart);
    if (end == std::string::npos)
        return std::nullopt;
    return line.substr(nameStart, end - nameStart);
}

static bool isWorldMutation(std::string_view command)
{
    return command.find("edit.") == 0 || command.find("insert.") == 0 || command.find("component.") == 0 || command.find("tag.") == 0 ||
           command == "file.import" || command == "file.new";
}

static std::pair<std::string, std::string> splitCommand(std::string_view line)
{
    const size_t separator = line.find(' ');
    if (separator == std::string_view::npos)
        return {std::string(line), {}};
    return {std::string(line.substr(0, separator)), std::string(line.substr(separator + 1))};
}

static WorldDumpParseResult parseCompletedWorldDump(const std::string& path)
{
    WorldDumpParseResult parsed;
    for (int attempt = 0; attempt < 6; ++attempt)
    {
        parsed = parseWorldDumpFile(path);
        if (parsed.snapshot)
            return parsed;
        if (attempt != 5)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return parsed;
}

static void reportStoreFailure(std::string_view operation, const ContextStoreResult& result)
{
    if (!result.success)
        std::cerr << "Luduvo context helper could not " << operation << ": " << result.error.value_or("unknown error") << '\n';
}

ContextLogProcessor::ContextLogProcessor(ContextStore store)
    : store(std::move(store))
{
}

void ContextLogProcessor::ingestStudioLine(const std::string& line)
{
    if (auto project = openedProjectName(line))
    {
        activeProject = *project;
        reportStoreFailure("set the active project", store.setActiveProject(activeProject));
    }
}

void ContextLogProcessor::completeCommand(const PendingCommand& pending)
{
    if (pending.projectName.empty())
        return;
    const auto [command, arguments] = splitCommand(pending.commandLine);
    if (command == "world.dump")
    {
        WorldDumpParseResult parsed = parseCompletedWorldDump(arguments);
        if (!parsed.snapshot)
        {
            std::cerr << "Luduvo context helper rejected " << arguments << ": " << parsed.error.value_or("invalid world dump") << '\n';
            return;
        }

        std::string projectName = pending.projectName;
        std::optional<std::string> projectDirectory;
        const std::filesystem::path dumpDirectory = std::filesystem::u8path(arguments).parent_path();
        if (std::filesystem::is_regular_file(dumpDirectory / "project.json"))
        {
            const ContextStoreResult files = store.updateProjectFiles(dumpDirectory.generic_u8string());
            std::string error;
            const auto catalog = store.load(&error);
            if (files.success && files.projectId && catalog && catalog->projects.count(*files.projectId))
            {
                projectName = catalog->projects.at(*files.projectId).name;
                projectDirectory = dumpDirectory.generic_u8string();
            }
        }
        else
        {
            std::string error;
            const auto catalog = store.load(&error);
            if (catalog)
            {
                const std::string normalizedDump = normalizeContextPath(arguments);
                for (const auto& [_, project] : catalog->projects)
                {
                    const std::string expected = normalizeContextPath(
                        (std::filesystem::u8path(store.directory()) / "projects" / project.id / "world.dump.txt").generic_u8string());
                    if (normalizedDump == expected)
                    {
                        projectName = project.name;
                        if (!project.directory.empty())
                            projectDirectory = project.directory;
                        break;
                    }
                }
            }
        }
        reportStoreFailure("publish a world dump", store.updateWorld(std::move(*parsed.snapshot), projectName, projectDirectory));
    }
    else if (command == "script.attach" && !arguments.empty())
        reportStoreFailure("attach a script", store.attachScript(pending.projectName, arguments));
    else if (command == "script.client" || command == "script.server")
        reportStoreFailure("mark a project stale",
            store.markProjectStale(pending.projectName, command + " created and attached a script after the last world dump"));
    else if (isWorldMutation(command))
        reportStoreFailure("mark a project stale", store.markProjectStale(pending.projectName, command + " changed the world after the last world dump"));
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
    const auto pending = pendingCommands.find(sequence);
    if (value == "ok")
    {
        if (pending != pendingCommands.end())
        {
            completeCommand(pending->second);
            pendingCommands.erase(pending);
        }
    }
    else if (pending != pendingCommands.end())
        pendingCommands.erase(pending);
    else
        pendingCommands[sequence] = {value, activeProject};
}

const std::string& ContextLogProcessor::activeProjectName() const
{
    return activeProject;
}

size_t ContextLogProcessor::pendingCommandCount() const
{
    return pendingCommands.size();
}

} // namespace luduvo
