#include "Platform/LuduvoHover.hpp"

#include "Luau/IterativeTypeVisitor.h"
#include "Luau/ToString.h"
#include "LuauFileUtils.hpp"

#include <string_view>
#include <vector>

namespace
{
struct UnlintableTypeFinder : Luau::IterativeTypeVisitor
{
    bool found = false;

    UnlintableTypeFinder()
        : IterativeTypeVisitor("UnlintableTypeFinder", /* visitOnce */ true, /* skipBoundTypes */ false)
    {
    }

    bool visit(Luau::TypeId, const Luau::AnyType&) override
    {
        found = true;
        return false;
    }

    bool visit(Luau::TypeId, const Luau::UnknownType&) override
    {
        found = true;
        return false;
    }

    bool visit(Luau::TypeId, const Luau::ErrorType&) override
    {
        found = true;
        return false;
    }
};

enum struct LuduvoDefinitionSide
{
    None,
    Server,
    Client,
};

struct LuduvoSymbolIdentity
{
    std::string title;
    LuduvoDefinitionSide side = LuduvoDefinitionSide::None;
};

struct LuduvoTypeProvenance
{
    bool server = false;
    bool client = false;
};

std::optional<LuduvoSymbolIdentity> getLuduvoSymbolIdentity(const std::optional<std::string>& documentationSymbol)
{
    if (!documentationSymbol)
        return std::nullopt;

    constexpr std::string_view serverPrefix = "@luduvo/server/";
    constexpr std::string_view clientPrefix = "@luduvo/client/";
    std::string_view symbol = *documentationSymbol;
    LuduvoDefinitionSide side = LuduvoDefinitionSide::None;

    if (symbol.substr(0, serverPrefix.size()) == serverPrefix)
    {
        symbol.remove_prefix(serverPrefix.size());
        side = LuduvoDefinitionSide::Server;
    }
    else if (symbol.substr(0, clientPrefix.size()) == clientPrefix)
    {
        symbol.remove_prefix(clientPrefix.size());
        side = LuduvoDefinitionSide::Client;
    }
    else
        return std::nullopt;

    constexpr std::string_view globalPrefix = "global/";
    constexpr std::string_view globalTypePrefix = "globaltype/";
    if (symbol.substr(0, globalPrefix.size()) == globalPrefix)
        symbol.remove_prefix(globalPrefix.size());
    else if (symbol.substr(0, globalTypePrefix.size()) == globalTypePrefix)
        symbol.remove_prefix(globalTypePrefix.size());
    else
        return std::nullopt;

    if (size_t overload = symbol.find("/overload/"); overload != std::string_view::npos)
        symbol = symbol.substr(0, overload);

    return LuduvoSymbolIdentity{std::string(symbol), side};
}

std::string versionLabel(LuduvoDefinitionSide side)
{
    switch (side)
    {
    case LuduvoDefinitionSide::Server:
        return " (Server Version)";
    case LuduvoDefinitionSide::Client:
        return " (Client Version)";
    case LuduvoDefinitionSide::None:
        return "";
    }
    return "";
}

void collectLuduvoTypeProvenance(Luau::TypeId type, LuduvoTypeProvenance& provenance)
{
    type = Luau::follow(type);
    if (auto identity = getLuduvoSymbolIdentity(type->documentationSymbol))
    {
        provenance.server |= identity->side == LuduvoDefinitionSide::Server;
        provenance.client |= identity->side == LuduvoDefinitionSide::Client;
        return;
    }

    if (auto unionType = Luau::get<Luau::UnionType>(type))
    {
        for (Luau::TypeId option : unionType->options)
            collectLuduvoTypeProvenance(option, provenance);
    }
    else if (auto intersectionType = Luau::get<Luau::IntersectionType>(type))
    {
        for (Luau::TypeId part : intersectionType->parts)
            collectLuduvoTypeProvenance(part, provenance);
    }
}

std::string versionLabel(const LuduvoTypeProvenance& provenance)
{
    if (provenance.server && provenance.client)
        return " (Client/Server Versions)";
    if (provenance.server)
        return " (Server Version)";
    if (provenance.client)
        return " (Client Version)";
    return "";
}

bool isUnlintable(Luau::TypeId type)
{
    UnlintableTypeFinder finder;
    finder.run(type);
    return finder.found;
}

bool isDeprecated(Luau::TypeId type)
{
    type = Luau::follow(type);
    if (auto functionType = Luau::get<Luau::FunctionType>(type))
        return functionType->isDeprecatedFunction;
    if (auto intersectionType = Luau::get<Luau::IntersectionType>(type))
    {
        if (intersectionType->parts.empty())
            return false;
        for (Luau::TypeId part : intersectionType->parts)
        {
            if (!isDeprecated(part))
                return false;
        }
        return true;
    }
    return false;
}

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

std::optional<std::pair<std::string, size_t>> getUnionSummary(Luau::TypeId type)
{
    type = Luau::follow(type);
    auto unionType = Luau::get<Luau::UnionType>(type);
    if (!unionType)
        return std::nullopt;

    std::vector<Luau::TypeId> members;
    for (Luau::TypeId option : unionType->options)
    {
        option = Luau::follow(option);
        if (auto primitive = Luau::get<Luau::PrimitiveType>(option); primitive && primitive->type == Luau::PrimitiveType::NilType)
            continue;
        members.push_back(option);
    }
    if (members.size() < 2)
        return std::nullopt;

    std::string firstMember = Luau::toString(members.front());
    if (auto identity = getLuduvoSymbolIdentity(members.front()->documentationSymbol))
        firstMember = identity->title;
    return std::make_pair(std::move(firstMember), members.size() - 1);
}

std::optional<std::string> retargetDocumentationSymbol(
    const std::optional<std::string>& documentationSymbol, const std::optional<std::string>& environmentName)
{
    auto identity = getLuduvoSymbolIdentity(documentationSymbol);
    if (!identity || !environmentName)
        return documentationSymbol;

    constexpr std::string_view serverPrefix = "@luduvo/server/";
    constexpr std::string_view clientPrefix = "@luduvo/client/";
    std::string_view symbol = *documentationSymbol;
    std::string_view replacement;
    if (*environmentName == "LuduvoClient")
        replacement = clientPrefix;
    else if (*environmentName == "LuduvoServer")
        replacement = serverPrefix;
    else
        return documentationSymbol;

    if (symbol.substr(0, serverPrefix.size()) == serverPrefix)
        symbol.remove_prefix(serverPrefix.size());
    else if (symbol.substr(0, clientPrefix.size()) == clientPrefix)
        symbol.remove_prefix(clientPrefix.size());
    return std::string(replacement) + std::string(symbol);
}
} // namespace

