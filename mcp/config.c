#include "config.h"
#include <stdio.h>

static const RDDatumSchema MCP_CONFIG_SCHEMA[] = {
    {
        .key = "plugin_dirs",
        .kind = RD_DATUM_ARRAY,
        .array_type = &RD_DATUM_ITEM_STRING,
    },
    {
        .key = "kb_dirs",
        .kind = RD_DATUM_ARRAY,
        .optional = true,
        .array_type = &RD_DATUM_ITEM_STRING,
    },
    {
        .key = "settings",
        .kind = RD_DATUM_STRING,
        .optional = true,
    },
    {0},
};

// the schema guarantees an array of strings, or a missing optional key
static const char** _mcp_config_strings(const RDDatum* arr) {
    const char** strings =
        (const char**)rd_alloc0(rd_datum_get_length(arr) + 1, sizeof(char*));

    usize i = 0;
    const RDDatum* d;
    rd_datum_each(d, arr) { strings[i++] = rd_datum_to_str(d); }
    return strings;
}

bool mcp_config_load(McpConfig* self, const char* filepath) {
    *self = (McpConfig){0};
    self->config = rd_datumdoc_load(filepath, MCP_CONFIG_SCHEMA);

    if(!self->config) {
        fprintf(stderr, "[REDasm MCP] invalid config '%s'\n", filepath);
        return false;
    }

    const RDDatum* root = rd_datumdoc_get_root(self->config);
    self->plugin_dirs =
        _mcp_config_strings(rd_datum_get_array(root, "plugin_dirs"));
    self->kb_dirs = _mcp_config_strings(rd_datum_get_array(root, "kb_dirs"));
    self->settings = rd_datum_get_str(root, "settings");
    return true;
}

void mcp_config_destroy(McpConfig* self) {
    rd_datumdoc_destroy(self->config);
    rd_free((void*)self->plugin_dirs);
    rd_free((void*)self->kb_dirs);
    *self = (McpConfig){0};
}
