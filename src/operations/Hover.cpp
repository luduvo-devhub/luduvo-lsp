#include "LSP/Workspace.hpp"

#include "Luau/AstQuery.h"
#include "Luau/Module.h"
#include "Luau/ToString.h"
#include "LSP/LuauExt.hpp"
#include "LSP/DocumentationParser.hpp"
#include "LSP/HoverSubject.hpp"

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


std::string renderStandardHover(const HoverSubject& subject, const Luau::ModulePtr& module, const ClientHoverConfiguration& config)
{
    auto exprOrLocal = subject.exprOrLocal;
    auto node = subject.node;
    auto scope = subject.scope;
    auto& typeAliasInformation = subject.typeAliasInformation;
    const auto type = subject.type;

    Luau::ToStringOptions opts;
    opts.exhaustive = true;
    opts.useLineBreaks = true;
    opts.functionTypeArguments = true;
    opts.hideNamedFunctionTypeParameters = false;
    opts.hideTableKind = !config.showTableKinds;
    opts.scope = scope;
    std::string typeString = Luau::toString(type, opts);

    // If we have a function and its corresponding name
    if (typeAliasInformation)
    {
        auto [typeName, typeFun] = typeAliasInformation.value();
        typeString = codeBlock("luau", "type " + toStringTypeFun(typeName, typeFun) + " = " + typeString);
    }
    else if (auto ftv = Luau::get<Luau::FunctionType>(type))
    {
        types::NameOrExpr name = "";
        if (auto localName = exprOrLocal.getName())
            name = localName->value;
        else if (auto expr = exprOrLocal.getExpr())
            name = expr;

        types::ToStringNamedFunctionOpts funcOpts;
        funcOpts.hideTableKind = !config.showTableKinds;
        funcOpts.multiline = config.multilineFunctionDefinitions;
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
        if (config.includeStringLength)
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

    if (subject.documentation)
        prependDocumentation(typeString, renderDocumentation(*subject.documentation, /* includeReadOnly: */ true));

    return typeString;
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
    if (!sourceModule || !module)
        return std::nullopt;

    if (Luau::isWithinComment(*sourceModule, position))
        return std::nullopt;

    if (auto hover = platform->handleHover(*textDocument, *sourceModule, position))
        return hover;

    auto subject = resolveHoverSubject(*this, moduleName, *sourceModule, *module, position);
    if (!subject)
    {
        logging::write(logging::Level::Debug, "requests.hover", "No hover subject in " + moduleName);
        return std::nullopt;
    }
    logging::write(logging::Level::Debug, "requests.hover",
        "Resolved hover in " + moduleName + "; declaration kind " + std::to_string(static_cast<int>(subject->declaration.kind)) + "; literal " +
            (subject->literal ? "yes" : "no") + "; documentation " + subject->documentationSymbol.value_or("none"));
    if (auto platformHover = platform->handleTypeHover(PlatformHoverContext{moduleName, *subject, config.hover.showTableKinds}))
        return platformHover;
    return lsp::Hover{{lsp::MarkupKind::Markdown, renderStandardHover(*subject, module, config.hover)}};
}
