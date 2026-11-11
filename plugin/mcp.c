#include "mcp.h"
#include "session.h"
#include "tools.h"
#include <stdlib.h>
#include <string.h>

#define MCP_PROTOCOL_VERSION "2025-11-25"

#define MCP_ERROR_PARSE (-32700)
#define MCP_ERROR_INVALID_REQUEST (-32600)
#define MCP_ERROR_METHOD_NOT_FOUND (-32601)
#define MCP_ERROR_INVALID_PARAMS (-32602)
#define MCP_ERROR_INTERNAL (-32603)

static char* mcp_initialize_raw = NULL; // serialized once in mcp_init()
static char* mcp_reply = NULL;          // valid until the next execute

static yyjson_mut_val* _mcp_message(yyjson_mut_doc* doc, yyjson_val* id) {
    yyjson_mut_val* msg = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, msg, "jsonrpc", "2.0");
    yyjson_mut_obj_add_val(doc, msg, "id",
                           id ? yyjson_val_mut_copy(doc, id)
                              : yyjson_mut_null(doc));
    return msg;
}

static void _mcp_set_error(yyjson_mut_doc* doc, yyjson_mut_val* msg, int code,
                           const char* message) {
    yyjson_mut_val* error = yyjson_mut_obj_add_obj(doc, msg, "error");
    yyjson_mut_obj_add_int(doc, error, "code", code);
    yyjson_mut_obj_add_strcpy(doc, error, "message", message);
}

static yyjson_mut_val* _mcp_error(yyjson_mut_doc* doc, yyjson_val* id, int code,
                                  const char* message) {
    yyjson_mut_val* msg = _mcp_message(doc, id);
    _mcp_set_error(doc, msg, code, message);
    return msg;
}

static yyjson_mut_val* _mcp_result(yyjson_mut_doc* doc, yyjson_val* id,
                                   yyjson_mut_val* result) {
    yyjson_mut_val* msg = _mcp_message(doc, id);
    yyjson_mut_obj_add_val(doc, msg, "result", result);
    return msg;
}

// NULL for notifications, which never get a reply
static yyjson_mut_val* _mcp_dispatch(RDContext* ctx, yyjson_val* req,
                                     yyjson_mut_doc* doc) {
    yyjson_val* id = yyjson_obj_get(req, "id");
    const char* method = yyjson_get_str(yyjson_obj_get(req, "method"));

    // no 'id' member: a notification
    if(!id) return NULL;

    // 0 is a valid id, so check the type and never the value
    if((!yyjson_is_num(id) && !yyjson_is_str(id)) || !method)
        return _mcp_error(doc, NULL, MCP_ERROR_INVALID_REQUEST,
                          "Invalid Request");

    if(!strcmp(method, "initialize"))
        return _mcp_result(doc, id, yyjson_mut_raw(doc, mcp_initialize_raw));

    if(!strcmp(method, "ping"))
        return _mcp_result(doc, id, yyjson_mut_obj(doc));

    if(!strcmp(method, "tools/list"))
        return _mcp_result(doc, id, yyjson_mut_raw(doc, mcp_tools_list()));

    if(!strcmp(method, "tools/call")) {
        yyjson_val* params = yyjson_obj_get(req, "params");
        const char* name = yyjson_get_str(yyjson_obj_get(params, "name"));
        yyjson_val* args = yyjson_obj_get(params, "arguments");

        yyjson_mut_val* result =
            name ? mcp_tools_call(mcp_session_context(ctx), name, args, doc)
                 : NULL;

        if(!result)
            return _mcp_error(doc, id, MCP_ERROR_INVALID_PARAMS,
                              "Unknown tool");

        return _mcp_result(doc, id, result);
    }

    return _mcp_error(doc, id, MCP_ERROR_METHOD_NOT_FOUND, "Method not found");
}

static const char* _mcp_write(yyjson_mut_doc* doc, yyjson_mut_val* msg) {
    free(mcp_reply);

    // the default flags write minified JSON: on stdio a newline ends a message
    yyjson_mut_doc_set_root(doc, msg);
    mcp_reply = yyjson_mut_write(doc, YYJSON_WRITE_NOFLAG, NULL);

    if(!mcp_reply) {
        // e.g. invalid UTF-8 from the binary: answer anyway, never stay silent
        yyjson_mut_val* id = yyjson_mut_obj_get(msg, "id");
        yyjson_mut_val* fallback = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, fallback, "jsonrpc", "2.0");
        yyjson_mut_obj_add_val(doc, fallback, "id",
                               id ? id : yyjson_mut_null(doc));
        _mcp_set_error(doc, fallback, MCP_ERROR_INTERNAL,
                       "Reply could not be encoded as valid JSON");

        yyjson_mut_doc_set_root(doc, fallback);
        mcp_reply = yyjson_mut_write(doc, YYJSON_WRITE_NOFLAG, NULL);
    }

    // RD_LOG_DEBUG("send: %.500s", mcp_reply ? mcp_reply : "(null)");
    return mcp_reply ? mcp_reply : "";
}

