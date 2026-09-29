#include "doctest.h"
#include "Fixture.h"
#include "LSP/DocumentationParser.hpp"
#include "LuauFileUtils.hpp"

static std::string hoverWithDocumentation(std::string documentation, std::string type)
{
    return documentation + kDocumentationBreaker + type;
}

TEST_SUITE_BEGIN("Hover");

TEST_CASE_FIXTURE(Fixture, "show_string_length_on_hover")
{
    auto source = R"(
        local x = "this is a string"
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 18};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luau", "string (16 bytes)"));
}

TEST_CASE_FIXTURE(Fixture, "documentation_links_do_not_form_setext_headings")
{
    loadDefinition("@test", "declare documentedValue: string");
    client->documentation["@test/global/documentedValue"] =
        Luau::BasicDocumentation{"Value documentation", "https://example.com/documented-value", ""};

    auto uri = newDocument("foo.luau", "local value = documentedValue");

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{0, 15};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value,
        "Value documentation\n\n[Learn More](https://example.com/documented-value)\n\n___\n\n" +
            codeBlock("luau", "type documentedValue = string"));
}

TEST_CASE("luduvo_hover_preserves_named_extern_type_documentation_symbols")
{
    ScopedFastFlag solverFlag{FFlag::LuauSolverV2, true};
    TempDir temp("luduvo_hover_extern_documentation");
    const std::string definitions = temp.write_child("luduvo.d.luau", R"(
        declare extern type Instance with
            Name: string
        end
        declare game: {
            read Selected: Instance,
        }
    )");

    TestClient client;
    auto config = Luau::LanguageServer::defaultTestClientConfiguration();
    config.platform.type = LSPPlatformConfig::Luduvo;
    config.platform.luduvo.definitions.serverOverride = definitions;
    config.platform.luduvo.definitions.clientOverride = definitions;
    config.platform.luduvo.definitions.exposePrivateTypes = true;
    client.globalConfig = config;

    WorkspaceFolder workspace(
        &client, "$LUDUVO_HOVER_EXTERN_DOCUMENTATION", Uri::file(*Luau::FileUtils::getCurrentWorkingDirectory()), std::nullopt);
    workspace.setupWithConfiguration(config);
    workspace.isReady = true;
    client.documentation["@luduvo/server/globaltype/Instance"] = Luau::BasicDocumentation{"Instance documentation"};
    client.documentation["@luduvo/server/globaltype/Instance.Name"] = Luau::BasicDocumentation{"Name documentation"};

    auto uri = Luau::LanguageServer::newDocument(
        workspace, "extern.server.luau", "local selected: Instance = game.Selected\nprint(selected.Name)");
    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};

    params.position = lsp::Position{0, 17};
    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK(result->contents.value.find("Instance documentation") != std::string::npos);

    params.position = lsp::Position{1, 16};
    result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK(result->contents.value.find("Name documentation") != std::string::npos);
}

TEST_CASE("luduvo_rich_hover_identifies_server_read_only_unlintable_members")
{
    ScopedFastFlag solverFlag{FFlag::LuauSolverV2, true};
    TempDir temp("luduvo_rich_hover_member");
    const std::string definitions = temp.write_child("luduvo.d.luau", R"(
        declare game: {
            read World: {
                Camera: any,
            },
        }
    )");

    TestClient client;
    auto config = Luau::LanguageServer::defaultTestClientConfiguration();
    config.platform.type = LSPPlatformConfig::Luduvo;
    config.platform.luduvo.definitions.serverOverride = definitions;
    config.platform.luduvo.definitions.clientOverride = definitions;
    client.globalConfig = config;

    WorkspaceFolder workspace(&client, "$LUDUVO_RICH_HOVER_MEMBER", Uri::file(*Luau::FileUtils::getCurrentWorkingDirectory()), std::nullopt);
    workspace.setupWithConfiguration(config);
    workspace.isReady = true;
    client.documentation["@luduvo/server/global/game.World"] =
        Luau::BasicDocumentation{"The entity world", "https://docs.luduvo.com/reference/World", ""};

    auto uri = Luau::LanguageServer::newDocument(workspace, "member.server.luau", "local world = game.World");
    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{0, 20};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, "# [game.World ](https://docs.luduvo.com/reference/World)↗ (Server Version)\n"
                                     "`read-only` · `unlintable`\n\n"
                                     "The entity world\n\n___\n\n" +
                                         codeBlock("luau", "{\n    Camera: any\n}"));
}

