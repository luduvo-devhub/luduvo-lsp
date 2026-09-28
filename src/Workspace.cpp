#include "LSP/Workspace.hpp"

#include <algorithm>
#include <memory>

#include "LSP/Diagnostics.hpp"
#include "LSP/DocumentationParser.hpp"
#include "Platform/LSPPlatform.hpp"
#include "Platform/RobloxPlatform.hpp"
#include "Plugin/PluginManager.hpp"
#include "Plugin/PluginDefinitions.hpp"
#include "glob/match.h"
#include "Luau/BuiltinDefinitions.h"
#include "Luau/Clone.h"
#include "Luau/NotNull.h"
#include "Luau/Parser.h"
#include "Luau/TimeTrace.h"
#include "Luau/Type.h"
#include "LuauFileUtils.hpp"

LUAU_FASTFLAG(LuauSolverV2)

static void filterIgnoredLints(Luau::CheckResult& result, const LSPPlatform& platform)
{
    const auto ignored = [&platform](const Luau::LintWarning& lint)
    {
        return platform.isLintIgnored(lint);
    };
    result.lintResult.errors.erase(
        std::remove_if(result.lintResult.errors.begin(), result.lintResult.errors.end(), ignored), result.lintResult.errors.end());
    result.lintResult.warnings.erase(
        std::remove_if(result.lintResult.warnings.begin(), result.lintResult.warnings.end(), ignored), result.lintResult.warnings.end());
}

void throwIfCancelled(const LSPCancellationToken& cancellationToken)
{
    if (cancellationToken && cancellationToken->requested())
        throw RequestCancelledException();
}

const Luau::ModulePtr WorkspaceFolder::getModule(const Luau::ModuleName& moduleName, bool forAutocomplete) const
{
    if (FFlag::LuauSolverV2 || !forAutocomplete)
        return frontend.moduleResolver.getModule(moduleName);
    else
        return frontend.moduleResolverForAutocomplete.getModule(moduleName);
}

void WorkspaceFolder::openTextDocument(const lsp::DocumentUri& uri, const lsp::DidOpenTextDocumentParams& params)
{
    LUAU_ASSERT(isReady);
    fileResolver.managedFiles.emplace(
        std::make_pair(uri, TextDocument(uri, params.textDocument.languageId, params.textDocument.version, params.textDocument.text)));

    // Mark the file as dirty as we don't know what changes were made to it
    auto moduleName = fileResolver.getModuleName(uri);
    frontend.markDirty(moduleName);
}

static bool isWorkspaceDiagnosticsEnabled(const Client* client, const ClientConfiguration& config)
{
    return client->getWorkspaceDiagnosticsToken() && config.diagnostics.workspace;
}

void WorkspaceFolder::updateTextDocument(const lsp::DocumentUri& uri, const lsp::DidChangeTextDocumentParams& params)
{
    LUAU_ASSERT(isReady);

    if (fileResolver.managedFiles.find(uri) == fileResolver.managedFiles.end())
    {
        client->sendLogMessage(lsp::MessageType::Error, "Text Document not loaded locally: " + uri.toString());
        return;
    }
    auto& textDocument = fileResolver.managedFiles.at(uri);
    textDocument.update(params.contentChanges, params.textDocument.version);

    // Invalidate plugin cache for this document - forces re-transformation on next access
    fileResolver.invalidatePluginDocument(uri);

    // Keep a vector of reverse dependencies marked dirty to extend diagnostics for them
    std::vector<Luau::ModuleName> markedDirty{};

    // Mark the module dirty for the typechecker
    frontend.markDirty(fileResolver.getModuleName(uri), &markedDirty);

    // In pull based diagnostics module, documentDiagnostics will update the necessary files
    // But if we are still using push-based diagnostics, we need to send updates
    auto config = client->getConfiguration(rootUri);
    if (!usingPullDiagnostics(client->capabilities))
    {
        // Convert the diagnostics report into a series of diagnostics published for each relevant file
        auto diagnostics = documentDiagnostics(lsp::DocumentDiagnosticParams{{uri}}, /* cancellationToken= */ nullptr);
        client->publishDiagnostics(lsp::PublishDiagnosticsParams{uri, params.textDocument.version, diagnostics.items});

        // Compute diagnostics for reverse dependencies
        // TODO: should we put this inside documentDiagnostics so it works in the pull based model as well? (its a reverse BFS which is expensive)
        if (config.diagnostics.includeDependents)
        {
            for (auto& moduleName : markedDirty)
            {
                auto dirtyUri = fileResolver.getUri(moduleName);
                if (dirtyUri != uri && diagnostics.relatedDocuments.find(dirtyUri) == diagnostics.relatedDocuments.end() &&
                    !isIgnoredFile(dirtyUri, config))
                {
                    auto dependencyDiags = documentDiagnostics(
                        lsp::DocumentDiagnosticParams{{dirtyUri}}, /* cancellationToken=*/nullptr, /* allowUnmanagedFiles= */ true);
                    client->publishDiagnostics(lsp::PublishDiagnosticsParams{dirtyUri, std::nullopt, dependencyDiags.items});
                }
            }
        }
    }
}

