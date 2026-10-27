#include "doctest.h"
#include "LSP/ClientConfiguration.hpp"
#include "LSP/Workspace.hpp"
#include "Platform/luduvo/context/LuduvoAutoDump.hpp"
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
#include <map>
#include <regex>
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

static std::map<std::string, std::string> preservedAutosaves(const std::string& directory)
{
    const std::regex timestampedName(R"(world\.ldv\.autosave\.luduvo-lsp-hostage-\d{8}-\d{6}-\d{3}Z)");
    std::map<std::string, std::string> result;
    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::u8path(directory)))
        if (std::regex_match(entry.path().filename().generic_u8string(), timestampedName))
            result[entry.path().generic_u8string()] = Luau::FileUtils::readFile(entry.path().generic_u8string()).value_or("");
    return result;
}

TEST_CASE("automatic world dumps restore autosaves when the editor cannot start")
{
    TempDir temp("luduvo_autodump_autosave_restore");
    const std::string project = temp.path() + "/project";
    const std::string autosave = temp.write_child("project/world.ldv.autosave", "user autosave");
    const std::string invalidEditor = temp.write_child("LuduvoEditor.invalid", "not an executable");

    const luduvo::AutoDumpResult result =
        luduvo::runAutomaticWorldDump({"project", project, temp.path() + "/world.dump.txt", invalidEditor, std::chrono::seconds(1)});
    CHECK_FALSE(result.success);
    CHECK_EQ(Luau::FileUtils::readFile(autosave).value_or(""), "user autosave");
    CHECK_FALSE(std::filesystem::exists(std::filesystem::u8path(project) / "world.ldv.autosave.luduvo-lsp-hostage"));
}

TEST_CASE("automatic world dumps preserve conflicting autosaves")
{
    TempDir temp("luduvo_autodump_autosave_conflict");
    const std::string project = temp.path() + "/project";
    const std::string autosave = temp.write_child("project/world.ldv.autosave", "current autosave");
    const std::string held = temp.write_child("project/world.ldv.autosave.luduvo-lsp-hostage", "held autosave");
    const std::string invalidEditor = temp.write_child("LuduvoEditor.invalid", "not an executable");

    const luduvo::AutoDumpResult result =
        luduvo::runAutomaticWorldDump({"project", project, temp.path() + "/world.dump.txt", invalidEditor, std::chrono::seconds(1)});
    CHECK_FALSE(result.success);
    CHECK(result.error.find("both") == std::string::npos);
    CHECK_EQ(Luau::FileUtils::readFile(autosave).value_or(""), "current autosave");
    CHECK_FALSE(std::filesystem::exists(held));
    const auto firstPreserved = preservedAutosaves(project);
    REQUIRE_EQ(firstPreserved.size(), 1);
    CHECK_EQ(firstPreserved.begin()->second, "held autosave");

    temp.write_child("project/world.ldv.autosave.luduvo-lsp-hostage", "another held autosave");
    const auto repeated =
        luduvo::runAutomaticWorldDump({"project", project, temp.path() + "/world.dump.txt", invalidEditor, std::chrono::seconds(1)});
    CHECK_FALSE(repeated.success);
    CHECK(repeated.error.find("both") == std::string::npos);
    CHECK_EQ(Luau::FileUtils::readFile(autosave).value_or(""), "current autosave");
    const auto repeatedPreserved = preservedAutosaves(project);
    REQUIRE_EQ(repeatedPreserved.size(), 2);
    CHECK_EQ(repeatedPreserved.at(firstPreserved.begin()->first), "held autosave");
    CHECK_EQ(std::count_if(repeatedPreserved.begin(), repeatedPreserved.end(),
                 [](const auto& entry)
                 {
                     return entry.second == "another held autosave";
                 }),
        1);
    CHECK_FALSE(std::filesystem::exists(held));
}

