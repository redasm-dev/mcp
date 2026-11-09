#include "config.h"
#include <stdio.h>
#include <stdlib.h>

bool mcp_config_load(McpConfig* self, const char* filepath) {
    *self = (McpConfig){0};
    self->config = rd_settings_load(filepath);

    if(!self->config) {
        fprintf(stderr, "[REDasm MCP] cannot load config '%s'\n", filepath);
        goto fail;
    }

    const RDDatum* root = rd_settings_get_root(self->config);

    if(!root) {
        fprintf(stderr,
                "[REDasm MCP] cannot get document root for config '%s'\n",
                filepath);
        goto fail;
    }

    usize i = 0;
    const RDDatum *d, *plugin_dirs = rd_datum_get_array(root, "plugin_dirs"),
                      *kb_dirs = NULL;

    if(!plugin_dirs) {
        fprintf(stderr, "[REDasm MCP] plugin_dirs missing for config '%s'\n",
                filepath);
        goto fail;
    }

    self->plugin_dirs = (const char**)calloc(
        rd_datum_get_length(plugin_dirs) + 1, sizeof(char*));

    rd_datum_each(d, plugin_dirs) {
        const char* s = rd_datum_to_str(d);
        self->plugin_dirs[i++] = s;
    }

    kb_dirs = rd_datum_get_array(root, "kb_dirs");

    self->kb_dirs =
        (const char**)calloc(rd_datum_get_length(kb_dirs) + 1, sizeof(char*));

    i = 0;
    rd_datum_each(d, kb_dirs) {
        const char* s = rd_datum_to_str(d);
        self->kb_dirs[i++] = s;
    }

    self->settings = rd_datum_get_str(root, "settings");
    return true;

fail:
    mcp_config_destroy(self);
    return false;
}

void mcp_config_destroy(McpConfig* self) {
    rd_settings_destroy(self->config);
    free((void*)self->plugin_dirs);
    free((void*)self->kb_dirs);
    *self = (McpConfig){0};
}
