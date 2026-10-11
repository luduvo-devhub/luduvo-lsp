# Luduvo Language Server

A (very incomplete) fork of Jonny Morganz's [Luau Language Server](https://github.com/JohnnyMorganz/luau-lsp) to work with Luduvo. This readme only covers the Luduvo-specific changes, so check them out for more information on how to use this.

Hasn't been tested anywhere other than the Zed editor, but should theoretically work with other editors provided they are setup right.

## Known issues

These won't affect 99% of users, but there's a few gatchas to be aware of when using Luduvo LSP (for now):

- Make sure `luau-lsp.diagnostics.workspace` inside your editor is set to `false` if you're using Luduvo LSP inside of the Luduvo LSP's repo. There seems to be some issues with hangs and whatnot triggering editor's crash monitor and subsequently killing the server
- Avoid combining `luau-lsp.fflags.enable_by_default: false` with `luau-lsp.fflags.enable_new_solver: true` unless every required companion FFlag is explicitly enabled. Weird FFlag combinations can cause the luau anyalzer to crash, and it doesn't help that Luduvo LSP requires the `SolverV2` fflag to be enabled in most cases

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

As of Luduvo 45, the `globalTypes.luduvo.client.d.luau` and `globalTypes.luduvo.server.d.luau` files inside Luduvo LSP are essentially copy/pasted global type files straight from Luduvo itself (you can find them in the `defs/` directory of your Luduvo install's roaming data equivalent). Currently, these files contain ~845 entries spanning across:
- 16 global symbols
- 796 members, with
  - 56 of those are from Instance
  - 19 of those are from game services
Out of said 845 entries, there are docs for about 458 of them.

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

## Usage

If you install Luduvo LSP or point your editor's `luau-lsp.binary.path` to the builds in the releases tab, Luduvo will be automatically entered as the default platform provided that you don't override it with `luau-lsp.platform.type`. You can also select it explicitly with `luau-lsp.platform.type = "luduvo"`, or use `luau-lsp analyze --platform=luduvo path/to/script.luau`. The other platforms (`roblox` and `standard`) still exist and work.

Another thing to note is that while Luduvo uses Luau for its underlying engine, Luduvo generates `.client.lua` and `.server.lua` files when attaching new scripts to your game. I recommend either setting `luau-lsp.analyzeLuaFiles` to `true` in your editor's config file, or associate those 2 file patterns with Luduvo Luau in your editor's file associations.

Finally, Luduvo's official global files use features only available under Luau's SolverV2 flag. If Luduvo LSP attempts to load official client/server definition files while `luau-lsp.fflags.enableNewSolver` is false, the LSP reports an error and skips loading those files at all. If you set `luau-lsp.platform.luduvo.definitions.globalPolicy` to `"definitionFilesOnly"`, Luduvo LSP won't cause any flag any issues.

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
| `luau-lsp.platform.type` | `"standard"`, `"roblox"`, or `"luduvo"` | `"luduvo"` | Selects the platform integration. Luduvo LSP adds the `luduvo` option and makes it the default. |
| `luau-lsp.platform.luduvo.dataDirectory` | string | `""` | Overrides Luduvo's per-user Client data directory. An empty value uses the operating-system default. |
| `luau-lsp.platform.luduvo.definitions.serverOverride` | string | `""` | Uses the specified Luau declaration file for server scripts before trying the installed or bundled server declarations. |
| `luau-lsp.platform.luduvo.definitions.clientOverride` | string | `""` | Uses the specified Luau declaration file for client scripts before trying the installed or bundled client declarations. |
| `luau-lsp.platform.luduvo.definitions.defaultScriptSide` | `"server"` or `"client"` | `"server"` | Selects the declarations used by files without a recognized client or server suffix. |
| `luau-lsp.platform.luduvo.definitions.globalPolicy` | `"luduvoOnly"`, `"definitionFilesOnly"`, `"preferLuduvo"`, `"preferDefinitionFiles"`, or `"combine"` | `"combine"` | Controls how official Luduvo declarations interact with `types.definitionFiles`. |
| `luau-lsp.platform.luduvo.definitions.conflictWinner` | `"luduvo"` or `"definitionFiles"` | `"luduvo"` | Chooses which source owns a duplicate global when `globalPolicy` is `combine`. |
| `luau-lsp.platform.luduvo.definitions.exposePrivateTypes` | boolean | `false` | For some reason, official Luduvo definition files purposely hides certain types. If you enable this, Luduvo LSP will force all top-level `type` aliases from Luduvo definition files into exported globals. May cause issues with type checking and Luduvo's own type checker likely won't like that you are using, so it's disabled by default. |
| `luau-lsp.platform.luduvo.hover.presentation` | `"rich"` or `"standard"` | `"rich"` | Selects Luduvo's API-aware hover layout or the upstream-compatible hover layout. Standard hovers keep documentation above the type definition. |
| `luau-lsp.platform.luduvo.hover.maxTitleLength` | integer or `null` | `96` | Limits visible rich-hover title length while preserving provenance text. URLs do not count toward the limit; `null` disables truncation. |
| `luau-lsp.platform.luduvo.hover.maxTypeDefinitionLength` | integer or `null` | `2000` | Limits the Luau type definition shown in rich hovers. `null` disables truncation. |
| `luau-lsp.platform.luduvo.resources.groups` | map of resource-group names to group objects | `{}` | Each group defines `dataType`, `includeBuiltins`, and a list of source names. Missing built-in groups implicitly use their active Luduvo built-ins. Only the `string` data type is supported in this release. |
| `luau-lsp.platform.luduvo.resources.sources` | map of source names to source objects | `{}` | Defines reusable value sources. Static sources use `provider: "static"` and a `values` array; a source may be shared by multiple groups. |
| `luau-lsp.platform.luduvo.resources.bindings` | array of binding objects | `[]` | Adds resource constraints for custom function parameters. `kind` selects a resource group (such as `component` or `prefab`); the other fields are `symbol`, `parameter`, `variadic`, and `pluralResourceNoun`. The metadata is shared by completion, rich hovers, and signature help. |
| `luau-lsp.platform.luduvo.resources.completion.enabled` | boolean | `true` | Enables resource-name suggestions in resource-constrained arguments. Disabling it does not hide resource metadata from hovers or signature help. |
| `luau-lsp.platform.luduvo.resources.completion.disabledKinds` | string array | `[]` | Disables resource-name completion for the listed resource kinds. |
| `luau-lsp.platform.luduvo.resources.completion.disabledApis` | string array | `[]` | Disables resource-name completion for the listed canonical API symbols. |

Resource groups collect reusable value sources by kind. A group omitted from configuration uses its active Luduvo built-ins. Explicitly configuring a group controls its full membership: set `includeBuiltins` to retain that catalog, or leave it false for a custom-only group. An empty source list with built-ins disabled intentionally provides no names. Prefab built-ins come from the active Luduvo installation and then a bundled, precompiled fallback. Components currently use the generated bundled catalog because Luduvo does not expose a live component-name source.

For example, you can extend the built-in prefabs with your own project prefabs, create a custom "animation-state" group, and mark a custom function parameter as accepting prefab names:

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
            "values": ["MyProjectPrefab"]
          },
          "default-states": {
            "provider": "static",
            "values": ["Idle", "Running"]
          }
        },
        "bindings": [
          {
            "symbol": "MyLibrary.Spawn",
            "parameter": "name",
            "kind": "prefab"
          }
        ]
      }
    }
  }
}
```

## Building

Since I'm on Windows and I don't have a lot of disk space, I built this with MinGW GCC, CMake, and Ninja:

```sh
cmake -S . -B build/luduvo/[platform] -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++ -DLSP_WERROR=OFF
cmake --build build/luduvo/[platform] --target Luau.LanguageServer.CLI Luau.LanguageServer.Test -j 2
```

I was able to build successfully on both Windows and Linux (via WSL). Using MSVC's equivalents should still work fine as well, but mileage will vary. If you need a quick build, configure with
`-DCMAKE_CXX_FLAGS_RELEASE="-O0 -DNDEBUG"`.

## Current limitations

Resource-name completion does not yet discover custom components or prefabs from the current workspace. You can define project-specific static sources through `resources.sources` and attach them through `resources.groups`. Workspace and sourcemap-backed providers will be handled separately so they can update as project files change.

Dynamic component fields and event columns are currently `any` instead of a more specific type. While queryable component names are generated from the current engine snapshot, there's no smart detection for any column to hone type detection to only the components you queried for. Same goes for event column fields. I'm aiming to improve this in a later version, but it'll take more work because I have to reverse engineer Luduvo's `.ldv` file format first. If you want to help, do this for me!
