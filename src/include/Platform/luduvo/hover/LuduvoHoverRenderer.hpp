#pragma once

#include "LSP/ClientConfiguration.hpp"
#include "Platform/luduvo/LuduvoBindings.hpp"

#include <optional>
#include <cstddef>
#include <string>
#include <vector>

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
    std::string provenance;
    std::vector<std::string> tags;
    std::string documentation;
    std::vector<LuduvoResolvedResourceBinding> constrainedResources;
    std::vector<RichHoverOverload> overloads;
    std::string typeDefinition;
    std::optional<UnionSummary> unionSummary;
    bool showTypeDefinition = true;
};

std::string renderRichHover(const ClientLuduvoHoverConfiguration& config, const RichHoverRenderModel& model);
} // namespace luduvo::hover