void WorkspaceFolder::onDidSaveTextDocument(const lsp::DocumentUri& uri, const lsp::DidSaveTextDocumentParams& params)
{
    LUAU_ASSERT(isReady);

    auto config = client->getConfiguration(rootUri);
    if (isWorkspaceDiagnosticsEnabled(client, config))
    {
        Luau::DenseHashSet<Luau::ModuleName> dependents{};
        frontend.traverseDependents(fileResolver.getModuleName(uri),
            [&dependents](Luau::SourceNode& sourceNode)
            {
                if (dependents.contains(sourceNode.name))
                    return false;

                dependents.insert(sourceNode.name);
                return true;
            });

        lsp::WorkspaceDiagnosticReportPartialResult report;

        // Convert the diagnostics report into a series of diagnostics published for each relevant file
        auto diagnostics = documentDiagnostics(lsp::DocumentDiagnosticParams{{uri}}, /* cancellationToken= */ nullptr);

        lsp::WorkspaceDocumentDiagnosticReport mainDocumentReport;
        mainDocumentReport.uri = uri;
        mainDocumentReport.kind = diagnostics.kind;
        mainDocumentReport.items = diagnostics.items;
        mainDocumentReport.items = diagnostics.items;
        report.items.emplace_back(mainDocumentReport);

        for (auto& moduleName : dependents)
        {
            auto dirtyUri = fileResolver.getUri(moduleName);
            if (dirtyUri != uri && !isIgnoredFile(dirtyUri, config))
            {
                auto dependencyDiags =
                    documentDiagnostics(lsp::DocumentDiagnosticParams{{dirtyUri}}, /* cancellationToken= */ nullptr, /* allowUnmanagedFiles= */ true);

                lsp::WorkspaceDocumentDiagnosticReport documentReport;
                documentReport.uri = dirtyUri;
                documentReport.kind = dependencyDiags.kind;
                documentReport.items = dependencyDiags.items;
                documentReport.items = dependencyDiags.items;
                report.items.emplace_back(documentReport);
            }
        }

        client->sendProgress({*client->getWorkspaceDiagnosticsToken(), report});
    }
}

void WorkspaceFolder::closeTextDocument(const lsp::DocumentUri& uri)
{
    fileResolver.managedFiles.erase(uri);

    // Mark the module as dirty as we no longer track its changes
    auto config = client->getConfiguration(rootUri);
    auto moduleName = fileResolver.getModuleName(uri);
    frontend.markDirty(moduleName);

    // Refresh workspace diagnostics to clear diagnostics on ignored files
    if (!config.diagnostics.workspace || isIgnoredFile(uri))
        clearDiagnosticsForFile(uri);
}

void WorkspaceFolder::clearDiagnosticsForFiles(const std::vector<lsp::DocumentUri>& uris) const
{
    if (!client->capabilities.textDocument || !client->capabilities.textDocument->diagnostic)
    {
        for (const auto& uri : uris)
            client->publishDiagnostics(lsp::PublishDiagnosticsParams{uri, std::nullopt, {}});
    }
    else if (client->getWorkspaceDiagnosticsToken())
    {
        std::vector<lsp::WorkspaceDocumentDiagnosticReport> reports;
        reports.reserve(uris.size());
        for (const auto& uri : uris)
        {
            lsp::WorkspaceDocumentDiagnosticReport report;
            report.uri = uri;
            report.kind = lsp::DocumentDiagnosticReportKind::Full;
            reports.push_back(report);
        }
        lsp::WorkspaceDiagnosticReportPartialResult report{reports};
        client->sendProgress({client->getWorkspaceDiagnosticsToken().value(), report});
    }
    else
    {
        client->refreshWorkspaceDiagnostics();
    }
}

void WorkspaceFolder::clearDiagnosticsForFile(const lsp::DocumentUri& uri)
{
    clearDiagnosticsForFiles({uri});
}

static const char* kWatchedFilesProgressToken = "luau/onDidChangeWatchedFiles";

void WorkspaceFolder::onDidChangeWatchedFiles(const std::vector<lsp::FileEvent>& changes)
{
    client->sendTrace("workspace: processing " + std::to_string(changes.size()) + " watched files changes");

    client->createWorkDoneProgress(kWatchedFilesProgressToken);
    client->sendWorkDoneProgressBegin(kWatchedFilesProgressToken, "Luau: Processing " + std::to_string(changes.size()) + " file changes");

    auto config = client->getConfiguration(rootUri);

    std::vector<Luau::ModuleName> dirtyFiles;
    std::vector<Luau::ModuleName> deletedModules;
    std::vector<Uri> deletedFiles;
    bool pluginFileChanged = false;

    for (const auto& change : changes)
    {
        platform->onDidChangeWatchedFiles(change);

        if (change.uri.filename() == ".luaurc" || change.uri.filename() == ".robloxrc" || change.uri.filename() == ".config.luau")
        {
            client->sendLogMessage(lsp::MessageType::Info, "Acknowledge config changed for workspace " + name + ", clearing configuration cache");
            fileResolver.clearConfigCache();

            // Recompute diagnostics
            recomputeDiagnostics(config);
        }
        else if (change.uri.extension() == ".lua" || change.uri.extension() == ".luau")
        {
            // Notify if it was a definitions file
            if (isDefinitionFile(change.uri, config))
            {
                client->sendWindowMessage(
                    lsp::MessageType::Info, "Detected changes to global definitions files. Please reload your workspace for this to take effect");
                continue;
            }

            if (!pluginFileChanged && isPluginFile(change.uri))
                pluginFileChanged = true;

            auto moduleName = fileResolver.getModuleName(change.uri);

            if (change.type == lsp::FileChangeType::Deleted)
            {
                // Fully erase the module from the frontend's caches (including `sourceNodes`, which
                // is what drives string require auto-import suggestions) so a renamed/deleted file
                // stops being suggested. This also marks any dependents dirty so they get rechecked.
                deletedModules.push_back(moduleName);
                deletedFiles.push_back(change.uri);
            }
            else
            {
                // Note: we should always mark as dirty, even if the file is ignored
                frontend.markDirty(moduleName, &dirtyFiles);
            }
        }
    }

    if (pluginFileChanged)
        reloadPlugins();

    if (!deletedModules.empty())
        frontend.clearModules(deletedModules);

    // Parse require graph for files if indexing enable
    if (config.index.enabled && appliedFirstTimeConfiguration)
        frontend.parseModules(dirtyFiles);

    // Clear the diagnostics for files in case it was not managed
    clearDiagnosticsForFiles(deletedFiles);

    client->sendWorkDoneProgressEnd(kWatchedFilesProgressToken);
}

