#pragma once

#include "LSP/DocumentationParser.hpp"
#include "Luau/AstQuery.h"
#include "Luau/Scope.h"

class WorkspaceFolder;

enum struct HoverDeclarationKind
{
    None,
    Local,
    Member,
    Global,
    Type,
};

struct HoverDeclarationTarget
{
    HoverDeclarationKind kind = HoverDeclarationKind::None;
    std::string name;
    bool isConst = false;
};

// The selected subject and its documentation are resolved once, before choosing a renderer.
struct HoverSubject
{
    Luau::ExprOrLocal exprOrLocal;
    Luau::AstNode* node;
    Luau::ScopePtr scope;
    Luau::TypeId type;
    HoverDeclarationTarget declaration;
    bool literal;
    bool directMemberInitializer;
    std::optional<std::pair<std::string, Luau::TypeFun>> typeAliasInformation;
    std::optional<Luau::Property> property;
    std::optional<std::string> documentationSymbol;
    std::optional<PrintedDocumentation> documentation;
};

std::optional<HoverSubject> resolveHoverSubject(WorkspaceFolder& workspace, const Luau::ModuleName& moduleName,
    const Luau::SourceModule& sourceModule, const Luau::Module& module, const Luau::Position& position);
