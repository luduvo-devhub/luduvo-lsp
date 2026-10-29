#pragma once
#include "LSP/Logs.hpp"
#include <deque>
#include <mutex>

namespace luduvo
{
// A bounded IPC buffer, not a permanent log archive. One helper writes; every
// connected LSP reads independently and applies its own logging configuration.
class ContextLogWriter
{
    std::string path;
    std::string session;
    uint64_t sequence = 0;
    std::deque<nlohmann::json> records;
    std::mutex mutex;

public:
    explicit ContextLogWriter(const std::string& stateDirectory);
    void append(const logging::Record& record);
};
class ContextLogReader
{
    std::string path;
    std::string session;
    uint64_t sequence = 0;

public:
    explicit ContextLogReader(const std::string& stateDirectory);
    void poll(logging::Logger& destination = logging::logger());
};
} // namespace luduvo
