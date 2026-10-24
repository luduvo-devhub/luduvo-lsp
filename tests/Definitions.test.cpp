#include "doctest.h"
#include "Fixture.h"
#include "Platform/RobloxPlatform.hpp"
#include "Platform/luduvo/LuduvoBindings.hpp"
#include "Platform/luduvo/LuduvoResourceCatalog.hpp"
#include "TempDir.h"
#include "LuauFileUtils.hpp"
#include "Luau/Parser.h"

#include <algorithm>

using namespace Luau::LanguageServer;

TEST_SUITE_BEGIN("Definitions");

TEST_CASE("luduvo_resource_catalog_resolves_group_sources")
{
    TempDir temp("luduvo_resource_catalog");
    temp.write_child("content/current", "42\n");
    temp.touch_child("content/versions/42/prefabs/Installed.ldv");
    temp.touch_child("content/versions/42/prefabs/Another.LDV");
    temp.touch_child("content/versions/42/prefabs/ignored.txt");

    LuduvoResourceCatalog catalog(R"({
        "formatVersion": 2,
        "resources": {
            "component": [{"name": "Position", "documentation": "World-space position"}, "Health", "Position"],
            "prefab": ["Bundled"]
        }
    })");
    ClientLuduvoConfiguration config;
    config.dataDirectory = temp.path();
    catalog.configure(config);

    CHECK_EQ(catalog.names("component"), std::vector<std::string>({"Health", "Position"}));
    CHECK_EQ(catalog.names("prefab"), std::vector<std::string>({"Another", "Installed"}));
    REQUIRE(catalog.group("component"));
    CHECK_EQ(catalog.group("component")->kind, "component");
    REQUIRE_EQ(catalog.group("component")->resources.size(), 2);
    CHECK(catalog.group("component")->resources[0].builtIn);
    CHECK_EQ(catalog.group("component")->resources[1].documentation, "World-space position");
    REQUIRE(catalog.group("prefab"));
    REQUIRE_EQ(catalog.group("prefab")->resources.size(), 2);
    CHECK(catalog.group("prefab")->resources[0].builtIn);

    config.resources.sources["custom-components"].values = {
        "Custom", ClientLuduvoResourcesConfiguration::Source::Value{"Documented", "Project component documentation"}};
    config.resources.sources["default-states"].values = {"Running", "Idle"};
    config.resources.groups["component"] = {"string", true, {"custom-components"}};
    config.resources.groups["animation-state"] = {"string", false, {"default-states"}};
    catalog.configure(config);
    CHECK_EQ(catalog.names("component"), std::vector<std::string>({"Custom", "Documented", "Health", "Position"}));
    const auto documented = std::find_if(catalog.group("component")->resources.begin(), catalog.group("component")->resources.end(),
        [](const LuduvoResourceName& resource)
        {
            return resource.name == "Documented";
        });
    REQUIRE(documented != catalog.group("component")->resources.end());
    CHECK_EQ(documented->documentation, "Project component documentation");
    CHECK_FALSE(documented->builtIn);
    REQUIRE(catalog.group("animation-state"));
    CHECK_EQ(catalog.names("animation-state"), std::vector<std::string>({"Idle", "Running"}));
    CHECK_FALSE(catalog.group("animation-state")->resources[0].builtIn);

    config.resources.groups["prefab"] = {"string", false, {}};
    config.resources.groups["component"] = {"string", false, {"custom-components"}};
    catalog.configure(config);
    CHECK(catalog.names("prefab").empty());
    CHECK_EQ(catalog.names("component"), std::vector<std::string>({"Custom", "Documented"}));
    REQUIRE(catalog.group("component"));
    REQUIRE_EQ(catalog.group("component")->resources.size(), 2);
    CHECK_FALSE(catalog.group("component")->resources[0].builtIn);

    LuduvoResourceCatalog legacyCatalog(R"({"formatVersion": 1, "resources": {"component": ["Position"]}})");
    legacyCatalog.configure(ClientLuduvoConfiguration{});
    CHECK_EQ(legacyCatalog.names("component"), std::vector<std::string>({"Position"}));

    config.resources.groups["missing-group"] = {"string", false, {"missing-source"}};
    catalog.configure(config);
    REQUIRE_EQ(catalog.configurationWarnings().size(), 1);
    CHECK(catalog.configurationWarnings()[0].find("missing-source") != std::string::npos);
}

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

TEST_SUITE_BEGIN("LuduvoDefinitions");

TEST_CASE("luduvo_platform_loads_bundled_definitions_without_client_files")
{
    LuduvoFixture fixture("luduvo_bundled_definitions");
    REQUIRE(fixture.client.definitionsFiles.empty());
    CHECK_EQ(fixture.config.platform.luduvo.dataDirectory, fixture.tempDir.path());
    auto document = fixture.newDocument("luduvo-test.luau", R"(
        --!strict
        local entity: Instance = self
        entity.Anchored = true
        local physics = game.Physics
    )");
    auto result = fixture.workspace.frontend.check(fixture.workspace.fileResolver.getModuleName(document));
    REQUIRE(result.errors.empty());

    auto badDocument = fixture.newDocument("luduvo-bad.luau", R"(
        --!strict
        self.Anchored = "wrong"
    )");
    auto badResult = fixture.workspace.frontend.check(fixture.workspace.fileResolver.getModuleName(badDocument));
    REQUIRE_EQ(badResult.errors.size(), 1);
}

