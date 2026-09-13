# Luduvo Language Server

A (very incomplete) fork of Jonny Morganz's [Luau Language Server](https://github.com/JohnnyMorganz/luau-lsp) to work with Luduvo. This readme only covers the Luduvo-specific changes, so check them out for more information on how to use this.

Hasn't been tested anywhere other than the Zed editor, but should theoretically work with other editors provided they are setup right.

## Luduvo Definitions

`globalTypes.d.luau` describes the public Luduvo scripting API from [the engine binding reference](https://docs.luduvo.com/reference), retrieved September 12, 2026. Currently, there are ~462 entries, with 191 Instance members, 15 game services, globals, constants, and named result types. **Note that AI created the JSON needed to generate the declaration file, mostly because I was not going to be able to handwrite over 500 definitions by hand.**

`luduvo-api.json` is a snapshot of the references served made by the documentation site. Entries retain the original signatures, parents (if it belongs to instance, game, World, etc.), descriptions, flags, and component group identifiers. The AI dumped it from `https://docs.luduvo.com/_app/immutable/chunks/Bz7_dWXD.js`. More on the flags, the low three flag bits identify the symbol kind; 8 denotes server availability, 16 read-only access, and 32 means it's a game service.

You can regenerate the declaration file by running:

```sh
python scripts/luduvo/dumpLuduvoTypes.py
```

If you set `luduvo` as the platform, the declaration file is embedded at build time and loaded before user-provided definitions, for both type checking and autocompletion. Nothing gets downloaded at startup. CMake tracks declaration changes and automatically regenerates the embedded header when necessary (change `LUDUVO_DEFINITIONS` in `CMakeLists.txt` to change where the cmake build looks for updates to the declaration file).


## Usage

You can enable Luduvo-specific LSP by setting `luau-lsp.platform.type = "luduvo"` in your editor's server config, or use `luau-lsp analyze --platform=luduvo path/to/script.luau`. For now I decided to keep the platform default as roblox just to be safe. You also need to change `luau-lsp.binary.path` with this Luduvo-specific one.

For example, this is what I put in my `.zed/settings.json`:

```json
{
  "lsp": {
    "luau-lsp": {
      "binary": {
        "path": "C:/path/to/luduvo-lsp/build-luduvo/luduvo-lsp.exe",
        "arguments": ["lsp"]
      },
      "settings": {
        "roblox": { "enabled": false },
        "plugin": { "enabled": false },
        "fflags": { "sync": false },
        "luau-lsp": {
          "platform": { "type": "luduvo" },
          "types": { "roblox": false },
          "sourcemap": { "enabled": false }
        }
      }
    }
  }
}
```

It should work after that. Unless you are using a different declaration file than the one Luduvo LSP provides, there is no need to mess with the `definitionFiles` setting. However if you aren't using Luduvo LSP but still want luduvo type declarations, you will need to manually set `definitionFiles` to link to the declaration file generated in the scripts folder.

If you want to test if it works, see the example file at `examples/luduvo/test.luau`.

## Building from Source

I built this with MinGW GCC, CMake, and Ninja:

```sh
cmake -S . -B build-luduvo -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++ -DLSP_WERROR=OFF
cmake --build build-luduvo --target Luau.LanguageServer.CLI Luau.LanguageServer.Test -j 2
```

Milage will vary. If you need a quick build, configure with
`-DCMAKE_CXX_FLAGS_RELEASE="-O0 -DNDEBUG"`. MinGW builds link the runtime statically.

## Limitations

For now, Server-only and read-only annotations are just comments; Luduvo LSP does not enforce scope/access restrictions. Instance properties available only with certain components are exposed on the common Instance type.

Dynamic attributes, component fields, and event columns are currently just `any` instead of a more specific type. Some extra info is given for query columns, but its by and large `any` as well.

The (currently hidden) component name list comes from the components given in Luduvo 37 and are not up to date.