TEST_CASE("luduvo_rich_hover_preserves_direct_member_identity_for_unlintable_locals")
{
    ScopedFastFlag solverFlag{FFlag::LuauSolverV2, true};
    TempDir temp("luduvo_rich_hover_unlintable_local");
    const std::string definitions = temp.write_child("luduvo.d.luau", R"(
        declare game: {
            read World: {
                Camera: any,
            },
        }
    )");

    TestClient client;
    auto config = Luau::LanguageServer::defaultTestClientConfiguration();
    config.platform.type = LSPPlatformConfig::Luduvo;
    config.platform.luduvo.definitions.serverOverride = definitions;
    config.platform.luduvo.definitions.clientOverride = definitions;
    client.globalConfig = config;

    WorkspaceFolder workspace(
        &client, "$LUDUVO_RICH_HOVER_UNLINTABLE_LOCAL", Uri::file(*Luau::FileUtils::getCurrentWorkingDirectory()), std::nullopt);
    workspace.setupWithConfiguration(config);
    workspace.isReady = true;
    client.documentation["@luduvo/client/global/game.World.Camera"] =
        Luau::BasicDocumentation{"The active camera", "https://docs.luduvo.com/reference/Camera", ""};

    auto uri = Luau::LanguageServer::newDocument(workspace, "camera.client.luau", "local camera = game.World.Camera\nprint(camera)");
    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 7};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, "# [Camera ](https://docs.luduvo.com/reference/Camera)↗ (Client Version)\n"
                                     "`unlintable`\n\n"
                                     "The active camera\n\n___\n\n" +
                                         codeBlock("luau", "any"));
}

TEST_CASE("luduvo_rich_hover_uses_the_named_type_for_optional_locals")
{
    ScopedFastFlag solverFlag{FFlag::LuauSolverV2, true};
    TempDir temp("luduvo_rich_hover_optional_local");
    const std::string definitions = temp.write_child("luduvo.d.luau", R"(
        declare extern type RaycastResult with
            Position: vector
        end
    )");

    TestClient client;
    auto config = Luau::LanguageServer::defaultTestClientConfiguration();
    config.platform.type = LSPPlatformConfig::Luduvo;
    config.platform.luduvo.definitions.serverOverride = definitions;
    config.platform.luduvo.definitions.clientOverride = definitions;
    client.globalConfig = config;

    WorkspaceFolder workspace(&client, "$LUDUVO_RICH_HOVER_OPTIONAL", Uri::file(*Luau::FileUtils::getCurrentWorkingDirectory()), std::nullopt);
    workspace.setupWithConfiguration(config);
    workspace.isReady = true;
    client.documentation["@luduvo/client/globaltype/RaycastResult"] =
        Luau::BasicDocumentation{"The result of a raycast", "https://docs.luduvo.com/reference/RaycastResult", ""};

    auto uri = Luau::LanguageServer::newDocument(workspace, "optional.client.luau", "local hit: RaycastResult? = nil");
    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{0, 7};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, "# [RaycastResult ](https://docs.luduvo.com/reference/RaycastResult)↗ (Client Version)\n\n"
                                     "The result of a raycast\n\n___\n\n" +
                                         codeBlock("luau", "RaycastResult?"));
}

TEST_CASE("luduvo_standard_hover_keeps_documentation_above_the_type_definition")
{
    ScopedFastFlag solverFlag{FFlag::LuauSolverV2, true};
    TempDir temp("luduvo_standard_hover");
    const std::string definitions = temp.write_child("luduvo.d.luau", "declare documentedValue: string");

    TestClient client;
    auto config = Luau::LanguageServer::defaultTestClientConfiguration();
    config.platform.type = LSPPlatformConfig::Luduvo;
    config.platform.luduvo.definitions.serverOverride = definitions;
    config.platform.luduvo.definitions.clientOverride = definitions;
    config.platform.luduvo.hover.presentation = LuduvoHoverPresentation::Standard;
    client.globalConfig = config;

    WorkspaceFolder workspace(&client, "$LUDUVO_STANDARD_HOVER", Uri::file(*Luau::FileUtils::getCurrentWorkingDirectory()), std::nullopt);
    workspace.setupWithConfiguration(config);
    workspace.isReady = true;
    client.documentation["@luduvo/server/global/documentedValue"] =
        Luau::BasicDocumentation{"Value documentation", "https://docs.luduvo.com/reference/Value", ""};

    auto uri = Luau::LanguageServer::newDocument(workspace, "standard.server.luau", "local value = documentedValue");
    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{0, 15};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, "Value documentation\n\n[Learn More](https://docs.luduvo.com/reference/Value)\n\n___\n\n" +
                                         codeBlock("luau", "type documentedValue = string"));
}

