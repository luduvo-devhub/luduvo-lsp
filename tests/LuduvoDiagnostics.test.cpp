#include "doctest.h"
#include "Fixture.h"
#include "Platform/luduvo/diagnostics/LuduvoDiagnosticAdapter.hpp"
#include "Platform/luduvo/diagnostics/LuduvoDiagnostics.hpp"

#include <algorithm>
#include <string>

namespace
{
const lsp::Diagnostic* findDiagnostic(const std::vector<lsp::Diagnostic>& diagnostics, const std::string& code)
{
    auto result = std::find_if(diagnostics.begin(), diagnostics.end(),
        [&](const lsp::Diagnostic& diagnostic)
        {
            return diagnostic.code == std::optional<std::variant<std::string, int>>("luduvo/" + code);
        });
    return result == diagnostics.end() ? nullptr : &*result;
}

std::vector<lsp::Diagnostic> diagnosticsFor(LuduvoFixture& fixture, const Uri& document)
{
    return fixture.workspace.documentDiagnostics({{document}}, nullptr).items;
}
} // namespace

TEST_SUITE_BEGIN("LuduvoDiagnostics");

TEST_CASE("empty_world_query_reports_a_stable_luduvo_diagnostic")
{
    LuduvoFixture fixture("luduvo_empty_world_query");
    const Uri document = fixture.newDocument("query.server.luau", "local query = game.World.Query()");

    const auto diagnostics = diagnosticsFor(fixture, document);
    const lsp::Diagnostic* diagnostic = findDiagnostic(diagnostics, "world.empty-query");
    REQUIRE(diagnostic);
    CHECK_EQ(diagnostic->source, "Luduvo");
    CHECK_EQ(diagnostic->severity, lsp::DiagnosticSeverity::Error);
    CHECK_EQ(diagnostic->message, "game.World.Query requires at least one component name or Pair.");
    CHECK_EQ(diagnostic->range.start.line, 0);
    CHECK_EQ(diagnostic->range.start.character, 14);
}

TEST_CASE("empty_world_query_ignores_nonempty_and_shadowed_calls")
{
    LuduvoFixture fixture("luduvo_valid_world_queries");
    const Uri document = fixture.newDocument("query.server.luau", R"(
        local query = game.World.Query("Position")
        local game = { World = { Query = function() end } }
        game.World.Query()
    )");

    CHECK_FALSE(findDiagnostic(diagnosticsFor(fixture, document), "world.empty-query"));
}

TEST_CASE("minimum_argument_rules_match_global_paths_and_named_type_members")
{
    LuduvoFixture fixture("luduvo_minimum_arguments_symbols");
    const Uri document = fixture.newDocument("query.server.luau", R"(
        game.World:Query()
        local world = game.World
        world.Query()
        world:Query()
        world:Query("Position")
    )");

    const auto diagnostics = diagnosticsFor(fixture, document);
    const size_t emptyQueryDiagnostics = std::count_if(diagnostics.begin(), diagnostics.end(),
        [](const lsp::Diagnostic& diagnostic)
        {
            return diagnostic.code == std::optional<std::variant<std::string, int>>("luduvo/world.empty-query");
        });
    CHECK_EQ(emptyQueryDiagnostics, 3);
}

TEST_CASE("workspace_diagnostics_resolve_luduvo_calls_through_local_aliases")
{
    ClientConfiguration config = Luau::LanguageServer::defaultTestClientConfiguration();
    config.diagnostics.workspace = true;
    LuduvoFixture fixture("luduvo_workspace_diagnostic_alias", config);
    fixture.tempDir.write_child("query.server.luau", "local world = game.World\nworld.Query()");

    const lsp::WorkspaceDiagnosticReport report = fixture.workspace.workspaceDiagnostics({});
    REQUIRE_EQ(report.items.size(), 1);
    CHECK(findDiagnostic(report.items[0].items, "world.empty-query"));
}

