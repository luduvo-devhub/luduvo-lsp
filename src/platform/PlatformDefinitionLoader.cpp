#include "Platform/PlatformDefinitionLoader.hpp"

#include "LSP/LuauExt.hpp"
#include "LSP/Utils.hpp"
#include "LSP/Workspace.hpp"
#include "LuauFileUtils.hpp"

#include <algorithm>
#include <unordered_map>

namespace
{
struct DefinitionScopeSnapshot
{
    std::unordered_map<Luau::Symbol, Luau::Binding> bindings;
    std::unordered_map<Luau::Name, Luau::TypeFun> exportedTypeBindings;
};

DefinitionScopeSnapshot snapshot(const Luau::ScopePtr& scope)
{
    return {scope->bindings, scope->exportedTypeBindings};
}
} // namespace

PlatformDefinitionLoader::PlatformDefinitionLoader(WorkspaceFolder& workspace)
    : workspace(workspace)
{
}

PlatformDefinitionLoadResult PlatformDefinitionLoader::load(
    const PlatformDefinitionConfiguration& configuration, std::vector<PlatformDefinitionEnvironment> environments)
{
    PlatformDefinitionLoadResult result;
    const auto baseBindings = workspace.frontend.globals.globalScope->bindings;
    const auto baseTypeBindings = workspace.frontend.globals.globalScope->exportedTypeBindings;
    bool attemptedConfiguredDefinitions = false;

    auto loadConfiguredDefinitions = [&]()
    {
        if (attemptedConfiguredDefinitions)
            return;
        attemptedConfiguredDefinitions = true;

        std::vector<std::pair<std::string, std::string>> files;
        files.reserve(workspace.client->definitionsFiles.size());
        if (auto it = workspace.client->definitionsFiles.find("@roblox"); it != workspace.client->definitionsFiles.end())
            files.emplace_back(*it);
        for (const auto& file : workspace.client->definitionsFiles)
            if (file.first != "@roblox")
                files.emplace_back(file);

        for (const auto& [packageName, file] : files)
        {
            const std::string path = resolvePath(file);
            workspace.client->sendLogMessage(lsp::MessageType::Info, "Loading definitions file: " + packageName + " - " + path);
            auto source = Luau::FileUtils::readFile(path);
            if (!source)
            {
                workspace.client->sendWindowMessage(
                    lsp::MessageType::Error, "Failed to read definitions file " + path + ". Extended types will not be provided");
                continue;
            }

            workspace.client->sendTrace("workspace initialization: parsing definitions file metadata");
            auto metadata = types::parseDefinitionsFileMetadata(*source);
            if (!workspace.definitionsFileMetadata)
                workspace.definitionsFileMetadata = metadata;
            workspace.client->sendTrace(
                "workspace initialization: parsing definitions file metadata COMPLETED", json(workspace.definitionsFileMetadata).dump());

            auto loadResult = workspace.loadDefinitionFile(packageName, *source, metadata);
            const Uri uri = Uri::file(path);
            if (loadResult.success)
            {
                ++result.configuredDefinitionFilesLoaded;
                workspace.client->publishDiagnostics({uri, std::nullopt, {}});
                workspace.setDefinitionFileSource(packageName, uri, *source);
                continue;
            }

            workspace.client->sendWindowMessage(
                lsp::MessageType::Error, "Failed to load definitions file " + path + ". Extended types will not be provided");
            std::vector<lsp::Diagnostic> diagnostics;
            for (const auto& error : loadResult.parseResult.errors)
                diagnostics.emplace_back(createParseErrorDiagnostic(error));
            if (loadResult.module)
                for (const auto& error : loadResult.module->errors)
                    diagnostics.emplace_back(createTypeErrorDiagnostic(error, &workspace.fileResolver));
            workspace.client->publishDiagnostics({uri, std::nullopt, diagnostics});
        }
    };

    if (configuration.globalPolicy == PlatformGlobalDefinitionsPolicy::DefinitionFilesOnly ||
        configuration.globalPolicy == PlatformGlobalDefinitionsPolicy::PreferDefinitionFiles)
        loadConfiguredDefinitions();

    bool platformActive = !environments.empty();
    if (configuration.globalPolicy == PlatformGlobalDefinitionsPolicy::DefinitionFilesOnly)
        platformActive = false;
    else if (configuration.globalPolicy == PlatformGlobalDefinitionsPolicy::PreferDefinitionFiles && result.configuredDefinitionFilesLoaded > 0)
        platformActive = false;
    for (const auto& requirement : workspace.platform->getRequirements(platformActive))
    {
        if (requirement.satisfied)
            continue;

        platformActive = false;
        std::vector<lsp::MessageActionItem> actions;
        actions.reserve(requirement.actions.size());
        for (const auto& action : requirement.actions)
            actions.push_back({action.title});
        workspace.client->sendWindowMessageRequest(lsp::MessageType::Error, requirement.message, actions,
            [client = workspace.client, requirement](const JsonRpcMessage& response)
            {
                if (!response.result || response.result->is_null())
                    return;
                const auto action = response.result->get<lsp::MessageActionItem>();
                const auto selected = std::find_if(requirement.actions.begin(), requirement.actions.end(),
                    [&](const PlatformSettingsAction& candidate)
                    {
                        return candidate.title == action.title;
                    });
                if (selected == requirement.actions.end())
                    return;
                if (selected->value)
                    client->sendNotification("$/command", json{{"command", "luau-lsp.updateSettingAndReload"},
                                                              {"data", json{{"key", selected->settingsKey}, {"value", *selected->value}}}});
                else
                    client->sendNotification("$/command", json{{"command", "workbench.action.openSettings"}, {"data", selected->settingsKey}});
            });
    }

    std::vector<Luau::ScopePtr> platformScopes;
    bool loadedEveryPlatformEnvironment = platformActive;
    if (platformActive)
    {
        for (auto& environment : environments)
        {
            Luau::ScopePtr targetScope = workspace.frontend.addEnvironment(environment.environmentName);
            bool loaded = false;
            for (auto& candidate : environment.candidates)
            {
                if (!candidate.source)
                {
                    const std::string reason = candidate.error.value_or("no source was provided");
                    workspace.client->sendLogMessage(
                        lsp::MessageType::Warning, "Skipping " + candidate.label + " for " + environment.environmentName + ": " + reason);
                    if (candidate.notifyOnFailure)
                        result.platformWarnings.push_back(environment.environmentName + ": " + reason);
                    continue;
                }

                auto loadResult = workspace.loadDefinitionFile(
                    environment.packageName, *candidate.source, std::nullopt, targetScope, environment.exposePrivateTypes);
                if (!loadResult.success)
                {
                    std::string reason = !loadResult.parseResult.errors.empty() ? "Luau could not parse it" : "Luau could not register it";
                    workspace.client->sendLogMessage(
                        lsp::MessageType::Warning, "Skipping " + candidate.label + " for " + environment.environmentName + ": " + reason);
                    if (candidate.notifyOnFailure)
                    {
                        if (candidate.sourceUri)
                            reason += " at " + candidate.sourceUri->fsPath();
                        result.platformWarnings.push_back(environment.environmentName + ": " + reason);
                    }
                    continue;
                }

                if (candidate.sourceUri)
                    workspace.setDefinitionFileSource(environment.packageName, *candidate.sourceUri, *candidate.source);
                workspace.client->sendLogMessage(lsp::MessageType::Info, "Loaded " + candidate.label + " for " + environment.environmentName);
                loaded = true;
                break;
            }

            if (!loaded)
            {
                loadedEveryPlatformEnvironment = false;
                workspace.client->sendWindowMessage(
                    lsp::MessageType::Error, "Could not load any usable platform definitions for " + environment.environmentName + ".");
                continue;
            }
            platformScopes.push_back(targetScope);
        }
    }

    if (configuration.globalPolicy == PlatformGlobalDefinitionsPolicy::Combine)
    {
        std::vector<DefinitionScopeSnapshot> platformSnapshots;
        if (configuration.conflictWinner == PlatformDefinitionConflictWinner::Platform)
        {
            platformSnapshots.reserve(platformScopes.size());
            for (const Luau::ScopePtr& scope : platformScopes)
                platformSnapshots.push_back(snapshot(scope));
        }

        loadConfiguredDefinitions();
        if (result.configuredDefinitionFilesLoaded > 0 && configuration.conflictWinner == PlatformDefinitionConflictWinner::Platform)
        {
            for (const DefinitionScopeSnapshot& platformSnapshot : platformSnapshots)
            {
                for (const auto& [name, _] : platformSnapshot.bindings)
                {
                    auto current = workspace.frontend.globals.globalScope->bindings.find(name);
                    if (current == workspace.frontend.globals.globalScope->bindings.end())
                        continue;
                    const auto original = baseBindings.find(name);
                    if (original == baseBindings.end())
                        workspace.frontend.globals.globalScope->bindings.erase(current);
                    else if (original->second.typeId != current->second.typeId)
                        current->second = original->second;
                }
                for (const auto& [name, _] : platformSnapshot.exportedTypeBindings)
                {
                    auto current = workspace.frontend.globals.globalScope->exportedTypeBindings.find(name);
                    if (current == workspace.frontend.globals.globalScope->exportedTypeBindings.end())
                        continue;
                    const auto original = baseTypeBindings.find(name);
                    if (original == baseTypeBindings.end())
                        workspace.frontend.globals.globalScope->exportedTypeBindings.erase(current);
                    else if (original->second.type != current->second.type)
                        current->second = original->second;
                }
            }
            for (size_t index = 0; index < platformScopes.size(); ++index)
            {
                platformScopes[index]->bindings = std::move(platformSnapshots[index].bindings);
                platformScopes[index]->exportedTypeBindings = std::move(platformSnapshots[index].exportedTypeBindings);
            }
        }
        else if (result.configuredDefinitionFilesLoaded > 0 && configuration.conflictWinner == PlatformDefinitionConflictWinner::DefinitionFiles)
        {
            const DefinitionScopeSnapshot configuredSnapshot = snapshot(workspace.frontend.globals.globalScope);
            for (const Luau::ScopePtr& scope : platformScopes)
            {
                for (const auto& [name, binding] : configuredSnapshot.bindings)
                {
                    const auto original = baseBindings.find(name);
                    const auto current = workspace.frontend.globals.globalScope->bindings.find(name);
                    if (current != workspace.frontend.globals.globalScope->bindings.end() &&
                        (original == baseBindings.end() || original->second.typeId != current->second.typeId))
                        scope->bindings[name] = binding;
                }
                for (const auto& [name, binding] : configuredSnapshot.exportedTypeBindings)
                {
                    const auto original = baseTypeBindings.find(name);
                    const auto current = workspace.frontend.globals.globalScope->exportedTypeBindings.find(name);
                    if (current != workspace.frontend.globals.globalScope->exportedTypeBindings.end() &&
                        (original == baseTypeBindings.end() || original->second.type != current->second.type))
                        scope->exportedTypeBindings[name] = binding;
                }
            }
        }
    }

    if (!loadedEveryPlatformEnvironment)
        platformActive = false;
    if (configuration.globalPolicy == PlatformGlobalDefinitionsPolicy::PreferPlatform && !platformActive)
        loadConfiguredDefinitions();

    result.platformDefinitionsActive = platformActive;
    return result;
}
