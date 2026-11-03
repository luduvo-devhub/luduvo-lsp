#pragma once
#include "Platform/luduvo/schema/LuduvoSchemaIndex.hpp"
#include "Luau/TypeArena.h"
#include "Luau/TypeFunction.h"

namespace luduvo
{
struct SchemaTypeMetadata : Luau::ClassUserData
{
    std::shared_ptr<const ProjectSchemas> project;
    std::set<std::string> names;
    std::string script;
    std::string field;
    std::string resolution;
};

// A generation owns all nominal types and its reducer for as long as checked
// modules may reference them. Persisted records never contain these pointers.
class SchemaTypes
{
    Luau::TypeArena arena;
    std::shared_ptr<const ProjectSchemas> project;
    Luau::ScopePtr environment;
    Luau::NotNull<Luau::BuiltinTypes> builtins;
    Luau::TypeFunction lookup;
    Luau::TypeFunction handleLookup;
    Luau::TypeFunction keyLookup;
    std::map<std::string, Luau::TypeId> tables;
    std::map<Luau::TypeId, std::shared_ptr<SchemaTypeMetadata>> handleMetadata;
    std::map<std::string, Luau::TypeId> handles;
    Luau::TypeId valueType(const std::string& name);
    Luau::TypeId combinedTable(const std::set<std::string>& names);
    Luau::TypeId handleTable(const std::string& script);

public:
    SchemaTypes(std::shared_ptr<const ProjectSchemas> project, Luau::ScopePtr environment, Luau::NotNull<Luau::BuiltinTypes> builtins);
    void bind(const Luau::ScopePtr& scope, const std::string& script);
    static std::shared_ptr<const SchemaTypeMetadata> metadata(Luau::TypeId type);
    std::shared_ptr<const SchemaTypeMetadata> findMetadata(Luau::TypeId type) const;
    static std::string internalAlias(std::string_view helper);
};

std::string renderSchemaDebug(const SchemaTypeMetadata& metadata);
} // namespace luduvo
