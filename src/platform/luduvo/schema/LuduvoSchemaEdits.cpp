#include "Platform/luduvo/schema/LuduvoSchemaEdits.hpp"
#include "Platform/luduvo/schema/LuduvoSchemaExports.hpp"
#include "LSP/AtomicFile.hpp"
#include <algorithm>
#include <filesystem>

namespace luduvo
{
const std::vector<ExportedSchemaFeature>& exportedSchemaFeatures()
{
    static const std::vector<ExportedSchemaFeature> features = {
        {"EventTableFor", "Events", "event-table", "event-table", "EventTable", true, {"EventTable", "EventColumn"}},
        {"EventTableKeys", "EventKeys", "event-table", "event-table", "string", true, {}},
        {"HandlesFor", "ScriptHandles", "handle", "script", "typeof(handles)", false, {"typeof(handles)"}},
    };
    return features;
}
SchemaTypeImports::SchemaTypeImports(
    const Luau::SourceModule& source, const TextDocument& document, const std::string& projectDirectory, const std::string& projectId)
    : source(source)
    , document(document)
    , projectDirectory(projectDirectory)
    , exportPath(projectTypesPath(projectDirectory))
{
    const auto existing = readSharedFile(exportPath);
    std::error_code error;
    const bool exists = std::filesystem::exists(std::filesystem::u8path(exportPath), error);
    generatedDestination = !error && (!exists || (existing && isGeneratedProjectTypes(*existing, projectId)));
    if (!generatedDestination)
        return;
    SchemaSyntax syntax(source.root);
    struct Names : Luau::AstVisitor
    {
        SchemaTypeImports& owner;
        const SchemaSyntax& syntax;
        Names(SchemaTypeImports& owner, const SchemaSyntax& syntax)
            : owner(owner)
            , syntax(syntax)
        {
        }
        bool visit(Luau::AstStatLocal* local) override
        {
            for (auto var : local->vars)
                owner.names.insert(var->name.value);
            if (local->vars.size != 1 || local->values.size != 1)
                return true;
            const auto call = syntax.value(local->values.data[0])->as<Luau::AstExprCall>();
            if (!call || syntax.symbol(call->func) != "require" || call->args.size != 1)
                return true;
            const auto string = syntax.value(call->args.data[0])->as<Luau::AstExprConstantString>();
            if (!string)
                return true;
            ScriptSchema script;
            script.path = owner.document.uri().fsPath();
            script.imports.emplace_back(string->value.data, string->value.size);
            if (importsProjectTypes(script, owner.exportPath))
            {
                owner.imports[local->vars.data[0]] = local->vars.data[0]->name.value;
                owner.importDeclarations.push_back(local);
            }
            return true;
        }
        bool visit(Luau::AstType*) override
        {
            return true;
        }
        bool visit(Luau::AstExprGlobal* global) override
        {
            owner.names.insert(global->name.value);
            return true;
        }
        bool visit(Luau::AstStatTypeAlias* alias) override
        {
            owner.names.insert(alias->name.value);
            for (auto generic : alias->generics)
                owner.names.insert(generic->name.value);
            return true;
        }
        bool visit(Luau::AstExprFunction* function) override
        {
            for (auto argument : function->args)
                owner.names.insert(argument->name.value);
            for (auto generic : function->generics)
                owner.names.insert(generic->name.value);
            return true;
        }
        bool visit(Luau::AstTypeFunction* function) override
        {
            for (auto generic : function->generics)
                owner.names.insert(generic->name.value);
            return true;
        }
        bool visit(Luau::AstStatLocalFunction* function) override
        {
            owner.names.insert(function->name->name.value);
            return true;
        }
    } collect(*this, syntax);
    source.root->visit(&collect);
    for (auto statement : source.root->body)
        if (auto alias = statement->as<Luau::AstStatTypeAlias>())
            if (const auto reference = alias->type->as<Luau::AstTypeReference>())
                if (const auto resolved = feature(*reference))
                    aliases[alias->name.value] = resolved;
}
const ExportedSchemaFeature* SchemaTypeImports::feature(const Luau::AstTypeReference& reference) const
{
    if (reference.prefix)
    {
        if (!reference.prefixLocal || !imports.count(reference.prefixLocal))
            return nullptr;
        for (const auto& item : exportedSchemaFeatures())
            if (item.helper == reference.name.value)
                return &item;
        return nullptr;
    }
    const auto alias = aliases.find(reference.name.value);
    return alias == aliases.end() ? nullptr : alias->second;
}
lsp::CompletionItem SchemaTypeImports::completion(const ExportedSchemaFeature& feature, const std::string& script, bool snippets) const
{
    const auto unique = [&](const std::string& preferred)
    {
        std::string result = preferred;
        for (size_t suffix = 2; names.count(result); ++suffix)
            result = preferred + std::to_string(suffix);
        return result;
    };
    std::string alias;
    const auto relativeScript = std::filesystem::u8path(normalizeContextPath(script))
                                    .lexically_relative(std::filesystem::u8path(normalizeContextPath(projectDirectory)))
                                    .generic_u8string();
    for (const auto& existing : aliases)
        if (existing.second->helper == feature.helper)
            for (auto statement : source.root->body)
                if (const auto declaration = statement->as<Luau::AstStatTypeAlias>(); declaration && existing.first == declaration->name.value)
                    if (const auto reference = declaration->type->as<Luau::AstTypeReference>();
                        reference && reference->parameters.size == 1 + feature.contextArguments.size())
                    {
                        const auto argument = reference->parameters.data[0].type;
                        bool matches = false;
                        if (feature.genericAlias && declaration->generics.size == 1 && argument)
                            if (const auto name = argument->as<Luau::AstTypeReference>())
                                matches = !name->prefix && name->name == declaration->generics.data[0]->name;
                        if (!feature.genericAlias && declaration->generics.size == 0 && argument)
                            if (const auto name = argument->as<Luau::AstTypeSingletonString>())
                                matches = std::string_view(name->value.data, name->value.size) == relativeScript;
                        if (matches)
                            alias = existing.first;
                    }
    lsp::CompletionItem item;
    item.label = alias.empty() ? unique(feature.preferredAlias) : alias;
    item.kind = lsp::CompletionItemKind::TypeParameter;
    item.filterText = item.label + " " + feature.helper;
    item.detail = "Project " + feature.family + " type (imports generated project types)";
    item.insertText = item.label;
    if (feature.genericAlias)
    {
        item.insertText = item.label + (snippets ? "<${1:\"\"}>$0" : "<>");
        item.insertTextFormat = snippets ? lsp::InsertTextFormat::Snippet : lsp::InsertTextFormat::PlainText;
    }
    if (!alias.empty())
        return item;
    std::map<const Luau::AstLocal*, std::string> visibleImports;
    for (auto statement : source.root->body)
        if (const auto local = statement->as<Luau::AstStatLocal>(); local && local->vars.size == 1)
            if (const auto found = imports.find(local->vars.data[0]); found != imports.end())
                visibleImports.insert(*found);
    std::string insertion;
    std::string module;
    if (!visibleImports.empty())
        module = visibleImports.begin()->second;
    else
    {
        module = unique("LSP");
        const auto target = std::filesystem::u8path(normalizeContextPath(projectDirectory)) / "scripts/types/ProjectTypes";
        auto path =
            target.lexically_relative(std::filesystem::u8path(normalizeContextPath(document.uri().fsPath())).parent_path()).generic_u8string();
        if (path.find('.') != 0)
            path = "./" + path;
        insertion += "local " + module + " = require(" + quoteSchemaString(path) + ")\n";
    }
    insertion += "type " + item.label + (feature.genericAlias ? "<Name>" : "") + " = " + module + "." + feature.helper + "<" +
                 (feature.genericAlias ? "Name" : quoteSchemaString(relativeScript));
    for (const auto& argument : feature.contextArguments)
        insertion += ", " + argument;
    insertion += ">\n";
    unsigned line = 0;
    for (const auto& comment : source.hotcomments)
        line = std::max(line, comment.location.end.line + 1);
    if (!imports.empty())
        for (auto statement : source.root->body)
            if (const auto local = statement->as<Luau::AstStatLocal>(); local && local->vars.size == 1 && imports.count(local->vars.data[0]))
                line = std::max(line, statement->location.end.line + 1);
    const lsp::Position position{line, 0};
    item.additionalTextEdits.push_back({{position, position}, insertion});
    return item;
}
std::vector<lsp::TextEdit> SchemaTypeImports::remove(const std::set<std::string>& families, std::vector<std::string>& warnings) const
{
    if (!generatedDestination)
    {
        warnings.push_back("The project type destination is user-owned or unreadable: " + exportPath);
        return {};
    }
    std::vector<lsp::TextEdit> edits;
    std::vector<Luau::Location> removed;
    struct Visitor : Luau::AstVisitor
    {
        const SchemaTypeImports& owner;
        const std::set<std::string>& families;
        std::vector<lsp::TextEdit>& edits;
        std::vector<std::pair<const Luau::AstLocal*, Luau::Location>> runtimeUses;
        bool retained = false;
        std::map<std::string, size_t> aliasCounts;
        Visitor(const SchemaTypeImports& owner, const std::set<std::string>& families, std::vector<lsp::TextEdit>& edits)
            : owner(owner)
            , families(families)
            , edits(edits)
        {
        }
        bool visit(Luau::AstType*) override
        {
            return true;
        }
        bool visit(Luau::AstTypeReference* reference) override
        {
            const auto feature = owner.feature(*reference);
            if (!feature)
            {
                if (reference->prefixLocal && owner.imports.count(reference->prefixLocal))
                    retained = true;
                return true;
            }
            if (!families.empty() && !families.count(feature->family))
            {
                retained = true;
                return true;
            }
            edits.push_back({owner.document.convertLocation(reference->location), feature->broadType});
            return false;
        }
        bool visit(Luau::AstStatTypeAlias* alias) override
        {
            ++aliasCounts[alias->name.value];
            for (auto parameter : alias->generics)
                ++aliasCounts[parameter->name.value];
            return true;
        }
        bool visit(Luau::AstExprFunction* function) override
        {
            for (auto parameter : function->generics)
                ++aliasCounts[parameter->name.value];
            return true;
        }
        bool visit(Luau::AstTypeFunction* function) override
        {
            for (auto parameter : function->generics)
                ++aliasCounts[parameter->name.value];
            return true;
        }
        bool visit(Luau::AstExprLocal* local) override
        {
            runtimeUses.push_back({local->local, local->location});
            return true;
        }
    } visitor(*this, families, edits);
    source.root->visit(&visitor);
    for (const auto& alias : aliases)
        if (visitor.aliasCounts[alias.first] > 1)
        {
            warnings.push_back("Cannot safely remove shadowed type alias " + alias.first + " in " + document.uri().fsPath());
            return {};
        }
    for (auto statement : source.root->body)
    {
        if (const auto alias = statement->as<Luau::AstStatTypeAlias>())
        {
            const auto managed = aliases.find(alias->name.value);
            if (!alias->exported && managed != aliases.end() && (families.empty() || families.count(managed->second->family)))
                removed.push_back(alias->location);
        }
    }
    const auto isRemoved = [&](const Luau::Location& location)
    {
        return std::any_of(removed.begin(), removed.end(),
            [&](const auto& range)
            {
                return range.begin <= location.begin && range.end >= location.end;
            });
    };
    for (auto it = importDeclarations.rbegin(); it != importDeclarations.rend(); ++it)
    {
        const auto local = *it;
        const bool used = std::any_of(visitor.runtimeUses.begin(), visitor.runtimeUses.end(),
            [&](const auto& use)
            {
                return use.first == local->vars.data[0] && !isRemoved(use.second);
            });
        if (!used && !visitor.retained)
            removed.push_back(local->location);
    }
    edits.erase(std::remove_if(edits.begin(), edits.end(),
                    [&](const auto& edit)
                    {
                        return std::any_of(removed.begin(), removed.end(),
                            [&](const auto& location)
                            {
                                const auto range = document.convertLocation(location);
                                return !(edit.range.start < range.start) && !(edit.range.end > range.end);
                            });
                    }),
        edits.end());
    for (const auto& location : removed)
        edits.push_back({document.convertLocation(location), ""});
    return edits;
}
} // namespace luduvo
