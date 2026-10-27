#include "doctest.h"
#include "Analyze/CliConfigurationParser.hpp"

TEST_SUITE_BEGIN("CliConfigurationParser");

TEST_CASE("convert dotted dictionary")
{
    auto output = parseDottedConfiguration(R"DOTTED_CONFIG(
        {
            "luau-lsp.foo.bar": true,
            "luau-lsp.foo.baz": 1,
            "luau-lsp.bar": "testing"
        }
    )DOTTED_CONFIG");

    REQUIRE(output.is_object());
    REQUIRE(output.contains("foo"));
    REQUIRE(output.contains("bar"));
    REQUIRE(output["foo"].is_object());

    CHECK_EQ(output["foo"]["bar"], true);
    CHECK_EQ(output["foo"]["baz"], 1);
    CHECK_EQ(output["bar"], "testing");
}

TEST_CASE("convert dotted Luduvo resource configuration")
{
    auto output = parseDottedConfiguration(R"DOTTED_CONFIG(
        {
            "luau-lsp.platform.luduvo.resources.groups": {
                "animation-state": {
                    "dataType": "string",
                    "sources": ["default-states"]
                },
                "prefab": {
                    "dataType": "string",
                    "includeBuiltins": true,
                    "sources": ["project-prefabs"]
                }
            },
            "luau-lsp.platform.luduvo.resources.sources": {
                "default-states": {"provider": "static", "values": ["Idle", "Running"]},
                "project-prefabs": {
                    "provider": "static",
                    "values": [{"name": "Test", "documentation": "Project test prefab"}]
                }
            },
            "luau-lsp.platform.luduvo.resources.bindings": [
                {
                    "symbol": "MyLibrary.Query",
                    "parameter": "components",
                    "kind": "component",
                    "variadic": true,
                    "pluralResourceNoun": true
                }
            ],
            "luau-lsp.platform.luduvo.resources.completion.enabled": false
            ,"luau-lsp.platform.luduvo.bindings": [
                {"symbol": "Color3.fromHex", "kind": "color", "target": {"parameter": "hex"}, "encoding": "hex"}
            ],
            "luau-lsp.platform.luduvo.directories.roaming": "roaming-data",
            "luau-lsp.platform.luduvo.directories.local": "local-data",
            "luau-lsp.platform.luduvo.context.enabled": true,
            "luau-lsp.platform.luduvo.context.autoDump": true,
            "luau-lsp.platform.luduvo.context.lazyRefresh": false,
            "luau-lsp.platform.luduvo.notifications": {"context.updated": false}
        }
    )DOTTED_CONFIG");

    const auto& resources = output["platform"]["luduvo"]["resources"];
    REQUIRE(resources["groups"].contains("animation-state"));
    CHECK(resources["groups"]["animation-state"]["sources"] == json::array({"default-states"}));
    CHECK(resources["groups"]["prefab"]["includeBuiltins"]);
    CHECK(resources["sources"]["default-states"]["values"] == json::array({"Idle", "Running"}));
    CHECK(resources["sources"]["project-prefabs"]["values"][0]["documentation"] == "Project test prefab");
    CHECK(resources["bindings"][0]["parameter"] == "components");
    CHECK(resources["bindings"][0]["variadic"]);
    CHECK(resources["bindings"][0]["pluralResourceNoun"]);
    CHECK_FALSE(resources["completion"]["enabled"]);
    CHECK_EQ(output["platform"]["luduvo"]["bindings"][0]["kind"], "color");
    CHECK_EQ(output["platform"]["luduvo"]["bindings"][0]["encoding"], "hex");
    CHECK_EQ(output["platform"]["luduvo"]["bindings"][0]["target"]["parameter"], "hex");
    CHECK_EQ(output["platform"]["luduvo"]["directories"]["roaming"], "roaming-data");
    CHECK(output["platform"]["luduvo"]["context"]["autoDump"]);
    CHECK_FALSE(output["platform"]["luduvo"]["context"]["lazyRefresh"]);
    CHECK_FALSE(output["platform"]["luduvo"]["notifications"]["context.updated"]);
    CHECK_EQ(output["platform"]["luduvo"]["directories"]["local"], "local-data");
    CHECK(output["platform"]["luduvo"]["context"]["enabled"]);
}

TEST_SUITE_END();