TEST_CASE("call_rules_do_not_match_user_types_with_luduvo_names")
{
    LuduvoFixture fixture("luduvo_call_rule_user_type");
    const Uri document = fixture.newDocument("query.server.luau", R"(
        type World = { Query: () -> () }
        local world: World = { Query = function() end }
        world.Query()
    )");

    CHECK_FALSE(findDiagnostic(diagnosticsFor(fixture, document), "world.empty-query"));
}

TEST_CASE("global_self_is_rejected_only_inside_autorun_directories")
{
    LuduvoFixture fixture("luduvo_autorun_self");
    const Uri autorun = fixture.newDocument("scripts/autorun/start.client.luau", "print(self.Name)");
    const Uri attached = fixture.newDocument("scripts/start.client.luau", "print(self.Name)");
    const Uri localSelf = fixture.newDocument("scripts/autorun/local.client.luau", "local self = { Name = 'safe' }\nprint(self.Name)");

    const auto autorunDiagnostics = diagnosticsFor(fixture, autorun);
    const lsp::Diagnostic* diagnostic = findDiagnostic(autorunDiagnostics, "script.autorun-self-access");
    REQUIRE(diagnostic);
    CHECK_EQ(diagnostic->severity, lsp::DiagnosticSeverity::Error);
    CHECK_EQ(diagnostic->message, "'self' is nil in scripts inside an autorun directory.");
    CHECK_FALSE(findDiagnostic(diagnosticsFor(fixture, attached), "script.autorun-self-access"));
    CHECK_FALSE(findDiagnostic(diagnosticsFor(fixture, localSelf), "script.autorun-self-access"));
}

TEST_CASE("assigning_global_self_in_an_autorun_script_is_not_a_self_access")
{
    LuduvoFixture fixture("luduvo_autorun_global_self_assignment");
    const Uri document = fixture.newDocument("scripts/autorun/start.server.luau", "self = { Name = 'assigned' }");

    CHECK_FALSE(findDiagnostic(diagnosticsFor(fixture, document), "script.autorun-self-access"));
}

TEST_CASE("autorun_self_member_writes_and_assignment_value_reads_still_access_self")
{
    LuduvoFixture fixture("luduvo_autorun_self_assignment_reads");
    const Uri memberWrite = fixture.newDocument("scripts/autorun/member.server.luau", "self.Name = 'write'");
    const Uri valueRead = fixture.newDocument("scripts/autorun/value.server.luau", "self = self or {}");

    CHECK(findDiagnostic(diagnosticsFor(fixture, memberWrite), "script.autorun-self-access"));
    CHECK(findDiagnostic(diagnosticsFor(fixture, valueRead), "script.autorun-self-access"));
}

TEST_CASE("luduvo_diagnostics_can_be_disabled_globally_or_by_rule")
{
    SUBCASE("all Luduvo diagnostics")
    {
        ClientConfiguration config = Luau::LanguageServer::defaultTestClientConfiguration();
        config.platform.luduvo.diagnostics.defaultValue = "off";
        LuduvoFixture fixture("luduvo_diagnostics_disabled", config);
        CHECK_FALSE(fixture.workspace.platform->diagnosticsRequireFullTypeGraphs());
        const Uri document = fixture.newDocument("scripts/autorun/start.server.luau", "print(self)\ngame.World.Query()");

        const auto diagnostics = diagnosticsFor(fixture, document);
        CHECK_FALSE(findDiagnostic(diagnostics, "script.autorun-self-access"));
        CHECK_FALSE(findDiagnostic(diagnostics, "world.empty-query"));
    }

    SUBCASE("one rule by its public identifier")
    {
        ClientConfiguration config = Luau::LanguageServer::defaultTestClientConfiguration();
        config.platform.luduvo.diagnostics.categories = {{"world.empty-query", "off"}};
        LuduvoFixture fixture("luduvo_diagnostic_rule_disabled", config);
        const Uri document = fixture.newDocument("scripts/autorun/start.server.luau", "print(self)\ngame.World.Query()");

        const auto diagnostics = diagnosticsFor(fixture, document);
        CHECK(findDiagnostic(diagnostics, "script.autorun-self-access"));
        CHECK_FALSE(findDiagnostic(diagnostics, "world.empty-query"));
    }
}

