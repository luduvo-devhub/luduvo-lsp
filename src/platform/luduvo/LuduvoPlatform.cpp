#include "Platform/luduvo/LuduvoPlatform.hpp"
#include "Platform/luduvo/diagnostics/LuduvoDiagnosticAdapter.hpp"
#include "Platform/luduvo/LuduvoLifecycle.hpp"
#include "Platform/luduvo/LuduvoDefinitionResolver.hpp"
#include "LuduvoDefinitions.hpp"
#include "LuduvoDocumentation.hpp"
#include "LSP/Workspace.hpp"
#include "LSP/WorkspaceFileResolver.hpp"
#include "LSP/Utils.hpp"
#include "Luau/Common.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>

static constexpr const char* kServerEnvironment = "LuduvoServer";
static constexpr const char* kClientEnvironment = "LuduvoClient";

static bool isAutorunScriptPath(const std::string& path)
{
    const std::filesystem::path parent = std::filesystem::u8path(path).parent_path();
    return std::any_of(parent.begin(), parent.end(),
        [](const std::filesystem::path& segment)
        {
            std::string value = segment.generic_u8string();
            std::transform(value.begin(), value.end(), value.begin(),
                [](unsigned char character)
                {
                    return char(std::tolower(character));
                });
            return value == "autorun";
        });
}

LUAU_FASTFLAG(LuauSolverV2)

LuduvoPlatform::LuduvoPlatform(const ClientLuduvoConfiguration& configuration, WorkspaceFileResolver* fileResolver, WorkspaceFolder* workspaceFolder)
    : LSPPlatform(fileResolver, workspaceFolder)
    , configuration(configuration)
{
    diagnosticSettings = resolveLuduvoDiagnosticConfiguration(configuration.diagnostics).settings;
    resourceCatalog.configure(configuration);
    bindings.configure(configuration);
}

void LuduvoPlatform::configureContext()
{
    contextNotificationSubscription.reset();
    contextProvider.reset();
    contextHelperSession.reset();
    reportedContextStates.clear();
    if (!configuration.context.enabled || !workspaceFolder || workspaceFolder->rootUri.scheme != "file")
        return;

    contextProvider = std::make_shared<luduvo::ContextProvider>(
        luduvo::defaultContextStateDirectory(configuration.directories.local, configuration.directories.context), workspaceFolder->rootUri.fsPath());
    contextProvider->updateProjectFiles();
    std::string helperError;
    contextHelperSession = luduvo::acquireContextHelper(configuration, &helperError);
    if (!contextHelperSession && !helperError.empty())
        workspaceFolder->client->sendLogMessage(lsp::MessageType::Warning, "Luduvo context helper is unavailable: " + helperError);
    if (contextHelperSession)
    {
        Client* client = workspaceFolder->client;
        contextNotificationSubscription = luduvo::subscribeContextUpdates(contextHelperSession, client,
            [client](const luduvo::ContextUpdates& updates)
            {
                std::vector<std::string> messages;
                if (updates.updatedWorlds.size() == 1)
                    messages.push_back("Updated project context for '" + updates.updatedWorlds.front() + "' from a new world dump.");
                else if (!updates.updatedWorlds.empty())
                    messages.push_back("Updated project context for " + std::to_string(updates.updatedWorlds.size()) + " projects from new world dumps.");

                size_t inferredCount = 0;
                for (const auto& project : updates.inferredScripts)
                    inferredCount += project.scripts.size();
                if (inferredCount == 1)
                {
                    const auto& inferred = updates.inferredScripts.front();
                    const std::string name = std::filesystem::u8path(inferred.scripts.front()).filename().generic_u8string();
                    messages.push_back("Added '" + name + "' to project context for '" + inferred.projectName + "'.");
                }
                else if (inferredCount > 1)
                    messages.push_back("Added " + std::to_string(inferredCount) + " inferred files to project context across " +
                                       std::to_string(updates.inferredScripts.size()) +
                                       (updates.inferredScripts.size() == 1 ? " project." : " projects."));

                if (messages.empty())
                    return;
                std::string message = messages.front();
                for (size_t index = 1; index < messages.size(); ++index)
                    message += " " + messages[index];
                client->sendWindowMessage(lsp::MessageType::Info, message);
            });
    }
}

