#pragma once
#include "Platform/LSPPlatform.hpp"

class LuduvoPlatform : public LSPPlatform
{
    ClientLuduvoConfiguration configuration;

public:
    LuduvoPlatform(const ClientLuduvoConfiguration& configuration, WorkspaceFileResolver* fileResolver, WorkspaceFolder* workspaceFolder);

    const char* getBuiltinDocumentation() const override;
    std::vector<PlatformDefinitionEnvironment> getDefinitionEnvironments() const override;
    PlatformDefinitionConfiguration getDefinitionConfiguration() const override;
    std::vector<PlatformRequirement> getRequirements(bool platformDefinitionsActive) const override;
    std::optional<std::string> getEnvironmentForModule(const Luau::ModuleName& moduleName) const override;
    void setupWithConfiguration(const ClientConfiguration& config) override;
    bool isLintIgnored(const Luau::LintWarning& lint) const override;
};
