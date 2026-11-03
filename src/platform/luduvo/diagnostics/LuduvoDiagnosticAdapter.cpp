#include "Platform/luduvo/diagnostics/LuduvoDiagnosticAdapter.hpp"

#include "Platform/luduvo/diagnostics/LuduvoDiagnosticRules.hpp"

#include <algorithm>
#include <optional>
#include <string_view>

namespace
{
using namespace luduvo::diagnostics;

bool isKnownCategory(std::string_view category)
{
    return std::any_of(rules.begin(), rules.end(),
        [category](const Rule& rule)
        {
            return rule.metadata.id == category ||
                   (rule.metadata.id.size() > category.size() && rule.metadata.id.substr(0, category.size()) == category &&
                       rule.metadata.id[category.size()] == '.');
        });
}

std::optional<LuduvoDiagnosticRuleLevel> parseRuleLevel(std::string_view level)
{
    if (level == "default")
        return LuduvoDiagnosticRuleLevel::Default;
    if (level == "off")
        return LuduvoDiagnosticRuleLevel::Off;
    if (level == "error")
        return LuduvoDiagnosticRuleLevel::Error;
    if (level == "warning")
        return LuduvoDiagnosticRuleLevel::Warning;
    if (level == "information")
        return LuduvoDiagnosticRuleLevel::Information;
    if (level == "hint")
        return LuduvoDiagnosticRuleLevel::Hint;
    return std::nullopt;
}

PlatformDiagnosticSeverity platformSeverity(LuduvoDiagnosticSeverity severity)
{
    switch (severity)
    {
    case LuduvoDiagnosticSeverity::Error:
        return PlatformDiagnosticSeverity::Error;
    case LuduvoDiagnosticSeverity::Warning:
        return PlatformDiagnosticSeverity::Warning;
    case LuduvoDiagnosticSeverity::Information:
        return PlatformDiagnosticSeverity::Information;
    case LuduvoDiagnosticSeverity::Hint:
        return PlatformDiagnosticSeverity::Hint;
    }
    return PlatformDiagnosticSeverity::Warning;
}

std::string_view severityName(LuduvoDiagnosticSeverity severity)
{
    switch (severity)
    {
    case LuduvoDiagnosticSeverity::Error:
        return "error";
    case LuduvoDiagnosticSeverity::Warning:
        return "warning";
    case LuduvoDiagnosticSeverity::Information:
        return "information";
    case LuduvoDiagnosticSeverity::Hint:
        return "hint";
    }
    return "warning";
}
} // namespace

LuduvoDiagnosticConfigurationResult resolveLuduvoDiagnosticConfiguration(const ClientLuduvoDiagnosticsConfiguration& configuration)
{
    LuduvoDiagnosticConfigurationResult result;
    result.warnings = validateLuduvoDiagnosticRegistry();

    if (configuration.defaultValue)
    {
        const auto level = parseRuleLevel(*configuration.defaultValue);
        if (level)
            result.settings.defaultValue = *level;
        else
            result.warnings.push_back("diagnostics.default has unsupported severity '" + *configuration.defaultValue + "'.");
    }
    for (const auto& entry : configuration.categories)
    {
        if (!isKnownCategory(entry.first))
        {
            result.warnings.push_back("diagnostics.categories references unknown category or rule '" + entry.first + "'.");
            continue;
        }
        const auto level = parseRuleLevel(entry.second);
        if (!level)
        {
            result.warnings.push_back("diagnostics.categories['" + entry.first + "'] has unsupported severity '" + entry.second + "'.");
            continue;
        }
        result.settings.categories[entry.first] = *level;
    }
    return result;
}

std::vector<PlatformDiagnostic> collectLuduvoPlatformDiagnostics(const LuduvoDiagnosticSettings& settings, const PlatformDiagnosticContext& context)
{
    const std::vector<LuduvoDiagnostic> luduvoDiagnostics =
        collectLuduvoDiagnostics({std::filesystem::path(context.uri.fsPath()), context.sourceModule, context.module, settings});

    std::vector<PlatformDiagnostic> result;
    result.reserve(luduvoDiagnostics.size());
    for (const LuduvoDiagnostic& diagnostic : luduvoDiagnostics)
    {
        result.push_back({"luduvo/" + diagnostic.ruleId, "Luduvo", diagnostic.location, diagnostic.message, platformSeverity(diagnostic.severity)});
    }
    return result;
}

std::vector<PlatformLintRuleCompletion> getLuduvoLintRuleCompletions()
{
    std::vector<PlatformLintRuleCompletion> result;
    result.reserve(rules.size());
    for (const Rule& rule : rules)
    {
        result.push_back({"luduvo/" + std::string(rule.metadata.id),
            "Luduvo diagnostic rule · default: " + std::string(severityName(rule.metadata.defaultSeverity)), std::string(rule.metadata.description)});
    }
    return result;
}