TEST_CASE("luduvo_rich_hover_does_not_decorate_literals")
{
    TestClient client;
    auto config = Luau::LanguageServer::defaultTestClientConfiguration();
    config.platform.type = LSPPlatformConfig::Luduvo;
    config.platform.luduvo.definitions.globalPolicy = LuduvoGlobalDefinitionsPolicy::DefinitionFilesOnly;
    client.globalConfig = config;

    WorkspaceFolder workspace(&client, "$LUDUVO_LITERAL_HOVER", Uri::file(*Luau::FileUtils::getCurrentWorkingDirectory()), std::nullopt);
    workspace.setupWithConfiguration(config);
    workspace.isReady = true;

    auto uri = Luau::LanguageServer::newDocument(workspace, "literal.server.luau", "local value = 42\nprint(value)");
    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{0, 15};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luau", "number"));

    params.position = lsp::Position{1, 7};
    result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, "# number\n\n___\n\n" + codeBlock("luau", "number"));
}

TEST_CASE("luduvo_rich_hover_uses_declaration_comments_when_website_docs_are_missing")
{
    ScopedFastFlag solverFlag{FFlag::LuauSolverV2, true};
    TempDir temp("luduvo_rich_hover_fallback_docs");
    const std::string definitions = temp.write_child("luduvo.d.luau", R"(
        --- A value documented by the declaration file.
        declare fallbackValue: {}
    )");

    TestClient client;
    auto config = Luau::LanguageServer::defaultTestClientConfiguration();
    config.platform.type = LSPPlatformConfig::Luduvo;
    config.platform.luduvo.definitions.serverOverride = definitions;
    config.platform.luduvo.definitions.clientOverride = definitions;
    client.globalConfig = config;

    WorkspaceFolder workspace(&client, "$LUDUVO_RICH_HOVER_FALLBACK_DOCS", Uri::file(*Luau::FileUtils::getCurrentWorkingDirectory()), std::nullopt);
    workspace.setupWithConfiguration(config);
    workspace.isReady = true;

    auto uri = Luau::LanguageServer::newDocument(workspace, "fallback.server.luau", "local value = fallbackValue");
    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{0, 16};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, "# fallbackValue (Server Version)\n\n"
                                     "A value documented by the declaration file.\n\n___\n\n" +
                                         codeBlock("luau", "{  }"));
}

TEST_CASE("luduvo_rich_hover_truncates_titles_without_hiding_the_version")
{
    ScopedFastFlag solverFlag{FFlag::LuauSolverV2, true};
    TempDir temp("luduvo_rich_hover_title_limit");
    const std::string definitions = temp.write_child("luduvo.d.luau", "declare extremelyLongGlobalName: number");

    TestClient client;
    auto config = Luau::LanguageServer::defaultTestClientConfiguration();
    config.platform.type = LSPPlatformConfig::Luduvo;
    config.platform.luduvo.definitions.serverOverride = definitions;
    config.platform.luduvo.definitions.clientOverride = definitions;
    config.platform.luduvo.hover.maxTitleLength = 24;
    client.globalConfig = config;

    WorkspaceFolder workspace(&client, "$LUDUVO_RICH_HOVER_TITLE_LIMIT", Uri::file(*Luau::FileUtils::getCurrentWorkingDirectory()), std::nullopt);
    workspace.setupWithConfiguration(config);
    workspace.isReady = true;

    auto uri = Luau::LanguageServer::newDocument(workspace, "limit.server.luau", "local value = extremelyLongGlobalName");
    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{0, 20};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, "# extr... (Server Version)\n\n___\n\n" + codeBlock("luau", "number"));
}

