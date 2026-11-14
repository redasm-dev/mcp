#include "tools.h"
#include "mcp.h"
#include "tools_gen.h"
#include <inttypes.h>
#include <stdint.h>
#include <string.h>

// MCP_TOOLS_JSON parsed once: every call is checked against its inputSchema
static yyjson_doc* mcp_tools_doc;
static yyjson_val** mcp_tools_schemas; // same order as MCP_TOOL_TABLE

// the subset of JSON Schema the spec uses
static bool _mcp_type_matches(const char* type, yyjson_val* v) {
    if(!strcmp(type, "string")) return yyjson_is_str(v);
    if(!strcmp(type, "integer")) return yyjson_is_int(v); // 5.0 is not
    if(!strcmp(type, "boolean")) return yyjson_is_bool(v);
    return false;
}

// false when it doesn't fit an i64, the type the accessors return
static bool _mcp_int_value(yyjson_val* v, i64* out) {
    if(yyjson_is_uint(v)) {
        u64 u = yyjson_get_uint(v);
        if(u > INT64_MAX) return false;
        *out = (i64)u;
        return true;
    }

    *out = yyjson_get_sint(v);
    return true;
}

// JSON Schema counts characters, not bytes
static usize _mcp_utf8_length(const char* s, usize n) {
    usize len = 0;

    for(usize i = 0; i < n; i++) {
        if(((unsigned char)s[i] & 0xC0) != 0x80) len++;
    }

    return len;
}

static const char* _mcp_check_range(const char* name, yyjson_val* schema,
                                    i64 n) {
    yyjson_val* vmin = yyjson_obj_get(schema, "minimum");
    yyjson_val* vmax = yyjson_obj_get(schema, "maximum");
    i64 lo = 0, hi = 0;
    bool has_lo = vmin && _mcp_int_value(vmin, &lo);
    bool has_hi = vmax && _mcp_int_value(vmax, &hi);

    if((!has_lo || n >= lo) && (!has_hi || n <= hi)) return NULL;

    if(has_lo && has_hi) {
        return rd_format("Argument '%s' must be between %" PRId64
                         " and %" PRId64 ".",
                         name, lo, hi);
    }

    if(has_lo)
        return rd_format("Argument '%s' must be at least %" PRId64 ".", name,
                         lo);

    return rd_format("Argument '%s' must be at most %" PRId64 ".", name, hi);
}

static const char* _mcp_check_enum(const char* name, yyjson_val* values,
                                   yyjson_val* v) {
    usize idx, max;
    yyjson_val* e;

    yyjson_arr_foreach(values, idx, max, e) {
        if(yyjson_equals(e, v)) return NULL;
    }

    RDScratchBuffer* buf = rd_scratch_create();
    rd_scratch_putf(buf, "Argument '%s' must be one of:", name);

    yyjson_arr_foreach(values, idx, max, e) {
        rd_scratch_putf(buf, " %s", yyjson_get_str(e));
    }

    const char* msg = rd_format("%s.", rd_scratch_data(buf));
    rd_scratch_destroy(buf);
    return msg;
}

// NULL if 'v' matches the argument's 'schema', otherwise a message for the
// model
static const char* _mcp_check_arg(const char* name, yyjson_val* schema,
                                  yyjson_val* v) {
    const char* type = yyjson_get_str(yyjson_obj_get(schema, "type"));

    if(!_mcp_type_matches(type, v))
        return rd_format("Argument '%s' must be of type %s.", name, type);

    if(yyjson_is_int(v)) {
        i64 n;
        if(!_mcp_int_value(v, &n))
            return rd_format("Argument '%s' is too large.", name);

        return _mcp_check_range(name, schema, n);
    }

    if(yyjson_is_str(v)) {
        yyjson_val* maxlen = yyjson_obj_get(schema, "maxLength");
        usize len = _mcp_utf8_length(yyjson_get_str(v), yyjson_get_len(v));

        if(maxlen && len > yyjson_get_uint(maxlen)) {
            return rd_format("Argument '%s' can be at most %" PRIu64
                             " characters.",
                             name, yyjson_get_uint(maxlen));
        }

        yyjson_val* values = yyjson_obj_get(schema, "enum");
        if(values) return _mcp_check_enum(name, values, v);
    }

    return NULL;
}

