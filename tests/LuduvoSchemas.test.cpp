#include "Fixture.h"
#include "doctest.h"
#include "Platform/luduvo/schema/LuduvoSchemaIndex.hpp"
#include "Platform/luduvo/schema/LuduvoSchemaTypes.hpp"
#include "Platform/luduvo/schema/LuduvoSchemaExports.hpp"
#include "Platform/luduvo/schema/LuduvoSchemaEdits.hpp"
#include "Platform/luduvo/schema/LuduvoSchemaService.hpp"
#include "Platform/luduvo/LuduvoResourceCatalog.hpp"
#include "Platform/luduvo/LuduvoBindings.hpp"
#include "Platform/luduvo/hover/LuduvoHoverAnalysis.hpp"
#include "Platform/luduvo/diagnostics/LuduvoDiagnostics.hpp"
#include "Luau/Parser.h"
#include "LuauFileUtils.hpp"
#include <filesystem>

using namespace luduvo;
static std::string getErrors(const Luau::CheckResult& result)
{
    std::string message;
    for (const auto& error : result.errors)
        message += Luau::toString(error) + "\n";
    return message;
}
TEST_SUITE_BEGIN("LuduvoSchemas");

TEST_CASE("extracts_registrations_without_treating_readonly_handle_writes_as_declarations")
{
    const auto schema = extractScriptSchema("test.server.luau", R"(
        local ET = game.EventTable
        local fields = {{"message", ET.String}, {"speaker", ET.Instance}}
        if false then ET("Chat", ET.ToClients, fields) end
        local h = handles
        h.Button = self:FindFirstChild("Button")
        handles["Other Button"] = self
        local function unrelated() end
        local game = {EventTable = unrelated}
        game.EventTable("NotBuiltin", nil, fields)
    )");
    REQUIRE(schema.events.size() == 1);
    CHECK(schema.events[0].name == "Chat");
    CHECK(schema.events[0].direction == "ToClients");
    REQUIRE(schema.events[0].fields.size() == 2);
    CHECK(schema.events[0].fields[0].type == "string");
    CHECK(schema.events[0].fields[1].type == "Instance");
    CHECK(schema.handles.empty());
}

TEST_CASE("handle_reads_support_immutable_aliases_and_ignore_writes_or_shadowed_globals")
{
    const auto schema = extractScriptSchema("test.luau", R"(
        local h = handles
        local key = "Bracket"
        print(h.Dot, h[key])
        handles.Button.Name = "changed"
        handles.NotADeclaration = self
        handles["NotADeclarationEither"] = self
        local mutable = handles
        mutable = {}
        print(mutable.Unrelated)
        do local handles = {}; print(handles.Shadowed) end
    )");
    std::set<std::string> names;
    for (const auto& field : schema.handles)
    {
        names.insert(field.name);
        CHECK(field.type == "Instance");
        CHECK_FALSE(field.expression.empty());
    }
    CHECK(names == std::set<std::string>{"Dot", "Bracket", "Button"});
}

TEST_CASE("mutable_aliases_are_not_assumed_to_still_reference_the_api")
{
    const auto schema = extractScriptSchema("test.luau", R"(
        local ET = game.EventTable
        local function mutate() ET = print end
        ET("Wrong", nil, {{"value", game.EventTable.String}})
    )");
    CHECK(schema.events.empty());
}

TEST_CASE("only_complete_inline_column_lists_close_event_schemas")
{
    ProjectSchemas project;
    project.scripts["a"] = extractScriptSchema("a", R"(
        local E = game.EventTable
        local fields = {{"message", E.String}}
        E("Aliased", E.ToClients, fields)
        E("Computed", E.ToClients, getFields())
        E("Partial", E.ToClients, {{"message", E.String}, getField()})
        E("Closed", E.ToClients, {{"message", E.String}})
        E("Empty", E.ToClients, {})
        E("Keyed", E.ToClients, {[1] = {"message", E.String}})
    )");
    REQUIRE(project.scripts.at("a").events.size() == 6);
    CHECK_FALSE(hasCompleteEventSchema(project, {"Keyed"}));
    CHECK_FALSE(hasCompleteEventSchema(project, {"Aliased"}));
    CHECK_FALSE(hasCompleteEventSchema(project, {"Computed"}));
    CHECK_FALSE(hasCompleteEventSchema(project, {"Partial"}));
    CHECK(hasCompleteEventSchema(project, {"Closed", "Empty"}));
    CHECK_FALSE(hasCompleteEventSchema(project, {"Closed", "Missing"}));
    project.scripts["b"] = extractScriptSchema("b", "game.EventTable('Closed',game.EventTable.ToClients,getFields())");
    CHECK_FALSE(hasCompleteEventSchema(project, {"Closed"}));
}

TEST_CASE("handle_contributions_are_shared_only_by_matching_attachments")
{
    ProjectSchemas project;
    project.scripts["a"] = extractScriptSchema("a", "print(handles.Button)");
    project.scripts["b"] = extractScriptSchema("b", R"(local h = handles; print(h["Panel"]))");
    project.scripts["c"] = extractScriptSchema("c", "print(handles.Unrelated)");
    project.attachments["a"] = {{1, 100, "Menu"}};
    project.attachments["b"] = {{1, 100, "Menu"}};
    project.attachments["c"] = {{2, 101, "Other"}};
    CHECK(handleSchemaFields(project, "a").size() == 2);
    CHECK(handleSchemaFields(project, "missing").empty());
}

