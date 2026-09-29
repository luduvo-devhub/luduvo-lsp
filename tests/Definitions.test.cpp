#include "doctest.h"
#include "Fixture.h"
#include "Platform/RobloxPlatform.hpp"
#include "TempDir.h"
#include "LuauFileUtils.hpp"
#include "Luau/Parser.h"

#include <algorithm>

using namespace Luau::LanguageServer;

TEST_SUITE_BEGIN("Definitions");

TEST_CASE("use_platform_metadata_from_first_registered_definitions_file")
{
    TestClient client;
    auto workspace = WorkspaceFolder(&client, "$TEST_WORKSPACE", Uri(), std::nullopt);

    client.definitionsFiles.emplace("@roblox", "./tests/testdata/standard_definitions.d.luau");
    client.definitionsFiles.emplace("@roblox1", "./tests/testdata/extra_definitions_relying_on_mutations.d.luau");

    workspace.setupWithConfiguration(defaultTestClientConfiguration());
    workspace.isReady = true;

    REQUIRE(workspace.definitionsFileMetadata);

    RobloxDefinitionsFileMetadata metadata = workspace.definitionsFileMetadata.value();
    REQUIRE(!metadata.SERVICES.empty());
    REQUIRE(!metadata.CREATABLE_INSTANCES.empty());
}

TEST_CASE("handles_definitions_files_relying_on_mutations")
{
    TestClient client;
    auto workspace = WorkspaceFolder(&client, "$TEST_WORKSPACE", Uri::file(*Luau::FileUtils::getCurrentWorkingDirectory()), std::nullopt);

    client.definitionsFiles.emplace("@roblox", "./tests/testdata/standard_definitions.d.luau");
    client.definitionsFiles.emplace("@roblox1", "./tests/testdata/extra_definitions_relying_on_mutations.d.luau");

    workspace.setupWithConfiguration(defaultTestClientConfiguration());
    workspace.isReady = true;

    auto document = newDocument(workspace, "foo.luau", R"(
        local x: ExtraDataRelyingOnMutations
        local y = x.RigType
    )");

    auto result = workspace.frontend.check(workspace.fileResolver.getModuleName(document));
    REQUIRE(result.errors.empty());
}

TEST_CASE("dont_crash_when_mutating_a_definitions_file_that_does_not_contain_expected_state")
{
    TestClient client;
    auto workspace = WorkspaceFolder(&client, "$TEST_WORKSPACE", Uri(), std::nullopt);

    client.definitionsFiles.emplace("@roblox", "./tests/testdata/bad_standard_definitions.d.luau");

    workspace.setupWithConfiguration(defaultTestClientConfiguration());

    REQUIRE(workspace.definitionsFileMetadata);
}

TEST_CASE("support_disabling_global_types")
{
    TestClient client;
    auto workspace = WorkspaceFolder(&client, "$TEST_WORKSPACE", Uri::file(*Luau::FileUtils::getCurrentWorkingDirectory()), std::nullopt);

    auto config = defaultTestClientConfiguration();
    config.types.disabledGlobals = {
        "table",
    };

    workspace.setupWithConfiguration(config);
    workspace.isReady = true;

    auto document = newDocument(workspace, "foo.luau", R"(
        --!strict
        local x = string.split("", "")
        local y = table.insert({}, 1)
    )");

    auto result = workspace.frontend.check(workspace.fileResolver.getModuleName(document));
    REQUIRE_EQ(result.errors.size(), 1);

    auto err = Luau::get<Luau::UnknownSymbol>(result.errors[0]);
    REQUIRE(err);
    CHECK_EQ(err->name, "table");
    CHECK_EQ(err->context, Luau::UnknownSymbol::Context::Binding);
}

