#pragma once
#include "Platform/LSPPlatform.hpp"
#include "Platform/LuduvoResourceBindings.hpp"
#include "Platform/LuduvoResourceCatalog.hpp"

class LuduvoPlatform : public LSPPlatform
{
    ClientLuduvoConfiguration configuration;
    LuduvoResourceCatalog resourceCatalog;
    LuduvoResourceBindingRegistry resourceBindings;

public:
    LuduvoPlatform(const ClientLuduvoConfiguration& configuration, WorkspaceFileResolver* fileResolver, WorkspaceFolder* workspaceFolder);

    const char* getBuiltinDocumentation() const override;
    std::vector<PlatformDefinitionEnvironment> getDefinitionEnvironments() const override;
    PlatformDefinitionConfiguration getDefinitionConfiguration() const override;
    std::vector<PlatformRequirement> getRequirements(bool platformDefinitionsActive) const override;
    std::optional<std::string> getEnvironmentForModule(const Luau::ModuleName& moduleName) const override;
    void setupWithConfiguration(const ClientConfiguration& config) override;
    bool isLintIgnored(const Luau::LintWarning& lint) const override;
    void handleCompletion(const TextDocument& textDocument, const Luau::SourceModule& sourceModule, const Luau::ModulePtr& typedModule,
        const std::vector<Luau::AstNode*>& ancestry, Luau::Position position, std::vector<lsp::CompletionItem>& items) override;
    std::optional<lsp::Hover> handleTypeHover(const PlatformHoverContext& context) override;
    std::optional<lsp::MarkupContent> handleCompletionDocumentation(const PlatformCompletionDocumentationContext& context) override;
    std::optional<lsp::MarkupContent> handleSignatureHelpDocumentation(const PlatformSignatureHelpDocumentationContext& context) override;
    void handleSignatureHelp(const PlatformSignatureHelpContext& context) override;
};
