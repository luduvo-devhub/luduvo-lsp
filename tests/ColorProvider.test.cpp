#include "doctest.h"
#include "LSP/ColorProvider.hpp"
#include "Fixture.h"

void checkRGB(const RGB& lhs, const RGB& rhs)
{
    CHECK(lhs.r == doctest::Approx(rhs.r));
    CHECK(lhs.g == doctest::Approx(rhs.g));
    CHECK(lhs.b == doctest::Approx(rhs.b));
}

void checkHSV(const HSV& lhs, const HSV& rhs)
{
    CHECK(lhs.h == doctest::Approx(rhs.h));
    CHECK(lhs.s == doctest::Approx(rhs.s));
    CHECK(lhs.v == doctest::Approx(rhs.v));
}

TEST_SUITE_BEGIN("ColorProvider");

TEST_CASE("RGB to HSV conversions")
{
    checkHSV(rgbToHsv({0, 0, 0}), {0, 0, 0});
    checkHSV(rgbToHsv({255, 255, 255}), {0, 0, 1});
}

TEST_CASE("HSV to RGB conversions")
{
    checkRGB(hsvToRgb({0, 0, 0}), {0, 0, 0});
    checkRGB(hsvToRgb({0, 0, 1}), {255, 255, 255});
}

TEST_CASE("Hex to RGB conversions")
{
    checkRGB(hexToRgb("#000000"), {0, 0, 0});
    checkRGB(hexToRgb("#ffffff"), {255, 255, 255});
}

TEST_CASE("RGB to Hex conversions")
{
    CHECK_EQ(rgbToHex({0, 0, 0}), "#000000");
    CHECK_EQ(rgbToHex({255, 255, 255}), "#ffffff");
}

TEST_CASE_FIXTURE(Fixture, "color_presentation_uses_simplified_numbers")
{
    const Uri uri = newDocument("color-presentations.luau", "");
    auto presentation = workspace.colorPresentation(lsp::ColorPresentationParams{{uri}, lsp::Color{0.0, 0.0, 0.0}});
    REQUIRE_EQ(presentation.size(), 4);
    CHECK_EQ(presentation[0].label, "Color3.new(0, 0, 0)");
    CHECK_EQ(presentation[1].label, "Color3.fromRGB(0, 0, 0)");
    CHECK_EQ(presentation[2].label, "Color3.fromHSV(0, 0, 0)");
    CHECK_EQ(presentation[3].label, "Color3.fromHex(\"#000000\")");

    presentation = workspace.colorPresentation(lsp::ColorPresentationParams{{uri}, lsp::Color{1.0, 1.0, 1.0}});
    REQUIRE_EQ(presentation.size(), 4);
    CHECK_EQ(presentation[0].label, "Color3.new(1, 1, 1)");
    CHECK_EQ(presentation[1].label, "Color3.fromRGB(255, 255, 255)");
    CHECK_EQ(presentation[2].label, "Color3.fromHSV(0, 0, 1)");
    CHECK_EQ(presentation[3].label, "Color3.fromHex(\"#ffffff\")");

    presentation = workspace.colorPresentation(lsp::ColorPresentationParams{{uri}, lsp::Color{0.25, 0.25, 0.25}});
    REQUIRE_EQ(presentation.size(), 4);
    CHECK_EQ(presentation[0].label, "Color3.new(0.25, 0.25, 0.25)");
    CHECK_EQ(presentation[1].label, "Color3.fromRGB(63, 63, 63)");
    CHECK_EQ(presentation[2].label, "Color3.fromHSV(0, 0, 0.247059)");
    CHECK_EQ(presentation[3].label, "Color3.fromHex(\"#3f3f3f\")");
}

