#include "Platform/luduvo/schema/LuduvoSchemaIndex.hpp"
#include "LSP/AtomicFile.hpp"
#include "Luau/Parser.h"
#include "LuauFileUtils.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace luduvo
{
int64_t schemaTimeNow()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

namespace
{
const Luau::AstExpr* unwrap(const Luau::AstExpr* value)
{
    while (value)
    {
        if (const auto group = value->as<Luau::AstExprGroup>())
            value = group->expr;
        else if (const auto assertion = value->as<Luau::AstExprTypeAssertion>())
            value = assertion->expr;
        else
            break;
    }
    return value;
}
struct Aliases : Luau::AstVisitor
{
    std::map<const Luau::AstLocal*, const Luau::AstExpr*>& aliases;
    std::set<const Luau::AstLocal*>& assigned;
    Aliases(decltype(aliases) aliases, decltype(assigned) assigned)
        : aliases(aliases)
        , assigned(assigned)
    {
    }
    bool visit(Luau::AstStatLocal* statement) override
    {
        for (size_t i = 0; i < std::min(statement->vars.size, statement->values.size); ++i)
            aliases[statement->vars.data[i]] = statement->values.data[i];
        return true;
    }
    bool visit(Luau::AstStatAssign* statement) override
    {
        for (auto variable : statement->vars)
            if (auto local = variable->as<Luau::AstExprLocal>())
                assigned.insert(local->local);
        return true;
    }
    bool visit(Luau::AstStatCompoundAssign* statement) override
    {
        if (auto local = statement->var->as<Luau::AstExprLocal>())
            assigned.insert(local->local);
        return true;
    }
};
size_t offset(std::string_view source, Luau::Position position)
{
    size_t start = 0;
    for (unsigned line = 0; line < position.line && start < source.size(); ++line)
    {
        const auto end = source.find('\n', start);
        start = end == std::string::npos ? source.size() : end + 1;
    }
    return std::min(start + position.column, source.size());
}
std::string snippet(std::string_view source, const Luau::Location& location)
{
    const auto start = offset(source, location.begin);
    const auto end = offset(source, location.end);
    auto clipped = std::min(end, start + 2048);
    while (clipped > start && clipped < end && (static_cast<unsigned char>(source[clipped]) & 0xc0) == 0x80)
        --clipped;
    return std::string(source.substr(start, clipped - start)) + (clipped < end ? " … [truncated]" : "");
}
std::optional<std::string> stringValue(const SchemaSyntax& syntax, const Luau::AstExpr* value)
{
    if (const auto string = syntax.value(value)->as<Luau::AstExprConstantString>())
        return std::string(string->value.data, string->value.size);
    return std::nullopt;
}
struct Extractor : Luau::AstVisitor
{
    ScriptSchema& result;
    const SchemaSyntax& syntax;
    std::string_view source;
    Extractor(ScriptSchema& result, const SchemaSyntax& syntax, std::string_view source)
        : result(result)
        , syntax(syntax)
        , source(source)
    {
    }
    void observe(const Luau::AstExpr* receiver, const std::string& name, const Luau::Location& location)
    {
        if (syntax.symbol(receiver) != "handles" || name.empty())
            return;
        SchemaField field;
        field.name = name;
        field.type = "Instance";
        field.expression = snippet(source, location);
        field.line = location.begin.line;
        field.column = location.begin.column;
        result.handles.push_back(std::move(field));
    }
    bool visit(Luau::AstExprIndexName* expression) override
    {
        observe(expression->expr, expression->index.value, expression->location);
        return true;
    }
    bool visit(Luau::AstExprIndexExpr* expression) override
    {
        if (const auto name = stringValue(syntax, expression->index))
            observe(expression->expr, *name, expression->location);
        return true;
    }
    void visitWriteTarget(Luau::AstExpr* target)
    {
        // The target is a write, but its receiver and computed key are reads.
        if (const auto member = target->as<Luau::AstExprIndexName>())
            member->expr->visit(this);
        else if (const auto index = target->as<Luau::AstExprIndexExpr>())
        {
            index->expr->visit(this);
            index->index->visit(this);
        }
    }
    bool visit(Luau::AstStatAssign* statement) override
    {
        for (const auto target : statement->vars)
            visitWriteTarget(target);
        for (const auto value : statement->values)
            value->visit(this);
        return false;
    }
    bool visit(Luau::AstStatCompoundAssign* statement) override
    {
        visitWriteTarget(statement->var);
        statement->value->visit(this);
        return false;
    }
    bool visit(Luau::AstExprCall* call) override
    {
        const auto symbol = syntax.symbol(call->func);
        if (symbol == "require" && call->args.size == 1)
        {
            if (auto path = stringValue(syntax, call->args.data[0]))
                result.imports.push_back(*path);
        }
        if (symbol != "game.EventTable" || call->args.size < 3)
            return true;
        const auto name = stringValue(syntax, call->args.data[0]);
        const auto fields = syntax.value(call->args.data[2])->as<Luau::AstExprTable>();
        if (!name)
            return true;
        EventSchema event;
        event.name = *name;
        event.line = call->location.begin.line;
        event.direction = syntax.symbol(call->args.data[1]).value_or("");
        const auto dot = event.direction.rfind('.');
        if (dot != std::string::npos)
            event.direction.erase(0, dot + 1);
        // An aliased table can be mutated without rebinding its local. Keep its
        // observations, but only inline arrays establish a closed column list.
        event.columnsComplete = unwrap(call->args.data[2])->is<Luau::AstExprTable>();
        if (!fields)
        {
            result.events.push_back(std::move(event));
            return true;
        }
        static const std::map<std::string, std::string> types = {{"String", "string"}, {"F32", "number"}, {"I32", "number"}, {"U8", "number"},
            {"Bool", "boolean"}, {"Vec3", "vector"}, {"Color", "Color3"}, {"Instance", "Instance"}, {"Entity", "Instance"}};
        for (const auto& item : fields->items)
        {
            if (item.kind != Luau::AstExprTable::Item::Kind::List)
            {
                event.columnsComplete = false;
                continue;
            }
            const auto field = syntax.value(item.value)->as<Luau::AstExprTable>();
            if (!field || field->items.size < 2 || field->items.data[0].kind != Luau::AstExprTable::Item::Kind::List ||
                field->items.data[1].kind != Luau::AstExprTable::Item::Kind::List)
            {
                event.columnsComplete = false;
                continue;
            }
            if (!unwrap(item.value)->is<Luau::AstExprTable>())
                event.columnsComplete = false;
            const auto key = stringValue(syntax, field->items.data[0].value);
            if (!key)
            {
                event.columnsComplete = false;
                continue;
            }
            const auto endpoint = syntax.symbol(field->items.data[1].value).value_or("");
            const auto suffix = endpoint.substr(endpoint.rfind('.') == std::string::npos ? 0 : endpoint.rfind('.') + 1);
            SchemaField value;
            value.name = *key;
            if (endpoint.find("game.EventTable.") == 0 && types.count(suffix))
                value.type = types.at(suffix);
            value.expression = snippet(source, field->location);
            value.line = field->location.begin.line;
            value.column = field->location.begin.column;
            event.fields.push_back(std::move(value));
        }
        result.events.push_back(std::move(event));
        return true;
    }
};
} // namespace

SchemaSyntax::SchemaSyntax(Luau::AstStatBlock* root)
{
    Aliases visitor(aliases, assigned);
    if (root)
        root->visit(&visitor);
    for (const auto local : assigned)
        aliases.erase(local);
}
const Luau::AstExpr* SchemaSyntax::value(const Luau::AstExpr* expression) const
{
    std::set<const Luau::AstLocal*> seen;
    expression = unwrap(expression);
    while (expression)
    {
        const auto local = expression->as<Luau::AstExprLocal>();
        if (!local || !seen.insert(local->local).second)
            break;
        const auto found = aliases.find(local->local);
        if (found == aliases.end())
            break;
        expression = unwrap(found->second);
    }
    return expression;
}
std::optional<std::string> SchemaSyntax::symbol(const Luau::AstExpr* expression) const
{
    expression = value(expression);
    if (!expression)
        return std::nullopt;
    if (const auto global = expression->as<Luau::AstExprGlobal>())
        return global->name.value;
    if (const auto member = expression->as<Luau::AstExprIndexName>())
        if (auto parent = symbol(member->expr))
            return *parent + "." + member->index.value;
    if (const auto member = expression->as<Luau::AstExprIndexExpr>())
        if (auto parent = symbol(member->expr))
            if (auto key = stringValue(*this, member->index))
                return *parent + "." + *key;
    return std::nullopt;
}
std::string schemaFingerprint(std::string_view source)
{
    uint64_t value = 14695981039346656037ull;
    for (const unsigned char byte : source)
    {
        value ^= byte;
        value *= 1099511628211ull;
    }
    std::ostringstream result;
    result << std::hex << value;
    return result.str();
}
ScriptSchema extractScriptSchema(const std::string& path, std::string_view source, bool unsaved)
{
    ScriptSchema result;
    result.path = path;
    result.unsaved = unsaved;
    result.fingerprint = schemaFingerprint(source);
    result.updatedAt = schemaTimeNow();
    Luau::Allocator allocator;
    Luau::AstNameTable names(allocator);
    auto parsed = Luau::Parser::parse(source.data(), source.size(), names, allocator);
    for (const auto& error : parsed.errors)
        result.warnings.push_back(error.getMessage());
    if (parsed.root)
    {
        SchemaSyntax syntax(parsed.root);
        Extractor visitor(result, syntax, source);
        parsed.root->visit(&visitor);
    }
    return result;
}
std::set<std::string> eventSchemaNames(const ProjectSchemas& project)
{
    std::set<std::string> result;
    for (const auto& script : project.scripts)
        for (const auto& event : script.second.events)
            result.insert(event.name);
    return result;
}
bool hasCompleteEventSchema(const ProjectSchemas& project, const std::set<std::string>& names)
{
    if (names.empty())
        return false;
    for (const auto& name : names)
    {
        bool found = false;
        for (const auto& script : project.scripts)
            for (const auto& event : script.second.events)
                if (event.name == name)
                {
                    found = true;
                    if (!event.columnsComplete)
                        return false;
                }
        if (!found)
            return false;
    }
    return true;
}
std::vector<FieldObservation> eventSchemaFields(const ProjectSchemas& project, const std::set<std::string>& names)
{
    std::vector<FieldObservation> result;
    for (const auto& script : project.scripts)
        for (const auto& event : script.second.events)
            if (names.count(event.name))
                for (const auto& field : event.fields)
                    result.push_back({field, script.first, script.second.unsaved});
    return result;
}
std::vector<FieldObservation> handleSchemaFields(const ProjectSchemas& project, const std::string& script)
{
    std::set<uint64_t> attached;
    const auto current = project.attachments.find(script);
    if (current != project.attachments.end())
        for (const auto& instance : current->second)
            attached.insert(instance.nodeId);
    std::vector<FieldObservation> result;
    if (attached.empty())
        return result;
    for (const auto& source : project.scripts)
    {
        const auto scope = project.attachments.find(source.first);
        if (scope == project.attachments.end())
            continue;
        if (std::none_of(scope->second.begin(), scope->second.end(),
                [&](const auto& instance)
                {
                    return attached.count(instance.nodeId);
                }))
            continue;
        for (const auto& field : source.second.handles)
            result.push_back({field, source.first, source.second.unsaved});
    }
    return result;
}
std::string projectTypesPath(const std::string& directory)
{
    return (std::filesystem::u8path(directory) / "scripts/types/ProjectTypes.luau").generic_u8string();
}
bool importsProjectTypes(const ScriptSchema& script, const std::string& exportPath)
{
    for (auto import : script.imports)
    {
        const auto parent = std::filesystem::u8path(script.path).parent_path();
        auto target = std::filesystem::u8path(import);
        if (!target.has_extension())
            target += ".luau";
        if (normalizeContextPath((parent / target).generic_u8string()) == normalizeContextPath(exportPath))
            return true;
    }
    return false;
}
std::string schemaIndexPath(const std::string& directory, const std::string& projectId)
{
    return (std::filesystem::u8path(directory) / "schemas" / (schemaFingerprint(projectId) + ".json")).generic_u8string();
}
std::optional<ProjectSchemas> loadProjectSchemas(const std::string& directory, const std::string& projectId)
{
    const auto source = readSharedFile(schemaIndexPath(directory, projectId));
    if (!source)
        return std::nullopt;
    try
    {
        auto result = nlohmann::json::parse(*source).get<ProjectSchemas>();
        if (result.formatVersion == kSchemaIndexFormatVersion && result.projectId == projectId)
            return result;
    }
    catch (const std::exception&)
    {
    }
    return std::nullopt;
}
ProjectSchemas collectProjectSchemas(const ContextProject& project, const std::optional<WorldDumpSnapshot>& snapshot, const ProjectSchemas* previous,
    const SchemaSourceReader& reader, const std::string& coreDirectory)
{
    ProjectSchemas result;
    result.projectId = project.id;
    result.projectName = project.name;
    result.directory = project.directory;
    result.fresh = project.fresh;
    result.capturedAt = snapshot ? snapshot->capturedAtUnixMs : 0;
    result.inputSource = snapshot ? snapshot->inputSource : WorldDumpSource{};
    result.exportPath = projectTypesPath(project.directory);
    if (previous)
    {
        result.exportState = previous->exportState;
        result.exportedAt = previous->exportedAt;
        result.exportError = previous->exportError;
        result.updatedAt = previous->updatedAt;
    }
    std::map<uint64_t, const WorldInstance*> nodes;
    if (snapshot)
        for (const auto& instance : snapshot->instances)
            nodes[instance.nodeId] = &instance;
    const auto instancePath = [&](const WorldInstance& instance)
    {
        std::string path = instance.name;
        std::set<uint64_t> seen{instance.nodeId};
        auto parent = instance.parentNodeId;
        while (nodes.count(parent) && seen.insert(parent).second)
        {
            path = nodes.at(parent)->name + "/" + path;
            parent = nodes.at(parent)->parentNodeId;
        }
        return path;
    };
    std::set<std::string> paths(project.scripts.begin(), project.scripts.end());
    std::set<std::string> coreScripts;
    const auto resolve = [&](const std::string& path)
    {
        if (path.find("core://") == 0)
            return coreDirectory.empty() ? path : normalizeContextPath((std::filesystem::u8path(coreDirectory) / path.substr(7)).generic_u8string());
        auto file = std::filesystem::u8path(path);
        return normalizeContextPath((file.is_absolute() ? file : std::filesystem::u8path(project.directory) / file).generic_u8string());
    };
    if (snapshot)
        for (const auto& instance : snapshot->instances)
            for (const auto& script : instance.scripts)
            {
                const auto path = resolve(script);
                if (script.find("core://") == 0)
                    coreScripts.insert(path);
                paths.insert(path);
                result.attachments[path].push_back({instance.nodeId, instance.instanceId, instancePath(instance)});
            }
    for (const auto& raw : paths)
    {
        const auto path = resolve(raw);
        if (path == normalizeContextPath(result.exportPath) || path.find("core://") == 0)
            continue;
        const auto source = reader ? reader(path) : Luau::FileUtils::readFile(path);
        if (!source)
            continue;
        const auto old = previous ? previous->scripts.find(path) : result.scripts.end();
        if (previous && old != previous->scripts.end() && !old->second.unsaved && old->second.fingerprint == schemaFingerprint(*source))
            result.scripts[path] = old->second;
        else
        {
            result.scripts[path] = extractScriptSchema(path, *source);
            result.updatedAt = schemaTimeNow();
        }
        result.scripts[path].builtin = coreScripts.count(path) != 0;
    }
    if (!result.updatedAt || (previous && result.scripts.size() != previous->scripts.size()))
        result.updatedAt = schemaTimeNow();
    return result;
}
bool publishProjectSchemas(const std::string& directory, const ProjectSchemas& project, std::string* error)
{
    const auto path = schemaIndexPath(directory, project.projectId);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(path).parent_path(), ec);
    if (ec)
    {
        if (error)
            *error = ec.message();
        return false;
    }
    const auto source = nlohmann::json(project).dump(2);
    if (const auto old = readSharedFile(path); old && *old == source)
        return true;
    return writeFileAtomically(path, source, error);
}
} // namespace luduvo
