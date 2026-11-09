#include "tools.h"
#include "dsl.h"
#include "mcp.h"
#include <redasm/redasm.h>
#include <string.h>

static cJSON* tool_status(RDContext* ctx, const cJSON* args) {
    RD_UNUSED(args);
    return mcp_result_text(ctx ? "A file is open." : "No file open.");
}

MCP_NO_ARGS(status);

static const McpTool MCP_TOOLS[] = {
    MCP_TOOL(status, "Report the open file and analysis state"),
};

static cJSON* mcp_tools_cache = NULL;

static cJSON* _mcp_tool_schema(const McpArg* args) {
    cJSON* schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "object");

    cJSON* props = cJSON_AddObjectToObject(schema, "properties");
    cJSON* required = cJSON_CreateArray();

    for(const McpArg* a = args; a->name; a++) {
        cJSON* p = cJSON_AddObjectToObject(props, a->name);
        cJSON_AddStringToObject(p, "type", a->type);
        cJSON_AddStringToObject(p, "description", a->description);

        if(a->required)
            cJSON_AddItemToArray(required, cJSON_CreateString(a->name));
    }

    if(cJSON_GetArraySize(required))
        cJSON_AddItemToObject(schema, "required", required);
    else
        cJSON_Delete(required);

    return schema;
}

void mcp_tools_init(void) {
    if(mcp_tools_cache) return;

    mcp_tools_cache = cJSON_CreateObject();
    cJSON* list = cJSON_AddArrayToObject(mcp_tools_cache, "tools");

    for(size_t i = 0; i < rd_count_of(MCP_TOOLS); i++) {
        const McpTool* tool = &MCP_TOOLS[i];

        cJSON* t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "name", tool->name);
        cJSON_AddStringToObject(t, "description", tool->description);
        cJSON_AddItemToObject(t, "inputSchema", _mcp_tool_schema(tool->args));
        cJSON_AddItemToArray(list, t);
    }
}

void mcp_tools_deinit(void) {
    cJSON_Delete(mcp_tools_cache);
    mcp_tools_cache = NULL;
}
cJSON* mcp_tools_list(void) { return cJSON_Duplicate(mcp_tools_cache, true); }

cJSON* mcp_tools_call(RDContext* ctx, const char* name, const cJSON* args) {
    for(size_t i = 0; i < rd_count_of(MCP_TOOLS); i++) {
        const McpTool* tool = &MCP_TOOLS[i];
        if(strcmp(tool->name, name) != 0) continue;

        if(tool->needs_context && !ctx)
            return mcp_result_error("No file open.");

        cJSON* result = tool->run(ctx, args);
        return result ? result : mcp_result_error("Internal tool error.");
    }

    return NULL;
}
