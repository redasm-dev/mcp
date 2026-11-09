#pragma once

#include <cJSON.h>
#include <redasm/redasm.h>

void mcp_tools_init(void);
void mcp_tools_deinit(void);

cJSON* mcp_tools_list(void);
cJSON* mcp_tools_call(RDContext* ctx, const char* name, const cJSON* args);
