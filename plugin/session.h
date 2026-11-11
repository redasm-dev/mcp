#pragma once

#include <redasm/redasm.h>

// clang-format off
RDCommandValue mcp_session_open_file(RDContext* ctx, const RDCommandValue* args);
RDContext* mcp_session_context(RDContext* host_ctx);
bool mcp_session_debug(void);
bool mcp_session_settings_valid(void); 
bool mcp_session_is_standalone(void);
bool mcp_session_open(const char* filepath);
void mcp_session_close(void);
void mcp_session_init(void);
// clang-format on
