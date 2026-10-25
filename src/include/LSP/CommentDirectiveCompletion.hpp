#pragma once

#include "LSP/TextDocument.hpp"
#include "Platform/LSPPlatform.hpp"
#include "Protocol/Completion.hpp"
#include "Luau/Module.h"

#include <optional>
#include <vector>

std::optional<std::vector<lsp::CompletionItem>> completeCommentDirective(
    const TextDocument& document, const Luau::SourceModule& sourceModule, const lsp::Position& position, const LSPPlatform& platform);
