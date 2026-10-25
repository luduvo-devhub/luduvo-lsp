#include "Platform/luduvo/diagnostics/LuduvoDiagnostics.hpp"

#include "Platform/luduvo/diagnostics/LuduvoDiagnosticRules.hpp"
#include "Platform/luduvo/LuduvoTargetResolver.hpp"

#include "Luau/ParseResult.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_set>

namespace
{
using namespace luduvo::diagnostics;

struct ActiveRule
{
    const Rule& rule;
    LuduvoDiagnosticSeverity severity;
};

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

bool hasPathSegment(const std::filesystem::path& path, std::string_view expected)
{
    return std::any_of(path.begin(), path.end(),
        [expected](const std::filesystem::path& segment)
        {
            return lower(segment.string()) == expected;
        });
}

bool matchesAnySymbol(const std::vector<std::string_view>& expected, const std::vector<std::string>& actual)
{
    return std::any_of(expected.begin(), expected.end(),
        [&](std::string_view symbol)
        {
            return std::find(actual.begin(), actual.end(), symbol) != actual.end();
        });
}

bool violates(const CallConstraint& constraint, const LuduvoDiagnosticContext& context, const Luau::AstExprCall& call)
{
    return std::visit(
        [&](const auto& value)
        {
            using Constraint = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Constraint, MinimumArguments>)
                return context.explicitArgumentCount(call) < value.count;
        },
        constraint);
}

LuduvoDiagnosticSeverity configuredSeverity(const Rule& rule, const LuduvoDiagnosticSettings& settings)
{
    const auto configured = settings.rules.find(std::string(rule.metadata.id));
    if (configured == settings.rules.end() || configured->second == LuduvoDiagnosticRuleLevel::Default)
        return rule.metadata.defaultSeverity;

    switch (configured->second)
    {
    case LuduvoDiagnosticRuleLevel::Error:
        return LuduvoDiagnosticSeverity::Error;
    case LuduvoDiagnosticRuleLevel::Warning:
        return LuduvoDiagnosticSeverity::Warning;
    case LuduvoDiagnosticRuleLevel::Information:
        return LuduvoDiagnosticSeverity::Information;
    case LuduvoDiagnosticRuleLevel::Hint:
        return LuduvoDiagnosticSeverity::Hint;
    case LuduvoDiagnosticRuleLevel::Default:
    case LuduvoDiagnosticRuleLevel::Off:
        return rule.metadata.defaultSeverity;
    }
    return rule.metadata.defaultSeverity;
}

size_t editDistance(std::string_view left, std::string_view right)
{
    std::vector<size_t> previous(right.size() + 1);
    std::vector<size_t> current(right.size() + 1);
    for (size_t index = 0; index <= right.size(); ++index)
        previous[index] = index;

    for (size_t leftIndex = 1; leftIndex <= left.size(); ++leftIndex)
    {
        current[0] = leftIndex;
        for (size_t rightIndex = 1; rightIndex <= right.size(); ++rightIndex)
        {
            const size_t substitution = previous[rightIndex - 1] + (left[leftIndex - 1] == right[rightIndex - 1] ? 0 : 1);
            current[rightIndex] = std::min({previous[rightIndex] + 1, current[rightIndex - 1] + 1, substitution});
        }
        previous.swap(current);
    }
    return previous.back();
}

std::optional<std::string_view> closestRuleId(std::string_view id)
{
    size_t bestDistance = std::numeric_limits<size_t>::max();
    std::optional<std::string_view> result;
    for (const Rule& rule : rules)
    {
        const size_t distance = editDistance(id, rule.metadata.id);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            result = rule.metadata.id;
        }
    }

    const size_t threshold = std::max<size_t>(3, id.size() / 3);
    return bestDistance <= threshold ? result : std::nullopt;
}

const Rule* findRule(std::string_view id)
{
    const auto result = std::find_if(rules.begin(), rules.end(),
        [id](const Rule& rule)
        {
            return rule.metadata.id == id;
        });
    return result == rules.end() ? nullptr : &*result;
}

struct FileSuppressions
{
    bool all = false;
    std::unordered_set<std::string> rules;
    std::vector<RawFinding> invalidDirectives;
};

FileSuppressions parseSuppressions(const Luau::SourceModule& sourceModule)
{
    FileSuppressions result;
    constexpr std::string_view prefix = "luduvo/";

    for (const Luau::HotComment& comment : sourceModule.hotcomments)
    {
        if (!comment.header || comment.content.compare(0, 6, "nolint") != 0)
            continue;

        const size_t argumentStart = comment.content.find_first_not_of(" \t", 6);
        if (argumentStart == std::string::npos)
        {
            result.all = true;
            continue;
        }
        if (argumentStart == 6)
            continue;

        std::string_view argument(comment.content.data() + argumentStart, comment.content.size() - argumentStart);
        const size_t argumentEnd = argument.find_last_not_of(" \t");
        argument = argument.substr(0, argumentEnd + 1);
        if (argument.substr(0, prefix.size()) != prefix)
            continue;

        argument.remove_prefix(prefix.size());
        if (findRule(argument))
            result.rules.emplace(argument);
        else
        {
            std::string message = "nolint directive refers to unknown Luduvo diagnostic rule '" + std::string(argument) + "'";
            if (const auto suggestion = closestRuleId(argument))
                message += "; did you mean 'luduvo/" + std::string(*suggestion) + "'?";
            result.invalidDirectives.push_back({comment.location, std::move(message)});
        }
    }
    return result;
}

