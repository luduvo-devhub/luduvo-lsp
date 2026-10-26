import * as vscode from "vscode";
import * as os from "os";
import { fetch } from "undici";
import {
  CloseAction,
  CloseHandlerResult,
  ErrorAction,
  ErrorHandler,
  ErrorHandlerResult,
  Executable,
  LanguageClient,
  LanguageClientOptions,
  Message,
  ServerOptions,
  TransportKind,
} from "vscode-languageclient/node";

import {
  registerComputeBytecode,
  registerComputeCodeGen,
  registerComputeCompilerRemarks,
} from "./bytecode";

import { onTypeFormattingMiddleware } from "./onTypeFormattingMiddleware";

import { registerRequireGraph } from "./requireGraph";

import { registerViewInternalSource } from "./internalSource";

import * as roblox from "./roblox";
import * as utils from "./utils";
import {
  anyFileIsMissing,
  downloadExternalFiles,
  DownloadFileDefinition,
  isExternalFile,
  outputLocationForDefinition,
  outputLocationForDocumentation,
  shouldFetchDefinitions,
} from "./definitions";

export type PlatformContext = { client: LanguageClient | undefined };
export type AddArgCallback = (
  argument: string,
  mode?: "All" | "Prod" | "Debug",
) => void;

let client: LanguageClient | undefined = undefined;
let platformContext: PlatformContext = { client: undefined };
const clientDisposables: vscode.Disposable[] = [];
const EDITOR_NAMESPACE = "luduvo-lsp";
const SERVER_NAMESPACE = "luau-lsp";

const translateServerSettingKey = (key: string) => {
  const prefix = `${SERVER_NAMESPACE}.`;
  return key.startsWith(prefix)
    ? `${EDITOR_NAMESPACE}.${key.substring(prefix.length)}`
    : key;
};

const CURRENT_FFLAGS =
  "https://clientsettingscdn.roblox.com/v1/settings/application?applicationName=PCStudioApp";
const FFLAG_KINDS = ["FFlag", "FInt", "DFFlag", "DFInt"];

type FFlags = Record<string, string>;
type FFlagsEndpoint = { applicationSettings: FFlags };

type ProjectContextProject = {
  id: string;
  name: string;
  directory: string;
  fresh: boolean;
  staleReason: string;
};

type ProjectContextInspection = {
  script: string;
  candidates: Array<ProjectContextProject & { hasSnapshot: boolean }>;
  error?: string;
};

type ProjectContextCommandResult = {
  success: boolean;
  command?: string;
  outputPath?: string;
  error?: string;
};

type ContextCleanupCandidate = {
  id: string;
  label: string;
  path: string;
};

type ProjectContextStatus = {
  enabled: boolean;
  error?: string;
  cleanup: {
    clients: ContextCleanupCandidate[];
    projects: ContextCleanupCandidate[];
    snapshots: ContextCleanupCandidate[];
  };
  catalog: {
    projects: Record<string, ProjectContextProject>;
  };
};

type ContextMutationResult = {
  success: boolean;
  affected: number;
  projectId?: string;
  error?: string;
};

const activeLuauUri = (value?: string | vscode.Uri) => {
  if (value instanceof vscode.Uri) {
    return value;
  }
  if (typeof value === "string") {
    return vscode.Uri.parse(value);
  }
  const editor = vscode.window.activeTextEditor;
  return editor?.document.languageId === "luau"
    ? editor.document.uri
    : undefined;
};

const executeLuduvoContextCommand = <T>(
  languageClient: LanguageClient,
  command: string,
  uri?: vscode.Uri,
  extra: Record<string, unknown> = {},
) =>
  languageClient.sendRequest<T>("workspace/executeCommand", {
    command,
    arguments: [{ ...(uri ? { uri: uri.toString() } : {}), ...extra }],
  });