TEST_CASE("luduvo_rich_hover_truncates_type_definitions_independently")
{
    ScopedFastFlag solverFlag{FFlag::LuauSolverV2, true};
    TempDir temp("luduvo_rich_hover_definition_limit");
    const std::string definitions = temp.write_child("luduvo.d.luau", R"(
        declare shape: {
            Alpha: number,
            Beta: string,
        }
    )");

    TestClient client;
    auto config = Luau::LanguageServer::defaultTestClientConfiguration();
    config.platform.type = LSPPlatformConfig::Luduvo;
    config.platform.luduvo.definitions.serverOverride = definitions;
    config.platform.luduvo.definitions.clientOverride = definitions;
    config.platform.luduvo.hover.maxTypeDefinitionLength = 12;
    client.globalConfig = config;

    WorkspaceFolder workspace(
        &client, "$LUDUVO_RICH_HOVER_DEFINITION_LIMIT", Uri::file(*Luau::FileUtils::getCurrentWorkingDirectory()), std::nullopt);
    workspace.setupWithConfiguration(config);
    workspace.isReady = true;

    auto uri = Luau::LanguageServer::newDocument(workspace, "limit.server.luau", "local value = shape");
    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{0, 15};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, "# shape (Server Version)\n\n___\n\n" + codeBlock("luau", "{\n    Alp..."));
}

TEST_CASE("luduvo_rich_hover_keeps_short_union_titles_and_aggregates_their_side")
{
    ScopedFastFlag solverFlag{FFlag::LuauSolverV2, true};
    TempDir temp("luduvo_rich_hover_union");
    const std::string definitions = temp.write_child("luduvo.d.luau", R"(
        declare extern type Camera with end
        declare extern type RaycastResult with end
    )");

    TestClient client;
    auto config = Luau::LanguageServer::defaultTestClientConfiguration();
    config.platform.type = LSPPlatformConfig::Luduvo;
    config.platform.luduvo.definitions.serverOverride = definitions;
    config.platform.luduvo.definitions.clientOverride = definitions;
    client.globalConfig = config;

    WorkspaceFolder workspace(&client, "$LUDUVO_RICH_HOVER_UNION", Uri::file(*Luau::FileUtils::getCurrentWorkingDirectory()), std::nullopt);
    workspace.setupWithConfiguration(config);
    workspace.isReady = true;

    auto uri = Luau::LanguageServer::newDocument(workspace, "union.client.luau", "local result: Camera | RaycastResult = nil :: any");
    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{0, 7};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, "# Camera | RaycastResult (Client Version)\n\n___\n\n" + codeBlock("luau", "Camera | RaycastResult"));
}

TEST_CASE("luduvo_rich_hover_summarizes_long_union_titles")
{
    ScopedFastFlag solverFlag{FFlag::LuauSolverV2, true};
    TempDir temp("luduvo_rich_hover_long_union");
    const std::string definitions = temp.write_child("luduvo.d.luau", R"(
        declare extern type Camera with end
        declare extern type RaycastResult with end
        declare extern type ExtremelyVerbosePhysicsQueryResponse with end
    )");

    TestClient client;
    auto config = Luau::LanguageServer::defaultTestClientConfiguration();
    config.platform.type = LSPPlatformConfig::Luduvo;
    config.platform.luduvo.definitions.serverOverride = definitions;
    config.platform.luduvo.definitions.clientOverride = definitions;
    config.platform.luduvo.hover.maxTitleLength = 40;
    client.globalConfig = config;

    WorkspaceFolder workspace(&client, "$LUDUVO_RICH_HOVER_LONG_UNION", Uri::file(*Luau::FileUtils::getCurrentWorkingDirectory()), std::nullopt);
    workspace.setupWithConfiguration(config);
    workspace.isReady = true;

    auto uri = Luau::LanguageServer::newDocument(
        workspace, "union.client.luau", "local result: Camera | RaycastResult | ExtremelyVerbosePhysicsQueryResponse = nil :: any");
    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{0, 7};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value,
        "# Camera +2 union members (Client Version)\n\n___\n\n" + codeBlock("luau", "Camera | ExtremelyVerbosePhysicsQueryResponse | RaycastResult"));
}

