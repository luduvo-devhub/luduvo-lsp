#pragma once

#include "Luau/Ast.h"
#include "Luau/Module.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

enum struct LuduvoDiagnosticSeverity
{
    Error,
    Warning,
    Information,
    Hint,
};

enum struct LuduvoDiagnosticRuleLevel
{
    Default,
    Off,
    Error,
    Warning,
    Information,
    Hint,
};

struct LuduvoDiagnosticSettings
{
    bool enabled = true;
    std::unordered_map<std::string, LuduvoDiagnosticRuleLevel> rules;
};

struct LuduvoDiagnosticInput
{
    std::filesystem::path path;
    const Luau::SourceModule& sourceModule;
    const Luau::ModulePtr& module;
    const LuduvoDiagnosticSettings& settings;
};

struct LuduvoDiagnostic
{
    std::string ruleId;
    Luau::Location location;
    std::string message;
    LuduvoDiagnosticSeverity severity = LuduvoDiagnosticSeverity::Warning;
};

class LuduvoDiagnosticContext
{
    const LuduvoDiagnosticInput& input;

public:
    explicit LuduvoDiagnosticContext(const LuduvoDiagnosticInput& input);

    bool isAutorunScript() const;
    std::vector<std::string> resolvedCallSymbols(const Luau::AstExprCall& call) const;
    size_t explicitArgumentCount(const Luau::AstExprCall& call) const;
    const Luau::SourceModule& sourceModule() const;
};

std::vector<LuduvoDiagnostic> collectLuduvoDiagnostics(const LuduvoDiagnosticInput& input);
std::vector<std::string> validateLuduvoDiagnosticRegistry();
