#pragma once

#include "Platform/luduvo/context/LuduvoContextStore.hpp"
#include "Luau/Ast.h"
#include <functional>
#include <map>
#include <set>

namespace luduvo
{
constexpr int kSchemaIndexFormatVersion = 4;
struct SchemaField
{
    std::string name;
    std::string type = "any";
    std::string expression;
    unsigned line = 0;
    unsigned column = 0;
    bool optional = false;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SchemaField, name, type, expression, line, column, optional)

struct EventSchema
{
    std::string name;
    std::string direction;
    unsigned line = 0;
    std::vector<SchemaField> fields;
    bool columnsComplete = false;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(EventSchema, name, direction, line, fields, columnsComplete)

struct ScriptSchema
{
    std::string path;
    std::string fingerprint;
    int64_t updatedAt = 0;
    bool unsaved = false;
    bool builtin = false;
    std::vector<EventSchema> events;
    std::vector<SchemaField> handles;
    std::vector<std::string> imports;
    std::vector<std::string> warnings;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ScriptSchema, path, fingerprint, updatedAt, unsaved, builtin, events, handles, imports, warnings)

struct SchemaAttachment
{
    uint64_t nodeId = 0;
    uint64_t instanceId = 0;
    std::string path;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SchemaAttachment, nodeId, instanceId, path)

struct ProjectSchemas
{
    int formatVersion = kSchemaIndexFormatVersion;
    std::string projectId;
    std::string projectName;
    std::string directory;
    int64_t updatedAt = 0;
    int64_t capturedAt = 0;
    WorldDumpSource inputSource;
    bool fresh = false;
    std::map<std::string, ScriptSchema> scripts;
    std::map<std::string, std::vector<SchemaAttachment>> attachments;
    std::string exportPath;
    std::string exportState = "not configured";
    int64_t exportedAt = 0;
    std::string exportError;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ProjectSchemas, formatVersion, projectId, projectName, directory, updatedAt, capturedAt, inputSource,
    fresh, scripts, attachments, exportPath, exportState, exportedAt, exportError)

struct FieldObservation
{
    SchemaField field;
    std::string script;
    bool unsaved = false;
};

// Shared lexical resolution for extraction and analysis overlays. Mutable aliases
// are deliberately excluded; type checking remains responsible for their behavior.
class SchemaSyntax
{
    std::map<const Luau::AstLocal*, const Luau::AstExpr*> aliases;
    std::set<const Luau::AstLocal*> assigned;

public:
    explicit SchemaSyntax(Luau::AstStatBlock* root);
    const Luau::AstExpr* value(const Luau::AstExpr* expression) const;
    std::optional<std::string> symbol(const Luau::AstExpr* expression) const;
};

std::string schemaFingerprint(std::string_view source);
ScriptSchema extractScriptSchema(const std::string& path, std::string_view source, bool unsaved = false);
std::set<std::string> eventSchemaNames(const ProjectSchemas& project);
bool hasCompleteEventSchema(const ProjectSchemas& project, const std::set<std::string>& names);
std::vector<FieldObservation> eventSchemaFields(const ProjectSchemas& project, const std::set<std::string>& names);
std::vector<FieldObservation> handleSchemaFields(const ProjectSchemas& project, const std::string& script);
std::string projectTypesPath(const std::string& projectDirectory);
bool importsProjectTypes(const ScriptSchema& script, const std::string& exportPath);
std::string schemaIndexPath(const std::string& stateDirectory, const std::string& projectId);
std::optional<ProjectSchemas> loadProjectSchemas(const std::string& stateDirectory, const std::string& projectId);
using SchemaSourceReader = std::function<std::optional<std::string>(const std::string&)>;
ProjectSchemas collectProjectSchemas(const ContextProject& project, const std::optional<WorldDumpSnapshot>& snapshot,
    const ProjectSchemas* previous = nullptr, const SchemaSourceReader& reader = {}, const std::string& coreDirectory = {});
bool publishProjectSchemas(const std::string& stateDirectory, const ProjectSchemas& project, std::string* error = nullptr);
int64_t schemaTimeNow();
} // namespace luduvo
