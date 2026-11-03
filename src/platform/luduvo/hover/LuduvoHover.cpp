#include "Platform/luduvo/LuduvoPlatform.hpp"

#include "LSP/DocumentationParser.hpp"
#include "LSP/LuauExt.hpp"
#include "LSP/Workspace.hpp"
#include "Platform/luduvo/hover/LuduvoHoverAnalysis.hpp"
#include "Platform/luduvo/hover/LuduvoHoverRenderer.hpp"
#include "Platform/luduvo/LuduvoSymbols.hpp"

#include "Luau/ToString.h"

using namespace luduvo::hover;

std::optional<lsp::Hover> LuduvoPlatform::handleTypeHover(const PlatformHoverContext& context)
{
    if (configuration.context.lazyRefresh && fileResolver)
        notifyContextState(fileResolver->getUri(context.moduleName), false);
    const auto& subject = context.subject;
    if (subject.literal)
        return std::nullopt;
    if (configuration.hover.presentation != LuduvoHoverPresentation::Rich)
    {
        const auto debug = schemas ? schemas->debug(context.moduleName, subject.node->asExpr(), subject.type) : "";
        if (debug.empty() || !workspaceFolder)
            return std::nullopt;
        return lsp::Hover{{lsp::MarkupKind::Markdown, renderStandardHover(subject, workspaceFolder->getModule(context.moduleName),
                                                          workspaceFolder->client->getConfiguration(workspaceFolder->rootUri).hover) +
                                                          debug}};
    }

    Luau::ToStringOptions options;
    options.exhaustive = true;
    options.useLineBreaks = true;
    options.functionTypeArguments = true;
    options.hideNamedFunctionTypeParameters = false;
    options.hideTableKind = !context.showTableKinds;
    options.scope = subject.scope;
    options.maxTableLength = 0;
    options.maxTypeLength = 0;

    const std::string rawType = Luau::toString(subject.type, options);
    const auto& declarationTarget = subject.declaration;
    const bool directMemberInitializer = subject.directMemberInitializer;

    const auto& documentationSymbol = subject.documentationSymbol;
    std::optional<PrintedDocumentation> documentation;
    if (subject.documentation)
        documentation = *subject.documentation;

    const Luau::TypeId resolvedType = Luau::follow(subject.type);
    std::optional<std::string> fallbackTitle = types::getTypeName(resolvedType);
    if (!fallbackTitle && Luau::is<Luau::PrimitiveType, Luau::SingletonType, Luau::UnionType, Luau::IntersectionType>(resolvedType))
        fallbackTitle = rawType;
    if (!fallbackTitle && !declarationTarget.name.empty())
        fallbackTitle = declarationTarget.name;

    const std::string expandedType = getRichHoverTypeDefinition(subject.type, options);
    const std::string declaration = makeDeclaration(subject.type, normalizeRichHoverType(subject.type, options), expandedType, declarationTarget);
    std::optional<Luau::Property> property;
    if (subject.property)
        property = *subject.property;
    const auto constrainedResources = resolveResourceBindings(documentationSymbol, subject.type);
    RichHoverInput hoverInput{
        subject.type, declaration, documentationSymbol, std::move(property), documentation, fallbackTitle, directMemberInitializer, true};
    hoverInput.overloads = makeRichHoverOverloads(subject.type, declarationTarget, options, constrainedResources);
    std::string rendered = renderRichHover(configuration.hover, analyzeRichHover(constrainedResources, hoverInput));
    if (schemas)
        rendered += schemas->debug(context.moduleName, subject.node->asExpr(), subject.type);
    return lsp::Hover{{lsp::MarkupKind::Markdown, std::move(rendered)}};
}

