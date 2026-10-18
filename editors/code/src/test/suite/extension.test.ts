import * as assert from "assert";
import * as vscode from "vscode";

suite("Extension Test Suite", () => {
  vscode.window.showInformationMessage("Starting all tests.");
  test("Luduvo LSP is properly loaded", () => {
    const extension = vscode.extensions.all.find(
      (candidate) => candidate.packageJSON.name === "luduvo-lsp",
    );

    assert.ok(extension, "Luduvo Language Server extension was not loaded");
    assert.strictEqual(
      extension.packageJSON.displayName,
      "Luduvo Language Server",
    );
    assert.strictEqual(extension.packageJSON.publisher, "LuduvoDeveloperHub");
    assert.match(extension.packageJSON.version, /^\d+\.\d+\.\d+$/);

    const configuration = extension.packageJSON.contributes?.configuration;
    assert.ok(configuration, "extension configuration was not registered");
    assert.ok(
      configuration.properties?.["luduvo-lsp.platform.type"],
      "Luduvo platform setting was not registered",
    );
  });

  test("contributed commands use and register the Luduvo namespace", async () => {
    const extension = vscode.extensions.all.find(
      (candidate) => candidate.packageJSON.name === "luduvo-lsp",
    );
    assert.ok(extension);

    const contributed: Array<{ command: string }> =
      extension.packageJSON.contributes?.commands ?? [];
    assert.ok(contributed.length > 0);
    assert.ok(
      contributed.every(({ command }) => command.startsWith("luduvo-lsp.")),
      "all contributed commands should use luduvo-lsp.*",
    );

    const expectedCommands = contributed.map(({ command }) => command);
    const registrationDeadline = Date.now() + 15_000;
    let registered = new Set(await vscode.commands.getCommands(true));
    while (
      expectedCommands.some((command) => !registered.has(command)) &&
      Date.now() < registrationDeadline
    ) {
      await new Promise((resolve) => setTimeout(resolve, 100));
      registered = new Set(await vscode.commands.getCommands(true));
    }

    for (const { command } of contributed) {
      assert.ok(registered.has(command), `${command} was not registered`);
    }
  });
});