void LuduvoPlatform::reportConfigurationWarnings(std::string_view area, const std::vector<std::string>& warnings)
{
    if (!workspaceFolder)
        return;

    std::vector<std::string> newWarnings;
    for (const std::string& warning : warnings)
        if (reportedConfigurationWarnings.insert(std::string(area) + ":" + warning).second)
            newWarnings.push_back(warning);
    if (newWarnings.empty())
        return;

    std::string message = "Some Luduvo " + std::string(area) + " configuration entries were ignored:";
    for (const std::string& warning : newWarnings)
        message += "\n- " + warning;

    workspaceFolder->client->sendWindowMessageRequest(lsp::MessageType::Warning, message, {{"Open Luduvo Settings"}},
        [client = workspaceFolder->client](const JsonRpcMessage& response)
        {
            if (!response.result || response.result->is_null())
                return;
            const auto action = response.result->get<lsp::MessageActionItem>();
            if (action.title == "Open Luduvo Settings")
                client->sendNotification("$/command", json{{"command", "workbench.action.openSettings"}, {"data", "luau-lsp.platform.luduvo"}});
        });
}

std::vector<LuduvoResolvedResourceBinding> LuduvoPlatform::resolveResourceBindings(
    const std::optional<std::string>& documentationSymbol, Luau::TypeId callableType)
{
    std::vector<std::string> warnings;
    auto result = bindings.resolveResources(documentationSymbol, callableType, &warnings);
    result.erase(std::remove_if(result.begin(), result.end(),
                     [&](const LuduvoResolvedResourceBinding& binding)
                     {
                         const LuduvoResourceGroup* group = resourceCatalog.group(binding.kind);
                         if (!group)
                         {
                             warnings.push_back("Binding for '" + binding.parameter + "' references unknown resource group '" + binding.kind + "'.");
                             return true;
                         }
                         return group->dataType != "string";
                     }),
        result.end());
    reportConfigurationWarnings("binding or resource", warnings);
    return result;
}

const char* LuduvoPlatform::getBuiltinDocumentation() const
{
    return LUDUVO_DOCUMENTATION;
}

static PlatformDefinitionEnvironment definitionEnvironment(const char* environmentName, const char* packageName, LuduvoDefinitionKind kind,
    const ClientLuduvoConfiguration& configuration, std::string_view embeddedDefinitions)
{
    PlatformDefinitionEnvironment environment{environmentName, packageName};
    // Luduvo's official global type files declare table aliases with plain `type`
    // rather than `export type`, but those aliases are still part of the scripting API.
    environment.exposePrivateTypes = configuration.definitions.exposePrivateTypes;
    for (auto& candidate : resolveLuduvoDefinitionCandidates(kind, configuration, embeddedDefinitions))
    {
        environment.candidates.push_back({std::move(candidate.label), std::move(candidate.sourceUri), std::move(candidate.source),
            std::move(candidate.error), candidate.notifyOnFailure});
    }
    return environment;
}

std::vector<PlatformDefinitionEnvironment> LuduvoPlatform::getDefinitionEnvironments() const
{
    std::vector<PlatformDefinitionEnvironment> environments;
    environments.push_back(
        definitionEnvironment(kServerEnvironment, "@luduvo/server", LuduvoDefinitionKind::Server, configuration, LUDUVO_SERVER_DEFINITIONS));
    environments.push_back(
        definitionEnvironment(kClientEnvironment, "@luduvo/client", LuduvoDefinitionKind::Client, configuration, LUDUVO_CLIENT_DEFINITIONS));
    return environments;
}

PlatformDefinitionConfiguration LuduvoPlatform::getDefinitionConfiguration() const
{
    PlatformDefinitionConfiguration result;
    switch (configuration.definitions.globalPolicy)
    {
    case LuduvoGlobalDefinitionsPolicy::LuduvoOnly:
        result.globalPolicy = PlatformGlobalDefinitionsPolicy::PlatformOnly;
        break;
    case LuduvoGlobalDefinitionsPolicy::DefinitionFilesOnly:
        result.globalPolicy = PlatformGlobalDefinitionsPolicy::DefinitionFilesOnly;
        break;
    case LuduvoGlobalDefinitionsPolicy::PreferLuduvo:
        result.globalPolicy = PlatformGlobalDefinitionsPolicy::PreferPlatform;
        break;
    case LuduvoGlobalDefinitionsPolicy::PreferDefinitionFiles:
        result.globalPolicy = PlatformGlobalDefinitionsPolicy::PreferDefinitionFiles;
        break;
    case LuduvoGlobalDefinitionsPolicy::Combine:
        result.globalPolicy = PlatformGlobalDefinitionsPolicy::Combine;
        break;
    }
    result.conflictWinner = configuration.definitions.conflictWinner == LuduvoDefinitionConflictWinner::DefinitionFiles
                                ? PlatformDefinitionConflictWinner::DefinitionFiles
                                : PlatformDefinitionConflictWinner::Platform;
    return result;
}

