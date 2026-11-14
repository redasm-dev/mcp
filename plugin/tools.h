#pragma once

#include <redasm/redasm.h>
#include <yyjson.h>

void mcp_tools_init(void);
void mcp_tools_deinit(void);

// the minified 'tools/list' result, a static string
const char* mcp_tools_list(void);

// the 'tools/call' result built in 'doc', NULL if the tool doesn't exist.
// Arguments are validated against the tool's inputSchema before it runs.
yyjson_mut_val* mcp_tools_call(RDContext* ctx, const char* name,
                               yyjson_val* args, yyjson_mut_doc* doc);