TEST_CASE("luduvo_platform_ignores_unused_lifecycle_functions")
{
    ScopedFastFlag solverFlag{FFlag::LuauSolverV2, true};
    TempDir temp("luduvo_lifecycle_functions");
    TestClient client;
    auto config = defaultTestClientConfiguration();
    config.platform.type = LSPPlatformConfig::Luduvo;
    config.platform.luduvo.dataDirectory = temp.path();
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
        R"({"platform":{"type":"luduvo","luduvo":{"dataDirectory":"data","definitions":{"serverOverride":"server.luau","clientOverride":"client.luau","defaultScriptSide":"client","globalPolicy":"preferDefinitionFiles","conflictWinner":"definitionFiles","exposePrivateTypes":false},"hover":{"presentation":"standard","maxTitleLength":null,"maxTypeDefinitionLength":4096},"resources":{"groups":{"component":{"dataType":"string","includeBuiltins":false,"sources":["test-components"]},"prefab":{"dataType":"string","includeBuiltins":false,"sources":[]}},"sources":{"test-components":{"provider":"static","values":["Health",{"name":"Position","documentation":"World-space position"}]}},"bindings":[{"symbol":"MyLibrary.Query","parameter":"components","kind":"component","variadic":true,"pluralResourceNoun":true}],"completion":{"enabled":false,"disabledKinds":["component"],"disabledApis":["game.World.Query"]}}}}})")
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
    CHECK_EQ(config.platform.luduvo.resources.groups.at("component").dataType, "string");
    CHECK_FALSE(config.platform.luduvo.resources.groups.at("component").includeBuiltins);
    CHECK_EQ(config.platform.luduvo.resources.groups.at("component").sources, std::vector<std::string>({"test-components"}));
    CHECK(config.platform.luduvo.resources.groups.at("prefab").sources.empty());
    CHECK_EQ(config.platform.luduvo.resources.sources.at("test-components").provider, "static");
    const auto& componentValues = config.platform.luduvo.resources.sources.at("test-components").values;
    REQUIRE_EQ(componentValues.size(), 2);
    CHECK_EQ(componentValues[0].name, "Health");
    CHECK_FALSE(componentValues[0].documentation);
    CHECK_EQ(componentValues[1].name, "Position");
    CHECK_EQ(componentValues[1].documentation, "World-space position");
    REQUIRE_EQ(config.platform.luduvo.resources.bindings.size(), 1);
    CHECK_EQ(config.platform.luduvo.resources.bindings[0].symbol, "MyLibrary.Query");
    CHECK_EQ(config.platform.luduvo.resources.bindings[0].parameter, "components");
    CHECK_EQ(config.platform.luduvo.resources.bindings[0].kind, "component");
    CHECK(config.platform.luduvo.resources.bindings[0].variadic);
    CHECK(config.platform.luduvo.resources.bindings[0].pluralResourceNoun);
    CHECK_FALSE(config.platform.luduvo.resources.completion.enabled);
    CHECK_EQ(config.platform.luduvo.resources.completion.disabledKinds, std::vector<std::string>{"component"});
    CHECK_EQ(config.platform.luduvo.resources.completion.disabledApis, std::vector<std::string>{"game.World.Query"});
    CHECK(json(config)["platform"]["type"] == "luduvo");
    CHECK(json(config)["platform"]["luduvo"]["definitions"]["defaultScriptSide"] == "client");
    CHECK(json(config)["platform"]["luduvo"]["definitions"]["globalPolicy"] == "preferDefinitionFiles");
    CHECK(json(config)["platform"]["luduvo"]["hover"]["presentation"] == "standard");
    CHECK(json(config)["platform"]["luduvo"]["hover"]["maxTitleLength"].is_null());
    CHECK(json(config)["platform"]["luduvo"]["hover"]["maxTypeDefinitionLength"] == 4096);
    CHECK(json(config)["platform"]["luduvo"]["resources"]["groups"]["component"]["dataType"] == "string");
    CHECK_FALSE(json(config)["platform"]["luduvo"]["resources"]["groups"]["component"]["includeBuiltins"]);
    CHECK(json(config)["platform"]["luduvo"]["resources"]["groups"]["component"]["sources"] == json::array({"test-components"}));
    CHECK(json(config)["platform"]["luduvo"]["resources"]["groups"]["prefab"]["sources"] == json::array());
    CHECK(json(config)["platform"]["luduvo"]["resources"]["sources"]["test-components"]["provider"] == "static");
    CHECK(json(config)["platform"]["luduvo"]["resources"]["sources"]["test-components"]["values"] ==
          json::array({"Health", {{"name", "Position"}, {"documentation", "World-space position"}}}));
    CHECK(json(config)["platform"]["luduvo"]["resources"]["bindings"][0]["parameter"] == "components");
    CHECK(json(config)["platform"]["luduvo"]["resources"]["bindings"][0]["variadic"]);
    CHECK(json(config)["platform"]["luduvo"]["resources"]["bindings"][0]["pluralResourceNoun"]);
    CHECK_FALSE(json(config)["platform"]["luduvo"]["resources"]["completion"]["enabled"]);
}

