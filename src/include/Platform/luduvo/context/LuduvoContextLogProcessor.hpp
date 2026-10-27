#pragma once

#include "Platform/luduvo/context/LuduvoContextStore.hpp"

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
    };

    ContextStore store;
    std::set<std::string> knownScriptFiles;
    std::optional<std::chrono::steady_clock::time_point> pendingScriptCreation;
    std::vector<PendingDump> pendingDumps;

    std::map<std::string, std::string> scanScriptFiles() const;
    void staleLiveProjects(std::string reason, const std::vector<std::string>& preferred = {});

public:
    explicit ContextLogProcessor(ContextStore store);

    void ingestCommandLine(const std::string& line);
    void pollFilesystem();
    size_t pendingCommandCount() const;
};

} // namespace luduvo
