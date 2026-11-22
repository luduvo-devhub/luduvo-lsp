#pragma once

#include "LSP/ClientConfiguration.hpp"
#include "Platform/luduvo/LuduvoBindings.hpp"
#include "Protocol/Completion.hpp"

#include <optional>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

struct LuduvoResourceName;

namespace luduvo::hover
{
struct UnionSummary
{
    std::string firstMember;
    size_t additionalMembers = 0;
    bool multiline = false;
};

struct RichHoverOverload
{
    std::string declaration;
    std::optional<std::string> callStyle;
    std::vector<LuduvoResolvedResourceBinding> constrainedResources;
};

struct RichHoverRenderModel
{
    std::string title;
    std::optional<std::string> learnMoreLink;
    std::vector<std::string> tags;
    std::string documentation;
    bool deprecated = false;
    std::string deprecation;
    std::string documentedDefault;
    std::vector<std::string> notes;
    std::string example;
    std::vector<LuduvoResolvedResourceBinding> constrainedResources;
    std::vector<RichHoverOverload> overloads;
    std::string typeDefinition;
    std::optional<UnionSummary> unionSummary;
    bool showTypeDefinition = true;
    bool hasDocumentationIdentity = false;
};

std::string renderRichHover(const ClientLuduvoHoverConfiguration& config, const RichHoverRenderModel& model);
void applyResourceCompletionDocumentation(const ClientLuduvoHoverConfiguration& config, lsp::CompletionItem& item,
    std::string_view kind, const LuduvoResourceName& resource);
} // namespace luduvo::hover