/// Whether the file has been marked as ignored by any of the ignored lists in the configuration
bool WorkspaceFolder::isIgnoredFile(const Uri& uri, const std::optional<ClientConfiguration>& givenConfig) const
{
    // We want to test globs against a relative path to workspace, since that's what makes most sense
    auto relativePathString = uri.lexicallyRelative(rootUri);
    auto config = givenConfig ? *givenConfig : client->getConfiguration(rootUri);
    std::vector<std::string> patterns = config.ignoreGlobs; // TODO: extend further?
    for (auto& pattern : patterns)
    {
        if (glob::gitignore_glob_match(relativePathString, pattern))
        {
            return true;
        }
    }
    return false;
}

bool WorkspaceFolder::isIgnoredFileForAutoImports(const Uri& uri, const std::optional<ClientConfiguration>& givenConfig) const
{
    // We want to test globs against a relative path to workspace, since that's what makes most sense
    auto relativePathString = uri.lexicallyRelative(rootUri);
    auto config = givenConfig ? *givenConfig : client->getConfiguration(rootUri);
    std::vector<std::string> patterns = config.completion.imports.ignoreGlobs;
    for (auto& pattern : patterns)
    {
        if (glob::gitignore_glob_match(relativePathString, pattern))
        {
            return true;
        }
    }
    return false;
}

bool WorkspaceFolder::isDefinitionFile(const Uri& path, const std::optional<ClientConfiguration>& givenConfig) const
{
    auto config = givenConfig ? *givenConfig : client->getConfiguration(rootUri);

    for (auto& [_, file] : config.types.definitionFiles)
    {
        if (rootUri.resolvePath(resolvePath(file)) == path)
        {
            return true;
        }
    }

    return false;
}

bool WorkspaceFolder::isPluginFile(const Uri& uri) const
{
    return fileResolver.pluginManager && fileResolver.pluginManager->isPluginFile(uri);
}

void WorkspaceFolder::reloadPlugins()
{
    if (!fileResolver.pluginManager)
        return;

    client->sendLogMessage(lsp::MessageType::Info, "Plugin file changed, reloading plugins");

    fileResolver.pluginManager->reload();
    fileResolver.clearPluginDocuments();

    // Any source node could have plugin transformations applied (including non-managed files), so mark all dirty
    for (const auto& [name, _] : frontend.sourceNodes)
        frontend.markDirty(name);
}

// Runs `Frontend::check` on the module and DISCARDS THE TYPE GRAPH.
// Uses the diagnostic type checker, so strictness and DM awareness is not enforced
// NOTE: do NOT use this if you later retrieve a ModulePtr (via frontend.moduleResolver.getModule). Instead use `checkStrict`
// NOTE: use `frontend.parse` if you do not care about typechecking
Luau::CheckResult WorkspaceFolder::checkSimple(const Luau::ModuleName& moduleName, const LSPCancellationToken& cancellationToken)
{
    try
    {
        Luau::FrontendOptions options{/* retainFullTypeGraphs: */ false, /* forAutocomplete: */ false, /* runLintChecks: */ true};
        options.cancellationToken = cancellationToken;
        auto result = frontend.check(moduleName, options);
        filterIgnoredLints(result, *platform);
        return result;
    }
    catch (Luau::InternalCompilerError& err)
    {
        // TODO: RecursionLimitException is leaking out of frontend.check
        // https://github.com/Roblox/luau/issues/975
        // Remove this try-catch block once the above issue is fixed
        client->sendLogMessage(lsp::MessageType::Warning, "Luau InternalCompilerError caught in " + moduleName + ": " + err.what());
        return Luau::CheckResult{};
    }
}

// Runs `Frontend::check` on the module whilst retaining the type graph.
// Uses the autocomplete typechecker to enforce strictness and DM awareness.
// NOTE: a disadvantage of the autocomplete typechecker is that it has a timeout restriction that
// can often be hit
Luau::CheckResult WorkspaceFolder::checkStrict(
    const Luau::ModuleName& moduleName, const LSPCancellationToken& cancellationToken, bool forAutocomplete)
{
    if (FFlag::LuauSolverV2)
        forAutocomplete = false;

    // HACK: note that a previous call to `Frontend::check(moduleName, { retainTypeGraphs: false })`
    // and then a call `Frontend::check(moduleName, { retainTypeGraphs: true })` will NOT actually
    // retain the type graph if the module is not marked dirty.
    // We do a manual check and dirty marking to fix this
    auto module = getModule(moduleName, forAutocomplete);
    if (module && module->internalTypes->types.empty()) // If we didn't retain type graphs, then the internalTypes arena is empty
        frontend.markDirty(moduleName);

    Luau::FrontendOptions options{/* retainFullTypeGraphs: */ true, forAutocomplete, /* runLintChecks: */ true};
    options.cancellationToken = cancellationToken;
    auto result = frontend.check(moduleName, options);
    filterIgnoredLints(result, *platform);
    return result;
}

static const char* kIndexProgressToken = "luau/indexFiles";

