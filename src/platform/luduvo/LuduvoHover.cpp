#include "Platform/LuduvoPlatform.hpp"

#include "LSP/DocumentationParser.hpp"
#include "LSP/LuauExt.hpp"
#include "LSP/Utils.hpp"
#include "LSP/Workspace.hpp"

#include "Luau/AstQuery.h"
#include "Luau/ToString.h"
#include "Luau/TypePack.h"
#include "LuauFileUtils.hpp"

#include <algorithm>
#include <string_view>
#include <vector>

namespace
{
enum struct DeclarationKind
{
    None,
    Local,
    Member,
    Global,
    Type,
};

struct DeclarationTarget
{
    DeclarationKind kind = DeclarationKind::None;
    std::string name;
    bool isConst = false;
};

struct LuduvoRichHoverContext
{
    Luau::TypeId type;
    std::string typeDefinition;
    std::optional<std::string> documentationSymbol;
    std::optional<Luau::Property> property;
    std::optional<PrintedDocumentation> documentation;
    std::optional<std::string> fallbackTitle;
    bool preferLeafTitle = false;
    bool showTypeDefinition = true;
};

DeclarationTarget getDeclarationTarget(
    const Luau::SourceModule& sourceModule, Luau::ExprOrLocal& exprOrLocal, Luau::AstNode* node, const Luau::Position& position)
{
    const bool hoveringType = node->asType() != nullptr;
    const auto ancestry = Luau::findAstAncestryOfPosition(sourceModule, position, /* includeTypes: */ true);

    if (hoveringType)
    {
        for (Luau::AstNode* ancestor : ancestry)
        {
            if (auto local = ancestor->as<Luau::AstStatLocal>())
            {
                for (Luau::AstLocal* variable : local->vars)
                    if (variable->annotation && variable->annotation->location.containsClosed(position))
                        return {DeclarationKind::Local, variable->name.value, variable->isConst};
            }
            else if (auto localFunction = ancestor->as<Luau::AstStatLocalFunction>())
            {
                if (localFunction->func->returnAnnotation && localFunction->func->returnAnnotation->location.containsClosed(position))
                    return {DeclarationKind::Local, localFunction->name->name.value, localFunction->name->isConst};
            }
            else if (auto alias = ancestor->as<Luau::AstStatTypeAlias>())
                return {DeclarationKind::Type, alias->name.value};
        }
    }

    for (auto it = ancestry.rbegin(); it != ancestry.rend(); ++it)
        if (auto member = (*it)->as<Luau::AstExprIndexName>(); member && member->indexLocation.containsClosed(position))
            return {DeclarationKind::Member, member->index.value};

    const Luau::AstLocal* local = exprOrLocal.getLocal();
    if (!local)
        if (auto localExpression = node->as<Luau::AstExprLocal>())
            local = localExpression->local;
    if (local)
        return {DeclarationKind::Local, local->name.value, local->isConst};

    if (auto global = node->as<Luau::AstExprGlobal>())
        return {DeclarationKind::Global, global->name.value};
    if (auto alias = node->as<Luau::AstStatTypeAlias>())
        return {DeclarationKind::Type, alias->name.value};
    if (auto reference = node->as<Luau::AstTypeReference>())
        return {DeclarationKind::Type, reference->name.value};
    return {};
}

std::string makeDeclaration(
    Luau::TypeId type, const std::string& renderedType, const std::string& expandedType, const DeclarationTarget& target)
{
    auto declarationFor = [&](const std::string& typeText)
    {
        if (target.kind == DeclarationKind::None || target.name.empty())
            return typeText;
        if (Luau::get<Luau::FunctionType>(Luau::follow(type)))
            return "function " + target.name + typeText;

        switch (target.kind)
        {
        case DeclarationKind::Local:
            return std::string(target.isConst ? "const " : "local ") + target.name + ": " + typeText;
        case DeclarationKind::Member:
            return target.name + ": " + typeText;
        case DeclarationKind::Global:
            return "declare " + target.name + ": " + typeText;
        case DeclarationKind::Type:
            return "type " + target.name + " = " + typeText;
        case DeclarationKind::None:
            return typeText;
        }
        return typeText;
    };

    type = Luau::follow(type);
    auto standaloneDeclarationFor = [&](const std::string& typeText)
    {
        const std::string prefix = target.kind == DeclarationKind::Member ? "declare " : "";
        return prefix + declarationFor(typeText);
    };
    if (Luau::get<Luau::ExternType>(type))
    {
        if (target.kind == DeclarationKind::Type)
            return expandedType;
        return standaloneDeclarationFor(renderedType) + "\n\n" + expandedType;
    }
    if (Luau::get<Luau::UnionType>(type) && expandedType != renderedType)
    {
        const size_t supplemental = expandedType.find("\n\ndeclare extern type ");
        if (supplemental != std::string::npos)
            return standaloneDeclarationFor(renderedType) + expandedType.substr(supplemental);
    }
    return declarationFor(renderedType);
}

bool isLiteralPosition(const Luau::SourceModule& sourceModule, const Luau::Position& position)
{
    for (const Luau::AstNode* ancestor : Luau::findAstAncestryOfPosition(sourceModule, position))
        if ((ancestor->is<Luau::AstExprConstantNil>() || ancestor->is<Luau::AstExprConstantBool>() ||
                ancestor->is<Luau::AstExprConstantNumber>() || ancestor->is<Luau::AstExprConstantInteger>() ||
                ancestor->is<Luau::AstExprConstantString>()) &&
            ancestor->location.containsClosed(position))
            return true;
    return false;
}

struct FindDirectLocalInitializer : Luau::AstVisitor
{
    const Luau::AstLocal* target;
    Luau::AstExpr* result = nullptr;

