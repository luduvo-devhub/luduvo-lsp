#include "Platform/luduvo/diagnostics/LuduvoDiagnosticRules.hpp"
#include "Platform/luduvo/diagnostics/LuduvoCustomDiagnostics.hpp"

namespace luduvo::diagnostics
{
const std::vector<Rule> rules = {
    {
        {"empty-world-query", LuduvoDiagnosticSeverity::Error, "World.Query requires at least one query term."},
        CallRule{
            {"game.World.Query", "World.Query"},
            MinimumArguments{1},
            "game.World.Query requires at least one component name or Pair.",
        },
    },
    {
        {"autorun-self-access", LuduvoDiagnosticSeverity::Error, "self is nil in scripts inside an autorun directory."},
        CustomRule{analyzeAutorunSelfAccess},
    },
    {
        {"invalid-nolint-directive", LuduvoDiagnosticSeverity::Warning, "A Luduvo nolint directive refers to an unknown diagnostic rule."},
        InvalidNolintDirectiveRule{},
    },
};
} // namespace luduvo::diagnostics