TEST_CASE("schema_cache_round_trips_without_analysis_graphs")
{
    TempDir temp("schema_cache");
    ProjectSchemas project;
    project.projectId = "project";
    project.scripts["a"] = extractScriptSchema("a", "game.EventTable('Chat', game.EventTable.ToClients, {{'message',game.EventTable.String}})");
    project.scripts["a"].builtin = true;
    REQUIRE(publishProjectSchemas(temp.path(), project));
    const auto loaded = loadProjectSchemas(temp.path(), "project");
    REQUIRE(loaded);
    CHECK(eventSchemaNames(*loaded).count("Chat") == 1);
    CHECK(loaded->scripts.at("a").builtin);
    CHECK_FALSE(loadProjectSchemas(temp.path(), "other"));
    for (const int version : {1, 2, 3})
    {
        project.formatVersion = version;
        REQUIRE(publishProjectSchemas(temp.path(), project));
        CHECK_FALSE(loadProjectSchemas(temp.path(), "project"));
    }
}

TEST_CASE("native_schema_reducer_merges_arbitrary_known_names_and_preserves_methods")
{
    auto project = std::make_shared<ProjectSchemas>();
    project->scripts["a"] = extractScriptSchema("a", R"(
        game.EventTable("Chat", game.EventTable.ToClients, {{"value",game.EventTable.String},{"message",game.EventTable.String}})
        game.EventTable("Equip", game.EventTable.ToServer, {{"value",game.EventTable.I32}})
        game.EventTable("Empty", game.EventTable.ToClients, {})
        game.EventTable("Open", game.EventTable.ToClients, getFields())
    )");
    std::shared_ptr<SchemaTypes> types;
    LuduvoFixture fixture("native_schema_types");
    fixture.workspace.frontend.prepareModuleScope = [&](const Luau::ModuleName&, const Luau::ScopePtr& scope, bool)
    {
        if (!types)
            types = std::make_shared<SchemaTypes>(project, scope->parent, fixture.workspace.frontend.builtinTypes);
        types->bind(scope, "a");
    };
    const auto uri = fixture.newDocument("types.server.luau", R"(
        --!strict
        local name: "Chat" | "Equip" = nil :: any
        local event = game.EventTable(name)
        local value: string | number = event.value[1]
        local base: EventTable = event
        local column: EventColumn = event.value
        if event.message then local text: string = event.message[1] end
        local key: "value" | "message" = "value"
        local dynamicColumn = event[key]
        if dynamicColumn then local dynamicValue: string | number = dynamicColumn[1] end
        event:Push("hello")
    )");
    Luau::FrontendOptions options;
    options.retainFullTypeGraphs = true;
    const auto name = fixture.workspace.fileResolver.getModuleName(uri);
    const auto result = fixture.workspace.frontend.check(name, options);
    LUAU_LSP_REQUIRE_NO_ERRORS(result);
    const auto source = fixture.workspace.frontend.getSourceModule(name);
    const auto module = fixture.workspace.getModule(name);
    const auto declaration = source->root->body.data[2]->as<Luau::AstStatLocal>();
    REQUIRE(declaration);
    const auto index = declaration->values.data[0]->as<Luau::AstExprIndexExpr>();
    REQUIRE(index);
    const auto member = index->expr->as<Luau::AstExprIndexName>();
    REQUIRE(member);
    const auto eventType = module->astTypes.find(member->expr);
    REQUIRE(eventType);
    REQUIRE(SchemaTypes::metadata(*eventType));
    CHECK(luduvo::hover::getCheckability(*eventType) == luduvo::hover::TypeCheckability::Checkable);
    const auto bad = fixture.newDocument("bad.server.luau", R"(
        --!strict
        local name: "Chat" = "Chat"
        local event = game.EventTable(name)
        local invalid: number = event.message[1]
    )");
    const auto invalid = fixture.workspace.frontend.check(fixture.workspace.fileResolver.getModuleName(bad));
    LUAU_LSP_REQUIRE_ERROR_COUNT(1, invalid);
    const auto badName = fixture.newDocument("missing-column.server.luau", R"(
        --!strict
        local name: "Chat" = "Chat"
        local event = game.EventTable(name)
        print(event.NotAColumn)
        print(event["AlsoMissing"])
    )");
    const auto missingModule = fixture.workspace.fileResolver.getModuleName(badName);
    const auto missing = fixture.workspace.frontend.check(missingModule, options);
    INFO(getErrors(missing));
    LUAU_LSP_REQUIRE_NO_ERRORS(missing);
    LuduvoDiagnosticSettings settings;
    const auto diagnose = [&]()
    {
        return collectLuduvoDiagnostics({std::filesystem::u8path(badName.fsPath()), *fixture.workspace.frontend.getSourceModule(missingModule),
            fixture.workspace.getModule(missingModule), settings});
    };
    const auto diagnostics = diagnose();
    REQUIRE(diagnostics.size() == 2);
    CHECK(std::all_of(diagnostics.begin(), diagnostics.end(),
        [](const auto& diagnostic)
        {
            return diagnostic.ruleId == "event-table.unknown-column";
        }));
    CHECK(std::any_of(diagnostics.begin(), diagnostics.end(),
        [](const auto& error)
        {
            return error.location.begin.line == 4;
        }));
    CHECK(std::any_of(diagnostics.begin(), diagnostics.end(),
        [](const auto& error)
        {
            return error.location.begin.line == 5;
        }));
    CHECK(std::all_of(diagnostics.begin(), diagnostics.end(),
        [](const auto& diagnostic)
        {
            return diagnostic.severity == LuduvoDiagnosticSeverity::Error;
        }));
    settings.categories["event-table.unknown-column"] = LuduvoDiagnosticRuleLevel::Warning;
    const auto warnings = diagnose();
    CHECK(std::all_of(warnings.begin(), warnings.end(),
        [](const auto& diagnostic)
        {
            return diagnostic.severity == LuduvoDiagnosticSeverity::Warning;
        }));
    settings.categories["event-table.unknown-column"] = LuduvoDiagnosticRuleLevel::Off;
    CHECK(diagnose().empty());
    settings.categories.clear();
    const auto suppressed = fixture.newDocument("suppressed-column.server.luau", R"(--!nolint luduvo/event-table.unknown-column
        local name: "Chat" = "Chat"
        local event = game.EventTable(name)
        print(event.NotAColumn)
    )");
    const auto suppressedModule = fixture.workspace.fileResolver.getModuleName(suppressed);
    LUAU_LSP_REQUIRE_NO_ERRORS(fixture.workspace.frontend.check(suppressedModule, options));
    CHECK(collectLuduvoDiagnostics({std::filesystem::u8path(suppressed.fsPath()), *fixture.workspace.frontend.getSourceModule(suppressedModule),
                                       fixture.workspace.getModule(suppressedModule), settings})
            .empty());
    const auto open = fixture.newDocument("open.server.luau", R"(
        local name: "Open" = "Open"
        local event = game.EventTable(name)
        print(event.NotKnownYet[1])
    )");
    const auto openModule = fixture.workspace.fileResolver.getModuleName(open);
    LUAU_LSP_REQUIRE_NO_ERRORS(fixture.workspace.frontend.check(openModule, options));
    settings.categories.clear();
    CHECK(collectLuduvoDiagnostics({std::filesystem::u8path(open.fsPath()), *fixture.workspace.frontend.getSourceModule(openModule),
                                       fixture.workspace.getModule(openModule), settings})
            .empty());
    const auto empty = fixture.newDocument("empty.server.luau", R"(
        local name: "Empty" = "Empty"
        local event = game.EventTable(name)
        print(event.count)
        event:Push()
        print(event.NotAColumn)
    )");
    const auto emptyModule = fixture.workspace.fileResolver.getModuleName(empty);
    LUAU_LSP_REQUIRE_NO_ERRORS(fixture.workspace.frontend.check(emptyModule, options));
    const auto emptyDiagnostics = collectLuduvoDiagnostics({std::filesystem::u8path(empty.fsPath()),
        *fixture.workspace.frontend.getSourceModule(emptyModule), fixture.workspace.getModule(emptyModule), settings});
    REQUIRE(emptyDiagnostics.size() == 1);
    CHECK(emptyDiagnostics.front().ruleId == "event-table.unknown-column");
}