std::optional<lsp::MarkupContent> LuduvoPlatform::handleCompletionDocumentation(const PlatformCompletionDocumentationContext& context)
{
    if (!context.entry.type || !workspaceFolder)
        return std::nullopt;

    if (configuration.hover.presentation != LuduvoHoverPresentation::Rich)
    {
        const auto debug = schemas ? schemas->debug(context.moduleName, nullptr, *context.entry.type) : "";
        if (debug.empty())
            return std::nullopt;
        return lsp::MarkupContent{lsp::MarkupKind::Markdown, context.fallbackDocumentation.value_or("") + debug};
    }

    auto documentationSymbol =
        resolveDocumentationSymbol(*context.entry.type, context.entry.documentationSymbol, getEnvironmentForModule(context.moduleName));
    std::optional<PrintedDocumentation> documentation;
    if (documentationSymbol)
        documentation =
            getDocumentation(workspaceFolder->client->documentation, *documentationSymbol, &workspaceFolder->client->documentationMetadata);
    if (!documentation && context.fallbackDocumentation && !context.fallbackDocumentation->empty())
        documentation = PrintedDocumentation{*context.fallbackDocumentation, std::nullopt};

    std::optional<Luau::Property> property;
    if (context.entry.prop)
        property = **context.entry.prop;
    const auto constrainedResources = resolveResourceBindings(documentationSymbol, *context.entry.type);
    std::string rendered = renderRichHover(configuration.hover,
        analyzeRichHover(constrainedResources, RichHoverInput{*context.entry.type, Luau::toString(Luau::follow(*context.entry.type)),
                                                   documentationSymbol, std::move(property), documentation, context.name, false, false}));
    if (schemas)
        rendered += schemas->debug(context.moduleName, nullptr, *context.entry.type);
    return lsp::MarkupContent{lsp::MarkupKind::Markdown, std::move(rendered)};
}

std::optional<lsp::MarkupContent> LuduvoPlatform::handleSignatureHelpDocumentation(const PlatformSignatureHelpDocumentationContext& context)
{
    if (configuration.hover.presentation != LuduvoHoverPresentation::Rich || !workspaceFolder)
        return std::nullopt;

    auto documentationSymbol = resolveDocumentationSymbol(context.type, context.documentationSymbol, getEnvironmentForModule(context.moduleName));
    if (!getLuduvoSymbolIdentity(documentationSymbol))
        return std::nullopt;

    std::optional<PrintedDocumentation> documentation;
    if (documentationSymbol)
        documentation =
            getDocumentation(workspaceFolder->client->documentation, *documentationSymbol, &workspaceFolder->client->documentationMetadata);
    if (!documentation && !context.fallbackDocumentation.empty())
        documentation = PrintedDocumentation{context.fallbackDocumentation, std::nullopt};

    const auto constrainedResources = resolveResourceBindings(documentationSymbol, context.type);
    return lsp::MarkupContent{lsp::MarkupKind::Markdown,
        renderRichHover(configuration.hover,
            analyzeRichHover(constrainedResources, RichHoverInput{context.type, Luau::toString(Luau::follow(context.type)), documentationSymbol,
                                                       std::nullopt, documentation, std::nullopt, false, false}))};
}

void LuduvoPlatform::handleSignatureHelp(const PlatformSignatureHelpContext& context)
{
    if (configuration.hover.presentation != LuduvoHoverPresentation::Rich)
        return;

    auto documentationSymbol = resolveDocumentationSymbol(context.type, context.documentationSymbol, getEnvironmentForModule(context.moduleName));
    if (!getLuduvoSymbolIdentity(documentationSymbol))
        return;

    for (lsp::SignatureInformation& signature : context.signatureHelp.signatures)
    {
        const size_t activeParameter = signature.activeParameter.value_or(context.signatureHelp.activeParameter);
        std::optional<std::string> activeParameterDocumentation;

        if (signature.parameters)
        {
            if (activeParameter < signature.parameters->size())
            {
                const auto& documentation = signature.parameters->at(activeParameter).documentation;
                if (documentation && !documentation->value.empty())
                    activeParameterDocumentation = documentation->value;
            }

            for (lsp::ParameterInformation& parameter : *signature.parameters)
                parameter.documentation = std::nullopt;
        }

        if (!activeParameterDocumentation)
            continue;

        if (!signature.documentation)
            signature.documentation = lsp::MarkupContent{lsp::MarkupKind::Markdown, ""};
        if (!signature.documentation->value.empty())
            signature.documentation->value += "\n\n";
        signature.documentation->value += "**Active parameter**\n\n" + *activeParameterDocumentation;
    }
}
