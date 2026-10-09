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
            "luau-lsp.platform.luduvo.resources.overrides": {
                "component": ["Health"],
                "prefab": []
            },
            "luau-lsp.platform.luduvo.resources.bindings": [
                {"symbol": "MyLibrary.Spawn", "parameter": "name", "kind": "prefab"}
            ],
            "luau-lsp.platform.luduvo.resources.completion.enabled": false
        }
    )DOTTED_CONFIG");

    const auto& resources = output["platform"]["luduvo"]["resources"];
    REQUIRE(resources["overrides"].contains("component"));
    CHECK(resources["overrides"]["component"] == json::array({"Health"}));
    REQUIRE(resources["overrides"].contains("prefab"));
    CHECK(resources["overrides"]["prefab"].empty());
    CHECK(resources["bindings"][0]["parameter"] == "name");
    CHECK_FALSE(resources["completion"]["enabled"]);
}

TEST_SUITE_END();
