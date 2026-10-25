#pragma once

#include "Luau/Ast.h"
#include "Luau/Module.h"
#include "Luau/TypeFwd.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace luduvo
{
struct CallableTarget
{
    const Luau::FunctionType* function = nullptr;
    std::vector<Luau::TypeId> types;
    std::vector<size_t> indices;
    size_t overloadIndex = 0;
    bool variadic = false;
};

/// Resolves the canonical path represented directly by an expression. Local
/// aliases deliberately return no path; their identity is recovered from type
/// information by resolveCallSymbols.
std::optional<std::string> expressionPath(const Luau::AstExpr* expression);

/// Resolves the documentation identity of a callable, including members whose
/// documentation is carried by their owner or overload type.
std::optional<std::string> resolveCallableDocumentationSymbol(
    const Luau::SourceModule& sourceModule, const Luau::Module& module, const Luau::AstExprCall& call, Luau::TypeId callableType);

/// Returns every stable identity known for a call. Direct global paths and
/// type-derived documentation identities are kept together so consumers do not
/// need separate alias-handling logic.
std::vector<std::string> resolveCallSymbols(const Luau::SourceModule& sourceModule, const Luau::Module* module, const Luau::AstExprCall& call);

std::vector<CallableTarget> resolveCallableTarget(Luau::TypeId callableType, const std::vector<std::string>& parameters, bool variadic = false);
} // namespace luduvo
