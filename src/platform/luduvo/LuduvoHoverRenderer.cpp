#include "Platform/LuduvoHoverRenderer.hpp"

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
        const size_t newline = text.find('\n');
        if (newline != std::string_view::npos && text.back() == '}')
            return std::string(text.substr(0, newline)) + "\n    -- ...\n}";

        const bool declaration = text.substr(0, 6) == "local " || text.substr(0, 6) == "const " || text.substr(0, 8) == "declare " ||
                                 text.substr(0, 9) == "function " || text.substr(0, 5) == "type " || text.find(": ") != std::string_view::npos;
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

std::string renderResourceConstraints(const std::vector<LuduvoResolvedResourceBinding>& bindings)
{
    std::string result;
    for (const LuduvoResolvedResourceBinding& binding : bindings)
    {
        if (!result.empty())
            result += "\n\n";
        result += "`" + binding.parameter + ": string` expects ";
        if (binding.pluralResourceNoun)
            result += "Luduvo " + binding.kind + " names.";
        else
            result += "a Luduvo " + binding.kind + " name.";
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
        const size_t fixedLength = visibleLength(model.provenance) + (linked ? 2 : 0);
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

    std::string typeDefinition = model.typeDefinition;
    if (config.maxTypeDefinitionLength)
        typeDefinition = truncateTypeDefinition(typeDefinition, *config.maxTypeDefinitionLength);

    const bool hasVisibleDocumentation = !model.documentation.empty() || model.learnMoreLink.has_value();
    if (model.showTypeDefinition && model.tags.empty() && !hasVisibleDocumentation && model.constrainedResources.empty() && model.provenance.empty())
        return codeBlock("luau", typeDefinition);

    std::string result = linked ? "# [" + title + " ](" + *model.learnMoreLink + ")↗" : "# " + title;
    result += model.provenance;
    if (!model.tags.empty())
    {
        result += "\n";
        for (size_t index = 0; index < model.tags.size(); ++index)
        {
            if (index > 0)
                result += " · ";
            result += "`" + model.tags[index] + "`";
        }
    }
    if (!model.documentation.empty())
        result += "\n\n" + model.documentation;

    const std::string resourceConstraints = renderResourceConstraints(model.constrainedResources);
    if (!resourceConstraints.empty())
        result += "\n\n" + resourceConstraints;
    if (!model.showTypeDefinition)
        return result;
    return result + kDocumentationBreaker + codeBlock("luau", typeDefinition);
}

} // namespace luduvo::hover
