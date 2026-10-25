#include "Platform/luduvo/LuduvoTargetResolver.hpp"

#include "Platform/luduvo/LuduvoSymbols.hpp"

#include "Luau/AstQuery.h"
#include "Luau/Type.h"
#include "Luau/TypePack.h"

#include <algorithm>

namespace
{
struct CallableOverload
{
    const Luau::FunctionType* function = nullptr;
    size_t index = 0;
};

std::vector<CallableOverload> callableOverloads(Luau::TypeId callableType)
{
    std::vector<CallableOverload> result;
    callableType = Luau::follow(callableType);
    if (const auto function = Luau::get<Luau::FunctionType>(callableType))
        result.push_back({function, 0});
    else if (const auto intersection = Luau::get<Luau::IntersectionType>(callableType))
    {
        for (Luau::TypeId part : intersection->parts)
            if (const auto function = Luau::get<Luau::FunctionType>(Luau::follow(part)))
                result.push_back({function, result.size()});
    }
    return result;
}

std::optional<size_t> namedParameterIndex(const Luau::FunctionType& function, std::string_view parameter)
{
    auto [arguments, tail] = Luau::flatten(function.argTypes);
    for (size_t index = 0; index < function.argNames.size() && index < arguments.size(); ++index)
        if (function.argNames[index] && function.argNames[index]->name == parameter)
            return index;
    return std::nullopt;
}
} // namespace

namespace luduvo
{
std::optional<std::string> expressionPath(const Luau::AstExpr* expression)
{
    if (const auto global = expression->as<Luau::AstExprGlobal>())
        return global->name.value;
    if (const auto member = expression->as<Luau::AstExprIndexName>())
        if (auto owner = expressionPath(member->expr))
            return *owner + "." + member->index.value;
    return std::nullopt;
}

std::optional<std::string> resolveCallableDocumentationSymbol(
    const Luau::SourceModule& sourceModule, const Luau::Module& module, const Luau::AstExprCall& call, Luau::TypeId callableType)
{
    Luau::Location documentationLocation = call.func->location;
    if (const auto member = call.func->as<Luau::AstExprIndexName>())
        documentationLocation = member->indexLocation;

    Luau::Position documentationPosition = documentationLocation.end;
    if (documentationPosition.column > 0)
        --documentationPosition.column;
    if (auto direct = Luau::getDocumentationSymbolAtPosition(sourceModule, module, documentationPosition))
        return direct;

    if (const auto member = call.func->as<Luau::AstExprIndexName>())
    {
        if (const Luau::TypeId* owner = module.astTypes.find(member->expr))
        {
            Luau::TypeId ownerType = Luau::follow(*owner);
            if (const auto table = Luau::get<Luau::TableType>(ownerType))
            {
                if (auto property = table->props.find(member->index.value); property != table->props.end())
                {
                    if (property->second.documentationSymbol)
                        return property->second.documentationSymbol;
                    if (auto path = expressionPath(call.func))
                        return path;
                }
            }
            else if (const Luau::ExternType* external = Luau::get<Luau::ExternType>(ownerType))
            {
                while (external)
                {
                    if (auto property = external->props.find(member->index.value); property != external->props.end())
                    {
                        if (property->second.documentationSymbol)
                            return property->second.documentationSymbol;
                        return external->name + "." + member->index.value;
                    }
                    external = external->parent ? Luau::get<Luau::ExternType>(*external->parent) : nullptr;
                }
            }
            if (ownerType->documentationSymbol)
                return *ownerType->documentationSymbol + "." + member->index.value;
        }
    }

    callableType = Luau::follow(callableType);
    if (callableType->documentationSymbol)
        return callableType->documentationSymbol;
    if (const auto intersection = Luau::get<Luau::IntersectionType>(callableType))
    {
        for (Luau::TypeId part : intersection->parts)
        {
            part = Luau::follow(part);
            if (part->documentationSymbol)
                return part->documentationSymbol;
        }
    }
    return std::nullopt;
}

std::vector<std::string> resolveCallSymbols(const Luau::SourceModule& sourceModule, const Luau::Module* module, const Luau::AstExprCall& call)
{
    std::vector<std::string> result;
    if (auto path = expressionPath(call.func))
        result.push_back(std::move(*path));

    if (!module)
        return result;

    const Luau::TypeId* callableType = module->astTypes.find(call.func);
    if (!callableType)
        return result;

    if (auto documentationSymbol = resolveCallableDocumentationSymbol(sourceModule, *module, call, *callableType))
    {
        if (auto canonical = canonicalLuduvoSymbol(documentationSymbol))
            if (std::find(result.begin(), result.end(), *canonical) == result.end())
                result.push_back(std::move(*canonical));
    }
    return result;
}

std::vector<CallableTarget> resolveCallableTarget(Luau::TypeId callableType, const std::vector<std::string>& parameters, bool variadic)
{
    std::vector<CallableTarget> result;
    if (parameters.empty())
        return result;

    for (const CallableOverload& overload : callableOverloads(callableType))
    {
        auto [arguments, tail] = Luau::flatten(overload.function->argTypes);
        if (variadic)
        {
            if (!tail)
                continue;
            if (const auto variadicType = Luau::get<Luau::VariadicTypePack>(Luau::follow(*tail)))
                result.push_back({overload.function, {variadicType->ty}, {arguments.size()}, overload.index, true});
            continue;
        }

        CallableTarget target;
        target.function = overload.function;
        target.overloadIndex = overload.index;
        target.indices.reserve(parameters.size());
        target.types.reserve(parameters.size());
        for (const std::string& parameter : parameters)
        {
            const auto index = namedParameterIndex(*overload.function, parameter);
            if (!index)
            {
                target.indices.clear();
                break;
            }
            target.indices.push_back(*index);
            target.types.push_back(arguments[*index]);
        }
        if (!target.indices.empty())
            result.push_back(std::move(target));
    }
    return result;
}
} // namespace luduvo