void WorkspaceFolder::indexFiles(const ClientConfiguration& config)
{
    LUAU_TIMETRACE_SCOPE("WorkspaceFolder::indexFiles", "LSP");
    if (!config.index.enabled)
        return;

    if (isNullWorkspace())
        return;

    client->sendTrace("workspace: indexing all files");
    client->createWorkDoneProgress(kIndexProgressToken);
    client->sendWorkDoneProgressBegin(kIndexProgressToken, "Luau: Indexing");

    std::vector<Luau::ModuleName> moduleNames;
    std::vector<std::string> directories{rootUri.fsPath()};

    auto luauConfig = fileResolver.readConfigRec(rootUri, limits);
    for (const auto& [aliasName, aliasInfo] : luauConfig.aliases)
    {
        auto uri = resolveAliasLocation(aliasInfo);
        if (!rootUri.isAncestorOf(uri))
        {
            if (uri.isDirectory())
                directories.emplace_back(uri.fsPath());
            else
                moduleNames.emplace_back(fileResolver.getModuleName(uri));
        }
    }

    bool sentMessage = false;
    for (const auto& directory : directories)
    {
        client->sendTrace("workspace: indexing files from '" + directory + "'");
        Luau::FileUtils::traverseDirectoryRecursive(directory,
            [&](auto& path)
            {
                if (moduleNames.size() >= config.index.maxFiles)
                {
                    if (!sentMessage)
                    {
                        client->sendWindowMessage(
                            lsp::MessageType::Warning, "The maximum workspace index limit (" + std::to_string(config.index.maxFiles) +
                                                           ") has been hit. This may cause some language features to only work partially "
                                                           "(Find All References, Rename). If necessary, consider increasing the limit");
                        sentMessage = true;
                    }
                    return;
                }

                auto uri = Uri::file(path);
                auto ext = uri.extension();
                if ((ext == ".lua" || ext == ".luau") && !isDefinitionFile(uri, config) && !isIgnoredFile(uri, config))
                {
                    auto moduleName = fileResolver.getModuleName(uri);
                    moduleNames.emplace_back(moduleName);
                }
            });
    }

    client->sendWorkDoneProgressReport(kIndexProgressToken, std::to_string(moduleNames.size()) + " files");

    frontend.clearStats();
    frontend.parseModules(moduleNames);

    client->sendLogMessage(lsp::MessageType::Info,
        "Indexed " + std::to_string(frontend.stats.files) + " files (" + std::to_string(frontend.stats.lines) +
            " lines)\n Time read: " + std::to_string(frontend.stats.timeRead) + "\n Time parse: " + std::to_string(frontend.stats.timeParse));

    client->sendWorkDoneProgressEnd(kIndexProgressToken, "Indexed " + std::to_string(moduleNames.size()) + " files");
    client->sendTrace("workspace: indexing all files COMPLETED");
}

static void clearDisabledGlobals(const Client* client, const Luau::GlobalTypes& globalTypes, const std::vector<std::string>& disabledGlobals)
{
    const auto targetScope = globalTypes.globalScope;
    for (const auto& disabledGlobal : disabledGlobals)
    {
        std::string library = disabledGlobal;
        std::optional<std::string> method = std::nullopt;

        if (const auto separator = disabledGlobal.find('.'); separator != std::string::npos)
        {
            library = disabledGlobal.substr(0, separator);
            method = disabledGlobal.substr(separator + 1);
        }

        const auto globalName = globalTypes.globalNames.names->get(library.c_str());
        if (globalName.value == nullptr)
        {
            client->sendLogMessage(lsp::MessageType::Warning, "disabling globals: skipping unknown global - " + disabledGlobal);
            continue;
        }

        if (auto binding = targetScope->bindings.find(globalName); binding != targetScope->bindings.end())
        {
            if (method)
            {
                const auto typeId = Luau::follow(binding->second.typeId);
                if (const auto ttv = Luau::getMutable<Luau::TableType>(typeId))
                {
                    if (contains(ttv->props, *method))
                    {
                        client->sendLogMessage(lsp::MessageType::Info, "disabling globals: erasing global - " + disabledGlobal);
                        ttv->props.erase(*method);
                    }
                    else
                        client->sendLogMessage(lsp::MessageType::Warning, "disabling globals: could not find method - " + disabledGlobal);
                }
                else if (const auto ctv = Luau::getMutable<Luau::ExternType>(typeId))
                {
                    if (contains(ctv->props, *method))
                    {
                        client->sendLogMessage(lsp::MessageType::Info, "disabling globals: erasing global - " + disabledGlobal);
                        ctv->props.erase(*method);
                    }
                    else
                        client->sendLogMessage(lsp::MessageType::Warning, "disabling globals: could not find method - " + disabledGlobal);
                }
                else
                {
                    client->sendLogMessage(lsp::MessageType::Warning,
                        "disabling globals: cannot clear method from global, only tables or classes are supported - " + disabledGlobal);
                }
            }
            else
            {
                client->sendLogMessage(lsp::MessageType::Info, "disabling globals: erasing global - " + disabledGlobal);
                targetScope->bindings.erase(globalName);
            }
        }
        else
        {
            client->sendLogMessage(lsp::MessageType::Warning, "disabling globals: skipping unknown global - " + disabledGlobal);
        }
    }
}

static void assignNestedDocumentationSymbols(
    Luau::TypeId type, const std::string& symbol, Luau::DenseHashSet<Luau::TypeId>& seen)
{
    type = Luau::follow(type);
    if (seen.contains(type))
        return;
    seen.insert(type);

    Luau::asMutable(type)->documentationSymbol = symbol;
    auto assignProperties = [&](auto& properties)
    {
        for (auto& [name, property] : properties)
        {
            std::string propertySymbol = symbol + "." + name;
            property.documentationSymbol = propertySymbol;
            if (property.readTy)
                assignNestedDocumentationSymbols(*property.readTy, propertySymbol, seen);
        }
    };

    if (auto table = Luau::getMutable<Luau::TableType>(type))
        assignProperties(table->props);
    else if (auto externType = Luau::getMutable<Luau::ExternType>(type))
        assignProperties(externType->props);
}

static void assignNestedDocumentationSymbols(const Luau::ScopePtr& scope)
{
    Luau::DenseHashSet<Luau::TypeId> seen;
    for (const auto& [_, binding] : scope->bindings)
    {
        if (binding.documentationSymbol)
            assignNestedDocumentationSymbols(binding.typeId, *binding.documentationSymbol, seen);
    }
    for (const auto& [_, binding] : scope->exportedTypeBindings)
    {
        if (binding.type->documentationSymbol)
            assignNestedDocumentationSymbols(binding.type, *binding.type->documentationSymbol, seen);
    }
}

