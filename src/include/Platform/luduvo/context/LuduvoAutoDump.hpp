#pragma once

#include <atomic>
#include <chrono>
#include <optional>
#include <string>

#include "Platform/luduvo/context/LuduvoWorldDump.hpp"

namespace luduvo
{

struct AutoDumpPlan
{
    std::string projectId;
    std::string projectDirectory;
    std::string outputPath;
    std::string editorPath;
    std::chrono::seconds timeout{45};
};

struct AutoDumpResult
{
    bool success = false;
    std::string error;
    std::optional<WorldDumpSnapshot> snapshot;
};

AutoDumpResult runAutomaticWorldDump(const AutoDumpPlan& plan, const std::atomic<bool>* cancelled = nullptr);
AutoDumpResult recoverAutomaticWorldDumpAutosave(const std::string& projectDirectory);

} // namespace luduvo