TEST_CASE("automatic world dump recovery preserves a conflicting held autosave under an unused name")
{
    TempDir temp("luduvo_autodump_conflict_recovery");
    const std::string project = temp.path() + "/project";
    const std::string autosave = temp.write_child("project/world.ldv.autosave", "new autosave");
    const std::string held = temp.write_child("project/world.ldv.autosave.luduvo-lsp-hostage", "old autosave");
    temp.write_child("project/world.ldv.autosave.luduvo-lsp-hostage-1", "previous conflict");

    REQUIRE(luduvo::recoverAutomaticWorldDumpAutosave(project).success);
    CHECK_EQ(Luau::FileUtils::readFile(autosave).value_or(""), "new autosave");
    CHECK_EQ(Luau::FileUtils::readFile(held + "-1").value_or(""), "previous conflict");
    const auto preserved = preservedAutosaves(project);
    REQUIRE_EQ(preserved.size(), 1);
    CHECK_EQ(preserved.begin()->second, "old autosave");
    CHECK_FALSE(std::filesystem::exists(held));
    REQUIRE(luduvo::recoverAutomaticWorldDumpAutosave(project).success);
    CHECK_EQ(Luau::FileUtils::readFile(autosave).value_or(""), "new autosave");

    std::filesystem::remove(autosave);
    REQUIRE(luduvo::recoverAutomaticWorldDumpAutosave(project).success);
    CHECK_FALSE(std::filesystem::exists(autosave));
    CHECK_EQ(preservedAutosaves(project), preserved);
}

TEST_CASE("automatic world dump startup recovery restores a held autosave")
{
    TempDir temp("luduvo_autodump_autosave_recovery");
    const std::string project = temp.path() + "/project";
    const std::string held = temp.write_child("project/world.ldv.autosave.luduvo-lsp-hostage", "held autosave");

    const luduvo::AutoDumpResult result = luduvo::recoverAutomaticWorldDumpAutosave(project);
    CHECK(result.success);
    CHECK_FALSE(std::filesystem::exists(held));
    CHECK_EQ(Luau::FileUtils::readFile(project + "/world.ldv.autosave").value_or(""), "held autosave");
}

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

