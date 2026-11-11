#pragma once

#include <redasm/redasm.h>
#include <yyjson.h>

void mcp_init(void);
void mcp_deinit(void);
RDCommandValue mcp_execute(RDContext* ctx, const RDCommandValue* args);

// tool results, 'text' is copied into 'doc'
yyjson_mut_val* mcp_result_error(yyjson_mut_doc* doc, const char* text);
yyjson_mut_val* mcp_result_text(yyjson_mut_doc* doc, const char* text);
yyjson_mut_val* mcp_result_text_n(yyjson_mut_doc* doc, const char* text,
                                  usize n);

// a JSON string copied into 'doc', with invalid UTF-8 replaced by U+FFFD
yyjson_mut_val* mcp_str(yyjson_mut_doc* doc, const char* text, usize n);