TEST_CASE("event_table_keys_resolves_known_names_and_preserves_incomplete_fallback")
{
    auto project = std::make_shared<ProjectSchemas>();
    project->scripts["a"] = extractScriptSchema("a", R"(
        game.EventTable("Chat", game.EventTable.ToClients, {{"message",game.EventTable.String}})
        game.EventTable("Equip", game.EventTable.ToServer, {{"slot",game.EventTable.I32}})
        game.EventTable("Open", game.EventTable.ToClients, getFields())
        game.EventTable("Empty", game.EventTable.ToClients, {})
    )");
    std::shared_ptr<SchemaTypes> types;
    LuduvoFixture fixture("event_keys");
    fixture.workspace.frontend.prepareModuleScope = [&](const Luau::ModuleName&, const Luau::ScopePtr& scope, bool)
    {
        if (!types)
            types = std::make_shared<SchemaTypes>(project, scope->parent, fixture.workspace.frontend.builtinTypes);
        types->bind(scope, "a");
    };
    const auto valid = fixture.newDocument("valid.server.luau", R"(--!strict
        local key: EventTableKeys<"Chat"> = "message"
        local combined: EventTableKeys<"Chat" | "Equip"> = "slot"
        local open: EventTableKeys<"Open"> = "arbitrary"
        local unknown: EventTableKeys<"Missing"> = "arbitrary"
    )");
    LUAU_LSP_REQUIRE_NO_ERRORS(fixture.workspace.frontend.check(fixture.workspace.fileResolver.getModuleName(valid)));
    const auto invalid = fixture.newDocument("invalid.server.luau", R"(--!strict
        local key: EventTableKeys<"Chat"> = "typo"
        local empty: EventTableKeys<"Empty"> = "anything"
    )");
    LUAU_LSP_REQUIRE_ERROR_COUNT(2, fixture.workspace.frontend.check(fixture.workspace.fileResolver.getModuleName(invalid)));
    const auto generated = generateProjectTypes(*project);
    CHECK(generated.find("export type EventTableKeys<Name>") != std::string::npos);
    CHECK(generated.find("[\"Chat\"]: \"message\"") != std::string::npos);
}

