#include "Platform/LuduvoPlatform.hpp"
#include "Platform/LuduvoDefinitionResolver.hpp"
#include "LuduvoDefinitions.hpp"
#include "LuduvoDocumentation.hpp"
#include "LSP/Workspace.hpp"
#include "LSP/WorkspaceFileResolver.hpp"
#include "LSP/Utils.hpp"
#include "Luau/Common.h"

#include <algorithm>
#include <array>

static constexpr const char* kServerEnvironment = "LuduvoServer";
static constexpr const char* kClientEnvironment = "LuduvoClient";

LUAU_FASTFLAG(LuauSolverV2)

LuduvoPlatform::LuduvoPlatform(const ClientLuduvoConfiguration& configuration, WorkspaceFileResolver* fileResolver, WorkspaceFolder* workspaceFolder)
    : LSPPlatform(fileResolver, workspaceFolder)
    , configuration(configuration)
{
    resourceCatalog.configure(configuration);
    resourceBindings.configure(configuration);
}

void LuduvoPlatform::reportResourceConfigurationWarnings(const std::vector<std::string>& warnings)
{
    if (!workspaceFolder)
        return;

    std::vector<std::string> newWarnings;
    for (const std::string& warning : warnings)
        if (reportedResourceWarnings.insert(warning).second)
            newWarnings.push_back(warning);
    if (newWarnings.empty())
        return;

    std::string message = "Some Luduvo resource configuration entries were ignored:";
    for (const std::string& warning : newWarnings)
        message += "\n- " + warning;

    workspaceFolder->client->sendWindowMessageRequest(lsp::MessageType::Warning, message, {{"Open Resource Settings"}},
        [client = workspaceFolder->client](const JsonRpcMessage& response)
        {
            if (!response.result || response.result->is_null())
                return;
            const auto action = response.result->get<lsp::MessageActionItem>();
            if (action.title == "Open Resource Settings")
                client->sendNotification(
                    "$/command", json{{"command", "workbench.action.openSettings"}, {"data", "luau-lsp.platform.luduvo.resources"}});
        });
}

std::vector<LuduvoResolvedResourceBinding> LuduvoPlatform::resolveResourceBindings(
    const std::optional<std::string>& documentationSymbol, Luau::TypeId callableType)
{
    std::vector<std::string> warnings;
    auto result = resourceBindings.resolve(documentationSymbol, callableType, &warnings);
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
    reportResourceConfigurationWarnings(warnings);
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
    resourceCatalog.configure(configuration);
    resourceBindings.configure(configuration);
    reportedResourceWarnings.clear();
    reportResourceConfigurationWarnings(resourceCatalog.configurationWarnings());
    reportResourceConfigurationWarnings(resourceBindings.configurationWarnings());

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

bool LuduvoPlatform::isLintIgnored(const Luau::LintWarning& lint) const
{
    if (lint.code != Luau::LintWarning::Code_FunctionUnused)
        return false;

    static constexpr std::array lifecycleFunctions{"Update", "PhysicsUpdate", "Migrate"};
    for (const char* name : lifecycleFunctions)
    {
        if (lint.text == "Function '" + std::string(name) + "' is never used; prefix with '_' to silence")
            return true;
    }
    return false;
}
