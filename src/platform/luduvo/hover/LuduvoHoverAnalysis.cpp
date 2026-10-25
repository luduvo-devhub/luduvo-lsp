#include "Platform/luduvo/hover/LuduvoHoverAnalysis.hpp"

#include "LSP/LuauExt.hpp"
#include "Luau/AstQuery.h"
#include "Luau/TypePack.h"

#include <algorithm>
#include <string_view>
#include <vector>

namespace luduvo::hover
{
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

std::string makeDeclaration(Luau::TypeId type, const std::string& renderedType, const std::string& expandedType, const DeclarationTarget& target)
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
        if ((ancestor->is<Luau::AstExprConstantNil>() || ancestor->is<Luau::AstExprConstantBool>() || ancestor->is<Luau::AstExprConstantNumber>() ||
                ancestor->is<Luau::AstExprConstantInteger>() || ancestor->is<Luau::AstExprConstantString>()) &&
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

void collectTypeProvenance(Luau::TypeId type, TypeProvenance& provenance)
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
            collectTypeProvenance(option, provenance);
    }
    else if (auto intersectionType = Luau::get<Luau::IntersectionType>(type))
    {
        for (Luau::TypeId part : intersectionType->parts)
            collectTypeProvenance(part, provenance);
    }
}

std::string versionLabel(const TypeProvenance& provenance)
{
    if (provenance.server && provenance.client)
        return " (Client/Server Versions)";
    if (provenance.server)
        return " (Server Version)";
    if (provenance.client)
        return " (Client Version)";
    return "";
}

TypeCheckability mergeCheckability(TypeCheckability left, TypeCheckability right)
{
    return static_cast<int>(left) >= static_cast<int>(right) ? left : right;
}

TypeCheckability getCheckability(Luau::TypeId type, bool throughIndexer, std::vector<Luau::TypeId>& visited)
{
    type = Luau::follow(type);
    if (Luau::is<Luau::ErrorType>(type))
        return TypeCheckability::Unresolved;
    if (Luau::is<Luau::AnyType>(type))
        return throughIndexer ? TypeCheckability::UncheckableFields : TypeCheckability::Uncheckable;
    if (Luau::is<Luau::UnknownType>(type))
        return TypeCheckability::Checkable;

    if (std::find(visited.begin(), visited.end(), type) != visited.end())
        return TypeCheckability::Checkable;
    visited.push_back(type);

    TypeCheckability result = TypeCheckability::Checkable;

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
        if (result == TypeCheckability::Uncheckable)
            result = TypeCheckability::UncheckableFields;
    }
    else if (auto externType = Luau::get<Luau::ExternType>(type))
    {
        if (externType->indexer)
            result = getCheckability(externType->indexer->indexResultType, true, visited);
        if (externType->parent)
            result = mergeCheckability(result, getCheckability(*externType->parent, throughIndexer, visited));
        if (result == TypeCheckability::Uncheckable)
            result = TypeCheckability::UncheckableFields;
    }
    else if (auto metatableType = Luau::get<Luau::MetatableType>(type))
        result = getCheckability(metatableType->table, throughIndexer, visited);

    return result;
}

TypeCheckability getCheckability(Luau::TypeId type)
{
    std::vector<Luau::TypeId> visited;
    return getCheckability(type, false, visited);
}

