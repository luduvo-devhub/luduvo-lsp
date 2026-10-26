#include "Platform/luduvo/context/LuduvoWorldDump.hpp"

#include "LuauFileUtils.hpp"

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <set>

namespace luduvo
{

static std::optional<uint64_t> parseUnsigned(std::string_view value)
{
    uint64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        return std::nullopt;
    return result;
}

static bool startsWith(std::string_view value, std::string_view prefix)
{
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

static std::optional<double> parseNumber(std::string_view value)
{
    std::string text(value);
    char* end = nullptr;
    const double result = std::strtod(text.c_str(), &end);
    if (!end || end != text.c_str() + text.size())
        return std::nullopt;
    return result;
}

static std::vector<std::string_view> splitWords(std::string_view value)
{
    std::vector<std::string_view> result;
    size_t position = 0;
    while (position < value.size())
    {
        while (position < value.size() && value[position] == ' ')
            ++position;
        const size_t begin = position;
        while (position < value.size() && value[position] != ' ')
            ++position;
        if (begin != position)
            result.push_back(value.substr(begin, position - begin));
    }
    return result;
}

static bool parseStatus(std::string_view line, WorldDumpSnapshot& snapshot)
{
    const auto words = splitWords(line);
    if (words.size() != 7 || words[0] != "status" || words[1] != "seq" || words[3] != "undo" || words[5] != "redo")
        return false;

    const auto sequence = parseUnsigned(words[2]);
    const auto undo = parseUnsigned(words[4]);
    const auto redo = parseUnsigned(words[6]);
    if (!sequence || !undo || !redo)
        return false;

    snapshot.commandSequence = *sequence;
    snapshot.undoCount = *undo;
    snapshot.redoCount = *redo;
    return true;
}

static std::optional<WorldInstance> parseInstance(std::string_view line)
{
    constexpr std::string_view rowPrefix = "row ";
    constexpr std::string_view nodeMarker = " nid ";
    constexpr std::string_view instanceMarker = " entity ";
    constexpr std::string_view parentMarker = " parent ";
    constexpr std::string_view nameMarker = " name ";
    constexpr std::string_view typeMarker = " type ";
    constexpr std::string_view shapeMarker = " shape ";
    constexpr std::string_view positionMarker = " pos ";
    constexpr std::string_view scriptMarker = " script ";

    if (!startsWith(line, rowPrefix))
        return std::nullopt;

    const size_t node = line.find(nodeMarker, rowPrefix.size());
    const size_t instance = line.find(instanceMarker, node == std::string_view::npos ? 0 : node + nodeMarker.size());
    const size_t parent = line.find(parentMarker, instance == std::string_view::npos ? 0 : instance + instanceMarker.size());
    const size_t name = line.find(nameMarker, parent == std::string_view::npos ? 0 : parent + parentMarker.size());
    const size_t type = line.find(typeMarker, name == std::string_view::npos ? 0 : name + nameMarker.size());
    const size_t shape = line.find(shapeMarker, type == std::string_view::npos ? 0 : type + typeMarker.size());
    const size_t position = line.find(positionMarker, shape == std::string_view::npos ? 0 : shape + shapeMarker.size());
    if (node == std::string_view::npos || instance == std::string_view::npos || parent == std::string_view::npos || name == std::string_view::npos ||
        type == std::string_view::npos || shape == std::string_view::npos || position == std::string_view::npos)
        return std::nullopt;

    const size_t firstScript = line.find(scriptMarker, position + positionMarker.size());
    const size_t end = line.size();
    const auto rowValue = parseUnsigned(line.substr(rowPrefix.size(), node - rowPrefix.size()));
    const auto nodeValue = parseUnsigned(line.substr(node + nodeMarker.size(), instance - node - nodeMarker.size()));
    const auto instanceValue = parseUnsigned(line.substr(instance + instanceMarker.size(), parent - instance - instanceMarker.size()));
    const auto parentValue = parseUnsigned(line.substr(parent + parentMarker.size(), name - parent - parentMarker.size()));
    const auto shapeValue = parseUnsigned(line.substr(shape + shapeMarker.size(), position - shape - shapeMarker.size()));
    if (!rowValue || !nodeValue || !instanceValue || !parentValue || !shapeValue)
        return std::nullopt;

    const std::string_view positionText =
        line.substr(position + positionMarker.size(), (firstScript == std::string_view::npos ? end : firstScript) - position - positionMarker.size());
    const auto positionWords = splitWords(positionText);
    if (positionWords.size() != 3)
        return std::nullopt;
    const auto x = parseNumber(positionWords[0]);
    const auto y = parseNumber(positionWords[1]);
    const auto z = parseNumber(positionWords[2]);
    if (!x || !y || !z || !std::isfinite(*x) || !std::isfinite(*y) || !std::isfinite(*z))
        return std::nullopt;

    WorldInstance result;
    result.row = *rowValue;
    result.nodeId = *nodeValue;
    result.instanceId = *instanceValue;
    result.parentNodeId = *parentValue;
    result.name = std::string(line.substr(name + nameMarker.size(), type - name - nameMarker.size()));
    result.type = std::string(line.substr(type + typeMarker.size(), shape - type - typeMarker.size()));
    result.shape = *shapeValue;
    result.position = {*x, *y, *z};

    size_t script = firstScript;
    while (script != std::string_view::npos)
    {
        const size_t valueStart = script + scriptMarker.size();
        const size_t next = line.find(scriptMarker, valueStart);
        std::string value(line.substr(valueStart, (next == std::string_view::npos ? end : next) - valueStart));
        if (value.empty())
            return std::nullopt;
        if (value != "-")
            result.scripts.push_back(std::move(value));
        script = next;
    }
    return result;
}

WorldDumpParseResult parseWorldDump(std::string_view source, std::string sourcePath)
{
    WorldDumpParseResult result;
    WorldDumpSnapshot snapshot;
    snapshot.sourcePath = std::move(sourcePath);
    snapshot.capturedAtUnixMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

    bool sawTick = false;
    bool sawStatus = false;
    bool sawInvalidRecord = false;
    size_t lineNumber = 0;
    size_t position = 0;
    while (position <= source.size())
    {
        const size_t newline = source.find('\n', position);
        std::string_view line = source.substr(position, (newline == std::string_view::npos ? source.size() : newline) - position);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        ++lineNumber;

        if (startsWith(line, "tick "))
        {
            if (const auto tick = parseUnsigned(line.substr(5)))
            {
                snapshot.tick = *tick;
                sawTick = true;
            }
            else
            {
                result.warnings.push_back("line " + std::to_string(lineNumber) + ": invalid tick");
                sawInvalidRecord = true;
            }
        }
        else if (startsWith(line, "status "))
        {
            sawStatus = parseStatus(line, snapshot);
            if (!sawStatus)
            {
                result.warnings.push_back("line " + std::to_string(lineNumber) + ": invalid status");
                sawInvalidRecord = true;
            }
        }
        else if (startsWith(line, "row "))
        {
            if (auto instance = parseInstance(line))
                snapshot.instances.push_back(std::move(*instance));
            else
            {
                result.warnings.push_back("line " + std::to_string(lineNumber) + ": invalid instance row");
                sawInvalidRecord = true;
            }
        }

        if (newline == std::string_view::npos)
            break;
        position = newline + 1;
    }

    if (!sawTick)
        result.error = "world dump is missing its tick header";
    else if (!sawStatus)
        result.error = "world dump is missing a valid status header";
    else if (snapshot.instances.empty())
        result.error = "world dump contains no readable instance rows";
    else if (sawInvalidRecord)
        result.error = "world dump contains malformed records";
    else
    {
        std::set<uint64_t> rows;
        std::set<uint64_t> nodeIds;
        std::set<uint64_t> instanceIds;
        for (const WorldInstance& instance : snapshot.instances)
        {
            if (!rows.insert(instance.row).second)
            {
                result.error = "world dump contains duplicate row " + std::to_string(instance.row);
                break;
            }
            if (!nodeIds.insert(instance.nodeId).second)
            {
                result.error = "world dump contains duplicate node id " + std::to_string(instance.nodeId);
                break;
            }
            if (!instanceIds.insert(instance.instanceId).second)
            {
                result.error = "world dump contains duplicate instance id " + std::to_string(instance.instanceId);
                break;
            }
        }
        if (!result.error)
        {
            for (const WorldInstance& instance : snapshot.instances)
            {
                if (instance.parentNodeId == instance.nodeId)
                {
                    result.error = "world dump node " + std::to_string(instance.nodeId) + " is its own parent";
                    break;
                }
                if (instance.parentNodeId != 0 && !nodeIds.count(instance.parentNodeId))
                {
                    result.error =
                        "world dump node " + std::to_string(instance.nodeId) + " refers to missing parent " + std::to_string(instance.parentNodeId);
                    break;
                }
            }
        }
        if (!result.error)
            result.snapshot = std::move(snapshot);
    }
    return result;
}

WorldDumpParseResult parseWorldDumpFile(const std::string& path)
{
    auto source = Luau::FileUtils::readFile(path);
    if (!source)
        return {std::nullopt, {}, "could not read world dump: " + path};
    return parseWorldDump(*source, path);
}

} // namespace luduvo
