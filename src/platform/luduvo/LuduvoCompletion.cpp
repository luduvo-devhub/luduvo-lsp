#include "Platform/luduvo/LuduvoPlatform.hpp"
#include "Platform/luduvo/LuduvoLifecycle.hpp"
#include "Platform/luduvo/LuduvoSymbols.hpp"
#include "Platform/luduvo/LuduvoTargetResolver.hpp"

#include "LSP/LuauExt.hpp"
#include "LSP/TextDocument.hpp"
#include "LSP/Workspace.hpp"
#include "Luau/StringUtils.h"
#include "Luau/Type.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace
{
Luau::AstExprCall* callFromAncestry(const std::vector<Luau::AstNode*>& ancestry)
{
    for (auto it = ancestry.rbegin(); it != ancestry.rend(); ++it)
        if (auto call = (*it)->as<Luau::AstExprCall>())
            return call;
    return nullptr;
}

size_t activeArgument(const Luau::AstExprCall& call, const Luau::Position& position)
{
    size_t result = 0;
    for (Luau::AstExpr* argument : call.args)
    {
        if (argument->location.containsClosed(position) || argument->location.begin > position)
            break;
        ++result;
    }
    return result;
}

std::optional<lsp::Range> resourceCompletionRange(
    const TextDocument& textDocument, const Luau::AstExprCall& call, size_t argumentIndex, const Luau::Position& position)
{
    const lsp::Position cursor = textDocument.convertPosition(position);
    if (argumentIndex >= call.args.size)
        return lsp::Range{cursor, cursor};

    Luau::AstExpr* argument = call.args.data[argumentIndex];
    if (auto string = argument->as<Luau::AstExprConstantString>())
        return textDocument.convertLocation(string->location);

    if (auto error = argument->as<Luau::AstExprError>())
    {
        lsp::Range range = textDocument.convertLocation(error->location);
        if (range.start > cursor)
            return std::nullopt;
        if (range.end > cursor)
            range.end = cursor;

        const std::string current = textDocument.getText(range);
        if (!current.empty() && current.front() != '"' && current.front() != '\'' && current.front() != '`')
            return std::nullopt;
        return range;
    }

    return std::nullopt;
}

std::string resourceCompletionDocumentation(
    const ClientLuduvoConfiguration& configuration, const LuduvoResourceGroup& group, const LuduvoResourceName& resource)
{
    if (!resource.documentation)
        return {};
    if (configuration.hover.presentation == LuduvoHoverPresentation::Standard)
        return *resource.documentation;

    std::string result = "# " + resource.name + "\n`" + group.kind + "`";
    if (resource.builtIn)
        result += " · `built-in`";
    result += "\n\n" + *resource.documentation;
    return result;
}

bool isLifecycleCompletionPosition(
    const TextDocument& textDocument, const Luau::SourceModule& sourceModule, const std::vector<Luau::AstNode*>& ancestry, Luau::Position position)
{
    for (Luau::AstNode* ancestor : ancestry)
    {
        if (const auto block = ancestor->as<Luau::AstStatBlock>(); block && block != sourceModule.root)
            return false;

        if (ancestor->as<Luau::AstExprFunction>())
            return false;

        if (Luau::AstExpr* expression = ancestor->asExpr();
            expression && !expression->is<Luau::AstExprGlobal>() && !expression->is<Luau::AstExprError>())
            return false;
    }

    const std::string line = textDocument.getLine(position.line);
    if (position.column > line.size())
        return false;

    const std::string_view prefix(line.data(), position.column);
    const auto firstNonWhitespace = std::find_if_not(prefix.begin(), prefix.end(),
        [](unsigned char character)
        {
            return std::isspace(character);
        });
    if (firstNonWhitespace == prefix.end())
        return true;

    if (!std::isalpha(static_cast<unsigned char>(*firstNonWhitespace)) && *firstNonWhitespace != '_')
        return false;

    return std::all_of(firstNonWhitespace + 1, prefix.end(),
        [](unsigned char character)
        {
            return std::isalnum(character) || character == '_';
        });
}

bool hasTopLevelBinding(const Luau::SourceModule& sourceModule, std::string_view name)
{
    for (Luau::AstStat* statement : sourceModule.root->body)
    {
        if (const auto function = statement->as<Luau::AstStatFunction>())
        {
            if (const auto global = function->name->as<Luau::AstExprGlobal>(); global && global->name.value == name)
                return true;
        }
        else if (const auto function = statement->as<Luau::AstStatLocalFunction>())
        {
            if (function->name->name.value == name)
                return true;
        }
        else if (const auto local = statement->as<Luau::AstStatLocal>())
        {
            if (std::any_of(local->vars.begin(), local->vars.end(),
                    [name](const Luau::AstLocal* variable)
                    {
                        return variable->name.value == name;
                    }))
                return true;
        }
        else if (const auto assignment = statement->as<Luau::AstStatAssign>())
        {
            if (std::any_of(assignment->vars.begin(), assignment->vars.end(),
                    [name](const Luau::AstExpr* variable)
                    {
                        const auto global = variable->as<Luau::AstExprGlobal>();
                        return global && global->name.value == name;
                    }))
                return true;
        }
    }
    return false;
}

bool supportsSnippets(const WorkspaceFolder* workspaceFolder)
{
    if (!workspaceFolder || !workspaceFolder->client)
        return false;

    const lsp::ClientCapabilities& capabilities = workspaceFolder->client->capabilities;
    return capabilities.textDocument && capabilities.textDocument->completion && capabilities.textDocument->completion->completionItem &&
           capabilities.textDocument->completion->completionItem->snippetSupport;
}

} // namespace