TEST_CASE("export_publisher_is_reference_driven_and_preserves_user_files")
{
    TempDir temp("schema_exports");
    const auto directory = temp.path();
    temp.write_child("project/project.json", R"({"name":"Test","assets":{"main":{"type":"script","path":"scripts/main.server.luau"}}})");
    temp.write_child("project/scripts/main.server.luau", R"(local Types = require("./types/ProjectTypes")
        game.EventTable("Chat",game.EventTable.ToClients,{{"message",game.EventTable.String}}))");
    const auto root = (std::filesystem::u8path(directory) / "project").generic_u8string();
    ContextStore store((std::filesystem::u8path(directory) / "state").generic_u8string());
    const auto added = store.updateProjectFiles(root);
    REQUIRE(added.success);
    ClientLuduvoConfiguration config;
    config.dataDirectory = directory;
    updateSchemaDatabase(store, config);
    const auto generated = Luau::FileUtils::readFile(projectTypesPath(root));
    REQUIRE(generated);
    CHECK(generated->find("export type EventTableFor") != std::string::npos);
    const auto initial = loadProjectSchemas(store.directory(), *added.projectId);
    REQUIRE(initial);
    CHECK(initial->exportState == "up to date");
    temp.write_child("project/scripts/types/ProjectTypes.luau", "return {userOwned = true}");
    updateSchemaDatabase(store, config);
    CHECK(Luau::FileUtils::readFile(projectTypesPath(root)) == "return {userOwned = true}");
    const auto conflict = loadProjectSchemas(store.directory(), *added.projectId);
    REQUIRE(conflict);
    CHECK(conflict->exportError.find("conflict:") == 0);
}

TEST_CASE("noncall_binding_targets_round_trip_and_resolve_separately")
{
    auto config = nlohmann::json::parse(R"({"bindings":[
        {"symbol":"MyTable","kind":"resource","group":"prefab","target":{"kind":"member"}},
        {"symbol":"MyType","kind":"resource","group":"component","target":{"kind":"typeArgument","index":2}}
    ]})")
                      .get<ClientLuduvoConfiguration>();
    LuduvoBindingRegistry bindings;
    bindings.configure(config);
    CHECK(bindings.configurationWarnings().empty());
    CHECK(bindings.resolveResourceTargets("MyTable", LuduvoBindingTargetKind::Member) == std::vector<std::string>{"prefab"});
    CHECK(bindings.resolveResourceTargets("MyType", LuduvoBindingTargetKind::TypeArgument, 1) == std::vector<std::string>{"component"});
    CHECK(bindings.resolveResourceTargets("MyType", LuduvoBindingTargetKind::TypeArgument, 0).empty());
}

TEST_CASE("type_argument_binding_indices_default_to_one_and_reject_zero")
{
    auto config = nlohmann::json::parse(R"({"bindings":[
        {"symbol":"First","kind":"resource","group":"component","target":{"kind":"typeArgument"}},
        {"symbol":"Invalid","kind":"resource","group":"component","target":{"kind":"typeArgument","index":0}}
    ]})").get<ClientLuduvoConfiguration>();
    CHECK(config.bindings.front().target->index == 1);
    LuduvoBindingRegistry bindings;
    bindings.configure(config);
    CHECK(bindings.configurationWarnings().size() == 1);
    CHECK(bindings.resolveResourceTargets("First", LuduvoBindingTargetKind::TypeArgument, 0) == std::vector<std::string>{"component"});
    CHECK(bindings.resolveResourceTargets("First", LuduvoBindingTargetKind::TypeArgument, 1).empty());
    CHECK(bindings.resolveResourceTargets("Invalid", LuduvoBindingTargetKind::TypeArgument, 0).empty());
    const nlohmann::json saved = config;
    CHECK(saved["bindings"][0]["target"]["index"] == 1);
}

TEST_CASE("reporting_log_and_ui_inheritance_are_independent")
{
    const logging::Configuration config = nlohmann::json::parse(R"({"default":{"logs":"Info","ui":false},"categories":{
        "context":{"ui":true},"context.schemas":{"logs":"Verbose"},"context.exports":{"ui":false}}})");
    CHECK(config.uiEnabled("context.schemas"));
    CHECK_FALSE(config.uiEnabled("context.exports"));
    CHECK_FALSE(config.uiEnabled("bindings.resources"));
    CHECK(config.categories.at("context.schemas") == logging::Level::Verbose);
}

