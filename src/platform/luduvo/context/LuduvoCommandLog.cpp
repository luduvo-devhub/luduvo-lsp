#include "Platform/luduvo/context/LuduvoCommandLog.hpp"
#include "LSP/AtomicFile.hpp"
#include "LSP/Logs.hpp"
#include <algorithm>
#include <map>
#include <string_view>

namespace luduvo
{
namespace
{
std::string_view complete(std::string_view contents)
{
    const auto newline = contents.rfind('\n');
    return contents.substr(0, newline == std::string_view::npos ? 0 : newline + 1);
}
std::vector<std::string> lines(std::string_view contents)
{
    std::vector<std::string> result;
    for (size_t start = 0; start < contents.size();)
    {
        const auto end = contents.find('\n', start);
        if (end == std::string_view::npos)
            break;
        auto line = contents.substr(start, end - start);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        result.emplace_back(line);
        start = end + 1;
    }
    return result;
}
} // namespace
CommandLogReader::CommandLogReader(std::string path, bool startAtEnd)
    : path(std::move(path))
{
    if (startAtEnd)
        previous = readSharedFile(this->path).value_or("");
}
CommandLogChanges CommandLogReader::readLines()
{
    CommandLogChanges result;
    std::string error;
    auto contents = readSharedFile(path, &error);
    if (!contents)
    {
        if (!unavailable)
            logging::write(logging::Level::Warning, "helper.commands", "Cannot read command log " + path + ": " + error);
        unavailable = true;
        return result;
    }
    if (unavailable)
        logging::write(logging::Level::Info, "helper.commands", "Command log is readable again: " + path);
    unavailable = false;
    if (*contents == previous)
        return result;
    const auto old = complete(previous);
    const auto next = complete(*contents);
    size_t prefix = 0;
    if (next.size() >= old.size() && next.substr(0, old.size()) == old)
        prefix = old.size();
    else
        while (prefix < std::min(old.size(), next.size()) && old[prefix] == next[prefix])
            ++prefix;
    result.rewritten = prefix < old.size();
    if (prefix && next[prefix - 1] != '\n')
    {
        const auto newline = next.rfind('\n', prefix - 1);
        prefix = newline == std::string_view::npos ? 0 : newline + 1;
    }
    size_t suffix = 0;
    while (suffix < std::min(old.size(), next.size()) - prefix && old[old.size() - suffix - 1] == next[next.size() - suffix - 1])
        ++suffix;
    if (suffix && next.size() > suffix && next[next.size() - suffix - 1] != '\n')
        suffix = next.size() - next.find('\n', next.size() - suffix) - 1;
    std::map<std::string, size_t> unchanged;
    for (const auto& line : lines(old.substr(prefix, old.size() - prefix - suffix)))
        ++unchanged[line];
    for (auto& line : lines(next.substr(prefix, next.size() - prefix - suffix)))
    {
        auto found = unchanged.find(line);
        if (found != unchanged.end() && found->second)
            --found->second;
        else
            result.lines.push_back(std::move(line));
    }
    if (result.rewritten)
        logging::write(logging::Level::Warning, "helper.commands", "Command log contents were rewritten: " + path);
    previous = std::move(*contents);
    return result;
}
} // namespace luduvo