TEST_CASE("luduvo_rich_hover_tags_deprecated_members")
{
    ScopedFastFlag solverFlag{FFlag::LuauSolverV2, true};
    TempDir temp("luduvo_rich_hover_write_only");
    const std::string definitions = temp.write_child("luduvo.d.luau", R"(
        declare extern type Api with
            @deprecated
            function Secret(self): any
        end
        declare api: Api
    )");

    TestClient client;
    auto config = Luau::LanguageServer::defaultTestClientConfiguration();
    config.platform.type = LSPPlatformConfig::Luduvo;
    config.platform.luduvo.definitions.serverOverride = definitions;
    config.platform.luduvo.definitions.clientOverride = definitions;
    config.platform.luduvo.definitions.exposePrivateTypes = true;
    client.globalConfig = config;

    WorkspaceFolder workspace(&client, "$LUDUVO_RICH_HOVER_WRITE_ONLY", Uri::file(*Luau::FileUtils::getCurrentWorkingDirectory()), std::nullopt);
    workspace.setupWithConfiguration(config);
    workspace.isReady = true;

    auto uri = Luau::LanguageServer::newDocument(workspace, "write.server.luau", "local secret = api.Secret");
    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{0, 20};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK(result->contents.value.find("# Api.Secret (Server Version)\n`deprecated` · `unlintable`") == 0);
}

TEST_CASE("luduvo_rich_hover_tags_write_only_members")
{
    ScopedFastFlag solverFlag{FFlag::LuauSolverV2, true};
    TempDir temp("luduvo_rich_hover_write_only");
    const std::string definitions = temp.write_child("luduvo.d.luau", "declare api: { write Secret: any }");

    TestClient client;
    auto config = Luau::LanguageServer::defaultTestClientConfiguration();
    config.platform.type = LSPPlatformConfig::Luduvo;
    config.platform.luduvo.definitions.serverOverride = definitions;
    config.platform.luduvo.definitions.clientOverride = definitions;
    client.globalConfig = config;

    WorkspaceFolder workspace(&client, "$LUDUVO_RICH_HOVER_WRITE_ONLY", Uri::file(*Luau::FileUtils::getCurrentWorkingDirectory()), std::nullopt);
    workspace.setupWithConfiguration(config);
    workspace.isReady = true;

    auto uri = Luau::LanguageServer::newDocument(workspace, "write.server.luau", "api.Secret = 5");
    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{0, 5};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, "# api.Secret (Server Version)\n"
                                     "`write-only` · `unlintable`\n\n___\n\n" +
                                         codeBlock("luau", "any"));
}

TEST_CASE_FIXTURE(Fixture, "hover_shows_const_for_a_const_local")
{
    auto source = R"(
        const x = 5
        local y = x
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};

    // The declaration
    params.position = lsp::Position{1, 14};
    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luau", "const x: number"));

    // A use of the binding
    params.position = lsp::Position{2, 18};
    result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luau", "const x: number"));
}

TEST_CASE_FIXTURE(Fixture, "hover_shows_local_for_a_non_const_local")
{
    auto source = R"(
        local x = 5
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 14};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luau", "local x: number"));
}

TEST_CASE_FIXTURE(Fixture, "show_string_utf8_characters_on_hover")
{
    auto source = R"(
        local x = "this is an emoji: 😁"
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 18};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luau", "string (22 bytes, 19 characters)"));
}

TEST_CASE_FIXTURE(Fixture, "basic_type_alias_declaration")
{
    auto source = R"(
        type Identity = string
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 18};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luau", "type Identity = string"));
}

TEST_CASE_FIXTURE(Fixture, "type_alias_declaration_with_single_generic")
{
    auto source = R"(
        type Identity<T> = T
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 18};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luau", "type Identity<T> = T"));
}

TEST_CASE_FIXTURE(Fixture, "type_alias_declaration_with_generic_default_value")
{
    auto source = R"(
        type Identity<T = string> = T
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 18};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luau", "type Identity<T = string> = T"));
}

TEST_CASE_FIXTURE(Fixture, "type_alias_declaration_with_multiple_generics")
{
    auto source = R"(
        type Identity<T, U = string> = T
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 18};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luau", "type Identity<T, U = string> = T"));
}

