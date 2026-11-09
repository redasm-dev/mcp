#include "mcp.h"
#include "tools.h"
#include <redasm/redasm.h>

static const RDCommandParam MCP_PARAMS[] = {
    {RD_CMDARG_STRING, "request"},
    {RD_CMDARG_VOID, NULL},
};

static const RDCommandPlugin MCP_COMMAND = {
    .id = "mcp",
    .name = "MCP",
    .params = MCP_PARAMS,
    .execute = mcp_execute,
};

static void mcp_module_load(void) {
    mcp_init();
    mcp_tools_init();
    rd_register_command(&MCP_COMMAND);
}

static void mcp_module_unload(void) {
    mcp_tools_deinit();
    mcp_deinit();
}

RD_MODULE_EXPORT = {
    .api_version = RD_API_VERSION,
    .load = mcp_module_load,
    .unload = mcp_module_unload,
};
