#pragma once

#include <stdbool.h>

typedef struct cJSON cJSON; // NOLINT
typedef struct RDContext RDContext;

#define MCP_ARG_(n, t, d, r)                                                   \
    {.name = (n), .type = (t), .description = (d), .required = (r)}

#define MCP_REQ_STR(n, d) MCP_ARG_(n, "string", d, true)
#define MCP_OPT_STR(n, d) MCP_ARG_(n, "string", d, false)
#define MCP_REQ_INT(n, d) MCP_ARG_(n, "integer", d, true)
#define MCP_OPT_INT(n, d) MCP_ARG_(n, "integer", d, false)
#define MCP_OPT_BOOL(n, d) MCP_ARG_(n, "boolean", d, false)

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
    const char* type; // "string", "integer", "boolean"
    const char* description;
    bool required;
} McpArg;

typedef struct McpTool {
    const char* name;
    const char* description;
    const McpArg* args; // terminated by {NULL}
    bool needs_context;
    cJSON* (*run)(RDContext* ctx, const cJSON* args);
} McpTool;
