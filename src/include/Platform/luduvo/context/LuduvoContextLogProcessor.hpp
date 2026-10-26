#pragma once

#include "Platform/luduvo/context/LuduvoContextStore.hpp"

#include <map>
#include <string>

namespace luduvo
{

class ContextLogProcessor
{
    struct PendingCommand
    {
        std::string commandLine;
        std::string projectName;
    };

    ContextStore store;
    std::string activeProject;
    std::map<uint64_t, PendingCommand> pendingCommands;

    void completeCommand(const PendingCommand& pending);

public:
    explicit ContextLogProcessor(ContextStore store);

    void ingestStudioLine(const std::string& line);
    void ingestCommandLine(const std::string& line);
    const std::string& activeProjectName() const;
    size_t pendingCommandCount() const;
};

} // namespace luduvo
