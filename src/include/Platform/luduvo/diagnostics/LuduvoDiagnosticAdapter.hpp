#pragma once

#include "LSP/ClientConfiguration.hpp"
#include "Platform/LSPPlatform.hpp"
#include "LuduvoDiagnostics.hpp"

struct LuduvoDiagnosticConfigurationResult
{
    LuduvoDiagnosticSettings settings;
    std::vector<std::string> warnings;
};

LuduvoDiagnosticConfigurationResult resolveLuduvoDiagnosticConfiguration(const ClientLuduvoDiagnosticsConfiguration& configuration);
std::vector<PlatformDiagnostic> collectLuduvoPlatformDiagnostics(const LuduvoDiagnosticSettings& settings, const PlatformDiagnosticContext& context);
std::vector<PlatformLintRuleCompletion> getLuduvoLintRuleCompletions();