TEST_CASE("support_disabling_methods_in_global_types")
{
    TestClient client;
    auto workspace = WorkspaceFolder(&client, "$TEST_WORKSPACE", Uri::file(*Luau::FileUtils::getCurrentWorkingDirectory()), std::nullopt);

    auto config = defaultTestClientConfiguration();
    config.types.disabledGlobals = {
        "table.insert",
    };

    workspace.setupWithConfiguration(config);
    workspace.isReady = true;

    auto document = newDocument(workspace, "foo.luau", R"(
        --!strict
        local x = table.find({}, "value")
        local y = table.insert({}, 1)
    )");

    auto result = workspace.frontend.check(workspace.fileResolver.getModuleName(document));
    REQUIRE_EQ(result.errors.size(), 1);

    auto err = Luau::get<Luau::UnknownProperty>(result.errors[0]);
    REQUIRE(err);
    CHECK_EQ(Luau::toString(err->table), "typeof(table)");
    CHECK_EQ(err->key, "insert");
}

TEST_CASE("package_name_is_recorded_onto_the_loaded_types")
{
    TestClient client;
    auto workspace = WorkspaceFolder(&client, "$TEST_WORKSPACE", Uri::file(*Luau::FileUtils::getCurrentWorkingDirectory()), std::nullopt);

    client.definitionsFiles.emplace("@example", "./tests/testdata/standard_definitions.d.luau");

    workspace.setupWithConfiguration(defaultTestClientConfiguration());
    workspace.isReady = true;

    auto document = newDocument(workspace, "foo.luau", R"(
        local x: Instance
    )");

    auto result = workspace.frontend.check(workspace.fileResolver.getModuleName(document));
    auto module = workspace.frontend.moduleResolver.getModule(workspace.fileResolver.getModuleName(document));
    REQUIRE(module);

    auto binding = module->getModuleScope()->linearSearchForBinding("x");
    REQUIRE(binding);

    auto ty = Luau::follow(binding->typeId);
    CHECK_EQ(ty->documentationSymbol, "@example/globaltype/Instance");

    auto ctv = Luau::get<Luau::ExternType>(ty);
    REQUIRE(ctv);
    CHECK_EQ(ctv->definitionModuleName, "@example");
}

TEST_CASE("support_disabling_methods_in_extern_types_globals")
{
    TestClient client;
    auto workspace = WorkspaceFolder(&client, "$TEST_WORKSPACE", Uri::file(*Luau::FileUtils::getCurrentWorkingDirectory()), std::nullopt);

    client.definitionsFiles.emplace("@roblox", "./tests/testdata/standard_definitions.d.luau");

    auto config = defaultTestClientConfiguration();
    config.types.disabledGlobals = {
        "game.BindToClose",
    };

    workspace.setupWithConfiguration(config);
    workspace.isReady = true;

    auto document = newDocument(workspace, "foo.luau", R"(
        --!strict
        game:BindToClose(function() end)
    )");

    auto result = workspace.frontend.check(workspace.fileResolver.getModuleName(document));
    REQUIRE_EQ(result.errors.size(), 1);

    auto err = Luau::get<Luau::UnknownProperty>(result.errors[0]);
    REQUIRE(err);
    CHECK_EQ(Luau::toString(err->table), "DataModel");
    CHECK_EQ(err->key, "BindToClose");
}

TEST_CASE_FIXTURE(Fixture, "type_functions_in_definition_files_work")
{
    ENABLE_NEW_SOLVER();

    loadDefinition("@test", R"(
        export type function foo(ty)
            return types.negationof(ty)
        end
    )");

    auto result = check(R"(
        local x: foo<number> = nil :: any
    )");
    REQUIRE(result.errors.empty());
}

TEST_SUITE_END();

