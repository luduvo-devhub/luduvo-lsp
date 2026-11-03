#include "Platform/luduvo/diagnostics/LuduvoCustomDiagnostics.hpp"

#include "Luau/Ast.h"
#include "Platform/luduvo/schema/LuduvoSchemaTypes.hpp"

#include <algorithm>

namespace luduvo::diagnostics
{
namespace
{
bool missingEventColumn(Luau::TypeId type, const std::string& name, std::vector<Luau::TypeId> visited = {})
{
    type = Luau::follow(type);
    if (std::find(visited.begin(), visited.end(), type) != visited.end())
        return false;
    visited.push_back(type);
    if (const auto options = Luau::get<Luau::UnionType>(type))
        return !options->options.empty() && std::all_of(options->options.begin(), options->options.end(),
                                                [&](auto option)
                                                {
                                                    return missingEventColumn(option, name, visited);
                                                });
    if (const auto parts = Luau::get<Luau::IntersectionType>(type))
        return !parts->parts.empty() && std::all_of(parts->parts.begin(), parts->parts.end(),
                                            [&](auto part)
                                            {
                                                return missingEventColumn(part, name, visited);
                                            });
    const auto external = Luau::get<Luau::ExternType>(type);
    if (!external || external->name != "EventTable" || Luau::lookupExternTypeProp(external, name))
        return false;
    const auto metadata = luduvo::SchemaTypes::metadata(type);
    return metadata && luduvo::hasCompleteEventSchema(*metadata->project, metadata->names);
}

struct EventColumnVisitor : Luau::AstVisitor
{
    const LuduvoDiagnosticContext& context;
    DiagnosticReporter& reporter;
    EventColumnVisitor(const LuduvoDiagnosticContext& context, DiagnosticReporter& reporter)
        : context(context)
        , reporter(reporter)
    {
    }

    void check(const Luau::AstExpr* receiver, const std::string& name, const Luau::Location& access, const Luau::Location& location)
    {
        const auto type = context.expressionType(receiver);
        if (type && !context.hasTypeErrorAt(access) && missingEventColumn(*type, name))
            reporter.report(location, "Event table has no registered column '" + name + "'.");
    }
    bool visit(Luau::AstExprIndexName* expression) override
    {
        check(expression->expr, expression->index.value, expression->location, expression->indexLocation);
        return true;
    }
    bool visit(Luau::AstExprIndexExpr* expression) override
    {
        if (const auto key = expression->index->as<Luau::AstExprConstantString>())
            check(expression->expr, std::string(key->value.data, key->value.size), expression->location, key->location);
        return true;
    }
};

bool readOnlyEventType(Luau::TypeId type, std::vector<Luau::TypeId> visited = {})
{
    type = Luau::follow(type);
    if (std::find(visited.begin(), visited.end(), type) != visited.end())
        return false;
    visited.push_back(type);
    if (const auto options = Luau::get<Luau::UnionType>(type))
        return !options->options.empty() && std::all_of(options->options.begin(), options->options.end(),
                                                [&](auto option)
                                                {
                                                    return readOnlyEventType(option, visited);
                                                });
    if (const auto external = Luau::get<Luau::ExternType>(type))
        return external->name == "EventTable" || external->name == "EventColumn" ||
               (external->parent && readOnlyEventType(*external->parent, visited));
    return false;
}
struct EventWriteVisitor : Luau::AstVisitor
{
    const LuduvoDiagnosticContext& context;
    DiagnosticReporter& reporter;
    EventWriteVisitor(const LuduvoDiagnosticContext& context, DiagnosticReporter& reporter)
        : context(context)
        , reporter(reporter)
    {
    }
    void check(const Luau::AstExpr* target)
    {
        const Luau::AstExpr* receiver = nullptr;
        if (const auto member = target->as<Luau::AstExprIndexName>())
            receiver = member->expr;
        else if (const auto index = target->as<Luau::AstExprIndexExpr>())
            receiver = index->expr;
        if (receiver)
            if (const auto type = context.expressionType(receiver); type && readOnlyEventType(*type) && !context.hasTypeErrorAt(target->location))
                reporter.report(target->location, "EventTable members and EventColumn rows are read-only.");
    }
    bool visit(Luau::AstStatAssign* statement) override
    {
        for (const auto target : statement->vars)
            check(target);
        return true;
    }
    bool visit(Luau::AstStatCompoundAssign* statement) override
    {
        check(statement->var);
        return true;
    }
};

struct GlobalSelfVisitor : Luau::AstVisitor
{
    DiagnosticReporter& reporter;

    explicit GlobalSelfVisitor(DiagnosticReporter& reporter)
        : reporter(reporter)
    {
    }

    bool visit(Luau::AstExprGlobal* expression) override
    {
        if (std::string_view(expression->name.value) == "self")
            reporter.report(expression->location, "'self' is nil in scripts inside an autorun directory.");
        return false;
    }

    bool visit(Luau::AstStatAssign* statement) override
    {
        for (Luau::AstExpr* variable : statement->vars)
        {
            const auto global = variable->as<Luau::AstExprGlobal>();
            if (!global || std::string_view(global->name.value) != "self")
                variable->visit(this);
        }
        for (Luau::AstExpr* value : statement->values)
            value->visit(this);
        return false;
    }
};
} // namespace

void analyzeAutorunSelfAccess(const LuduvoDiagnosticContext& context, DiagnosticReporter& reporter)
{
    if (!context.isAutorunScript())
        return;

    GlobalSelfVisitor visitor(reporter);
    context.sourceModule().root->visit(&visitor);
}
void analyzeEventTableWrite(const LuduvoDiagnosticContext& context, DiagnosticReporter& reporter)
{
    EventWriteVisitor visitor(context, reporter);
    context.sourceModule().root->visit(&visitor);
}
void analyzeUnknownEventTableColumn(const LuduvoDiagnosticContext& context, DiagnosticReporter& reporter)
{
    EventColumnVisitor visitor(context, reporter);
    context.sourceModule().root->visit(&visitor);
}
} // namespace luduvo::diagnostics