TEST_CASE("luduvo_resource_configuration_defaults")
{
    ClientConfiguration config;
    CHECK(config.platform.luduvo.resources.groups.empty());
    CHECK(config.platform.luduvo.resources.sources.empty());
    CHECK(config.platform.luduvo.resources.bindings.empty());
    CHECK(config.platform.luduvo.resources.completion.enabled);
    CHECK(config.platform.luduvo.resources.completion.disabledKinds.empty());
    CHECK(config.platform.luduvo.resources.completion.disabledApis.empty());
}

TEST_CASE("luduvo_general_bindings_round_trip")
{
    const json input = json::parse(R"({
        "platform": {"luduvo": {
            "bindings": [
                {"symbol": "MyLibrary.Spawn", "kind": "resource", "parameter": "name", "group": "prefab"},
                {"symbol": "Color3.fromHex", "kind": "color", "parameter": "hex", "encoding": "hex"}
            ]
        }}
    })");
    const ClientConfiguration config = input.get<ClientConfiguration>();
    REQUIRE_EQ(config.platform.luduvo.bindings.size(), 2);
    CHECK_EQ(config.platform.luduvo.bindings[0].kind, "resource");
    CHECK_EQ(config.platform.luduvo.bindings[0].group, "prefab");
    CHECK_EQ(config.platform.luduvo.bindings[1].encoding, "hex");

    const json output = config;
    CHECK_EQ(output["platform"]["luduvo"]["bindings"][0]["group"], "prefab");
    CHECK_EQ(output["platform"]["luduvo"]["bindings"][1]["kind"], "color");
}

TEST_CASE("luduvo_binding_registry_rejects_unsupported_and_incomplete_bindings")
{
    ClientLuduvoConfiguration config;
    config.bindings.push_back({"Paint", "future-kind"});
    config.bindings.push_back({"Paint", "color", "red", {}, {}, "rgb"});

    LuduvoBindingRegistry registry;
    registry.configure(config);
    REQUIRE_EQ(registry.configurationWarnings().size(), 2);
    CHECK(registry.configurationWarnings()[0].find("unsupported kind") != std::string::npos);
    CHECK(registry.configurationWarnings()[1].find("requires 3") != std::string::npos);
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
    TempDir temp("luduvo_surface_types");
    TestClient client;
    auto config = defaultTestClientConfiguration();
    config.platform.type = LSPPlatformConfig::Luduvo;
    config.platform.luduvo.dataDirectory = temp.path();
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
    TempDir temp("luduvo_side_specific_named_types");
    TestClient client;
    auto config = defaultTestClientConfiguration();
    config.platform.type = LSPPlatformConfig::Luduvo;
    config.platform.luduvo.dataDirectory = temp.path();
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
    const std::string serverDefinitions =
        temp.write_child("server.luau", "type PrivateAlias = string\ndeclare valueUsingPrivateAlias: PrivateAlias\n");

    TestClient client;
    auto config = defaultTestClientConfiguration();
    config.platform.type = LSPPlatformConfig::Luduvo;
    config.platform.luduvo.dataDirectory = temp.path();
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
    TempDir temp("luduvo_default_script_side");
    TestClient client;
    auto config = defaultTestClientConfiguration();
    config.platform.type = LSPPlatformConfig::Luduvo;
    config.platform.luduvo.dataDirectory = temp.path();
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

    auto document =
        newDocument(workspace, "configured.server.luau", "--!strict\nlocal value: string = configuredServerOnly\nlocal absent = installedServerOnly");
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
        config.platform.luduvo.dataDirectory = temp.path();
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
        config.platform.luduvo.dataDirectory = temp.path();
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
    TempDir temp("luduvo_solver_requirement");
    TestClient client;
    auto config = defaultTestClientConfiguration();
    config.platform.type = LSPPlatformConfig::Luduvo;
    config.platform.luduvo.dataDirectory = temp.path();
    config.platform.luduvo.definitions.globalPolicy = LuduvoGlobalDefinitionsPolicy::LuduvoOnly;
    client.globalConfig = config;
    WorkspaceFolder workspace(&client, "$LUDUVO_SOLVER_REQUIREMENT", Uri::file(*Luau::FileUtils::getCurrentWorkingDirectory()), std::nullopt);
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
    config.platform.luduvo.dataDirectory = temp.path();
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

TEST_SUITE_END();
