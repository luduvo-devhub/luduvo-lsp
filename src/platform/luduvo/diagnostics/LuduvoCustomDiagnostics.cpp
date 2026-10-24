#include "Platform/luduvo/diagnostics/LuduvoCustomDiagnostics.hpp"

#include "Luau/Ast.h"

namespace luduvo::diagnostics
{
namespace
{
struct GlobalSelfVisitor : Luau::AstVisitor
{
    DiagnosticReporter& reporter;

    explicit GlobalSelfVisitor(DiagnosticReporter& reporter)
        : reporter(reporter)
    {
    }

    bool visit(Luau::AstExprGlobal* expression) override
    {
        if (std::string_view(expression->name.value) == "self")
            reporter.report(expression->location, "'self' is nil in scripts inside an autorun directory.");
        return false;
    }

    bool visit(Luau::AstStatAssign* statement) override
    {
        for (Luau::AstExpr* variable : statement->vars)
        {
            const auto global = variable->as<Luau::AstExprGlobal>();
            if (!global || std::string_view(global->name.value) != "self")
                variable->visit(this);
        }
        for (Luau::AstExpr* value : statement->values)
            value->visit(this);
        return false;
    }
};
} // namespace

void analyzeAutorunSelfAccess(const LuduvoDiagnosticContext& context, DiagnosticReporter& reporter)
{
    if (!context.isAutorunScript())
        return;

    GlobalSelfVisitor visitor(reporter);
    context.sourceModule().root->visit(&visitor);
}
} // namespace luduvo::diagnostics