TEST_CASE("luduvo_diagnostic_configuration_round_trips")
{
    const json input = json::parse(R"({
        "platform": {"luduvo": {"diagnostics": {
            "default": "hint",
            "categories": {
                "script.autorun-self-access": "warning",
                "world.empty-query": "off"
            }
        }}}
    })");

    const ClientConfiguration config = input.get<ClientConfiguration>();
    CHECK_EQ(config.platform.luduvo.diagnostics.defaultValue.value(), "hint");
    CHECK_EQ(config.platform.luduvo.diagnostics.categories.at("script.autorun-self-access"), "warning");
    CHECK_EQ(config.platform.luduvo.diagnostics.categories.at("world.empty-query"), "off");

    const json output = config;
    CHECK_FALSE(output["platform"]["luduvo"]["diagnostics"].contains("enabled"));
    CHECK_EQ(output["platform"]["luduvo"]["diagnostics"]["default"], "hint");
    CHECK_EQ(output["platform"]["luduvo"]["diagnostics"]["categories"]["script.autorun-self-access"], "warning");
    CHECK_EQ(output["platform"]["luduvo"]["diagnostics"]["categories"]["world.empty-query"], "off");
}

TEST_CASE("diagnostic_categories_override_global_defaults_and_default_resets_to_registered_severity")
{
    ClientConfiguration config = Luau::LanguageServer::defaultTestClientConfiguration();
    config.platform.luduvo.diagnostics.defaultValue = "off";
    config.platform.luduvo.diagnostics.categories = {{"world", "hint"}, {"world.empty-query", "default"}};
    LuduvoFixture fixture("diagnostic_categories", config);
    CHECK(fixture.workspace.platform->diagnosticsRequireFullTypeGraphs());
    const auto document = fixture.newDocument("scripts/autorun/start.server.luau", "print(self)\ngame.World.Query()");
    const auto diagnostics = diagnosticsFor(fixture, document);
    CHECK_FALSE(findDiagnostic(diagnostics, "script.autorun-self-access"));
    const auto query = findDiagnostic(diagnostics, "world.empty-query");
    REQUIRE(query);
    CHECK(query->severity == lsp::DiagnosticSeverity::Error);
    const auto resolved = resolveLuduvoDiagnosticConfiguration(config.platform.luduvo.diagnostics);
    CHECK(resolved.warnings.empty());

    config.platform.luduvo.diagnostics.categories.erase("world.empty-query");
    LuduvoFixture inherited("diagnostic_parent_category", config);
    const auto inheritedDocument = inherited.newDocument("query.server.luau", "game.World.Query()");
    const auto inheritedDiagnostics = diagnosticsFor(inherited, inheritedDocument);
    const auto inheritedQuery = findDiagnostic(inheritedDiagnostics, "world.empty-query");
    REQUIRE(inheritedQuery);
    CHECK(inheritedQuery->severity == lsp::DiagnosticSeverity::Hint);
}

TEST_CASE("legacy_diagnostic_settings_are_rejected_and_nolint_categories_are_not_rule_ids")
{
    CHECK_THROWS(json::parse(R"({"rules":{"empty-world-query":"off"}})").get<ClientLuduvoDiagnosticsConfiguration>());
    CHECK_THROWS(json::parse(R"({"enabled":false})").get<ClientLuduvoDiagnosticsConfiguration>());
    LuduvoFixture fixture("nolint_category");
    const auto document = fixture.newDocument("query.server.luau", "--!nolint luduvo/world\ngame.World.Query()");
    const auto diagnostics = diagnosticsFor(fixture, document);
    CHECK(findDiagnostic(diagnostics, "directive.invalid-nolint"));
    CHECK(findDiagnostic(diagnostics, "world.empty-query"));
}

