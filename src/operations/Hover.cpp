#include "LSP/Workspace.hpp"

#include "Luau/AstQuery.h"
#include "Luau/Module.h"
#include "Luau/ToString.h"
#include "LSP/LuauExt.hpp"
#include "LSP/DocumentationParser.hpp"

// Lifted from lutf8lib.cpp
/*
** Decode one UTF-8 sequence, returning NULL if byte sequence is invalid.
*/
static const char* utf8_decode(const char* o, int* val)
{
    static const unsigned int limits[] = {0xFF, 0x7F, 0x7FF, 0xFFFF};
    const unsigned char* s = (const unsigned char*)o;
    unsigned int c = s[0];
    unsigned int res = 0; // final result
    if (c < 0x80)         // ascii?
        res = c;
    else
    {
        int count = 0; // to count number of continuation bytes
        while (c & 0x40)
        {                                   // still have continuation bytes?
            int cc = s[++count];            // read next byte
            if ((cc & 0xC0) != 0x80)        // not a continuation byte?
                return NULL;                // invalid byte sequence
            res = (res << 6) | (cc & 0x3F); // add lower 6 bits from cont. byte
            c <<= 1;                        // to test next bit
        }
        res |= ((c & 0x7F) << (count * 5)); // add first byte
        if (count > 3 || res > 0x10FFFF || res <= limits[count])
            return NULL; // invalid byte sequence
        if (unsigned(res - 0xD800) < 0x800)
            return NULL; // surrogate
        s += count;      // skip continuation bytes read
    }
    if (val)
        *val = res;
    return (const char*)s + 1; // +1 to include first byte
}

static std::optional<size_t> utflen(const char* s, size_t len)
{
    size_t n = 0;
    size_t posi = 0;
    while (posi < len)
    {
        const char* s1 = utf8_decode(s + posi, NULL);
        if (s1 == NULL)
        {
            return std::nullopt;
        }
        posi = (int)(s1 - s);
        n++;
    }
    return n;
}

static void prependDocumentation(std::string& hoverText, const std::string& documentation)
{
    hoverText = documentation + kDocumentationBreaker + hoverText;
}

/// Construct the initial type description from a typeFun, i.e. Foo<T>
static std::string toStringTypeFun(const std::string typeName, const Luau::TypeFun& typeFun)
{
    std::string output = typeName;
    if (!typeFun.typeParams.empty() || !typeFun.typePackParams.empty())
    {
        output += "<";
        bool addComma = false;
        for (const auto& typeParam : typeFun.typeParams)
        {
            if (addComma)
                output += ", ";
            output += Luau::toString(Luau::follow(typeParam.ty));
            if (typeParam.defaultValue)
            {
                output += " = " + Luau::toString(Luau::follow(typeParam.defaultValue.value()));
            }
            addComma = true;
        }
        for (const auto& typePack : typeFun.typePackParams)
        {
            if (addComma)
                output += ", ";
            output += Luau::toString(Luau::follow(typePack.tp));
            if (typePack.defaultValue)
            {
                output += " = " + Luau::toString(Luau::follow(typePack.defaultValue.value()));
            }
            addComma = true;
        }
        output += ">";
    }
    return output;
}


static bool isBetween(const Luau::Position& position, const Luau::Position& left, const Luau::Position& right)
{
    return left <= position && position <= right;
}

static bool isOperatorPosition(const Luau::SourceModule& sourceModule, const Luau::Position& position)
{
    for (Luau::AstNode* ancestor : Luau::findAstAncestryOfPosition(sourceModule, position))
    {
        if (auto binary = ancestor->as<Luau::AstExprBinary>(); binary && isBetween(position, binary->left->location.end, binary->right->location.begin))
            return true;
        if (auto unary = ancestor->as<Luau::AstExprUnary>(); unary && isBetween(position, unary->location.begin, unary->expr->location.begin))
            return true;
        if (auto local = ancestor->as<Luau::AstStatLocal>(); local && local->equalsSignLocation && local->equalsSignLocation->containsClosed(position))
            return true;
        if (auto assignment = ancestor->as<Luau::AstStatAssign>(); assignment && assignment->vars.size > 0 && assignment->values.size > 0 &&
            isBetween(position, assignment->vars.data[assignment->vars.size - 1]->location.end, assignment->values.data[0]->location.begin))
            return true;
        if (auto assignment = ancestor->as<Luau::AstStatCompoundAssign>(); assignment &&
            isBetween(position, assignment->var->location.end, assignment->value->location.begin))
            return true;
    }
    return false;
}

