#include "Platform/luduvo/diagnostics/LuduvoDiagnosticRules.hpp"
#include "Platform/luduvo/diagnostics/LuduvoCustomDiagnostics.hpp"

namespace luduvo::diagnostics
{
const std::vector<Rule> rules = {
    {
        {"event-table.write", LuduvoDiagnosticSeverity::Error, "EventTable members and EventColumn rows are read-only."},
        CustomRule{analyzeEventTableWrite},
    },
    {
        {"event-table.fields.count", LuduvoDiagnosticSeverity::Error, "An event schema cannot contain more than 24 fields."},
        EventSchemaRule{EventSchemaConstraint::FieldCount},
    },
    {
        {"event-table.fields.shape", LuduvoDiagnosticSeverity::Error, "An event schema entry must be a name/type pair."},
        EventSchemaRule{EventSchemaConstraint::EntryShape},
    },
    {
        {"event-table.fields.empty-name", LuduvoDiagnosticSeverity::Error, "An event schema field name cannot be empty."},
        EventSchemaRule{EventSchemaConstraint::EmptyName},
    },
    {
        {"event-table.fields.name-length", LuduvoDiagnosticSeverity::Error, "An event schema field name cannot exceed 31 bytes."},
        EventSchemaRule{EventSchemaConstraint::NameLength},
    },
    {
        {"event-table.fields.reserved-name", LuduvoDiagnosticSeverity::Error, "An event schema field name is reserved by the engine."},
        EventSchemaRule{EventSchemaConstraint::ReservedName},
    },
    {
        {"event-table.fields.duplicate", LuduvoDiagnosticSeverity::Error, "An event schema cannot repeat a field name."},
        EventSchemaRule{EventSchemaConstraint::DuplicateName},
    },
    {
        {"event-table.unknown-column", LuduvoDiagnosticSeverity::Error, "A fully indexed event table has no such column."},
        CustomRule{analyzeUnknownEventTableColumn},
    },
    {
        {"world.empty-query", LuduvoDiagnosticSeverity::Error, "World.Query requires at least one query term."},
        CallRule{
            {"game.World.Query", "World.Query"},
            MinimumArguments{1},
            "game.World.Query requires at least one component name or Pair.",
        },
    },
    {
        {"script.autorun-self-access", LuduvoDiagnosticSeverity::Error, "self is nil in scripts inside an autorun directory."},
        CustomRule{analyzeAutorunSelfAccess},
    },
    {
        {"directive.invalid-nolint", LuduvoDiagnosticSeverity::Warning, "A Luduvo nolint directive refers to an unknown diagnostic rule."},
        InvalidNolintDirectiveRule{},
    },
};
} // namespace luduvo::diagnostics
