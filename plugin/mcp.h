#pragma once

#include <cJSON.h>
#include <redasm/redasm.h>

void mcp_init(void);
void mcp_deinit(void);
cJSON* mcp_result_text(const char* text);
cJSON* mcp_result_error(const char* text);
RDCommandValue mcp_execute(RDContext* ctx, const RDCommandValue* args);