TEST_CASE("luduvo_platform_loads_bundled_definitions_without_client_files")
{
    ScopedFastFlag solverFlag{FFlag::LuauSolverV2, true};
    TestClient client;
    auto config = defaultTestClientConfiguration();
    config.platform.type = LSPPlatformConfig::Luduvo;
    client.globalConfig = config;
    WorkspaceFolder workspace(&client, "$LUDUVO_TEST", Uri::file(*Luau::FileUtils::getCurrentWorkingDirectory()), std::nullopt);
    workspace.setupWithConfiguration(config);
    workspace.isReady = true;
    REQUIRE(client.definitionsFiles.empty());
    auto document = newDocument(workspace, "luduvo-test.luau", R"(
        --!strict
        local entity: Instance = self
        entity.Anchored = true
        local physics = game.Physics
    )");
    auto result = workspace.frontend.check(workspace.fileResolver.getModuleName(document));
    REQUIRE(result.errors.empty());

    auto badDocument = newDocument(workspace, "luduvo-bad.luau", R"(
        --!strict
        self.Anchored = "wrong"
    )");
    auto badResult = workspace.frontend.check(workspace.fileResolver.getModuleName(badDocument));
    REQUIRE_EQ(badResult.errors.size(), 1);
}

TEST_CASE("luduvo_platform_ignores_unused_lifecycle_functions")
{
    ScopedFastFlag solverFlag{FFlag::LuauSolverV2, true};
    TestClient client;
    auto config = defaultTestClientConfiguration();
    config.platform.type = LSPPlatformConfig::Luduvo;
    client.globalConfig = config;
    WorkspaceFolder workspace(&client, "$LUDUVO_LINT_TEST", Uri::file(*Luau::FileUtils::getCurrentWorkingDirectory()), std::nullopt);
    workspace.setupWithConfiguration(config);
    workspace.isReady = true;

    auto document = newDocument(workspace, "luduvo-lifecycle.luau", R"(
        local function Update(dt) print(dt) end
        local function PhysicsUpdate(dt) print(dt) end
        local function Migrate(old) print(old) end
        local function StillUnused() end
    )");
    auto result = workspace.checkSimple(workspace.fileResolver.getModuleName(document), nullptr);
    REQUIRE_EQ(result.lintResult.warnings.size(), 1);
    CHECK(result.lintResult.warnings[0].text.find("StillUnused") != std::string::npos);
}

TEST_CASE("luduvo_platform_configuration_round_trips")
{
    auto config = json::parse(
        R"({"platform":{"type":"luduvo","luduvo":{"dataDirectory":"data","definitions":{"serverOverride":"server.luau","clientOverride":"client.luau","defaultScriptSide":"client","globalPolicy":"preferDefinitionFiles","conflictWinner":"definitionFiles","exposePrivateTypes":false},"hover":{"presentation":"standard","maxTitleLength":null,"maxTypeDefinitionLength":4096}}}})")
                      .get<ClientConfiguration>();
    CHECK(config.platform.type == LSPPlatformConfig::Luduvo);
    CHECK_EQ(config.platform.luduvo.dataDirectory, "data");
    CHECK_EQ(config.platform.luduvo.definitions.serverOverride, "server.luau");
    CHECK_EQ(config.platform.luduvo.definitions.clientOverride, "client.luau");
    CHECK(config.platform.luduvo.definitions.defaultScriptSide == LuduvoScriptSide::Client);
    CHECK(config.platform.luduvo.definitions.globalPolicy == LuduvoGlobalDefinitionsPolicy::PreferDefinitionFiles);
    CHECK(config.platform.luduvo.definitions.conflictWinner == LuduvoDefinitionConflictWinner::DefinitionFiles);
    CHECK_FALSE(config.platform.luduvo.definitions.exposePrivateTypes);
    CHECK(config.platform.luduvo.hover.presentation == LuduvoHoverPresentation::Standard);
    CHECK_FALSE(config.platform.luduvo.hover.maxTitleLength);
    REQUIRE(config.platform.luduvo.hover.maxTypeDefinitionLength);
    CHECK_EQ(*config.platform.luduvo.hover.maxTypeDefinitionLength, 4096);
    CHECK(json(config)["platform"]["type"] == "luduvo");
    CHECK(json(config)["platform"]["luduvo"]["definitions"]["defaultScriptSide"] == "client");
    CHECK(json(config)["platform"]["luduvo"]["definitions"]["globalPolicy"] == "preferDefinitionFiles");
    CHECK(json(config)["platform"]["luduvo"]["hover"]["presentation"] == "standard");
    CHECK(json(config)["platform"]["luduvo"]["hover"]["maxTitleLength"].is_null());
    CHECK(json(config)["platform"]["luduvo"]["hover"]["maxTypeDefinitionLength"] == 4096);
}