TEST_CASE_FIXTURE(Fixture, "type_alias_declaration_generic_type_pack")
{
    auto source = R"(
        type Identity<T...> = (any) -> T...
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 18};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luau", "type Identity<T...> = (any) -> (T...)"));
}

TEST_CASE_FIXTURE(Fixture, "type_alias_declaration_generic_type_pack_with_default")
{
    auto source = R"(
        type Identity<T... = ...string> = (any) -> T...
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 18};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luau", "type Identity<T... = ...string> = (any) -> (T...)"));
}

TEST_CASE_FIXTURE(Fixture, "complex_type_alias_declaration_with_generics")
{
    auto source = R"(
        type Identity<T, U = number, V... = ...string> = (T, U) -> V...
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 18};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luau", "type Identity<T, U = number, V... = ...string> = (T, U) -> (V...)"));
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_for_a_type_table")
{
    auto source = R"(
        --- This is documentation for Foo
        type Foo = {
        }
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{2, 14};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, hoverWithDocumentation("This is documentation for Foo\n", codeBlock("luau", "type Foo = {  }")));
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_for_a_type_table_when_hovering_over_variable_with_type")
{
    auto source = R"(
        --- This is documentation for Foo
        type Foo = {
        }
        local x: Foo = nil
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{4, 14};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, hoverWithDocumentation("This is documentation for Foo\n", codeBlock("luau", "local x: {  }")));
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_for_a_member_of_a_type_table")
{
    auto source = R"(
        --- This is documentation for Foo
        type Foo = {
            --- This is a member bar
            bar: string,
        }
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{4, 13};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, hoverWithDocumentation("This is a member bar\n", codeBlock("luau", "string")));
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_for_a_member_of_a_type_table_when_hovering_over_property")
{
    auto source = R"(
        --- This is documentation for Foo
        type Foo = {
            --- This is a member bar
            bar: string,
        }
        local x: Foo
        local y = x.bar
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{7, 21};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, hoverWithDocumentation("This is a member bar\n", codeBlock("luau", "string")));
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_for_a_member_of_an_intersected_type_table_when_hovering_over_property")
{
    auto source = R"(
        type A = {
            --- Example sick number
            Hello: number
        }

        type B = {
            --- Example sick string
            Heya: string
        } & A

        local item: B = nil
        print(item.Heya)
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{12, 21};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, hoverWithDocumentation("Example sick string\n", codeBlock("luau", "string")));
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_for_a_function")
{
    auto source = R"(
        --- This is documentation for Foo
        function foo()
        end
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{2, 18};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, hoverWithDocumentation("This is documentation for Foo\n", codeBlock("luau", "function foo(): ()")));
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_for_a_function_call")
{
    auto source = R"(
        --- This is documentation for Foo
        function foo()
        end
        foo()
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{4, 9};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, hoverWithDocumentation("This is documentation for Foo\n", codeBlock("luau", "function foo(): ()")));
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_for_type_alias_declarations")
{
    auto source = R"(
        --- The metre (or meter in [US spelling]; symbol: m) is the [base unit] of [length]
        --- in the [International System of Units] (SI)
        export type Meters = number
    )";

    auto uri = newDocument("meters.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{3, 21};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value,
        hoverWithDocumentation("The metre (or meter in [US spelling]; symbol: m) is the [base unit] of [length]\n"
                               "in the [International System of Units] (SI)\n",
            codeBlock("luau", "type Meters = number")));
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_for_type_alias_declarations_of_intersected_tables")
{
    auto source = R"(
        type Foo = {
            foo: "Foo",
        }

        type Bar = {
            bar: "Bar",
        }

        --- The terms foobar (/ˈfuːbɑːr/), foo, bar, baz, qux, quux, and others are used as
        --- metasyntactic variables and placeholder names in computer programming or computer-related documentation
        export type Foobar = Foo & Bar
    )";

    auto uri = newDocument("meters.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{11, 21};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, hoverWithDocumentation(
        "The terms foobar (/ˈfuːbɑːr/), foo, bar, baz, qux, quux, and others are used as\n"
        "metasyntactic variables and placeholder names in computer programming or computer-related documentation\n",
        codeBlock("luau", "type Foobar = {\n    bar: \"Bar\"\n} & {\n    foo: \"Foo\"\n}")));
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_for_type_references")
{
    auto source = R"(
        type Foo = {
            foo: "Foo",
        }

        type Bar = {
            bar: "Bar",
        }

        --- This is the intersection of two types
        export type Foobar = Foo & Bar

        function consumer(value: Foobar)
        end
    )";

    auto uri = newDocument("meters.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{12, 36};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, hoverWithDocumentation("This is the intersection of two types\n",
                                                codeBlock("luau", "type Foobar = {\n    bar: \"Bar\"\n} & {\n    foo: \"Foo\"\n}")));
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_for_external_type_references")
{
    auto source = newDocument("types.luau", R"(
        --- This is a type
        export type Value = string
    )");

    auto uri = newDocument("source.luau", R"(
        local Types = require("types.luau")

        local x: Types.Value
    )");

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{3, 25};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, hoverWithDocumentation("This is a type\n", codeBlock("luau", "type Types.Value = string")));
}