    explicit FindDirectLocalInitializer(const Luau::AstLocal* target)
        : target(target)
    {
    }

    bool visit(Luau::AstStatLocal* local) override
    {
        for (size_t index = 0; index < local->vars.size && index < local->values.size; ++index)
        {
            if (local->vars.data[index] == target)
            {
                result = local->values.data[index];
                return false;
            }
        }
        return result == nullptr;
    }
};

bool localHasDirectMemberInitializer(const Luau::SourceModule& sourceModule, const Luau::AstLocal* local)
{
    if (!local)
        return false;
    FindDirectLocalInitializer finder{local};
    sourceModule.root->visit(&finder);
    return finder.result && finder.result->is<Luau::AstExprIndexName>();
}

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

enum struct LuduvoTypeCheckability
{
    Checkable,
    UncheckableFields,
    Uncheckable,
    Unresolved,
};

enum struct LuduvoResourceKind
{
    Component,
    Prefab,
};

struct LuduvoResourceArgumentApi
{
    std::string_view symbol;
    LuduvoResourceKind resourceKind;
};

constexpr LuduvoResourceArgumentApi kLuduvoResourceArgumentApis[] = {
    {"Instance.AddComponent", LuduvoResourceKind::Component},
    {"Instance.FindAncestorWithComponent", LuduvoResourceKind::Component},
    {"Instance.HasComponent", LuduvoResourceKind::Component},
    {"Instance.RemoveComponent", LuduvoResourceKind::Component},
    {"Query.With", LuduvoResourceKind::Component},
    {"Query.Without", LuduvoResourceKind::Component},
    {"game.Prefabs.Exists", LuduvoResourceKind::Prefab},
    {"game.Prefabs.Id", LuduvoResourceKind::Prefab},
    {"game.Prefabs.RevertAddedComponent", LuduvoResourceKind::Component},
    {"game.Prefabs.Spawn", LuduvoResourceKind::Prefab},
    {"game.World.Each", LuduvoResourceKind::Component},
    {"game.World.Pair", LuduvoResourceKind::Component},
    {"game.World.Query", LuduvoResourceKind::Component},
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

std::optional<LuduvoResourceKind> getLuduvoResourceArgumentKind(const std::optional<std::string>& documentationSymbol)
{
    auto identity = getLuduvoSymbolIdentity(documentationSymbol);
    if (!identity)
        return std::nullopt;

    for (const LuduvoResourceArgumentApi& api : kLuduvoResourceArgumentApis)
        if (api.symbol == identity->title)
            return api.resourceKind;
    return std::nullopt;
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
    if (Luau::is<Luau::ExternType, Luau::TableType, Luau::FunctionType>(type))
    {
        if (auto identity = getLuduvoSymbolIdentity(type->documentationSymbol))
        {
            provenance.server |= identity->side == LuduvoDefinitionSide::Server;
            provenance.client |= identity->side == LuduvoDefinitionSide::Client;
            return;
        }
        if (auto definitionModuleName = Luau::getDefinitionModuleName(type))
        {
            provenance.server |= *definitionModuleName == "@luduvo/server";
            provenance.client |= *definitionModuleName == "@luduvo/client";
            if (provenance.server || provenance.client)
                return;
        }
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

LuduvoTypeCheckability mergeCheckability(LuduvoTypeCheckability left, LuduvoTypeCheckability right)
{
    return static_cast<int>(left) >= static_cast<int>(right) ? left : right;
}

LuduvoTypeCheckability getCheckability(Luau::TypeId type, bool throughIndexer, std::vector<Luau::TypeId>& visited)
{
    type = Luau::follow(type);
    if (Luau::is<Luau::ErrorType>(type))
        return LuduvoTypeCheckability::Unresolved;
    if (Luau::is<Luau::AnyType>(type))
        return throughIndexer ? LuduvoTypeCheckability::UncheckableFields : LuduvoTypeCheckability::Uncheckable;
    if (Luau::is<Luau::UnknownType>(type))
        return LuduvoTypeCheckability::Checkable;

    if (std::find(visited.begin(), visited.end(), type) != visited.end())
        return LuduvoTypeCheckability::Checkable;
    visited.push_back(type);

    LuduvoTypeCheckability result = LuduvoTypeCheckability::Checkable;

    if (auto unionType = Luau::get<Luau::UnionType>(type))
    {
        for (Luau::TypeId option : unionType->options)
            result = mergeCheckability(result, getCheckability(option, throughIndexer, visited));
    }
    else if (auto intersectionType = Luau::get<Luau::IntersectionType>(type))
    {
        for (Luau::TypeId part : intersectionType->parts)
            result = mergeCheckability(result, getCheckability(part, throughIndexer, visited));
    }
    else if (auto tableType = Luau::get<Luau::TableType>(type); tableType && tableType->indexer)
    {
        result = getCheckability(tableType->indexer->indexResultType, true, visited);
        if (result == LuduvoTypeCheckability::Uncheckable)
            result = LuduvoTypeCheckability::UncheckableFields;
    }
    else if (auto externType = Luau::get<Luau::ExternType>(type))
    {
        if (externType->indexer)
            result = getCheckability(externType->indexer->indexResultType, true, visited);
        if (externType->parent)
            result = mergeCheckability(result, getCheckability(*externType->parent, throughIndexer, visited));
        if (result == LuduvoTypeCheckability::Uncheckable)
            result = LuduvoTypeCheckability::UncheckableFields;
    }
    else if (auto metatableType = Luau::get<Luau::MetatableType>(type))
        result = getCheckability(metatableType->table, throughIndexer, visited);

    return result;
}

LuduvoTypeCheckability getCheckability(Luau::TypeId type)
{
    std::vector<Luau::TypeId> visited;
    return getCheckability(type, false, visited);
}

bool isUncheckableArgumentType(
    Luau::TypeId type, const std::vector<Luau::TypeId>& functionGenerics, std::vector<Luau::TypeId>& visited)
{
    if (std::find(functionGenerics.begin(), functionGenerics.end(), type) != functionGenerics.end())
        return false;
    if (Luau::is<Luau::GenericType>(type) || Luau::is<Luau::FreeType>(type))
        return false;
    type = Luau::follow(type);
    if (std::find(functionGenerics.begin(), functionGenerics.end(), type) != functionGenerics.end())
        return false;
    if (Luau::is<Luau::GenericType>(type) || Luau::is<Luau::FreeType>(type))
        return false;
    if (Luau::is<Luau::AnyType>(type))
        return true;
    if (std::find(visited.begin(), visited.end(), type) != visited.end())
        return false;
    visited.push_back(type);
    if (const auto functionType = Luau::get<Luau::FunctionType>(type))
    {
        std::vector<Luau::TypeId> nestedGenerics = functionGenerics;
        nestedGenerics.insert(nestedGenerics.end(), functionType->generics.begin(), functionType->generics.end());
        auto [arguments, tail] = Luau::flatten(functionType->argTypes);
        for (Luau::TypeId argument : arguments)
            if (isUncheckableArgumentType(argument, nestedGenerics, visited))
                return true;
        if (tail)
            if (const auto variadic = Luau::get<Luau::VariadicTypePack>(Luau::follow(*tail)))
                return !variadic->hidden && isUncheckableArgumentType(variadic->ty, nestedGenerics, visited);
        return false;
    }
    if (const auto unionType = Luau::get<Luau::UnionType>(type))
        return std::any_of(unionType->options.begin(), unionType->options.end(),
            [&](Luau::TypeId option)
            {
                return isUncheckableArgumentType(option, functionGenerics, visited);
            });
    if (const auto intersectionType = Luau::get<Luau::IntersectionType>(type))
        return std::any_of(intersectionType->parts.begin(), intersectionType->parts.end(),
            [&](Luau::TypeId part)
            {
                return isUncheckableArgumentType(part, functionGenerics, visited);
            });
    if (const auto tableType = Luau::get<Luau::TableType>(type); tableType && tableType->indexer)
        return isUncheckableArgumentType(tableType->indexer->indexResultType, functionGenerics, visited);
    return false;
}

bool hasUncheckableArguments(const Luau::FunctionType& functionType)
{
    auto [arguments, tail] = Luau::flatten(functionType.argTypes);
    const size_t firstExplicitArgument = functionType.hasSelf && !arguments.empty() ? 1 : 0;
    for (size_t index = firstExplicitArgument; index < arguments.size(); ++index)
    {
        std::vector<Luau::TypeId> visited;
        if (isUncheckableArgumentType(arguments[index], functionType.generics, visited))
            return true;
    }

    if (tail)
        if (const auto variadic = Luau::get<Luau::VariadicTypePack>(Luau::follow(*tail)))
        {
            std::vector<Luau::TypeId> visited;
            return !variadic->hidden && isUncheckableArgumentType(variadic->ty, functionType.generics, visited);
        }

    return false;
}

bool hasUncheckableArguments(Luau::TypeId type)
{
    type = Luau::follow(type);
    if (const auto functionType = Luau::get<Luau::FunctionType>(type))
        return hasUncheckableArguments(*functionType);
    if (const auto intersectionType = Luau::get<Luau::IntersectionType>(type))
        return std::any_of(intersectionType->parts.begin(), intersectionType->parts.end(),
            [](Luau::TypeId part)
            {
                return hasUncheckableArguments(part);
            });
    if (const auto unionType = Luau::get<Luau::UnionType>(type))
        return std::any_of(unionType->options.begin(), unionType->options.end(),
            [](Luau::TypeId option)
            {
                return hasUncheckableArguments(option);
            });
    return false;
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

std::string normalizeDisplayedType(std::string text)
{
    constexpr std::string_view checkedAttribute = "@checked ";
    size_t position = 0;
    while ((position = text.find(checkedAttribute, position)) != std::string::npos)
        text.erase(position, checkedAttribute.size());
    return text;
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

    std::string firstMember = normalizeDisplayedType(Luau::toString(members.front()));
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

static std::string renderLuduvoUnionWithoutDuplicateMembers(Luau::TypeId type, Luau::ToStringOptions& options)
{
    type = Luau::follow(type);
    const auto unionType = Luau::get<Luau::UnionType>(type);
    if (!unionType)
        return Luau::toString(type, options);

    bool hasNil = false;
    std::vector<std::string> members;
    for (Luau::TypeId option : unionType->options)
    {
        option = Luau::follow(option);
        if (const auto primitive = Luau::get<Luau::PrimitiveType>(option); primitive && primitive->type == Luau::PrimitiveType::NilType)
        {
            hasNil = true;
            continue;
        }

        std::string rendered = Luau::toString(option, options);
        if (std::find(members.begin(), members.end(), rendered) == members.end())
            members.push_back(std::move(rendered));
    }

    if (members.size() == 1 && hasNil)
        return members.front() + "?";
    if (members.empty())
        return hasNil ? "nil" : Luau::toString(type, options);
    if (hasNil)
        members.emplace_back("nil");

    std::string result;
    for (const std::string& member : members)
    {
        if (!result.empty())
            result += " | ";
        result += member;
    }
    return result;
}

std::string normalizeLuduvoRichHoverType(Luau::TypeId type, Luau::ToStringOptions& options)
{
    type = Luau::follow(type);
    std::string rendered = Luau::toString(type, options);
    const auto functionType = Luau::get<Luau::FunctionType>(type);
    if (!functionType)
        return rendered;

    auto [returns, tail] = Luau::flatten(functionType->retTypes);
    std::vector<std::string> renderedReturns;
    renderedReturns.reserve(returns.size() + (tail ? 1 : 0));
    for (Luau::TypeId returnType : returns)
        renderedReturns.push_back(renderLuduvoUnionWithoutDuplicateMembers(returnType, options));
    if (tail)
        renderedReturns.push_back(Luau::toString(*tail, options));

    std::string returnPack;
    if (renderedReturns.empty())
        returnPack = "()";
    else if (renderedReturns.size() == 1 && !tail)
        returnPack = renderedReturns.front();
    else
    {
        returnPack = "(";
        for (size_t index = 0; index < renderedReturns.size(); ++index)
        {
            if (index > 0)
                returnPack += ", ";
            returnPack += renderedReturns[index];
        }
        returnPack += ")";
    }

    int depth = 0;
    for (size_t index = 0; index + 4 <= rendered.size(); ++index)
    {
        if (rendered[index] == '(' || rendered[index] == '{' || rendered[index] == '[' || rendered[index] == '<')
            ++depth;
        else if (rendered[index] == ')' || rendered[index] == '}' || rendered[index] == ']' || rendered[index] == '>')
            --depth;
        else if (depth == 0 && rendered.substr(index, 4) == " -> ")
            return rendered.substr(0, index + 4) + returnPack;
    }
    return rendered;
}

std::string getLuduvoRichHoverTypeDefinition(Luau::TypeId type, Luau::ToStringOptions& options)
{
    type = Luau::follow(type);
    auto renderExtern = [&](const Luau::ExternType* externType)
    {
        std::string result = "declare extern type " + externType->name;
        if (externType->parent)
            result += " extends " + Luau::toString(Luau::follow(*externType->parent), options);
        result += " with";

        auto appendProperty = [&](std::string_view prefix, const std::string& name, Luau::TypeId propertyType)
        {
            std::string renderedType = Luau::toString(Luau::follow(propertyType), options);
            size_t newline = 0;
            while ((newline = renderedType.find('\n', newline)) != std::string::npos)
            {
                renderedType.insert(newline + 1, "    ");
                newline += 5;
            }
            result += "\n    " + std::string(prefix) + name + ": " + renderedType;
        };

        for (const auto& [name, property] : externType->props)
        {
            if (property.isReadOnly())
                appendProperty("read ", name, *property.readTy);
            else if (property.isWriteOnly())
                appendProperty("write ", name, *property.writeTy);
            else if (property.readTy && property.writeTy && Luau::follow(*property.readTy) != Luau::follow(*property.writeTy))
            {
                appendProperty("read ", name, *property.readTy);
                appendProperty("write ", name, *property.writeTy);
            }
            else
                appendProperty("", name, *property.readTy);
        }

        if (externType->indexer)
        {
            result += "\n    [" + Luau::toString(Luau::follow(externType->indexer->indexType), options) +
                      "]: " + Luau::toString(Luau::follow(externType->indexer->indexResultType), options);
        }
        result += "\nend";
        return result;
    };

    if (const auto externType = Luau::get<Luau::ExternType>(type))
        return renderExtern(externType);

    if (const auto unionType = Luau::get<Luau::UnionType>(type))
    {
        const Luau::ExternType* optionalExtern = nullptr;
        bool hasNil = false;
        for (Luau::TypeId option : unionType->options)
        {
            option = Luau::follow(option);
            if (const auto primitive = Luau::get<Luau::PrimitiveType>(option); primitive && primitive->type == Luau::PrimitiveType::NilType)
                hasNil = true;
            else if (!optionalExtern)
                optionalExtern = Luau::get<Luau::ExternType>(option);
            else
                return Luau::toString(type, options);
        }
        if (hasNil && optionalExtern)
            return "type HoveredType = " + Luau::toString(type, options) + "\n\n" + renderExtern(optionalExtern);
    }

    return normalizeLuduvoRichHoverType(type, options);
}

std::optional<std::string> resolveLuduvoHoverDocumentationSymbol(
    Luau::TypeId type, const std::optional<std::string>& directDocumentationSymbol, const std::optional<std::string>& environmentName)
{
    if (getLuduvoSymbolIdentity(directDocumentationSymbol))
        return retargetDocumentationSymbol(directDocumentationSymbol, environmentName);

    type = Luau::follow(type);
    if (Luau::is<Luau::ExternType, Luau::TableType, Luau::FunctionType>(type) && getLuduvoSymbolIdentity(type->documentationSymbol))
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

        // Primitive and singleton nodes are shared by unrelated declarations. A documentation
        // symbol found on one is not evidence that the union itself represents that API member.
        if (Luau::is<Luau::PrimitiveType, Luau::SingletonType>(option))
            return std::nullopt;

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
    std::string title = normalizeDisplayedType(identity ? identity->title : context.fallbackTitle.value_or(context.typeDefinition));
    if (identity && context.preferLeafTitle)
    {
        if (size_t separator = title.rfind('.'); separator != std::string::npos)
            title.erase(0, separator + 1);
    }
    const bool linked = identity && context.documentation && context.documentation->learnMoreLink;
    LuduvoTypeProvenance typeProvenance;
    collectLuduvoTypeProvenance(context.type, typeProvenance);
    const std::string provenance = identity ? versionLabel(identity->side) : versionLabel(typeProvenance);
    std::replace(title.begin(), title.end(), '\n', ' ');
    std::replace(title.begin(), title.end(), '\r', ' ');
    if (config.maxTitleLength)
    {
        const size_t fixedLength = visibleLength(provenance) + (linked ? 2 : 0); // Linked headings render a space and ↗ after the title.
        const size_t titleBudget = *config.maxTitleLength > fixedLength ? *config.maxTitleLength - fixedLength : 3;
        const bool complexMultilineUnion = Luau::is<Luau::UnionType>(Luau::follow(context.type)) && context.typeDefinition.find('\n') != std::string::npos;
        if (auto summary = getUnionSummary(context.type); summary && (complexMultilineUnion || visibleLength(title) > titleBudget))
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
    if (context.documentation && context.documentation->readOnly &&
        std::find(tags.begin(), tags.end(), "read-only") == tags.end())
        tags.emplace_back("read-only");
    if (deprecated || isDeprecated(context.type))
        tags.emplace_back("deprecated");
    if (getLuduvoResourceArgumentKind(context.documentationSymbol))
        tags.emplace_back("resource-constrained");
    if (hasUncheckableArguments(context.type))
        tags.emplace_back("uncheckable arguments");
    switch (getCheckability(context.type))
    {
    case LuduvoTypeCheckability::UncheckableFields:
        tags.emplace_back("uncheckable fields");
        break;
    case LuduvoTypeCheckability::Uncheckable:
        tags.emplace_back("uncheckable");
        break;
    case LuduvoTypeCheckability::Unresolved:
        tags.emplace_back("unresolved");
        break;
    case LuduvoTypeCheckability::Checkable:
        break;
    }

    std::string typeDefinition = normalizeDisplayedType(context.typeDefinition);
    if (config.maxTypeDefinitionLength)
        typeDefinition = truncateTypeDefinition(typeDefinition, *config.maxTypeDefinitionLength);

    const bool hasVisibleDocumentation = context.documentation &&
                                         (!context.documentation->markdown.empty() || context.documentation->learnMoreLink.has_value());
    if (context.showTypeDefinition && tags.empty() && !hasVisibleDocumentation && provenance.empty())
        return codeBlock("luau", typeDefinition);

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

    if (!context.showTypeDefinition)
        return result;

    return result + kDocumentationBreaker + codeBlock("luau", typeDefinition);
}

std::optional<lsp::Hover> LuduvoPlatform::handleTypeHover(const PlatformHoverContext& context)
{
    if (configuration.hover.presentation != LuduvoHoverPresentation::Rich || isLiteralPosition(context.sourceModule, context.position))
        return std::nullopt;

    Luau::ToStringOptions options;
    options.exhaustive = true;
    options.useLineBreaks = true;
    options.functionTypeArguments = true;
    options.hideNamedFunctionTypeParameters = false;
    options.hideTableKind = !context.showTableKinds;
    options.scope = context.scope;
    options.maxTableLength = 0;
    options.maxTypeLength = 0;

    const std::string rawType = Luau::toString(context.type, options);
    const DeclarationTarget declarationTarget =
        getDeclarationTarget(context.sourceModule, context.exprOrLocal, context.node, context.position);
    const Luau::AstLocal* hoveredLocal = context.exprOrLocal.getLocal();
    if (!hoveredLocal)
        if (auto localExpression = context.node->as<Luau::AstExprLocal>())
            hoveredLocal = localExpression->local;
    const bool directMemberInitializer = localHasDirectMemberInitializer(context.sourceModule, hoveredLocal);

    std::optional<std::string> documentationSymbol = context.documentationSymbol;
    const bool directIdentityBelongsToHover =
        !((declarationTarget.kind == DeclarationKind::Local && !directMemberInitializer) || context.node->asType());
    if (!directIdentityBelongsToHover)
        documentationSymbol = std::nullopt;
    documentationSymbol =
        resolveLuduvoHoverDocumentationSymbol(context.type, documentationSymbol, getEnvironmentForModule(context.moduleName));

    std::optional<PrintedDocumentation> documentation;
    if (workspaceFolder && documentationSymbol)
        documentation = getDocumentation(
            workspaceFolder->client->documentation, *documentationSymbol, &workspaceFolder->client->documentationMetadata);
    if (!documentation && workspaceFolder)
    {
        if (directIdentityBelongsToHover)
        {
            if (auto fallback = workspaceFolder->getDocumentationForType(context.type); fallback && !fallback->empty())
                documentation = PrintedDocumentation{*fallback, std::nullopt};
        }
        if (!documentation)
        {
            auto fallback = workspaceFolder->getDocumentationForAstNode(context.moduleName, context.node, context.scope);
            if (fallback && !fallback->empty())
                documentation = PrintedDocumentation{*fallback, std::nullopt};
        }
        if (!documentation && context.documentationLocation)
        {
            auto fallbackComments = printMoonwaveDocumentation(
                workspaceFolder->getComments(context.documentationLocation->moduleName, context.documentationLocation->location));
            if (!fallbackComments.empty())
                documentation = PrintedDocumentation{std::move(fallbackComments), std::nullopt};
        }
    }

    std::optional<std::string> fallbackTitle = types::getTypeName(context.type);
    if (!fallbackTitle &&
        Luau::is<Luau::PrimitiveType, Luau::SingletonType, Luau::UnionType, Luau::IntersectionType>(context.type))
        fallbackTitle = rawType;
    if (!fallbackTitle && context.exprOrLocal.getName())
        fallbackTitle = context.exprOrLocal.getName()->value;

    const std::string expandedType = getLuduvoRichHoverTypeDefinition(context.type, options);
    const std::string declaration =
        makeDeclaration(context.type, normalizeLuduvoRichHoverType(context.type, options), expandedType, declarationTarget);
    std::optional<Luau::Property> property;
    if (context.hoveredProperty)
        property = *context.hoveredProperty;
    return lsp::Hover{{lsp::MarkupKind::Markdown,
        renderLuduvoRichHover(configuration.hover,
            LuduvoRichHoverContext{context.type, declaration, documentationSymbol, std::move(property), documentation, fallbackTitle,
                directMemberInitializer, true})}};
}

std::optional<lsp::MarkupContent> LuduvoPlatform::handleCompletionDocumentation(
    const PlatformCompletionDocumentationContext& context)
{
    if (configuration.hover.presentation != LuduvoHoverPresentation::Rich || !context.entry.type || !workspaceFolder)
        return std::nullopt;

    auto documentationSymbol = resolveLuduvoHoverDocumentationSymbol(
        *context.entry.type, context.entry.documentationSymbol, getEnvironmentForModule(context.moduleName));
    std::optional<PrintedDocumentation> documentation;
    if (documentationSymbol)
        documentation = getDocumentation(
            workspaceFolder->client->documentation, *documentationSymbol, &workspaceFolder->client->documentationMetadata);
    if (!documentation && context.fallbackDocumentation && !context.fallbackDocumentation->empty())
        documentation = PrintedDocumentation{*context.fallbackDocumentation, std::nullopt};

    std::optional<Luau::Property> property;
    if (context.entry.prop)
        property = **context.entry.prop;
    return lsp::MarkupContent{lsp::MarkupKind::Markdown,
        renderLuduvoRichHover(configuration.hover,
            LuduvoRichHoverContext{*context.entry.type, Luau::toString(Luau::follow(*context.entry.type)), documentationSymbol,
                std::move(property), documentation, context.name, false, false})};
}
