#pragma once

#include "Platform/luduvo/context/LuduvoContextStore.hpp"
#include "Platform/luduvo/context/LuduvoProcessIdentity.hpp"

#include <chrono>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace luduvo
{

class ContextLogProcessor
{
    struct PendingDump
    {
        std::string path;
        std::chrono::steady_clock::time_point observedAt;
        std::optional<std::string> projectDirectory;
        bool legacy = false;
    };

    struct StudioSession
    {
        ProcessIdentity identity;
        std::optional<std::string> projectDirectory;
    };

    struct PendingCommand
    {
        std::string value;
        std::optional<ProcessIdentity> identity;
    };

    ContextStore store;
    std::map<uint64_t, StudioSession> studioSessions;
    std::map<std::pair<uint64_t, uint64_t>, PendingCommand> studioCommands;
    std::map<std::string, ProcessIdentity> liveCaptures;
    std::chrono::steady_clock::time_point nextStudioCapture{};
    std::string lastStudioProject;
    std::set<std::string> knownScriptFiles;
    std::optional<std::chrono::steady_clock::time_point> pendingScriptCreation;
    std::vector<PendingDump> pendingDumps;
    std::map<std::string, std::string> knownProjectFiles;
    std::map<std::string, std::pair<std::string, std::chrono::steady_clock::time_point>> recentScriptFiles;

    std::map<std::string, std::string> scanScriptFiles() const;
    void staleLiveProjects(std::string reason, const std::vector<std::string>& preferred = {});
    std::vector<std::string> projectsInDirectory(const std::string& directory) const;
    void applyCommand(std::string_view value, const std::optional<std::string>& projectDirectory, bool legacy = false);

public:
    explicit ContextLogProcessor(ContextStore store);

    bool ingestCommandLine(const std::string& line, std::string_view previousContents = {});
    void invalidateJournal();
    void pollFilesystem();
    void pollStudio();
    size_t pendingCommandCount() const;
};

} // namespace luduvo
