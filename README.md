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

Currently, the LSP doesn't do much with this information (which is why its off by default). Right now, only the lifecycle and project management parts are hooked into the LSP. That means that you can inspect project contexts, reselect project contexts if the LSP gets it wrong for the file its linting, automatic stale-context warnings and instructions, and VSCode code actions for all of this on `game.World` and `self` if the LSP detects that the project is stale or otherwise unresolved. The LSP also sends standard editor notifications when a new world dump updates context or an attached script is inferred. Commands for context inspection and management are available via the VSCode extension as the following:

- `Luduvo: Add Project to Context`, which adds or reindexes a Luduvo project selected from a folder picker
- `Luduvo: Inspect Project Context`, which inspects the currently opened script when one is available and otherwise shows the complete stored project catalog
- `Luduvo: Choose Project Context`, which lets you select a different project context if the LSP gets it wrong
- `Luduvo: Refresh Project Context`, which starts an automatic, offscreen dump when `context.autoDump` is enabled and otherwise generates the manual `world.dump` command
- `Luduvo: Reset Project Context Choice`, which resets the current project context choice
- `Luduvo: Show Project Context Status`, which shows helper health, refresh settings, registered LSP clients, and cleanup counts

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
| `platform.type` | `"standard"`, `"roblox"`, or `"luduvo"` | `"luduvo"` | Selects the platform integration. Luduvo LSP adds the `luduvo` option and makes it the default. |
| `platform.luduvo.dataDirectory` | string | `""` | Overrides Luduvo's per-user Client data directory. An empty value uses the operating-system default. |
| `platform.luduvo.directories.roaming` | string | `""` | Overrides the root of Luduvo's roaming per-user data. An empty value uses the operating-system default. This takes precedence over the legacy `dataDirectory` setting. |
| `platform.luduvo.directories.local` | string | `""` | Overrides the root of Luduvo's local per-user data. An empty value uses the operating-system default. |
| `platform.luduvo.directories.context` | string | `""` | Overrides the directory containing Luduvo LSP's project-context database. An empty value derives it from the local data directory. |
| `platform.luduvo.context.enabled` | boolean | `false` | Enables the experimental Luduvo context integration. The Release 1 preview remains opt-in. |
| `platform.luduvo.context.autoDump` | boolean | `true` | Refreshes stale context through a disposable, offscreen Luduvo Editor process when context integration is enabled. If it is disabled or fails, the LSP provides the manual `world.dump` command instead. |
| `platform.luduvo.context.lazyRefresh` | boolean | `true` | Waits until context is needed before refreshing. Set to `false` with `context.autoDump: true` to refresh tracked projects as soon as they become stale. Reported by `context status`. |
| `platform.luduvo.notifications` | map of notification IDs to booleans | `{}` | Overrides notification defaults. Set an ID such as `context.project-stale` to `false` to suppress its popup without disabling tracking or refresh. See the notification table above. |
| `platform.luduvo.definitions.serverOverride` | string | `""` | Uses the specified Luau declaration file for server scripts before trying the installed or bundled server declarations. |
| `platform.luduvo.definitions.clientOverride` | string | `""` | Uses the specified Luau declaration file for client scripts before trying the installed or bundled client declarations. |
| `platform.luduvo.definitions.defaultScriptSide` | `"server"` or `"client"` | `"server"` | Selects the declarations used by files without a recognized client or server suffix. |
| `platform.luduvo.definitions.globalPolicy` | `"luduvoOnly"`, `"definitionFilesOnly"`, `"preferLuduvo"`, `"preferDefinitionFiles"`, or `"combine"` | `"combine"` | Controls how official Luduvo declarations interact with `types.definitionFiles`. |
| `platform.luduvo.definitions.conflictWinner` | `"luduvo"` or `"definitionFiles"` | `"luduvo"` | Chooses which source owns a duplicate global when `globalPolicy` is `combine`. |
| `platform.luduvo.definitions.exposePrivateTypes` | boolean | `false` | For some reason, official Luduvo definition files purposely hides certain types. If you enable this, Luduvo LSP will force all top-level `type` aliases from Luduvo definition files into exported globals. May cause issues with type checking and Luduvo's own type checker likely won't like that you are using, so it's disabled by default. |
| `platform.luduvo.diagnostics.enabled` | boolean | `true` | Globally enables diagnostics for Luduvo-specific runtime rules |
| `platform.luduvo.diagnostics.rules` | map of rule IDs to severity | `{}` | Overrides individual Luduvo diagnostics with `default`, `error`, `warning`, `information`, `hint`, or `off`. |
| `platform.luduvo.hover.presentation` | `"rich"` or `"standard"` | `"rich"` | Selects Luduvo's API-aware hover layout or the upstream-compatible hover layout. Standard hovers keep documentation above the type definition. |
| `platform.luduvo.hover.maxTitleLength` | integer or `null` | `96` | Limits visible rich-hover title length while preserving provenance text. URLs do not count toward the limit; `null` disables truncation. |
| `platform.luduvo.hover.maxTypeDefinitionLength` | integer or `null` | `2000` | Limits the Luau type definition shown in rich hovers. `null` disables truncation. |
| `platform.luduvo.resources.groups` | map of resource-group names to group dictionaries | `{}` | Each group defines `dataType`, `includeBuiltins` (boolean that decides whether or not to auto-include LSP-maintained Luduvo built-in sources), and a list of source names. Missing built-in groups implicitly use their active Luduvo built-ins. Currently, only the `string` data type is supported. |
| `platform.luduvo.resources.sources` | map of source names to source dictionaries | `{}` | Defines the resources that each resource group uses. Static `values` entries may be strings or dictionaries containing `name` and optional Markdown `documentation`; a source may be shared by multiple groups. |
| `platform.luduvo.bindings` | array of binding dictionaries | `[]` | Adds resource constraints for custom function parameters. `group` selects a resource group (such as `component` or `prefab`); the other fields are `symbol` (string of the endpoint you want to bind resources to), `target`, which contains both a `parameter` (string of the parameter name to bind resources to in the function) and `variadic` (boolean indicating whether the parameter takes an arbitrary amount of said resources) field, and `pluralResourceNoun` (boolean indicating whether the resource parameter's name is meant to be described as plural). The metadata is shared by completion, rich hovers, and signature highlighting. |
| `platform.luduvo.resources.completion.enabled` | boolean | `true` | Enables resource-name suggestions in resource-constrained arguments. Disabling it does not hide resource metadata from hovers or signature highlighting. |
| `platform.luduvo.resources.completion.disabledKinds` | string array | `[]` | Disables resource-name completion for the listed resource kinds. |
| `platform.luduvo.resources.completion.disabledApis` | string array | `[]` | Disables resource-name completion for the listed canonical API symbols. |

## Bindings

Bindings are a way to associate special LSP behavior with specific methods and functions. For now its usecases are somewhat limited but they'll expand in the future. While the exact binding fields will vary depending on the kind of binding you define, they all share the same 3 required fields:

1. `symbol` is the name of the function or method to bind, like `MyLibrary.Spawn`, `Color3.new`, etc. It's a bit unintuitive, but this also supports bindings to functions in types like `Instance`, `World`, etc. So if you wanted `self:AddAccessory` to get a custom type, you would use `Instance.AddAccessory` as the symbol name.
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

Resources are a type of binding that extends autocomplete to provide enum-like name suggestions for function arguments that aren't actually typed enums. These come in handy with respect to Luduvo's API, as there are many generic strings that don't actually act as enums when they should.

To create and bind resources, there are 3 main systems in play:

#### Sources

In order to use assemble a resource, you must first tell Luduvo LSP where to find its contents. To do so, you need to give Luduvo LSP a list of providers to use for resource autocomplete called `sources`.

To make a source, you need to add them to a dictionary in `resources.sources`. Each key in the dictionary is the name of the source, and the value is another dictionary that defines how Luduvo LSP should fetch the resource values. Each source dictionary can define some `provider`, and that provider may have additional fields that define how the provider should be used. If not defined, it defaults to `static`.

With that said, the only supported provider is also just `static`, which defines a static list of `values` to use for autocomplete. These values can just be plain strings that are used as-is, or they can be `name`ed dictionaries that describe markdown `documentation` fields that will appear fully rendered in the autocomplete's enum tooltip:

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
  - Currently, the names of the builtin resources are `prefab` and `component`. If you make a group with a those names, it will be treated as overriding those builtin groups. 
  - And if you're curious, this is how the builtin groups are defined:
    - `prefab` resources come from the builtin core prefabs defined in your active Luduvo installation. When Luduvo LSP cannot find your Luduvo installation, it falls back to a precompiled version that is bundled with the LSP.
    - `component` resources are only precompiled names gathered from dumps preformed by Luduvo LSP when it's being built because Luduvo hardcodes components directly into their EXE. As such, they won't update dynamically like how `prefab` can. 
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
        "includeBuiltins": false
      }
    }
  }
}
```

When you need to apply custom resources to your code, these are the names that you need to refer to your resource as so remember them good. You will never refer to a resource by its source name-- it has to be this.

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
| `autorun-self-access` | error | Attempting to use Luduvo's `self` inside an `autorun` directory, where Luduvo supplies `nil`. Should not trigger if you redefine `self` locally |
| `empty-world-query` | error | Calling `game.World.Query` or `World.Query` without any expected arguments (a string component name or `Pair`) |
| `invalid-nolint-directive` | warning | Referring to an unknown Luduvo diagnostic rule in a `--!nolint luduvo/...` directive, or slightly mispelling it to the point that the LSP can guess what your intent was. Any lint that doesn't start with `luduvo/` won't get this lint and instead gets the default upstream Luau hint |

If you feel the need, severities can be changed without globally disabling every Luduvo diagnostic:

```jsonc
{
  "platform": {
    "luduvo": {
      "diagnostics": {
        "enabled": true,
        "rules": {
          "empty-world-query": "warning",
          "autorun-self-access": "off"
        }
      }
    }
  }
}
```

Supported values for rules are `default`, `error`, `warning`, `information`, `hint`, and `off`.

Just like normal lints, Luduvo lints can also be suppressed via `--!nolint`:

```luau
--!nolint luduvo/empty-world-query
```

Only 1 rule per line is supported. A bare `--!nolint` suppresses all Luduvo lints alongside Luau's built-in ones

## Notification suppression

Since context notifications can get annoying, Luduvo can now selectively suppress them through `platform.luduvo.notifications`. For example:

```json
{
  "platform": {
    "luduvo": {
      "notifications": {
        "context.project-stale": false, // Suppress/disables the "project stale" notification
      }
    }
  }
}
```

The key is a notification ID that is at the bottom of all suppressable notifications. Set an ID to `true` to enable its popup or `false` to suppress it. Below is the full list of notification IDs and their default values:

| Notification ID | Default | Appears when... |
|---|---|---|
| `configuration.invalid` | On | Invalid Luduvo diagnostics, bindings, or resource settings are ignored |
| `definitions.fallback` | On | Installed or overridden Luduvo definitions cannot be loaded and fallback definitions are used |
| `server.crash-reporting-suggestion` | On | The VS Code extension crashes after an unexpected exit, though only if the binary its using has reporting support. For Luduvo LSP specifically, you will likely never natually see it |
| `context.project-tracking-started` | On | The first live LSP begins tracking a project |
| `context.project-tracking-stopped` | Off | The last live LSP stops tracking a project during a registration update. A forcibly terminated LSP cannot send this message |
| `context.project-stale` | On | A tracked project's context becomes stale, or tracking begins with stale context. Exclusive to people with lazy refreshing on |
| `context.files-added` | On | Newly inferred script attachments are added to context. Multiple additions may be grouped into one message |
| `context.autodump-started` | Off | An offscreen automatic dump starts |
| `context.updated` | Off | A new world dump replaces captured project context. Multiple updates may be grouped |
| `context.autodump-failed` | On | An automatic dump fails. Includes the manual `world.dump` command as a fallback |
| `context.refresh-required` | On | Fresh context is needed for whatever action you are about to preform but said project's content is stale. Also will appear if automatic dumps are enabled and it can't refresh on its own for whatever reason |
| `context.selection-required` | On | Luduvo LSP doesn't know which project context to use for an opened script that belongs to multiple projects |
| `context.state` | Off | A context-state check reports that no project matches or that the selected capture is current |
| `context.error` | On | Some error occurs while reading context or updating project tracking |

VSCode users also get a "Do not show again" option to do all of this automatically. Lucky them.

## Building

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

## Current limitations

Resource-name completion does not yet discover custom components or prefabs from within your project (but it will for prefabs soon). For now, you will need to define project-specific static sources through `resources.sources` and attach them through `resources.groups`. 

Dynamic component fields and event columns are currently `any` instead of a more specific type. While queryable component names are generated from the current engine snapshot, there's no smart detection for any column to hone type detection to only the components you queried for. Same goes for event column fields. I'm aiming to improve this in a later version, and hopefully it'll be done in a couple releases from now.
