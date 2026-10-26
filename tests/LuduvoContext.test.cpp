#include "doctest.h"
#include "LSP/ClientConfiguration.hpp"
#include "LSP/Workspace.hpp"
#include "Platform/luduvo/context/LuduvoContextProvider.hpp"
#include "Platform/luduvo/context/LuduvoContextHelper.hpp"
#include "Platform/luduvo/context/LuduvoContextInspection.hpp"
#include "Platform/luduvo/context/LuduvoContextLogProcessor.hpp"
#include "Platform/luduvo/context/LuduvoProcessIdentity.hpp"
#include "Platform/luduvo/context/LuduvoContextReport.hpp"
#include "Platform/luduvo/context/LuduvoContextStore.hpp"
#include "Platform/luduvo/context/LuduvoWorldDump.hpp"
#include "Platform/luduvo/LuduvoPlatform.hpp"
#include "TempDir.h"
#include "TestClient.h"
#include "LuauFileUtils.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <limits>
#include <thread>

TEST_SUITE_BEGIN("LuduvoContext");

static constexpr std::string_view worldDump = R"(tick 394735
status seq 30 undo 2 redo 1
selection
hover 0
row 0 nid 298 entity 25769805710 parent 0 name Baseplate type 7d00 shape 0 pos 0 -0.5 0 script -
row 1 nid 299 entity 38654707543 parent 0 name Named Part type 27c80 shape 3 pos -2.46113 1 -11.2453 script scripts/Part.server.lua script scripts/Part.client.lua
watch nid 299 tid 25 bytes 06000000
)";

TEST_CASE("world dump parser reads headers instances and scripts")
{
    luduvo::WorldDumpParseResult result = luduvo::parseWorldDump(worldDump, "world.txt");
    REQUIRE(result.snapshot);
    CHECK_EQ(result.snapshot->tick, 394735);
    CHECK_EQ(result.snapshot->commandSequence, 30);
    CHECK_EQ(result.snapshot->undoCount, 2);
    CHECK_EQ(result.snapshot->redoCount, 1);
    CHECK_EQ(result.snapshot->sourcePath, "world.txt");
    REQUIRE_EQ(result.snapshot->instances.size(), 2);
    CHECK_EQ(result.snapshot->instances[0].name, "Baseplate");
    CHECK(result.snapshot->instances[0].scripts.empty());
    CHECK_EQ(result.snapshot->instances[1].name, "Named Part");
    CHECK_EQ(result.snapshot->instances[1].nodeId, 299);
    CHECK_EQ(result.snapshot->instances[1].instanceId, 38654707543);
    CHECK_EQ(result.snapshot->instances[1].shape, 3);
    CHECK_EQ(result.snapshot->instances[1].position.z, doctest::Approx(-11.2453));
    CHECK_EQ(result.snapshot->instances[1].scripts, std::vector<std::string>({"scripts/Part.server.lua", "scripts/Part.client.lua"}));
    CHECK(result.warnings.empty());
}

TEST_CASE("world dump parser rejects incomplete input")
{
    const luduvo::WorldDumpParseResult noTick = luduvo::parseWorldDump("status seq 1 undo 0 redo 0\n");
    CHECK_FALSE(noTick.snapshot);
    REQUIRE(noTick.error);
    CHECK(noTick.error->find("tick") != std::string::npos);

    const luduvo::WorldDumpParseResult badRow = luduvo::parseWorldDump("tick 1\nstatus seq 1 undo 0 redo 0\nrow broken\n");
    CHECK_FALSE(badRow.snapshot);
    REQUIRE(badRow.error);
    CHECK(badRow.error->find("instance") != std::string::npos);
    REQUIRE_EQ(badRow.warnings.size(), 1);

    const luduvo::WorldDumpParseResult partial = luduvo::parseWorldDump(
        "tick 1\nstatus seq 1 undo 0 redo 0\nrow 0 nid 1 entity 2 parent 0 name Good type 7d00 shape 0 pos 0 0 0 script -\nrow broken\n");
    CHECK_FALSE(partial.snapshot);
    REQUIRE(partial.error);
    CHECK(partial.error->find("malformed") != std::string::npos);

    const luduvo::WorldDumpParseResult duplicate =
        luduvo::parseWorldDump("tick 1\nstatus seq 1 undo 0 redo 0\nrow 0 nid 1 entity 2 parent 0 name First type 7d00 shape 0 pos 0 0 0 script -\n"
                               "row 1 nid 1 entity 3 parent 0 name Second type 7d00 shape 0 pos 0 0 0 script -\n");
    CHECK_FALSE(duplicate.snapshot);
    REQUIRE(duplicate.error);
    CHECK(duplicate.error->find("duplicate node") != std::string::npos);

    const luduvo::WorldDumpParseResult missingParent = luduvo::parseWorldDump(
        "tick 1\nstatus seq 1 undo 0 redo 0\nrow 0 nid 1 entity 2 parent 99 name Orphan type 7d00 shape 0 pos 0 0 0 script -\n");
    CHECK_FALSE(missingParent.snapshot);
    REQUIRE(missingParent.error);
    CHECK(missingParent.error->find("missing parent") != std::string::npos);
}