const registerProjectContextCommands = (languageClient: LanguageClient) => {
  const requireUri = (value?: string | vscode.Uri) => {
    const uri = activeLuauUri(value);
    if (!uri) {
      vscode.window.showWarningMessage(
        "Open a Luau file before running this project-context command.",
      );
    }
    return uri;
  };

  return [
    vscode.commands.registerCommand(
      `${EDITOR_NAMESPACE}.copyText`,
      (value: string) => vscode.env.clipboard.writeText(value),
    ),
    vscode.commands.registerCommand(
      `${EDITOR_NAMESPACE}.addProjectContext`,
      async () => {
        const selected = await vscode.window.showOpenDialog({
          canSelectFiles: false,
          canSelectFolders: true,
          canSelectMany: false,
          openLabel: "Add Project to Context",
          title: "Choose a Luduvo project directory",
        });
        if (!selected?.[0]) {
          return;
        }
        const result = await executeLuduvoContextCommand<ContextMutationResult>(
          languageClient,
          "luduvo.addProjectContext",
          undefined,
          { directory: selected[0].fsPath },
        );
        if (!result.success) {
          vscode.window.showErrorMessage(
            result.error ?? "Could not add the project to Luduvo context.",
          );
          return;
        }
        vscode.window.showInformationMessage(
          `Added '${vscode.workspace.asRelativePath(selected[0], false)}' to Luduvo project context.`,
        );
      },
    ),
    vscode.commands.registerCommand(
      `${EDITOR_NAMESPACE}.inspectProjectContext`,
      async (value?: string | vscode.Uri) => {
        const uri = requireUri(value);
        if (!uri) {
          return;
        }
        const result =
          await executeLuduvoContextCommand<ProjectContextInspection>(
            languageClient,
            "luduvo.inspectProjectContext",
            uri,
          );
        const document = await vscode.workspace.openTextDocument({
          language: "json",
          content: JSON.stringify(result, null, 2),
        });
        await vscode.window.showTextDocument(document, { preview: true });
      },
    ),
    vscode.commands.registerCommand(
      `${EDITOR_NAMESPACE}.chooseProjectContext`,
      async (value?: string | vscode.Uri) => {
        const uri = requireUri(value);
        if (!uri) {
          return;
        }
        const inspection =
          await executeLuduvoContextCommand<ProjectContextInspection>(
            languageClient,
            "luduvo.inspectProjectContext",
            uri,
          );
        if (inspection.error) {
          vscode.window.showErrorMessage(inspection.error);
          return;
        }
        if (inspection.candidates.length === 0) {
          vscode.window.showInformationMessage(
            "No captured Luduvo project contains this script.",
          );
          return;
        }
        const selected = await vscode.window.showQuickPick(
          inspection.candidates.map((project) => ({
            label: project.name,
            description: project.fresh ? "current" : "stale",
            detail: project.directory,
            projectId: project.id,
          })),
          { placeHolder: "Choose the Luduvo project context for this script" },
        );
        if (selected) {
          await executeLuduvoContextCommand(
            languageClient,
            "luduvo.chooseProjectContext",
            uri,
            { projectId: selected.projectId },
          );
        }
      },
    ),
    vscode.commands.registerCommand(
      `${EDITOR_NAMESPACE}.refreshProjectContext`,
      async (value?: string | vscode.Uri) => {
        const uri = requireUri(value);
        if (!uri) {
          return;
        }
        const result =
          await executeLuduvoContextCommand<ProjectContextCommandResult>(
            languageClient,
            "luduvo.refreshProjectContext",
            uri,
          );
        if (!result.success || !result.command) {
          vscode.window.showWarningMessage(
            result.error ?? "A world.dump command could not be generated.",
          );
          return;
        }
        const action = await vscode.window.showInformationMessage(
          `Run this command in Luduvo Studio:\n${result.command}`,
          "Copy command",
          "Copy output path",
        );
        if (action === "Copy command") {
          await vscode.env.clipboard.writeText(result.command);
        } else if (action === "Copy output path" && result.outputPath) {
          await vscode.env.clipboard.writeText(result.outputPath);
        }
      },
    ),
    vscode.commands.registerCommand(
      `${EDITOR_NAMESPACE}.resetProjectContext`,
      async (value?: string | vscode.Uri) => {
        const uri = requireUri(value);
        if (!uri) {
          return;
        }
        await executeLuduvoContextCommand(
          languageClient,
          "luduvo.resetProjectContext",
          uri,
        );
        vscode.window.showInformationMessage(
          "The manual project-context choice for this script was cleared.",
        );
      },
    ),
    vscode.commands.registerCommand(
      `${EDITOR_NAMESPACE}.projectContextStatus`,
      async () => {
        const result = await executeLuduvoContextCommand<unknown>(
          languageClient,
          "luduvo.contextHelperStatus",
          activeLuauUri(),
        );
        const document = await vscode.workspace.openTextDocument({
          language: "json",
          content: JSON.stringify(result, null, 2),
        });
        await vscode.window.showTextDocument(document, { preview: true });
      },
    ),
    vscode.commands.registerCommand(
      `${EDITOR_NAMESPACE}.cleanProjectContext`,
      async () => {
        const status = await executeLuduvoContextCommand<ProjectContextStatus>(
          languageClient,
          "luduvo.contextHelperStatus",
          activeLuauUri(),
        );
        const candidates = (["client", "project", "snapshot"] as const).flatMap(
          (kind) =>
            status.cleanup[`${kind}s`].map((candidate) => ({
              label: candidate.label,
              description: kind,
              detail: candidate.path,
              candidate,
              contextKind: kind,
            })),
        );
        if (candidates.length === 0) {
          vscode.window.showInformationMessage(
            "Luduvo project context has no cleanup candidates.",
          );
          return;
        }
        const selected = await vscode.window.showQuickPick(candidates, {
          canPickMany: true,
          placeHolder: "Choose invalid context records to clean",
        });
        if (!selected || selected.length === 0) {
          return;
        }
        let affected = 0;
        for (const kind of ["client", "project", "snapshot"] as const) {
          const selectors = selected
            .filter((candidate) => candidate.contextKind === kind)
            .map((candidate) => candidate.candidate.id);
          if (selectors.length === 0) {
            continue;
          }
          const result =
            await executeLuduvoContextCommand<ContextMutationResult>(
              languageClient,
              "luduvo.cleanProjectContext",
              undefined,
              { kind, selectors },
            );
          if (!result.success) {
            vscode.window.showErrorMessage(
              result.error ?? `Could not clean ${kind} context.`,
            );
            return;
          }
          affected += result.affected;
        }
        vscode.window.showInformationMessage(
          `Cleaned ${affected} Luduvo context record${affected === 1 ? "" : "s"}.`,
        );
      },
    ),
    vscode.commands.registerCommand(
      `${EDITOR_NAMESPACE}.forgetProjectContext`,
      async () => {
        const status = await executeLuduvoContextCommand<ProjectContextStatus>(
          languageClient,
          "luduvo.contextHelperStatus",
          activeLuauUri(),
        );
        const projects = Object.values(status.catalog.projects).sort(
          (left, right) =>
            left.name.localeCompare(right.name) ||
            left.directory.localeCompare(right.directory),
        );
        const selected = await vscode.window.showQuickPick(
          projects.map((project) => ({
            label: project.name,
            description: project.fresh ? "current" : "stale",
            detail: project.directory,
            project,
          })),
          { placeHolder: "Choose cached project context to forget" },
        );
        if (!selected) {
          return;
        }
        const confirmed = await vscode.window.showWarningMessage(
          `Permanently forget cached context for '${selected.project.name}'? This does not delete its Luduvo project or source files.`,
          { modal: true },
          "Forget context",
        );
        if (confirmed !== "Forget context") {
          return;
        }
        const result = await executeLuduvoContextCommand<ContextMutationResult>(
          languageClient,
          "luduvo.forgetProjectContext",
          undefined,
          { kind: "project", selectors: [selected.project.id] },
        );
        if (!result.success) {
          vscode.window.showErrorMessage(
            result.error ?? "Could not forget project context.",
          );
          return;
        }
        vscode.window.showInformationMessage(
          `Forgot cached context for '${selected.project.name}'.`,
        );
      },
    ),
  ];
};

