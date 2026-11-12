#pragma once

#include "spec/def.h"
#include <redasm/redasm.h>
#include <yyjson.h>

// the first entry is the default
#define EXTERNAL_KINDS(X)                                                      \
    X(all, RD_EXT_NONE)                                                        \
    X(import, RD_EXT_IMPORTED)                                                 \
    X(export, RD_EXT_EXPORTED)

MCP_ENUM_DECL(external_kind, RDExternalKind);

#define SYM_BIT(k) (1U << (k))

// the first entry is the default: what list_functions and
// list_segments don't cover
#define SYMBOL_KINDS(X)                                                        \
    X(labels, SYM_BIT(RD_SYMBOL_TYPE) | SYM_BIT(RD_SYMBOL_NAME))               \
    X(data, SYM_BIT(RD_SYMBOL_TYPE))                                           \
    X(name, SYM_BIT(RD_SYMBOL_NAME))                                           \
    X(function, SYM_BIT(RD_SYMBOL_FUNCTION))                                   \
    X(segment, SYM_BIT(RD_SYMBOL_SEGMENT))                                     \
    X(all, ~0U)

MCP_ENUM_DECL(symbol_kind, u32);

// clang-format off
yyjson_mut_val* mcp_spec_status(RDContext* ctx, yyjson_val* args, yyjson_mut_doc* doc);
yyjson_mut_val* mcp_spec_open_file(RDContext* ctx, yyjson_val* args, yyjson_mut_doc* doc);
yyjson_mut_val* mcp_spec_list_segments(RDContext* ctx, yyjson_val* args, yyjson_mut_doc* doc);
yyjson_mut_val* mcp_spec_list_functions(RDContext* ctx, yyjson_val* args, yyjson_mut_doc* doc);
yyjson_mut_val* mcp_spec_list_mappings(RDContext* ctx, yyjson_val* args, yyjson_mut_doc* doc);
yyjson_mut_val* mcp_spec_list_externals(RDContext* ctx, yyjson_val* args, yyjson_mut_doc* doc);
yyjson_mut_val* mcp_spec_list_symbols(RDContext* ctx, yyjson_val* args, yyjson_mut_doc* doc);
yyjson_mut_val* mcp_spec_get_name(RDContext* ctx, yyjson_val* args, yyjson_mut_doc* doc);
yyjson_mut_val* mcp_spec_get_address(RDContext* ctx, yyjson_val* args, yyjson_mut_doc* doc);
// clang-format on