std::optional<std::string> resolveLuduvoHoverDocumentationSymbol(
    Luau::TypeId type, const std::optional<std::string>& directDocumentationSymbol, const std::optional<std::string>& environmentName)
{
    if (getLuduvoSymbolIdentity(directDocumentationSymbol))
        return retargetDocumentationSymbol(directDocumentationSymbol, environmentName);

    type = Luau::follow(type);
    if (getLuduvoSymbolIdentity(type->documentationSymbol))
        return retargetDocumentationSymbol(type->documentationSymbol, environmentName);

    auto unionType = Luau::get<Luau::UnionType>(type);
    if (!unionType)
        return std::nullopt;

    std::optional<std::string> resolved;
    for (Luau::TypeId option : unionType->options)
    {
        option = Luau::follow(option);
        if (auto primitive = Luau::get<Luau::PrimitiveType>(option); primitive && primitive->type == Luau::PrimitiveType::NilType)
            continue;

        if (!getLuduvoSymbolIdentity(option->documentationSymbol))
            return std::nullopt;
        if (resolved && *resolved != *option->documentationSymbol)
            return std::nullopt;
        resolved = option->documentationSymbol;
    }
    return retargetDocumentationSymbol(resolved, environmentName);
}

std::string renderLuduvoRichHover(const ClientLuduvoHoverConfiguration& config, const LuduvoRichHoverContext& context)
{
    auto identity = getLuduvoSymbolIdentity(context.documentationSymbol);
    std::string title = identity ? identity->title : context.fallbackTitle.value_or(context.typeDefinition);
    if (identity && context.preferLeafTitle)
    {
        if (size_t separator = title.rfind('.'); separator != std::string::npos)
            title.erase(0, separator + 1);
    }
    const bool linked = identity && context.documentation && context.documentation->learnMoreLink;
    LuduvoTypeProvenance typeProvenance;
    collectLuduvoTypeProvenance(context.type, typeProvenance);
    const std::string provenance = identity ? versionLabel(identity->side) : versionLabel(typeProvenance);
    if (!identity && (typeProvenance.server || typeProvenance.client))
        title = context.typeDefinition;
    if (config.maxTitleLength)
    {
        const size_t fixedLength = visibleLength(provenance) + (linked ? 2 : 0); // Linked headings render a space and ↗ after the title.
        const size_t titleBudget = *config.maxTitleLength > fixedLength ? *config.maxTitleLength - fixedLength : 3;
        if (auto summary = getUnionSummary(context.type); summary && visibleLength(title) > titleBudget)
        {
            const std::string countSuffix = " +" + std::to_string(summary->second) + " union member" + (summary->second == 1 ? "" : "s");
            const size_t firstMemberBudget = titleBudget > visibleLength(countSuffix) ? titleBudget - visibleLength(countSuffix) : 3;
            title = truncateVisible(summary->first, firstMemberBudget) + countSuffix;
        }
        else
            title = truncateVisible(title, titleBudget);
    }
    std::string heading;

    if (linked)
        heading = "# [" + title + " ](" + *context.documentation->learnMoreLink + ")↗";
    else
        heading = "# " + title;

    heading += provenance;

    std::vector<std::string> tags;
    bool deprecated = false;
    if (context.property)
    {
        if (context.property->isReadOnly())
            tags.emplace_back("read-only");
        else if (context.property->isWriteOnly())
            tags.emplace_back("write-only");
        deprecated = context.property->deprecated;
    }
    if (deprecated || isDeprecated(context.type))
        tags.emplace_back("deprecated");
    if (isUnlintable(context.type))
        tags.emplace_back("unlintable");

    std::string result = heading;
    if (!tags.empty())
    {
        result += "\n";
        for (size_t i = 0; i < tags.size(); ++i)
        {
            if (i > 0)
                result += " · ";
            result += "`" + tags[i] + "`";
        }
    }

    if (context.documentation && !context.documentation->markdown.empty())
    {
        std::string documentation = context.documentation->markdown;
        while (!documentation.empty() && (documentation.back() == '\n' || documentation.back() == '\r'))
            documentation.pop_back();
        if (!documentation.empty())
            result += "\n\n" + documentation;
    }

    std::string typeDefinition = context.typeDefinition;
    if (config.maxTypeDefinitionLength)
        typeDefinition = truncateVisible(typeDefinition, *config.maxTypeDefinitionLength);

    return result + kDocumentationBreaker + codeBlock("luau", typeDefinition);
}