// length of a valid UTF-8 sequence at 's' (RFC 3629), 0 if invalid
static usize _mcp_utf8_seq(const unsigned char* s, usize n) {
    unsigned char c = s[0];
    if(c < 0x80) return 1;

    usize len;
    u32 min;

    if((c & 0xE0) == 0xC0)
        len = 2, min = 0x80;
    else if((c & 0xF0) == 0xE0)
        len = 3, min = 0x800;
    else if((c & 0xF8) == 0xF0)
        len = 4, min = 0x10000;
    else
        return 0;

    if(len > n) return 0;

    u32 cp = c & (0x7F >> len);

    for(usize i = 1; i < len; i++) {
        if((s[i] & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (s[i] & 0x3F);
    }

    // overlong, surrogate or out of range
    if(cp < min || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) return 0;
    return len;
}

static yyjson_mut_val* _mcp_tool_result(yyjson_mut_doc* doc, const char* text,
                                        usize n, bool is_error) {
    yyjson_mut_val* result = yyjson_mut_obj(doc);
    yyjson_mut_val* content = yyjson_mut_obj_add_arr(doc, result, "content");

    yyjson_mut_val* item = yyjson_mut_arr_add_obj(doc, content);
    yyjson_mut_obj_add_str(doc, item, "type", "text");
    yyjson_mut_obj_add_val(doc, item, "text", mcp_str(doc, text, n));

    yyjson_mut_obj_add_bool(doc, result, "isError", is_error);
    return result;
}

RDCommandValue mcp_execute(RDContext* ctx, const RDCommandValue* args) {
    const char* line = args[0].s;
    // RD_LOG_DEBUG("recv: %.500s", line);

    yyjson_doc* req = yyjson_read(line, strlen(line), YYJSON_READ_NOFLAG);
    yyjson_val* root = yyjson_doc_get_root(req);
    yyjson_mut_doc* doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val* msg;

    // clang-format off
    if(yyjson_is_obj(root)) msg = _mcp_dispatch(ctx, root, doc);
    else if(req) msg = _mcp_error(doc, NULL, MCP_ERROR_INVALID_REQUEST, "Invalid Request");
    else msg = _mcp_error(doc, NULL, MCP_ERROR_PARSE, "Parse error");
    // clang-format on

    // strings from 'req' are referenced by 'doc', write before freeing it
    const char* reply = msg ? _mcp_write(doc, msg) : "";

    yyjson_mut_doc_free(doc);
    yyjson_doc_free(req);
    return (RDCommandValue){.kind = RD_CMDARG_STRING, .s = reply};
}

// tool text often comes from the binary: invalid bytes become U+FFFD instead
// of making the whole reply unencodable
yyjson_mut_val* mcp_str(yyjson_mut_doc* doc, const char* text, usize n) {
    const unsigned char* s = (const unsigned char*)text;
    usize i = 0;

    while(i < n) {
        usize len = _mcp_utf8_seq(s + i, n - i);
        if(!len || !s[i]) break;
        i += len;
    }

    // common case: already valid
    if(i == n) return yyjson_mut_strncpy(doc, text, n);

    RDScratchBuffer* buf = rd_scratch_create();
    rd_scratch_append(buf, text, i);

    while(i < n) {
        usize len = _mcp_utf8_seq(s + i, n - i);

        if(len && s[i]) {
            rd_scratch_append(buf, s + i, len);
            i += len;
        }
        else {
            rd_scratch_append(buf, "\xEF\xBF\xBD", 3); // U+FFFD
            i++;
        }
    }

    yyjson_mut_val* v =
        yyjson_mut_strncpy(doc, rd_scratch_data(buf), rd_scratch_length(buf));
    rd_scratch_destroy(buf);
    return v;
}

yyjson_mut_val* mcp_result_text_n(yyjson_mut_doc* doc, const char* text,
                                  usize n) {
    return _mcp_tool_result(doc, text ? text : "", text ? n : 0, false);
}

yyjson_mut_val* mcp_result_text(yyjson_mut_doc* doc, const char* text) {
    return mcp_result_text_n(doc, text, text ? strlen(text) : 0);
}

yyjson_mut_val* mcp_result_error(yyjson_mut_doc* doc, const char* text) {
    if(!text) text = "";
    return _mcp_tool_result(doc, text, strlen(text), true);
}

void mcp_init(void) {
    if(mcp_initialize_raw) return;

    yyjson_mut_doc* doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val* r = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, r);

    yyjson_mut_obj_add_str(doc, r, "protocolVersion", MCP_PROTOCOL_VERSION);

    yyjson_mut_val* caps = yyjson_mut_obj_add_obj(doc, r, "capabilities");
    yyjson_mut_obj_add_obj(doc, caps, "tools");

    yyjson_mut_val* info = yyjson_mut_obj_add_obj(doc, r, "serverInfo");
    yyjson_mut_obj_add_str(doc, info, "name", "redasm");
    yyjson_mut_obj_add_strcpy(doc, info, "version", rd_version_string());

    mcp_initialize_raw = yyjson_mut_write(doc, YYJSON_WRITE_NOFLAG, NULL);
    yyjson_mut_doc_free(doc);
}

void mcp_deinit(void) {
    free(mcp_initialize_raw);
    free(mcp_reply);
    mcp_initialize_raw = NULL;
    mcp_reply = NULL;
}
