#include "doctest.h"
#include "LSP/Logs.hpp"
#include "Analyze/CliConfigurationParser.hpp"
#include "LSP/AtomicFile.hpp"
#include "Platform/luduvo/context/LuduvoContextLogs.hpp"
#include "TempDir.h"
#include "TestClient.h"
#include "Platform/luduvo/context/LuduvoContextHelper.hpp"
#include "Platform/luduvo/context/LuduvoContextStore.hpp"
#include "Platform/luduvo/context/LuduvoAutoDump.hpp"
#include <thread>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

TEST_SUITE_BEGIN("Logs");

TEST_CASE("dotted categories inherit the most specific configured ancestor")
{
    std::vector<logging::Record> records;
    logging::Logger logs(
        [&](const auto& record)
        {
            records.push_back(record);
        });
    const logging::Configuration configuration = nlohmann::json::parse(R"({
        "default":{"logs":"iNfO"}, "categories":{"helper":{"logs":"Debug"}, "helper.autodump":{"logs":"Off"},
        "helper.autodump.process":{"logs":"Verbose"}, "requests.hover":{"logs":"Debug"}}})");
    logs.configure(configuration);
    logs.write(logging::Level::Debug, "helper.clients", "registered");
    logs.write(logging::Level::Error, "helper.autodump.autosaves", "hidden");
    logs.write(logging::Level::Verbose, "helper.autodump.process", "exit");
    logs.write(logging::Level::Debug, "requests.hover", "resolved");
    logs.write(logging::Level::Debug, "requests.completion", "hidden");
    logs.write(logging::Level::Debug, "helpers", "hidden");
    REQUIRE_EQ(records.size(), 3);
    CHECK_EQ(records[0].message, "registered");
    CHECK_EQ(records[1].message, "exit");
    CHECK_EQ(records[2].message, "resolved");
    logs.configure({logging::Level::Off, {}});
    logs.write(logging::Level::Error, "other", "hidden");
    CHECK_EQ(records.size(), 3);
    CHECK_EQ(nlohmann::json(configuration).at("default").at("logs"), "Info");
}

TEST_CASE("nested and dotted server configurations preserve dotted category names")
{
    const ClientConfiguration nested =
        nlohmann::json::parse(R"({"reporting":{"default":{"logs":"Warning"},"categories":{"helper.autodump":{"logs":"Verbose"}}}})");
    const auto dotted = dottedToClientConfiguration(
        R"({"luau-lsp.reporting.default":{"logs":"Warning"},"luau-lsp.reporting.categories":{"helper.autodump":{"logs":"Verbose"}}})");
    CHECK_EQ(nested.reporting.level, logging::Level::Warning);
    CHECK_EQ(dotted.reporting.level, nested.reporting.level);
    CHECK(dotted.reporting.categories == nested.reporting.categories);
    CHECK_EQ(dotted.reporting.categories.at("helper.autodump"), logging::Level::Verbose);
}

TEST_CASE("invalid logging settings are rejected instead of silently ignored")
{
    CHECK_THROWS_WITH_AS(logging::parseLevel("trace"), doctest::Contains("unknown log level"), std::invalid_argument);
    CHECK_THROWS_AS(nlohmann::json::parse(R"({"categories":{"helper.autodumps":"Debug"}})").get<logging::Configuration>(), std::invalid_argument);
    CHECK_THROWS_AS(nlohmann::json::parse(R"({"categories":[]})").get<logging::Configuration>(), std::invalid_argument);
}

TEST_CASE("operational levels use standard LSP messages independently of trace settings")
{
    TestClient client;
    const auto previous = logging::logger().settings();
    logging::logger().configure({logging::Level::Verbose, {}});
    client.traceMode = lsp::TraceValue::Off;
    client.log(logging::Level::Verbose, "helper.autodump.process", "editor exited");
    logging::logger().configure({logging::Level::Off, {}});
    client.log(logging::Level::Error, "other", "hidden");
    logging::logger().configure(previous);
    REQUIRE_EQ(client.notificationQueue.size(), 1);
    CHECK_EQ(client.notificationQueue[0].first, "window/logMessage");
    const auto& message = *client.notificationQueue[0].second;
    CHECK_EQ(message.at("type"), int(lsp::MessageType::Log));
    CHECK(message.at("message").get<std::string>().find("[Verbose] [helper.autodump.process]") != std::string::npos);
}

