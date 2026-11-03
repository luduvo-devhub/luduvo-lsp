import * as assert from "assert";
import * as vscode from "vscode";
import {
  notificationActions,
  notificationEnabled,
  showNotification,
  suppressNotification,
} from "../../notifications";

suite("Notification Test Suite", () => {
  test("adds the shared suppression action once without changing existing actions", () => {
    const actions = ["Enable", "Not now"];
    assert.deepStrictEqual(notificationActions(actions), [
      "Enable",
      "Not now",
      "Do not show again",
    ]);
    assert.deepStrictEqual(actions, ["Enable", "Not now"]);
    assert.deepStrictEqual(notificationActions(["Do not show again"]), [
      "Do not show again",
    ]);
  });

  test("inherits global and category notification overrides", async function () {
    this.timeout(15_000);
    const configuration = vscode.workspace.getConfiguration("luduvo-lsp");
    const setting = "platform.luduvo.notifications";
    const original = configuration.inspect<{
      default?: boolean;
      categories?: Record<string, boolean>;
    }>(setting)?.globalValue;
    try {
      await configuration.update(
        setting,
        {
          default: false,
          categories: {
            context: true,
            ...Object.fromEntries([["context.updated", false]]),
          },
        },
        vscode.ConfigurationTarget.Global,
      );
      assert.strictEqual(notificationEnabled("context.project.stale"), true);
      assert.strictEqual(notificationEnabled("context.updated"), false);
      assert.strictEqual(
        notificationEnabled("server.crash-reporting.suggestion"),
        false,
      );
    } finally {
      await configuration.update(
        setting,
        original,
        vscode.ConfigurationTarget.Global,
      );
    }
  });

  test("saves concurrent suppressions without losing existing settings and permits reenabling", async function () {
    this.timeout(15_000);
    const configuration = vscode.workspace.getConfiguration("luduvo-lsp");
    const setting = "platform.luduvo.notifications";
    const original = configuration.inspect<{
      default?: boolean;
      categories?: Record<string, boolean>;
    }>(setting)?.globalValue;
    const firstId = "tests.notification-one";
    const secondId = "tests.notification-two";
    try {
      await configuration.update(
        setting,
        { categories: Object.fromEntries([["configuration.invalid", true]]) },
        vscode.ConfigurationTarget.Global,
      );
      assert.strictEqual(notificationEnabled("context.updated"), false);
      assert.strictEqual(
        notificationEnabled("server.crash-reporting.suggestion"),
        true,
      );
      await Promise.all([
        suppressNotification(firstId),
        suppressNotification(secondId),
      ]);
      const saved = configuration.inspect<{
        default?: boolean;
        categories?: Record<string, boolean>;
      }>(setting)?.globalValue;
      assert.strictEqual(saved?.categories?.[firstId], false);
      assert.strictEqual(saved?.categories?.[secondId], false);
      assert.strictEqual(saved?.categories?.["configuration.invalid"], true);
      assert.strictEqual(notificationEnabled(firstId), false);
      assert.strictEqual(
        await showNotification(firstId, "Suppressed"),
        undefined,
      );
      await configuration.update(
        setting,
        { ...saved, categories: { ...saved?.categories, [firstId]: true } },
        vscode.ConfigurationTarget.Global,
      );
      assert.strictEqual(notificationEnabled(firstId), true);
    } finally {
      await configuration.update(
        setting,
        original,
        vscode.ConfigurationTarget.Global,
      );
    }
  });
});
