#pragma once
#include "Platform/luduvo/schema/LuduvoSchemaIndex.hpp"
#include "LSP/ClientConfiguration.hpp"

namespace luduvo
{
std::string quoteSchemaString(std::string_view value);
bool isGeneratedProjectTypes(std::string_view source, std::string_view projectId = {});
std::string generateProjectTypes(const ProjectSchemas& project);
void updateSchemaDatabase(const ContextStore& store, const ClientLuduvoConfiguration& configuration);
void writeSchemaExportReference(const std::string& stateDirectory, const std::string& workspace, const std::string& projectId, bool active);
} // namespace luduvo