TEST_CASE("context inspection summarizes world dumps without updating state")
{
    TempDir temp("luduvo_context_inspect_dump");
    const std::string dumpPath = temp.write_child("world.txt", std::string(worldDump));
    const luduvo::ContextInspectionResult inspected = luduvo::inspectContextInput(dumpPath);
    REQUIRE_MESSAGE(inspected.report, inspected.error.value_or(""));
    CHECK_EQ(inspected.report->at("kind"), "worldDump");
    CHECK_EQ(inspected.report->at("summary").at("instanceCount"), 2);
    CHECK_EQ(inspected.report->at("summary").at("rootInstanceCount"), 2);
    CHECK_EQ(inspected.report->at("summary").at("scriptAttachmentCount"), 2);
    CHECK_EQ(inspected.report->at("summary").at("uniqueScriptCount"), 2);
    CHECK_EQ(inspected.report->at("summary").at("tick"), 394735);
    CHECK_EQ(inspected.report->at("scripts").size(), 2);
    CHECK_FALSE(std::filesystem::exists(std::filesystem::u8path(temp.path()) / "catalog.json"));
}

TEST_CASE("project inspection shares script discovery with context updates")
{
    TempDir temp("luduvo_context_inspect_project");
    const std::string projectFile = temp.write_child("project/project.json", R"({
        "name": "Inspected Project",
        "assets": {
            "1": {"type": "script", "path": "scripts/Main.server.luau"},
            "2": {"type": "script", "path": "scripts/Missing.client.luau"},
            "3": {"type": "model", "path": "prefabs/Part.ldv"}
        }
    })");
    const std::filesystem::path projectDirectory = std::filesystem::u8path(projectFile).parent_path();
    temp.touch_child("project/scripts/Main.server.luau");
    temp.touch_child("project/scripts/autorun/Startup.client.luau");

    const luduvo::ContextInspectionResult inspected = luduvo::inspectContextInput(projectFile);
    REQUIRE_MESSAGE(inspected.report, inspected.error.value_or(""));
    CHECK_EQ(inspected.report->at("kind"), "project");
    CHECK_EQ(inspected.report->at("name"), "Inspected Project");
    CHECK_EQ(inspected.report->at("summary").at("assetCount"), 3);
    CHECK_EQ(inspected.report->at("summary").at("scriptAssetCount"), 2);
    CHECK_EQ(inspected.report->at("summary").at("autorunScriptCount"), 1);
    CHECK_EQ(inspected.report->at("summary").at("scriptCount"), 3);
    CHECK_EQ(inspected.report->at("summary").at("missingScriptCount"), 1);
    CHECK_EQ(inspected.report->at("summary").at("assetTypes").at("script"), 2);
    CHECK_EQ(inspected.report->at("declaredScripts").size(), 2);
    CHECK_EQ(inspected.report->at("autorunScripts").size(), 1);

    luduvo::ContextStore store(temp.path() + "/state");
    const luduvo::ContextStoreResult updated = store.updateProjectFiles(projectDirectory.generic_u8string());
    REQUIRE(updated.success);
    REQUIRE(updated.projectId);
    const auto catalog = store.load();
    REQUIRE(catalog);
    CHECK_EQ(catalog->projects.at(*updated.projectId).scripts, inspected.report->at("scripts").get<std::vector<std::string>>());
}

TEST_CASE("configured project names retain autorun discovery without project json")
{
    TempDir temp("luduvo_context_named_project");
    const std::string autorunScript = temp.touch_child("project/scripts/autorun/Startup.client.luau");
    luduvo::ContextStore store(temp.path() + "/state");
    const luduvo::ContextStoreResult updated = store.updateProjectFiles(temp.path() + "/project", "Configured Project");
    REQUIRE(updated.success);
    REQUIRE(updated.projectId);
    const auto catalog = store.load();
    REQUIRE(catalog);
    const luduvo::ContextProject& project = catalog->projects.at(*updated.projectId);
    CHECK_EQ(project.name, "Configured Project");
    CHECK_EQ(project.scripts, std::vector<std::string>{luduvo::normalizeContextPath(autorunScript)});
}