TEST_CASE("workspace_diagnostics_run_for_unopened_luduvo_files")
{
    ClientConfiguration config = Luau::LanguageServer::defaultTestClientConfiguration();
    config.diagnostics.workspace = true;
    LuduvoFixture fixture("luduvo_workspace_diagnostics", config);
    const Uri document = Uri::file(fixture.tempDir.write_child("scripts/autorun/start.server.luau", "print(self)"));

    const auto report = fixture.workspace.workspaceDiagnostics({});
    const auto documentReport = std::find_if(report.items.begin(), report.items.end(),
        [&](const lsp::WorkspaceDocumentDiagnosticReport& item)
        {
            return item.uri == document;
        });
    REQUIRE(documentReport != report.items.end());
    CHECK(findDiagnostic(documentReport->items, "script.autorun-self-access"));
}

TEST_CASE("luduvo_diagnostic_registry_is_valid")
{
    CHECK(validateLuduvoDiagnosticRegistry().empty());
}

TEST_CASE("luduvo_diagnostic_rule_severity_can_be_overridden_or_disabled")
{
    ClientConfiguration config = Luau::LanguageServer::defaultTestClientConfiguration();
    config.platform.luduvo.diagnostics.categories = {
        {"world.empty-query", "warning"},
        {"script.autorun-self-access", "off"},
    };
    LuduvoFixture fixture("luduvo_diagnostic_rule_severity", config);
    const Uri document = fixture.newDocument("scripts/autorun/start.server.luau", "print(self)\ngame.World.Query()");

    const auto diagnostics = diagnosticsFor(fixture, document);
    const lsp::Diagnostic* query = findDiagnostic(diagnostics, "world.empty-query");
    REQUIRE(query);
    CHECK_EQ(query->severity, lsp::DiagnosticSeverity::Warning);
    CHECK_FALSE(findDiagnostic(diagnostics, "script.autorun-self-access"));
}

TEST_CASE("luduvo_diagnostic_configuration_reports_unknown_rules_and_levels")
{
    ClientLuduvoDiagnosticsConfiguration config;
    config.categories = {
        {"not-a-rule", "warning"},
        {"world.empty-query", "loud"},
    };

    const LuduvoDiagnosticConfigurationResult result = resolveLuduvoDiagnosticConfiguration(config);
    CHECK_EQ(result.warnings.size(), 2);
    CHECK(result.settings.categories.empty());
}

TEST_CASE("luduvo_nolint_directives_suppress_custom_diagnostics")
{
    SUBCASE("one rule")
    {
        LuduvoFixture fixture("luduvo_nolint_one_rule");
        const Uri document =
            fixture.newDocument("scripts/autorun/start.server.luau", "--!nolint luduvo/world.empty-query\nprint(self)\ngame.World.Query()");

        const auto diagnostics = diagnosticsFor(fixture, document);
        CHECK_FALSE(findDiagnostic(diagnostics, "world.empty-query"));
        CHECK(findDiagnostic(diagnostics, "script.autorun-self-access"));
        CHECK_FALSE(std::any_of(diagnostics.begin(), diagnostics.end(),
            [](const lsp::Diagnostic& diagnostic)
            {
                return diagnostic.source == "Luau" && diagnostic.message.find("luduvo/world.empty-query") != std::string::npos;
            }));
    }

    SUBCASE("all rules")
    {
        LuduvoFixture fixture("luduvo_nolint_all_rules");
        const Uri document = fixture.newDocument("scripts/autorun/start.server.luau", "--!nolint\nprint(self)\ngame.World.Query()");

        const auto diagnostics = diagnosticsFor(fixture, document);
        CHECK_FALSE(findDiagnostic(diagnostics, "world.empty-query"));
        CHECK_FALSE(findDiagnostic(diagnostics, "script.autorun-self-access"));
    }
}

TEST_CASE("unknown_luduvo_nolint_rule_reports_a_suggestion")
{
    LuduvoFixture fixture("luduvo_nolint_unknown_rule");
    const Uri document = fixture.newDocument("query.server.luau", "--!nolint luduvo/world.empty-qury\ngame.World.Query()");

    const auto diagnostics = diagnosticsFor(fixture, document);
    const lsp::Diagnostic* invalid = findDiagnostic(diagnostics, "directive.invalid-nolint");
    REQUIRE(invalid);
    CHECK(invalid->message.find("luduvo/world.empty-query") != std::string::npos);
    CHECK(findDiagnostic(diagnostics, "world.empty-query"));
}

