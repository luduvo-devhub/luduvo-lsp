#pragma once

#include "LSP/TextDocument.hpp"
#include "Platform/LSPPlatform.hpp"
#include "Protocol/Completion.hpp"

#include <optional>
#include <vector>

std::optional<std::vector<lsp::CompletionItem>> completeCommentDirective(
    const TextDocument& document, const lsp::Position& position, const LSPPlatform& platform);