TEST_CASE("context store indexes project dump assets and autorun scripts")
{
    TempDir temp("luduvo_context_store");
    const std::string projectDirectory = temp.write_child("project/project.json", R"({
        "name": "Example Project",
        "assets": {
            "1": {"type": "script", "path": "scripts/Part.server.lua"},
            "2": {"type": "model", "path": "prefabs/Part.ldv"}
        }
    })");
    const std::filesystem::path projectRoot = std::filesystem::u8path(projectDirectory).parent_path();
    temp.touch_child("project/scripts/Part.server.lua");
    temp.touch_child("project/scripts/Part.client.lua");
    temp.touch_child("project/scripts/autorun/Startup.client.luau");

    luduvo::ContextStore store(temp.path() + "/state");
    luduvo::ContextStoreResult files = store.updateProjectFiles(projectRoot.generic_u8string());
    REQUIRE(files.success);
    REQUIRE(files.projectId);

    luduvo::WorldDumpParseResult parsed = luduvo::parseWorldDump(worldDump, temp.path() + "/world.txt");
    REQUIRE(parsed.snapshot);
    luduvo::ContextStoreResult world = store.updateWorld(std::move(*parsed.snapshot), "Example Project", projectRoot.generic_u8string());
    REQUIRE(world.success);
    CHECK_EQ(world.projectId, files.projectId);

    std::string error;
    const std::optional<luduvo::ContextCatalog> catalog = store.load(&error);
    REQUIRE_MESSAGE(catalog, error);
    REQUIRE_EQ(catalog->projects.size(), 1);
    const luduvo::ContextProject& project = catalog->projects.at(*world.projectId);
    CHECK(project.fresh);
    CHECK_EQ(project.worldGeneration, catalog->generation);
    CHECK_EQ(project.name, "Example Project");
    CHECK_EQ(project.scripts.size(), 3);
    CHECK_EQ(catalog->scriptProjects.size(), 3);
    CHECK(std::filesystem::is_regular_file(std::filesystem::u8path(project.snapshotPath)));
    CHECK_EQ(project.instanceCount, 2);
    CHECK_EQ(project.scriptAttachmentCount, 2);
    REQUIRE(store.setActiveProject("Example Project").success);

    REQUIRE(store.updateProjectFiles(projectRoot.generic_u8string()).success);
    const auto rescannedCatalog = store.load(&error);
    REQUIRE(rescannedCatalog);
    CHECK_EQ(rescannedCatalog->projects.at(*world.projectId).scripts.size(), 3);
    CHECK(rescannedCatalog->projects.at(*world.projectId).worldGeneration < rescannedCatalog->generation);

    const std::string reducedDump = R"(tick 394736
status seq 31 undo 2 redo 1
row 0 nid 298 entity 25769805710 parent 0 name Baseplate type 7d00 shape 0 pos 0 -0.5 0 script -
row 1 nid 299 entity 38654707543 parent 0 name Named Part type 27c80 shape 3 pos -2.46113 1 -11.2453 script scripts/Part.server.lua
)";
    luduvo::WorldDumpParseResult reduced = luduvo::parseWorldDump(reducedDump, temp.path() + "/world-2.txt");
    REQUIRE(reduced.snapshot);
    REQUIRE(store.updateWorld(std::move(*reduced.snapshot), "Example Project", projectRoot.generic_u8string()).success);
    const auto reconciledCatalog = store.load(&error);
    REQUIRE(reconciledCatalog);
    CHECK_EQ(reconciledCatalog->projects.at(*world.projectId).scripts.size(), 2);
    CHECK_EQ(reconciledCatalog->projects.at(*world.projectId).worldGeneration, reconciledCatalog->generation);

    const std::string inspectedScript = luduvo::normalizeContextPath((projectRoot / "scripts" / "Part.server.lua").generic_u8string());
    const luduvo::ContextInspectionResult scriptInspection = luduvo::inspectContextInput(inspectedScript, temp.path() + "/state");
    REQUIRE_MESSAGE(scriptInspection.report, scriptInspection.error.value_or(""));
    CHECK_EQ(scriptInspection.report->at("kind"), "script");
    CHECK_EQ(scriptInspection.report->at("side"), "server");
    CHECK_EQ(scriptInspection.report->at("projectCount"), 1);
    REQUIRE_EQ(scriptInspection.report->at("projects").size(), 1);
    const json& inspectedProject = scriptInspection.report->at("projects")[0];
    CHECK(inspectedProject.at("active").get<bool>());
    CHECK_EQ(inspectedProject.at("attachments").size(), 1);
    CHECK_EQ(inspectedProject.at("attachments")[0].at("name"), "Named Part");
    CHECK(std::find(inspectedProject.at("sources").begin(), inspectedProject.at("sources").end(), "catalog") != inspectedProject.at("sources").end());
    CHECK(std::find(inspectedProject.at("sources").begin(), inspectedProject.at("sources").end(), "projectAsset") !=
          inspectedProject.at("sources").end());

    const luduvo::ContextInspectionResult projectInspection =
        luduvo::inspectContextInput(projectRoot.generic_u8string(), temp.path() + "/state");
    REQUIRE_MESSAGE(projectInspection.report, projectInspection.error.value_or(""));
    CHECK(projectInspection.report->contains("capturedContext"));
    CHECK_EQ(projectInspection.report->at("capturedContext").at("instanceCount"), 2);
    CHECK_EQ(projectInspection.report->at("attachments").size(), 1);
    CHECK(
        std::find(inspectedProject.at("sources").begin(), inspectedProject.at("sources").end(), "worldDump") != inspectedProject.at("sources").end());

    const luduvo::ContextStoreResult stale = store.markProjectStale("Example Project", "edit.delete changed the world");
    REQUIRE(stale.success);
    const auto staleCatalog = store.load(&error);
    REQUIRE(staleCatalog);
    CHECK_FALSE(staleCatalog->projects.at(*world.projectId).fresh);
    CHECK_EQ(staleCatalog->projects.at(*world.projectId).staleReason, "edit.delete changed the world");
}