const getFFlags = async () => {
  return vscode.window.withProgress(
    {
      location: vscode.ProgressLocation.Window,
      title: "Luau: Fetching FFlags",
      cancellable: false,
    },
    () =>
      fetch(CURRENT_FFLAGS)
        .then((r) => r.json() as Promise<FFlagsEndpoint>)
        .then((r) => r.applicationSettings),
  );
};

const isAlphanumericUnderscore = (str: string) => {
  return /^[a-zA-Z0-9_]+$/.test(str);
};

const isCrashReportingEnabled = () => {
  const config = vscode.workspace.getConfiguration(
    `${EDITOR_NAMESPACE}.server`,
  );
  return config.get("crashReporting.enabled", false);
};

const DO_NOT_SHOW_CRASH_REPORTING_SUGGESTION_KEY =
  "doNotShowCrashReportingSuggestion";

const shouldShowEnableCrashReportsMessage = (
  context: vscode.ExtensionContext,
) => {
  return !context.globalState.get<boolean>(
    DO_NOT_SHOW_CRASH_REPORTING_SUGGESTION_KEY,
  );
};

class ClientErrorHandler implements ErrorHandler {
  private readonly restarts: number[];
  private recommendedCrashReporting: boolean = false;

  constructor(
    private context: vscode.ExtensionContext,
    private maxRestartCount: number,
  ) {
    this.restarts = [];
  }

