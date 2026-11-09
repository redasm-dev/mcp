#pragma once

#include <redasm/redasm.h>

typedef struct McpConfig {
    RDSettings* config;
    const char** plugin_dirs; // NULL terminated
    const char** kb_dirs;     // NULL terminated
    const char* settings;
} McpConfig;

bool mcp_config_load(McpConfig* self, const char* filepath);
void mcp_config_destroy(McpConfig* self);