TEST_CASE("project context updates identify world dumps and newly inferred scripts")
{
    luduvo::ContextCatalog previous;
    previous.generation = 4;
    previous.projects["example"] = luduvo::ContextProject{};
    previous.projects["example"].name = "Example";
    previous.projects["example"].attachedScripts = {"C:/Project/Old.server.luau"};

    luduvo::ContextCatalog current = previous;
    current.generation = 5;
    current.projects["example"].worldGeneration = 5;
    current.projects["example"].attachedScripts = {"C:/Project/New.client.luau", "C:/Project/Old.server.luau"};

    const luduvo::ContextUpdates updates = luduvo::findContextUpdates(previous, current);
    CHECK_EQ(updates.updatedWorlds, std::vector<std::string>{"Example"});
    REQUIRE_EQ(updates.inferredScripts.size(), 1);
    CHECK_EQ(updates.inferredScripts.front().projectName, "Example");
    CHECK_EQ(updates.inferredScripts.front().scripts, std::vector<std::string>{"C:/Project/New.client.luau"});
}

TEST_CASE("context helper registrations are owned and cleaned up by process identity")
{
    TempDir temp("luduvo_context_process_identity");
    const std::string clients = temp.path() + "/clients";
    const auto current = luduvo::currentProcessIdentity();
    REQUIRE(current);
    CHECK(luduvo::isProcessAlive(*current));
    REQUIRE(luduvo::writeProcessIdentity(clients + "/current.lease", *current));
    REQUIRE(luduvo::writeProcessIdentity(
        clients + "/dead.lease", luduvo::ProcessIdentity{std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint64_t>::max()}));

    CHECK_EQ(luduvo::removeDeadProcessRegistrations(clients), 1);
    CHECK(std::filesystem::is_regular_file(std::filesystem::u8path(clients + "/current.lease")));
    CHECK_FALSE(std::filesystem::exists(std::filesystem::u8path(clients + "/dead.lease")));

    luduvo::ContextStore store(temp.path() + "/state");
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::u8path(store.controlDirectory()) / "lock", error);
    REQUIRE_FALSE(error);
    REQUIRE(luduvo::writeProcessIdentity(store.ownerPath(), *current));
    CHECK(store.helperRunning());
    REQUIRE(luduvo::writeProcessIdentity(
        store.ownerPath(), luduvo::ProcessIdentity{std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint64_t>::max()}));
    CHECK_FALSE(store.helperRunning());
}

TEST_CASE("context log processor retains the project active when a command starts")
{
    TempDir temp("luduvo_context_log_project_switch");
    const std::string dumpPath = temp.write_child("world.txt", std::string(worldDump));
    luduvo::ContextStore store(temp.path() + "/state");
    luduvo::ContextLogProcessor processor(store);

    processor.ingestStudioLine("Opened project 'Project A' with 1 assets");
    processor.ingestCommandLine("12 world.dump " + dumpPath);
    processor.ingestStudioLine("Opened project 'Project B' with 1 assets");
    processor.ingestCommandLine("12 ok");

    const auto catalog = store.load();
    REQUIRE(catalog);
    CHECK_EQ(catalog->activeProjectId, "project-b");
    REQUIRE(catalog->projects.count("project-a"));
    CHECK(catalog->projects.at("project-a").fresh);
    CHECK_FALSE(catalog->projects.at("project-b").fresh);
    CHECK_EQ(processor.pendingCommandCount(), 0);
}

TEST_CASE("failed context commands do not mutate project freshness")
{
    TempDir temp("luduvo_context_log_failed_command");
    luduvo::ContextStore store(temp.path() + "/state");
    REQUIRE(store.setActiveProject("Example").success);
    luduvo::WorldDumpParseResult parsed = luduvo::parseWorldDump(worldDump, temp.path() + "/world.txt");
    REQUIRE(parsed.snapshot);
    REQUIRE(store.updateWorld(std::move(*parsed.snapshot), "Example").success);

    luduvo::ContextLogProcessor processor(store);
    processor.ingestStudioLine("Opened project 'Example' with 1 assets");
    processor.ingestCommandLine("13 edit.delete");
    processor.ingestCommandLine("13 failed");

    const auto catalog = store.load();
    REQUIRE(catalog);
    CHECK(catalog->projects.at("example").fresh);
    CHECK_EQ(processor.pendingCommandCount(), 0);
}