std::vector<PlatformRequirement> LuduvoPlatform::getRequirements(bool platformDefinitionsActive) const
{
    if (!platformDefinitionsActive || FFlag::LuauSolverV2)
        return {};

    return {{"SolverV2", false,
        "Official Luduvo global definitions require SolverV2. Enable luau-lsp.fflags.enableNewSolver and restart the language server, or use "
        "types.definitionFiles instead.",
        {{"Open Settings", "luau-lsp.fflags.enableNewSolver", std::nullopt},
            {"Use Definition Files Only", "luau-lsp.platform.luduvo.definitions.globalPolicy", "definitionFilesOnly"}}}};
}

void LuduvoPlatform::reportDefinitionLoadWarnings(Client* client, const std::vector<std::string>& warnings) const
{
    if (warnings.empty())
        return;

    std::string message = "Could not load the active Luduvo API definitions and used a fallback. "
                          "Check luau-lsp.platform.luduvo.dataDirectory, then reload the workspace.";
    for (const std::string& warning : warnings)
        message += "\n" + warning;

    client->sendWindowMessageRequest(lsp::MessageType::Warning, message, {{"Open Luduvo Settings"}},
        [client](const JsonRpcMessage& response)
        {
            if (!response.result || response.result->is_null())
                return;
            const auto action = response.result->get<lsp::MessageActionItem>();
            if (action.title == "Open Luduvo Settings")
                client->sendNotification(
                    "$/command", json{{"command", "workbench.action.openSettings"}, {"data", "luau-lsp.platform.luduvo.dataDirectory"}});
        });
}

std::optional<std::string> LuduvoPlatform::getEnvironmentForModule(const Luau::ModuleName& moduleName) const
{
    if (!definitionEnvironmentsActive)
        return std::nullopt;

    if (!fileResolver)
        return configuration.definitions.defaultScriptSide == LuduvoScriptSide::Client ? kClientEnvironment : kServerEnvironment;

    const auto side = scriptSideFromPath(toLower(fileResolver->getUri(moduleName).filename()));
    if (side == ScriptSide::Client)
        return kClientEnvironment;
    if (side == ScriptSide::Server)
        return kServerEnvironment;

    return configuration.definitions.defaultScriptSide == LuduvoScriptSide::Client ? kClientEnvironment : kServerEnvironment;
}

void LuduvoPlatform::setupWithConfiguration(const ClientConfiguration& config)
{
    const bool defaultChanged = configuration.definitions.defaultScriptSide != config.platform.luduvo.definitions.defaultScriptSide;
    configuration = config.platform.luduvo;
    const LuduvoDiagnosticConfigurationResult diagnosticConfiguration = resolveLuduvoDiagnosticConfiguration(configuration.diagnostics);
    diagnosticSettings = diagnosticConfiguration.settings;
    resourceCatalog.configure(configuration);
    bindings.configure(configuration);
    configureContext();
    reportedConfigurationWarnings.clear();
    reportConfigurationWarnings("diagnostics", diagnosticConfiguration.warnings);
    reportConfigurationWarnings("binding or resource", resourceCatalog.configurationWarnings());
    reportConfigurationWarnings("binding or resource", bindings.configurationWarnings());

    if (defaultChanged && workspaceFolder)
    {
        std::vector<Luau::ModuleName> modules;
        modules.reserve(workspaceFolder->frontend.sourceNodes.size());
        for (const auto& [name, _] : workspaceFolder->frontend.sourceNodes)
            modules.push_back(name);
        for (const auto& name : modules)
            workspaceFolder->frontend.markDirty(name);
    }
}