TEST_CASE("dump project inspection distinguishes unavailable fields from known empty values")
{
    TempDir temp("luduvo_dump_project_fields");
    const std::string state = temp.path() + "/state";
    const std::string dumpPath = temp.write_child("world.txt", std::string(worldDump));
    luduvo::ContextStore store(state);
    auto parsed = luduvo::parseWorldDump(worldDump, dumpPath);
    REQUIRE(parsed.snapshot);
    REQUIRE(store.updateWorld(std::move(*parsed.snapshot), "Unlocated").success);

    auto inspected = luduvo::inspectContextInput(dumpPath, state);
    REQUIRE(inspected.report);
    REQUIRE_EQ(inspected.report->at("projects").size(), 1);
    const json& project = inspected.report->at("projects").at(0);
    CHECK(project.at("inputPath").is_null());
    CHECK(project.at("directory").is_null());
    CHECK(project.at("projectFile").is_null());
    for (const auto& field : project.at("summary").items())
        CHECK(field.value().is_null());
    CHECK(project.at("missingScripts").is_null());
    CHECK_EQ(project.at("attachments").size(), 1);
    CHECK_EQ(project.at("capturedContext").at("scriptAttachmentCount"), 2);
    const auto text = luduvo::renderReportText(luduvo::buildContextInspectionReport(*inspected.report));
    CHECK(text.find("Directory: unknown") != std::string::npos);
    CHECK(text.find("Assets: unknown") != std::string::npos);
    CHECK(text.find("Scripts: unknown") != std::string::npos);
    CHECK(text.find("Missing scripts: unknown") != std::string::npos);
    const auto sections = luduvo::selectReportSections(luduvo::buildContextInspectionReport(project), {"summary"});
    REQUIRE(sections);
    const json sectionJson = luduvo::reportToJson(*sections);
    CHECK(sectionJson.at("children").at(0).at("fields").at("Directory").is_null());
    CHECK(sectionJson.at("children").at(0).at("fields").at("Assets").is_null());

    temp.write_child("project/project.json", R"({"name":"Located","assets":{}})");
    parsed = luduvo::parseWorldDump(worldDump, dumpPath);
    REQUIRE(parsed.snapshot);
    REQUIRE(store.updateWorld(std::move(*parsed.snapshot), "Located", temp.path() + "/project").success);
    inspected = luduvo::inspectContextInput(dumpPath, state);
    REQUIRE(inspected.report);
    REQUIRE_EQ(inspected.report->at("projects").size(), 2);
    for (const json& associated : inspected.report->at("projects"))
    {
        if (associated.at("name") != "Located")
            continue;
        CHECK(associated.at("directory").is_string());
        CHECK(associated.at("projectFile").is_string());
        CHECK_EQ(associated.at("summary").at("assetCount"), 0);
        CHECK_EQ(associated.at("summary").at("missingScriptCount"), 0);
        CHECK(associated.at("missingScripts").is_array());
    }
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

TEST_CASE("stored projects can be inspected by name or ID")
{
    TempDir temp("luduvo_context_inspect_stored_project");
    const std::string state = temp.path() + "/state";
    luduvo::ContextStore store(state);
    REQUIRE(store.attachScript("Proving a point", temp.path() + "/scripts/Test.server.luau").success);
    temp.write_child("duplicate/project.json", R"({"name":"Proving a point","assets":{}})");
    REQUIRE(store.updateProjectFiles(temp.path() + "/duplicate").success);

    luduvo::ProjectInspectionRequest request;
    request.name = "Proving a point";
    const luduvo::ContextInspectionResult inspected = luduvo::inspectContextProject(request, state);
    REQUIRE_MESSAGE(inspected.report, inspected.error.value_or(""));
    CHECK_EQ(inspected.report->at("kind"), "projects");
    CHECK_EQ(inspected.report->at("matchCount"), 2);
    CHECK_EQ(inspected.report->at("projects").at(0).at("capturedContext").at("id"), "proving-a-point");
    CHECK(inspected.report->at("projects").at(0).at("summary").at("scriptCount").is_null());
    const std::string rendered = luduvo::renderReportText(luduvo::buildContextInspectionReport(*inspected.report));
    CHECK(rendered.find("Proving a point [stale]") != std::string::npos);

    const std::string output = temp.path() + "/project.json";
    luduvo::ContextCli cli;
    cli.parser().parse_args({"context", "inspect", "project", "Proving a point", "--state-directory", state, "--format", "json", "--output", output});
    REQUIRE_EQ(cli.execute(), 0);
    const auto contents = Luau::FileUtils::readFile(output);
    REQUIRE(contents);
    const json report = json::parse(*contents);
    CHECK_EQ(report.at("kind"), "projects");
    CHECK_EQ(report.at("projects").at(0).at("capturedContext").at("id"), "proving-a-point");

    luduvo::ProjectInspectionRequest idRequest;
    idRequest.id = "proving-a-point";
    const luduvo::ContextInspectionResult inspectedById = luduvo::inspectContextProject(idRequest, state);
    REQUIRE(inspectedById.report);
    CHECK_EQ(inspectedById.report->at("name"), "Proving a point");
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
    CHECK_EQ(inspectedProject.at("attachments").size(), 1);
    CHECK_EQ(inspectedProject.at("attachments")[0].at("name"), "Named Part");
    CHECK(std::find(inspectedProject.at("sources").begin(), inspectedProject.at("sources").end(), "catalog") != inspectedProject.at("sources").end());
    CHECK(std::find(inspectedProject.at("sources").begin(), inspectedProject.at("sources").end(), "projectAsset") !=
          inspectedProject.at("sources").end());

    const luduvo::ContextInspectionResult projectInspection = luduvo::inspectContextInput(projectRoot.generic_u8string(), temp.path() + "/state");
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

TEST_CASE("context updates announce each fresh to stale transition once")
{
    luduvo::ContextCatalog previous;
    auto& project = previous.projects["example"];
    project.name = "Example";
    project.directory = "project";
    project.fresh = true;
    luduvo::ContextCatalog current = previous;
    current.projects.at("example").fresh = false;
    const auto updates = luduvo::findContextUpdates(previous, current);
    CHECK_FALSE(updates.empty());
    REQUIRE_EQ(updates.staleProjects.size(), 1);
    CHECK_EQ(updates.staleProjects[0].id, "example");
    CHECK_EQ(updates.staleProjects[0].name, "Example");
    CHECK(luduvo::findContextUpdates(current, current).empty());
    CHECK(luduvo::findContextUpdates(current, previous).staleProjects.empty());
    CHECK_EQ(luduvo::findContextUpdates(previous, current).staleProjects.size(), 1);
}

TEST_CASE("project dump requests recheck freshness before offering another refresh")
{
    TempDir temp("luduvo_context_refresh_freshness");
    const std::string state = temp.path() + "/state";
    luduvo::ContextStore store(state);
    auto parsed = luduvo::parseWorldDump(worldDump, temp.path() + "/world.txt");
    REQUIRE(parsed.snapshot);
    const auto updated = store.updateWorld(std::move(*parsed.snapshot), "Example", temp.path());
    REQUIRE(updated.success);
    REQUIRE(updated.projectId);
    luduvo::ContextProvider provider(state, temp.path());
    CHECK_FALSE(provider.createProjectDumpRequest(*updated.projectId));
    REQUIRE(store.markProjectStale("Example", "world changed").success);
    const auto request = provider.createProjectDumpRequest(*updated.projectId);
    REQUIRE(request);
    CHECK_EQ(request->projectName, "Example");
    CHECK_EQ(request->projectDirectory, luduvo::normalizeContextPath(temp.path()));
    CHECK(request->outputPath.find(*updated.projectId) != std::string::npos);
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

TEST_CASE("context log processor publishes an unambiguous world dump")
{
    TempDir temp("luduvo_context_log_project_switch");
    const std::string dumpPath = temp.write_child("world.txt", std::string(worldDump));
    luduvo::ContextStore store(temp.path() + "/state");
    REQUIRE(store.attachScript("Project A", temp.path() + "/project-a/Main.server.luau").success);
    const auto identity = luduvo::currentProcessIdentity();
    REQUIRE(identity);
    REQUIRE(
        store.updateClientProjects(identity->processId, identity->startToken, temp.path(), {{"project-a", "Project A", temp.path() + "/project-a"}})
            .result.success);
    luduvo::ContextLogProcessor processor(store);

    processor.ingestCommandLine("12 world.dump " + dumpPath);
    processor.pollFilesystem();

    const auto catalog = store.load();
    REQUIRE(catalog);
    REQUIRE(catalog->projects.count("project-a"));
    CHECK(catalog->projects.at("project-a").fresh);
    CHECK_EQ(processor.pendingCommandCount(), 0);
}

TEST_CASE("world mutations stale context when submitted even without an acknowledgement")
{
    TempDir temp("luduvo_context_log_failed_command");
    luduvo::ContextStore store(temp.path() + "/state");
    REQUIRE(store.attachScript("Example", temp.path() + "/Example.server.luau").success);
    luduvo::WorldDumpParseResult parsed = luduvo::parseWorldDump(worldDump, temp.path() + "/world.txt");
    REQUIRE(parsed.snapshot);
    REQUIRE(store.updateWorld(std::move(*parsed.snapshot), "Example").success);
    const auto identity = luduvo::currentProcessIdentity();
    REQUIRE(identity);
    REQUIRE(store.updateClientProjects(identity->processId, identity->startToken, temp.path(), {{"example", "Example", ""}}).result.success);

    luduvo::ContextLogProcessor processor(store);
    processor.ingestCommandLine("13 edit.delete");
    processor.ingestCommandLine("13 failed");

    const auto catalog = store.load();
    REQUIRE(catalog);
    CHECK_FALSE(catalog->projects.at("example").fresh);
    CHECK_EQ(processor.pendingCommandCount(), 0);
}

TEST_CASE("context catalog serializes concurrent writers")
{
    TempDir temp("luduvo_context_concurrent_writers");
    const std::string state = temp.path() + "/state";
    luduvo::ContextStore first(state);
    luduvo::ContextStore second(state);
    REQUIRE(first.attachScript("Concurrent", "seed.server.luau").success);
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
    CHECK_EQ(catalog->projects.at("concurrent").attachedScripts.size(), 41);
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
    CHECK(configuration.platform.luduvo.context.autoDump);
    CHECK_FALSE(ClientLuduvoContextConfiguration{}.enabled);
    CHECK_FALSE(json({{"autoDump", false}}).get<ClientLuduvoContextConfiguration>().autoDump);
    CHECK_EQ(luduvo::defaultContextStateDirectory("ignored-local", "context-root"), luduvo::normalizeContextPath("context-root"));
}

TEST_CASE("context CLI reads Luduvo directories from LSP settings")
{
    TempDir temp("luduvo_context_cli_settings");
    const std::string contextDirectory = temp.path() + "/configured-context";
    const std::string settings = temp.write_child("settings.json",
        json({{"luau-lsp.platform.luduvo.directories.context", contextDirectory}, {"luau-lsp.platform.luduvo.context.lazyRefresh", false}}).dump());
    const std::string output = temp.path() + "/status.json";

    luduvo::ContextCli cli;
    cli.parser().parse_args({"context", "status", "--settings", settings, "--format", "json", "--output", output});
    CHECK_EQ(cli.execute(), 0);
    const auto contents = Luau::FileUtils::readFile(output);
    REQUIRE(contents);
    const json status = json::parse(*contents);
    CHECK_EQ(status.at("stateDirectory"), luduvo::normalizeContextPath(contextDirectory));
    CHECK_FALSE(status.at("configuration").at("lazyRefresh").get<bool>());
    CHECK(luduvo::renderReportText(luduvo::buildContextStatusReport(status)).find("Lazy refresh: disabled") != std::string::npos);
}

TEST_CASE("context status reports helper health while argument-free inspection reports stored projects")
{
    TempDir temp("luduvo_context_status_and_overview");
    const std::string state = temp.path() + "/state";
    luduvo::ContextStore store(state);
    REQUIRE(store.attachScript("Example", temp.path() + "/Example.server.luau").success);

    const auto identity = luduvo::currentProcessIdentity();
    REQUIRE(identity);
    std::error_code filesystemError;
    std::filesystem::create_directories(std::filesystem::u8path(store.controlDirectory()) / "lock", filesystemError);
    REQUIRE_FALSE(filesystemError);
    REQUIRE(luduvo::writeProcessIdentity(store.ownerPath(), *identity));
    REQUIRE(store.updateClientProjects(identity->processId, identity->startToken, temp.path(), {{"example", "Example", ""}}).result.success);

    const std::string statusPath = temp.path() + "/status.json";
    luduvo::ContextCli statusCli;
    statusCli.parser().parse_args({"context", "status", "--state-directory", state, "--format", "json", "--output", statusPath});
    REQUIRE_EQ(statusCli.execute(), 0);
    const auto statusSource = Luau::FileUtils::readFile(statusPath);
    REQUIRE(statusSource);
    const json status = json::parse(*statusSource);
    CHECK(status.at("running").get<bool>());
    CHECK_EQ(status.at("helperProcessId"), identity->processId);
    REQUIRE_EQ(status.at("clients").size(), 1);
    CHECK_EQ(status.at("clients")[0].at("processId"), identity->processId);
    CHECK_FALSE(status.contains("catalog"));

    const std::string overviewPath = temp.path() + "/overview.json";
    luduvo::ContextCli inspectCli;
    inspectCli.parser().parse_args({"context", "inspect", "--state-directory", state, "--format", "json", "--output", overviewPath});
    REQUIRE_EQ(inspectCli.execute(), 0);
    const auto overviewSource = Luau::FileUtils::readFile(overviewPath);
    REQUIRE(overviewSource);
    const json overview = json::parse(*overviewSource);
    CHECK_EQ(overview.at("kind"), "context");
    CHECK(overview.at("catalog").at("projects").contains("example"));
    CHECK(overview.at("cleanup").contains("clients"));
}

TEST_CASE("context reports share composable tree formatting")
{
    const json inspection = {{"kind", "script"}, {"inputPath", "scripts/chat.client.luau"}, {"side", "client"},
        {"projects", {{{"name", "Sandbox"}, {"directory", "C:/Sandbox"}, {"fresh", false}, {"active", false}, {"selected", false},
                          {"attachments", {{{"name", "Debug Chat"}, {"type", "Frame"}, {"nodeId", 51}, {"instanceId", 7}}}}},
                         {{"name", "Adventure Game"}, {"directory", "C:/Adventure"}, {"fresh", true}, {"active", true}, {"selected", true},
                             {"attachments", {{{"name", "Chat Window"}, {"type", "Frame"}, {"nodeId", 298}, {"instanceId", 8}}}}}}}};
    const luduvo::ReportNode report = luduvo::buildContextInspectionReport(inspection);
    const auto sections = luduvo::listReportSections(report);
    REQUIRE_FALSE(sections.empty());
    CHECK_EQ(sections.front().first, "summary");
    CHECK(std::none_of(sections.begin(), sections.end(),
        [](const auto& section)
        {
            return section.first == "script" || section.first.compare(0, 7, "script.") == 0;
        }));
    CHECK(std::none_of(sections.begin(), sections.end(),
        [](const auto& section)
        {
            return section.first == "summarySummary" || section.first == "summary-summary";
        }));

    const std::string output = luduvo::renderReportText(report);
    CHECK(output.find("Projects (2)") != std::string::npos);
    CHECK(output.find("Adventure Game [active in LSP] [fresh]") != std::string::npos);
    CHECK(output.find("Attached instances (2)") != std::string::npos);
    CHECK(output.find("Adventure Game (1)") < output.find("Sandbox (1)"));
    CHECK(output.find("Chat Window [Frame] [node 298]") != std::string::npos);

    const json overview = {{"kind", "context"}, {"stateDirectory", "C:/context"},
        {"catalog", {{"projects", {{"adventure", {{"name", "Adventure Game"}, {"directory", "C:/Adventure"}, {"fresh", true},
                                                     {"declaredScripts", json::array({"C:/Adventure/scripts/chat.client.luau"})},
                                                     {"worldScripts", json::array({"C:/Adventure/scripts/world.server.luau"})},
                                                     {"attachedScripts", json::array({"C:/Adventure/scripts/inferred.server.luau"})}}}}}}},
        {"cleanup", {{"clients", json::array()}, {"projects", json::array()}, {"snapshots", json::array()}}}};
    const std::string overviewOutput = luduvo::renderReportText(luduvo::buildContextInspectionReport(overview));
    CHECK(overviewOutput.find("Adventure Game [fresh]") != std::string::npos);
    CHECK(overviewOutput.find("Declared scripts (1)") != std::string::npos);
    CHECK(overviewOutput.find("World scripts (1)") != std::string::npos);
    CHECK(overviewOutput.find("Inferred scripts (1)") != std::string::npos);

    const json status = {{"running", true}, {"stateDirectory", "C:/context"}, {"catalogPath", "C:/context/catalog.json"}, {"helperProcessId", 42},
        {"catalogGeneration", 9}, {"projectCount", 1}, {"configuration", {{"enabled", true}, {"autoDump", true}}},
        {"clients", json::array({{{"processId", 101}, {"live", true}, {"projects", json::array()}},
                        {{"processId", 202}, {"live", false}, {"projects", json::array()}}})},
        {"deadClientCount", 1}, {"missingProjectCount", 2}, {"orphanedSnapshotCount", 3}};
    const std::string statusOutput = luduvo::renderReportText(luduvo::buildContextStatusReport(status));
    CHECK(statusOutput.find("Luduvo context helper [running]") != std::string::npos);
    CHECK(statusOutput.find("Helper process: PID 42") != std::string::npos);
    CHECK(statusOutput.find("LSP clients (2)") != std::string::npos);
    CHECK(statusOutput.find("Automatic dumps: enabled") != std::string::npos);
    CHECK(statusOutput.find("PID 101 [live]") != std::string::npos);
    CHECK(statusOutput.find("PID 202 [dead]") != std::string::npos);
    CHECK(statusOutput.find("Cleanup candidates (6)") != std::string::npos);
    CHECK(statusOutput.find("Projects (1)") == std::string::npos);

    const auto statusSections = luduvo::listReportSections(luduvo::buildContextStatusReport(status));
    CHECK(std::none_of(statusSections.begin(), statusSections.end(),
        [](const auto& section)
        {
            return section.first == "status" || section.first.compare(0, 7, "status.") == 0;
        }));
    CHECK(std::any_of(statusSections.begin(), statusSections.end(),
        [](const auto& section)
        {
            return section.first == "configuration";
        }));
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
    CHECK_FALSE(provider.createDumpRequest(scriptPath));
    const auto dump = provider.createProjectDumpRequest(*second.projectId);
    REQUIRE(dump);
    CHECK_EQ(dump->projectId, *second.projectId);
    CHECK_EQ(dump->projectDirectory, luduvo::normalizeContextPath(secondDirectory));
    CHECK_FALSE(provider.createProjectDumpRequest("missing"));
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
    const auto added = platform.handleWorkspaceCommand("luduvo.addProjectContext", {json{{"directory", projectRoot.generic_u8string()}}});
    REQUIRE(added);
    CHECK(added->at("success").get<bool>());
    CHECK_EQ(added->at("projectId"), "command-project");

    const auto beforeNotifications = store.load();
    REQUIRE(beforeNotifications);
    clientConfiguration.platform.luduvo.notifications["context.updated"] = true;
    platform.setupWithConfiguration(clientConfiguration);
    const auto afterNotifications = store.load();
    REQUIRE(afterNotifications);
    CHECK_EQ(afterNotifications->generation, beforeNotifications->generation);

    const auto overview = platform.handleWorkspaceCommand("luduvo.inspectProjectContext", {json::object()});
    REQUIRE(overview);
    CHECK_EQ(overview->at("kind"), "context");
    CHECK(overview->at("catalog").at("projects").contains("command-project"));

    const auto status = platform.handleWorkspaceCommand("luduvo.contextHelperStatus", {json::object()});
    REQUIRE(status);
    CHECK(status->contains("clients"));
    CHECK_FALSE(status->contains("catalog"));
    CHECK(status->at("configuration").at("enabled").get<bool>());
    CHECK(status->at("configuration").at("autoDump").get<bool>());
    CHECK(status->at("configuration").at("lazyRefresh").get<bool>());

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

TEST_CASE("luduvo context notifications can suppress passive refresh guidance")
{
    TempDir temp("luduvo_platform_context_suppressed_guidance");
    const std::string projectFile = temp.write_child("project/project.json", R"({
        "name": "Quiet Project",
        "assets": {"1": {"type": "script", "path": "scripts/Main.server.luau"}}
    })");
    const std::filesystem::path projectRoot = std::filesystem::u8path(projectFile).parent_path();
    const std::string scriptPath = temp.touch_child("project/scripts/Main.server.luau");

    TestClient client;
    WorkspaceFolder workspace(&client, "Quiet Project", Uri::file(projectRoot.generic_u8string()), std::nullopt);
    ClientLuduvoConfiguration configuration;
    configuration.context.enabled = true;
    configuration.directories.local = temp.path() + "/local";
    configuration.notifications["context.refresh-required"] = false;
    LuduvoPlatform platform(configuration, &workspace.fileResolver, &workspace);
    ClientConfiguration clientConfiguration;
    clientConfiguration.platform.luduvo = configuration;
    platform.setupWithConfiguration(clientConfiguration);
    luduvo::ContextStore store(luduvo::defaultContextStateDirectory(configuration.directories.local));
    platform.onDidOpenTextDocument(Uri::file(scriptPath));
    CHECK(client.requestQueue.empty());
    const auto logged = std::find_if(client.notificationQueue.begin(), client.notificationQueue.end(),
        [](const auto& notification)
        {
            return notification.first == "window/logMessage";
        });
    CHECK(logged != client.notificationQueue.end());
}

TEST_SUITE_END();
