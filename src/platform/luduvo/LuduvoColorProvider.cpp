#include "Platform/luduvo/LuduvoPlatform.hpp"

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

std::optional<size_t> callArgumentIndex(const Luau::AstExprCall* call, size_t typeParameterIndex)
{
    const size_t implicitParameters = call->self ? 1 : 0;
    if (typeParameterIndex < implicitParameters)
        return std::nullopt;
    const size_t argumentIndex = typeParameterIndex - implicitParameters;
    if (argumentIndex >= call->args.size)
        return std::nullopt;
    return argumentIndex;
}

std::optional<std::array<double, 3>> numericArguments(const Luau::AstExprCall* call, const std::vector<size_t>& typeParameterIndices)
{
    if (typeParameterIndices.size() != 3)
        return std::nullopt;
    std::array<double, 3> values{};
    for (size_t index = 0; index < values.size(); ++index)
    {
        const auto argumentIndex = callArgumentIndex(call, typeParameterIndices[index]);
        if (!argumentIndex)
            return std::nullopt;
        const auto number = call->args.data[*argumentIndex]->as<Luau::AstExprConstantNumber>();
        if (!number)
            return std::nullopt;
        values[index] = number->value;
    }
    return values;
}

struct LuduvoColorVisitor : Luau::AstVisitor
{
    const TextDocument& textDocument;
    const LuduvoBindingRegistry& bindings;
    const Luau::ModulePtr& module;
    std::vector<lsp::ColorInformation> colors;

    LuduvoColorVisitor(const TextDocument& textDocument, const LuduvoBindingRegistry& bindings, const Luau::ModulePtr& module)
        : textDocument(textDocument)
        , bindings(bindings)
        , module(module)
    {
    }

    bool visit(Luau::AstExprCall* call) override
    {
        const auto symbol = expressionSymbol(call->func);
        if (!symbol)
            return true;
        Luau::TypeId* callableType = module->astTypes.find(call->func);
        if (!callableType)
            return true;

        const auto resolvedBindings = bindings.resolveColors(*symbol, *callableType);
        for (const LuduvoResolvedColorBinding& resolved : resolvedBindings)
        {
            std::optional<lsp::Color> color;
            if (resolved.binding.encoding == ColorPresentationEncoding::Hex)
            {
                if (resolved.typeParameterIndices.size() != 1)
                    continue;
                const auto argumentIndex = callArgumentIndex(call, resolved.typeParameterIndices.front());
                if (!argumentIndex)
                    continue;
                const auto string = call->args.data[*argumentIndex]->as<Luau::AstExprConstantString>();
                if (!string)
                    continue;
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
                        continue;
                    const RGB rgb = hexToRgb(hex);
                    color = lsp::Color{rgb.r / 255.0, rgb.g / 255.0, rgb.b / 255.0, 1.0};
                }
                catch (const std::exception&)
                {
                    continue;
                }
            }
            else
            {
                auto values = numericArguments(call, resolved.typeParameterIndices);
                if (!values)
                    continue;
                if (resolved.binding.encoding == ColorPresentationEncoding::Rgb)
                    color = lsp::Color{(*values)[0], (*values)[1], (*values)[2], 1.0};
                else if (resolved.binding.encoding == ColorPresentationEncoding::Rgb255)
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
            break;
        }
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

lsp::DocumentColorResult LuduvoPlatform::documentColor(const PlatformDocumentColorContext& context)
{
    LuduvoColorVisitor visitor{context.textDocument, bindings, context.module};
    context.sourceModule.root->visit(&visitor);
    return visitor.colors;
}

namespace
{
struct ColorCallAtRangeVisitor : Luau::AstVisitor
{
    const TextDocument& textDocument;
    const lsp::Range& target;
    std::optional<std::string> symbol;
    Luau::AstExprCall* call = nullptr;

    ColorCallAtRangeVisitor(const TextDocument& textDocument, const lsp::Range& target)
        : textDocument(textDocument)
        , target(target)
    {
    }

    bool visit(Luau::AstExprCall* call) override
    {
        const lsp::Range range{textDocument.convertPosition(call->location.begin), textDocument.convertPosition(call->location.end)};
        if (range == target)
        {
            symbol = expressionSymbol(call->func);
            this->call = call;
        }
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
    return symbol == "Color3.new" || symbol == "Color3.fromRGB" || symbol == "Color3.fromHSV" ||
           symbol == "Color3.fromHex"; // Todo: Change to something more scaleable if this gets ugly
}
} // namespace

lsp::ColorPresentationResult LuduvoPlatform::colorPresentation(const PlatformColorPresentationContext& context)
{
    ColorCallAtRangeVisitor visitor{context.textDocument, context.params.range};
    context.sourceModule.root->visit(&visitor);
    if (!visitor.symbol || !visitor.call)
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

    Luau::TypeId* callableType = context.module->astTypes.find(visitor.call->func);
    if (!callableType)
        return {};
    for (const LuduvoResolvedColorBinding& resolved : bindings.resolveColors(*visitor.symbol, *callableType))
    {
        const std::vector<std::string> arguments = colorArguments(resolved.binding.encoding, context.params.color);
        if (arguments.size() != resolved.typeParameterIndices.size())
            continue;

        lsp::ColorPresentation presentation = colorCallPresentation(resolved.binding.symbol, resolved.binding.encoding, context.params.color);
        std::vector<lsp::TextEdit> edits;
        for (size_t index = 0; index < arguments.size(); ++index)
        {
            const auto argumentIndex = callArgumentIndex(visitor.call, resolved.typeParameterIndices[index]);
            if (!argumentIndex)
            {
                edits.clear();
                break;
            }
            edits.push_back({context.textDocument.convertLocation(visitor.call->args.data[*argumentIndex]->location), arguments[index]});
        }
        if (edits.empty())
            continue;
        presentation.textEdit = edits.front();
        if (edits.size() > 1)
            presentation.additionalTextEdits = std::vector<lsp::TextEdit>(edits.begin() + 1, edits.end());
        return {std::move(presentation)};
    }
    return {};
}
