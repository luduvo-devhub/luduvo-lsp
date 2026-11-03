#include "Platform/luduvo/LuduvoNotifications.hpp"
#include "LSP/HierarchicalSettings.hpp"

#include <algorithm>

namespace luduvo
{

static std::string formatMessage(std::string_view id, const std::string& message)
{
    return message + "\n\nNotification ID: " + std::string(id);
}

static std::string_view logCategory(std::string_view id)
{
    if (id.find("definitions.") == 0)
        return "workspace.definitions";
    if (id.find("configuration.") == 0)
        return "server.configuration";
    if (id.find("context.autodump") == 0)
        return "helper.autodump";
    if (id.find("types.export") == 0)
        return "context.exports";
    return "context";
}

NotificationSender::NotificationSender(Client* client, ClientLuduvoNotificationConfiguration overrides)
    : client(client)
    , overrides(std::move(overrides))
{
}

bool NotificationSender::enabled(std::string_view id) const
{
    if (client && client->isNotificationSuppressed(id))
        return false;
    const bool registeredDefault = id == "context.tracking.started" || id == "context.files.added" || id == "context.autodump.failed" ||
                                   id == "context.refresh.required" || id == "context.selection.required" || id == "context.error" ||
                                   id == "context.project.stale" || id == "configuration.invalid" || id == "definitions.fallback" ||
                                   id == "types.export.created" || id == "types.export.failed" || id == "types.export.conflict";
    return overrides.resolve(id, registeredDefault);
}

void NotificationSender::log(std::string_view id, lsp::MessageType type, const std::string& message) const
{
    client->log(type, logCategory(id), formatMessage(id, message));
}

void NotificationSender::show(std::string_view id, lsp::MessageType type, const std::string& message) const
{
    request(id, type, message, {});
}

void NotificationSender::request(std::string_view id, lsp::MessageType type, const std::string& message,
    const std::vector<lsp::MessageActionItem>& actions, const std::optional<ResponseHandler>& handler, bool force) const
{
    const std::string formatted = formatMessage(id, message);
    client->log(type, logCategory(id), formatted);
    if (!force && !enabled(id))
        return;
    auto availableActions = actions;
    if (!force && std::none_of(availableActions.begin(), availableActions.end(),
                      [](const auto& action)
                      {
                          return action.title == "Do not show again";
                      }))
        availableActions.push_back({"Do not show again"});
    client->sendWindowMessageRequest(type, formatted, availableActions,
        [client = client, id = std::string(id), handler, force](const JsonRpcMessage& response)
        {
            if (!force && response.result && response.result->is_object() && response.result->value("title", "") == "Do not show again")
            {
                client->setNotificationSuppressed(id, true);
                client->sendNotification("$/command", json{{"command", "luduvo-lsp.suppressNotification"}, {"data", id}});
                return;
            }
            if (handler)
                (*handler)(response);
        });
}

} // namespace luduvo