bool isUncheckableArgumentType(Luau::TypeId type, const std::vector<Luau::TypeId>& functionGenerics, std::vector<Luau::TypeId>& visited)
{
    if (std::find(functionGenerics.begin(), functionGenerics.end(), type) != functionGenerics.end())
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

static std::optional<size_t> findTopLevelFunctionArrow(std::string_view rendered)
{
    int depth = 0;
    for (size_t index = 0; index + 4 <= rendered.size(); ++index)
    {
        if (rendered[index] == '(' || rendered[index] == '{' || rendered[index] == '[' || rendered[index] == '<')
            ++depth;
        else if (rendered[index] == ')' || rendered[index] == '}' || rendered[index] == ']' || rendered[index] == '>')
            --depth;
        else if (depth == 0 && rendered.substr(index, 4) == " -> ")
            return index;
    }
    return std::nullopt;
}

static std::string renderFunctionReturnPack(const Luau::FunctionType& functionType, Luau::ToStringOptions& options)
{
    auto [returns, tail] = Luau::flatten(functionType.retTypes);
    std::vector<std::string> renderedReturns;
    renderedReturns.reserve(returns.size() + (tail ? 1 : 0));
    for (Luau::TypeId returnType : returns)
        renderedReturns.push_back(renderLuduvoUnionWithoutDuplicateMembers(returnType, options));
    if (tail)
        renderedReturns.push_back(Luau::toString(*tail, options));

    if (renderedReturns.empty())
        return "()";
    if (renderedReturns.size() == 1 && !tail)
        return renderedReturns.front();

    std::string result = "(";
    for (size_t index = 0; index < renderedReturns.size(); ++index)
    {
        if (index > 0)
            result += ", ";
        result += renderedReturns[index];
    }
    return result + ")";
}

std::string normalizeRichHoverType(Luau::TypeId type, Luau::ToStringOptions& options)
{
    type = Luau::follow(type);
    if (const auto intersectionType = Luau::get<Luau::IntersectionType>(type);
        intersectionType && !intersectionType->parts.empty() &&
        std::all_of(intersectionType->parts.begin(), intersectionType->parts.end(),
            [](Luau::TypeId part)
            {
                return Luau::is<Luau::FunctionType>(Luau::follow(part));
            }))
    {
        std::string rendered;
        for (Luau::TypeId part : intersectionType->parts)
        {
            if (!rendered.empty())
                rendered += "\n& ";
            rendered += "(" + normalizeRichHoverType(part, options) + ")";
        }
        return rendered;
    }

    Luau::ToStringOptions callableOptions = options;
    if (Luau::is<Luau::FunctionType>(type))
        callableOptions.hideTableAliasExpansions = true;

    std::string rendered = Luau::toString(type, callableOptions);
    const auto functionType = Luau::get<Luau::FunctionType>(type);
    if (!functionType)
        return rendered;

    // The exhaustive printer can retain a recursive binder for a function whose self type
    // points back to the function, even when the named self alias hides that recursion. Drop
    // the binder only when its name no longer occurs in the rendered function body.
    if (const size_t where = rendered.find(" where "); where != std::string::npos)
    {
        const std::string binder = rendered.substr(0, where);
        const std::string assignment = binder + " = ";
        const size_t body = where + std::string_view(" where ").size();
        if (!binder.empty() && binder.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") == std::string::npos &&
            rendered.compare(body, assignment.size(), assignment) == 0)
        {
            const size_t bodyStart = body + assignment.size();
            const size_t occurrence = rendered.find(binder, bodyStart);
            if (occurrence == std::string::npos)
                rendered.erase(0, bodyStart);
        }
    }

    const std::string returnPack = renderFunctionReturnPack(*functionType, callableOptions);

    if (const auto arrow = findTopLevelFunctionArrow(rendered))
        return rendered.substr(0, *arrow + 4) + returnPack;
    return rendered;
}

std::string getRichHoverTypeDefinition(Luau::TypeId type, Luau::ToStringOptions& options)
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

    return normalizeRichHoverType(type, options);
}