TEST_CASE("luduvo_rich_hover_configuration_defaults")
{
    ClientConfiguration config;
    CHECK(config.platform.luduvo.hover.presentation == LuduvoHoverPresentation::Rich);
    REQUIRE(config.platform.luduvo.hover.maxTitleLength);
    CHECK_EQ(*config.platform.luduvo.hover.maxTitleLength, 96);
    REQUIRE(config.platform.luduvo.hover.maxTypeDefinitionLength);
    CHECK_EQ(*config.platform.luduvo.hover.maxTypeDefinitionLength, 2000);
}

TEST_CASE("luduvo_is_the_default_platform")
{
    ClientConfiguration config;
    CHECK(config.platform.type == LSPPlatformConfig::Luduvo);
    CHECK(json(config)["platform"]["type"] == "luduvo");
}

TEST_CASE("luduvo_platform_selects_server_and_client_types_from_file_names")
{
    ScopedFastFlag solverFlag{FFlag::LuauSolverV2, true};
    TestClient client;
    auto config = defaultTestClientConfiguration();
    config.platform.type = LSPPlatformConfig::Luduvo;
    client.globalConfig = config;
    WorkspaceFolder workspace(&client, "$LUDUVO_SURFACE_TEST", Uri::file(*Luau::FileUtils::getCurrentWorkingDirectory()), std::nullopt);
    workspace.setupWithConfiguration(config);
    workspace.isReady = true;

    auto serverDocument = newDocument(workspace, "physics.server.luau", "--!strict\nlocal explode = game.Physics.Explode");
    auto clientDocument = newDocument(workspace, "physics.client.luau", "--!strict\nlocal explode = game.Physics.Explode");
    auto plainDocument = newDocument(workspace, "physics.luau", "--!strict\nlocal explode = game.Physics.Explode");

    CHECK(workspace.frontend.check(workspace.fileResolver.getModuleName(serverDocument)).errors.empty());
    CHECK_EQ(workspace.frontend.check(workspace.fileResolver.getModuleName(clientDocument)).errors.size(), 1);
    CHECK(workspace.frontend.check(workspace.fileResolver.getModuleName(plainDocument)).errors.empty());

    CHECK_EQ(*workspace.fileResolver.getEnvironmentForModule(workspace.fileResolver.getModuleName(serverDocument)), "LuduvoServer");
    CHECK_EQ(*workspace.fileResolver.getEnvironmentForModule(workspace.fileResolver.getModuleName(clientDocument)), "LuduvoClient");
}

TEST_CASE("luduvo_platform_exposes_named_types_in_side_specific_environments")
{
    ScopedFastFlag solverFlag{FFlag::LuauSolverV2, true};
    TestClient client;
    auto config = defaultTestClientConfiguration();
    config.platform.type = LSPPlatformConfig::Luduvo;
    config.platform.luduvo.definitions.exposePrivateTypes = true;
    client.globalConfig = config;
    WorkspaceFolder workspace(&client, "$LUDUVO_NAMED_TYPE_TEST", Uri::file(*Luau::FileUtils::getCurrentWorkingDirectory()), std::nullopt);
    workspace.setupWithConfiguration(config);
    workspace.isReady = true;

    auto serverDocument = newDocument(
        workspace, "raycast.server.luau", "--!strict\nlocal hit: RaycastResult? = game.Physics.Raycast(Vector3.zero, Vector3.yAxis, 100)");
    auto clientDocument = newDocument(
        workspace, "raycast.client.luau", "--!strict\nlocal hit: RaycastResult? = game.Physics.Raycast(Vector3.zero, Vector3.yAxis, 100)");

    const auto serverScope = workspace.frontend.getEnvironmentScope("LuduvoServer");
    const auto clientScope = workspace.frontend.getEnvironmentScope("LuduvoClient");
    REQUIRE(serverScope);
    REQUIRE(clientScope);
    CHECK(serverScope->exportedTypeBindings.find("RaycastResult") != serverScope->exportedTypeBindings.end());
    CHECK(clientScope->exportedTypeBindings.find("RaycastResult") != clientScope->exportedTypeBindings.end());

    const auto serverResult = workspace.frontend.check(workspace.fileResolver.getModuleName(serverDocument));
    const auto clientResult = workspace.frontend.check(workspace.fileResolver.getModuleName(clientDocument));
    if (!serverResult.errors.empty())
        CAPTURE(Luau::toString(serverResult.errors.front()));
    if (!clientResult.errors.empty())
        CAPTURE(Luau::toString(clientResult.errors.front()));
    CHECK(serverResult.errors.empty());
    CHECK(clientResult.errors.empty());
}

