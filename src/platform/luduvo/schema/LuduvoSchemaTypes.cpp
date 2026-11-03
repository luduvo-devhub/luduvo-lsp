#include "Platform/luduvo/schema/LuduvoSchemaTypes.hpp"
#include "Luau/BuiltinTypeFunctions.h"
#include "Luau/TypePack.h"
#include "LSP/Uri.hpp"
#include "Platform/luduvo/schema/LuduvoSchemaEdits.hpp"
#include <filesystem>
#include <algorithm>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace luduvo
{
namespace
{
bool literalNames(Luau::TypeId type, std::set<std::string>& names)
{
    type = Luau::follow(type);
    if (const auto singleton = Luau::get<Luau::SingletonType>(type))
        if (const auto string = Luau::get<Luau::StringSingleton>(singleton))
        {
            names.insert(string->value);
            return true;
        }
    if (const auto options = Luau::get<Luau::UnionType>(type))
    {
        for (auto option : options->options)
            if (!literalNames(option, names))
                return false;
        return true;
    }
    if (const auto intersection = Luau::get<Luau::IntersectionType>(type))
    {
        bool found = false;
        for (auto part : intersection->parts)
        {
            part = Luau::follow(part);
            if (const auto primitive = Luau::get<Luau::PrimitiveType>(part); primitive && primitive->type == Luau::PrimitiveType::String)
                continue;
            if (!literalNames(part, names))
                return false;
            found = true;
        }
        return found;
    }
    return false;
}
std::string timestamp(int64_t value)
{
    return formatContextTimestamp(value);
}
std::string fileLink(const std::string& path, std::optional<unsigned> line = std::nullopt)
{
    if (path.empty())
        return "not available";
    std::string label;
    for (char character : path)
    {
        if (character == '\n' || character == '\r')
        {
            label += character == '\n' ? "\\n" : "\\r";
            continue;
        }
        if (std::string_view("\\[]`*_<>").find(character) != std::string_view::npos)
            label += '\\';
        label += character;
    }
    if (line)
        label += ":" + std::to_string(*line);
    return "[" + label + "](<" + Uri::file(path).toString() + (line ? "#L" + std::to_string(*line) : "") + ">)";
}
} // namespace
SchemaTypes::SchemaTypes(std::shared_ptr<const ProjectSchemas> project, Luau::ScopePtr environment, Luau::NotNull<Luau::BuiltinTypes> builtins)
    : project(std::move(project))
    , environment(std::move(environment))
    , builtins(builtins)
{
    lookup.name = "EventTableFor";
    lookup.reducer = [this](Luau::TypeId, const std::vector<Luau::TypeId>& arguments, const std::vector<Luau::TypePackId>&,
                         Luau::NotNull<Luau::TypeFunctionContext> context) -> Luau::TypeFunctionReductionResult<Luau::TypeId>
    {
        const auto base = this->environment->lookupType("EventTable");
        if (!base)
            return {this->builtins->anyType, Luau::Reduction::MaybeOk, {}, {}};
        if (arguments.empty())
            return {base->type, Luau::Reduction::MaybeOk, {}, {}};
        const auto name = Luau::follow(arguments[0]);
        if (Luau::isPending(name, context->solver))
            return {std::nullopt, Luau::Reduction::MaybeOk, {name}, {}};
        std::set<std::string> names;
        const auto known = eventSchemaNames(*this->project);
        if (!literalNames(name, names) || names.empty() ||
            std::any_of(names.begin(), names.end(),
                [&](const auto& item)
                {
                    return !known.count(item);
                }))
            return {base->type, Luau::Reduction::MaybeOk, {}, {}};
        return {combinedTable(names), Luau::Reduction::MaybeOk, {}, {}};
    };
    keyLookup.name = "EventTableKeys";
    keyLookup.reducer = [this](Luau::TypeId type, const std::vector<Luau::TypeId>& arguments, const std::vector<Luau::TypePackId>& packs,
                            Luau::NotNull<Luau::TypeFunctionContext> context) -> Luau::TypeFunctionReductionResult<Luau::TypeId>
    {
        auto result = lookup.reducer(type, arguments, packs, context);
        if (!result.result)
            return result;
        const auto data = metadata(*result.result);
        const auto table = Luau::get<Luau::ExternType>(Luau::follow(*result.result));
        result.result = data && hasCompleteEventSchema(*data->project, data->names) && table && table->indexer ? table->indexer->indexType
                                                                                                               : this->builtins->stringType;
        return result;
    };
    handleLookup.name = "HandlesFor";
    handleLookup.reducer = [this](Luau::TypeId, const std::vector<Luau::TypeId>& arguments, const std::vector<Luau::TypePackId>&,
                               Luau::NotNull<Luau::TypeFunctionContext> context) -> Luau::TypeFunctionReductionResult<Luau::TypeId>
    {
        const auto base = this->environment->lookup(Luau::AstName("handles"));
        if (!base || arguments.empty())
            return {base ? *base : this->builtins->anyType, Luau::Reduction::MaybeOk, {}, {}};
        const auto argument = Luau::follow(arguments[0]);
        if (Luau::isPending(argument, context->solver))
            return {std::nullopt, Luau::Reduction::MaybeOk, {argument}, {}};
        std::set<std::string> scripts;
        if (!literalNames(argument, scripts) || scripts.size() != 1)
            return {*base, Luau::Reduction::MaybeOk, {}, {}};
        const auto path = normalizeContextPath((std::filesystem::u8path(this->project->directory) / *scripts.begin()).generic_u8string());
        return {handleTable(path), Luau::Reduction::MaybeOk, {}, {}};
    };
}
std::string SchemaTypes::internalAlias(std::string_view helper)
{
    // Not lexable in source, so user aliases cannot collide with this binding.
    return "@luduvo/schema/" + std::string(helper);
}
Luau::TypeId SchemaTypes::valueType(const std::string& name)
{
    if (name == "string")
        return builtins->stringType;
    if (name == "number")
        return builtins->numberType;
    if (name == "boolean")
        return builtins->booleanType;
    if (const auto type = environment->lookupType(name))
        return type->type;
    return builtins->anyType;
}
Luau::TypeId SchemaTypes::combinedTable(const std::set<std::string>& names)
{
    std::string key;
    for (const auto& name : names)
        key += std::to_string(name.size()) + ":" + name;
    if (const auto cached = tables.find(key); cached != tables.end())
        return cached->second;
    const auto eventBase = environment->lookupType("EventTable");
    const auto columnBase = environment->lookupType("EventColumn");
    if (!eventBase || !columnBase)
        return builtins->anyType;
    const auto eventClass = Luau::get<Luau::ExternType>(Luau::follow(eventBase->type));
    const auto columnClass = Luau::get<Luau::ExternType>(Luau::follow(columnBase->type));
    if (!eventClass || !columnClass)
        return eventBase->type;
    auto table = *eventClass;
    table.parent = eventBase->type;
    table.props.clear();
    auto metadata = std::make_shared<SchemaTypeMetadata>();
    metadata->project = project;
    metadata->names = names;
    metadata->resolution = names.size() == 1 ? "named event-table schema" : "combined event-table schemas";
    table.userData = metadata;
    std::map<std::string, std::vector<Luau::TypeId>> values;
    std::map<std::string, size_t> present;
    for (const auto& name : names)
    {
        std::set<std::string> fields;
        for (const auto& observation : eventSchemaFields(*project, {name}))
        {
            auto value = valueType(observation.field.type);
            auto& types = values[observation.field.name];
            if (std::find(types.begin(), types.end(), value) == types.end())
                types.push_back(value);
            fields.insert(observation.field.name);
        }
        for (const auto& field : fields)
            ++present[field];
    }
    const auto unionOf = [&](const std::vector<Luau::TypeId>& choices) -> Luau::TypeId
    {
        if (choices.empty())
            return builtins->neverType;
        return choices.size() == 1 ? choices.front() : arena.addType(Luau::UnionType{choices});
    };
    const auto makeColumn = [&](Luau::TypeId value, const std::string& field)
    {
        auto column = *columnClass;
        column.parent = columnBase->type;
        column.props.clear();
        auto fieldMetadata = std::make_shared<SchemaTypeMetadata>(*metadata);
        fieldMetadata->field = field;
        if (field.empty())
            fieldMetadata->resolution = "dynamic event-table column";
        column.userData = fieldMetadata;
        column.indexer = Luau::TableIndexer{builtins->numberType, value};
        const auto type = arena.addType(std::move(column));
        Luau::asMutable(type)->documentationSymbol = Luau::follow(columnBase->type)->documentationSymbol;
        return type;
    };
    std::vector<Luau::TypeId> columnNames;
    std::vector<Luau::TypeId> columnValues;
    bool optionalColumn = false;
    for (const auto& entry : values)
    {
        auto type = makeColumn(unionOf(entry.second), entry.first);
        if (present.at(entry.first) < names.size())
        {
            type = arena.addType(Luau::UnionType{{type, builtins->nilType}});
            optionalColumn = true;
        }
        table.props[entry.first] = Luau::Property::readonly(type);
        columnNames.push_back(arena.addType(Luau::SingletonType{Luau::StringSingleton{entry.first}}));
        for (auto value : entry.second)
            if (std::find(columnValues.begin(), columnValues.end(), value) == columnValues.end())
                columnValues.push_back(value);
    }
    if (hasCompleteEventSchema(*project, names))
    {
        auto fallback = makeColumn(unionOf(columnValues), "");
        if (optionalColumn)
            fallback = arena.addType(Luau::UnionType{{fallback, builtins->nilType}});
        table.indexer = Luau::TableIndexer{unionOf(columnNames), fallback};
    }
    else
        metadata->resolution += "; column list is not proven complete; open fallback retained";
    auto result = arena.addType(std::move(table));
    Luau::asMutable(result)->documentationSymbol = Luau::follow(eventBase->type)->documentationSymbol;
    tables[key] = result;
    return result;
}
void SchemaTypes::bind(const Luau::ScopePtr& scope, const std::string& script)
{
    const auto specialize = [&](const char* name, Luau::TypeId type)
    {
        Luau::Binding binding{type};
        if (const auto original = scope->lookupEx(Luau::AstName(name)))
        {
            binding = *original->first;
            if (type != binding.typeId)
                Luau::asMutable(type)->documentationSymbol = Luau::follow(binding.typeId)->documentationSymbol;
            binding.typeId = type;
        }
        scope->bindings[Luau::AstName(name)] = std::move(binding);
    };
    for (const auto& feature : exportedSchemaFeatures())
    {
        const auto parameter = arena.addType(Luau::GenericType{"Name", Luau::Polarity::Negative});
        const auto function = feature.family == "handle" ? &handleLookup : feature.helper == "EventTableKeys" ? &keyLookup : &lookup;
        scope->privateTypeBindings[internalAlias(feature.helper)] =
            Luau::TypeFun{{{parameter, std::nullopt}}, arena.addType(Luau::TypeFunctionInstanceType{*function, {parameter}})};
    }
    scope->privateTypeBindings["EventTableKeys"] = scope->privateTypeBindings.at(internalAlias("EventTableKeys"));
    const auto binding = scope->lookup(Luau::AstName("game"));
    if (!binding)
        return;
    const auto original = Luau::getTableType(Luau::follow(*binding));
    if (!original)
        return;
    auto game = *original;
    const auto property = game.props.find("EventTable");
    if (property == game.props.end() || !property->second.readTy)
        return;
    const auto intersection = Luau::get<Luau::IntersectionType>(Luau::follow(*property->second.readTy));
    if (!intersection)
        return;
    auto callable = *intersection;
    for (auto& part : callable.parts)
    {
        const auto originalFunction = Luau::get<Luau::FunctionType>(Luau::follow(part));
        if (!originalFunction)
            continue;
        auto function = *originalFunction;
        const auto name = arena.addType(Luau::GenericType{"Name", Luau::Polarity::Negative});
        function.generics.push_back(name);
        auto [parameters, tail] = Luau::flatten(function.argTypes);
        if (parameters.empty())
            continue;
        parameters[0] = arena.addType(Luau::IntersectionType{{name, builtins->stringType}});
        function.argTypes = arena.addTypePack(std::move(parameters), tail);
        function.retTypes = arena.addTypePack({arena.addType(Luau::TypeFunctionInstanceType{lookup, {name}})});
        const auto specialized = arena.addType(std::move(function));
        Luau::asMutable(specialized)->documentationSymbol = Luau::follow(part)->documentationSymbol;
        part = specialized;
    }
    const auto specialized = arena.addType(std::move(callable));
    Luau::asMutable(specialized)->documentationSymbol = Luau::follow(*property->second.readTy)->documentationSymbol;
    game.props["EventTable"].readTy = specialized;
    specialize("game", arena.addType(std::move(game)));
    if (scope->lookup(Luau::AstName("handles")))
        specialize("handles", handleTable(script));
}
Luau::TypeId SchemaTypes::handleTable(const std::string& script)
{
    if (const auto found = handles.find(script); found != handles.end())
        return found->second;
    const auto handlesBinding = environment->lookup(Luau::AstName("handles"));
    const auto instance = environment->lookupType("Instance");
    if (handlesBinding && instance && project->fresh)
    {
        const auto originalHandles = Luau::getTableType(Luau::follow(*handlesBinding));
        if (originalHandles)
        {
            auto handles = *originalHandles;
            const auto fields = handleSchemaFields(*project, script);
            for (const auto& observation : fields)
                handles.props[observation.field.name] = Luau::Property::readonly(instance->type);
            const auto type = arena.addType(std::move(handles));
            auto metadata = std::make_shared<SchemaTypeMetadata>();
            metadata->project = project;
            metadata->script = script;
            metadata->resolution = fields.empty() ? "no observed handle names in the current context; generic read-only handles"
                                                  : "handle reads in scripts attached to the same instance";
            handleMetadata[type] = std::move(metadata);
            this->handles[script] = type;
            return type;
        }
    }
    return handlesBinding ? *handlesBinding : builtins->anyType;
}
std::shared_ptr<const SchemaTypeMetadata> SchemaTypes::metadata(Luau::TypeId type)
{
    type = Luau::follow(type);
    if (const auto external = Luau::get<Luau::ExternType>(type))
        return std::dynamic_pointer_cast<SchemaTypeMetadata>(external->userData);
    if (const auto optional = Luau::get<Luau::UnionType>(type))
        for (auto part : optional->options)
            if (auto data = metadata(part))
                return data;
    return nullptr;
}
std::shared_ptr<const SchemaTypeMetadata> SchemaTypes::findMetadata(Luau::TypeId type) const
{
    if (auto result = metadata(type))
        return result;
    const auto found = handleMetadata.find(Luau::follow(type));
    return found == handleMetadata.end() ? nullptr : found->second;
}
std::string renderSchemaDebug(const SchemaTypeMetadata& metadata)
{
    const auto& project = *metadata.project;
    std::string result = "\n\n___\n\n**Debug information**\n\nProject: " + project.projectName + " [" + project.projectId +
                         "]  \nResolution: " + metadata.resolution + "\n";
    const auto observations = metadata.names.empty() ? handleSchemaFields(project, metadata.script) : eventSchemaFields(project, metadata.names);
    if (!metadata.script.empty())
        if (const auto attachments = project.attachments.find(metadata.script); attachments != project.attachments.end())
            for (const auto& instance : attachments->second)
                result += "\nAttachment: " + instance.path + " [node " + std::to_string(instance.nodeId) + "] [instance " +
                          std::to_string(instance.instanceId) + "]\n";
    size_t count = 0;
    for (const auto& observation : observations)
    {
        if (!metadata.field.empty() && observation.field.name != metadata.field)
            continue;
        if (++count > 6)
        {
            result += "\nAdditional declarations are available in context inspection.\n";
            break;
        }
        result += "\n" + fileLink(observation.script, observation.field.line + 1) + (observation.unsaved ? " [unsaved]" : " [saved]");
        std::string expression = observation.field.expression;
        // Source text cannot terminate the Markdown block containing it.
        size_t longest = 0, run = 0;
        for (char c : expression)
        {
            run = c == '`' ? run + 1 : 0;
            longest = std::max(longest, run);
        }
        const std::string fence(std::max<size_t>(3, longest + 1), '`');
        result += "\n\n" + fence + "luau\n" + expression + "\n" + fence + "\n";
    }
    result += "\n**Context**\n\nWorld snapshot: " + timestamp(project.capturedAt) + (project.fresh ? "" : " [stale]") +
              "  \nScript index updated: " + timestamp(project.updatedAt) + "  \nType export: " + project.exportState;
    if (!project.inputSource.kind.empty())
        result += "  \nSource: " + (project.inputSource.kind == "save" ? std::string("saved project") : project.inputSource.kind) +
                  "  \nSource path: " + fileLink(project.inputSource.path) + "  \nSource written: " + timestamp(project.inputSource.modifiedAtUnixMs);
    if (project.exportState != "not configured")
        result += "  \nExport path: " + fileLink(project.exportPath);
    if (project.exportedAt)
        result += "  \nExport updated: " + timestamp(project.exportedAt) + " [saved index]";
    return result;
}
} // namespace luduvo