static std::optional<std::string> commandScriptPath(const std::vector<json>& arguments)
{
    if (arguments.empty() || !arguments.front().is_object())
        return std::nullopt;
    const json& context = arguments.front();
    const auto uri = context.find("uri");
    if (uri == context.end() || !uri->is_string())
        return std::nullopt;
    const lsp::DocumentUri documentUri = lsp::DocumentUri::parse(uri->get<std::string>());
    if (documentUri.scheme != "file")
        return std::nullopt;
    return documentUri.fsPath();
}

static lsp::CodeAction contextCodeAction(std::string title, const lsp::DocumentUri& uri)
{
    lsp::CodeAction action;
    action.title = std::move(title);
    action.kind = lsp::CodeActionKind::QuickFix;
    action.command = lsp::Command{action.title, "luduvo.resolveProjectContext", {json{{"uri", uri.toString()}}}};
    return action;
}

void LuduvoPlatform::handleCodeAction(const lsp::CodeActionParams& params, std::vector<lsp::CodeAction>& items)
{
    if (!configuration.context.enabled || !params.context.wants(lsp::CodeActionKind::QuickFix) || !contextProvider ||
        params.context.diagnostics.empty())
        return;
    const auto document = fileResolver->getTextDocument(params.textDocument.uri);
    if (!document)
        return;
    const std::string selectedText = document->getText(params.range);
    const bool alreadyAdded = std::any_of(items.begin(), items.end(),
        [](const lsp::CodeAction& action)
        {
            return action.command && action.command->command == "luduvo.resolveProjectContext";
        });
    if (!alreadyAdded && (selectedText == "World" || (selectedText == "self" && !isAutorunScriptPath(params.textDocument.uri.fsPath()))))
        items.push_back(contextCodeAction("Resolve Luduvo project context", params.textDocument.uri));
}

void LuduvoPlatform::handleUnknownSymbolFix(const UnknownSymbolFixContext& ctx, const Luau::UnknownSymbol& unknownSymbol,
    const std::optional<lsp::Diagnostic>& diagnostic, std::vector<lsp::CodeAction>& result)
{
    LSPPlatform::handleUnknownSymbolFix(ctx, unknownSymbol, diagnostic, result);
    if (!configuration.context.enabled || !contextProvider || unknownSymbol.context != Luau::UnknownSymbol::Binding ||
        (unknownSymbol.name != "World" && unknownSymbol.name != "self") || (unknownSymbol.name == "self" && isAutorunScriptPath(ctx.uri.fsPath())))
        return;
    lsp::CodeAction action = contextCodeAction("Resolve Luduvo project context for '" + unknownSymbol.name + "'", ctx.uri);
    if (diagnostic)
        action.diagnostics.push_back(*diagnostic);
    result.push_back(std::move(action));
}

