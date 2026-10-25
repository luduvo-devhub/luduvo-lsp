#include "LSP/CommentDirectiveCompletion.hpp"

#include "Luau/LinterConfig.h"

#include <array>
#include <string_view>

namespace
{
struct Directive
{
    std::string_view name;
    std::string_view documentation;
};

constexpr std::array directives = {
    Directive{"nolint", "Suppress all Luau lint warnings, or one named lint rule."},
    Directive{"nocheck", "Disable type checking for this file."},
    Directive{"nonstrict", "Use nonstrict type checking for this file."},
    Directive{"strict", "Use strict type checking for this file."},
    Directive{"optimize", "Select optimization level 0, 1, or 2."},
    Directive{"native", "Enable native code generation for this file."},
};

lsp::Range replacementRange(const TextDocument& document, size_t line, size_t begin, size_t end)
{
    return {document.convertPosition(Luau::Position{static_cast<unsigned int>(line), static_cast<unsigned int>(begin)}),
        document.convertPosition(Luau::Position{static_cast<unsigned int>(line), static_cast<unsigned int>(end)})};
}

lsp::CompletionItem item(std::string label, std::string detail, std::string documentation, const lsp::Range& range)
{
    lsp::CompletionItem result;
    result.label = std::move(label);
    result.kind = lsp::CompletionItemKind::Keyword;
    result.detail = std::move(detail);
    if (!documentation.empty())
        result.documentation = lsp::MarkupContent{lsp::MarkupKind::Markdown, std::move(documentation)};
    result.textEdit = lsp::TextEdit{range, result.label};
    return result;
}
} // namespace

std::optional<std::vector<lsp::CompletionItem>> completeCommentDirective(
    const TextDocument& document, const Luau::SourceModule& sourceModule, const lsp::Position& position, const LSPPlatform& platform)
{
    const Luau::Position cursor = document.convertPosition(position);
    if (!Luau::isWithinHotComment(sourceModule, cursor))
        return std::nullopt;
    const std::string line = document.getLine(cursor.line);
    if (cursor.column > line.size())
        return std::nullopt;

    const size_t commentStart = line.find_first_not_of(" \t");
    if (commentStart == std::string::npos || line.compare(commentStart, 3, "--!") != 0)
        return std::nullopt;

    const size_t directiveStart = commentStart + 3;
    const size_t separator = line.find_first_of(" \t", directiveStart);
    if (separator == std::string::npos || cursor.column <= separator)
    {
        const size_t directiveEnd = line.find_first_of(" \t", directiveStart);
        const lsp::Range range =
            replacementRange(document, cursor.line, directiveStart, directiveEnd == std::string::npos ? line.size() : directiveEnd);

        std::vector<lsp::CompletionItem> result;
        result.reserve(directives.size());
        for (const Directive& directive : directives)
            result.push_back(item(std::string(directive.name), "Luau comment directive", std::string(directive.documentation), range));
        return result;
    }

    const std::string_view directive(line.data() + directiveStart, separator - directiveStart);
    const size_t firstArgument = line.find_first_not_of(" \t", separator);
    const bool cursorBeforeArgument = firstArgument == std::string::npos || cursor.column < firstArgument;
    const size_t argumentStart = cursorBeforeArgument ? cursor.column : firstArgument;
    size_t argumentEnd = cursorBeforeArgument ? cursor.column : line.find_first_of(" \t", firstArgument);
    if (argumentEnd == std::string::npos)
        argumentEnd = line.size();
    const lsp::Range range = replacementRange(document, cursor.line, argumentStart, argumentEnd);

    std::vector<lsp::CompletionItem> result;
    if (directive == "nolint")
    {
        const std::vector<PlatformLintRuleCompletion> platformRules = platform.getLintRuleCompletions();
        result.reserve(static_cast<size_t>(Luau::LintWarning::Code__Count - 1) + platformRules.size());
        for (size_t index = 1; index < static_cast<size_t>(Luau::LintWarning::Code__Count); ++index)
            result.push_back(item(Luau::kWarningNames[index], "Luau lint rule", "Suppress this Luau lint rule for the file.", range));
        for (const PlatformLintRuleCompletion& rule : platformRules)
            result.push_back(item(rule.name, rule.detail, rule.documentation, range));
        return result;
    }

    if (directive == "optimize")
    {
        for (const std::string level : {"0", "1", "2"})
            result.push_back(item(level, "Luau optimization level", "Use optimization level " + level + " for this file.", range));
        return result;
    }

    for (const Directive& known : directives)
    {
        if (directive == known.name)
            return result;
    }
    return std::nullopt;
}
