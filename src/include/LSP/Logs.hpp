#pragma once

#include "nlohmann/json.hpp"
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <chrono>

namespace logging
{
enum class Level
{
    Off,
    Error,
    Warning,
    Info,
    Debug,
    Verbose
};
std::string_view name(Level level);
Level parseLevel(std::string_view value);
bool knownCategory(std::string_view category);

struct Configuration
{
    Level level = Level::Info;
    std::map<std::string, Level> categories;
};
void to_json(nlohmann::json& json, const Configuration& configuration);
void from_json(const nlohmann::json& json, Configuration& configuration);

struct Record
{
    int64_t time = 0;
    uint64_t process = 0;
    Level level = Level::Info;
    std::string category;
    std::string message;
};
void to_json(nlohmann::json& json, const Record& record);
void from_json(const nlohmann::json& json, Record& record);
std::string format(const Record& record);
using Sink = std::function<void(const Record&)>;

// Filtering, formatting and transport are shared by the server and standalone CLI.
// Sinks receive records after filtering; callbacks run outside the settings lock.
class Logger
{
    mutable std::mutex mutex;
    Configuration configuration;
    Sink sink;

public:
    explicit Logger(Sink sink = {});
    Configuration settings() const;
    void configure(Configuration settings);
    Sink exchangeSink(Sink replacement);
    bool enabled(Level level, std::string_view category) const;
    void emit(const Record& record) const;
    void write(Level level, std::string_view category, std::string message) const;
};
Logger& logger();
void write(Level level, std::string_view category, std::string message);

class Operation
{
    std::string category;
    std::string description;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    int exceptions;

public:
    Operation(std::string category, std::string description);
    ~Operation();
};

class ScopedSink
{
    Logger& target;
    Sink previous;

public:
    explicit ScopedSink(Sink sink, Logger& target = logger());
    ~ScopedSink();
    ScopedSink(const ScopedSink&) = delete;
    ScopedSink& operator=(const ScopedSink&) = delete;
};
} // namespace logging
