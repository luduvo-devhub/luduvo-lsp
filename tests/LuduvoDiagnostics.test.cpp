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
            return diagnostic.code == std::optional<std::variant<std::string, int>>(code);
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
    const lsp::Diagnostic* diagnostic = findDiagnostic(diagnostics, "empty-world-query");
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

    CHECK_FALSE(findDiagnostic(diagnosticsFor(fixture, document), "empty-world-query"));
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
            return diagnostic.code == std::optional<std::variant<std::string, int>>("empty-world-query");
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
    CHECK(findDiagnostic(report.items[0].items, "empty-world-query"));
}

TEST_CASE("call_rules_do_not_match_user_types_with_luduvo_names")
{
    LuduvoFixture fixture("luduvo_call_rule_user_type");
    const Uri document = fixture.newDocument("query.server.luau", R"(
        type World = { Query: () -> () }
        local world: World = { Query = function() end }
        world.Query()
    )");

    CHECK_FALSE(findDiagnostic(diagnosticsFor(fixture, document), "empty-world-query"));
}

TEST_CASE("global_self_is_rejected_only_inside_autorun_directories")
{
    LuduvoFixture fixture("luduvo_autorun_self");
    const Uri autorun = fixture.newDocument("scripts/autorun/start.client.luau", "print(self.Name)");
    const Uri attached = fixture.newDocument("scripts/start.client.luau", "print(self.Name)");
    const Uri localSelf = fixture.newDocument("scripts/autorun/local.client.luau", "local self = { Name = 'safe' }\nprint(self.Name)");

    const lsp::Diagnostic* diagnostic = findDiagnostic(diagnosticsFor(fixture, autorun), "autorun-self-access");
    REQUIRE(diagnostic);
    CHECK_EQ(diagnostic->severity, lsp::DiagnosticSeverity::Error);
    CHECK_EQ(diagnostic->message, "'self' is nil in scripts inside an autorun directory.");
    CHECK_FALSE(findDiagnostic(diagnosticsFor(fixture, attached), "autorun-self-access"));
    CHECK_FALSE(findDiagnostic(diagnosticsFor(fixture, localSelf), "autorun-self-access"));
}

TEST_CASE("assigning_global_self_in_an_autorun_script_is_not_a_self_access")
{
    LuduvoFixture fixture("luduvo_autorun_global_self_assignment");
    const Uri document = fixture.newDocument("scripts/autorun/start.server.luau", "self = { Name = 'assigned' }");

    CHECK_FALSE(findDiagnostic(diagnosticsFor(fixture, document), "autorun-self-access"));
}

TEST_CASE("autorun_self_member_writes_and_assignment_value_reads_still_access_self")
{
    LuduvoFixture fixture("luduvo_autorun_self_assignment_reads");
    const Uri memberWrite = fixture.newDocument("scripts/autorun/member.server.luau", "self.Name = 'write'");
    const Uri valueRead = fixture.newDocument("scripts/autorun/value.server.luau", "self = self or {}");

    CHECK(findDiagnostic(diagnosticsFor(fixture, memberWrite), "autorun-self-access"));
    CHECK(findDiagnostic(diagnosticsFor(fixture, valueRead), "autorun-self-access"));
}

TEST_CASE("luduvo_diagnostics_can_be_disabled_globally_or_by_rule")
{
    SUBCASE("all Luduvo diagnostics")
    {
        ClientConfiguration config = Luau::LanguageServer::defaultTestClientConfiguration();
        config.platform.luduvo.diagnostics.enabled = false;
        LuduvoFixture fixture("luduvo_diagnostics_disabled", config);
        const Uri document = fixture.newDocument("scripts/autorun/start.server.luau", "print(self)\ngame.World.Query()");

        const auto diagnostics = diagnosticsFor(fixture, document);
        CHECK_FALSE(findDiagnostic(diagnostics, "autorun-self-access"));
        CHECK_FALSE(findDiagnostic(diagnostics, "empty-world-query"));
    }

    SUBCASE("one rule by its public identifier")
    {
        ClientConfiguration config = Luau::LanguageServer::defaultTestClientConfiguration();
        config.platform.luduvo.diagnostics.rules = {{"empty-world-query", "off"}};
        LuduvoFixture fixture("luduvo_diagnostic_rule_disabled", config);
        const Uri document = fixture.newDocument("scripts/autorun/start.server.luau", "print(self)\ngame.World.Query()");

        const auto diagnostics = diagnosticsFor(fixture, document);
        CHECK(findDiagnostic(diagnostics, "autorun-self-access"));
        CHECK_FALSE(findDiagnostic(diagnostics, "empty-world-query"));
    }
}

TEST_CASE("luduvo_diagnostic_configuration_round_trips")
{
    const json input = json::parse(R"({
        "platform": {"luduvo": {"diagnostics": {
            "enabled": false,
            "rules": {
                "autorun-self-access": "warning",
                "empty-world-query": "off"
            }
        }}}
    })");

    const ClientConfiguration config = input.get<ClientConfiguration>();
    CHECK_FALSE(config.platform.luduvo.diagnostics.enabled);
    CHECK_EQ(config.platform.luduvo.diagnostics.rules.at("autorun-self-access"), "warning");
    CHECK_EQ(config.platform.luduvo.diagnostics.rules.at("empty-world-query"), "off");

    const json output = config;
    CHECK_FALSE(output["platform"]["luduvo"]["diagnostics"]["enabled"]);
    CHECK_EQ(output["platform"]["luduvo"]["diagnostics"]["rules"]["autorun-self-access"], "warning");
    CHECK_EQ(output["platform"]["luduvo"]["diagnostics"]["rules"]["empty-world-query"], "off");
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
    CHECK(findDiagnostic(documentReport->items, "autorun-self-access"));
}

