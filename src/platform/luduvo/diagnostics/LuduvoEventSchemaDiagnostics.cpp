#include "Platform/luduvo/diagnostics/LuduvoCustomDiagnostics.hpp"
#include "Luau/Ast.h"
#include <algorithm>
#include <set>

namespace luduvo::diagnostics
{
namespace
{
const Luau::AstExpr* ungroup(const Luau::AstExpr* expression)
{
    while (const auto group = expression->as<Luau::AstExprGroup>())
        expression = group->expr;
    return expression;
}
struct SchemaVisitor : Luau::AstVisitor
{
    const LuduvoDiagnosticContext& context;
    DiagnosticReporter& reporter;
    EventSchemaConstraint constraint;
    SchemaVisitor(const LuduvoDiagnosticContext& context, DiagnosticReporter& reporter, EventSchemaConstraint constraint)
        : context(context)
        , reporter(reporter)
        , constraint(constraint)
    {
    }
    bool visit(Luau::AstExprCall* call) override
    {
        if (call->self || call->args.size < 3)
            return true;
        const auto symbols = context.resolvedCallSymbols(*call);
        if (std::find(symbols.begin(), symbols.end(), "game.EventTable") == symbols.end())
            return true;
        const auto schema = ungroup(call->args.data[2])->as<Luau::AstExprTable>();
        if (!schema)
            return true; // Never evaluate variables or functions to validate a schema.
        if (constraint == EventSchemaConstraint::FieldCount && schema->items.size > 24 &&
            std::all_of(schema->items.begin(), schema->items.end(),
                [](const Luau::AstExprTable::Item& item)
                {
                    return item.kind == Luau::AstExprTable::Item::Kind::List && ungroup(item.value)->is<Luau::AstExprTable>();
                }))
            reporter.report(schema->location, "EventTable schemas cannot contain more than 24 fields.");
        static const std::set<std::string> reserved = {"count", "sender", "tick", "Push", "PushTo", "Rows", "ToServer", "ToClients", "F32", "I32",
            "U8", "Bool", "Vec3", "Color", "Instance", "Entity", "String", "FireServer", "FireClient", "FireAllClients", "OnServerEvent",
            "OnClientEvent"};
        std::set<std::string> names;
        for (const auto& item : schema->items)
        {
            if (item.kind != Luau::AstExprTable::Item::Kind::List)
                continue;
            const auto value = ungroup(item.value);
            const auto pair = value->as<Luau::AstExprTable>();
            const bool scalar = value->is<Luau::AstExprConstantString>() || value->is<Luau::AstExprConstantNumber>() ||
                                value->is<Luau::AstExprConstantBool>() || value->is<Luau::AstExprFunction>();
            if (constraint == EventSchemaConstraint::EntryShape && (scalar || (pair && pair->items.size < 2)))
                reporter.report(value->location, "EventTable fields must be {name, type} pairs.");
            if (!pair || pair->items.size == 0 || pair->items.data[0].kind != Luau::AstExprTable::Item::Kind::List)
                continue;
            const auto literal = ungroup(pair->items.data[0].value)->as<Luau::AstExprConstantString>();
            if (!literal)
                continue;
            const std::string name(literal->value.data, literal->value.size);
            const bool duplicate = !names.insert(name).second;
            std::string message;
            switch (constraint)
            {
            case EventSchemaConstraint::EmptyName:
                if (name.empty())
                    message = "EventTable field names cannot be empty.";
                break;
            case EventSchemaConstraint::NameLength:
                if (name.size() > 31)
                    message = "EventTable field names cannot exceed 31 bytes.";
                break;
            case EventSchemaConstraint::ReservedName:
                if (reserved.count(name))
                    message = "EventTable field name '" + name + "' is reserved.";
                break;
            case EventSchemaConstraint::DuplicateName:
                if (duplicate)
                    message = "EventTable field '" + name + "' is declared more than once.";
                break;
            default:
                break;
            }
            if (!message.empty())
                reporter.report(literal->location, std::move(message));
        }
        return true;
    }
};
} // namespace
void analyzeEventSchema(const LuduvoDiagnosticContext& context, DiagnosticReporter& reporter, EventSchemaConstraint constraint)
{
    SchemaVisitor visitor(context, reporter, constraint);
    context.sourceModule().root->visit(&visitor);
}
} // namespace luduvo::diagnostics
