import * as vscode from "vscode";

const namespace = "luduvo-lsp";
const setting = "platform.luduvo.notifications";
const doNotShowAgain = "Do not show again";
const suppressedInSession = new Set<string>();
let pendingUpdate: Promise<void> = Promise.resolve();

export function notificationActions(actions: readonly string[]): string[] {
  return [
    ...actions.filter((action) => action !== doNotShowAgain),
    doNotShowAgain,
  ];
}

export function notificationEnabled(id: string): boolean {
  if (suppressedInSession.has(id)) {
    return false;
  }
  const overrides = vscode.workspace
    .getConfiguration(namespace)
    .get<Record<string, boolean>>(setting, {});
  const defaults = vscode.extensions.getExtension(
    "LuduvoDeveloperHub.luduvo-lsp",
  )?.packageJSON.contributes.configuration.properties[`${namespace}.${setting}`]
    ?.properties?.[id];
  return overrides[id] ?? defaults?.default ?? true;
}

export function suppressNotification(id: string): Promise<void> {
  if (!/^[a-zA-Z0-9_.-]+$/.test(id)) {
    return Promise.reject(new Error("Invalid notification ID"));
  }
  suppressedInSession.add(id);
  const update = pendingUpdate.then(async () => {
    const configuration = vscode.workspace.getConfiguration(namespace);
    const inspected = configuration.inspect<Record<string, boolean>>(setting);
    const useWorkspace = Object.prototype.hasOwnProperty.call(
      inspected?.workspaceValue ?? {},
      id,
    );
    const values = useWorkspace
      ? inspected?.workspaceValue
      : inspected?.globalValue;
    await configuration.update(
      setting,
      { ...values, [id]: false },
      useWorkspace
        ? vscode.ConfigurationTarget.Workspace
        : vscode.ConfigurationTarget.Global,
    );
    suppressedInSession.delete(id);
  });
  pendingUpdate = update.catch(() => undefined);
  return update;
}

export async function showNotification(
  id: string,
  message: string,
  actions: readonly string[] = [],
): Promise<string | undefined> {
  const formatted = `${message}\n\nNotification ID: ${id}`;
  console.info(formatted);
  if (!notificationEnabled(id)) {
    return undefined;
  }
  const selected = await vscode.window.showInformationMessage(
    formatted,
    ...notificationActions(actions),
  );
  if (selected === doNotShowAgain) {
    try {
      await suppressNotification(id);
    } catch (error) {
      console.error("Could not save notification suppression", error);
    }
    return undefined;
  }
  return selected;
}
