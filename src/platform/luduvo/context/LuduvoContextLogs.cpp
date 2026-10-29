#include "Platform/luduvo/context/LuduvoContextLogs.hpp"
#include "Platform/luduvo/context/LuduvoProcessIdentity.hpp"
#include "LSP/AtomicFile.hpp"
#include <atomic>
#include <filesystem>
#include <iostream>

namespace luduvo
{
static std::string logPath(const std::string& state)
{
    return (std::filesystem::u8path(state) / "control" / "helper-events.json").generic_u8string();
}
ContextLogWriter::ContextLogWriter(const std::string& stateDirectory)
    : path(logPath(stateDirectory))
{
    const auto identity = currentProcessIdentity();
    static std::atomic<uint64_t> instances{0};
    session = std::to_string(identity ? identity->processId : 0) + ":" + std::to_string(identity ? identity->startToken : 0) + ":" +
              std::to_string(instances++);
}
void ContextLogWriter::append(const logging::Record& record)
{
    std::lock_guard guard(mutex);
    auto bounded = record;
    if (bounded.message.size() > 4096)
        bounded.message = bounded.message.substr(0, 4096) + " [truncated]";
    records.push_back({{"sequence", ++sequence}, {"record", bounded}});
    if (records.size() > 128)
        records.pop_front();
    std::string error;
    if (!writeFileAtomically(
            path, nlohmann::json{{"session", session}, {"records", records}}.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), &error))
        std::cerr << "Could not relay helper logs: " << error << '\n';
}
ContextLogReader::ContextLogReader(const std::string& stateDirectory)
    : path(logPath(stateDirectory))
{
}
void ContextLogReader::poll(logging::Logger& destination)
{
    // File timestamps can repeat across atomic replacements. Session and sequence
    // are the authority; the bounded buffer makes checking them inexpensive.
    const auto source = readSharedFile(path);
    if (!source)
        return;
    try
    {
        const auto buffer = nlohmann::json::parse(*source);
        const std::string currentSession = buffer.at("session").get<std::string>();
        if (currentSession != session)
        {
            session = currentSession;
            sequence = 0;
        }
        for (const auto& item : buffer.at("records"))
        {
            const auto next = item.at("sequence").get<uint64_t>();
            if (next <= sequence)
                continue;
            if (sequence && next > sequence + 1)
                destination.write(
                    logging::Level::Warning, "context", "Helper log relay overflowed; missed " + std::to_string(next - sequence - 1) + " events");
            destination.emit(item.at("record").get<logging::Record>());
            sequence = next;
        }
    }
    catch (const std::exception&)
    {
    } // Retry incomplete/replaced buffers next poll.
}
} // namespace luduvo