TEST_CASE("portable_module_is_valid_and_documents_broad_indexer_limit")
{
    LuduvoFixture fixture("generated_schema_module");
    ProjectSchemas project;
    project.projectId = "test";
    project.directory = fixture.tempDir.path();
    project.scripts["a"] = extractScriptSchema("a", R"(
        game.EventTable("Chat",game.EventTable.ToClients,{{"message",game.EventTable.String}})
    )");
    const auto generated = generateProjectTypes(project);
    Luau::Allocator allocator;
    Luau::AstNameTable names(allocator);
    const auto parsed = Luau::Parser::parse(generated.data(), generated.size(), names, allocator);
    REQUIRE(parsed.errors.empty());
    fixture.tempDir.write_child("types/ProjectTypes.luau", generated);
    const auto uri = fixture.newDocument("consumer.server.luau", R"(
        --!strict
        local LSP = require("./types/ProjectTypes")
        local event: LSP.EventTableFor<"Chat"> = nil :: any
        local value: string = event.message[1]
        local ordinary: EventTable = event
        local key: LSP.EventTableKeys<"Chat"> = "message"
    )");
    const auto result = fixture.workspace.checkStrict(fixture.workspace.fileResolver.getModuleName(uri), nullptr);
    LUAU_LSP_REQUIRE_NO_ERRORS(result);
    const auto bad = fixture.newDocument("invalid.server.luau", R"(
        --!strict
        local LSP = require("./types/ProjectTypes")
        local event: LSP.EventTableFor<"Chat"> = nil :: any
        local invalid: number = event.message[1]
    )");
    const auto invalid = fixture.workspace.checkStrict(fixture.workspace.fileResolver.getModuleName(bad), nullptr);
    // Portable Luau intersections retain the base column's any indexer.
    // Native checking is tested separately through SchemaService.
    LUAU_LSP_REQUIRE_NO_ERRORS(invalid);
    const auto badKey = fixture.newDocument("bad-key.server.luau", R"(--!strict
        local LSP = require("./types/ProjectTypes")
        local invalid: LSP.EventTableKeys<"Chat"> = "typo"
    )");
    LUAU_LSP_REQUIRE_ERROR_COUNT(1, fixture.workspace.checkStrict(fixture.workspace.fileResolver.getModuleName(badKey), nullptr));
    const auto client = fixture.newDocument("consumer.client.luau", R"(
        --!strict
        local LSP = require("./types/ProjectTypes")
        local event: LSP.EventTableFor<"Chat", EventTable, EventColumn> = nil :: any
        local value: string = event.message[1]
        local ordinary: EventTable = event
        local key: LSP.EventTableKeys<"Chat"> = "message"
    )");
    const auto clientResult = fixture.workspace.checkStrict(fixture.workspace.fileResolver.getModuleName(client), nullptr);
    LUAU_LSP_REQUIRE_NO_ERRORS(clientResult);
}

TEST_CASE("autoimport_and_family_removal_apply_as_valid_source_edits")
{
    LuduvoFixture fixture("schema_import_edits");
    const std::string text = "--!strict\nlocal value: Events<\"Chat\"> = nil :: any\n";
    const auto uri = fixture.newDocument("consumer.server.luau", text);
    const auto name = fixture.workspace.fileResolver.getModuleName(uri);
    fixture.workspace.frontend.parse(name);
    const auto source = fixture.workspace.frontend.getSourceModule(name);
    const auto document = fixture.workspace.fileResolver.getTextDocument(uri);
    REQUIRE(source);
    REQUIRE(document);
    const SchemaTypeImports imports(*source, *document, fixture.tempDir.path());
    const auto item = imports.completion(exportedSchemaFeatures().front(), uri.fsPath());
    const auto imported = applyEdit(text, item.additionalTextEdits);
    CHECK(imported.find("local LSP = require(\"./scripts/types/ProjectTypes\")") != std::string::npos);
    CHECK(imported.find("type Events<Name> = LSP.EventTableFor<Name, EventTable, EventColumn>") != std::string::npos);
    const auto importedUri = fixture.newDocument("with-import.server.luau", imported);
    const auto importedName = fixture.workspace.fileResolver.getModuleName(importedUri);
    fixture.workspace.frontend.parse(importedName);
    const auto importedSource = fixture.workspace.frontend.getSourceModule(importedName);
    const auto importedDocument = fixture.workspace.fileResolver.getTextDocument(importedUri);
    const SchemaTypeImports importedTypes(*importedSource, *importedDocument, fixture.tempDir.path());
    std::vector<std::string> warnings;
    const auto removed = applyEdit(imported, importedTypes.remove({"event-table"}, warnings));
    CHECK(warnings.empty());
    CHECK(removed.find("require") == std::string::npos);
    CHECK(removed.find("local value: EventTable = nil :: any") != std::string::npos);
    Luau::Allocator allocator;
    Luau::AstNameTable names(allocator);
    CHECK(Luau::Parser::parse(removed.data(), removed.size(), names, allocator).errors.empty());
}

