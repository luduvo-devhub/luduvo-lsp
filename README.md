# Luduvo Language Server

A (very incomplete) fork of Jonny Morganz's [Luau Language Server](https://github.com/JohnnyMorganz/luau-lsp) to work with Luduvo. This readme only covers the Luduvo-specific changes, so check them out for more information on how to use this.

Hasn't been tested anywhere other than the Zed editor, but should theoretically work with other editors provided they are setup right.

## Known issues

These won't affect 99% of users, but there's a few gatchas to be aware of when using Luduvo LSP (for now):

- Make sure `diagnostics.workspace` inside your editor is set to `false` if you're using Luduvo LSP inside of the Luduvo LSP's repo. There seems to be some issues with hangs and whatnot triggering editor's crash monitor and subsequently killing the server
- Avoid combining `fflags.enable_by_default: false` with `fflags.enable_new_solver: true` unless every required companion FFlag is explicitly enabled. Weird FFlag combinations can cause the luau anyalzer to crash, and it doesn't help that Luduvo LSP requires the `SolverV2` fflag to be enabled in most cases

If your settings look something like this:

```jsonc
"fflags": {
  "enable_by_default": true,
  "enable_new_solver": true,
  "sync": false
},
"luau-lsp": {
  "diagnostics": {
    "workspace": false
  }
}
```
Then you'll probable be fine

## Luduvo definitions

