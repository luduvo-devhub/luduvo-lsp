import * as vscode from "vscode";
import { LanguageClient, WorkspaceEdit } from "vscode-languageclient/node";

/** Editor conveniences over the same commands used by code actions and other clients. */
export function registerProjectSchemaCommands(client: LanguageClient) {
  const activeUri = () => {
    const document = vscode.window.activeTextEditor?.document;
    return document?.languageId === "luau"
      ? document.uri.toString()
      : undefined;
  };
  return [
    vscode.commands.registerCommand(
      "luduvo-lsp.inspectProjectSchemas",
      async () => {
        const uri = activeUri();
        if (!uri) {
          return;
        }
        const result = await client.sendRequest<{ error?: string }>(
          "workspace/executeCommand",
          {
            command: "luduvo.inspectProjectSchemas",
            arguments: [{ uri }],
          },
        );
        if (result.error) {
          await vscode.window.showWarningMessage(result.error);
        }
      },
    ),
    vscode.commands.registerCommand(
      "luduvo-lsp.removeGeneratedTypes",
      async () => {
        const uri = activeUri();
        if (!uri) {
          return;
        }
        const scope = await vscode.window.showQuickPick(
          [
            { label: "This script", value: "script" },
            { label: "This project", value: "project" },
          ],
          { title: "Remove generated type dependencies" },
        );
        if (!scope) {
          return;
        }
        const families = await vscode.window.showQuickPick(
          [
            { label: "Event-table types", value: "event-table", picked: true },
            { label: "Handle types", value: "handle", picked: true },
          ],
          {
            title: "Choose type families (use Check All to select both)",
            canPickMany: true,
          },
        );
        if (!families?.length) {
          return;
        }
        const result = await client.sendRequest<{
          edit?: WorkspaceEdit;
          warnings?: string[];
          error?: string;
        }>("workspace/executeCommand", {
          command: "luduvo.removeGeneratedTypes",
          arguments: [
            {
              uri,
              scope: scope.value,
              families: families.map((item) => item.value),
            },
          ],
        });
        if (result.error || !result.edit) {
          await vscode.window.showWarningMessage(
            result.error ?? "Generated type edits are unavailable.",
          );
          return;
        }
        if (result.warnings?.length) {
          await vscode.window.showWarningMessage(result.warnings.join("\n"));
          return;
        }
        const applied = await vscode.workspace.applyEdit(
          await client.protocol2CodeConverter.asWorkspaceEdit(result.edit),
        );
        if (!applied) {
          await vscode.window.showWarningMessage(
            "The generated type edits could not be applied. Files may have changed; run the command again.",
          );
        }
      },
    ),
  ];
}
