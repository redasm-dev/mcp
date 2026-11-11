#include "tools.h"
#include "dsl.h"
#include "mcp.h"
#include "session.h"
#include <stdlib.h>
#include <string.h>

static bool _mcp_is_allowed(const char* resolved) {
    const RDDatum* dirs =
        rd_datum_get_array(rd_settings_global(), "mcp.allowed_dirs");

    const RDDatum* d;
    rd_datum_each(d, dirs) {
        const char* dir = rd_datum_to_str(d);
        if(dir && rd_path_isinside(resolved, dir)) return true;
    }

    return false;
}

static yyjson_mut_val* tool_status(RDContext* ctx, yyjson_val* args,
                                   yyjson_mut_doc* doc) {
    RD_UNUSED(args);
    if(!ctx) return mcp_result_text(doc, "No file open.");

    const RDLoaderPlugin* loader = rd_get_loader_plugin(ctx);
    const RDProcessorPlugin* processor = rd_get_processor_plugin(ctx);

    const char* text =
        rd_format("File: %s\nLoader: %s\nProcessor: %s", rd_get_file_name(ctx),
                  loader ? loader->id : "?", processor ? processor->id : "?");

    return mcp_result_text(doc, text);
}

static yyjson_mut_val* tool_open_file(RDContext* ctx, yyjson_val* args,
                                      yyjson_mut_doc* doc) {
    RD_UNUSED(ctx);

    if(!mcp_session_is_standalone())
        return mcp_result_error(
            doc, "Opening files is only available in redasm_mcp.");

    if(!mcp_session_settings_valid())
        return mcp_result_error(
            doc, "Opening files is disabled: invalid [mcp] settings.");

    if(rd_datum_is_empty(
           rd_datum_get_array(rd_settings_global(), "mcp.allowed_dirs")))
        return mcp_result_error(
            doc, "Opening files is disabled: no allowed_dirs set.");

    // validated: present and a string
    const char* path = yyjson_get_str(yyjson_obj_get(args, "path"));
    if(!(*path)) return mcp_result_error(doc, "'path' cannot be empty.");

    // resolve first, then check and open the same resolved path
    const char* r = rd_path_resolve(path);
    if(!r) return mcp_result_error(doc, "File not found.");

    char* resolved = rd_strdup(r); // rd_path_resolve's buffer is shared
    yyjson_mut_val* result;

    if(!rd_path_isfile(resolved)) {
        result = mcp_result_error(doc, "Not a regular file.");
    }
    else if(!_mcp_is_allowed(resolved)) {
        result =
            mcp_result_error(doc, "Path is outside the allowed directories.");
    }
    else if(!mcp_session_open(resolved)) {
        result = mcp_result_error(
            doc, "Cannot open this file, no file is open now.");
    }
    else {
        const char* text = rd_format(
            "Opened %s, analysis complete. Addresses and names from any "
            "previous file no longer apply.",
            rd_path_filename(resolved));
        result = mcp_result_text(doc, text);
    }

    rd_free(resolved);
    return result;
}

// clang-format off
MCP_NO_ARGS(status);
MCP_ARGS(open_file, MCP_REQ_STR("path", "Absolute path of the file to open"));
// clang-format on

// clang-format off
static const McpTool MCP_TOOLS[] = {
    MCP_TOOL(status, "Report the open file and analysis state"),
    MCP_TOOL(open_file, "Open a file for analysis, replacing the current one. "
                        "Only files inside the configured allowed directories "
                        "can be opened."),
};
// clang-format on

static char* mcp_tools_raw = NULL; // serialized once in mcp_tools_init()

static const char* _mcp_type_name(McpArgType t) {
    switch(t) {
        case MCP_ARG_STRING: return "string";
        case MCP_ARG_INT: return "integer";
        case MCP_ARG_BOOL: return "boolean";
        default: break;
    }

    return "unknown";
}

static bool _mcp_type_matches(McpArgType t, yyjson_val* v) {
    switch(t) {
        case MCP_ARG_STRING: return yyjson_is_str(v);
        case MCP_ARG_INT: return yyjson_is_int(v); // 5.0 is not an integer
        case MCP_ARG_BOOL: return yyjson_is_bool(v);
        default: break;
    }

    return false;
}

