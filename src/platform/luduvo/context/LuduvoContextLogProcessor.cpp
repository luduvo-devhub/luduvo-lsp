#include "Platform/luduvo/context/LuduvoContextLogProcessor.hpp"
#include "Platform/luduvo/context/LuduvoStudioContext.hpp"

#include "LSP/Logs.hpp"
#include "Platform/luduvo/context/LuduvoProcessIdentity.hpp"
#include "LSP/AtomicFile.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace luduvo
{

namespace
{

static bool isWorldMutation(std::string_view command)
{
    return command.find("edit.") == 0 || command.find("insert.") == 0 || command.find("component.") == 0 || command.find("tag.") == 0 ||
           command.find("attr.") == 0 || command == "file.import" || command == "file.new" || command == "file.save";
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

struct StudioLogLine
{
    std::string_view timestamp;
    uint64_t pid = 0;
    std::optional<uint64_t> sequence;
    std::string_view value;
};

static std::optional<uint64_t> parseNumber(std::string_view value)
{
    if (value.empty())
        return std::nullopt;
    uint64_t number = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        return std::nullopt;
    return number;
}

static std::optional<StudioLogLine> studioLogLine(std::string_view line)
{
    const auto timeEnd = line.find(' ');
    if (timeEnd == std::string_view::npos || timeEnd < 20 || line[10] != 'T' || line[timeEnd - 1] != 'Z')
        return std::nullopt;
    const auto pidEnd = line.find(' ', timeEnd + 1);
    if (pidEnd == std::string_view::npos)
        return std::nullopt;
    const auto pid = parseNumber(line.substr(timeEnd + 1, pidEnd - timeEnd - 1));
    if (!pid || !*pid)
        return std::nullopt;
    auto value = line.substr(pidEnd + 1);
    const auto sequenceEnd = value.find(' ');
    const auto sequence = parseNumber(value.substr(0, sequenceEnd));
    if (sequence)
    {
        if (sequenceEnd == std::string_view::npos)
            return std::nullopt;
        value.remove_prefix(sequenceEnd + 1);
    }
    return StudioLogLine{line.substr(0, timeEnd), *pid, sequence, value};
}

static bool opensProject(std::string_view command)
{
    return command == "file.open" || command == "file.new" || command == "file.create";
}

static bool closesProject(std::string_view command)
{
    return command == "file.close" || command == "file.quit";
}

static bool resetsSession(std::string_view value)
{
    return value == "start" || value == "close" || value == "exit" || value == "crash" || value.substr(0, 6) == "crash ";
}

static std::optional<std::string> openedDirectory(const std::string& arguments)
{
    auto path = std::filesystem::u8path(unquote(arguments));
    if (!path.is_absolute())
        return std::nullopt;
    if (path.filename() == "world.ldv" || path.filename() == "project.json")
        path = path.parent_path();
    return normalizeContextPath(path.generic_u8string());
}

static std::optional<uint64_t> logTimestamp(std::string_view timestamp)
{
    std::tm date{};
    std::istringstream stream{std::string(timestamp.substr(0, 19))};
    stream >> std::get_time(&date, "%Y-%m-%dT%H:%M:%S");
    if (stream.fail())
        return std::nullopt;
#ifdef _WIN32
    const auto seconds = _mkgmtime(&date);
#else
    const auto seconds = timegm(&date);
#endif
    if (seconds < 0)
        return std::nullopt;
    const auto milliseconds = timestamp.size() == 24 && timestamp[19] == '.' ? parseNumber(timestamp.substr(20, 3)) : std::nullopt;
    if (!milliseconds && timestamp.size() != 20)
        return std::nullopt;
    return uint64_t(seconds) * 1000 + milliseconds.value_or(0);
}

static std::optional<std::string> findOpenProject(std::string_view history, const ProcessIdentity& identity)
{
    const auto startedAt = processStartUnixMs(identity);
    if (!startedAt)
        return std::nullopt;

    // Only a completed transition from this process lifetime can establish a project.
    std::set<uint64_t> succeeded;
    const auto lastNewline = history.rfind('\n');
    if (lastNewline == std::string_view::npos)
        return std::nullopt;
    history = history.substr(0, lastNewline);
    while (!history.empty())
    {
        const auto separator = history.rfind('\n');
        auto line = history.substr(separator == std::string_view::npos ? 0 : separator + 1);
        history = history.substr(0, separator == std::string_view::npos ? 0 : separator);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        const auto record = studioLogLine(line);
        if (!record || record->pid != identity.processId)
            continue;
        const auto time = logTimestamp(record->timestamp);
        if (!time || *time < *startedAt || (!record->sequence && resetsSession(record->value)))
            return std::nullopt;
        if (!record->sequence)
            continue;
        const auto [command, arguments] = splitCommand(record->value);
        if (closesProject(command))
            return std::nullopt;
        if (opensProject(command))
            return succeeded.count(*record->sequence) ? openedDirectory(arguments) : std::nullopt;
        if (command == "ok")
            succeeded.insert(*record->sequence);
    }
    return std::nullopt;
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

std::vector<std::string> ContextLogProcessor::projectsInDirectory(const std::string& directory) const
{
    std::vector<std::string> projects;
    if (const auto catalog = store.load())
        for (const auto& [id, project] : catalog->projects)
            if (project.directory == directory)
                projects.push_back(id);
    return projects;
}

bool ContextLogProcessor::ingestCommandLine(const std::string& line, std::string_view previousContents)
{
    if (const auto record = studioLogLine(line))
    {
        if (!record->sequence)
        {
            if (resetsSession(record->value))
            {
                studioSessions.erase(record->pid);
                for (auto it = studioCommands.lower_bound({record->pid, 0}); it != studioCommands.end() && it->first.first == record->pid;)
                    it = studioCommands.erase(it);
            }
            return true;
        }

        const auto [command, _] = splitCommand(record->value);
        if (command.empty())
            return false;
        const auto key = std::make_pair(record->pid, *record->sequence);
        if (command != "ok" && command != "fail")
        {
            if (opensProject(command) || closesProject(command) || isWorldMutation(command) || command == "world.dump" ||
                command == "script.attach" || command == "script.client" || command == "script.server" ||
                (!studioSessions.count(record->pid) && command.find('.') != std::string::npos))
            {
                if (opensProject(command) || closesProject(command))
                {
                    const auto session = studioSessions.find(record->pid);
                    if (session != studioSessions.end() && session->second.projectDirectory)
                        for (const auto& projectId : projectsInDirectory(*session->second.projectDirectory))
                        {
                            clearStudioHandles(store, projectId);
                            liveCaptures.erase(projectId);
                        }
                    studioSessions.erase(record->pid);
                }
                if (studioCommands.size() >= 1024)
                    invalidateJournal();
                studioCommands[key] = {std::string(record->value), processIdentityForPid(record->pid)};
            }
            return true;
        }
        const auto pending = studioCommands.find(key);
        if (pending == studioCommands.end())
            return true;
        auto commandRecord = std::move(pending->second);
        studioCommands.erase(pending);
        const auto [completed, completedArguments] = splitCommand(commandRecord.value);
        const auto identity = processIdentityForPid(record->pid);
        const bool sameProcess = identity && commandRecord.identity && identity->startToken == commandRecord.identity->startToken;
        if (!sameProcess)
            studioSessions.erase(record->pid);

        if (opensProject(completed) || closesProject(completed))
        {
            const auto directory = sameProcess && opensProject(completed) && command == "ok" ? openedDirectory(completedArguments) : std::nullopt;
            if (sameProcess)
                studioSessions[record->pid] = {*identity, directory};
            if (command == "ok" && isWorldMutation(completed))
                applyCommand(commandRecord.value, directory);
            return true;
        }
        if (command == "ok")
        {
            std::optional<std::string> directory;
            if (sameProcess)
            {
                auto session = studioSessions.find(record->pid);
                if (session == studioSessions.end() || session->second.identity.startToken != identity->startToken)
                    session =
                        studioSessions.insert_or_assign(record->pid, StudioSession{*identity, findOpenProject(previousContents, *identity)}).first;
                directory = session->second.projectDirectory;
            }
            applyCommand(commandRecord.value, directory);
        }
        return true;
    }

    if (line.empty() || line.compare(0, 4, "seq=") == 0)
        return true;
    const size_t separator = line.find(' ');
    if (separator == std::string::npos)
        return false;
    if (!parseNumber(std::string_view(line).substr(0, separator)))
        return false;
    const std::string value = line.substr(separator + 1);
    if (value.empty())
        return false;
    if (value == "ok")
        return true;

    const auto [command, _] = splitCommand(value);
    if (command.empty() || std::any_of(command.begin(), command.end(),
                               [](unsigned char character)
                               {
                                   return !std::isalnum(character) && character != '.' && character != '_' && character != '-';
                               }))
        return false;
    applyCommand(value, std::nullopt, true);
    return true;
}

void ContextLogProcessor::applyCommand(std::string_view value, const std::optional<std::string>& projectDirectory, bool legacy)
{
    const auto [command, rawArguments] = splitCommand(value);
    const std::string arguments = unquote(rawArguments);
    logging::write(logging::Level::Verbose, "helper.commands", "Observed command " + command);
    const bool worldMutation = isWorldMutation(command);
    if (worldMutation || command == "script.server" || command == "script.client" || (command == "script.attach" && !arguments.empty()))
        pendingDumps.erase(std::remove_if(pendingDumps.begin(), pendingDumps.end(),
                               [&](const PendingDump& dump)
                               {
                                   return !projectDirectory || !dump.projectDirectory || dump.projectDirectory == projectDirectory;
                               }),
            pendingDumps.end());

    if (command == "world.dump" && !arguments.empty())
    {
        pendingDumps.push_back({normalizeContextPath(arguments), std::chrono::steady_clock::now(), projectDirectory, legacy});
        return;
    }
    if (projectDirectory && (worldMutation || command == "script.attach" || command == "script.client" || command == "script.server"))
    {
        const auto projects = projectsInDirectory(*projectDirectory);
        if (command == "script.attach" && projects.size() == 1 && !arguments.empty())
        {
            const auto script = std::filesystem::u8path(arguments);
            const auto fullPath = std::filesystem::u8path(*projectDirectory) / script;
            std::error_code error;
            if (isScriptFile(fullPath) && std::filesystem::is_regular_file(fullPath, error))
                reportStoreFailure("attach a script", store.attachScriptById(projects.front(), fullPath.generic_u8string()));
        }
        if (!projects.empty())
            staleLiveProjects(command + " changed the world after the last world dump", projects);
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
    if (worldMutation)
        staleLiveProjects(command + " changed or may have changed a live project after the last world dump");
}

void ContextLogProcessor::invalidateJournal()
{
    pendingDumps.clear();
    pendingScriptCreation.reset();
    studioSessions.clear();
    studioCommands.clear();
    for (const auto& [projectId, _] : liveCaptures)
        clearStudioHandles(store, projectId);
    liveCaptures.clear();
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
            if (pending->projectDirectory)
            {
                const auto attributed = projectsInDirectory(*pending->projectDirectory);
                if (matches.empty())
                    matches = attributed;
                else if (matches.size() != 1 || std::find(attributed.begin(), attributed.end(), matches.front()) == attributed.end())
                    matches.clear();
            }
            else if (pending->legacy && matches.empty() && live.size() == 1)
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
                    worldDumpScene(*published) == worldDumpScene(*parsed.snapshot);
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

void ContextLogProcessor::pollStudio()
{
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
    std::map<std::string, std::vector<ProcessIdentity>> sessions;
    for (auto session = studioSessions.begin(); session != studioSessions.end();)
    {
        if (!isProcessAlive(session->second.identity))
            session = studioSessions.erase(session);
        else
        {
            if (session->second.projectDirectory)
                sessions[*session->second.projectDirectory].push_back(session->second.identity);
            ++session;
        }
    }
    const auto catalog = store.load();
    if (!catalog)
        return;
    const auto active = store.liveProjectIds();
    std::map<std::string, ProcessIdentity> eligible;
    for (const auto& id : active)
    {
        const auto project = catalog->projects.find(id);
        if (project == catalog->projects.end())
            continue;
        const auto session = sessions.find(project->second.directory);
        if (session != sessions.end() && session->second.size() == 1)
            eligible.emplace(id, session->second.front());
    }
    for (auto capture = liveCaptures.begin(); capture != liveCaptures.end();)
    {
        const auto session = eligible.find(capture->first);
        if (session != eligible.end() && session->second.processId == capture->second.processId &&
            session->second.startToken == capture->second.startToken)
            ++capture;
        else
        {
            clearStudioHandles(store, capture->first);
            capture = liveCaptures.erase(capture);
        }
    }
    const auto now = std::chrono::steady_clock::now();
    if (eligible.empty() || now < nextStudioCapture)
        return;
    nextStudioCapture = now + std::chrono::seconds(2);
    auto next = eligible.upper_bound(lastStudioProject);
    if (next == eligible.end())
        next = eligible.begin();
    lastStudioProject = next->first;
    const auto& project = catalog->projects.at(next->first);
    if (const auto handles = captureStudioHandles(project, next->second))
    {
        publishStudioHandles(store, project, next->second, *handles);
        liveCaptures.insert_or_assign(project.id, next->second);
    }
    else if (liveCaptures.erase(project.id))
        clearStudioHandles(store, project.id);
#endif
}

size_t ContextLogProcessor::pendingCommandCount() const
{
    return pendingDumps.size() + (pendingScriptCreation ? 1 : 0);
}

} // namespace luduvo
