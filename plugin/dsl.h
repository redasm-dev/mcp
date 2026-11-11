#pragma once

#include <stdbool.h>
#include <yyjson.h>

typedef struct RDContext RDContext;

typedef enum McpArgType {
    MCP_ARG_INVALID = 0,
    MCP_ARG_STRING, // 0 terminates the argument list
    MCP_ARG_INT,
    MCP_ARG_BOOL,
} McpArgType;

#define MCP_ARG_(n, t, d, r)                                                   \
    {.name = (n), .type = (t), .description = (d), .required = (r)}

#define MCP_REQ_STR(n, d) MCP_ARG_(n, MCP_ARG_STRING, d, true)
#define MCP_OPT_STR(n, d) MCP_ARG_(n, MCP_ARG_STRING, d, false)
#define MCP_REQ_INT(n, d) MCP_ARG_(n, MCP_ARG_INT, d, true)
#define MCP_OPT_INT(n, d) MCP_ARG_(n, MCP_ARG_INT, d, false)
#define MCP_OPT_BOOL(n, d) MCP_ARG_(n, MCP_ARG_BOOL, d, false)

#define MCP_ARGS(tool, ...)                                                    \
    static const McpArg tool##_args[] = {__VA_ARGS__, {0}}
#define MCP_NO_ARGS(tool) static const McpArg tool##_args[] = {{0}}

#define MCP_TOOL_(fn, d, ctx)                                                  \
    {                                                                          \
        .name = #fn,                                                           \
        .description = (d),                                                    \
        .args = fn##_args,                                                     \
        .needs_context = (ctx),                                                \
        .run = tool_##fn,                                                      \
    }

#define MCP_TOOL(fn, d) MCP_TOOL_(fn, d, false)
#define MCP_CTX_TOOL(fn, d) MCP_TOOL_(fn, d, true)

typedef struct McpArg {
    const char* name;
    McpArgType type;
    const char* description;
    bool required;
} McpArg;

// 'args' is already validated against the tool's McpArg list: every required
// argument is present and every present argument has the declared type.
// The result is built inside 'doc', use mcp_result_text/error.
typedef yyjson_mut_val* (*McpToolRun)(RDContext* ctx, yyjson_val* args,
                                      yyjson_mut_doc* doc);

typedef struct McpTool {
    const char* name;
    const char* description;
    const McpArg* args; // terminated by {0}
    bool needs_context;
    McpToolRun run;
} McpTool;