TEST_CASE_FIXTURE(Fixture, "show_type_of_global_variable")
{
    auto source = R"(
        print(DocumentedGlobalVariable)
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 23};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, codeBlock("luau", "type DocumentedGlobalVariable = number"));
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_for_a_global_type_table_from_definitions_file")
{
    auto source = R"(
        local x: DocumentedTable = nil
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 24};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, hoverWithDocumentation("This is a documented table\n", codeBlock("luau", "type DocumentedTable = {\n"
                                                                                                   "    member1: string\n"
                                                                                                   "}")));
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_for_a_global_type_table_from_definitions_file_when_hovering_over_variable_with_type")
{
    auto source = R"(
        local x: DocumentedTable = nil
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 14};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, hoverWithDocumentation("This is a documented table\n", codeBlock("luau", "local x: {\n"
                                                                                                   "    member1: string\n"
                                                                                                   "}")));
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_for_a_global_type_table_from_definitions_file_when_hovering_over_property")
{
    auto source = R"(
        local x: DocumentedTable = nil
        local y = x.member1
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{2, 23};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, hoverWithDocumentation("This is documented member1 of the table\n", codeBlock("luau", "string")));
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_for_a_global_function_call_from_definitions_file")
{
    auto source = R"(
        DocumentedGlobalFunction()
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 20};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, hoverWithDocumentation("This is a documented global function\n", codeBlock("luau", "function DocumentedGlobalFunction(): number")));
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_when_hovering_over_class_type_from_definitions_file")
{
    auto source = R"(
        local x: DocumentedClass
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 23};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(
        result->contents.value, hoverWithDocumentation("This is a documented class\n", codeBlock("luau", "type DocumentedClass = DocumentedClass")));
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_when_hovering_over_variable_with_class_type")
{
    auto source = R"(
        local x: DocumentedClass
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{1, 14};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, hoverWithDocumentation("This is a documented class\n", codeBlock("luau", "local x: DocumentedClass")));
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_when_hovering_over_class_type_property")
{
    auto source = R"(
        local x: DocumentedClass
        local y = x.member1
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{2, 23};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, hoverWithDocumentation("This is a documented member1 of the class\n", codeBlock("luau", "string")));
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_when_hovering_over_class_type_method_call")
{
    auto source = R"(
        local x: DocumentedClass
        local y = x:function1()
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{2, 23};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, hoverWithDocumentation("This is a documented function1 of the class\n", codeBlock("luau", "function DocumentedClass:function1(): number")));
}

// TEST_CASE_FIXTURE(Fixture, "includes_documentation_when_hovering_over_global_variable_from_definitions_file")
//{
//     auto source = R"(
//          print(DocumentedGlobalVariable)
//      )";
//
//     auto uri = newDocument("foo.luau", source);
//
//     lsp::HoverParams params;
//     params.textDocument = lsp::TextDocumentIdentifier{uri};
//     params.position = lsp::Position{1, 23};
//
//     auto result = workspace.hover(params, nullptr);
//     REQUIRE(result);
//     CHECK_EQ(result->contents.value,
//         hoverWithDocumentation("This is a documented global variable\n", codeBlock("luau", "type DocumentedGlobalVariable = number")));
// }

