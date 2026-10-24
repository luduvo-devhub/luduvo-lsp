#include "Platform/luduvo/diagnostics/LuduvoDiagnosticAdapter.hpp"

#include "Platform/luduvo/diagnostics/LuduvoDiagnosticRules.hpp"

#include <algorithm>
#include <optional>
#include <string_view>

namespace
{
using namespace luduvo::diagnostics;

std::string canonicalRuleId(std::string_view id)
{
    constexpr std::string_view prefix = "luduvo/";
    if (id.substr(0, prefix.size()) == prefix)
        id.remove_prefix(prefix.size());
    return std::string(id);
}

bool isKnownRule(std::string_view id)
{
    return std::any_of(rules.begin(), rules.end(),
        [id](const Rule& rule)
        {
            return rule.metadata.id == id;
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
    result.settings.enabled = configuration.enabled;
    result.warnings = validateLuduvoDiagnosticRegistry();

    for (const auto& [configuredId, configuredLevel] : configuration.rules)
    {
        const std::string id = canonicalRuleId(configuredId);
        if (!isKnownRule(id))
        {
            result.warnings.push_back("diagnostics.rules references unknown rule '" + configuredId + "'.");
            continue;
        }

        const std::optional<LuduvoDiagnosticRuleLevel> level = parseRuleLevel(configuredLevel);
        if (!level)
        {
            result.warnings.push_back("diagnostics.rules['" + configuredId + "'] has unsupported severity '" + configuredLevel + "'.");
            continue;
        }

        if (*level == LuduvoDiagnosticRuleLevel::Default)
            result.settings.rules.erase(id);
        else
            result.settings.rules[id] = *level;
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
        result.push_back({diagnostic.ruleId, "Luduvo", diagnostic.location, diagnostic.message, platformSeverity(diagnostic.severity)});
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
