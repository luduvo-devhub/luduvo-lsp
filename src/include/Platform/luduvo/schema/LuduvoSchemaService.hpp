#pragma once
#include "Platform/LSPPlatform.hpp"
#include "Platform/luduvo/schema/LuduvoSchemaTypes.hpp"
#include "Platform/luduvo/context/LuduvoContextProvider.hpp"
#include "Platform/luduvo/LuduvoBindings.hpp"
#include "Platform/luduvo/LuduvoResourceCatalog.hpp"

namespace luduvo
{
class SchemaService
{
    WorkspaceFolder& workspace;
    ClientLuduvoConfiguration configuration;
    std::map<std::string, std::shared_ptr<ProjectSchemas>> projects;
    std::map<std::string, std::string> scriptProjects;
    std::map<std::string, std::shared_ptr<SchemaTypes>> currentTypes;
    std::vector<std::weak_ptr<SchemaTypes>> retainedTypes;
    std::map<Luau::ModuleName, std::vector<std::shared_ptr<SchemaTypes>>> pendingTypes;
    void retainCheckedModules();
    bool synchronizing = false;
    const LuduvoBindingRegistry& bindings;
    const LuduvoResourceCatalog& resources;

public:
    SchemaService(WorkspaceFolder& workspace, ClientLuduvoConfiguration configuration, const LuduvoBindingRegistry& bindings,
        const LuduvoResourceCatalog& resources);
    ~SchemaService();
    void configure(const ClientLuduvoConfiguration& value);
    std::shared_ptr<const ProjectSchemas> synchronize(const Luau::ModuleName& moduleName);
    std::unique_ptr<PlatformAnalysisOverlay> prepare(const Luau::ModuleName& moduleName);
    void bind(const Luau::ModuleName& moduleName, const Luau::ScopePtr& scope);
    void complete(const TextDocument& document, const Luau::SourceModule& source, Luau::Position position,
        const std::vector<Luau::AstNode*>& ancestry, std::vector<lsp::CompletionItem>& items);
    std::string debug(const Luau::ModuleName& moduleName, const Luau::AstExpr* expression, Luau::TypeId type, std::string_view field = {});
    nlohmann::json inspect(const std::string& script);
    void forgetOverlay(const std::string& script);
    lsp::WorkspaceEdit removalEdits(
        const std::string& script, bool wholeProject, const std::set<std::string>& families, std::vector<std::string>& warnings);
};
} // namespace luduvo