struct CallConstraintVisitor : Luau::AstVisitor
{
    const LuduvoDiagnosticContext& context;
    const std::vector<ActiveRule>& activeRules;
    std::vector<LuduvoDiagnostic>& diagnostics;

    CallConstraintVisitor(
        const LuduvoDiagnosticContext& context, const std::vector<ActiveRule>& activeRules, std::vector<LuduvoDiagnostic>& diagnostics)
        : context(context)
        , activeRules(activeRules)
        , diagnostics(diagnostics)
    {
    }

    bool visit(Luau::AstExprCall* call) override
    {
        const std::vector<std::string> symbols = context.resolvedCallSymbols(*call);
        for (const ActiveRule& active : activeRules)
        {
            const CallRule* rule = std::get_if<CallRule>(&active.rule.implementation);
            if (rule && matchesAnySymbol(rule->symbols, symbols) && violates(rule->constraint, context, *call))
                diagnostics.push_back({std::string(active.rule.metadata.id), call->location, std::string(rule->message), active.severity});
        }
        return true;
    }
};
} // namespace

namespace luduvo::diagnostics
{
DiagnosticReporter::DiagnosticReporter(std::vector<RawFinding>& findings)
    : findings(findings)
{
}

void DiagnosticReporter::report(const Luau::Location& location, std::string message)
{
    findings.push_back({location, std::move(message)});
}
} // namespace luduvo::diagnostics

LuduvoDiagnosticContext::LuduvoDiagnosticContext(const LuduvoDiagnosticInput& input)
    : input(input)
{
}

bool LuduvoDiagnosticContext::isAutorunScript() const
{
    return hasPathSegment(input.path.parent_path(), "autorun");
}

std::vector<std::string> LuduvoDiagnosticContext::resolvedCallSymbols(const Luau::AstExprCall& call) const
{
    return luduvo::resolveCallSymbols(input.sourceModule, input.module.get(), call);
}

size_t LuduvoDiagnosticContext::explicitArgumentCount(const Luau::AstExprCall& call) const
{
    return call.args.size;
}

const Luau::SourceModule& LuduvoDiagnosticContext::sourceModule() const
{
    return input.sourceModule;
}

std::vector<LuduvoDiagnostic> collectLuduvoDiagnostics(const LuduvoDiagnosticInput& input)
{
    if (!input.settings.enabled)
        return {};

    const FileSuppressions suppressions = parseSuppressions(input.sourceModule);
    std::vector<LuduvoDiagnostic> diagnostics;
    std::vector<ActiveRule> activeRules;
    for (const Rule& rule : rules)
    {
        const auto configured = input.settings.rules.find(std::string(rule.metadata.id));
        if (suppressions.all || suppressions.rules.count(std::string(rule.metadata.id)) != 0 ||
            (configured != input.settings.rules.end() && configured->second == LuduvoDiagnosticRuleLevel::Off))
            continue;
        activeRules.push_back({rule, configuredSeverity(rule, input.settings)});
    }

    for (const ActiveRule& active : activeRules)
    {
        if (!std::holds_alternative<InvalidNolintDirectiveRule>(active.rule.implementation))
            continue;
        for (const RawFinding& finding : suppressions.invalidDirectives)
            diagnostics.push_back({std::string(active.rule.metadata.id), finding.location, finding.message, active.severity});
    }

    const LuduvoDiagnosticContext context(input);
    CallConstraintVisitor callVisitor(context, activeRules, diagnostics);
    input.sourceModule.root->visit(&callVisitor);

    for (const ActiveRule& active : activeRules)
    {
        const CustomRule* rule = std::get_if<CustomRule>(&active.rule.implementation);
        if (!rule)
            continue;

        std::vector<RawFinding> findings;
        DiagnosticReporter reporter(findings);
        rule->analyze(context, reporter);
        for (RawFinding& finding : findings)
            diagnostics.push_back({std::string(active.rule.metadata.id), finding.location, std::move(finding.message), active.severity});
    }
    return diagnostics;
}

std::vector<std::string> validateLuduvoDiagnosticRegistry()
{
    std::vector<std::string> errors;
    std::unordered_set<std::string_view> ids;
    for (const Rule& rule : rules)
    {
        if (rule.metadata.id.empty())
            errors.emplace_back("A Luduvo diagnostic rule has an empty ID.");
        else if (!ids.insert(rule.metadata.id).second)
            errors.push_back("Duplicate Luduvo diagnostic rule ID '" + std::string(rule.metadata.id) + "'.");
        if (rule.metadata.description.empty())
            errors.push_back("Luduvo diagnostic rule '" + std::string(rule.metadata.id) + "' has no description.");
    }
    return errors;
}