<!-- luduvo-api-stats:start -->
As of Luduvo 51, the prefab section of `resources.luduvo.json`, `globalTypes.luduvo.client.d.luau`, and `globalTypes.luduvo.server.d.luau` files inside Luduvo LSP are essentially copy/pasted global type files straight from Luduvo itself (you can find them in the `prefabs/` and `defs/` directory of your Luduvo install's roaming data equivalent respectively). The components side of `resources.luduvo.json` was dumped from Luduvo's `LuduvoEditor.exe`. Currently, these files contain ~1340 entries spanning across:

```text
├── 1,080 API entries (508 documented)
│   ├── 896 Entries under 55 Global Types (325 documented)
│   │   ├── 46 Types with members (20 documented)
│   │   │   ├── 44 Other types (Query, Signal, UDim2, etc. | 18 documented)
│   │   │   │   └── 264 Members (Query.With, Signal.Connect, UDim2.Scale, etc. | 99 documented)
│   │   │   ├── The literal `Instance` type (documented)
│   │   │   └── The literal `World` type (documented)
│   │   └── 9 Types without members (AnimLayers, CloudStore, Pair, etc. | 7 documented)
│   └── 184 Entries under 18 Global Variables/Classes (183 documented)
│       ├── 12 Variables/classes with members (11 documented)
│       │   ├── 10 Other variables/classes (UDim2, Vector3, task, etc. | all documented)
│       │   │   └── 30 Members (UDim2.new, Vector3.xAxis, task.wait, etc. | all documented)
│       │   ├── `Instance` (documented)
│       │   │   ├── 1 Static constructor (`Instance.new` | documented)
│       │   │   └── 560 Members from the literal `Instance` type above (182 documented)
│       │   │       ├── 49 Instance methods (instance:AddComponent, instance:ApplyImpulse, etc. | 24 documented)
│       │   │       └── 511 Instance properties (instance.Character, instance.Activated, etc. | 158 documented)
│       │   └── `game` (undocumented)
│       │       ├── 31 Direct members (all documented)
│       │       │   ├── 19 Services (game.World, game.Input, etc. | all documented)
│       │       │   └── 12 Other symbols (game.Bool, game.EventTable, game.ToServer, etc. | all documented)
│       │       └── 121 Members across the 19 services (all documented)
│       │           ├── 104 Members under 18 table services (game.Physics.Raycast, game.Input.Key, etc. | all documented)
│       │           └── 17 Members from the literal `World` type above (game.World.Query, game.World.Each, etc. | all documented)
│       └── 6 Variables without members (EventTableDump, handles, print, self, tick, warn | all documented)
├── 229 Exposed* components (Anchored, MultiJump, etc. | 172 documented)
│   └── * = Luduvo uses more components than this, but those aren't exposed in scripts
└── 31 Prefabs (Part, Hotbar, etc. | none documented)
```

Out of said 1340 entries, there are docs for about 680 of them.
<!-- luduvo-api-stats:end -->

Luduvo also refers to different global types depending on the script's scope (client or server). Files ending in `.client.lua` or `.client.luau` receive client globals, and files ending in `.server.lua` or `.server.luau` receive server globals. Right now the default script side is set to `server` when parsing files that don't have a special suffix, but you can override this behavior in your config.

All of these entries are sourced from `luduvo-api.json`, which itself is a dump of all the references served by both Luduvo Studio editor's declaration file and the [API reference page](https://docs.luduvo.com/reference) done by `dumpLuduvoApi.py`. Once `luduvo-api.json` is created, `dumpLuduvoDocs.py` and `dumpLuduvoTypes.py` turns it into definition/documentation files usable by a LSP.

At runtime though, Luduvo LSP will opt to instead use either the files that you tell it to in your config, or the files housed in Luduvo's data directory if no override is given before resorting to the dumps made by `dumpLuduvoApi.py`. If you don't tell it where your Luduvo install is, it'll default to using your OS's default data directory (`%APPDATA%\Luduvo\Client` on Windows, `~/Library/Application Support/Luduvo/Client` on macOS, and `$XDG_DATA_HOME/Luduvo/Client` (or `~/.local/share/Luduvo/Client`) on Linux)

To refresh the installed types, the website catalog, and docs snapshot, run:

```sh
python scripts/luduvo/dumpLuduvoApi.py --generate
```

The type snapshot can also be updated without using the website reference at all by directing running:

```sh
python scripts/luduvo/dumpLuduvoTypes.py
```

## Project context (Beta)

Luduvo LSP has a helper process that captures project context from world dumps and watches Luduvo Studio's command log. If you want to try it out, set `platform.luduvo.context.enabled` to `true` and restart the LSP.

When Context is enabled, the LSP starts a helper process that imports project-associated `world.dump` outputs into a shared local database. Captures include script attachments, instances, and the world hierarchy. Commands that may change the world mark tracked context stale. Studio's log is not watched, and the helper does not attempt to identify which projects are open in Studio.

Automatic refresh uses `world.ldv.autosave` when its modification time is newer than `world.ldv`; otherwise it uses the saved world. Autosave captures run from a disposable project copy and do not replace or save your original world. The helper waits for a stable autosave read before copying it. Source files that cannot be copied safely, including symbolic links, produce a failure with the existing manual-dump fallback.

A capture reflects the selected save/autosave, not necessarily the latest changes in Studio memory. The tracking and update notifications explain this limitation; update notifications identify the source and its age. If you have made unsaved changes since then, save in Studio or choose **Redump manually** and run the provided project-specific command in your existing Studio session.

`file.save` and world-changing commands mark tracked context stale. `context.lazyRefresh` defaults to `false`, so `context.autoDump` refreshes in the background as soon as context becomes stale. Refresh requests are coalesced per project across LSP clients. Set `lazyRefresh` to `true` to schedule refresh when analysis next requests project context. Requests return immediately using available typing; world-dependent features fall back while context is stale. Script-derived schemas continue updating independently of world dumps.

`context inspect` shows world-capture and schema-index ages, export timestamps, and the selected save/autosave path and modification time. Capture age and source age are separate. External/manual dumps with no recorded source retain unknown provenance as JSON `null`.


Project context also connects scripts to the shared event-table and handle schemas described in [Project schemas](#project-schemas-and-portable-types). You can inspect project contexts, reselect context for a script, and refresh stale captures. Code actions on `game.World` and `self` offer these operations when context is stale or unresolved. Standard editor notifications report context changes according to your notification settings. Commands for context inspection and management are available via the VSCode extension as the following:

- `Luduvo: Add Project to Context`, which adds or reindexes a Luduvo project selected from a folder picker
- `Luduvo: Inspect Project Context`, which inspects the currently opened script when one is available and otherwise shows the complete stored project catalog
- `Luduvo: Choose Project Context`, which lets you select a different project context if the LSP gets it wrong
- `Luduvo: Refresh Project Context`, which starts an automatic, offscreen dump when `context.autoDump` is enabled and otherwise generates the manual `world.dump` command
- `Luduvo: Reset Project Context Choice`, which resets the current project context choice
- `Luduvo: Show Project Context Status`, which shows helper health, refresh settings, registered LSP clients, and cleanup counts
- `Luduvo: Remove Generated Type Dependencies`, which removes Luduvo LSP's generated type file from the project as well as cleans up other any references to it in the project's code (depending on which option(s) you choose)

### Context CLI

If you don't have VSCode installed, Luduvo LSP's CLI also got extended (which a lot more effort went into):

```text
luduvo-lsp context parse <world-dump.txt> [--format text|json] [--output <path>]
luduvo-lsp context inspect [--format text|json] [--output <path>]
luduvo-lsp context inspect <dump|project|script> <path> [--format text|json] [--output <path>]
luduvo-lsp context inspect project <stored-project-name>
luduvo-lsp context inspect project <--project NAME|--project-id ID|--project-directory DIRECTORY>
luduvo-lsp context inspect instance <--node-id ID|--instance-id ID|--name NAME> [project selector] [--section <name>]
luduvo-lsp context update <project|dump> <path> [--project <name>] [--project-directory <path>]
luduvo-lsp context clean [client|project|snapshot] [selectors...] [--all]
luduvo-lsp context forget [project|script] [selectors...] [--all] [--yes]
luduvo-lsp context run [--commands-log <path>] [--state-directory <directory>] [--poll-interval <milliseconds>] [--idle-timeout <seconds>]
luduvo-lsp context status [--state-directory <directory>] [--format text|json] [--output <path>]
luduvo-lsp context stop [--state-directory <directory>]
```

Commands that can access a project database also can accept your LSP config via `--settings <path>`, like with other LSP command from upstream (namely `luduvo-lsp analyze`). If you share other arguments that may conflict with the information in `--settings`, said arguments will take precedence

`run` starts the context helper and watches `--commands-log`, using the operating-system default location when omitted. It imports project-associated dumps into `--state-directory` and checks client registrations every `--poll-interval` milliseconds. If no live LSP registrations remain for `--idle-timeout` seconds, it exits.

`stop` also stops the context helper but without any of the autmoated checks

`parse` parses a dump and turns it into a snapshot of some kind (depends on what you specify). Note that merely pasring a project does not add it to the project database. Use `update` for that.

`inspect` gives information about the thing that you provide it as an input in some way (depends on what you specify). It can be:
  - all projects (if nothing other than `inspect` is provided), which reports surfscenlevel information about all stored projects including indexed scripts, captured instances, freshness, and named cleanup candidates.
- a project (via its directory, `project.json`, project name, or project ID), which reports all of its assets, its asset types, autorun scripts, attached scripts, and relavent cleanup information if files are missing and whatnot
- a script path, which reports every known project containing that script, how it was found to be apart of said project, its selected and active context, freshness, snapshot metadata, and attached world instances. Script inspection reads the shared catalog from `--state-directory` or the default helper project state directory decided by the config file or your OSes' default.
- a tracked instance's name or ID, which returns information about all tracked projects that the instance could belong to. It will do nothing if the instance's name or ID doesn't belong to a project inside of the LSP's project database. If found, it reports its basic information, its hierarchy, attached scripts, and more.
- a dump, which it reports instance, instance hierarchy, and script counts.

Both `parse` and `inspect` can display its output in a human-readable format or JSON via `--format`, then output it to some path via `--output`

`update` reindexes the project database, either by reindexing project scripts or checking for a dump to import, then reports the resulting catalog if anything changed. If the project dir given to update isn't already in the database, it will add it

`status` gives operational information about the helper, including its process, state and catalog paths, catalog generation, active Studio project, registered LSP clients, and cleanup candidate counts. Use argument-free `inspect` for the stored projects and the full cleanup-candidate list.

`clean` deletes anything that you tell it to provided that the helper has proven that its stale or otherwise invalid, such as dead client registrations deleted projects, etc. You can just clean everything all at once with `clean --all`, or clean only specific categories with `clean <category> --all`.

`forget` is like `clean`, but removes anything regardless of if the LSP thinks its stale or not. You will need a confirmation unless `--yes` is in the prompt, and it only accepts projects and scripts instead of everythig that `clean` takes in. Otherwise, its command structure is identical

Neither `clean` nor `forget` will actually delete any of your Luduvo projects/source files. It only delete's the LSP context

### LSP Features

With project context enabled, Luduvo LSP gains context for all the entities, scripts, and general world hierarchy for any given project. This enables Luduvo LSP to provide some features that have never been possible before:

#### Cross-script Event table context

Known `game.EventTable("Name")` calls can now extend the regular `EventTable` with named, typed columns that are shared across all scripts in the same project. The exception to this is if the event table name/fields are dynamic (set with a variable) or are defined multiple times in different ways in the same project (via an if statement or something similar). The LSP will do its best to infer the schema from the known event tables and combine unclear types with unions where it can, but a lot of times in those scenarios, the LSP falls back to regular `[string]: any` event table indexing. 

Another thing to note is that the LSP is still a Luau type analyzer at the end of the day, so the LSP is still privy to all the same drawbacks and shortcomings that Luau's own type solver has. For example, explicitly typing any variable with the old broad `: EventTable` annotation also widens away any specialized fields the LSP adds. If you try to access an eventColumn field with a variable and assign it to a typed variable/field, you will get a type error even if it would work in any other situation. Trying to keep type checking for a specific eventTable if its passed in as an argument (e.g. `eventTable.OnServerEvent/OnClientEvent(table: EventTable)`) is nigh impossible. To help soften this blow, Luduvo LSP bundles event tables with 2 new type functions that you can use to help yourself out: `Events<name>` (AKA `EventTableFor<name, EventTable, EventColumn>`) and `EventKeys<name>` (AKA `EventTableKeys<name>`).

`Events<name>` exposes the types the LSP uses for its own type checking for EventTables to Luau so that you can explicitly tell luau what a specific variable is suppoed to type. `name` is the name of the event table as a string literal.

`EventTableKeys<name>` provides a union of all the known event column type values inside Luduvo LSP. If the event table keys you are asking for are dynamic or otherwise incomplete/unknown, schemas fall back to `string`. If the EventTable fields are empty, it'll instead return `never` (because it'll never have any custom keys). `name` is the name of the event table as a string literal.

Both `Events<name>` and `EventKeys<name>` are automatically generated and maintained by the LSP for as long as the event table is actually registered somewhere in your codebase. To activate exporting of these types, all you have to do is find them in your autocomplete suggestions and then Luduvo LSP will automatically make the alias, create the export, and link it to your script via a require and some autogenerated type aliases that get added to the top of your script. If you get rid of all references to the event table types and delete the LSP type require, the LSP will stop exporting/maintaining the types file it generated for you.

For example, if you wanted to use the custom types for an event table named `Chat`, this is what the LSP generates for it:

```luau
-- Generated by Luduvo LSP
local LSP = require("./types/ProjectTypes")
type Events<Name> = LSP.EventTableFor<Name, EventTable, EventColumn>
type EventKeys<Name> = LSP.EventTableKeys<Name>

-- Your stuff
local events: Events<"Chat"> = game.EventTable("Chat")
local keys: EventKeys<"Chat"> = "some valid event column key (or error)"
local value: number = events[keys][1]
```

#### Cross-Script Handle Completion

While `world.dump` dumps a lot of things, handle names are not one of them. However, Luduvo LSP does the next best thing and exposes handle names that you've read/referenced in your script (`handles.Testing`, `handles["Testing"]`, and indirect handle reads through variable assignements like `local h = handles ; local h2 = h.Testing`) and adds them to the completion hints for all scripts that are reported to be attached to the same instance via project context.

To get around Luau's type inference limitations, handles come with a new `ScriptHandle` type (AKA `HandlesFor<scriptPath, typeof(handles)>`) that exports the shared handle type for a given script path. As with all Luduvo LSP types, the LSP will automatically update the types file with the context it gains from after you add it to your script. To use it, all you need to do is select the `ScriptHandle` type from inside autocomplete and Luduvo LSP will automatically set everything up for you.

For example, if you wanted to typecheck a script handle, this is what the LSP generates for it:

```luau
-- Generated by Luduvo LSP
local LSP = require("./types/ProjectTypes")
type ScriptHandles = LSP.HandlesFor<"scripts/menu.server.luau", typeof(handles)>

-- Your stuff
local shared: ScriptHandles = handles
```

## Installing & Usage

For now, there is no official "auto updater" for Luduvo LSP (largely because making one might conflict with other autoupdaters already packaged inside your LSP). Maybe one will exist in the future if I decide that I no longer care (like with what happened with platform=luduvo), but for now you will need to manually reinstall it via the instructions below.

### Via the `luduvo-lsp` extension

If you install the `luduvo-lsp` VSCode extension, it will automatically use the Luduvo LSP binary from the releases tab and apply the necessary configuration and reccomended defaults for you without you needing to do much of anything. It just works. For transparency, these are the settings that changed:

- `luau-lsp.platform.type` -> `"luduvo"`
- `luau-lsp.analyzeLuaFiles` -> `true`
- `luau-lsp.fflags.enableNewSolver` -> `true`
- `luau-lsp.sourcemap.enabled` -> `false`
- `luau-lsp.sourcemap.autogenerate` -> `false`
- `luau-lsp.diagnostics.workspace` -> `true`

You can still rechange these in your config and it will override these defaults.

Since the `luduvo-lsp` extension is not on the VSCode/VSCodium Marketplace, you will need to manually install it. Here's how to do that:

1. Download the latest release from the [releases tab](https://github.com/luduvo/luduvo-lsp/releases)
2. Extract the contents of the release archive
3. Open VSCode/VSCodium and go to `Extensions` (or `Preferences` > `Extensions`)
4. Click the `...` button in the top right corner and select `Install from VSIX...`
5. Navigate to the extracted release archive and select the `.vsix` file
6. Restart VSCode/VSCodium

Even though Luduvo LSP doesn't have auto updates, you can still use [VSIX Manager](https://marketplace.visualstudio.com/items?itemName=zokugun.vsix-manager) ([Codeium version](https://open-vsx.org/extension/zokugun/vsix-manager)) to add this repo as a custom extension source for automatic updates. A quick tutorial on that as well:

1. Download VSIX manager and make sure its running
2. Navigate to your VS Code JSON settings (not the regular settings page)
3. Add the following entry to `vsix.extensions`:
```json
{
  "vsix.extensions": [
    "github:luduvo-devhub/luduvo-lsp!luduvo-lsp"
  ]
}
```
4. Run **VSIX Manager: Adopt unmanaged extensions**
  - If you didn't install luduvo lsp beforehand, you can run **VSIX Manager: Synchronize extensions** instead
5. Run **VSIX Manager: Update extensions** anytime you want to check for updates

To *automatically* update, VSIX Manager wants you to use their accompanying [Cron Tasks](https://marketplace.visualstudio.com/items?itemName=zokugun.cron-tasks) extension ([Codeium version](https://open-vsx.org/extension/zokugun/cron-tasks)):

```json
{
  "cronTasks.tasks": [
    { "at": "0 12 * * *", "run": "vsix.update" }
  ]
}
```

One final note, though this is probably not necessary. Anywhere you see config paths start with `luau-lsp`, use `luduvo-lsp` instead. Other than that, you can use luduvo lsp just as you would if it were luau lsp.

### Via the `luau-lsp` extension

If you install Luduvo LSP or point your editor's `binary.path` to the builds in the releases tab, Luduvo will be automatically entered as the default platform provided that you don't override it with `platform.type`. You can also select it explicitly with `platform.type = "luduvo"`, or use `luau-lsp analyze --platform=luduvo path/to/script.luau`. Otherwise, its just a drag and drop replacement. The other platforms (`roblox` and `standard`) still exist and will work with minor (but theoretically invisible) changes.

Another thing to note is that while Luduvo uses Luau for its underlying engine, Luduvo generates `.client.lua` and `.server.lua` files when attaching new scripts to your game. I recommend either setting `analyzeLuaFiles` to `true` in your editor's config file, or associate those 2 file patterns with Luduvo Luau in your editor's file associations.

Finally, Luduvo's official global files use features only available under Luau's SolverV2 flag. If Luduvo LSP attempts to load official client/server definition files while `fflags.enableNewSolver` is false, the LSP reports an error and skips loading those files at all. If you set `platform.luduvo.definitions.globalPolicy` to `"definitionFilesOnly"`, Luduvo LSP won't cause any flag any issues.

For example, this is what I put in my `.zed/settings.json`:

```jsonc
{
  "file_types": {
    "Luau": [ // This is one way to get Luduvo LSP to run on Luduvo files...
      "**/*.client.lua",
      "**/*.server.lua",
      "**/scripts/*.lua"
    ]
  },
  "lsp": {
    "luau-lsp": {
      "settings": {
        "binary": {
          "path": "C:\\my\\path\\to\\luduvo-lsp.exe",
          "ignore_system_version": true
        },

        "fflags": {
          "enable_new_solver": true
        },

        "luau-lsp": {
          "analyzeLuaFiles": true, // ...and this is the other way. It's nested inside of here because these are override settings that get passed into luau-lsp directly instead of going through Zed's (slightly outdated) LSP config settings

          "platform": {
            "type": "luduvo",
            "luduvo": {
                // Put any Luduvo-specific settings here
            }
          },

          "sourcemap": {
            "enabled": false,
            "autogenerate": false
          },

          "diagnostics": {
            "workspace": true
          }
        }
      }
    }
  }
}
```

If you want to test if it works, see the example file at `examples/luduvo/test.luau`.

## Settings

Luduvo LSP introduces or otherwise changes plenty of config settings to customize its behavior. These include:

| Setting | Type | Default | Description |
|---|---|---:|---|
| `reporting.default` | reporting defaults | `{ "logs": "Info", "ui": false }` | Controls operational log verbosity. See [Logs](#logs). |
| `reporting.categories` | map of dotted categories to reporting overrides | `{}` | Overrides verbosity for individual categories and their children. |
| `platform.type` | `"standard"`, `"roblox"`, or `"luduvo"` | `"luduvo"` | Selects the platform integration. Luduvo LSP adds the `luduvo` option and makes it the default. |
| `platform.luduvo.dataDirectory` | string | `""` | Overrides Luduvo's per-user Client data directory. An empty value uses the operating-system default. |
| `platform.luduvo.directories.roaming` | string | `""` | Overrides the root of Luduvo's roaming per-user data. An empty value uses the operating-system default. This takes precedence over the legacy `dataDirectory` setting. |
| `platform.luduvo.directories.local` | string | `""` | Overrides the root of Luduvo's local per-user data. An empty value uses the operating-system default. |
| `platform.luduvo.directories.context` | string | `""` | Overrides the directory containing Luduvo LSP's project-context database. An empty value derives it from the local data directory. |
| `platform.luduvo.context.enabled` | boolean | `false` | Enables the experimental Luduvo context integration. The Release 1 preview remains opt-in. |
| `platform.luduvo.context.autoDump` | boolean | `true` | Refreshes stale context through a disposable, offscreen Luduvo Editor process when context integration is enabled. If it is disabled or fails, the LSP provides the manual `world.dump` command instead. |
| `platform.luduvo.context.lazyRefresh` | boolean | `false` | Waits until context is needed before refreshing. Set to `false` with `context.autoDump: true` to refresh tracked projects as soon as they become stale. Reported by `context status`. |
| `platform.luduvo.notifications` | `{ default?, categories }` | `{}` | Overrides notification defaults. Set `categories["context.project.stale"]` to `false` to suppress its popup without disabling tracking or refresh. See [notification suppression](#notification-suppression). |
| `platform.luduvo.definitions.serverOverride` | string | `""` | Uses the specified Luau declaration file for server scripts before trying the installed or bundled server declarations. |
| `platform.luduvo.definitions.clientOverride` | string | `""` | Uses the specified Luau declaration file for client scripts before trying the installed or bundled client declarations. |
| `platform.luduvo.definitions.defaultScriptSide` | `"server"` or `"client"` | `"server"` | Selects the declarations used by files without a recognized client or server suffix. |
| `platform.luduvo.definitions.globalPolicy` | `"luduvoOnly"`, `"definitionFilesOnly"`, `"preferLuduvo"`, `"preferDefinitionFiles"`, or `"combine"` | `"combine"` | Controls how official Luduvo declarations interact with `types.definitionFiles`. |
| `platform.luduvo.definitions.conflictWinner` | `"luduvo"` or `"definitionFiles"` | `"luduvo"` | Chooses which source owns a duplicate global when `globalPolicy` is `combine`. |
| `platform.luduvo.definitions.exposePrivateTypes` | boolean | `false` | For some reason, official Luduvo definition files purposely hides certain types. If you enable this, Luduvo LSP will force all top-level `type` aliases from Luduvo definition files into exported globals. May cause issues with type checking and Luduvo's own type checker likely won't like that you are using, so it's disabled by default. |
| `platform.luduvo.diagnostics.default` | severity | `"default"` | Default diagnostic severity; `default` preserves each rule's registered severity. |
| `platform.luduvo.diagnostics.categories` | map of dotted rule IDs/categories to severity | `{}` | Overrides individual Luduvo diagnostics with `default`, `error`, `warning`, `information`, `hint`, or `off`. |
| `platform.luduvo.hover.presentation` | `"rich"` or `"standard"` | `"rich"` | Selects Luduvo's API-aware hover layout or the upstream-compatible hover layout. Standard hovers keep documentation above the type definition. |
| `platform.luduvo.hover.maxTitleLength` | integer or `null` | `96` | Limits visible rich-hover title length while preserving provenance text. URLs do not count toward the limit; `null` disables truncation. |
| `platform.luduvo.hover.maxTypeDefinitionLength` | integer or `null` | `2000` | Limits the Luau type definition shown in rich hovers. `null` disables truncation. |
| `platform.luduvo.resources.groups` | map of resource-group names to group dictionaries | `{}` | Each group defines `dataType`, `includeBuiltins` (boolean that decides whether or not to auto-include LSP-maintained Luduvo built-in sources), and a list of source names. Missing built-in groups implicitly use their active Luduvo built-ins. Currently, only the `string` data type is supported. |
| `platform.luduvo.resources.sources` | map of source names to source dictionaries | `{}` | Defines the resources that each resource group uses. Static `values` entries may be strings or dictionaries containing `name` and optional Markdown `documentation`; a source may be shared by multiple groups. |
| `platform.luduvo.bindings` | array of binding dictionaries | `[]` | Adds some constraint/feature for custom symbols |
| `platform.luduvo.resources.completion.enabled` | boolean | `true` | Enables resource-name suggestions in resource-constrained arguments. Disabling it does not hide resource metadata from hovers or signature highlighting. |
| `platform.luduvo.resources.completion.disabledKinds` | string array | `[]` | Disables resource-name completion for the listed resource kinds. |
| `platform.luduvo.resources.completion.disabledApis` | string array | `[]` | Disables resource-name completion for the listed canonical API symbols. |

## Bindings

Bindings are a way to associate special LSP behavior with specific methods and functions. For now its usecases are somewhat limited but they'll expand in the future. While the exact binding fields will vary depending on the kind of binding you define, they all share the same 3 required fields:

1. `symbol` is the name of the thing to bind, like `MyLibrary.Spawn`, `Color3.new`, etc. It's a bit unintuitive, but this also supports bindings to functions in types like `Instance`, `World`, etc. So if you wanted `self:AddAccessory` to get a custom type, you would use `Instance.AddAccessory` as the symbol name.
  - Currently, (most) bindings will NOT work with normal functions because they are too vague and can pop up in multiple places defined differently thanks to scope-related issues. If you want to bind a local function, you must declare it in a `.d.luau` file, add it to your config via `types.definitionFiles`, then change `luduvo.definitions.globalPolicy` to `combine`. If the Luduvo declarations and your custom declarations conflict with each other, also change `luduvo.definitions.conflictWinner` to `definitionFiles`. Future releases will make this process easier, I promise.
2. `kind` is the type of binding to apply to the symbol. Right now the only bindings I made are `resource`s and `color`, but more will be added in the future.
3. `target` is the name of the parameter(s) that get affects or gets affected by your chosen binding alongside additional information that might be needed to better resolve the binding for said parameter(s). It's hard to explain what this does in words since every binding kind could use `target` in different ways. Depending on the kind of binding, this can be a single parameter name or a list of parameter names.

For example, to bind `MyLibrary.Spawn` to `coolBinding` that affects its `cool` parameter I'd write:

```json
{
    "bindings": [
      {
        "symbol": "MyLibrary.Spawn", // note that the symbol is literally just the function's name. Do not include the rest of the API path like `@luduvo/client/globaltype/MyLibrary.Spawn`
        "kind": "coolBinding",
        "target": {
          "parameter": "cool"
        }
      }
    ]
}
```

### Colors

Colors are a type of binding that provides LSP color picker for previewing and argument autofill for the functions and parameters that are binded to it. Since colors can be filled in a variety of ways, the color binding supports a special `encoding` field that allows you to specify a custom color palette to use for the color picker.

For example, an RGB-autofiller color picker is specified like this:

```json
{
  "bindings": [
    {
      "symbol": "MyLibrary.Color",
      "kind": "color",
      "target": {
        "parameters": ["r", "g", "b"]
      },
      "encoding": "rgb"
    }
  ]
}
```

While something like an Hexadecimal-autofiller color picker is specified like this:

```json
{
  "bindings": [
    {
      "symbol": "MyLibrary.Color",
      "kind": "color",
      "target": {
        "parameter": "hexColor"
      },
      "encoding": "hex"
    }
  ]
}
```
Note that you should only be mentioning the **affected** parameters in your target, as Luduvo LSP will only change those. If the target has extra parameters other than the ones mentioned, they will be ignored.

Currently, the full list of supported encodings are:

| Color Encoding | Parameters |
|---|---|
| `rgb` | `r: number(0-1)`, `g: number(0-1)`, `b: number(0-1)` |
| `rgb255` | `r: number(0-255)`, `g: number(0-255)`, `b: number(0-255)` |
| `hex` | `hexColor: string` |
| `hsv` | `hue: number(0-360)`, `saturation: number(0-1)`, `value: number(0-1)` |

### Resources

Resources supply enum-like name suggestions through bindings for function arguments, type arguments, and member/table fields. These come in handy with respect to Luduvo's API, as there are many generic strings that don't actually act as enums when they should.

To create and bind resources, there are 3 main systems in play:

#### Sources

In order to use assemble a resource, you must first tell Luduvo LSP where to find its contents. To do so, you need to give Luduvo LSP a list of providers to use for resource autocomplete called `sources`.

To make a source, you need to add them to a dictionary in `resources.sources`. Each key in the dictionary is the name of the source, and the value is another dictionary that defines how Luduvo LSP should fetch the resource values. Each source dictionary can define some `provider`, and that provider may have additional fields that define how the provider should be used. If not defined, it defaults to `static`.

Currently, all the providers are:

| Provider | Extra settings | Supplies |
|---|---|---|
| `static` | `values: ["" \| {name: "", documentation: "" }]` | A list of names with optional documentation |
| `project` | `kind: "event-table"` | Event-table names indexed from associated project scripts and attached core scripts. |
| `project` | `kind: "handle"` | Handle names observed in reads from scripts attached to the same instance as the current script. These are hints, not proof that the handles exist at runtime. |
| `project` | `kind: "script"` | Paths of all scripts used with the selected project |

Project sources require project context. For example, `{ "provider": "project", "kind": "script" }` supplies script paths to any group that includes that source. See [Project schemas](#project-schemas-and-portable-types) for indexing details.

For a static source, `values` can be plain strings or dictionaries containing `name` and optional Markdown `documentation`, which appears in the completion tooltip:

```json
{
  "resources": {
    "sources": {
      "sourcelist-name": {
        "provider": "static",
        "values": [
          {
            "name": "Test",
            "documentation": "This is my awesome test description."
          },
          "Test2 with no documentation"
        ]
      }
    }
  }
}
```

#### Groups

After defining your sources, you can define groups that apply those sources to your code. When making a group, you need to specify 3 things:

- The name of the group via naming the dictionary key that references the group the group's name
- The `sources` the group is using, and optionally `includeBuiltins` if the group is meant to build on top of existing built-in resources with the same name. An empty source list with `includeBuiltins` set to `false` intentionally provides no names, so if you need to "disable" a resource, you can.
- The `dataType` the group is using, which can only be a `string` for now

These 3-4ish settings must go into a new dictionary inside `resources` called `groups`. For example:

```json
{
  "resources": {
    "groups": {
      "group-name": {
        "sources": [
          "sourcelist-name"
        ],
        "dataType": "string",
        "includeBuiltins": false // Unless you're extending built-in resources, you normally don't even need to include this in your config
      }
    }
  }
}
```

When you need to apply custom resources to your code, these are the names that you need to refer to your resource as so remember them good. You will never refer to a resource by its source name-- it has to be this.

If you ever want to modify the built-in groups Luduvo LSP comes with, this is the full list:

- `component`, which contains all of the names for the components in Luduvo
- `prefab`, which contains all of the names for the prefabs in Luduvo
- `event-table`, which contains all of the names for the event tables in your project
- `handle`, which contains all of the names for the handles in your project
- `script`, which contains all of the names for the scripts in your project

The last three groups uses sources with the `project` provider, so they won't normally show up unless you have project context enabled. To overwrite these builtin groups, just define a new group with one of these names and it'll override that built-in group. Set `includeBuiltins: true` if you are looking to extend the builtins and not override them completely.

#### Bindings

Finally, after defining your resource groups, you need to tell Luduvo LSP how to bind them to your code. This would be where the `bindings` come in. To use resource bindings, you need to define the new binding kind as `resource`, then specify the group name and the parameter name to bind it to like so:

```json
"bindings": [
  {
    "symbol": "MyLibrary.Spawn",
    "kind": "resource",
    "target": {
      "parameter": "name" // The name of the parameter in the symbol's signature
    },
    "group": "prefab" // The name of the resource group that needs to be binded to this symbol
  }
]
```

Bindings also support `target.kind`, which tells the LSP how to resolve the binding for a given symbol. The values are `callParameter` (for functions and is the default if `target.kind` is not specified), `typeArgument` (for type functions), and `member` (for table/class fields). `callParameter` targets will use the `parameter` setting you see above, but other `target.kind` values won't. For example, `typeArgument` targets will use the `index` setting instead, while `member` targets won't have a special setting at all. For example:

```json
{ "symbol": "MyAwesomeType", "kind": "resource", "group": "event-table", "target": { "kind": "typeArgument", "index": 1 } }
```

Note that the built-in `event-table`, `handle`, and `script` groups require that project context is enabled for you to get anything out of them.

Resource bindings intentionally only take one parameter name at a time because different parameters might want different resource groups to bind to. As such, you need to make a separate binding for each parameter:

```json
"bindings": [
  {
    "symbol": "MyLibrary.Spawn",
    "kind": "resource",
    "target": {
      "parameter": "name" // The name of the parameter in the symbol's signature
    },
    "group": "test" // The name of the resource group that needs to be binded to this symbol
  },
  {
    "symbol": "MyLibrary.Spawn",
    "kind": "resource",
    "target": {
      "parameter": "newName" // A different name in the same symbol's signature
    },
    "group": "test2"
  }
]
```
There is a chance that said parameter is a varadic, or accepts multiple values than just one. If so, you can specify it via a `varadic` field in the `target` section like so:

```json
"bindings": [
  {
    "symbol": "MyLibrary.Spawn",
    "kind": "resource",
    "target": {
      "parameter": "name", // The name of the parameter in the symbol's signature
      "varadic": true // Whether the parameter accepts multiple values
    },
    "group": "test" // The name of the resource group that needs to be binded to this symbol
  }
]
```
This should go without saying, but there are a few more things to keep in mind when configuring resource bindings to a specific parameter:

- The parameter name must match the name of the parameter in the symbol's signature (its case sensitive)
- The binded parameter must match the `dataType` of the resource group it is being binded to (which again is just `string` for now)
- The last binding defined for a given symbol/parameter combo will overwrite any previous bindings (including bindings that are predefined internally by Luduvo LSP)

Once this is done, Luduvo LSP will automatically update any tooltip references to this parameter in your code. For grammar reasons, you might also need to tell Luduvo that the parameter is a plural noun:

```json
"bindings": [
  {
    "symbol": "MyLibrary.Spawn",
    "kind": "resource",
    "target": {
      "parameter": "name", // The name of the parameter in the symbol's signature
      "varadic": true, // Whether the parameter accepts multiple values
    },
    "group": "test", // The name of the resource group that needs to be binded to this symbol
    "pluralResourceNoun": true
  }
]
```

To combine everything together, take this for example that extends the built-in prefabs with your own project prefabs, create a custom "animation-state" group, and mark a custom function parameter as accepting prefab names:

```jsonc
{
  "platform": {
    "luduvo": {
      "resources": {
        "groups": {
          "prefab": {
            "dataType": "string",
            "includeBuiltins": true,
            "sources": ["project-prefabs"]
          },
          "animation-state": {
            "dataType": "string",
            "sources": ["default-states"]
          }
        },
        "sources": {
          "project-prefabs": {
            "provider": "static",
            "values": [
              {
                "name": "MyProjectPrefab",
                "documentation": "This is my awesome prefab description."
              }
            ]
          },
          "default-states": {
            "provider": "static",
            "values": ["Idle", "Running"]
          }
        }
      },
      "bindings": [
          {
            "symbol": "MyLibrary.Spawn", // note that the symbol is literally just the function's name. Do not include the rest of the API path like `@luduvo/client/globaltype/MyLibrary.Spawn`
            "kind": "resource",
            "target": {
              "parameter": "name"
            },
            "group": "prefab"
          }
      ]
    }
  }
}
```

## Diagnostics & Linting

Luduvo LSP comes packed with its own custom error diagnostics that lint a select few runtime errors that would normally only trigger when running the project in a live game. So far, these are the ones that have been implemented:

| Rule ID | Default Severity | Trigger Condition |
|---|---|---|
| `script.autorun-self-access` | error | Attempting to use Luduvo's `self` inside an `autorun` directory, where Luduvo supplies `nil`. Should not trigger if you redefine `self` locally |
| `world.empty-query` | error | Calling `game.World.Query` or `World.Query` without any expected arguments (a string component name or `Pair`) |
| `event-table.write` | error | Writing EventTable members or EventColumn rows. |
| `event-table.fields.count` | error | Using more than 24 fields in an event-table |
| `event-table.fields.shape` | error | Not formatting the fields for an event-table in the correct `{{"field", game.EventTable.Type}}` shape |
| `event-table.fields.empty-name` | error | Having empty string(s) as an event-table field name |
| `event-table.fields.name-length` | error | Writing a table field name that exceeds 31 bytes (usually that's 31 total characters but some characters can take up more than 1 byte) |
| `event-table.fields.reserved-name` | error | Using count, sender, tick, Push, PushTo, Rows, ToServer, ToClients, F32, I32, U8, Bool, Vec3, Color, Instance, Entity, String, FireServer, FireClient, FireAllClients, OnServerEvent, or OnClientEvent as an event-table field name |
| `event-table.fields.duplicate` | error | Using the same field name twice in an event table field |
| `event-table.unknown-column` | error | Accessing an undeclared literal column on an event table whose inline column lists are fully indexed. Inherited API members remain available; incomplete or aliased schema lists retain an open fallback |
| `directive.invalid-nolint` | warning | Referring to an unknown Luduvo diagnostic rule in a `--!nolint luduvo/...` directive, or slightly mispelling it to the point that the LSP can guess what your intent was. Any lint that doesn't start with `luduvo/` won't get this lint and instead gets the default upstream Luau hint |

If you feel the need, severities can be changed without globally disabling every Luduvo diagnostic:

```jsonc
{
  "platform": {
    "luduvo": {
      "diagnostics": {
        "default": "default", // Keep the LSP-assigned default severities for all diagnostics not specified in categories
        "categories": {
          "script": "warning", // All script diagnostics are treated as warnings...
          "script.autorun-self-access": "off" // ... Save for self-access diagnostics, which are disabled entirely
        }
      }
    }
  }
}
```

Supported severities are `default`, `error`, `warning`, `information`, `hint`, and `off`. Category keys work somewhat like a collasped dictionary, with dotted IDs representing how far nested you are when modifying severities. The most specific matching entry wins, followed by `diagnostics.default`, followed by the builtin default severities shown in the table above.

Just like normal lints, Luduvo lints can also be suppressed via `--!nolint`:

```luau
--!nolint luduvo/world.empty-query
```

Luduvo nolint arguments are start with `luduvo/`, but note that diagnostic IDs used in your config omit that prefix. Nolint arguments can only specify individual rule IDs for now. Only 1 rule per line is supported. A bare `--!nolint` suppresses all Luduvo lints alongside Luau's built-in ones

## Notification suppression

Since context notifications can get annoying, Luduvo can now selectively suppress them through `platform.luduvo.notifications`. For example:

```json
{
  "platform": {
    "luduvo": {
      "notifications": {
        "default": false, // Every other notification not specified in categories is disabled by default
        "categories": {
          "context": true, // All context notifications are enabled...
          "context.autodump.started": false // ...except for the autodump notification, which is disabled/suppressed
        }
      }
    }
  }
}
```

Keys inside `categories` can be notification IDs or dotted categories such as `context`, `types`, `server`, `definitions`, and `configuration`. Outside that, the config works identically to diagnostic's own `categories` config so there's no need to repeat how it works here.

Below is the full list of notification IDs and their supression defaults:

| Notification ID | Default | Appears when... |
|---|---|---|
| `configuration.invalid` | On | Invalid Luduvo diagnostics, bindings, or resource settings are ignored |
| `definitions.fallback` | On | Installed or overridden Luduvo definitions cannot be loaded and fallback definitions are used |
| `types.export.created` | On | The helper creates a referenced generated project type module. |
| `types.export.updated` | Off | Saved schemas change a generated module. |
| `types.export.failed` | On | An active type export cannot be written. |
| `types.export.conflict` | On | A user-owned file occupies the generated module path. |
| `types.export.stopped` | Off | The last generated module reference disappears. |
| `server.crash-reporting.suggestion` | On | The VS Code extension crashes after an unexpected exit, though only if the binary its using has reporting support. For Luduvo LSP specifically, you will likely never naturally see it |
| `context.tracking.started` | On | The first live LSP begins tracking a project |
| `context.tracking.stopped` | Off | The last live LSP stops tracking a project during a registration update. A forcibly terminated LSP cannot send this message |
| `context.project.stale` | On | A tracked project becomes stale. Indicates when a background refresh is starting and offers Autodump and Redump manually |
| `context.files.added` | On | Newly inferred script attachments are added to context. Multiple additions may be grouped into one message |
| `context.autodump.started` | Off | An offscreen automatic dump starts |
| `context.updated` | Off | A new world dump replaces captured project context. Includes save/autosave provenance and source age, with a Redump manually action |
| `context.autodump.failed` | On | An automatic dump fails. Includes the manual `world.dump` command as a fallback |
| `context.refresh.required` | On | Fresh context is needed for whatever action you are about to preform but said project's content is stale. Also will appear if automatic dumps are enabled and it can't refresh on its own for whatever reason |
| `context.selection.required` | On | Luduvo LSP doesn't know which project context to use for an opened script that belongs to multiple projects |
| `context.state` | Off | A context-state check reports that no project matches or that the selected capture is current |
| `context.error` | On | Some error occurs while reading context or updating project tracking |

VSCode users also get a "Do not show again" option to do all of this automatically. Lucky them.

## Building & Debugging

Since I'm on Windows and I don't have a lot of disk space, I built this with MinGW GCC, CMake, and Ninja:

```sh
cmake -S . -B build/luduvo/[platform] -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++ -DLSP_WERROR=OFF
cmake --build build/luduvo/[platform] --target Luau.LanguageServer.CLI Luau.LanguageServer.Test -j 2
```

I was able to build successfully on both Windows and Linux (via WSL). Using MSVC's equivalents should still work fine as well, but mileage will vary. 

If you need a quick build, configure with
`-DCMAKE_CXX_FLAGS_RELEASE="-O0 -DNDEBUG"`.

If you are on Windows and test new Luduvo LSP builds directly from the build directory like I do, you may also find `-DLSP_KILL_BEFORE_LINK=ON` helpful. Especially since killing Luduvo LSP through Zed's LSP process killer won't also kill the helper process, meaning that builds still fail on Windows with permission errors. If on, this flag stops that by killing all running processes of it everywhere. This wont do anything on macOS or Linux because they don't have this problem (lucky you)

On final note that pertains to crash reporting: You have to provide your own Sentry DSN now. Alongside `LSP_BUILD_WITH_SENTRY=ON`, you also need `LSP_SENTRY_DSN` and a URL to your own Sentry project. 

Runtime reporting still requires the user to enable `luduvo-lsp.server.crashReporting.enabled` or pass `lsp --enable-crash-reporting`

### Logs

Functionally, Luduvo logs work the same as logs from any other LSP (outside of the helper, which writes its logs to stderr because stdout is already being use to spit out outputs). However, you can configure how verbose the logs are and what the logs talk about in `luau-lsp.reporting` in your config:

```json
"reporting": {
  "default": { "logs": "Info", "ui": false },
  "categories": {
    "helper": { "logs": "Debug" }, // All helper logs get reported with debug-level verbosity, but...
    "helper.autodump": { "logs": "Verbose" }, // helper.autodump logs specifically get reported with verbose verbosity
    "context.schemas": { "logs": "Debug", "ui": true } // and context.schemas logs get reported with debug ui appended to hovers
  }
}
```

Note that notification suppression is separate from logging, so disabling logging for something like `helper.autodump` won't make autodump notifications go away. You'll have to set both to off to achieve that behavior.
This is probably the only Luduvo-specific setting that isn't inside the `luduvo` platform settings, since EVERY part of luau needed to be hooked into Luduvo's log wrapper or else the log file will look weird and work haphazardly.

Reporting's dot-separated category names work exactly the same as diagnostics and notification's `category` structure, where the most specific category name takes precedence over the ones before it.

Each reporting entry independently controls `logs` and `ui`. UI debugging can be enabled without enabling debug logs, and vice versa. If you enable UI debugging for a category, hover and autocomplete markdown UI will get debug information relating to that specific setting appended to the bottom of it.

Log levels are `Off`, `Error`, `Warning`, `Info`, `Debug`, and `Verbose` (all case-insensitive). Debug-level logs and higher can contain local paths and editor output, so if you plan on sharing your logs and you care about things like that, be sure to sanitize them before sharing. Source files, full world dumps, and protocol payloads are not included normal logging. 

Here is a table of all the categories and what they generally log:

| Categories | Events |
| --- | --- |
| `server.lifecycle`, `server.configuration`, `server.communication` | Server lifecycle, settings, communication failures |
| `workspace.files`, `workspace.definitions` | File watching, definition loading and fallback |
| `bindings.resources`, `bindings.colors` | Resource and color binding configuration and resolution |
| `analysis`, `analysis.diagnostics` | Analysis failures, diagnostics, analysis timing |
| `requests.hover`, `requests.completion`, `requests.signatureHelp`, `requests` | Request timing and failures; operation-specific decisions where instrumented |
| `context.tracking`, `context.snapshots`, `context.refresh`, `context.schemas`, `context.exports` | LSP context tracking, snapshot updates, freshness and refresh coordination |
| `helper.lifecycle`, `helper.clients`, `helper.commands`, `helper.storage`, `helper.cleanup` | Shared helper lifecycle, registrations, command watching, storage, cleanup |
| `helper.autodump`, `helper.autodump.process`, `helper.autodump.autosaves` | Dump lifecycle, editor launch/exit/output, autosave preservation |
| `other` | Events without a more specific category |

CLI commands accept `--log-level [level]`, which overrides category settings for that run. Context commands also respect `reporting` settings if you pass in your config with `--settings`. Since all running LSPs share the same helper process, helper events belonging to a specific LSP process are relayed sent back to said LSP, where each LSP process will apply its own logging settings.

Finally, note that `reporting` doesn't control notifications and general notification suppression. If you need to change that, do so in the dedicated `notifications` section of your config

## Current limitations

Resource-name completion does not yet discover custom components or prefabs from within your project (but it will for prefabs soon). For now, you will need to define project-specific static sources through `resources.sources` and attach them through `resources.groups`.

Dynamic component fields are currently `any` instead of a more specific type. While queryable component names are generated from the current engine snapshot, there's no smart detection for any column to hone type detection to only the components you queried for. I'm aiming to improve this in a later version, and hopefully it'll be done in a couple releases from now.