TEST_CASE("invalid_nolint_directive_is_a_configurable_luduvo_diagnostic")
{
    SUBCASE("severity override")
    {
        ClientConfiguration config = Luau::LanguageServer::defaultTestClientConfiguration();
        config.platform.luduvo.diagnostics.categories = {{"directive.invalid-nolint", "hint"}};
        LuduvoFixture fixture("luduvo_invalid_nolint_severity", config);
        const Uri document = fixture.newDocument("query.server.luau", "--!nolint luduvo/not-a-rule");

        const auto diagnostics = diagnosticsFor(fixture, document);
        const lsp::Diagnostic* diagnostic = findDiagnostic(diagnostics, "directive.invalid-nolint");
        REQUIRE(diagnostic);
        CHECK_EQ(diagnostic->severity, lsp::DiagnosticSeverity::Hint);
    }

    SUBCASE("configuration can disable it")
    {
        ClientConfiguration config = Luau::LanguageServer::defaultTestClientConfiguration();
        config.platform.luduvo.diagnostics.categories = {{"directive.invalid-nolint", "off"}};
        LuduvoFixture fixture("luduvo_invalid_nolint_disabled", config);
        const Uri document = fixture.newDocument("query.server.luau", "--!nolint luduvo/not-a-rule");

        CHECK_FALSE(findDiagnostic(diagnosticsFor(fixture, document), "directive.invalid-nolint"));
    }

    SUBCASE("it can suppress itself")
    {
        LuduvoFixture fixture("luduvo_invalid_nolint_suppressed");
        const Uri document = fixture.newDocument("query.server.luau", "--!nolint luduvo/directive.invalid-nolint\n--!nolint luduvo/not-a-rule");

        CHECK_FALSE(findDiagnostic(diagnosticsFor(fixture, document), "directive.invalid-nolint"));
    }
}


TEST_CASE("event_table_write_checks_use_receiver_types_without_project_context")
{
    LuduvoFixture fixture("event_writes");
    const auto uri = fixture.newDocument("writes.server.luau", R"(
        local function write(events: EventTable, column: EventColumn, key: string, row: number)
            events[key] = column
            column[row] = "hello"
            column[row] += 1
            events, column = events, column
        end
        local function alias(column: EventColumn)
            local other = column
            other[1] = 12
        end
        local ordinary = {message = {1}}
        ordinary.message[1] = 2
        local unknown: any = ordinary
        unknown.message = {}
    )");
    const auto diagnostics = diagnosticsFor(fixture, uri);
    INFO(diagnostics.size());
    CHECK(std::count_if(diagnostics.begin(), diagnostics.end(),
              [](const auto& diagnostic)
              {
                  return diagnostic.code == std::optional<std::variant<std::string, int>>("luduvo/event-table.write");
              }) == 4);
}

TEST_CASE("event_table_write_rule_can_be_suppressed")
{
    LuduvoFixture fixture("event_writes_suppressed");
    const auto uri = fixture.newDocument("writes.server.luau", R"(--!nolint luduvo/event-table.write
        local function write(column: EventColumn) column[1] = 2 end
    )");
    CHECK_FALSE(findDiagnostic(diagnosticsFor(fixture, uri), "event-table.write"));
}

TEST_CASE("event_schema_literal_rules_report_independently")
{
    LuduvoFixture fixture("schema_validation");
    const auto uri = fixture.newDocument("schema.server.luau", R"(
        local ET = game.EventTable
        ET("Test", ET.ToClients, {
            {"", ET.String},
            {"abcdefghijklmnopqrstuvwxyz123456", ET.String},
            {"Push", ET.String},
            {"same", ET.String},
            {"same", ET.String},
            {"missingType"},
            42,
            {"Prefab", ET.String},
        })
    )");
    const auto diagnostics = diagnosticsFor(fixture, uri);
    for (const auto id : {"event-table.fields.empty-name", "event-table.fields.name-length", "event-table.fields.reserved-name",
             "event-table.fields.duplicate", "event-table.fields.shape"})
        CHECK_MESSAGE(findDiagnostic(diagnostics, id), id);
    CHECK_FALSE(findDiagnostic(diagnostics, "event-table.fields.count"));
}