TEST_CASE("context catalog serializes concurrent writers")
{
    TempDir temp("luduvo_context_concurrent_writers");
    const std::string state = temp.path() + "/state";
    luduvo::ContextStore first(state);
    luduvo::ContextStore second(state);
    REQUIRE(first.setActiveProject("Concurrent").success);
    std::atomic<size_t> failures{0};
    auto writeScripts = [&](luduvo::ContextStore& store, std::string prefix)
    {
        for (size_t index = 0; index < 20; ++index)
            if (!store.attachScript("Concurrent", prefix + std::to_string(index) + ".server.luau").success)
                ++failures;
    };
    std::thread firstWriter(writeScripts, std::ref(first), "first-");
    std::thread secondWriter(writeScripts, std::ref(second), "second-");
    firstWriter.join();
    secondWriter.join();

    CHECK_EQ(failures.load(), 0);
    const auto catalog = first.load();
    REQUIRE(catalog);
    CHECK_EQ(catalog->projects.at("concurrent").attachedScripts.size(), 40);
    CHECK(catalog->generation >= 41);
}

TEST_CASE("new directory and context settings round trip independently of legacy settings")
{
    const nlohmann::json input = {
        {"platform", {{"luduvo", {{"dataDirectory", "legacy-client"},
                                     {"directories", {{"roaming", "roaming-root"}, {"local", "local-root"}, {"context", "context-root"}}},
                                     {"context", {{"enabled", true}}}}}}}};
    const ClientConfiguration configuration = input.get<ClientConfiguration>();
    CHECK_EQ(configuration.platform.luduvo.dataDirectory, "legacy-client");
    CHECK_EQ(configuration.platform.luduvo.directories.roaming, "roaming-root");
    CHECK_EQ(configuration.platform.luduvo.directories.local, "local-root");
    CHECK_EQ(configuration.platform.luduvo.directories.context, "context-root");
    CHECK(configuration.platform.luduvo.context.enabled);
    CHECK_EQ(luduvo::defaultContextStateDirectory("ignored-local", "context-root"), luduvo::normalizeContextPath("context-root"));
}

TEST_CASE("context CLI reads Luduvo directories from LSP settings")
{
    TempDir temp("luduvo_context_cli_settings");
    const std::string contextDirectory = temp.path() + "/configured-context";
    const std::string settings = temp.write_child("settings.json",
        json({{"luau-lsp.platform.luduvo.directories.context", contextDirectory}}).dump());
    const std::string output = temp.path() + "/status.json";

    luduvo::ContextCli cli;
    cli.parser().parse_args({"context", "status", "--settings", settings, "--format", "json", "--output", output});
    CHECK_EQ(cli.execute(), 0);
    const auto contents = Luau::FileUtils::readFile(output);
    REQUIRE(contents);
    const json status = json::parse(*contents);
    CHECK_EQ(status.at("stateDirectory"), luduvo::normalizeContextPath(contextDirectory));
}

TEST_CASE("context reports share composable tree formatting")
{
    const json inspection = {{"kind", "script"}, {"inputPath", "scripts/chat.client.luau"}, {"side", "client"},
        {"projects", {{{"name", "Sandbox"}, {"directory", "C:/Sandbox"}, {"fresh", false}, {"active", false}, {"selected", false},
                          {"attachments", {{{"name", "Debug Chat"}, {"type", "Frame"}, {"nodeId", 51}, {"instanceId", 7}}}}},
                         {{"name", "Adventure Game"}, {"directory", "C:/Adventure"}, {"fresh", true}, {"active", true}, {"selected", true},
                             {"attachments", {{{"name", "Chat Window"}, {"type", "Frame"}, {"nodeId", 298}, {"instanceId", 8}}}}}}}};
    const std::string output = luduvo::renderReportText(luduvo::buildContextInspectionReport(inspection));
    CHECK(output.find("Projects (2)") != std::string::npos);
    CHECK(output.find("Adventure Game [active in LSP] [active in Studio] [fresh]") != std::string::npos);
    CHECK(output.find("Attached instances (2)") != std::string::npos);
    CHECK(output.find("Adventure Game (1)") < output.find("Sandbox (1)"));
    CHECK(output.find("Chat Window [Frame] [node 298]") != std::string::npos);

    const json status = {{"running", true},
        {"stateDirectory", "C:/context"},
        {"catalog",
            {{"activeProjectId", "adventure"},
                {"projects",
                    {{"adventure",
                        {{"name", "Adventure Game"},
                            {"directory", "C:/Adventure"},
                            {"fresh", true},
                            {"declaredScripts", json::array({"C:/Adventure/scripts/chat.client.luau"})},
                            {"worldScripts", json::array({"C:/Adventure/scripts/world.server.luau"})},
                            {"attachedScripts", json::array({"C:/Adventure/scripts/inferred.server.luau"})}}}}}}},
        {"cleanup", {{"clients", json::array()}, {"projects", json::array()}, {"snapshots", json::array()}}}};
    const std::string statusOutput = luduvo::renderReportText(luduvo::buildContextStatusReport(status));
    CHECK(statusOutput.find("Adventure Game [active in Studio] [fresh]") != std::string::npos);
    CHECK(statusOutput.find("Declared scripts (1)") != std::string::npos);
    CHECK(statusOutput.find("World scripts (1)") != std::string::npos);
    CHECK(statusOutput.find("Inferred scripts (1)") != std::string::npos);
}

