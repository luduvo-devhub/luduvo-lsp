#pragma once

#include <atomic>
#include <chrono>
#include <optional>
#include <string>

#include "Platform/luduvo/context/LuduvoWorldDump.hpp"

namespace luduvo
{

enum class DumpMethod
{
    Auto,
    SavedScene,
    Studio
};

struct AutoDumpPlan
{
    std::string projectId;
    std::string projectDirectory;
    std::string outputPath;
    std::string editorPath;
    std::chrono::seconds timeout{45};
    DumpMethod method = DumpMethod::Auto;
};

struct AutoDumpResult
{
    bool success = false;
    std::string error;
    std::optional<WorldDumpSnapshot> snapshot;
};

std::optional<WorldDumpSource> selectWorldDumpSource(const std::string& projectDirectory, std::string* error = nullptr);
AutoDumpResult runAutomaticWorldDump(const AutoDumpPlan& plan, const std::atomic<bool>* cancelled = nullptr);
AutoDumpResult recoverAutomaticWorldDumpAutosave(const std::string& projectDirectory);
std::string sceneDumperPath(const std::string& editorPath);
std::string defaultLuduvoEditorPath(const std::string& localDirectory);

} // namespace luduvo
