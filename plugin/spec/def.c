#include "def.h"
#include "spec/entries.h"

// clang-format off
#define MCP_RANGE_ARGS                                                                     \
    MCP_OPT_INT("offset", "Index of the first item to return (default 0)"),               \
    MCP_OPT_INT("limit", "Maximum number of items to return (default 100, max 500)")

#define MCP_PAGE_ARGS                                                                    \
    MCP_OPT_STR("filter", "Only items whose name contains this text (case-insensitive)"), \
    MCP_RANGE_ARGS

MCP_NO_ARGS(status);
MCP_ARGS(open_file, 
         MCP_REQ_STR("path", "Absolute path of the file to open"));
MCP_NO_ARGS(list_segments);
MCP_ARGS(list_functions, 
         MCP_PAGE_ARGS);
MCP_NO_ARGS(list_mappings);
MCP_ARGS(list_externals, 
         MCP_OPT_ENUM("kind", external_kind, "Imports, exports or both (default all)"), 
         MCP_PAGE_ARGS);
MCP_ARGS(list_symbols, 
         MCP_OPT_ENUM("kind", symbol_kind, "Symbol kind; the default 'labels' is data and plain names, which list_functions and list_segments don't cover"), 
         MCP_PAGE_ARGS);
MCP_ARGS(get_name, 
         MCP_REQ_STR("address", "Hex address like 0x401000, or a symbol name"));
MCP_ARGS(get_address, 
         MCP_REQ_STR("name", "Exact symbol name, e.g. WndProc or sub_401000"));
MCP_ARGS(get_xrefs_to,
         MCP_REQ_STR("address", "Hex address like 0x401000, or a symbol name"),
         MCP_OPT_ENUM("kind", xref_kind, "Reference kind: code is call and jump, data is read, write and address (default all)"),
         MCP_OPT_STR("filter", "Only references from functions whose name contains this text (case-insensitive)"),
         MCP_RANGE_ARGS);
MCP_ARGS(get_xrefs_from,
         MCP_REQ_STR("address", "Hex address of an instruction or data item, or a symbol name"),
         MCP_OPT_ENUM("kind", xref_kind, "Reference kind: code is call and jump, data is read, write and address (default all)"),
         MCP_OPT_STR("filter", "Only references to targets whose name contains this text (case-insensitive)"),
         MCP_RANGE_ARGS);
MCP_ARGS(get_callees,
         MCP_REQ_STR("address", "Any address inside the function, or its name"),
         MCP_OPT_STR("filter", "Only calls to targets whose name contains this text (case-insensitive)"),
         MCP_RANGE_ARGS);
MCP_ARGS(get_callers,
         MCP_REQ_STR("address", "Any address inside the function, or its name"),
         MCP_OPT_STR("filter", "Only calls from functions whose name contains this text (case-insensitive)"),
         MCP_RANGE_ARGS);

const McpTool MCP_TOOLS[] = {
    MCP_TOOL(status, "Report the open file and analysis state"),
    MCP_TOOL(open_file, "Open a file for analysis, replacing the current one. Only files inside the configured allowed directories can be opened."),
    MCP_CTX_TOOL(list_segments, "List the segments of the open file: address range, size in bytes, permissions and name."),
    MCP_CTX_TOOL(list_functions, "List the functions found by the analysis, sorted by address. Supports filtering by name and paging."),
    MCP_CTX_TOOL(list_mappings, "List how the input file is mapped in memory: file offset, address range and size of each mapping."),
    MCP_CTX_TOOL(list_externals, "List imported and exported symbols with their module. Supports filtering by name and paging."),
    MCP_CTX_TOOL(list_symbols, "List named data and labels, with their type when known. Supports filtering by name and paging."),
    MCP_CTX_TOOL(get_name, "Describe an address: its name, or the function it falls inside, plus its segment and import/export status."),
    MCP_CTX_TOOL(get_address, "Find the address of a symbol by its exact name."),
    MCP_CTX_TOOL(get_xrefs_to, "List the places that reference an address (who calls this function, who reads this variable), with the function each one is in. Supports paging."),
    MCP_CTX_TOOL(get_xrefs_from, "List what a single instruction or data item references: call and jump targets, data it reads, writes or takes the address of. Supports paging."),
    MCP_CTX_TOOL(get_callees, "List every call made by a function, one row per call site, with direct or indirect. Supports filtering by target name and paging."),
    MCP_CTX_TOOL(get_callers, "List every call to a function, one row per call site, with the calling function. Includes indirect calls. Supports filtering by caller name and paging."),
};

const usize MCP_TOOLS_COUNT = rd_count_of(MCP_TOOLS);

// clang-format on
