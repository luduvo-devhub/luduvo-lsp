#include "Platform/luduvo/schema/LuduvoSchemaService.hpp"
#include "Platform/luduvo/hover/LuduvoHoverRenderer.hpp"
#include "Platform/luduvo/schema/LuduvoSchemaExports.hpp"
#include "Platform/luduvo/schema/LuduvoSchemaEdits.hpp"
#include "Platform/luduvo/LuduvoDefinitionResolver.hpp"
#include "Platform/luduvo/LuduvoNotifications.hpp"
#include "Platform/luduvo/LuduvoTargetResolver.hpp"
#include "LSP/Workspace.hpp"
#include "LSP/LuauExt.hpp"
#include "LSP/AtomicFile.hpp"
#include "LSP/TextDocument.hpp"
#include "Luau/Allocator.h"
#include "Luau/Parser.h"
#include "Luau/Lexer.h"
#include "Luau/StringUtils.h"
#include "LuauFileUtils.hpp"
#include <filesystem>
#include <cctype>

namespace luduvo
{
namespace
{
struct LiteralOverlay final
    : PlatformAnalysisOverlay
    , Luau::AstVisitor
{
    struct Patch
    {
        Luau::AstExprCall* call;
        Luau::AstExpr* original;
    };
    std::vector<Patch> patches;
    struct TypePatch
    {
        Luau::AstTypeReference* reference;
        Luau::AstName name;
        std::optional<Luau::AstName> prefix;
        std::optional<Luau::Location> prefixLocation;
        Luau::AstLocal* prefixLocal;
        Luau::AstArray<Luau::AstTypeOrPack> parameters;
    };
    std::vector<TypePatch> types;
    std::vector<std::shared_ptr<Luau::Allocator>> allocators;
    Luau::Allocator* allocator = nullptr;
    const SchemaSyntax* syntax = nullptr;
    const SchemaTypeImports* imports = nullptr;
    Luau::AstNameTable* typeNames = nullptr;
    std::set<std::string> names;
    std::function<void()> completed;
    ~LiteralOverlay() override
    {
        for (auto it = patches.rbegin(); it != patches.rend(); ++it)
            it->call->args.data[0] = it->original;
        for (const auto& patch : types)
        {
            patch.reference->name = patch.name;
            patch.reference->prefix = patch.prefix;
            patch.reference->prefixLocation = patch.prefixLocation;
            patch.reference->prefixLocal = patch.prefixLocal;
            patch.reference->parameters = patch.parameters;
        }
        if (completed)
            completed();
    }
    bool visit(Luau::AstType*) override
    {
        return true;
    }
    bool visit(Luau::AstTypeReference* reference) override
    {
        if (!reference->prefix || !reference->parameters.size || !imports)
            return true;
        const auto feature = imports->feature(*reference);
        if (!feature)
            return true;
        types.push_back({reference, reference->name, reference->prefix, reference->prefixLocation, reference->prefixLocal, reference->parameters});
        reference->name = typeNames->getOrAdd(SchemaTypes::internalAlias(feature->helper).c_str());
        reference->prefix.reset();
        reference->prefixLocation.reset();
        reference->prefixLocal = nullptr;
        reference->parameters.size = 1;
        return true;
    }
    bool visit(Luau::AstExprCall* call) override
    {
        if (call->self || !call->args.size || syntax->symbol(call->func) != "game.EventTable")
            return true;
        auto string = call->args.data[0]->as<Luau::AstExprConstantString>();
        if (!string || !names.count(std::string(string->value.data, string->value.size)))
            return true;
        const auto annotation = allocator->alloc<Luau::AstTypeSingletonString>(string->location, string->value);
        patches.push_back({call, call->args.data[0]});
        call->args.data[0] = allocator->alloc<Luau::AstExprTypeAssertion>(string->location, string, annotation);
        return true;
    }
};
std::string stateDirectory(const ClientLuduvoConfiguration& config)
{
    return defaultContextStateDirectory(config.directories.local, config.directories.context);
}
} // namespace
SchemaService::SchemaService(WorkspaceFolder& workspace, ClientLuduvoConfiguration configuration, const LuduvoBindingRegistry& bindings,
    const LuduvoResourceCatalog& resources)
    : workspace(workspace)
    , workspacePath(workspace.rootUri.fsPath())
    , configuration(std::move(configuration))
    , bindings(bindings)
    , resources(resources)
{
}
SchemaService::~SchemaService()
{
    for (const auto& project : projects)
        writeSchemaExportReference(stateDirectory(configuration), workspacePath, project.first, false);
}
void SchemaService::configure(const ClientLuduvoConfiguration& value)
{
    for (const auto& project : projects)
        writeSchemaExportReference(stateDirectory(configuration), workspacePath, project.first, false);
    configuration = value;
    projects.clear();
    scriptProjects.clear();
    currentTypes.clear();
}
std::shared_ptr<const ProjectSchemas> SchemaService::synchronize(const Luau::ModuleName& moduleName)
{
    if (!configuration.context.enabled || synchronizing)
        return nullptr;
    struct Guard
    {
        bool& active;
        Guard(bool& value)
            : active(value)
        {
            active = true;
        }
        ~Guard()
        {
            active = false;
        }
    } guard(synchronizing);
    const auto script = normalizeContextPath(workspace.fileResolver.getUri(moduleName).fsPath());
    ContextProvider provider(stateDirectory(configuration), workspacePath);
    auto candidates = provider.findForScript(script);
    if (candidates.size() != 1)
    {
        scriptProjects.erase(script);
        logging::write(logging::Level::Debug, "context.schemas", "No unambiguous project for " + script);
        return nullptr;
    }
    const auto& candidate = candidates.front();
    const auto saved = loadProjectSchemas(stateDirectory(configuration), candidate.project.id);
    std::string coreDirectory;
    if (const auto content = resolveActiveLuduvoContent(configuration))
        coreDirectory = content->versionRoot;
    auto project =
        std::make_shared<ProjectSchemas>(collectProjectSchemas(candidate.project, candidate.snapshot, saved ? &*saved : nullptr, {}, coreDirectory));
    project->captureDetails = candidate.captureDetails;
    for (const auto& entry : workspace.fileResolver.managedFiles)
    {
        const auto path = normalizeContextPath(entry.first.fsPath());
        const auto existing = project->scripts.find(path);
        if (existing == project->scripts.end())
            continue;
        const auto source = entry.second.getText();
        if (existing->second.fingerprint != schemaFingerprint(source))
        {
            const bool builtin = existing->second.builtin;
            existing->second = extractScriptSchema(path, source, true);
            existing->second.builtin = builtin;
            project->updatedAt = schemaTimeNow();
        }
    }
    const auto key = [](const ProjectSchemas& view)
    {
        auto record = nlohmann::json(view);
        record.erase("updatedAt");
        record["captureDetails"] = view.captureDetails;
        for (auto& file : record["scripts"].items())
            file.value().erase("updatedAt");
        return schemaFingerprint(record.dump());
    };
    const auto fingerprint = key(*project);
    const auto old = projects.find(project->projectId);
    if (old == projects.end() || key(*old->second) != fingerprint)
    {
        projects[project->projectId] = project;
        for (auto types = currentTypes.begin(); types != currentTypes.end();)
            if (types->first.find(project->projectId + ":") == 0)
                types = currentTypes.erase(types);
            else
                ++types;
        // Schema changes affect consumers, not just the script declaring a field.
        std::vector<std::string> dirty;
        for (const auto& node : workspace.frontend.sourceNodes)
            if (project->scripts.count(normalizeContextPath(workspace.fileResolver.getUri(node.first).fsPath())))
                dirty.push_back(node.first);
        for (const auto& name : dirty)
            workspace.frontend.markDirty(name);
        logging::write(logging::Level::Debug, "context.schemas", "Updated schema view for " + project->projectId);
    }
    else
        project = old->second;
    for (const auto& file : project->scripts)
        scriptProjects[file.first] = project->projectId;
    scriptProjects[normalizeContextPath(project->exportPath)] = project->projectId;
    bool exportsReferenced = false;
    for (const auto& file : project->scripts)
        if (file.second.unsaved && importsProjectTypes(file.second, project->exportPath))
            exportsReferenced = true;
    writeSchemaExportReference(stateDirectory(configuration), workspacePath, project->projectId, exportsReferenced);
    return project;
}
std::unique_ptr<PlatformAnalysisOverlay> SchemaService::prepare(const Luau::ModuleName& moduleName)
{
    auto project = synchronize(moduleName);
    if (!project)
    {
        if (currentTypes.empty())
            return nullptr;
        auto lifetime = std::make_unique<LiteralOverlay>();
        lifetime->completed = [this]
        {
            retainCheckedModules();
        };
        return lifetime;
    }
    workspace.frontend.parse(moduleName);
    auto overlay = std::make_unique<LiteralOverlay>();
    overlay->completed = [this]
    {
        retainCheckedModules();
    };
    // parse() prepares the dependency graph, so required scripts use the same pass.
    for (const auto& entry : workspace.frontend.sourceModules)
    {
        const auto& source = entry.second;
        const auto path = normalizeContextPath(workspace.fileResolver.getUri(entry.first).fsPath());
        const auto selected = scriptProjects.find(path);
        if (selected == scriptProjects.end())
            continue;
        const auto view = projects.find(selected->second);
        if (view == projects.end() || !workspace.frontend.isDirty(entry.first))
            continue;
        SchemaSyntax syntax(source->root);
        const auto managed = workspace.fileResolver.getTextDocument(workspace.fileResolver.getUri(entry.first));
        const auto text = managed ? managed->getText() : Luau::FileUtils::readFile(path).value_or("");
        TextDocument document(Uri::file(path), "luau", 0, text);
        SchemaTypeImports imports(*source, document, view->second->directory, view->second->projectId);
        overlay->imports = &imports;
        overlay->typeNames = source->names.get();
        overlay->syntax = &syntax;
        overlay->allocator = source->allocator.get();
        overlay->allocators.push_back(source->allocator);
        overlay->names = eventSchemaNames(*view->second);
        source->root->visit(overlay.get());
    }
    return overlay;
}
void SchemaService::bind(const Luau::ModuleName& moduleName, const Luau::ScopePtr& scope)
{
    if (!currentTypes.empty())
        pendingTypes.try_emplace(moduleName);
    const auto path = normalizeContextPath(workspace.fileResolver.getUri(moduleName).fsPath());
    const auto found = scriptProjects.find(path);
    if (found == scriptProjects.end())
        return;
    const auto eventBase = scope->lookupType("EventTable");
    if (!eventBase)
        return;
    const auto key = found->second + ":" + std::to_string(reinterpret_cast<uintptr_t>(eventBase->type));
    auto& types = currentTypes[key];
    if (!types)
    {
        types = std::make_shared<SchemaTypes>(projects.at(found->second), scope->parent, workspace.frontend.builtinTypes);
        retainedTypes.push_back(types);
    }
    types->bind(scope, path);
    pendingTypes[moduleName].push_back(types);
}
void SchemaService::retainCheckedModules()
{
    struct Lifetime
    {
        std::vector<std::shared_ptr<SchemaTypes>> types;
        std::vector<Luau::ModulePtr> dependencies;
        Luau::ModulePtr module;
    };
    // Attach ownership to checked modules instead of retaining every index
    // generation for the workspace's entire lifetime. Flatten pending require
    // cycles so their module owners cannot form shared_ptr cycles.
    for (const auto& checked : pendingTypes)
    {
        auto owner = std::make_shared<Lifetime>();
        owner->module = workspace.frontend.moduleResolver.getModule(checked.first);
        if (!owner->module)
            continue;
        std::set<Luau::ModuleName> visited;
        const auto collect = [&](const auto& self, const Luau::ModuleName& name) -> void
        {
            if (!visited.insert(name).second)
                return;
            if (const auto pending = pendingTypes.find(name); pending != pendingTypes.end())
                owner->types.insert(owner->types.end(), pending->second.begin(), pending->second.end());
            const auto node = workspace.frontend.sourceNodes.find(name);
            if (node == workspace.frontend.sourceNodes.end())
                return;
            for (const auto& dependency : node->second->requireSet)
                if (pendingTypes.count(dependency))
                    self(self, dependency);
                else if (const auto module = workspace.frontend.moduleResolver.getModule(dependency))
                    owner->dependencies.push_back(module);
        };
        collect(collect, checked.first);
        workspace.frontend.moduleResolver.setModule(checked.first, Luau::ModulePtr(owner, owner->module.get()));
    }
    pendingTypes.clear();
    retainedTypes.erase(std::remove_if(retainedTypes.begin(), retainedTypes.end(),
                            [](const auto& generation)
                            {
                                return generation.expired();
                            }),
        retainedTypes.end());
}
std::string SchemaService::staleHint(const Luau::ModuleName& moduleName, const Luau::AstExpr* expression, Luau::TypeId type) const
{
    constexpr auto tag = "\n\n[stale]";
    for (auto generation = retainedTypes.rbegin(); generation != retainedTypes.rend(); ++generation)
        if (const auto owner = generation->lock())
            if (const auto metadata = owner->findMetadata(type); metadata && metadata->names.empty() && !metadata->script.empty() &&
                                                                 !metadata->project->fresh &&
                                                                 !metadata->project->capturedHandles.count(metadata->script) &&
                                                                 !handleSchemaFields(*metadata->project, metadata->script).empty())
                return tag;
    if (!expression)
        return {};
    const auto source = workspace.frontend.getSourceModule(moduleName);
    const auto script = normalizeContextPath(workspace.fileResolver.getUri(moduleName).fsPath());
    const auto selection = scriptProjects.find(script);
    if (!source || selection == scriptProjects.end())
        return {};
    const auto project = projects.at(selection->second);
    if (project->fresh || project->capturedHandles.count(script))
        return {};

    SchemaSyntax syntax(source->root);
    const auto resolved = syntax.value(expression);
    std::string name;
    const Luau::AstExpr* receiver = nullptr;
    if (const auto dot = resolved->as<Luau::AstExprIndexName>())
    {
        receiver = dot->expr;
        name = dot->index.value;
    }
    else if (const auto bracket = resolved->as<Luau::AstExprIndexExpr>())
    {
        receiver = bracket->expr;
        if (const auto literal = bracket->index->as<Luau::AstExprConstantString>())
            name.assign(literal->value.data, literal->value.size);
    }
    if (!receiver || syntax.symbol(receiver) != "handles")
        return {};
    for (const auto& observation : handleSchemaFields(*project, script))
        if (observation.field.name == name)
            return tag;
    return {};
}

std::string SchemaService::debug(const Luau::ModuleName& moduleName, const Luau::AstExpr* expression, Luau::TypeId type, std::string_view field)
{
    if (!workspace.client->getConfiguration(workspace.rootUri).reporting.uiEnabled("context.schemas"))
        return {};
    for (auto generation = retainedTypes.rbegin(); generation != retainedTypes.rend(); ++generation)
        if (const auto owner = generation->lock())
            if (const auto metadata = owner->findMetadata(type))
                return renderSchemaDebug(*metadata);
    const auto source = workspace.frontend.getSourceModule(moduleName);
    if (!source || !expression)
        return {};
    const auto path = normalizeContextPath(workspace.fileResolver.getUri(moduleName).fsPath());
    const auto selected = scriptProjects.find(path);
    SchemaSyntax syntax(source->root);
    const auto symbol = syntax.symbol(expression).value_or("");
    const auto initializer = syntax.value(expression)->as<Luau::AstExprCall>();
    const bool eventLookup = symbol == "game.EventTable" || (initializer && syntax.symbol(initializer->func) == "game.EventTable");
    const bool handleLookup = symbol == "handles" || symbol.find("handles.") == 0;
    if (selected == scriptProjects.end())
    {
        if (!eventLookup && !handleLookup)
            return {};
        return "\n\n___\n\n**Debug information**\n\nResolution: N/A  \nProject context: " +
               std::string(configuration.context.enabled ? "no unambiguous project association" : "not enabled");
    }
    const auto module = workspace.getModule(moduleName);
    // Scalar reads no longer carry the column's external type. Recover provenance
    // from the receiver, rather than guessing from unrelated primitive types.
    const auto resolved = syntax.value(expression);
    const Luau::AstExpr* receiver = nullptr;
    if (const auto member = resolved->as<Luau::AstExprIndexName>())
        receiver = member->expr;
    else if (const auto member = resolved->as<Luau::AstExprIndexExpr>())
        receiver = member->expr;
    if (receiver && module)
        if (const auto receiverType = module->astTypes.find(receiver))
            if (const auto metadata = SchemaTypes::metadata(*receiverType))
                return renderSchemaDebug(*metadata);
    if (handleLookup)
    {
        SchemaTypeMetadata metadata;
        metadata.project = projects.at(selected->second);
        metadata.script = path;
        metadata.field = field.empty() && symbol.size() > 8 ? symbol.substr(8) : std::string(field);
        metadata.resolution = metadata.project->capturedHandles.count(path) ? "project capture" :
                              metadata.project->fresh ? symbol : "attachment context is stale; handle names may have changed";
        return renderSchemaDebug(metadata);
    }
    const Luau::AstExpr* annotated = expression;
    if (receiver)
        annotated = receiver;
    if (const auto local = annotated->as<Luau::AstExprLocal>(); local && local->local->annotation)
        if (const auto reference = local->local->annotation->as<Luau::AstTypeReference>())
        {
            const auto document = workspace.fileResolver.getTextDocument(workspace.fileResolver.getUri(moduleName));
            if (document)
            {
                SchemaTypeImports imports(*source, *document, projects.at(selected->second)->directory, selected->second);
                if (const auto feature = imports.feature(*reference))
                {
                    SchemaTypeMetadata metadata;
                    metadata.project = projects.at(selected->second);
                    metadata.resolution = "imported project type";
                    if (feature->family == "handle")
                        metadata.script = path;
                    else if (reference->parameters.size && reference->parameters.data[0].type)
                        if (const auto literal = reference->parameters.data[0].type->as<Luau::AstTypeSingletonString>())
                            metadata.names.emplace(literal->value.data, literal->value.size);
                    if (!metadata.script.empty() || !metadata.names.empty())
                        return renderSchemaDebug(metadata);
                }
            }
        }
    if (eventLookup)
    {
        SchemaTypeMetadata metadata;
        metadata.project = projects.at(selected->second);
        metadata.resolution =
            initializer ? "no specialized schema; table name is unknown or has been widened to string" : "project event-table index";
        if (!initializer)
            metadata.names = eventSchemaNames(*metadata.project);
        return renderSchemaDebug(metadata);
    }
    return {};
}
nlohmann::json SchemaService::inspect(const std::string& script)
{
    const auto project = synchronize(workspace.fileResolver.getModuleName(Uri::file(script)));
    if (!project)
        return nullptr;
    auto result = nlohmann::json(*project);
    result["captures"] = project->captureDetails;
    return result;
}
void SchemaService::forgetOverlay(const std::string& closedScript)
{
    for (const auto& project : projects)
    {
        bool active = false;
        for (const auto& entry : workspace.fileResolver.managedFiles)
        {
            const auto path = normalizeContextPath(entry.first.fsPath());
            if (path == normalizeContextPath(closedScript) || !project.second->scripts.count(path))
                continue;
            active = active || importsProjectTypes(extractScriptSchema(path, entry.second.getText()), project.second->exportPath);
        }
        writeSchemaExportReference(stateDirectory(configuration), workspacePath, project.first, active);
    }
}

lsp::WorkspaceEdit SchemaService::removalEdits(
    const std::string& script, bool wholeProject, const std::set<std::string>& families, std::vector<std::string>& warnings)
{
    lsp::WorkspaceEdit result;
    const auto project = synchronize(workspace.fileResolver.getModuleName(Uri::file(script)));
    if (!project)
    {
        warnings.push_back("No unambiguous project context is available.");
        return result;
    }
    result.documentChanges = std::vector<lsp::TextDocumentEdit>{};
    for (const auto& file : project->scripts)
    {
        if (!wholeProject && normalizeContextPath(script) != file.first)
            continue;
        if (!importsProjectTypes(file.second, project->exportPath))
            continue;
        const auto uri = Uri::file(file.first);
        const auto managed = workspace.fileResolver.getTextDocument(uri);
        const auto text = managed ? std::optional<std::string>(managed->getText()) : Luau::FileUtils::readFile(file.first);
        if (!text)
        {
            warnings.push_back("Could not read " + file.first);
            continue;
        }
        TextDocument document(uri, "luau", managed ? managed->version() : 0, *text);
        Luau::SourceModule source;
        auto parsed = Luau::Parser::parse(text->data(), text->size(), *source.names, *source.allocator);
        if (!parsed.root || !parsed.errors.empty())
        {
            warnings.push_back("Cannot safely edit a script with syntax errors: " + file.first);
            continue;
        }
        source.root = parsed.root;
        const SchemaTypeImports imports(source, document, project->directory, project->projectId);
        auto edits = imports.remove(families, warnings);
        if (edits.empty())
            continue;
        result.documentChanges->push_back({{{uri}, managed ? std::optional<size_t>(managed->version()) : std::nullopt}, std::move(edits)});
    }
    return result;
}

void SchemaService::complete(const TextDocument& document, const Luau::SourceModule& source, Luau::Position position,
    const std::vector<Luau::AstNode*>& ancestry, std::vector<lsp::CompletionItem>& items)
{
    const auto script = normalizeContextPath(workspace.fileResolver.getUri(source.name).fsPath());
    const auto selected = scriptProjects.find(script);
    if (selected == scriptProjects.end())
        return;
    const auto project = projects.at(selected->second);
    SchemaSyntax syntax(source.root);
    ContextProvider provider(stateDirectory(configuration), workspacePath);
    const auto candidates = provider.findForScript(script);
    const auto* snapshot = candidates.size() == 1 && candidates.front().project.id == project->projectId && candidates.front().snapshot
                               ? &*candidates.front().snapshot
                               : nullptr;
    const auto receiverNodes = [&](const auto& resolve, const Luau::AstExpr* expression) -> std::set<uint64_t>
    {
        expression = syntax.value(expression);
        if (!expression || !snapshot)
            return {};
        if (const auto symbol = syntax.symbol(expression))
        {
            if (*symbol == "game.World")
                return {0};
            if (*symbol == "self")
            {
                std::set<uint64_t> result;
                if (const auto attachments = project->attachments.find(script); attachments != project->attachments.end())
                    for (const auto& attachment : attachments->second)
                        result.insert(attachment.nodeId);
                return result;
            }
        }
        const Luau::AstExpr* parent = nullptr;
        std::string_view name;
        if (const auto call = expression->as<Luau::AstExprCall>())
        {
            const auto method = call->func->as<Luau::AstExprIndexName>();
            if (!method || (std::string_view(method->index.value) != "FindFirstChild" && std::string_view(method->index.value) != "WaitForChild"))
                return {};
            const size_t index = call->self || syntax.symbol(method->expr) == "game.World" ? 0 : 1;
            if (call->args.size <= index)
                return {};
            if (std::string_view(method->index.value) == "FindFirstChild" && call->args.size > index + 1)
            {
                const auto recursive = syntax.value(call->args.data[index + 1]);
                const auto literal = recursive ? recursive->as<Luau::AstExprConstantBool>() : nullptr;
                if (!literal || literal->value)
                    return {};
            }
            const auto key = syntax.value(call->args.data[index]);
            const auto literal = key ? key->as<Luau::AstExprConstantString>() : nullptr;
            if (!literal)
                return {};
            parent = index == 0 ? method->expr : call->args.data[0];
            name = {literal->value.data, literal->value.size};
        }
        if (const auto dot = expression->as<Luau::AstExprIndexName>())
        {
            parent = dot->expr;
            name = dot->index.value;
        }
        else if (const auto bracket = expression->as<Luau::AstExprIndexExpr>())
        {
            const auto key = syntax.value(bracket->index);
            const auto literal = key ? key->as<Luau::AstExprConstantString>() : nullptr;
            if (!literal)
                return {};
            parent = bracket->expr;
            name = {literal->value.data, literal->value.size};
        }
        if (!parent)
            return {};
        const auto parents = resolve(resolve, parent);
        std::set<uint64_t> result;
        for (const auto& instance : snapshot->instances)
            if (parents.count(instance.parentNodeId) && instance.name == name)
                result.insert(instance.nodeId);
        return result;
    };
    std::map<std::string, const LuduvoResourceName*> staticResources;
    const bool showDeprecated = workspace.client->getConfiguration(workspace.rootUri).completion.showDeprecatedItems;
    std::map<std::string, std::string> resourceOrigins;
    const auto resourceDebug = [&](const std::string& name, const std::string& resolution)
    {
        const auto origin = resourceOrigins.find(name);
        if (origin == resourceOrigins.end() || origin->second.empty() ||
            !workspace.client->getConfiguration(workspace.rootUri).reporting.uiEnabled("context.schemas"))
            return std::string{};
        SchemaTypeMetadata metadata;
        metadata.project = project;
        metadata.resolution = resolution;
        if (origin->second == "event-table")
            metadata.names.insert(name);
        else if (origin->second == "script")
            metadata.script = normalizeContextPath((std::filesystem::u8path(project->directory) / name).generic_u8string());
        else
        {
            metadata.script = script;
            metadata.field = name;
        }
        return renderSchemaDebug(metadata);
    };
    const auto markStale = [&](lsp::CompletionItem& item, const std::string& name)
    {
        if (!project->fresh && !project->capturedHandles.count(script) && resourceOrigins[name] == "handle")
        {
            const auto detail = item.detail.value_or("Handle");
            if (detail.find("[stale]") == std::string::npos)
                item.detail = detail + " [stale]";
        }
    };
    const auto addNames = [&](const std::vector<std::string>& names, const Luau::Location& location, const std::string& detail)
    {
        for (const auto& name : names)
        {
            const auto configured = staticResources.find(name);
            if (configured != staticResources.end() && configured->second->deprecated && !showDeprecated)
                continue;
            const auto markBuiltin = [&](lsp::CompletionItem& item)
            {
                if (resourceOrigins[name] != "event-table")
                    return false;
                const bool builtin = std::any_of(project->scripts.begin(), project->scripts.end(),
                    [&](const auto& script)
                    {
                        return script.second.builtin && std::any_of(script.second.events.begin(), script.second.events.end(),
                                                            [&](const auto& event)
                                                            {
                                                                return event.name == name;
                                                            });
                    });
                if (!builtin)
                    return false;
                item.detail = "Luduvo event table (built-in)";
                return true;
            };
            const auto existing = std::find_if(items.begin(), items.end(),
                [&](const auto& item)
                {
                    return item.label == name;
                });
            if (existing != items.end())
            {
                markStale(*existing, name);
                const bool builtin = markBuiltin(*existing);
                if (configured != staticResources.end() || (builtin && !existing->documentation))
                {
                    LuduvoResourceName resource =
                        configured == staticResources.end() ? LuduvoResourceName{name, std::nullopt, builtin} : *configured->second;
                    resource.builtIn = resource.builtIn || builtin;
                    hover::applyResourceCompletionDocumentation(configuration.hover, *existing,
                        resourceOrigins[name].empty() ? detail : resourceOrigins[name], resource);
                }
                const auto debug = resourceDebug(name, detail);
                if (!debug.empty() && (!existing->documentation || existing->documentation->value.find("**Debug information**") == std::string::npos))
                {
                    if (!existing->documentation)
                        existing->documentation = lsp::MarkupContent{lsp::MarkupKind::Markdown, ""};
                    existing->documentation->value += debug;
                }
                continue;
            }
            lsp::CompletionItem item;
            item.label = name;
            item.kind = lsp::CompletionItemKind::Value;
            item.detail = detail;
            markStale(item, name);
            const bool builtin = markBuiltin(item);
            item.textEdit = lsp::TextEdit{document.convertLocation(location), quoteSchemaString(name)};
            LuduvoResourceName resource = configured == staticResources.end() ? LuduvoResourceName{name, std::nullopt, builtin} : *configured->second;
            resource.builtIn = resource.builtIn || builtin;
            std::string columns;
            if (resourceOrigins[name] == "event-table")
            {
                std::map<std::string, std::set<std::string>> fields;
                for (const auto& observation : eventSchemaFields(*project, {name}))
                    fields[observation.field.name].insert(observation.field.type);
                if (!fields.empty())
                    columns += "**Columns**\n";
                for (const auto& field : fields)
                {
                    columns += "\n- `" + field.first + ": ";
                    bool first = true;
                    for (const auto& type : field.second)
                    {
                        if (!first)
                            columns += " | ";
                        columns += type;
                        first = false;
                    }
                    columns += "`";
                }
            }
            if (!columns.empty())
                resource.documentation = resource.documentation ? *resource.documentation + "\n\n" + columns : std::move(columns);
            hover::applyResourceCompletionDocumentation(configuration.hover, item,
                resourceOrigins[name].empty() ? detail : resourceOrigins[name], resource);
            if (const auto debug = resourceDebug(name, detail); !debug.empty())
            {
                if (!item.documentation)
                    item.documentation = lsp::MarkupContent{lsp::MarkupKind::Markdown, ""};
                item.documentation->value += debug;
            }
            items.push_back(std::move(item));
        }
    };
    SchemaTypeImports imports(source, document, project->directory, project->projectId);
    struct TypeAtPosition : Luau::AstVisitor
    {
        Luau::Position position;
        Luau::AstTypeReference* reference = nullptr;
        explicit TypeAtPosition(Luau::Position position)
            : position(position)
        {
        }
        bool visit(Luau::AstType* type) override
        {
            return type->location.containsClosed(position);
        }
        bool visit(Luau::AstTypeReference* type) override
        {
            if (!type->location.containsClosed(position))
                return false;
            reference = type;
            return true;
        }
    } typeAt(position);
    source.root->visit(&typeAt);
    const auto resourceNames = [&](const std::string& kind, bool includeStatic, const Luau::AstExpr* receiver = nullptr)
    {
        std::set<std::string> result;
        if (std::find(configuration.resources.completion.disabledKinds.begin(), configuration.resources.completion.disabledKinds.end(), kind) !=
            configuration.resources.completion.disabledKinds.end())
            return result;
        const auto group = resources.group(kind);
        if (!group)
            return result;
        if (includeStatic)
            for (const auto& value : group->resources)
            {
                result.insert(value.name);
                staticResources[value.name] = &value;
            }
        for (const auto& provider : group->projectSources)
        {
            if (snapshot && (provider == "children" || provider == "descendants" || provider == "attribute" || provider == "tag"))
            {
                auto nodes = receiverNodes(receiverNodes, receiver);
                if (provider == "descendants")
                {
                    std::vector<uint64_t> pending(nodes.begin(), nodes.end());
                    for (size_t i = 0; i < pending.size(); ++i)
                        for (const auto& instance : snapshot->instances)
                            if (instance.parentNodeId == pending[i] && nodes.insert(instance.nodeId).second)
                            {
                                pending.push_back(instance.nodeId);
                                if (!instance.name.empty())
                                    result.insert(instance.name);
                            }
                }
                else
                    for (const auto& instance : snapshot->instances)
                    {
                        if (provider == "children" && nodes.count(instance.parentNodeId) && !instance.name.empty())
                            result.insert(instance.name);
                        else if (provider == "attribute" && nodes.count(instance.nodeId))
                        {
                            const auto attributes = instance.scene.find("attributes");
                            if (attributes != instance.scene.end() && attributes->is_object())
                                for (const auto& entry : attributes->items())
                                    result.insert(entry.key());
                        }
                        else if (provider == "tag")
                        {
                            const auto tags = instance.scene.find("tags");
                            if (tags != instance.scene.end() && tags->is_array())
                                for (const auto& tag : *tags)
                                    if (tag.is_string())
                                        result.insert(tag.get<std::string>());
                        }
                    }
            }
            if (provider == "event-table")
            {
                const auto names = eventSchemaNames(*project);
                result.insert(names.begin(), names.end());
                for (const auto& name : names)
                    resourceOrigins[name] = provider;
            }
            else if (provider == "handle")
                for (const auto& field : handleSchemaFields(*project, script))
                {
                    result.insert(field.field.name);
                    resourceOrigins[field.field.name] = provider;
                }
            else if (provider == "script")
                for (const auto& file : project->scripts)
                {
                    const auto name =
                        std::filesystem::u8path(file.first).lexically_relative(std::filesystem::u8path(project->directory)).generic_u8string();
                    result.insert(name);
                    resourceOrigins[name] = provider;
                }
        }
        return result;
    };
    if (typeAt.reference)
    {
        if (typeAt.reference->nameLocation.containsClosed(position) && !typeAt.reference->prefix && imports.canImport())
        {
            const auto& capabilities = workspace.client->capabilities;
            const bool snippets = capabilities.textDocument && capabilities.textDocument->completion &&
                                  capabilities.textDocument->completion->completionItem &&
                                  capabilities.textDocument->completion->completionItem->snippetSupport;
            for (const auto& feature : exportedSchemaFeatures())
            {
                auto item = imports.completion(feature, script, snippets);
                items.erase(std::remove_if(items.begin(), items.end(),
                                [&](const auto& existing)
                                {
                                    return existing.label == feature.helper || existing.label == item.label;
                                }),
                    items.end());
                // Existing type arguments belong to the user. Replace only the name.
                const auto insertion = typeAt.reference->hasParameterList ? item.label : item.insertText.value_or(item.label);
                item.textEdit = lsp::TextEdit{document.convertLocation(typeAt.reference->nameLocation), insertion};
                if (typeAt.reference->hasParameterList)
                    item.insertTextFormat = lsp::InsertTextFormat::PlainText;
                items.push_back(std::move(item));
            }
            return;
        }
        if (typeAt.reference->parameters.size && configuration.resources.completion.enabled)
        {
            const auto feature = imports.feature(*typeAt.reference);
            const auto symbol = feature
                                    ? feature->helper
                                    : (typeAt.reference->prefix ? std::string(typeAt.reference->prefix->value) + "." + typeAt.reference->name.value
                                                                : typeAt.reference->name.value);
            for (size_t index = 0; index < typeAt.reference->parameters.size; ++index)
            {
                const auto parameter = typeAt.reference->parameters.data[index].type;
                if (!parameter || !parameter->location.containsClosed(position))
                    continue;
                for (const auto& kind : bindings.resolveResourceTargets(symbol, LuduvoBindingTargetKind::TypeArgument, index))
                {
                    const auto names = resourceNames(kind, true);
                    addNames({names.begin(), names.end()}, parameter->location, kind == "event-table" ? "Event table" : kind);
                }
            }
            return;
        }
    }
    if (!configuration.resources.completion.enabled)
        return;
    const auto module = workspace.getModule(source.name);
    for (auto it = ancestry.rbegin(); it != ancestry.rend(); ++it)
    {
        if (const auto call = (*it)->as<Luau::AstExprCall>(); call && module)
        {
            const auto type = module->astTypes.find(call->func);
            if (!type)
                continue;
            auto symbols = resolveCallSymbols(source, module.get(), *call);
            if (const auto symbol = syntax.symbol(call->func); symbol && std::find(symbols.begin(), symbols.end(), *symbol) == symbols.end())
                symbols.push_back(*symbol);
            const bool implicitSelf = call->self || findCallMetamethod(*type).has_value();
            for (const auto& symbol : symbols)
            {
                if (std::find(configuration.resources.completion.disabledApis.begin(), configuration.resources.completion.disabledApis.end(),
                        symbol) != configuration.resources.completion.disabledApis.end())
                    continue;
                for (const auto& binding : bindings.resolveResources(symbol, *type))
                {
                    const auto index = binding.typeParameterIndex - (implicitSelf && binding.typeParameterIndex ? 1 : 0);
                    const auto active = activeArgument(*call, position);
                    if (binding.variadic ? active < index : active != index)
                        continue;
                    const auto range = resourceCompletionRange(document, *call, active, position);
                    if (!range)
                        continue;
                    const auto member = call->func->as<Luau::AstExprIndexName>();
                    const auto receiver = call->self ? (member ? member->expr : nullptr) :
                                          symbol.compare(0, 9, "Instance.") == 0 && call->args.size ? call->args.data[0] :
                                          symbol.compare(0, 6, "World.") == 0 ? (member ? member->expr : nullptr) : nullptr;
                    const auto names = resourceNames(binding.kind, false, receiver);
                    const auto first = items.size();
                    addNames({names.begin(), names.end()}, Luau::Location(position, position),
                        binding.kind == "event-table" ? "Event table" : binding.kind);
                    for (size_t i = first; i < items.size(); ++i)
                        items[i].textEdit->range = *range;
                }
            }
            return;
        }
        const Luau::AstExpr* receiver = nullptr;
        Luau::Location location;
        bool bracket = false;
        if (const auto index = (*it)->as<Luau::AstExprIndexExpr>();
            index && index->index->is<Luau::AstExprConstantString>() && index->index->location.containsClosed(position))
        {
            receiver = index->expr;
            location = index->index->location;
            bracket = true;
        }
        if (const auto index = (*it)->as<Luau::AstExprIndexName>(); index && index->op == '.' && index->indexLocation.containsClosed(position))
        {
            receiver = index->expr;
            location = index->indexLocation;
        }
        if (!receiver)
            continue;
        auto symbol = syntax.symbol(receiver);
        const auto resolvedReceiver = syntax.value(receiver);
        const auto member = resolvedReceiver ? resolvedReceiver->as<Luau::AstExprIndexName>() : nullptr;
        if (!symbol || bindings.resolveResourceTargets(*symbol, LuduvoBindingTargetKind::Member).empty())
        {
            if (member && std::string_view(member->index.value) == "attr" && !receiverNodes(receiverNodes, member->expr).empty())
            {
                symbol = "Instance.attr";
                receiver = member->expr;
            }
            else if (!receiverNodes(receiverNodes, receiver).empty())
                symbol = symbol == "game.World" ? "World" : "Instance";
        }
        if (!symbol)
            continue;
        if (std::find(configuration.resources.completion.disabledApis.begin(), configuration.resources.completion.disabledApis.end(), *symbol) !=
            configuration.resources.completion.disabledApis.end())
            continue;
        for (const auto& kind : bindings.resolveResourceTargets(*symbol, LuduvoBindingTargetKind::Member))
        {
            const auto names = resourceNames(kind, true, receiver);
            if (bracket)
                addNames({names.begin(), names.end()}, location, kind == "handle" ? "Handle" : kind);
            else
                for (const auto& name : names)
                {
                    const auto existing = std::find_if(items.begin(), items.end(),
                        [&](const auto& item)
                        {
                            return item.label == name;
                        });
                    if (existing != items.end())
                    {
                        markStale(*existing, name);
                        if (const auto debug = resourceDebug(name, *symbol + "." + name); !debug.empty())
                        {
                            if (!existing->documentation)
                                existing->documentation = lsp::MarkupContent{lsp::MarkupKind::Markdown, ""};
                            existing->documentation->value += debug;
                        }
                        continue;
                    }
                    lsp::CompletionItem item;
                    item.label = name;
                    item.kind = lsp::CompletionItemKind::Field;
                    item.detail = kind;
                    markStale(item, name);
                    const auto configured = staticResources.find(name);
                    if (configured != staticResources.end() && configured->second->deprecated && !showDeprecated)
                        continue;
                    hover::applyResourceCompletionDocumentation(configuration.hover, item, kind,
                        configured == staticResources.end() ? LuduvoResourceName{name, std::nullopt, false} : *configured->second);
                    if (const auto debug = resourceDebug(name, *symbol + "." + name); !debug.empty())
                    {
                        if (!item.documentation)
                            item.documentation = lsp::MarkupContent{lsp::MarkupKind::Markdown, ""};
                        item.documentation->value += debug;
                    }
                    item.textEdit = lsp::TextEdit{document.convertLocation(location), name};
                    if (!Luau::isIdentifier(name) || Luau::Lexer::isReserved(name))
                    {
                        auto range = document.convertLocation(location);
                        if (range.start.character)
                            --range.start.character;
                        item.textEdit = lsp::TextEdit{range, "[" + quoteSchemaString(name) + "]"};
                    }
                    items.push_back(std::move(item));
                }
        }
        return;
    }
}
} // namespace luduvo