TEST_CASE("luduvo_platform_can_leave_private_definition_types_private")
{
    ScopedFastFlag solverFlag{FFlag::LuauSolverV2, true};
    TempDir temp("luduvo_private_definition_types");
    const std::string serverDefinitions = temp.write_child(
        "server.luau", "type PrivateAlias = string\ndeclare valueUsingPrivateAlias: PrivateAlias\n");

    TestClient client;
    auto config = defaultTestClientConfiguration();
    config.platform.type = LSPPlatformConfig::Luduvo;
    config.platform.luduvo.definitions.serverOverride = serverDefinitions;
    config.platform.luduvo.definitions.exposePrivateTypes = false;
    client.globalConfig = config;
    WorkspaceFolder workspace(&client, "$LUDUVO_PRIVATE_TYPE_TEST", Uri::file(*Luau::FileUtils::getCurrentWorkingDirectory()), std::nullopt);
    workspace.setupWithConfiguration(config);
    workspace.isReady = true;

    auto valueDocument = newDocument(workspace, "value.server.luau", "local value: string = valueUsingPrivateAlias");
    auto aliasDocument = newDocument(workspace, "alias.server.luau", "local value: PrivateAlias = valueUsingPrivateAlias");

    CHECK(workspace.frontend.check(workspace.fileResolver.getModuleName(valueDocument)).errors.empty());
    CHECK_EQ(workspace.frontend.check(workspace.fileResolver.getModuleName(aliasDocument)).errors.size(), 1);
}

TEST_CASE("luduvo_platform_uses_configured_default_and_exact_script_suffixes")
{
    ScopedFastFlag solverFlag{FFlag::LuauSolverV2, true};
    TestClient client;
    auto config = defaultTestClientConfiguration();
    config.platform.type = LSPPlatformConfig::Luduvo;
    config.platform.luduvo.definitions.defaultScriptSide = LuduvoScriptSide::Client;
    client.globalConfig = config;
    WorkspaceFolder workspace(&client, "$LUDUVO_DEFAULT_TEST", Uri::file(*Luau::FileUtils::getCurrentWorkingDirectory()), std::nullopt);
    workspace.setupWithConfiguration(config);
    workspace.isReady = true;

    auto plain = newDocument(workspace, "plain.luau", "--!strict\nlocal explode = game.Physics.Explode");
    auto clientLast = newDocument(workspace, "mixed.server.client.luau", "return game.Physics");
    auto serverLast = newDocument(workspace, "mixed.client.server.luau", "return game.Physics");

    CHECK_EQ(workspace.frontend.check(workspace.fileResolver.getModuleName(plain)).errors.size(), 1);
    CHECK_EQ(*workspace.fileResolver.getEnvironmentForModule(workspace.fileResolver.getModuleName(clientLast)), "LuduvoClient");
    CHECK_EQ(*workspace.fileResolver.getEnvironmentForModule(workspace.fileResolver.getModuleName(serverLast)), "LuduvoServer");
}