  public error(
    _error: Error,
    _message: Message,
    count: number,
  ): ErrorHandlerResult {
    if (count && count <= 3) {
      return { action: ErrorAction.Continue };
    }
    return { action: ErrorAction.Shutdown };
  }

  public closed(): CloseHandlerResult {
    if (
      !this.recommendedCrashReporting &&
      !isCrashReportingEnabled() &&
      shouldShowEnableCrashReportsMessage(this.context)
    ) {
      this.recommendedCrashReporting = true;
      vscode.window
        .showInformationMessage(
          "The Luau Language server exited unexpected. Would you like to enable crash reporting?",
          "Enable",
          "Not now",
          "Do not show again",
        )
        .then((value) => {
          if (value === "Enable") {
            vscode.workspace
              .getConfiguration(`${EDITOR_NAMESPACE}.server`)
              .update(
                "crashReporting.enabled",
                true,
                vscode.ConfigurationTarget.Global,
              );
          } else if (value === "Do not show again") {
            this.context.globalState.update(
              DO_NOT_SHOW_CRASH_REPORTING_SUGGESTION_KEY,
              true,
            );
          }
        });
    }

    this.restarts.push(Date.now());
    if (this.restarts.length <= this.maxRestartCount) {
      return { action: CloseAction.Restart };
    } else {
      const diff = this.restarts[this.restarts.length - 1] - this.restarts[0];
      if (diff <= 3 * 60 * 1000) {
        return {
          action: CloseAction.DoNotRestart,
          message: `The Luau Language server crashed ${this.maxRestartCount + 1} times in the last 3 minutes. The server will not be restarted. See the output for more information.`,
        };
      } else {
        this.restarts.shift();
        return { action: CloseAction.Restart };
      }
    }
  }
}

const handleExternalFiles = async (
  context: vscode.ExtensionContext,
  definitionFiles: {
    [packageName: string]: string;
  },
  documentationFiles: string[],
  builtinDefinitionFiles: {
    [packageName: string]: DownloadFileDefinition;
  },
  builtinDocumentationFiles: DownloadFileDefinition[],
) => {
  const finalDefinitionFiles = new Map<string, string>();

  // Determine a list of external files to fetch
  const externalFiles: DownloadFileDefinition[] = [];
  for (let [packageName, definitionPath] of Object.entries(definitionFiles)) {
    if (isExternalFile(definitionPath)) {
      const outputLocation = outputLocationForDefinition(context, packageName);
      externalFiles.push({
        url: definitionPath,
        outputUri: outputLocation,
      });
      finalDefinitionFiles.set(packageName, outputLocation.fsPath);
    } else {
      finalDefinitionFiles.set(packageName, definitionPath);
    }
  }
  documentationFiles = documentationFiles.map((documentationPath) => {
    if (isExternalFile(documentationPath)) {
      const outputLocation = outputLocationForDocumentation(
        context,
        documentationPath,
      );
      externalFiles.push({
        url: documentationPath,
        outputUri: outputLocation,
      });
      return outputLocation.fsPath;
    } else {
      return documentationPath;
    }
  });

  for (const [packageName, downloadDefinition] of Object.entries(
    builtinDefinitionFiles,
  )) {
    if (!finalDefinitionFiles.has(packageName)) {
      externalFiles.push(downloadDefinition);
      finalDefinitionFiles.set(
        packageName,
        downloadDefinition.outputUri.fsPath,
      );
    }
  }

  if (builtinDocumentationFiles) {
    externalFiles.push(...builtinDocumentationFiles);
    documentationFiles = documentationFiles.concat(
      builtinDocumentationFiles.map((info) => info.outputUri.fsPath),
    );
  }

  if (externalFiles) {
    const mustUpdate = await anyFileIsMissing(
      externalFiles.map((info) => info.outputUri),
    );
    if (mustUpdate || shouldFetchDefinitions(context)) {
      try {
        await downloadExternalFiles(externalFiles);
      } catch (err) {
        vscode.window.showWarningMessage(
          "Failed to donwload API information: " + err,
        );
      }
    }
  }

  return {
    definitionFiles: finalDefinitionFiles,
    documentationFiles,
    externalFiles,
  };
};