void LuduvoPlatform::notifyContextState(const lsp::DocumentUri& uri, bool force)
{
    if (!contextProvider || uri.scheme != "file" || !workspaceFolder)
        return;

    std::string error;
    const std::vector<luduvo::ProjectContextCandidate> candidates = contextProvider->findForScript(uri.fsPath(), &error);
    if (!error.empty())
    {
        const std::string state = uri.toString() + ":error:" + error;
        if (force || reportedContextStates.insert(state).second)
            workspaceFolder->client->sendWindowMessage(lsp::MessageType::Warning, "Luduvo project context could not be read: " + error);
        return;
    }
    if (candidates.empty())
    {
        if (force)
            workspaceFolder->client->sendWindowMessage(
                lsp::MessageType::Info, "No captured Luduvo project contains this script. Open or attach it in Luduvo Studio, then run world.dump.");
        return;
    }
    if (candidates.size() > 1)
    {
        const std::string state = uri.toString() + ":ambiguous:" + std::to_string(candidates.size());
        if (!force && !reportedContextStates.insert(state).second)
            return;
        if (candidates.size() <= 3)
        {
            std::vector<lsp::MessageActionItem> actions;
            std::vector<std::pair<std::string, std::string>> choices;
            for (const auto& candidate : candidates)
            {
                std::string title = "Use " + candidate.project.name;
                if (!candidate.project.directory.empty())
                    title += " (" + candidate.project.directory + ")";
                actions.push_back({title});
                choices.emplace_back(std::move(title), candidate.project.id);
            }
            workspaceFolder->client->sendWindowMessageRequest(lsp::MessageType::Info,
                "This script belongs to more than one captured Luduvo project. Choose the project context to use.", actions,
                [provider = std::weak_ptr<luduvo::ContextProvider>(contextProvider), scriptPath = uri.fsPath(), choices = std::move(choices)](
                    const JsonRpcMessage& response)
                {
                    if (!response.result || response.result->is_null())
                        return;
                    const std::string title = response.result->get<lsp::MessageActionItem>().title;
                    const auto choice = std::find_if(choices.begin(), choices.end(),
                        [&](const auto& item)
                        {
                            return item.first == title;
                        });
                    if (choice != choices.end())
                        if (const auto locked = provider.lock())
                            locked->select(scriptPath, choice->second);
                });
        }
        else
        {
            workspaceFolder->client->sendWindowMessageRequest(lsp::MessageType::Info,
                "This script belongs to " + std::to_string(candidates.size()) +
                    " captured Luduvo projects. Use Choose Luduvo Project Context to select one.",
                {{"Choose Project Context"}},
                [client = workspaceFolder->client, uri](const JsonRpcMessage& response)
                {
                    if (response.result && !response.result->is_null())
                        client->sendNotification("$/command", json{{"command", "luduvo-lsp.chooseProjectContext"}, {"data", uri.toString()}});
                });
        }
        return;
    }

    const luduvo::ContextProject& project = candidates.front().project;
    if (project.fresh)
    {
        if (force)
            workspaceFolder->client->sendWindowMessage(
                lsp::MessageType::Info, "Luduvo project context for '" + project.name + "' is current as of the last world.dump.");
        return;
    }

    std::string catalogError;
    const auto catalog = contextProvider->contextStore().load(&catalogError);
    if (!catalog)
    {
        workspaceFolder->client->sendWindowMessage(lsp::MessageType::Warning, "Luduvo project context could not be read: " + catalogError);
        return;
    }
    const bool active = catalog->activeProjectId == project.id;
    const std::string state = uri.toString() + ":stale:" + project.id + ":" + project.staleReason + (active ? ":active" : ":inactive");
    if (!force && !reportedContextStates.insert(state).second)
        return;
    if (!active)
    {
        workspaceFolder->client->sendWindowMessage(
            lsp::MessageType::Warning, "The captured project context for '" + project.name +
                                           "' is stale, but that project is not active in Luduvo Studio. Open it there before refreshing the dump.");
        return;
    }

    const auto request = contextProvider->createDumpRequest(uri.fsPath(), &error);
    if (!request)
    {
        workspaceFolder->client->sendWindowMessage(lsp::MessageType::Warning, "Luduvo project context needs refreshing. " + error);
        return;
    }
    const std::string message = "Luduvo project context for '" + project.name + "' is stale. Run this command in Luduvo Studio:\n" + request->command;
    workspaceFolder->client->sendWindowMessageRequest(lsp::MessageType::Warning, message, {{"Copy command"}, {"Copy output path"}},
        [client = workspaceFolder->client, command = request->command, outputPath = request->outputPath](const JsonRpcMessage& response)
        {
            if (!response.result || response.result->is_null())
                return;
            const std::string title = response.result->get<lsp::MessageActionItem>().title;
            const std::string value = title == "Copy command" ? command : outputPath;
            client->sendNotification("$/command", json{{"command", "luduvo-lsp.copyText"}, {"data", value}});
        });
}

void LuduvoPlatform::onDidOpenTextDocument(const lsp::DocumentUri& uri)
{
    notifyContextState(uri, false);
}

