#include "LSP/HoverSubject.hpp"
#include "LSP/Workspace.hpp"
#include "LSP/LuauExt.hpp"

namespace
{
struct DocumentationLocation
{
    Luau::ModuleName moduleName;
    Luau::Location location;
};

static bool isBetween(const Luau::Position& position, const Luau::Position& left, const Luau::Position& right)
{
    return left <= position && position <= right;
}

static bool isOperatorPosition(const Luau::SourceModule& sourceModule, const Luau::Position& position)
{
    for (Luau::AstNode* ancestor : Luau::findAstAncestryOfPosition(sourceModule, position))
    {
        if (auto binary = ancestor->as<Luau::AstExprBinary>();
            binary && isBetween(position, binary->left->location.end, binary->right->location.begin))
            return true;
        if (auto unary = ancestor->as<Luau::AstExprUnary>(); unary && isBetween(position, unary->location.begin, unary->expr->location.begin))
            return true;
        if (auto local = ancestor->as<Luau::AstStatLocal>();
            local && local->equalsSignLocation && local->equalsSignLocation->containsClosed(position))
            return true;
        if (auto assignment = ancestor->as<Luau::AstStatAssign>();
            assignment && assignment->vars.size > 0 && assignment->values.size > 0 &&
            isBetween(position, assignment->vars.data[assignment->vars.size - 1]->location.end, assignment->values.data[0]->location.begin))
            return true;
        if (auto assignment = ancestor->as<Luau::AstStatCompoundAssign>();
            assignment && isBetween(position, assignment->var->location.end, assignment->value->location.begin))
            return true;
    }
    return false;
}

static bool isHoverableTokenPosition(
    const Luau::SourceModule& sourceModule, Luau::ExprOrLocal& exprOrLocal, Luau::AstNode* node, const Luau::Position& position)
{
    if (const Luau::AstLocal* local = exprOrLocal.getLocal(); local && local->location.containsClosed(position))
        return true;

    for (const Luau::AstNode* ancestor : Luau::findAstAncestryOfPosition(sourceModule, position))
    {
        if ((ancestor->is<Luau::AstExprLocal>() || ancestor->is<Luau::AstExprGlobal>() || ancestor->is<Luau::AstExprConstantNil>() ||
                ancestor->is<Luau::AstExprConstantBool>() || ancestor->is<Luau::AstExprConstantNumber>() ||
                ancestor->is<Luau::AstExprConstantInteger>() || ancestor->is<Luau::AstExprConstantString>()) &&
            ancestor->location.containsClosed(position))
            return true;
        if (auto index = ancestor->as<Luau::AstExprIndexName>(); index && index->indexLocation.containsClosed(position))
            return true;
        if (auto reference = ancestor->as<Luau::AstTypeReference>();
            reference &&
            (reference->nameLocation.containsClosed(position) || (reference->prefixLocation && reference->prefixLocation->containsClosed(position))))
            return true;
        if (auto alias = ancestor->as<Luau::AstStatTypeAlias>(); alias && alias->nameLocation.containsClosed(position))
            return true;
        if (auto typeFunction = ancestor->as<Luau::AstStatTypeFunction>(); typeFunction && typeFunction->nameLocation.containsClosed(position))
            return true;
        if (auto declaredGlobal = ancestor->as<Luau::AstStatDeclareGlobal>(); declaredGlobal && declaredGlobal->nameLocation.containsClosed(position))
            return true;
        if (auto declaredFunction = ancestor->as<Luau::AstStatDeclareFunction>();
            declaredFunction && declaredFunction->nameLocation.containsClosed(position))
            return true;
    }

    return node->asType() && node->location.containsClosed(position);
}

HoverDeclarationTarget resolveDeclarationTarget(
    const Luau::SourceModule& sourceModule, Luau::ExprOrLocal& exprOrLocal, Luau::AstNode* node, const Luau::Position& position)
{
    const bool hoveringType = node->asType() != nullptr;
    const auto ancestry = Luau::findAstAncestryOfPosition(sourceModule, position, /* includeTypes: */ true);

    if (hoveringType)
    {
        for (Luau::AstNode* ancestor : ancestry)
        {
            if (auto local = ancestor->as<Luau::AstStatLocal>())
            {
                for (Luau::AstLocal* variable : local->vars)
                    if (variable->annotation && variable->annotation->location.containsClosed(position))
                        return {HoverDeclarationKind::Local, variable->name.value, variable->isConst};
            }
            else if (auto localFunction = ancestor->as<Luau::AstStatLocalFunction>())
            {
                if (localFunction->func->returnAnnotation && localFunction->func->returnAnnotation->location.containsClosed(position))
                    return {HoverDeclarationKind::Local, localFunction->name->name.value, localFunction->name->isConst};
            }
            else if (auto alias = ancestor->as<Luau::AstStatTypeAlias>())
                return {HoverDeclarationKind::Type, alias->name.value};
        }
    }

    for (auto it = ancestry.rbegin(); it != ancestry.rend(); ++it)
        if (auto member = (*it)->as<Luau::AstExprIndexName>(); member && member->indexLocation.containsClosed(position))
            return {HoverDeclarationKind::Member, member->index.value};

    const Luau::AstLocal* local = exprOrLocal.getLocal();
    if (!local)
        if (auto localExpression = node->as<Luau::AstExprLocal>())
            local = localExpression->local;
    if (local)
        return {HoverDeclarationKind::Local, local->name.value, local->isConst};

    if (auto global = node->as<Luau::AstExprGlobal>())
        return {HoverDeclarationKind::Global, global->name.value};
    if (auto alias = node->as<Luau::AstStatTypeAlias>())
        return {HoverDeclarationKind::Type, alias->name.value};
    if (auto reference = node->as<Luau::AstTypeReference>())
        return {HoverDeclarationKind::Type, reference->name.value};
    return {};
}

bool isLiteralPosition(const Luau::SourceModule& sourceModule, const Luau::Position& position)
{
    for (const Luau::AstNode* ancestor : Luau::findAstAncestryOfPosition(sourceModule, position))
        if ((ancestor->is<Luau::AstExprConstantNil>() || ancestor->is<Luau::AstExprConstantBool>() || ancestor->is<Luau::AstExprConstantNumber>() ||
                ancestor->is<Luau::AstExprConstantInteger>() || ancestor->is<Luau::AstExprConstantString>()) &&
            ancestor->location.containsClosed(position))
            return true;
    return false;
}

struct FindDirectLocalInitializer : Luau::AstVisitor
{
    const Luau::AstLocal* target;
    Luau::AstExpr* result = nullptr;
    bool reassigned = false;