std::vector<RichHoverOverload> makeRichHoverOverloads(Luau::TypeId type, const DeclarationTarget& target, Luau::ToStringOptions& options,
    const std::vector<LuduvoResolvedResourceBinding>& constrainedResources)
{
    type = Luau::follow(type);
    const auto intersection = Luau::get<Luau::IntersectionType>(type);
    if (!intersection || intersection->parts.size() < 2 || target.name.empty())
        return {};

    std::vector<const Luau::FunctionType*> functions;
    functions.reserve(intersection->parts.size());
    for (Luau::TypeId part : intersection->parts)
    {
        const auto function = Luau::get<Luau::FunctionType>(Luau::follow(part));
        if (!function)
            return {};
        functions.push_back(function);
    }

    auto hasExplicitSelf = [](const Luau::FunctionType& function)
    {
        auto [arguments, tail] = Luau::flatten(function.argTypes);
        return !arguments.empty() &&
               (function.hasSelf || (!function.argNames.empty() && function.argNames[0] && function.argNames[0]->name == "self"));
    };

    const bool hasDotCall = std::any_of(functions.begin(), functions.end(),
        [&](const Luau::FunctionType* function)
        {
            return !hasExplicitSelf(*function);
        });
    const bool hasMethodCall = std::any_of(functions.begin(), functions.end(),
        [&](const Luau::FunctionType* function)
        {
            return hasExplicitSelf(*function);
        });

    std::optional<std::string> receiverName;
    for (const Luau::FunctionType* function : functions)
    {
        if (!hasExplicitSelf(*function))
            continue;
        auto [arguments, tail] = Luau::flatten(function->argTypes);
        Luau::ToStringOptions receiverOptions = options;
        receiverOptions.hideTableAliasExpansions = true;
        std::string candidate = types::getTypeName(Luau::follow(arguments.front())).value_or(Luau::toString(arguments.front(), receiverOptions));
        if (!candidate.empty() &&
            candidate.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.") == std::string::npos)
            receiverName = std::move(candidate);
        break;
    }

    struct ParameterList
    {
        size_t open;
        std::optional<size_t> firstComma;
        size_t close;
    };

    auto findParameterList = [](std::string_view signature) -> std::optional<ParameterList>
    {
        const size_t open = signature.find('(');
        if (open == std::string_view::npos)
            return std::nullopt;

        int depth = 0;
        std::optional<size_t> firstComma;
        char quote = '\0';
        bool escaped = false;
        for (size_t index = open + 1; index < signature.size(); ++index)
        {
            const char character = signature[index];
            if (quote)
            {
                if (escaped)
                    escaped = false;
                else if (character == '\\')
                    escaped = true;
                else if (character == quote)
                    quote = '\0';
                continue;
            }
            if (character == '\'' || character == '"')
            {
                quote = character;
                continue;
            }
            if (character == '(' || character == '{' || character == '[' || character == '<')
                ++depth;
            else if (character == ')' || character == '}' || character == ']' || character == '>')
            {
                if (character == ')' && depth == 0)
                    return ParameterList{open, firstComma, index};
                --depth;
            }
            else if (character == ',' && depth == 0 && !firstComma)
                firstComma = index;
        }
        return std::nullopt;
    };

    auto removeFirstParameter = [&](std::string& signature)
    {
        const auto parameters = findParameterList(signature);
        if (!parameters)
            return;
        if (!parameters->firstComma)
        {
            signature.erase(parameters->open + 1, parameters->close - parameters->open - 1);
            return;
        }
        size_t following = *parameters->firstComma + 1;
        while (following < signature.size() && signature[following] == ' ')
            ++following;
        signature.erase(parameters->open + 1, following - parameters->open - 1);
    };

    std::vector<RichHoverOverload> overloads;
    overloads.reserve(functions.size());
    for (size_t index = 0; index < functions.size(); ++index)
    {
        RichHoverOverload overload;
        const bool methodCall = hasExplicitSelf(*functions[index]);
        const bool renderAsMember = target.kind == DeclarationKind::Member && receiverName.has_value();
        std::string declarationName = target.name;
        if (renderAsMember)
            declarationName = *receiverName + (methodCall ? ":" : ".") + target.name;

        Luau::ToStringOptions callableOptions = options;
        callableOptions.hideTableAliasExpansions = true;
        std::string signature = Luau::toStringNamedFunction(declarationName, *functions[index], callableOptions);
        if (renderAsMember && methodCall)
            removeFirstParameter(signature);

        if (const auto parameters = findParameterList(signature))
            signature = signature.substr(0, parameters->close + 1) + ": " + renderFunctionReturnPack(*functions[index], callableOptions);

        overload.declaration = "function " + signature + "\nend";
        if (renderAsMember && hasDotCall && hasMethodCall)
            overload.callStyle = methodCall ? "Method call" : "Dot call";
        for (const LuduvoResolvedResourceBinding& binding : constrainedResources)
            if (binding.overloadIndex == index)
                overload.constrainedResources.push_back(binding);
        overloads.push_back(std::move(overload));
    }
    return overloads;
}

std::optional<std::string> resolveDocumentationSymbol(
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

static std::string normalizeDisplayedType(std::string text)
{
    constexpr std::string_view checkedAttribute = "@checked ";
    size_t position = 0;
    while ((position = text.find(checkedAttribute, position)) != std::string::npos)
        text.erase(position, checkedAttribute.size());
    return text;
}

static std::optional<UnionSummary> summarizeUnion(Luau::TypeId type, std::string_view typeDefinition)
{
    type = Luau::follow(type);
    const auto unionType = Luau::get<Luau::UnionType>(type);
    if (!unionType)
        return std::nullopt;

    std::vector<Luau::TypeId> members;
    for (Luau::TypeId option : unionType->options)
    {
        option = Luau::follow(option);
        if (const auto primitive = Luau::get<Luau::PrimitiveType>(option); primitive && primitive->type == Luau::PrimitiveType::NilType)
            continue;
        members.push_back(option);
    }
    if (members.size() < 2)
        return std::nullopt;

    std::string firstMember = normalizeDisplayedType(Luau::toString(members.front()));
    if (auto identity = getLuduvoSymbolIdentity(members.front()->documentationSymbol))
        firstMember = identity->title;
    return UnionSummary{std::move(firstMember), members.size() - 1, typeDefinition.find('\n') != std::string_view::npos};
}

static bool sameResourceConstraint(const LuduvoResolvedResourceBinding& left, const LuduvoResolvedResourceBinding& right)
{
    return left.parameter == right.parameter && left.kind == right.kind && left.variadic == right.variadic &&
           left.pluralResourceNoun == right.pluralResourceNoun;
}

RichHoverRenderModel analyzeRichHover(const std::vector<LuduvoResolvedResourceBinding>& constrainedResources, const RichHoverInput& input)
{
    RichHoverRenderModel result;
    auto identity = getLuduvoSymbolIdentity(input.documentationSymbol);
    result.title = normalizeDisplayedType(identity ? identity->title : input.fallbackTitle.value_or(input.typeDefinition));
    if (identity && input.preferLeafTitle)
        if (const size_t separator = result.title.rfind('.'); separator != std::string::npos)
            result.title.erase(0, separator + 1);

    TypeProvenance provenance;
    collectTypeProvenance(input.type, provenance);
    result.provenance = identity ? versionLabel(identity->side) : versionLabel(provenance);
    if (identity && input.documentation)
        result.learnMoreLink = input.documentation->learnMoreLink;

    bool deprecated = false;
    if (input.property)
    {
        if (input.property->isReadOnly())
            result.tags.emplace_back("read-only");
        else if (input.property->isWriteOnly())
            result.tags.emplace_back("write-only");
        deprecated = input.property->deprecated;
    }
    if (input.documentation && input.documentation->readOnly && std::find(result.tags.begin(), result.tags.end(), "read-only") == result.tags.end())
        result.tags.emplace_back("read-only");
    if (deprecated || isDeprecated(input.type))
        result.tags.emplace_back("deprecated");
    if (!constrainedResources.empty())
        result.tags.emplace_back("resource-constrained");
    if (hasUncheckableArguments(input.type))
        result.tags.emplace_back("uncheckable arguments");
    switch (getCheckability(input.type))
    {
    case TypeCheckability::UncheckableFields:
        result.tags.emplace_back("uncheckable fields");
        break;
    case TypeCheckability::Uncheckable:
        result.tags.emplace_back("uncheckable");
        break;
    case TypeCheckability::Unresolved:
        result.tags.emplace_back("unresolved");
        break;
    case TypeCheckability::Checkable:
        break;
    }

    if (input.documentation)
    {
        result.documentation = input.documentation->markdown;
        while (!result.documentation.empty() && (result.documentation.back() == '\n' || result.documentation.back() == '\r'))
            result.documentation.pop_back();
    }
    result.overloads = input.overloads;
    if (result.overloads.empty())
        result.constrainedResources = constrainedResources;
    else
    {
        for (const LuduvoResolvedResourceBinding& candidate : result.overloads.front().constrainedResources)
        {
            const bool appliesToEveryOverload = std::all_of(result.overloads.begin() + 1, result.overloads.end(),
                [&](const RichHoverOverload& overload)
                {
                    return std::any_of(overload.constrainedResources.begin(), overload.constrainedResources.end(),
                        [&](const LuduvoResolvedResourceBinding& binding)
                        {
                            return sameResourceConstraint(candidate, binding);
                        });
                });
            const bool alreadyHoisted = std::any_of(result.constrainedResources.begin(), result.constrainedResources.end(),
                [&](const LuduvoResolvedResourceBinding& binding)
                {
                    return sameResourceConstraint(candidate, binding);
                });
            if (appliesToEveryOverload && !alreadyHoisted)
                result.constrainedResources.push_back(candidate);
        }

        for (RichHoverOverload& overload : result.overloads)
            overload.constrainedResources.erase(std::remove_if(overload.constrainedResources.begin(), overload.constrainedResources.end(),
                                                    [&](const LuduvoResolvedResourceBinding& binding)
                                                    {
                                                        return std::any_of(result.constrainedResources.begin(), result.constrainedResources.end(),
                                                            [&](const LuduvoResolvedResourceBinding& common)
                                                            {
                                                                return sameResourceConstraint(binding, common);
                                                            });
                                                    }),
                overload.constrainedResources.end());
    }
    result.typeDefinition = normalizeDisplayedType(input.typeDefinition);
    result.unionSummary = summarizeUnion(input.type, input.typeDefinition);
    result.showTypeDefinition = input.showTypeDefinition;
    return result;
}

} // namespace luduvo::hover
