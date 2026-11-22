# REDasm MCP

> **Experimental / Work in Progress.** The tools and their output can still change.

An [MCP](https://modelcontextprotocol.io) server for [REDasm Core Engine](https://github.com/redasm-dev/core). It lets an MCP client such as Claude Desktop open a binary, read its analysis, follow references and calls, and annotate or patch it, using REDasm's own engine.

It's written in plain C17 and builds into one native executable plus a REDasm plugin.

## How it works

```
MCP client  ──stdio, JSON-RPC──►  redasm-mcp  ──►  libredasm + plugins
(Claude Desktop, ...)              │                (loaders, processors, mcp)
                                   └─ forwards each request to the 'mcp' plugin
```

- **`redasm-mcp`** is the executable the client starts. It links the REDasm core, reads a TOML config, loads the plugins and forwards every line it reads on stdin to the `mcp` plugin. It doesn't parse JSON itself.
- **The `mcp` plugin** is a REDasm command plugin that implements the protocol (MCP `2025-11-25`) and the tools. It owns the analysis of the open file.

> Because the protocol lives in a plugin, the same tools can later be served from inside the GUI, working on the file you have open there. That part is planned, not done yet.

## Building

Requirements:
- CMake 3.20 or newer and a C17 compiler
- [REDasm Core Engine](https://github.com/redasm-dev/core): installed or built in the same workspace
- [yyjson](https://github.com/ibireme/yyjson): fetched only if it isn't available locally

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build
```

`cmake --install` puts:
- `redasm-mcp`, the server executable, in `bin/` 
- the `mcp` plugin (`libmcp.so`, `mcp.dll` on Windows) in REDasm's plugin directory

## Configuration

`redasm-mcp` takes everything from one TOML file:

```toml
plugin_dirs = ["/usr/lib/redasm/plugins"]
kb_dirs = ["/home/me/redasm/kb"]

[mcp]
allowed_dirs = ["/home/me/samples"]
working_dir = "/home/me/redasm/work"
name_prefix = "vibe_"
debug = false
```

| Key | Required | Meaning |
|---|---|---|
| `plugin_dirs` | yes | Folders searched recursively for REDasm plugins. They must include the `mcp` plugin, loaders and processors. |
| `kb_dirs` | no | Knowledge base folders. Without a KB, API prototypes are unknown, so callbacks passed to APIs (window and dialog procedures, thread start routines) may stay unexplored. `status` tells the model when there isn't one. |
| `settings` | no | A separate REDasm settings file. Without it, this same file is used, which is why the `[mcp]` section can live here. |
| `mcp.allowed_dirs` | no | The folders the model may open files from. Paths are resolved before they're checked, so `..` and symbolic links can't escape them. |
| `mcp.working_dir` | no | Where databases and `.rdx` projects are written. Projects can also be loaded from here. |
| `mcp.name_prefix` | no | Added to names set by `rename`, unless the name already has it. |
| `mcp.debug` | no | Logs the JSON-RPC traffic on stderr. |

## Connecting a client

```
redasm-mcp --config <redasm-mcp.toml> [file]
```

The optional `file` is opened at startup, and can be a binary or a saved `.rdx` project. Without it, the model opens files with `open_file` or `load_project`.

For Claude Desktop, add the server to `claude_desktop_config.json`:

```json
{
  "mcpServers": {
    "redasm": {
      "command": "/usr/bin/redasm-mcp",
      "args": ["--config", "/home/me/redasm/redasm-mcp.toml"]
    }
  }
}
```

Any MCP client that starts servers over stdio works the same way. The [MCP Inspector](https://github.com/modelcontextprotocol/inspector) is handy for trying tools by hand:

```sh
npx @modelcontextprotocol/inspector redasm-mcp --config redasm-mcp.toml
```

## Security

The model decides which tools to call, so the server limits what it can reach:
- **Files:** `open_file` only reads inside `allowed_dirs`, and `load_project` inside those folders or `working_dir`. Paths are fully resolved first, and the resolved path is the one that's checked and opened.
- **Writing:** the server writes only the analysis database and saved projects, to `working_dir` (next to the input file when it isn't set). The input file itself is never modified.
- **Untrusted content:** names and strings come from the binary being analyzed, which may have been crafted to mislead a model, so every tool output that contains them is marked.

## Adding a tool

A tool is two things: an entry in `plugin/gen_tools/spec.json`, and a function in `plugin/executors.c`.

1. **Describe it in the spec,** in MCP's own tool format:

   ```json
   {
     "name": "get_name",
     "description": "The cheapest way to label an address: ...",
     "x-context": true,
     "inputSchema": {
       "type": "object",
       "properties": {
         "address": { "type": "string", "description": "Hex address like 0x401000, or a symbol name" }
       },
       "required": ["address"]
     }
   }
   ```

   A few extensions are removed before the client sees the tool:

   | Extension | Meaning |
   |---|---|
   | `"x-context": true` | The tool needs an open file. |
   | `"x-paging": true` | Adds `offset` and `limit` from the spec's `paging` section. |
   | `"x-enum": "<name>"` | The argument is one of an enum's values, which become its `"enum"` list. An optional enum needs a `"default"`. |
   | `"x-only": [...]` | Accepts only part of that enum. |

   Enums are declared once under `enums`: a list of names for enums only the plugin uses, or names mapped to existing C constants (`"call": "RD_CR_CALL"`).

2. **Write the executor,** `mcp_exec_<name>`, which the build already declares:

   ```c
   yyjson_mut_val* mcp_exec_get_name(RDContext* ctx, yyjson_val* args,
                                     yyjson_mut_doc* doc) {
       const char* address = mcp_arg_get_name_address(args);
       ...
   }
   ```

   The arguments were validated against the schema before the function runs: types, required arguments, enum values, `minimum`/`maximum` and `maxLength`. Each one is read with a generated accessor, `mcp_arg_<tool>_<argument>()`, which returns its default when it's missing. A misspelled argument name is a compile error.

The build stops on a malformed spec, such as an unknown extension, an optional enum without a default or a duplicate tool, and a tool without an executor fails to link.

## Status

Experimental. The server works end to end with real clients, but:
- the tools cover roughly half of the core's features, with more to come
- serving the tools from inside the GUI isn't implemented yet
- there's no rebase yet: the core doesn't apply relocations when the base address changes

## License

GPL-3.0. See [LICENSE](LICENSE).
