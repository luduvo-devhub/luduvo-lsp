#pragma once

#include "nlohmann/json.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace luduvo
{

struct WorldPosition
{
    double x = 0;
    double y = 0;
    double z = 0;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(WorldPosition, x, y, z)

struct WorldInstance
{
    uint64_t row = 0;
    uint64_t nodeId = 0;
    uint64_t instanceId = 0;
    uint64_t parentNodeId = 0;
    std::string name;
    std::string type;
    uint64_t shape = 0;
    WorldPosition position;
    std::vector<std::string> scripts;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(WorldInstance, row, nodeId, instanceId, parentNodeId, name, type, shape, position, scripts)

struct WorldDumpSnapshot
{
    int formatVersion = 1;
    uint64_t tick = 0;
    uint64_t commandSequence = 0;
    uint64_t undoCount = 0;
    uint64_t redoCount = 0;
    int64_t capturedAtUnixMs = 0;
    std::string sourcePath;
    std::vector<WorldInstance> instances;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    WorldDumpSnapshot, formatVersion, tick, commandSequence, undoCount, redoCount, capturedAtUnixMs, sourcePath, instances)

struct WorldDumpParseResult
{
    std::optional<WorldDumpSnapshot> snapshot;
    std::vector<std::string> warnings;
    std::optional<std::string> error;
};

WorldDumpParseResult parseWorldDump(std::string_view source, std::string sourcePath = {});
WorldDumpParseResult parseWorldDumpFile(const std::string& path);

} // namespace luduvo