TEST_CASE("instance inspection searches project-local IDs and groups matches by project")
{
    TempDir temp("luduvo_context_instance_inspection");
    const std::string firstDirectory = temp.write_child("zeta/project.json", R"({"name":"Zeta","assets":{}})");
    const std::string secondDirectory = temp.write_child("alpha/project.json", R"({"name":"Alpha","assets":{}})");
    const std::string state = temp.path() + "/state";
    luduvo::ContextStore store(state);
    REQUIRE(store.updateProjectFiles(std::filesystem::u8path(firstDirectory).parent_path().generic_u8string()).success);
    REQUIRE(store.updateProjectFiles(std::filesystem::u8path(secondDirectory).parent_path().generic_u8string()).success);

    const std::string nestedDump = R"(tick 1
status seq 1 undo 0 redo 0
row 0 nid 298 entity 10 parent 0 name Root type Folder shape 0 pos 0 0 0 script -
row 1 nid 299 entity 11 parent 298 name Child type Frame shape 0 pos 1 2 3 script scripts/Child.client.luau
)";
    for (const auto& [name, directory] :
        std::vector<std::pair<std::string, std::string>>{{"Zeta", std::filesystem::u8path(firstDirectory).parent_path().generic_u8string()},
            {"Alpha", std::filesystem::u8path(secondDirectory).parent_path().generic_u8string()}})
    {
        luduvo::WorldDumpParseResult parsed = luduvo::parseWorldDump(nestedDump, temp.path() + "/" + name + ".txt");
        REQUIRE(parsed.snapshot);
        REQUIRE(store.updateWorld(std::move(*parsed.snapshot), name, directory).success);
    }

    luduvo::InstanceInspectionRequest request;
    request.nodeId = 298;
    request.sections = {"hierarchy"};
    const luduvo::ContextInspectionResult inspected = luduvo::inspectContextInstance(request, state);
    REQUIRE_MESSAGE(inspected.report, inspected.error.value_or(""));
    CHECK_EQ(inspected.report->at("matchCount"), 2);
    REQUIRE_EQ(inspected.report->at("projects").size(), 2);
    CHECK_EQ(inspected.report->at("projects")[0].at("name"), "Alpha");
    CHECK_EQ(inspected.report->at("projects")[1].at("name"), "Zeta");
    CHECK_EQ(inspected.report->at("projects")[0].at("instances")[0].at("children")[0].at("name"), "Child");
    CHECK_FALSE(inspected.report->at("projects")[0].at("instances")[0].contains("position"));
    CHECK_FALSE(inspected.report->at("projects")[0].at("instances")[0].contains("scripts"));
}

TEST_CASE("context cleanup is selective and forget is intentionally broader")
{
    TempDir temp("luduvo_context_cleanup");
    const std::string projectFile = temp.write_child("project/project.json", R"({"name":"Missing Later","assets":{}})");
    const std::string projectDirectory = std::filesystem::u8path(projectFile).parent_path().generic_u8string();
    const std::string state = temp.path() + "/state";
    luduvo::ContextStore store(state);
    const luduvo::ContextStoreResult project = store.updateProjectFiles(projectDirectory);
    REQUIRE(project.success);
    REQUIRE(project.projectId);
    temp.touch_child("state/projects/orphan/world.json");
    const std::string deadRegistration = temp.path() + "/state/clients/dead.lease";
    REQUIRE(luduvo::writeProcessIdentity(
        deadRegistration, luduvo::ProcessIdentity{std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint64_t>::max()}));
    std::error_code filesystemError;
    std::filesystem::remove_all(std::filesystem::u8path(projectDirectory), filesystemError);
    REQUIRE_FALSE(filesystemError);

    const luduvo::ContextCleanupSummary candidates = store.cleanupCandidates();
    REQUIRE_EQ(candidates.clients.size(), 1);
    REQUIRE_EQ(candidates.projects.size(), 1);
    REQUIRE_EQ(candidates.snapshots.size(), 1);
    CHECK_EQ(candidates.projects[0].id, *project.projectId);

    const luduvo::ContextStoreResult cleanedClient = store.clean("client", {candidates.clients[0].id}, false);
    REQUIRE(cleanedClient.success);
    CHECK_EQ(cleanedClient.affected, 1);
    CHECK_FALSE(std::filesystem::exists(std::filesystem::u8path(deadRegistration)));
    CHECK(store.load()->projects.count(*project.projectId));

    const luduvo::ContextStoreResult forgotProject = store.forget("project", {*project.projectId}, false);
    REQUIRE(forgotProject.success);
    CHECK_EQ(forgotProject.affected, 1);
    CHECK_FALSE(store.load()->projects.count(*project.projectId));
}

