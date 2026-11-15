#pragma once

#include <redasm/redasm.h>

// clang-format off
RDCommandValue mcp_session_open_file(RDContext* ctx, const RDCommandValue* args);
RDContext* mcp_session_context(RDContext* host_ctx);
bool mcp_session_debug(void);
bool mcp_session_settings_valid(void); 
bool mcp_session_is_standalone(void);

// the [mcp] working_dir, NULL when not set
const char* mcp_session_working_dir(void);

// prepended to names set by the model, NULL for none
const char* mcp_session_name_prefix(void);

// runs the analysis a change made pending, when this plugin owns the context
void mcp_session_settle(RDContext* ctx);

// a binary to analyze, or a .rdx project to restore
bool mcp_session_open(const char* filepath);
bool mcp_session_is_project(const char* filepath);
void mcp_session_close(void);
void mcp_session_init(void);
// clang-format on
