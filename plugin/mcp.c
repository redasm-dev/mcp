#include "mcp.h"
#include "tools.h"
#include <redasm/redasm.h>
#include <string.h>

#define MCP_PROTOCOL_VERSION "2025-11-25"

#define MCP_ERROR_PARSE (-32700)
#define MCP_ERROR_INVALID_REQUEST (-32600)
#define MCP_ERROR_METHOD_NOT_FOUND (-32601)
#define MCP_ERROR_INVALID_PARAMS (-32602)

static cJSON* mcp_initialize_obj = NULL;
static char* mcp_reply = NULL;

static const char* _mcp_send(cJSON* msg) {
    cJSON_free(mcp_reply);

    mcp_reply = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);

    RD_LOG_DEBUG("send: %.500s", mcp_reply ? mcp_reply : "(null)");
    return mcp_reply ? mcp_reply : "";
}

static cJSON* _mcp_result(const char* text, bool is_error) {
    cJSON* result = cJSON_CreateObject();
    cJSON* content = cJSON_AddArrayToObject(result, "content");

    cJSON* item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text);
    cJSON_AddItemToArray(content, item);

    cJSON_AddBoolToObject(result, "isError", is_error);
    return result;
}

static cJSON* _mcp_message(const cJSON* id) {
    cJSON* msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "jsonrpc", "2.0");
    cJSON_AddItemToObject(msg, "id",
                          id ? cJSON_Duplicate(id, true) : cJSON_CreateNull());
    return msg;
}

static const char* mcp_send_result(const cJSON* id, cJSON* result) {
    cJSON* msg = _mcp_message(id);
    cJSON_AddItemToObject(msg, "result", result);
    return _mcp_send(msg);
}

static const char* _mcp_send_error(const cJSON* id, int code,
                                   const char* message) {
    cJSON* msg = _mcp_message(id);
    cJSON* error = cJSON_AddObjectToObject(msg, "error");
    cJSON_AddNumberToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    return _mcp_send(msg);
}

static const char* _mcp_dispatch(RDContext* ctx, const cJSON* req) {
    const cJSON* id = cJSON_GetObjectItemCaseSensitive(req, "id");
    const char* method =
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "method"));

    // no 'id' member: a notification never gets a reply
    if(!id) return "";

    // 0 is a valid id, so check the type and never the value
    if((!cJSON_IsNumber(id) && !cJSON_IsString(id)) || !method)
        return _mcp_send_error(NULL, MCP_ERROR_INVALID_REQUEST,
                               "Invalid Request");

    if(!strcmp(method, "initialize"))
        return mcp_send_result(id, cJSON_Duplicate(mcp_initialize_obj, true));

    if(!strcmp(method, "ping"))
        return mcp_send_result(id, cJSON_CreateObject());

    if(!strcmp(method, "tools/list"))
        return mcp_send_result(id, mcp_tools_list());

    if(!strcmp(method, "tools/call")) {
        const cJSON* params = cJSON_GetObjectItemCaseSensitive(req, "params");
        const cJSON* args =
            cJSON_GetObjectItemCaseSensitive(params, "arguments");
        const char* name = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(params, "name"));

        cJSON* result = name ? mcp_tools_call(ctx, name, args) : NULL;

        if(!result) {
            return _mcp_send_error(id, MCP_ERROR_INVALID_PARAMS,
                                   "Unknown tool");
        }

        return mcp_send_result(id, result);
    }

    return _mcp_send_error(id, MCP_ERROR_METHOD_NOT_FOUND, "Method not found");
}

void mcp_init(void) {
    if(mcp_initialize_obj) return;

    mcp_initialize_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(mcp_initialize_obj, "protocolVersion",
                            MCP_PROTOCOL_VERSION);

    cJSON* caps = cJSON_AddObjectToObject(mcp_initialize_obj, "capabilities");
    cJSON_AddObjectToObject(caps, "tools");

    cJSON* info = cJSON_AddObjectToObject(mcp_initialize_obj, "serverInfo");
    cJSON_AddStringToObject(info, "name", "redasm");
    cJSON_AddStringToObject(info, "version", rd_version_string());
}

void mcp_deinit(void) {
    cJSON_Delete(mcp_initialize_obj);
    cJSON_free(mcp_reply);
    mcp_initialize_obj = NULL;
    mcp_reply = NULL;
}

cJSON* mcp_result_text(const char* text) { return _mcp_result(text, false); }
cJSON* mcp_result_error(const char* text) { return _mcp_result(text, true); }

RDCommandValue mcp_execute(RDContext* ctx, const RDCommandValue* args) {
    const char* line = args[0].s;
    RD_LOG_DEBUG("recv: %.500s", line);

    const char* reply;
    cJSON* req = cJSON_Parse(line);

    // clang-format off
    if(cJSON_IsObject(req)) reply = _mcp_dispatch(ctx, req);
    else if(req) reply = _mcp_send_error(NULL, MCP_ERROR_INVALID_REQUEST, "Invalid Request");
    else reply = _mcp_send_error(NULL, MCP_ERROR_PARSE, "Parse error");
    // clang-format on

    cJSON_Delete(req);
    return (RDCommandValue){.kind = RD_CMDARG_STRING, .s = reply};
}