TEST_CASE("event_schema_rules_skip_indirect_schemas_and_shadowed_apis")
{
    LuduvoFixture fixture("schema_validation_indirect");
    const auto uri = fixture.newDocument("schema.server.luau", R"(
        local fields = {{"Push", game.EventTable.String}, {"", game.EventTable.String}}
        game.EventTable("Indirect", game.EventTable.ToClients, fields)
        local game = {EventTable = function(...) end}
        game.EventTable("Unrelated", nil, {{"Push", nil}, {"", nil}})
    )");
    const auto diagnostics = diagnosticsFor(fixture, uri);
    CHECK_FALSE(findDiagnostic(diagnostics, "event-table.fields.reserved-name"));
    CHECK_FALSE(findDiagnostic(diagnostics, "event-table.fields.empty-name"));
}

TEST_CASE("event_schema_count_and_byte_limits_use_literal_values")
{
    LuduvoFixture fixture("schema_limits");
    std::string entries;
    for (int i = 0; i < 25; ++i)
        entries += "{\"field" + std::to_string(i) + "\",game.EventTable.String},";
    const auto uri = fixture.newDocument("limits.server.luau", "game.EventTable('TooMany',game.EventTable.ToClients,{" + entries + "})\n" +
                                                                   "game.EventTable('Bytes',game.EventTable.ToClients,{{'" + std::string(32, 'x') +
                                                                   "',game.EventTable.String}})");
    const auto diagnostics = diagnosticsFor(fixture, uri);
    CHECK(findDiagnostic(diagnostics, "event-table.fields.count"));
    CHECK(findDiagnostic(diagnostics, "event-table.fields.name-length"));
}


TEST_CASE("event_write_unions_are_conservative_and_existing_errors_are_not_duplicated")
{
    LuduvoFixture fixture("event_write_unions");
    const auto uri = fixture.newDocument("unions.server.luau", R"(--!strict
        local function protected(value: EventTable | EventColumn) value[nil :: any] = nil :: any end
        local function mixed(value: EventColumn | {[number]: any}) value[1] = nil :: any end
        local function alreadyInvalid(value: EventTable) value[1] = nil :: any end
    )");
    const auto diagnostics = diagnosticsFor(fixture, uri);
    CHECK_FALSE(findDiagnostic(diagnostics, "event-table.write"));
    for (unsigned int line = 1; line <= 3; ++line)
        CHECK(std::any_of(diagnostics.begin(), diagnostics.end(),
            [&](const auto& diagnostic)
            {
                return diagnostic.source == "Luau" && diagnostic.severity == lsp::DiagnosticSeverity::Error && diagnostic.range.start.line == line;
            }));
}

TEST_CASE("event_schema_lengths_use_decoded_bytes_and_skip_computed_counts")
{
    LuduvoFixture fixture("event_schema_bytes");
    std::string entries;
    for (int i = 0; i < 24; ++i)
        entries += "{'f" + std::to_string(i) + "',game.EventTable.String},";
    const auto uri = fixture.newDocument("bytes.server.luau",
        "game.EventTable('Computed',game.EventTable.ToClients,{" + entries + "getField()})\n" +
            "game.EventTable('Escaped',game.EventTable.ToClients,{{'\\120\\120',game.EventTable.String}})\n" +
            "game.EventTable('Utf8',game.EventTable.ToClients,{{'" +
            std::string(
                "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9") +
            "',game.EventTable.String}})");
    const auto diagnostics = diagnosticsFor(fixture, uri);
    CHECK_FALSE(findDiagnostic(diagnostics, "event-table.fields.count"));
    CHECK(std::count_if(diagnostics.begin(), diagnostics.end(),
              [](const auto& diagnostic)
              {
                  return diagnostic.code == std::optional<std::variant<std::string, int>>("luduvo/event-table.fields.name-length");
              }) == 1);
}

TEST_SUITE_END();