static const McpArg* _mcp_find_arg(const McpArg* args, const char* name) {
    for(const McpArg* a = args; a->name; a++) {
        if(!strcmp(a->name, name)) return a;
    }

    return NULL;
}

// NULL if 'obj' matches 'args', otherwise a message for the model
static const char* _mcp_validate(const McpArg* args, yyjson_val* obj) {
    // a missing 'arguments' member counts as an empty object
    if(obj && !yyjson_is_obj(obj)) return "'arguments' must be an object.";

    for(const McpArg* a = args; a->name; a++) {
        yyjson_val* v = yyjson_obj_get(obj, a->name);

        if(!v) {
            if(a->required)
                return rd_format("Missing required argument '%s'.", a->name);
            continue;
        }

        if(!_mcp_type_matches(a->type, v)) {
            return rd_format("Argument '%s' must be of type %s.", a->name,
                             _mcp_type_name(a->type));
        }
    }

    if(obj) {
        usize idx, max;
        yyjson_val *key, *val;

        yyjson_obj_foreach(obj, idx, max, key, val) {
            const char* name = yyjson_get_str(key);
            if(!_mcp_find_arg(args, name))
                return rd_format("Unknown argument '%.100s'.", name);
        }
    }

    return NULL;
}

static yyjson_mut_val* _mcp_tool_schema(yyjson_mut_doc* doc,
                                        const McpArg* args) {
    yyjson_mut_val* schema = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, schema, "type", "object");

    yyjson_mut_val* props = yyjson_mut_obj_add_obj(doc, schema, "properties");
    yyjson_mut_val* required = yyjson_mut_arr(doc);

    for(const McpArg* a = args; a->name; a++) {
        yyjson_mut_val* p = yyjson_mut_obj_add_obj(doc, props, a->name);
        yyjson_mut_obj_add_str(doc, p, "type", _mcp_type_name(a->type));
        yyjson_mut_obj_add_str(doc, p, "description", a->description);

        if(a->required) yyjson_mut_arr_add_str(doc, required, a->name);
    }

    if(yyjson_mut_arr_size(required))
        yyjson_mut_obj_add_val(doc, schema, "required", required);

    // unknown arguments are rejected by _mcp_validate()
    yyjson_mut_obj_add_bool(doc, schema, "additionalProperties", false);
    return schema;
}

void mcp_tools_init(void) {
    if(mcp_tools_raw) return;

    // static strings from MCP_TOOLS are referenced, not copied
    yyjson_mut_doc* doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_val* list = yyjson_mut_obj_add_arr(doc, root, "tools");

    for(usize i = 0; i < rd_count_of(MCP_TOOLS); i++) {
        const McpTool* tool = &MCP_TOOLS[i];

        yyjson_mut_val* t = yyjson_mut_arr_add_obj(doc, list);
        yyjson_mut_obj_add_str(doc, t, "name", tool->name);
        yyjson_mut_obj_add_str(doc, t, "description", tool->description);
        yyjson_mut_obj_add_val(doc, t, "inputSchema",
                               _mcp_tool_schema(doc, tool->args));
    }

    mcp_tools_raw = yyjson_mut_write(doc, YYJSON_WRITE_NOFLAG, NULL);
    yyjson_mut_doc_free(doc);
}

void mcp_tools_deinit(void) {
    free(mcp_tools_raw);
    mcp_tools_raw = NULL;
}

const char* mcp_tools_list(void) {
    return mcp_tools_raw ? mcp_tools_raw : "{\"tools\":[]}";
}

yyjson_mut_val* mcp_tools_call(RDContext* ctx, const char* name,
                               yyjson_val* args, yyjson_mut_doc* doc) {
    for(usize i = 0; i < rd_count_of(MCP_TOOLS); i++) {
        const McpTool* tool = &MCP_TOOLS[i];
        if(strcmp(tool->name, name) != 0) continue;

        const char* err = _mcp_validate(tool->args, args);
        if(err) return mcp_result_error(doc, err);

        if(tool->needs_context && !ctx)
            return mcp_result_error(doc, "No file open.");

        yyjson_mut_val* result = tool->run(ctx, args, doc);
        return result ? result : mcp_result_error(doc, "Internal tool error.");
    }

    return NULL;
}