const startLanguageServer = async (context: vscode.ExtensionContext) => {
  for (const disposable of clientDisposables) {
    disposable.dispose();
  }
  clientDisposables.splice(0, clientDisposables.length); // empty the list
  if (client) {
    await client.stop();
  }

  console.log("Starting Luau Language Server");

  const args = ["lsp"];
  const debugArgs = ["lsp"];

  const addArg = (argument: string, mode: "All" | "Prod" | "Debug" = "All") => {
    if (mode === "All" || mode === "Prod") {
      args.push(argument);
    }
    if (mode === "All" || mode === "Debug") {
      debugArgs.push(argument);
    }
  };

  const {
    definitions: builtinDefinitionFiles,
    documentation: builtinDocumentationFiles,
  } = await roblox.preLanguageServerStart(context);

  const typesConfig = vscode.workspace.getConfiguration(
    `${EDITOR_NAMESPACE}.types`,
  );

  // Load extra type definitions
  // TODO: deprecate and remove support of array-based definitionFiles configuration
  let definitionFilesConfig =
    typesConfig.get<{ [packageName: string]: string } | string[]>(
      "definitionFiles",
    ) ?? {};

  if (Array.isArray(definitionFilesConfig)) {
    definitionFilesConfig = Object.fromEntries(
      definitionFilesConfig.map((path, index) => ["roblox" + index, path]),
    );
  }

  const documentationFilesConfig =
    typesConfig.get<string[]>("documentationFiles") ?? [];

  const result = await handleExternalFiles(
    context,
    definitionFilesConfig,
    documentationFilesConfig,
    builtinDefinitionFiles ?? {},
    builtinDocumentationFiles ?? [],
  );

  for (let [packageName, definitionPath] of result.definitionFiles) {
    definitionPath = utils.resolvePath(definitionPath);
    let uri;
    if (vscode.workspace.workspaceFolders) {
      uri = utils.resolveUri(
        vscode.workspace.workspaceFolders[0].uri,
        definitionPath,
      );
    } else {
      uri = vscode.Uri.file(definitionPath);
    }
    if (await utils.exists(uri)) {
      addArg(`--definitions:${packageName}=${uri.fsPath}`);
    } else {
      vscode.window.showWarningMessage(
        `Definitions file '${packageName}' at ${definitionPath} does not exist, types will not be provided from this file`,
      );
    }
  }

  // Load extra documentation files
  for (let documentationPath of result.documentationFiles) {
    documentationPath = utils.resolvePath(documentationPath);
    let uri;
    if (vscode.workspace.workspaceFolders) {
      uri = utils.resolveUri(
        vscode.workspace.workspaceFolders[0].uri,
        documentationPath,
      );
    } else {
      uri = vscode.Uri.file(documentationPath);
    }
    if (await utils.exists(uri)) {
      addArg(`--docs=${uri.fsPath}`);
    } else {
      vscode.window.showWarningMessage(
        `Documentations file at ${documentationPath} does not exist`,
      );
    }
  }

  // Handle FFlags
  const fflags: FFlags = {};
  const fflagsConfig = vscode.workspace.getConfiguration(
    `${EDITOR_NAMESPACE}.fflags`,
  );

  if (!fflagsConfig.get<boolean>("enableByDefault")) {
    addArg("--no-flags-enabled");
  }

  // Sync FFlags with upstream
  if (fflagsConfig.get<boolean>("sync")) {
    try {
      const currentFlags = await getFFlags();
      if (currentFlags) {
        for (const [name, value] of Object.entries(currentFlags)) {
          for (const kind of FFLAG_KINDS) {
            if (name.startsWith(`${kind}Luau`)) {
              // Remove the "FFlag" part from the name
              fflags[name.substring(kind.length)] = value;
            }
          }
        }
      }
    } catch (err) {
      vscode.window.showWarningMessage(
        "Failed to fetch current Luau FFlags: " + err,
      );
    }
  }

  // Enable new solver
  if (fflagsConfig.get<boolean>("enableNewSolver")) {
    fflags["LuauSolverV2"] = "true";
  }

  // Handle overrides
  const overridenFFlags = fflagsConfig.get<FFlags>("override");
  if (overridenFFlags) {
    for (let [name, value] of Object.entries(overridenFFlags)) {
      if (!isAlphanumericUnderscore(name)) {
        vscode.window.showWarningMessage(
          `Invalid FFlag name: '${name}'. It can only contain alphanumeric characters`,
        );
      }

      name = name.trim();
      value = value.trim();

      // Strip kind prefix if it was included
      for (const kind of FFLAG_KINDS) {
        if (name.startsWith(`${kind}`)) {
          name = name.substring(kind.length);
        }
      }

      // Validate that the name and value is valid
      if (name.length > 0 && value.length > 0) {
        fflags[name] = value;
      }
    }
  }

  const serverConfiguration = vscode.workspace.getConfiguration(
    `${EDITOR_NAMESPACE}.server`,
  );

  const serverBinConfig = utils.resolvePath(
    serverConfiguration.get("path", "").trim(),
  );
  const serverBinUri =
    vscode.workspace.workspaceFolders &&
    vscode.workspace.workspaceFolders.length > 0
      ? utils.resolveUri(
          vscode.workspace.workspaceFolders[0].uri,
          serverBinConfig,
        )
      : vscode.Uri.file(serverBinConfig);
  let serverBinPath;

  if (serverBinConfig !== "" && (await utils.exists(serverBinUri))) {
    serverBinPath = serverBinUri.fsPath;
  } else {
    if (serverBinConfig !== "") {
      vscode.window.showWarningMessage(
        `Server binary at path \`${serverBinUri.fsPath}\` does not exist, falling back to bundled binary`,
      );
    }
    serverBinPath = vscode.Uri.joinPath(
      context.extensionUri,
      "bin",
      os.platform() === "win32" ? "server.exe" : "server",
    ).fsPath;
  }

  const transport =
    serverConfiguration.get<"stdio" | "pipe">(
      "communicationChannel",
      "stdio",
    ) === "pipe"
      ? TransportKind.pipe
      : TransportKind.stdio;

  const delayStartup = serverConfiguration.get<boolean>("delayStartup", false);
  if (delayStartup) {
    addArg("--delay-startup");
  }

  if (isCrashReportingEnabled()) {
    addArg("--enable-crash-reporting");
    addArg(
      `--crash-report-directory=${vscode.Uri.joinPath(context.globalStorageUri, "sentry").fsPath}`,
    );
  }

  // Handle base luaurc
  const baseLuaurcConfig = serverConfiguration.get<string>("baseLuaurc");
  if (baseLuaurcConfig) {
    const baseLuaurcPath = utils.resolvePath(baseLuaurcConfig);
    let uri;
    if (vscode.workspace.workspaceFolders) {
      uri = utils.resolveUri(
        vscode.workspace.workspaceFolders[0].uri,
        baseLuaurcPath,
      );
    } else {
      uri = vscode.Uri.file(baseLuaurcPath);
    }
    if (await utils.exists(uri)) {
      addArg(`--base-luaurc=${uri.fsPath}`);
    } else {
      vscode.window
        .showWarningMessage(
          `Base .luaurc file at ${baseLuaurcPath} does not exist`,
          "Configure Settings",
        )
        .then((action) => {
          if (action === "Configure Settings") {
            vscode.commands.executeCommand(
              "workbench.action.openSettings",
              `${EDITOR_NAMESPACE}.server.baseLuaurc`,
            );
          }
        });
    }
  }

  const run: Executable = {
    command: serverBinPath,
    args,
    transport,
  };

  // If debugging, run the locally build extension, with local type definitions file
  const debug: Executable = {
    command: process.env["LUAU_LSP_SERVER_PATH"]
      ? vscode.Uri.file(process.env["LUAU_LSP_SERVER_PATH"]).fsPath
      : serverBinPath,
    args: debugArgs,
    transport,
  };

  const serverOptions: ServerOptions = { run, debug };

  const clientOptions: LanguageClientOptions = {
    documentSelector: vscode.workspace
      .getConfiguration(EDITOR_NAMESPACE)
      .get<boolean>("analyzeLuaFiles", true)
      ? [
          { language: "luau", scheme: "file" },
          { language: "luau", scheme: "untitled" },
          { language: "lua", scheme: "file" },
          { language: "lua", scheme: "untitled" },
        ]
      : [
          { language: "luau", scheme: "file" },
          { language: "luau", scheme: "untitled" },
        ],
    diagnosticPullOptions: {
      onChange: vscode.workspace
        .getConfiguration(`${EDITOR_NAMESPACE}.diagnostics`)
        .get("pullOnChange", true),
      onSave: vscode.workspace
        .getConfiguration(`${EDITOR_NAMESPACE}.diagnostics`)
        .get("pullOnSave", true),
    },
    initializationOptions: {
      fflags,
      configurationSection: EDITOR_NAMESPACE,
    },
    markdown: {
      supportHtml: true,
    },
    errorHandler: new ClientErrorHandler(context, 4),
    middleware: {
      provideOnTypeFormattingEdits: onTypeFormattingMiddleware,
    },
  };

  client = new LanguageClient(
    "luau",
    "Luau Language Server",
    serverOptions,
    clientOptions,
  );

  platformContext.client = client;

  // Register commands
  client.onNotification("$/command", (params) => {
    const data =
      params.command === "workbench.action.openSettings" &&
      typeof params.data === "string"
        ? translateServerSettingKey(params.data)
        : params.data;
    vscode.commands.executeCommand(params.command, data);
  });

  clientDisposables.push(
    vscode.commands.registerCommand(
      `${EDITOR_NAMESPACE}.updateSettingAndReload`,
      async (params: { key: string; value: unknown }) => {
        const serverPrefix = `${SERVER_NAMESPACE}.`;
        const editorPrefix = `${EDITOR_NAMESPACE}.`;
        if (
          !params?.key?.startsWith(serverPrefix) &&
          !params?.key?.startsWith(editorPrefix)
        ) {
          throw new Error(`Invalid Luau LSP setting key: ${params?.key}`);
        }
        const prefix = params.key.startsWith(serverPrefix)
          ? serverPrefix
          : editorPrefix;
        const target = vscode.workspace.workspaceFolders
          ? vscode.ConfigurationTarget.Workspace
          : vscode.ConfigurationTarget.Global;
        await vscode.workspace
          .getConfiguration(EDITOR_NAMESPACE)
          .update(params.key.substring(prefix.length), params.value, target);
        await startLanguageServer(context);
      },
    ),
  );

  clientDisposables.push(
    vscode.commands.registerCommand(
      `${EDITOR_NAMESPACE}.rename`,
      async (
        uriString: string,
        position: { line: number; character: number },
      ) => {
        const uri = vscode.Uri.parse(uriString);
        const pos = new vscode.Position(position.line, position.character);
        const editor = vscode.window.activeTextEditor;
        if (editor && editor.document.uri.toString() === uri.toString()) {
          editor.selection = new vscode.Selection(pos, pos);
          await vscode.commands.executeCommand("editor.action.rename");
        }
      },
    ),
  );

  // The upstream server still emits these command identifiers. Keep the
  // translation at the client boundary while exposing only luduvo-lsp.* in
  // the extension manifest and user settings.
  clientDisposables.push(
    vscode.commands.registerCommand(
      `${SERVER_NAMESPACE}.updateSettingAndReload`,
      (params: { key: string; value: unknown }) =>
        vscode.commands.executeCommand(
          `${EDITOR_NAMESPACE}.updateSettingAndReload`,
          params,
        ),
    ),
    vscode.commands.registerCommand(
      `${SERVER_NAMESPACE}.rename`,
      (uriString: string, position: { line: number; character: number }) =>
        vscode.commands.executeCommand(
          `${EDITOR_NAMESPACE}.rename`,
          uriString,
          position,
        ),
    ),
  );

  clientDisposables.push(...registerComputeBytecode(context, client));
  clientDisposables.push(...registerComputeCompilerRemarks(context, client));
  clientDisposables.push(...registerComputeCodeGen(context, client));
  clientDisposables.push(...registerRequireGraph(context, client));
  clientDisposables.push(...registerViewInternalSource(context, client));
  clientDisposables.push(...registerProjectContextCommands(client));
  clientDisposables.push(
    vscode.commands.registerCommand(
      `${EDITOR_NAMESPACE}.openWalkthrough`,
      () => {
        return vscode.commands.executeCommand(
          "workbench.action.openWalkthrough",
          "LuduvoDeveloperHub.luduvo-lsp#getting-started",
          false,
        );
      },
    ),
  );
  clientDisposables.push(
    vscode.commands.registerCommand(
      `${EDITOR_NAMESPACE}.updateApi`,
      async () => {
        await downloadExternalFiles(result.externalFiles);
        vscode.window
          .showInformationMessage(
            "API Types have been updated, reload server to take effect.",
            "Reload Language Server",
          )
          .then((command) => {
            if (command === "Reload Language Server") {
              vscode.commands.executeCommand(
                `${EDITOR_NAMESPACE}.reloadServer`,
              );
            }
          });
      },
    ),
  );

  console.log("LSP Setup");
  await client.start();
};

