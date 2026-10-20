#include "Platform/LuduvoPlatform.hpp"

#include "LSP/ColorProvider.hpp"
#include "Platform/ColorPresentation.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace
{
std::optional<std::string> expressionSymbol(const Luau::AstExpr* expression)
{
    if (const auto global = expression->as<Luau::AstExprGlobal>())
        return std::string(global->name.value);
    if (const auto index = expression->as<Luau::AstExprIndexName>())
    {
        auto parent = expressionSymbol(index->expr);
        if (parent)
            return *parent + "." + std::string(index->index.value);
    }
    return std::nullopt;
}

std::optional<std::array<double, 3>> numericArguments(const Luau::AstExprCall* call)
{
    if (call->args.size != 3)
        return std::nullopt;
    std::array<double, 3> values{};
    for (size_t index = 0; index < values.size(); ++index)
    {
        const auto number = call->args.data[index]->as<Luau::AstExprConstantNumber>();
        if (!number)
            return std::nullopt;
        values[index] = number->value;
    }
    return values;
}

struct LuduvoColorVisitor : Luau::AstVisitor
{
    const TextDocument& textDocument;
    const std::vector<LuduvoColorBinding>& bindings;
    std::vector<lsp::ColorInformation> colors;

    LuduvoColorVisitor(const TextDocument& textDocument, const std::vector<LuduvoColorBinding>& bindings)
        : textDocument(textDocument)
        , bindings(bindings)
    {
    }

    bool visit(Luau::AstExprCall* call) override
    {
        const auto symbol = expressionSymbol(call->func);
        if (!symbol)
            return true;
        const auto binding = std::find_if(bindings.begin(), bindings.end(),
            [&](const LuduvoColorBinding& candidate)
            {
                return candidate.symbol == *symbol;
            });
        if (binding == bindings.end())
            return true;

        std::optional<lsp::Color> color;
        if (binding->encoding == ColorPresentationEncoding::Hex)
        {
            if (call->args.size != 1)
                return true;
            const auto string = call->args.data[0]->as<Luau::AstExprConstantString>();
            if (!string)
                return true;
            try
            {
                std::string hex(string->value.data, string->value.size);
                if (!hex.empty() && hex.front() == '#')
                    hex.erase(hex.begin());
                if (hex.size() != 6 || !std::all_of(hex.begin(), hex.end(),
                                           [](unsigned char character)
                                           {
                                               return std::isxdigit(character);
                                           }))
                    return true;
                const RGB rgb = hexToRgb(hex);
                color = lsp::Color{rgb.r / 255.0, rgb.g / 255.0, rgb.b / 255.0, 1.0};
            }
            catch (const std::exception&)
            {
                return true;
            }
        }
        else
        {
            auto values = numericArguments(call);
            if (!values)
                return true;
            if (binding->encoding == ColorPresentationEncoding::Rgb)
                color = lsp::Color{(*values)[0], (*values)[1], (*values)[2], 1.0};
            else if (binding->encoding == ColorPresentationEncoding::Rgb255)
                color = lsp::Color{(*values)[0] / 255.0, (*values)[1] / 255.0, (*values)[2] / 255.0, 1.0};
            else
            {
                const RGB rgb = hsvToRgb({(*values)[0], (*values)[1], (*values)[2]});
                color = lsp::Color{rgb.r / 255.0, rgb.g / 255.0, rgb.b / 255.0, 1.0};
            }
        }

        color->red = std::clamp(color->red, 0.0, 1.0);
        color->green = std::clamp(color->green, 0.0, 1.0);
        color->blue = std::clamp(color->blue, 0.0, 1.0);
        color->alpha = std::clamp(color->alpha, 0.0, 1.0);
        colors.push_back({{textDocument.convertPosition(call->location.begin), textDocument.convertPosition(call->location.end)}, *color});
        return true;
    }

    bool visit(Luau::AstStatBlock* block) override
    {
        for (Luau::AstStat* statement : block->body)
            statement->visit(this);
        return false;
    }
};
} // namespace

lsp::DocumentColorResult LuduvoPlatform::documentColor(const TextDocument& textDocument, const Luau::SourceModule& module)
{
    LuduvoColorVisitor visitor{textDocument, bindings.colors()};
    module.root->visit(&visitor);
    return visitor.colors;
}

namespace
{
struct ColorCallAtRangeVisitor : Luau::AstVisitor
{
    const TextDocument& textDocument;
    const lsp::Range& target;
    std::optional<std::string> symbol;

    ColorCallAtRangeVisitor(const TextDocument& textDocument, const lsp::Range& target)
        : textDocument(textDocument)
        , target(target)
    {
    }

    bool visit(Luau::AstExprCall* call) override
    {
        const lsp::Range range{textDocument.convertPosition(call->location.begin), textDocument.convertPosition(call->location.end)};
        if (range == target)
            symbol = expressionSymbol(call->func);
        return !symbol;
    }

    bool visit(Luau::AstStatBlock* block) override
    {
        for (Luau::AstStat* statement : block->body)
        {
            if (symbol)
                break;
            statement->visit(this);
        }
        return false;
    }
};

bool isBuiltinColor3Symbol(std::string_view symbol)
{
    return symbol == "Color3.new" || symbol == "Color3.fromRGB" || symbol == "Color3.fromHSV" || symbol == "Color3.fromHex"; // Todo: Change to something more scaleable if this gets ugly
}
} // namespace

lsp::ColorPresentationResult LuduvoPlatform::colorPresentation(const PlatformColorPresentationContext& context)
{
    ColorCallAtRangeVisitor visitor{context.textDocument, context.params.range};
    context.sourceModule.root->visit(&visitor);
    if (!visitor.symbol)
        return {};

    const auto binding = std::find_if(bindings.colors().begin(), bindings.colors().end(),
        [&](const LuduvoColorBinding& candidate)
        {
            return candidate.symbol == *visitor.symbol;
        });
    if (binding == bindings.colors().end())
        return {};

    if (isBuiltinColor3Symbol(binding->symbol))
        return color3Presentations(context.params.color);
    return {colorCallPresentation(binding->symbol, binding->encoding, context.params.color)};
}