TEST_CASE("context mutations reject ambiguous project names before changing state")
{
    TempDir temp("luduvo_context_ambiguous_mutation");
    const std::string firstFile = temp.write_child("first/project.json", R"({"name":"Shared Name","assets":{}})");
    const std::string secondFile = temp.write_child("second/project.json", R"({"name":"Shared Name","assets":{}})");
    const std::string firstDirectory = std::filesystem::u8path(firstFile).parent_path().generic_u8string();
    const std::string secondDirectory = std::filesystem::u8path(secondFile).parent_path().generic_u8string();
    luduvo::ContextStore store(temp.path() + "/state");
    REQUIRE(store.updateProjectFiles(firstDirectory).success);
    REQUIRE(store.updateProjectFiles(secondDirectory).success);

    const luduvo::ContextStoreResult forgot = store.forget("project", {"Shared Name"}, false);
    CHECK_FALSE(forgot.success);
    REQUIRE(forgot.error);
    CHECK(forgot.error->find("ambiguous") != std::string::npos);
    REQUIRE(store.load());
    CHECK_EQ(store.load()->projects.size(), 2);

    std::error_code filesystemError;
    std::filesystem::remove_all(std::filesystem::u8path(firstDirectory), filesystemError);
    REQUIRE_FALSE(filesystemError);
    std::filesystem::remove_all(std::filesystem::u8path(secondDirectory), filesystemError);
    REQUIRE_FALSE(filesystemError);
    const luduvo::ContextStoreResult cleaned = store.clean("project", {"Shared Name"}, false);
    CHECK_FALSE(cleaned.success);
    REQUIRE(cleaned.error);
    CHECK(cleaned.error->find("ambiguous") != std::string::npos);
    REQUIRE(store.load());
    CHECK_EQ(store.load()->projects.size(), 2);
}

TEST_CASE("context provider resolves and selects ambiguous script projects")
{
    TempDir temp("luduvo_context_provider");
    const std::string firstProjectFile = temp.write_child("first/project.json", R"({
        "name": "First Project",
        "assets": {"1": {"type": "script", "path": "../shared/Main.server.luau"}}
    })");
    const std::string secondProjectFile = temp.write_child("second/project.json", R"({
        "name": "Second Project",
        "assets": {"1": {"type": "script", "path": "../shared/Main.server.luau"}}
    })");
    const std::string scriptPath = temp.touch_child("shared/Main.server.luau");
    const std::string firstDirectory = std::filesystem::u8path(firstProjectFile).parent_path().generic_u8string();
    const std::string secondDirectory = std::filesystem::u8path(secondProjectFile).parent_path().generic_u8string();

    luduvo::ContextStore store(temp.path() + "/state");
    const luduvo::ContextStoreResult first = store.updateProjectFiles(firstDirectory);
    const luduvo::ContextStoreResult second = store.updateProjectFiles(secondDirectory);
    REQUIRE(first.success);
    REQUIRE(first.projectId);
    REQUIRE(second.success);
    REQUIRE(second.projectId);

    luduvo::ContextProvider provider(temp.path() + "/state", firstDirectory);
    CHECK_EQ(provider.findForScript(scriptPath).size(), 2);
    REQUIRE(provider.select(scriptPath, *second.projectId).success);
    const std::vector<luduvo::ProjectContextCandidate> selected = provider.findForScript(scriptPath);
    REQUIRE_EQ(selected.size(), 1);
    CHECK_EQ(selected[0].project.id, *second.projectId);
    REQUIRE(provider.reset(scriptPath).success);
    CHECK_EQ(provider.findForScript(scriptPath).size(), 2);

    luduvo::ContextProvider otherWorkspace(temp.path() + "/state", secondDirectory);
    REQUIRE(provider.select(scriptPath, *first.projectId).success);
    REQUIRE(otherWorkspace.select(scriptPath, *second.projectId).success);
    REQUIRE_EQ(provider.findForScript(scriptPath).size(), 1);
    REQUIRE_EQ(otherWorkspace.findForScript(scriptPath).size(), 1);
    CHECK_EQ(provider.findForScript(scriptPath)[0].project.id, *first.projectId);
    CHECK_EQ(otherWorkspace.findForScript(scriptPath)[0].project.id, *second.projectId);
    const json inspection = provider.inspect(scriptPath);
    CHECK_EQ(inspection.at("candidates").size(), 2);
}

