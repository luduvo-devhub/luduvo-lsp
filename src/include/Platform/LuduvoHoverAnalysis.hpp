#pragma once

#include "Platform/LuduvoResourceBindings.hpp"
#include "Platform/LuduvoHoverRenderer.hpp"
#include "LSP/DocumentationParser.hpp"

#include "Luau/Ast.h"
#include "Luau/AstQuery.h"
#include "Luau/Autocomplete.h"
#include "Luau/ToString.h"
#include "Luau/Type.h"

#include <optional>
#include <string>

namespace luduvo::hover
{
enum struct DeclarationKind
{
    None,
    Local,
    Member,
    Global,
    Type,
};

struct DeclarationTarget
{
    DeclarationKind kind = DeclarationKind::None;
    std::string name;
    bool isConst = false;
};

struct TypeProvenance
{
    bool server = false;
    bool client = false;
};

enum struct TypeCheckability
{
    Checkable,
    UncheckableFields,
    Uncheckable,
    Unresolved,
};

struct RichHoverInput
{
    Luau::TypeId type;
    std::string typeDefinition;
    std::optional<std::string> documentationSymbol;
    std::optional<Luau::Property> property;
    std::optional<PrintedDocumentation> documentation;
    std::optional<std::string> fallbackTitle;
    bool preferLeafTitle = false;
    bool showTypeDefinition = true;
};

DeclarationTarget getDeclarationTarget(
    const Luau::SourceModule& sourceModule, Luau::ExprOrLocal& exprOrLocal, Luau::AstNode* node, const Luau::Position& position);
std::string makeDeclaration(Luau::TypeId type, const std::string& renderedType, const std::string& expandedType, const DeclarationTarget& target);
bool isLiteralPosition(const Luau::SourceModule& sourceModule, const Luau::Position& position);
bool localHasDirectMemberInitializer(const Luau::SourceModule& sourceModule, const Luau::AstLocal* local);
std::string versionLabel(LuduvoDefinitionSide side);
void collectTypeProvenance(Luau::TypeId type, TypeProvenance& provenance);
std::string versionLabel(const TypeProvenance& provenance);
TypeCheckability getCheckability(Luau::TypeId type);
bool hasUncheckableArguments(Luau::TypeId type);
bool isDeprecated(Luau::TypeId type);
std::string normalizeRichHoverType(Luau::TypeId type, Luau::ToStringOptions& options);
std::string getRichHoverTypeDefinition(Luau::TypeId type, Luau::ToStringOptions& options);
std::optional<std::string> resolveDocumentationSymbol(
    Luau::TypeId type, const std::optional<std::string>& directDocumentationSymbol, const std::optional<std::string>& environmentName);
RichHoverRenderModel analyzeRichHover(const std::vector<LuduvoResolvedResourceBinding>& constrainedResources, const RichHoverInput& input);
} // namespace luduvo::hover
