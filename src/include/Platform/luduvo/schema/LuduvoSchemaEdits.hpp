#pragma once
#include "Platform/luduvo/schema/LuduvoSchemaIndex.hpp"
#include "LSP/TextDocument.hpp"
#include "Protocol/Completion.hpp"
#include "Luau/Module.h"

namespace luduvo
{
struct ExportedSchemaFeature
{
    std::string helper;
    std::string preferredAlias;
    std::string family;
    std::string resourceGroup;
    std::string broadType;
    bool genericAlias = false;
    std::vector<std::string> contextArguments;
};
const std::vector<ExportedSchemaFeature>& exportedSchemaFeatures();

class SchemaTypeImports
{
    const Luau::SourceModule& source;
    const TextDocument& document;
    std::string projectDirectory;
    std::string exportPath;
    std::map<const Luau::AstLocal*, std::string> imports;
    std::vector<const Luau::AstStatLocal*> importDeclarations;
    std::map<std::string, const ExportedSchemaFeature*> aliases;
    std::set<std::string> names;
    bool generatedDestination = true;

public:
    SchemaTypeImports(
        const Luau::SourceModule& source, const TextDocument& document, const std::string& projectDirectory, const std::string& projectId = {});
    bool canImport() const
    {
        return generatedDestination;
    }
    const ExportedSchemaFeature* feature(const Luau::AstTypeReference& reference) const;
    lsp::CompletionItem completion(const ExportedSchemaFeature& feature, const std::string& script, bool snippets = false) const;
    std::vector<lsp::TextEdit> remove(const std::set<std::string>& families, std::vector<std::string>& warnings) const;
};
} // namespace luduvo