static void persistDefinitionTypes(
    const Luau::ModulePtr& module, Luau::GlobalTypes& globals, const Luau::ScopePtr& targetScope, const std::string& packageName)
{
    Luau::CloneState cloneState{globals.builtinTypes};
    std::vector<Luau::TypeId> persistedTypes;
    persistedTypes.reserve(module->declaredGlobals.size() + module->exportedTypeBindings.size());

    for (const auto& [name, type] : module->declaredGlobals)
    {
        Luau::TypeId persistedType = Luau::clone(type, globals.globalTypes, cloneState);
        const std::string documentationSymbol = packageName + "/global/" + name;
        Luau::DenseHashSet<Luau::TypeId> seen;
        assignNestedDocumentationSymbols(persistedType, documentationSymbol, seen);
        targetScope->bindings[globals.globalNames.names->getOrAdd(name.c_str())] = {
            persistedType, Luau::Location(), false, {}, documentationSymbol};
        persistedTypes.push_back(persistedType);
    }

    for (const auto& [name, typeFunction] : module->exportedTypeBindings)
    {
        Luau::TypeFun persistedType = Luau::clone(typeFunction, globals.globalTypes, cloneState);
        const std::string documentationSymbol = packageName + "/globaltype/" + name;
        Luau::DenseHashSet<Luau::TypeId> seen;
        assignNestedDocumentationSymbols(persistedType.type, documentationSymbol, seen);
        persistedTypes.push_back(persistedType.type);
        targetScope->exportedTypeBindings[name] = std::move(persistedType);
    }

    for (Luau::TypeId type : persistedTypes)
        Luau::persist(type);
}

static Luau::LoadDefinitionFileResult loadDefinitionFileWithPrivateTypesExposed(Luau::Frontend& frontend, Luau::GlobalTypes& globals,
    const Luau::ScopePtr& targetScope, const std::string& source, const std::string& packageName)
{
    Luau::SourceModule sourceModule;
    sourceModule.name = packageName;
    sourceModule.humanReadableName = packageName;

    Luau::ParseOptions options;
    options.allowDeclarationSyntax = true;
    options.captureComments = true;
    Luau::ParseResult parseResult =
        Luau::Parser::parse(source.data(), source.size(), *sourceModule.names, *sourceModule.allocator, options);
    sourceModule.root = parseResult.root;
    sourceModule.mode = Luau::Mode::Definition;
    sourceModule.hotcomments = parseResult.hotcomments;
    sourceModule.commentLocations = parseResult.commentLocations;

    if (!parseResult.errors.empty())
        return {false, std::move(parseResult), std::move(sourceModule), nullptr};

    // Platform definition files describe a global environment. When explicitly enabled,
    // treat their top-level private type declarations as exports before type checking so
    // globals and aliases are cloned as one coherent public type graph.
    for (Luau::AstStat* statement : sourceModule.root->body)
    {
        if (auto alias = statement->as<Luau::AstStatTypeAlias>())
            alias->exported = true;
        else if (auto typeFunction = statement->as<Luau::AstStatTypeFunction>())
            typeFunction->exported = true;
    }

    auto prepareModuleScope = [&frontend](const Luau::ModuleName& name, const Luau::ScopePtr& scope)
    {
        if (frontend.prepareModuleScope)
            frontend.prepareModuleScope(name, scope, false);
    };

    Luau::Frontend::Stats stats;
    Luau::ModulePtr checkedModule = Luau::check(sourceModule, Luau::Mode::Definition, {}, frontend.builtinTypes,
        Luau::NotNull{&frontend.iceHandler}, Luau::NotNull{&frontend.moduleResolver}, Luau::NotNull{frontend.fileResolver}, globals.globalScope,
        globals.globalTypeFunctionScope, prepareModuleScope, frontend.options, {}, false, stats, frontend.writeJsonLog);

    if (!checkedModule->errors.empty())
        return {false, std::move(parseResult), std::move(sourceModule), std::move(checkedModule)};

    persistDefinitionTypes(checkedModule, globals, targetScope, packageName);
    return {true, std::move(parseResult), std::move(sourceModule), std::move(checkedModule)};
}

struct DefinitionScopeSnapshot
{
    std::unordered_map<Luau::Symbol, Luau::Binding> bindings;
    std::unordered_map<Luau::Name, Luau::TypeFun> exportedTypeBindings;
};

static DefinitionScopeSnapshot snapshotDefinitionScope(const Luau::ScopePtr& scope)
{
    return {scope->bindings, scope->exportedTypeBindings};
}

Luau::LoadDefinitionFileResult WorkspaceFolder::loadDefinitionFile(
    const std::string& packageName, const std::string& source, std::optional<nlohmann::json> metadata,
    std::optional<Luau::ScopePtr> targetScope, bool exposePrivateTypes)
{
    auto result = targetScope && exposePrivateTypes
                      ? loadDefinitionFileWithPrivateTypesExposed(frontend, frontend.globals, *targetScope, source, packageName)
                  : targetScope ? frontend.loadDefinitionFile(frontend.globals, *targetScope, source, packageName, /* captureComments= */ true)
                                : types::registerDefinitions(frontend, frontend.globals, packageName, source);
    if (!targetScope && !FFlag::LuauSolverV2)
        types::registerDefinitions(frontend, frontend.globalsForAutocomplete, packageName, source);

    if (result.success && targetScope)
        assignNestedDocumentationSymbols(*targetScope);

    platform->mutateRegisteredDefinitions(frontend.globals, metadata);
    platform->mutateRegisteredDefinitions(frontend.globalsForAutocomplete, metadata);

    if (result.success)
    {
        TextDocument textDocument(Uri(), "luau", 0, source);
        definitionsFileState.emplace(packageName, DefinitionsFileState{std::move(textDocument), result.sourceModule, std::move(result.module)});
    }

    return result;
}

