#pragma once

#include "Platform/luduvo/context/LuduvoContextStore.hpp"
#include "Platform/luduvo/context/LuduvoProcessIdentity.hpp"

namespace luduvo
{
using StudioHandles = std::map<std::string, std::vector<std::string>>;

std::optional<StudioHandles> captureStudioHandles(const ContextProject& project, const ProcessIdentity& studio);
struct StudioCapture
{
    WorldDumpSnapshot snapshot;
    bool active = false;
};

std::optional<StudioCapture> loadStudioCapture(const ContextStore& store, const ContextProject& project);
void publishStudioHandles(const ContextStore& store, const ContextProject& project, const ProcessIdentity& studio, const StudioHandles& handles);
void clearStudioHandles(const ContextStore& store, const std::string& projectId);
}