TEST_CASE("luduvo_platform_falls_back_when_configured_definitions_do_not_parse")
{
    ScopedFastFlag solverFlag{FFlag::LuauSolverV2, true};
    TempDir temp("luduvo_definition_fallback");
    const std::string badDefinitions = temp.write_child("bad-server.luau", "declare this is not Luau");

    TestClient client;
    auto config = defaultTestClientConfiguration();
    config.platform.type = LSPPlatformConfig::Luduvo;
    config.platform.luduvo.dataDirectory = temp.path();
    config.platform.luduvo.definitions.serverOverride = badDefinitions;
    client.globalConfig = config;
    WorkspaceFolder workspace(&client, "$LUDUVO_FALLBACK_TEST", Uri::file(*Luau::FileUtils::getCurrentWorkingDirectory()), std::nullopt);
    workspace.setupWithConfiguration(config);
    workspace.isReady = true;

    auto document = newDocument(workspace, "fallback.server.luau", "local explode = game.Physics.Explode");
    CHECK(workspace.frontend.check(workspace.fileResolver.getModuleName(document)).errors.empty());

    size_t warningRequest = client.requestQueue.size();
    for (size_t index = 0; index < client.requestQueue.size(); ++index)
    {
        if (client.requestQueue[index].first == "window/showMessageRequest")
        {
            warningRequest = index;
            break;
        }
    }
    REQUIRE(warningRequest < client.requestQueue.size());
    REQUIRE(client.requestQueue[warningRequest].second);
    const json& warning = *client.requestQueue[warningRequest].second;
    CHECK(warning["type"] == lsp::MessageType::Warning);
    CHECK(warning["message"].get<std::string>().find("luau-lsp.platform.luduvo.dataDirectory") != std::string::npos);
    CHECK(warning["actions"][0]["title"] == "Open Luduvo Settings");

    REQUIRE(client.requestHandlers[warningRequest]);
    JsonRpcMessage response;
    response.result = json{{"title", "Open Luduvo Settings"}};
    (*client.requestHandlers[warningRequest])(response);
    REQUIRE(!client.notificationQueue.empty());
    const auto& [method, params] = client.notificationQueue.back();
    CHECK_EQ(method, "$/command");
    REQUIRE(params);
    CHECK((*params)["command"] == "workbench.action.openSettings");
    CHECK((*params)["data"] == "luau-lsp.platform.luduvo.dataDirectory");
}

TEST_CASE("luduvo_platform_prefers_installed_definitions_over_embedded_definitions")
{
    ScopedFastFlag solverFlag{FFlag::LuauSolverV2, true};
    TempDir temp("luduvo_installed_definitions");
    temp.write_child("content/current", "42\n");
    temp.write_child("content/versions/42/defs/luduvo.d.luau", "declare installedServerOnly: string\n");

    TestClient client;
    auto config = defaultTestClientConfiguration();
    config.platform.type = LSPPlatformConfig::Luduvo;
    config.platform.luduvo.dataDirectory = temp.path();
    client.globalConfig = config;
    WorkspaceFolder workspace(&client, "$LUDUVO_INSTALLED_TEST", Uri::file(*Luau::FileUtils::getCurrentWorkingDirectory()), std::nullopt);
    workspace.setupWithConfiguration(config);
    workspace.isReady = true;

    auto document = newDocument(workspace, "installed.server.luau", "local value: string = installedServerOnly");
    CHECK(workspace.frontend.check(workspace.fileResolver.getModuleName(document)).errors.empty());
}

