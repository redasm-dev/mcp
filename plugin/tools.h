#pragma once

#include <redasm/redasm.h>
#include <yyjson.h>

// the value chosen for argument 'key', the first entry when it's missing
#define MCP_GET_ENUM_VAL(args, key, name)                                      \
    name##_values[mcp_enum_index((args), (key), name##_names)]

// only in the .c with MCP_ENUM: rd_count_of needs the complete array
#define MCP_GET_ENUM_NAME(name, v)                                             \
    mcp_enum_name(name##_names, name##_values, sizeof(*name##_values),         \
                  rd_count_of(name##_values), &(v))

void mcp_tools_init(void);
void mcp_tools_deinit(void);

// the serialized 'tools/list' result, valid until mcp_tools_deinit()
const char* mcp_tools_list(void);

// the 'tools/call' result built in 'doc', NULL if the tool doesn't exist
yyjson_mut_val* mcp_tools_call(RDContext* ctx, const char* name,
                               yyjson_val* args, yyjson_mut_doc* doc);

// index of the value of 'key' in 'names', 0 (the default) when it's missing.
// tools.c rejects values not in 'names' before the tool runs.
usize mcp_enum_index(yyjson_val* args, const char* key,
                     const char* const* names);

// the name of value 'v', NULL if it has none
const char* mcp_enum_name(const char* const* names, const void* values,
                          usize size, usize n, const void* v);