// NULL if 'args' matches the tool's 'schema', otherwise a message for the model
static const char* _mcp_validate(yyjson_val* schema, yyjson_val* args) {
    // a missing 'arguments' member counts as an empty object
    if(args && !yyjson_is_obj(args)) return "'arguments' must be an object.";

    yyjson_val* props = yyjson_obj_get(schema, "properties");

    usize idx, max;
    yyjson_val* r;

    yyjson_arr_foreach(yyjson_obj_get(schema, "required"), idx, max, r) {
        const char* name = yyjson_get_str(r);
        if(!yyjson_obj_get(args, name))
            return rd_format("Missing required argument '%s'.", name);
    }

    yyjson_val *key, *v;

    yyjson_obj_foreach(args, idx, max, key, v) {
        const char* name = yyjson_get_str(key);
        yyjson_val* p = yyjson_obj_get(props, name);

        if(!p) return rd_format("Unknown argument '%.100s'.", name);

        const char* err = _mcp_check_arg(name, p, v);
        if(err) return err;
    }

    return NULL;
}

void mcp_tools_init(void) {
    if(mcp_tools_doc) return;

    // read only: without YYJSON_READ_INSITU the input isn't modified
    mcp_tools_doc =
        yyjson_read(MCP_TOOLS_JSON, MCP_TOOLS_JSON_SIZE, YYJSON_READ_NOFLAG);

    if(!mcp_tools_doc) {
        RD_LOG_FAIL("%s", "MCP: the embedded tool list is not valid JSON");
        return;
    }

    yyjson_val* tools =
        yyjson_obj_get(yyjson_doc_get_root(mcp_tools_doc), "tools");
    mcp_tools_schemas =
        (yyjson_val**)rd_alloc(MCP_TOOL_COUNT * sizeof(*mcp_tools_schemas));

    // generated together: a mismatch is a generator bug
    for(usize i = 0; i < MCP_TOOL_COUNT; i++) {
        yyjson_val* t = yyjson_arr_get(tools, i);

        if(!yyjson_equals_str(yyjson_obj_get(t, "name"),
                              MCP_TOOL_TABLE[i].name)) {
            RD_LOG_FAIL("MCP: tool '%s' doesn't match the tool list",
                        MCP_TOOL_TABLE[i].name);
            mcp_tools_deinit();
            return;
        }

        mcp_tools_schemas[i] = yyjson_obj_get(t, "inputSchema");
    }
}

void mcp_tools_deinit(void) {
    rd_free((void*)mcp_tools_schemas);
    yyjson_doc_free(mcp_tools_doc);
    mcp_tools_schemas = NULL;
    mcp_tools_doc = NULL;
}

const char* mcp_tools_list(void) {
    return mcp_tools_doc ? MCP_TOOLS_JSON : "{\"tools\":[]}";
}

yyjson_mut_val* mcp_tools_call(RDContext* ctx, const char* name,
                               yyjson_val* args, yyjson_mut_doc* doc) {
    if(!mcp_tools_doc) return NULL;

    for(usize i = 0; i < MCP_TOOL_COUNT; i++) {
        const McpToolEntry* tool = &MCP_TOOL_TABLE[i];
        if(strcmp(tool->name, name) != 0) continue;

        const char* err = _mcp_validate(mcp_tools_schemas[i], args);
        if(err) return mcp_result_error(doc, err);

        if(tool->needs_context && !ctx)
            return mcp_result_error(doc, "No file open.");

        yyjson_mut_val* result = tool->run(ctx, args, doc);
        return result ? result : mcp_result_error(doc, "Internal tool error.");
    }

    return NULL;
}
