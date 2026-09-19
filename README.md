# Luduvo Language Server

A (very incomplete) fork of Jonny Morganz's [Luau Language Server](https://github.com/JohnnyMorganz/luau-lsp) to work with Luduvo. This readme only covers the Luduvo-specific changes, so check them out for more information on how to use this.

Hasn't been tested anywhere other than the Zed editor, but should theoretically work with other editors provided they are setup right.

## Luduvo Definitions

`globalTypes.luduvo.d.luau` describes the public Luduvo scripting API from [the engine binding reference](https://docs.luduvo.com/reference). Currently, there are ~462 entries spanning across:
- 64 global symbols
- 398 members, with
  - 191 of those are from Instance
  - 16 of those are from game services
Out of said 462 entries, there are docs for about 443 of them.

All of these entries are sourced from `luduvo-api.json`, which itself is a dump of all the references served by the reference page on the official API docs. Entries (mostly) retain the original signatures, parents (if it belongs to instance, game, World, etc.), descriptions, flags, and component group identifiers. More on the flags, they are somewhat simple to decipher: the low three flag bits identify the symbol "kind" (an enum, global class, instance property, etc.), 8 denotes its scope (server only or replicated to both), 16 denotes if its read-only, and 32 means it's a thing/service under the `game` umbrella.

Luduvo-api.json used to be populated by AI, but is now done programmatically via `dumpLuduvoApi.py`. `dumpLuduvoApi.py` not only dumps and decodes the info from the site, but also gets Luduvo's builtin component names by downloading from `LuduvoGame.exe` and painstakingly finding the component names directly in the program's resources. I won't be surprised if it breaks immediately upon a new Luduvo release.

Since `dumpLuduvoApi.py` only dumps content from the reference page, small `luduvo-api-overrides.json` and `luduvo-component-overrides.json` files also exist specifically for the API endpoints that are documented somewhere in the Luduvo Docs, but absent from the reference (like the entire camera/screen API).

You can refresh the JSON snapshots and then regenerate the declaration file by cloning the scripts folder in the repo and running:

```sh
python scripts/luduvo/dumpLuduvoApi.py --generate
```
`dumpLuduvoTypes.py` and `dumpLuduvoDocs.py` also exist in that same folder if you want to generate type and documentation snapshots separately.

If you set `luduvo` as the platform type in your editor's config, you won't have to worry about any of this. The necessary files get embedded directly into Luduvo LSP when it gets compiled. Of course, you can still override the embedded files with your own custom ones by setting the definition/documentation files the config or by setting `LUDUVO_DEFINITIONS` and `LUDUVO_DOCUMENTATION` in `CMakeLists.txt` if you are building from scratch.

## Usage

You can enable Luduvo-specific LSP by setting `luau-lsp.platform.type = "luduvo"` in your editor's server config, or use `luau-lsp analyze --platform=luduvo path/to/script.luau`. For now I decided to keep the platform default as roblox just to be safe. You also need to change your editor-specific `luau-lsp.binary.path` with this Luduvo-specific one.

Another thing to note is that while Luduvo uses Luau for its underlying engine, Luduvo generates `.client.lua` and `.server.lua` files when attaching new scripts to your game. If your editor supports it, associate those patterns with Luau so ordinary `.lua` files can continue using Lua tooling. For me (Zed), this involved editing my default `"file_types"`.

For example, this is what I put in my `.zed/settings.json`:

```json
	"file_types": {
		"Luau": ["**/*.client.lua", "**/*.server.lua", "**/scripts/*.lua"],
	},
	"lsp": {
		"luau-lsp": {
			"settings": {
				"binary": {
					"path": "C:\\my\\path\\to\\luduvo-lsp.exe",
					"ignore_system_version": true,
				},

				"luau-lsp": {
					"platform": {
						"type": "luduvo",
					},

					"sourcemap": {
						"enabled": false,
						"autogenerate": false,
					},

					"diagnostics": {
						"workspace": true,
					},
				},
			},
		},
	},
```

It should work after that. Unless you are using a different declaration file than the one Luduvo LSP provides, there is no need to mess with the `definitionFiles` or `documentationFiles` setting. However if you aren't using Luduvo LSP but still want Luduvo type declarations, you will need to manually set `definitionFiles` and `documentationFiles` to link to `api-docs/en-us/globalTypes.luduvo.d.luau` and `api-docs/en-us/documentation.luduvo.json` respectively. You can find these files in the .zip file bundled in the releases.

If you want to test if it works, see the example file at `examples/luduvo/test.luau`.

## Building

Since I'm on Windows and I don't have a lot of disk space, I built this with MinGW GCC, CMake, and Ninja:

```sh
cmake -S . -B build/luduvo/[platform] -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++ -DLSP_WERROR=OFF
cmake --build build/luduvo/[platform] --target Luau.LanguageServer.CLI Luau.LanguageServer.Test -j 2
```

I was able to build successfully on both Windows and Linux (via WSL). Using MSVC's equivalents should still work fine as well, but mileage will vary. If you need a quick build, configure with
`-DCMAKE_CXX_FLAGS_RELEASE="-O0 -DNDEBUG"`.

## Current Limitations

For now, Server-only and read-only annotations are just comments in the globalTypes file. Luduvo LSP does not enforce these scope/access restrictions. Instance properties available only with certain components are exposed on the common Instance type.

Dynamic component fields and event columns are currently `any` instead of a more specific type. While Queryable component names are generated from the current engine snapshot, there's no smart detection for any column to hone type detection to only the components you queried for. Same goes for event column fields.