static bool isHoverableTokenPosition(
    const Luau::SourceModule& sourceModule, Luau::ExprOrLocal& exprOrLocal, Luau::AstNode* node, const Luau::Position& position)
{
    if (const Luau::AstLocal* local = exprOrLocal.getLocal(); local && local->location.containsClosed(position))
        return true;

    for (const Luau::AstNode* ancestor : Luau::findAstAncestryOfPosition(sourceModule, position))
    {
        if ((ancestor->is<Luau::AstExprLocal>() || ancestor->is<Luau::AstExprGlobal>() || ancestor->is<Luau::AstExprConstantNil>() ||
                ancestor->is<Luau::AstExprConstantBool>() || ancestor->is<Luau::AstExprConstantNumber>() ||
                ancestor->is<Luau::AstExprConstantInteger>() || ancestor->is<Luau::AstExprConstantString>()) &&
            ancestor->location.containsClosed(position))
            return true;
        if (auto index = ancestor->as<Luau::AstExprIndexName>(); index && index->indexLocation.containsClosed(position))
            return true;
        if (auto reference = ancestor->as<Luau::AstTypeReference>(); reference &&
            (reference->nameLocation.containsClosed(position) || (reference->prefixLocation && reference->prefixLocation->containsClosed(position))))
            return true;
        if (auto alias = ancestor->as<Luau::AstStatTypeAlias>(); alias && alias->nameLocation.containsClosed(position))
            return true;
        if (auto typeFunction = ancestor->as<Luau::AstStatTypeFunction>(); typeFunction && typeFunction->nameLocation.containsClosed(position))
            return true;
        if (auto declaredGlobal = ancestor->as<Luau::AstStatDeclareGlobal>(); declaredGlobal && declaredGlobal->nameLocation.containsClosed(position))
            return true;
        if (auto declaredFunction = ancestor->as<Luau::AstStatDeclareFunction>(); declaredFunction && declaredFunction->nameLocation.containsClosed(position))
            return true;
    }

    return node->asType() && node->location.containsClosed(position);
}

