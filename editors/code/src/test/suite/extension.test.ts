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
});