TEST_CASE("luduvo_color_bindings_find_builtin_encodings")
{
    LuduvoFixture fixture("luduvo_color_bindings");
    const Uri uri = fixture.newDocument("colors.client.luau", R"(
local a = Color3.new(1, 0.5, 0)
local b = Color3.fromRGB(0, 128, 255)
local c = Color3.fromHSV(0, 1, 1)
local d = Color3.fromHex("#00ff00")
local invalid = Color3.fromHex("#f")
)");

    const auto colors = fixture.workspace.documentColor({{uri}});
    REQUIRE_EQ(colors.size(), 4);
    CHECK(colors[0].color.red == doctest::Approx(1));
    CHECK(colors[0].color.green == doctest::Approx(0.5));
    CHECK(colors[1].color.green == doctest::Approx(128.0 / 255.0));
    CHECK(colors[2].color.red == doctest::Approx(1));
    CHECK(colors[3].color.green == doctest::Approx(1));

    const auto presentations = fixture.workspace.colorPresentation({{uri}, lsp::Color{1.0, 0.0, 0.0}, colors[3].range});
    REQUIRE_EQ(presentations.size(), 4);
    CHECK_EQ(presentations[0].label, "Color3.new(1, 0, 0)");
    CHECK_EQ(presentations[3].label, "Color3.fromHex(\"#ff0000\")");
}

TEST_CASE("luduvo_custom_color_binding_preserves_its_symbol_in_presentations")
{
    const std::string definitions = "declare Palette: { fromHex: (hex: string) -> number }";
    ClientConfiguration config = Luau::LanguageServer::defaultTestClientConfiguration();
    config.platform.luduvo.bindings.push_back({"Palette.fromHex", "color", "hex", {}, {}, "hex"});

    LuduvoFixture fixture("luduvo_custom_color_presentation", config, definitions, definitions);
    const Uri uri = fixture.newDocument("custom-color.client.luau", "local color = Palette.fromHex(\"#ff0000\")");
    const auto colors = fixture.workspace.documentColor({{uri}});
    REQUIRE_EQ(colors.size(), 1);

    const auto presentations = fixture.workspace.colorPresentation({{uri}, lsp::Color{0.0, 1.0, 0.0}, colors[0].range});
    REQUIRE_EQ(presentations.size(), 1);
    CHECK_EQ(presentations[0].label, "Palette.fromHex(\"#00ff00\")");
}

TEST_CASE("luduvo_color_bindings_resolve_named_parameters")
{
    const std::string definitions = R"(
        declare Palette: {
            make: (label: string, r: number, g: number, b: number) -> number,
        }
    )";
    ClientConfiguration config = Luau::LanguageServer::defaultTestClientConfiguration();
    config.platform.luduvo.bindings.push_back({"Palette.make", "color", {}, {"r", "g", "b"}, {}, "rgb", false, false});

    LuduvoFixture fixture("luduvo_custom_color_parameters", config, definitions, definitions);
    const Uri uri = fixture.newDocument("custom-color.client.luau", "local color = Palette.make(\"accent\", 1, 0.5, 0)");
    const auto colors = fixture.workspace.documentColor({{uri}});
    REQUIRE_EQ(colors.size(), 1);
    CHECK(colors[0].color.red == doctest::Approx(1));
    CHECK(colors[0].color.green == doctest::Approx(0.5));
    CHECK(colors[0].color.blue == doctest::Approx(0));

    const auto presentations = fixture.workspace.colorPresentation({{uri}, lsp::Color{0.0, 1.0, 0.0}, colors[0].range});
    REQUIRE_EQ(presentations.size(), 1);
    REQUIRE(presentations[0].textEdit);
    std::vector<lsp::TextEdit> edits{*presentations[0].textEdit};
    REQUIRE(presentations[0].additionalTextEdits);
    edits.insert(edits.end(), presentations[0].additionalTextEdits->begin(), presentations[0].additionalTextEdits->end());
    CHECK_EQ(applyEdit("local color = Palette.make(\"accent\", 1, 0.5, 0)", edits), "local color = Palette.make(\"accent\", 0, 1, 0)");
}

TEST_SUITE_END();