void LuduvoPlatform::handleCompletion(const TextDocument& textDocument, const Luau::SourceModule& sourceModule, const Luau::ModulePtr& typedModule,
    const std::vector<Luau::AstNode*>& ancestry, Luau::Position position, std::vector<lsp::CompletionItem>& items)
{
    if (isLifecycleCompletionPosition(textDocument, sourceModule, ancestry, position))
    {
        const bool snippets = supportsSnippets(workspaceFolder);
        for (const LuduvoLifecycleFunction& lifecycle : luduvoLifecycleFunctions)
        {
            if (hasTopLevelBinding(sourceModule, lifecycle.name))
                continue;

            lsp::CompletionItem item;
            item.label = std::string(lifecycle.name);
            item.kind = lsp::CompletionItemKind::Snippet;
            item.detail = "Luduvo lifecycle function";
            item.filterText = std::string(lifecycle.name);
            item.insertText =
                "function " + std::string(lifecycle.name) + "(" + std::string(lifecycle.parameters) + ")\n\t" + (snippets ? "$0" : "") + "\nend";
            item.insertTextFormat = snippets ? lsp::InsertTextFormat::Snippet : lsp::InsertTextFormat::PlainText;
            items.push_back(std::move(item));
        }
    }

    if (!configuration.resources.completion.enabled || !typedModule)
        return;

    Luau::AstExprCall* call = callFromAncestry(ancestry);
    if (!call)
        return;

    Luau::TypeId* callable = typedModule->astTypes.find(call->func);
    if (!callable)
        return;

    Luau::TypeId callableType = Luau::follow(*callable);
    const std::optional<std::string> documentationSymbol =
        luduvo::resolveCallableDocumentationSymbol(sourceModule, *typedModule, *call, callableType);
    const std::optional<std::string> canonicalSymbol = canonicalLuduvoSymbol(documentationSymbol);
    if (!canonicalSymbol || contains(configuration.resources.completion.disabledApis, *canonicalSymbol))
        return;

    const size_t argumentIndex = activeArgument(*call, position);
    const bool implicitSelf = call->self || findCallMetamethod(callableType).has_value();
    const size_t typeParameterIndex = argumentIndex + (implicitSelf ? 1 : 0);

    std::optional<LuduvoResolvedResourceBinding> activeBinding;
    for (const LuduvoResolvedResourceBinding& binding : resolveResourceBindings(documentationSymbol, callableType))
    {
        const bool matches = binding.variadic ? typeParameterIndex >= binding.typeParameterIndex : typeParameterIndex == binding.typeParameterIndex;
        if (matches)
        {
            activeBinding = binding;
            break;
        }
    }
    if (!activeBinding || contains(configuration.resources.completion.disabledKinds, activeBinding->kind))
        return;

    const auto replacementRange = resourceCompletionRange(textDocument, *call, argumentIndex, position);
    if (!replacementRange)
        return;

    std::unordered_set<std::string> existingLabels;
    existingLabels.reserve(items.size());
    for (const lsp::CompletionItem& item : items)
        existingLabels.insert(item.label);

    const LuduvoResourceGroup* resourceGroup = resourceCatalog.group(activeBinding->kind);
    if (!resourceGroup)
        return;

    for (const LuduvoResourceName& resource : resourceGroup->resources)
    {
        const std::string& name = resource.name;
        if (name.empty() || !existingLabels.insert(name).second)
            continue;

        lsp::CompletionItem item;
        item.label = name;
        item.kind = lsp::CompletionItemKind::Constant;
        item.detail = resource.builtIn ? "Luduvo " + resourceGroup->kind + " (built-in)" : resourceGroup->kind;
        if (std::string documentation = resourceCompletionDocumentation(configuration, *resourceGroup, resource); !documentation.empty())
            item.documentation = lsp::MarkupContent{lsp::MarkupKind::Markdown, std::move(documentation)};
        item.sortText = name;
        item.filterText = name;
        item.textEdit = lsp::TextEdit{*replacementRange, "\"" + Luau::escape(name) + "\""};
        items.push_back(std::move(item));
    }
}