void WorkspaceFolder::registerTypes(const std::vector<std::string>& disabledGlobals)
{
    LUAU_TIMETRACE_SCOPE("WorkspaceFolder::initialize", "LSP");
    client->sendTrace("workspace initialization: registering Luau globals");
    Luau::registerBuiltinGlobals(frontend, frontend.globals);
    if (!FFlag::LuauSolverV2)
        Luau::registerBuiltinGlobals(frontend, frontend.globalsForAutocomplete);

    auto& tagRegisterGlobals = FFlag::LuauSolverV2 ? frontend.globals : frontend.globalsForAutocomplete;
    Luau::attachTag(Luau::getGlobalBinding(tagRegisterGlobals, "require"), "Require");

    // Register LSPPlugin environment for plugin type checking
    client->sendTrace("workspace initialization: registering LSPPlugin environment");
    frontend.registerBuiltinDefinition(
        "LSPPlugin",
        [this](Luau::Frontend& frontend, Luau::GlobalTypes& globals, Luau::ScopePtr scope)
        {
            auto result = frontend.loadDefinitionFile(
                globals, scope, Luau::LanguageServer::Plugin::LSPPLUGIN_DEFINITIONS, "@LSPPlugin", /* captureComments */ true);
            if (result.success)
            {
                TextDocument textDocument(Uri::parse("internal://environments/LSPPlugin"), "luau", 0, Luau::LanguageServer::Plugin::LSPPLUGIN_DEFINITIONS);
                definitionsFileState.emplace(
                    "@LSPPlugin", DefinitionsFileState{std::move(textDocument), std::move(result.sourceModule), std::move(result.module)});
            }
            else
            {
                client->sendLogMessage(lsp::MessageType::Warning, "Failed to register plugin type definitions");
            }
        });
    frontend.addEnvironment("LSPPlugin");
    frontend.applyBuiltinDefinitionToEnvironment("LSPPlugin", "LSPPlugin");
    client->sendTrace("workspace initialization: registering LSPPlugin environment COMPLETED");

    if (const auto* documentation = platform->getBuiltinDocumentation())
        parseDocumentationContents(documentation, "bundled platform documentation", client->documentation, client, /* overwriteExisting= */ false);

    auto platformDefinitionEnvironments = platform->getDefinitionEnvironments();
    if (client->definitionsFiles.empty() && !platform->getBuiltinDefinitions() && platformDefinitionEnvironments.empty())
        client->sendLogMessage(lsp::MessageType::Warning, "No definitions file provided by client");

    const PlatformDefinitionConfiguration definitionConfiguration = platform->getDefinitionConfiguration();
    const auto baseBindings = frontend.globals.globalScope->bindings;
    const auto baseTypeBindings = frontend.globals.globalScope->exportedTypeBindings;
    bool attemptedDefinitionFiles = false;
    size_t loadedDefinitionFiles = 0;

    auto loadConfiguredDefinitionFiles = [&]()
    {
        if (attemptedDefinitionFiles)
            return;
        attemptedDefinitionFiles = true;

        // For backwards compatibility, process '@roblox' first.
        std::vector<std::pair<std::string, std::string>> definitionsFilesToProcess{};
        definitionsFilesToProcess.reserve(client->definitionsFiles.size());
        if (auto it = client->definitionsFiles.find("@roblox"); it != client->definitionsFiles.end())
            definitionsFilesToProcess.emplace_back(*it);
        for (const auto& pair : client->definitionsFiles)
            if (pair.first != "@roblox")
                definitionsFilesToProcess.emplace_back(pair);

        for (const auto& [packageName, definitionsFile] : definitionsFilesToProcess)
        {
            auto resolvedFilePath = resolvePath(definitionsFile);
            client->sendLogMessage(lsp::MessageType::Info, "Loading definitions file: " + packageName + " - " + resolvedFilePath);
            auto definitionsContents = Luau::FileUtils::readFile(resolvedFilePath);
            if (!definitionsContents)
            {
                client->sendWindowMessage(
                    lsp::MessageType::Error, "Failed to read definitions file " + resolvedFilePath + ". Extended types will not be provided");
                continue;
            }

            client->sendTrace("workspace initialization: parsing definitions file metadata");
            auto metadata = types::parseDefinitionsFileMetadata(*definitionsContents);
            if (!definitionsFileMetadata)
                definitionsFileMetadata = metadata;
            client->sendTrace("workspace initialization: parsing definitions file metadata COMPLETED", json(definitionsFileMetadata).dump());

            client->sendTrace("workspace initialization: registering types definition");
            auto result = loadDefinitionFile(packageName, *definitionsContents, metadata);
            client->sendTrace("workspace initialization: registering types definition COMPLETED");
            auto uri = Uri::file(resolvedFilePath);

            if (result.success)
            {
                ++loadedDefinitionFiles;
                client->publishDiagnostics({uri, std::nullopt, {}});
                if (auto it = definitionsFileState.find(packageName); it != definitionsFileState.end())
                    it->second.textDocument = TextDocument(uri, "luau", 0, *definitionsContents);
            }
            else
            {
                client->sendWindowMessage(
                    lsp::MessageType::Error, "Failed to load definitions file " + resolvedFilePath + ". Extended types will not be provided");
                std::vector<lsp::Diagnostic> diagnostics;
                for (auto& error : result.parseResult.errors)
                    diagnostics.emplace_back(createParseErrorDiagnostic(error));
                if (result.module)
                    for (auto& error : result.module->errors)
                        diagnostics.emplace_back(createTypeErrorDiagnostic(error, &fileResolver));
                client->publishDiagnostics({uri, std::nullopt, diagnostics});
            }
        }
    };

    if (definitionConfiguration.globalPolicy == PlatformGlobalDefinitionsPolicy::DefinitionFilesOnly ||
        definitionConfiguration.globalPolicy == PlatformGlobalDefinitionsPolicy::PreferDefinitionFiles)
        loadConfiguredDefinitionFiles();

    bool platformDefinitionsActive = !platformDefinitionEnvironments.empty();
    if (definitionConfiguration.globalPolicy == PlatformGlobalDefinitionsPolicy::DefinitionFilesOnly)
        platformDefinitionsActive = false;
    else if (definitionConfiguration.globalPolicy == PlatformGlobalDefinitionsPolicy::PreferDefinitionFiles && loadedDefinitionFiles > 0)
        platformDefinitionsActive = false;

    for (const auto& requirement : platform->getRequirements(platformDefinitionsActive))
    {
        if (requirement.satisfied)
            continue;

        platformDefinitionsActive = false;
        std::vector<lsp::MessageActionItem> actions{{"Open Settings"}};
        if (requirement.fallbackSettingsKey && requirement.fallbackSettingsValue)
            actions.push_back({"Use Definition Files Only"});
        client->sendWindowMessageRequest(lsp::MessageType::Error, requirement.message, actions,
            [client = client, requirement](const JsonRpcMessage& response)
            {
                if (!response.result || response.result->is_null())
                    return;
                const auto action = response.result->get<lsp::MessageActionItem>();
                if (action.title == "Open Settings")
                    client->sendNotification(
                        "$/command", json{{"command", "workbench.action.openSettings"}, {"data", requirement.settingsKey}});
                else if (action.title == "Use Definition Files Only" && requirement.fallbackSettingsKey && requirement.fallbackSettingsValue)
                    client->sendNotification("$/command",
                        json{{"command", "luau-lsp.updateSettingAndReload"},
                            {"data", json{{"key", *requirement.fallbackSettingsKey}, {"value", *requirement.fallbackSettingsValue}}}});
            });
    }

    std::vector<std::string> platformDefinitionWarnings;
    std::vector<Luau::ScopePtr> platformDefinitionScopes;
    bool loadedAllPlatformEnvironments = platformDefinitionsActive;
    if (platformDefinitionsActive)
    {
        for (auto& environment : platformDefinitionEnvironments)
        {
            auto targetScope = frontend.addEnvironment(environment.environmentName);
            bool loaded = false;
            for (auto& candidate : environment.candidates)
            {
                if (!candidate.source)
                {
                    const std::string reason = candidate.error.value_or("no source was provided");
                    client->sendLogMessage(lsp::MessageType::Warning,
                        "Skipping " + candidate.label + " for " + environment.environmentName + ": " + reason);
                    if (candidate.notifyOnFailure)
                        platformDefinitionWarnings.push_back(environment.environmentName + ": " + reason);
                    continue;
                }

                auto result = loadDefinitionFile(
                    environment.packageName, *candidate.source, std::nullopt, targetScope, environment.exposePrivateTypes);
                if (!result.success)
                {
                    std::string reason = !result.parseResult.errors.empty() ? "Luau could not parse it" : "Luau could not register it";
                    client->sendLogMessage(lsp::MessageType::Warning,
                        "Skipping " + candidate.label + " for " + environment.environmentName + ": " + reason);
                    if (candidate.notifyOnFailure)
                    {
                        if (candidate.sourceUri)
                            reason += " at " + candidate.sourceUri->fsPath();
                        platformDefinitionWarnings.push_back(environment.environmentName + ": " + reason);
                    }
                    continue;
                }

                if (candidate.sourceUri)
                    if (auto it = definitionsFileState.find(environment.packageName); it != definitionsFileState.end())
                        it->second.textDocument = TextDocument(*candidate.sourceUri, "luau", 0, *candidate.source);
                client->sendLogMessage(
                    lsp::MessageType::Info, "Loaded " + candidate.label + " for " + environment.environmentName);
                loaded = true;
                break;
            }

            if (!loaded)
            {
                loadedAllPlatformEnvironments = false;
                client->sendWindowMessage(
                    lsp::MessageType::Error, "Could not load any usable platform definitions for " + environment.environmentName + ".");
                continue;
            }
            platformDefinitionScopes.push_back(targetScope);
        }
    }

    if (definitionConfiguration.globalPolicy == PlatformGlobalDefinitionsPolicy::Combine)
    {
        std::vector<DefinitionScopeSnapshot> platformSnapshots;
        if (definitionConfiguration.conflictWinner == PlatformDefinitionConflictWinner::Platform)
        {
            platformSnapshots.reserve(platformDefinitionScopes.size());
            for (const auto& targetScope : platformDefinitionScopes)
                platformSnapshots.push_back(snapshotDefinitionScope(targetScope));
        }

        loadConfiguredDefinitionFiles();
        if (loadedDefinitionFiles > 0 && definitionConfiguration.conflictWinner == PlatformDefinitionConflictWinner::Platform)
        {
            // Luau combines same-named declarations from an environment scope and its parent globals.
            // Remove only configured definition-file bindings that collide with the selected platform bindings,
            // restoring any binding that existed before configured definitions were loaded.
            for (const auto& snapshot : platformSnapshots)
            {
                for (const auto& [name, _] : snapshot.bindings)
                {
                    const auto current = frontend.globals.globalScope->bindings.find(name);
                    if (current == frontend.globals.globalScope->bindings.end())
                        continue;

                    const auto original = baseBindings.find(name);
                    if (original == baseBindings.end())
                        frontend.globals.globalScope->bindings.erase(current);
                    else if (original->second.typeId != current->second.typeId)
                        current->second = original->second;
                }
                for (const auto& [name, _] : snapshot.exportedTypeBindings)
                {
                    const auto current = frontend.globals.globalScope->exportedTypeBindings.find(name);
                    if (current == frontend.globals.globalScope->exportedTypeBindings.end())
                        continue;

                    const auto original = baseTypeBindings.find(name);
                    if (original == baseTypeBindings.end())
                        frontend.globals.globalScope->exportedTypeBindings.erase(current);
                    else if (original->second.type != current->second.type)
                        current->second = original->second;
                }
            }

            for (size_t index = 0; index < platformDefinitionScopes.size(); ++index)
            {
                platformDefinitionScopes[index]->bindings = std::move(platformSnapshots[index].bindings);
                platformDefinitionScopes[index]->exportedTypeBindings = std::move(platformSnapshots[index].exportedTypeBindings);
            }
        }
        else if (loadedDefinitionFiles > 0 &&
                 definitionConfiguration.conflictWinner == PlatformDefinitionConflictWinner::DefinitionFiles)
        {
            const DefinitionScopeSnapshot definitionFilesSnapshot =
                snapshotDefinitionScope(frontend.globals.globalScope);
            for (const auto& targetScope : platformDefinitionScopes)
            {
                for (const auto& [name, binding] : definitionFilesSnapshot.bindings)
                {
                    const auto original = baseBindings.find(name);
                    const auto current = frontend.globals.globalScope->bindings.find(name);
                    if (current != frontend.globals.globalScope->bindings.end() &&
                        (original == baseBindings.end() || original->second.typeId != current->second.typeId))
                        targetScope->bindings[name] = binding;
                }
                for (const auto& [name, binding] : definitionFilesSnapshot.exportedTypeBindings)
                {
                    const auto original = baseTypeBindings.find(name);
                    const auto current = frontend.globals.globalScope->exportedTypeBindings.find(name);
                    if (current != frontend.globals.globalScope->exportedTypeBindings.end() &&
                        (original == baseTypeBindings.end() || original->second.type != current->second.type))
                        targetScope->exportedTypeBindings[name] = binding;
                }
            }
        }
    }

    if (!loadedAllPlatformEnvironments)
        platformDefinitionsActive = false;
    platform->setDefinitionEnvironmentsActive(platformDefinitionsActive);

    if (definitionConfiguration.globalPolicy == PlatformGlobalDefinitionsPolicy::PreferPlatform && !platformDefinitionsActive)
        loadConfiguredDefinitionFiles();

    if (!platformDefinitionWarnings.empty())
    {
        std::string message = "Could not load the active platform API definitions and used a fallback. "
                              "Check luau-lsp.platform.luduvo.dataDirectory, then reload the workspace.";
        for (const auto& warning : platformDefinitionWarnings)
            message += "\n" + warning;

        client->sendWindowMessageRequest(lsp::MessageType::Warning, message, {{"Open Luduvo Settings"}},
            [client = client](const JsonRpcMessage& response)
            {
                if (!response.result || response.result->is_null())
                    return;
                const auto action = response.result->get<lsp::MessageActionItem>();
                if (action.title == "Open Luduvo Settings")
                    client->sendNotification("$/command",
                        json{{"command", "workbench.action.openSettings"}, {"data", "luau-lsp.platform.luduvo.dataDirectory"}});
            });
    }

    if (!disabledGlobals.empty())
    {
        client->sendTrace("workspace initialization: removing disabled globals");
        clearDisabledGlobals(client, frontend.globals, disabledGlobals);
        if (!FFlag::LuauSolverV2)
            clearDisabledGlobals(client, frontend.globalsForAutocomplete, disabledGlobals);
        client->sendTrace("workspace initialization: removing disabled globals COMPLETED");
    }

    Luau::freeze(frontend.globals.globalTypes);
    if (!FFlag::LuauSolverV2)
        Luau::freeze(frontend.globalsForAutocomplete.globalTypes);
}