TEST_CASE("luduvo_platform_prefers_configured_definitions_over_installed_definitions")
{
    ScopedFastFlag solverFlag{FFlag::LuauSolverV2, true};
    TempDir temp("luduvo_configured_definitions");
    temp.write_child("content/current", "42\n");
    temp.write_child("content/versions/42/defs/luduvo.d.luau", "declare installedServerOnly: string\n");
    const std::string configured = temp.write_child("configured-server.luau", "declare configuredServerOnly: string\n");

    TestClient client;
    auto config = defaultTestClientConfiguration();
    config.platform.type = LSPPlatformConfig::Luduvo;
    config.platform.luduvo.dataDirectory = temp.path();
    config.platform.luduvo.definitions.serverOverride = configured;
    client.globalConfig = config;
    WorkspaceFolder workspace(&client, "$LUDUVO_CONFIGURED_TEST", Uri::file(*Luau::FileUtils::getCurrentWorkingDirectory()), std::nullopt);
    workspace.setupWithConfiguration(config);
    workspace.isReady = true;

    auto document = newDocument(workspace, "configured.server.luau", "--!strict\nlocal value: string = configuredServerOnly\nlocal absent = installedServerOnly");
    auto result = workspace.frontend.check(workspace.fileResolver.getModuleName(document));
    REQUIRE_EQ(result.errors.size(), 1);
    auto unknown = Luau::get<Luau::UnknownSymbol>(result.errors[0]);
    REQUIRE(unknown);
    CHECK_EQ(unknown->name, "installedServerOnly");
}

TEST_CASE("luduvo_global_definitions_policy_selects_definition_sources")
{
    ScopedFastFlag solverFlag{FFlag::LuauSolverV2, true};
    TempDir temp("luduvo_global_policy");
    const std::string definitions = temp.write_child("custom.d.luau", "declare customDefinitionOnly: string\n");

    struct Case
    {
        LuduvoGlobalDefinitionsPolicy policy;
        bool hasLuduvo;
        bool hasDefinitionFiles;
    };
    const Case cases[] = {
        {LuduvoGlobalDefinitionsPolicy::LuduvoOnly, true, false},
        {LuduvoGlobalDefinitionsPolicy::DefinitionFilesOnly, false, true},
        {LuduvoGlobalDefinitionsPolicy::PreferLuduvo, true, false},
        {LuduvoGlobalDefinitionsPolicy::PreferDefinitionFiles, false, true},
        {LuduvoGlobalDefinitionsPolicy::Combine, true, true},
    };

    size_t index = 0;
    for (const auto& testCase : cases)
    {
        CAPTURE(index);
        TestClient client;
        auto config = defaultTestClientConfiguration();
        config.platform.type = LSPPlatformConfig::Luduvo;
        config.platform.luduvo.definitions.globalPolicy = testCase.policy;
        client.globalConfig = config;
        client.definitionsFiles["@custom"] = definitions;
        WorkspaceFolder workspace(
            &client, "$LUDUVO_POLICY_" + std::to_string(index++), Uri::file(*Luau::FileUtils::getCurrentWorkingDirectory()), std::nullopt);
        workspace.setupWithConfiguration(config);
        workspace.isReady = true;

        auto luduvoDocument = newDocument(workspace, "policy.server.luau", "local physics = game.Physics");
        auto customDocument = newDocument(workspace, "policy-custom.server.luau", "local value: string = customDefinitionOnly");
        CHECK(workspace.frontend.check(workspace.fileResolver.getModuleName(luduvoDocument)).errors.empty() == testCase.hasLuduvo);
        CHECK(workspace.frontend.check(workspace.fileResolver.getModuleName(customDocument)).errors.empty() == testCase.hasDefinitionFiles);
    }
}

