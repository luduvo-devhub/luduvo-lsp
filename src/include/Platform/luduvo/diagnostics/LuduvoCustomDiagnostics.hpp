#pragma once

#include "LuduvoDiagnosticRules.hpp"

namespace luduvo::diagnostics
{
void analyzeAutorunSelfAccess(const LuduvoDiagnosticContext& context, DiagnosticReporter& reporter);
} // namespace luduvo::diagnostics