    explicit FindDirectLocalInitializer(const Luau::AstLocal* target)
        : target(target)
    {
    }

    bool visit(Luau::AstStatLocal* local) override
    {
        for (size_t index = 0; index < local->vars.size && index < local->values.size; ++index)
        {
            if (local->vars.data[index] == target)
            {
                result = local->values.data[index];
                return false;
            }
        }
        return true;
    }

    bool isTarget(const Luau::AstExpr* expression) const
    {
        const auto local = expression->as<Luau::AstExprLocal>();
        return local && local->local == target;
    }

    bool visit(Luau::AstStatAssign* assignment) override
    {
        for (const auto variable : assignment->vars)
            reassigned |= isTarget(variable);
        return true;
    }

    bool visit(Luau::AstStatCompoundAssign* assignment) override
    {
        reassigned |= isTarget(assignment->var);
        return true;
    }

    bool visit(Luau::AstStatFunction* function) override
    {
        reassigned |= isTarget(function->name);
        return true;
    }
};

const Luau::AstExprIndexName* getDirectMemberInitializer(const Luau::SourceModule& sourceModule, const Luau::AstLocal* local)
{
    if (!local)
        return nullptr;
    FindDirectLocalInitializer finder{local};
    sourceModule.root->visit(&finder);
    return finder.result && !finder.reassigned ? finder.result->as<Luau::AstExprIndexName>() : nullptr;
}

std::vector<PropLookup> getMemberProperties(const Luau::Module& module, const Luau::AstExprIndexName& member)
{
    if (const auto receiver = module.astTypes.find(member.expr))
        return lookupProp(Luau::follow(*receiver), member.index.value);
    return {};
}

} // namespace

