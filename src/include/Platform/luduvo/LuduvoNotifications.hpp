#pragma once

#include "LSP/Client.hpp"

#include <map>
#include <string_view>

namespace luduvo
{

class NotificationSender
{
    Client* client;
    std::map<std::string, bool> overrides;

public:
    NotificationSender(Client* client, std::map<std::string, bool> overrides = {});
    bool enabled(std::string_view id) const;
    void log(std::string_view id, lsp::MessageType type, const std::string& message) const;
    void show(std::string_view id, lsp::MessageType type, const std::string& message) const;
    void request(std::string_view id, lsp::MessageType type, const std::string& message, const std::vector<lsp::MessageActionItem>& actions,
        const std::optional<ResponseHandler>& handler = std::nullopt, bool force = false) const;
};

} // namespace luduvo
