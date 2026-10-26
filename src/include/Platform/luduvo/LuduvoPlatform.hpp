#pragma once
#include "Platform/LSPPlatform.hpp"
#include "LuduvoBindings.hpp"
#include "diagnostics/LuduvoDiagnostics.hpp"
#include "LuduvoResourceCatalog.hpp"
#include "context/LuduvoContextProvider.hpp"

class LuduvoPlatform : public LSPPlatform
{
    ClientLuduvoConfiguration configuration;
    LuduvoDiagnosticSettings diagnosticSettings;
    LuduvoResourceCatalog resourceCatalog;
    LuduvoBindingRegistry bindings;
    std::unordered_set<std::string> reportedConfigurationWarnings;
    std::shared_ptr<luduvo::ContextProvider> contextProvider;
    std::shared_ptr<luduvo::ContextHelperSession> contextHelperSession;
    std::shared_ptr<luduvo::ContextNotificationSubscription> contextNotificationSubscription;
    std::unordered_set<std::string> reportedContextStates;

    std::vector<LuduvoResolvedResourceBinding> resolveResourceBindings(
        const std::optional<std::string>& documentationSymbol, Luau::TypeId callableType);
    void reportConfigurationWarnings(std::string_view area, const std::vector<std::string>& warnings);
    void configureContext();
    void notifyContextState(const lsp::DocumentUri& uri, bool force);

public:
    LuduvoPlatform(const ClientLuduvoConfiguration& configuration, WorkspaceFileResolver* fileResolver, WorkspaceFolder* workspaceFolder);

    const char* getBuiltinDocumentation() const override;
    std::vector<PlatformDefinitionEnvironment> getDefinitionEnvironments() const override;
    PlatformDefinitionConfiguration getDefinitionConfiguration() const override;
    std::vector<PlatformRequirement> getRequirements(bool platformDefinitionsActive) const override;
    void reportDefinitionLoadWarnings(Client* client, const std::vector<std::string>& warnings) const override;
    std::optional<std::string> getEnvironmentForModule(const Luau::ModuleName& moduleName) const override;
    void setupWithConfiguration(const ClientConfiguration& config) override;
    bool isLintIgnored(const Luau::LintWarning& lint) const override;
    bool diagnosticsRequireFullTypeGraphs() const override;
    std::vector<PlatformDiagnostic> collectDiagnostics(const PlatformDiagnosticContext& context) override;
    std::vector<PlatformLintRuleCompletion> getLintRuleCompletions() const override;
    void handleCompletion(const TextDocument& textDocument, const Luau::SourceModule& sourceModule, const Luau::ModulePtr& typedModule,
        const std::vector<Luau::AstNode*>& ancestry, Luau::Position position, std::vector<lsp::CompletionItem>& items) override;
    std::optional<lsp::Hover> handleTypeHover(const PlatformHoverContext& context) override;
    std::optional<lsp::MarkupContent> handleCompletionDocumentation(const PlatformCompletionDocumentationContext& context) override;
    std::optional<lsp::MarkupContent> handleSignatureHelpDocumentation(const PlatformSignatureHelpDocumentationContext& context) override;
    void handleSignatureHelp(const PlatformSignatureHelpContext& context) override;
    lsp::DocumentColorResult documentColor(const PlatformDocumentColorContext& context) override;
    lsp::ColorPresentationResult colorPresentation(const PlatformColorPresentationContext& context) override;
    void onDidOpenTextDocument(const lsp::DocumentUri& uri) override;
    void handleCodeAction(const lsp::CodeActionParams& params, std::vector<lsp::CodeAction>& items) override;
    void handleUnknownSymbolFix(const UnknownSymbolFixContext& ctx, const Luau::UnknownSymbol& unknownSymbol,
        const std::optional<lsp::Diagnostic>& diagnostic, std::vector<lsp::CodeAction>& result) override;
    std::optional<json> handleWorkspaceCommand(std::string_view command, const std::vector<json>& arguments) override;
};