TEST_CASE("luduvo_diagnostic_registry_is_valid")
{
    CHECK(validateLuduvoDiagnosticRegistry().empty());
}

TEST_CASE("luduvo_diagnostic_rule_severity_can_be_overridden_or_disabled")
{
    ClientConfiguration config = Luau::LanguageServer::defaultTestClientConfiguration();
    config.platform.luduvo.diagnostics.rules = {
        {"empty-world-query", "warning"},
        {"autorun-self-access", "off"},
    };
    LuduvoFixture fixture("luduvo_diagnostic_rule_severity", config);
    const Uri document = fixture.newDocument("scripts/autorun/start.server.luau", "print(self)\ngame.World.Query()");

    const auto diagnostics = diagnosticsFor(fixture, document);
    const lsp::Diagnostic* query = findDiagnostic(diagnostics, "empty-world-query");
    REQUIRE(query);
    CHECK_EQ(query->severity, lsp::DiagnosticSeverity::Warning);
    CHECK_FALSE(findDiagnostic(diagnostics, "autorun-self-access"));
}

TEST_CASE("luduvo_diagnostic_configuration_reports_unknown_rules_and_levels")
{
    ClientLuduvoDiagnosticsConfiguration config;
    config.rules = {
        {"not-a-rule", "warning"},
        {"empty-world-query", "loud"},
    };

    const LuduvoDiagnosticConfigurationResult result = resolveLuduvoDiagnosticConfiguration(config);
    CHECK_EQ(result.warnings.size(), 2);
    CHECK(result.settings.rules.empty());
}

TEST_CASE("luduvo_nolint_directives_suppress_custom_diagnostics")
{
    SUBCASE("one rule")
    {
        LuduvoFixture fixture("luduvo_nolint_one_rule");
        const Uri document =
            fixture.newDocument("scripts/autorun/start.server.luau", "--!nolint luduvo/empty-world-query\nprint(self)\ngame.World.Query()");

        const auto diagnostics = diagnosticsFor(fixture, document);
        CHECK_FALSE(findDiagnostic(diagnostics, "empty-world-query"));
        CHECK(findDiagnostic(diagnostics, "autorun-self-access"));
        CHECK_FALSE(std::any_of(diagnostics.begin(), diagnostics.end(),
            [](const lsp::Diagnostic& diagnostic)
            {
                return diagnostic.source == "Luau" && diagnostic.message.find("luduvo/empty-world-query") != std::string::npos;
            }));
    }

    SUBCASE("all rules")
    {
        LuduvoFixture fixture("luduvo_nolint_all_rules");
        const Uri document = fixture.newDocument("scripts/autorun/start.server.luau", "--!nolint\nprint(self)\ngame.World.Query()");

        const auto diagnostics = diagnosticsFor(fixture, document);
        CHECK_FALSE(findDiagnostic(diagnostics, "empty-world-query"));
        CHECK_FALSE(findDiagnostic(diagnostics, "autorun-self-access"));
    }
}

TEST_CASE("unknown_luduvo_nolint_rule_reports_a_suggestion")
{
    LuduvoFixture fixture("luduvo_nolint_unknown_rule");
    const Uri document = fixture.newDocument("query.server.luau", "--!nolint luduvo/empty-world-qury\ngame.World.Query()");

    const auto diagnostics = diagnosticsFor(fixture, document);
    const lsp::Diagnostic* invalid = findDiagnostic(diagnostics, "invalid-nolint-directive");
    REQUIRE(invalid);
    CHECK(invalid->message.find("luduvo/empty-world-query") != std::string::npos);
    CHECK(findDiagnostic(diagnostics, "empty-world-query"));
}

TEST_CASE("invalid_nolint_directive_is_a_configurable_luduvo_diagnostic")
{
    SUBCASE("severity override")
    {
        ClientConfiguration config = Luau::LanguageServer::defaultTestClientConfiguration();
        config.platform.luduvo.diagnostics.rules = {{"invalid-nolint-directive", "hint"}};
        LuduvoFixture fixture("luduvo_invalid_nolint_severity", config);
        const Uri document = fixture.newDocument("query.server.luau", "--!nolint luduvo/not-a-rule");

        const lsp::Diagnostic* diagnostic = findDiagnostic(diagnosticsFor(fixture, document), "invalid-nolint-directive");
        REQUIRE(diagnostic);
        CHECK_EQ(diagnostic->severity, lsp::DiagnosticSeverity::Hint);
    }

    SUBCASE("configuration can disable it")
    {
        ClientConfiguration config = Luau::LanguageServer::defaultTestClientConfiguration();
        config.platform.luduvo.diagnostics.rules = {{"invalid-nolint-directive", "off"}};
        LuduvoFixture fixture("luduvo_invalid_nolint_disabled", config);
        const Uri document = fixture.newDocument("query.server.luau", "--!nolint luduvo/not-a-rule");

        CHECK_FALSE(findDiagnostic(diagnosticsFor(fixture, document), "invalid-nolint-directive"));
    }

    SUBCASE("it can suppress itself")
    {
        LuduvoFixture fixture("luduvo_invalid_nolint_suppressed");
        const Uri document = fixture.newDocument("query.server.luau", "--!nolint luduvo/invalid-nolint-directive\n--!nolint luduvo/not-a-rule");

        CHECK_FALSE(findDiagnostic(diagnosticsFor(fixture, document), "invalid-nolint-directive"));
    }
}

TEST_SUITE_END();