TEST_CASE("an invalid log category warns without blocking workspace configuration")
{
    TestClient client;
    bool applied = false;
    client.configChangedCallback = [&](const auto&, const ClientConfiguration& configuration, const auto*)
    {
        applied = configuration.diagnostics.workspace;
    };
    client.requestConfiguration({Uri()});
    REQUIRE_EQ(client.requestHandlers.size(), 1);
    REQUIRE(client.requestHandlers[0]);
    JsonRpcMessage response;
    response.result = nlohmann::json::array({nlohmann::json::parse(R"({
        "reporting":{"categories":{"helper.typo":{"logs":"Debug"}}}, "diagnostics":{"workspace":true}
    })")});
    const auto previous = logging::logger().settings();
    (*client.requestHandlers[0])(response);
    logging::logger().configure(previous);
    CHECK(applied);
    CHECK_EQ(client.getConfiguration(Uri()).reporting.level, logging::Level::Info);
    const auto warning = std::find_if(client.notificationQueue.begin(), client.notificationQueue.end(),
        [](const auto& item)
        {
            return item.first == "window/logMessage" &&
                   item.second->at("message").template get<std::string>().find("helper.typo") != std::string::npos;
        });
    CHECK(warning != client.notificationQueue.end());
}

TEST_CASE("helper relay is bounded and every reader filters independently without replay")
{
    TempDir temp("helper_log_relay");
    luduvo::ContextLogWriter writer(temp.path());
    for (size_t i = 0; i < 130; ++i)
        writer.append({int64_t(i), 123, logging::Level::Debug, "helper.clients", std::to_string(i)});
    std::vector<logging::Record> records;
    logging::Logger verbose(
        [&](const auto& record)
        {
            records.push_back(record);
        });
    verbose.configure({logging::Level::Verbose, {}});
    logging::Logger quiet(
        [&](const auto&)
        {
            FAIL("Info reader must not emit Debug records");
        });
    luduvo::ContextLogReader first(temp.path()), second(temp.path());
    first.poll(verbose);
    second.poll(quiet);
    REQUIRE_EQ(records.size(), 128);
    CHECK_EQ(records.front().message, "2");
    CHECK_EQ(records.back().message, "129");
    first.poll(verbose);
    CHECK_EQ(records.size(), 128);
    const auto path = std::filesystem::u8path(temp.path()) / "control" / "helper-events.json";
    const auto previousTime = std::filesystem::last_write_time(path);
    luduvo::ContextLogWriter restarted(temp.path());
    restarted.append({500, 321, logging::Level::Info, "helper.lifecycle", "restarted"});
    std::filesystem::last_write_time(path, previousTime);
    first.poll(verbose);
    REQUIRE_EQ(records.size(), 129);
    CHECK_EQ(records.back().message, "restarted");
    CHECK_EQ(records.back().process, 321);
}

TEST_CASE("atomic state replacement preserves valid contents")
{
    TempDir temp("atomic_state");
    const auto path = temp.write_child("client.lease", "old");
    REQUIRE(writeFileAtomically(path, "new"));
    CHECK_EQ(readSharedFile(path), std::optional<std::string>("new"));
}

TEST_CASE("context CLI honors log settings and invocation overrides")
{
    TempDir temp("cli_log_settings");
    const std::string settings = temp.write_child("settings.json", R"({"luau-lsp.reporting.default":{"logs":"Off"}})");
    std::vector<logging::Record> records;
    logging::ScopedSink capture(
        [&](const logging::Record& record)
        {
            records.push_back(record);
        });
    luduvo::ContextCli quiet;
    quiet.parser().parse_args(
        {"context", "status", "--state-directory", temp.path() + "/state", "--settings", settings, "--output", temp.path() + "/quiet.json"});
    REQUIRE_EQ(quiet.execute(), 0);
    CHECK(records.empty());
    luduvo::ContextCli debug;
    debug.parser().parse_args({"context", "status", "--state-directory", temp.path() + "/state", "--settings", settings, "--log-level", "verbose",
        "--output", temp.path() + "/debug.json"});
    REQUIRE_EQ(debug.execute(), 0);
    REQUIRE_FALSE(records.empty());
    CHECK_EQ(records.front().category, "helper.lifecycle");
    CHECK_EQ(records.front().message, "Executing context status");
}

TEST_CASE("an unreadable existing catalog is never treated as an empty database")
{
    TempDir temp("unreadable_catalog");
    luduvo::ContextStore store(temp.path());
    std::filesystem::create_directories(std::filesystem::u8path(store.catalogPath()));
    std::string error;
    CHECK_FALSE(store.load(&error));
    CHECK(error.find("could not read") != std::string::npos);
    CHECK(std::filesystem::is_directory(std::filesystem::u8path(store.catalogPath())));
}

#ifdef _WIN32
TEST_CASE("inaccessible command logs are reported once instead of failing silently")
{
    TempDir temp("command_log_permissions");
    const auto commands = temp.touch_child("commands.log");
    HANDLE lock = CreateFileW(std::filesystem::u8path(commands).c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(lock != INVALID_HANDLE_VALUE);
    std::vector<logging::Record> records;
    logging::ScopedSink capture(
        [&](const logging::Record& record)
        {
            records.push_back(record);
        });
    luduvo::ContextCli cli;
    cli.parser().parse_args(
        {"context", "run", "--state-directory", temp.path() + "/state", "--commands-log", commands, "--idle-timeout", "0", "--poll-interval", "50"});
    const int result = cli.execute();
    CloseHandle(lock);
    CHECK_EQ(result, 0);
    CHECK_EQ(std::count_if(records.begin(), records.end(),
                 [](const auto& record)
                 {
                     return record.level == logging::Level::Warning && record.category == "helper.commands";
                 }),
        1);
}

TEST_CASE("failed editor launches report exit status and captured stderr")
{
    TempDir temp("editor_exit_logs");
    const auto autosave = temp.write_child("world.ldv.autosave", "preserve this autosave");
    temp.write_child("world.ldv", "saved world");
    wchar_t systemDirectory[MAX_PATH];
    REQUIRE(GetSystemDirectoryW(systemDirectory, MAX_PATH) > 0);
    const auto editor = (std::filesystem::path(systemDirectory) / "where.exe").generic_u8string();
    REQUIRE(std::filesystem::exists(std::filesystem::u8path(editor)));
    std::vector<logging::Record> records;
    logging::ScopedSink capture(
        [&](const logging::Record& record)
        {
            records.push_back(record);
        });
    const auto previous = logging::logger().settings();
    logging::logger().configure({logging::Level::Verbose, {}});
    const auto result = luduvo::runAutomaticWorldDump({"failure-test", temp.path(), temp.path() + "/dump.txt", editor, std::chrono::seconds(2)});
    logging::logger().configure(previous);
    CHECK_FALSE(result.success);
    CHECK(result.error.find("exit code") != std::string::npos);
    CHECK_EQ(readSharedFile(autosave), std::optional<std::string>("preserve this autosave"));
    const auto output = std::find_if(records.begin(), records.end(),
        [](const auto& record)
        {
            return record.category == "helper.autodump.process" && record.message.find("Editor output tail") == 0;
        });
    REQUIRE(output != records.end());
    const auto newline = output->message.find('\n');
    REQUIRE(newline != std::string::npos);
    CHECK(output->message.size() > newline + 1);
}

TEST_CASE("atomic Windows state writes recover from a transient reader sharing conflict")
{
    TempDir temp("atomic_sharing");
    const auto path = temp.write_child("client.lease", "old");
    const auto wide = std::filesystem::u8path(path);
    HANDLE reader =
        CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(reader != INVALID_HANDLE_VALUE);
    std::thread release(
        [reader]
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            CloseHandle(reader);
        });
    std::string error;
    const bool success = writeFileAtomically(path, "new", &error);
    release.join();
    CHECK_MESSAGE(success, error);
    CHECK_EQ(readSharedFile(path), std::optional<std::string>("new"));

    reader = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(reader != INVALID_HANDLE_VALUE);
    const bool denied = writeFileAtomically(path, "must not replace", &error);
    CloseHandle(reader);
    CHECK_FALSE(denied);
    CHECK_EQ(readSharedFile(path), std::optional<std::string>("new"));
    CHECK(error.find("could not replace") != std::string::npos);
    size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::u8path(temp.path())))
        if (entry.is_regular_file())
            ++files;
    CHECK_EQ(files, 1);
}
#endif
TEST_SUITE_END();