std::optional<lsp::Hover> WorkspaceFolder::hover(const lsp::HoverParams& params, const LSPCancellationToken& cancellationToken)
{
    auto config = client->getConfiguration(rootUri);

    if (!config.hover.enabled)
        return std::nullopt;

    auto moduleName = fileResolver.getModuleName(params.textDocument.uri);
    auto textDocument = fileResolver.getTextDocument(params.textDocument.uri);
    if (!textDocument)
        throw JsonRpcException(lsp::ErrorCode::RequestFailed, "No managed text document for " + params.textDocument.uri.toString());

    auto position = textDocument->convertPosition(params.position);

    // Run the type checker to ensure we are up to date
    // TODO: expressiveTypes - remove "forAutocomplete" once the types have been fixed
    checkStrict(moduleName, cancellationToken, /* forAutocomplete: */ config.hover.strictDatamodelTypes);
    throwIfCancelled(cancellationToken);

    auto sourceModule = frontend.getSourceModule(moduleName);
    auto module = getModule(moduleName, /* forAutocomplete: */ config.hover.strictDatamodelTypes);
    if (!sourceModule)
        return std::nullopt;

    if (Luau::isWithinComment(*sourceModule, position))
        return std::nullopt;

    if (auto hover = platform->handleHover(*textDocument, *sourceModule, position))
        return hover;

    auto exprOrLocal = Luau::findExprOrLocalAtPosition(*sourceModule, position);
    auto node = findNodeOrTypeAtPosition(*sourceModule, position);
    auto scope = Luau::findScopeAtPosition(*module, position);
    if (!node || !scope)
        return std::nullopt;
    if (isOperatorPosition(*sourceModule, position))
        return std::nullopt;
    if (!isHoverableTokenPosition(*sourceModule, exprOrLocal, node, position))
        return std::nullopt;

    std::optional<std::pair<std::string, Luau::TypeFun>> typeAliasInformation = std::nullopt;
    std::optional<Luau::TypeId> type = std::nullopt;
    std::optional<std::string> documentationSymbol = getDocumentationSymbolAtPosition(*sourceModule, *module, position);
    std::optional<PlatformDocumentationLocation> documentationLocation = std::nullopt;
    std::optional<Luau::Property> hoveredProperty = std::nullopt;

    if (auto ref = node->as<Luau::AstTypeReference>())
    {
        std::string typeName;
        std::optional<Luau::TypeFun> typeFun;
        if (ref->prefix)
        {
            typeName = std::string(ref->prefix->value) + "." + ref->name.value;
            typeFun = scope->lookupImportedType(ref->prefix->value, ref->name.value);
        }
        else
        {
            typeName = ref->name.value;
            typeFun = scope->lookupType(ref->name.value);
        }
        if (!typeFun)
            return std::nullopt;
        typeAliasInformation = std::make_pair(typeName, *typeFun);
        type = typeFun->type;
    }
    else if (auto alias = node->as<Luau::AstStatTypeAlias>())
    {
        auto typeName = alias->name.value;
        auto typeFun = scope->lookupType(typeName);
        if (!typeFun)
            return std::nullopt;
        typeAliasInformation = std::make_pair(typeName, *typeFun);
        type = typeFun->type;
    }
    else if (auto typeTable = node->as<Luau::AstTypeTable>())
    {
        if (auto tableTy = module->astResolvedTypes.find(typeTable))
        {
            type = *tableTy;

            // Check if we are inside one of the properties
            for (auto& prop : typeTable->props)
            {
                if (prop.location.containsClosed(position))
                {
                    auto parentType = Luau::follow(*tableTy);
                    if (auto definitionModuleName = Luau::getDefinitionModuleName(parentType))
                        documentationLocation = {definitionModuleName.value(), prop.location};
                    auto resolvedProperty = lookupProp(parentType, prop.name.value);
                    if (resolvedProperty.size() == 1 && resolvedProperty[0].property.readTy)
                        type = resolvedProperty[0].property.readTy;
                    break;
                }
            }
        }
    }
    else if (auto astType = node->asType())
    {
        if (auto ty = module->astResolvedTypes.find(astType))
        {
            type = *ty;
        }
    }
    else if (auto local = exprOrLocal.getLocal()) // TODO: can we just use node here instead of also calling exprOrLocal?
    {
        type = scope->lookup(local);
        documentationLocation = {moduleName, local->location};
    }
    else if (auto expr = exprOrLocal.getExpr())
    {
        // Special case, we want to check if there is a parent in the ancestry, and if it is an AstTable
        // If so, and we are hovering over a prop, we want to give type info for the assigned expression to the prop
        // rather than just "string"
        auto ancestry = Luau::findAstAncestryOfPosition(*sourceModule, position);
        if (ancestry.size() >= 2 && ancestry.at(ancestry.size() - 2)->is<Luau::AstExprTable>())
        {
            auto parent = ancestry.at(ancestry.size() - 2)->as<Luau::AstExprTable>();
            for (const auto& [kind, key, value] : parent->items)
            {
                if (key && key->location.contains(position))
                {
                    // Return type type of the value
                    if (auto it = module->astTypes.find(value))
                    {
                        type = *it;
                    }
                    break;
                }
            }
        }

        // Handle table properties (so that we can get documentation info)
        if (auto index = expr->as<Luau::AstExprIndexName>())
        {
            if (auto parentIt = module->astTypes.find(index->expr))
            {
                auto parentType = Luau::follow(*parentIt);
                auto indexName = index->index.value;
                if (auto propInformation = lookupProp(parentType, indexName); !propInformation.empty())
                {
                    auto [baseTy, prop] = propInformation[0];
                    if (propInformation.size() == 1)
                    {
                        hoveredProperty = prop;
                        if (!documentationSymbol && prop.documentationSymbol)
                            documentationSymbol = prop.documentationSymbol;
                    }
                    if (propInformation.size() == 1 && prop.readTy)
                        type = prop.readTy;
                    if (auto definitionModuleName = Luau::getDefinitionModuleName(baseTy))
                    {
                        if (prop.location)
                            documentationLocation = {definitionModuleName.value(), prop.location.value()};
                        else if (prop.typeLocation)
                            documentationLocation = {definitionModuleName.value(), prop.typeLocation.value()};
                    }
                }
            }
        }

        // Handle local variables separately to retrieve documentation location info
        if (auto local = expr->as<Luau::AstExprLocal>(); !documentationLocation.has_value() && local && local->local)
        {
            documentationLocation = {moduleName, local->local->location};
        }

        if (!type)
        {
            if (auto it = module->astTypes.find(expr))
            {
                type = *it;
            }
            else if (auto global = expr->as<Luau::AstExprGlobal>())
            {
                type = scope->lookup(global->name);
            }
            else if (auto local = expr->as<Luau::AstExprLocal>())
            {
                type = scope->lookup(local->local);
            }
        }
    }

    if (!type)
        return std::nullopt;
    type = Luau::follow(*type);

    if (!documentationSymbol)
        documentationSymbol = type.value()->documentationSymbol;

    Luau::ToStringOptions opts;
    opts.exhaustive = true;
    opts.useLineBreaks = true;
    opts.functionTypeArguments = true;
    opts.hideNamedFunctionTypeParameters = false;
    opts.hideTableKind = !config.hover.showTableKinds;
    opts.scope = scope;
    std::string typeString = Luau::toString(*type, opts);
    const std::string rawTypeString = typeString;

    if (auto platformHover = platform->handleTypeHover(PlatformHoverContext{moduleName, *sourceModule, module, scope, exprOrLocal, node, position,
            *type, documentationSymbol, hoveredProperty ? &*hoveredProperty : nullptr, documentationLocation, config.hover.showTableKinds}))
        return platformHover;

    // If we have a function and its corresponding name
    if (typeAliasInformation)
    {
        auto [typeName, typeFun] = typeAliasInformation.value();
        typeString = codeBlock("luau", "type " + toStringTypeFun(typeName, typeFun) + " = " + typeString);
    }
    else if (auto ftv = Luau::get<Luau::FunctionType>(*type))
    {
        types::NameOrExpr name = "";
        if (auto localName = exprOrLocal.getName())
            name = localName->value;
        else if (auto expr = exprOrLocal.getExpr())
            name = expr;

        types::ToStringNamedFunctionOpts funcOpts;
        funcOpts.hideTableKind = !config.hover.showTableKinds;
        funcOpts.multiline = config.hover.multilineFunctionDefinitions;
        typeString = codeBlock("luau", types::toStringNamedFunction(module, ftv, name, scope, funcOpts));
    }
    else if (exprOrLocal.getLocal() || node->as<Luau::AstExprLocal>())
    {
        const Luau::AstLocal* local = exprOrLocal.getLocal();
        if (!local)
            local = node->as<Luau::AstExprLocal>()->local;

        std::string builder = local->isConst ? "const " : "local ";
        if (auto name = exprOrLocal.getName())
            builder += name->value;
        else
            builder += Luau::getIdentifier(node->asExpr()).value;
        builder += ": " + typeString;
        typeString = codeBlock("luau", builder);
    }
    else if (auto global = node->as<Luau::AstExprGlobal>())
    {
        // TODO: should we indicate this is a global somehow?
        std::string builder = "type ";
        builder += global->name.value;
        builder += " = " + typeString;
        typeString = codeBlock("luau", builder);
    }
    else if (auto string = node->as<Luau::AstExprConstantString>())
    {
        if (config.hover.includeStringLength)
        {
            auto byteLen = string->value.size;
            auto utf8Len = utflen(string->value.data, string->value.size);
            if (utf8Len && utf8Len != byteLen)
                typeString = codeBlock("luau", "string (" + std::to_string(byteLen) + " bytes, " + std::to_string(utf8Len.value()) + " characters)");
            else
                typeString = codeBlock("luau", "string (" + std::to_string(byteLen) + " bytes)");
        }
        else
            typeString = codeBlock("luau", "string");
    }
    else
    {
        typeString = codeBlock("luau", typeString);
    }

    if (std::optional<std::string> docs;
        documentationSymbol && (docs = printDocumentation(client->documentation, *documentationSymbol)) && docs && !docs->empty())
    {
        prependDocumentation(typeString, *docs);
    }
    else if (auto documentation = getDocumentationForType(*type); documentation && !documentation->empty())
    {
        prependDocumentation(typeString, *documentation);
    }
    else if (auto documentation = getDocumentationForAstNode(moduleName, node, scope); documentation && !documentation->empty())
    {
        prependDocumentation(typeString, *documentation);
    }
    else if (documentationLocation)
    {
        if (auto text = printMoonwaveDocumentation(getComments(documentationLocation->moduleName, documentationLocation->location)); !text.empty())
        {
            prependDocumentation(typeString, text);
        }
    }

    return lsp::Hover{{lsp::MarkupKind::Markdown, typeString}};
}
