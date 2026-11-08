#include "Platform/luduvo/diagnostics/LuduvoCustomDiagnostics.hpp"
#include "Luau/Ast.h"
#include "Luau/ToString.h"
#include "Platform/luduvo/schema/LuduvoSchemaIndex.hpp"
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
bool incompatibleFieldType(Luau::TypeId type, std::set<Luau::TypeId>& active)
{
    type = Luau::follow(type);
    if (!active.insert(type).second)
        return false;
    const auto incompatiblePart = [&](Luau::TypeId part)
    {
        return incompatibleFieldType(part, active);
    };
    bool incompatible = false;
    if (const auto external = Luau::get<Luau::ExternType>(type))
        incompatible = external->name != "EventFieldType";
    else if (const auto options = Luau::get<Luau::UnionType>(type))
        incompatible = std::any_of(options->options.begin(), options->options.end(), incompatiblePart);
    else if (const auto parts = Luau::get<Luau::IntersectionType>(type))
        incompatible = std::any_of(parts->parts.begin(), parts->parts.end(), incompatiblePart);
    else
        incompatible = Luau::is<Luau::PrimitiveType, Luau::SingletonType, Luau::TableType, Luau::FunctionType, Luau::MetatableType>(type);
    active.erase(type);
    return incompatible;
}

struct SchemaVisitor : Luau::AstVisitor
{
    const LuduvoDiagnosticContext& context;
    DiagnosticReporter& reporter;
    EventSchemaConstraint constraint;
    std::optional<SchemaSyntax> syntax;
    SchemaVisitor(const LuduvoDiagnosticContext& context, DiagnosticReporter& reporter, EventSchemaConstraint constraint)
        : context(context)
        , reporter(reporter)
        , constraint(constraint)
    {
        if (constraint == EventSchemaConstraint::FieldType)
            syntax.emplace(context.sourceModule().root);
    }

    std::optional<std::string> invalidFieldValue(const Luau::AstExpr* expression) const
    {
        constexpr std::string_view prefix = "game.EventTable.";
        if (const auto symbol = syntax->symbol(expression); symbol && symbol->compare(0, prefix.size(), prefix) == 0)
        {
            const std::string_view member = std::string_view(*symbol).substr(prefix.size());
            if (eventFieldValueType(member).empty())
                return "'" + *symbol + "'";
            return std::nullopt;
        }

        const auto value = syntax->value(expression);
        if (const auto conditional = value->as<Luau::AstExprIfElse>())
        {
            if (const auto invalid = invalidFieldValue(conditional->trueExpr))
                return invalid;
            return invalidFieldValue(conditional->falseExpr);
        }
        for (const auto candidate : {expression, value})
            if (const auto type = context.expressionType(candidate))
            {
                std::set<Luau::TypeId> active;
                if (incompatibleFieldType(*type, active))
                {
                    Luau::ToStringOptions options;
                    options.maxTypeLength = 100;
                    return "a value of type '" + Luau::toString(*type, options) + "'";
                }
            }
        if (value->is<Luau::AstExprConstantString>() || value->is<Luau::AstExprInterpString>())
            return "a string";
        if (value->is<Luau::AstExprConstantNumber>())
            return "a number";
        if (value->is<Luau::AstExprConstantBool>())
            return "a boolean";
        if (value->is<Luau::AstExprConstantNil>())
            return "nil";
        if (value->is<Luau::AstExprTable>())
            return "a table";
        if (value->is<Luau::AstExprFunction>())
            return "a function";
        return std::nullopt;
    }

    bool visit(Luau::AstExprCall* call) override
    {
        if (call->self || call->args.size < 3)
            return true;
        const auto symbols = context.resolvedCallSymbols(*call);
        if (std::find(symbols.begin(), symbols.end(), "game.EventTable") == symbols.end() &&
            (!syntax || syntax->symbol(call->func) != "game.EventTable"))
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
            if (constraint == EventSchemaConstraint::FieldType)
            {
                if (pair->items.size >= 2 && pair->items.data[1].kind == Luau::AstExprTable::Item::Kind::List)
                {
                    const auto type = ungroup(pair->items.data[1].value);
                    if (const auto invalid = invalidFieldValue(type))
                    {
                        const auto name = syntax->value(pair->items.data[0].value)->as<Luau::AstExprConstantString>();
                        const std::string field = name ? "'" + std::string(name->value.data, name->value.size) + "'"
                                                       : "#" + std::to_string(&item - schema->items.begin() + 1);
                        reporter.report(type->location,
                            "EventTable field " + field + " expects a constant from game.EventTable (" + std::string(eventFieldTypeNames()) +
                                "), but got " + *invalid + ".");
                    }
                }
                continue;
            }
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
