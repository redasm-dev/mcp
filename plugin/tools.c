#include "tools.h"
#include "mcp.h"
#include "spec/def.h"
#include <string.h>

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

static bool _mcp_enum_find(const char* const* names, const char* s,
                           usize* idx) {
    for(usize i = 0; names[i]; i++) {
        if(strcmp(names[i], s) != 0) continue;
        if(idx) *idx = i;
        return true;
    }

    return false;
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

        if(a->values && !_mcp_enum_find(a->values, yyjson_get_str(v), NULL)) {
            RDScratchBuffer* buf = rd_scratch_create();
            rd_scratch_putf(buf, "Argument '%s' must be one of:", a->name);

            for(const char* const* n = a->values; *n; n++)
                rd_scratch_putf(buf, " %s", *n);

            const char* msg = rd_format("%s.", rd_scratch_data(buf));
            rd_scratch_destroy(buf);
            return msg;
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

        if(a->values) {
            yyjson_mut_val* e = yyjson_mut_obj_add_arr(doc, p, "enum");

            for(const char* const* n = a->values; *n; n++)
                yyjson_mut_arr_add_str(doc, e, *n);

            // the first entry is used when the argument is missing
            if(!a->required)
                yyjson_mut_obj_add_str(doc, p, "default", a->values[0]);
        }

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

    for(usize i = 0; i < MCP_TOOLS_COUNT; i++) {
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
    for(usize i = 0; i < MCP_TOOLS_COUNT; i++) {
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

usize mcp_enum_index(yyjson_val* args, const char* key,
                     const char* const* names) {
    const char* s = yyjson_get_str(yyjson_obj_get(args, key));
    usize idx = 0;
    if(s) _mcp_enum_find(names, s, &idx);
    return idx;
}

const char* mcp_enum_name(const char* const* names, const void* values,
                          usize size, usize n, const void* v) {
    const char* p = (const char*)values;

    for(usize i = 0; i < n; i++, p += size) {
        if(!memcmp(p, v, size)) return names[i];
    }

    return NULL;
}
