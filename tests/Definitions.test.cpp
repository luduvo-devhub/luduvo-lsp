#include "doctest.h"
#include "Fixture.h"
#include "Platform/RobloxPlatform.hpp"
#include "LuauFileUtils.hpp"
#include "Luau/Parser.h"

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
        local position: vector = Vector3.new(1, 2, 3)
        entity.Position = position
        entity.Parent = game.World
        local found: Instance? = game.Prefabs.Spawn("Part")
        local query = game.World.Query("Position")
        local count: number = query:Refresh()
        local row: Instance = query.Entity[1]
        local signal: Signal = entity:GetSignal("Changed")
        signal:Connect(function() print(count, row, found) end)
        local events = Event("Hit", ToServer, {{"target", Entity}})
        events:Push(entity)
    )");
    auto result = workspace.frontend.check(workspace.fileResolver.getModuleName(document));
    REQUIRE(result.errors.empty());

    auto badDocument = newDocument(workspace, "luduvo-bad.luau", R"(
        --!strict
        self.Anchored = "wrong"
        game.Physics.Raycast("wrong", Vector3.zero, 10)
    )");
    auto badResult = workspace.frontend.check(workspace.fileResolver.getModuleName(badDocument));
    REQUIRE(badResult.errors.size() >= 2);
}

TEST_CASE("luduvo_platform_configuration_round_trips")
{
    auto config = json::parse(R"({"platform":{"type":"luduvo"}})").get<ClientConfiguration>();
    CHECK(config.platform.type == LSPPlatformConfig::Luduvo);
    CHECK(json(config)["platform"]["type"] == "luduvo");
}
