#include "doctest.h"
#include "Platform/luduvo/LuduvoNotifications.hpp"
#include "Platform/luduvo/LuduvoPlatform.hpp"
#include "TempDir.h"
#include "TestClient.h"

TEST_SUITE_BEGIN("LuduvoNotifications");

TEST_CASE("notification sender appends IDs to logs popups and action prompts")
{
    TestClient client;
    const luduvo::NotificationSender sender(&client);
    const std::string expected = "Plain message\n\nNotification ID: context.project-stale";
    sender.show("context.project-stale", lsp::MessageType::Info, "Plain message");
    REQUIRE_EQ(client.notificationQueue.size(), 1);
    CHECK_EQ(client.notificationQueue[0].first, "window/logMessage");
    for (const auto& notification : client.notificationQueue)
        CHECK_EQ(notification.second->at("message"), expected);

    sender.request("context.project-stale", lsp::MessageType::Info, "Plain message", {{"Autodump"}});
    REQUIRE_EQ(client.requestQueue.size(), 2);
    CHECK_EQ(client.requestQueue[0].first, "window/showMessageRequest");
    CHECK_EQ(client.requestQueue[0].second->at("message"), expected);
    CHECK_EQ(client.requestQueue[0].second->at("actions").at(0).at("title"), "Do not show again");
    CHECK_EQ(client.requestQueue[1].second->at("actions").at(0).at("title"), "Autodump");
    CHECK_EQ(client.requestQueue[1].second->at("actions").at(1).at("title"), "Do not show again");
}

TEST_CASE("do not show again suppresses future senders without invoking the notification action")
{
    TestClient client;
    bool actionInvoked = false;
    const luduvo::NotificationSender sender(&client);
    sender.request("context.project-stale", lsp::MessageType::Info, "Stale", {{"Autodump"}},
        [&](const JsonRpcMessage&)
        {
            actionInvoked = true;
        });
    REQUIRE_EQ(client.requestHandlers.size(), 1);
    REQUIRE(client.requestHandlers[0]);
    JsonRpcMessage response;
    response.result = json{{"title", "Do not show again"}};
    (*client.requestHandlers[0])(response);
    CHECK_FALSE(actionInvoked);
    CHECK_FALSE(luduvo::NotificationSender(&client).enabled("context.project-stale"));
    REQUIRE(client.notificationQueue.back().second);
    CHECK_EQ(client.notificationQueue.back().second->at("command"), "luduvo-lsp.suppressNotification");
    CHECK_EQ(client.notificationQueue.back().second->at("data"), "context.project-stale");
    luduvo::NotificationSender(&client).show("context.project-stale", lsp::MessageType::Info, "Stale again");
    CHECK_EQ(client.requestQueue.size(), 1);
    CHECK_EQ(client.notificationQueue.back().first, "window/logMessage");
    client.setNotificationSuppressed("context.project-stale", false);
    CHECK(luduvo::NotificationSender(&client).enabled("context.project-stale"));
    TestClient otherClient;
    CHECK(luduvo::NotificationSender(&otherClient).enabled("context.project-stale"));
}

TEST_CASE("notification sender forwards ordinary actions to their existing handler")
{
    TestClient client;
    bool actionInvoked = false;
    luduvo::NotificationSender(&client).request("context.project-stale", lsp::MessageType::Info, "Stale", {{"Autodump"}},
        [&](const JsonRpcMessage& response)
        {
            actionInvoked = response.result->at("title") == "Autodump";
        });
    JsonRpcMessage response;
    response.result = json{{"title", "Autodump"}};
    REQUIRE(client.requestHandlers[0]);
    (*client.requestHandlers[0])(response);
    CHECK(actionInvoked);
    CHECK(luduvo::NotificationSender(&client).enabled("context.project-stale"));
}

TEST_CASE("notification sender suppresses popups and action prompts while retaining logs")
{
    TestClient client;
    const luduvo::NotificationSender sender(&client, {{"context.project-stale", false}});
    sender.show("context.project-stale", lsp::MessageType::Info, "Stale");
    sender.request("context.project-stale", lsp::MessageType::Info, "Stale", {{"Autodump"}});
    REQUIRE_EQ(client.notificationQueue.size(), 2);
    for (const auto& notification : client.notificationQueue)
        CHECK_EQ(notification.first, "window/logMessage");
    CHECK(client.requestQueue.empty());
    CHECK_FALSE(sender.enabled("context.project-stale"));
    CHECK_FALSE(sender.enabled("context.updated"));
    CHECK(sender.enabled("context.files-added"));
}

TEST_CASE("notification sender allows explicit opt in and user requested prompts")
{
    TestClient client;
    const luduvo::NotificationSender sender(&client, {{"context.updated", true}, {"context.refresh-required", false}});
    CHECK(sender.enabled("context.updated"));
    sender.request("context.refresh-required", lsp::MessageType::Info, "Manual command", {{"Copy"}}, std::nullopt, true);
    REQUIRE_EQ(client.requestQueue.size(), 1);
    CHECK_EQ(client.requestQueue[0].second->at("message"), "Manual command\n\nNotification ID: context.refresh-required");
    CHECK_EQ(client.requestQueue[0].second->at("actions").size(), 1);
}

TEST_CASE("definition fallback warnings honor suppression and append their ID")
{
    TempDir temp("luduvo_notification_fallback");
    TestClient client;
    ClientLuduvoConfiguration configuration;
    configuration.dataDirectory = temp.path() + "/data";
    configuration.directories.roaming = temp.path() + "/roaming";
    configuration.notifications["definitions.fallback"] = false;
    LuduvoPlatform suppressed(configuration, nullptr, nullptr);
    suppressed.reportDefinitionLoadWarnings(&client, {"missing installed definitions"});
    CHECK(client.requestQueue.empty());
    REQUIRE_EQ(client.notificationQueue.size(), 1);
    CHECK_EQ(client.notificationQueue[0].first, "window/logMessage");
    CHECK(client.notificationQueue[0].second->at("message").get<std::string>().find("Notification ID: definitions.fallback") != std::string::npos);

    configuration.notifications.clear();
    LuduvoPlatform visible(configuration, nullptr, nullptr);
    visible.reportDefinitionLoadWarnings(&client, {"missing installed definitions"});
    REQUIRE_EQ(client.requestQueue.size(), 1);
    CHECK_EQ(client.requestQueue[0].first, "window/showMessageRequest");
    CHECK(client.requestQueue[0].second->at("message").get<std::string>().find("Notification ID: definitions.fallback") != std::string::npos);
    CHECK(luduvo::NotificationSender(&client).enabled("configuration.invalid"));
    CHECK_FALSE(luduvo::NotificationSender(&client, {{"configuration.invalid", false}}).enabled("configuration.invalid"));
}

TEST_SUITE_END();