TEST_CASE("handle_member_completion_shares_read_hints_only_with_scripts_on_the_same_instance")
{
    LuduvoFixture fixture("handle_completion_attachments");
    fixture.tempDir.write_child("project.json", R"({"name":"Handles","assets":{
        "producer":{"type":"script","path":"scripts/producer.server.luau"},
        "consumer":{"type":"script","path":"scripts/consumer.server.luau"},
        "other":{"type":"script","path":"scripts/other.server.luau"}}})");
    fixture.tempDir.write_child("scripts/producer.server.luau", "print(handles.Testing)");
    fixture.tempDir.write_child("scripts/consumer.server.luau", "");
    fixture.tempDir.write_child("scripts/other.server.luau", "print(handles.Unrelated)");
    auto config = fixture.config.platform.luduvo;
    config.context.enabled = true;
    config.directories.context = fixture.tempDir.path() + "/state";
    ContextStore store(config.directories.context);
    const auto added = store.updateProjectFiles(fixture.tempDir.path());
    REQUIRE(added.success);
    REQUIRE(added.projectId);
    WorldDumpSnapshot snapshot;
    WorldInstance shared;
    shared.nodeId = 1;
    shared.scripts = {"scripts/producer.server.luau", "scripts/consumer.server.luau"};
    WorldInstance other;
    other.nodeId = 2;
    other.scripts = {"scripts/other.server.luau"};
    snapshot.instances = {shared, other};
    REQUIRE(store.updateWorldForProject(snapshot, *added.projectId).success);
    LuduvoBindingRegistry bindings;
    bindings.configure(config);
    LuduvoResourceCatalog resources;
    resources.configure(config);
    SchemaService service(fixture.workspace, config, bindings, resources);
    fixture.workspace.frontend.prepareModuleScope = [&](const Luau::ModuleName& name, const Luau::ScopePtr& scope, bool)
    {
        service.bind(name, scope);
    };
    const auto uri = fixture.newDocument("scripts/consumer.server.luau", "");
    for (const auto text : {"handles.|", "local h = handles; h[\"|\"]"})
    {
        const auto [content, marker] = sourceWithMarker(text);
        Luau::LanguageServer::updateDocument(fixture.workspace, uri, content);
        const auto name = fixture.workspace.fileResolver.getModuleName(uri);
        {
            const auto overlay = service.prepare(name);
            fixture.workspace.frontend.check(name);
        }
        const auto doc = fixture.workspace.fileResolver.getTextDocument(uri);
        const auto source = fixture.workspace.frontend.getSourceModule(name);
        const auto position = doc->convertPosition(marker);
        std::vector<lsp::CompletionItem> items;
        service.complete(*doc, *source, position, Luau::findAncestryAtPositionForAutocomplete(*source, position), items);
        CHECK(std::any_of(items.begin(), items.end(),
            [](const auto& item)
            {
                return item.label == "Testing";
            }));
        CHECK(std::none_of(items.begin(), items.end(),
            [](const auto& item)
            {
                return item.label == "Unrelated";
            }));
    }
}