TEST_CASE_FIXTURE(Fixture, "includes_documentation_when_all_parts_of_union_point_to_same_location")
{
    auto uri = newDocument("foo.luau", R"(
        type BaseNode<HOS> = {
	        --[[
		        Indicates if the node has only a single supporter, this is purely internal
		        and only used by `object_tree.closest_empty_node`,
		        as an optimization for trees that have a taper type of "Flat" or "Slope".
	        ]]
	        has_one_supporter: HOS,
        }

        export type Node = BaseNode<true> | BaseNode<false>

        local x: Node = {} :: any

        x.has_one_supporter
    )");

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{14, 17}; // 'x.has_one_supporter'

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, hoverWithDocumentation(
        "Indicates if the node has only a single supporter, this is purely internal\n"
        "and only used by `object_tree.closest_empty_node`,\n"
        "as an optimization for trees that have a taper type of \"Flat\" or \"Slope\".\n",
        codeBlock("luau", FFlag::LuauSolverV2 ? "boolean" : "false | true")));
}

TEST_CASE_FIXTURE(Fixture, "handles_type_references_without_types_graph")
{
    auto source = newDocument("types.luau", R"(
        --- This is a type
        export type Value = string
    )");

    auto uri = newDocument("source.luau", R"(
        local Types = require("types.luau")

        local x: Types.Value
    )");

    // This test explicitly expects type graphs to not be retained (i.e., the required module scope was cleared)
    // We should still be able to find the type references.
    workspace.checkSimple(workspace.fileResolver.getModuleName(uri), /* cancellationToken= */ nullptr);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{3, 25};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK_EQ(result->contents.value, hoverWithDocumentation("This is a type\n", codeBlock("luau", "type Types.Value = string")));
}

TEST_CASE_FIXTURE(Fixture, "hover_respects_cancellation")
{
    auto cancellationToken = std::make_shared<Luau::FrontendCancellationToken>();
    cancellationToken->cancel();

    auto document = newDocument("a.luau", "local x = 1");
    CHECK_THROWS_AS(workspace.hover(lsp::HoverParams{{{document}}}, cancellationToken), RequestCancelledException);
}

TEST_CASE_FIXTURE(Fixture, "hovering_over_comment_inside_local_function_body_does_not_show_function_type")
{
    auto [source, marker] = sourceWithMarker(R"(
        local function add1(n: number): number
            -- hovering | over me should not show function type
            return n + 1
        end
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.hover(params, nullptr);
    CHECK_FALSE(result.has_value());
}

TEST_CASE_FIXTURE(Fixture, "hovering_over_comment_inside_global_function_body_does_not_show_function_type")
{
    auto [source, marker] = sourceWithMarker(R"(
        function add1(n: number): number
            -- hovering | over me should not show function type
            return n + 1
        end
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.hover(params, nullptr);
    CHECK_FALSE(result.has_value());
}

TEST_CASE_FIXTURE(Fixture, "hovering_over_comment_inside_anonymous_function_body_does_not_show_function_type")
{
    auto [source, marker] = sourceWithMarker(R"(
        local add1 = function(n: number): number
            -- hovering | over me should not show function type
            return n + 1
        end
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.hover(params, nullptr);
    CHECK_FALSE(result.has_value());
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_for_base_table_member_of_setmetatable_type")
{
    auto source = R"(
        local meta = {
            __index = {
                --- Documentation for prop_b.
                prop_b = "hello",
            }
        }

        local obj = setmetatable({
            --- Documentation for prop_a.
            prop_a = "world",
        }, meta)

        local y = obj.prop_a
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{13, 22};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK(result->contents.value.find("Documentation for prop_a.") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "includes_documentation_for_index_member_of_setmetatable_type")
{
    auto source = R"(
        local meta = {
            __index = {
                --- Documentation for prop_b.
                prop_b = "hello",
            }
        }

        local obj = setmetatable({
            --- Documentation for prop_a.
            prop_a = "world",
        }, meta)

        local y = obj.prop_b
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::HoverParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{13, 22};

    auto result = workspace.hover(params, nullptr);
    REQUIRE(result);
    CHECK(result->contents.value.find("Documentation for prop_b.") != std::string::npos);
}

TEST_SUITE_END();
