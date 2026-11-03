#include "LSP/Logs.hpp"
#include "LSP/HierarchicalSettings.hpp"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <exception>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <unistd.h>
#endif

namespace logging
{
static constexpr std::string_view categories[] = {"server", "server.lifecycle", "server.configuration", "server.communication", "workspace",
    "workspace.files", "workspace.definitions", "bindings", "bindings.resources", "bindings.colors", "analysis", "analysis.diagnostics", "requests",
    "requests.hover", "requests.completion", "requests.signatureHelp", "context", "context.tracking", "context.snapshots", "context.refresh",
    "helper", "helper.lifecycle", "helper.clients", "helper.commands", "helper.storage", "helper.cleanup", "helper.autodump",
    "helper.autodump.process", "helper.autodump.autosaves", "context.schemas", "context.exports", "bindings.schemas", "other"};

std::string_view name(Level level)
{
    static constexpr std::string_view names[] = {"Off", "Error", "Warning", "Info", "Debug", "Verbose"};
    return names[static_cast<size_t>(level)];
}

Level parseLevel(std::string_view value)
{
    std::string lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
        [](unsigned char c)
        {
            return char(std::tolower(c));
        });
    for (auto level : {Level::Off, Level::Error, Level::Warning, Level::Info, Level::Debug, Level::Verbose})
    {
        std::string candidate(name(level));
        std::transform(candidate.begin(), candidate.end(), candidate.begin(),
            [](unsigned char c)
            {
                return char(std::tolower(c));
            });
        if (candidate == lowered)
            return level;
    }
    throw std::invalid_argument("unknown log level '" + std::string(value) + "'; expected Off, Error, Warning, Info, Debug, or Verbose");
}

bool knownCategory(std::string_view category)
{
    return std::find(std::begin(categories), std::end(categories), category) != std::end(categories);
}

void to_json(nlohmann::json& json, const Configuration& configuration)
{
    json = {{"default", {{"logs", name(configuration.level)}, {"ui", configuration.ui}}}, {"categories", nlohmann::json::object()}};
    for (const auto& entry : configuration.categories)
        json["categories"][entry.first]["logs"] = name(entry.second);
    for (const auto& entry : configuration.uiCategories)
        json["categories"][entry.first]["ui"] = entry.second;
}

void from_json(const nlohmann::json& json, Configuration& configuration)
{
    Configuration parsed;
    const auto defaults = json.value("default", nlohmann::json::object());
    parsed.level = parseLevel(defaults.value("logs", std::string("Info")));
    parsed.ui = defaults.value("ui", false);
    if (json.contains("categories"))
    {
        const auto& overrides = json.at("categories");
        if (!overrides.is_object())
            throw std::invalid_argument("reporting.categories must be an object");
        for (const auto& entry : overrides.items())
        {
            if (!knownCategory(entry.key()))
                throw std::invalid_argument("unknown log category '" + entry.key() + "'");
            if (!entry.value().is_object())
                throw std::invalid_argument("reporting category overrides must contain logs and/or ui");
            if (entry.value().contains("logs"))
                parsed.categories[entry.key()] = parseLevel(entry.value().at("logs").get<std::string>());
            if (entry.value().contains("ui"))
                parsed.uiCategories[entry.key()] = entry.value().at("ui").get<bool>();
        }
    }
    configuration = std::move(parsed);
}

bool Configuration::uiEnabled(std::string_view category) const
{
    return settings::findOverride(uiCategories, category).value_or(ui);
}

void to_json(nlohmann::json& json, const Record& record)
{
    json = {{"time", record.time}, {"process", record.process}, {"level", name(record.level)}, {"category", record.category},
        {"message", record.message}};
}
void from_json(const nlohmann::json& json, Record& record)
{
    record = {json.at("time").get<int64_t>(), json.at("process").get<uint64_t>(), parseLevel(json.at("level").get<std::string>()),
        json.at("category").get<std::string>(), json.at("message").get<std::string>()};
}

std::string format(const Record& record)
{
    const auto seconds = std::time_t(record.time / 1000);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << record.time % 1000 << "Z ["
           << name(record.level) << "] [" << record.category << "] [PID " << record.process << "] " << record.message;
    return output.str();
}

static bool accepts(const Configuration& configuration, Level level, std::string_view category)
{
    const auto threshold = settings::findOverride(configuration.categories, category).value_or(configuration.level);
    return level != Level::Off && static_cast<int>(level) <= static_cast<int>(threshold);
}

Logger::Logger(Sink sink)
    : sink(std::move(sink))
{
}
Configuration Logger::settings() const
{
    std::lock_guard guard(mutex);
    return configuration;
}
void Logger::configure(Configuration settings)
{
    std::lock_guard guard(mutex);
    configuration = std::move(settings);
}
Sink Logger::exchangeSink(Sink replacement)
{
    std::lock_guard guard(mutex);
    return std::exchange(sink, std::move(replacement));
}
bool Logger::enabled(Level level, std::string_view category) const
{
    std::lock_guard guard(mutex);
    return accepts(configuration, level, category);
}
void Logger::emit(const Record& record) const
{
    Sink destination;
    {
        std::lock_guard guard(mutex);
        if (!accepts(configuration, record.level, record.category))
            return;
        destination = sink;
    }
    // troubleshooting must never make an otherwise successful request fail
    try
    {
        if (destination)
            destination(record);
    }
    catch (...)
    {
    }
}
void Logger::write(Level level, std::string_view category, std::string message) const
{
    if (!enabled(level, category))
        return;
    const auto time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
#ifdef _WIN32
    const uint64_t process = GetCurrentProcessId();
#else
    const auto process = uint64_t(getpid());
#endif
    emit({time, process, level, std::string(category), std::move(message)});
}
Logger& logger()
{
    static std::mutex outputMutex;
    static Logger instance(
        [](const Record& record)
        {
            std::lock_guard guard(outputMutex);
            std::cerr << format(record) << '\n';
        });
    return instance;
}
void write(Level level, std::string_view category, std::string message)
{
    logger().write(level, category, std::move(message));
}
Operation::Operation(std::string category, std::string description)
    : category(std::move(category))
    , description(std::move(description))
    , exceptions(std::uncaught_exceptions())
{
    write(Level::Debug, this->category, "Started " + this->description);
}
Operation::~Operation()
{
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    const bool failed = std::uncaught_exceptions() > exceptions;
    write(failed           ? Level::Warning
          : elapsed > 1000 ? Level::Warning
                           : Level::Debug,
        category, std::string(failed ? "Failed " : "Finished ") + description + " in " + std::to_string(elapsed) + " ms");
}
ScopedSink::ScopedSink(Sink sink, Logger& target)
    : target(target)
    , previous(target.exchangeSink(std::move(sink)))
{
}
ScopedSink::~ScopedSink()
{
    target.exchangeSink(std::move(previous));
}
} // namespace logging
