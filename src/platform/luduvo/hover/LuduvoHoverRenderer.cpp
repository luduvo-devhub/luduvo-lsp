#include "Platform/luduvo/hover/LuduvoHoverRenderer.hpp"
#include "Platform/luduvo/LuduvoResourceCatalog.hpp"
#include "LSP/Completion.hpp"

#include "LSP/DocumentationParser.hpp"

#include <algorithm>
#include <string_view>

namespace luduvo::hover
{
namespace
{
size_t visibleLength(std::string_view text)
{
    size_t result = 0;
    for (unsigned char character : text)
    {
        if ((character & 0xc0) != 0x80)
            ++result;
    }
    return result;
}

std::string truncateVisible(std::string_view text, size_t maximumLength)
{
    if (visibleLength(text) <= maximumLength)
        return std::string(text);
    if (maximumLength <= 3)
        return std::string(maximumLength, '.');

    size_t codepoints = 0;
    size_t byteLength = 0;
    const size_t retainedCodepoints = maximumLength - 3;
    while (byteLength < text.size() && codepoints < retainedCodepoints)
    {
        ++byteLength;
        while (byteLength < text.size() && (static_cast<unsigned char>(text[byteLength]) & 0xc0) == 0x80)
            ++byteLength;
        ++codepoints;
    }
    return std::string(text.substr(0, byteLength)) + "...";
}

std::string truncateVisibleMiddle(std::string_view text, size_t maximumLength)
{
    if (visibleLength(text) <= maximumLength)
        return std::string(text);
    if (maximumLength <= 3)
        return std::string(maximumLength, '.');

    const size_t retained = maximumLength - 3;
    const size_t prefixCodepoints = (retained + 1) / 2;
    const size_t suffixCodepoints = retained / 2;

    size_t prefixBytes = 0;
    for (size_t codepoints = 0; prefixBytes < text.size() && codepoints < prefixCodepoints; ++codepoints)
    {
        ++prefixBytes;
        while (prefixBytes < text.size() && (static_cast<unsigned char>(text[prefixBytes]) & 0xc0) == 0x80)
            ++prefixBytes;
    }

    size_t suffixBegin = text.size();
    for (size_t codepoints = 0; suffixBegin > 0 && codepoints < suffixCodepoints; ++codepoints)
    {
        --suffixBegin;
        while (suffixBegin > 0 && (static_cast<unsigned char>(text[suffixBegin]) & 0xc0) == 0x80)
            --suffixBegin;
    }
    return std::string(text.substr(0, prefixBytes)) + "..." + std::string(text.substr(suffixBegin));
}

std::string truncateTypeDefinition(std::string_view text, size_t maximumLength)
{
    if (visibleLength(text) <= maximumLength)
        return std::string(text);

    constexpr std::string_view externPrefix = "declare extern type ";
    const size_t externBegin = text.find(externPrefix);
    if (externBegin == std::string_view::npos || text.size() < 4 || text.substr(text.size() - 4) != "\nend")
    {
        if (text.substr(0, 9) == "function " || text.find(" where ") != std::string_view::npos)
            return std::string(text);
        const size_t newline = text.find('\n');
        if (newline != std::string_view::npos && text.back() == '}')
            return std::string(text.substr(0, newline)) + "\n    -- ...\n}";

        const bool declaration = text.substr(0, 6) == "local " || text.substr(0, 6) == "const " || text.substr(0, 8) == "declare " ||
                                 text.substr(0, 5) == "type " || text.find(": ") != std::string_view::npos;
        return declaration ? std::string(text) : truncateVisibleMiddle(text, maximumLength);
    }

    const std::string prefix(text.substr(0, externBegin));
    text.remove_prefix(externBegin);
    const size_t externBudget = maximumLength > visibleLength(prefix) ? maximumLength - visibleLength(prefix) : 0;

    std::vector<std::string_view> lines;
    for (size_t begin = 0; begin <= text.size();)
    {
        const size_t end = text.find('\n', begin);
        lines.push_back(text.substr(begin, end == std::string_view::npos ? text.size() - begin : end - begin));
        if (end == std::string_view::npos)
            break;
        begin = end + 1;
    }

    const std::string marker = "\n    -- ...\nend";
    std::string result(lines.front());
    for (size_t index = 1; index + 1 < lines.size();)
    {
        size_t next = index + 1;
        while (next + 1 < lines.size() && lines[next].substr(0, 8) == "        ")
            ++next;

        std::string block;
        for (size_t line = index; line < next; ++line)
            block += "\n" + std::string(lines[line]);
        if (visibleLength(result) + visibleLength(block) + visibleLength(marker) > externBudget)
            break;
        result += block;
        index = next;
    }
    return prefix + result + marker;
}

} // namespace

static std::string renderResourceConstraint(const LuduvoResolvedResourceBinding& binding, bool markdown)
{
    std::string result = binding.parameter + ": string";
    if (markdown)
        result = "`" + result + "`";
    result += " expects ";
    if (binding.pluralResourceNoun)
        result += "Luduvo " + binding.kind + " names.";
    else
        result += "a Luduvo " + binding.kind + " name.";
    return result;
}

static std::string renderResourceConstraints(const std::vector<LuduvoResolvedResourceBinding>& bindings)
{
    std::string result;
    for (size_t index = 0; index < bindings.size(); ++index)
    {
        const LuduvoResolvedResourceBinding& binding = bindings[index];
        const auto duplicate = std::find_if(bindings.begin(), bindings.begin() + index,
            [&](const LuduvoResolvedResourceBinding& existing)
            {
                return existing.parameter == binding.parameter && existing.kind == binding.kind &&
                       existing.pluralResourceNoun == binding.pluralResourceNoun;
            });
        if (duplicate != bindings.begin() + index)
            continue;

        if (!result.empty())
            result += "\n\n";
        result += renderResourceConstraint(binding, true);
    }
    return result;
}

static std::string renderOverloadBlock(const RichHoverOverload& overload)
{
    std::string result;
    if (overload.callStyle)
        result += "-- " + *overload.callStyle + "\n";
    for (const LuduvoResolvedResourceBinding& binding : overload.constrainedResources)
        result += "-- " + renderResourceConstraint(binding, false) + "\n";
    return result + overload.declaration;
}

static std::string renderOverloads(const std::vector<RichHoverOverload>& overloads, const std::optional<size_t>& maximumLength)
{
    std::string result;
    for (size_t index = 0; index < overloads.size(); ++index)
    {
        const std::string block = renderOverloadBlock(overloads[index]);
        const std::string separator = result.empty() ? "" : "\n\n";
        if (!result.empty() && maximumLength)
        {
            const size_t omitted = overloads.size() - index;
            const std::string marker = "\n\n-- +" + std::to_string(omitted) + " overload" + (omitted == 1 ? "" : "s");
            if (visibleLength(result) + visibleLength(separator) + visibleLength(block) + visibleLength(marker) > *maximumLength)
            {
                result += marker;
                break;
            }
        }
        result += separator + block;
    }
    return result;
}

std::string renderRichHover(const ClientLuduvoHoverConfiguration& config, const RichHoverRenderModel& model)
{
    std::string title = model.title;
    std::replace(title.begin(), title.end(), '\n', ' ');
    std::replace(title.begin(), title.end(), '\r', ' ');
    const bool linked = model.learnMoreLink.has_value();
    if (config.maxTitleLength)
    {
        const size_t fixedLength = linked ? 2 : 0;
        const size_t titleBudget = *config.maxTitleLength > fixedLength ? *config.maxTitleLength - fixedLength : 3;
        if (model.unionSummary && (model.unionSummary->multiline || visibleLength(title) > titleBudget))
        {
            const std::string countSuffix = " +" + std::to_string(model.unionSummary->additionalMembers) + " union member" +
                                            (model.unionSummary->additionalMembers == 1 ? "" : "s");
            const size_t firstMemberBudget = titleBudget > visibleLength(countSuffix) ? titleBudget - visibleLength(countSuffix) : 3;
            title = truncateVisible(model.unionSummary->firstMember, firstMemberBudget) + countSuffix;
        }
        else
            title = truncateVisible(title, titleBudget);
    }

    std::string result;
    bool dividerPending = false;
    const auto append = [&](const std::string& markdown)
    {
        if (markdown.empty())
            return;
        if (!result.empty())
            result += dividerPending ? kDocumentationBreaker : "\n\n";
        result += markdown;
        dividerPending = false;
    };

    const bool showTitle = model.hasDocumentationIdentity || linked || !model.tags.empty() || !model.documentation.empty() ||
                           !model.constrainedResources.empty() || !model.showTypeDefinition;
    for (const auto& section : config.sections)
    {
        if (section == "divider")
            dividerPending = !result.empty();
        else if (section == "title" && showTitle)
            append(linked ? "# [" + title + " ](" + *model.learnMoreLink + ")↗" : "# " + title);
        else if (section == "tags")
        {
            std::string tags;
            for (const auto& tag : model.tags)
            {
                if (!tags.empty())
                    tags += " · ";
                tags += "`" + tag + "`";
            }
            append(tags);
        }
        else if (section == "deprecation" && model.deprecated && !model.deprecation.empty())
            append("**Deprecated** " + model.deprecation);
        else if (section == "summary")
            append(model.documentation);
        else if (section == "resources")
            append(renderResourceConstraints(model.constrainedResources));
        else if (section == "type" && model.showTypeDefinition)
        {
            std::string definition = model.overloads.empty() ? model.typeDefinition : renderOverloads(model.overloads, config.maxTypeDefinitionLength);
            if (model.overloads.empty() && config.maxTypeDefinitionLength)
                definition = truncateTypeDefinition(definition, *config.maxTypeDefinitionLength);
            append(codeBlock("luau", definition));
        }
        else if (section == "default" && !model.documentedDefault.empty())
            append("> Default: " + model.documentedDefault);
        else if (section == "notes" && !model.notes.empty())
        {
            std::string notes = "**Notes**";
            for (const auto& note : model.notes)
                notes += "\n- " + note;
            append(notes);
        }
        else if (section == "example" && !model.example.empty())
            append(codeBlock("luau", model.example));
    }
    return result;
}

void applyResourceCompletionDocumentation(const ClientLuduvoHoverConfiguration& config, lsp::CompletionItem& item,
    std::string_view kind, const LuduvoResourceName& resource)
{
    item.sortText = std::string(resource.deprecated ? SortText::Deprioritized : SortText::Default) + resource.name;
    if (resource.deprecated)
    {
        item.deprecated = true;
        item.tags = std::vector{lsp::CompletionItemTag::Deprecated};
    }

    RichHoverRenderModel model;
    model.title = resource.name;
    model.tags.emplace_back(kind);
    if (resource.builtIn)
        model.tags.emplace_back("built-in");
    if (resource.deprecated)
        model.tags.emplace_back("deprecated");
    model.documentation = resource.documentation.value_or("");
    model.deprecated = resource.deprecated;
    model.deprecation = resource.deprecation;
    model.notes = resource.notes;
    model.example = resource.example;
    model.showTypeDefinition = false;

    auto presentation = config;
    if (config.presentation == LuduvoHoverPresentation::Standard)
        presentation.sections.erase(std::remove_if(presentation.sections.begin(), presentation.sections.end(),
            [](const std::string& section) { return section == "title" || section == "tags"; }), presentation.sections.end());
    if (auto markdown = renderRichHover(presentation, model); !markdown.empty())
        item.documentation = lsp::MarkupContent{lsp::MarkupKind::Markdown, std::move(markdown)};
}

} // namespace luduvo::hover
