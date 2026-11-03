#pragma once

#include "LuduvoDiagnosticRules.hpp"

namespace luduvo::diagnostics
{
void analyzeEventTableWrite(const LuduvoDiagnosticContext& context, DiagnosticReporter& reporter);
void analyzeEventSchema(const LuduvoDiagnosticContext& context, DiagnosticReporter& reporter, EventSchemaConstraint constraint);
void analyzeAutorunSelfAccess(const LuduvoDiagnosticContext& context, DiagnosticReporter& reporter);
void analyzeUnknownEventTableColumn(const LuduvoDiagnosticContext& context, DiagnosticReporter& reporter);
} // namespace luduvo::diagnostics