std::optional<json> LuduvoPlatform::handleWorkspaceCommand(std::string_view command, const std::vector<json>& arguments)
{
    constexpr std::string_view prefix = "luduvo.";
    if (command.compare(0, prefix.size(), prefix) != 0)
        return std::nullopt;
    if (!configuration.context.enabled)
        return json{{"enabled", false}, {"error", "Luduvo context integration is disabled"}};
    if (!contextProvider)
        return json{{"enabled", true}, {"error", "Luduvo context is unavailable for this workspace"}};

    if (command == "luduvo.contextHelperStatus")
    {
        std::string error;
        const auto status = contextProvider->contextStore().status(&error);
        if (!status)
            return json{{"enabled", true}, {"error", std::move(error)}};
        json result = *status;
        result["enabled"] = true;
        return result;
    }

    if (command == "luduvo.addProjectContext")
    {
        if (arguments.empty() || !arguments.front().is_object())
            return json{{"success", false}, {"error", "The command requires a project directory"}};
        const auto directory = arguments.front().find("directory");
        if (directory == arguments.front().end() || !directory->is_string() || directory->get_ref<const std::string&>().empty())
            return json{{"success", false}, {"error", "The command requires a project directory"}};
        const luduvo::ContextStoreResult result = contextProvider->addProject(directory->get<std::string>());
        return json{{"success", result.success}, {"projectId", result.projectId}, {"error", result.error}};
    }

    if (command == "luduvo.inspectProjectContext")
    {
        if (const auto scriptPath = commandScriptPath(arguments))
            return contextProvider->inspect(*scriptPath);
        std::string error;
        const auto overview = contextProvider->contextStore().overview(&error);
        if (!overview)
            return json{{"error", std::move(error)}};
        json result = *overview;
        result["kind"] = "context";
        return result;
    }

    if (command == "luduvo.cleanProjectContext" || command == "luduvo.forgetProjectContext")
    {
        if (arguments.empty() || !arguments.front().is_object())
            return json{{"success", false}, {"error", "The command requires a context subject"}};
        const json& argument = arguments.front();
        const std::string kind = argument.value("kind", "");
        const std::vector<std::string> selectors = argument.value("selectors", std::vector<std::string>{});
        const bool all = argument.value("all", false);
        const luduvo::ContextStoreResult result = command == "luduvo.cleanProjectContext"
                                                      ? contextProvider->contextStore().clean(kind, selectors, all)
                                                      : contextProvider->contextStore().forget(kind, selectors, all);
        return json{{"success", result.success}, {"affected", result.affected}, {"error", result.error}};
    }

    const auto scriptPath = commandScriptPath(arguments);
    if (!scriptPath)
        return json{{"enabled", true}, {"error", "The command requires a file URI in its first argument"}};
    if (command == "luduvo.resolveProjectContext")
    {
        notifyContextState(lsp::DocumentUri::parse(arguments.front().at("uri").get<std::string>()), true);
        return json(nullptr);
    }
    if (command == "luduvo.resetProjectContext")
    {
        const luduvo::ContextStoreResult result = contextProvider->reset(*scriptPath);
        return json{{"success", result.success}, {"error", result.error}};
    }
    if (command == "luduvo.chooseProjectContext")
    {
        const json& context = arguments.front();
        const auto projectId = context.find("projectId");
        if (projectId == context.end() || !projectId->is_string())
            return json{{"success", false}, {"error", "The command requires a projectId"}};
        const luduvo::ContextStoreResult result = contextProvider->select(*scriptPath, projectId->get<std::string>());
        return json{{"success", result.success}, {"projectId", result.projectId}, {"error", result.error}};
    }
    if (command == "luduvo.refreshProjectContext")
    {
        std::string error;
        const auto request = contextProvider->createDumpRequest(*scriptPath, &error);
        if (!request)
            return json{{"success", false}, {"error", error}};
        return json{{"success", true}, {"command", request->command}, {"outputPath", request->outputPath}, {"projectId", request->projectId}};
    }
    return std::nullopt;
}

bool LuduvoPlatform::isLintIgnored(const Luau::LintWarning& lint) const
{
    if (lint.code == Luau::LintWarning::Code_CommentDirective && lint.text.find("unknown lint rule 'luduvo/") != std::string::npos)
        return true;

    if (lint.code != Luau::LintWarning::Code_FunctionUnused)
        return false;

    for (const LuduvoLifecycleFunction& lifecycle : luduvoLifecycleFunctions)
    {
        if (lint.text == "Function '" + std::string(lifecycle.name) + "' is never used; prefix with '_' to silence")
            return true;
    }
    return false;
}

bool LuduvoPlatform::diagnosticsRequireFullTypeGraphs() const
{
    return diagnosticSettings.enabled;
}

std::vector<PlatformDiagnostic> LuduvoPlatform::collectDiagnostics(const PlatformDiagnosticContext& context)
{
    return collectLuduvoPlatformDiagnostics(diagnosticSettings, context);
}

std::vector<PlatformLintRuleCompletion> LuduvoPlatform::getLintRuleCompletions() const
{
    return getLuduvoLintRuleCompletions();
}
