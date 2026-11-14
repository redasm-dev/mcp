#include "mcp.h"
#include "session.h"
#include "table.h"
#include "tools_gen.h"
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

// The tool executors. Arguments and descriptions live in spec/tools.json:
// an executor runs only after its arguments were validated against it, and
// reads them with the generated mcp_arg_<tool>_<arg>() accessors.

#define MCP_MAX_BYTES 4096   // patch_bytes: hex text, can't be a maxLength
#define MCP_MAX_STR_CELL 200 // list_strings, get_data str: output only

#define MCP_BAD_ADDRESS                                                        \
    "Not an address or a known name: use hex like 0x401000 or a symbol name."

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

static const char* _mcp_format_address(const RDContext* ctx, RDAddress a) {
    int width = (int)rd_get_ptr_size(ctx) * 2;
    return rd_format("0x%0*" PRIX64, width > 0 ? width : 8, (u64)a);
}

// accepts "0x401000", a name such as "main" or "sub_401000", or plain hex
static bool _mcp_parse_address(RDContext* ctx, const char* s, RDAddress* out) {
    bool hexprefix = s[0] == '0' && (s[1] == 'x' || s[1] == 'X');

    // a name first: "add" or "face" are valid hex too
    if(!hexprefix && rd_get_address(ctx, s, out)) return true;

    const char* digits = hexprefix ? s + 2 : s;
    if(!(*digits) || strlen(digits) > 16) return false;

    for(const char* p = digits; *p; p++) {
        if(!isxdigit((unsigned char)*p)) return false;
    }

    *out = (RDAddress)strtoull(digits, NULL, 16);
    return true;
}

// the facts about an address: segment, function, external
static void _mcp_describe_address(RDContext* ctx, RDAddress address,
                                  RDScratchBuffer* out) {
    const RDSegment* seg = rd_segment_find(ctx, address);

    if(!seg) {
        rd_scratch_puts(out, "not inside any segment");
        return;
    }

    rd_scratch_puts(out, rd_is_function(ctx, address) ? "function" : "address");
    rd_scratch_putf(out, " in segment %s", rd_segment_get_name(seg));

    RDExternal ext;
    if(!rd_get_external(ctx, address, &ext)) return;

    if(ext.kind == RD_EXT_EXPORTED)
        rd_scratch_puts(out, ", exported");
    else
        rd_scratch_putf(out, ", imported from %s",
                        ext.module ? ext.module : "?");
}

