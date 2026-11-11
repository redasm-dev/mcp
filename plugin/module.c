#include "mcp.h"
#include "session.h"
#include "tools.h"
#include <redasm/redasm.h>

static const RDCommandParam MCP_PARAMS[] = {
    {RD_CMDARG_STRING, "request"},
    {RD_CMDARG_VOID, NULL},
};

static const RDCommandParam MCP_OPEN_FILE_PARAMS[] = {
    {RD_CMDARG_STRING, "path"},
    {RD_CMDARG_VOID, NULL},
};

static const RDCommandPlugin MCP_COMMAND = {
    .id = "mcp",
    .name = "MCP",
    .params = MCP_PARAMS,
    .execute = mcp_execute,
};

static const RDCommandPlugin MCP_OPEN_FILE_COMMAND = {
    .id = "mcp.open_file",
    .name = "MCP Open File",
    .params = MCP_OPEN_FILE_PARAMS,
    .execute = mcp_session_open_file,
};

static void mcp_module_load(void) {
    mcp_init();
    mcp_tools_init();
    mcp_session_init();
    rd_register_command(&MCP_COMMAND);
    rd_register_command(&MCP_OPEN_FILE_COMMAND);
}

static void mcp_module_unload(void) {
    mcp_tools_deinit();
    mcp_deinit();
}

static void mcp_module_shutdown(void) { mcp_session_close(); }

RD_MODULE_EXPORT = {
    .api_version = RD_API_VERSION,
    .load = mcp_module_load,
    .unload = mcp_module_unload,
    .shutdown = mcp_module_shutdown,
};