TEST_CASE("project_service_specializes_cross_script_calls_and_restores_literal_ast")
{
    LuduvoFixture fixture("schema_service_integration");
    fixture.tempDir.write_child("project.json", R"({"name":"Test","assets":{
        "registration":{"type":"script","path":"scripts/register.server.luau"},
        "consumer":{"type":"script","path":"scripts/consumer.server.luau"}}})");
    fixture.tempDir.write_child(
        "scripts/register.server.luau", "game.EventTable('Chat', game.EventTable.ToClients, {{'message', game.EventTable.String}})");
    fixture.tempDir.write_child("scripts/consumer.server.luau", "");
    auto config = fixture.config.platform.luduvo;
    config.context.enabled = true;
    config.directories.context = fixture.tempDir.path() + "/state";
    ContextStore store(config.directories.context);
    const auto added = store.updateProjectFiles(fixture.tempDir.path());
    REQUIRE(added.success);
    REQUIRE(added.projectId);
    fixture.tempDir.write_child("content/current", "11");
    fixture.tempDir.write_child(
        "content/versions/11/scripts/Core.server.luau", "game.EventTable('CoreChat',game.EventTable.ToClients,{{'message',game.EventTable.String}})");
    WorldDumpSnapshot snapshot;
    WorldInstance core;
    core.nodeId = 1;
    core.scripts = {"core://scripts/Core.server.luau"};
    snapshot.instances.push_back(core);
    REQUIRE(store.updateWorldForProject(snapshot, *added.projectId).success);
    LuduvoBindingRegistry bindings;
    bindings.configure(config);
    LuduvoResourceCatalog resources;
    resources.configure(config);
    SchemaService service(fixture.workspace, config, bindings, resources);
    fixture.workspace.frontend.prepareModuleScope = [&](const Luau::ModuleName& name, const Luau::ScopePtr& scope, bool)
    {
        service.bind(name, scope);
    };
    const auto uri = fixture.newDocument("scripts/consumer.server.luau", R"(
        --!strict
        local events = game.EventTable("Chat")
        local text: string = events.message[1]
    )");
    const auto name = fixture.workspace.fileResolver.getModuleName(uri);
    {
        const auto overlay = service.prepare(name);
        const auto result = fixture.workspace.frontend.check(name);
        LUAU_LSP_REQUIRE_NO_ERRORS(result);
    }
    struct StringArguments : Luau::AstVisitor
    {
        size_t found = 0;
        bool visit(Luau::AstExprCall* call) override
        {
            if (call->args.size == 1 && call->args.data[0]->is<Luau::AstExprConstantString>())
                ++found;
            return true;
        }
    } visitor;
    fixture.workspace.frontend.getSourceModule(name)->root->visit(&visitor);
    CHECK(visitor.found == 1);
    const auto view = service.inspect(uri.fsPath());
    REQUIRE_FALSE(view.is_null());
    CHECK(eventSchemaNames(view.get<ProjectSchemas>()).count("Chat") == 1);
    fixture.tempDir.write_child("scripts/types/ProjectTypes.luau", generateProjectTypes(view.get<ProjectSchemas>()));
    Luau::LanguageServer::updateDocument(fixture.workspace, uri, R"(
        --!strict
        local LSP = require("./types/ProjectTypes")
        type Events<Name> = LSP.EventTableFor<Name, EventTable, EventColumn>
        local event: Events<"Chat"> = game.EventTable("Chat")
        local invalid: number = event.message[1]
    )");
    {
        const auto overlay = service.prepare(name);
        struct Rewritten : Luau::AstVisitor
        {
            size_t count = 0;
            bool visit(Luau::AstType*) override
            {
                return true;
            }
            bool visit(Luau::AstTypeReference* type) override
            {
                if (std::string_view(type->name.value).find("@luduvo/schema/") == 0)
                    ++count;
                return true;
            }
        } rewritten;
        fixture.workspace.frontend.getSourceModule(name)->root->visit(&rewritten);
        REQUIRE(rewritten.count == 1);
        const auto result = fixture.workspace.frontend.check(name);
        LUAU_LSP_REQUIRE_ERROR_COUNT(1, result);
    }
    auto [completionText, marker] = sourceWithMarker("local event = game.EventTable(\"Ch|\")");
    Luau::LanguageServer::updateDocument(fixture.workspace, uri, completionText);
    {
        const auto overlay = service.prepare(name);
        Luau::FrontendOptions options;
        options.retainFullTypeGraphs = true;
        fixture.workspace.frontend.check(name, options);
    }
    const auto document = fixture.workspace.fileResolver.getTextDocument(uri);
    const auto source = fixture.workspace.frontend.getSourceModule(name);
    std::vector<lsp::CompletionItem> items;
    const auto position = document->convertPosition(marker);
    service.complete(*document, *source, position, Luau::findAncestryAtPositionForAutocomplete(*source, position), items);
    const auto item = std::find_if(items.begin(), items.end(),
        [](const auto& item)
        {
            return item.label == "Chat";
        });
    REQUIRE(item != items.end());
    CHECK(item->detail == "Event table");
    const auto coreItem = std::find_if(items.begin(), items.end(),
        [](const auto& entry)
        {
            return entry.label == "CoreChat";
        });
    REQUIRE(coreItem != items.end());
    CHECK(coreItem->detail == "Luduvo event table (built-in)");
    REQUIRE(coreItem->documentation);
    CHECK(coreItem->documentation->value.find("`built-in`") != std::string::npos);
    REQUIRE(coreItem->textEdit);
    CHECK(applyEdit(completionText, {*coreItem->textEdit}) == "local event = game.EventTable(\"CoreChat\")");
    REQUIRE(item->textEdit);
    CHECK(applyEdit(completionText, {*item->textEdit}) == "local event = game.EventTable(\"Chat\")");
    const auto producer = fixture.newDocument("scripts/register.server.luau", "");
    for (size_t revision = 0; revision < 6; ++revision)
    {
        const bool numeric = revision % 2 == 0;
        Luau::LanguageServer::updateDocument(fixture.workspace, producer,
            "game.EventTable('Chat',game.EventTable.ToClients,{{'message',game.EventTable." + std::string(numeric ? "I32" : "String") + "}})");
        Luau::LanguageServer::updateDocument(
            fixture.workspace, uri, "--!strict\nlocal events=game.EventTable('Chat')\nlocal value: number=events.message[1]");
        const auto overlay = service.prepare(name);
        const auto result = fixture.workspace.frontend.check(name);
        LUAU_LSP_REQUIRE_ERROR_COUNT((numeric ? 0 : 1), result);
    }

    for (const auto text :
        {"--!strict\nlocal key: EventTableK|", "--!strict\nlocal key: EventTableK|<\"Chat\">", "--!strict\nlocal key: EventTableK|<>"})
    {
        auto [content, cursor] = sourceWithMarker(text);
        Luau::LanguageServer::updateDocument(fixture.workspace, uri, content);
        {
            const auto overlay = service.prepare(name);
            fixture.workspace.frontend.check(name);
        }
        const auto doc = fixture.workspace.fileResolver.getTextDocument(uri);
        const auto src = fixture.workspace.frontend.getSourceModule(name);
        const auto pos = doc->convertPosition(cursor);
        lsp::CompletionItem raw;
        raw.label = "EventTableKeys";
        std::vector<lsp::CompletionItem> candidates{raw};
        service.complete(*doc, *src, pos, Luau::findAncestryAtPositionForAutocomplete(*src, pos), candidates);
        CHECK(std::none_of(candidates.begin(), candidates.end(),
            [](const auto& candidate)
            {
                return candidate.label == "EventTableKeys";
            }));
        const auto keys = std::find_if(candidates.begin(), candidates.end(),
            [](const auto& candidate)
            {
                return candidate.label == "EventKeys";
            });
        REQUIRE(keys != candidates.end());
        REQUIRE(keys->filterText);
        CHECK(keys->filterText->find("EventTableKeys") != std::string::npos);
        REQUIRE(keys->textEdit);
        auto edits = keys->additionalTextEdits;
        edits.push_back(*keys->textEdit);
        const auto completed = applyEdit(content, edits);
        CHECK(completed.find("local LSP = require(\"./types/ProjectTypes\")") != std::string::npos);
        CHECK(completed.find("type EventKeys<Name> = LSP.EventTableKeys<Name>") != std::string::npos);
        CHECK(completed.find("local key: EventKeys<") != std::string::npos);
        CHECK(completed.find("<><") == std::string::npos);
    }
}

