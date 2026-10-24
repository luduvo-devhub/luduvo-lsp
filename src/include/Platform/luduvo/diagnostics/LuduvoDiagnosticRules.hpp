#pragma once

#include "LuduvoDiagnostics.hpp"

#include <string_view>
#include <variant>
#include <vector>

namespace luduvo::diagnostics
{
struct RuleMetadata
{
    std::string_view id;
    LuduvoDiagnosticSeverity defaultSeverity;
    std::string_view description;
};

struct MinimumArguments
{
    size_t count;
};

using CallConstraint = std::variant<MinimumArguments>;

struct CallRule
{
    std::vector<std::string_view> symbols;
    CallConstraint constraint;
    std::string_view message;
};

struct RawFinding
{
    Luau::Location location;
    std::string message;
};

class DiagnosticReporter
{
    std::vector<RawFinding>& findings;

public:
    explicit DiagnosticReporter(std::vector<RawFinding>& findings);
    void report(const Luau::Location& location, std::string message);
};

using AnalyzeCustomRule = void (*)(const LuduvoDiagnosticContext&, DiagnosticReporter&);

struct CustomRule
{
    AnalyzeCustomRule analyze;
};

struct InvalidNolintDirectiveRule
{
};

using RuleImplementation = std::variant<CallRule, CustomRule, InvalidNolintDirectiveRule>;

struct Rule
{
    RuleMetadata metadata;
    RuleImplementation implementation;
};

extern const std::vector<Rule> rules;
} // namespace luduvo::diagnostics
