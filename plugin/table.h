#pragma once

#include <redasm/redasm.h>
#include <yyjson.h>

typedef struct McpPage {
    usize offset;
    usize limit;
    usize total;        // items matching the filter
    const char* filter; // NULL for none
} McpPage;

// A table stored as JSON inside the reply document:
//   {"columns": ["address", "name"], "rows": [["0x00401000", "main"], ...]}
// It's freed with the document, there's nothing to destroy.
// mcp_table_result() renders it as aligned text for the model;
typedef struct McpTable {
    yyjson_mut_doc* doc;
    yyjson_mut_val* root;
    yyjson_mut_val* rows;
    yyjson_mut_val* row; // the row being filled, NULL before the first one
    usize ncolumns;
    bool untrusted;
} McpTable;

// "offset", "limit" and "filter" of a tool with x-paging
void mcp_page_init(McpPage* self, yyjson_val* args);

// call once per item, in a stable order: false if the item is filtered out or
// outside the requested window ('name' can be NULL when there's no filter)
bool mcp_page_take(McpPage* self, const char* name);

// 'columns' is NULL-terminated
McpTable mcp_table_begin(yyjson_mut_doc* doc, const char* const* columns);

// names in this table come from the analyzed binary: a warning line is added
void mcp_table_set_untrusted(McpTable* self);

void mcp_table_row(McpTable* self);
void mcp_table_str(McpTable* self, const char* s); // NULL is shown as "-"
void mcp_table_uint(McpTable* self, u64 v);
void mcp_table_fmt(McpTable* self, const char* fmt, ...) RD_PRINTF_CHECK(2, 3);
void mcp_table_address(McpTable* self, const RDContext* ctx, RDAddress address);
void mcp_table_val(McpTable* self, yyjson_mut_val* v);

// the tool result: header, rows and, with 'page', a paging footer
yyjson_mut_val* mcp_table_result(McpTable* self, const McpPage* page);

void mcp_table_deinit(void);