void WorkspaceFolder::lazyInitialize()
{
    if (isReady)
        return;

    LUAU_TIMETRACE_SCOPE("WorkspaceFolder::lazyInitialize", "LSP");

    if (isNullWorkspace())
    {
        client->sendTrace("initializing null workspace");
        setupWithConfiguration(client->globalConfig);
    }
    else
    {
        client->sendTrace("initializing workspace: " + rootUri.toString());
        auto config = client->getConfiguration(rootUri);
        setupWithConfiguration(config);
    }

    isReady = true;
}

void WorkspaceFolder::setupWithConfiguration(const ClientConfiguration& configuration)
{
    LUAU_TIMETRACE_SCOPE("WorkspaceFolder::setupWithConfiguration", "LSP");
    client->sendTrace("workspace: setting up with configuration");

    // Apply first-time configuration
    if (!appliedFirstTimeConfiguration)
    {
        appliedFirstTimeConfiguration = true;

        client->sendTrace("workspace: first time configuration, setting appropriate platform");
        platform = LSPPlatform::getPlatform(configuration, &fileResolver, this);
        fileResolver.platform = platform.get();
        fileResolver.requireSuggester = fileResolver.platform->getRequireSuggester();

        registerTypes(configuration.types.disabledGlobals);
    }

    client->sendTrace("workspace: apply platform-specific configuration");

    platform->setupWithConfiguration(configuration);

    // Configure plugins
    if (configuration.plugins.enabled && !configuration.plugins.paths.empty())
    {
        client->sendTrace("workspace: configuring plugins");

        // Always recreate the plugin manager to ensure clean state
        fileResolver.pluginManager = std::make_unique<Luau::LanguageServer::Plugin::PluginManager>(client, Luau::NotNull<WorkspaceFolder>{this});

        size_t loadedCount = fileResolver.pluginManager->configure(configuration.plugins.paths, configuration.plugins.timeoutMs);
        client->sendLogMessage(lsp::MessageType::Info, "Loaded " + std::to_string(loadedCount) + " of " +
            std::to_string(configuration.plugins.paths.size()) + " plugins");

        // Clear plugin document cache when plugins change
        fileResolver.clearPluginDocuments();
    }
    else if (fileResolver.pluginManager)
    {
        // Plugins disabled - clear plugin manager and caches
        fileResolver.pluginManager.reset();
        fileResolver.clearPluginDocuments();
    }

    if (configuration.index.enabled)
        indexFiles(configuration);

    client->sendTrace("workspace: setting up with configuration COMPLETED");
}