export async function activate(context: vscode.ExtensionContext) {
  console.log("Luau LSP activated");

  await roblox.onActivate(platformContext, context);

  context.subscriptions.push(
    vscode.commands.registerCommand(
      `${EDITOR_NAMESPACE}.reloadServer`,
      async () => {
        vscode.window.showInformationMessage("Reloading Language Server");
        await startLanguageServer(context);
      },
    ),
    vscode.commands.registerCommand(
      `${EDITOR_NAMESPACE}.flushTimeTrace`,
      async () => {
        if (client) {
          client.sendNotification("$/flushTimeTrace");
        }
      },
    ),
  );

  context.subscriptions.push(
    vscode.workspace.onDidChangeConfiguration((e) => {
      if (e.affectsConfiguration(`${EDITOR_NAMESPACE}.server`)) {
        vscode.window
          .showInformationMessage(
            "Luau LSP server configuration has changed, reload server for this to take effect.",
            "Reload Language Server",
          )
          .then((command) => {
            if (command === "Reload Language Server") {
              vscode.commands.executeCommand(
                `${EDITOR_NAMESPACE}.reloadServer`,
              );
            }
          });
      } else if (
        e.affectsConfiguration(`${EDITOR_NAMESPACE}.fflags`) ||
        e.affectsConfiguration(
          `${EDITOR_NAMESPACE}.completion.enableFragmentAutocomplete`,
        )
      ) {
        vscode.window
          .showInformationMessage(
            "Luau FFlags have been changed, reload server for this to take effect.",
            "Reload Language Server",
          )
          .then((command) => {
            if (command === "Reload Language Server") {
              vscode.commands.executeCommand(
                `${EDITOR_NAMESPACE}.reloadServer`,
              );
            }
          });
      } else if (
        e.affectsConfiguration(`${EDITOR_NAMESPACE}.types`) ||
        e.affectsConfiguration(`${EDITOR_NAMESPACE}.platform.type`) ||
        e.affectsConfiguration(`${EDITOR_NAMESPACE}.analyzeLuaFiles`) ||
        e.affectsConfiguration(`${EDITOR_NAMESPACE}.platform.luduvo`)
      ) {
        vscode.window
          .showInformationMessage(
            "Luau type definitions have been changed, reload server for this to take effect.",
            "Reload Language Server",
          )
          .then((command) => {
            if (command === "Reload Language Server") {
              vscode.commands.executeCommand(
                `${EDITOR_NAMESPACE}.reloadServer`,
              );
            }
          });
      }
    }),
  );

  await startLanguageServer(context);

  await roblox.postLanguageServerStart(platformContext, context);
}

export async function deactivate() {
  return Promise.allSettled([
    ...roblox.onDeactivate(),
    client?.stop(),
    clientDisposables.map((disposable) => disposable.dispose()),
  ]);
}