std::optional<HoverSubject> resolveHoverSubject(WorkspaceFolder& workspace, const Luau::ModuleName& moduleName,
    const Luau::SourceModule& sourceModule, const Luau::Module& module, const Luau::Position& position)
{
    auto exprOrLocal = Luau::findExprOrLocalAtPosition(sourceModule, position);
    auto node = findNodeOrTypeAtPosition(sourceModule, position);
    auto scope = Luau::findScopeAtPosition(module, position);
    if (!node || !scope)
        return std::nullopt;
    if (isOperatorPosition(sourceModule, position))
        return std::nullopt;
    if (!isHoverableTokenPosition(sourceModule, exprOrLocal, node, position))
        return std::nullopt;

    std::optional<std::pair<std::string, Luau::TypeFun>> typeAliasInformation = std::nullopt;
    std::optional<Luau::TypeId> type = std::nullopt;
    std::optional<std::string> documentationSymbol = std::nullopt;
    std::optional<DocumentationLocation> documentationLocation = std::nullopt;
    std::optional<Luau::Property> hoveredProperty = std::nullopt;

    if (auto ref = node->as<Luau::AstTypeReference>())
    {
        std::string typeName;
        std::optional<Luau::TypeFun> typeFun;
        if (ref->prefix)
        {
            typeName = std::string(ref->prefix->value) + "." + ref->name.value;
            typeFun = scope->lookupImportedType(ref->prefix->value, ref->name.value);
        }
        else
        {
            typeName = ref->name.value;
            typeFun = scope->lookupType(ref->name.value);
        }
        if (!typeFun)
            return std::nullopt;
        typeAliasInformation = std::make_pair(typeName, *typeFun);
        type = typeFun->type;
        if (!ref->prefix)
            documentationSymbol = workspace.getDefinitionTypeDocumentationSymbol(scope, typeName);
    }
    else if (auto alias = node->as<Luau::AstStatTypeAlias>())
    {
        auto typeName = alias->name.value;
        auto typeFun = scope->lookupType(typeName);
        if (!typeFun)
            return std::nullopt;
        typeAliasInformation = std::make_pair(typeName, *typeFun);
        type = typeFun->type;
    }
    else if (auto typeTable = node->as<Luau::AstTypeTable>())
    {
        if (auto tableTy = module.astResolvedTypes.find(typeTable))
        {
            type = *tableTy;

            // Check if we are inside one of the properties
            for (auto& prop : typeTable->props)
            {
                if (prop.location.containsClosed(position))
                {
                    auto parentType = Luau::follow(*tableTy);
                    if (auto definitionModuleName = Luau::getDefinitionModuleName(parentType))
                        documentationLocation = {definitionModuleName.value(), prop.location};
                    auto resolvedProperty = lookupProp(parentType, prop.name.value);
                    if (resolvedProperty.size() == 1 && resolvedProperty[0].property.readTy)
                        type = resolvedProperty[0].property.readTy;
                    break;
                }
            }
        }
    }
    else if (auto astType = node->asType())
    {
        if (auto ty = module.astResolvedTypes.find(astType))
        {
            type = *ty;
        }
    }
    else if (auto local = exprOrLocal.getLocal()) // TODO: can we just use node here instead of also calling exprOrLocal?
    {
        type = scope->lookup(local);
        documentationLocation = {moduleName, local->location};
    }
    else if (auto expr = exprOrLocal.getExpr())
    {
        // Special case, we want to check if there is a parent in the ancestry, and if it is an AstTable
        // If so, and we are hovering over a prop, we want to give type info for the assigned expression to the prop
        // rather than just "string"
        auto ancestry = Luau::findAstAncestryOfPosition(sourceModule, position);
        if (ancestry.size() >= 2 && ancestry.at(ancestry.size() - 2)->is<Luau::AstExprTable>())
        {
            auto parent = ancestry.at(ancestry.size() - 2)->as<Luau::AstExprTable>();
            for (const auto& [kind, key, value] : parent->items)
            {
                if (key && key->location.contains(position))
                {
                    // Return type type of the value
                    if (auto it = module.astTypes.find(value))
                    {
                        type = *it;
                    }
                    break;
                }
            }
        }

        // Handle table properties (so that we can get documentation info)
        if (auto index = expr->as<Luau::AstExprIndexName>())
        {
            if (auto propInformation = getMemberProperties(module, *index); !propInformation.empty())
            {
                auto [baseTy, prop] = propInformation[0];
                if (propInformation.size() == 1)
                {
                    hoveredProperty = prop;
                    if (!documentationSymbol && prop.documentationSymbol)
                        documentationSymbol = prop.documentationSymbol;
                }
                if (propInformation.size() == 1 && prop.readTy)
                    type = prop.readTy;
                if (auto definitionModuleName = Luau::getDefinitionModuleName(baseTy))
                {
                    if (prop.location)
                        documentationLocation = {definitionModuleName.value(), prop.location.value()};
                    else if (prop.typeLocation)
                        documentationLocation = {definitionModuleName.value(), prop.typeLocation.value()};
                }
            }
        }

        // Handle local variables separately to retrieve documentation location info
        if (auto local = expr->as<Luau::AstExprLocal>(); !documentationLocation.has_value() && local && local->local)
        {
            documentationLocation = {moduleName, local->local->location};
        }

        if (!type)
        {
            if (auto it = module.astTypes.find(expr))
            {
                type = *it;
            }
            else if (auto global = expr->as<Luau::AstExprGlobal>())
            {
                type = scope->lookup(global->name);
            }
            else if (auto local = expr->as<Luau::AstExprLocal>())
            {
                type = scope->lookup(local->local);
            }
        }
    }

    if (!type)
        return std::nullopt;
    type = Luau::follow(*type);

    const bool literal = isLiteralPosition(sourceModule, position);
    const auto declaration = resolveDeclarationTarget(sourceModule, exprOrLocal, node, position);
    const Luau::AstLocal* hoveredLocal = exprOrLocal.getLocal();
    if (!hoveredLocal)
        if (auto localExpression = node->as<Luau::AstExprLocal>())
            hoveredLocal = localExpression->local;
    const auto initializer = getDirectMemberInitializer(sourceModule, hoveredLocal);
    const bool directMemberInitializer = initializer != nullptr;
    if (!node->asType() && !documentationSymbol && initializer)
        if (const auto properties = getMemberProperties(module, *initializer); properties.size() == 1)
            documentationSymbol = properties.front().property.documentationSymbol;
    if (!literal && !documentationSymbol)
    {
        // Bindings and properties own their documentation independently of their value type.
        if (auto binding = Luau::findBindingAtPosition(module, sourceModule, position))
            documentationSymbol = binding->documentationSymbol;
        if (!documentationSymbol)
            documentationSymbol = getTypeDocumentationSymbol(*type);
    }
    // Keep the subject's identity separate from documentation selected for one
    // overload or callback parameter. A parameter's docs do not make it the callee.
    const auto subjectSymbol = documentationSymbol;
    if (!literal)
    {
        // Luau can refine an owned callable to the selected overload. Accept that
        // refinement only within the declaration's documentation namespace.
        const auto selected = Luau::getDocumentationSymbolAtPosition(sourceModule, module, position);
        if (documentationSymbol && selected && selected->find(*documentationSymbol + "/overload/") == 0)
            documentationSymbol = selected;
        if (!documentationSymbol && selected && selected->find("/param/") != std::string::npos)
            if (const auto function = node->as<Luau::AstExprFunction>())
                for (const Luau::AstLocal* argument : function->args)
                    if (argument->location.containsClosed(position))
                        documentationSymbol = selected;
    }

    std::optional<PrintedDocumentation> documentation;
    if (!literal)
    {
        if (documentationSymbol)
            documentation = getDocumentation(workspace.client->documentation, *documentationSymbol, &workspace.client->documentationMetadata);
        if (documentation && documentation->markdown.empty() && !documentation->learnMoreLink && !documentation->readOnly)
            documentation.reset();
        const auto useComments = [&](const std::optional<std::string>& comments)
        {
            if (!documentation && comments && !comments->empty())
                documentation = PrintedDocumentation{*comments, std::nullopt};
        };
        if (!documentation)
            useComments(workspace.getDocumentationForType(*type));
        if (!documentation)
            useComments(workspace.getDocumentationForAstNode(moduleName, node, scope));
        if (!documentation && documentationLocation)
            useComments(printMoonwaveDocumentation(workspace.getComments(documentationLocation->moduleName, documentationLocation->location)));
    }
    return HoverSubject{exprOrLocal, node, scope, *type, declaration, literal, directMemberInitializer, std::move(typeAliasInformation),
        std::move(hoveredProperty), subjectSymbol, std::move(documentation)};
}