yyjson_mut_val* mcp_exec_status(RDContext* ctx, yyjson_val* args,
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

yyjson_mut_val* mcp_exec_open_file(RDContext* ctx, yyjson_val* args,
                                   yyjson_mut_doc* doc) {
    RD_UNUSED(ctx);

    if(!mcp_session_is_standalone())
        return mcp_result_error(
            doc, "Opening files is only available in redasm_mcp.");

    if(!mcp_session_settings_valid())
        return mcp_result_error(
            doc, "Opening files is disabled: the [mcp] settings are invalid.");

    if(rd_datum_is_empty(
           rd_datum_get_array(rd_settings_global(), "mcp.allowed_dirs")))
        return mcp_result_error(
            doc, "Opening files is disabled: no allowed_dirs set.");

    const char* path = mcp_arg_open_file_path(args);
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

yyjson_mut_val* mcp_exec_list_segments(RDContext* ctx, yyjson_val* args,
                                       yyjson_mut_doc* doc) {
    RD_UNUSED(args);

    static const char* const COLUMNS[] = {
        "start", "end", "size", "perm", "name", NULL,
    };

    McpTable t = mcp_table_begin(doc, COLUMNS);
    mcp_table_set_untrusted(&t);

    RDSegmentSlice segments = rd_get_all_segments(ctx);

    const RDSegment** it;
    rd_slice_each(it, segments) {
        const RDSegment* seg = *it;
        RDSegmentPerm p = rd_segment_get_perm(seg);

        mcp_table_row(&t);
        mcp_table_address(&t, ctx, rd_segment_get_start(seg));
        mcp_table_address(&t, ctx, rd_segment_get_end(seg));
        mcp_table_uint(&t, rd_segment_get_size(seg));
        mcp_table_fmt(&t, "%c%c%c", (p & RD_SP_R) ? 'r' : '-',
                      (p & RD_SP_W) ? 'w' : '-', (p & RD_SP_X) ? 'x' : '-');
        mcp_table_str(&t, rd_segment_get_name(seg));
    }

    return mcp_table_result(&t, NULL);
}

yyjson_mut_val* mcp_exec_list_functions(RDContext* ctx, yyjson_val* args,
                                        yyjson_mut_doc* doc) {
    McpPage page;
    mcp_page_init(&page, args);

    static const char* const COLUMNS[] = {"address", "name", NULL};

    McpTable t = mcp_table_begin(doc, COLUMNS);
    mcp_table_set_untrusted(&t);

    RDAddressSlice functions = rd_get_all_functions(ctx);

    const RDAddress* it;
    rd_slice_each(it, functions) {
        // rd_get_name's buffer is reused: consume it before the next call
        const char* name = rd_get_name(ctx, *it);
        if(!mcp_page_take(&page, name)) continue;

        mcp_table_row(&t);
        mcp_table_address(&t, ctx, *it);
        mcp_table_str(&t, name);
    }

    return mcp_table_result(&t, &page);
}

yyjson_mut_val* mcp_exec_list_mappings(RDContext* ctx, yyjson_val* args,
                                       yyjson_mut_doc* doc) {
    RD_UNUSED(args);

    static const char* const COLUMNS[] = {
        "offset", "start", "end", "size", NULL,
    };

    McpTable t = mcp_table_begin(doc, COLUMNS);
    RDInputMappingSlice mappings = rd_get_all_mappings(ctx);

    const RDInputMapping** it;
    rd_slice_each(it, mappings) {
        mcp_table_row(&t);
        mcp_table_fmt(&t, "0x%" PRIX64, (u64)rd_inputmapping_get_offset(*it));
        mcp_table_address(&t, ctx, rd_inputmapping_get_start(*it));
        mcp_table_address(&t, ctx, rd_inputmapping_get_end(*it));
        mcp_table_uint(&t, rd_inputmapping_get_size(*it));
    }

    return mcp_table_result(&t, NULL);
}

yyjson_mut_val* mcp_exec_list_externals(RDContext* ctx, yyjson_val* args,
                                        yyjson_mut_doc* doc) {
    McpPage page;
    mcp_page_init(&page, args);

    RDExternalKind kind = mcp_arg_list_externals_kind(args);

    static const char* const COLUMNS[] = {
        "address", "kind", "module", "name", NULL,
    };

    McpTable t = mcp_table_begin(doc, COLUMNS);
    mcp_table_set_untrusted(&t);

    RDExternalSlice externals = rd_get_all_externals(ctx, kind);

    const RDExternal* ext;
    rd_slice_each(ext, externals) {
        const char* name = rd_get_name(ctx, ext->address);
        if(!mcp_page_take(&page, name)) continue;

        mcp_table_row(&t);
        mcp_table_address(&t, ctx, ext->address);
        mcp_table_str(&t, mcp_external_kind_name(ext->kind));
        mcp_table_str(&t, ext->module);

        // imports by ordinal often have no name
        if(name && *name)
            mcp_table_str(&t, name);
        else if(ext->ordinal.has_value)
            mcp_table_fmt(&t, "#%u", ext->ordinal.value);
        else
            mcp_table_str(&t, NULL);
    }

    return mcp_table_result(&t, &page);
}

yyjson_mut_val* mcp_exec_list_symbols(RDContext* ctx, yyjson_val* args,
                                      yyjson_mut_doc* doc) {
    McpPage page;
    mcp_page_init(&page, args);

    static const char* const COLUMNS[] = {
        "address", "kind", "type", "name", NULL,
    };

    McpTable t = mcp_table_begin(doc, COLUMNS);
    mcp_table_set_untrusted(&t);

    RDSymbolKind kind = mcp_arg_list_symbols_kind(args);
    RDSymbolSlice symbols = rd_get_all_symbols(ctx);

    const RDSymbol* sym;
    rd_slice_each(sym, symbols) {
        // RD_SYMBOL_INVALID is "all", like the other core kinds
        if(kind != RD_SYMBOL_INVALID && sym->kind != kind) continue;

        // rd_get_name's buffer is reused: consume it before the next call
        const char* name = sym->kind == RD_SYMBOL_SEGMENT
                               ? rd_segment_get_name(sym->segment)
                               : rd_get_name(ctx, sym->address);

        if(!mcp_page_take(&page, name)) continue;

        // copied now: rd_type_to_str below may reuse the same buffers
        yyjson_mut_val* nameval =
            name ? mcp_str(doc, name, strlen(name)) : NULL;

        mcp_table_row(&t);
        mcp_table_address(&t, ctx, sym->address);
        mcp_table_str(&t, mcp_symbol_kind_name(sym->kind));
        mcp_table_str(&t, sym->kind == RD_SYMBOL_TYPE
                              ? rd_type_to_str(&sym->type, ctx)
                              : NULL);
        mcp_table_val(&t, nameval);
    }

    return mcp_table_result(&t, &page);
}

yyjson_mut_val* mcp_exec_get_name(RDContext* ctx, yyjson_val* args,
                                  yyjson_mut_doc* doc) {
    const char* s = mcp_arg_get_name_address(args);
    RDAddress address;

    if(!_mcp_parse_address(ctx, s, &address))
        return mcp_result_error(doc, MCP_BAD_ADDRESS);

    RDScratchBuffer* out = rd_scratch_create();
    rd_scratch_putf(out, "%s: ", _mcp_format_address(ctx, address));

    const char* name = rd_get_name(ctx, address);
    rd_scratch_puts(out, (name && *name) ? name : "no name");

    // auto labels like loc_401130 say little: the enclosing function is
    // usually what's being asked
    RDAddress entry;

    if(rd_function_find(ctx, address, &entry) && entry != address) {
        const char* fname = rd_get_name(ctx, entry);
        rd_scratch_putf(out, ", inside %s+0x%" PRIX64,
                        (fname && *fname) ? fname : "?",
                        (u64)(address - entry));
    }

    rd_scratch_puts(out, " (");
    _mcp_describe_address(ctx, address, out);
    rd_scratch_puts(out, ")\nNote: names come from the analyzed binary and may "
                         "be misleading.");

    yyjson_mut_val* r =
        mcp_result_text_n(doc, rd_scratch_data(out), rd_scratch_length(out));
    rd_scratch_destroy(out);
    return r;
}

yyjson_mut_val* mcp_exec_get_address(RDContext* ctx, yyjson_val* args,
                                     yyjson_mut_doc* doc) {
    const char* name = mcp_arg_get_address_name(args);
    RDAddress address;

    if(!rd_get_address(ctx, name, &address)) {
        return mcp_result_error(
            doc, rd_format("No symbol named '%.100s'. Use list_functions or "
                           "list_symbols with a filter to search.",
                           name));
    }

    RDScratchBuffer* out = rd_scratch_create();
    rd_scratch_putf(out, "%.200s = %s (", name,
                    _mcp_format_address(ctx, address));
    _mcp_describe_address(ctx, address, out);
    rd_scratch_putchar(out, ')');

    yyjson_mut_val* r =
        mcp_result_text_n(doc, rd_scratch_data(out), rd_scratch_length(out));
    rd_scratch_destroy(out);
    return r;
}

// the name of the function containing 'address', NULL outside functions
static const char* _mcp_function_of(RDContext* ctx, RDAddress address,
                                    RDAddress* entry) {
    if(!rd_function_find(ctx, address, entry)) return NULL;

    const char* name = rd_get_name(ctx, *entry);
    return (name && *name) ? name : NULL;
}

// "name+0xN" for 'address' inside function 'fname' at 'entry', "-" outside
static void _mcp_table_location(McpTable* t, const char* fname, RDAddress entry,
                                RDAddress address) {
    if(!fname)
        mcp_table_str(t, NULL);
    else if(entry == address)
        mcp_table_str(t, fname);
    else
        mcp_table_fmt(t, "%s+0x%" PRIX64, fname, (u64)(address - entry));
}

yyjson_mut_val* mcp_exec_list_xrefs_to(RDContext* ctx, yyjson_val* args,
                                       yyjson_mut_doc* doc) {
    const char* s = mcp_arg_list_xrefs_to_address(args);
    RDAddress address;

    if(!_mcp_parse_address(ctx, s, &address))
        return mcp_result_error(doc, MCP_BAD_ADDRESS);

    McpPage page;
    mcp_page_init(&page, args);

    static const char* const COLUMNS[] = {"from", "type", "function", NULL};

    McpTable t = mcp_table_begin(doc, COLUMNS);
    mcp_table_set_untrusted(&t);

    // the slice lives in the context until the next rd_get_xrefs_to():
    // rd_function_find and rd_get_name below don't touch it
    RDXRefSlice refs =
        rd_get_xrefs_to(ctx, address, mcp_arg_list_xrefs_to_kind(args));

    const RDXRef* xr;
    rd_slice_each(xr, refs) {
        RDAddress entry;
        const char* fname = _mcp_function_of(ctx, xr->address, &entry);
        if(!mcp_page_take(&page, fname)) continue;

        mcp_table_row(&t);
        mcp_table_address(&t, ctx, xr->address);
        mcp_table_str(&t, mcp_xref_kind_name(xr->type));

        _mcp_table_location(&t, fname, entry, xr->address);
    }

    return mcp_table_result(&t, &page);
}

yyjson_mut_val* mcp_exec_list_xrefs_from(RDContext* ctx, yyjson_val* args,
                                         yyjson_mut_doc* doc) {
    const char* s = mcp_arg_list_xrefs_from_address(args);
    RDAddress address;

    if(!_mcp_parse_address(ctx, s, &address))
        return mcp_result_error(doc, MCP_BAD_ADDRESS);

    McpPage page;
    mcp_page_init(&page, args);

    static const char* const COLUMNS[] = {"to", "type", "name", NULL};

    McpTable t = mcp_table_begin(doc, COLUMNS);
    mcp_table_set_untrusted(&t);

    // same lifetime as in list_xrefs_to, with rd_get_xrefs_from()
    RDXRefSlice refs =
        rd_get_xrefs_from(ctx, address, mcp_arg_list_xrefs_from_kind(args));

    const RDXRef* xr;
    rd_slice_each(xr, refs) {
        const char* name = rd_get_name(ctx, xr->address);
        if(name && !*name) name = NULL;
        if(!mcp_page_take(&page, name)) continue;

        mcp_table_row(&t);
        mcp_table_address(&t, ctx, xr->address);
        mcp_table_str(&t, mcp_xref_kind_name(xr->type));
        mcp_table_str(&t, name);
    }

    return mcp_table_result(&t, &page);
}

static yyjson_mut_val* _mcp_list_calls(RDContext* ctx, yyjson_val* args,
                                       yyjson_mut_doc* doc, bool callees) {
    const char* s = callees ? mcp_arg_list_callees_address(args)
                            : mcp_arg_list_callers_address(args);
    RDAddress address, entry;

    if(!_mcp_parse_address(ctx, s, &address))
        return mcp_result_error(doc, MCP_BAD_ADDRESS);

    if(!rd_function_find(ctx, address, &entry))
        return mcp_result_error(doc, "This address is not inside a function.");

    McpPage page;
    mcp_page_init(&page, args);

    static const char* const CALLEES[] = {"from", "call", "to", "name", NULL};
    static const char* const CALLERS[] = {"from", "call", "function", NULL};

    McpTable t = mcp_table_begin(doc, callees ? CALLEES : CALLERS);
    mcp_table_set_untrusted(&t);

    RDCallSlice calls =
        callees ? rd_get_callees(ctx, address) : rd_get_callers(ctx, address);

    const RDCall* call;
    rd_slice_each(call, calls) {
        const char* kind = call->type == RD_CR_CALL ? "direct" : "indirect";

        if(callees) {
            const char* name = rd_get_name(ctx, call->target);
            if(name && !*name) name = NULL;
            if(!mcp_page_take(&page, name)) continue;

            mcp_table_row(&t);
            mcp_table_address(&t, ctx, call->address);
            mcp_table_str(&t, kind);
            mcp_table_address(&t, ctx, call->target);
            mcp_table_str(&t, name);
        }
        else {
            RDAddress caller;
            const char* fname = _mcp_function_of(ctx, call->address, &caller);
            if(!mcp_page_take(&page, fname)) continue;

            mcp_table_row(&t);
            mcp_table_address(&t, ctx, call->address);
            mcp_table_str(&t, kind);
            _mcp_table_location(&t, fname, caller, call->address);
        }
    }

    return mcp_table_result(&t, &page);
}

yyjson_mut_val* mcp_exec_list_callees(RDContext* ctx, yyjson_val* args,
                                      yyjson_mut_doc* doc) {
    return _mcp_list_calls(ctx, args, doc, true);
}

yyjson_mut_val* mcp_exec_list_callers(RDContext* ctx, yyjson_val* args,
                                      yyjson_mut_doc* doc) {
    return _mcp_list_calls(ctx, args, doc, false);
}

// ---- shared by the read and write tools ------------------------------------

static usize _mcp_format_width(McpDataFormat f) {
    switch(f) {
        case MCP_DATA_FORMAT_BYTE: return 1;
        case MCP_DATA_FORMAT_LE16:
        case MCP_DATA_FORMAT_BE16: return 2;
        case MCP_DATA_FORMAT_LE32:
        case MCP_DATA_FORMAT_BE32: return 4;
        case MCP_DATA_FORMAT_LE64:
        case MCP_DATA_FORMAT_BE64: return 8;
        default: return 0; // variable or context dependent
    }
}

static bool _mcp_format_is_be(McpDataFormat f) {
    return f == MCP_DATA_FORMAT_BE16 || f == MCP_DATA_FORMAT_BE32 ||
           f == MCP_DATA_FORMAT_BE64;
}

// "xx xx xx", for reporting old and new bytes
static void _mcp_put_hex(RDScratchBuffer* out, const u8* data, usize n) {
    for(usize i = 0; i < n; i++)
        rd_scratch_putf(out, i ? " %02X" : "%02X", data[i]);
}

// keeps the first MCP_MAX_STR_CELL bytes, never splitting a UTF-8 sequence
static void _mcp_table_text(McpTable* t, const char* s, usize n) {
    if(n <= MCP_MAX_STR_CELL) {
        mcp_table_val(t, mcp_str(t->doc, s, n));
        return;
    }

    usize cut = MCP_MAX_STR_CELL;
    while(cut && ((unsigned char)s[cut] & 0xC0) == 0x80)
        cut--;

    mcp_table_fmt(t, "%.*s... (%zu bytes)", (int)cut, s, n);
}

// ---- strings, types ---------------------------------------------------------

yyjson_mut_val* mcp_exec_list_strings(RDContext* ctx, yyjson_val* args,
                                      yyjson_mut_doc* doc) {
    McpPage page;
    mcp_page_init(&page, args);

    static const char* const COLUMNS[] = {
        "address", "type", "length", "value", NULL,
    };

    McpTable t = mcp_table_begin(doc, COLUMNS);
    mcp_table_set_untrusted(&t);

    RDSymbolSlice symbols = rd_get_all_symbols(ctx);

    const RDSymbol* sym;
    rd_slice_each(sym, symbols) {
        if(sym->kind != RD_SYMBOL_TYPE || !rd_type_is_string(&sym->type))
            continue;

        // quoted and escaped by core, in the context's buffer
        const char* value = rd_symbol_to_string(sym, ctx);
        if(!mcp_page_take(&page, value)) continue;

        mcp_table_row(&t);
        mcp_table_address(&t, ctx, sym->address);
        mcp_table_str(&t, strcmp(rd_typedef_name(sym->type.def), "char16")
                              ? "ascii"
                              : "utf16");
        mcp_table_uint(&t, sym->type.count);
        _mcp_table_text(&t, value, strlen(value));
    }

    return mcp_table_result(&t, &page);
}

static const char* _mcp_type_kind(RDTypeKind k) {
    switch(k) {
        case RD_TKIND_PRIM: return "primitive";
        case RD_TKIND_STRUCT: return "struct";
        case RD_TKIND_UNION: return "union";
        case RD_TKIND_ENUM: return "enum";
        case RD_TKIND_FUNC: return "function";
        default: return NULL;
    }
}

yyjson_mut_val* mcp_exec_list_types(RDContext* ctx, yyjson_val* args,
                                    yyjson_mut_doc* doc) {
    McpPage page;
    mcp_page_init(&page, args);

    static const char* const COLUMNS[] = {"name", "kind", "size", NULL};

    McpTable t = mcp_table_begin(doc, COLUMNS);
    mcp_table_set_untrusted(&t);

    RDTypeDefSlice defs = rd_get_all_type_defs(ctx);

    const RDTypeDef** it;
    rd_slice_each(it, defs) {
        const char* name = rd_typedef_name(*it);
        if(!mcp_page_take(&page, name)) continue;

        RDTypeKind k = rd_typedef_kind(*it);
        usize size = rd_typedef_size(*it);

        mcp_table_row(&t);
        mcp_table_str(&t, name);
        mcp_table_str(&t, _mcp_type_kind(k));

        if(k == RD_TKIND_FUNC || !size)
            mcp_table_str(&t, NULL);
        else
            mcp_table_uint(&t, size);
    }

    return mcp_table_result(&t, &page);
}

// ---- reading ----------------------------------------------------------------

yyjson_mut_val* mcp_exec_get_bytes(RDContext* ctx, yyjson_val* args,
                                   yyjson_mut_doc* doc) {
    RDAddress address;
    if(!_mcp_parse_address(ctx, mcp_arg_get_bytes_address(args), &address))
        return mcp_result_error(doc, MCP_BAD_ADDRESS);

    usize count = (usize)mcp_arg_get_bytes_count(args);

    static const char* const COLUMNS[] = {"address", "hex", "ascii", NULL};
    McpTable t = mcp_table_begin(doc, COLUMNS);

    RDScratchBuffer* hex = rd_scratch_create();
    RDScratchBuffer* ascii = rd_scratch_create();
    usize total = 0;

    while(total < count) {
        u8 row[16];
        usize want = count - total;
        if(want > sizeof(row)) want = sizeof(row);
        usize got = rd_read(ctx, address + total, row, want);
        if(!got) break;

        rd_scratch_clear(hex);
        rd_scratch_clear(ascii);
        _mcp_put_hex(hex, row, got);

        for(usize i = 0; i < got; i++)
            rd_scratch_putchar(ascii, isprint(row[i]) ? (char)row[i] : '.');

        mcp_table_row(&t);
        mcp_table_address(&t, ctx, address + total);
        mcp_table_str(&t, rd_scratch_data(hex));
        mcp_table_str(&t, rd_scratch_data(ascii));

        total += got;
        if(got < want) break; // the end of the readable range
    }

    rd_scratch_destroy(ascii);
    rd_scratch_destroy(hex);

    // the table lives in the reply document, nothing to release
    if(!total)
        return mcp_result_error(doc, "Nothing readable at this address.");

    return mcp_table_result(&t, NULL);
}

// one value of format 'f' at 'a' as a table row, its size in 'size'
static bool _mcp_read_item(McpTable* t, RDContext* ctx, RDAddress a,
                           McpDataFormat f, usize* size) {
    u64 v = 0;
    bool ok = false;

    switch(f) {
        case MCP_DATA_FORMAT_BYTE: {
            u8 x;
            ok = rd_read_byte(ctx, a, &x);
            v = x;
            break;
        }

        case MCP_DATA_FORMAT_LE16:
        case MCP_DATA_FORMAT_BE16: {
            u16 x;
            ok = f == MCP_DATA_FORMAT_LE16 ? rd_read_le16(ctx, a, &x)
                                           : rd_read_be16(ctx, a, &x);
            v = x;
            break;
        }

        case MCP_DATA_FORMAT_LE32:
        case MCP_DATA_FORMAT_BE32: {
            u32 x;
            ok = f == MCP_DATA_FORMAT_LE32 ? rd_read_le32(ctx, a, &x)
                                           : rd_read_be32(ctx, a, &x);
            v = x;
            break;
        }

        case MCP_DATA_FORMAT_LE64: ok = rd_read_le64(ctx, a, &v); break;

        case MCP_DATA_FORMAT_BE64: ok = rd_read_be64(ctx, a, &v); break;

        case MCP_DATA_FORMAT_ULEB128: {
            RDULeb128 x;
            if(!rd_read_uleb128(ctx, a, &x)) return false;

            *size = x.length;
            mcp_table_row(t);
            mcp_table_address(t, ctx, a);
            mcp_table_fmt(t, "0x%" PRIX64 " (%" PRIu64 ")", x.value, x.value);
            mcp_table_str(t, NULL);
            return true;
        }

        case MCP_DATA_FORMAT_SLEB128: {
            RDSLeb128 x;
            if(!rd_read_sleb128(ctx, a, &x)) return false;

            *size = x.length;
            mcp_table_row(t);
            mcp_table_address(t, ctx, a);
            mcp_table_fmt(t, "%" PRId64, x.value);
            mcp_table_str(t, NULL);
            return true;
        }

        case MCP_DATA_FORMAT_PTR:
        case MCP_DATA_FORMAT_CODE_PTR: {
            RDAddress p;
            ok = f == MCP_DATA_FORMAT_PTR ? rd_read_ptr(ctx, a, &p)
                                          : rd_read_code_ptr(ctx, a, &p);
            if(!ok) return false;

            *size = rd_get_ptr_size(ctx);
            mcp_table_row(t);
            mcp_table_address(t, ctx, a);
            mcp_table_address(t, ctx, p);

            const char* name = rd_get_name(ctx, p);
            mcp_table_str(t, (name && *name) ? name : NULL);
            return true;
        }

        case MCP_DATA_FORMAT_STR: {
            usize n = 0;
            const char* s = rd_read_str(ctx, a, &n);
            if(!s) return false;

            *size = n + 1; // and the terminator
            mcp_table_row(t);
            mcp_table_address(t, ctx, a);
            _mcp_table_text(t, s, n);
            mcp_table_str(t, NULL);
            return true;
        }

        default: return false;
    }

    if(!ok) return false;

    usize w = _mcp_format_width(f);
    *size = w;

    mcp_table_row(t);
    mcp_table_address(t, ctx, a);
    mcp_table_fmt(t, "0x%0*" PRIX64 " (%" PRIu64 ")", (int)(w * 2), v, v);
    mcp_table_str(t, NULL);
    return true;
}

yyjson_mut_val* mcp_exec_get_data(RDContext* ctx, yyjson_val* args,
                                  yyjson_mut_doc* doc) {
    RDAddress address;
    if(!_mcp_parse_address(ctx, mcp_arg_get_data_address(args), &address))
        return mcp_result_error(doc, MCP_BAD_ADDRESS);

    usize count = (usize)mcp_arg_get_data_count(args);
    McpDataFormat f = mcp_arg_get_data_format(args);

    static const char* const COLUMNS[] = {"address", "value", "name", NULL};
    McpTable t = mcp_table_begin(doc, COLUMNS);
    mcp_table_set_untrusted(&t);

    RDAddress a = address;
    usize i = 0;

    for(; i < count; i++) {
        usize size = 0;
        if(!_mcp_read_item(&t, ctx, a, f, &size) || !size) break;
        a += size;
    }

    if(!i) {
        // not one rd_format() inside another: they share a buffer
        RDScratchBuffer* msg = rd_scratch_create();
        rd_scratch_putf(msg, "Cannot read %s at ", mcp_data_format_name(f));
        rd_scratch_putf(msg, "%s.", _mcp_format_address(ctx, address));

        yyjson_mut_val* r = mcp_result_error(doc, rd_scratch_data(msg));
        rd_scratch_destroy(msg);
        return r;
    }

    return mcp_table_result(&t, NULL);
}

// ---- writing ----------------------------------------------------------------
// Every change goes through rd_patch, never rd_write: rd_write is the loaders'
// raw write, it isn't recorded as a patch and doesn't requeue the analysis.

// sends 'out' as the result and destroys it
static yyjson_mut_val* _mcp_reply(yyjson_mut_doc* doc, RDScratchBuffer* out,
                                  bool error) {
    const char* text = rd_scratch_data(out);
    yyjson_mut_val* r =
        error ? mcp_result_error(doc, text)
              : mcp_result_text_n(doc, text, rd_scratch_length(out));
    rd_scratch_destroy(out);
    return r;
}

// "0x90", "90 90 C3", "9090c3": pairs of hex digits, spaces ignored
static const char* _mcp_parse_hex(const char* s, u8** data, usize* n) {
    *data = NULL;
    *n = 0;

    usize digits = 0;
    for(const char* p = s; *p; p++) {
        if(isspace((unsigned char)*p)) continue;
        if(!isxdigit((unsigned char)*p))
            return "'bytes' must be hex digits, e.g. \"90 90 C3\".";
        digits++;
    }

    if(!digits || digits % 2)
        return "'bytes' must be whole bytes, two hex digits each.";
    if(digits / 2 > MCP_MAX_BYTES)
        return rd_format("At most %d bytes per patch.", MCP_MAX_BYTES);

    u8* out = rd_alloc(digits / 2);
    usize i = 0;
    int hi = -1;

    for(const char* p = s; *p; p++) {
        if(isspace((unsigned char)*p)) continue;

        char c = (char)tolower((unsigned char)*p);
        int v = isdigit((unsigned char)c) ? c - '0' : c - 'a' + 10;

        if(hi < 0) {
            hi = v;
        }
        else {
            out[i++] = (u8)((hi << 4) | v);
            hi = -1;
        }
    }

    *data = out;
    *n = i;
    return NULL;
}

// writes 'data' as a patch: the old bytes are reported so it can be undone
static yyjson_mut_val* _mcp_patch(RDContext* ctx, yyjson_mut_doc* doc,
                                  RDAddress address, const u8* data, usize n) {
    u8* old = rd_alloc(n);
    usize nold = rd_read(ctx, address, old, n);

    if(nold < n) {
        rd_free(old);
        return mcp_result_error(
            doc, "The range isn't fully readable: nothing was patched.");
    }

    usize written = rd_patch(ctx, address, data, n);
    mcp_session_settle(ctx);

    RDScratchBuffer* out = rd_scratch_create();

    if(written != n) {
        rd_scratch_putf(out, "Only %zu of %zu bytes were patched at ", written,
                        n);
        rd_scratch_putf(out, "%s.", _mcp_format_address(ctx, address));
        rd_free(old);
        return _mcp_reply(doc, out, true);
    }

    rd_scratch_putf(out, "Patched %zu byte%s at ", n, n == 1 ? "" : "s");
    rd_scratch_putf(
        out, "%s, analysis updated.\nOld: ", _mcp_format_address(ctx, address));
    _mcp_put_hex(out, old, n);
    rd_scratch_puts(out, "\nNew: ");
    _mcp_put_hex(out, data, n);

    rd_free(old);
    return _mcp_reply(doc, out, false);
}

yyjson_mut_val* mcp_exec_patch_bytes(RDContext* ctx, yyjson_val* args,
                                     yyjson_mut_doc* doc) {
    RDAddress address;
    if(!_mcp_parse_address(ctx, mcp_arg_patch_bytes_address(args), &address))
        return mcp_result_error(doc, MCP_BAD_ADDRESS);

    u8* data;
    usize n;
    const char* err =
        _mcp_parse_hex(mcp_arg_patch_bytes_bytes(args), &data, &n);

    if(err) return mcp_result_error(doc, err);

    yyjson_mut_val* r = _mcp_patch(ctx, doc, address, data, n);
    rd_free(data);
    return r;
}

// "0x1234", "4660" or "-5", checked against the width in bytes
static const char* _mcp_parse_int(const char* s, usize width, u64* out) {
    if(!(*s)) return "'value' can't be empty.";

    bool neg = *s == '-';
    const char* p = neg ? s + 1 : s;
    int base = (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) ? 16 : 10;
    if(base == 16) p += 2;

    if(!(*p) || !isxdigit((unsigned char)*p) ||
       (base == 10 && !isdigit((unsigned char)*p)))
        return "'value' must be decimal, or hex with 0x, e.g. 4660 or 0x1234.";

    errno = 0;
    char* end = NULL;
    u64 mag = strtoull(p, &end, base);

    if(errno || *end)
        return "'value' must be decimal, or hex with 0x, e.g. 4660 or 0x1234.";

    unsigned bits = (unsigned)(width * 8);
    u64 umax = bits == 64 ? UINT64_MAX : ((1ULL << bits) - 1);

    if(neg) {
        u64 smin = 1ULL << (bits - 1); // magnitude of the most negative value
        if(mag > smin) return "'value' doesn't fit the format.";
        *out = (~mag + 1) & umax; // two's complement, truncated to width
    }
    else {
        if(mag > umax) return "'value' doesn't fit the format.";
        *out = mag;
    }

    return NULL;
}

yyjson_mut_val* mcp_exec_patch_data(RDContext* ctx, yyjson_val* args,
                                    yyjson_mut_doc* doc) {
    RDAddress address;
    if(!_mcp_parse_address(ctx, mcp_arg_patch_data_address(args), &address))
        return mcp_result_error(doc, MCP_BAD_ADDRESS);

    // the schema only allows the fixed-size integers here
    McpDataFormat f = mcp_arg_patch_data_format(args);
    usize w = _mcp_format_width(f);

    u64 v;
    const char* err = _mcp_parse_int(mcp_arg_patch_data_value(args), w, &v);

    if(err) return mcp_result_error(doc, err);

    u8 data[8];
    bool be = _mcp_format_is_be(f);

    for(usize i = 0; i < w; i++) {
        usize shift = be ? (w - 1 - i) * 8 : i * 8;
        data[i] = (u8)(v >> shift);
    }

    return _mcp_patch(ctx, doc, address, data, w);
}

yyjson_mut_val* mcp_exec_patch_instruction(RDContext* ctx, yyjson_val* args,
                                           yyjson_mut_doc* doc) {
    RDAddress address;
    if(!_mcp_parse_address(ctx, mcp_arg_patch_instruction_address(args),
                           &address))
        return mcp_result_error(doc, MCP_BAD_ADDRESS);

    if(!rd_can_encode(ctx)) {
        return mcp_result_error(doc, "The processor can't assemble "
                                     "instructions: use patch_bytes.");
    }

    const char* instr = mcp_arg_patch_instruction_instruction(args);
    if(!(*instr)) return mcp_result_error(doc, "'instruction' can't be empty.");

    bool fill = mcp_arg_patch_instruction_fill_nops(args);

    if(!rd_patch_instruction(ctx, address, instr, fill)) {
        return mcp_result_error(doc, "Cannot assemble or place this "
                                     "instruction here: check the syntax "
                                     "and that the address is code.");
    }

    mcp_session_settle(ctx);

    RDScratchBuffer* out = rd_scratch_create();
    rd_scratch_putf(out, "Patched %s", _mcp_format_address(ctx, address));
    rd_scratch_putf(out, " with '%s', analysis updated.", instr);
    return _mcp_reply(doc, out, false);
}

// ---- names, comments, types ------------------------------------------------

// letters, digits and _ $ @ ? . : first char not a digit, and never hex-like:
// tools resolve names before addresses. The length is checked by the schema.
static const char* _mcp_check_name(const char* s) {
    if(isdigit((unsigned char)s[0])) return "A name can't start with a digit.";

    for(const char* p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if(isalnum(c) || strchr("_$@?.:", c)) continue;
        return "A name can only use letters, digits and _ $ @ ? . :";
    }

    return NULL;
}

yyjson_mut_val* mcp_exec_rename(RDContext* ctx, yyjson_val* args,
                                yyjson_mut_doc* doc) {
    RDAddress address;
    if(!_mcp_parse_address(ctx, mcp_arg_rename_address(args), &address))
        return mcp_result_error(doc, MCP_BAD_ADDRESS);

    const char* name = mcp_arg_rename_name(args);

    RDScratchBuffer* newname = rd_scratch_create();

    if(*name) {
        const char* err = _mcp_check_name(name);

        if(err) {
            rd_scratch_destroy(newname);
            return mcp_result_error(doc, err);
        }

        // marks what the model named, unless it's already there
        const char* prefix = mcp_session_name_prefix();
        if(prefix && strncmp(name, prefix, strlen(prefix)) != 0)
            rd_scratch_puts(newname, prefix);

        rd_scratch_puts(newname, name);
    }

    const char* final = rd_scratch_data(newname);
    RDAddress existing;

    if(*final && rd_get_address(ctx, final, &existing) && existing != address) {
        RDScratchBuffer* out = rd_scratch_create();
        rd_scratch_putf(out, "'%s' is already the name of ", final);
        rd_scratch_putf(out, "%s.", _mcp_format_address(ctx, existing));
        rd_scratch_destroy(newname);
        return _mcp_reply(doc, out, true);
    }

    // copied: rd_get_name's buffer is reused
    const char* cur = rd_get_name(ctx, address);
    char* old = rd_strdup((cur && *cur) ? cur : "");

    bool ok = rd_user_name(ctx, address, final);
    RDScratchBuffer* out = rd_scratch_create();

    if(!ok) {
        rd_scratch_puts(out, *final ? "Cannot rename this address: it must be "
                                      "the start of an item inside a segment."
                                    : "There's no name to remove here.");
    }
    else if(*final) {
        rd_scratch_putf(out, "Renamed %s", _mcp_format_address(ctx, address));
        rd_scratch_putf(out, " from '%s' to '%s'.", old, final);
    }
    else {
        cur = rd_get_name(ctx, address);
        rd_scratch_putf(out, "Removed '%s' at ", old);
        rd_scratch_putf(out, "%s, ", _mcp_format_address(ctx, address));
        rd_scratch_putf(out, "now '%s'.", (cur && *cur) ? cur : "unnamed");
    }

    rd_free(old);
    rd_scratch_destroy(newname);
    return _mcp_reply(doc, out, !ok);
}

yyjson_mut_val* mcp_exec_set_comment(RDContext* ctx, yyjson_val* args,
                                     yyjson_mut_doc* doc) {
    RDAddress address;
    if(!_mcp_parse_address(ctx, mcp_arg_set_comment_address(args), &address))
        return mcp_result_error(doc, MCP_BAD_ADDRESS);

    // the length is checked by the schema
    const char* text = mcp_arg_set_comment_text(args);
    McpCommentPlacement p = mcp_arg_set_comment_placement(args);
    const char* where;
    bool ok;

    // every placement replaces what's there, empty text removes it
    switch(p) {
        case MCP_COMMENT_PLACEMENT_BEFORE:
            rd_del_comment_before(ctx, address);
            ok = !(*text) || rd_add_comment_before(ctx, address, text);
            where = "comment before";
            break;

        case MCP_COMMENT_PLACEMENT_AFTER:
            rd_del_comment_after(ctx, address);
            ok = !(*text) || rd_add_comment_after(ctx, address, text);
            where = "comment after";
            break;

        default:
            ok = rd_set_comment_inline(ctx, address, text);
            where = "inline comment at";
            break;
    }

    if(!ok) {
        return mcp_result_error(doc, "Cannot comment this address: it must be "
                                     "the start of an item inside a segment.");
    }

    RDScratchBuffer* out = rd_scratch_create();
    rd_scratch_putf(out, "%s the %s ", *text ? "Set" : "Removed", where);
    rd_scratch_putf(out, "%s.", _mcp_format_address(ctx, address));
    return _mcp_reply(doc, out, false);
}

// "u32", "char[16]", "u8*", "u8*[4]": a type name, an optional pointer and an
// optional array count. 'name' is a copy for the caller to free.
static const char* _mcp_parse_type(const char* s, char** name, usize* n,
                                   RDTypeModifier* mod) {
    *name = NULL;
    *n = 0;
    *mod = RD_TYPE_NONE;

    while(isspace((unsigned char)*s))
        s++;

    const char* start = s;
    while(*s && (isalnum((unsigned char)*s) || *s == '_'))
        s++;

    if(s == start) return "'type' must start with a type name, see list_types.";

    usize len = (usize)(s - start);

    while(isspace((unsigned char)*s))
        s++;

    if(*s == '*') {
        *mod = RD_TYPE_PTR;
        s++;
        while(isspace((unsigned char)*s))
            s++;
    }

    if(*s == '[') {
        errno = 0;
        char* end = NULL;
        unsigned long long c = strtoull(s + 1, &end, 10);

        if(errno || end == s + 1 || *end != ']' || !c || c > SIZE_MAX)
            return "The array count must be a positive number, e.g. char[16].";

        *n = (usize)c;
        s = end + 1;
    }

    while(isspace((unsigned char)*s))
        s++;

    if(*s) return "'type' must look like u32, char[16], u8* or u8*[4].";

    char* copy = rd_alloc(len + 1);
    memcpy(copy, start, len);
    copy[len] = 0;
    *name = copy;
    return NULL;
}

static yyjson_mut_val* _mcp_set_type(RDContext* ctx, yyjson_val* args,
                                     yyjson_mut_doc* doc, RDAddress address,
                                     const char* name, usize n,
                                     RDTypeModifier mod) {
    RDType t;
    if(!rd_type_init(&t, name, n, mod, ctx)) {
        return mcp_result_error(doc, "Unknown type or invalid count: see "
                                     "list_types for the names.");
    }

    RDTypeKind kind = rd_typedef_kind(t.def);

    // a function type on its own types the function at that address
    if(kind == RD_TKIND_FUNC && mod == RD_TYPE_NONE) {
        if(n)
            return mcp_result_error(doc, "A function type can't be an array.");

        if(!rd_set_typed_function(ctx, address, name)) {
            return mcp_result_error(doc,
                                    "Cannot make a typed function here: it "
                                    "must be code in an executable segment.");
        }

        mcp_session_settle(ctx);

        RDScratchBuffer* out = rd_scratch_create();
        rd_scratch_putf(out, "%s is now a function of type ",
                        _mcp_format_address(ctx, address));
        rd_scratch_putf(out, "'%s', analysis updated.", name);
        return _mcp_reply(doc, out, false);
    }

    // core panics on a type without size: refuse it before asking
    if(mod == RD_TYPE_NONE && !rd_typedef_size(t.def))
        return mcp_result_error(doc,
                                "This type has no size, it can't be placed.");

    usize size = rd_type_size(&t, ctx);

    bool force = mcp_arg_set_type_force(args);

    // a user type silently replaces code with data: only when asked to
    if(!force) {
        for(usize i = 0; i < size; i++) {
            if(!rd_is_code(ctx, address + i)) continue;

            RDScratchBuffer* out = rd_scratch_create();
            rd_scratch_putf(out, "The %zu bytes at ", size);
            rd_scratch_putf(out,
                            "%s contain code: pass force=true to turn "
                            "them into data.",
                            _mcp_format_address(ctx, address));
            return _mcp_reply(doc, out, true);
        }
    }

    if(!rd_user_type(ctx, address, name, n, mod)) {
        return mcp_result_error(doc, "Cannot place this type here: it must fit "
                                     "inside the segment.");
    }

    mcp_session_settle(ctx);

    RDScratchBuffer* out = rd_scratch_create();
    rd_scratch_putf(out, "%s is now ", _mcp_format_address(ctx, address));

    RDType placed;
    rd_scratch_putf(out, "%s (%zu bytes), analysis updated.",
                    rd_get_type(ctx, address, &placed)
                        ? rd_type_to_str(&placed, ctx)
                        : name,
                    size);

    return _mcp_reply(doc, out, false);
}

yyjson_mut_val* mcp_exec_set_type(RDContext* ctx, yyjson_val* args,
                                  yyjson_mut_doc* doc) {
    RDAddress address;
    if(!_mcp_parse_address(ctx, mcp_arg_set_type_address(args), &address))
        return mcp_result_error(doc, MCP_BAD_ADDRESS);

    const char* s = mcp_arg_set_type_type(args);

    char* name;
    usize n;
    RDTypeModifier mod;
    const char* err = _mcp_parse_type(s, &name, &n, &mod);
    if(err) return mcp_result_error(doc, err);

    yyjson_mut_val* r = _mcp_set_type(ctx, args, doc, address, name, n, mod);
    rd_free(name);
    return r;
}