TEST_CASE("luduvo_definition_conflict_winner_controls_colliding_globals")
{
    ScopedFastFlag solverFlag{FFlag::LuauSolverV2, true};
    TempDir temp("luduvo_conflict_winner");
    const std::string definitions = temp.write_child("custom.d.luau", "declare game: { FromDefinitionFiles: string }\n");

    for (auto winner : {LuduvoDefinitionConflictWinner::Luduvo, LuduvoDefinitionConflictWinner::DefinitionFiles})
    {
        CAPTURE(static_cast<int>(winner));
        TestClient client;
        auto config = defaultTestClientConfiguration();
        config.platform.type = LSPPlatformConfig::Luduvo;
        config.platform.luduvo.definitions.globalPolicy = LuduvoGlobalDefinitionsPolicy::Combine;
        config.platform.luduvo.definitions.conflictWinner = winner;
        client.globalConfig = config;
        client.definitionsFiles["@custom"] = definitions;
        WorkspaceFolder workspace(&client, "$LUDUVO_CONFLICT_" + std::to_string(static_cast<int>(winner)),
            Uri::file(*Luau::FileUtils::getCurrentWorkingDirectory()), std::nullopt);
        workspace.setupWithConfiguration(config);
        workspace.isReady = true;

        const auto environmentScope = workspace.frontend.getEnvironmentScope("LuduvoServer");
        REQUIRE(environmentScope);
        const auto game = environmentScope->linearSearchForBinding("game", false);
        REQUIRE(game);
        const std::string gameType = Luau::toString(game->typeId);
        const bool luduvoWins = winner == LuduvoDefinitionConflictWinner::Luduvo;
        CHECK((gameType.find("Animation") != std::string::npos) == luduvoWins);
        CHECK((gameType.find("FromDefinitionFiles") != std::string::npos) != luduvoWins);
    }
}

TEST_CASE("luduvo_official_definitions_report_an_unmet_solver_requirement")
{
    ScopedFastFlag solverFlag{FFlag::LuauSolverV2, false};
    TestClient client;
    auto config = defaultTestClientConfiguration();
    config.platform.type = LSPPlatformConfig::Luduvo;
    config.platform.luduvo.definitions.globalPolicy = LuduvoGlobalDefinitionsPolicy::LuduvoOnly;
    client.globalConfig = config;
    WorkspaceFolder workspace(
        &client, "$LUDUVO_SOLVER_REQUIREMENT", Uri::file(*Luau::FileUtils::getCurrentWorkingDirectory()), std::nullopt);
    workspace.setupWithConfiguration(config);
    workspace.isReady = true;

    auto request = std::find_if(client.requestQueue.begin(), client.requestQueue.end(),
        [](const auto& item)
        {
            return item.first == "window/showMessageRequest" && item.second &&
                   (*item.second)["message"].template get<std::string>().find("SolverV2") != std::string::npos;
        });
    REQUIRE(request != client.requestQueue.end());
    CHECK((*request->second)["type"] == lsp::MessageType::Error);
    CHECK((*request->second)["actions"][0]["title"] == "Open Settings");
    CHECK((*request->second)["actions"][1]["title"] == "Use Definition Files Only");

    auto document = newDocument(workspace, "missing.server.luau", "local physics = game.Physics");
    CHECK_FALSE(workspace.frontend.check(workspace.fileResolver.getModuleName(document)).errors.empty());
    CHECK_FALSE(workspace.fileResolver.getEnvironmentForModule(workspace.fileResolver.getModuleName(document)).has_value());
}

TEST_CASE("definition_files_only_does_not_require_solver_v2")
{
    ScopedFastFlag solverFlag{FFlag::LuauSolverV2, false};
    TempDir temp("luduvo_old_solver_definitions");
    const std::string definitions = temp.write_child("custom.d.luau", "declare oldSolverDefinition: string\n");
    TestClient client;
    auto config = defaultTestClientConfiguration();
    config.platform.type = LSPPlatformConfig::Luduvo;
    config.platform.luduvo.definitions.globalPolicy = LuduvoGlobalDefinitionsPolicy::DefinitionFilesOnly;
    client.globalConfig = config;
    client.definitionsFiles["@custom"] = definitions;
    WorkspaceFolder workspace(&client, "$LUDUVO_OLD_SOLVER", Uri::file(*Luau::FileUtils::getCurrentWorkingDirectory()), std::nullopt);
    workspace.setupWithConfiguration(config);
    workspace.isReady = true;

    auto document = newDocument(workspace, "custom.server.luau", "local value: string = oldSolverDefinition");
    CHECK(workspace.frontend.check(workspace.fileResolver.getModuleName(document)).errors.empty());
    CHECK(client.requestQueue.empty());
}