TEST_CASE("removal_handles_nested_imports_runtime_uses_and_shadowed_generics_safely")
{
    LuduvoFixture fixture("schema_removal_safety");
    const std::vector<std::string> cases = {
        R"(do
            local Types = require("./scripts/types/ProjectTypes")
            local value: {entry: Types.EventTableFor<"Chat">} = nil :: any
        end)",
        R"(local Types = require("./scripts/types/ProjectTypes")
            local value: Types.EventTableFor<"Chat"> = nil :: any
            print(Types))",
        R"(local Types = require("./scripts/types/ProjectTypes")
            type Events<Name> = Types.EventTableFor<Name>
            local function f<Events>() local value: Events = nil :: any end)",
    };
    for (size_t index = 0; index < cases.size(); ++index)
    {
        const auto uri = fixture.newDocument("case" + std::to_string(index) + ".server.luau", cases[index]);
        const auto module = fixture.workspace.frontend.getSourceModule(fixture.workspace.fileResolver.getModuleName(uri));
        const auto document = fixture.workspace.fileResolver.getTextDocument(uri);
        SchemaTypeImports imports(*module, *document, fixture.tempDir.path());
        std::vector<std::string> warnings;
        const auto edits = imports.remove({"event-table"}, warnings);
        if (index == 2)
        {
            CHECK_FALSE(warnings.empty());
            CHECK(edits.empty());
            continue;
        }
        CHECK(warnings.empty());
        const auto edited = applyEdit(cases[index], edits);
        CHECK(edited.find("Types.EventTableFor") == std::string::npos);
        CHECK((edited.find("require") != std::string::npos) == (index == 1));
        Luau::Allocator allocator;
        Luau::AstNameTable names(allocator);
        CHECK(Luau::Parser::parse(edited.data(), edited.size(), names, allocator).errors.empty());
    }
}

TEST_CASE("truncated_source_observations_remain_valid_utf8_json")
{
    std::string source = "game.EventTable(\"Long\",game.EventTable.ToClients,{{\"message\",game.EventTable.String,\"";
    for (size_t index = 0; index < 1000; ++index)
        source += "\xF0\x9F\x8E\xAF";
    source += "\"}})";
    const auto schema = extractScriptSchema("test.luau", source);
    REQUIRE(schema.events.size() == 1);
    REQUIRE(schema.events[0].fields.size() == 1);
    CHECK_NOTHROW(nlohmann::json(schema).dump());
    CHECK(schema.events[0].fields[0].expression.find("[truncated]") != std::string::npos);
}

TEST_CASE("checkability_uses_the_nearest_external_indexer")
{
    using luduvo::hover::getCheckability;
    using luduvo::hover::TypeCheckability;
    Luau::BuiltinTypes builtins;
    Luau::TypeArena arena;
    Luau::ExternType base{"Column", {}, std::nullopt, std::nullopt, {}, nullptr, "", std::nullopt};
    base.indexer = Luau::TableIndexer{builtins.numberType, builtins.anyType};
    const auto parent = arena.addType(base);
    CHECK(getCheckability(parent) == TypeCheckability::UncheckableFields);
    auto derived = base;
    derived.parent = parent;
    derived.indexer = Luau::TableIndexer{builtins.numberType, builtins.stringType};
    CHECK(getCheckability(arena.addType(derived)) == TypeCheckability::Checkable);
    derived.indexer.reset();
    CHECK(getCheckability(arena.addType(derived)) == TypeCheckability::UncheckableFields);
}

TEST_CASE("schema_debug_source_and_export_paths_are_clickable_file_links")
{
    TempDir temp("schema_debug_links");
    auto project = std::make_shared<ProjectSchemas>();
    project->projectId = "test";
    project->projectName = "Test";
    const auto path = temp.path() + "/scripts/chat [draft] #1.server.luau";
    project->scripts[path] = extractScriptSchema(path, "\n\n game.EventTable('Chat',game.EventTable.ToClients,{{'message',game.EventTable.String}})");
    project->exportState = "up to date";
    project->exportPath = projectTypesPath(temp.path());
    project->inputSource = {"autosave", temp.path() + "/world.ldv.autosave", schemaTimeNow()};
    SchemaTypeMetadata metadata;
    metadata.project = project;
    metadata.names.insert("Chat");
    const auto rendered = renderSchemaDebug(metadata);
    CHECK(rendered.find("](<" + Uri::file(path).toString() + "#L3>)") != std::string::npos);
    CHECK(rendered.find("](<" + Uri::file(project->exportPath).toString() + ">)") != std::string::npos);
    CHECK(rendered.find("\\[draft\\]") != std::string::npos);
    CHECK(rendered.find(":3](<file:") != std::string::npos);
    CHECK(rendered.find("#L3>)") != std::string::npos);
    CHECK(rendered.find("Source: autosave") != std::string::npos);
    CHECK(rendered.find("Source written:") != std::string::npos);
}


TEST_CASE("handles_are_readonly_even_without_context_but_instances_remain_mutable")
{
    LuduvoFixture fixture("readonly_handles");
    const auto valid = fixture.newDocument("valid.server.luau", R"(--!strict
        local target = handles.Testing
        target.Name = "renamed"
        local h = handles
        h = handles
        local handles: {[string]: Instance} = {}
        handles.Testing = self
    )");
    LUAU_LSP_REQUIRE_NO_ERRORS(fixture.workspace.checkStrict(fixture.workspace.fileResolver.getModuleName(valid), nullptr));
    const auto invalid = fixture.newDocument("invalid.client.luau", R"(--!strict
        handles.Testing = self
        local key: string = "Testing"
        handles[key] = self
        local h = handles
        h.Testing = self
        local function mutate(h: typeof(handles)) h.Testing = nil :: any end
        local writable: {[string]: Instance} = handles
    )");
    LUAU_LSP_REQUIRE_ERROR_COUNT(5, fixture.workspace.checkStrict(fixture.workspace.fileResolver.getModuleName(invalid), nullptr));
}

TEST_SUITE_END();