TEST_CASE("luduvo platform context commands use the workspace context provider")
{
    TempDir temp("luduvo_platform_context_commands");
    const std::string projectFile = temp.write_child("project/project.json", R"({
        "name": "Command Project",
        "assets": {"1": {"type": "script", "path": "scripts/Main.server.luau"}}
    })");
    const std::filesystem::path projectRoot = std::filesystem::u8path(projectFile).parent_path();
    const std::string scriptPath = temp.touch_child("project/scripts/Main.server.luau");

    TestClient client;
    WorkspaceFolder workspace(&client, "Command Project", Uri::file(projectRoot.generic_u8string()), std::nullopt);
    ClientLuduvoConfiguration configuration;
    configuration.context.enabled = true;
    configuration.directories.local = temp.path() + "/local";
    LuduvoPlatform platform(configuration, &workspace.fileResolver, &workspace);
    ClientConfiguration clientConfiguration;
    clientConfiguration.platform.luduvo = configuration;
    platform.setupWithConfiguration(clientConfiguration);
    luduvo::ContextStore store(luduvo::defaultContextStateDirectory(configuration.directories.local));
    REQUIRE(store.setActiveProject("Command Project").success);

    const auto added = platform.handleWorkspaceCommand(
        "luduvo.addProjectContext", {json{{"directory", projectRoot.generic_u8string()}}});
    REQUIRE(added);
    CHECK(added->at("success").get<bool>());
    CHECK_EQ(added->at("projectId"), "command-project");

    const json argument = {{"uri", Uri::file(scriptPath).toString()}};
    const auto inspected = platform.handleWorkspaceCommand("luduvo.inspectProjectContext", {argument});
    REQUIRE(inspected);
    REQUIRE(inspected->contains("candidates"));
    REQUIRE_EQ(inspected->at("candidates").size(), 1);
    CHECK_EQ(inspected->at("candidates")[0]["name"], "Command Project");

    const auto refresh = platform.handleWorkspaceCommand("luduvo.refreshProjectContext", {argument});
    REQUIRE(refresh);
    CHECK(refresh->at("success").get<bool>());
    CHECK(refresh->at("command").get<std::string>().find("world.dump ") == 0);
    CHECK(refresh->at("outputPath").get<std::string>().find("world.dump.txt") != std::string::npos);
}

TEST_CASE("luduvo platform offers refresh guidance and context code actions")
{
    TempDir temp("luduvo_platform_context_guidance");
    const std::string projectFile = temp.write_child("project/project.json", R"({
        "name": "Guidance Project",
        "assets": {"1": {"type": "script", "path": "scripts/Main.server.luau"}}
    })");
    const std::filesystem::path projectRoot = std::filesystem::u8path(projectFile).parent_path();
    const std::string scriptPath = temp.touch_child("project/scripts/Main.server.luau");
    const lsp::DocumentUri scriptUri = Uri::file(scriptPath);

    TestClient client;
    WorkspaceFolder workspace(&client, "Guidance Project", Uri::file(projectRoot.generic_u8string()), std::nullopt);
    ClientLuduvoConfiguration configuration;
    configuration.context.enabled = true;
    configuration.directories.local = temp.path() + "/local";
    LuduvoPlatform platform(configuration, &workspace.fileResolver, &workspace);
    ClientConfiguration clientConfiguration;
    clientConfiguration.platform.luduvo = configuration;
    platform.setupWithConfiguration(clientConfiguration);
    luduvo::ContextStore store(luduvo::defaultContextStateDirectory(configuration.directories.local));
    REQUIRE(store.setActiveProject("Guidance Project").success);

    platform.onDidOpenTextDocument(scriptUri);
    REQUIRE_EQ(client.requestQueue.size(), 1);
    CHECK_EQ(client.requestQueue[0].first, "window/showMessageRequest");
    REQUIRE(client.requestQueue[0].second);
    CHECK(client.requestQueue[0].second->at("message").get<std::string>().find("world.dump ") != std::string::npos);

    workspace.fileResolver.managedFiles.emplace(scriptUri, TextDocument(scriptUri, "luau", 1, "self"));
    lsp::CodeActionParams params;
    params.textDocument.uri = scriptUri;
    params.range = {{0, 0}, {0, 4}};
    params.context.only = {lsp::CodeActionKind::QuickFix};
    params.context.diagnostics.push_back(
        lsp::Diagnostic{params.range, std::nullopt, std::nullopt, std::nullopt, std::nullopt, "Unknown global 'self'", {}, {}});
    std::vector<lsp::CodeAction> actions;
    platform.handleCodeAction(params, actions);
    REQUIRE_EQ(actions.size(), 1);
    REQUIRE(actions[0].command);
    CHECK_EQ(actions[0].command->command, "luduvo.resolveProjectContext");
}

TEST_SUITE_END();
